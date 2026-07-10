/* srv_web.c — see srv_web.h. */

#include "srv_web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sb_init(Sb *s) {
    s->cap = 256;
    s->len = 0;
    s->buf = (char *)malloc(s->cap);
    if (!s->buf) abort();
    s->buf[0] = 0;
}

void sb_free(Sb *s) {
    free(s->buf);
    s->buf = NULL;
    s->len = s->cap = 0;
}

static void sb_grow(Sb *s, size_t need) {
    char *nb;
    if (s->len + need + 1 <= s->cap) return;
    while (s->cap < s->len + need + 1) s->cap *= 2;
    nb = (char *)realloc(s->buf, s->cap);
    if (!nb) abort();
    s->buf = nb;
}

void sb_puts(Sb *s, const char *str) {
    size_t n = strlen(str);
    sb_grow(s, n);
    memcpy(s->buf + s->len, str, n);
    s->len += n;
    s->buf[s->len] = 0;
}

void sb_put_html(Sb *s, const char *str) {
    const char *p;
    char one[2];
    one[1] = 0;
    for (p = str; *p; p++) {
        switch (*p) {
        case '&':  sb_puts(s, "&amp;");  break;
        case '<':  sb_puts(s, "&lt;");   break;
        case '>':  sb_puts(s, "&gt;");   break;
        case '"':  sb_puts(s, "&quot;"); break;
        case '\'': sb_puts(s, "&#39;");  break;
        default:   one[0] = *p; sb_puts(s, one); break;
        }
    }
}

void sb_put_long(Sb *s, long v) {
    char buf[32];
    sprintf(buf, "%ld", v);
    sb_puts(s, buf);
}

/* ---- pages ------------------------------------------------------------- */

static void page_open(Sb *o, const char *title) {
    sb_puts(o,
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>");
    sb_put_html(o, title);
    sb_puts(o,
        " — Solarium</title><style>"
        "body{font-family:Georgia,serif;background:#1a1714;color:#e8e0d0;"
        "max-width:40rem;margin:2rem auto;padding:0 1rem}"
        "a{color:#d4a94f}"
        "h1{font-weight:normal;border-bottom:1px solid #3a342c;padding-bottom:.4rem}"
        "li{margin:.4rem 0}"
        "form.inline{display:inline}"
        "input{background:#26221c;color:#e8e0d0;border:1px solid #3a342c;"
        "padding:.4rem;font:inherit}"
        "button{background:#3a342c;color:#e8e0d0;border:0;padding:.4rem 1rem;"
        "font:inherit;cursor:pointer}"
        ".err{color:#d46a4f}.muted{color:#8a8274;font-size:.9rem}"
        "</style></head><body>");
}

static void page_close(Sb *o) {
    sb_puts(o, "</body></html>");
}

void web_page_login(Sb *out, const char *err_msg) {
    page_open(out, "Sign in");
    sb_puts(out, "<h1>Solarium</h1>");
    if (err_msg) {
        sb_puts(out, "<p class=\"err\">");
        sb_put_html(out, err_msg);
        sb_puts(out, "</p>");
    }
    sb_puts(out,
        "<form method=\"post\" action=\"/login\">"
        "<p><input name=\"user\" placeholder=\"user\" autofocus></p>"
        "<p><input name=\"pass\" type=\"password\" placeholder=\"password\"></p>"
        "<p><button>Sign in</button></p>"
        "</form>");
    page_close(out);
}

void web_page_boards(Sb *out, const WebBoardRow *rows, int n) {
    int i;
    page_open(out, "Boards");
    sb_puts(out,
        "<h1>Boards</h1>"
        "<form class=\"inline\" method=\"post\" action=\"/logout\">"
        "<button>Sign out</button></form>");
    if (n == 0) {
        sb_puts(out, "<p class=\"muted\">No boards yet — the app syncs them here.</p>");
    } else {
        sb_puts(out, "<ul>");
        for (i = 0; i < n; i++) {
            sb_puts(out, "<li>");
            sb_put_html(out, rows[i].title[0] ? rows[i].title : "(untitled)");
            sb_puts(out, " <span class=\"muted\">");
            sb_put_html(out, rows[i].nid);
            sb_puts(out, "</span></li>");
        }
        sb_puts(out, "</ul>");
    }
    page_close(out);
}

void web_page_error(Sb *out, int status, const char *msg) {
    page_open(out, "Error");
    sb_puts(out, "<h1>");
    sb_put_long(out, (long)status);
    sb_puts(out, "</h1><p>");
    sb_put_html(out, msg);
    sb_puts(out, "</p><p><a href=\"/boards\">Back to boards</a></p>");
    page_close(out);
}
