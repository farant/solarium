#ifndef SCENE_H
#define SCENE_H

#include "sol_base.h"
#include "sol_types.h"
#include "mesh.h"

/* One object in the scene. Identity is the stable `handle`, NOT the array
   index — that decoupling is the whole point (it survives deletion/reload).
   The persistent ULID nid and the parent/metadata/relationship/content slots
   arrive in later checkpoints; 2.2 keeps the core. */
typedef struct {
    sol_u32 handle;    /* stable, monotonic, never reused; not the index */
    sol_u32 parent;    /* parent handle; 0 = root */
    vec3    pos;
    quat    rot;
    vec3    scale;
    Mesh    mesh;      /* shared reference; a zero Mesh (index_count 0) = empty */
} SceneObject;

typedef struct {
    SceneObject *objects;
    sol_u32      count;
    sol_u32      capacity;
    sol_u32      next_handle;   /* monotonic counter; never reused */
} Scene;

void         scene_init(Scene *s);
void         scene_free(Scene *s);
sol_u32      scene_add(Scene *s, sol_u32 parent, Mesh mesh, vec3 pos, quat rot, vec3 scale);
SceneObject *scene_get(Scene *s, sol_u32 handle);  /* NULL if none; valid until next scene_add */

#endif /* SCENE_H */
