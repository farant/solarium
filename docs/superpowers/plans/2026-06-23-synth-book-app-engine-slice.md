# Synth Book — App-Engine Vertical Slice Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build one diegetic synth book whose single page has labeled sliders + Sound/Roll buttons, proving the whole TODO5 stack: cursor-over-page input → pure widget core → synth schema → `synth_render` → mixer.

**Architecture:** Three units — `widget.c/.h` (pure immediate-mode core emitting a page-local draw-list), `app_synth.c/.h` (near-pure page layout over the introspected synth schema), and `main.c` glue (the one impure site: page-plane geometry, cursor ray, draw-list walk onto the page via `page_m`, input mode, mint). No new shader → no MSL twin.

**Tech Stack:** C89 (strict, `-pedantic-errors`), the existing RHI (`draw_mesh` + `wtext`), GLFW input, the existing synth (`synth.h`) + mixer (`play_oneshot`).

**Branch:** `todo5-synth-book` (create at start; ff-merge to `main` at the end via finishing-a-development-branch).

**Spec:** `docs/superpowers/specs/2026-06-23-synth-book-app-engine-slice-design.md`

**C89 reminders (this codebase is strict):** declarations at the top of every block; `/* */` comments only (no `//`); no mixed declaration/statement; no VLAs; `snprintf`/`sscanf`/`strncpy`; never bare `fabsf` — use `fabs((double)x)`. The gauntlet `./build.sh c89check` enforces this on all non-test sources.

**Never commit** `NOTES.stml` or `paper-picture.png` (Fran's files). Commit messages end with:
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

---

## File Structure

| File | New? | Responsibility |
|------|------|----------------|
| `widget.h` | new | `WidgetCtx`, `WidgetCmd`, the 5 entry points |
| `widget.c` | new | pure immediate-mode core (hot/active, draw-list) |
| `widget_test.c` | new | interaction + emitted-geometry assertions |
| `app_synth.h` | new | `SynthAction`, knob introspection, `app_synth_page`/`_roll` |
| `app_synth.c` | new | page layout over the curated knobs |
| `app_synth_test.c` | new | layout / action / roll-determinism assertions |
| `main.c` | modify | includes, AppState fields, helpers, reader integration, input mode, mint |
| `build.sh` | modify | 4 source lists + 2 test targets |

---

### Task 0: Branch

- [ ] **Step 1: Create the feature branch**

```bash
cd /Users/francisarant/Documents/projects/solarium
git checkout -b todo5-synth-book
```

---

### Task 1: The widget core (pure)

**Files:**
- Create: `widget.h`, `widget.c`, `widget_test.c`
- Modify: `build.sh` (add the `widgettest` target)

**Coordinate convention (matches `wtext_block`):** page-local meters, x→right, y→**up**. A widget rect is given by its **top-left** corner `(x, y)` plus width `w` (rightward) and height `h` (downward) — so it occupies `x..x+w` horizontally and `y-h..y` vertically. A TEXT command's `h` field carries the desired text height **in meters** (the host converts to the font's px-to-m). `id` is a caller-assigned stable non-zero integer.

- [ ] **Step 1: Write `widget.h`**

```c
/* widget.h — a pure, coordinate-agnostic immediate-mode widget core (TODO5).
   No GL, no scene, no synth: it takes a pointer in page-local 2D + mouse bits,
   runs the classic imgui hot/active state machine, and emits each widget's
   geometry as a draw-list of page-local rects + text for a host to render.
   The host (main.c reader) maps the draw-list onto the open book's page. */
#ifndef WIDGET_H
#define WIDGET_H

#include "sol_base.h"

#define WIDGET_MAX_CMDS 128

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

void     widget_begin(WidgetCtx *c, float ptr_x, float ptr_y,
                      sol_bool ptr_in, sol_bool down);
sol_bool widget_button(WidgetCtx *c, int id, float x, float y, float w, float h,
                       const char *label);
sol_bool widget_slider(WidgetCtx *c, int id, float x, float y, float w, float h,
                       float *value, float lo, float hi);
void     widget_label (WidgetCtx *c, float x, float y, const char *text,
                       float size);
void     widget_end(WidgetCtx *c);

#endif /* WIDGET_H */
```

- [ ] **Step 2: Write the failing test `widget_test.c`**

```c
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
    if (fails == 0) printf("widget_test: OK\n");
    return fails ? 1 : 0;
}
```

- [ ] **Step 3: Add the `widgettest` build target to `build.sh`**

