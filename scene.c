/* scene.c — the scene object graph. Above the seam: uses the math types and
   Mesh (RHI handles), never GL. */

#include "scene.h"
#include "nid.h"

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
    o->nid = (char *)malloc(NID_LEN + 1);
    if (o->nid) nid_generate(o->nid);   /* identity born here; stable across re-saves */
    o->pos = pos;
    o->rot = rot;
    o->scale = scale;
    o->mesh = mesh;
    o->mesh_ref = NULL;
    o->meta = NULL; o->meta_count = 0; o->meta_cap = 0;
    o->relations = NULL; o->rel_count = 0; o->rel_cap = 0;
    o->content = NULL;
    return o->handle;
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
