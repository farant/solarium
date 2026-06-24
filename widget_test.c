#include "widget.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* one frame with a single button at rect x[0,0.5], y[0.8,1.0] */
static sol_bool frame_button(WidgetCtx *c, float px, float py, sol_bool down) {
    sol_bool f;
    widget_begin(c, px, py, SOL_TRUE, down);
    f = widget_button(c, 1, 0.0f, 1.0f, 0.5f, 0.2f, "go");
    widget_end(c);
    return f;
}

int main(void) {
    /* a button fires once on press-then-release while hovered */
    {
        WidgetCtx c; memset(&c, 0, sizeof c);
        CHECK(!frame_button(&c, 0.2f, 0.9f, SOL_FALSE));  /* hover, up */
        CHECK(!frame_button(&c, 0.2f, 0.9f, SOL_TRUE));   /* press: not yet */
        CHECK( frame_button(&c, 0.2f, 0.9f, SOL_FALSE));  /* release over: fire */
        CHECK(!frame_button(&c, 0.2f, 0.9f, SOL_FALSE));  /* no double-fire */
    }
    /* released off the button: no fire */
    {
        WidgetCtx c; memset(&c, 0, sizeof c);
        frame_button(&c, 0.2f, 0.9f, SOL_TRUE);            /* press on */
        CHECK(!frame_button(&c, 5.0f, 5.0f, SOL_FALSE));   /* release off */
    }
    /* a slider tracks the pointer x while dragging (rect x[0,1], y[0.5,0.7]) */
    {
        WidgetCtx c; float v = 0.0f; memset(&c, 0, sizeof c);
        widget_begin(&c, 0.0f, 0.6f, SOL_TRUE, SOL_TRUE);  /* press at left */
        widget_slider(&c, 1, 0.0f, 0.7f, 1.0f, 0.2f, &v, 0.0f, 10.0f);
        widget_end(&c);
        widget_begin(&c, 0.5f, 0.6f, SOL_TRUE, SOL_TRUE);  /* drag to middle */
        widget_slider(&c, 1, 0.0f, 0.7f, 1.0f, 0.2f, &v, 0.0f, 10.0f);
        widget_end(&c);
        CHECK(v > 4.9f && v < 5.1f);
    }
    /* a drag that never pressed on the slider leaves it unchanged */
    {
        WidgetCtx c; float v = 3.0f; memset(&c, 0, sizeof c);
        widget_begin(&c, 5.0f, 5.0f, SOL_TRUE, SOL_TRUE);  /* press off */
        widget_slider(&c, 1, 0.0f, 0.7f, 1.0f, 0.2f, &v, 0.0f, 10.0f);
        widget_end(&c);
        widget_begin(&c, 0.5f, 0.6f, SOL_TRUE, SOL_TRUE);  /* move over, still down */
        widget_slider(&c, 1, 0.0f, 0.7f, 1.0f, 0.2f, &v, 0.0f, 10.0f);
        widget_end(&c);
        CHECK(v > 2.9f && v < 3.1f);
    }
    /* the slider emits a track + a handle, the handle near mid for v=5 */
    {
        WidgetCtx c; float v = 5.0f; int i, rects = 0; float hx = -1.0f;
        memset(&c, 0, sizeof c);
        widget_begin(&c, 0.0f, 0.0f, SOL_FALSE, SOL_FALSE);
        widget_slider(&c, 1, 0.0f, 0.7f, 1.0f, 0.2f, &v, 0.0f, 10.0f);
        widget_end(&c);
        for (i = 0; i < c.cmd_count; i++)
            if (c.cmds[i].type == WIDGET_CMD_RECT) {
                rects++;
                if (rects == 2) hx = c.cmds[i].x;
            }
        CHECK(rects == 2);
        CHECK(hx > 0.3f && hx < 0.6f);
    }
    /* degenerate hi==lo: value untouched, no write */
    {
        WidgetCtx c; float v = 7.0f; memset(&c, 0, sizeof c);
        widget_begin(&c, 0.5f, 0.6f, SOL_TRUE, SOL_TRUE);
        CHECK(!widget_slider(&c, 1, 0.0f, 0.7f, 1.0f, 0.2f, &v, 5.0f, 5.0f));
        widget_end(&c);
        CHECK(v > 6.9f && v < 7.1f);
    }
    /* checkbox toggles on a press+release over the box (box x[0,0.1] y[0.9,1.0]) */
    {
        WidgetCtx c; sol_bool v = SOL_FALSE; memset(&c, 0, sizeof c);
        widget_begin(&c, 0.05f, 0.95f, SOL_TRUE, SOL_TRUE);   /* press on box */
        widget_checkbox(&c, 1, 0.0f, 1.0f, 0.1f, &v, "x");
        widget_end(&c);
        CHECK(!v);                                             /* not yet */
        widget_begin(&c, 0.05f, 0.95f, SOL_TRUE, SOL_FALSE);  /* release on box */
        CHECK(widget_checkbox(&c, 1, 0.0f, 1.0f, 0.1f, &v, "x"));
        widget_end(&c);
        CHECK(v);                                              /* toggled on */
    }
    /* checkbox does NOT toggle when released off the box */
    {
        WidgetCtx c; sol_bool v = SOL_FALSE; memset(&c, 0, sizeof c);
        widget_begin(&c, 0.05f, 0.95f, SOL_TRUE, SOL_TRUE);
        widget_checkbox(&c, 1, 0.0f, 1.0f, 0.1f, &v, "x");
        widget_end(&c);
        widget_begin(&c, 5.0f, 5.0f, SOL_TRUE, SOL_FALSE);    /* release off */
        CHECK(!widget_checkbox(&c, 1, 0.0f, 1.0f, 0.1f, &v, "x"));
        widget_end(&c);
        CHECK(!v);
    }
    /* radio selects the clicked cell (bar x[0,0.3] y[0.7,0.9], 3 cells of 0.1) */
    {
        WidgetCtx c; int sel; const char *labels[3];
        memset(&c, 0, sizeof c);
        labels[0] = "a"; labels[1] = "b"; labels[2] = "c";
        widget_begin(&c, 0.25f, 0.8f, SOL_TRUE, SOL_TRUE);    /* press cell 2 */
        widget_radio(&c, 1, 0.0f, 0.9f, 0.3f, 0.2f, labels, 3, 0);
        widget_end(&c);
        widget_begin(&c, 0.25f, 0.8f, SOL_TRUE, SOL_FALSE);   /* release cell 2 */
        sel = widget_radio(&c, 1, 0.0f, 0.9f, 0.3f, 0.2f, labels, 3, 0);
        widget_end(&c);
        CHECK(sel == 2);
    }
    /* radio: a release outside the bar keeps the value; it emits count cells */
    {
        WidgetCtx c; int sel, i, rects = 0, texts = 0; const char *labels[3];
        memset(&c, 0, sizeof c);
        labels[0] = "a"; labels[1] = "b"; labels[2] = "c";
        widget_begin(&c, 0.25f, 0.8f, SOL_TRUE, SOL_TRUE);    /* press in bar */
        widget_radio(&c, 1, 0.0f, 0.9f, 0.3f, 0.2f, labels, 3, 1);
        widget_end(&c);
        widget_begin(&c, 5.0f, 5.0f, SOL_TRUE, SOL_FALSE);    /* release off */
        sel = widget_radio(&c, 1, 0.0f, 0.9f, 0.3f, 0.2f, labels, 3, 1);
        widget_end(&c);
        CHECK(sel == 1);
        for (i = 0; i < c.cmd_count; i++)
            if (c.cmds[i].type == WIDGET_CMD_RECT) rects++; else texts++;
        CHECK(rects == 3 && texts == 3);
    }
    /* stepper: minus decrements; plus clamps at hi; middle release is a no-op
       (bar x[0,0.3] y[0.7,0.9]; minus x[0,0.084], plus x[0.216,0.3]) */
    {
        WidgetCtx c; int v; memset(&c, 0, sizeof c);
        widget_begin(&c, 0.02f, 0.8f, SOL_TRUE, SOL_TRUE);    /* press minus */
        widget_stepper(&c, 1, 0.0f, 0.9f, 0.3f, 0.2f, "5", 5, 0, 16);
        widget_end(&c);
        widget_begin(&c, 0.02f, 0.8f, SOL_TRUE, SOL_FALSE);   /* release minus */
        v = widget_stepper(&c, 1, 0.0f, 0.9f, 0.3f, 0.2f, "5", 5, 0, 16);
        widget_end(&c);
        CHECK(v == 4);
    }
    {
        WidgetCtx c; int v; memset(&c, 0, sizeof c);
        widget_begin(&c, 0.28f, 0.8f, SOL_TRUE, SOL_TRUE);    /* press plus at hi */
        widget_stepper(&c, 1, 0.0f, 0.9f, 0.3f, 0.2f, "16", 16, 0, 16);
        widget_end(&c);
        widget_begin(&c, 0.28f, 0.8f, SOL_TRUE, SOL_FALSE);
        v = widget_stepper(&c, 1, 0.0f, 0.9f, 0.3f, 0.2f, "16", 16, 0, 16);
        widget_end(&c);
        CHECK(v == 16);                                        /* clamped */
    }
    {
        WidgetCtx c; int v; memset(&c, 0, sizeof c);
        widget_begin(&c, 0.15f, 0.8f, SOL_TRUE, SOL_TRUE);    /* press middle */
        widget_stepper(&c, 1, 0.0f, 0.9f, 0.3f, 0.2f, "8", 8, 0, 16);
        widget_end(&c);
        widget_begin(&c, 0.15f, 0.8f, SOL_TRUE, SOL_FALSE);
        v = widget_stepper(&c, 1, 0.0f, 0.9f, 0.3f, 0.2f, "8", 8, 0, 16);
        widget_end(&c);
        CHECK(v == 8);                                         /* no-op */
    }
    if (fails == 0) printf("widget_test: OK\n");
    return fails ? 1 : 0;
}
