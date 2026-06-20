#ifndef SCENE_H
#define SCENE_H

#include "sol_base.h"
#include "sol_types.h"
#include "mesh.h"
#include "material.h"

#define SOL_WS_NAME_CAP 64   /* max workspace-name length + NUL */

typedef struct { char *key; char *value; }     MetaEntry;
typedef struct { char *type; sol_u32 target; } Relation;

/* A behavior attachment (P4 item 6): a registered type name + the file's
   own param PREFIX (defaults fill the rest at update time, the mesh-params
   rule). `state` is runtime scratch — lazily allocated by the component
   walk, freed with the object, NEVER serialized. */
enum { COMPONENT_MAX_PARAMS = 24 };  /* item 7's emit is the high-water
                                        mark: rate, life, velocity+spread,
                                        position spread, sizes, two RGBA
                                        endpoints, acceleration = 24 */
typedef struct {
    char  *type;
    float  params[COMPONENT_MAX_PARAMS];
    int    param_count;
    void  *state;
} Component;

/* What an object MEANS (P3 item 6) — the first semantic typing. PLAIN =
   props/geometry (everything before item 6). FILE and FOLDER live in mirror
   rooms, governed by reconciliation; an ALIAS is a user-placed REFERENCE to
   a path (never reconciled; staleness is flagged, not removed); a NOTE is
   the user's own text; a TOMBSTONE marks a vanished file whose attachments
   survive. content holds the path for FILE/FOLDER/ALIAS. */
typedef enum {
    KIND_PLAIN = 0,
    KIND_FILE,
    KIND_FOLDER,
    KIND_ALIAS,
    KIND_NOTE,
    KIND_TOMBSTONE,
    KIND_PORTAL          /* a workspace travel gate (Portals & Workspaces) */
} ObjectKind;

/* room for texgen's 10 knobs with slack — scene stays generic about kinds */
#define TEX_REF_MAX_PARAMS 12

/* One object in the scene. Identity is the stable `handle`, NOT the array
   index — that decoupling survives deletion/reload. */
typedef struct {
    sol_u32 handle;    /* runtime identity: stable, monotonic, never reused; not the index */
    sol_u32 parent;    /* parent handle; 0 = root */
    char   *nid;       /* persistent identity (ULID-style); minted in scene_add */
    ObjectKind kind;   /* semantic type (item 6); KIND_PLAIN for props */
    vec3    pos;
    quat    rot;
    vec3    scale;
    Mesh    mesh;      /* shared reference; a zero Mesh (index_count 0) = empty */
    char   *mesh_ref;  /* asset name for geometry-by-reference; NULL = none/empty */
    float   mesh_params[MESH_REF_MAX_PARAMS];  /* the ref's parameters (item 5: room w/d/h...) */
    int     mesh_param_count;                  /* 0 = use the registry defaults */
    Material material; /* PBR material (item 8); texture handles shared, not owned */
    char   *tex_ref;   /* synthesized-material kind ("stone"...); NULL = none.
                          Maps-by-reference, the mesh_ref pattern: the file
                          records (kind, knob prefix), the app resolves the
                          pixels — texture handles stay runtime-only. */
    float   tex_params[TEX_REF_MAX_PARAMS];    /* knob PREFIX over the kind's defaults */
    int     tex_param_count;

    /* overbuilt slots — mostly empty this phase, serialized in 2.5 */
    MetaEntry *meta;       sol_u32 meta_count;  sol_u32 meta_cap;   /* string->string */
    Relation  *relations;  sol_u32 rel_count;   sol_u32 rel_cap;    /* typed edges     */
    char      *content;    /* attached doc/note path; NULL = none */

    /* behavior attachments (P4 item 6) */
    Component *components; sol_u32 comp_count;  sol_u32 comp_cap;

    /* TRANSIENT overlays (TODO4 §1.6): reset and rewritten by the component
       walk every frame, composed into the world walks below — NEVER
       serialized. The file records the BASE the user placed, not a frame
       of the dance. overlay_glow multiplies light intensity + emissive at
       their points of use. overlay_clip/speed are a behavior's CURRENT
       animation choice (the wander brain's gait): -1 = unset, the skinned
       renderer falls through to the persisted animate component. */
    vec3  overlay_pos;
    quat  overlay_rot;
    float overlay_glow;
    int   overlay_clip;
    float overlay_speed;
} SceneObject;

