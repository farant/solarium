# Moveable Insertion Caret for Notes — Design Spec

**Date:** 2026-06-27
**Status:** Approved (brainstorm), pending implementation plan
**Author:** Fran + Claude

## Goal

Text editing on board notes is "typewriter mode" — you can only add/delete at the END of
the text. Give notes a **moveable insertion caret**: an offset into the note's text that you
move with the arrow keys (and, in board view, by clicking), with typing/Backspace/Enter
acting AT the caret instead of at the end.

This is the first of two passes; the reader-book (codex) caret is a deliberate follow-up
(it is paginated and already uses Left/Right to flip pages).

## Decisions (from brainstorming)

- **Notes only** for v1 (the single-buffer `edit_buf` path). Reader books later.
- **Movement = char + visual line.** Left/Right move by one codepoint; Up/Down move by
  visual (wrapped) line, preserving a remembered horizontal column (`edit_goal_x`). **No
  Home/End, no word-jumps.**
- **Click-to-place the caret works in BOARD VIEW only** (the cursor is already free there).
  In first-person, the caret moves by arrows only; clicking still blurs (ends editing) as
  today. No new cursor-unlock mode.
- **Editor-owned layout.** A new `caret.c`/`caret.h` maps the note's source text → wrapped
  lines with per-caret-slot x positions + source byte offsets, by calling the **unchanged
  `text_wrap`** for the line breaks and reconciling source↔wrapped. `text_shape` /
  `ShapedGlyph` / `wtext_block` (the §1.6 render seam) are **untouched**.
- **Forward Delete** (the `Delete` key) removes the codepoint AFTER the caret — a small,
  natural companion to Backspace. Included.
- **Solid caret** (a thin vertical quad) for v1; blinking is a noted easy follow-on.

## Non-Goals

- No reader-book / codex caret (follow-up).
- No Home/End, no word-jump (Option/Ctrl+arrow), no selection/highlight, no copy/paste of a
  range (Cmd+C/V/X of board *cards* already exists; this feature does not add text-range
  clipboard).
- No first-person click-to-position (no cursor-unlock-while-editing mode).
- No change to `text_shape` / `ShapedGlyph` / `text_wrap` / `wtext_block` — rendering of the
  note body still flows through `wtext_block` exactly as today.
- No blinking caret in v1 (solid).

## Background (current state — verified)

- A note is edited through `st->edit_handle` (0 = none), a scratch `st->edit_buf[EDIT_BUF_CAP]`
  (2048) and `st->edit_len`. `note_edit_begin` copies `meta["text"]` into the buffer;
  `note_edit_end` writes it back. Every keystroke mirrors `edit_buf` to `meta["text"]` and
  calls `note_autosize`.
- **`on_char`** (main.c, ~16500s) UTF-8-encodes the codepoint and **appends at `edit_len`**.
- The **edit-mode key block** (`if (st->edit_handle != 0) { … }`, main.c ~16600s) handles only
  **Esc** (`note_edit_end`), **Backspace** (drops the LAST codepoint, UTF-8-aware), **Enter**
  (appends `\n`), then `return;` — so **arrow keys currently do nothing while editing a note**
  (they are free to repurpose).
- The note body **renders** in the world-text pass (main.c ~15630s): when `edit_handle ==
  this note` it copies the text into `ebuf` and **appends a `_`** as the caret, then draws via
  `wtext_block(uf, vp, face, txt, x0, top_y, bpx2m, usable, …)` where `x0 = -cw*0.5 +
  2*margin`, `top_y = ch - 2*margin`, `bpx2m = note_text_size(scene,h)/font_line_height`,
  `usable = cw - 3*margin`. These four values define the note-local text frame the caret must
  match.
- A **click while editing** currently BLURS: in `read_input`'s modal gate (main.c ~10800s),
  `if (st->edit_handle != 0) … if (lmb && !lmb_was_down) note_edit_end(st);` — any press ends
  the edit.
