# Map View Refinements Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Free the cursor in map view (clicks inert for now) and let a map be entered from inside board view, returning to that board view on Esc.

**Architecture:** Two independent main.c edits on the existing map-view mode. Change 1 generalizes the board-view cursor-unlock to a focus-mode predicate (`board_view || map_view`) and makes clicks inert while framed. Change 2 turns `map_view_enter`/`map_view_exit` into a board↔map *swap* that remembers a return board (one new `AppState` field), preserving the never-both-modes-set invariant.

**Tech Stack:** C89 engine source (`-std=c89 -pedantic-errors -Werror -Wall -Wextra`). GLFW/GL + a runtime-compiled Metal twin (no shader change → no MSL change). Build via `./build.sh <mode>`.

---

## Context the implementer needs

- **Reference the design spec:** `docs/superpowers/specs/2026-06-30-map-view-refinements-design.md`.
- This builds on the just-merged map-view mode: `map_view` is an `AppState` handle parallel to `board_view`; both reuse the `bv_*` camera-glide scratch; `bv_active` (read_input) freezes walking/look while either is set; `board_view_update` advances the shared glide and holds the vanish-guards.
- **No new pure-logic module → no unit test.** Both tasks are input/mode glue; verification is the build gauntlet plus human live-verify. The framing math (`camera_frame_pose_up`) is already tested.
- **Do NOT run the binary** (`./solarium` / `./solarium-metal`) — there is no display. Build only.
- **Commit discipline:** `git add main.c` only. NEVER `git add -A` / `git add .`. NEVER stage `NOTES.stml` or `paper-picture.png` (they show modified — leave them). Each commit body ends EXACTLY with the `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>` line.
- **C89:** all declarations at the top of their block, no `//`, no C99/C11.

## File Structure

- **main.c** (modify) — the only file. Change 1: the `board_view_was` field (rename), the cursor-toggle block in `read_input`, the press handler. Change 2: a new `map_view_return_board` field, `map_view_enter`, `map_view_exit`.

No new files; no shader change.

---

## Task 1: Free the cursor in map view (clicks inert)

**Files:**
- Modify: `main.c` — `AppState` field (~line 2845); the cursor-toggle block in `read_input` (~lines 10975–10985); the press handler (~line 11225).

- [ ] **Step 1: Rename `board_view_was` → `focus_was`**

The field is used ONLY by the cursor toggle (the declaration + three sites, all touched in this task). Find:
```c
    sol_bool board_view_was;      /* edge-detect for the cursor toggle           */
```
Replace with:
```c
    sol_bool focus_was;           /* edge-detect for the cursor toggle (board OR map view) */
```

- [ ] **Step 2: Key the cursor toggle on either focus mode**

Find this block in `read_input`:
```c
    /* board view frees the cursor for pointing at cards (mirrors inventory);
       first-person look re-locks on exit. */
    if (st->board_view && !st->board_view_was) {
        glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        st->mouse_skip = 2;
    } else if (!st->board_view && st->board_view_was &&
               !st->inv_open && !st->editor.active) {
        glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        st->mouse_skip = 2;
    }
    st->board_view_was = (sol_bool)(st->board_view != 0);
```
Replace it with:
```c
    /* board/map view frees the cursor for pointing (mirrors inventory);
       first-person look re-locks on exit. Keyed on EITHER focus mode so a
       board<->map swap doesn't flicker the cursor. */
    {
        sol_bool focus_now = (sol_bool)(st->board_view != 0 || st->map_view != 0);
        if (focus_now && !st->focus_was) {
            glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            st->mouse_skip = 2;
        } else if (!focus_now && st->focus_was &&
                   !st->inv_open && !st->editor.active) {
            glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            st->mouse_skip = 2;
        }
        st->focus_was = focus_now;
    }
```

- [ ] **Step 3: Make clicks inert in map view**

In the press handler (the `if (lmb && !st->lmb_was_down)` block), find:
```c
            if (st->reader_state != READER_IDLE) {
                reader_close(st);                       /* click-away, like blur:
                                                           this press only closes */
            } else if (fp) {
```
Insert a `map_view` no-op branch between the reader branch and the `else if (fp)` branch:
```c
            if (st->reader_state != READER_IDLE) {
                reader_close(st);                       /* click-away, like blur:
                                                           this press only closes */
            } else if (st->map_view != 0) {
                /* map view is view-only: a click does nothing (no pick, so no
                   selection or drag is armed) until pin management defines it */
            } else if (fp) {
```
This intercepts the press before `do_pick`, so no selection or drag is armed; the drag/release/marquee paths (which require an armed drag or `marquee_active`, never set in map view) stay inert.

- [ ] **Step 4: Build the gauntlet**

