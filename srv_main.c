/* srv_main.c — solsrv entry point: flags, CivetWeb boot, the route table.
   Sub-project 1 of the server arc (docs/superpowers/specs/
   2026-07-01-server-sync-web-mcp-architecture-design.md). Strict C89; binds
   loopback only — Caddy fronts it in production. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include <civetweb.h>

static volatile sig_atomic_t g_stop = 0;
static void on_stop(int sig) { (void)sig; g_stop = 1; }

static int h_health(struct mg_connection *c, void *ud) {
    (void)ud;
    mg_printf(c, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                 "Content-Length: 3\r\nConnection: close\r\n\r\nok\n");
    return 200;
}

int main(int argc, char **argv) {
    const char *port = "127.0.0.1:8080";
    const char *dbpath = "solsrv.db";
    struct mg_callbacks cbs;
    struct mg_context *ctx;
    const char *opts[8];
    int i, n = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) port = argv[++i];
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) dbpath = argv[++i];
        else {
            fprintf(stderr, "usage: solsrv [-p host:port] [-d dbfile]\n");
            return 2;
        }
    }
    (void)dbpath;   /* used from Task 8 on */

    opts[n++] = "listening_ports";    opts[n++] = port;
    opts[n++] = "num_threads";        opts[n++] = "8";
    opts[n++] = "request_timeout_ms"; opts[n++] = "10000";
    opts[n] = NULL;

    mg_init_library(0);
    memset(&cbs, 0, sizeof cbs);
    ctx = mg_start(&cbs, NULL, opts);
    if (!ctx) { fprintf(stderr, "solsrv: mg_start failed on %s\n", port); return 1; }
    mg_set_request_handler(ctx, "/health", h_health, NULL);

    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);
    printf("solsrv listening on %s\n", port);
    while (!g_stop) sleep(1);

    mg_stop(ctx);
    mg_exit_library();
    return 0;
}
