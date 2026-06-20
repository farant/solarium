/* workspace.c — see workspace.h. */
#include "workspace.h"
#include "sol_math.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

const char *workspace_of(Scene *s, sol_u32 handle) {
    sol_u32 h = handle;
    int     guard = 0;                 /* parent-chain runaway bound */
    while (h != 0 && guard++ < 64) {
        const char  *w = scene_meta_get(s, h, "workspace");
        SceneObject *o;
        if (w) return w;
        o = scene_get(s, h);
        if (!o) break;
        h = o->parent;
    }
    return "home";
}

sol_bool scene_object_active(Scene *s, sol_u32 handle) {
    if (s->active_ws[0] == '\0') return SOL_TRUE;     /* unfiltered */
    return (sol_bool)(strcmp(workspace_of(s, handle), s->active_ws) == 0);
}

sol_u32 workspace_anchor_find(Scene *s, const char *name) {
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        const char *n = scene_meta_get(s, s->objects[i].handle, "workspace_name");
        if (n && strcmp(n, name) == 0) return s->objects[i].handle;
    }
    return 0;
}

sol_u32 workspace_anchor_add(Scene *s, const char *name) {
    Mesh    empty;
    sol_u32 h = workspace_anchor_find(s, name);
    vec3    one, zero;
    quat    qid;
    if (h != 0) return h;
    memset(&empty, 0, sizeof empty);
    one.x = one.y = one.z = 1.0f; zero.x = zero.y = zero.z = 0.0f;
    qid.x = qid.y = qid.z = 0.0f; qid.w = 1.0f;
    h = scene_add(s, 0, empty, zero, qid, one);
    scene_meta_set(s, h, "workspace_name", name);
    return h;
}

int workspace_gate_count(Scene *s, const char *ws) {
    sol_u32 i; int n = 0;
    for (i = 0; i < s->count; i++) {
        if (s->objects[i].kind != KIND_PORTAL) continue;
        if (strcmp(workspace_of(s, s->objects[i].handle), ws) == 0) n++;
    }
    return n;
}

sol_u32 workspace_add_gate(Scene *s, const char *ws, vec3 pos, float yaw,
                           const char *self_id, const char *target_ws,
                           const char *target_id, const char *label) {
    Mesh    empty;
    sol_u32 h;
    vec3    one;
    quat    rot;
    memset(&empty, 0, sizeof empty);
    one.x = one.y = one.z = 1.0f;
    rot = quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), yaw);
    h = scene_add(s, 0, empty, pos, rot, one);
    scene_kind_set(s, h, KIND_PORTAL);
    scene_mesh_ref_set(s, h, "gate");
    scene_meta_set(s, h, "workspace", ws);
    scene_meta_set(s, h, "portal_id", self_id);
    scene_meta_set(s, h, "target_ws", target_ws);
    scene_meta_set(s, h, "target_portal_id", target_id);
    if (label) scene_meta_set(s, h, "name", label);
    return h;
}

sol_u32 workspace_add_home_room(Scene *s, const char *ws, vec3 pos) {
    Mesh    empty;
    sol_u32 room, shell;
    float   p[8];
    vec3    one, zero;
    quat    qid;
    memset(&empty, 0, sizeof empty);
    one.x = one.y = one.z = 1.0f; zero.x = zero.y = zero.z = 0.0f;
    qid.x = qid.y = qid.z = 0.0f; qid.w = 1.0f;
    room = scene_add(s, 0, empty, pos, qid, one);
    scene_meta_set(s, room, "room_type", "home");
    scene_meta_set(s, room, "name", ws);
    scene_meta_set(s, room, "workspace", ws);
    shell = scene_add(s, room, empty, zero, qid, one);
    scene_mesh_ref_set(s, shell, "room");
    p[0] = 8.0f; p[1] = 8.0f; p[2] = 3.0f; p[3] = 1.0f;
    p[4] = 1.0f; p[5] = 1.0f; p[6] = 1.0f; p[7] = 0.0f;
    scene_mesh_params_set(s, shell, p, 8);
    return room;
}

sol_u32 workspace_link(Scene *s,
                       const char *from_ws, vec3 from_pos, float from_yaw,
                       const char *to_ws,   vec3 to_pos,   float to_yaw) {
    char id_from[SOL_WS_NAME_CAP + 16];
    char id_to[SOL_WS_NAME_CAP + 16];
    sol_u32 out;
    snprintf(id_from, sizeof id_from, "%s-%d", from_ws, workspace_gate_count(s, from_ws) + 1);
    snprintf(id_to,   sizeof id_to,   "%s-%d", to_ws,   workspace_gate_count(s, to_ws)   + 1);
    out = workspace_add_gate(s, from_ws, from_pos, from_yaw,
                             id_from, to_ws, id_to, to_ws);
    workspace_add_gate(s, to_ws, to_pos, to_yaw,
                       id_to, from_ws, id_from, from_ws);
    return out;
}

sol_u32 workspace_find_gate_by_id(Scene *s, const char *portal_id) {
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        const char *pid;
        if (s->objects[i].kind != KIND_PORTAL) continue;
        pid = scene_meta_get(s, s->objects[i].handle, "portal_id");
        if (pid && strcmp(pid, portal_id) == 0) return s->objects[i].handle;
    }
    return 0;
}

void workspace_spawn_at_gate(Scene *s, sol_u32 gate, float stand, float eye,
                             vec3 *out_pos, float *out_yaw) {
    SceneObject *o = scene_get(s, gate);
    mat4  m;
    vec3  gp, fwd;
    quat  q;
    float yaw;
    if (!o) { out_pos->x = out_pos->y = out_pos->z = 0.0f; *out_yaw = 0.0f; return; }
    m   = scene_world_matrix(s, o);
    gp  = vec3_make(m.m[12], m.m[13], m.m[14]);             /* translation column */
    q   = o->rot;                                           /* pure-yaw quaternion */
    yaw = 2.0f * (float)atan2((double)q.y, (double)q.w);
    fwd = vec3_make((float)cos((double)yaw), 0.0f, (float)sin((double)yaw));
    out_pos->x = gp.x + fwd.x * stand;
    out_pos->y = gp.y + eye;
    out_pos->z = gp.z + fwd.z * stand;
    *out_yaw   = yaw;
}
