/* srv_events_test.c — append is transactional and projects boards; unknown
   kinds are logged but not projected; query-after-cursor pages in id order.
   Built by `build.sh srveventstest` with ASan/UBSan. */

#include "srv_events.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DBF "srv_events_test.db"

static int g_fail = 0;
static void expect(int cond, const char *what) {
    if (!cond) { printf("FAIL: %s\n", what); g_fail = 1; }
    else printf("ok   %s\n", what);
}

static long append(SrvDb *db, const char *nid, const char *op, const char *payload) {
    SrvEventIn ev;
    long id = 0;
    memset(&ev, 0, sizeof ev);
    ev.actor_id = 7;
    ev.origin_device = "testdev";
    ev.entity_kind = "board";
    ev.entity_nid = nid;
    ev.op = op;
    ev.payload = payload;
    if (srv_events_append(db, &ev, &id) != 0) return -1;
    return id;
}

static int board_title_is(SrvDb *db, const char *nid, const char *want, int want_deleted) {
    sqlite3 *r = srv_db_ropen(db);
    sqlite3_stmt *st = NULL;
    int ok = 0;
    sqlite3_prepare_v2(r, "SELECT title, deleted FROM boards WHERE nid=?", -1, &st, NULL);
    sqlite3_bind_text(st, 1, nid, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        ok = strcmp((const char *)sqlite3_column_text(st, 0), want) == 0
          && sqlite3_column_int(st, 1) == want_deleted;
    }
    sqlite3_finalize(st);
    srv_db_rclose(r);
    return ok;
}

int main(void) {
    SrvDb db;
    SrvEventOut *evs = NULL;
    SrvEventIn bad;
    int count = 0;
    long id1, id2, id3, id4, idu;

    unlink(DBF); unlink(DBF "-wal"); unlink(DBF "-shm");
    if (srv_db_open(&db, DBF) != 0) { printf("FAIL: open\n"); return 1; }

    id1 = append(&db, "AAAAAAAAAAAAAAAAAAAAAAAAAA", "create", "{\"title\":\"Alpha\"}");
    id2 = append(&db, "BBBBBBBBBBBBBBBBBBBBBBBBBB", "create", "{\"title\":\"Beta\"}");
    id3 = append(&db, "AAAAAAAAAAAAAAAAAAAAAAAAAA", "update", "{\"title\":\"Alpha 2\"}");
    id4 = append(&db, "BBBBBBBBBBBBBBBBBBBBBBBBBB", "delete", "{}");
    expect(id1 > 0 && id2 > id1 && id3 > id2 && id4 > id3, "ids ascend");

    expect(board_title_is(&db, "AAAAAAAAAAAAAAAAAAAAAAAAAA", "Alpha 2", 0),
           "A projected: title updated, not deleted");
    expect(board_title_is(&db, "BBBBBBBBBBBBBBBBBBBBBBBBBB", "Beta", 1),
           "B projected: deleted, title kept");

    /* update with no title field must not clobber the title */
    idu = append(&db, "AAAAAAAAAAAAAAAAAAAAAAAAAA", "update", "{\"other\":1}");
    expect(idu > id4, "titleless update appended");
    expect(board_title_is(&db, "AAAAAAAAAAAAAAAAAAAAAAAAAA", "Alpha 2", 0),
           "titleless update keeps title");

    /* unknown entity kind: logged, not projected, no error */
    memset(&bad, 0, sizeof bad);
    bad.actor_id = 7;
    bad.origin_device = "testdev";
    bad.entity_kind = "gizmo";
    bad.entity_nid = "CCCCCCCCCCCCCCCCCCCCCCCCCC";
    bad.op = "create";
    bad.payload = "{}";
    {
        long idg = 0;
        expect(srv_events_append(&db, &bad, &idg) == 0 && idg > idu,
               "unknown kind appends fine");
    }

    /* cursor query */
    expect(srv_events_after(&db, 0, 100, &evs, &count) == 0 && count == 6,
           "after(0) returns all 6");
    expect(evs[0].id == id1 && strcmp(evs[0].op, "create") == 0
        && strcmp(evs[0].origin_device, "testdev") == 0
        && strcmp(evs[0].payload, "{\"title\":\"Alpha\"}") == 0,
           "first event roundtrips");
    srv_events_free(evs, count);

    expect(srv_events_after(&db, id2, 100, &evs, &count) == 0 && count == 4
        && evs[0].id == id3,
           "after(id2) starts at id3");
    srv_events_free(evs, count);

    expect(srv_events_after(&db, 0, 2, &evs, &count) == 0 && count == 2,
           "max caps the page");
    srv_events_free(evs, count);

    srv_db_close(&db);
    unlink(DBF); unlink(DBF "-wal"); unlink(DBF "-shm");
    if (g_fail) { printf("srv_events_test: FAIL\n"); return 1; }
    printf("srv_events_test: all ok\n");
    return 0;
}
