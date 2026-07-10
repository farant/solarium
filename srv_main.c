/* srv_main.c — solsrv entry point: flags, CivetWeb boot, the route table and
   HTTP handlers, and (Task 9) the admin CLI. Strict C89; binds loopback only —
   Caddy fronts it in production (spec §8). One catch-all CivetWeb handler
   ("**") dispatches through ROUTES[] so all routing lives in one table. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include <civetweb.h>

#include "srv_db.h"
#include "srv_events.h"
#include "srv_auth.h"
#include "srv_web.h"
#include "nid.h"

typedef struct SrvApp {
    SrvDb db;
    int   secure_cookies;   /* -s: append "; Secure" (production, behind Caddy) */
} SrvApp;

static volatile sig_atomic_t g_stop = 0;
static void on_stop(int sig) { (void)sig; g_stop = 1; }

static void copy_capped(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* ---- response helpers --------------------------------------------------- */

/* extra_hdr: NULL or one or more full "Name: value\r\n" lines. */
static void send_page(struct mg_connection *c, int status, const char *reason,
                      const Sb *body, const char *extra_hdr) {
    mg_printf(c, "HTTP/1.1 %d %s\r\n"
                 "Content-Type: text/html; charset=utf-8\r\n"
                 "Content-Length: %lu\r\n"
                 "Cache-Control: no-store\r\n"
                 "%s"
                 "Connection: close\r\n\r\n",
              status, reason, (unsigned long)body->len,
              extra_hdr ? extra_hdr : "");
    mg_write(c, body->buf, body->len);
}

static void send_redirect(struct mg_connection *c, const char *loc,
                          const char *set_cookie) {
    mg_printf(c, "HTTP/1.1 302 Found\r\n"
                 "Location: %s\r\n"
                 "%s"
                 "Content-Length: 0\r\nConnection: close\r\n\r\n",
              loc, set_cookie ? set_cookie : "");
}

static void send_error_page(struct mg_connection *c, int status,
                            const char *reason, const char *msg) {
    Sb b;
    sb_init(&b);
    web_page_error(&b, status, msg);
    send_page(c, status, reason, &b, NULL);
    sb_free(&b);
}

/* The request's session user id (0 if none). If token_out is non-NULL it
   receives the raw cookie token (valid or not) — logout wants it either way. */
static long req_user(struct mg_connection *c, SrvApp *app,
                     char token_out[SRV_TOKEN_CHARS + 1]) {
    const char *hdr = mg_get_header(c, "Cookie");
    char tok[SRV_TOKEN_CHARS + 1];
    if (token_out) token_out[0] = 0;
    if (!hdr) return 0;
    if (mg_get_cookie(hdr, "sid", tok, sizeof tok) < 0) return 0;
    if (token_out) strcpy(token_out, tok);
    return srv_auth_session_user(&app->db, tok);
}

/* ---- handlers ----------------------------------------------------------- */

static int h_health(struct mg_connection *c, SrvApp *app) {
    (void)app;
    mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                 "Content-Length: 3\r\nConnection: close\r\n\r\nok\n");
    return 200;
}

static int h_root(struct mg_connection *c, SrvApp *app) {
    (void)app;
    send_redirect(c, "/boards", NULL);
    return 302;
}

static int h_login_get(struct mg_connection *c, SrvApp *app) {
    Sb b;
    if (req_user(c, app, NULL) > 0) {
        send_redirect(c, "/boards", NULL);
        return 302;
    }
    sb_init(&b);
    web_page_login(&b, NULL);
    send_page(c, 200, "OK", &b, NULL);
    sb_free(&b);
    return 200;
}

static int h_login_post(struct mg_connection *c, SrvApp *app) {
    const struct mg_request_info *ri = mg_get_request_info(c);
    char body[4096], probe[1], user[64], pass[256];
    char token[SRV_TOKEN_CHARS + 1];
    char cookie[256];
    Sb b;
    int n = 0, r = 0;
    long uid;

    if (!srv_auth_throttle_ok(&app->db, ri->remote_addr)) {
        sb_init(&b);
        web_page_login(&b, "Too many attempts. Try again later.");
        send_page(c, 429, "Too Many Requests", &b, NULL);
        sb_free(&b);
        return 429;
    }

    while (n < (int)sizeof body
           && (r = mg_read(c, body + n, sizeof body - (size_t)n)) > 0) n += r;
    if (n == (int)sizeof body && mg_read(c, probe, 1) > 0) {
        send_error_page(c, 413, "Payload Too Large", "Request too large.");
        return 413;
    }

    if (mg_get_var(body, (size_t)n, "user", user, sizeof user) < 0) user[0] = 0;
    if (mg_get_var(body, (size_t)n, "pass", pass, sizeof pass) < 0) pass[0] = 0;

    uid = srv_auth_login(&app->db, user, pass, token);
    if (uid == 0) {
        srv_auth_throttle_fail(&app->db, ri->remote_addr);
        sb_init(&b);
        web_page_login(&b, "Bad username or password.");
        send_page(c, 403, "Forbidden", &b, NULL);
        sb_free(&b);
        return 403;
    }
    srv_auth_throttle_clear(&app->db, ri->remote_addr);

    sprintf(cookie, "Set-Cookie: sid=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=2592000%s\r\n",
            token, app->secure_cookies ? "; Secure" : "");
    send_redirect(c, "/boards", cookie);
    return 302;
}

