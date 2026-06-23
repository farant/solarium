/* widget.c — see widget.h. Pure C89: hot/active immediate-mode interaction,
   geometry emitted as a page-local draw-list. No GL, no scene, no synth. */
#include "widget.h"

static int point_in(float px, float py, float x, float y, float w, float h) {
    return px >= x && px <= x + w && py <= y && py >= y - h;
}

static void push_rect(WidgetCtx *c, float x, float y, float w, float h,
                      float r, float g, float b) {
    WidgetCmd *cmd;
    if (c->cmd_count >= WIDGET_MAX_CMDS) return;
    cmd = &c->cmds[c->cmd_count++];
    cmd->type = WIDGET_CMD_RECT;
    cmd->x = x; cmd->y = y; cmd->w = w; cmd->h = h;
    cmd->r = r; cmd->g = g; cmd->b = b;
    cmd->text = (const char *)0;
}

static void push_text(WidgetCtx *c, float x, float y, const char *text,
                      float size, float r, float g, float b) {
    WidgetCmd *cmd;
    if (c->cmd_count >= WIDGET_MAX_CMDS) return;
    cmd = &c->cmds[c->cmd_count++];
    cmd->type = WIDGET_CMD_TEXT;
    cmd->x = x; cmd->y = y; cmd->w = 0.0f; cmd->h = size;
    cmd->r = r; cmd->g = g; cmd->b = b;
    cmd->text = text;
}

void widget_begin(WidgetCtx *c, float ptr_x, float ptr_y,
                  sol_bool ptr_in, sol_bool down) {
    c->ptr_x     = ptr_x;
    c->ptr_y     = ptr_y;
    c->ptr_in    = ptr_in;
    c->down_prev = c->down;
    c->down      = down;
    c->hot_id    = 0;
    c->cmd_count = 0;
}

sol_bool widget_button(WidgetCtx *c, int id, float x, float y, float w, float h,
                       const char *label) {
    int      hover = c->ptr_in && point_in(c->ptr_x, c->ptr_y, x, y, w, h);
    sol_bool fired = SOL_FALSE;
    float    lum;
    if (id == 0) return SOL_FALSE;           /* 0 is the idle sentinel */
    if (hover) c->hot_id = id;
    if (c->active_id == id) {
        if (!c->down) {                      /* release */
            if (hover) fired = SOL_TRUE;
            c->active_id = 0;
        }
    } else if (hover && c->down && !c->down_prev) {
        c->active_id = id;                   /* a fresh press over us */
    }
    lum = (c->active_id == id) ? 0.30f : (hover ? 0.55f : 0.42f);
    push_rect(c, x, y, w, h, lum, lum, lum * 1.05f);
    push_text(c, x + w * 0.10f, y - h * 0.18f, label, h * 0.50f,
              0.96f, 0.94f, 0.88f);
    return fired;
}

sol_bool widget_slider(WidgetCtx *c, int id, float x, float y, float w, float h,
                       float *value, float lo, float hi) {
    int      hover = c->ptr_in && point_in(c->ptr_x, c->ptr_y, x, y, w, h);
    sol_bool changed = SOL_FALSE;
    float    frac, track_h, track_y, hw, hxl;
    if (id == 0) return SOL_FALSE;           /* 0 is the idle sentinel */
    if (hover) c->hot_id = id;
    if (c->active_id == id) {
        if (!c->down) c->active_id = 0;
    } else if (hover && c->down && !c->down_prev) {
        c->active_id = id;
    }
    if (c->active_id == id && hi > lo) {     /* drag -> write *value */
        float f = (c->ptr_x - x) / w;
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        *value  = lo + f * (hi - lo);
        changed = SOL_TRUE;
    }
    frac = (hi > lo) ? (*value - lo) / (hi - lo) : 0.0f;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    track_h = h * 0.30f;
    track_y = y - (h - track_h) * 0.5f;
    push_rect(c, x, track_y, w, track_h, 0.22f, 0.20f, 0.18f);
    hw  = h * 0.18f;
    hxl = x + frac * w - hw * 0.5f;
    if (hxl < x)            hxl = x;
    if (hxl > x + w - hw)   hxl = x + w - hw;
    push_rect(c, hxl, y, hw, h,
              (c->active_id == id) ? 0.90f : 0.70f, 0.62f, 0.30f);
    return changed;
}

void widget_label(WidgetCtx *c, float x, float y, const char *text, float size) {
    push_text(c, x, y, text, size, 0.13f, 0.10f, 0.08f);   /* ink */
}

void widget_end(WidgetCtx *c) {
    if (!c->down) c->active_id = 0;          /* safety: a lost release */
}
