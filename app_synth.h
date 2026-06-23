/* app_synth.h — the synth book's page (TODO5 slice). Lays widgets over the
   introspected synth schema and binds them to a live param array. GL/scene/
   mixer-free: returns an action the host (main.c) synthesizes + plays. */
#ifndef APP_SYNTH_H
#define APP_SYNTH_H

#include "sol_base.h"
#include "widget.h"

typedef enum { SYNTH_ACT_NONE, SYNTH_ACT_PLAY, SYNTH_ACT_ROLL } SynthAction;

/* the curated, editable knobs (a subset of the 20-param schema). */
int app_synth_knob_count(void);          /* 4 for the slice */
int app_synth_knob_param(int i);         /* schema index of curated knob i */

/* lay the synth page into `ctx` over `params`, within the page-local rect whose
   top-left is (x0,y0) and size is (w,h) (meters, y-up). returns the action the
   host must service: PLAY/ROLL -> synthesize + play; NONE -> nothing. */
/* expects a dedicated WidgetCtx (it uses widget ids 1..N); do not share the ctx with host widgets. */
SynthAction app_synth_page(WidgetCtx *ctx, float *params,
                           float x0, float y0, float w, float h);

/* randomize the curated knobs in place, advancing the LCG state *rng. */
void app_synth_roll(float *params, sol_u32 *rng);

#endif /* APP_SYNTH_H */
