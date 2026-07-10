/* srv_web_test.c — the pure HTML layer: builder growth, escape correctness,
   page shape (form action, escaped titles, empty state). No server, no DB.
   Built by `build.sh srvwebtest` with ASan/UBSan. */

#include "srv_web.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;
static void expect(int cond, const char *what) {
    if (!cond) { printf("FAIL: %s\n", what); g_fail = 1; }
    else printf("ok   %s\n", what);
}

int main(void) {
    Sb s;
    WebBoardRow rows[2];
    int i;

    /* builder grows past its initial capacity */
    sb_init(&s);
    for (i = 0; i < 1000; i++) sb_puts(&s, "0123456789");
    expect(s.len == 10000 && strlen(s.buf) == 10000, "sb grows to 10k");
    sb_free(&s);

    /* escape: every special, embedded in text */
    sb_init(&s);
    sb_put_html(&s, "a<b>&\"c'd");
    expect(strcmp(s.buf, "a&lt;b&gt;&amp;&quot;c&#39;d") == 0, "html escape");
    sb_free(&s);

    sb_init(&s);
    sb_put_long(&s, -12345L);
    expect(strcmp(s.buf, "-12345") == 0, "sb_put_long");
    sb_free(&s);

    /* login page */
    sb_init(&s);
    web_page_login(&s, NULL);
    expect(strstr(s.buf, "<form method=\"post\" action=\"/login\">") != NULL,
           "login form present");
    expect(strstr(s.buf, "class=\"err\"") == NULL, "no error block by default");
    sb_free(&s);

    sb_init(&s);
    web_page_login(&s, "Bad username or password.");
    expect(strstr(s.buf, "Bad username or password.") != NULL, "error text shown");
    sb_free(&s);

    /* boards page: titles escaped, logout form present */
    memset(rows, 0, sizeof rows);
    strcpy(rows[0].nid, "AAAAAAAAAAAAAAAAAAAAAAAAAA");
    strcpy(rows[0].title, "<script>alert(1)</script>");
    rows[0].updated_at = 100;
    strcpy(rows[1].nid, "BBBBBBBBBBBBBBBBBBBBBBBBBB");
    strcpy(rows[1].title, "Reading & writing");
    rows[1].updated_at = 200;

    sb_init(&s);
    web_page_boards(&s, rows, 2);
    expect(strstr(s.buf, "<script>alert(1)</script>") == NULL, "script tag not raw");
    expect(strstr(s.buf, "&lt;script&gt;") != NULL, "script tag escaped");
    expect(strstr(s.buf, "Reading &amp; writing") != NULL, "ampersand escaped");
    expect(strstr(s.buf, "action=\"/logout\"") != NULL, "logout form present");
    sb_free(&s);

    /* empty state */
    sb_init(&s);
    web_page_boards(&s, rows, 0);
    expect(strstr(s.buf, "No boards yet") != NULL, "empty state");
    sb_free(&s);

    /* error page */
    sb_init(&s);
    web_page_error(&s, 404, "No such page.");
    expect(strstr(s.buf, "404") != NULL && strstr(s.buf, "No such page.") != NULL,
           "error page");
    sb_free(&s);

    if (g_fail) { printf("srv_web_test: FAIL\n"); return 1; }
    printf("srv_web_test: all ok\n");
    return 0;
}
