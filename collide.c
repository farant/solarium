/* collide.c — see collide.h. Pure plan-view math + the scene derivation
   bridge at the bottom; no GL. C89. */

#include "collide.h"
#include "sol_math.h"   /* mat4_mul_point (the derivation bridge) */

#include <math.h>       /* sqrtf, cosf, sinf, atan2f */
#include <stdlib.h>     /* realloc, free */
#include "flora.h"      /* trunk dims: the tree read a second time (P7) */
#include "gothic.h"     /* the plan + survival: the church read a
                           THIRD time — render and physics agree by
                           construction (P6 item 9) */
#include <string.h>     /* strcmp */

#define COLLIDE_MAX_ITERS 3       /* push-outs per substep; a corner needs 2 */
#define COLLIDE_SKIN      0.002f  /* resolve margin so a contact doesn't re-trip */
#define COLLIDE_MAX_SUB   8       /* substep runaway bound (a teleport is not a move) */

void collide_set_init(ColliderSet *cs) {
    cs->boxes = NULL;
    cs->count = 0;
    cs->cap   = 0;
}

void collide_set_free(ColliderSet *cs) {
    free(cs->boxes);
    cs->boxes = NULL;
    cs->count = 0;
    cs->cap   = 0;
}

void collide_set_clear(ColliderSet *cs) {
    cs->count = 0;
}

void collide_set_add(ColliderSet *cs, float cx, float cz, float yaw,
                     float hx, float hz, float y0, float y1, sol_u32 source) {
    ColliderBox *b;
    if (cs->count == cs->cap) {
        int          ncap = (cs->cap == 0) ? 16 : cs->cap * 2;
        ColliderBox *nb   = (ColliderBox *)realloc(cs->boxes,
                                (size_t)ncap * sizeof *nb);
        if (nb == NULL) return;             /* OOM: drop it, never corrupt */
        cs->boxes = nb;
        cs->cap   = ncap;
    }
    b = &cs->boxes[cs->count];
    b->cx = cx;  b->cz = cz;
    b->cyaw = cosf(yaw);
    b->syaw = sinf(yaw);
    b->hx = hx;  b->hz = hz;
    b->y0 = y0;  b->y1 = y1;
    b->source = source;
    cs->count += 1;
}

/* The one primitive everything reduces to: the capsule's plan circle vs one
   box's plan rectangle, gated by the vertical interval. Writes the world-plan
   push-out normal + depth and returns whether there is a contact at all.

   The height gate encodes the treaty (collide.h): a top within STEP_UP of the
   feet is a step the ground seam will climb — not our business; a bottom at
   or above the crown is clear headroom.

   The rotation convention matches sol_math's R_y(theta) exactly:
   world = [c 0 s; 0 1 0; -s 0 c] * local, so
   local  x = c*wx - s*wz,  z =  s*wx + c*wz   (world -> box frame)
   world  x = c*lx + s*lz,  z = -s*lx + c*lz   (box frame -> world)        */
