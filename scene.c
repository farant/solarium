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
    for (i = 0; i < o->comp_count; i++) {           /* P4 item 6: type + opaque state */
        free(o->components[i].type);
        free(o->components[i].state);
    }
    free(o->components);
    free(o->content);
    free(o->nid);
    free(o->mesh_ref);
    free(o->tex_ref);
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
    o->tex_ref = NULL;              /* maps-by-reference (the texture side-quest) */
    o->tex_param_count = 0;
    o->meta = NULL; o->meta_count = 0; o->meta_cap = 0;
    o->relations = NULL; o->rel_count = 0; o->rel_cap = 0;
    o->content = NULL;
    o->components = NULL; o->comp_count = 0; o->comp_cap = 0;   /* P4 item 6 */
    o->overlay_pos  = vec3_make(0.0f, 0.0f, 0.0f);  /* overlays start as identity */
    o->overlay_rot  = quat_identity();
    o->overlay_glow = 1.0f;
    o->overlay_clip  = -1;                          /* -1 = no animation override */
    o->overlay_speed = 1.0f;
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

/* The EFFECTIVE local transform (P4 item 6, §1.6): the persisted BASE
   composed with the transient overlay — the ONE place they meet. The
   forward walk, the inverse walk, and the rotation walk all come through
   here, so rendering, picking, dragging, and collision see the same
   animated pose while the file sees only the base. The overlay rotation
   multiplies on the RIGHT: it turns the object about its own placed axes. */
static void effective_trs(const SceneObject *o, vec3 *p, quat *r) {
    p->x = o->pos.x + o->overlay_pos.x;
    p->y = o->pos.y + o->overlay_pos.y;
    p->z = o->pos.z + o->overlay_pos.z;
    *r = quat_mul(o->rot, o->overlay_rot);
}

/* World matrix by walking up the parent chain: parent.world * ... * local.
   Iterative (no recursion), depth-capped against cycles, NULL-guarded against a
   dangling parent. Shared by render and the item-4 picker. */
mat4 scene_world_matrix(Scene *s, const SceneObject *o) {
    mat4    world;
    vec3    ep;
    quat    er;
    sol_u32 p;
    int     depth = 0;
    effective_trs(o, &ep, &er);
    world = mat4_from_trs(ep, er, o->scale);
    p     = o->parent;
    while (p != 0 && depth < 64) {
        SceneObject *par = scene_get(s, p);
        if (!par) break;                                   /* dangling parent -> stop */
        effective_trs(par, &ep, &er);
        world = mat4_mul(mat4_from_trs(ep, er, par->scale), world);
        p     = par->parent;                               /* climb */
        depth++;                                           /* cycle guard */
    }
    return world;
}

/* World point -> the local space of `parent` (0 = the world frame: identity).
   scene_world_matrix's inverse, computed WITHOUT flattening: collect the
   chain, then run each node's single-TRS inverse root-first — the inverse of
   a composition applies the inverse steps in reverse order. Step-by-step
   inverses stay exact even where a flattened R*S would shear (non-uniform
   scale under rotation). Same depth cap + dangling guard as world_matrix.
   This is what writes a dragged child's world position back into a ROTATED
   parent (item 8: cards on a vertical board). */
vec3 scene_world_to_local(Scene *s, sol_u32 parent, vec3 p) {
    sol_u32 chain[64];
    int     n = 0;
    sol_u32 h = parent;
    while (h != 0 && n < 64) {
        SceneObject *o = scene_get(s, h);
        if (!o) break;                                     /* dangling parent -> stop */
        chain[n++] = h;
        h = o->parent;
    }
    while (n > 0) {                                        /* root first, `parent` last */
        SceneObject *o = scene_get(s, chain[--n]);
        if (o) {
            vec3 ep;
            quat er;
            effective_trs(o, &ep, &er);                    /* the inverse sees the
                                                              same pose the forward
                                                              walk renders (§1.6) */
            p = trs_point_to_local(p, ep, er, o->scale);
        }
    }
    return p;
}

/* Chain-composed world ROTATION. Quats compose like the matrices they mirror
   (quat_mul(a,b) = apply b then a), so ancestors multiply on the left as we
   climb. Assumes the no-shear TRS chains we actually build — non-uniform
   scale beneath a rotated child would add shear no quaternion can hold.
   Normalized once at the end against float drift. Used for a board's world
   plane normal and for rotation write-back when reparenting (item 8). */
quat scene_world_rotation(Scene *s, sol_u32 handle) {
    SceneObject *o = scene_get(s, handle);
    quat    r;
    vec3    ep;
    sol_u32 p;
    int     depth = 0;
    if (!o) return quat_identity();
    effective_trs(o, &ep, &r);
    p = o->parent;
    while (p != 0 && depth < 64) {
        SceneObject *par = scene_get(s, p);
        quat pr;
        if (!par) break;
        effective_trs(par, &ep, &pr);
        r = quat_mul(pr, r);
        p = par->parent;
        depth++;
    }
    return quat_normalize(r);
}

/* CPU ray-cast against object AABBs; returns the nearest hit's stable handle
   (0 = none). Empties (no mesh) aren't pickable. TRIANGLE-PRECISE (P4 item
   2) where CPU geometry was retained at upload (mesh_geom_get by vbuffer
   id): the world AABB is only the broad phase, then the ray is taken into
   the object's LOCAL frame — two points through scene_world_to_local, so
   direction needs no new machinery, and the un-normalized local dir keeps
   t in WORLD units, comparable across objects — and tested against the
   actual triangles. A doorway's opening is genuinely empty: the ray passes
   through to whatever stands beyond. The item-5 "can't pick what you're
   inside" rule survives only on the AABB FALLBACK path (no retained
   geometry); with triangles an enclosing shell no longer shadows at t=0,
   because what you hit must be a real surface. `skip` is the app's policy
   hook (pick-transparency) — engine picking knows no palace names. */
