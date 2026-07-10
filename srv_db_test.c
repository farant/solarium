/* srv_db_test.c — open/migrate on a scratch file, WAL mode active, migration
   idempotent across reopen, read conn sees the writer's committed rows.
   Built by `build.sh srvdbtest` with ASan/UBSan. */

#include "srv_db.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DBF "srv_db_test.db"

static int g_fail = 0;
static void expect(int cond, const char *what) {
    if (!cond) { printf("FAIL: %s\n", what); g_fail = 1; }
    else printf("ok   %s\n", what);
}

static int table_exists(sqlite3 *c, const char *name) {
    sqlite3_stmt *st = NULL;
    int found = 0;
    sqlite3_prepare_v2(c, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", -1, &st, NULL);
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) found = 1;
    sqlite3_finalize(st);
    return found;
}

int main(void) {
    SrvDb db;
    sqlite3 *r;
    sqlite3_stmt *st;
    int rc;

    unlink(DBF); unlink(DBF "-wal"); unlink(DBF "-shm");

    expect(srv_db_open(&db, DBF) == 0, "open + migrate");
    expect(table_exists(db.w, "events"), "events table");
    expect(table_exists(db.w, "boards"), "boards table");
    expect(table_exists(db.w, "users"), "users table");
    expect(table_exists(db.w, "sessions"), "sessions table");
    expect(table_exists(db.w, "login_attempts"), "login_attempts table");

    /* WAL is active */
    st = NULL;
    sqlite3_prepare_v2(db.w, "PRAGMA journal_mode;", -1, &st, NULL);
    sqlite3_step(st);
    expect(strcmp((const char *)sqlite3_column_text(st, 0), "wal") == 0, "journal_mode=wal");
    sqlite3_finalize(st);

    /* writer inserts under the lock; a read conn sees it */
    srv_db_wlock(&db);
    rc = srv_db_exec(&db, "INSERT INTO boards(nid,title,deleted,updated_at,updated_by)"
                          " VALUES('TESTNID','t',0,1,0);");
    srv_db_wunlock(&db);
    expect(rc == 0, "insert via write conn");

    r = srv_db_ropen(&db);
    expect(r != NULL, "ropen");
    st = NULL;
    sqlite3_prepare_v2(r, "SELECT title FROM boards WHERE nid='TESTNID'", -1, &st, NULL);
    expect(sqlite3_step(st) == SQLITE_ROW, "read conn sees committed row");
    sqlite3_finalize(st);

    /* read conn is really read-only */
    expect(sqlite3_exec(r, "INSERT INTO boards(nid,title,deleted,updated_at,updated_by)"
                           " VALUES('X','x',0,1,0);", NULL, NULL, NULL) != SQLITE_OK,
           "read conn rejects writes");
    srv_db_rclose(r);

    /* reopen: migrations are idempotent (user_version gates them) */
    srv_db_close(&db);
    expect(srv_db_open(&db, DBF) == 0, "reopen (no re-migrate error)");
    srv_db_close(&db);

    unlink(DBF); unlink(DBF "-wal"); unlink(DBF "-shm");
    if (g_fail) { printf("srv_db_test: FAIL\n"); return 1; }
    printf("srv_db_test: all ok\n");
    return 0;
}