Insert immediately after the `inventorytest` block (after its `fi`, currently line 156):

```bash
# widgettest: the pure immediate-mode widget core (scene-free C89). libc only.
if [ "$MODE" = "widgettest" ]; then
    set -x
    clang -std=c89 -pedantic-errors -Werror -g -fsanitize=address,undefined \
        widget.c widget_test.c \
        -o widget_test
    echo "built ./widget_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi
```

- [ ] **Step 4: Run the test to verify it fails to link**

Run: `./build.sh widgettest`
Expected: FAIL — `widget.c` has no function bodies yet (undefined symbols `widget_begin`, …).

- [ ] **Step 5: Write `widget.c`**

```c
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
    if (hover) c->hot_id = id;
    if (c->active_id == id) {
        if (!c->down) c->active_id = 0;
    } else if (hover && c->down && !c->down_prev) {
        c->active_id = id;
    }
    if (c->active_id == id && hi > lo) {     /* drag → write *value */
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
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `./build.sh widgettest && ./widget_test`
Expected: `widget_test: OK`

- [ ] **Step 7: Commit**

```bash
git add widget.h widget.c widget_test.c build.sh
git commit -m "$(cat <<'EOF'
TODO5 widget core: pure immediate-mode button/slider/label

Coordinate-agnostic hot/active state machine that emits a page-local draw-list
of rects + text. No GL/scene/synth — unit-tested by synthetic pointer/click.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: The synth app (near-pure)

**Files:**
- Create: `app_synth.h`, `app_synth.c`, `app_synth_test.c`
- Modify: `build.sh` (add the `appsynthtest` target)

The app lays the page out over four curated, expressive, continuous schema knobs (`freq` idx 5, `sustain` idx 2, `decay` idx 4, `duty` idx 12 — see the schema in `synth.h`). It is free of GL/scene/mixer; the host services the returned action.

- [ ] **Step 1: Write `app_synth.h`**

```c
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
   host must service: PLAY/ROLL → synthesize + play; NONE → nothing. */
SynthAction app_synth_page(WidgetCtx *ctx, float *params,
                           float x0, float y0, float w, float h);

/* randomize the curated knobs in place, advancing the LCG state *rng. */
void app_synth_roll(float *params, sol_u32 *rng);

#endif /* APP_SYNTH_H */
```

- [ ] **Step 2: Write the failing test `app_synth_test.c`**

```c
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
        CHECK(!diff);                                   /* same seed → same roll */
        CHECK(a[5] != synth_preset("blip")[5] ||
              a[4] != synth_preset("blip")[4]);          /* a curated knob moved */
        CHECK(a[0] == synth_preset("blip")[0]);          /* wave (idx 0) untouched */
    }
    if (fails == 0) printf("app_synth_test: OK\n");
    return fails ? 1 : 0;
}
```

- [ ] **Step 3: Add the `appsynthtest` build target to `build.sh`**

Insert immediately after the `synthtest` block (after its `fi`, currently line 215):

```bash
# appsynthtest: the synth book's page layout (scene/GL-free C89). Links the
# widget core + the synth schema it introspects.
if [ "$MODE" = "appsynthtest" ]; then
    set -x
    clang -std=c89 -pedantic-errors -Werror -g -fsanitize=address,undefined \
        app_synth.c widget.c synth.c app_synth_test.c \
        -o app_synth_test
    echo "built ./app_synth_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi
```

- [ ] **Step 4: Run the test to verify it fails to link**

Run: `./build.sh appsynthtest`
Expected: FAIL — `app_synth.c` has no bodies yet (undefined `app_synth_page`, …).

- [ ] **Step 5: Write `app_synth.c`**

```c
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
    if (i < 0 || i >= KNOB_COUNT) return 0;
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

    row = h * 0.13f;
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
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `./build.sh appsynthtest && ./app_synth_test`
Expected: `app_synth_test: OK`

- [ ] **Step 7: Commit**

```bash
git add app_synth.h app_synth.c app_synth_test.c build.sh
git commit -m "$(cat <<'EOF'
TODO5 synth app: the synth book page over the schema

Four curated sliders (freq/sustain/decay/duty) + Sound/Roll buttons laid into a
given page rect; returns an action the host services. Roll randomizes the
curated knobs via the engine LCG. GL/scene/mixer-free; unit-tested.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: main.c scaffolding — includes, state, helpers, source-list wiring

