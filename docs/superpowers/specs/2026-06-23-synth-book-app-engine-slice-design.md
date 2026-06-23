# Synth Book — the App-Engine Vertical Slice (TODO5 opener)

**Status:** design approved 2026-06-23, ready for implementation plan.
**Phase:** TODO5 ("apps live in books"), first slice. This does **not** write the full
Phase 5 brief — it builds the thinnest real app that exercises the whole engine, so the
binding brief can be written against working machinery instead of a prospectus.

## Goal

Build one synth book whose single page carries labeled **sliders**, a **"Sound"**
button, and a **"Roll"** button — proving the complete stack end to end:

```
cursor-over-page input → pure widget core → synth schema enumeration → synth_render → mixer
```

When it works, the app engine is real: a second app (settings, palette editor) is new
page-layout code over the same widget core + routing seam, not new engine.

## What already exists (the standing machinery)

The P4 item-8 contract landed intact and makes this buildable:

- **Introspectable schema** — `synth_param_count()`, `synth_param_names()` (synth.c:14-23),
  presets enumerable via `synth_preset_count()` / `synth_preset_name()` / `synth_preset()`.
- **Callable, seed-deterministic renderer** — `synth_render(params, seed, out, max)`
  (synth.c:71): same params + same seed = bit-identical buffer.
- **Mixer playback** — `play_oneshot(buf, frames, gain, pan)` (main.c:3510).
- **The reader rig** is already a view host (main.c:5881 `reader_open`): lift-and-face,
  the flat page matrix `page = bm·translate(0,fy,0)·rotX(-90°)` (main.c:11794) that
  `wtext_block` draws ink into, arrow page-turns, editable-codex mode.
- **Ray → surface → local 2D** is solved every frame for whiteboards: `board_under_ray`
  (main.c:4021) = ray-vs-plane then `scene_world_to_local`, clamped to a rect.
- **The cursor-free modal precedent** — `inv_open` frees the OS cursor and hit-tests
  absolute mouse position (main.c:8152).
- **Immediate-mode philosophy** — `ui.c` (per-frame stream batch, SDF text) is the
  template, though it is screen-space; this slice renders on the page, not over it.
