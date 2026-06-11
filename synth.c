/* synth.c — see synth.h. Pure CPU; no GL, no CoreAudio, no I/O. C89. */

#include "synth.h"

#include <math.h>
#include <string.h>

/* the LCG again (meadow, codex, particles, now sound) */
static float rnd01(sol_u32 *rng) {
    *rng = *rng * 1664525u + 1013904223u;
    return (float)((*rng >> 8) & 0xFFFFu) / 65535.0f;
}

static const char *const PARAM_NAMES[SYNTH_PARAMS] = {
    "wave", "attack", "sustain", "punch", "decay",
    "freq", "slide", "dslide", "vibd", "vibs",
    "arpm", "arpt", "duty", "dutys", "lpcut",
    "hpcut", "crush", "decim", "gain", "jitter"
};

int synth_param_count(void) { return SYNTH_PARAMS; }

const char *const *synth_param_names(void) { return PARAM_NAMES; }

/* The palace's voice, v1 — tuned by ear via synthtest's .wav exports.
   Indices follow the schema comment in synth.h. */
typedef struct {
    const char *name;
    float       p[SYNTH_PARAMS];
} SynthPreset;

static const SynthPreset PRESETS[] = {
    /*            wave  att    sus    punch  decay  freq    slide dslide vibd vibs  arpm  arpt   duty  dutys lpcut   hpcut  crush decim gain  jitter */
    { "blip",   { 0.0f, 0.0f,  0.045f, 0.2f, 0.09f,  880.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.5f, 0.05f, 0.5f, 0.0f, 0.0f,    0.0f,  0.0f, 0.0f, 0.40f, 0.0f } },
    { "step",   { 4.0f, 0.002f,0.018f, 0.0f, 0.08f,  320.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,  0.5f, 0.0f, 1400.0f, 220.0f, 0.0f, 0.0f, 0.33f, 0.5f } },
    { "whoosh", { 4.0f, 0.10f, 0.05f,  0.0f, 0.28f,  180.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,  0.5f, 0.0f, 800.0f,  250.0f, 0.0f, 0.0f, 0.30f, 0.3f } },
    { "thump",  { 3.0f, 0.002f,0.04f,  0.6f, 0.28f,  110.0f,-3.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,  0.5f, 0.0f, 300.0f,  0.0f,   0.0f, 0.0f, 0.60f, 0.25f } },
    { "wind",   { 4.0f, 0.40f, 1.60f,  0.0f, 0.50f,  60.0f,  0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,  0.5f, 0.0f, 450.0f,  0.0f,   0.0f, 0.0f, 0.25f, 0.0f } },
    { "laser",  { 0.0f, 0.0f,  0.04f,  0.3f, 0.18f, 1400.0f,-9.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,  0.18f,0.3f, 0.0f,    0.0f,   0.0f, 0.0f, 0.35f, 0.2f } },
    /* loop-shaped (attack/decay 0): a flame's sputter — slow sample-held
       noise so the crunch is CHUNKS, band-passed to the fire register */
    { "crackle",{ 4.0f, 0.0f,  3.0f,   0.0f, 0.0f,   18.0f,  0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,  0.5f, 0.0f, 2000.0f, 400.0f, 0.0f, 0.0f, 0.50f, 0.0f } }
};
#define PRESET_COUNT ((int)(sizeof PRESETS / sizeof PRESETS[0]))

const float *synth_preset(const char *type) {
    int i;
    for (i = 0; i < PRESET_COUNT; i++) {
        if (strcmp(PRESETS[i].name, type) == 0) return PRESETS[i].p;
    }
    return (const float *)0;
}

int synth_preset_count(void) { return PRESET_COUNT; }

const char *synth_preset_name(int i) {
    if (i < 0 || i >= PRESET_COUNT) return (const char *)0;
    return PRESETS[i].name;
}

static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

