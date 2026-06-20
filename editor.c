/* editor.c — see editor.h. Pure CPU; no GLFW, no GL, no AppState. */

#include "editor.h"
#include "mesh.h"       /* mesh_ref_param */
#include "sol_math.h"

#include <string.h>     /* strcmp, memset */

/* The room footprint: center + floor from the parent-0 anchor's pos, w/d from
   the "room" shell child's mesh_params. */
RoomRect editor_room_rect(Scene *s, sol_u32 room) {
    RoomRect     r;
    SceneObject *ro = scene_get(s, room);
    sol_u32      i;
    float        w = 10.0f, d = 10.0f;
    r.cx = r.cz = r.floor_y = 0.0f; r.hw = r.hd = 5.0f;
    if (!ro) return r;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        if (o->parent == room && o->mesh_ref && strcmp(o->mesh_ref, "room") == 0) {
            w = mesh_ref_param("room", o->mesh_params, o->mesh_param_count, "w");
            d = mesh_ref_param("room", o->mesh_params, o->mesh_param_count, "d");
            break;
        }
    }
    r.cx = ro->pos.x; r.cz = ro->pos.z; r.floor_y = ro->pos.y;
    r.hw = 0.5f * w;  r.hd = 0.5f * d;
    return r;
}

/* Classify a ground point against a footprint, with a grab band (world units)
   straddling each wall. Outside (footprint + band) -> NONE. */
EditZone editor_classify(RoomRect r, float gx, float gz, float band) {
    float dx = gx - r.cx, dz = gz - r.cz;
    int   xp, xn, zp, zn;
    if (dx >  r.hw + band || dx < -r.hw - band) return EDIT_ZONE_NONE;
    if (dz >  r.hd + band || dz < -r.hd - band) return EDIT_ZONE_NONE;
    xp = (dx >  r.hw - band);
    xn = (dx < -r.hw + band);
    zp = (dz >  r.hd - band);
    zn = (dz < -r.hd + band);
    if (xp && zp) return EDIT_ZONE_CORNER_XPZP;
    if (xp && zn) return EDIT_ZONE_CORNER_XPZN;
    if (xn && zp) return EDIT_ZONE_CORNER_XNZP;
    if (xn && zn) return EDIT_ZONE_CORNER_XNZN;
    if (xp) return EDIT_ZONE_EDGE_XP;
    if (xn) return EDIT_ZONE_EDGE_XN;
    if (zp) return EDIT_ZONE_EDGE_ZP;
    if (zn) return EDIT_ZONE_EDGE_ZN;
    return EDIT_ZONE_BODY;
}

/* Resize one axis with the OPPOSITE wall held fixed. sign = which face moves
   (+1 = the +axis wall, -1 = the -axis wall). face_world = cursor coord on
   that axis. Clamps so the box keeps at least min_size. */
void editor_resize_axis(float center, float half, int sign, float face_world,
                        float min_size, float *new_center, float *new_half) {
    float opp  = center - (float)sign * half;   /* the wall that stays put */
    float face = face_world;
    if (sign > 0) { if (face < opp + min_size) face = opp + min_size; }
    else          { if (face > opp - min_size) face = opp - min_size; }
    *new_half   = 0.5f * (face > opp ? face - opp : opp - face);
    *new_center = 0.5f * (opp + face);
}

/* Can we add a walkway between rooms a and b? Both must be rooms, distinct, and
   not already joined by a walkway. */
sol_bool editor_can_connect(Scene *s, sol_u32 a, sol_u32 b) {
    sol_u32 i;
    if (a == 0 || b == 0 || a == b) return SOL_FALSE;
    if (!scene_meta_get(s, a, "room_type")) return SOL_FALSE;
    if (!scene_meta_get(s, b, "room_type")) return SOL_FALSE;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        sol_u32      x = 0, y = 0, j;
        if (!o->mesh_ref || strcmp(o->mesh_ref, "walkway") != 0) continue;
        for (j = 0; j < o->rel_count; j++) {
            if (strcmp(o->relations[j].type, "connects") != 0) continue;
            if      (x == 0) x = o->relations[j].target;
            else if (y == 0) y = o->relations[j].target;
        }
        if ((x == a && y == b) || (x == b && y == a)) return SOL_FALSE;
    }
    return SOL_TRUE;
}

/* Create a walkway joining rooms a and b (parent-0, two "connects" edges).
   Returns the new walkway handle, or 0 if the pair is invalid. */
