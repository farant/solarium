/* pick_test.c — headless checks for the picking math: ray_vs_aabb (item 4a),
   scene_pick (4b), the item-8 inverse-transform suite, slerp/smoothstep
   (item 9), and the P4-item-2 triangle precision: ray_vs_triangle units, a
   wall-with-opening whose DOORWAY admits the ray to the box beyond (the
   item's acceptance, headless), the pick-transparency filter, and the
   local-space narrow phase under scale + yaw. Built by `build.sh picktest`. */

#include "sol_math.h"
#include "scene.h"
#include "mesh.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

static int approx(float a, float b) { return fabsf(a - b) < 0.001f; }

/* headless "upload": what mesh_from_builder does minus the GPU — register
   the triangles under a chosen fake vbuffer id, bounds from the verts */
static Mesh geom_mesh(MeshBuilder *b, sol_u32 fake_id) {
    Mesh    m;
    sol_u32 i;
    m.vbuffer.id  = fake_id;
    m.ibuffer.id  = 0;
    m.index_count = (int)b->index_count;
    m.bounds.min  = vec3_make(b->vertices[0], b->vertices[1], b->vertices[2]);
    m.bounds.max  = m.bounds.min;
    for (i = 1; i < b->vertex_count; i++) {
        float x = b->vertices[i*12+0], y = b->vertices[i*12+1], z = b->vertices[i*12+2];
        if (x < m.bounds.min.x) m.bounds.min.x = x;
        if (x > m.bounds.max.x) m.bounds.max.x = x;
        if (y < m.bounds.min.y) m.bounds.min.y = y;
        if (y > m.bounds.max.y) m.bounds.max.y = y;
        if (z < m.bounds.min.z) m.bounds.min.z = z;
        if (z > m.bounds.max.z) m.bounds.max.z = z;
    }
    mesh_geom_register(fake_id, b);
    return m;
}

