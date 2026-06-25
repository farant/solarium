# Board-View Double-Click — Design Spec

**Date:** 2026-06-25
**Status:** Approved (brainstorm), pending implementation plan
**Author:** Fran + Claude

## Goal

Two new mouse interactions **in board view** (the framed whiteboard view):
- **Double-click the empty board** → create a new note at the cursor and immediately enter edit mode (type right away).
- **Double-click a note** → enter edit mode.

Both gestures end in editing — one creates-then-edits, one just edits.

## Decisions (from brainstorming)

- A "card" here = a **note** (the `N`-key text card; image/alias cards aren't text-editable).
- A freshly double-click-created note starts **empty** (no "press Enter to edit me" placeholder) so you type straight away.
- **Board view only.** Single-clicks are unchanged (select a card / deselect on the empty board). `N` and `Enter`-to-edit still work.
- Double-clicking a **non-note** card (image picture / alias) does nothing special — the single-click selection stands.

## Non-Goals

- No double-click behavior outside board view.
- No new card kinds; only notes are created/edited.
- No new shader → no MSL twin.

## Background (current state)

- The board-view press handler is the first-person press branch (`} else if (fp) {`): it picks at the cursor (`do_pick`), deselects if you clicked the board itself (`selected_handle == board_view → 0`), then dispatches connect / resize-corner / picture-move / drag.
- `N` (main.c ~10344) spawns a `KIND_NOTE` `"card"` pinned to the board under the cursor (`board_under_ray(pick_ray)`), else on the floor; sets `text` = "press Enter to edit me", landscape `NOTE_CARD_*` params + `min_h`, tags the active workspace, selects it.
- `note_edit_begin(AppState*, handle)` (main.c ~14684) opens a note for typing (seeds the edit buffer from its `text` meta). `Enter` on a selected note calls it.
- There is **no double-click tracking** today.

## Architecture (all in `main.c`)

### 1. Factor `spawn_note(st, w)`

Extract the `N`-key note-spawn into a helper returning the new note's handle:
```c
/* Spawn a KIND_NOTE "card": pinned to the board under the cursor, else on the
   floor ahead. Tags the active workspace, selects it, saves. Returns the handle. */
static sol_u32 spawn_note(AppState *st, GLFWwindow *w);
```
It contains the current `N` body verbatim (board_under_ray → scene_add → kind/name/workspace/text("press Enter to edit me")/mesh_ref/`NOTE_CARD_*` params+min_h → board_pin_pos if on a board → resolve+apply → `selected_handle = h` → save → the `printf`), returning `h`. The `N`-key block becomes a one-line call `(void)spawn_note(st, w);`.

### 2. Double-click state

Add to `AppState`:
```c
double last_press_t, last_press_x, last_press_y;   /* board-view double-click detect */
```
Init `last_press_t = 0.0` (so the first press is never a double). Constants near the board-view ones:
```c
#define BOARD_DBL_S   0.35   /* max seconds between the two clicks */
#define BOARD_DBL_PX  6.0    /* max cursor drift (px) between them */
```

### 3. The double-click branch (board-view press handler)

In the `} else if (fp) {` press branch, AFTER `do_pick` + the board-click-deselect and BEFORE the existing `if (try_connect(...))` dispatch, compute a double-click flag (board view only) and make it the first arm of the dispatch:

```c
sol_bool is_dbl = SOL_FALSE;
if (st->board_view != 0) {
    double now = glfwGetTime();
    is_dbl = (sol_bool)(now - st->last_press_t < BOARD_DBL_S &&
                        fabs(mx - st->last_press_x) < BOARD_DBL_PX &&
                        fabs(my - st->last_press_y) < BOARD_DBL_PX);
    st->last_press_t = now; st->last_press_x = mx; st->last_press_y = my;
    if (is_dbl) st->last_press_t = 0.0;   /* consume: a 3rd click isn't a 2nd double */
}
if (is_dbl) {
    SceneObject *so = scene_get(&st->scene, st->selected_handle);
    if (so && so->kind == KIND_NOTE) {
        note_edit_begin(st, st->selected_handle);          /* edit the note */
    } else if (st->selected_handle == 0) {                 /* empty: on the board only */
        vec3 bl;
        if (board_under_ray(st, pick_ray(st, w), &bl) != 0) {
            sol_u32 nh = spawn_note(st, w);                /* board-pins at the cursor */
            scene_meta_set(&st->scene, nh, "text", "");    /* type into it empty */
            note_edit_begin(st, nh);
        }
    }
    /* else: double-click on a non-note card -> nothing (single-click select stands) */
} else if (try_connect(st, st->selected_handle)) {
    ...existing resize / move / drag dispatch unchanged...
}
```

`is_dbl` short-circuits the resize/move/drag dispatch, so a double-click never also starts a drag. `mx`/`my` are the current cursor pos (already in scope). `fabs` is C89 (`<math.h>`, already used in main.c).

### Why this works

- **Note under the cursor** → `selected_handle` is that note (set by `do_pick`) → edit it.
- **Empty board** → the board-click-deselect already set `selected_handle = 0`; gated on `board_under_ray != 0` so it only fires over the board → create + clear text + edit.
- **Non-note card** → `selected_handle` is that card, kind ≠ NOTE, handle ≠ 0 → no branch taken (the single click already selected it).
- The first click of a double is a normal single click (select/deselect, maybe a tap); its release clears any drag candidate, so the second (double) click cleanly edits/creates.

## Data Flow

```
board-view press → do_pick (cursor) → board-click-deselect →
   within 0.35s & 6px of the last press?  -- no  --> normal dispatch (select/drag/resize)
                                          -- yes -->
       note under cursor   -> note_edit_begin
       empty over board    -> spawn_note + clear text + note_edit_begin
       non-note card       -> nothing
```

## File Touch List

- `main.c` only: `AppState` fields + 2 constants; `spawn_note` helper + `N`-key one-liner; the double-click branch in the board-view press handler.

## Testing

- **Build gauntlet (all three):** `./build.sh c89check`, `./build.sh`, `./build.sh metal`.
- **Human live-verify:** in board view — double-click the empty board → a new note appears at the cursor and you're typing in it (empty, no placeholder); double-click a note → edit its text; single-click still selects a card / deselects on empty; double-click an image/alias card does nothing special; `N` still spawns a note; double-click does nothing outside board view.

## Risks

- Double-click vs. drag: the `is_dbl` short-circuit prevents a double-click from also dragging; the first click's tap-release clears the drag candidate. Low risk; live-verify the "double-click then drag" feel.
- No new shader → no MSL twin risk.
