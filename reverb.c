/* reverb.c — see reverb.h. Pure C89; no threads, no I/O, no allocation.
   A literal Freeverb: 8 comb filters in parallel -> 4 allpass in series.
   The comb/allpass delay lengths are Freeverb's 44.1k tunings scaled to our
   48k rate (x 48000/44100); the staggered, mutually-prime lengths are what
   make the tail dense instead of a flutter. */

#include "reverb.h"

/* Freeverb's tunings, pre-scaled to 48000 Hz. The originals (44100 Hz) were
   {1116,1188,1277,1356,1422,1491,1557,1617} and {556,441,341,225}. */
static const int COMB_LEN[REVERB_COMBS] =
    { 1214, 1293, 1389, 1475, 1547, 1622, 1694, 1760 };
static const int AP_LEN[REVERB_ALLPASS] =
    { 605, 480, 371, 244 };

void reverb_init(Reverb *r) {
    int i, j;
    for (i = 0; i < REVERB_COMBS; i++) {
        r->comb[i].size     = COMB_LEN[i];
        r->comb[i].idx      = 0;
        r->comb[i].store    = 0.0f;
        r->comb[i].feedback = 0.0f;
        r->comb[i].damp1    = 0.0f;
        r->comb[i].damp2    = 1.0f;
        for (j = 0; j < REVERB_COMB_MAX; j++) r->comb[i].buf[j] = 0.0f;
    }
    for (i = 0; i < REVERB_ALLPASS; i++) {
        r->ap[i].size = AP_LEN[i];
        r->ap[i].idx  = 0;
        for (j = 0; j < REVERB_AP_MAX; j++) r->ap[i].buf[j] = 0.0f;
    }
    r->wet  = 0.0f;
    r->gain = 0.015f;            /* Freeverb's fixedgain — keeps the loop stable */
}

void reverb_set(Reverb *r, float decay, float damp, float wet) {
    int   i;
    float fb;
    if (decay < 0.0f) decay = 0.0f;
    if (decay > 1.0f) decay = 1.0f;
    if (damp  < 0.0f) damp  = 0.0f;
    if (damp  > 1.0f) damp  = 1.0f;
    if (wet   < 0.0f) wet   = 0.0f;
    if (wet   > 1.0f) wet   = 1.0f;
    fb = 0.70f + 0.28f * decay;   /* 0.70 (short) .. 0.98 (a cathedral's long ring) */
    for (i = 0; i < REVERB_COMBS; i++) {
        r->comb[i].feedback = fb;
        r->comb[i].damp1    = damp;          /* fraction of the old store retained */
        r->comb[i].damp2    = 1.0f - damp;   /* fraction of the fresh sample passed */
    }
    r->wet = wet;
}

void reverb_process(Reverb *r, float *out_lr, int frames) {
    int s, i;
    for (s = 0; s < frames; s++) {
        float in  = (out_lr[s * 2] + out_lr[s * 2 + 1]) * 0.5f * r->gain;
        float acc = 0.0f, w;
        for (i = 0; i < REVERB_COMBS; i++) {     /* parallel combs, summed */
            RevComb *c = &r->comb[i];
            float    y = c->buf[c->idx];
            c->store        = y * c->damp2 + c->store * c->damp1;   /* lowpass in the loop */
            c->buf[c->idx]  = in + c->store * c->feedback;
            if (++c->idx >= c->size) c->idx = 0;
            acc += y;
        }
        for (i = 0; i < REVERB_ALLPASS; i++) {   /* series allpass, density */
            RevAllpass *a = &r->ap[i];
            float bufout  = a->buf[a->idx];
            float y       = -acc + bufout;
            a->buf[a->idx] = acc + bufout * 0.5f;
            if (++a->idx >= a->size) a->idx = 0;
            acc = y;
        }
        w = acc * r->wet;
        out_lr[s * 2]     += w;                  /* mono tail into both channels */
        out_lr[s * 2 + 1] += w;
    }
}