- **`text_wrap`** (text.c) is a STRING transform: copies utf8 into `out`, inserting `\n` at
  greedy word breaks and **collapsing runs of separating spaces at those breaks**; `\n` passes
  through. Returns line count. Unit-agnostic (scale × advance vs max_width), so the render's
  meter-space call (`px2m`, `wrap_w_m`) yields meter-space breaks.
- **`text_shape`** yields `ShapedGlyph{glyph,x,y}` with **NO source byte offset**, and SKIPS
  codepoints the font can't render. So shaped glyphs cannot be indexed back to source bytes —
  the caret needs its own offset-aware walk.
- **A `Font` requires GL** (`font_load` uploads the atlas via `rhi_create_texture`). So
  `caret.c` code that touches font metrics is NOT headless-testable; the **pure** parts
  (source↔wrapped reconciliation, slot search) must be factored out so they ARE.

## Architecture

### 1. State (AppState)

```c
int   edit_cursor;   /* byte offset into edit_buf, 0..edit_len; the caret position */
float edit_goal_x;   /* remembered note-local x for Up/Down column preservation */
```
`note_edit_begin` sets `edit_cursor = edit_len` (caret at end, matching today's feel) and
`edit_goal_x` to the caret's x there. `note_edit_end` needs no caret state.

### 2. `caret.c` / `caret.h` (PURE) + `caret_build` (font glue in main.c)

To keep the testable logic GL-free, the code splits in two: **`caret.c` is pure** (no
`font.h`/`text.h`, links standalone under libc), and the **font-bound assembly `caret_build`
lives in `main.c`** with the other render glue (a `Font` requires GL, so anything touching it
cannot be in the unit-tested TU).

`caret.h` — types + the pure declarations:
```c
typedef struct { int src; float x; } CaretSlot;   /* a caret position: source byte + note-local x */
typedef struct { int slot0, nslots, line; } CaretLine;  /* slots[slot0..slot0+nslots), visual line index */
typedef struct {
    CaretSlot slots[CARET_MAX_SLOTS];   /* every inter-char boundary + the end-of-line/text slot */
    int       slot_count;
    CaretLine lines[CARET_MAX_LINES];
    int       line_count;
    float     line_h;                   /* note-local line height (px2m * font_line_height) */
} CaretField;

/* source<->wrapped reconciliation: text_wrap only INSERTS '\n' and COLLAPSES runs of
   break-spaces, so walking both in lockstep recovers each wrapped char's source offset.
   out_src[i] = source byte offset of wrapped[i]; a soft-break '\n' gets the offset of the
   collapsed space run it replaced. Returns the wrapped length. */
int  caret_reconcile(const char *src, const char *wrapped, int *out_src, int cap);

int  caret_slot_for_offset(const CaretField *cf, int cursor);          /* slot with .src==cursor; -1 none */
int  caret_slot_nearest_x(const CaretField *cf, int line, float goal_x); /* nearest .x on `line` */
int  caret_line_of_slot(const CaretField *cf, int slot);              /* slot -> visual line */
```
**`caret.c`** implements exactly those four — strings / the built struct only, no font, no GL.
Covered by `caret_test`.

**`caret_build`** (a `static` in main.c, font-bound, NOT unit-tested — verified live):
```c
static int caret_build(const Font *f, const char *src, float px2m, float wrap_w, CaretField *out);
```
1. `text_wrap(f, src, px2m, wrap_w, wrapped, cap)` → the SAME wrapped string the renderer draws
   (line authority stays single).
2. `caret_reconcile(src, wrapped, out_src, cap)` → each wrapped char's source byte offset.
3. Walk the wrapped string accumulating pen x from font advances/kern (mirroring `text_shape`);
   at each char boundary emit `CaretSlot{src = out_src offset, x = pen}`; a leading slot opens
   each line at x=0; the end-of-line slot sits after the last glyph; `\n` (hard or soft) closes
   the line and opens the next at the next baseline. Returns the line count.

The font-advance x-accumulation is the thin glue verified by live-verify (it mirrors the render
advance, so a mismatch shows as a caret offset from the text).

### 3. Editing operations — insert/delete at the caret (main.c)

Replace the three end-anchored operations with caret-anchored ones (all then mirror to
`meta["text"]` + `note_autosize`, as today):
- **`on_char`**: insert the encoded bytes at `edit_cursor` (`memmove` the `[cursor,len)` tail
  right by `n`, copy in, `edit_len += n`, `edit_cursor += n`), guard `edit_len + n <
  EDIT_BUF_CAP`.
- **Backspace**: if `edit_cursor > 0`, find the previous codepoint start (walk back over
  `0x80` continuation bytes), `memmove` the tail left, shrink `edit_len`, set `edit_cursor` to
  the deleted start.
- **Enter**: insert `\n` at `edit_cursor` (one byte), advance the cursor.
- **Delete (forward)**: if `edit_cursor < edit_len`, find the next codepoint end, `memmove` the
  tail left, shrink `edit_len`; `edit_cursor` unchanged.
After any horizontal edit, refresh `edit_goal_x` from the new caret slot.

### 4. Movement (main.c, in the edit-mode key block)

Build a `CaretField` for the current note (its font = `ui_font`, `px2m`/`wrap_w` from the same
values the renderer uses), then:
- **Left**: `edit_cursor` ← previous codepoint start (no-op at 0); refresh `edit_goal_x`.
- **Right**: `edit_cursor` ← next codepoint start (no-op at `edit_len`); refresh `edit_goal_x`.
- **Up**: line = `caret_line_of_slot(slot_for_offset(cursor))`; if line>0, `edit_cursor` ←
  `slots[caret_slot_nearest_x(cf, line-1, edit_goal_x)].src`; else `edit_cursor = 0`.
  `edit_goal_x` is PRESERVED (not recomputed) so the column survives short lines.
- **Down**: symmetric; on the last line, `edit_cursor = edit_len`.

(Recomputing the `CaretField` per keypress is fine — it's one note's worth of text on a
keystroke, not per-frame.)

### 5. Caret rendering (main.c, the note-body render)

- Stop appending the `_` to `ebuf`. Draw the text unmodified through `wtext_block` as today.
- When `edit_handle == this note`, build the `CaretField`, look up the caret slot for
  `edit_cursor`, and draw a **thin vertical quad** at note-local `(x0 + slot.x, top_y -
  line*line_h)` spanning ~one line height, dark ink, on the note's `face` matrix. Use a small
  dedicated caret mesh (built once, cached on AppState like `resize_handle_mesh`), drawn via
  `draw_mesh`. Solid (no blink) in v1.

### 6. Click hit-test (board view only) — main.c

In `read_input`'s edit-mode blur path, change "any press blurs" to:
- If `st->board_view != 0` AND the click ray hits the **note being edited** (ray vs the note's
  plane → note-local `(lx, ly)`): map to a caret. `line = clamp((top_y - ly)/line_h, 0,
  line_count-1)`; `edit_cursor = slots[caret_slot_nearest_x(cf, line, lx - x0)].src`;
  `edit_goal_x = lx - x0`. Do NOT blur.
- Otherwise (click misses the edited note, or first-person): `note_edit_end(st)` as today.

The note's plane/extent comes from its world matrix + card `w/h` (the same `face` frame the
text uses), reusing the existing board-card ray helpers.

