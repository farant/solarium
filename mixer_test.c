/* mixer_test.c — headless checks for the mixer core (P4 item 8 piece 2):
   the SAME functions the real-time callback calls, asserted on actual
   output samples. Pan law, voice lifecycle, loop wrap, producer-driven
   steal + stale generations, and the final clamp. `build.sh mixtest`. */

#include "mixer.h"

#include <stdio.h>
#include <math.h>

static Mixer m;
static float ramp[100];
static float ones[100];
static float out[1024];

static int feq(float a, float b) {
    return fabsf(a - b) < 1e-5f;
}

static MixCmd start_cmd(int slot, sol_u32 gen, const float *buf, int frames,
                        float gain, float pan, int loop) {
    MixCmd c;
    c.kind = MIX_CMD_START;
    c.slot = slot; c.gen = gen;
    c.buf = buf; c.frames = frames;
    c.gain = gain; c.pan = pan; c.loop = loop;
    return c;
}

int main(void) {
    MixCmd c;
    int    i;
    float  inv_sqrt2 = 0.70710678f;

    for (i = 0; i < 100; i++) { ramp[i] = (float)i / 100.0f; ones[i] = 1.0f; }

    /* silence: no voices, all zeros */
    mixer_init(&m);
    mixer_render(&m, out, 64);
    for (i = 0; i < 128; i++) {
        if (out[i] != 0.0f) { printf("FAIL: silence must be zero\n"); return 1; }
    }

    /* passthrough at center pan: both channels = sample * gain * 1/sqrt2 */
    c = start_cmd(0, 1u, ramp, 100, 1.0f, 0.0f, 0);
    mixer_apply(&m, &c);
    mixer_render(&m, out, 64);
    if (!feq(out[2 * 10], ramp[10] * inv_sqrt2) ||
        !feq(out[2 * 10 + 1], ramp[10] * inv_sqrt2)) {
        printf("FAIL: center pan must be 1/sqrt2 both sides\n"); return 1;
    }
    printf("silence + center passthrough: ok\n");

    /* hard left: all energy left, zero right; constant power across pans */
    {
        float pans[5];
        pans[0] = -1.0f; pans[1] = -0.5f; pans[2] = 0.0f;
        pans[3] = 0.5f;  pans[4] = 1.0f;
        for (i = 0; i < 5; i++) {
            float l, r;
            mixer_init(&m);
            c = start_cmd(0, 1u, ones, 100, 1.0f, pans[i], 0);
            mixer_apply(&m, &c);
            mixer_render(&m, out, 8);
            l = out[0]; r = out[1];
            if (!feq(l * l + r * r, 1.0f)) {
                printf("FAIL: pan %.1f power %.4f != 1\n",
                       (double)pans[i], (double)(l * l + r * r));
                return 1;
            }
        }
        mixer_init(&m);
        c = start_cmd(0, 1u, ones, 100, 1.0f, -1.0f, 0);
        mixer_apply(&m, &c);
        mixer_render(&m, out, 8);
        if (!feq(out[0], 1.0f) || !feq(out[1], 0.0f)) {
            printf("FAIL: hard left must silence the right\n"); return 1;
        }
    }
    printf("constant-power pan law: ok\n");

    /* a one-shot ends exactly at its last frame and frees its slot */
    mixer_init(&m);
    c = start_cmd(0, 1u, ones, 100, 1.0f, 0.0f, 0);
    mixer_apply(&m, &c);
    mixer_render(&m, out, 150);
    if (!feq(out[2 * 99], inv_sqrt2) || out[2 * 100] != 0.0f) {
        printf("FAIL: one-shot must end at frame 100\n"); return 1;
    }
    if (m.v[0].active) { printf("FAIL: ended voice must go inactive\n"); return 1; }

    /* a loop wraps seamlessly: sample 105 == sample 5 */
    mixer_init(&m);
    c = start_cmd(0, 1u, ramp, 100, 1.0f, 0.0f, 1);
    mixer_apply(&m, &c);
    mixer_render(&m, out, 256);
    if (!feq(out[2 * 105], out[2 * 5]) || !m.v[0].active) {
        printf("FAIL: loop must wrap and stay alive\n"); return 1;
    }
    printf("one-shot end + loop wrap: ok\n");

    /* steal: a fresh START on a busy slot restarts it under a new gen;
       commands stamped with the OLD gen are then ignored */
    mixer_init(&m);
    c = start_cmd(0, 1u, ramp, 100, 1.0f, 0.0f, 1);
    mixer_apply(&m, &c);
    mixer_render(&m, out, 32);
    c = start_cmd(0, 2u, ones, 100, 0.5f, 0.0f, 1);   /* the steal */
    mixer_apply(&m, &c);
    c.kind = MIX_CMD_SET; c.gen = 1u; c.gain = 0.9f;  /* stale: must be ignored */
    mixer_apply(&m, &c);
    mixer_render(&m, out, 8);
    if (!feq(out[0], 0.5f * inv_sqrt2)) {
        printf("FAIL: stale-generation SET must be ignored (got %.4f)\n",
               (double)out[0]);
        return 1;
    }
    /* a CURRENT-gen SET does land */
    c.kind = MIX_CMD_SET; c.gen = 2u; c.gain = 0.25f; c.pan = 0.0f;
    mixer_apply(&m, &c);
    mixer_render(&m, out, 8);
    if (!feq(out[0], 0.25f * inv_sqrt2)) {
        printf("FAIL: current-generation SET must apply\n"); return 1;
    }
    /* STOP with the current gen kills it */
    c.kind = MIX_CMD_STOP; c.gen = 2u;
    mixer_apply(&m, &c);
    mixer_render(&m, out, 8);
    if (out[0] != 0.0f) { printf("FAIL: STOP must silence the voice\n"); return 1; }
    printf("steal + generations + stop: ok\n");

    /* two hot voices clamp at full scale, not beyond */
    mixer_init(&m);
    c = start_cmd(0, 1u, ones, 100, 1.0f, 0.0f, 0); mixer_apply(&m, &c);
    c = start_cmd(1, 1u, ones, 100, 1.0f, 0.0f, 0); mixer_apply(&m, &c);
    mixer_render(&m, out, 8);
    if (!feq(out[0], 1.0f)) {
        printf("FAIL: hot mix must clamp to 1 (got %.4f)\n", (double)out[0]);
        return 1;
    }
    printf("hot-mix clamp: ok\n");

    printf("mixer_test: ALL OK\n");
    return 0;
}
