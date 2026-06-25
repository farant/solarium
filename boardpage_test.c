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

int main(void) {
    test_slugify();
    test_collect();
    if (fails == 0) printf("boardpage_test: all passed\n");
    return fails ? 1 : 0;
}
