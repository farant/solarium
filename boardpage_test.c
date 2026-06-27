#include "boardpage.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

static void test_slugify(void) {
    char b[PAGE_SLUG_CAP];
    int  n;
    n = boardpage_slugify("Chapter 1: Notes!", b, sizeof b);
    CHECK(strcmp(b, "/chapter-1-notes") == 0, "slug: spaces+punct -> dashes");
    CHECK(n == (int)strlen(b), "slug: returns length");
    boardpage_slugify("  trailing  ", b, sizeof b);
    CHECK(strcmp(b, "/trailing") == 0, "slug: trims edges, no trailing dash");
    boardpage_slugify("a///b", b, sizeof b);
    CHECK(strcmp(b, "/a-b") == 0, "slug: internal slashes -> single dash");
    boardpage_slugify("/already-slug", b, sizeof b);
    CHECK(strcmp(b, "/already-slug") == 0, "slug: idempotent on a clean slug");
    n = boardpage_slugify("!!!", b, sizeof b);
    CHECK(strcmp(b, "/") == 0 && n == 1, "slug: all-punct -> '/' (cancel)");
    n = boardpage_slugify("", b, sizeof b);
    CHECK(strcmp(b, "/") == 0 && n == 1, "slug: empty -> '/' (cancel)");
    boardpage_slugify("UPPER Mixed", b, sizeof b);
    CHECK(strcmp(b, "/upper-mixed") == 0, "slug: lowercases");
    {
        char small[5];
        boardpage_slugify("ab cd", small, 5);
        CHECK(small[(int)sizeof small - 2] != '-', "slug: no trailing dash on truncation");
    }
}

static void test_collect(void) {
    char out[BOARD_PAGE_MAX][PAGE_SLUG_CAP];
    const char *raw[4];
    int n;
    raw[0] = "/beta"; raw[1] = "/alpha"; raw[2] = "/beta"; raw[3] = "/";
    n = boardpage_collect(raw, 4, "/alpha", out, BOARD_PAGE_MAX);
    CHECK(n == 3, "collect: dedupes to 3 (/, /alpha, /beta)");
    CHECK(strcmp(out[0], "/") == 0, "collect: '/' sorts first");
    CHECK(strcmp(out[1], "/alpha") == 0, "collect: then ascending");
    CHECK(strcmp(out[2], "/beta") == 0, "collect: ascending");
    raw[0] = "/x";
    n = boardpage_collect(raw, 1, "/zzz", out, BOARD_PAGE_MAX);
    CHECK(n == 3, "collect: active + '/' always present");
    CHECK(strcmp(out[0], "/") == 0, "collect: '/' first even when not raw");
    n = boardpage_collect((const char *const *)0, 0, (const char *)0, out, BOARD_PAGE_MAX);
    CHECK(n == 1 && strcmp(out[0], "/") == 0, "collect: empty -> just '/'");
}

static void test_order_and_list(void) {
    const char *raw[3];
    char out[BOARD_PAGE_MAX][PAGE_SLUG_CAP];
    char ser[BOARD_PAGE_MAX * 8];
    int  n;

    raw[0] = "/page-10"; raw[1] = "/page-2"; raw[2] = "/page-1";
    n = boardpage_collect(raw, 3, "/", out, BOARD_PAGE_MAX);
    CHECK(n == 4, "natural: 4 pages");
    CHECK(strcmp(out[0], "/") == 0,        "natural: '/' first");
    CHECK(strcmp(out[1], "/page-1") == 0,  "natural: page-1");
    CHECK(strcmp(out[2], "/page-2") == 0,  "natural: page-2 before page-10");
    CHECK(strcmp(out[3], "/page-10") == 0, "natural: page-10 last");

    n = boardpage_list("/page-3 /page-1 /page-2", "/page-1", out, BOARD_PAGE_MAX);
    CHECK(n == 4, "list: 4 pages");
    CHECK(strcmp(out[0], "/") == 0,       "list: '/' first");
    CHECK(strcmp(out[1], "/page-3") == 0, "list: order preserved (3)");
    CHECK(strcmp(out[2], "/page-1") == 0, "list: order preserved (1)");
    CHECK(strcmp(out[3], "/page-2") == 0, "list: order preserved (2)");

    n = boardpage_list("/page-1", "/page-9", out, BOARD_PAGE_MAX);
    CHECK(n == 3 && strcmp(out[2], "/page-9") == 0, "list: missing active appended");

    CHECK(boardpage_contains("/page-1 /page-10", "/page-1") == 1, "contains: token present");
    CHECK(boardpage_contains("/page-10", "/page-1") == 0,         "contains: not a substring");
    CHECK(boardpage_contains("", "/page-1") == 0,                 "contains: empty list");
    CHECK(boardpage_contains((const char *)0, "/x") == 0, "contains: NULL list");
    { char o2[BOARD_PAGE_MAX][PAGE_SLUG_CAP]; int m = boardpage_list((const char *)0, "/page-1", o2, BOARD_PAGE_MAX);
      CHECK(m == 2 && strcmp(o2[1], "/page-1") == 0, "list: NULL stored -> '/' + active"); }
    { const char *r2[2]; char o3[BOARD_PAGE_MAX][PAGE_SLUG_CAP]; int m;
      r2[0] = "/a1"; r2[1] = "/a"; m = boardpage_collect(r2, 2, "/", o3, BOARD_PAGE_MAX);
      CHECK(m == 3 && strcmp(o3[1], "/a") == 0 && strcmp(o3[2], "/a1") == 0, "natural: /a before /a1"); }

    n = boardpage_list("/page-3 /page-1", "/", out, BOARD_PAGE_MAX);
    boardpage_serialize(out, n, ser, (int)sizeof ser);
    CHECK(strcmp(ser, "/page-3 /page-1") == 0, "serialize: '/' skipped, order kept");
}

int main(void) {
    test_slugify();
    test_collect();
    test_order_and_list();
    if (fails == 0) printf("boardpage_test: all passed\n");
    return fails ? 1 : 0;
}
