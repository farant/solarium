/* srv_auth.c — see srv_auth.h. */

#include "srv_auth.h"
#include "sha256.h"
#include "b64.h"
#include "srv_rand.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define SESSION_TTL      (30L * 24L * 3600L)   /* 30 days */
#define THROTTLE_WINDOW  900L                  /* 15 minutes */
#define THROTTLE_MAX     10

static int name_ok(const char *name) {
    size_t n = strlen(name);
    size_t i;
    if (n < 1 || n > 32) return 0;
    for (i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

int srv_auth_user_create_iters(SrvDb *db, const char *name,
                               const char *password, sol_u32 iters) {
    unsigned char salt[16], hash[32];
    sqlite3_stmt *st = NULL;
    int ok = 0;

    if (!name_ok(name) || strlen(password) < 8) return -1;

    srv_rand_bytes(salt, sizeof salt);
    sha256_pbkdf2(password, strlen(password), salt, sizeof salt,
                  iters, hash, sizeof hash);

    srv_db_wlock(db);
    if (sqlite3_prepare_v2(db->w,
            "INSERT INTO users(name,pw_salt,pw_hash,pw_iters,created_at)"
            " VALUES(?,?,?,?,?)", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        sqlite3_bind_blob(st, 2, salt, (int)sizeof salt, SQLITE_STATIC);
        sqlite3_bind_blob(st, 3, hash, (int)sizeof hash, SQLITE_STATIC);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)iters);
        sqlite3_bind_int64(st, 5, (sqlite3_int64)time(NULL));
        ok = sqlite3_step(st) == SQLITE_DONE;   /* UNIQUE(name) rejects dupes */
        sqlite3_finalize(st);
    }
    srv_db_wunlock(db);
    return ok ? 0 : -1;
}

int srv_auth_user_create(SrvDb *db, const char *name, const char *password) {
    return srv_auth_user_create_iters(db, name, password, SRV_PBKDF2_ITERS);
}

long srv_auth_login(SrvDb *db, const char *name, const char *password,
                    char token_out[SRV_TOKEN_CHARS + 1]) {
    static const unsigned char DUMMY_SALT[16] = {0};
    unsigned char salt[16], stored[32], calc[32], raw[32], th[32];
    sqlite3_stmt *st = NULL;
    sol_u32 iters = SRV_PBKDF2_ITERS;
    long uid = 0, now;
    int found = 0;

    /* fetch credentials under the lock… */
    srv_db_wlock(db);
    if (sqlite3_prepare_v2(db->w,
            "SELECT id,pw_salt,pw_hash,pw_iters FROM users WHERE name=?",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW
            && sqlite3_column_bytes(st, 1) == 16
            && sqlite3_column_bytes(st, 2) == 32) {
            uid = (long)sqlite3_column_int64(st, 0);
            memcpy(salt, sqlite3_column_blob(st, 1), 16);
            memcpy(stored, sqlite3_column_blob(st, 2), 32);
            iters = (sol_u32)sqlite3_column_int64(st, 3);
            found = 1;
        }
        sqlite3_finalize(st);
    }
    srv_db_wunlock(db);

    /* …but run the slow KDF OUTSIDE it: at 600k iterations it takes real
       wall-time, and holding wlock would stall every writer. Unknown user
       pays the same KDF so timing doesn't leak which names exist. */
    if (!found) {
        sha256_pbkdf2(password, strlen(password), DUMMY_SALT, 16, iters, calc, 32);
        return 0;
    }
    sha256_pbkdf2(password, strlen(password), salt, 16, iters, calc, 32);
    if (!sha256_ct_equal(calc, stored, 32)) return 0;

    /* mint the session: token to the caller, only its hash at rest */
    srv_rand_bytes(raw, sizeof raw);
    b64url_encode(raw, sizeof raw, token_out);       /* exactly 43 chars */
    sha256(token_out, SRV_TOKEN_CHARS, th);
    now = (long)time(NULL);

    srv_db_wlock(db);
    st = NULL;
    if (sqlite3_prepare_v2(db->w,
            "INSERT INTO sessions(token_hash,user_id,created_at,expires_at)"
            " VALUES(?,?,?,?)", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_blob(st, 1, th, 32, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)uid);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)now);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)(now + SESSION_TTL));
        if (sqlite3_step(st) != SQLITE_DONE) uid = 0;
        sqlite3_finalize(st);
    } else uid = 0;
    srv_db_wunlock(db);
    return uid;
}