Run each and confirm success (do NOT run the binaries):
- `./build.sh` — Expected: `built ./solarium (debug)`.
- `./build.sh c89check` — Expected: `c89check: PASS — all sources are C89-pedantic clean`.
- `./build.sh asan` — Expected: links cleanly (the lone `sprintf` deprecation at the existing `main.c` site is pre-existing, not from this change).
- `./build.sh metal` — Expected: `built ./solarium-metal (...links clean...)`.

- [ ] **Step 5: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
Map view: free the cursor (clicks inert for now)

Generalize the board-view cursor-unlock to a focus-mode predicate
(board_view || map_view), so map view frees the cursor like board view and a
board<->map swap won't flicker it. Clicks stay inert in map view via a no-op
press branch (no pick, so nothing is selected/dragged) until pin management.
Renames board_view_was -> focus_was (its only use is this toggle).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Enter a map from inside board view (the swap)

Depends on Task 1 (the unified cursor predicate is what keeps the board↔map swap from flickering the cursor).

**Files:**
- Modify: `main.c` — `AppState` field after `map_view` (~line 2847); `map_view_enter` (~lines 9647–9671); `map_view_exit` (~lines 9673–9683).

- [ ] **Step 1: Add the `map_view_return_board` field**

Find:
```c
    sol_u32  map_view;            /* map being viewed; 0 = not in map view; reuses
                                     the bv_* glide scratch (modes are exclusive) */
```
Insert the new field right after it:
```c
    sol_u32  map_view;            /* map being viewed; 0 = not in map view; reuses
                                     the bv_* glide scratch (modes are exclusive) */
    sol_u32  map_view_return_board; /* board to re-enter on map-view exit; 0 = entered
                                       from first-person (return to standing) */
```

- [ ] **Step 2: Rewrite `map_view_enter` to allow a nested entry**

