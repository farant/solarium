/* particles_test.c — headless checks for the particle pool (P4 item 7
   piece 1): the fixed arena (spawn/kill/recycle), semi-implicit Euler,
   swap-with-last conservation, the fade envelope and lerps at the fill
   site, and the deterministic spread math. No GL, no Scene — the pool is
   only arithmetic, which is exactly why it tests headless.
   `build.sh parttest`. */

#include "particles.h"

#include <stdio.h>
#include <math.h>

static ParticlePool pool;   /* ~344KB — static, not stack */

static int feq(float a, float b) {
    return fabsf(a - b) < 1e-5f;
}

static Particle base_particle(void) {
    Particle p;
    p.pos = (vec3){ 0.0f, 0.0f, 0.0f };
    p.vel = (vec3){ 0.0f, 0.0f, 0.0f };
    p.acc = (vec3){ 0.0f, 0.0f, 0.0f };
    p.age = 0.0f;
    p.life = 10.0f;
    p.size0 = 1.0f;
    p.size1 = 1.0f;
    p.col0 = (vec4){ 1.0f, 1.0f, 1.0f, 1.0f };
    p.col1 = (vec4){ 1.0f, 1.0f, 1.0f, 1.0f };
    return p;
}

int main(void) {
    Particle p;
    float    inst[8 * PARTICLE_INST_FLOATS];
    int      i, n;
    vec3     zero_wind = { 0.0f, 0.0f, 0.0f };   /* item 9: drift off here */

    /* empty pool: nothing lives, fill writes nothing */
    particles_init(&pool);
    if (pool.live != 0 || particles_fill(&pool, inst, 8) != 0) {
        printf("FAIL: init must leave an empty pool\n"); return 1;
    }

    /* spawn copies the particle in; born-dead (life <= 0) is dropped */
    p = base_particle();
    p.pos = (vec3){ 4.0f, 5.0f, 6.0f };
    particles_spawn(&pool, &p);
    if (pool.live != 1 || !feq(pool.items[0].pos.y, 5.0f)) {
        printf("FAIL: spawn must copy the particle in\n"); return 1;
    }
    p.life = 0.0f;
    particles_spawn(&pool, &p);
    p.life = -2.0f;
    particles_spawn(&pool, &p);
    if (pool.live != 1) {
        printf("FAIL: life <= 0 must be dropped at spawn\n"); return 1;
    }
    printf("spawn/drop: ok\n");

    /* semi-implicit Euler, exact: vel feels acc first, pos uses NEW vel */
    particles_init(&pool);
    p = base_particle();
    p.vel = (vec3){ 1.0f, 2.0f, 3.0f };
    p.acc = (vec3){ 0.0f, -1.0f, 0.0f };
    particles_spawn(&pool, &p);
    particles_update(&pool, 0.5f, zero_wind);
    if (!feq(pool.items[0].vel.y, 1.5f) ||
        !feq(pool.items[0].pos.x, 0.5f) ||
        !feq(pool.items[0].pos.y, 0.75f) ||
        !feq(pool.items[0].pos.z, 1.5f) ||
        !feq(pool.items[0].age, 0.5f)) {
        printf("FAIL: Euler step must be vel += acc dt, then pos += vel dt\n");
        return 1;
    }
    printf("euler: ok\n");

    /* death at age >= life, removal by swap-with-last; survivors intact.
       A (short life) dies; B and C remain in some order. */
    particles_init(&pool);
    p = base_particle(); p.life = 0.5f;  p.size0 = 1.0f; particles_spawn(&pool, &p);
    p = base_particle(); p.life = 10.0f; p.size0 = 2.0f; particles_spawn(&pool, &p);
    p = base_particle(); p.life = 10.0f; p.size0 = 3.0f; particles_spawn(&pool, &p);
    particles_update(&pool, 1.0f, zero_wind);
    if (pool.live != 2) {
        printf("FAIL: the short-lived particle must die\n"); return 1;
    }
    {
        int saw2 = 0, saw3 = 0;
        for (i = 0; i < pool.live; i++) {
            if (feq(pool.items[i].size0, 2.0f)) saw2 = 1;
            if (feq(pool.items[i].size0, 3.0f)) saw3 = 1;
            if (!feq(pool.items[i].age, 1.0f)) {
                printf("FAIL: survivors must still age the frame they swap\n");
                return 1;
            }
        }
        if (!saw2 || !saw3) {
            printf("FAIL: swap-remove must preserve the survivors\n"); return 1;
        }
    }
    /* exact boundary: age == life is death, not a last frame at u = 1 */
    particles_init(&pool);
    p = base_particle(); p.life = 1.0f;
    particles_spawn(&pool, &p);
    particles_update(&pool, 1.0f, zero_wind);
    if (pool.live != 0) {
        printf("FAIL: age == life must die\n"); return 1;
    }
    printf("death/swap-remove: ok\n");

    /* the fill site: lerped size/color, fade envelope folded into alpha.
       spawn copies age too, so pre-aged particles probe the curve. */
    particles_init(&pool);
    p = base_particle();
    p.life = 1.0f;
    p.size0 = 2.0f; p.size1 = 4.0f;
    p.col0 = (vec4){ 1.0f, 0.0f, 0.0f, 1.0f };
    p.col1 = (vec4){ 0.0f, 0.0f, 1.0f, 1.0f };
    p.age = 0.0f; particles_spawn(&pool, &p);   /* newborn: faded in to 0 */
    p.age = 0.5f; particles_spawn(&pool, &p);   /* mid-life: full alpha */
    p.age = 0.9f; particles_spawn(&pool, &p);   /* dying: fading out */
    n = particles_fill(&pool, inst, 8);
    if (n != 3) { printf("FAIL: fill must report 3\n"); return 1; }
    if (!feq(inst[7], 0.0f)) {
        printf("FAIL: a newborn must start at alpha 0 (fade-in)\n"); return 1;
    }
    if (!feq(inst[8 + 3], 3.0f) ||                       /* size at u=0.5 */
        !feq(inst[8 + 4], 0.5f) || !feq(inst[8 + 6], 0.5f) ||  /* color lerp */
        !feq(inst[8 + 7], 1.0f)) {                       /* full alpha mid-life */
        printf("FAIL: mid-life lerp/envelope wrong\n"); return 1;
    }
    if (!feq(inst[16 + 7], 0.4f)) {                      /* (1-0.9)/0.25 */
        printf("FAIL: fade-out envelope wrong at u=0.9\n"); return 1;
    }
    printf("fill lerps + fade envelope: ok\n");

    /* fill clamps to max_count and writes not a float more */
    particles_init(&pool);
    p = base_particle();
    for (i = 0; i < 5; i++) particles_spawn(&pool, &p);
    inst[3 * PARTICLE_INST_FLOATS] = 12345.0f;           /* canary */
    n = particles_fill(&pool, inst, 3);
    if (n != 3 || !feq(inst[3 * PARTICLE_INST_FLOATS], 12345.0f)) {
        printf("FAIL: fill must clamp to max_count\n"); return 1;
    }
    printf("fill clamp: ok\n");

    /* cap recycling: a full pool retires the slot CLOSEST TO DEATH
       (max age/life), never the newborn, and live never exceeds the cap */
    particles_init(&pool);
    p = base_particle();
    p.life = 10.0f;
    for (i = 0; i < PARTICLE_CAP - 1; i++) particles_spawn(&pool, &p);
    particles_update(&pool, 1.0f, zero_wind);                /* the old guard: u = 0.1 */
    p.size0 = 777.0f;                             /* the newborn: u = 0 */
    particles_spawn(&pool, &p);
    if (pool.live != PARTICLE_CAP) {
        printf("FAIL: pool should be exactly full\n"); return 1;
    }
    p.size0 = 888.0f;                             /* the overflow spawn */
    particles_spawn(&pool, &p);
    if (pool.live != PARTICLE_CAP) {
        printf("FAIL: overflow must not grow the pool\n"); return 1;
    }
    {
        int saw_newborn = 0, saw_overflow = 0;
        for (i = 0; i < pool.live; i++) {
            if (feq(pool.items[i].size0, 777.0f)) saw_newborn += 1;
            if (feq(pool.items[i].size0, 888.0f)) saw_overflow += 1;
        }
        if (saw_overflow != 1 || saw_newborn != 1) {
            printf("FAIL: recycle must take an old slot, not the newborn\n");
            return 1;
        }
    }
    printf("cap recycle (live=%d): ok\n", pool.live);

    /* the spread math: deterministic, in range, jitter stays in its box */
    {
        sol_u32 a = 7u, b = 7u;
        for (i = 0; i < 1000; i++) {
            float ra = particles_rand01(&a);
            float rb = particles_rand01(&b);
            if (ra != rb) {
                printf("FAIL: same seed must roll the same sequence\n");
                return 1;
            }
            if (ra < 0.0f || ra >= 1.0f + 1e-6f) {
                printf("FAIL: rand01 out of range: %f\n", ra); return 1;
            }
        }
    }
    {
        sol_u32 rng = 42u;
        vec3 base   = { 1.0f, 2.0f, 3.0f };
        vec3 spread = { 0.5f, 0.0f, 2.0f };
        for (i = 0; i < 200; i++) {
            vec3 v = particles_jitter(&rng, base, spread);
            if (v.x < 0.5f - 1e-5f || v.x > 1.5f + 1e-5f ||
                !feq(v.y, 2.0f) ||
                v.z < 1.0f - 1e-5f || v.z > 5.0f + 1e-5f) {
                printf("FAIL: jitter outside base +- spread\n"); return 1;
            }
        }
    }
    printf("spread math: ok\n");

    printf("particles_test: ALL OK\n");
    return 0;
}
