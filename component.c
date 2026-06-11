/* component.c — see component.h. Pure CPU; no GL. C89. */

#include "component.h"
#include "sol_math.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    const char *type;
    int         param_count;
    const char *param_names[COMPONENT_MAX_PARAMS];
    float       defaults[COMPONENT_MAX_PARAMS];
    size_t      state_size;     /* runtime scratch per attachment; 0 = none */
    ComponentFn fn;
} ComponentDef;

/* spin: turn about an axis in the object's OWN frame (the overlay rotation
   multiplies on the right of the base), angle = speed * t — absolute time,
   so the motion is deterministic and drift-free */
static void comp_spin(Scene *s, SceneObject *o, const float *p,
                      void *st, float t, float dt) {
    vec3 axis;
    (void)s; (void)st; (void)dt;
    axis = vec3_make(p[0], p[1], p[2]);
    if (vec3_dot(axis, axis) < 1e-8f) return;
    o->overlay_rot = quat_mul(o->overlay_rot,
        quat_from_axis_angle(vec3_normalize(axis), p[3] * t));
}

/* bob: a gentle hover along the parent's vertical */
static void comp_bob(Scene *s, SceneObject *o, const float *p,
                     void *st, float t, float dt) {
    (void)s; (void)st; (void)dt;
    o->overlay_pos.y += p[0] * sinf(p[1] * t);
}

/* flicker: a flame's life on the GLOW channel — three incommensurate sines
   make organic non-repeating wobble; depth scales the swing around 1. The
   FIRST customer of per-instance state: a phase seeded from the handle, so
   two flames in one room never breathe in step. The persisted material and
   light metas never hear about any of this (§1.6 — the glow multiplies in
   at the consumers: the light collector and the emissive draw). */
typedef struct { float phase; } FlickerState;

static void comp_flicker(Scene *s, SceneObject *o, const float *p,
                         void *st, float t, float dt) {
    FlickerState *fs = (FlickerState *)st;
    float n, tt;
    (void)s; (void)dt;
    if (fs == NULL) return;                    /* alloc failed: a steady flame */
    if (fs->phase == 0.0f)                     /* seed once per attachment */
        fs->phase = (float)(o->handle % 97u) * 0.37f + 0.01f;
    tt = t + fs->phase;
    n  = sinf(p[1] * tt)                 * 0.5f
       + sinf(p[1] * 2.7f  * tt + 1.3f) * 0.3f
       + sinf(p[1] * 0.83f * tt + 4.1f) * 0.2f;
    o->overlay_glow *= 1.0f + p[0] * n * 0.5f;
}

/* THE single source of truth for what each component type means — the
   third registry (P3's mesh emitters, P4i4's assets, now behavior). New
   behaviors are one entry + one function. */
static const ComponentDef C_REGISTRY[] = {
    { "spin", 4, { "ax", "ay", "az", "speed" },
                 { 0.0f, 1.0f, 0.0f, 0.8f }, 0, comp_spin },
    { "bob",  2, { "amp", "speed" },
                 { 0.15f, 1.0f }, 0, comp_bob },
    { "flicker", 2, { "depth", "speed" },
                 { 0.30f, 9.0f }, sizeof(FlickerState), comp_flicker }
};
#define C_COUNT ((int)(sizeof C_REGISTRY / sizeof C_REGISTRY[0]))

static const ComponentDef *find_def(const char *type) {
    int i;
    for (i = 0; i < C_COUNT; i++) {
        if (strcmp(C_REGISTRY[i].type, type) == 0) return &C_REGISTRY[i];
    }
    return NULL;
}

int component_schema(const char *type, const char *const **names,
                     const float **defaults) {
    const ComponentDef *d = find_def(type);
    if (d == NULL) return -1;
    if (names)    *names    = d->param_names;
    if (defaults) *defaults = d->defaults;
    return d->param_count;
}

void components_update(Scene *s, float t, float dt) {
    sol_u32 i, c;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        /* reset first, every object, every frame: a removed component's
           motion stops at once; attached ones accumulate from identity */
        o->overlay_pos  = vec3_make(0.0f, 0.0f, 0.0f);
        o->overlay_rot  = quat_identity();
        o->overlay_glow = 1.0f;
        for (c = 0; c < o->comp_count; c++) {
            Component          *cp = &o->components[c];
            const ComponentDef *d  = find_def(cp->type);
            float eff[COMPONENT_MAX_PARAMS];
            int   k;
            if (d == NULL) continue;            /* unknown: the data survives */
            for (k = 0; k < d->param_count; k++)
                eff[k] = (k < cp->param_count) ? cp->params[k] : d->defaults[k];
            if (d->state_size > 0 && cp->state == NULL)
                cp->state = calloc(1, d->state_size);
            d->fn(s, o, eff, cp->state, t, dt);
        }
    }
}
