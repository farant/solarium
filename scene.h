#ifndef SCENE_H
#define SCENE_H

#include "sol_base.h"
#include "sol_types.h"
#include "mesh.h"
#include "material.h"

typedef struct { char *key; char *value; }     MetaEntry;
typedef struct { char *type; sol_u32 target; } Relation;

/* One object in the scene. Identity is the stable `handle`, NOT the array
   index — that decoupling survives deletion/reload. */
typedef struct {
    sol_u32 handle;    /* runtime identity: stable, monotonic, never reused; not the index */
    sol_u32 parent;    /* parent handle; 0 = root */
    char   *nid;       /* persistent identity (ULID-style); minted in scene_add */
    vec3    pos;
    quat    rot;
    vec3    scale;
    Mesh    mesh;      /* shared reference; a zero Mesh (index_count 0) = empty */
    char   *mesh_ref;  /* asset name for geometry-by-reference; NULL = none/empty */
    Material material; /* PBR material (item 8); texture handles shared, not owned */

    /* overbuilt slots — mostly empty this phase, serialized in 2.5 */
    MetaEntry *meta;       sol_u32 meta_count;  sol_u32 meta_cap;   /* string->string */
    Relation  *relations;  sol_u32 rel_count;   sol_u32 rel_cap;    /* typed edges     */
    char      *content;    /* attached doc/note path; NULL = none */
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
void         scene_remove(Scene *s, sol_u32 handle);  /* no-op if absent; invalidates SceneObject* */
SceneObject *scene_get(Scene *s, sol_u32 handle);  /* NULL if none; valid until next scene_add */
sol_u32      scene_handle_for_nid(Scene *s, const char *nid);  /* 0 if none — the nid->handle map */
mat4         scene_world_matrix(Scene *s, const SceneObject *o);  /* parent-chain * local */
sol_u32      scene_pick(Scene *s, Ray ray, float *out_t);  /* nearest AABB hit's handle; 0 = none */

/* slot operations — handle-based (resolve internally, immune to stale pointers) */
void        scene_meta_set(Scene *s, sol_u32 handle, const char *key, const char *value);
const char *scene_meta_get(Scene *s, sol_u32 handle, const char *key);   /* NULL if none */
void        scene_rel_add(Scene *s, sol_u32 handle, const char *type, sol_u32 target);
void        scene_content_set(Scene *s, sol_u32 handle, const char *path);
void        scene_mesh_ref_set(Scene *s, sol_u32 handle, const char *name);
void        scene_material_set(Scene *s, sol_u32 handle, Material mat);   /* texture handles shared, not owned */

/* serialization — defined in scene_io.c */
sol_bool    scene_save(Scene *s, const char *path);   /* SOL_FALSE if the file won't open */
sol_bool    scene_load(Scene *s, const char *path);   /* inits s; SOL_FALSE on open/parse error */

#endif /* SCENE_H */
