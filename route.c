#include "route.h"
#include "mesh.h"
#include "workspace.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* world translation of an object (parent chain applied) */
static vec3 obj_pos(Scene *s, sol_u32 h) {
    SceneObject *o = scene_get(s, h);
    if (o) { mat4 w = scene_world_matrix(s, o); return vec3_make(w.m[12], w.m[13], w.m[14]); }
    return vec3_make(0.0f, 0.0f, 0.0f);
}

/* a routable endpoint's half-extents. A room: its "room" shell child's w/d. An
   island (mesh_ref "terrain"): its OWN w/d params (the island IS its footprint). */
static void room_half(Scene *s, sol_u32 room, float *hw, float *hd) {
    SceneObject *ro = scene_get(s, room);
    sol_u32 i;
    *hw = 4.0f; *hd = 4.0f;
    if (ro && ro->mesh_ref && strcmp(ro->mesh_ref, "terrain") == 0) {
        *hw = 0.5f * mesh_ref_param("terrain", ro->mesh_params, ro->mesh_param_count, "w");
        *hd = 0.5f * mesh_ref_param("terrain", ro->mesh_params, ro->mesh_param_count, "d");
        return;
    }
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        if (o->parent == room && o->mesh_ref && strcmp(o->mesh_ref, "room") == 0) {
            *hw = 0.5f * mesh_ref_param("room", o->mesh_params, o->mesh_param_count, "w");
            *hd = 0.5f * mesh_ref_param("room", o->mesh_params, o->mesh_param_count, "d");
            return;
        }
    }
}

/* A room reduced to what the solver needs, collected ONCE per route_all so the
   candidate search doesn't re-scan the whole scene (476 string-keyed objects)
   for every leg test. This is what turns the brute-force search from
   O(candidates x objects) into O(candidates x rooms). */
typedef struct { sol_u32 handle; vec3 pos; float hw, hd; } RoomInfo;

static int collect_rooms(Scene *s, RoomInfo *ri, int max) {
    sol_u32 i;
    int     n = 0;
    for (i = 0; i < s->count; i++) {
        SceneObject *o  = &s->objects[i];
        const char  *rt = scene_meta_get(s, o->handle, "room_type");
        if (!rt) continue;
        if (strcmp(rt, "home")    != 0 && strcmp(rt, "mirror") != 0 &&
            strcmp(rt, "terrain") != 0 && strcmp(rt, "land")   != 0) continue;
        if (!scene_object_active(s, o->handle)) continue;   /* hidden workspace */
        if (n >= max) {
            static int warned = 0;
            if (!warned) { printf("route: room table full (%d) — further "
                                  "rooms get no doorways or walkways\n", max);
                           warned = 1; }
            break;
        }
        ri[n].handle = o->handle;
        ri[n].pos    = obj_pos(s, o->handle);
        room_half(s, o->handle, &ri[n].hw, &ri[n].hd);
        n++;
    }
    return n;
}

/* a room's collected pos/half by handle (NULL if not found) */
static const RoomInfo *room_find(const RoomInfo *ri, int nr, sol_u32 h) {
    int i;
    for (i = 0; i < nr; i++) if (ri[i].handle == h) return &ri[i];
    return NULL;
}

/* Does the axis-aligned leg from (ax,az) to (bx,bz), swept to the deck
   half-width `hwd`, pass through any room other than `ea`/`eb`? Used to pick
   the L variant that doesn't cut through a bystander room. */
