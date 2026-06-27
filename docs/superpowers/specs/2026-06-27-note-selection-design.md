# Text Selection for Notes — Design Spec

**Date:** 2026-06-27
**Status:** Approved (brainstorm), pending implementation plan
**Author:** Fran + Claude

## Goal

Build on the note caret ([[note-cursor]]) with **text selection**: click-drag to select a span,
double-click to select a word, triple-click to select the whole note, Shift+arrows to extend, a
visible highlight, and selection-aware editing (type-over, delete). Normal selection behavior plus
the multi-click affordances.

## Decisions (from brainstorming)

- **Notes only** (the reader-book caret/selection stays deferred).
- **Mouse selection is board-view** (free cursor): click-drag span, double-click word, triple-click
  whole note. **Shift+arrows work everywhere** (incl. first-person, where there's no mouse drag).
  **Shift+click** extends to the click (board view).
- **Editing replaces/deletes the selection:** typing or Enter type-over the selection; Backspace/
  Delete remove it; a plain click/arrow/Esc collapses it (no deletion).
- **No text clipboard in v1** (Cmd+C/X/V on selected text is a deliberate follow-up — they currently
  cut/copy/paste board *cards*, and routing them to text is its own chunk).
- **Word = a maximal run of one class** (word / space / other), multibyte/non-ASCII bytes counting as
  word chars. **Highlight = a soft-blue translucent quad** behind the ink.
- `text_shape`/`wtext_block` (the §1.6 render seam) stay untouched. Selection is editing-session state,
  cleared on blur.

## Non-Goals

- No reader-book selection; no cross-note selection.
- **No text clipboard** (copy/cut/paste of the selected text) — the deferred follow-up.
- No word-granular drag (double-click-then-drag extends char-wise, not word-by-word).
- No selection persistence past blur; no undo/redo.
- No change to `text_shape`/`wtext_block`/`text_wrap`.

## Background (current state — verified)

- A note edits through `edit_handle`/`edit_buf`/`edit_len`/`edit_cursor`/`edit_goal_x` (the caret).
  `caret.c` (pure) has `CaretField` (slots = `{src offset, x}`, lines), `caret_reconcile`,
  `caret_field_build`, `caret_slot_for_offset`/`caret_line_of_slot`/`caret_slot_nearest_x`,
  `caret_cplen`; `caret_build` (font glue, main.c) assembles a field; `caret_click_place` ray-hits the
  edited note's surface → nearest slot → `edit_cursor`.
- **While editing**, the modal gate (`read_input`, main.c:10817) handles only a left-press
  (`caret_click_place` or `note_edit_end`); **drag-while-editing currently does nothing** — free for
  drag-select. The render's `KIND_NOTE` block draws the text via `wtext_block` then the caret quad.
- **Multi-click**: the board-view (NOT-editing) press handler detects a double-click via
  `last_press_t`/`last_press_x`/`last_press_y` + `BOARD_DBL_S`/`BOARD_DBL_PX` and, for a NOTE, calls
  `note_edit_begin` (enters edit, caret at end). The editing path (modal gate) is a separate branch
  (mutually exclusive: editing vs not).

## Architecture

### 1. Selection state (AppState)

```c
int edit_sel_anchor;   /* the fixed end; selection = [min(anchor,cursor), max(anchor,cursor)) */
```
Helpers (main.c): `edit_sel_lo(st)`/`edit_sel_hi(st)` (min/max of anchor,cursor),
`edit_has_sel(st)` (`anchor != cursor`). `note_edit_begin` sets `edit_sel_anchor = edit_cursor`
(no selection). `note_edit_end` needs nothing (state dies with the edit).

### 2. A shared click-sequence counter (uniform multi-click)

Add `int click_seq;` to AppState. On **every left-press anywhere**, if it's within `BOARD_DBL_S` /
`BOARD_DBL_PX` of the last press, `click_seq++`, else `click_seq = 1` (then update `last_press_*`).
Both press paths read it:
- **Board-view (not editing):** `is_dbl` becomes `click_seq == 2` (a behavior-preserving refactor of
  the existing double-click). A double-click on a NOTE enters edit AND selects the word at the click
  (see §4). No triple action on the board itself.
- **Editing (modal gate):** `click_seq == 1` → caret place (+ start a drag); `== 2` → select word;
  `== 3` → select all; `>= 4` → caret place. Because the counter is shared, **triple-click from a
  cold note works**: click 1 selects the card, click 2 enters edit + word, click 3 selects all.

### 3. The hit-test (shared) and drag

Factor `caret_click_place`'s ray→note-local→slot math into `caret_hit_offset(st, w, *out)` → the source
byte offset at the cursor (returns 1 on a hit, 0 if off-card / not board view). Then in the modal gate:
- **Press** (`lmb && !lmb_was_down`): `caret_hit_offset` → (if it returns 0, the click is off the
  note → `note_edit_end` blur, as today; otherwise:)
  - **Shift held** (checked first, overrides the click-seq cases): extend — `edit_cursor = off`, keep
    `edit_sel_anchor`; arm drag.
  - `click_seq==1`: `edit_cursor = edit_sel_anchor = off` (caret, empty selection), arm drag.
  - `click_seq==2`: word-select at `off` (§4).
  - `click_seq==3`: select all (`anchor=0, cursor=edit_len`).
- **Held** (`lmb && lmb_was_down`): if a drag is armed, `caret_hit_offset` → `edit_cursor = off`
  (anchor fixed) — the span grows. (No drag past the card edge / no auto-scroll; notes autosize.)
- **Release**: disarm drag. A press with no move leaves an empty selection (a plain caret).

### 4. Word + select-all (pure logic)

`caret_word_at(const char *src, int off, int *start, int *end)` in `caret.c` (pure, unit-tested):
classify each byte as **word** (`[A-Za-z0-9_]` or byte `>= 0x80`), **space** (`' '`,`\t`,`\n`), or
**other** (punctuation); return the maximal run of `off`'s class around it (`off` clamped to
`[0,len]`; at the end-of-text, take the run ending there). A note-level `select_word_at(st, off)`
sets `edit_sel_anchor = start`, `edit_cursor = end`. Select-all = `anchor=0, cursor=edit_len`.
Double-click entering edit: `note_edit_begin` then `select_word_at(st, caret_hit_offset)`.

