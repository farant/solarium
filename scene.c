/* scene.c — the scene object graph. Above the seam: uses the math types and
   Mesh (RHI handles), never GL. */

#include "scene.h"
#include "nid.h"
#include "sol_math.h"

#include <stdlib.h>
#include <string.h>

/* C89 has no strdup; the object owns copies of every string it's given. */
static char *sol_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char  *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

void scene_init(Scene *s) {
    s->objects = NULL;
    s->count = 0;
    s->capacity = 0;
    s->next_handle = 1;       /* 0 reserved for "none" / root */
}

/* free every string + collection an object owns (symmetric with the setters) */
static void scene_object_free(SceneObject *o) {
    sol_u32 i;
    for (i = 0; i < o->meta_count; i++) { free(o->meta[i].key); free(o->meta[i].value); }
    free(o->meta);
    for (i = 0; i < o->rel_count; i++) { free(o->relations[i].type); }
    free(o->relations);
    free(o->content);
    free(o->nid);
    free(o->mesh_ref);
}

void scene_free(Scene *s) {
    sol_u32 i;
    for (i = 0; i < s->count; i++) scene_object_free(&s->objects[i]);
    free(s->objects);
    s->objects = NULL;
    s->count = 0;
    s->capacity = 0;
}

sol_u32 scene_add(Scene *s, sol_u32 parent, Mesh mesh, vec3 pos, quat rot, vec3 scale) {
    SceneObject *o;
    if (s->count == s->capacity) {
        s->capacity = s->capacity ? s->capacity * 2 : 16;
        s->objects = realloc(s->objects, (size_t)s->capacity * sizeof(SceneObject));
    }
    o = &s->objects[s->count++];
    o->handle = s->next_handle++;   /* monotonic; decoupled from array index */
    o->parent = parent;
    o->kind   = KIND_PLAIN;
    o->nid = (char *)malloc(NID_LEN + 1);
    if (o->nid) nid_generate(o->nid);   /* identity born here; stable across re-saves */
    o->pos = pos;
    o->rot = rot;
    o->scale = scale;
    o->mesh = mesh;
    o->mesh_ref = NULL;
    o->mesh_param_count = 0;        /* 0 = registry defaults; count gates reads */
    o->material = material_default();
    o->meta = NULL; o->meta_count = 0; o->meta_cap = 0;
    o->relations = NULL; o->rel_count = 0; o->rel_cap = 0;
    o->content = NULL;
    return o->handle;
}

/* Remove by handle, preserving array order (shift the tail down). Identity is
   decoupled from index, so survivors keep their handles/nids — their slots just
   move. References TO the removed object are left dangling on purpose: scene_get
   returns NULL for them, which the world-matrix walk and the serializer already
   tolerate (a cascade/reparent policy is a later editor concern). */
void scene_remove(Scene *s, sol_u32 handle) {
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        if (s->objects[i].handle == handle) {
            scene_object_free(&s->objects[i]);
            if (i + 1 < s->count) {
                memmove(&s->objects[i], &s->objects[i + 1],
                        (size_t)(s->count - i - 1) * sizeof(SceneObject));
            }
            s->count--;
            return;
        }
    }
}

/* Linear scan: identity is decoupled from position, so we search by handle
   rather than indexing. Returned pointer is valid until the next scene_add
   (which may realloc the object array). */
SceneObject *scene_get(Scene *s, sol_u32 handle) {
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        if (s->objects[i].handle == handle) return &s->objects[i];
    }
    return NULL;
}

/* The nid -> runtime handle map (linear scan). The loader uses it to translate
   persistent references (parent, rel target) back into runtime handles. */
sol_u32 scene_handle_for_nid(Scene *s, const char *nid) {
    sol_u32 i;
    if (!nid) return 0;
    for (i = 0; i < s->count; i++) {
        if (s->objects[i].nid && strcmp(s->objects[i].nid, nid) == 0) return s->objects[i].handle;
    }
    return 0;
}

/* World matrix by walking up the parent chain: parent.world * ... * local.
   Iterative (no recursion), depth-capped against cycles, NULL-guarded against a
   dangling parent. Shared by render and the item-4 picker. */
mat4 scene_world_matrix(Scene *s, const SceneObject *o) {
    mat4    world = mat4_from_trs(o->pos, o->rot, o->scale);
    sol_u32 p     = o->parent;
    int     depth = 0;
    while (p != 0 && depth < 64) {
        SceneObject *par = scene_get(s, p);
        if (!par) break;                                   /* dangling parent -> stop */
        world = mat4_mul(mat4_from_trs(par->pos, par->rot, par->scale), world);
        p     = par->parent;                               /* climb */
        depth++;                                           /* cycle guard */
    }
    return world;
}

