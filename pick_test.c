/* pick_test.c — headless checks for the picking math. 4a covers ray_vs_aabb;
   4b will add scene_pick cases. Built by `build.sh picktest` under ASan/UBSan. */

#include "sol_math.h"
#include "scene.h"

#include <stdio.h>
#include <math.h>

static int approx(float a, float b) { return fabsf(a - b) < 0.001f; }

/* A pickable Mesh built by hand — scene_pick reads only bounds + index_count,
   so no GPU upload is needed (keeps this test headless). Unit box [-0.5, 0.5]. */
static Mesh box_mesh(void) {
    Mesh m;
    m.vbuffer.id = 0;
    m.ibuffer.id = 0;
    m.index_count = 36;                           /* nonzero -> pickable */
    m.bounds.min = vec3_make(-0.5f, -0.5f, -0.5f);
    m.bounds.max = vec3_make( 0.5f,  0.5f,  0.5f);
    return m;
}

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

    /* ---- scene_pick: nearest hit, miss, and a scaled AABB ---- */
    {
        Scene   sc;
        sol_u32 near_h, far_h, hit;
        scene_init(&sc);
        near_h = scene_add(&sc, 0, box_mesh(), vec3_make(0.0f, 0.0f, -2.0f),
                           quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
        far_h  = scene_add(&sc, 0, box_mesh(), vec3_make(0.0f, 0.0f, -5.0f),
                           quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));

        /* ray down -Z through both boxes -> the NEARER (z=-2) wins */
        r.origin = vec3_make(0.0f, 0.0f, 0.0f);
        r.dir    = vec3_make(0.0f, 0.0f, -1.0f);
        hit = scene_pick(&sc, r, &t);
        printf("scene_pick nearest = handle %u (near=%u far=%u) t=%.3f\n",
               (unsigned)hit, (unsigned)near_h, (unsigned)far_h, t);
        if (hit != near_h) { printf("FAIL: should pick the nearer box\n"); scene_free(&sc); return 1; }

        /* ray pointing +Z hits nothing */
        r.dir = vec3_make(0.0f, 0.0f, 1.0f);
        if (scene_pick(&sc, r, &t) != 0) { printf("FAIL: +Z ray should hit nothing\n"); scene_free(&sc); return 1; }

        /* ray offset in X misses both unit boxes */
        r.origin = vec3_make(10.0f, 0.0f, 0.0f);
        r.dir    = vec3_make(0.0f, 0.0f, -1.0f);
        if (scene_pick(&sc, r, &t) != 0) { printf("FAIL: offset ray should miss\n"); scene_free(&sc); return 1; }
        printf("scene_pick miss cases: ok\n");
        scene_free(&sc);
    }

    /* a box scaled x3 (half-extent 1.5) is hit by a ray at x=1.0 that would
       miss the unit box -> exercises aabb_transform scaling */
    {
        Scene   sc;
        sol_u32 big_h;
        scene_init(&sc);
        big_h = scene_add(&sc, 0, box_mesh(), vec3_make(0.0f, 0.0f, -2.0f),
                          quat_identity(), vec3_make(3.0f, 3.0f, 3.0f));
        r.origin = vec3_make(1.0f, 0.0f, 0.0f);
        r.dir    = vec3_make(0.0f, 0.0f, -1.0f);
        if (scene_pick(&sc, r, &t) != big_h) { printf("FAIL: scaled box should be hit at x=1.0\n"); scene_free(&sc); return 1; }
        printf("scene_pick scaled AABB: ok\n");
        scene_free(&sc);
    }

    /* ray_vs_plane (item 4 drag): a downward ray hits the ground plane at
       the expected distance; parallel and behind-origin rays are rejected */
    {
        vec3 ground = vec3_make(0.0f, 1.0f, 0.0f);   /* plane y=1 */
        vec3 up     = vec3_make(0.0f, 1.0f, 0.0f);
        r.origin = vec3_make(0.0f, 5.0f, 0.0f);
        r.dir    = vec3_make(0.0f, -1.0f, 0.0f);
        if (!ray_vs_plane(r, ground, up, &t) || !approx(t, 4.0f)) {
            printf("FAIL: downward ray should hit y=1 at t=4\n");
            return 1;
        }
        r.dir = vec3_make(1.0f, 0.0f, 0.0f);          /* parallel: no hit */
        if (ray_vs_plane(r, ground, up, &t)) {
            printf("FAIL: parallel ray must miss the plane\n");
            return 1;
        }
        r.dir = vec3_make(0.0f, 1.0f, 0.0f);          /* plane behind: no hit */
        if (ray_vs_plane(r, ground, up, &t)) {
            printf("FAIL: plane behind the ray must be rejected\n");
            return 1;
        }
        printf("ray_vs_plane hit/parallel/behind: ok\n");
    }

    printf("pick_test: OK\n");
    return 0;
}
