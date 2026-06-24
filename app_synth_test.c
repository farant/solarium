#include "app_synth.h"
#include "synth.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
    /* the page lays out the title, wave radio, 4 sliders, crush stepper,
       low-pass checkbox, and the two buttons */
    {
        WidgetCtx ctx; float p[SYNTH_PARAMS];
        int i, rects = 0, texts = 0;
        int has_title = 0, has_wave = 0, has_lp = 0, has_sound = 0, has_roll = 0;
        memset(&ctx, 0, sizeof ctx);
        memcpy(p, synth_preset("blip"), sizeof p);
        widget_begin(&ctx, 0.0f, 0.0f, SOL_FALSE, SOL_FALSE);
        app_synth_page(&ctx, p, 0.0f, 0.5f, 0.30f, 0.40f);
        widget_end(&ctx);
        for (i = 0; i < ctx.cmd_count; i++) {
            if (ctx.cmds[i].type == WIDGET_CMD_RECT) { rects++; continue; }
            texts++;
            if (!strcmp(ctx.cmds[i].text, "synth"))    has_title = 1;
            if (!strcmp(ctx.cmds[i].text, "SQ"))       has_wave  = 1;
            if (!strcmp(ctx.cmds[i].text, "low-pass")) has_lp    = 1;
            if (!strcmp(ctx.cmds[i].text, "Sound"))    has_sound = 1;
            if (!strcmp(ctx.cmds[i].text, "Roll"))     has_roll  = 1;
        }
        CHECK(has_title && has_wave && has_lp && has_sound && has_roll);
        /* sliders(8) + radio(5) + stepper(2) + checkbox-off(1) + buttons(2) */
        CHECK(rects == app_synth_knob_count() * 2 + 5 + 2 + 1 + 2);
        (void)texts;
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
    /* clicking the NZ wave cell sets params[0] (wave) to 4 (noise) */
    {
        WidgetCtx ctx; float p[SYNTH_PARAMS]; int i; float wx = -1.0f, wy = -1.0f;
        memset(&ctx, 0, sizeof ctx);
        memcpy(p, synth_preset("blip"), sizeof p);
        widget_begin(&ctx, 0.0f, 0.0f, SOL_FALSE, SOL_FALSE);
        app_synth_page(&ctx, p, 0.0f, 0.5f, 0.30f, 0.40f);
        widget_end(&ctx);
        for (i = 0; i < ctx.cmd_count; i++)
            if (ctx.cmds[i].type == WIDGET_CMD_TEXT &&
                strcmp(ctx.cmds[i].text, "NZ") == 0) {
                wx = ctx.cmds[i].x; wy = ctx.cmds[i].y;
            }
        CHECK(wx >= 0.0f);
        widget_begin(&ctx, wx + 0.004f, wy - 0.004f, SOL_TRUE, SOL_TRUE);
        app_synth_page(&ctx, p, 0.0f, 0.5f, 0.30f, 0.40f);
        widget_end(&ctx);
        widget_begin(&ctx, wx + 0.004f, wy - 0.004f, SOL_TRUE, SOL_FALSE);
        app_synth_page(&ctx, p, 0.0f, 0.5f, 0.30f, 0.40f);
        widget_end(&ctx);
        CHECK((int)p[0] == 4);
    }
    /* clicking the crush "+" steps params[16] from 0 to 1 */
    {
        WidgetCtx ctx; float p[SYNTH_PARAMS]; int i; float px = -1.0f, py = -1.0f;
        memset(&ctx, 0, sizeof ctx);
        memcpy(p, synth_preset("blip"), sizeof p);
        widget_begin(&ctx, 0.0f, 0.0f, SOL_FALSE, SOL_FALSE);
        app_synth_page(&ctx, p, 0.0f, 0.5f, 0.30f, 0.40f);
        widget_end(&ctx);
        for (i = 0; i < ctx.cmd_count; i++)
            if (ctx.cmds[i].type == WIDGET_CMD_TEXT &&
                strcmp(ctx.cmds[i].text, "+") == 0) {
                px = ctx.cmds[i].x; py = ctx.cmds[i].y;
            }
        CHECK(px >= 0.0f);
        widget_begin(&ctx, px + 0.002f, py - 0.004f, SOL_TRUE, SOL_TRUE);
        app_synth_page(&ctx, p, 0.0f, 0.5f, 0.30f, 0.40f);
        widget_end(&ctx);
        widget_begin(&ctx, px + 0.002f, py - 0.004f, SOL_TRUE, SOL_FALSE);
        app_synth_page(&ctx, p, 0.0f, 0.5f, 0.30f, 0.40f);
        widget_end(&ctx);
        CHECK((int)p[16] == 1);
    }
    /* clicking the low-pass box toggles params[14] from 0 to 2000 */
    {
        WidgetCtx ctx; float p[SYNTH_PARAMS];
        int i, lp_idx = -1, box_idx = -1;
        float bx, by, bs;
        memset(&ctx, 0, sizeof ctx);
        memcpy(p, synth_preset("blip"), sizeof p);
        widget_begin(&ctx, 0.0f, 0.0f, SOL_FALSE, SOL_FALSE);
        app_synth_page(&ctx, p, 0.0f, 0.5f, 0.30f, 0.40f);
        widget_end(&ctx);
        for (i = 0; i < ctx.cmd_count; i++)
            if (ctx.cmds[i].type == WIDGET_CMD_TEXT &&
                strcmp(ctx.cmds[i].text, "low-pass") == 0)
                lp_idx = i;
        CHECK(lp_idx > 0);
        for (i = lp_idx - 1; i >= 0; i--)
            if (ctx.cmds[i].type == WIDGET_CMD_RECT) { box_idx = i; break; }
        CHECK(box_idx >= 0);
        bx = ctx.cmds[box_idx].x;
        by = ctx.cmds[box_idx].y;
        bs = ctx.cmds[box_idx].w;
        widget_begin(&ctx, bx + bs * 0.5f, by - bs * 0.5f, SOL_TRUE, SOL_TRUE);
        app_synth_page(&ctx, p, 0.0f, 0.5f, 0.30f, 0.40f);
        widget_end(&ctx);
        widget_begin(&ctx, bx + bs * 0.5f, by - bs * 0.5f, SOL_TRUE, SOL_FALSE);
        app_synth_page(&ctx, p, 0.0f, 0.5f, 0.30f, 0.40f);
        widget_end(&ctx);
        CHECK(p[14] > 1000.0f);
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