static int leg_clips(const RoomInfo *ri, int nr, float ax, float az,
                     float bx, float bz, float hwd, sol_u32 ea, sol_u32 eb) {
    int   i;
    float lx0 = (ax < bx ? ax : bx) - hwd, lx1 = (ax > bx ? ax : bx) + hwd;
    float lz0 = (az < bz ? az : bz) - hwd, lz1 = (az > bz ? az : bz) + hwd;
    for (i = 0; i < nr; i++) {
        if (ri[i].handle == ea || ri[i].handle == eb) continue;
        if (lx1 < ri[i].pos.x - ri[i].hw || lx0 > ri[i].pos.x + ri[i].hw) continue; /* no X overlap */
        if (lz1 < ri[i].pos.z - ri[i].hd || lz0 > ri[i].pos.z + ri[i].hd) continue; /* no Z overlap */
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
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        sol_u32 ra, rb, lo, hi;
        vec3    pa, pb, plo, phi;
        float   dx, dz;
        Route  *r;
        if (!o->mesh_ref || strcmp(o->mesh_ref, "walkway") != 0) continue;
        if (n >= max) {
            static int warned = 0;
            if (!warned) { printf("route: walkway table full (%d) — further "
                                  "walkways get no routes\n", max);
                           warned = 1; }
            break;
        }
        walkway_rooms(o, &ra, &rb);
        r = &out[n];
        memset(r, 0, sizeof *r);
        r->walkway = o->handle;
        if (ra == 0 || rb == 0 || !scene_get(s, ra) || !scene_get(s, rb) ||
            !scene_object_active(s, ra) || !scene_object_active(s, rb)) {
            n++; continue;   /* missing OR hidden-workspace endpoint: no route */
        }
        pa = obj_pos(s, ra); pb = obj_pos(s, rb);
        lo = (pa.y <= pb.y) ? ra : rb;
        hi = (lo == ra) ? rb : ra;
        r->room_lo = lo; r->room_hi = hi;
        plo = obj_pos(s, lo); phi = obj_pos(s, hi);
        dx = phi.x - plo.x; dz = phi.z - plo.z;
        if ((dx < 0.0f ? -dx : dx) < 1e-3f &&
            (dz < 0.0f ? -dz : dz) < 1e-3f) { n++; continue; }   /* overlapping */
        r->valid = 1;     /* walls + door offsets are chosen by solve_path */
        n++;
    }
    return n;
}

/* a placed door, so a later connection on the same (room,wall) doesn't land
   its door on top of an already-placed one */
typedef struct { sol_u32 room; int wall; float off; } PlacedDoor;

/* world door position on a room (center c, half-extents hw/hd) at run-axis
   offset `off` along `wall` */
static vec3 door_at(vec3 c, float hw, float hd, int wall, float off) {
    if (wall == ROOM_WALL_N) return vec3_make(c.x + off, c.y, c.z - hd);
    if (wall == ROOM_WALL_S) return vec3_make(c.x + off, c.y, c.z + hd);
    if (wall == ROOM_WALL_E) return vec3_make(c.x + hw, c.y, c.z + off);
    return vec3_make(c.x - hw, c.y, c.z + off);   /* W */
}

/* is a door at (room,wall,off) within a door-width of an already-placed one? */
static int door_taken(PlacedDoor *pl, int np, sol_u32 room, int wall, float off) {
    int   i;
    float d;
    for (i = 0; i < np; i++) {
        if (pl[i].room != room || pl[i].wall != wall) continue;
        d = pl[i].off - off;
        if (d < 0.0f) d = -d;
        if (d < ROUTE_DOOR_W) return 1;
    }
    return 0;
}

/* is point p inside room (center c, half hw/hd)? */
static int pt_in_room(vec3 p, vec3 c, float hw, float hd) {
    return (p.x > c.x - hw && p.x < c.x + hw && p.z > c.z - hd && p.z < c.z + hd);
}

/* Choose the single-bend (or straight) path for connection lo->hi: search WHICH
   wall each door sits on AND WHERE along the wall it slides, picking the lowest-
   cost placement whose legs clear other rooms and whose corner stays outside
   both endpoint rooms. This is door freedom: the door lands wherever its leg
   meets the wall, not at the wall center. Fills walls + door_lo/corner/door_hi
   and records the two placed doors. Falls back to a best-effort dominant-axis L
   (which may clip) only if no clean placement exists. */
