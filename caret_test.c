/* caret_test.c — pure-logic test for caret.c (the note-caret layout math).
   GL-free, ASan/UBSan via `build.sh carettest`. Feeds synthetic advances so no
   Font is needed. */

#include "caret.h"

#include <stdio.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)

/* unit advance per byte: 1.0 at each lead byte, 0.0 at continuation bytes/'\n'. */
static void unit_adv(const char *wrapped, int wlen, float *adv) {
    int i = 0;
    while (i < wlen) {
        int n = caret_cplen((unsigned char)wrapped[i]);
        adv[i] = (wrapped[i] == '\n') ? 0.0f : 1.0f;
        for (++i; n > 1 && i < wlen; n--, i++) adv[i] = 0.0f;
    }
}

static int build(const char *src, const char *wrapped, CaretField *cf) {
    int   map[CARET_MAX_SLOTS];
    float adv[CARET_MAX_SLOTS];
    int   wlen = caret_reconcile(src, wrapped, map, CARET_MAX_SLOTS);
    unit_adv(wrapped, wlen, adv);
    return caret_field_build(src, wrapped, map, adv, wlen, 1.0f, 1.0f, cf);
}

static void test_cplen(void) {
    CHECK(caret_cplen((unsigned char)'a') == 1, "cplen ascii=1");
    CHECK(caret_cplen(0xC3u) == 2, "cplen 2-byte lead");
    CHECK(caret_cplen(0xE2u) == 3, "cplen 3-byte lead");
    CHECK(caret_cplen(0x80u) == 1, "cplen stray continuation -> 1 (progress)");
}

static void test_reconcile_plain(void) {
    int map[16], n = caret_reconcile("hello", "hello", map, 16), i;
    CHECK(n == 5, "reconcile plain: len");
    for (i = 0; i < 5; i++) CHECK(map[i] == i, "reconcile plain: identity offsets");
}

static void test_reconcile_softwrap(void) {
    /* "ab cd" wrapped to "ab\ncd": the space (src 2) collapses into the '\n' */
    int map[16], n = caret_reconcile("ab cd", "ab\ncd", map, 16);
    CHECK(n == 5, "reconcile softwrap: len");
    CHECK(map[0] == 0 && map[1] == 1, "reconcile softwrap: ab -> 0,1");
    CHECK(map[2] == 2, "reconcile softwrap: '\\n' maps to the collapsed space (2)");
    CHECK(map[3] == 3 && map[4] == 4, "reconcile softwrap: cd -> 3,4 (post-space)");
}

static void test_reconcile_multispace(void) {
    /* "ab   cd" -> "ab\ncd": three spaces (2,3,4) collapse to one '\n' */
    int map[16], n = caret_reconcile("ab   cd", "ab\ncd", map, 16);
    CHECK(n == 5, "reconcile multispace: len");
    CHECK(map[2] == 2, "reconcile multispace: '\\n' at first collapsed space");
    CHECK(map[3] == 5 && map[4] == 6, "reconcile multispace: cd -> 5,6 (past 3 spaces)");
}

static void test_reconcile_hard_nl(void) {
    int map[16], n = caret_reconcile("a\nb", "a\nb", map, 16);
    CHECK(n == 3 && map[0] == 0 && map[1] == 1 && map[2] == 2, "reconcile hard '\\n' passes through");
}

static void test_field_plain(void) {
    CaretField cf;
    int lines = build("abc", "abc", &cf);
    CHECK(lines == 1, "field plain: one line");
    /* slots: leading(0) + after a,b,c => 4 slots at x 0,1,2,3, src 0,1,2,3 */
    CHECK(cf.slot_count == 4, "field plain: 4 slots");
    CHECK(cf.slots[0].src == 0 && cf.slots[0].x == 0.0f, "field plain: leading slot");
    CHECK(cf.slots[3].src == 3 && cf.slots[3].x == 3.0f, "field plain: end slot at x=3,src=3");
    CHECK(caret_slot_for_offset(&cf, 2) == 2, "field plain: offset 2 -> slot 2");
    CHECK(caret_line_of_slot(&cf, 3) == 0, "field plain: slot 3 on line 0");
    CHECK(caret_slot_nearest_x(&cf, 0, 1.4f) == 1, "field plain: x~1.4 -> slot 1");
    CHECK(caret_slot_nearest_x(&cf, 0, 1.6f) == 2, "field plain: x~1.6 -> slot 2");
    CHECK(caret_slot_nearest_x(&cf, -1, 1.0f) == -1, "nearest_x: line -1 -> -1");
    CHECK(caret_slot_nearest_x(&cf, 99, 1.0f) == -1, "nearest_x: line OOB -> -1");
}