typedef struct {
    SceneObject *objects;
    sol_u32      count;
    sol_u32      capacity;
    sol_u32      next_handle;   /* monotonic counter; never reused */
    char     active_ws[SOL_WS_NAME_CAP];  /* runtime view filter: the workspace
                          currently shown; "" = unfiltered (show all). NEVER
                          serialized — reset on load, set by the app. */
} Scene;

void         scene_init(Scene *s);
void         scene_free(Scene *s);
sol_u32      scene_add(Scene *s, sol_u32 parent, Mesh mesh, vec3 pos, quat rot, vec3 scale);
void         scene_remove(Scene *s, sol_u32 handle);  /* no-op if absent; invalidates SceneObject* */
SceneObject *scene_get(Scene *s, sol_u32 handle);  /* NULL if none; valid until next scene_add */
sol_u32      scene_handle_for_nid(Scene *s, const char *nid);  /* 0 if none — the nid->handle map */
mat4         scene_world_matrix(Scene *s, const SceneObject *o);  /* parent-chain * local */
vec3         scene_world_to_local(Scene *s, sol_u32 parent, vec3 p);  /* its inverse on a point; parent 0 = world */
quat         scene_world_rotation(Scene *s, sol_u32 handle);  /* chain-composed rotation (no-shear TRS assumed) */
/* App policy hook for picking: return SOL_TRUE to make an object pick-
   transparent (the palace declares room shells and terrain LAND this way —
   the engine never learns those names). NULL = nothing skipped. */
typedef sol_bool (*ScenePickSkip)(const Scene *s, const SceneObject *o, void *ctx);
sol_u32      scene_pick(Scene *s, Ray ray, float *out_t,
                        ScenePickSkip skip, void *skip_ctx);  /* nearest hit's handle; 0 = none.
                        TRIANGLE-precise where CPU geometry is retained
                        (mesh_geom_get), AABB fallback elsewhere (P4 item 2) */
sol_bool     scene_pick_object(Scene *s, SceneObject *o, Ray ray,
                               float best_t, float *out_t);  /* one object's test:
                        SOL_TRUE iff it beats best_t. The shared narrow phase —
                        linear scene_pick and the BVH walk are both clients. */

/* slot operations — handle-based (resolve internally, immune to stale pointers) */
void        scene_meta_set(Scene *s, sol_u32 handle, const char *key, const char *value);
const char *scene_meta_get(Scene *s, sol_u32 handle, const char *key);   /* NULL if none */
void        scene_rel_add(Scene *s, sol_u32 handle, const char *type, sol_u32 target);
void        scene_content_set(Scene *s, sol_u32 handle, const char *path);
void        scene_mesh_ref_set(Scene *s, sol_u32 handle, const char *name);
void        scene_mesh_params_set(Scene *s, sol_u32 handle, const float *params, int count);
void        scene_tex_ref_set(Scene *s, sol_u32 handle, const char *name);
void        scene_tex_params_set(Scene *s, sol_u32 handle, const float *params, int count);
void        scene_component_add(Scene *s, sol_u32 handle, const char *type,
                                const float *params, int count);  /* P4 item 6 */
void        scene_kind_set(Scene *s, sol_u32 handle, ObjectKind kind);
void        scene_material_set(Scene *s, sol_u32 handle, Material mat);   /* texture handles shared, not owned */

/* serialization — defined in scene_io.c */
sol_bool    scene_save(Scene *s, const char *path);   /* SOL_FALSE if the file won't open */
sol_bool    scene_load(Scene *s, const char *path);   /* inits s; SOL_FALSE on open/parse error */

#endif /* SCENE_H */