Find the whole function:
```c
/* Enter map view: frame the selected map (any orientation) and begin the glide.
   Returns 0 (and does nothing) if the selection isn't a map, a focus view is
   already active, or another mode owns the keyboard/cursor. Mirrors
   board_view_enter; reuses the bv_* glide scratch. */
static int map_view_enter(AppState *st) {
    SceneObject *o = scene_get(&st->scene, st->selected_handle);
    CameraPose pose;
    if (st->map_view != 0 || st->board_view != 0) return 0;
    if (!o || !o->mesh_ref || strcmp(o->mesh_ref, "map") != 0) return 0;
    if (st->carried != 0 || st->place_active || st->editor.active ||
        st->palette.open || st->inv_open || st->edit_handle != 0 ||
        st->reader_state != READER_IDLE) return 0;
    if (!map_view_pose(st, st->selected_handle, &pose)) return 0;
    st->bv_return_pos   = st->camera.pos;
    st->bv_return_yaw   = st->camera.yaw;
    st->bv_return_pitch = st->camera.pitch;
    st->bv_from_pos = st->camera.pos;   st->bv_to_pos = pose.pos;
    st->bv_from_yaw = st->camera.yaw;   st->bv_to_yaw = pose.yaw;
    st->bv_from_pitch = st->camera.pitch; st->bv_to_pitch = pose.pitch;
    st->bv_t   = 0.0f;
    st->bv_dir = 1.0f;
    st->map_view = st->selected_handle;
    st->selected_handle = 0;   /* entering map view clears the map's highlight */
    return 1;
}
```
Replace it with:
```c
/* Enter map view: frame the selected map (any orientation) and begin the glide.
   Returns 0 (and does nothing) if the selection isn't a map, map view is already
   active, or another mode owns the keyboard/cursor. Entering FROM board view is
   allowed (the book-in-board-view pattern): it swaps board->map, remembering the
   board so Esc returns there. Reuses the bv_* glide scratch. */
static int map_view_enter(AppState *st) {
    SceneObject *o = scene_get(&st->scene, st->selected_handle);
    CameraPose pose;
    sol_u32    from_board = st->board_view;   /* 0 unless entering from board view */
    if (st->map_view != 0) return 0;
    if (!o || !o->mesh_ref || strcmp(o->mesh_ref, "map") != 0) return 0;
    if (st->carried != 0 || st->place_active || st->editor.active ||
        st->palette.open || st->inv_open || st->edit_handle != 0 ||
        st->reader_state != READER_IDLE) return 0;
    if (!map_view_pose(st, st->selected_handle, &pose)) return 0;
    if (from_board != 0) {                    /* nested: return to the board frame */
        CameraPose bp;
        st->map_view_return_board = from_board;
        if (board_view_pose(st, from_board, &bp)) {
            st->bv_return_pos = bp.pos; st->bv_return_yaw = bp.yaw; st->bv_return_pitch = bp.pitch;
        } else {
            st->bv_return_pos = st->camera.pos; st->bv_return_yaw = st->camera.yaw;
            st->bv_return_pitch = st->camera.pitch;
        }
        st->board_view = 0;                   /* swap board->map (cursor stays free) */
    } else {                                  /* first-person: return to where you stood */
        st->map_view_return_board = 0;
        st->bv_return_pos   = st->camera.pos;
        st->bv_return_yaw   = st->camera.yaw;
        st->bv_return_pitch = st->camera.pitch;
    }
    st->bv_from_pos = st->camera.pos;   st->bv_to_pos = pose.pos;
    st->bv_from_yaw = st->camera.yaw;   st->bv_to_yaw = pose.yaw;
    st->bv_from_pitch = st->camera.pitch; st->bv_to_pitch = pose.pitch;
    st->bv_t   = 0.0f;
    st->bv_dir = 1.0f;
    st->map_view = st->selected_handle;
    st->selected_handle = 0;   /* entering map view clears the map's highlight */
    return 1;
}
```
(`board_view_pose` is defined earlier in main.c, so it's in scope. C89: `from_board` is declared at the top of the function; `bp` at the top of the `if (from_board != 0)` block.)

- [ ] **Step 3: Rewrite `map_view_exit` to re-enter the board view**

Find the whole function:
```c
/* Leave map view: glide the camera back to the stored return pose. Safe to call
   when already out. View-only, so no cursor/selection cleanup (unlike a board). */
static void map_view_exit(AppState *st) {
    if (st->map_view == 0) return;
    st->bv_from_pos = st->camera.pos;   st->bv_to_pos = st->bv_return_pos;
    st->bv_from_yaw = st->camera.yaw;   st->bv_to_yaw = st->bv_return_yaw;
    st->bv_from_pitch = st->camera.pitch; st->bv_to_pitch = st->bv_return_pitch;
    st->bv_t   = 0.0f;
    st->bv_dir = -1.0f;
    st->map_view = 0;
}
```
Replace it with:
```c
/* Leave map view: glide the camera back to the stored return pose. If map view
   was entered from board view, re-enter that board view (the return pose IS its
   frame) so Esc lands you back on the board, mirroring closing a book. Safe to
   call when already out. View-only, so no cursor/selection cleanup. */
static void map_view_exit(AppState *st) {
    if (st->map_view == 0) return;
    st->bv_from_pos = st->camera.pos;   st->bv_to_pos = st->bv_return_pos;
    st->bv_from_yaw = st->camera.yaw;   st->bv_to_yaw = st->bv_return_yaw;
    st->bv_from_pitch = st->camera.pitch; st->bv_to_pitch = st->bv_return_pitch;
    st->bv_t   = 0.0f;
    st->bv_dir = -1.0f;
    st->map_view = 0;
    if (st->map_view_return_board != 0) {
        if (scene_get(&st->scene, st->map_view_return_board) != 0)
            st->board_view = st->map_view_return_board;   /* re-enter board view */
        st->map_view_return_board = 0;
    }
}
```

- [ ] **Step 4: Build the gauntlet**

Run each and confirm success (do NOT run the binaries):
- `./build.sh` — Expected: `built ./solarium (debug)`.
- `./build.sh c89check` — Expected: `c89check: PASS — all sources are C89-pedantic clean`.
- `./build.sh asan` — Expected: links cleanly.
- `./build.sh metal` — Expected: `built ./solarium-metal (...links clean...)`.

- [ ] **Step 5: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
Map view: enter a map from board view (swap with return board)

map_view_enter now allows entering from board view: it swaps board->map and
remembers the board (new map_view_return_board field); map_view_exit re-enters
that board view on Esc (the return pose is its frame), mirroring closing a
book. Never both modes set at once, so the mutual-exclusion invariant holds.
Deleting the framed map now pops you back to the board view too.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## After both tasks: human live-verify (do not skip)

Hand off this checklist (the work merges only after Fran live-verifies):

- **Map in first-person:** Enter frames it; the **cursor is now visible and movable**; clicking does nothing; Esc returns to standing and the cursor re-locks.
- **Map pinned to a board:** enter board view, click the map, Enter → glide to the map frame (cursor stays free); Esc → back to the **board view** (board re-framed); second Esc → standing.
- The cursor **never flickers/relocks** across the board→map→board transitions.
- **Delete the framed map** (entered from a board) → pops back to that board view.
- While framed, WASD / mouselook / Tab / `:` / `;` / `i` remain inert (unchanged from the MVP).

## Self-review notes (spec coverage)

- Free cursor via focus predicate → Task 1 Steps 1–2. Clicks inert → Task 1 Step 3.
- Nested entry swap + remembered return board → Task 2 Steps 1–2. Re-enter board on exit (with deleted-board guard) → Task 2 Step 3.
- Mutual exclusion preserved (only one of `board_view`/`map_view` ever set); `board_view_enter`'s `|| map_view != 0` guard and the Esc ladder are unchanged and still correct.
- No shader / no MSL; gauntlet incl. metal → Steps 4 in each task.
- Out-of-scope (defining click actions, routing pick_ray to the cursor) intentionally not implemented.
