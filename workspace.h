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

#endif /* SOL_WORKSPACE_H */
