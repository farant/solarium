/* widget.h — a pure, coordinate-agnostic immediate-mode widget core (TODO5).
   No GL, no scene, no synth: it takes a pointer in page-local 2D + mouse bits,
   runs the classic imgui hot/active state machine, and emits each widget's
   geometry as a draw-list of page-local rects + text for a host to render.
   The host (main.c reader) maps the draw-list onto the open book's page. */
#ifndef WIDGET_H
#define WIDGET_H

#include "sol_base.h"

#define WIDGET_MAX_CMDS 128  /* commands past the cap are silently dropped; ample for one page */

typedef enum { WIDGET_CMD_RECT, WIDGET_CMD_TEXT } WidgetCmdType;

/* page-local meters, y-up; (x,y) = top-left. RECT spans x..x+w, y-h..y.
   TEXT draws from top-left (x,y); h = text height in meters; w unused.
   `text` is a BORROWED pointer (string literal or static schema name) and
   must stay valid until the host has walked the list. */
typedef struct {
    WidgetCmdType type;
    float         x, y, w, h;
    float         r, g, b;
    const char   *text;
} WidgetCmd;

typedef struct {
    float     ptr_x, ptr_y;     /* pointer in page-local meters this frame */
    sol_bool  ptr_in;           /* pointer is over the page at all */
    sol_bool  down, down_prev;  /* left mouse this frame / last frame */
    int       hot_id, active_id;
    WidgetCmd cmds[WIDGET_MAX_CMDS];
    int       cmd_count;
} WidgetCtx;

/* id must be > 0; 0 is the "no widget active" sentinel */
void     widget_begin(WidgetCtx *c, float ptr_x, float ptr_y,
                      sol_bool ptr_in, sol_bool down);
/* returns SOL_TRUE on the one frame the button fires (release while hovered) */
sol_bool widget_button(WidgetCtx *c, int id, float x, float y, float w, float h,
                       const char *label);
/* returns SOL_TRUE on every frame the value is written (the whole drag, not just deltas) */
sol_bool widget_slider(WidgetCtx *c, int id, float x, float y, float w, float h,
                       float *value, float lo, float hi);
void     widget_label(WidgetCtx *c, float x, float y, const char *text,
                      float size);
void     widget_end(WidgetCtx *c);

#endif /* WIDGET_H */