static void solve_path(const RoomInfo *ri, int nr, Route *r, vec3 plo, vec3 phi,
                       float hwl, float hdl, float hwh, float hdh,
                       PlacedDoor *pl, int *np) {
    float dx = phi.x - plo.x, dz = phi.z - plo.z;
    float adx = dx < 0.0f ? -dx : dx, adz = dz < 0.0f ? -dz : dz;
    int   hwx = (dx > 0.0f) ? ROOM_WALL_E : ROOM_WALL_W;   /* H wall facing R on x */
    int   hwz = (dz > 0.0f) ? ROOM_WALL_S : ROOM_WALL_N;   /* H wall facing R on z */
    int   rwx = (dx > 0.0f) ? ROOM_WALL_W : ROOM_WALL_E;   /* R wall facing H on x */
    int   rwz = (dz > 0.0f) ? ROOM_WALL_N : ROOM_WALL_S;   /* R wall facing H on z */
    float m   = ROUTE_DOOR_W * 0.5f + 0.4f;                /* keep door off corners */
    float hwd = ROUTE_DECK_W * 0.5f + 0.1f;                /* deck half-width */
    sol_u32 lo = r->room_lo, hi = r->room_hi;
    int   cwh[4], cwr[4], ckind[4];   /* per-combo: H wall, R wall, kind */
    float csh[4], csr[4];             /* per-combo: H/R run-axis half-extent */
    static const float frac[7] = { 0.0f, 0.35f, -0.35f, 0.65f, -0.65f, 0.9f, -0.9f };
    float best = 1e30f, tlo = 0.0f;
    int   found = 0, ci, gi, gj, gjn;
    int   bWlo = hwx, bWhi = rwz;
    float bOlo = 0.0f, bOhi = 0.0f;
    vec3  bDlo = plo, bCor = plo, bDhi = phi;

    cwh[0] = hwx; cwr[0] = rwx; ckind[0] = 0; csh[0] = hdl; csr[0] = hdh; /* facing x: straight */
    cwh[1] = hwx; cwr[1] = rwz; ckind[1] = 1; csh[1] = hdl; csr[1] = hwh; /* H exits x, R enters z */
    cwh[2] = hwz; cwr[2] = rwx; ckind[2] = 2; csh[2] = hwl; csr[2] = hdh; /* H exits z, R enters x */
    cwh[3] = hwz; cwr[3] = rwz; ckind[3] = 3; csh[3] = hwl; csr[3] = hwh; /* facing z: straight */

    for (ci = 0; ci < 4; ci++) {
        if (csh[ci] - m <= 0.0f || csr[ci] - m <= 0.0f) continue;
        /* the ideal home-door offset: track the destination's position along the
           wall's run axis, clamped to the wall, so doors on a shared wall order
           by destination and the paths fan out instead of crossing each other */
        tlo = (cwh[ci] == ROOM_WALL_E || cwh[ci] == ROOM_WALL_W) ? (phi.z - plo.z)
                                                                 : (phi.x - plo.x);
        if (tlo >   csh[ci] - m)  tlo =   csh[ci] - m;
        if (tlo < -(csh[ci] - m)) tlo = -(csh[ci] - m);
        gjn = (ckind[ci] == 0 || ckind[ci] == 3) ? 1 : 7;   /* straight derives dR */
        for (gi = 0; gi < 7; gi++) {
            float dH = frac[gi] * (csh[ci] - m);
            for (gj = 0; gj < gjn; gj++) {
                float dR, cost, al;
                vec3  dlo, dhi, cor;
                int   ok = 1;
                dlo = door_at(plo, hwl, hdl, cwh[ci], dH);
                if (ckind[ci] == 0) {              /* straight along x: match world z */
                    dR  = dlo.z - phi.z;
                    dhi = door_at(phi, hwh, hdh, cwr[ci], dR);
                    cor = dhi;
                } else if (ckind[ci] == 3) {       /* straight along z: match world x */
                    dR  = dlo.x - phi.x;
                    dhi = door_at(phi, hwh, hdh, cwr[ci], dR);
                    cor = dhi;
                } else if (ckind[ci] == 1) {       /* H exits x, R enters z */
                    dR  = frac[gj] * (csr[ci] - m);
                    dhi = door_at(phi, hwh, hdh, cwr[ci], dR);
                    cor = vec3_make(dhi.x, plo.y, dlo.z);
                } else {                           /* H exits z, R enters x */
                    dR  = frac[gj] * (csr[ci] - m);
                    dhi = door_at(phi, hwh, hdh, cwr[ci], dR);
                    cor = vec3_make(dlo.x, plo.y, dhi.z);
                }
                if (dR > csr[ci] - m || dR < -(csr[ci] - m)) ok = 0;   /* door off the wall */
                if (ok && door_taken(pl, *np, lo, cwh[ci], dH)) ok = 0;
                if (ok && door_taken(pl, *np, hi, cwr[ci], dR)) ok = 0;
                if (ok && (ckind[ci] == 1 || ckind[ci] == 2)) {        /* corner clear of rooms */
                    if (pt_in_room(cor, plo, hwl, hdl)) ok = 0;
                    if (pt_in_room(cor, phi, hwh, hdh)) ok = 0;
                }
                if (ok) {
                    if (ckind[ci] == 0 || ckind[ci] == 3) {
                        if (leg_clips(ri, nr, dlo.x, dlo.z, dhi.x, dhi.z, hwd, lo, hi)) ok = 0;
                    } else {
                        if (leg_clips(ri, nr, dlo.x, dlo.z, cor.x, cor.z, hwd, lo, hi)) ok = 0;
                        if (leg_clips(ri, nr, cor.x, cor.z, dhi.x, dhi.z, hwd, lo, hi)) ok = 0;
                    }
                }
                if (!ok) continue;
                al   = ((dH - tlo) < 0.0f ? -(dH - tlo) : (dH - tlo))
                         + (dR < 0.0f ? -dR : dR);
                cost = al;
                if (ckind[ci] == 1 || ckind[ci] == 2) cost += 2.0f;            /* a bend costs */
                /* strongly prefer exiting toward the room's dominant direction,
                   so e.g. two mostly-east rooms both use the east wall (spread)
                   rather than one scattering onto a side wall */
                if ((ckind[ci] == 0 || ckind[ci] == 1) && adx < adz) cost += 2.5f;  /* exits x, z dominant */
                if ((ckind[ci] == 2 || ckind[ci] == 3) && adz < adx) cost += 2.5f;  /* exits z, x dominant */
                if (cost < best) {
                    best = cost; found = 1;
                    bWlo = cwh[ci]; bWhi = cwr[ci]; bOlo = dH; bOhi = dR;
                    bDlo = dlo; bCor = cor; bDhi = dhi;
                }
            }
        }
    }
    if (!found) {            /* best-effort: dominant-axis L, centered (may clip) */
        if (adx >= adz) {
            bWlo = hwx; bWhi = rwz;
            bDlo = door_at(plo, hwl, hdl, hwx, 0.0f);
            bDhi = door_at(phi, hwh, hdh, rwz, 0.0f);
            bCor = vec3_make(bDhi.x, plo.y, bDlo.z);
        } else {
            bWlo = hwz; bWhi = rwx;
            bDlo = door_at(plo, hwl, hdl, hwz, 0.0f);
            bDhi = door_at(phi, hwh, hdh, rwx, 0.0f);
            bCor = vec3_make(bDlo.x, plo.y, bDhi.z);
        }
        bOlo = 0.0f; bOhi = 0.0f;
    }
    r->wall_lo = bWlo; r->wall_hi = bWhi;
    r->straight = (bWlo == ROOM_WALL_E || bWlo == ROOM_WALL_W)
                    ? (bWhi == ROOM_WALL_E || bWhi == ROOM_WALL_W)
                    : (bWhi == ROOM_WALL_N || bWhi == ROOM_WALL_S);
    r->door_lo = bDlo; r->corner = bCor; r->door_hi = bDhi;
    if (*np + 2 <= ROUTE_MAX * 2) {
        pl[*np].room = lo; pl[*np].wall = bWlo; pl[*np].off = bOlo; (*np)++;
        pl[*np].room = hi; pl[*np].wall = bWhi; pl[*np].off = bOhi; (*np)++;
    }
}

