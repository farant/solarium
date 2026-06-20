/* descend.c — see descend.h. Headless: no GL, no filesystem. */

#include "descend.h"
#include "mesh.h"       /* ROOM_WALL_* */
#include "route.h"      /* ROUTE_DOOR_W / ROUTE_DOOR_H */
#include "sol_math.h"

#include <string.h>     /* strcmp, strrchr, memset */

/* a point's room: home/mirror/preview anchor whose footprint contains it */
sol_u32 descend_room_at(Scene *s, vec3 p) {
    sol_u32 i, best = 0;
    float   bestd = 1e30f;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        const char  *rt;
        RoomRect     r;
        float        dy, dx, dz, dd;
        if (o->mesh_ref) continue;                 /* anchors are empties */
        rt = scene_meta_get(s, o->handle, "room_type");
        if (!rt) continue;
        if (strcmp(rt, "home") != 0 && strcmp(rt, "mirror") != 0 &&
            strcmp(rt, "preview") != 0) continue;
        r = editor_room_rect(s, o->handle);
        if (p.x < r.cx - r.hw || p.x > r.cx + r.hw) continue;
        if (p.z < r.cz - r.hd || p.z > r.cz + r.hd) continue;
        dy = p.y - r.floor_y;
        if (dy < -0.5f || dy > 5.0f) continue;     /* near this floor (room h ~3 + headroom) */
        dx = p.x - r.cx; dz = p.z - r.cz; dd = dx * dx + dz * dz;
        if (dd < bestd) { bestd = dd; best = o->handle; }
    }
    return best;
}

vec3 descend_door_point(RoomRect r, int wall, float offset) {
    if (wall == ROOM_WALL_N) return vec3_make(r.cx + offset, r.floor_y, r.cz - r.hd);
    if (wall == ROOM_WALL_S) return vec3_make(r.cx + offset, r.floor_y, r.cz + r.hd);
    if (wall == ROOM_WALL_E) return vec3_make(r.cx + r.hw, r.floor_y, r.cz + offset);
    return vec3_make(r.cx - r.hw, r.floor_y, r.cz + offset);   /* W */
}

int descend_wall_aim(RoomRect r, Ray ray, float door_h, int *wall, float *offset) {
    /* the 4 interior walls in ROOM_WALL_* order (N,E,S,W): a point on the plane,
       the inward normal, the run-axis half-span, and whether the run axis is X */
    struct { vec3 pt, n; float half; int runx; } w[4];
    int   bestw = -1, k;
    float bestt = 1e30f, besto = 0.0f;
    w[0].pt = vec3_make(r.cx, r.floor_y, r.cz - r.hd); w[0].n = vec3_make(0.0f,0.0f, 1.0f); w[0].half = r.hw; w[0].runx = 1; /* N */
    w[1].pt = vec3_make(r.cx + r.hw, r.floor_y, r.cz); w[1].n = vec3_make(-1.0f,0.0f,0.0f); w[1].half = r.hd; w[1].runx = 0; /* E */
    w[2].pt = vec3_make(r.cx, r.floor_y, r.cz + r.hd); w[2].n = vec3_make(0.0f,0.0f,-1.0f); w[2].half = r.hw; w[2].runx = 1; /* S */
    w[3].pt = vec3_make(r.cx - r.hw, r.floor_y, r.cz); w[3].n = vec3_make( 1.0f,0.0f,0.0f); w[3].half = r.hd; w[3].runx = 0; /* W */
    for (k = 0; k < 4; k++) {
        float t, run, lim;
        vec3  hit;
        if (!ray_vs_plane(ray, w[k].pt, w[k].n, &t)) continue;
        if (t <= 0.05f || t >= bestt) continue;
        hit = vec3_add(ray.origin, vec3_scale(ray.dir, t));
        if (hit.y < r.floor_y - 0.1f || hit.y > r.floor_y + door_h + 1.0f) continue;
        run = w[k].runx ? (hit.x - r.cx) : (hit.z - r.cz);
        lim = w[k].half - ROUTE_DOOR_W * 0.5f - 0.4f;   /* keep the door off the corners */
        if (lim < 0.0f) continue;
        if (run < -lim) run = -lim;
        if (run >  lim) run =  lim;
        bestt = t; bestw = k; besto = run;
    }
    if (bestw < 0) return 0;
    *wall = bestw;       /* 0/1/2/3 == ROOM_WALL_N/E/S/W */
    *offset = besto;
    return 1;
}
