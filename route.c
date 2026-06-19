#include "route.h"
#include "mesh.h"
#include <math.h>
#include <string.h>

/* world translation of an object (parent chain applied) */
static vec3 obj_pos(Scene *s, sol_u32 h) {
    SceneObject *o = scene_get(s, h);
    if (o) { mat4 w = scene_world_matrix(s, o); return vec3_make(w.m[12], w.m[13], w.m[14]); }
    return vec3_make(0.0f, 0.0f, 0.0f);
}

/* a room's interior half-extents (its "room" shell child's w/d, halved) */
static void room_half(Scene *s, sol_u32 room, float *hw, float *hd) {
    sol_u32 i;
    *hw = 4.0f; *hd = 4.0f;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        if (o->parent == room && o->mesh_ref && strcmp(o->mesh_ref, "room") == 0) {
            *hw = 0.5f * mesh_ref_param("room", o->mesh_params, o->mesh_param_count, "w");
            *hd = 0.5f * mesh_ref_param("room", o->mesh_params, o->mesh_param_count, "d");
            return;
        }
    }
}

/* Does the axis-aligned leg from (ax,az) to (bx,bz), swept to the deck
   half-width `hwd`, pass through any home/mirror room other than `ea`/`eb`?
   Used to pick the L variant that doesn't cut through a bystander room. */
static int leg_clips(Scene *s, float ax, float az, float bx, float bz,
                     float hwd, sol_u32 ea, sol_u32 eb) {
    sol_u32 i;
    float   lx0 = (ax < bx ? ax : bx) - hwd, lx1 = (ax > bx ? ax : bx) + hwd;
    float   lz0 = (az < bz ? az : bz) - hwd, lz1 = (az > bz ? az : bz) + hwd;
    for (i = 0; i < s->count; i++) {
        SceneObject *o  = &s->objects[i];
        const char  *rt = scene_meta_get(s, o->handle, "room_type");
        float        hw, hd;
        vec3         p;
        if (!rt) continue;
        if (strcmp(rt, "home") != 0 && strcmp(rt, "mirror") != 0) continue;
        if (o->handle == ea || o->handle == eb) continue;
        room_half(s, o->handle, &hw, &hd);
        p = obj_pos(s, o->handle);
        if (lx1 < p.x - hw || lx0 > p.x + hw) continue;   /* no X overlap */
        if (lz1 < p.z - hd || lz0 > p.z + hd) continue;   /* no Z overlap */
        return 1;
    }
    return 0;
}

/* the two rooms a walkway connects (its first two `connects` targets) */
static void walkway_rooms(SceneObject *o, sol_u32 *a, sol_u32 *b) {
    sol_u32 j;
    *a = 0; *b = 0;
    for (j = 0; j < o->rel_count; j++) {
        if (strcmp(o->relations[j].type, "connects") != 0) continue;
        if (*a == 0) *a = o->relations[j].target;
        else if (*b == 0) *b = o->relations[j].target;
    }
}

/* pass 1: per walkway, fill room_lo/hi (by Y) + wall_lo/hi (by geometry).
   door centers + corner are filled in route_all. */
