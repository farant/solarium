/* srv_db.c — see srv_db.h. Migrations: MIGRATIONS[i] is applied when
   PRAGMA user_version == i, inside a transaction that also bumps
   user_version to i+1 — so the array only ever grows and old databases
   upgrade in order. */

#include "srv_db.h"

#include <stdio.h>
#include <string.h>

static const char *MIGRATIONS[] = {
    /* v1 — event log + first projection + auth tables (spec §5, §7).
       The events table is the source of truth AND the sync feed: id is the
       global cursor. Projections (boards) update in the same transaction as
       the append (srv_events.c). Auth tables are ordinary state. */
    "CREATE TABLE events("
    "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ts            INTEGER NOT NULL,"
    "  actor_id      INTEGER NOT NULL,"
    "  origin_device TEXT NOT NULL DEFAULT '',"
    "  entity_kind   TEXT NOT NULL,"
    "  entity_nid    TEXT NOT NULL,"
    "  op            TEXT NOT NULL,"
    "  payload       TEXT NOT NULL DEFAULT '{}');"
    "CREATE INDEX idx_events_entity ON events(entity_kind, entity_nid);"
    "CREATE TABLE boards("
    "  nid        TEXT PRIMARY KEY,"
    "  title      TEXT NOT NULL DEFAULT '',"
    "  deleted    INTEGER NOT NULL DEFAULT 0,"
    "  updated_at INTEGER NOT NULL,"
    "  updated_by INTEGER NOT NULL) WITHOUT ROWID;"
    "CREATE TABLE users("
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  name       TEXT UNIQUE NOT NULL,"
    "  pw_salt    BLOB NOT NULL,"
    "  pw_hash    BLOB NOT NULL,"
    "  pw_iters   INTEGER NOT NULL,"
    "  created_at INTEGER NOT NULL);"
    "CREATE TABLE sessions("
    "  token_hash BLOB PRIMARY KEY,"
    "  user_id    INTEGER NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    "  expires_at INTEGER NOT NULL) WITHOUT ROWID;"
    "CREATE TABLE login_attempts("
    "  ip           TEXT PRIMARY KEY,"
    "  fails        INTEGER NOT NULL,"
    "  window_start INTEGER NOT NULL) WITHOUT ROWID;"
};

static int db_exec_on(sqlite3 *c, const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(c, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "srv_db: %s\n", err ? err : "exec failed");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

int srv_db_open(SrvDb *db, const char *path) {
    sqlite3_stmt *st;
    char bump[64];
    int ver, i, n;

    memset(db, 0, sizeof *db);
    if (strlen(path) >= sizeof db->path) {
        fprintf(stderr, "srv_db: path too long\n");
        return -1;
    }
    strcpy(db->path, path);

    if (sqlite3_open(path, &db->w) != SQLITE_OK) {
        fprintf(stderr, "srv_db: open %s: %s\n", path, sqlite3_errmsg(db->w));
        sqlite3_close(db->w);
        db->w = NULL;
        return -1;
    }
    /* Initialize mutex immediately after successful open, before any failure path. */
    pthread_mutex_init(&db->wlock, NULL);

    if (db_exec_on(db->w, "PRAGMA journal_mode=WAL;"
                          "PRAGMA busy_timeout=5000;"
                          "PRAGMA foreign_keys=ON;"
                          "PRAGMA synchronous=NORMAL;") != 0) goto fail;

    ver = 0;
    st = NULL;
    if (sqlite3_prepare_v2(db->w, "PRAGMA user_version;", -1, &st, NULL) == SQLITE_OK
        && sqlite3_step(st) == SQLITE_ROW) {
        ver = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);

    n = (int)(sizeof MIGRATIONS / sizeof MIGRATIONS[0]);
    for (i = ver; i < n; i++) {
        if (db_exec_on(db->w, "BEGIN;") != 0) goto fail;
        if (db_exec_on(db->w, MIGRATIONS[i]) != 0) {
            db_exec_on(db->w, "ROLLBACK;");
            goto fail;
        }
        sprintf(bump, "PRAGMA user_version=%d;", i + 1);
        if (db_exec_on(db->w, bump) != 0 || db_exec_on(db->w, "COMMIT;") != 0) {
            db_exec_on(db->w, "ROLLBACK;");
            goto fail;
        }
    }

    return 0;

fail:
    sqlite3_close(db->w);
    db->w = NULL;
    pthread_mutex_destroy(&db->wlock);
    return -1;
}

void srv_db_close(SrvDb *db) {
    if (db->w) {
        sqlite3_close(db->w);
        db->w = NULL;
        pthread_mutex_destroy(&db->wlock);
    }
}

void srv_db_wlock  (SrvDb *db) { pthread_mutex_lock(&db->wlock); }
void srv_db_wunlock(SrvDb *db) { pthread_mutex_unlock(&db->wlock); }

int srv_db_exec(SrvDb *db, const char *sql) { return db_exec_on(db->w, sql); }

sqlite3 *srv_db_ropen(SrvDb *db) {
    sqlite3 *c = NULL;
    if (sqlite3_open_v2(db->path, &c, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        fprintf(stderr, "srv_db: ropen: %s\n", c ? sqlite3_errmsg(c) : "open_v2 failed");
        if (c) sqlite3_close(c);
        return NULL;
    }
    sqlite3_exec(c, "PRAGMA busy_timeout=5000;", NULL, NULL, NULL);
    return c;
}

void srv_db_rclose(sqlite3 *c) { if (c) sqlite3_close(c); }