/* One object's pick test: broad AABB phase, then real triangles where CPU
   geometry was retained (narrow), AABB fallback (+ the can't-pick-what-
   you're-inside rule) elsewhere. SOL_TRUE iff this object beats best_t;
   *out_t is the improved t. ONE narrow phase, TWO broad phases: the linear
   scene_pick below and the BVH traversal (P4 item 2 piece 3) both end here. */
sol_bool scene_pick_object(Scene *s, SceneObject *o, Ray ray,
                           float best_t, float *out_t) {
    mat4  world;
    Aabb  wbox;
    float t;
    int   inside;
    const CpuGeom *g;
    sol_bool improved = SOL_FALSE;

    if (o->mesh.index_count == 0) return SOL_FALSE;        /* empties aren't pickable */
    world  = scene_world_matrix(s, o);
    wbox   = aabb_transform(world, o->mesh.bounds);        /* local AABB -> world */
    inside = (ray.origin.x >= wbox.min.x && ray.origin.x <= wbox.max.x &&
              ray.origin.y >= wbox.min.y && ray.origin.y <= wbox.max.y &&
              ray.origin.z >= wbox.min.z && ray.origin.z <= wbox.max.z);
    if (!inside) {                                         /* broad phase */
        if (!ray_vs_aabb(ray, wbox, &t)) return SOL_FALSE;
        if (t >= best_t) return SOL_FALSE;                 /* can't beat the champion */
    }
    g = mesh_geom_get(o->mesh.vbuffer.id);
    if (g) {                                               /* narrow phase: real triangles */
        Ray     lr;
        vec3    l1;
        sol_u32 k;
        lr.origin = scene_world_to_local(s, o->handle, ray.origin);
        l1        = scene_world_to_local(s, o->handle,
                        vec3_add(ray.origin, ray.dir));
        lr.dir    = vec3_sub(l1, lr.origin);       /* NOT normalized: t stays world-scaled */
        for (k = 0; k + 2 < g->idx_count; k += 3) {
            const sol_f32 *a = &g->pos[g->idx[k]     * 3];
            const sol_f32 *b = &g->pos[g->idx[k + 1] * 3];
            const sol_f32 *c = &g->pos[g->idx[k + 2] * 3];
            float tt;
            if (ray_vs_triangle(lr,
                    vec3_make(a[0], a[1], a[2]),
                    vec3_make(b[0], b[1], b[2]),
                    vec3_make(c[0], c[1], c[2]), &tt) && tt < best_t) {
                best_t   = tt;
                improved = SOL_TRUE;
            }
        }
    } else if (!inside && t < best_t) {                    /* AABB fallback */
        best_t   = t;
        improved = SOL_TRUE;
    }
    if (improved && out_t) *out_t = best_t;
    return improved;
}

sol_u32 scene_pick(Scene *s, Ray ray, float *out_t,
                   ScenePickSkip skip, void *skip_ctx) {
    sol_u32 i, best = 0;
    float   best_t = 1e30f;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        if (skip && skip(s, o, skip_ctx)) continue;        /* app policy: land, not things */
        if (scene_pick_object(s, o, ray, best_t, &best_t))
            best = o->handle;
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

/* Maps-by-reference (the texture side-quest): the same pair as above —
   a synthesized-material kind plus a knob PREFIX; the app resolves the
   pixels, the file never holds a texture. */
void scene_tex_ref_set(Scene *s, sol_u32 handle, const char *name) {
    SceneObject *o = scene_get(s, handle);
    if (!o) return;
    free(o->tex_ref);
    o->tex_ref = sol_strdup(name);
}

void scene_tex_params_set(Scene *s, sol_u32 handle, const float *params, int count) {
    SceneObject *o = scene_get(s, handle);
    int i;
    if (!o) return;
    if (count < 0) count = 0;
    if (count > TEX_REF_MAX_PARAMS) count = TEX_REF_MAX_PARAMS;
    for (i = 0; i < count; i++) o->tex_params[i] = params[i];
    o->tex_param_count = count;
}

/* Attach a behavior (P4 item 6): the type string is COPIED (the object
   owns it, the meta pattern); params are the file's prefix — the component
   walk merges defaults at update time. State starts empty (lazily
   allocated by the walk, freed with the object). */
void scene_component_add(Scene *s, sol_u32 handle, const char *type,
                         const float *params, int count) {
    SceneObject *o = scene_get(s, handle);
    Component   *c;
    int          i;
    if (!o || !type) return;
    if (count < 0) count = 0;
    if (count > COMPONENT_MAX_PARAMS) count = COMPONENT_MAX_PARAMS;
    if (o->comp_count == o->comp_cap) {
        o->comp_cap   = o->comp_cap ? o->comp_cap * 2 : 2;
        o->components = realloc(o->components,
                                (size_t)o->comp_cap * sizeof(Component));
    }
    c = &o->components[o->comp_count++];
    c->type        = sol_strdup(type);
    c->param_count = count;
    for (i = 0; i < count; i++) c->params[i] = params[i];
    c->state = NULL;
}

void scene_material_set(Scene *s, sol_u32 handle, Material mat) {
    SceneObject *o = scene_get(s, handle);
    if (o) o->material = mat;   /* texture handles inside are shared; scene doesn't own them */
}