int route_all(Scene *s, Route *out, int max) {
    RoomInfo   ri[ROUTE_MAX];
    int        nr = collect_rooms(s, ri, ROUTE_MAX);
    int        n = routes_pass1(s, out, max), i, np = 0;
    PlacedDoor placed[ROUTE_MAX * 2];
    for (i = 0; i < n; i++) {
        Route          *r = &out[i];
        const RoomInfo *lo, *hi;
        vec3   plo, phi;
        float  hwl, hdl, hwh, hdh, l1, l2, tot;
        if (!r->valid) continue;
        lo = room_find(ri, nr, r->room_lo);
        hi = room_find(ri, nr, r->room_hi);
        if (!lo || !hi) { r->valid = 0; continue; }   /* a connects target that isn't a room: drop it */
        plo = lo->pos; phi = hi->pos;
        hwl = lo->hw;  hdl = lo->hd;  hwh = hi->hw;  hdh = hi->hd;
        solve_path(ri, nr, r, plo, phi, hwl, hdl, hwh, hdh, placed, &np);
        /* rise: steady climb over the run, corner height interpolated by leg len */
        l1 = (float)sqrt((double)((r->corner.x - r->door_lo.x) * (r->corner.x - r->door_lo.x) +
                                  (r->corner.z - r->door_lo.z) * (r->corner.z - r->door_lo.z)));
        l2 = (float)sqrt((double)((r->door_hi.x - r->corner.x) * (r->door_hi.x - r->corner.x) +
                                  (r->door_hi.z - r->corner.z) * (r->door_hi.z - r->corner.z)));
        tot = l1 + l2;
        r->corner.y = (tot > 1e-4f)
                        ? r->door_lo.y + (r->door_hi.y - r->door_lo.y) * (l1 / tot)
                        : r->door_lo.y;
    }
    return n;
}