static sol_bool box_penetration(const ColliderBox *b, float px, float pz,
                                float feet, float head, float r,
                                float *nx, float *nz, float *depth) {
    float dx, dz, lx, lz, qx, qz;

    if (b->y1 <= feet + COLLIDE_STEP_UP) return SOL_FALSE;  /* a step, not a wall */
    if (b->y0 >= head)                   return SOL_FALSE;  /* above the crown    */

    dx = px - b->cx;
    dz = pz - b->cz;
    lx = b->cyaw * dx - b->syaw * dz;
    lz = b->syaw * dx + b->cyaw * dz;

    /* clamping the center to the rectangle IS the closest-point query */
    qx = lx;
    if (qx < -b->hx) qx = -b->hx; else if (qx > b->hx) qx = b->hx;
    qz = lz;
    if (qz < -b->hz) qz = -b->hz; else if (qz > b->hz) qz = b->hz;

    if (lx != qx || lz != qz) {
        /* center outside the rectangle: the usual case. Distance from the
           clamp point decides contact; the normal points from the surface
           toward the center, which rounds corners automatically. */
        float ddx = lx - qx;
        float ddz = lz - qz;
        float d2  = ddx * ddx + ddz * ddz;
        float d, lnx, lnz;
        if (d2 >= r * r) return SOL_FALSE;
        d   = sqrtf(d2);                    /* > 0: at least one axis differs */
        lnx = ddx / d;
        lnz = ddz / d;
        *depth = r - d;
        *nx = b->cyaw * lnx + b->syaw * lnz;
        *nz = -b->syaw * lnx + b->cyaw * lnz;
        return SOL_TRUE;
    }

    {
        /* center INSIDE the rectangle (a fast frame, a spawn): the clamp
           returned the center itself, so the closest-point normal is
           undefined and we choose one — out the nearest face, far enough to
           clear the surface by the radius. */
        float dxp = b->hx - lx;             /* distance to each face */
        float dxm = lx + b->hx;
        float dzp = b->hz - lz;
        float dzm = lz + b->hz;
        float lnx, lnz, m;
        m = dxp; lnx = 1.0f; lnz = 0.0f;
        if (dxm < m) { m = dxm; lnx = -1.0f; lnz = 0.0f; }
        if (dzp < m) { m = dzp; lnx = 0.0f;  lnz = 1.0f; }
        if (dzm < m) { m = dzm; lnx = 0.0f;  lnz = -1.0f; }
        *depth = m + r;
        *nx = b->cyaw * lnx + b->syaw * lnz;
        *nz = -b->syaw * lnx + b->cyaw * lnz;
        return SOL_TRUE;
    }
}

vec3 collide_slide(const ColliderSet *cs, vec3 feet, vec3 move,
                   float radius, float height) {
    vec3  p;
    float mlen2;
    float sub_x, sub_z;
    int   nsub, s, it, i;

    p    = feet;
    p.y += move.y;                          /* vertical passes through */

    mlen2 = move.x * move.x + move.z * move.z;
    if (mlen2 <= 0.0f || cs == NULL || cs->count == 0) {
        p.x += move.x;
        p.z += move.z;
        return p;
    }

    /* Substep so one clamped-dt frame can't tunnel: each lateral step stays
       under half a radius, and the contact zone around a wall is at least a
       full radius wide on each side — a substep can never jump it. */
    {
        float mlen = sqrtf(mlen2);
        nsub = (int)(mlen / (radius * 0.5f)) + 1;
        if (nsub > COLLIDE_MAX_SUB) nsub = COLLIDE_MAX_SUB;
    }
    sub_x = move.x / (float)nsub;
    sub_z = move.z / (float)nsub;

    for (s = 0; s < nsub; s++) {
        p.x += sub_x;
        p.z += sub_z;
        for (it = 0; it < COLLIDE_MAX_ITERS; it++) {
            /* deepest contact first: resolving it can only shrink the rest,
               so the loop converges instead of ping-ponging in a corner */
            float    bnx = 0.0f, bnz = 0.0f, bdepth = 0.0f;
            sol_bool hit = SOL_FALSE;
            for (i = 0; i < cs->count; i++) {
                float cnx, cnz, cdepth;
                if (box_penetration(&cs->boxes[i], p.x, p.z,
                                    p.y, p.y + height, radius,
                                    &cnx, &cnz, &cdepth)) {
                    if (!hit || cdepth > bdepth) {
                        hit = SOL_TRUE;
                        bnx = cnx; bnz = cnz; bdepth = cdepth;
                    }
                }
            }
            if (!hit) break;
            p.x += bnx * (bdepth + COLLIDE_SKIN);
            p.z += bnz * (bdepth + COLLIDE_SKIN);
        }
    }
    return p;
}

float collide_stand(const ColliderSet *cs, vec3 feet, float radius) {
    float best = -1e30f;
    int   i;
    if (cs == NULL) return best;
    for (i = 0; i < cs->count; i++) {
        const ColliderBox *b = &cs->boxes[i];
        float dx, dz, lx, lz, qx, qz, ddx, ddz;
        if (b->y1 > feet.y + COLLIDE_STEP_UP) continue;   /* a wall   */
        if (b->y1 <= best) continue;                      /* not best */
        dx = feet.x - b->cx;
        dz = feet.z - b->cz;
        lx = b->cyaw * dx - b->syaw * dz;
        lz = b->syaw * dx + b->cyaw * dz;
        qx = lx;
        if (qx < -b->hx) qx = -b->hx; else if (qx > b->hx) qx = b->hx;
        qz = lz;
        if (qz < -b->hz) qz = -b->hz; else if (qz > b->hz) qz = b->hz;
        ddx = lx - qx;
        ddz = lz - qz;
        if (ddx * ddx + ddz * ddz >= radius * radius) continue;
        best = b->y1;
    }
    return best;
}

