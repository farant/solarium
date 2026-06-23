/* app_synth.c — see app_synth.h. The synth book's single page. C89. */
#include "app_synth.h"
#include "synth.h"

/* curated knobs: schema index + display range. Indices per synth.h. */
typedef struct { int param; float lo, hi; } Knob;
static const Knob KNOBS[] = {
    {  5,  80.0f, 2000.0f },   /* freq    Hz   */
    {  2,   0.0f,    1.0f },   /* sustain s    */
    {  4,   0.0f,    1.0f },   /* decay   s    */
    { 12,   0.0f,    1.0f }    /* duty    0..1 */
};
#define KNOB_COUNT ((int)(sizeof KNOBS / sizeof KNOBS[0]))

int app_synth_knob_count(void) { return KNOB_COUNT; }

int app_synth_knob_param(int i) {
    if (i < 0 || i >= KNOB_COUNT) return -1;
    return KNOBS[i].param;
}

/* the engine LCG (synth.c, meadow, codex, particles). */
static float rnd01(sol_u32 *rng) {
    *rng = *rng * 1664525u + 1013904223u;
    return (float)((*rng >> 8) & 0xFFFFu) / 65535.0f;
}

void app_synth_roll(float *params, sol_u32 *rng) {
    int i;
    for (i = 0; i < KNOB_COUNT; i++)
        params[KNOBS[i].param] =
            KNOBS[i].lo + rnd01(rng) * (KNOBS[i].hi - KNOBS[i].lo);
}

SynthAction app_synth_page(WidgetCtx *ctx, float *params,
                           float x0, float y0, float w, float h) {
    const char *const *names = synth_param_names();
    SynthAction act = SYNTH_ACT_NONE;
    float row, y, labw, sldw, lab_sz, btn_w, btn_h;
    int   i, id;

    /* the page consumes 7.95*row vertically (title 1.5 + 4*1.25 + 0.4 gap + 1.05 btn);
       keep row <= h/7.95 (~0.126*h) so nothing clips the page bottom. */
    row = h * 0.12f;
    if (row > 0.055f) row = 0.055f;
    lab_sz = row * 0.40f;
    labw   = w * 0.36f;
    sldw   = w - labw;
    y      = y0;
    id     = 1;

    widget_label(ctx, x0, y, "synth", row * 0.55f);
    y -= row * 1.5f;

    for (i = 0; i < KNOB_COUNT; i++) {
        widget_label(ctx, x0, y - (row - lab_sz) * 0.5f,
                     names[KNOBS[i].param], lab_sz);
        widget_slider(ctx, id++, x0 + labw, y, sldw, row * 0.72f,
                      &params[KNOBS[i].param], KNOBS[i].lo, KNOBS[i].hi);
        y -= row * 1.25f;
    }

    btn_w = w * 0.42f;
    btn_h = row * 1.05f;
    y    -= row * 0.4f;
    if (widget_button(ctx, id++, x0, y, btn_w, btn_h, "Sound"))
        act = SYNTH_ACT_PLAY;
    if (widget_button(ctx, id++, x0 + w - btn_w, y, btn_w, btn_h, "Roll"))
        act = SYNTH_ACT_ROLL;
    return act;
}
