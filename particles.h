/* particles.h — the particle pool (P4 item 7): runtime-only ephemera.
   Particles are STATISTICS, not objects — no handles, no picking, no
   persistence (the reader-rig doctrine at landscape scale: the file
   records the EMITTER, never a frame of the weather). This module is the
   pure-CPU half: a fixed arena, semi-implicit Euler, and the spread math
   emitters roll at spawn. It never touches the GPU or the Scene — the
   emit component (one layer up) bridges scene -> pool, and the render
   path reads the pool out through particles_fill. Links libc only;
   headless-tested as `build.sh parttest`. */

#ifndef PARTICLES_H
#define PARTICLES_H

#include "sol_base.h"
#include "sol_types.h"

/* The pool's whole budget, fixed at compile time: no allocation ever
   happens after init. 4096 x 84 bytes = ~344KB of arena; the per-frame
   instance upload is 4096 x 32 bytes = 128KB at worst. */
#define PARTICLE_CAP 4096

/* The instance layout particles_fill emits: pos3 + size1 + color4 —
   eight floats, the meadow's exact stride (item 3's compact layout). */
#define PARTICLE_INST_FLOATS 8

/* Every particle fades in and out by an envelope on top of its color
   curve — fractions of life, guaranteed at the fill site, so nothing
   ever pops into or out of existence regardless of emitter settings. */
#define PARTICLE_FADE_IN  0.10f
#define PARTICLE_FADE_OUT 0.25f

/* Fully baked at birth: after spawn a particle NEVER consults its
   emitter again. size and color lerp from 0-endpoints to 1-endpoints
   over the particle's life (sparks shrink and redden; mist grows and
   thins). acc is per-particle so one pool serves falling sparks and
   buoyant motes in the same frame. */
typedef struct {
    vec3  pos, vel, acc;    /* semi-implicit Euler: vel += acc dt; pos += vel dt */
    float age, life;        /* seconds; age >= life is death */
    float size0, size1;     /* world units, lerped over life */
    vec4  col0, col1;       /* RGBA endpoints, lerped over life (HDR: rgb may exceed 1) */
} Particle;

typedef struct {
    Particle items[PARTICLE_CAP];   /* [0, live) are alive; beyond is garbage */
    int      live;
} ParticlePool;

void particles_init(ParticlePool *pp);

/* Copies the caller's fully-baked particle in. When the pool is full the
   slot closest to death (max age/life) is recycled — the room never
   visibly stops emitting, an old mote just retires early. A particle
   with life <= 0 is born dead and silently dropped. */
void particles_spawn(ParticlePool *pp, const Particle *p);

/* One Euler step for every live particle; the dead are removed by
   swap-with-last (order scrambles — additive blending is commutative,
   so draw order is a freedom the renderer already paid for). */
void particles_update(ParticlePool *pp, float dt);

/* Writes up to max_count instances (PARTICLE_INST_FLOATS floats each)
   and returns how many: position, lerped size, lerped color with the
   fade envelope folded into alpha. The renderer's whole view of the
   pool — and headless-testable, because it is only arithmetic. */
int particles_fill(const ParticlePool *pp, float *out, int max_count);

/* Spread math for emitters. The LCG's third minting (the meadow scatter
   and the codex mint each keep a private one in main.c) — exported here
   because emitters AND tests need the same deterministic rolls. */
float particles_rand01(sol_u32 *rng);                      /* [0, 1) */
vec3  particles_jitter(sol_u32 *rng, vec3 base, vec3 spread); /* base +- spread per axis */

#endif /* PARTICLES_H */
