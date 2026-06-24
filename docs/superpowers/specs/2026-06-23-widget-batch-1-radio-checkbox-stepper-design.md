# Widget Batch 1 — Radio, Checkbox, Stepper (TODO5 app engine)

**Status:** design approved 2026-06-23, ready for implementation plan.
**Phase:** TODO5 ("apps live in books"), the second slice — broadening the widget core after the synth-book vertical slice ([[phase5-app-engine-brief]]).

## Goal

Add three pointer-driven widgets to the immediate-mode core — **checkbox**, **radio (segmented)**, **stepper** — and wire them into the synth book as the testbed: a **wave** selector (radio), a **crush** bit-depth (stepper), and a **low-pass** on/off (checkbox). This turns the synth page from sliders-only into a representative mixed-control panel and grows the reusable widget vocabulary the future apps (settings, material editor, sequencer) need.

## Scope

**In:** three new `widget.*` functions + their unit tests; the synth page reorganized in `app_synth.c` to add the three controls + its unit test updated.

**Out / unchanged:** **no `main.c` changes, no shader, no MSL twin** — the three widgets emit only the existing `WIDGET_CMD_RECT` / `WIDGET_CMD_TEXT` commands and take only pointer input, so the reader's draw-list render walk and the `synth_book_input` path are untouched. Also out: text input / keyboard-focus widgets (a separate later batch); the 2D color square / knob / XY pad (wait on a gradient draw-primitive decision); multi-page pagination (the page stays one panel).

## Why these are cheap (the key property)

The current core emits flat RECT + TEXT and is pointer-only. Checkbox, radio, and stepper are all **compositions of filled rects + text + the existing hot/active click logic** — no new command type, no keyboard focus. So they are pure additions to `widget.c` (3 functions + tests) and `app_synth.c` (layout + wiring + test). The 655 KB `main.c` is not opened.

## Component 1 — the three widgets (`widget.h` / `widget.c`)

All three are **single-id** (one `WidgetCtx` interaction id each, like `button`/`slider`), keeping `app_synth`'s uniform `id++` per widget. Radio and stepper achieve multiple click targets **position-based** (the control becomes active on a press anywhere inside it; the *release position* selects the cell / arrow), rather than burdening the caller with sub-id ranges.

```c
/* toggles *value on a click that presses and releases over the box; the box is
   a square of side `size` at top-left (x,y), with `label` drawn to its right.
   returns SOL_TRUE on the one frame it flips. */
sol_bool widget_checkbox(WidgetCtx *c, int id, float x, float y, float size,
                         sol_bool *value, const char *label);

/* a horizontal segmented bar of `count` labelled cells filling (w,h) from
   top-left (x,y); the cell whose index == value is highlighted. A press+release
   inside the bar selects the cell under the release point. returns the selected
   index (unchanged `value` if no selection happened this frame). */
int widget_radio(WidgetCtx *c, int id, float x, float y, float w, float h,
                 const char *const *labels, int count, int value);

/* a [ - value + ] stepper filling (w,h): a left minus button, the value in the
   middle, a right plus button. A press+release over the minus end yields
   value-1, over the plus end value+1, clamped to [lo,hi]. returns the new value
   (unchanged if neither end was clicked). */
int widget_stepper(WidgetCtx *c, int id, float x, float y, float w, float h,
                   int value, int lo, int hi);
```

