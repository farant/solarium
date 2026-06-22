/* descend.c — see descend.h. Headless: no GL, no filesystem. */

#include "descend.h"
#include "mesh.h"       /* ROOM_WALL_* */
#include "route.h"      /* ROUTE_DOOR_W / ROUTE_DOOR_H */
#include "sol_math.h"
#include "material.h"   /* Material, material_default */
#include "workspace.h"  /* workspace_of */

#include <string.h>     /* strcmp, strrchr, memset, strncpy */

/* a point's room: the home/mirror anchor whose footprint contains it */
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
        if (strcmp(rt, "home") != 0 && strcmp(rt, "mirror") != 0) continue;
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

int descend_wall_mount(RoomRect r, Ray ray, float ceil_y,
                       float w_half, float h_half, float t,
                       int *out_wall, vec3 *out_center) {
    struct { vec3 pt, n; float half; int runx; } w[4];
    int   bestw = -1, k;
    float bestt = 1e30f;
    vec3  besthit;
    besthit.x = 0.0f; besthit.y = 0.0f; besthit.z = 0.0f;
    w[0].pt = vec3_make(r.cx, r.floor_y, r.cz - r.hd); w[0].n = vec3_make(0.0f,0.0f, 1.0f); w[0].half = r.hw; w[0].runx = 1; /* N */
    w[1].pt = vec3_make(r.cx + r.hw, r.floor_y, r.cz); w[1].n = vec3_make(-1.0f,0.0f,0.0f); w[1].half = r.hd; w[1].runx = 0; /* E */
    w[2].pt = vec3_make(r.cx, r.floor_y, r.cz + r.hd); w[2].n = vec3_make(0.0f,0.0f,-1.0f); w[2].half = r.hw; w[2].runx = 1; /* S */
    w[3].pt = vec3_make(r.cx - r.hw, r.floor_y, r.cz); w[3].n = vec3_make( 1.0f,0.0f,0.0f); w[3].half = r.hd; w[3].runx = 0; /* W */
    for (k = 0; k < 4; k++) {
        float t0;
        vec3  hit;
        if (!ray_vs_plane(ray, w[k].pt, w[k].n, &t0)) continue;
        if (t0 <= 0.05f || t0 >= bestt) continue;
        hit = vec3_add(ray.origin, vec3_scale(ray.dir, t0));
        if (hit.y < r.floor_y - 0.1f || hit.y > ceil_y + 0.1f) continue;
        bestt = t0; bestw = k; besthit = hit;
    }
    if (bestw < 0) return 0;
    {
        float lim = w[bestw].half - w_half;          /* along-wall room for the board */
        float ylo = r.floor_y + h_half, yhi = ceil_y - h_half;
        float run, cy;
        vec3  c;
        if (lim < 0.0f || yhi < ylo) return 0;       /* board bigger than the wall */
        run = w[bestw].runx ? (besthit.x - r.cx) : (besthit.z - r.cz);
        if (run < -lim) run = -lim;
        if (run >  lim) run =  lim;
        cy = besthit.y;
        if (cy < ylo) cy = ylo;
        if (cy > yhi) cy = yhi;
        if (w[bestw].runx) { c.x = r.cx + run; c.z = w[bestw].pt.z; }
        else               { c.z = r.cz + run; c.x = w[bestw].pt.x; }
        c.x += w[bestw].n.x * (t * 0.5f);            /* push out so the back is flush */
        c.z += w[bestw].n.z * (t * 0.5f);
        c.y  = cy;
        *out_wall   = bestw;
        *out_center = c;
        return 1;
    }
}

void board_corners(vec3 p, float w, float h, vec3 u, vec3 out[4]) {
    vec3 half = vec3_scale(u, w * 0.5f);
    vec3 up   = vec3_make(0.0f, h, 0.0f);
    out[0] = vec3_sub(p, half);                          /* bottom-left  */
    out[1] = vec3_add(p, half);                          /* bottom-right */
    out[2] = vec3_add(vec3_add(p, half), up);            /* top-right    */
    out[3] = vec3_add(vec3_sub(p, half), up);            /* top-left     */
}

void board_resize_corner(vec3 anchor, vec3 dragged, vec3 u, float min_size,
                         float aspect, float *out_w, float *out_h, vec3 *out_origin) {
    vec3  d  = vec3_sub(dragged, anchor);
    float du = vec3_dot(d, u);
    float dv = dragged.y - anchor.y;
    float su = (du < 0.0f) ? -1.0f : 1.0f;
    float sv = (dv < 0.0f) ? -1.0f : 1.0f;
    float w  = (du < 0.0f) ? -du : du;
    float h  = (dv < 0.0f) ? -dv : dv;
    vec3  p;
    if (w < min_size) w = min_size;
    if (h < min_size) h = min_size;
    if (aspect > 0.0f) {                          /* lock w:h = aspect */
        if (w / aspect >= h) h = w / aspect;      /* the wider drag drives */
        else                 w = h * aspect;
        if (w < min_size) { w = min_size; h = w / aspect; }
        if (h < min_size) { h = min_size; w = h * aspect; }
    }
    p   = vec3_add(anchor, vec3_scale(u, su * w * 0.5f));
    p.y = (sv >= 0.0f) ? anchor.y : anchor.y - h;
    *out_w = w; *out_h = h; *out_origin = p;
}

