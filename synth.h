/* synth.h — the synthesizer (P4 item 8, amended): sounds are MINTED from
   parameters, never sourced. An sfxr-lineage one-voice SFX engine, pure
   C89 arithmetic, rendered OFFLINE to a mono float buffer — no GL, no
   CoreAudio, no threads, no files. The mixer (piece 2) plays the buffers;
   the WAV writer (wav.c) exports them for ears and debugging.

   ONE SCHEMA, TYPES ARE PRESETS: every sound runs the same engine, so
   unlike the component registry (per-type schemas) there is ONE global
   knob list and a named sound type is a complete default setting of it —
   sfxr's own shape. A sound reference is type + param-prefix overrides,
   the established rule. The schema is INTROSPECTABLE (names enumerable)
   because TODO5's synth book will build its pages from it; that contract
   is honored from birth.

   The "8-bit" feel is parameters, not architecture: crush (amplitude
   quantization) and decim (sample-hold) are knobs on a sound; the engine
   renders float throughout. Naive oscillators alias at high pitch by
   DESIGN — folded harmonics are the crunch we're chasing (polyBLEP is the
   deliberate non-feature). */

#ifndef SYNTH_H
#define SYNTH_H

#include "sol_base.h"

#define SYNTH_RATE   48000          /* render rate = the mixer's rate */
#define SYNTH_PARAMS 20

/* The knobs, in prefix order (schema indices):
    0 wave    0=square 1=saw 2=triangle 3=sine 4=noise
    1 attack  seconds, amplitude ramp 0 -> 1
    2 sustain seconds at full level
    3 punch   0..1 extra gain at sustain start, fading across sustain
    4 decay   seconds, 1 -> 0
    5 freq    Hz (for noise: the sample-hold pitch of the crunch)
    6 slide   octaves/second of frequency glide
    7 dslide  octaves/second^2 (the glide's own drift)
    8 vibd    vibrato depth, semitones
    9 vibs    vibrato speed, Hz
   10 arpm    arpeggio frequency multiplier (1 = none)
   11 arpt    seconds until the arpeggio jump (0 = never)
   12 duty    square duty cycle 0..1
   13 dutys   duty sweep per second
   14 lpcut   one-pole low-pass cutoff Hz (0 = off)
   15 hpcut   one-pole high-pass cutoff Hz (0 = off)
   16 crush   bit depth for amplitude quantization (0 = off)
   17 decim   hold each value N samples (0/1 = off)
   18 gain    linear output gain
   19 jitter  0..1: how much the SEED perturbs freq/decay/duty per render
              (the per-trigger variation no recorded sample can do) */

int                synth_param_count(void);
const char *const *synth_param_names(void);

/* presets — the sound types. NULL for an unknown name. Enumerable so
   synthtest auditions everything and the future app lists patches. */
const float *synth_preset(const char *type);
int          synth_preset_count(void);
const char  *synth_preset_name(int i);

/* THE renderer: a full SYNTH_PARAMS param set (merge prefixes against a
   preset before calling), a seed (drives the noise stream always, and
   perturbs pitch/decay/duty when jitter > 0 — same params + same seed =
   bit-identical buffer), a mono float out buffer. Returns frames written
   (duration = attack + sustain + decay, truncated to max_frames). */
int synth_render(const float *params, sol_u32 seed, float *out, int max_frames);

/* render a SEAMLESS LOOP: synth_render, then crossfade the tail into the
   head and trim it — sample[0] continues sample[len-1] by construction.
   This is how "no loop seams" is achieved OFFLINE (wind, crackle) without
   putting synthesis inside the real-time callback. Returns the loopable
   length (frames - blend_frames). Use loop-shaped params (attack 0,
   decay 0) — fades belong to the mixer's gain, not the loop body. */
int synth_render_loop(const float *params, sol_u32 seed, float *out,
                      int max_frames, int blend_frames);

#endif /* SYNTH_H */