/* reader over already-computed routes (no re-solve) — the one-author pattern:
   compute route_all ONCE per rebuild, then query the result many times. */
int route_for_walkway_in(const Route *all, int n, sol_u32 walkway, Route *out) {
    int i;
    for (i = 0; i < n; i++) {
        if (all[i].walkway == walkway && all[i].valid) { *out = all[i]; return 1; }
    }
    return 0;
}

int route_for_walkway(Scene *s, sol_u32 walkway, Route *out) {
    Route all[ROUTE_MAX];
    int   n = route_all(s, all, ROUTE_MAX);
    return route_for_walkway_in(all, n, walkway, out);
}

int route_room_openings_in(const Route *all, int n, Scene *s, sol_u32 room,
                           RoomOpening *out, int max) {
    int  i, m = 0;
    vec3 rp = obj_pos(s, room);
    for (i = 0; i < n && m < max; i++) {
        const Route *r = &all[i];
        if (!r->valid) continue;
        if (r->room_lo == room) {
            int   wall = r->wall_lo;
            float c = (wall == ROOM_WALL_N || wall == ROOM_WALL_S)
                        ? r->door_lo.x - rp.x : r->door_lo.z - rp.z;
            out[m].wall = wall; out[m].center = c;
            out[m].width = ROUTE_DOOR_W; out[m].height = ROUTE_DOOR_H;
            out[m].sill = 0.0f; m++;
        }
        if (m >= max) break;
        if (r->room_hi == room) {
            int   wall = r->wall_hi;
            float c = (wall == ROOM_WALL_N || wall == ROOM_WALL_S)
                        ? r->door_hi.x - rp.x : r->door_hi.z - rp.z;
            out[m].wall = wall; out[m].center = c;
            out[m].width = ROUTE_DOOR_W; out[m].height = ROUTE_DOOR_H;
            out[m].sill = 0.0f; m++;
        }
    }
    return m;
}

int route_room_openings(Scene *s, sol_u32 room, RoomOpening *out, int max) {
    Route all[ROUTE_MAX];
    int   n = route_all(s, all, ROUTE_MAX);
    return route_room_openings_in(all, n, s, room, out, max);
}
