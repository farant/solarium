#include "app_synth.h"
#include "synth.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
    /* the page emits 2 rects per slider + 1 rect per button, and labels */
    {
        WidgetCtx ctx; float p[SYNTH_PARAMS];
        int i, rects = 0, texts = 0;
        memset(&ctx, 0, sizeof ctx);
        memcpy(p, synth_preset("blip"), sizeof p);
        widget_begin(&ctx, 0.0f, 0.0f, SOL_FALSE, SOL_FALSE);
        app_synth_page(&ctx, p, 0.0f, 0.5f, 0.30f, 0.40f);
        widget_end(&ctx);
        for (i = 0; i < ctx.cmd_count; i++) {
            if (ctx.cmds[i].type == WIDGET_CMD_RECT) rects++;
            else texts++;
        }
        CHECK(rects == app_synth_knob_count() * 2 + 2);     /* sliders + 2 buttons */
        CHECK(texts >= app_synth_knob_count() + 1);          /* knob labels + title */
    }
    /* press + release over the "Sound" label returns PLAY */
    {
        WidgetCtx ctx; float p[SYNTH_PARAMS];
        int   i; float sx = -1.0f, sy = -1.0f;
        SynthAction a;
        memset(&ctx, 0, sizeof ctx);
        memcpy(p, synth_preset("blip"), sizeof p);
        widget_begin(&ctx, 0.0f, 0.0f, SOL_FALSE, SOL_FALSE);
        app_synth_page(&ctx, p, 0.0f, 0.5f, 0.30f, 0.40f);
        widget_end(&ctx);
        for (i = 0; i < ctx.cmd_count; i++)
            if (ctx.cmds[i].type == WIDGET_CMD_TEXT &&
                strcmp(ctx.cmds[i].text, "Sound") == 0) {
                sx = ctx.cmds[i].x; sy = ctx.cmds[i].y;
            }
        CHECK(sx >= 0.0f);
        widget_begin(&ctx, sx + 0.004f, sy - 0.004f, SOL_TRUE, SOL_TRUE);
        app_synth_page(&ctx, p, 0.0f, 0.5f, 0.30f, 0.40f);
        widget_end(&ctx);
        widget_begin(&ctx, sx + 0.004f, sy - 0.004f, SOL_TRUE, SOL_FALSE);
        a = app_synth_page(&ctx, p, 0.0f, 0.5f, 0.30f, 0.40f);
        widget_end(&ctx);
        CHECK(a == SYNTH_ACT_PLAY);
    }
    /* roll is seed-deterministic, moves curated knobs, leaves others alone */
    {
        float a[SYNTH_PARAMS], b[SYNTH_PARAMS];
        sol_u32 r1 = 12345u, r2 = 12345u;
        int i, diff = 0;
        memcpy(a, synth_preset("blip"), sizeof a);
        memcpy(b, synth_preset("blip"), sizeof b);
        app_synth_roll(a, &r1);
        app_synth_roll(b, &r2);
        for (i = 0; i < SYNTH_PARAMS; i++) if (a[i] != b[i]) diff = 1;
        CHECK(!diff);                                   /* same seed -> same roll */
        CHECK(a[5] + a[2] + a[4] + a[12] !=
              synth_preset("blip")[5] + synth_preset("blip")[2] +
              synth_preset("blip")[4] + synth_preset("blip")[12]); /* a curated knob moved */
        CHECK(a[0] == synth_preset("blip")[0]);          /* wave (idx 0) untouched */
    }
    if (fails == 0) printf("app_synth_test: OK\n");
    return fails ? 1 : 0;
}
