/* collide.c — see collide.h. Pure plan-view math + the scene derivation
   bridge at the bottom; no GL. C89. */

#include "collide.h"
#include "sol_math.h"   /* mat4_mul_point (the derivation bridge) */

#include <math.h>       /* sqrtf, cosf, sinf, atan2f */
#include <stdlib.h>     /* realloc, free */
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

        } else if (strcmp(o->mesh_ref, "path") == 0) {
            /* the deck slab (make_path: x along length, walking surface at
               y=0, body in [-t,0]) — a step laterally, a landing for fly */
            float len = ref_p(o, "len"), w = ref_p(o, "w"), t = ref_p(o, "t");
            mat4  m = scene_world_matrix(s, o);
            emit_local_box(cs, m, o->handle, 0.0f, 0.0f,
                           len * 0.5f, w * 0.5f, -t, 0.0f);
        }
    }
}