long srv_auth_session_user(SrvDb *db, const char *token) {
    unsigned char th[32];
    sqlite3 *r;
    sqlite3_stmt *st = NULL;
    size_t tl = strlen(token);
    long uid = 0, exp = 0;

    if (tl != SRV_TOKEN_CHARS) return 0;
    sha256(token, tl, th);

    r = srv_db_ropen(db);
    if (!r) return 0;
    if (sqlite3_prepare_v2(r,
            "SELECT user_id,expires_at FROM sessions WHERE token_hash=?",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_blob(st, 1, th, 32, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            uid = (long)sqlite3_column_int64(st, 0);
            exp = (long)sqlite3_column_int64(st, 1);
        }
        sqlite3_finalize(st);
    }
    srv_db_rclose(r);

    if (uid && exp <= (long)time(NULL)) {
        srv_auth_logout(db, token);   /* lazy expiry sweep, one row at a time */
        return 0;
    }
    return uid;
}

void srv_auth_logout(SrvDb *db, const char *token) {
    unsigned char th[32];
    sqlite3_stmt *st = NULL;
    size_t tl = strlen(token);

    if (tl != SRV_TOKEN_CHARS) return;
    sha256(token, tl, th);
    srv_db_wlock(db);
    if (sqlite3_prepare_v2(db->w, "DELETE FROM sessions WHERE token_hash=?",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_blob(st, 1, th, 32, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    srv_db_wunlock(db);
}

int srv_auth_throttle_ok(SrvDb *db, const char *ip) {
    sqlite3 *r;
    sqlite3_stmt *st = NULL;
    long fails = 0, start = 0;
    int seen = 0;

    r = srv_db_ropen(db);
    if (!r) return 0;   /* fail closed: no read path, no login attempts */
    if (sqlite3_prepare_v2(r,
            "SELECT fails,window_start FROM login_attempts WHERE ip=?",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, ip, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            fails = (long)sqlite3_column_int64(st, 0);
            start = (long)sqlite3_column_int64(st, 1);
            seen = 1;
        }
        sqlite3_finalize(st);
    }
    srv_db_rclose(r);

    if (!seen) return 1;
    if (start + THROTTLE_WINDOW < (long)time(NULL)) return 1;   /* window lapsed */
    return fails < THROTTLE_MAX;
}

/* NOTE: the 900 inside the upsert SQL is THROTTLE_WINDOW — SQL can't read the
   macro; change both together. */
void srv_auth_throttle_fail(SrvDb *db, const char *ip) {
    sqlite3_stmt *st = NULL;
    srv_db_wlock(db);
    if (sqlite3_prepare_v2(db->w,
            "INSERT INTO login_attempts(ip,fails,window_start) VALUES(?,1,?)"
            " ON CONFLICT(ip) DO UPDATE SET"
            "  fails = CASE WHEN login_attempts.window_start + 900 < excluded.window_start"
            "               THEN 1 ELSE login_attempts.fails + 1 END,"
            "  window_start = CASE WHEN login_attempts.window_start + 900 < excluded.window_start"
            "               THEN excluded.window_start ELSE login_attempts.window_start END",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, ip, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)time(NULL));
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    srv_db_wunlock(db);
}

void srv_auth_throttle_clear(SrvDb *db, const char *ip) {
    sqlite3_stmt *st = NULL;
    srv_db_wlock(db);
    if (sqlite3_prepare_v2(db->w, "DELETE FROM login_attempts WHERE ip=?",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, ip, -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    srv_db_wunlock(db);
}