**Files:**
- Modify: `main.c` (includes near top; AppState fields near line 2779; helpers; `reader_page_matrix` extraction)
- Modify: `build.sh` (4 source lists)

This task adds compile-only plumbing — no behavior yet — and verifies it builds on both backends.

- [ ] **Step 1: Add includes**

Near the other module includes at the top of `main.c` (the `#include "..."` block around line 28), add:

```c
#include "widget.h"
#include "app_synth.h"
```

(`synth.h` is already included — it backs the existing audio. If a build error says `SYNTH_PARAMS` is undefined, add `#include "synth.h"` here too.)

- [ ] **Step 2: Add AppState fields**

Immediately after `LeafShape reader_leaf_shape;` (currently main.c:2779), inside the `AppState` struct, add:

```c
    /* the app book (TODO5 slice): an open book whose meta["app"] routes the
       reader to an in-world widget UI instead of page text. */
    int       reader_app;                  /* 0 none, 1 synth */
    sol_bool  reader_app_was;              /* edge tracker for the cursor toggle */
    float     synth_params[SYNTH_PARAMS];  /* the open book's live patch */
    sol_u32   synth_rng;                   /* the "Roll" LCG state */
    WidgetCtx widget_ctx;                  /* this frame's emitted draw-list */
    Mesh      widget_quad;                 /* unit XY quad for widget rects (lazy) */
```

- [ ] **Step 3: Extract the page-matrix helper**

The reader render block (main.c:11782-11796) computes the page plane inline. Extract it into a helper so the input path shares the exact same geometry. Add this **static helper above `reader_open`** (before main.c:5881):

```c
/* The open book's flat page-plane transform: wtext_block draws ink on its z=0
   plane (x right, y up, page-local meters). Shared by the page render and the
   app-book cursor hit-test. Optionally returns the page rect metrics. */
static mat4 reader_page_matrix(const AppState *st, float *out_wb, float *out_zh,
                               float *out_xf, float *out_mg) {
    const float *bp    = st->reader_params;
    float        wb    = bp[0] - bp[4];
    float        zh    = bp[1] * 0.5f - bp[4];
    float        stack = (bp[2] - 2.0f * bp[3]) * 0.5f;
    float        xf, fy, mg;
    mat4         bm, page;
    if (stack < 0.004f) stack = 0.004f;
    xf = wb * BOOK_GUTTER_FRAC;
    fy = bp[3] + stack + 0.0012f;
    mg = wb * 0.06f;
    bm = mat4_from_trs(st->reader_pos, st->reader_rot, vec3_make(1.0f, 1.0f, 1.0f));
    page = mat4_mul(bm, mat4_mul(mat4_translate(vec3_make(0.0f, fy, 0.0f)),
               quat_to_mat4(quat_from_axis_angle(
                   vec3_make(1.0f, 0.0f, 0.0f), sol_radians(-90.0f)))));
    if (out_wb) *out_wb = wb;
    if (out_zh) *out_zh = zh;
    if (out_xf) *out_xf = xf;
    if (out_mg) *out_mg = mg;
    return page;
}
```

Then replace the inline computation in the render block. The current code at main.c:11782-11796 is:

```c
            const float *bp = state->reader_params;
            float wb    = bp[0] - bp[4];
            float zh    = bp[1] * 0.5f - bp[4];
            float stack = (bp[2] - 2.0f * bp[3]) * 0.5f;
            float xf, fy, mg;
            mat4  bm, page;
            if (stack < 0.004f) stack = 0.004f;
            xf   = wb * BOOK_GUTTER_FRAC;
            fy   = bp[3] + stack + 0.0012f;
            mg   = wb * 0.06f;
            bm   = mat4_from_trs(state->reader_pos, state->reader_rot,
                                 vec3_make(1.0f, 1.0f, 1.0f));
            page = mat4_mul(bm, mat4_mul(mat4_translate(vec3_make(0.0f, fy, 0.0f)),
                       quat_to_mat4(quat_from_axis_angle(
                           vec3_make(1.0f, 0.0f, 0.0f), sol_radians(-90.0f)))));
```

Replace it with (note: `bm` is still needed below for the leaf hinge at ~11861, so keep it):

```c
            const float *bp = state->reader_params;
            float wb, zh, xf, mg;
            mat4  bm, page;
            page = reader_page_matrix(state, &wb, &zh, &xf, &mg);
            bm   = mat4_from_trs(state->reader_pos, state->reader_rot,
                                 vec3_make(1.0f, 1.0f, 1.0f));
```