**Behaviour details (so the implementation + tests are unambiguous):**
- **Reserved id:** as with the existing widgets, `id == 0` returns the no-op default (`SOL_FALSE` / `value`) — 0 is the idle sentinel.
- **Checkbox:** clickable region = the `size`×`size` box only (label is non-interactive). Fires on the same press-then-release-while-hovered edge the button uses (reuse the hot/active pattern). Emits: a box rect (always); a smaller inset fill rect when `*value` is true (the "check"); the label TEXT to the right. Toggles `*value` and returns `SOL_TRUE` only on the firing frame.
- **Radio:** cell `i` occupies `x + i*(w/count) .. x + (i+1)*(w/count)`. The hovered cell brightens; the selected cell (`== value`) is highlit. On a press+release inside the bar, the chosen index = `clamp(floor((ptr_x - x)/w * count), 0, count-1)`. Emits `count` cell rects + `count` cell labels (each label drawn at a fixed left inset of its cell, matching the button's `x + w*0.10` convention — the core has no text measurement, so no true centering).
- **Stepper:** minus region = `[x, x + w*0.28]`, plus region = `[x + w*0.72, x + w]`, value display in the middle. Active is tracked on the single id (press anywhere in `(w,h)`); on release, if the release point is in the minus region → `max(lo, value-1)`, in the plus region → `min(hi, value+1)`, else `value`. Emits: minus button rect + "-" glyph text, the value as TEXT in the middle, plus button rect + "+" glyph text.
- **Coordinate convention** is the established one: page-local meters, y-up, `(x,y)` = top-left, rect spans `x..x+w`, `y-h..y`; TEXT `h` = height in meters.

## Component 2 — the synth page reorganized (`app_synth.c`)

New one-page layout (right page), top to bottom:
1. title `"synth"` (label)
2. **wave** radio — full-width cell bar, `WAVE_NAMES = {"SQ","SW","TR","SI","NZ"}` (5 cells, matching synth wave 0=square,1=saw,2=triangle,3=sine,4=noise)
3. the four existing sliders — `freq`, `sustain`, `decay`, `duty` (unchanged)
4. a compact row sharing two controls: **crush** stepper (left ~half) + **low-pass** checkbox (right ~half)
5. the **Sound** / **Roll** buttons

```
┌───────────────────────────┐  right page
│  synth                    │
│  wave [SQ|SW|TR|SI|NZ]    │  radio
│  freq    ====O=========   │
│  sustain ===O==========   │  4 sliders (unchanged)
│  decay   ==O===========   │
│  duty    ======O=======   │
│  crush [- 8 +]   lp [x]   │  stepper + checkbox (one compact row)
│   [ Sound ]    [ Roll ]   │  buttons
└───────────────────────────┘
```

**Vertical budget** grows from the slice's ~7.95·row to **~10.45·row** (title 1.5 + wave 1.25 + 4×1.25 + compact 1.25 + 0.4 gap + 1.05 btn). So `app_synth_page`'s row factor drops from `h*0.12` to **`h*0.09`** (10.45 × 0.09 = 0.94·h, fits with margin), and the height-budget comment is updated to the new arithmetic. The 0.055 row cap stays (bounds rows on tall pages). Total draw commands ≈ 33 — well under `WIDGET_MAX_CMDS` (128).

**Wiring** (schema indices per `synth.h`):
- `wave` (0): `params[0] = (float)widget_radio(ctx, id++, x0, y, w, row*0.9, WAVE_NAMES, 5, (int)params[0]);`
- `crush` (16): `params[16] = (float)widget_stepper(ctx, id++, x0, y, w*0.45, row*0.9, (int)params[16], 0, 16);`
- `lpcut` (14): `{ sol_bool on = params[14] > 0.0f; if (widget_checkbox(ctx, id++, x0 + w*0.55, y, row*0.6, &on, "low-pass")) params[14] = on ? 2000.0f : 0.0f; }`

`app_synth_roll` is **unchanged** — it randomizes the four slider knobs only; `wave`/`crush`/`lpcut` keep their preset/edited values through a roll (acceptable; randomizing them is a later nicety).

## Data flow (unchanged from the slice)

`synth_book_input` already calls `app_synth_page(ctx, params, x0,y0,w,h)` every frame, then services the `SynthAction`. The new controls just write more `params[]` slots in place. The render walk draws whatever `widget_ctx` holds. Sound/Roll continue to drive `synth_render` on press. So the wave/crush/lpcut edits are audible the next time **Sound** is pressed — consistent with the press-to-audition model (no per-edit re-render).

## Error handling / edge cases

- `id == 0` → widgets return their no-op default (idle sentinel).
- Radio with the pointer outside the bar, or no click this frame → returns `value` unchanged. An out-of-range incoming `value` simply highlights no cell (and the next click sets a valid one).
- Stepper clamps to `[lo,hi]`; a release outside both ends returns `value`.
- Checkbox reads/writes only `*value`; a release off the box does not toggle.

## Testing

**`widget_test.c`** (add cases):
- **checkbox:** press+release over the box toggles `*value` exactly once; release off the box does not toggle; emits a box rect (and an inset fill rect when on).
- **radio:** a press+release in cell `k` returns `k`; a press+release outside the bar returns the incoming `value`; emits `count` rects + `count` texts.
- **stepper:** press+release in the minus end returns `value-1` (and stops at `lo`); plus end returns `value+1` (stops at `hi`); a release in the middle returns `value`.

**`app_synth_test.c`** (update):
- recount the rect/text expectations for the new page (radio cells + stepper + checkbox in addition to the sliders/buttons/labels) — assert the totals match the new layout rather than the old `knob_count*2 + 2` rect formula.
- a press+release on a wave cell changes `params[0]` to that cell's index; the stepper changes `params[16]`; the low-pass checkbox toggles `params[14]` between `0` and `2000`.
- the existing Sound→`SYNTH_ACT_PLAY` and roll-determinism tests still pass (roll still only touches the four curated knobs).

**Build gauntlet** (no new TUs, no shader): `./build.sh c89check && ./build.sh widgettest && ./widget_test && ./build.sh appsynthtest && ./app_synth_test && ./build.sh debug && ./build.sh metal`. Then a human live-verify: open a synth book, pick a wave (hear it change on Sound), step crush, toggle low-pass.

## Constraints honored

- **Strict C89** (decls at top of block, `/* */`, no VLAs, `-Wall -Wextra -Werror` clean — these files are now in the c89check list).
- **Pure-module discipline** — widgets stay GL/scene/synth-free; `app_synth` stays GL/scene/mixer-free; both keep their `*_test.c`.
- **No new shader / no MSL twin / no `main.c` change** — the defining scope win.
- Commits end with the `Co-Authored-By: Claude Opus 4.8 (1M context)` line; never stage `NOTES.stml` / `paper-picture.png`.

## File structure

```
widget.h        modify — 3 new declarations + behaviour comments
widget.c        modify — widget_checkbox / widget_radio / widget_stepper (~90 lines)
widget_test.c   modify — checkbox / radio / stepper cases
app_synth.c     modify — WAVE_NAMES, the reorganized app_synth_page layout + wiring, row factor 0.12→0.09
app_synth_test.c modify — updated counts + wave/crush/lpcut assertions
```
