/* component.h — the component model (P4 item 6): behavior as DATA. The
   registry-as-schema pattern's third application (meshes, assets, now
   behavior): a registry entry declares a type name, a param schema with
   defaults, an update function, and a per-instance state size; the scene
   attaches behaviors by name and the registry says what they mean.
   Deliberately NOT a scripting language — these are named, parameterized
   behaviors the engine provides in C (the registry is the socket a future
   interpreter could plug into; we built the outlet, not the appliance).

   THE OVERLAY DOCTRINE (TODO4 §1.6) is enforced here: update functions
   write the object's TRANSIENT overlay fields (overlay_pos/rot/glow),
   which scene.c composes into the world walks — they never touch the
   persisted pos/rot/scale. Saving writes the BASE the user placed; a
   mid-spin save reloads into a scene that resumes the dance from the
   base, never a baked frame. Overlays RESET every frame before the walk,
   so removing a component stops its motion at once, and two components
   on one object ACCUMULATE (two spins compose). */

#ifndef COMPONENT_H
#define COMPONENT_H

#include "scene.h"

/* an update receives EFFECTIVE params — the attachment's prefix merged
   with the schema defaults, the mesh-registry rule — plus its lazily
   allocated state and both clocks: t is ABSOLUTE (deterministic overlays;
   angle = speed * t survives load and pause), dt is for the genuinely
   integrating (item 7's particles) */
typedef void (*ComponentFn)(Scene *s, SceneObject *o, const float *params,
                            void *state, float t, float dt);

/* schema lookup for the io layer and tests: param count, or -1 for an
   unknown type; names/defaults out-params may be NULL */
int component_schema(const char *type, const char *const **names,
                     const float **defaults);

/* one pass over every object's attachments per frame: reset the overlays,
   merge params, run each update. Unknown types are skipped INTACT —
   forward compatibility, the format's standing rule. */
void components_update(Scene *s, float t, float dt);

#endif /* COMPONENT_H */