#define DESCEND_GAP 4.0f   /* clear gap between the parent wall and the sub-room */

/* does a 2*half footprint at center c overlap an existing room close in Y?
   (mirrors main.c's root_spot_occupied but scene-level over Scene) */
static sol_bool descend_spot_occupied(Scene *s, vec3 c, float half) {
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        const char  *rt;
        RoomRect     r;
        float        e;
        if (o->mesh_ref) continue;
        rt = scene_meta_get(s, o->handle, "room_type");
        if (!rt) continue;
        if (strcmp(rt, "home") != 0 && strcmp(rt, "mirror") != 0) continue;
        r = editor_room_rect(s, o->handle);
        e = (r.hw > r.hd) ? r.hw : r.hd;
        if ((c.y > r.floor_y ? c.y - r.floor_y : r.floor_y - c.y) >= 3.5f) continue;
        if (c.x + half < r.cx - e || c.x - half > r.cx + e) continue;
        if (c.z + half < r.cz - e || c.z - half > r.cz + e) continue;
        return SOL_TRUE;
    }
    return SOL_FALSE;
}

sol_u32 descend_plant(Scene *s, sol_u32 parent_room, sol_u32 folder_card,
                      int wall, float offset) {
    SceneObject *card = scene_get(s, folder_card);
    Mesh         empty;
    RoomRect     pr;
    vec3         door, outn, center;
    sol_u32      room, shell, wk;
    float        p[8];
    const char  *path, *name, *slash;
    Material     stone = material_default();
    int          guard;
    char         ws[SOL_WS_NAME_CAP];
    if (!card || card->kind != KIND_FOLDER) return 0;
    if (scene_meta_get(s, folder_card, "planted")) return 0;   /* already opened */
    if (!card->content) return 0;
    path = card->content;   /* capture BEFORE any scene_add reallocs s->objects (dangles card) */
    strncpy(ws, workspace_of(s, parent_room), SOL_WS_NAME_CAP - 1);
    ws[SOL_WS_NAME_CAP - 1] = '\0';   /* parent's workspace, copied before scene_add reallocs */

    pr   = editor_room_rect(s, parent_room);
    door = descend_door_point(pr, wall, offset);
    if      (wall == ROOM_WALL_N) outn = vec3_make(0.0f, 0.0f, -1.0f);
    else if (wall == ROOM_WALL_S) outn = vec3_make(0.0f, 0.0f,  1.0f);
    else if (wall == ROOM_WALL_E) outn = vec3_make(1.0f, 0.0f,  0.0f);
    else                          outn = vec3_make(-1.0f, 0.0f, 0.0f);   /* W */
    center   = vec3_add(door, vec3_scale(outn, DESCEND_GAP + 5.0f));     /* gap + half sub-room depth */
    center.y = pr.floor_y;
    guard = 0;
    while (descend_spot_occupied(s, center, 5.5f) && guard < 20) {
        center.y += 5.0f; guard++;                 /* 1-D Y nudge until clear */
    }

    memset(&empty, 0, sizeof empty);
    room  = scene_add(s, 0, empty, center, quat_identity(), vec3_make(1.0f,1.0f,1.0f));
    slash = strrchr(path, '/');
    name  = (slash && slash[1]) ? slash + 1 : path;
    scene_meta_set(s, room, "room_type",   "mirror");   /* a real room: routes + scans like a root */
    scene_meta_set(s, room, "source_path", path);
    scene_meta_set(s, room, "name",        name);
    scene_meta_set(s, room, "workspace",   ws);

    shell = scene_add(s, room, empty, vec3_make(0.0f,0.0f,0.0f), quat_identity(),
                      vec3_make(1.0f,1.0f,1.0f));
    scene_mesh_ref_set(s, shell, "room");
    p[0]=10.0f; p[1]=10.0f; p[2]=3.0f; p[3]=1.0f; p[4]=1.0f; p[5]=1.0f; p[6]=1.0f; p[7]=0.0f;
    scene_mesh_params_set(s, shell, p, 8);
    stone.base_color = vec3_make(0.55f, 0.53f, 0.50f);   /* same stone as a root room */
    stone.roughness  = 0.92f;
    scene_material_set(s, shell, stone);

    wk = scene_add(s, 0, empty, vec3_make(0.0f,0.0f,0.0f), quat_identity(),
                   vec3_make(1.0f,1.0f,1.0f));
    scene_mesh_ref_set(s, wk, "walkway");
    scene_rel_add(s, wk, "connects", parent_room);
    scene_rel_add(s, wk, "connects", room);
    scene_meta_set(s, wk, "workspace", ws);

    scene_meta_set(s, folder_card, "planted", "1");   /* opened: refuse a duplicate room */
    return room;
}
