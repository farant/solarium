#include "multiselect.h"
#include <stdio.h>

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static void test_overlap(void) {
    CHECK(msel_rect_overlap(0,0, 2,2,  1,1, 3,3), "overlap: corner overlap");
    CHECK(!msel_rect_overlap(0,0, 1,1, 2,2, 3,3), "overlap: disjoint");
    CHECK(msel_rect_overlap(0,0, 4,4,  1,1, 2,2), "overlap: A contains B");
    CHECK(msel_rect_overlap(2,2, 0,0,  1,1, 3,3), "overlap: unnormalized A");
    CHECK(msel_rect_overlap(0,0, 2,2,  2,2, 3,3), "overlap: edge touch inclusive");
    CHECK(!msel_rect_overlap(0,0, 1,0.5f, 0,1, 1,2), "overlap: vertical gap");
}

static void test_setops(void) {
    sol_u32 s[8];
    int     n = 0;
    msel_add(s, &n, 8, 5); msel_add(s, &n, 8, 7); msel_add(s, &n, 8, 5);
    CHECK(n == 2, "set: add dedupes");
    CHECK(msel_contains(s, n, 7) && !msel_contains(s, n, 9), "set: contains");
    msel_remove(s, &n, 5);
    CHECK(n == 1 && s[0] == 7, "set: remove compacts");
    CHECK(msel_toggle(s, &n, 8, 9) == SOL_TRUE && n == 2, "set: toggle adds");
    CHECK(msel_toggle(s, &n, 8, 9) == SOL_FALSE && n == 1, "set: toggle removes");
    /* cap boundary: a third add into a cap-2 set must no-op */
    {
        sol_u32 s2[2]; int n2 = 0;
        msel_add(s2, &n2, 2, 10);
        msel_add(s2, &n2, 2, 11);
        msel_add(s2, &n2, 2, 12);   /* full -> no-op */
        CHECK(n2 == 2, "set: add no-op at cap");
    }
    /* remove of an absent handle must not change len */
    msel_remove(s, &n, 99);
    CHECK(n == 1, "set: remove absent no-op");
}

int main(void) {
    test_overlap();
    test_setops();
    if (fails == 0) printf("multiselect_test: all passed\n");
    return fails ? 1 : 0;
}
