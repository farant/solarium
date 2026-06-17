/* reverb.h — Schroeder/Freeverb reverb (P8 item 8): pure C89, NO threads,
   NO allocation, NO I/O. The mixer owns one Reverb and runs reverb_process
   on the summed stereo bus each callback; the LISTENER's room KIND drives
   the preset (a church rings long + bright, a small room is dry) through
   reverb_set, itself fed by a MIX_CMD_REVERB command off the SAME SPSC ring
   as every other parameter — so the audio thread is the only place the
   reverb state is touched, and no new threading enters the engine.

   THE TOPOLOGY: Freeverb's classic 8 parallel comb filters (each a delay
   line with a damped feedback loop) summed into 4 series allpass filters.
   v1 is a MONO bank — the wet tail is centered/ambient while the dry signal
   keeps its spatial pan; stereo width is the flagged refinement.

   DETERMINISTIC: no randomness, so the same params + same input produce a
   bit-identical buffer — mixtest feeds an impulse and asserts the response
   decays. Kinds-are-presets, the synth lesson applied to acoustics. */

#ifndef REVERB_H
#define REVERB_H

#define REVERB_COMBS    8
#define REVERB_ALLPASS  4
#define REVERB_COMB_MAX 1800     /* longest comb delay (48k-scaled) + headroom */
#define REVERB_AP_MAX   650      /* longest allpass delay */

typedef struct {
    float buf[REVERB_COMB_MAX];
    int   size, idx;
    float feedback, store, damp1, damp2;   /* store = the one-pole lowpass in the loop */
} RevComb;

typedef struct {
    float buf[REVERB_AP_MAX];
    int   size, idx;                         /* feedback is the fixed Freeverb 0.5 */
} RevAllpass;

typedef struct {
    RevComb    comb[REVERB_COMBS];
    RevAllpass ap[REVERB_ALLPASS];
    float      wet;     /* the added wet level (0 = dry/inaudible) */
    float      gain;    /* input gain into the network (Freeverb's fixedgain) */
} Reverb;

void reverb_init(Reverb *r);

/* decay/damp/wet each in [0,1]: decay -> comb feedback (a church ~1 rings
   long), damp -> high-frequency loss in the tail (darker as it rises),
   wet -> how much of the reverberant signal is added back. */
void reverb_set(Reverb *r, float decay, float damp, float wet);

/* Process the interleaved stereo bus IN PLACE: add the (mono) wet tail of
   the summed signal back into both channels. The dry signal is already in
   out_lr; this only ADDS, like every other voice. */
void reverb_process(Reverb *r, float *out_lr, int frames);

#endif /* REVERB_H */
