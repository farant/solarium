# Widget Batch 1 — Radio, Checkbox, Stepper Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add three pointer-driven widgets (checkbox, radio/segmented, stepper) to the immediate-mode core and wire them into the synth book (wave→radio, crush→stepper, low-pass→checkbox).

**Architecture:** Three new functions in the pure `widget.c` core, each a composition of the existing filled-RECT + TEXT commands and the existing hot/active click logic — single-id, position-based for multi-target controls. Then `app_synth.c`'s page is reorganized to lay them out and bind them to synth params. **No `main.c` changes, no shader, no MSL twin** — the widgets emit only existing command types and take only pointer input.

**Tech Stack:** Strict C89 (`-std=c89 -pedantic-errors -Werror -Wall -Wextra`), the existing `widget.*` / `app_synth.*` modules + their `*_test.c` (already wired into `build.sh` as `widgettest`/`appsynthtest` and into the c89check/debug/metal source lists — **no build.sh changes needed**).

**Branch:** `widget-batch-1` (create at start; ff-merge to `main` at the end).

**Spec:** `docs/superpowers/specs/2026-06-23-widget-batch-1-radio-checkbox-stepper-design.md`

**C89 reminders:** declarations at the top of every block (no mixed decl/statement); `/* */` only; no VLAs; no `//`. The `widgettest`/`appsynthtest` targets compile `-pedantic-errors -Werror`, and c89check adds `-Wall -Wextra`.

**Never commit** `NOTES.stml` / `paper-picture.png`. Commit messages end with:
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

**Stepper signature note (refinement over the spec):** the spec sketched `widget_stepper(c,id,x,y,w,h,value,lo,hi)`. The real signature adds a `const char *value_text` the caller supplies, because `WidgetCmd.text` is a borrowed pointer that must outlive the render walk — the core cannot safely format/own a transient int string. The caller passes a stable label (for `crush`, a literal table). This still delivers the spec's `[- value +]` control.

---

## File Structure

| File | Change |
|------|--------|
| `widget.h` | + 3 declarations (checkbox, radio, stepper) with behaviour comments |
| `widget.c` | + 3 implementations (~95 lines), reusing `point_in`/`push_rect`/`push_text` |
| `widget_test.c` | + checkbox / radio / stepper cases |
| `app_synth.c` | reorganized `app_synth_page` (wave radio, crush stepper, low-pass checkbox) + row factor 0.12→0.09 |
| `app_synth_test.c` | updated layout counts + wave/crush/lpcut click assertions |

---

### Task 0: Branch

- [ ] **Step 1: Create the branch**

```bash
cd /Users/francisarant/Documents/projects/solarium
git checkout -b widget-batch-1
```

---

### Task 1: The three widgets (pure core)

**Files:** Modify `widget.h`, `widget.c`, `widget_test.c`.

The existing `widget.c` provides the helpers these reuse: `point_in(px,py,x,y,w,h)` (returns `px>=x && px<=x+w && py<=y && py>=y-h`), `push_rect(c,x,y,w,h,r,g,b)`, `push_text(c,x,y,text,size,r,g,b)`, and the hot/active pattern in `widget_button`/`widget_slider`. Coordinate convention: page-local meters, y-up, `(x,y)`=top-left.

- [ ] **Step 1: Add the three declarations to `widget.h`**

Insert after the `widget_label` declaration (currently widget.h:44-45), before `void widget_end`:

```c
/* toggles *value on a press+release over the box (a square of side `size` at
   top-left (x,y)); `label` is drawn to its right and is non-interactive.
   returns SOL_TRUE on the one frame it flips. */
sol_bool widget_checkbox(WidgetCtx *c, int id, float x, float y, float size,
                         sol_bool *value, const char *label);
/* a horizontal segmented bar of `count` labelled cells filling (w,h) from
   top-left (x,y); the cell == value is highlit. A press+release inside selects
   the cell under the release point. returns the selected index. */
int      widget_radio(WidgetCtx *c, int id, float x, float y, float w, float h,
                      const char *const *labels, int count, int value);
/* a [ - value + ] stepper filling (w,h): minus button | value_text | plus
   button. A press+release over the minus/plus end yields value-/+1, clamped to
   [lo,hi]. `value_text` is drawn in the middle (caller-owned, must outlive the
   render walk). returns the new value. */
int      widget_stepper(WidgetCtx *c, int id, float x, float y, float w, float h,
                        const char *value_text, int value, int lo, int hi);
```

- [ ] **Step 2: Write the failing tests in `widget_test.c`**

Add these blocks inside `main()`, immediately before the final `if (fails == 0) printf("widget_test: OK\n");` line:

