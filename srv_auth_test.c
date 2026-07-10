/* srv_auth_test.c — password roundtrip, session mint/validate/expire/logout,
   per-IP throttle. Uses the _iters variant with a small count so the test
   stays fast; production uses SRV_PBKDF2_ITERS. Built by `build.sh
   srvauthtest` with ASan/UBSan. */

#include "srv_auth.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DBF "srv_auth_test.db"
#define FAST_ITERS 1000u

static int g_fail = 0;
static void expect(int cond, const char *what) {
    if (!cond) { printf("FAIL: %s\n", what); g_fail = 1; }
    else printf("ok   %s\n", what);
}

int main(void) {
    SrvDb db;
    char tok[SRV_TOKEN_CHARS + 1], tok2[SRV_TOKEN_CHARS + 1];
    long uid;
    int i;

    unlink(DBF); unlink(DBF "-wal"); unlink(DBF "-shm");
    if (srv_db_open(&db, DBF) != 0) { printf("FAIL: open\n"); return 1; }

    expect(srv_auth_user_create_iters(&db, "fran", "hunter2boogaloo", FAST_ITERS) == 0,
           "create user");
    expect(srv_auth_user_create_iters(&db, "fran", "whatever12", FAST_ITERS) != 0,
           "duplicate name rejected");
    expect(srv_auth_user_create_iters(&db, "Fran!", "whatever12", FAST_ITERS) != 0,
           "bad name rejected");
    expect(srv_auth_user_create_iters(&db, "shorty", "short", FAST_ITERS) != 0,
           "short password rejected");

    uid = srv_auth_login(&db, "fran", "hunter2boogaloo", tok);
    expect(uid > 0, "login good");
    expect(strlen(tok) == SRV_TOKEN_CHARS, "token is 43 chars");
    expect(srv_auth_session_user(&db, tok) == uid, "session validates");

    expect(srv_auth_login(&db, "fran", "wrongwrong", tok2) == 0, "login bad pass");
    expect(srv_auth_login(&db, "nobody", "hunter2boogaloo", tok2) == 0, "login bad user");

    /* two sessions coexist */
    expect(srv_auth_login(&db, "fran", "hunter2boogaloo", tok2) == uid, "second login");
    expect(srv_auth_session_user(&db, tok2) == uid, "second session validates");

    srv_auth_logout(&db, tok);
    expect(srv_auth_session_user(&db, tok) == 0, "logged-out token dead");
    expect(srv_auth_session_user(&db, tok2) == uid, "other session survives");

    expect(srv_auth_session_user(&db, "notatoken") == 0, "garbage token rejected");

    /* expiry: back-date a session directly and check it dies */
    srv_db_wlock(&db);
    srv_db_exec(&db, "UPDATE sessions SET expires_at = 1;");
    srv_db_wunlock(&db);
    expect(srv_auth_session_user(&db, tok2) == 0, "expired session rejected");

    /* throttle: 10 fails trip it; clear resets */
    expect(srv_auth_throttle_ok(&db, "10.0.0.1"), "fresh ip allowed");
    for (i = 0; i < 10; i++) srv_auth_throttle_fail(&db, "10.0.0.1");
    expect(!srv_auth_throttle_ok(&db, "10.0.0.1"), "10 fails trip the throttle");
    expect(srv_auth_throttle_ok(&db, "10.0.0.2"), "other ip unaffected");
    srv_auth_throttle_clear(&db, "10.0.0.1");
    expect(srv_auth_throttle_ok(&db, "10.0.0.1"), "clear resets");

    srv_db_close(&db);
    unlink(DBF); unlink(DBF "-wal"); unlink(DBF "-shm");
    if (g_fail) { printf("srv_auth_test: FAIL\n"); return 1; }
    printf("srv_auth_test: all ok\n");
    return 0;
}