/* CPU ray-cast against object AABBs; returns the nearest hit's stable handle
   (0 = none). Empties (no mesh) aren't pickable. Broad-phase only — AABB is
   exact for boxes; a triangle test would need retained CPU geometry (future). */
sol_u32 scene_pick(Scene *s, Ray ray, float *out_t) {
    sol_u32 i, best = 0;
    float   best_t = 1e30f;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        mat4  world;
        Aabb  wbox;
        float t;
        if (o->mesh.index_count == 0) continue;            /* empties aren't pickable */
        world = scene_world_matrix(s, o);
        wbox  = aabb_transform(world, o->mesh.bounds);     /* local AABB -> world */
        /* you cannot pick what you are INSIDE: a ray starting in the box hits
           at t=0, so a room shell enclosing the camera (item 5) would shadow
           every pick. Skip containers of the ray origin. */
        if (ray.origin.x >= wbox.min.x && ray.origin.x <= wbox.max.x &&
            ray.origin.y >= wbox.min.y && ray.origin.y <= wbox.max.y &&
            ray.origin.z >= wbox.min.z && ray.origin.z <= wbox.max.z) continue;
        if (ray_vs_aabb(ray, wbox, &t) && t < best_t) {    /* keep the nearest */
            best_t = t;
            best   = o->handle;
        }
    }
    if (out_t) *out_t = (best != 0) ? best_t : 0.0f;
    return best;
}

void scene_meta_set(Scene *s, sol_u32 handle, const char *key, const char *value) {
    SceneObject *o = scene_get(s, handle);
    sol_u32 i;
    if (!o) return;
    for (i = 0; i < o->meta_count; i++) {            /* update existing key */
        if (strcmp(o->meta[i].key, key) == 0) {
            free(o->meta[i].value);                  /* free old, own new   */
            o->meta[i].value = sol_strdup(value);
            return;
        }
    }
    if (o->meta_count == o->meta_cap) {              /* else append */
        o->meta_cap = o->meta_cap ? o->meta_cap * 2 : 4;
        o->meta = realloc(o->meta, (size_t)o->meta_cap * sizeof(MetaEntry));
    }
    o->meta[o->meta_count].key   = sol_strdup(key);  /* copy — never store the caller's ptr */
    o->meta[o->meta_count].value = sol_strdup(value);
    o->meta_count++;
}

const char *scene_meta_get(Scene *s, sol_u32 handle, const char *key) {
    SceneObject *o = scene_get(s, handle);
    sol_u32 i;
    if (!o) return NULL;
    for (i = 0; i < o->meta_count; i++) {
        if (strcmp(o->meta[i].key, key) == 0) return o->meta[i].value;
    }
    return NULL;
}

void scene_rel_add(Scene *s, sol_u32 handle, const char *type, sol_u32 target) {
    SceneObject *o = scene_get(s, handle);
    if (!o) return;
    if (o->rel_count == o->rel_cap) {
        o->rel_cap = o->rel_cap ? o->rel_cap * 2 : 4;
        o->relations = realloc(o->relations, (size_t)o->rel_cap * sizeof(Relation));
    }
    o->relations[o->rel_count].type   = sol_strdup(type);
    o->relations[o->rel_count].target = target;
    o->rel_count++;
}

void scene_content_set(Scene *s, sol_u32 handle, const char *path) {
    SceneObject *o = scene_get(s, handle);
    if (!o) return;
    free(o->content);          /* free(NULL) is safe */
    o->content = sol_strdup(path);
}

void scene_mesh_ref_set(Scene *s, sol_u32 handle, const char *name) {
    SceneObject *o = scene_get(s, handle);
    if (!o) return;
    free(o->mesh_ref);
    o->mesh_ref = sol_strdup(name);
}

void scene_kind_set(Scene *s, sol_u32 handle, ObjectKind kind) {
    SceneObject *o = scene_get(s, handle);
    if (o) o->kind = kind;
}

void scene_mesh_params_set(Scene *s, sol_u32 handle, const float *params, int count) {
    SceneObject *o = scene_get(s, handle);
    int i;
    if (!o) return;
    if (count < 0) count = 0;
    if (count > MESH_REF_MAX_PARAMS) count = MESH_REF_MAX_PARAMS;
    for (i = 0; i < count; i++) o->mesh_params[i] = params[i];
    o->mesh_param_count = count;
}

void scene_material_set(Scene *s, sol_u32 handle, Material mat) {
    SceneObject *o = scene_get(s, handle);
    if (o) o->material = mat;   /* texture handles inside are shared; scene doesn't own them */
}