(`bp` stays — later lines use `bp[3]`, `bp[1]`, etc. `bm` is recomputed cheaply for the leaf hinge. The values are bit-identical to before.)

- [ ] **Step 4: Add the cursor-ray + page-hit + reader-app helpers**

Add these static helpers **above `reader_page_matrix`** (so `cursor_ray` can use `pick_ray`'s neighbors; place them just before `reader_open`, after the existing reader helpers). `reader_is_app` needs `READER_OPEN` (defined at main.c:2793) — already in scope.

```c
/* a pick ray through the OS cursor (app-book mode frees the cursor; pick_ray
   only reads the cursor in ORBIT, so the app book needs its own). */
static Ray cursor_ray(AppState *st, GLFWwindow *w) {
    int    ww, wh;
    float  aspect, nx, ny;
    double mx, my;
    glfwGetWindowSize(w, &ww, &wh);
    aspect = (wh > 0) ? (float)ww / (float)wh : 1.0f;
    glfwGetCursorPos(w, &mx, &my);
    nx = (ww > 0) ? 2.0f * (float)mx / (float)ww - 1.0f : 0.0f;
    ny = (wh > 0) ? 1.0f - 2.0f * (float)my / (float)wh : 0.0f;
    return camera_ray(&st->camera, nx, ny, aspect);
}

/* cursor → the open page's z=0 plane → page-local 2D meters. `in` is true when
   the hit lands within the book's page rect [-wb,wb] x [-zh,zh]. */
static sol_bool page_under_cursor(AppState *st, GLFWwindow *w, mat4 page,
                                  float wb, float zh, float *px, float *py) {
    Ray   r;
    vec3  o, zp, n, hit, loc;
    float t;
    mat4  inv;
    r   = cursor_ray(st, w);
    o   = mat4_mul_point(page, vec3_make(0.0f, 0.0f, 0.0f));
    zp  = mat4_mul_point(page, vec3_make(0.0f, 0.0f, 1.0f));
    n   = vec3_normalize(vec3_sub(zp, o));
    if (!ray_vs_plane(r, o, n, &t)) return SOL_FALSE;
    hit = vec3_add(r.origin, vec3_scale(r.dir, t));
    inv = mat4_inverse(page);
    loc = mat4_mul_point(inv, hit);
    *px = loc.x;
    *py = loc.y;
    return (sol_bool)(loc.x >= -wb && loc.x <= wb &&
                      loc.y >= -zh && loc.y <= zh);
}

/* an app book is OPEN and routes to the widget UI. */
static sol_bool reader_is_app(const AppState *st) {
    return (sol_bool)(st->reader_app != 0 && st->reader_state == READER_OPEN);
}
```

- [ ] **Step 5: Wire `widget.c` and `app_synth.c` into the 4 source lists in `build.sh`**

In each of these four `clang`/source lines, append ` widget.c app_synth.c` to the end of the source file list (they currently end with `... furniture.c inventory.c`):
- `c89check` list (main.c:16)
- `metal` link list (main.c:330)
- `debug` list (main.c:346)
- release/default list (main.c:361)

Example (the c89check line becomes):

```
        -fsyntax-only $GLFW_CFLAGS main.c rhi_gl.c mesh.c flora.c rock.c gothic.c sweep.c texgen.c mesh_gpu.c ui.c text.c wtext.c scene.c mirror.c material.c scene_io.c stml.c nid.c sol_math.c camera.c collide.c bvh.c asset.c component.c particles.c synth.c wav.c mixer.c reverb.c skel.c json.c glb.c fuzzy.c palette.c route.c editor.c descend.c workspace.c furniture.c inventory.c widget.c app_synth.c
```

- [ ] **Step 6: Build both backends (compile-only verification)**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: all three PASS. No behavior change yet — `reader_app` is always 0, the helpers are unused (the compiler may warn "unused function" for `cursor_ray`/`page_under_cursor`/`reader_is_app`; that is fine — Tasks 5-7 wire them. If the build uses `-Werror` on unused-function for these and fails, proceed directly to Tasks 4-7 in one batch before building, or temporarily reference them — but the debug/metal builds here do not use `-Werror`, only `c89check` does, and `-fsyntax-only` does not error on unused static functions).

- [ ] **Step 7: Commit**

```bash
git add main.c build.sh
git commit -m "$(cat <<'EOF'
TODO5 scaffolding: app-book state, page-matrix + cursor-hit helpers

AppState fields for an open app book; reader_page_matrix extracted from the
render block (shared with the input path); cursor_ray / page_under_cursor /
reader_is_app helpers; widget.c + app_synth.c wired into the 4 source lists.
No behavior yet (reader_app stays 0).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: The mint — `cmd_mint_synth` + palette row

**Files:**
- Modify: `main.c` (add `cmd_mint_synth` after `cmd_mint_codex` ~6414; add a registry row in `g_commands[]` ~8023)

- [ ] **Step 1: Add `cmd_mint_synth`**

Immediately after `cmd_mint_codex` closes (main.c:6414) — and before `mint_tag_ws` at 6419 — add:

```c
/* A synth book (TODO5): an ordinary codex tagged as an app. Reuses the whole
   codex mint (cover + page block + workspace tag), then stamps meta["app"] so
   the reader routes it to the widget UI. Unknown apps degrade to plain books. */
static void cmd_mint_synth(AppState *st) {
    sol_u32 h;
    cmd_mint_codex(st);                 /* mints a codex, sets selected_handle, saves */
    h = st->selected_handle;
    if (h != 0) {
        scene_meta_set(&st->scene, h, "name", "synth book");
        scene_meta_set(&st->scene, h, "app", "synth");
        scene_save(&st->scene, "scene.stml");
        printf("synth book minted — read it (look at it, press R) to open the synth\n");
    }
}
```

- [ ] **Step 2: Add the palette row**

In the `g_commands[]` table, immediately after the `"Mint codex (book)"` row (main.c:8023), add (palette-only, no inline key — the command-palette law):

```c
    { "Mint synth book",             "",  0,            cmd_mint_synth,        NULL,                  SOL_FALSE },
```

(Match the column layout of the surrounding rows. `""`/`0` = no hotkey; reachable via the `:` palette. Confirm the struct field order against the adjacent rows — label, key-hint, GLFW key, fn, arg, flag.)

- [ ] **Step 3: Build**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: all PASS.

- [ ] **Step 4: Manual smoke (optional, fast)**

Run `./build.sh debug && ./solarium` (or the run command the repo uses), press `:`, type "synth", run "Mint synth book". A codex appears. (It still opens as an ordinary book until Tasks 5-7 land — that is expected here.) Close the app.

- [ ] **Step 5: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
TODO5 mint: "Mint synth book" palette command

Reuses cmd_mint_codex and stamps meta[app]=synth; the routing seam (next tasks)
keys on it. Palette-only, per the command-registry law.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: Reader identity + patch persistence

**Files:**
- Modify: `main.c` (`reader_open` ~5964; `reader_close` ~5977; two small helpers)

An app book is also a codex (it has a cover), so `reader_open` sets `reader_editable = TRUE`. That would make `reader_is_editing()` true and hijack input into the note-typing path. We must clear `reader_editable` for app books and load the patch; and persist it on close.

- [ ] **Step 1: Add the load/save helpers**

Add these static helpers just above `reader_open` (main.c:5881), after the helpers from Task 3:

```c
/* load the open synth book's patch: start from the "blip" preset, then apply
   the curated-knob overrides stored in meta["synth"] (space-separated floats in
   curated-knob order). Absent/short meta → the bare preset. */
static void synth_book_load(AppState *st, sol_u32 root) {
    const float *pre = synth_preset("blip");
    const char  *m;
    int          i, k = app_synth_knob_count();
    for (i = 0; i < SYNTH_PARAMS; i++)
        st->synth_params[i] = pre ? pre[i] : 0.0f;
    m = scene_meta_get(&st->scene, root, "synth");
    if (m) {
        float v[8];
        int   got = sscanf(m, "%f %f %f %f", &v[0], &v[1], &v[2], &v[3]);
        if (got == k)
            for (i = 0; i < k; i++)
                st->synth_params[app_synth_knob_param(i)] = v[i];
    }
    st->synth_rng = 0x9E3779B9u ^ (sol_u32)root;
}

/* serialize the curated knobs back into meta["synth"] and save the scene. */
static void synth_book_save(AppState *st, sol_u32 root) {
    char buf[128];
    int  i, n = 0, k = app_synth_knob_count();
    for (i = 0; i < k; i++)
        n += snprintf(buf + n, sizeof buf - (size_t)n, (i ? " %.4f" : "%.4f"),
                      (double)st->synth_params[app_synth_knob_param(i)]);
    scene_meta_set(&st->scene, root, "synth", buf);
    scene_save(&st->scene, "scene.stml");
}
```

(The `sscanf`/`snprintf` formats hold exactly `app_synth_knob_count()` (4) floats; if the curated set grows, update both formats. `8` in `v[8]` leaves headroom.)

- [ ] **Step 2: Detect the app in `reader_open`**

In `reader_open`, the editable-vs-content branch ends at main.c:5967 (`reader_load_content(...)` else-arm). Immediately after that `}` (before `st->reader_page = 0;` at 5968), add:

```c
    {
        const char *app = scene_meta_get(s, root, "app");
        st->reader_app = (app && strcmp(app, "synth") == 0) ? 1 : 0;
        if (st->reader_app) {
            st->reader_editable = SOL_FALSE;   /* an app book types nothing */
            synth_book_load(st, root);
        }
    }
