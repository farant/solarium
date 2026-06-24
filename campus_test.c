/* campus_test: pure checks on campus_height (no GL, no scene). */
#include "mesh.h"
#include <stdio.h>

static int fails = 0;
static void check(const char *what, float got, float lo, float hi) {
    int ok = (got >= lo && got <= hi);
    printf("%-40s got=%7.3f  [%.3f, %.3f]  %s\n",
           what, got, lo, hi, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

int main(void) {
    /* two pads in a 60x60 campus: A at origin floor 0, B at +x20 floor 10.
       amp=0 so there are no hills -> deterministic. */
    CampusPad two[2];
    CampusPad overlap[2];
    two[0].cx = 0.0f;  two[0].cz = 0.0f;  two[0].hw = 3.0f; two[0].hd = 3.0f; two[0].floor_y = 0.0f;
    two[1].cx = 20.0f; two[1].cz = 0.0f;  two[1].hw = 3.0f; two[1].hd = 3.0f; two[1].floor_y = 10.0f;

    check("on pad A centre -> A floor",
          campus_height(two, 2, 60.0f, 60.0f, 0.0f, 7u, 0.0f, 0.0f),  -0.05f, 0.05f);
    check("on pad B centre -> B floor",
          campus_height(two, 2, 60.0f, 60.0f, 0.0f, 7u, 20.0f, 0.0f),  9.95f, 10.05f);
    check("midpoint -> between the two floors",
          campus_height(two, 2, 60.0f, 60.0f, 0.0f, 7u, 10.0f, 0.0f),  4.0f, 6.0f);

    /* overlap: two pads at the same spot, floors 0 and 8 -> lowest (0) wins. */
    overlap[0].cx = 0.0f; overlap[0].cz = 0.0f; overlap[0].hw = 5.0f; overlap[0].hd = 5.0f; overlap[0].floor_y = 0.0f;
    overlap[1].cx = 0.0f; overlap[1].cz = 0.0f; overlap[1].hw = 5.0f; overlap[1].hd = 5.0f; overlap[1].floor_y = 8.0f;
    check("overlap -> lowest pad wins",
          campus_height(overlap, 2, 60.0f, 60.0f, 0.0f, 7u, 0.0f, 0.0f), -0.05f, 0.05f);

    /* outside the rectangle -> 0 (rim). */
    check("outside rect -> 0",
          campus_height(two, 2, 60.0f, 60.0f, 0.0f, 7u, 40.0f, 0.0f), -0.001f, 0.001f);

    /* campus_point_blocked: a 6x6 pad at origin blocks nearby points. */
    {
        CampusPad pad;
        pad.cx = 0.0f; pad.cz = 0.0f; pad.hw = 3.0f; pad.hd = 3.0f; pad.floor_y = 0.0f;
        check("blocked: inside the pad",
              (float)campus_point_blocked(&pad, 1, 0.0f, 0.0f, 0.5f), 0.5f, 1.5f);
        check("blocked: just outside within clearance",
              (float)campus_point_blocked(&pad, 1, 3.2f, 0.0f, 0.5f), 0.5f, 1.5f);
        check("clear: well outside",
              (float)campus_point_blocked(&pad, 1, 10.0f, 0.0f, 0.5f), -0.5f, 0.5f);
    }

    printf(fails ? "\n%d FAIL\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
