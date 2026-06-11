/* collide_test.c — headless checks for the collision math (P4 item 1 piece 1):
   the slide preserves tangential motion, corners stop you, doorways admit and
   funnel, the step-up/headroom gates work, yawed boxes resolve in their own
   frame, the inside-the-box degenerate pushes out the nearest face, a huge
   clamped-dt move cannot tunnel a thin wall, and fly's vertical clamp catches
   undersides and tops. Links collide.c only — no scene, no GL. Built by
   `build.sh coltest`. */

#include "collide.h"
#include "sol_math.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

static int approx(float a, float b, float tol) { return fabsf(a - b) < tol; }

static vec3 v3(float x, float y, float z) {
    vec3 v;
    v.x = x; v.y = y; v.z = z;
    return v;
}

static quat qid(void) {
    quat q;
    q.x = q.y = q.z = 0.0f; q.w = 1.0f;
    return q;
}

/* add an empty-meshed object carrying a parametric ref */
static sol_u32 add_ref(Scene *s, sol_u32 parent, vec3 pos, quat rot,
                       const char *ref, const float *params, int n) {
    Mesh    m0;
    sol_u32 h;
    memset(&m0, 0, sizeof m0);
    h = scene_add(s, parent, m0, pos, rot, v3(1, 1, 1));
    scene_mesh_ref_set(s, h, ref);
    if (n > 0) scene_mesh_params_set(s, h, params, n);
    return h;
}

/* press the same move N times, like holding a key for N frames */
static vec3 press(const ColliderSet *cs, vec3 p, vec3 move, int n) {
    int i;
    for (i = 0; i < n; i++)
        p = collide_slide(cs, p, move, COLLIDE_RADIUS, COLLIDE_HEIGHT);
    return p;
}