```c
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
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `./build.sh widgettest`
Expected: FAIL (link/compile errors — `widget_checkbox`/`widget_radio`/`widget_stepper` undefined).

- [ ] **Step 4: Implement the three widgets in `widget.c`**

Insert these three functions after `widget_slider` (currently ends widget.c:96) and before `widget_label` (widget.c:98):

```c
sol_bool widget_checkbox(WidgetCtx *c, int id, float x, float y, float size,
                         sol_bool *value, const char *label) {
    int      hover;
    sol_bool fired = SOL_FALSE;
    float    lum, inset;
    if (id == 0) return SOL_FALSE;           /* 0 is the idle sentinel */
    hover = c->ptr_in && point_in(c->ptr_x, c->ptr_y, x, y, size, size);
    if (hover) c->hot_id = id;
    if (c->active_id == id) {
        if (!c->down) {                      /* release */
            if (hover) { *value = (sol_bool)!*value; fired = SOL_TRUE; }
            c->active_id = 0;
        }
    } else if (hover && c->down && !c->down_prev) {
        c->active_id = id;
    }
    lum = (c->active_id == id) ? 0.30f : (hover ? 0.55f : 0.42f);
    push_rect(c, x, y, size, size, lum, lum, lum * 1.05f);   /* the box */
    if (*value) {                                            /* the check */
        inset = size * 0.22f;
        push_rect(c, x + inset, y - inset,
                  size - 2.0f * inset, size - 2.0f * inset, 0.85f, 0.62f, 0.30f);
    }
    push_text(c, x + size * 1.25f, y - size * 0.10f, label, size * 0.85f,
              0.13f, 0.10f, 0.08f);                          /* label, ink */
    return fired;
}

int widget_radio(WidgetCtx *c, int id, float x, float y, float w, float h,
                 const char *const *labels, int count, int value) {
    int   i, sel = value, hovcell = -1;
    float cw;
    if (id == 0 || count <= 0) return value;
    cw = w / (float)count;
    if (c->ptr_in && point_in(c->ptr_x, c->ptr_y, x, y, w, h)) {
        hovcell = (int)((c->ptr_x - x) / cw);
        if (hovcell < 0) hovcell = 0;
        if (hovcell >= count) hovcell = count - 1;
        c->hot_id = id;
    }
    if (c->active_id == id) {
        if (!c->down) {                      /* release in the bar selects */
            if (hovcell >= 0) sel = hovcell;
            c->active_id = 0;
        }
    } else if (hovcell >= 0 && c->down && !c->down_prev) {
        c->active_id = id;
    }
    for (i = 0; i < count; i++) {
        float cx = x + (float)i * cw;
        if (i == sel)
            push_rect(c, cx, y, cw, h, 0.80f, 0.62f, 0.30f);   /* selected: amber */
        else if (i == hovcell)
            push_rect(c, cx, y, cw, h, 0.55f, 0.55f, 0.58f);   /* hovered */
        else
            push_rect(c, cx, y, cw, h, 0.40f, 0.40f, 0.42f);   /* idle */
        push_text(c, cx + cw * 0.12f, y - h * 0.18f, labels[i], h * 0.50f,
                  0.96f, 0.94f, 0.88f);
    }
    return sel;
}