float collide_clamp_y(const ColliderSet *cs, vec3 feet, float dy,
                      float radius, float height) {
    int i;
    if (cs == NULL || dy == 0.0f) return dy;
    for (i = 0; i < cs->count; i++) {
        const ColliderBox *b = &cs->boxes[i];
        float dx, dz, lx, lz, qx, qz, ddx, ddz;

        dx = feet.x - b->cx;
        dz = feet.z - b->cz;
        lx = b->cyaw * dx - b->syaw * dz;
        lz = b->syaw * dx + b->cyaw * dz;
        qx = lx;
        if (qx < -b->hx) qx = -b->hx; else if (qx > b->hx) qx = b->hx;
        qz = lz;
        if (qz < -b->hz) qz = -b->hz; else if (qz > b->hz) qz = b->hz;
        ddx = lx - qx;
        ddz = lz - qz;
        if (ddx * ddx + ddz * ddz >= radius * radius) continue;

        if (dy > 0.0f) {                    /* rising: crown vs underside */
            float head = feet.y + height;
            if (head <= b->y0 + COLLIDE_SKIN) {
                float room = b->y0 - COLLIDE_SKIN - head;
                if (room < 0.0f) room = 0.0f;
                if (dy > room) dy = room;
            }
        } else {                            /* sinking: feet vs top */
            if (feet.y >= b->y1 - COLLIDE_SKIN) {
                float room = b->y1 + COLLIDE_SKIN - feet.y;
                if (room > 0.0f) room = 0.0f;
                if (dy < room) dy = room;
            }
        }
    }
    return dy;
}

/* ------------------------------------------------- the scene derivation */

/* Synthetic thickness for a room shell's zero-thickness interior planes:
   the collider's INTERIOR face sits exactly at the rendered wall, and the
   slab extends outward where no one stands. Wall slabs also run past the
   room's corners by this much so two meeting walls leave no diagonal gap. */
#define COLLIDE_SHELL_T 0.20f

/* Emit one box given in an object's LOCAL frame through its world matrix.
   Rests on the layer's standing assumption — upright (yaw + per-axis scale,
   never tilt) — which lets the transform be read off the matrix columns:
   column 0 is the world image of local +X (its XZ length is sx; its
   direction is the yaw, since R_y maps X to (cos, 0, -sin)), column 1's y
   is sy, column 2's XZ length is sz. */
static void emit_local_box_yawed(ColliderSet *cs, mat4 m, sol_u32 source,
                                 float lcx, float lcz, float lyaw,
                                 float lhx, float lhz,
                                 float ly0, float ly1);

static void emit_local_box(ColliderSet *cs, mat4 m, sol_u32 source,
                           float lcx, float lcz, float lhx, float lhz,
                           float ly0, float ly1) {
    float sx  = sqrtf(m.m[0] * m.m[0] + m.m[2]  * m.m[2]);
    float sy  = m.m[5];
    float sz  = sqrtf(m.m[8] * m.m[8] + m.m[10] * m.m[10]);
    float yaw = atan2f(-m.m[2], m.m[0]);
    vec3  c;
    c.x = lcx;
    c.y = (ly0 + ly1) * 0.5f;
    c.z = lcz;
    c = mat4_mul_point(m, c);
    collide_set_add(cs, c.x, c.z, yaw, lhx * sx, lhz * sz,
                    c.y - (ly1 - ly0) * 0.5f * sy,
                    c.y + (ly1 - ly0) * 0.5f * sy, source);
}

/* the same, with an extra yaw INSIDE the object's frame — the apse's
   angled walls (the cyaw/syaw fields were built for this day) */