sol_u32 editor_connect(Scene *s, sol_u32 a, sol_u32 b) {
    Mesh    empty;
    sol_u32 wk;
    if (!editor_can_connect(s, a, b)) return 0;
    memset(&empty, 0, sizeof empty);
    wk = scene_add(s, 0, empty, vec3_make(0.0f, 0.0f, 0.0f), quat_identity(),
                   vec3_make(1.0f, 1.0f, 1.0f));
    scene_mesh_ref_set(s, wk, "walkway");
    scene_rel_add(s, wk, "connects", a);
    scene_rel_add(s, wk, "connects", b);
    return wk;
}

/* Remove a walkway (its connects edges go with it). No-op if not a walkway. */
void editor_disconnect(Scene *s, sol_u32 walkway) {
    SceneObject *o = scene_get(s, walkway);
    if (o && o->mesh_ref && strcmp(o->mesh_ref, "walkway") == 0)
        scene_remove(s, walkway);
}

/* Move a room: write its anchor's world XZ (anchors are parent-0). */
void editor_apply_move(Scene *s, sol_u32 room, float cx, float cz) {
    SceneObject *o = scene_get(s, room);
    if (!o) return;
    o->pos.x = cx;
    o->pos.z = cz;
}

/* Resize a room by dragging zone's wall(s) to the ground point (gx,gz), keeping
   the opposite wall(s) fixed. Writes the new center to the anchor and the new
   w/d to the shell child's params (h + wall flags preserved). */
void editor_apply_resize(Scene *s, sol_u32 room, EditZone zone, float gx, float gz) {
    RoomRect     r  = editor_room_rect(s, room);
    SceneObject *ro = scene_get(s, room);
    sol_u32      i;
    float        ncx = r.cx, ncz = r.cz, nhw = r.hw, nhd = r.hd;
    int          tx = 0, tz = 0, sx = 0, sz = 0;
    if (!ro) return;
    switch (zone) {
        case EDIT_ZONE_EDGE_XP:     tx = 1; sx =  1; break;
        case EDIT_ZONE_EDGE_XN:     tx = 1; sx = -1; break;
        case EDIT_ZONE_EDGE_ZP:     tz = 1; sz =  1; break;
        case EDIT_ZONE_EDGE_ZN:     tz = 1; sz = -1; break;
        case EDIT_ZONE_CORNER_XPZP: tx = tz = 1; sx =  1; sz =  1; break;
        case EDIT_ZONE_CORNER_XPZN: tx = tz = 1; sx =  1; sz = -1; break;
        case EDIT_ZONE_CORNER_XNZP: tx = tz = 1; sx = -1; sz =  1; break;
        case EDIT_ZONE_CORNER_XNZN: tx = tz = 1; sx = -1; sz = -1; break;
        default: return;
    }
    if (tx) editor_resize_axis(r.cx, r.hw, sx, gx, EDITOR_MIN_SIZE, &ncx, &nhw);
    if (tz) editor_resize_axis(r.cz, r.hd, sz, gz, EDITOR_MIN_SIZE, &ncz, &nhd);
    ro->pos.x = ncx;
    ro->pos.z = ncz;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        if (o->parent == room && o->mesh_ref && strcmp(o->mesh_ref, "room") == 0) {
            float p[8];
            int   k;
            for (k = 0; k < 8; k++)
                p[k] = (k < o->mesh_param_count) ? o->mesh_params[k] : 0.0f;
            if (o->mesh_param_count < 8) {       /* defensive: shells are made with 8 */
                p[2] = 3.0f; p[3] = 1.0f; p[4] = 1.0f; p[5] = 1.0f; p[6] = 1.0f; p[7] = 0.0f;
            }
            p[0] = 2.0f * nhw;
            p[1] = 2.0f * nhd;
            scene_mesh_params_set(s, o->handle, p, 8);
            break;
        }
    }
}

/* world ground point where the cursor ray meets the plane y = floor_y */
static sol_bool editor_ground_at(const Camera *c, float nx, float ny, float aspect,
                                 float floor_y, vec3 *out) {
    Ray   r = camera_ray(c, nx, ny, aspect);
    float t;
    if (!ray_vs_plane(r, vec3_make(0.0f, floor_y, 0.0f),
                      vec3_make(0.0f, 1.0f, 0.0f), &t)) return SOL_FALSE;
    *out = vec3_add(r.origin, vec3_scale(r.dir, t));
    return SOL_TRUE;
}

/* nearest room (parent handle) the cursor falls within (body or grab band).
   Fills zone + ground point. 0 if none. */
