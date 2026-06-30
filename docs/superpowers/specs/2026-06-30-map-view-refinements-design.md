# Map View Refinements — Design

**Date:** 2026-06-30
**Status:** Approved, ready for plan
**Builds on:** `2026-06-30-map-view-design.md` (the map-view mode) and its implementation on the `map-view` branch.

## Why

Live-verify of the map-view MVP surfaced two refinements:

1. **Free the cursor in map view.** It was built view-only with the cursor locked/invisible. It should free the cursor (visible, movable) like board view — both for comfort and to set up pin management.
2. **Enter a map from inside board view.** Today `map_view_enter` bails when `board_view != 0`, so a map pinned to a board can't be focused from board view. It should work like Enter-on-a-book in board view: zoom into the map, and Esc returns to the board view you came from.

## Change 1 — free the cursor in map view

The cursor-unlock is currently keyed on `board_view` alone (a rising edge frees the cursor with `GLFW_CURSOR_NORMAL`; a falling edge re-locks with `GLFW_CURSOR_DISABLED`). Generalize it to a **focus-mode predicate**: the cursor is free whenever `board_view != 0 || map_view != 0`.

- Entering map view (from anywhere) → cursor becomes visible and movable. The camera stays frozen and mouselook stays suppressed (`bv_active` already covers `map_view`).
- Exiting map view to first-person → cursor re-locks.
- A board↔map transition (change 2) keeps the predicate true throughout, so the cursor never flickers/relocks between modes.

**Clicks stay inert in map view.** The freed cursor is for comfort and future pins; a click must not select/move/resize anything yet. The first-person press handler gets a `map_view`-active no-op branch so a click does nothing while framed. (Selection of a map *in board view* — used to pick the map before Enter in change 2 — is unaffected, since that happens with `map_view == 0`.)

### State / code touched (Change 1)

- **`board_view_was` → `focus_was`** (rename the `AppState` field; it is used *only* by the cursor toggle — decl + 3 sites). It now tracks `(board_view != 0 || map_view != 0)`.
- The cursor-toggle edge-detect (read_input) keys on a local `focus_now = (board_view != 0 || map_view != 0)` against `focus_was`, with the same `!inv_open && !editor.active` guard on the relock branch.
- The press handler (`read_input`, the `if (lmb && !lmb_was_down)` press block, around the `else if (fp)` at main.c:11228) gets an earlier `else if (st->map_view != 0) { /* view-only: a click does nothing until pin management */ }` branch so the `do_pick`/drag/resize path never runs in map view.

## Change 2 — Enter a map from inside board view (the swap)

Mirror the book: in board view, click the pinned map (board view's normal selection), press **Enter** → focus the map; **Esc** returns to the board view; a second **Esc** exits board view to standing (two-step).

Implemented as a **swap with a remembered return target**, NOT a both-modes-active nesting (that would break the mutual-exclusion invariant the MVP hardened). Map view gains one field naming the board to re-enter on exit:

```c
sol_u32 map_view_return_board;   /* board to re-enter on exit; 0 = entered from first-person */
```

### `map_view_enter` changes

- Relax the guard from `if (st->map_view != 0 || st->board_view != 0) return 0;` to `if (st->map_view != 0) return 0;` — entering from board view is now allowed; the other-mode bails (carried/place/editor/palette/inv/edit/reader) stay.
- Capture `from_board = st->board_view` at the top.
- If `from_board != 0` (nested entry):
  - `map_view_return_board = from_board`.
  - Set `bv_return_*` to **`board_view_pose(from_board)`** (the board frame is the return target, robust even if the board-view glide hadn't settled), falling back to the current camera pose if `board_view_pose` fails.
  - `board_view = 0` (swap board → map; the focus predicate keeps the cursor free, no flicker).
- Else (`from_board == 0`, first-person entry):
  - `map_view_return_board = 0`.
  - `bv_return_*` = current camera (unchanged from the MVP).
- Then as before: set the forward glide toward the map pose, `map_view = selected_handle`, clear `selected_handle`.

### `map_view_exit` changes

- Set the reverse glide to `bv_return_*` (unchanged), `map_view = 0`.
- **If `map_view_return_board != 0` and that board still exists** (`scene_get != 0`): set `board_view = map_view_return_board` — you land back in board view at its frame (the glide target `bv_return_*` *is* that frame). Clear `map_view_return_board`.
- If the return board was deleted while framed, skip re-entering board view (return to standing at the former board frame); clear `map_view_return_board` regardless. (Acceptable edge case.)

### Interactions / invariants

- **Mutual exclusion preserved:** `board_view` and `map_view` are never both non-zero. `board_view_enter`'s `|| st->map_view != 0` guard stays (can't enter board view while in map view).
- **Esc ladder unchanged** (reader → map_view → board_view → absorb → close). After `map_view_exit` re-enters board view, a second Esc hits the `board_view` branch and exits to standing.
- **Vanish-guards:** deleting the framed map (entered from a board) now pops you back to that board view via `map_view_exit` — a welcome side effect. Deleting the return board while framed is handled by the exit guard above.
- **Modal-open keys** (`:`/`;`/`i`) already require `map_view == 0`, so they stay blocked while framed (added in the MVP).

## Testing

- No new pure-logic module → no new unit test. Verification is the build gauntlet: `./build.sh`, `./build.sh c89check`, `./build.sh asan`, `./build.sh metal`. **No shader change → no MSL twin.**
- C89 discipline in all new main.c code (declarations at block top, `/* */` only).
- Human live-verify:
  - Map in first-person: Enter frames it, **cursor is now visible and movable**; clicking does nothing; Esc returns to standing and re-locks the cursor.
  - Map pinned to a board: enter board view, click the map, Enter → glide to the map frame; Esc → back to the board view (board still framed, cursor free); second Esc → standing.
  - Cursor never flickers/relocks across the board→map→board transitions.
  - Delete the framed map (entered from a board) → pops back to the board view.

## Out of scope (deferred to pin management)

- Defining what a click *does* in map view (place/select/move a pin).
- Routing `pick_ray` through the cursor in map view (only needed once clicks act).
- Entering map view for a map that isn't reachable/selectable from the current board.
