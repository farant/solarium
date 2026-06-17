/* mixer.c — see mixer.h. Pure C89; no threads, no I/O, no allocation. */

#include "mixer.h"

#include <math.h>

void mixer_init(Mixer *m) {
    int i;
    reverb_init(&m->rev);
    for (i = 0; i < MIX_VOICES; i++) {
        m->v[i].active = 0;
        m->v[i].gen    = 0u;
    }
}

void mixer_apply(Mixer *m, const MixCmd *c) {
    MixVoice *v;
    if (c->slot < 0 || c->slot >= MIX_VOICES) return;
    v = &m->v[c->slot];
    switch (c->kind) {
    case MIX_CMD_START:
        v->buf    = c->buf;
        v->frames = c->frames;
        v->cursor = 0;
        v->gain   = c->gain;
        v->pan    = c->pan;
        v->gen    = c->gen;
        v->loop   = c->loop;
        v->active = (c->buf != (const float *)0 && c->frames > 0);
        break;
    case MIX_CMD_STOP:
        if (v->gen == c->gen) v->active = 0;
        break;
    case MIX_CMD_SET:
        if (v->gen == c->gen && v->active) {
            v->gain = c->gain;
            v->pan  = c->pan;
        }
        break;
    case MIX_CMD_REVERB:                       /* no voice — the global reverb (P8 item 8) */
        reverb_set(&m->rev, c->rv_decay, c->rv_damp, c->rv_wet);
        break;
    default: break;
    }
}

void mixer_render(Mixer *m, float *out_lr, int frames) {
    int i, s;
    for (i = 0; i < frames * 2; i++) out_lr[i] = 0.0f;

    for (i = 0; i < MIX_VOICES; i++) {
        MixVoice *v = &m->v[i];
        float pl, pr, th;
        if (!v->active) continue;
        /* constant power: a quarter circle from hard-left to hard-right */
        th = (v->pan + 1.0f) * 0.78539816f;       /* pi/4 */
        pl = cosf(th) * v->gain;
        pr = sinf(th) * v->gain;
        for (s = 0; s < frames; s++) {
            float x;
            if (v->cursor >= v->frames) {
                if (v->loop) v->cursor = 0;
                else { v->active = 0; break; }
            }
            x = v->buf[v->cursor++];
            out_lr[s * 2]     += x * pl;
            out_lr[s * 2 + 1] += x * pr;
        }
    }

    reverb_process(&m->rev, out_lr, frames);   /* the room's tail (P8 item 8), before the clamp */

    for (i = 0; i < frames * 2; i++) {
        if (out_lr[i] >  1.0f) out_lr[i] =  1.0f;
        if (out_lr[i] < -1.0f) out_lr[i] = -1.0f;
    }
}
