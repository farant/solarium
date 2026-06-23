# Width- & Depth-Aware Shelf Packing — Design

**Date:** 2026-06-23
**Status:** Approved

## Goal

Shelve filed items by their actual size so large/varied books don't overlap.
Replace the fixed-pitch slot grid with **variable-width packing** that lays each
item out by its own spine thickness, seats its spine flush at the shelf front
(depth-aware), and **re-flows the whole shelf** when an item is added or removed.
Unified for **all filed items** (cards and codices), so mixed shelves lay out
cleanly. Width + depth only this pass (vertical-fit deferred).

## Background — what exists today

- **Fixed grid:** `furniture_shelf_slot(params, count, i)` (`furniture.c`) returns
  the i-th slot's LOCAL position on a fixed grid — `cols = furn_shelf_cols(w)`
  columns at a constant `FURN_SPINE_PITCH` (6 cm), `sh+1` rows (boards + floor),
  `s.z = d*0.5 - 0.04` (just inside the front). `furniture_shelf_capacity` =
  `cols*(sh+1)`. `shelf_free_slot` (`main.c`) scans 0..capacity for the lowest
  unoccupied slot (a child within 0.025 m). A card (thin) fits the 6 cm pitch;
  a codex (thicker, varied) overflows it → overlap.
- **Filing:** `carry_update`'s furniture-preview loop sets `st->file_local`
  (= `furniture_shelf_slot` for a shelf) + `st->file_rot`; `cmd_carry_toggle`'s
  `file_aim` drop re-parents the carried item to the furniture at `file_local`/
  `file_rot`. Pickup detaches a shelved item (`on_furn` branch).
- **Axis mapping (filed spine-out):** a card files with `file_rot = Y(90°)`, a
  codex with `Y(90°)·X(-90°)` (furniture-LOCAL). Either way the item's
  **thickness `t`** runs along the shelf, its **width `w`** goes into the shelf
  (depth, spine→fore), its **height `h`** is vertical. Both meshes carry `w`/`h`/`t`
  params (`mesh_ref_param`); a codex reads them from its `book_cover` child, a card
  from itself.
- **Origins:** a card is bottom-origin (`pos.y` = its base). A codex anchor's
  origin is the book's CENTRE (`pos.y` = centre; base = `pos.y - h/2`).

## Footprint helper

`shelf_item_dims(AppState*, sol_u32 handle, float *along_w, float *depth, float *height)`
— the item's shelf footprint:
- If it's a codex (`codex_cover_child(handle) != 0`): read the cover child's params
  → `along_w = t`, `depth = w`, `height = h`.
- Else (a card): read the item's own params → `along_w = t`, `depth = w`,
  `height = h`.

## Variable-width layout (pure math, `furniture.c`)

```c
/* Lay n items of along-shelf widths widths[0..n) left-to-right across the shelf,
   each separated by FURN_SHELF_GAP, wrapping to the next row (0 = top board ...
   sh = floor) when the next item would overrun the usable width. Fills out_x[i]
   (LOCAL x, item CENTRE) and out_row[i]. Returns rows used. An item wider than a
   whole row still gets placed (its own row), never dropped. */
int furniture_shelf_layout(const float *params, int count,
                           const float *widths, int n,
                           float *out_x, int *out_row);

/* the LOCAL y of a row's board top (extracted from the old slot's s.y formula),
   so the codex/card vertical anchor can sit a base on it. */
float furniture_shelf_row_y(const float *params, int count, int row);
```

- Usable width = `w - 2*FURN_SHELF_MARGIN`; rows available = `sh+1`.
- `out_x[i]` = running cursor + `widths[i]/2`; cursor advances by
  `widths[i] + FURN_SHELF_GAP`; wrap when `cursor + widths[i] > usable`.