static void emit_local_box_yawed(ColliderSet *cs, mat4 m, sol_u32 source,
                                 float lcx, float lcz, float lyaw,
                                 float lhx, float lhz,
                                 float ly0, float ly1) {
    float sx  = sqrtf(m.m[0] * m.m[0] + m.m[2]  * m.m[2]);
    float sy  = m.m[5];
    float sz  = sqrtf(m.m[8] * m.m[8] + m.m[10] * m.m[10]);
    float yaw = atan2f(-m.m[2], m.m[0]) + lyaw;
    vec3  c;
    c.x = lcx;
    c.y = (ly0 + ly1) * 0.5f;
    c.z = lcz;
    c = mat4_mul_point(m, c);
    collide_set_add(cs, c.x, c.z, yaw, lhx * sx, lhz * sz,
                    c.y - (ly1 - ly0) * 0.5f * sy,
                    c.y + (ly1 - ly0) * 0.5f * sy, source);
}

/* read one named param through the registry's defaults-merge */
static float ref_p(const SceneObject *o, const char *name) {
    return mesh_ref_param(o->mesh_ref, o->mesh_params, o->mesh_param_count, name);
}

void collide_rebuild(ColliderSet *cs, Scene *s) {
    sol_u32 i;
    collide_set_clear(cs);
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        if (o->mesh_ref == NULL) continue;

        if (strcmp(o->mesh_ref, "room") == 0) {
            /* the shell: a slab per PRESENT wall, interior face at the
               rendered plane (make_room: x = +/-w/2, z = +/-d/2, y in
               [0,h]); the ceiling underside at y = h for fly's crown */
            float w  = ref_p(o, "w"), d = ref_p(o, "d"), h = ref_p(o, "h");
            float hw = w * 0.5f, hd = d * 0.5f;
            float t  = COLLIDE_SHELL_T, ht = t * 0.5f;
            mat4  m  = scene_world_matrix(s, o);
            if (ref_p(o, "wn") > 0.5f)
                emit_local_box(cs, m, o->handle, 0.0f, -hd - ht, hw + t, ht, 0.0f, h);
            if (ref_p(o, "ws") > 0.5f)
                emit_local_box(cs, m, o->handle, 0.0f,  hd + ht, hw + t, ht, 0.0f, h);
            if (ref_p(o, "ww") > 0.5f)
                emit_local_box(cs, m, o->handle, -hw - ht, 0.0f, ht, hd + t, 0.0f, h);
            if (ref_p(o, "we") > 0.5f)
                emit_local_box(cs, m, o->handle,  hw + ht, 0.0f, ht, hd + t, 0.0f, h);
            if (ref_p(o, "ceil") > 0.5f)
                emit_local_box(cs, m, o->handle, 0.0f, 0.0f, hw, hd, h, h + t);

        } else if (strcmp(o->mesh_ref, "wall") == 0) {
            /* the around-the-gap pieces at their REAL thickness — mirror
               make_wall_with_opening's clamps exactly, so the colliders
               describe the same panels the emitter builds */
            float w  = ref_p(o, "w"),  h  = ref_p(o, "h");
            float ox = ref_p(o, "ox"), ow = ref_p(o, "ow");
            float oh = ref_p(o, "oh"), t  = ref_p(o, "t");
            float left = -w * 0.5f;
            float ht;
            mat4  m = scene_world_matrix(s, o);
            if (t < 0.01f) t = 0.01f;
            if (ox < 0.0f) ox = 0.0f;
            if (ox > w)    ox = w;
            if (ow > w - ox) ow = w - ox;
            if (oh > h)      oh = h;
            ht = t * 0.5f;
            if (ow < 1e-5f || oh < 1e-5f) {            /* no opening: one solid box */
                emit_local_box(cs, m, o->handle, 0.0f, 0.0f, w * 0.5f, ht, 0.0f, h);
            } else {
                float lx0 = left, lx1 = left + ox;
                float rx0 = left + ox + ow, rx1 = left + w;
                if (ox > 1e-5f)                        /* left panel */
                    emit_local_box(cs, m, o->handle, (lx0 + lx1) * 0.5f, 0.0f,
                                   (lx1 - lx0) * 0.5f, ht, 0.0f, h);
                if (ox + ow < w - 1e-5f)               /* right panel */
                    emit_local_box(cs, m, o->handle, (rx0 + rx1) * 0.5f, 0.0f,
                                   (rx1 - rx0) * 0.5f, ht, 0.0f, h);
                if (oh < h - 1e-5f)                    /* header over the gap */
                    emit_local_box(cs, m, o->handle, (lx1 + rx0) * 0.5f, 0.0f,
                                   (rx0 - lx1) * 0.5f, ht, oh, h);
                /* the threshold is a top FACE at y=0 — no volume, nothing
                   to emit; laterally the step gate would ignore it anyway */
            }

        } else if (strcmp(o->mesh_ref, "church_stone") == 0) {
            /* THE SAME church_plan + THE SAME church_survives the
               renderer consults — walls box at their KEPT heights,
               broken columns at theirs, the portal admits because the
               jambs were boxed around the gap (never a hole cut) */
            ChurchPlan cp;
            mat4  m = scene_world_matrix(s, o);
            float wt, hwid, hloc, F = GOTHIC_FOUNDATION;
            float rp2, tk;
            int   ci, sd;
            church_plan(&cp, o->mesh_params, o->mesh_param_count);
            wt   = cp.wall_t;
            hwid = 0.5f * cp.nave_w + cp.aisle_w;
            hloc = cp.aisle_h - cp.plinth_h;
            rp2  = 0.18f + 0.035f * cp.nave_w;
            for (ci = 0; ci < cp.nbays; ci++) {        /* S/N walls */
                float x0 = cp.west_x + cp.tower_d + (float)ci * cp.bay_l;
                float xc = x0 + 0.5f * cp.bay_l, hx = 0.5f * cp.bay_l;
                if (ci == 0 && !cp.tower) { hx += 0.5f * wt; xc -= 0.5f * wt; }
                if (ci == cp.nbays - 1)   { hx += 0.5f * wt; xc += 0.5f * wt; }
                for (sd = 0; sd <= 2; sd += 2) {
                    float zc = (sd ? 1.0f : -1.0f) * (hwid + 0.5f * wt);
                    if (!church_survives(&cp, ELEM_WALL, ci, sd, &tk))
                        continue;
                    if (tk * hloc < 0.4f) continue;
                    emit_local_box(cs, m, o->handle, xc, zc, hx, 0.5f * wt,
                                   -F, cp.plinth_h + tk * hloc);
                }
            }
            if (church_survives(&cp, ELEM_WALL, 0, 1, &tk)) {
                /* the facade: jambs AROUND the portal, flanks beside */
                GothicOpening door;
                float xc = cp.west_x - 0.5f * wt;
                float kept = cp.plinth_h + tk * (cp.wall_h - cp.plinth_h);
                float hw2, jz;
                plan_opening(&cp, WALL_WEST, 0, &door);
                hw2 = 0.5f * door.w;
                jz  = 0.5f * (hw2 + 0.5f * cp.nave_w);
                emit_local_box(cs, m, o->handle, xc, -jz, 0.5f * wt,
                               0.5f * (0.5f * cp.nave_w - hw2), -F, kept);
                emit_local_box(cs, m, o->handle, xc,  jz, 0.5f * wt,
                               0.5f * (0.5f * cp.nave_w - hw2), -F, kept);
            }
            for (sd = 0; sd <= 2; sd += 2) {            /* facade flanks */
                float zc = (sd ? 1.0f : -1.0f)
                         * 0.5f * (0.5f * cp.nave_w + hwid);
                if (!cp.aisles) break;
                if (!church_survives(&cp, ELEM_WALL, 0, sd, &tk)) continue;
                if (tk * hloc < 0.4f) continue;
                emit_local_box(cs, m, o->handle, cp.west_x - 0.5f * wt, zc,
                               0.5f * wt, 0.5f * cp.aisle_w, -F,
                               cp.plinth_h + tk * hloc);
            }
            if (cp.apse_sides == 0) {                   /* flat east */
                if (church_survives(&cp, ELEM_WALL, cp.nbays, 1, &tk) &&
                    tk * (cp.wall_h - cp.plinth_h) >= 0.4f)
                    emit_local_box(cs, m, o->handle, cp.east_x + 0.5f * wt,
                                   0.0f, 0.5f * wt, 0.5f * cp.nave_w, -F,
                                   cp.plinth_h
                                   + tk * (cp.wall_h - cp.plinth_h));
                for (sd = 0; sd <= 2; sd += 2) {
                    float zc = (sd ? 1.0f : -1.0f)
                             * 0.5f * (0.5f * cp.nave_w + hwid);
                    if (!cp.aisles) break;
                    if (!church_survives(&cp, ELEM_WALL, cp.nbays, sd, &tk))
                        continue;
                    if (tk * hloc < 0.4f) continue;
                    emit_local_box(cs, m, o->handle, cp.east_x + 0.5f * wt,
                                   zc, 0.5f * wt, 0.5f * cp.aisle_w, -F,
                                   cp.plinth_h + tk * hloc);
                }
            } else {                                    /* the chevet */
                int k2;
                for (k2 = 0; k2 < 5; k2++) {
                    float ax, az, bx, bz, dx, dz, nl, len, mx, mz;
                    plan_apse_pier(&cp, k2,     &ax, &az);
                    plan_apse_pier(&cp, k2 + 1, &bx, &bz);
                    dx = bx - ax; dz = bz - az;
                    nl = sqrtf(dx * dx + dz * dz);
                    if (nl < 1e-5f) continue;
                    if (!church_survives(&cp, ELEM_WALL, cp.nbays, k2, &tk))
                        continue;
                    if (tk * cp.wall_h < 0.4f) continue;
                    len = nl;
                    mx = 0.5f * (ax + bx) + (dz / nl) * 0.5f * wt;
                    mz = 0.5f * (az + bz) - (dx / nl) * 0.5f * wt;
                    emit_local_box_yawed(cs, m, o->handle, mx, mz,
                                         atan2f(-dz, dx), 0.5f * len,
                                         0.5f * wt, -F, tk * cp.wall_h);
                }
                for (k2 = 0; k2 < 6; k2++) {            /* radial piers */
                    float ax, az, rap;
                    if (!church_survives(&cp, ELEM_PIER, cp.nbays, k2, &tk))
                        continue;
                    plan_apse_pier(&cp, k2, &ax, &az);
                    rap = (0.4f > 0.7f * wt ? 0.4f : 0.7f * wt) * 0.924f;
                    emit_local_box(cs, m, o->handle, ax, az, rap, rap,
                                   -F, tk * (cp.wall_h + 0.25f));
                }
            }
            if (cp.aisles)                              /* arcade piers */
                for (ci = cp.tower ? 0 : 1; ci < cp.nbays; ci++) {
                    float px, pz;
                    for (sd = 0; sd <= 2; sd += 2) {
                        int row = sd ? PIER_ROW_N_ARCADE : PIER_ROW_S_ARCADE;
                        if (!plan_pier(&cp, ci, row, &px, &pz)) continue;
                        if (!church_survives(&cp, ELEM_PIER, ci, sd, &tk))
                            continue;
                        emit_local_box(cs, m, o->handle, px, pz,
                                       rp2 * 0.924f, rp2 * 0.924f, -F,
                                       cp.plinth_h
                                       + tk * (cp.impost_h - cp.plinth_h));
                    }
                }
            {   /* buttresses + the porch steps */
                float bd = cp.style == CHURCH_BASILICA ? 1.05f :
                           cp.style == CHURCH_HALL ? 0.75f : 0.55f;
                float bw = wt + 0.3f;
                float bh = cp.style == CHURCH_BASILICA ? cp.aisle_h + 0.6f
                                                       : cp.aisle_h;
                for (ci = 1; ci < cp.nbays; ci++) {
                    float x = cp.west_x + cp.tower_d + (float)ci * cp.bay_l;
                    for (sd = 0; sd <= 2; sd += 2) {
                        float zc = (sd ? 1.0f : -1.0f)
                                 * (hwid + wt + 0.5f * bd);
                        if (!church_survives(&cp, ELEM_BUTTRESS, ci, sd,
                                             (float *)0))
                            continue;
                        emit_local_box(cs, m, o->handle, x, zc, 0.5f * bw,
                                       0.5f * bd, -F, 0.93f * bh);
                    }
                }
                if (church_survives(&cp, ELEM_WALL, 0, 1, &tk)) {
                    GothicOpening door;
                    float riser = 0.16f, tread = 0.34f;
                    int   nstep = (int)(cp.plinth_h / riser), k3;
                    float sw;
                    plan_opening(&cp, WALL_WEST, 0, &door);
                    sw = 0.5f * door.w + 0.7f;
                    if (cp.porch > tread * (float)nstep * 0.5f)
                        for (k3 = 1; k3 <= nstep; k3++) {
                            float top = cp.plinth_h - riser * (float)k3;
                            float xs0 = cp.west_x - wt
                                      - tread * (float)k3;
                            if (top <= 0.02f) break;
                            emit_local_box(cs, m, o->handle,
                                           xs0 + 0.5f * tread, 0.0f,
                                           0.5f * tread, sw, -1.2f, top);
                        }
                }
            }

        } else if (strcmp(o->mesh_ref, "church_floor") == 0) {
            /* pavement: thin top-claiming slabs per SURVIVING bay — the
               step gate lifts you onto them; roofless bays have none,
               so ground_under's terrain answer wins: the grass floor */
            ChurchPlan cp;
            mat4  m = scene_world_matrix(s, o);
            float hwid, yt;
            int   ci, lane;
            church_plan(&cp, o->mesh_params, o->mesh_param_count);
            hwid = 0.5f * cp.nave_w + cp.aisle_w;
            yt   = cp.plinth_h - 0.02f;
            for (ci = 0; ci < cp.nbays; ci++) {
                float x0 = cp.west_x + cp.tower_d + (float)ci * cp.bay_l;
                for (lane = 0; lane <= 2; lane++) {
                    float zc, hz;
                    if (lane == 1) { zc = 0.0f; hz = 0.5f * cp.nave_w; }
                    else if (!cp.aisles) continue;
                    else {
                        zc = (lane ? 1.0f : -1.0f)
                           * 0.5f * (0.5f * cp.nave_w + hwid);
                        hz = 0.5f * cp.aisle_w;
                    }
                    if (!church_survives(&cp, ELEM_WEB, ci, lane, (float *)0))
                        continue;
                    emit_local_box(cs, m, o->handle, x0 + 0.5f * cp.bay_l,
                                   zc, 0.5f * cp.bay_l, hz, yt - 0.1f, yt);
                }
            }
            if (cp.tower &&
                church_survives(&cp, ELEM_WEB, 0, 1, (float *)0))
                emit_local_box(cs, m, o->handle,
                               cp.west_x + 0.5f * cp.tower_d, 0.0f,
                               0.5f * cp.tower_d, 0.5f * cp.nave_w,
                               yt - 0.1f, yt);
            if (cp.apse_sides == 5 &&
                church_survives(&cp, ELEM_WEB, cp.nbays, 1, (float *)0)) {
                float r = 0.5411961f * cp.nave_w;
                emit_local_box(cs, m, o->handle, cp.east_x + 0.35f * r,
                               0.0f, 0.35f * r, 0.85f * r, yt - 0.1f, yt);
            }

        } else if (strcmp(o->mesh_ref, "path") == 0) {
            /* the deck slab (make_path: x along length, walking surface at
               y=0, body in [-t,0]) — a step laterally, a landing for fly */
            float len = ref_p(o, "len"), w = ref_p(o, "w"), t = ref_p(o, "t");
            mat4  m = scene_world_matrix(s, o);
            emit_local_box(cs, m, o->handle, 0.0f, 0.0f,
                           len * 0.5f, w * 0.5f, -t, 0.0f);

        /* ---- the follies (P6 item 10): each reads the SAME truncation
           the emitter performs — gothic_column_top / arch_frag_dims are
           the shared formulas (one author, two readers) ---- */
        } else if (strcmp(o->mesh_ref, "column") == 0) {
            float h   = ref_p(o, "h");
            float top = gothic_column_top(h, ref_p(o, "style"),
                                          ref_p(o, "broken"));
            float rp  = 0.06f * (h < 0.6f ? 0.6f : h);
            mat4  m   = scene_world_matrix(s, o);
            if (rp < 0.09f) rp = 0.09f;
            if (rp > 0.45f) rp = 0.45f;
            emit_local_box(cs, m, o->handle, 0.0f, 0.0f,
                           rp, rp, 0.0f, top);

        } else if (strcmp(o->mesh_ref, "arch_frag") == 0) {
            /* two jamb boxes — the opening between them admits, the
               item-9 jambs-box-AROUND-the-portal law; kept heights make
               fallen jambs climbable stumps */
            float span = ref_p(o, "span"), h = ref_p(o, "h");
            float depth = ref_p(o, "depth"), ruin = ref_p(o, "ruin");
            float jw, spring, aa, hl, hr;
            float hw;
            mat4  m = scene_world_matrix(s, o);
            if (span < 0.5f) span = 0.5f;
            if (depth < 0.2f) depth = 0.2f;
            gothic_arch_frag_dims(span, ref_p(o, "acute"), h, ruin,
                                  &jw, &spring, &aa, &hl, &hr);
            hw = 0.5f * (span + 2.0f * jw);
            emit_local_box(cs, m, o->handle, -hw + 0.5f * jw, 0.0f,
                           0.5f * jw, 0.5f * depth, 0.0f, hl);
            emit_local_box(cs, m, o->handle,  hw - 0.5f * jw, 0.0f,
                           0.5f * jw, 0.5f * depth, 0.0f, hr);

        } else if (strcmp(o->mesh_ref, "stair") == 0) {
            /* one box per step, each top a STEP_UP within the last —
               the treaty climbs them, collide_stand makes them floors */
            float w = ref_p(o, "w"), rise = ref_p(o, "rise");
            float run = ref_p(o, "run");
            int   n = (int)(ref_p(o, "steps") + 0.5f), k;
            mat4  m = scene_world_matrix(s, o);
            if (n < 1) n = 1;
            if (n > 64) n = 64;
            if (w < 0.4f) w = 0.4f;
            if (rise < 0.05f) rise = 0.05f;
            if (run < 0.12f) run = 0.12f;
            for (k = 0; k < n; k++)
                emit_local_box(cs, m, o->handle,
                               run * ((float)k + 0.5f), 0.0f,
                               0.5f * run, 0.5f * w,
                               0.0f, rise * (float)(k + 1));

        } else if (strcmp(o->mesh_ref, "balustrade") == 0) {
            float len = ref_p(o, "len"), h = ref_p(o, "h");
            mat4  m = scene_world_matrix(s, o);
            if (len < 0.8f) len = 0.8f;
            if (h < 0.5f) h = 0.5f;
            emit_local_box(cs, m, o->handle, 0.0f, 0.0f,
                           0.5f * len, 0.11f, 0.0f, h);

        } else if (strcmp(o->mesh_ref, "cross") == 0) {
            /* the step slab (standable) + the shaft's mass above it */
            float h = ref_p(o, "h");
            mat4  m = scene_world_matrix(s, o);
            if (h < 1.5f) h = 1.5f;
            emit_local_box(cs, m, o->handle, 0.0f, 0.0f,
                           0.85f, 0.85f, 0.0f, 0.36f);
            emit_local_box(cs, m, o->handle, 0.0f, 0.0f,
                           0.26f, 0.26f, 0.36f, h);

        } else if (flora_species(o->mesh_ref) >= 0) {
            /* a tree (P7 item 3): the trunk blocks, the boughs admit —
               flora_trunk_dims is the SAME read the emitter grew from
               (one formula, two readers) */
            float r, top;
            mat4  m = scene_world_matrix(s, o);
            flora_trunk_dims(flora_species(o->mesh_ref),
                             o->mesh_params, o->mesh_param_count,
                             &r, &top);
            if (r > 0.0f && top > 0.2f)
                emit_local_box(cs, m, o->handle, 0.0f, 0.0f,
                               r, r, 0.0f, top);
        }
    }
}