/* the app-policy shape: skip ref "wall" (the palace skips room/terrain) */
static sol_bool skip_wall_ref(const Scene *s, const SceneObject *o, void *ctx) {
    (void)s; (void)ctx;
    return o->mesh_ref != NULL && strcmp(o->mesh_ref, "wall") == 0;
}

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
        hit = scene_pick(&sc, r, &t, NULL, NULL);
        printf("scene_pick nearest = handle %u (near=%u far=%u) t=%.3f\n",
               (unsigned)hit, (unsigned)near_h, (unsigned)far_h, t);
        if (hit != near_h) { printf("FAIL: should pick the nearer box\n"); scene_free(&sc); return 1; }

        /* ray pointing +Z hits nothing */
        r.dir = vec3_make(0.0f, 0.0f, 1.0f);
        if (scene_pick(&sc, r, &t, NULL, NULL) != 0) { printf("FAIL: +Z ray should hit nothing\n"); scene_free(&sc); return 1; }

        /* ray offset in X misses both unit boxes */
        r.origin = vec3_make(10.0f, 0.0f, 0.0f);
        r.dir    = vec3_make(0.0f, 0.0f, -1.0f);
        if (scene_pick(&sc, r, &t, NULL, NULL) != 0) { printf("FAIL: offset ray should miss\n"); scene_free(&sc); return 1; }
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
        if (scene_pick(&sc, r, &t, NULL, NULL) != big_h) { printf("FAIL: scaled box should be hit at x=1.0\n"); scene_free(&sc); return 1; }
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

    /* ---- quat_slerp (item 9: the book's lift-and-face) ---- */
    {
        quat id  = quat_identity();
        quat y90 = quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), sol_radians(90.0f));
        quat q;
        vec3 v;
        float len;

        /* endpoints */
        q = quat_slerp(id, y90, 0.0f);
        v = quat_rotate(q, vec3_make(1.0f, 0.0f, 0.0f));
        if (!approx(v.x, 1.0f) || !approx(v.z, 0.0f)) {
            printf("FAIL: slerp t=0 should be the start\n"); return 1;
        }
        q = quat_slerp(id, y90, 1.0f);
        v = quat_rotate(q, vec3_make(1.0f, 0.0f, 0.0f));
        if (!approx(v.x, 0.0f) || !approx(v.z, -1.0f)) {
            printf("FAIL: slerp t=1 should be the end\n"); return 1;
        }

        /* constant angular velocity: t=0.25 of a 90-degree arc is 22.5 deg.
           +X rotated by 22.5 about Y -> (cos22.5, 0, -sin22.5) */
        q = quat_slerp(id, y90, 0.25f);
        v = quat_rotate(q, vec3_make(1.0f, 0.0f, 0.0f));
        printf("slerp quarter point -> (%.4f, %.4f, %.4f)\n", v.x, v.y, v.z);
        if (!approx(v.x, 0.9239f) || !approx(v.z, -0.3827f)) {
            printf("FAIL: slerp must walk the arc at constant rate\n"); return 1;
        }
        len = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
        if (!approx(len, 1.0f)) { printf("FAIL: slerp left the unit sphere\n"); return 1; }

        /* shortest path: -y90 is the SAME rotation; the midpoint must still
           rotate +X by 45 degrees, not take the 270-degree scenic route */
        {
            quat neg = y90;
            neg.x = -neg.x; neg.y = -neg.y; neg.z = -neg.z; neg.w = -neg.w;
            q = quat_slerp(id, neg, 0.5f);
            v = quat_rotate(q, vec3_make(1.0f, 0.0f, 0.0f));
            if (!approx(v.x, 0.7071f) || !approx(v.z, -0.7071f)) {
                printf("FAIL: slerp must take the short way around\n"); return 1;
            }
        }

        /* nearly-parallel fallback stays finite and unit */
        {
            quat tiny = quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), sol_radians(0.01f));
            q = quat_slerp(id, tiny, 0.5f);
            len = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
            if (!approx(len, 1.0f)) { printf("FAIL: nlerp fallback not unit\n"); return 1; }
        }
        printf("quat_slerp endpoints/arc/short-path/fallback: ok\n");
    }

    /* ---- sol_smoothstep: endpoints, midpoint, clamp, ease shape ---- */
    {
        if (!approx(sol_smoothstep(0.0f), 0.0f)  ||
            !approx(sol_smoothstep(1.0f), 1.0f)  ||
            !approx(sol_smoothstep(0.5f), 0.5f)  ||
            !approx(sol_smoothstep(-1.0f), 0.0f) ||
            !approx(sol_smoothstep(2.0f), 1.0f)  ||
            !approx(sol_smoothstep(0.25f), 0.15625f)) {
            printf("FAIL: sol_smoothstep\n"); return 1;
        }
        printf("sol_smoothstep: ok\n");
    }

    /* ---- ray_vs_triangle (P4 item 2): Moller-Trumbore units ---- */
    {
        vec3 v0 = vec3_make(0.0f, 0.0f, 0.0f);
        vec3 v1 = vec3_make(2.0f, 0.0f, 0.0f);
        vec3 v2 = vec3_make(0.0f, 2.0f, 0.0f);

        r.origin = vec3_make(0.5f, 0.5f, 5.0f);     /* inside, straight down -Z */
        r.dir    = vec3_make(0.0f, 0.0f, -1.0f);
        if (!ray_vs_triangle(r, v0, v1, v2, &t) || !approx(t, 5.0f)) {
            printf("FAIL: triangle straight-on hit (t=%.3f)\n", t); return 1;
        }
        r.origin = vec3_make(1.5f, 1.5f, 5.0f);     /* u+v > 1: past the hypotenuse */
        if (ray_vs_triangle(r, v0, v1, v2, &t)) {
            printf("FAIL: outside the hypotenuse must miss\n"); return 1;
        }
        r.origin = vec3_make(0.5f, 0.5f, -5.0f);    /* TWO-SIDED: from behind */
        r.dir    = vec3_make(0.0f, 0.0f, 1.0f);
        if (!ray_vs_triangle(r, v0, v1, v2, &t) || !approx(t, 5.0f)) {
            printf("FAIL: triangles are two-sided for picking\n"); return 1;
        }
        r.dir    = vec3_make(0.0f, 0.0f, -1.0f);    /* behind the origin */
        if (ray_vs_triangle(r, v0, v1, v2, &t)) {
            printf("FAIL: behind-the-origin hit must be rejected\n"); return 1;
        }
        r.origin = vec3_make(0.5f, 0.5f, 5.0f);     /* parallel to the plane */
        r.dir    = vec3_make(1.0f, 0.0f, 0.0f);
        if (ray_vs_triangle(r, v0, v1, v2, &t)) {
            printf("FAIL: parallel ray must miss\n"); return 1;
        }
        r.dir    = vec3_make(0.0f, 0.0f, -2.0f);    /* |dir|=2: t in dir units */
        if (!ray_vs_triangle(r, v0, v1, v2, &t) || !approx(t, 2.5f)) {
            printf("FAIL: t must come back in units of |dir| (t=%.3f)\n", t); return 1;
        }
        printf("ray_vs_triangle hit/edges/two-sided/behind/parallel/units: ok\n");
    }

    /* ---- THE DOORWAY (P4 item 2 acceptance, headless): a wall-with-opening
       between the ray and a box. AABB picking stops at the wall's box; the
       triangles know the opening is EMPTY — the ray reaches what stands
       beyond. Geometry comes through the real registry (one author). ---- */
    {
        Scene       sc;
        MeshBuilder wb, bb;
        Mesh        wm, bm;
        sol_u32     wall, box, hit;
        float       wprm[6];
        float       want_t;
        wprm[0] = 4.0f; wprm[1] = 3.0f; wprm[2] = 1.5f;   /* opening x in [-0.5, 0.5] */
        wprm[3] = 1.0f; wprm[4] = 2.2f; wprm[5] = 0.2f;

        scene_init(&sc);
        mb_init(&wb);
        if (!mesh_ref_build("wall", wprm, 6, &wb)) { printf("FAIL: registry wall\n"); return 1; }
        wm   = geom_mesh(&wb, 101);
        wall = scene_add(&sc, 0, wm, vec3_make(0.0f, 0.0f, 0.0f),
                         quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
        scene_mesh_ref_set(&sc, wall, "wall");

        mb_init(&bb);
        if (!mesh_ref_build("box", NULL, 0, &bb)) { printf("FAIL: registry box\n"); return 1; }
        bm  = geom_mesh(&bb, 102);
        box = scene_add(&sc, 0, bm,
                        vec3_make(0.0f, 1.0f - (bm.bounds.min.y + bm.bounds.max.y) * 0.5f, 3.0f),
                        quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));

        /* through the gap: |x|<0.5 and y=1 < oh — the ray must sail through
           the wall's AABB and land on the box's near face */
        r.origin = vec3_make(0.0f, 1.0f, -3.0f);
        r.dir    = vec3_make(0.0f, 0.0f, 1.0f);
        want_t   = (3.0f + bm.bounds.min.z) - (-3.0f);
        hit = scene_pick(&sc, r, &t, NULL, NULL);
        printf("doorway: hit=%u (wall=%u box=%u) t=%.3f (want %.3f)\n",
               (unsigned)hit, (unsigned)wall, (unsigned)box, t, want_t);
        if (hit != box || !approx(t, want_t)) {
            printf("FAIL: the click must pass THROUGH the doorway\n");
            scene_free(&sc); return 1;
        }

        /* into the left panel: a solid part of the wall still catches it */
        r.origin = vec3_make(-1.5f, 1.0f, -3.0f);
        hit = scene_pick(&sc, r, &t, NULL, NULL);
        if (hit != wall || !approx(t, 2.9f)) {
            printf("FAIL: the solid panel must still catch the ray (t=%.3f)\n", t);
            scene_free(&sc); return 1;
        }

        /* the policy filter: with "wall" pick-transparent, the same panel
           click passes through to nothing (the palace's land rule) */
        if (scene_pick(&sc, r, &t, skip_wall_ref, NULL) != 0) {
            printf("FAIL: a skipped ref must be transparent to picking\n");
            scene_free(&sc); return 1;
        }
        printf("doorway admits / panel catches / filter passes: ok\n");
        mb_free(&wb);
        mb_free(&bb);
        scene_free(&sc);
    }

    /* ---- narrow phase in LOCAL space: one hand-built triangle under
       scale (1,1,2) + yaw 90. Local plane z=1 -> scaled z=2 -> rotated to
       world x=+2 facing +X; a -X ray from (5, .5, -.5) must hit at t=3
       with t in WORLD units despite the local-space test. ---- */
    {
        Scene       sc;
        MeshBuilder tb;
        sol_u32     h, hit;
        mb_init(&tb);
        mb_push_vertex(&tb, 0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f);
        mb_push_vertex(&tb, 4.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f,  1.0f, 0.0f);
        mb_push_vertex(&tb, 0.0f, 4.0f, 1.0f,  0.0f, 0.0f, 1.0f,  0.0f, 1.0f);
        mb_push_triangle(&tb, 0, 1, 2);
        scene_init(&sc);
        h = scene_add(&sc, 0, geom_mesh(&tb, 103), vec3_make(0.0f, 0.0f, 0.0f),
                      quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), sol_radians(90.0f)),
                      vec3_make(1.0f, 1.0f, 2.0f));
        r.origin = vec3_make(5.0f, 0.5f, -0.5f);
        r.dir    = vec3_make(-1.0f, 0.0f, 0.0f);
        hit = scene_pick(&sc, r, &t, NULL, NULL);
        printf("local narrow phase: hit=%u t=%.3f (want %u, 3.0)\n",
               (unsigned)hit, t, (unsigned)h);
        if (hit != h || !approx(t, 3.0f)) {
            printf("FAIL: scaled+yawed triangle must hit at world t\n");
            scene_free(&sc); return 1;
        }
        mb_free(&tb);
        scene_free(&sc);
    }

    printf("pick_test: OK\n");
    return 0;
}