int widget_stepper(WidgetCtx *c, int id, float x, float y, float w, float h,
                   const char *value_text, int value, int lo, int hi) {
    int   v = value, hover, rel_minus, rel_plus;
    float mw = w * 0.28f;
    if (id == 0) return value;
    hover     = c->ptr_in && point_in(c->ptr_x, c->ptr_y, x, y, w, h);
    rel_minus = c->ptr_in && point_in(c->ptr_x, c->ptr_y, x, y, mw, h);
    rel_plus  = c->ptr_in && point_in(c->ptr_x, c->ptr_y, x + w - mw, y, mw, h);
    if (hover) c->hot_id = id;
    if (c->active_id == id) {
        if (!c->down) {                      /* release: which end? */
            if (rel_minus)     v = value - 1;
            else if (rel_plus) v = value + 1;
            if (v < lo) v = lo;
            if (v > hi) v = hi;
            c->active_id = 0;
        }
    } else if (hover && c->down && !c->down_prev) {
        c->active_id = id;
    }
    push_rect(c, x, y, mw, h,
              rel_minus ? 0.55f : 0.42f, rel_minus ? 0.55f : 0.42f, 0.45f);
    push_text(c, x + mw * 0.30f, y - h * 0.18f, "-", h * 0.55f, 0.96f, 0.94f, 0.88f);
    push_text(c, x + mw + (w - 2.0f * mw) * 0.18f, y - h * 0.18f, value_text,
              h * 0.50f, 0.13f, 0.10f, 0.08f);
    push_rect(c, x + w - mw, y, mw, h,
              rel_plus ? 0.55f : 0.42f, rel_plus ? 0.55f : 0.42f, 0.45f);
    push_text(c, x + w - mw + mw * 0.30f, y - h * 0.18f, "+", h * 0.55f,
              0.96f, 0.94f, 0.88f);
    return v;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `./build.sh widgettest && ./widget_test`
Expected: `widget_test: OK` (exit 0, no ASan/UBSan output).

- [ ] **Step 6: Verify the core stays `-Wall -Wextra` clean**

Run: `./build.sh c89check`
Expected: `c89check: PASS` (widget.c is in the c89check list, compiled `-Wall -Wextra -Werror`).

- [ ] **Step 7: Commit**

```bash
git add widget.h widget.c widget_test.c
git commit -m "$(cat <<'EOF'
Widget batch 1: checkbox, radio, stepper

Three pointer-driven widgets on the existing RECT+TEXT draw-list + hot/active
logic. Single-id; radio/stepper pick the cell/arrow by release position. The
stepper takes a caller-owned value_text (WidgetCmd.text must outlive the walk).
No new cmd type, no keyboard focus.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Wire them into the synth page

**Files:** Modify `app_synth.c`, `app_synth_test.c`.

- [ ] **Step 1: Replace `app_synth_page` in `app_synth.c`**

Replace the entire current `app_synth_page` function (app_synth.c:35-71) with:

```c
SynthAction app_synth_page(WidgetCtx *ctx, float *params,
                           float x0, float y0, float w, float h) {
    static const char *const WAVE_NAMES[5] = { "SQ", "SW", "TR", "SI", "NZ" };
    static const char *const CRUSH_LABELS[17] = {
        "off", "1", "2", "3", "4", "5", "6", "7", "8",
        "9", "10", "11", "12", "13", "14", "15", "16"
    };
    const char *const *names = synth_param_names();
    SynthAction act = SYNTH_ACT_NONE;
    float    row, y, labw, sldw, lab_sz, btn_w, btn_h;
    int      i, id, wv, cv;
    sol_bool lp;

    /* the page consumes ~10.45*row vertically (title 1.5 + wave 1.25 + 4*1.25
       + compact 1.25 + 0.4 gap + 1.05 btn); keep row <= h/10.45 (~0.095*h). */
    row = h * 0.09f;
    if (row > 0.055f) row = 0.055f;
    lab_sz = row * 0.40f;
    labw   = w * 0.36f;
    sldw   = w - labw;
    y      = y0;
    id     = 1;

    widget_label(ctx, x0, y, "synth", row * 0.55f);
    y -= row * 1.5f;

    /* wave: a 5-cell radio over schema index 0 */
    wv = (int)params[0];
    wv = widget_radio(ctx, id++, x0, y, w, row * 0.9f, WAVE_NAMES, 5, wv);
    params[0] = (float)wv;
    y -= row * 1.25f;

    for (i = 0; i < KNOB_COUNT; i++) {
        widget_label(ctx, x0, y - (row - lab_sz) * 0.5f,
                     names[KNOBS[i].param], lab_sz);
        widget_slider(ctx, id++, x0 + labw, y, sldw, row * 0.72f,
                      &params[KNOBS[i].param], KNOBS[i].lo, KNOBS[i].hi);
        y -= row * 1.25f;
    }

    /* compact row: crush stepper (left) + low-pass checkbox (right) */
    cv = (int)params[16];
    if (cv < 0)  cv = 0;
    if (cv > 16) cv = 16;
    cv = widget_stepper(ctx, id++, x0, y, w * 0.45f, row * 0.9f,
                        CRUSH_LABELS[cv], cv, 0, 16);
    params[16] = (float)cv;
    lp = (sol_bool)(params[14] > 0.0f);
    if (widget_checkbox(ctx, id++, x0 + w * 0.55f, y, row * 0.6f, &lp, "low-pass"))
        params[14] = lp ? 2000.0f : 0.0f;
    y -= row * 1.25f;

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

(`app_synth_roll`, `KNOBS`, `app_synth_knob_count`/`_param`, and `rnd01` are UNCHANGED. Roll still randomizes only the four slider knobs; wave/crush/lpcut keep their values through a roll.)

- [ ] **Step 2: Update the layout test in `app_synth_test.c`**

Replace the first test block (app_synth_test.c:10-25, the `/* the page emits ... */` block) with this label-presence + rect-count version:

```c
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
```

- [ ] **Step 3: Add the three click-binding tests in `app_synth_test.c`**

Insert these three blocks immediately AFTER the existing "Sound returns PLAY" block (after app_synth_test.c:49, before the roll test):

```c
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
```

- [ ] **Step 4: Run the app_synth test**

Run: `./build.sh appsynthtest && ./app_synth_test`
Expected: `app_synth_test: OK`. If the layout-count `CHECK` fails, the rect count printed tells you which widget emitted an unexpected number — reconcile against the Task 1 emission (radio=count rects, stepper=2 rects, checkbox-off=1 rect) rather than loosening the assertion.

- [ ] **Step 5: Full gauntlet (both backends)**

Run: `./build.sh c89check && ./build.sh widgettest && ./widget_test && ./build.sh appsynthtest && ./app_synth_test && ./build.sh debug && ./build.sh metal`
Expected: c89check PASS, both tests OK, debug + metal build clean. **No shader changed** → the Metal risk is only compilation, which `./build.sh metal` covers.

- [ ] **Step 6: Commit**

```bash
git add app_synth.c app_synth_test.c
git commit -m "$(cat <<'EOF'
Widget batch 1: wire wave/crush/low-pass into the synth page

wave -> radio (SQ/SW/TR/SI/NZ), crush -> stepper (0..16, "off"+literals), low-
pass -> checkbox (toggles lpcut 0<->2000). Row factor 0.12->0.09 for the taller
budget; app_synth_roll unchanged. No main.c change, no shader.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Gauntlet, live-verify, finish

**Files:** none (verification only).

- [ ] **Step 1: Full build gauntlet + all tests**

```bash
./build.sh c89check
./build.sh widgettest && ./widget_test
./build.sh appsynthtest && ./app_synth_test
./build.sh synthtest && ./synth_test     # regression
./build.sh debug
./build.sh metal
```
Expected: c89check PASS; widget_test / app_synth_test / synth_test OK; debug + metal clean.

- [ ] **Step 2: Human live-verify (hand to Fran — subagents cannot GUI-test)**

On both the GL (`./solarium`) and Metal (`./solarium-metal`) binaries: mint + open a synth book and confirm —
1. the page now shows a **wave** segmented bar, the four sliders, a **crush [- N +]** stepper + a **low-pass** checkbox on one row, then Sound/Roll;
2. clicking a wave cell highlights it; pressing **Sound** plays the new timbre (square vs noise is obvious);
3. the crush − / + steps the bit-depth (audible crunch) and clamps at off / 16;
4. the low-pass checkbox toggles (audible high-end change on Sound);
5. Esc, reopen → the radio/stepper/checkbox states persist with the rest of the patch (they ride `meta["synth"]`? NO — only the 4 curated knobs persist; wave/crush/lpcut reset to the preset on reload. Flag to Fran: persisting these is a small follow-on if wanted — extend `synth_book_load`/`save` to include indices 0/14/16).
6. everything still legible / not clipping the page.

- [ ] **Step 3: Finish the branch**

Use **superpowers:finishing-a-development-branch**. Tests pass (Step 1) + human verify (Step 2) green → ff-merge `widget-batch-1` to `main` (the project convention), or per Fran's choice. Do NOT stage `NOTES.stml` / `paper-picture.png`.

---

## Plan self-review

**Spec coverage:** checkbox/radio/stepper widgets (Task 1) ✓; single-id position-based radio/stepper ✓; synth wiring wave→radio / crush→stepper / lpcut→checkbox-toggle-to-default (Task 2) ✓; one-page reorganized layout + row factor 0.12→0.09 ✓; no main.c/shader/MSL ✓ (Tasks touch only widget.*/app_synth.*); widget_test + app_synth_test updates ✓; gauntlet + live-verify (Task 3) ✓.

**Deviations flagged:** (1) `widget_stepper` gains a `value_text` param vs the spec sketch — justified in the header (borrowed-pointer lifetime; the core can't own a formatted int). (2) The spec didn't address persistence of wave/crush/lpcut; Task 3 Step 2 flags that they do NOT persist (only the 4 curated knobs do) as a known, optional follow-on — not in scope here.

**Type consistency:** `widget_checkbox`/`widget_radio`/`widget_stepper` signatures are identical in widget.h (Task 1 Step 1), widget.c (Step 4), widget_test.c (Step 2), and the app_synth call sites (Task 2). `WAVE_NAMES`(5), `CRUSH_LABELS`(17), schema indices 0/14/16, and the rect-count formula (`knob_count*2 + 5 + 2 + 1 + 2`) all match the emission defined in Task 1.

**Placeholder scan:** none — every step has complete code and exact commands.