static int routes_pass1(Scene *s, Route *out, int max) {
    sol_u32 i;
    int     n = 0;
    for (i = 0; i < s->count && n < max; i++) {
        SceneObject *o = &s->objects[i];
        sol_u32 ra, rb, lo, hi;
        vec3    pa, pb, plo, phi;
        float   dx, dz, adx, adz, hwl, hdl, hwh, hdh, mh;
        Route  *r;
        if (!o->mesh_ref || strcmp(o->mesh_ref, "walkway") != 0) continue;
        walkway_rooms(o, &ra, &rb);
        r = &out[n];
        memset(r, 0, sizeof *r);
        r->walkway = o->handle;
        if (ra == 0 || rb == 0 || !scene_get(s, ra) || !scene_get(s, rb)) { n++; continue; }
        pa = obj_pos(s, ra); pb = obj_pos(s, rb);
        lo = (pa.y <= pb.y) ? ra : rb;
        hi = (lo == ra) ? rb : ra;
        r->room_lo = lo; r->room_hi = hi;
        plo = obj_pos(s, lo); phi = obj_pos(s, hi);
        dx = phi.x - plo.x; dz = phi.z - plo.z;
        adx = dx < 0.0f ? -dx : dx; adz = dz < 0.0f ? -dz : dz;
        if (adx < 1e-3f && adz < 1e-3f) { n++; continue; }
        room_half(s, lo, &hwl, &hdl);
        room_half(s, hi, &hwh, &hdh);
        mh = ROUTE_DOOR_W * 0.5f + 0.5f;     /* keep an off-center door clear of the corner */
        /* STRAIGHT (off-center coupled doors) when the perpendicular offset
           still fits within BOTH facing walls — no L corner needed, the door
           just slides along the wall. Otherwise an L. This is what lets doors
           sit off-center: a modestly-offset room gets a clean straight path. */
        if (adx >= adz && adz <= hdh - mh && adz <= hdl - mh) {
            r->wall_lo = (dx > 0.0f) ? ROOM_WALL_E : ROOM_WALL_W;
            r->wall_hi = (dx > 0.0f) ? ROOM_WALL_W : ROOM_WALL_E;
            r->straight = 1;
        } else if (adz > adx && adx <= hwh - mh && adx <= hwl - mh) {
            r->wall_lo = (dz > 0.0f) ? ROOM_WALL_S : ROOM_WALL_N;
            r->wall_hi = (dz > 0.0f) ? ROOM_WALL_N : ROOM_WALL_S;
            r->straight = 1;
        } else {
            /* diagonal L: variant A exits along x (corner hi.x,lo.z); variant B
               exits along z (corner lo.x,hi.z). A variant is "bad" if a leg cuts
               through another room OR its corner lands inside an endpoint room.
               Default to the dominant axis; switch only to escape a bad variant. */
            float hwd    = ROUTE_DECK_W * 0.5f + 0.1f;
            int   a_clip = leg_clips(s, plo.x, plo.z, phi.x, plo.z, hwd, lo, hi) ||
                           leg_clips(s, phi.x, plo.z, phi.x, phi.z, hwd, lo, hi);
            int   b_clip = leg_clips(s, plo.x, plo.z, plo.x, phi.z, hwd, lo, hi) ||
                           leg_clips(s, plo.x, phi.z, phi.x, phi.z, hwd, lo, hi);
            int   a_bad  = a_clip || (adz < hdh) || (adx < hwl);  /* A corner in hi/lo */
            int   b_bad  = b_clip || (adx < hwh) || (adz < hdl);  /* B corner in hi/lo */
            int   use_b  = (adz > adx) ? 1 : 0;
            if (use_b && b_bad && !a_bad)  use_b = 0;
            if (!use_b && a_bad && !b_bad) use_b = 1;
            if (!use_b) {
                r->wall_lo = (dx > 0.0f) ? ROOM_WALL_E : ROOM_WALL_W;
                r->wall_hi = (dz > 0.0f) ? ROOM_WALL_N : ROOM_WALL_S;
            } else {
                r->wall_lo = (dz > 0.0f) ? ROOM_WALL_S : ROOM_WALL_N;
                r->wall_hi = (dx > 0.0f) ? ROOM_WALL_W : ROOM_WALL_E;
            }
            r->straight = 0;
        }
        r->valid = 1;
        n++;
    }
    return n;
}

/* the door center (signed offset along the wall's run axis from room center)
   for route index `idx`. side==0 => the lo room, side==1 => the hi room. */
static float spread_center(Scene *s, Route *out, int n, int idx, int side) {
    sol_u32 room = side ? out[idx].room_hi : out[idx].room_lo;
    int     wall = side ? out[idx].wall_hi : out[idx].wall_lo;
    float   hw, hd, span, margin, lo, hiend;
    int     i, count = 0, rank = 0;
    /* count members of this (room,wall) group + this route's 1-based rank
       (stable by array order). A route touches `room` as its lo XOR hi side. */
    for (i = 0; i < n; i++) {
        int hit_lo, hit_hi;
        if (!out[i].valid) continue;
        hit_lo = (out[i].room_lo == room && out[i].wall_lo == wall);
        hit_hi = (out[i].room_hi == room && out[i].wall_hi == wall);
        if (hit_lo) { if (i < idx || (i == idx && side == 0)) rank++; count++; }
        if (hit_hi) { if (i < idx || (i == idx && side == 1)) rank++; count++; }
    }
    if (count < 1) count = 1;
    if (rank < 1) rank = 1;
    room_half(s, room, &hw, &hd);
    margin = ROUTE_DOOR_W;
    span   = (wall == ROOM_WALL_N || wall == ROOM_WALL_S) ? (2.0f * hw) : (2.0f * hd);
    lo     = -0.5f * span + margin;
    hiend  =  0.5f * span - margin;
    if (hiend < lo) { lo = 0.0f; hiend = 0.0f; }
    return lo + (hiend - lo) * ((float)rank / (float)(count + 1));
}