## Data Flow

```
edit a note  -> edit_cursor = edit_len (caret at end)
type/Bksp/Del/Enter -> edit at edit_cursor -> meta["text"] + note_autosize -> refresh edit_goal_x
Left/Right -> +/- one codepoint -> refresh edit_goal_x
Up/Down -> caret_build -> nearest slot on adjacent line at edit_goal_x (goal preserved)
click in board view on the edited note -> ray->note-local (lx,ly) -> nearest slot -> edit_cursor
render the edited note -> caret_build -> thin quad at slots[cursor] (x,y) on the note face
```

## File Touch List

- **Create `caret.c` / `caret.h`** (PURE, C89, no font/GL): `CaretField`/`CaretSlot`/`CaretLine`
  + `caret_reconcile` / `caret_slot_for_offset` / `caret_slot_nearest_x` / `caret_line_of_slot`.
- **Create `caret_test.c`** + a `carettest` `build.sh` target (GL-free, ASan/UBSan): links
  **`caret.c` + `caret_test.c` only** (libc; no font/text/GL). Exercises `caret_reconcile` and
  the slot-search on hand-built `CaretField`s. `caret_build` is NOT covered here (it's in main.c,
  font-bound) — it's verified by live-verify.
- **Modify `main.c`**: the `static caret_build` (font glue); `edit_cursor`/`edit_goal_x` fields;
  `note_edit_begin` seeds them; `on_char` insert-at-cursor; the edit-mode key block
  (Backspace-before / Delete-after / Enter / Left / Right / Up / Down); the note-body render
  (caret quad instead of `_`, + a cached caret mesh); the click-to-position branch in the blur
  path.