- `furniture_shelf_row_y(row)` returns the existing `s.y` board height for that row.
- New constant `FURN_SHELF_GAP` (~1 cm). Both functions are pure → unit-tested in
  `furniture_test` (widths pack in order, wrap at the right point, row_y matches the
  old grid's row heights).

The fixed-grid `furniture_shelf_slot`/`furniture_shelf_capacity` and the
`shelf_free_slot` scan are **retired** (removed) — all filing routes through the
layout.

## Re-pack on file / unfile (`main.c`)

`shelf_repack(AppState *st, sol_u32 furniture)`:
1. Collect every filed item parented to `furniture` (codex anchors + cards — not
   the shelf's own structural children), into an array.
2. Sort them into a stable fill order: by current local row then x
   (top-to-bottom, left-to-right). A freshly dropped item, placed at the append
   position, sorts last.
3. Build `widths[]` from `shelf_item_dims` (each `along_w + 0` — the gap is added
   inside the layout), call `furniture_shelf_layout`.
4. For each item set its LOCAL pos: `x` = `out_x[i]`; `y` =
   `furniture_shelf_row_y(out_row[i])` + the item's vertical anchor (codex:
   `+ height/2`; card: `+ 0`); `z` = `(d*0.5 - 0.02) - depth/2` (spine flush at the
   front; a book deeper than the shelf overhangs the back).

Called after the **drop** re-parents an item to a shelf, and after a **pickup**
detaches an item from a shelf (re-flow the remaining), then `scene_save`.

## Preview & drop integration

- **Preview** (`carry_update` shelf branch): instead of `shelf_free_slot` +
  `furniture_shelf_slot`, compute the **append position** — gather the existing
  items' widths, append the carried item's width, run `furniture_shelf_layout`, and
  take the last entry's `x`/`row` for `st->file_local` (with the same y/z anchor as
  the repack). A faithful hint of where it lands before the reflow.
- **Drop** (`cmd_carry_toggle` `file_aim` branch): after `o->parent = file_target`
  etc., if the target is a shelf, call `shelf_repack(st, file_target)`.
- **Pickup** (`cmd_carry_toggle` pickup, the `on_furn` detach): capture the source
  furniture before detaching; if it's a shelf, `shelf_repack` it after the item
  leaves.

## Scope notes

- **Width + depth only:** a book taller than a row's board gap may clip the board
  above; vertical-fit (seeking a tall-enough row) is a deliberate later pass.
- **Unified:** cards now pack through the same path (replacing the fixed grid). A
  pure card shelf should look ~the same (thin items, tight pitch); a mixed shelf
  lays out without overlap.

## Files

- Modify: `furniture.c` / `furniture.h` — `furniture_shelf_layout`,
  `furniture_shelf_row_y`, `FURN_SHELF_GAP`; remove `furniture_shelf_slot`/
  `furniture_shelf_capacity`.
- Modify: `furniture_test.c` — cases for the layout (packing, wrap, row_y).
- Modify: `main.c` — `shelf_item_dims`, `shelf_repack`; the `carry_update` preview;
  the drop + pickup repack calls; remove `shelf_free_slot`.
- No new mesh, **no new shader → no MSL twin**.

## Testing

The packing math is pure → **unit-tested** in `furniture_test` (new cases). The
placement/reflow is GUI → **human live-verify** (build gauntlet
`./build.sh c89check && ./build.sh debug && ./build.sh metal && ./build.sh
furnituretest` must pass). Manual checks:
1. File several different-sized books on one shelf → they pack tight, no overlap,
   spines flush at the front; large books overhang the back.
2. Remove a mid-shelf book → the rest slide to close the gap.
3. Fill a row → the next book wraps to the row below.
4. Mix cards and books on one shelf → all lay out without overlap.
5. Reload → shelved layout persists (positions are saved).

## Constraints (project laws)

- Strict C89; build gauntlet (incl. `furnituretest`) passes on both backends.
- `furniture.c` stays pure (no scene/GL); the scene gathering lives in `main.c`.
- No new shader → no MSL twin.
- Never stage/commit `NOTES.stml` or `paper-picture.png`.
- Feature branch in-place → ff-merge to main; commits end with the
  `Co-Authored-By: Claude Opus 4.8 (1M context)` line.