/* world door-center on `room`'s `wall` at run-axis offset `center` */
static vec3 door_world(Scene *s, sol_u32 room, int wall, float center) {
    vec3  p = obj_pos(s, room);
    float hw, hd;
    room_half(s, room, &hw, &hd);
    if      (wall == ROOM_WALL_N) return vec3_make(p.x + center, p.y, p.z - hd);
    else if (wall == ROOM_WALL_S) return vec3_make(p.x + center, p.y, p.z + hd);
    else if (wall == ROOM_WALL_E) return vec3_make(p.x + hw, p.y, p.z + center);
    else                          return vec3_make(p.x - hw, p.y, p.z + center);
}

int route_all(Scene *s, Route *out, int max) {
    int n, i;
    n = routes_pass1(s, out, max);
    for (i = 0; i < n; i++) {
        Route *r = &out[i];
        float  clo, chi;
        vec3   dlo, dhi, cor;
        if (!r->valid) continue;
        clo = spread_center(s, out, n, i, 0);
        chi = spread_center(s, out, n, i, 1);
        dlo = door_world(s, r->room_lo, r->wall_lo, clo);
        dhi = door_world(s, r->room_hi, r->wall_hi, chi);
        if (r->straight) {
            /* couple the doors: snap the far door onto the near door's run-axis
               line so the path is dead straight and the door lands exactly at
               the path's end, even when the near door was spread off-center. */
            if (r->wall_lo == ROOM_WALL_E || r->wall_lo == ROOM_WALL_W)
                dhi.z = dlo.z;
            else
                dhi.x = dlo.x;
        }
        /* always build the L corner: leg1 runs along wall_lo's exit axis, leg2
           along wall_hi's. An aligned pair yields a zero-length leg2 (dropped by
           make_walkway_L), so a "straight" path is just the degenerate L.
           v1 limitation: this single-bend L assumes the exit-axis center
           separation >= (hw_lo + hw_hi); closer rooms can place the corner
           inside a room. Ring placement keeps rooms far enough apart for v1. */
        if (r->wall_lo == ROOM_WALL_E || r->wall_lo == ROOM_WALL_W)
            cor = vec3_make(dhi.x, 0.0f, dlo.z);
        else
            cor = vec3_make(dlo.x, 0.0f, dhi.z);
        {
            float l1 = (float)sqrt((double)((cor.x - dlo.x) * (cor.x - dlo.x) +
                                            (cor.z - dlo.z) * (cor.z - dlo.z)));
            float l2 = (float)sqrt((double)((dhi.x - cor.x) * (dhi.x - cor.x) +
                                            (dhi.z - cor.z) * (dhi.z - cor.z)));
            float tot = l1 + l2;
            cor.y = (tot > 1e-4f) ? dlo.y + (dhi.y - dlo.y) * (l1 / tot) : dlo.y;
        }
        r->door_lo = dlo; r->corner = cor; r->door_hi = dhi;
    }
    return n;
}

int route_for_walkway(Scene *s, sol_u32 walkway, Route *out) {
    Route all[ROUTE_MAX];
    int   n = route_all(s, all, ROUTE_MAX), i;
    for (i = 0; i < n; i++) {
        if (all[i].walkway == walkway && all[i].valid) { *out = all[i]; return 1; }
    }
    return 0;
}

int route_room_openings(Scene *s, sol_u32 room, RoomOpening *out, int max) {
    Route all[ROUTE_MAX];
    int   n = route_all(s, all, ROUTE_MAX), i, m = 0;
    vec3  rp = obj_pos(s, room);
    for (i = 0; i < n && m < max; i++) {
        Route *r = &all[i];
        if (!r->valid) continue;
        if (r->room_lo == room) {
            int   wall = r->wall_lo;
            float c = (wall == ROOM_WALL_N || wall == ROOM_WALL_S)
                        ? r->door_lo.x - rp.x : r->door_lo.z - rp.z;
            out[m].wall = wall; out[m].center = c;
            out[m].width = ROUTE_DOOR_W; out[m].height = ROUTE_DOOR_H; m++;
        }
        if (m >= max) break;
        if (r->room_hi == room) {
            int   wall = r->wall_hi;
            float c = (wall == ROOM_WALL_N || wall == ROOM_WALL_S)
                        ? r->door_hi.x - rp.x : r->door_hi.z - rp.z;
            out[m].wall = wall; out[m].center = c;
            out[m].width = ROUTE_DOOR_W; out[m].height = ROUTE_DOOR_H; m++;
        }
    }
    return m;
}