### 5. Shift+arrows + plain-arrow collapse (edit-key block, main.c)

The edit-key block (Left/Right/Up/Down) gains a Shift check (`mods & GLFW_MOD_SHIFT`):
- **Shift held:** move `edit_cursor` as today (codepoint for L/R, visual line for U/D) but **do NOT
  touch `edit_sel_anchor`** — the span extends. (Shift+Up/Down still preserve `edit_goal_x`.)
- **No Shift, selection present (`edit_has_sel`):** **collapse to an edge without an extra move** —
  Left/Up → `cursor = anchor = edit_sel_lo`; Right/Down → `cursor = anchor = edit_sel_hi`. Refresh
  `edit_goal_x`. (Simple and unambiguous; "move from the collapsed edge" is a later refinement.)
- **No Shift, no selection:** move as today, and set `edit_sel_anchor = edit_cursor` (stay collapsed).

### 6. Editing with a selection (on_char, Backspace, Delete, Enter — main.c)

`selection_delete(st)` (new helper): if `edit_has_sel`, `memmove` out [lo,hi), `edit_len -= (hi-lo)`,
`edit_cursor = edit_sel_anchor = lo`, NUL-terminate, mirror to meta + `note_autosize`. Then:
- **on_char / Enter:** call `selection_delete` first (type-over), then insert at the (now collapsed)
  cursor as today; keep `edit_sel_anchor = edit_cursor` after.
- **Backspace / Delete:** if `edit_has_sel` → `selection_delete` (and stop); else the existing
  single-codepoint path (which keeps anchor==cursor).
- Every edit ends collapsed (`anchor == cursor`).

### 7. Rendering the highlight (the note render, main.c)