int main(void) {
    const float R = COLLIDE_RADIUS;

    /* ---- a wall along X at z=2 (hz=0.1): head-on stops, diagonal slides */
    {
        ColliderSet cs;
        vec3        p;
        float       face = 2.0f - 0.1f - R;          /* where the circle rests */
        collide_set_init(&cs);
        collide_set_add(&cs, 0.0f, 2.0f, 0.0f, 5.0f, 0.1f, 0.0f, 3.0f, 0);

        p = press(&cs, v3(0, 0, 1.0f), v3(0, 0, 0.1f), 20);
        printf("head-on:  p=(%.3f, %.3f)  face=%.3f\n", p.x, p.z, face);
        if (!approx(p.z, face, 0.01f) || !approx(p.x, 0.0f, 0.001f)) {
            printf("FAIL: head-on should pin z at the face, x untouched\n");
            return 1;
        }

        p = press(&cs, v3(0, 0, 1.0f), v3(0.1f, 0, 0.1f), 10);
        printf("diagonal: p=(%.3f, %.3f)\n", p.x, p.z);
        if (!approx(p.x, 1.0f, 0.001f)) {
            printf("FAIL: tangential motion must survive the slide whole\n");
            return 1;
        }
        if (!approx(p.z, face, 0.01f)) {
            printf("FAIL: diagonal should still ride the face\n");
            return 1;
        }
        collide_set_free(&cs);
    }

    /* ---- an inside corner (walls at z=2 and x=2): motion dies there */
    {
        ColliderSet cs;
        vec3        p;
        collide_set_init(&cs);
        collide_set_add(&cs, 0.0f, 2.0f, 0.0f, 5.0f, 0.1f, 0.0f, 3.0f, 0);
        collide_set_add(&cs, 2.0f, 0.0f, 0.0f, 0.1f, 5.0f, 0.0f, 3.0f, 0);
        p = press(&cs, v3(1.0f, 0, 1.0f), v3(0.1f, 0, 0.1f), 20);
        printf("corner:   p=(%.3f, %.3f)\n", p.x, p.z);
        if (p.x > 1.62f || p.z > 1.62f) {
            printf("FAIL: the corner should pin both axes at the faces\n");
            return 1;
        }
        if (!approx(p.x, 1.6f, 0.02f) || !approx(p.z, 1.6f, 0.02f)) {
            printf("FAIL: corner rest position should touch both walls\n");
            return 1;
        }
        collide_set_free(&cs);
    }

    /* ---- a doorway (two pieces, gap x in [-0.5, 0.5]): admits dead-center,
       and FUNNELS you through when you aim slightly off the jamb */
    {
        ColliderSet cs;
        vec3        p;
        collide_set_init(&cs);
        collide_set_add(&cs, -3.0f, 2.0f, 0.0f, 2.5f, 0.1f, 0.0f, 3.0f, 0);
        collide_set_add(&cs,  3.0f, 2.0f, 0.0f, 2.5f, 0.1f, 0.0f, 3.0f, 0);

        p = press(&cs, v3(0, 0, 0.5f), v3(0, 0, 0.1f), 30);
        printf("doorway centered: p=(%.3f, %.3f)\n", p.x, p.z);
        if (p.z < 2.5f) {
            printf("FAIL: a centered walk must pass the 1m gap\n");
            return 1;
        }

        p = press(&cs, v3(-0.4f, 0, 0.5f), v3(0, 0, 0.1f), 60);
        printf("doorway funnel:   p=(%.3f, %.3f)\n", p.x, p.z);
        if (p.z < 2.5f) {
            printf("FAIL: the rounded jamb should funnel an off-center walk through\n");
            return 1;
        }
        collide_set_free(&cs);
    }

    /* ---- the treaty gates: a curb under STEP_UP is ignored (ground's job),
       the same footprint as a real wall blocks, a beam above the crown is
       clear headroom, a beam below it is not */
    {
        ColliderSet cs;
        vec3        p;

        collide_set_init(&cs);
        collide_set_add(&cs, 0.0f, 2.0f, 0.0f, 5.0f, 0.3f, 0.0f, 0.4f, 0);
        p = press(&cs, v3(0, 0, 0.5f), v3(0, 0, 0.1f), 30);
        printf("curb 0.4:  z=%.3f\n", p.z);
        if (p.z < 3.0f) { printf("FAIL: a step is not a wall\n"); return 1; }
        collide_set_free(&cs);

        collide_set_init(&cs);
        collide_set_add(&cs, 0.0f, 2.0f, 0.0f, 5.0f, 0.3f, 0.0f, 0.8f, 0);
        p = press(&cs, v3(0, 0, 0.5f), v3(0, 0, 0.1f), 30);
        printf("wall 0.8:  z=%.3f\n", p.z);
        if (!approx(p.z, 2.0f - 0.3f - R, 0.01f)) {
            printf("FAIL: over the step-up it IS a wall\n");
            return 1;
        }
        collide_set_free(&cs);

        collide_set_init(&cs);
        collide_set_add(&cs, 0.0f, 2.0f, 0.0f, 5.0f, 0.3f, 2.0f, 2.4f, 0);
        p = press(&cs, v3(0, 0, 0.5f), v3(0, 0, 0.1f), 30);
        printf("beam 2.0:  z=%.3f\n", p.z);
        if (p.z < 3.0f) { printf("FAIL: headroom above the crown is clear\n"); return 1; }
        collide_set_free(&cs);

        collide_set_init(&cs);
        collide_set_add(&cs, 0.0f, 2.0f, 0.0f, 5.0f, 0.3f, 1.5f, 2.4f, 0);
        p = press(&cs, v3(0, 0, 0.5f), v3(0, 0, 0.1f), 30);
        printf("beam 1.5:  z=%.3f\n", p.z);
        if (p.z > 1.7f) { printf("FAIL: a beam below the crown blocks\n"); return 1; }
        collide_set_free(&cs);
    }

    /* ---- yaw 90 degrees: a long-X box turned to lie along world Z. At
       z=1.5 the UNROTATED footprint is nowhere near the path — only correct
       frame handling blocks the walk. */
    {
        ColliderSet cs;
        vec3        p;
        collide_set_init(&cs);
        collide_set_add(&cs, 0.0f, 0.0f, (float)(3.14159265358979 * 0.5),
                        3.0f, 0.1f, 0.0f, 3.0f, 0);
        p = press(&cs, v3(-1.0f, 0, 1.5f), v3(0.1f, 0, 0), 20);
        printf("yaw 90:   p=(%.3f, %.3f)\n", p.x, p.z);
        if (!approx(p.x, -(0.1f + R), 0.01f)) {
            printf("FAIL: the rotated box must block in its own frame\n");
            return 1;
        }
        collide_set_free(&cs);
    }

    /* ---- yaw 45 degrees: pressing straight -x against the diagonal wall
       slides you +z, and the whole ride stays ON the face: the normal
       distance (px+pz)/sqrt(2) holds at hz+R. */
    {
        ColliderSet cs;
        vec3        p;
        float       ndist;
        collide_set_init(&cs);
        collide_set_add(&cs, 0.0f, 0.0f, (float)(3.14159265358979 * 0.25),
                        4.0f, 0.1f, 0.0f, 3.0f, 0);
        p = press(&cs, v3(1.0f, 0, 1.0f), v3(-0.1f, 0, 0), 30);
        ndist = (p.x + p.z) * 0.70710678f;
        printf("yaw 45:   p=(%.3f, %.3f)  ndist=%.3f (want %.3f)\n",
               p.x, p.z, ndist, 0.1f + R);
        if (p.z < 1.4f) {
            printf("FAIL: the diagonal wall should shed you sideways (+z)\n");
            return 1;
        }
        if (!approx(ndist, 0.1f + R, 0.02f)) {
            printf("FAIL: sliding must ride the rotated face, not drift off it\n");
            return 1;
        }
        collide_set_free(&cs);
    }

    /* ---- inside-the-box degenerate: a center inside the rectangle pushes
       out the NEAREST face, clearing it by the radius */
    {
        ColliderSet cs;
        vec3        p;
        collide_set_init(&cs);
        collide_set_add(&cs, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 3.0f, 0);
        p = collide_slide(&cs, v3(0.7f, 0, 0.0f), v3(0.001f, 0, 0),
                          COLLIDE_RADIUS, COLLIDE_HEIGHT);
        printf("inside:   p=(%.3f, %.3f)\n", p.x, p.z);
        if (!approx(p.x, 1.0f + R, 0.02f) || !approx(p.z, 0.0f, 0.02f)) {
            printf("FAIL: embedded center must eject out the nearest face\n");
            return 1;
        }
        collide_set_free(&cs);
    }

    /* ---- tunneling: one huge move (the 0.1s clamped-dt frame) across a
       thin wall would jump the whole contact zone without substepping */
    {
        ColliderSet cs;
        vec3        p;
        collide_set_init(&cs);
        collide_set_add(&cs, 0.0f, 2.0f, 0.0f, 5.0f, 0.05f, 0.0f, 3.0f, 0);
        p = collide_slide(&cs, v3(0, 0, 1.0f), v3(0, 0, 1.5f),
                          COLLIDE_RADIUS, COLLIDE_HEIGHT);
        printf("tunnel:   z=%.3f\n", p.z);
        if (p.z > 1.7f) {
            printf("FAIL: substepping must catch a one-frame wall crossing\n");
            return 1;
        }
        collide_set_free(&cs);
    }

    /* ---- fly clamp: rising stops the crown at an underside (only when the
       plan circle overlaps the footprint); sinking lands the feet on a top */
    {
        ColliderSet cs;
        float       dy;
        collide_set_init(&cs);
        collide_set_add(&cs, 0.0f, 0.0f, 0.0f, 2.0f, 2.0f, 2.5f, 2.7f, 0);

        dy = collide_clamp_y(&cs, v3(0, 0, 0), 1.0f, COLLIDE_RADIUS, COLLIDE_HEIGHT);
        printf("clamp up:   dy=%.3f (want ~%.3f)\n", dy, 2.5f - COLLIDE_HEIGHT);
        if (!approx(dy, 2.5f - COLLIDE_HEIGHT, 0.01f)) {
            printf("FAIL: rising crown must stop at the underside\n");
            return 1;
        }

        dy = collide_clamp_y(&cs, v3(5.0f, 0, 0), 1.0f, COLLIDE_RADIUS, COLLIDE_HEIGHT);
        printf("clamp miss: dy=%.3f\n", dy);
        if (!approx(dy, 1.0f, 0.0001f)) {
            printf("FAIL: a box you are not under must not clamp\n");
            return 1;
        }
        collide_set_free(&cs);

        collide_set_init(&cs);
        collide_set_add(&cs, 0.0f, 0.0f, 0.0f, 2.0f, 2.0f, 0.5f, 1.0f, 0);
        dy = collide_clamp_y(&cs, v3(0, 1.5f, 0), -1.0f, COLLIDE_RADIUS, COLLIDE_HEIGHT);
        printf("clamp down: dy=%.3f (want ~-0.5)\n", dy);
        if (!approx(dy, -0.5f, 0.01f)) {
            printf("FAIL: sinking feet must land on the top\n");
            return 1;
        }
        collide_set_free(&cs);
    }

    /* ---- empty set: motion passes through unchanged, y included */
    {
        ColliderSet cs;
        vec3        p;
        collide_set_init(&cs);
        p = collide_slide(&cs, v3(1, 2, 3), v3(0.5f, 0.25f, -0.5f),
                          COLLIDE_RADIUS, COLLIDE_HEIGHT);
        printf("empty:    p=(%.3f, %.3f, %.3f)\n", p.x, p.y, p.z);
        if (!approx(p.x, 1.5f, 0.0001f) || !approx(p.y, 2.25f, 0.0001f) ||
            !approx(p.z, 2.5f, 0.0001f)) {
            printf("FAIL: an empty world constrains nothing\n");
            return 1;
        }
        collide_set_free(&cs);
    }

    /* ================= piece 2: the derivation (collide_rebuild) ========= */

    /* ---- a sealed room: 4 wall slabs + ceiling, interior faces exactly at
       the rendered planes (make_room: x=+/-w/2, z=+/-d/2) */
    {
        Scene       s;
        ColliderSet cs;
        vec3        p;
        float       prm[8];
        prm[0] = 6; prm[1] = 4; prm[2] = 3;
        prm[3] = 1; prm[4] = 1; prm[5] = 1; prm[6] = 1; prm[7] = 1;
        scene_init(&s);
        collide_set_init(&cs);
        add_ref(&s, 0, v3(0, 0, 0), qid(), "room", prm, 8);
        collide_rebuild(&cs, &s);
        printf("room: %d boxes\n", cs.count);
        if (cs.count != 5) { printf("FAIL: sealed room = 4 walls + ceiling\n"); return 1; }

        p = press(&cs, v3(0, 0, 0), v3(0, 0, 0.1f), 30);
        printf("room south: z=%.3f (want ~%.3f)\n", p.z, 2.0f - R);
        if (!approx(p.z, 2.0f - R, 0.01f)) {
            printf("FAIL: the collider face must sit at the rendered wall\n");
            return 1;
        }
        p = press(&cs, p, v3(-0.1f, 0, 0), 40);
        printf("room west:  x=%.3f (want ~%.3f)\n", p.x, -(3.0f - R));
        if (!approx(p.x, -(3.0f - R), 0.01f)) {
            printf("FAIL: hugging one wall must still slide into the next\n");
            return 1;
        }
        collide_set_free(&cs);
        scene_free(&s);
    }

    /* ---- an open side emits no slab: walk out through it */
    {
        Scene       s;
        ColliderSet cs;
        vec3        p;
        float       prm[8];
        prm[0] = 6; prm[1] = 4; prm[2] = 3;
        prm[3] = 1; prm[4] = 1; prm[5] = 0; prm[6] = 1; prm[7] = 1;   /* ws=0 */
        scene_init(&s);
        collide_set_init(&cs);
        add_ref(&s, 0, v3(0, 0, 0), qid(), "room", prm, 8);
        collide_rebuild(&cs, &s);
        p = press(&cs, v3(0, 0, 0), v3(0, 0, 0.1f), 40);
        printf("open south: %d boxes, z=%.3f\n", cs.count, p.z);
        if (cs.count != 4 || p.z < 2.5f) {
            printf("FAIL: an absent wall must not resist\n");
            return 1;
        }
        collide_set_free(&cs);
        scene_free(&s);
    }

    /* ---- the defaults-merge: a 3-param room (w,d,h only) still gets its
       flags from the registry schema — sealed, 5 boxes (the item-7
       schema-growth rule, honored by the derivation too) */
    {
        Scene       s;
        ColliderSet cs;
        float       prm[3];
        prm[0] = 6; prm[1] = 4; prm[2] = 3;
        scene_init(&s);
        collide_set_init(&cs);
        add_ref(&s, 0, v3(0, 0, 0), qid(), "room", prm, 3);
        collide_rebuild(&cs, &s);
        printf("defaults: %d boxes\n", cs.count);
        if (cs.count != 5) {
            printf("FAIL: absent params must take registry defaults\n");
            return 1;
        }
        collide_set_free(&cs);
        scene_free(&s);
    }

    /* ---- a room under a ROTATED PARENT: anchor at (10,0,0) yawed 90deg;
       the room's local +Z (south, d/2=2) lands at world x=+2 off the anchor.
       This is the chain + yaw extraction working through scene_world_matrix. */
    {
        Scene       s;
        ColliderSet cs;
        vec3        p;
        sol_u32     anchor;
        Mesh        m0;
        float       prm[8];
        prm[0] = 6; prm[1] = 4; prm[2] = 3;
        prm[3] = 1; prm[4] = 1; prm[5] = 1; prm[6] = 1; prm[7] = 1;
        memset(&m0, 0, sizeof m0);
        scene_init(&s);
        collide_set_init(&cs);
        anchor = scene_add(&s, 0, m0, v3(10, 0, 0),
                           quat_from_axis_angle(v3(0, 1, 0), 3.14159265f * 0.5f),
                           v3(1, 1, 1));
        add_ref(&s, anchor, v3(0, 0, 0), qid(), "room", prm, 8);
        collide_rebuild(&cs, &s);
        p = press(&cs, v3(10, 0, 0), v3(0.1f, 0, 0), 40);
        printf("rotated room: x=%.3f (want ~%.3f)\n", p.x, 12.0f - R);
        if (!approx(p.x, 12.0f - R, 0.01f)) {
            printf("FAIL: a yawed parent must carry the walls with it\n");
            return 1;
        }
        collide_set_free(&cs);
        scene_free(&s);
    }

    /* ---- the doorway wall: three derived panels; the gap admits, the jamb
       resists from inside the opening, the solid panel blocks */
    {
        Scene       s;
        ColliderSet cs;
        vec3        p;
        float       prm[6];
        prm[0] = 4; prm[1] = 3; prm[2] = 1.5f;        /* opening x in [-0.5, 0.5] */
        prm[3] = 1; prm[4] = 2.2f; prm[5] = 0.2f;
        scene_init(&s);
        collide_set_init(&cs);
        add_ref(&s, 0, v3(0, 0, 0), qid(), "wall", prm, 6);
        collide_rebuild(&cs, &s);
        printf("wall: %d boxes\n", cs.count);
        if (cs.count != 3) { printf("FAIL: left + right + header\n"); return 1; }

        p = press(&cs, v3(0, 0, -1.0f), v3(0, 0, 0.1f), 30);
        printf("wall gap:   z=%.3f\n", p.z);
        if (p.z < 1.0f) { printf("FAIL: the derived doorway must admit\n"); return 1; }

        p = press(&cs, v3(0, 0, 0), v3(-0.1f, 0, 0), 30);
        printf("wall jamb:  x=%.3f (want ~%.3f)\n", p.x, -0.5f + R);
        if (!approx(p.x, -0.5f + R, 0.01f)) {
            printf("FAIL: the jamb must resist at the panel's true end\n");
            return 1;
        }

        p = press(&cs, v3(-1.5f, 0, -1.0f), v3(0, 0, 0.1f), 30);
        printf("wall panel: z=%.3f (want ~%.3f)\n", p.z, -(0.1f + R));
        if (!approx(p.z, -(0.1f + R), 0.01f)) {
            printf("FAIL: the solid panel must block at its true thickness\n");
            return 1;
        }
        collide_set_free(&cs);
        scene_free(&s);
    }

    /* ---- a solid wall (no opening) collapses to one box, like the emitter */
    {
        Scene       s;
        ColliderSet cs;
        float       prm[6];
        prm[0] = 4; prm[1] = 3; prm[2] = 0;
        prm[3] = 0; prm[4] = 0; prm[5] = 0.15f;
        scene_init(&s);
        collide_set_init(&cs);
        add_ref(&s, 0, v3(0, 0, 0), qid(), "wall", prm, 6);
        collide_rebuild(&cs, &s);
        printf("solid wall: %d box\n", cs.count);
        if (cs.count != 1) { printf("FAIL: no opening = one solid box\n"); return 1; }
        collide_set_free(&cs);
        scene_free(&s);
    }

    /* ---- a path: one deck slab — a step laterally (the gate ignores it),
       a landing for fly's sinking clamp */
    {
        Scene       s;
        ColliderSet cs;
        vec3        p;
        float       dy;
        float       prm[3];
        prm[0] = 6; prm[1] = 1.5f; prm[2] = 0.15f;
        scene_init(&s);
        collide_set_init(&cs);
        add_ref(&s, 0, v3(0, 0, 0), qid(), "path", prm, 3);
        collide_rebuild(&cs, &s);
        p  = press(&cs, v3(-4, 0, 0), v3(0.1f, 0, 0), 80);
        dy = collide_clamp_y(&cs, v3(0, 0.5f, 0), -1.0f, COLLIDE_RADIUS, COLLIDE_HEIGHT);
        printf("path: %d box, walk x=%.3f, fly-land dy=%.3f\n", cs.count, p.x, dy);
        if (cs.count != 1 || p.x < 3.5f) {
            printf("FAIL: a deck is a step, not a wall\n");
            return 1;
        }
        if (!approx(dy, -0.5f, 0.01f)) {
            printf("FAIL: fly must land on the deck top\n");
            return 1;
        }
        collide_set_free(&cs);
        scene_free(&s);
    }

    /* ---- ONE AUTHOR (the acceptance check of the piece): build the room's
       actual mesh through the registry and compare the emitter's outermost
       vertex against the collider's interior face — same params, two
       consumers, zero drift allowed */
    {
        Scene       s;
        ColliderSet cs;
        MeshBuilder b;
        sol_u32     i;
        float       maxz = -1e9f;
        float       inner;
        float       prm[8];
        prm[0] = 6; prm[1] = 4; prm[2] = 3;
        prm[3] = 1; prm[4] = 1; prm[5] = 1; prm[6] = 1; prm[7] = 1;
        scene_init(&s);
        collide_set_init(&cs);
        add_ref(&s, 0, v3(0, 0, 0), qid(), "room", prm, 8);
        collide_rebuild(&cs, &s);

        mb_init(&b);
        if (!mesh_ref_build("room", prm, 8, &b)) {
            printf("FAIL: registry refused the room\n");
            return 1;
        }
        for (i = 0; i < b.vertex_count; i++) {
            float z = b.vertices[i * 12 + 2];
            if (z > maxz) maxz = z;
        }
        /* emit order in collide_rebuild: n, s, w, e, ceil -> south is [1] */
        inner = cs.boxes[1].cz - cs.boxes[1].hz;
        printf("one author: mesh max z=%.5f, collider face=%.5f\n", maxz, inner);
        if (!approx(inner, maxz, 0.0001f)) {
            printf("FAIL: emitter and collider have diverged — two authors\n");
            return 1;
        }
        mb_free(&b);
        collide_set_free(&cs);
        scene_free(&s);
    }

    printf("collide_test: OK\n");
    return 0;
}
