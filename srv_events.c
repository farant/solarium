/* srv_events.c — see srv_events.h. */

#include "srv_events.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static char *xstrdup(const char *s) {
    char *d;
    if (!s) return NULL;
    d = (char *)malloc(strlen(s) + 1);
    if (d) strcpy(d, s);
    return d;
}

static void copy_capped(char *dst, size_t cap, const char *src) {
    size_t n;
    if (!src) { dst[0] = 0; return; }
    n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* Project one board event into the boards table. Caller holds wlock and an
   open transaction. Returns 0 ok. Update touches only fields present in the
   payload; delete is a tombstone (deleted=1), never a row removal — the web
   list filters on it and sync history stays intact. */
static int project_board(SrvDb *db, const char *nid, const char *op,
                         const char *payload, long ts, long actor) {
    sqlite3_stmt *st = NULL;
    JsonValue *v = NULL;
    const char *title = NULL;
    int rc = SQLITE_ERROR;

    if (strcmp(op, "delete") == 0) {
        if (sqlite3_prepare_v2(db->w,
                "UPDATE boards SET deleted=1, updated_at=?, updated_by=? WHERE nid=?",
                -1, &st, NULL) != SQLITE_OK) return -1;
        sqlite3_bind_int64(st, 1, (sqlite3_int64)ts);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)actor);
        sqlite3_bind_text(st, 3, nid, -1, SQLITE_STATIC);
        rc = sqlite3_step(st);
        sqlite3_finalize(st);
        return rc == SQLITE_DONE ? 0 : -1;
    }

    v = json_parse(payload);
    title = json_string(json_member(v, "title"));

    if (strcmp(op, "create") == 0) {
        if (sqlite3_prepare_v2(db->w,
                "INSERT INTO boards(nid,title,deleted,updated_at,updated_by)"
                " VALUES(?,?,0,?,?)"
                " ON CONFLICT(nid) DO UPDATE SET title=excluded.title, deleted=0,"
                "   updated_at=excluded.updated_at, updated_by=excluded.updated_by",
                -1, &st, NULL) != SQLITE_OK) { json_free(v); return -1; }
        sqlite3_bind_text(st, 1, nid, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, title ? title : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)ts);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)actor);
    } else if (title) {   /* update, title present */
        if (sqlite3_prepare_v2(db->w,
                "UPDATE boards SET title=?, updated_at=?, updated_by=? WHERE nid=?",
                -1, &st, NULL) != SQLITE_OK) { json_free(v); return -1; }
        sqlite3_bind_text(st, 1, title, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)ts);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)actor);
        sqlite3_bind_text(st, 4, nid, -1, SQLITE_STATIC);
    } else {              /* update, nothing we project — bump attribution only */
        if (sqlite3_prepare_v2(db->w,
                "UPDATE boards SET updated_at=?, updated_by=? WHERE nid=?",
                -1, &st, NULL) != SQLITE_OK) { json_free(v); return -1; }
        sqlite3_bind_int64(st, 1, (sqlite3_int64)ts);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)actor);
        sqlite3_bind_text(st, 3, nid, -1, SQLITE_STATIC);
    }
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    json_free(v);
    return rc == SQLITE_DONE ? 0 : -1;
}

int srv_events_append(SrvDb *db, const SrvEventIn *in, long *out_id) {
    sqlite3_stmt *st = NULL;
    long ts;
    long new_id = 0;
    int ok = 0;

    ts = in->ts ? in->ts : (long)time(NULL);

    srv_db_wlock(db);
    if (srv_db_exec(db, "BEGIN IMMEDIATE;") != 0) { srv_db_wunlock(db); return -1; }

    if (sqlite3_prepare_v2(db->w,
            "INSERT INTO events(ts,actor_id,origin_device,entity_kind,entity_nid,op,payload)"
            " VALUES(?,?,?,?,?,?,?)", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)ts);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)in->actor_id);
        sqlite3_bind_text(st, 3, in->origin_device ? in->origin_device : "", -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 4, in->entity_kind, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 5, in->entity_nid, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 6, in->op, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 7, in->payload ? in->payload : "{}", -1, SQLITE_STATIC);
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        /* Capture the event id immediately after the insert succeeds, before projection
           tables are touched — a future rowid-projected table can't clobber it. */
        if (ok) new_id = (long)sqlite3_last_insert_rowid(db->w);
    }

    if (ok && strcmp(in->entity_kind, "board") == 0) {
        ok = project_board(db, in->entity_nid, in->op,
                           in->payload ? in->payload : "{}", ts, in->actor_id) == 0;
    }

    if (ok) {
        ok = srv_db_exec(db, "COMMIT;") == 0;
    }
    if (!ok) srv_db_exec(db, "ROLLBACK;");
    if (ok) *out_id = new_id;
    srv_db_wunlock(db);
    return ok ? 0 : -1;
}

int srv_events_after(SrvDb *db, long cursor, int max,
                     SrvEventOut **out, int *count) {
    sqlite3 *r;
    sqlite3_stmt *st = NULL;
    SrvEventOut *evs;
    int n = 0;

    *out = NULL;
    *count = 0;
    if (max <= 0) return 0;

    r = srv_db_ropen(db);
    if (!r) return -1;

    evs = (SrvEventOut *)calloc((size_t)max, sizeof *evs);
    if (!evs) { srv_db_rclose(r); return -1; }

    if (sqlite3_prepare_v2(r,
            "SELECT id,ts,actor_id,origin_device,entity_kind,entity_nid,op,payload"
            " FROM events WHERE id > ? ORDER BY id LIMIT ?", -1, &st, NULL) != SQLITE_OK) {
        free(evs);
        srv_db_rclose(r);
        return -1;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)cursor);
    sqlite3_bind_int(st, 2, max);

    while (n < max && sqlite3_step(st) == SQLITE_ROW) {
        SrvEventOut *e = &evs[n];
        e->id       = (long)sqlite3_column_int64(st, 0);
        e->ts       = (long)sqlite3_column_int64(st, 1);
        e->actor_id = (long)sqlite3_column_int64(st, 2);
        copy_capped(e->origin_device, sizeof e->origin_device,
                    (const char *)sqlite3_column_text(st, 3));
        copy_capped(e->entity_kind, sizeof e->entity_kind,
                    (const char *)sqlite3_column_text(st, 4));
        copy_capped(e->entity_nid, sizeof e->entity_nid,
                    (const char *)sqlite3_column_text(st, 5));
        copy_capped(e->op, sizeof e->op,
                    (const char *)sqlite3_column_text(st, 6));
        e->payload = xstrdup((const char *)sqlite3_column_text(st, 7));
        if (!e->payload) break;
        n++;
    }
    sqlite3_finalize(st);
    srv_db_rclose(r);

    *out = evs;
    *count = n;
    return 0;
}

void srv_events_free(SrvEventOut *evs, int count) {
    int i;
    if (!evs) return;
    for (i = 0; i < count; i++) free(evs[i].payload);
    free(evs);
}