```

- [ ] **Step 3: Persist + reset in `reader_close`**

At the very top of `reader_close` (main.c:5977), after the early `return` guard (the `if (... READER_RETURNING) return;` at 5978-5979) and **before** the existing `if (st->reader_editable)` save block at 5980, add:

```c
    if (st->reader_app) {
        synth_book_save(st, st->reader_source);
        st->reader_app = 0;
    }
```

- [ ] **Step 4: Build**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: all PASS. (`reader_app` is now set on open and reset on close; still no widget input/render — that is Tasks 6-7. Opening a synth book here shows a blank book; ESC saves an empty/default `meta["synth"]`.)

- [ ] **Step 5: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
TODO5 reader identity + patch persistence

reader_open detects meta[app]=synth, clears reader_editable (an app book is a
codex but types nothing), and loads the patch from meta[synth] over the blip
preset; reader_close serializes the curated knobs back and resets reader_app.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: Input mode — free the cursor, run the widget core, service actions

**Files:**
- Modify: `main.c` (a `synth_book_input` + `synth_book_play` helper; the cursor toggle ~8121; the modal gate ~8136 and its handler block ~8152)

- [ ] **Step 1: Add the input + play helpers**

Add these static helpers above `reader_open` (with the Task 3/5 helpers). `synth_book_play` uses a file-scope static scratch buffer (single-threaded; avoids bloating AppState):

```c
static void widget_quad_build(AppState *st) {
    MeshBuilder mb;
    mb_init(&mb);
    make_page(&mb, 1.0f, 1.0f);          /* centered unit XY quad, z=0 */
    st->widget_quad = mesh_from_builder(&mb);
    mb_free(&mb);
}

