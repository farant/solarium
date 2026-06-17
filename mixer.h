/* mixer.h — the mixer core (P4 item 8 piece 2): pure C89, NO THREADS
   ANYWHERE IN THIS TU. The audio callback (platform_audio.c, the
   quarantine) drains a command ring and calls mixer_render; mixtest calls
   the very same functions headless and asserts on output samples — the
   real-time path and the test path are one code path.

   Mixing IS addition (the same reason additive blending worked for
   particles): per output sample, sum every active voice x gain x pan,
   advance cursors, clamp. Pan is CONSTANT-POWER (cos/sin on a quarter
   circle, L^2+R^2 = 1 always) because loudness tracks power, not
   amplitude — naive linear pan dips at center.

   VOICE OWNERSHIP IS PRODUCER-DRIVEN: the main thread chooses slots
   (round-robin steal for one-shots, reserved slots for loops) and stamps
   each START with a generation; STOP/SET carrying a stale generation are
   ignored. The consumer never allocates — it only obeys. Buffer lifetime
   contract: the producer keeps a buffer alive while any voice may read
   it (synth buffers are session-lived in practice). */

#ifndef MIXER_H
#define MIXER_H

#include "sol_base.h"
#include "reverb.h"

#define MIX_RATE   48000
#define MIX_VOICES 24

/* MIX_CMD_REVERB carries no voice — it sets the one global reverb (P8 item 8)
   from the listener's room, via the rv_* fields below. */
enum { MIX_CMD_START, MIX_CMD_STOP, MIX_CMD_SET, MIX_CMD_REVERB };

/* one fixed-size command — the ring's whole vocabulary */
typedef struct {
    int          kind;
    int          slot;       /* 0..MIX_VOICES-1, producer-chosen */
    sol_u32      gen;        /* producer-stamped; stale commands are ignored */
    const float *buf;        /* mono samples (START only) */
    int          frames;
    float        gain;
    float        pan;        /* -1 left .. +1 right */
    int          loop;
    float        rv_decay;   /* MIX_CMD_REVERB: decay/damp/wet, each [0,1] (P8 item 8) */
    float        rv_damp;
    float        rv_wet;
} MixCmd;

typedef struct {
    const float *buf;
    int          frames, cursor;
    float        gain, pan;
    sol_u32      gen;
    int          loop, active;
} MixVoice;

typedef struct {
    MixVoice v[MIX_VOICES];
    Reverb   rev;            /* the one global reverb (P8 item 8) */
} Mixer;

void mixer_init(Mixer *m);
void mixer_apply(Mixer *m, const MixCmd *c);

/* interleaved stereo float out: ZEROES the buffer, accumulates every
   active voice, clamps to [-1, 1]. Looping voices wrap; ended one-shots
   go inactive (their slot is reusable; a fresh START re-arms it). */
void mixer_render(Mixer *m, float *out_lr, int frames);

#endif /* MIXER_H */
