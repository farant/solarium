/* scene.c — the scene object graph. Above the seam: uses the math types and
   Mesh (RHI handles), never GL. */

#include "scene.h"

#include <stdlib.h>

void scene_init(Scene *s) {
    s->objects = NULL;
    s->count = 0;
    s->capacity = 0;
    s->next_handle = 1;       /* 0 reserved for "none" / root */
}

void scene_free(Scene *s) {
    free(s->objects);
    s->objects = NULL;
    s->count = 0;
    s->capacity = 0;
}

sol_u32 scene_add(Scene *s, Mesh mesh, vec3 pos, quat rot, vec3 scale) {
    SceneObject *o;
    if (s->count == s->capacity) {
        s->capacity = s->capacity ? s->capacity * 2 : 16;
        s->objects = realloc(s->objects, (size_t)s->capacity * sizeof(SceneObject));
    }
    o = &s->objects[s->count++];
    o->handle = s->next_handle++;   /* monotonic; decoupled from array index */
    o->pos = pos;
    o->rot = rot;
    o->scale = scale;
    o->mesh = mesh;
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