static int h_logout_post(struct mg_connection *c, SrvApp *app) {
    char tok[SRV_TOKEN_CHARS + 1];
    if (req_user(c, app, tok) > 0) srv_auth_logout(&app->db, tok);
    send_redirect(c, "/login", "Set-Cookie: sid=; Path=/; HttpOnly; Max-Age=0\r\n");
    return 302;
}

static int h_boards_get(struct mg_connection *c, SrvApp *app) {
    sqlite3 *r;
    sqlite3_stmt *st = NULL;
    WebBoardRow *rows;
    Sb b;
    int n = 0;

    if (req_user(c, app, NULL) == 0) {
        send_redirect(c, "/login", NULL);
        return 302;
    }

    r = srv_db_ropen(&app->db);
    if (!r) {
        send_error_page(c, 500, "Internal Server Error", "Database unavailable.");
        return 500;
    }
    rows = (WebBoardRow *)calloc(256, sizeof *rows);
    if (!rows) {
        srv_db_rclose(r);
        send_error_page(c, 500, "Internal Server Error", "Out of memory.");
        return 500;
    }

    if (sqlite3_prepare_v2(r,
            "SELECT nid,title,updated_at FROM boards WHERE deleted=0"
            " ORDER BY updated_at DESC, nid LIMIT 256", -1, &st, NULL) == SQLITE_OK) {
        while (n < 256 && sqlite3_step(st) == SQLITE_ROW) {
            copy_capped(rows[n].nid, sizeof rows[n].nid,
                        (const char *)sqlite3_column_text(st, 0));
            copy_capped(rows[n].title, sizeof rows[n].title,
                        (const char *)sqlite3_column_text(st, 1));
            rows[n].updated_at = (long)sqlite3_column_int64(st, 2);
            n++;
        }
        sqlite3_finalize(st);
    }
    srv_db_rclose(r);

    sb_init(&b);
    web_page_boards(&b, rows, n);
    send_page(c, 200, "OK", &b, NULL);
    sb_free(&b);
    free(rows);
    return 200;
}

/* ---- route table (spec §4: method + path -> handler) -------------------- */

typedef struct Route {
    const char *method;
    const char *path;
    int (*fn)(struct mg_connection *c, SrvApp *app);
} Route;

static const Route ROUTES[] = {
    { "GET",  "/health", h_health      },
    { "GET",  "/",       h_root        },
    { "GET",  "/login",  h_login_get   },
    { "POST", "/login",  h_login_post  },
    { "POST", "/logout", h_logout_post },
    { "GET",  "/boards", h_boards_get  }
};

static int route_all(struct mg_connection *c, void *ud) {
    SrvApp *app = (SrvApp *)ud;
    const struct mg_request_info *ri = mg_get_request_info(c);
    int i;
    int n = (int)(sizeof ROUTES / sizeof ROUTES[0]);

    for (i = 0; i < n; i++) {
        if (strcmp(ri->request_method, ROUTES[i].method) == 0
            && strcmp(ri->local_uri, ROUTES[i].path) == 0) {
            return ROUTES[i].fn(c, app);
        }
    }
    send_error_page(c, 404, "Not Found", "No such page.");
    return 404;
}

/* ---- entry -------------------------------------------------------------- */

int main(int argc, char **argv) {
    static SrvApp app;              /* zero-initialized, process-lifetime */
    const char *port = "127.0.0.1:8080";
    const char *dbpath = "solsrv.db";
    struct mg_callbacks cbs;
    struct mg_context *ctx;
    const char *opts[8];
    int i, n = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) port = argv[++i];
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) dbpath = argv[++i];
        else if (strcmp(argv[i], "-s") == 0) app.secure_cookies = 1;
        else {
            fprintf(stderr, "usage: solsrv [-p host:port] [-d dbfile] [-s]\n");
            return 2;
        }
    }

    if (srv_db_open(&app.db, dbpath) != 0) return 1;

    opts[n++] = "listening_ports";    opts[n++] = port;
    opts[n++] = "num_threads";        opts[n++] = "8";
    opts[n++] = "request_timeout_ms"; opts[n++] = "10000";
    opts[n] = NULL;

    mg_init_library(0);
    memset(&cbs, 0, sizeof cbs);
    ctx = mg_start(&cbs, NULL, opts);
    if (!ctx) {
        fprintf(stderr, "solsrv: mg_start failed on %s\n", port);
        srv_db_close(&app.db);
        return 1;
    }
    mg_set_request_handler(ctx, "**", route_all, &app);

    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);
    printf("solsrv listening on %s (db %s)\n", port, dbpath);
    while (!g_stop) sleep(1);

    mg_stop(ctx);
    mg_exit_library();
    srv_db_close(&app.db);
    return 0;
}
