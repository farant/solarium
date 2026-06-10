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

    /* ---- quat rotate + conjugate (item 8: rotated-parent math) ---- */
    {
        quat ry = quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), sol_radians(90.0f));
        vec3 v  = quat_rotate(ry, vec3_make(1.0f, 0.0f, 0.0f));   /* +X -> -Z */
        vec3 b;
        printf("rotate(+90Y, +X) = (%.3f, %.3f, %.3f)\n", v.x, v.y, v.z);
        if (!approx(v.x, 0.0f) || !approx(v.y, 0.0f) || !approx(v.z, -1.0f)) {
            printf("FAIL: quat_rotate 90Y\n"); return 1;
        }
        b = quat_rotate(quat_conjugate(ry), v);                   /* and back */
        if (!approx(b.x, 1.0f) || !approx(b.y, 0.0f) || !approx(b.z, 0.0f)) {
            printf("FAIL: conjugate did not undo the rotation\n"); return 1;
        }
        printf("quat rotate/conjugate: ok\n");
    }

    /* ---- single-node TRS forward + inverse ----
       T=(1,2,3), R=+90deg Y, S=(2,3,4). local (1,1,1): scale -> (2,3,4);
       rotate (x'=z, z'=-x) -> (4,3,-2); translate -> (5,5,1). */
    {
        vec3 t   = vec3_make(1.0f, 2.0f, 3.0f);
        quat r90 = quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), sol_radians(90.0f));
        vec3 s   = vec3_make(2.0f, 3.0f, 4.0f);
        vec3 w   = trs_point_to_world(vec3_make(1.0f, 1.0f, 1.0f), t, r90, s);
        vec3 back;
        printf("trs to_world = (%.3f, %.3f, %.3f)\n", w.x, w.y, w.z);
        if (!approx(w.x, 5.0f) || !approx(w.y, 5.0f) || !approx(w.z, 1.0f)) {
            printf("FAIL: trs_point_to_world known case\n"); return 1;
        }
        back = trs_point_to_local(w, t, r90, s);
        if (!approx(back.x, 1.0f) || !approx(back.y, 1.0f) || !approx(back.z, 1.0f)) {
            printf("FAIL: trs_point_to_local did not invert to_world\n"); return 1;
        }
        printf("trs round-trip: ok\n");
    }

    /* ---- chain inverse through the scene (the drag write-back contract) ----
       A rotated, uniformly scaled grandparent and a rotated parent. The
       forward path is the RENDERER'S OWN math (scene_world_matrix +
       mat4_mul_point), so any convention mismatch in the inverse fails here. */
    {
        Scene        sc;
        sol_u32      g, par, child;
        SceneObject *co;
        vec3         w0, loc, idw, qd, md;
        quat         rw;
        scene_init(&sc);
        g     = scene_add(&sc, 0, box_mesh(), vec3_make(10.0f, 0.0f, 0.0f),
                          quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), sol_radians(90.0f)),
                          vec3_make(2.0f, 2.0f, 2.0f));
        par   = scene_add(&sc, g, box_mesh(), vec3_make(0.0f, 1.5f, 0.0f),
                          quat_from_axis_angle(vec3_make(0.0f, 0.0f, 1.0f), sol_radians(45.0f)),
                          vec3_make(1.0f, 1.0f, 1.0f));
        child = scene_add(&sc, par, box_mesh(), vec3_make(0.25f, -0.5f, 0.75f),
                          quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));

        co = scene_get(&sc, child);
        w0 = mat4_mul_point(scene_world_matrix(&sc, co), vec3_make(0.0f, 0.0f, 0.0f));

        /* drag contract: world origin -> parent-local recovers child->pos */
        loc = scene_world_to_local(&sc, par, w0);
        printf("chain world->local = (%.3f, %.3f, %.3f)\n", loc.x, loc.y, loc.z);
        if (!approx(loc.x, 0.25f) || !approx(loc.y, -0.5f) || !approx(loc.z, 0.75f)) {
            printf("FAIL: chain inverse did not recover the child's local pos\n");
            scene_free(&sc); return 1;
        }

        /* parent 0 = the world frame: identity */
        idw = scene_world_to_local(&sc, 0, w0);
        if (!approx(idw.x, w0.x) || !approx(idw.y, w0.y) || !approx(idw.z, w0.z)) {
            printf("FAIL: parent 0 must be identity\n"); scene_free(&sc); return 1;
        }

        /* world rotation: rotating +Z by the composed quat must match the
           matrix path's direction (normalized away from the uniform scale) */
        rw = scene_world_rotation(&sc, child);
        qd = quat_rotate(rw, vec3_make(0.0f, 0.0f, 1.0f));
        md = vec3_normalize(mat4_mul_dir(scene_world_matrix(&sc, co), vec3_make(0.0f, 0.0f, 1.0f)));
        if (!approx(qd.x, md.x) || !approx(qd.y, md.y) || !approx(qd.z, md.z)) {
            printf("FAIL: scene_world_rotation disagrees with the matrix path\n");
            scene_free(&sc); return 1;
        }
        printf("scene chain inverse + world rotation: ok\n");
        scene_free(&sc);
    }

    printf("pick_test: OK\n");
    return 0;
}