static void synth_book_play(AppState *st) {
    static float buf[SYNTH_RATE];        /* 1 s scratch */
    int n = synth_render(st->synth_params, 1u, buf, SYNTH_RATE);
    if (n > 0) play_oneshot(buf, n, 0.6f, 0.0f);
}

/* run the widget UI for the open synth book: hit-test the cursor against the
   page, lay out the page, service Sound/Roll. The emitted draw-list lives in
   st->widget_ctx for the render pass to walk. */
static void synth_book_input(AppState *st, GLFWwindow *w, sol_bool lmb) {
    mat4        page;
    float       wb, zh, xf, mg, px = 0.0f, py = 0.0f;
    sol_bool    in;
    SynthAction act;
    if (st->widget_quad.index_count == 0) widget_quad_build(st);
    page = reader_page_matrix(st, &wb, &zh, &xf, &mg);
    in   = page_under_cursor(st, w, page, wb, zh, &px, &py);
    widget_begin(&st->widget_ctx, px, py, in, lmb);
    act = app_synth_page(&st->widget_ctx, st->synth_params,
                         xf + mg, zh - mg,
                         (wb - mg) - (xf + mg), 2.0f * (zh - mg));
    widget_end(&st->widget_ctx);
    if (act == SYNTH_ACT_PLAY) {
        synth_book_play(st);
    } else if (act == SYNTH_ACT_ROLL) {
        app_synth_roll(st->synth_params, &st->synth_rng);
        synth_book_play(st);
    }
}
```

- [ ] **Step 2: Free the cursor while an app book is open**

The inventory cursor toggle is at main.c:8121-8128. Immediately after it (after `st->inv_was_open = st->inv_open;` at 8128), add a parallel edge toggle for the app book:

```c
    {
        sol_bool app_now = reader_is_app(st);
        if (app_now && !st->reader_app_was)
            glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        else if (!app_now && st->reader_app_was &&
                 !st->inv_open && !st->editor.active)
            glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        st->reader_app_was = app_now;
    }