static sol_u32 editor_room_under(Scene *s, const Camera *c, float nx, float ny,
                                 float aspect, EditZone *zone_out, vec3 *gp_out) {
    sol_u32 i, best = 0;
    float   bestd = 1e30f;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        RoomRect     r;
        vec3         gp;
        EditZone     z;
        float        dx, dz, dd;
        if (o->mesh_ref) continue;                      /* anchors are empties */
        if (!scene_meta_get(s, o->handle, "room_type")) continue;
        r = editor_room_rect(s, o->handle);
        if (!editor_ground_at(c, nx, ny, aspect, r.floor_y, &gp)) continue;
        z = editor_classify(r, gp.x, gp.z, EDITOR_GRAB_BAND);
        if (z == EDIT_ZONE_NONE) continue;
        dx = gp.x - r.cx; dz = gp.z - r.cz; dd = dx * dx + dz * dz;
        if (dd < bestd) {
            bestd = dd; best = o->handle;
            if (zone_out) *zone_out = z;
            if (gp_out)   *gp_out   = gp;
        }
    }
    return best;
}

/* is the cursor over a room's connection node (projected to screen)? */
static sol_bool editor_port_hit(const Camera *c, RoomRect r, float nx, float ny,
                                float aspect) {
    vec3 pw = vec3_make(r.cx, r.floor_y + EDITOR_PORT_LIFT, r.cz);
    mat4 vp = mat4_mul(camera_proj(c, aspect), camera_view(c));
    vec3 ndc;
    float ex, ey;
    if (!mat4_project_point(vp, pw, &ndc)) return SOL_FALSE;
    ex = ndc.x - nx; ey = ndc.y - ny;
    return (sol_bool)(ex * ex + ey * ey <= EDITOR_PORT_NDC * EDITOR_PORT_NDC);
}

void editor_press(Editor *e, Scene *s, const Camera *c,
                  float nx, float ny, float aspect) {
    EditZone z = EDIT_ZONE_NONE;
    vec3     gp;
    sol_u32  room;
    e->action = EDIT_IDLE;
    room = editor_room_under(s, c, nx, ny, aspect, &z, &gp);
    if (room == 0) return;                          /* main.c may select a walkway */
    {
        RoomRect r = editor_room_rect(s, room);
        if (editor_port_hit(c, r, nx, ny, aspect)) {
            e->action       = EDIT_CONNECT;
            e->room         = room;          /* highlight the connect source */
            e->connect_from = room;
            e->cursor_world = gp;
            e->selected_wk  = 0;
            return;
        }
        e->room        = room;
        e->selected_wk = 0;
        if (z == EDIT_ZONE_BODY) {
            e->action   = EDIT_MOVE;
            e->grab_off = vec3_make(r.cx - gp.x, 0.0f, r.cz - gp.z);
        } else {
            e->action = EDIT_RESIZE;
            e->zone   = z;
        }
    }
}

void editor_drag(Editor *e, Scene *s, const Camera *c,
                 float nx, float ny, float aspect) {
    vec3     gp;
    RoomRect r;
    if (e->action == EDIT_IDLE) return;
    if (e->action == EDIT_CONNECT) {
        r = editor_room_rect(s, e->connect_from);
        if (editor_ground_at(c, nx, ny, aspect, r.floor_y, &gp)) e->cursor_world = gp;
        return;
    }
    r = editor_room_rect(s, e->room);
    if (!editor_ground_at(c, nx, ny, aspect, r.floor_y, &gp)) return;
    if (e->action == EDIT_MOVE) {
        editor_apply_move(s, e->room, gp.x + e->grab_off.x, gp.z + e->grab_off.z);
        e->dirty = SOL_TRUE;
    } else if (e->action == EDIT_RESIZE) {
        editor_apply_resize(s, e->room, e->zone, gp.x, gp.z);
        e->dirty = SOL_TRUE;
    }
}

void editor_release(Editor *e, Scene *s, const Camera *c,
                    float nx, float ny, float aspect) {
    if (e->action == EDIT_CONNECT) {
        EditZone z;
        vec3     gp;
        sol_u32  target = editor_room_under(s, c, nx, ny, aspect, &z, &gp);
        if (target != 0 && editor_connect(s, e->connect_from, target) != 0) {
            e->dirty = SOL_TRUE; e->commit = SOL_TRUE;
        }
    } else if (e->action == EDIT_MOVE || e->action == EDIT_RESIZE) {
        e->commit = SOL_TRUE;
    }
    e->action       = EDIT_IDLE;
    e->connect_from = 0;
}

void editor_delete_selected(Editor *e, Scene *s) {
    if (e->selected_wk != 0) {
        editor_disconnect(s, e->selected_wk);
        e->selected_wk = 0;
        e->dirty = SOL_TRUE; e->commit = SOL_TRUE;
    }
}
