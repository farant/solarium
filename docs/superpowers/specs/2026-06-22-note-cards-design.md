# Note Cards — Design

**Date:** 2026-06-22
**Status:** Approved

## Goal

Improve note cards in three ways:

1. **Drop the "note" label** that prints across the top of every note card.
2. **Per-note text size**, adjusted with `+`/`-` while the note is selected.
3. **Resizable notes** via corner-drag (reusing the whiteboard resize machinery),
   with the card **auto-growing its height** to fit the wrapped text.

Scope is **note cards only** (`KIND_NOTE`). File/alias/tombstone cards keep their
name labels and fixed size.

## Background — how note cards work today

- A card is `mesh_ref "card"` (registry params `w`/`h`/`t`, defaults `0.35/0.5/0.03`).
  The render loop at `main.c:10793` draws, for every card: a **name label** across
  the top of the front and back faces, and — for `KIND_NOTE` — the **body text**
  (`meta["text"]`) below it, word-wrapped to `usable = w - 2*margin` at a **fixed**
  size `0.028f/lh` (`main.c:10888`), where `lh = font_line_height(state->ui_font)`.
- Text reflow is already per-frame: `wtext_block(..., usable, ...)` re-wraps to the
  current width every frame, so changing `w` reflows the text for free.
- `text_wrap(font, utf8, px2m, max_width, out, cap)` (`text.c:126`) returns the
  **line count** and the wrapped string — the measurement auto-height needs.
- Editing mirrors every keystroke into `meta["text"]`: `on_char` (`main.c:11629`)
  and the backspace/enter branches of `on_key` (`main.c:11695`,`:11699`) all call
  `scene_meta_set(... "text" ...)`. So a hook after those makes the card grow live.
- Resize today (whiteboards/pictures) is corner-drag, gated to wall-mounted objects
  (`board_is_mounted` → `resize_corner_pick` → `board_world_corners`, then a drag
  block at `main.c:7625`). The **pick** is wall-agnostic — `board_world_corners`
  builds corners from the object's own world position, yaw, and `w`/`h`. The
  **drag** is wall-anchored: it intersects the room's wall planes and clamps to the
  wall run / floor / ceiling, keyed off `resize_room`.
- Free keys confirmed: `GLFW_KEY_EQUAL`, `GLFW_KEY_MINUS`, `GLFW_KEY_KP_ADD`,
  `GLFW_KEY_KP_SUBTRACT` are unused in `main.c`.

## Feature 1 — remove the "note" label

In the card render loop, skip the front+back **name label** when `o->kind ==
KIND_NOTE`, and start the note body at the top of the card (`ch - margin`) instead
of below the (now absent) label. The note keeps `meta["name"]="note"` internally
(used nowhere visible after this) — only the label *draw* is removed. Non-note
cards are unchanged. (A note filed onto a bookshelf still shows its name on the
spine; that path is left as-is — an edge case, not worth special-casing now.)

## Feature 2 — per-note text size with +/-

- Store the body text size in `meta["text_size"]` as meters-per-line; absent ⇒ the
  current default `0.028`. The render reads it: `bpx2m = text_size / lh`.
- While a note is **selected** (`selected_handle` is a `KIND_NOTE` card) and **not**
  being edited (`edit_handle == 0`), `=`/`KP_ADD` grows and `-`/`KP_SUBTRACT`
  shrinks it by a fixed step (`0.004`), clamped to `[0.015, 0.060]`. Edge-triggered
  (one step per press), then `scene_save`. After the change, call `note_autosize`
  (Feature 3) so the card reflows and regrows.
- Keys are polled in `read_input` alongside the other selected-object keys, behind
  the existing `edit_handle == 0 && !palette.open` guard (so they never fire while
  typing).

## Feature 3 — resize + auto-grow height

### Behavior

Select a note → four corner handles appear (same render + grab as whiteboards).
Aim the crosshair at a corner, hold, and look to drag:

- **Horizontal drag sets the WIDTH** → the wrap boundary → the text reflows live.
- **Height is automatic.** The card is always tall enough to show every wrapped
  line at the current text size; it is **top-anchored** (the top edge stays put,
  the card extends downward). Dragging a corner **down** sets a larger *minimum*
  height (blank padding below the text); the card never shrinks below the text.

So in practice: drag = width, height takes care of itself. Auto-grow also fires
when text size changes (Feature 2) or when the text is edited.

### `note_autosize(AppState *st, sol_u32 handle)` — the height authority

The one place a note's height and vertical position are set. Steps:

1. Read `cw` (current width), `text_size`, and `meta["text"]`.
2. `px2m = text_size / lh`; `usable = cw - 2*margin`;
   `lines = text_wrap(ui_font, text, px2m, usable, buf, cap)` (≥ 1).
