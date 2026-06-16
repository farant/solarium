/* particles.c — see particles.h. Pure CPU; no GL, no Scene. C89. */

#include "particles.h"

static float lerpf(float a, float b, float u) {
    return a + (b - a) * u;
}

void particles_init(ParticlePool *pp) {
    pp->live = 0;   /* items beyond live are never read — no need to zero 344KB */
}

void particles_spawn(ParticlePool *pp, const Particle *p) {
    int slot;
    if (p->life <= 0.0f) return;            /* born dead: drop it */
    if (pp->live < PARTICLE_CAP) {
        slot = pp->live;
        pp->live += 1;
    } else {
        /* full: recycle the slot closest to death — max age/life, not max
           age, so a near-dead spark retires before a middle-aged mote */
        int   i;
        float worst = -1.0f;
        slot = 0;
        for (i = 0; i < PARTICLE_CAP; i++) {
            float u = pp->items[i].age / pp->items[i].life;
            if (u > worst) { worst = u; slot = i; }
        }
    }
    pp->items[slot] = *p;
}

void particles_update(ParticlePool *pp, float dt, vec3 wind) {
    int i = 0;
    while (i < pp->live) {
        Particle *p = &pp->items[i];
        p->age += dt;
        if (p->age >= p->life) {
            /* swap-with-last; the swapped-in particle takes its turn at i */
            pp->items[i] = pp->items[pp->live - 1];
            pp->live -= 1;
            continue;
        }
        /* the one wind (P7 item 9): a global drift acceleration — pass
           {0,0,0} for the old behavior (the headless test does) */
        p->vel.x += (p->acc.x + wind.x) * dt;
        p->vel.y += (p->acc.y + wind.y) * dt;
        p->vel.z += (p->acc.z + wind.z) * dt;
        p->pos.x += p->vel.x * dt;
        p->pos.y += p->vel.y * dt;
        p->pos.z += p->vel.z * dt;
        i += 1;
    }
}

int particles_fill(const ParticlePool *pp, float *out, int max_count) {
    int n = pp->live < max_count ? pp->live : max_count;
    int i;
    for (i = 0; i < n; i++) {
        const Particle *p = &pp->items[i];
        float *o = out + i * PARTICLE_INST_FLOATS;
        float  u = p->age / p->life;
        float  env_in, env_out;
        if (u < 0.0f) u = 0.0f;
        if (u > 1.0f) u = 1.0f;
        env_in  = u / PARTICLE_FADE_IN;
        env_out = (1.0f - u) / PARTICLE_FADE_OUT;
        if (env_in  > 1.0f) env_in  = 1.0f;
        if (env_out > 1.0f) env_out = 1.0f;
        o[0] = p->pos.x;
        o[1] = p->pos.y;
        o[2] = p->pos.z;
        o[3] = lerpf(p->size0, p->size1, u);
        o[4] = lerpf(p->col0.x, p->col1.x, u);
        o[5] = lerpf(p->col0.y, p->col1.y, u);
        o[6] = lerpf(p->col0.z, p->col1.z, u);
        o[7] = lerpf(p->col0.w, p->col1.w, u) * env_in * env_out;
    }
    return n;
}

float particles_rand01(sol_u32 *rng) {
    *rng = *rng * 1664525u + 1013904223u;
    return (float)((*rng >> 8) & 0xFFFFu) / 65535.0f;
}

vec3 particles_jitter(sol_u32 *rng, vec3 base, vec3 spread) {
    /* one statement per roll: argument evaluation order is unspecified in
       C, and the LCG advances per call — sequence points keep the rolls
       deterministic across compilers */
    vec3 r;
    r.x = base.x + (particles_rand01(rng) * 2.0f - 1.0f) * spread.x;
    r.y = base.y + (particles_rand01(rng) * 2.0f - 1.0f) * spread.y;
    r.z = base.z + (particles_rand01(rng) * 2.0f - 1.0f) * spread.z;
    return r;
}
