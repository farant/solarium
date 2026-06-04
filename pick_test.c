/* pick_test.c — headless checks for the picking math. 4a covers ray_vs_aabb;
   4b will add scene_pick cases. Built by `build.sh picktest` under ASan/UBSan. */

#include "sol_math.h"

#include <stdio.h>
#include <math.h>

static int approx(float a, float b) { return fabsf(a - b) < 0.001f; }

int main(void) {
    Aabb  box;
    Ray   r;
    float t;

    box.min = vec3_make(-1.0f, -1.0f, -1.0f);
    box.max = vec3_make( 1.0f,  1.0f,  1.0f);

    /* straight-on hit: from (0,0,5) toward -Z enters the +Z face at t=4 */
    r.origin = vec3_make(0.0f, 0.0f, 5.0f);
    r.dir    = vec3_make(0.0f, 0.0f, -1.0f);
    if (!ray_vs_aabb(r, box, &t) || !approx(t, 4.0f)) {
        printf("FAIL: straight-on hit (t=%.3f)\n", t);
        return 1;
    }
    printf("hit: t=%.3f\n", t);

    /* miss: offset in X so the ray passes beside the box */
    r.origin = vec3_make(5.0f, 0.0f, 5.0f);
    r.dir    = vec3_make(0.0f, 0.0f, -1.0f);
    if (ray_vs_aabb(r, box, &t)) { printf("FAIL: offset ray should miss\n"); return 1; }
    printf("miss: ok\n");

    /* origin inside the box -> t = 0 */
    r.origin = vec3_make(0.0f, 0.0f, 0.0f);
    r.dir    = vec3_make(0.0f, 0.0f, -1.0f);
    if (!ray_vs_aabb(r, box, &t) || !approx(t, 0.0f)) {
        printf("FAIL: inside should give t=0 (t=%.3f)\n", t);
        return 1;
    }
    printf("inside: t=%.3f\n", t);

    /* box entirely behind the ray -> miss */
    r.origin = vec3_make(0.0f, 0.0f, 5.0f);
    r.dir    = vec3_make(0.0f, 0.0f, 1.0f);          /* pointing away from the box */
    if (ray_vs_aabb(r, box, &t)) { printf("FAIL: behind-the-ray box should miss\n"); return 1; }
    printf("behind: ok\n");

    printf("pick_test: OK\n");
    return 0;
}