3. `content_h = top_margin + lines * text_size + bottom_margin`.
4. `min_h` = the note's stored minimum (`meta["min_h"]`, default = the default
   card height `0.5`); `new_h = max(min_h, content_h)`.
5. If `new_h` differs from the current `h` param (beyond an epsilon): capture the
   card's current **world top-centre**, then do the registry-shared-mesh rebuild —
   `mesh_asset_key` (old) → `asset_release` → `scene_mesh_params_set([cw,new_h,t])`
   → clear the borrow (`memset(&o->mesh,0,...)`) → `scene_resolve_meshes` — and set
   `o->pos` so the world top-centre is unchanged (top-anchored growth). `cw` and the
   horizontal position are **not** touched.

Called after: a text-size change, a width change during resize, and each note text
mutation (`on_char` + the `on_key` backspace/enter branches, for the edited note).

### Resize wiring

- **Handles + pick gate:** a small predicate `note_resizable(s, h)` = the handle is
  a `KIND_NOTE` `"card"`. Render handles when `board_is_mounted(sel) ||
  note_resizable(sel)` (`main.c:10965`). On press, try `resize_corner_pick` when
  `board_is_mounted(sel) || note_resizable(sel)` (`main.c:7544`). `resize_corner_pick`
  already works for a card (`board_world_corners` reads the `"card"` params). If no
  corner is grabbed it falls through to `drag_begin` (carry), as today.
- **Drag branch:** in the resize-drag block (`main.c:7625`), re-fetch `o` and branch
  on kind **before** the wall guard (a free note's `resize_room` is 0 or a board, not
  a room):
  - `KIND_NOTE`: project the pick ray onto the card's own front-face plane
    (`ray_vs_plane(ray, resize_anchor, n)`, `n` from `board_yaw`), feed the hit to
    `board_resize_corner(anchor, hit, resize_u, MIN_W, aspect=0, &nw,&nh,&origin)`,
    set the **width** param + horizontal position from `nw`/`origin`, store
    `min_h = nh` into `meta["min_h"]`, then call `note_autosize` for the height +
    vertical position. No wall, no floor/ceiling clamp (just `MIN_W`).
  - else: the existing wall-anchored board/picture path, unchanged.

### State

No new `AppState` field: the resize-drag block distinguishes the note path from the
wall path by the resized object's `kind`, and reuses the existing
`resize_board`/`resize_anchor`/`resize_u` state. The per-note state lives in
`meta["text_size"]` and `meta["min_h"]`, which round-trip through `scene_save`.

## Data flow summary

```
select note ──► handles render; +/- and resize enabled
+/- key ─────► meta["text_size"] ±step (clamp) ─► note_autosize ─► save
edit text ───► meta["text"] (per keystroke) ────► note_autosize (live grow)
resize drag ─► width param + meta["min_h"] ─────► note_autosize ─► (release) save
note_autosize: text+size+width ─► lines ─► h=max(min_h,content_h) ─► rebuild + top-anchor
render: skip label for notes; body at top, size = text_size/lh, wrap = usable
```

## Files

All changes are in `main.c` (one self-contained area: the card render block, the
new `note_autosize` near the card/board helpers, the selected-note key handling in
`read_input`, the resize pick-gate + drag branch, and the `note_autosize` hooks in
`on_char`/`on_key`). No new files, no new mesh ref, **no new shader → no MSL twin**.

## Testing

This is GUI input + world rendering, verified by **human live-verify** (subagents
can't GUI-test), consistent with every prior card/carry/place feature. The build
gauntlet (`./build.sh c89check && ./build.sh debug && ./build.sh metal`) must pass.
The pure arithmetic of step 3 (`content_h` from a line count) is small and may be
left inline; no new `*_test` target is required (`board_resize_corner` already has
unit coverage and is reused unchanged).

Manual verification:
1. Spawn a note (`N`), open it (`Enter`), type a paragraph → the card grows
   downward as lines wrap; top edge stays put.
2. With the note selected (not editing), press `+`/`-` → body text grows/shrinks,
   card height re-fits, persists across reload.
3. Select the note → corner handles appear → drag a corner horizontally → width
   changes and text reflows; drag down → extra blank height; release → saved.
4. No "note" label appears on the front or back of a note.
5. Regression: file/alias/tombstone cards still show their name labels and are not
   resizable; whiteboard/picture corner-resize still works; typing in a note still
   doesn't trigger the +/- size keys.

## Constraints (project laws)

- Strict C89; build gauntlet passes on both backends.
- No new shader → no MSL twin.
- Never stage/commit `NOTES.stml` or `paper-picture.png`.
- Feature branch in-place → ff-merge to main; commits end with the
  `Co-Authored-By: Claude Opus 4.8 (1M context)` line.