- **Modify `build.sh`**: add the `carettest` target; add `caret.c` to the main GL + Metal link
  lines and the `c89check` `-fsyntax-only` source list.

## Testing

- **`carettest` (GL-free, ASan/UBSan; links `caret.c` + `caret_test.c` only)** — pure logic:
  - `caret_reconcile`: a plain line (offsets are identity); a soft wrap (a long line splits;
    each wrapped char maps back to its source byte; the collapsed break-space is accounted);
    a hard `\n` passes through; multiple spaces at a break collapse correctly; a multi-byte
    UTF-8 char keeps its byte offset; trailing text/empty string.
  - `caret_slot_for_offset`: every source boundary has a slot; the end-of-text slot resolves.
  - `caret_slot_nearest_x`: nearest-by-x picks the right slot on a hand-built line (ties, ends).
  - `caret_line_of_slot`: slot→line mapping across multi-line fields.
- **Build gauntlet**: `./build.sh c89check`, `./build.sh`, `./build.sh metal`.
- **Human live-verify** (board view + first-person):
  - Type in the MIDDLE of an existing note → text inserts at the caret, not the end.
  - Backspace deletes before the caret; Delete deletes after; Enter splits at the caret.
  - Left/Right step by character (incl. across a multi-byte glyph); Up/Down move by visual line
    and keep the column through a short line.
  - In board view, click in the note → the caret lands at the clicked spot; the caret quad is
    drawn at the caret, not a trailing `_`.
  - Clicking off the note still blurs; first-person editing works by arrows; nothing regresses
    in note autosize/resize.

## Risks

- **Reconciliation correctness** (source↔wrapped through space-collapse) is the main risk —
  hence the dedicated pure unit test on tricky inputs. If `text_wrap`'s collapse rule is
  subtler than "runs of break-spaces," the test will surface it and we adjust `caret_reconcile`.
- **Caret-vs-text alignment**: the x-accumulation in `caret_build` must mirror the render
  advance/kern (it reads the same `Font`); a mismatch shows as a caret slightly off the text —
  caught immediately in live-verify, tuned against the wtext advance.
- **Font is GL-bound** → `caret_build` lives in main.c and isn't unit-tested; mitigated by
  factoring the risky logic into the pure `caret.c` helpers that ARE tested, plus live-verify
  for the pixel placement.
- **EDIT_BUF_CAP / CARET_MAX_SLOTS bounds**: insert guards on `EDIT_BUF_CAP`; `caret_build`
  caps slots/lines and stops cleanly (a pathologically long note clamps rather than overflows).
- **Per-keystroke `caret_build`**: one note's text per keypress (and once per frame while the
  note is the edit target, for the caret draw) — negligible, not a per-frame-all-notes cost.