static void test_field_softwrap(void) {
    CaretField cf;
    int lines = build("ab cd", "ab\ncd", &cf);
    CHECK(lines == 2, "field softwrap: two lines");
    CHECK(cf.lines[1].line == 1, "field softwrap: second line index");
    /* line 1 leading slot src = first char after the collapsed space = 3 */
    {
        int s0 = cf.lines[1].slot0;
        CHECK(cf.slots[s0].src == 3 && cf.slots[s0].x == 0.0f, "field softwrap: line1 leading slot src=3 x=0");
    }
    CHECK(caret_line_of_slot(&cf, cf.lines[1].slot0) == 1, "field softwrap: leading slot of line1 -> line 1");
    /* cursor 4 (between c and d) resolves on line 1 */
    {
        int s = caret_slot_for_offset(&cf, 4);
        CHECK(caret_line_of_slot(&cf, s) == 1, "field softwrap: offset 4 on line 1");
    }
}

static void test_field_empty(void) {
    CaretField cf;
    int lines = build("", "", &cf);
    CHECK(lines == 1 && cf.slot_count == 1, "field empty: one line, one slot");
    CHECK(cf.slots[0].src == 0 && cf.slots[0].x == 0.0f, "field empty: slot at origin");
}

static void test_field_multibyte(void) {
    /* "é" = 0xC3 0xA9 (2 bytes), then 'x'. src "éx". One line; slots: leading(0),
       after é (x=1, src=2), after x (x=2, src=3). */
    CaretField cf;
    int lines = build("\xC3\xA9x", "\xC3\xA9x", &cf);
    CHECK(lines == 1, "field multibyte: one line");
    CHECK(cf.slot_count == 3, "field multibyte: 3 slots (not 4 — é is one codepoint)");
    CHECK(cf.slots[1].src == 2 && cf.slots[1].x == 1.0f, "field multibyte: after é src=2 x=1");
    CHECK(cf.slots[2].src == 3 && cf.slots[2].x == 2.0f, "field multibyte: after x src=3 x=2");
    CHECK(caret_slot_for_offset(&cf, 1) == 0, "multibyte: fallback for mid-codepoint -> slot 0");
}

static void test_field_trailing_nl(void) {
    CaretField cf;
    int lines = build("abc\n", "abc\n", &cf);
    CHECK(lines == 2, "field trailing-nl: two lines");
    /* line 1 is empty: exactly one leading slot whose src == srclen (4) */
    {
        int s0 = cf.lines[1].slot0;
        CHECK(cf.lines[1].nslots == 1, "field trailing-nl: line1 has 1 slot");
        CHECK(cf.slots[s0].src == 4 && cf.slots[s0].x == 0.0f, "field trailing-nl: line1 leading slot src=4 x=0");
    }
    CHECK(caret_slot_for_offset(&cf, 4) == cf.lines[1].slot0, "field trailing-nl: offset 4 -> line1 leading slot");
}

static void test_field_trailing_space(void) {
    /* text_wrap drops a trailing space: src "ab " wraps to "ab". With space_adv=1
       the dropped space still gets a caret slot one space-width past 'b', so the
       caret advances when you type a trailing space. */
    CaretField cf;
    int lines = build("ab ", "ab", &cf);
    CHECK(lines == 1, "field trailing-space: one line");
    /* slots: leading(src0,x0), after-a(src1,x1), after-b(src2,x2), space(src3,x3) */
    CHECK(cf.slot_count == 4, "field trailing-space: 4 slots incl. the dropped space");
    CHECK(cf.slots[3].src == 3 && cf.slots[3].x == 3.0f, "field trailing-space: caret after the space at x=3");
    CHECK(caret_slot_for_offset(&cf, 3) == 3, "field trailing-space: offset 3 -> the trailing-space slot");
}

int main(void) {
    test_cplen();
    test_reconcile_plain();
    test_reconcile_softwrap();
    test_reconcile_multispace();
    test_reconcile_hard_nl();
    test_field_plain();
    test_field_softwrap();
    test_field_empty();
    test_field_multibyte();
    test_field_trailing_nl();
    test_field_trailing_space();
    if (fails == 0) printf("caret_test: all passed\n");
    return fails ? 1 : 0;
}