`caret_sel_spans(const CaretField *cf, int lo, int hi, CaretSpan *out, int cap)` in `caret.c` (pure,
unit-tested): for the range `[lo,hi)`, emit one `CaretSpan {int line; float x0, x1;}` per visual line
the range covers — first line `slot(lo).x → line's last-slot x` (or `slot(hi).x` if same line), middle
lines `0 → last-slot x`, last line `0 → slot(hi).x`. (A line's "end x" = the x of its rightmost slot.)
The render, when editing with `edit_has_sel`, builds the `CaretField`, calls `caret_sel_spans`, and
draws a **translucent soft-blue quad per span** (reuse the `caret_mesh` unit quad scaled to
`(x1-x0, line_h)`, positioned at `(x0_base + x0, y0 - line*line_h - line_h/2)` on `face`), through the
alpha path (`draw_glass`-style), **before** `wtext_block` so the ink reads on top. The caret quad
still draws (at `edit_cursor`).

## Data Flow

```
press (board, not editing): click_seq -> 1 card-select / 2 enter-edit+word / 3 select-all
press (editing): caret_hit_offset -> 1 caret+anchor / 2 word / 3 all / shift extend / off-card blur
drag (editing, armed): caret_hit_offset -> cursor (anchor fixed) -> span grows
Shift+arrow: move cursor, keep anchor (extend); plain arrow w/ selection: collapse to an edge
type / Enter: selection_delete (if any) -> insert; Backspace/Delete: selection_delete or 1 char
render (editing, has sel): caret_sel_spans -> translucent quads behind the ink; caret quad on top
```

## File Touch List

- **`caret.c` / `caret.h`**: `caret_word_at`; `CaretSpan` + `caret_sel_spans`. (Pure, GL-free.)
- **`caret_test.c`**: `caret_word_at` (word/space/other runs, ends, multibyte) + `caret_sel_spans`
  (single-line span, multi-line span, full-line middles, empty range) cases.
- **`main.c`**: `edit_sel_anchor`/`click_seq` fields + the helpers (`edit_sel_lo/hi`, `edit_has_sel`,
  `selection_delete`, `select_word_at`, `caret_hit_offset` factored from `caret_click_place`); the
  shared click-seq update; the editing modal gate (press/drag/multi-click/shift); the board-view
  double-click refactor (`click_seq==2`) + word-on-enter-edit; the edit-key block (Shift + collapse);
  `on_char`/Backspace/Delete/Enter (selection_delete); the note render (highlight quads).
- No `build.sh` change (caret.c already linked + in `carettest`/c89check).

## Testing

- **`carettest` (pure, GL-free):**
  - `caret_word_at`: a word in the middle (`"the |quick| fox"`), a word at the start/end, a click on a
    space (selects the space run), on punctuation (selects the punct run), a multibyte word, an empty
    string, `off` at `len`.
  - `caret_sel_spans`: a selection within one line (one span `lo.x→hi.x`); a two-line selection (first
    line `lo.x→end`, second `0→hi.x`); a three-line selection (middle line `0→end`); an empty range
    (`lo==hi` → zero spans); a full-note range.
- **Build gauntlet**: `c89check` / GL / Metal / `carettest`.
- **Human live-verify** (board view + first-person):
  - Click-drag selects a span (highlight follows); double-click selects the word; triple-click selects
    the whole note; from a cold note, triple-click selects all (card→edit+word→all).
  - Shift+Left/Right/Up/Down extend; a plain arrow collapses to the right edge; Shift+click extends.
  - Type over a selection (replaces it); Backspace/Delete removes a selection; Enter splits/replaces.
  - The highlight spans wrapped lines correctly; the caret sits at the active end; blur clears it.
  - No regression: caret movement, click-to-place, double-click-to-edit, autosize, Esc-to-finish,
    board cut/paste of CARDS (Cmd+X/V) still act on cards (we didn't touch them).

## Risks

- **Cross-path multi-click**: the shared `click_seq` must increment uniformly and the board-view
  `is_dbl` refactor must stay behavior-identical (folder nav, create-note, edit-note) — covered by the
  live-verify regression list. If `click_seq` ever desyncs, the worst case is a mis-counted click
  (caret place instead of word) — recoverable, not corrupting.
- **Drag vs board card-move**: drag-select only runs inside the editing modal gate (you must be editing
  first); the board-view card drag/marquee is a different, mutually-exclusive path — no overlap.
- **Highlight z-order**: the translucent quad must sit just behind the ink (text on top) without
  z-fighting — a small z offset under the text plane, tuned in live-verify (same surface the caret
  quad already lives on).
- **Selection offsets always at codepoint boundaries**: word/click/arrow set offsets to slot `.src`
  values or codepoint-walked positions, so `selection_delete`'s `memmove` never splits a codepoint.
- No `text_shape`/`wtext_block`/`text_wrap` change; no new shader (the highlight reuses the alpha path).
