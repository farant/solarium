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
    if (fails == 0) printf("widget_test: OK\n");
    return fails ? 1 : 0;
}