int synth_render(const float *p, sol_u32 seed, float *out, int max_frames) {
    /* effective params after the seed's jitter rolls — rolled FIRST and
       unconditionally, so the rng stream (and thus the noise) is the same
       whether jitter is 0 or not */
    sol_u32 rng    = seed ? seed : 1u;
    float   jit    = clampf(p[19], 0.0f, 1.0f);
    float   r_f    = rnd01(&rng), r_d = rnd01(&rng), r_y = rnd01(&rng);
    float   freq0  = p[5]  * (1.0f + jit * 0.16f * (r_f * 2.0f - 1.0f));
    float   decay  = p[4]  * (1.0f + jit * 0.35f * (r_d * 2.0f - 1.0f));
    float   duty   = clampf(p[12] + jit * 0.10f * (r_y * 2.0f - 1.0f), 0.05f, 0.95f);
    float   attack  = p[1] < 0.0f ? 0.0f : p[1];
    float   sustain = p[2] < 0.0f ? 0.0f : p[2];
    int     wave   = (int)p[0];
    int     total, i;
    float   dt     = 1.0f / (float)SYNTH_RATE;

    /* running state */
    float phase = 0.0f;
    float lp = 0.0f, hp_lp = 0.0f;          /* one-pole memories */
    float noise_val = 0.0f;
    int   noise_left = 0;
    float decim_val = 0.0f;
    int   decim_left = 0;

    if (decay < 0.0f) decay = 0.0f;
    total = (int)((attack + sustain + decay) * (float)SYNTH_RATE);
    if (total > max_frames) total = max_frames;

    for (i = 0; i < total; i++) {
        float t = (float)i * dt;
        float env, freq, x;

        /* envelope: ramp, then full (punch fades across the sustain),
           then a straight line down */
        if (t < attack) {
            env = t / attack;
        } else if (t < attack + sustain) {
            float sp = sustain > 0.0f ? (t - attack) / sustain : 1.0f;
            env = 1.0f + p[3] * (1.0f - sp);
        } else {
            float dp = decay > 0.0f ? (t - attack - sustain) / decay : 1.0f;
            env = 1.0f - dp;
        }

        /* frequency: exponential glide (octaves are how PITCH moves),
           vibrato as a slow wobble in semitones, the arpeggio's one jump */
        freq = freq0 * powf(2.0f, p[6] * t + 0.5f * p[7] * t * t);
        if (p[8] != 0.0f)
            freq *= powf(2.0f, p[8] * sinf(6.2831853f * p[9] * t) / 12.0f);
        if (p[11] > 0.0f && t >= p[11]) freq *= p[10];
        freq = clampf(freq, 1.0f, 20000.0f);

        /* the oscillator: a phase accumulator and a shape */
        phase += freq * dt;
        phase -= floorf(phase);
        switch (wave) {
        case 0: {                                   /* square, swept duty */
            float d = clampf(duty + p[13] * t, 0.05f, 0.95f);
            x = phase < d ? 1.0f : -1.0f;
        } break;
        case 1: x = 2.0f * phase - 1.0f; break;     /* saw */
        case 2:                                     /* triangle */
            x = phase < 0.5f ? 4.0f * phase - 1.0f : 3.0f - 4.0f * phase;
            break;
        case 3: x = sinf(6.2831853f * phase); break;
        default: {                                  /* noise: sample-held so
                                                       freq pitches the crunch */
            if (noise_left <= 0) {
                noise_val  = rnd01(&rng) * 2.0f - 1.0f;
                noise_left = (int)((float)SYNTH_RATE / (freq * 8.0f));
                if (noise_left < 1) noise_left = 1;
            }
            noise_left--;
            x = noise_val;
        } break;
        }

        x *= env;

        /* one-pole filters: lp IS "muffled"; hp = the signal minus its
           own low end */
        if (p[14] > 0.0f) {
            float a = 1.0f - expf(-6.2831853f * p[14] * dt);
            lp += a * (x - lp);
            x = lp;
        }
        if (p[15] > 0.0f) {
            float a = 1.0f - expf(-6.2831853f * p[15] * dt);
            hp_lp += a * (x - hp_lp);
            x = x - hp_lp;
        }

        /* the 8-bit knobs: quantize amplitude, hold samples */
        if (p[16] > 0.0f) {
            float levels = powf(2.0f, p[16]);
            x = floorf(x * levels + 0.5f) / levels;
        }
        if (p[17] >= 2.0f) {
            if (decim_left <= 0) {
                decim_val  = x;
                decim_left = (int)p[17];
            }
            decim_left--;
            x = decim_val;
        }

        out[i] = clampf(x * p[18], -1.0f, 1.0f);
    }
    return total;
}

int synth_render_loop(const float *params, sol_u32 seed, float *out,
                      int max_frames, int blend_frames) {
    int n = synth_render(params, seed, out, max_frames);
    int len, i;
    if (blend_frames >= n) return n;             /* too short to blend: as-is */
    len = n - blend_frames;
    for (i = 0; i < blend_frames; i++) {
        float w = (float)i / (float)blend_frames;
        out[i] = out[i] * w + out[len + i] * (1.0f - w);
    }
    return len;
}
