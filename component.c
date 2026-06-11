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

/* emit: the component that spawns instead of moving (item 7) — its update
   writes NOTHING on its object; it deposits newborns into the injected
   pool, at the object's WORLD position (effective pose: a bobbing lantern
   emits from its bobbed position). This is the dt-integrating component
   the ComponentFn signature reserved a clock for: weather, not
   choreography — nothing persists, so determinism buys nothing here.
   Params are baked into each particle at birth; after that the particle
   never consults its emitter again. */
typedef struct {
    float   acc;    /* fractional spawns carried between frames */
    sol_u32 rng;    /* per-attachment stream; 0 = not yet seeded */
} EmitState;

static ParticlePool *g_emit_pool = (ParticlePool *)0;

void component_set_particle_pool(ParticlePool *pool) {
    g_emit_pool = pool;
}

/* schema order = prefix order (most-customized first): 0 rate, 1 life,
   2-4 velocity, 5-7 velocity spread, 8-10 position spread (a world-axis
   box: dust wants a VOLUME, not a fountain), 11-12 size endpoints,
   13-16 / 17-20 RGBA endpoints (rgb may exceed 1 — bloom's food),
   21-23 acceleration. The DEFAULTS ARE DUST: a bare
   <component type="emit"/> fills the air with drifting motes. */
static void comp_emit(Scene *s, SceneObject *o, const float *p,
                      void *st, float t, float dt) {
    EmitState *es = (EmitState *)st;
    mat4       w;
    vec3       origin;
    int        n, i;
    (void)t;
    if (es == NULL || g_emit_pool == NULL) return;
    if (es->rng == 0u)                       /* seed once, from identity */
        es->rng = (sol_u32)o->handle * 2654435761u + 1u;
    es->acc += p[0] * dt;
    if (es->acc > 64.0f) es->acc = 64.0f;    /* a stall never bursts */
    n = (int)es->acc;
    es->acc -= (float)n;
    if (n <= 0) return;
    w = scene_world_matrix(s, o);
    origin = vec3_make(w.m[12], w.m[13], w.m[14]);
    for (i = 0; i < n; i++) {
        Particle q;
        float    sj;
        q.pos  = particles_jitter(&es->rng, origin,
                                  vec3_make(p[8], p[9], p[10]));
        q.vel  = particles_jitter(&es->rng, vec3_make(p[2], p[3], p[4]),
                                  vec3_make(p[5], p[6], p[7]));
        q.acc  = vec3_make(p[21], p[22], p[23]);
        q.age  = 0.0f;
        q.life = p[1] * (0.75f + 0.5f * particles_rand01(&es->rng));
        sj     = 0.75f + 0.5f * particles_rand01(&es->rng);
        q.size0 = p[11] * sj;                /* one jitter scales BOTH ends:
                                                a small mote stays small */
        q.size1 = p[12] * sj;
        q.col0.x = p[13]; q.col0.y = p[14]; q.col0.z = p[15]; q.col0.w = p[16];
        q.col1.x = p[17]; q.col1.y = p[18]; q.col1.z = p[19]; q.col1.w = p[20];
        particles_spawn(g_emit_pool, &q);
    }
}

/* animate (item 9): which clip a skinned model plays, and how fast. The
   update fn is a NO-OP — the RENDERER is this component's consumer (the
   pose is computed at draw time from absolute t x speed, view state per
   §1.6; nothing here could usefully run earlier). The component exists so
   the choice is DATA: round-trips in the file, editable in a text editor,
   removable (absent = the rest pose — the file IS the behavior, item 6's
   acceptance). Clip is an INDEX in v1: params are floats; names wait for
   string params. */
static void comp_animate(Scene *s, SceneObject *o, const float *p,
                         void *st, float t, float dt) {
    (void)s; (void)o; (void)p; (void)st; (void)t; (void)dt;
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
                 { 0.30f, 9.0f }, sizeof(FlickerState), comp_flicker },
    { "emit", 24, { "rate", "life", "vx", "vy", "vz", "sx", "sy", "sz",
                    "px", "py", "pz", "size0", "size1",
                    "r0", "g0", "b0", "a0", "r1", "g1", "b1", "a1",
                    "ax", "ay", "az" },
                  { 40.0f, 5.0f,  0.0f, 0.03f, 0.0f,  0.04f, 0.02f, 0.04f,
                    1.0f, 0.8f, 1.0f,  0.015f, 0.015f,
                    1.0f, 0.97f, 0.90f, 0.35f,  1.0f, 0.97f, 0.90f, 0.10f,
                    0.0f, 0.0f, 0.0f }, sizeof(EmitState), comp_emit },
    { "animate", 2, { "clip", "speed" },
                 { 0.0f, 1.0f }, 0, comp_animate }
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