```

- [ ] **Step 3: Add the app book to the modal gate + handler**

The modal early-return block begins at main.c:8136:

```c
    if (st->edit_handle != 0 || st->palette.open || reader_is_editing(st) || st->inv_open) {
```

Add `reader_is_app(st)`:

```c
    if (st->edit_handle != 0 || st->palette.open || reader_is_editing(st) ||
        st->inv_open || reader_is_app(st)) {
```

Then add a handler branch. The existing chain inside that block handles `edit_handle` / `reader_is_editing` / `inv_open` (main.c:8144-8181, ending `}` before `st->mouse_last_x = mx;`). Add one more `else if` to that chain, immediately after the `inv_open` branch closes (after its `st->lmb_was_down = lmb;` and `}` at ~8180-8181):

```c
        } else if (reader_is_app(st)) {     /* the synth book's widget UI */
            sol_bool lmb = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            synth_book_input(st, w, lmb);
            st->lmb_was_down = lmb;
        }
```

(This runs every frame the book is open; `synth_book_input` rebuilds `st->widget_ctx` for the render pass. Movement/look stay frozen because the block zeros the look deltas and early-returns, exactly as for inventory.)

- [ ] **Step 4: Build**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: all PASS. (Input now drives the widgets and audio; the page still renders nothing visible — Task 7 draws the draw-list. You can already hear Sound/Roll if you click where the buttons *will* be, but verify visually after Task 7.)

- [ ] **Step 5: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
TODO5 input mode: cursor-over-page widget interaction

Opening a synth book frees the OS cursor (inv_open precedent); a per-frame
synth_book_input hit-tests the cursor against the page, runs the widget core,
and services Sound (synth_render -> play_oneshot) / Roll (randomize + play).
The draw-list is staged in st->widget_ctx for the render pass.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 7: Render — walk the draw-list onto the page

**Files:**
- Modify: `main.c` (the reader render block, ~11797, add the app branch before the image/text branches)

- [ ] **Step 1: Add the app-render branch**

In the reader render block, the page content currently branches `if (state->reader_is_image ...) else if (state->reader_text) else ...` starting at main.c:11797. Add an app branch **first**. Immediately after the `page`/`bm` setup from Task 3 (after the line `bm = mat4_from_trs(...)` you added, before `if (state->reader_is_image && ...)` at 11797), insert:

```c
            if (state->reader_app) {
                int ci;
                for (ci = 0; ci < state->widget_ctx.cmd_count; ci++) {
                    const WidgetCmd *cmd = &state->widget_ctx.cmds[ci];
                    float z = 0.0006f + (float)ci * 0.00004f;  /* paint order via depth */
                    if (cmd->type == WIDGET_CMD_RECT) {
                        vec3     ctr = vec3_make(cmd->x + cmd->w * 0.5f,
                                                 cmd->y - cmd->h * 0.5f, z);
                        mat4     m   = mat4_mul(page,
                                       mat4_from_trs(ctr, quat_identity(),
                                           vec3_make(cmd->w, cmd->h, 1.0f)));
                        Material wm  = material_default();
                        wm.base_color = vec3_make(cmd->r, cmd->g, cmd->b);
                        wm.roughness  = 0.85f;
                        if (state->widget_quad.index_count > 0)
                            draw_mesh(state, state->widget_quad, m,
                                      view, proj, eye, 0.0f, wm);
                    } else {
                        mat4  tm    = mat4_mul(page,
                                      mat4_translate(vec3_make(0.0f, 0.0f, z)));
                        float px2m  = (lh > 0.0f) ? cmd->h / lh : cmd->h;
                        wtext_block(uf, vp, tm, cmd->text, cmd->x, cmd->y,
                                    px2m, 0.0f, cmd->r, cmd->g, cmd->b);
                    }
                }
            } else if (state->reader_is_image && state->reader_image_tex.id) {
```

That is: change the existing `if (state->reader_is_image && state->reader_image_tex.id) {` line into `} else if (state->reader_is_image && state->reader_image_tex.id) {` and prepend the `if (state->reader_app) { ... }` block above it.

(`lh = font_line_height(uf)` is already in scope here — it is computed at main.c:11774. `vp`, `uf`, `view`, `proj`, `eye` are all in scope. The TEXT command's `h` is the desired height in meters; dividing by the font line-height in pixels gives the px-to-m wtext expects. RECT fills sit proud of the page (z ≥ 0.0006) so they don't z-fight the page block; the per-command z step keeps the slider handle above its track and labels above fills.)

- [ ] **Step 2: Build both backends**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: all PASS. **Metal note:** no shader changed (RECT reuses the PBR pipeline via `draw_mesh`, TEXT reuses `wtext`), so the only Metal risk is compilation of the new TUs — which `./build.sh metal` covers.

- [ ] **Step 3: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
TODO5 render: draw the widget draw-list onto the page

The reader walks st->widget_ctx and maps each page-local command to world via
page_m: RECT -> draw_mesh of a lazy unit quad (flat-color, existing lit path,
no new shader); TEXT -> wtext_block. Per-command z step preserves paint order.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 8: Full gauntlet + human live-verify + finish

**Files:** none (verification only)

- [ ] **Step 1: Run the full build gauntlet + both unit tests**

```bash
./build.sh c89check
./build.sh debug
./build.sh metal
./build.sh widgettest && ./widget_test
./build.sh appsynthtest && ./app_synth_test
./build.sh synthtest && ./synth_test     # existing — confirm nothing regressed
```

Expected: `c89check: PASS`, debug + metal link clean, `widget_test: OK`, `app_synth_test: OK`, `synth_test` OK.

- [ ] **Step 2: Human live-verify (subagents cannot GUI-test — hand to Fran)**

Verification checklist, on **both** the GL (`./build.sh debug`) and Metal (`./build.sh metal`) binaries:
1. `:` palette → "Mint synth book" → a book appears.
2. Look at it, press `R` (the read key) → it rises and opens; the cursor frees; the page shows a title, four labeled sliders, and Sound/Roll buttons.
3. Drag a slider → the handle moves live and smoothly.
4. Press **Sound** → a tone plays; change `freq`, press Sound again → the pitch changed.
5. Press **Roll** → the sliders jump to new values and a new sound plays.
6. `Esc` → the book returns; the cursor re-locks to mouselook.
7. Re-open the same book → the sliders are where you left them (patch persisted via `meta["synth"]`).
8. Quit and relaunch → the book still remembers its patch (scene.stml round-trip).
9. Open an ordinary codex (non-app) → it still reads as text (degrade rule intact).

- [ ] **Step 3: Finish the branch**

Announce and use **superpowers:finishing-a-development-branch**. Tests pass (Step 1) and the human verify (Step 2) is green → merge `todo5-synth-book` back to `main` (ff-merge, the project convention), or per Fran's choice. Do **not** stage `NOTES.stml` / `paper-picture.png`.

---

## Plan self-review

**Spec coverage:** widget core (Task 1) ✓; synth app + curated knobs + roll (Task 2) ✓; app identity via `meta["app"]` + degrade (Tasks 4-5) ✓; cursor-over-page input freeing the OS cursor (Task 6) ✓; diegetic page render, no new shader (Task 7) ✓; patch persistence in `meta` not a file (Task 5) ✓; palette mint (Task 4) ✓; build wiring + both unit tests + gauntlet (Tasks 1-3, 8) ✓; explicitly-out items (file-binding, extra widgets, multi-page, cursor sprite) are absent ✓.

**Type consistency:** `WidgetCtx`/`WidgetCmd`/`WIDGET_CMD_RECT`/`WIDGET_CMD_TEXT`, `SynthAction`/`SYNTH_ACT_PLAY`/`SYNTH_ACT_ROLL`, `app_synth_page(ctx, params, x0,y0,w,h)`, `app_synth_roll(params, rng)`, `app_synth_knob_count/_param`, `reader_page_matrix(st, &wb,&zh,&xf,&mg)`, `page_under_cursor`, `reader_is_app`, `synth_book_load/_save/_input/_play`, `widget_quad_build` — all used consistently across tasks. `synth_render(params, seed, out, max)` and `play_oneshot(buf, frames, gain, pan)` match `synth.h` / main.c:3510.

**Known soft spots flagged for the implementer:** (a) confirm the `g_commands[]` row field order against neighbors (Task 4 Step 2); (b) the `sscanf`/`snprintf` formats are hand-tied to 4 curated knobs (Task 5 Step 1) — update both if the set grows; (c) if `./build.sh debug`/`metal` ever enable `-Werror` for unused statics, Tasks 3→7 must land before a clean standalone build of Task 3 (they wire every helper).
