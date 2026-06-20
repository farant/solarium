/* workspace.h — workspaces are tagged partitions of the one scene; exactly
   one is ACTIVE. This module owns the membership predicate every scene reader
   consults, plus the portal-pair authoring used by the palette commands.
   Headless: only public scene/mesh API, no GL. */
#ifndef SOL_WORKSPACE_H
#define SOL_WORKSPACE_H

#include "scene.h"

/* The workspace an object belongs to: walk the parent chain to the first
   meta["workspace"]; absent before parent 0 => "home". Never NULL. */
const char *workspace_of(Scene *s, sol_u32 handle);

/* Visible/live under the current filter? s->active_ws == "" => always true. */
sol_bool    scene_object_active(Scene *s, sol_u32 handle);

/* a WorkspaceAnchor: a parent-0 empty carrying meta["workspace_name"]. The
   identity + display handle of a workspace, and what "Portal to..." lists. */
sol_u32 workspace_anchor_find(Scene *s, const char *name);   /* 0 if none */
sol_u32 workspace_anchor_add(Scene *s, const char *name);    /* find-or-create */

/* count of KIND_PORTAL gates tagged into workspace `ws` (for id minting) */
int     workspace_gate_count(Scene *s, const char *ws);

/* a fresh open-topped home room tagged into `ws`, at `pos`. Returns the room
   anchor handle. (Factored to mirror populate_home_scene's room.) */
sol_u32 workspace_add_home_room(Scene *s, const char *ws, vec3 pos);

/* Add one gate: a KIND_PORTAL object, mesh_ref "gate", tagged into `ws`, with
   the full link meta. `yaw` is its facing (radians about world up). Returns
   the gate handle. */
sol_u32 workspace_add_gate(Scene *s, const char *ws, vec3 pos, float yaw,
                           const char *self_id, const char *target_ws,
                           const char *target_id, const char *label);

/* Create a LINKED PAIR: an outbound gate in `from_ws` at (from_pos,from_yaw)
   and a return gate in `to_ws` at (to_pos,to_yaw), cross-referenced by id.
   Each gate's display name is the OTHER workspace's name, derived internally.
   Returns the outbound gate handle. */
sol_u32 workspace_link(Scene *s,
                       const char *from_ws, vec3 from_pos, float from_yaw,
                       const char *to_ws,   vec3 to_pos,   float to_yaw);

/* the gate carrying meta["portal_id"] == id, or 0. */
sol_u32 workspace_find_gate_by_id(Scene *s, const char *portal_id);

/* Where a traveller arriving at `gate` stands: `stand` meters in front of the
   gate along its facing, raised by `eye`; *out_yaw is the gate's facing (you
   arrive looking the way it points, into the world). */
void    workspace_spawn_at_gate(Scene *s, sol_u32 gate, float stand, float eye,
                                vec3 *out_pos, float *out_yaw);

#endif /* SOL_WORKSPACE_H */