- **The LCG** — `x = x*1664525 + 1013904223` (synth.c:9, "meadow, codex, particles,
  now sound") is the project's standard PRNG; "Roll" reuses it.

## Scope

**In:**
- A `widget` core: `button`, `slider`, `label` (immediate-mode, pure, coordinate-agnostic).
- A `synth` app: one page of sliders over curated schema params + "Sound" + "Roll".
- Reader app-routing seam keyed on `meta["app"]`.
- Cursor-over-page input mode (OS cursor; ray through it to page-local 2D).
- Patch persistence in the book object's `meta`.
- A palette mint that creates a synth book.

**Out (explicitly deferred):**
- Book-binds-a-file / a patch-library document (TODO5's deepest piece; waits for the
  fs-tree file-ownership story).
- Checkbox / knob / list / text-field widgets.
- Multi-page panels (the seam supports them; the slice ships one page).
- A diegetic on-page cursor sprite (OS cursor for now — the agreed option (a)).
- Any non-synth app.

## Architecture — three isolated pieces + one integration site

| Unit | Files | Purity | Depends on |
|------|-------|--------|------------|
| Widget core | `widget.c` / `widget.h` + `widget_test.c` | **pure** (no GL/scene/synth) | `sol_base` |
| Synth app | `app_synth.c` / `app_synth.h` + `app_synth_test.c` | **near-pure** (no GL/scene) | `widget`, `synth` |
| Reader integration | `main.c` (existing) | impure (the one glue site) | everything |

The keystone (widget core) and the app logic are pure and unit-tested, exactly like
`inventory.c` / `route.c` / `furniture.c`. Only `main.c` touches the renderer, the
scene, and input — it owns the page-plane geometry, the cursor ray, the draw-list walk,
the input mode, and the mint.

This is **Approach A**: the widget core emits geometry as a page-local **draw-list**; the
reader is the single renderer that maps page-local → world through `page_m`. The same
core could later drive a screen-space painter (the TODO5 "geometry-agnostic" promise).

## Component 1 — the widget core (`widget.h`)

```c
#define WIDGET_MAX_CMDS 128

typedef enum { WIDGET_CMD_RECT, WIDGET_CMD_TEXT } WidgetCmdType;

typedef struct {
    WidgetCmdType type;
    float x, y, w, h;            /* page-local meters; TEXT uses x,y + h as size */
    float r, g, b, a;
    const char *text;           /* TEXT only; borrowed, valid until widget_end */
} WidgetCmd;

typedef struct {
    /* input for this frame (page-local 2D meters + button bits) */
    float    ptr_x, ptr_y;
    sol_bool ptr_in;            /* cursor is over the page at all */
    sol_bool down, down_prev;   /* left mouse this frame / last frame */
    /* persistent imgui interaction state (survives across frames) */
    int      hot_id, active_id;
    /* output */
    WidgetCmd cmds[WIDGET_MAX_CMDS];
    int       cmd_count;
} WidgetCtx;

void     widget_begin(WidgetCtx *c, float ptr_x, float ptr_y,
                      sol_bool ptr_in, sol_bool down);
sol_bool widget_button(WidgetCtx *c, int id, float x, float y, float w, float h,
                       const char *label);
sol_bool widget_slider(WidgetCtx *c, int id, float x, float y, float w, float h,
                       float *value, float lo, float hi);
void     widget_label (WidgetCtx *c, float x, float y, const char *text, float size);
void     widget_end(WidgetCtx *c);
```

**Interaction model (classic imgui hot/active):**
- `widget_begin` stores this frame's input, clears `cmd_count`, resets `hot_id = 0`.
- A widget computes hover (`ptr_in` && point-in-rect) → sets `hot_id`. On a fresh press
  over a hot widget, `active_id = id`. A button **fires** on release while still hot. A
  slider, while `active_id == id`, maps `ptr_x` across `[x, x+w]` to `[lo, hi]` and writes
  `*value` — it keeps tracking even when the cursor leaves the rect (that is the whole
  point of retained `active_id`). Release clears `active_id`.
- Each widget appends its geometry to `cmds` (track rect + handle rect + label TEXT for a
  slider; box + centered label for a button; one TEXT for a label). Colors shift on
  hot/active so feedback is visible.
- `id` is a caller-assigned stable integer (the slice hand-assigns 1..N).

**Testability (`widget_test.c`, no GL):** drive `widget_begin`/widget call/`widget_end`
across synthetic frames and assert (a) returns — a press-then-release over a button
fires exactly once; a drag moves a slider's `*value` to the expected fraction; a drag
that starts off-widget does nothing — and (b) emitted geometry — a slider emits a handle
rect whose x tracks `*value`. Pure C, the project pattern.

## Component 2 — the synth app (`app_synth.h`)

```c
/* curated slider set for the slice: schema index + display range.
   (the full 20-param schema is enumerable; the slice exposes an expressive few) */
typedef struct { int param; float lo, hi; } SynthKnob;

/* lays out the synth page into `ctx` over `params` (a live SYNTH_PARAMS array);
   returns an action for main.c to act on (render/play lives in main.c, not here, so
   this stays GL/scene/mixer-free and testable). */
typedef enum { SYNTH_ACT_NONE, SYNTH_ACT_PLAY, SYNTH_ACT_ROLL } SynthAction;

SynthAction app_synth_page(WidgetCtx *ctx, float *params);

/* "Roll": randomize the curated knobs in place using the engine LCG. */
void app_synth_roll(float *params, sol_u32 *rng);
```

- **Curated knobs** (continuous, expressive; named from `synth_param_names()` for labels):
  `freq` (idx 5, 80–2000 Hz), `sustain` (idx 2, 0–1.0 s), `decay` (idx 4, 0–1.0 s),
  `duty` (idx 12, 0–1.0). Each is a labeled slider.
- **Layout** lives on the **right page** (page-local x ∈ `[xf, wb]`, the rect the
  image-book layout already uses) — a title label, the four sliders stacked, then the two
  buttons. The left page is left blank for the slice.
- `app_synth_page` calls `widget_label` / `widget_slider` / `widget_button` and returns
  `SYNTH_ACT_PLAY` when "Sound" fires, `SYNTH_ACT_ROLL` when "Roll" fires, else
  `SYNTH_ACT_NONE`. It does **not** synthesize or play — that is main.c's job, keeping
  this unit free of the mixer and GL.
- `app_synth_roll` randomizes the four curated indices within their ranges via the
  shared LCG, advancing `*rng`.

**Testability (`app_synth_test.c`):** assert the page emits a label + 4 sliders + 2
buttons; a synthetic press on the Sound rect returns `SYNTH_ACT_PLAY`; `app_synth_roll`
moves the curated params and leaves the others untouched; same `rng` seed → same roll.

## Component 3 — reader integration (`main.c`, the glue)

### 3a. App identity + the routing seam
- New `AppState` field `int reader_app` (0 = none / ordinary book; 1 = synth). Set in
  `reader_open` (main.c:5881): after resolving `root`, read `meta["app"]` off the root;
  `"synth"` → `reader_app = 1`, else `0`. Unknown/absent → ordinary book (degrade rule).
- New `AppState` fields for the open app's live state: `float synth_params[SYNTH_PARAMS]`,
  `sol_u32 synth_rng`, `sol_u32 synth_seed`, `WidgetCtx widget_ctx`.
- On open of a synth book: load `meta["synth"]` overrides merged against the `"blip"`
  preset into `synth_params` (absent meta → the bare preset); seed `synth_rng` from a
  fixed constant XOR the object handle (deterministic, varies per book).

### 3b. Page plane + cursor → page-local
- The render block already builds `page` (main.c:11794). Compute the page plane once per
  frame while open: `origin = page·(0,0,0)`, `normal = page·(0,1,0) − origin`.
- New helper `page_under_cursor(st, &px, &py, &in)`: build the pick ray through the **OS
  cursor** position (not screen center — the cursor is free in this mode), ray-vs-plane
  against the page, transform the hit by `inverse(page)` → page-local `(px, py)`; `in` =
  hit within the page rect. This generalizes `board_under_ray` from a scene object to the
  transient `page` matrix.

### 3c. Rendering widgets onto the page (no new shader)
- When `reader_app == 1`, the reader skips `reader_draw_page` and instead:
  1. `widget_begin(&st->widget_ctx, px, py, in, lmb_down)`
  2. `act = app_synth_page(&st->widget_ctx, st->synth_params)`
  3. `widget_end(...)`
  4. Walk `widget_ctx.cmds`: **RECT** → `draw_mesh` of a shared unit quad with a
     flat-color `Material` (base_color = cmd color, high roughness), model =
     `page · translate(x,y,ε) · scale(w,h,1)`; **TEXT** → `wtext_block(reader_font, vp,
     page, text, x, y, size, …)`. RECT uses the **existing lit albedo path** — no new
     shader, **no MSL twin** (the zero-Metal-debt promise). Widgets read as subtly lit
     colored paper, consistent with everything else on the page.
- Act on `act` (this is where the mixer lives, keeping app_synth pure):
  - `SYNTH_ACT_PLAY` → `n = synth_render(synth_params, synth_seed, buf, CAP)` then
    `play_oneshot(buf, n, gain, 0)`.
  - `SYNTH_ACT_ROLL` → `app_synth_roll(synth_params, &synth_rng)` then render + play
    (Roll auditions the new sound; a button press is a legitimate render point).
  - A scratch render buffer (`float buf[SYNTH_RATE]` ≈ 1 s) lives in `AppState`.

### 3d. Input mode
- While a synth book is open, free the OS cursor exactly as `inv_open` does (main.c:8152
  block) and route left-click/drag to the widget core. **Left-click no longer closes the
  book** (the current reader reflex at main.c:8315) when `reader_app != 0`.
- **ESC closes** the book; on close, serialize `synth_params` (as a compact override
  string vs. the `"blip"` preset, only the changed knobs) into `meta["synth"]` so the
  patch persists in `scene.stml`. (Optional nicety: a click landing **off** the page may
  also close, preserving the click-away reflex — decide during build.)
- `reader_app` resets to 0 in `reader_close`.

### 3e. The mint
- `cmd_mint_synth(AppState*)` modeled on `cmd_mint_codex` (main.c:6342): mint a codex
  book, then tag the root `meta["app"] = "synth"` and the active workspace
  (`mint_tag_ws`). Registry row `{ "Mint synth book", … cmd_mint_synth … }` so it is
  palette-reachable (no new inline keybind — the command-palette law).

## Data flow

```
mint ───────────────► a codex book, meta[app]=synth, in the active workspace
select + read ──────► reader_open: reader_app=1, load meta[synth]∙blip → synth_params,
                        free cursor (inv_open precedent)
each frame (open) ──► page_under_cursor → (px,py,in)
                        widget_begin → app_synth_page → widget_end
                        walk draw-list onto page (draw_mesh quads + wtext)
drag a slider ──────► widget_slider writes synth_params[idx]; handle moves live (visual)
press "Sound" ──────► synth_render(synth_params, seed) → play_oneshot   (audio re-render
press "Roll" ───────► app_synth_roll → synth_render → play_oneshot       only on press)
ESC ────────────────► serialize synth_params → meta[synth]; reader_close; reader_app=0
reload ─────────────► meta[synth] restores the patch
```

**Audio vs. visual render:** the frame (and the slider handle) redraws every frame from
`synth_params`, so dragging always looks live. `synth_render` (the costly offline
synthesis) runs only on a button press — never per intermediate slider value. This
matches the sfxr instrument: a one-shot triggered sound has no held note to bend, so
tweak → audition is the natural loop.

## Error handling / degrade rules

- Unknown or absent `meta["app"]` → ordinary book (no app path taken).
- A malformed `meta["synth"]` override string → fall back to the bare `"blip"` preset
  (per-knob parse, skip unparseable tokens).
- `page_under_cursor` miss (cursor off the page or ray parallel) → `in = false`; the
  widget core sees no hover/press; nothing fires.
- `synth_render` returning 0 frames → `play_oneshot` is a no-op (guarded).

## Testing & build gauntlet

- `widget_test.c` — interaction + emitted geometry (above).
- `app_synth_test.c` — page layout, action returns, roll determinism (above).
- `build.sh`: add `widget.c` / `app_synth.c` to the 4 source lists and `widgettest` /
  `appsynthtest` targets.
- Full gauntlet before any commit: `./build.sh c89check && ./build.sh debug &&
  ./build.sh metal && ./build.sh widgettest && ./build.sh appsynthtest` (plus the
  existing `synthtest`). **No shader change**, so the Metal risk is only that the new TUs
  compile under `build.sh metal`.
- Human live-verify on both backends (subagents cannot GUI-test): mint a synth book,
  read it, drag a slider (handle moves), press Sound (hear it), press Roll (new sound),
  ESC, reload, confirm the patch persisted.

## Constraints honored

- **RHI seam inviolable** — no GL/Metal outside `rhi_gl.c`/`rhi_metal.m`; widget rects
  reuse `draw_mesh`'s existing pipeline. **No new shader → no MSL twin.**
- **Strict C89** — decls at top of block, `/* */` comments, no `//`, no mixed
  decl/statement, no VLAs, `fabs((double)x)`, `snprintf`/`strncpy`.
- **Pure-module discipline** — widget + synth-app are GL/scene-free with `*_test.c`.
- **Command-palette law** — the mint is a registry row, not a new inline keybind.
- **Persistence is ordinary meta** — `meta["synth"]` is object state like a note's
  `text_size`; this is NOT the deferred book-binds-a-file.
- **Never commit** `NOTES.stml` / `paper-picture.png`; commits end with the
  `Co-Authored-By: Claude Opus 4.8 (1M context)` line.

## File structure

```
widget.h        new — WidgetCtx, WidgetCmd, the 5 entry points
widget.c        new — pure immediate-mode core (~250 lines)
widget_test.c   new — interaction + geometry assertions
app_synth.h     new — SynthKnob, SynthAction, app_synth_page / app_synth_roll
app_synth.c     new — page layout over the curated knobs (~120 lines)
app_synth_test.c new — layout / action / roll-determinism assertions
main.c          modify — reader_app field + state; reader_open identity + load;
                page_under_cursor; the app render/act branch; input mode; cmd_mint_synth
build.sh        modify — 4 source lists + 2 test targets
```
