# Board Multi-Select — Design Spec

**Date:** 2026-06-25
**Status:** Approved (brainstorm), pending implementation plan
**Author:** Fran + Claude

## Goal

Finder-style multi-select for board cards **in board view**: shift-click to add/remove
individual cards, drag a rubber-band rectangle to select many at once, across folders,
notes, and pictures simultaneously. A multi-selection can be **dragged together**,
**deleted together**, and **filed onto a folder together** — and filing keeps the cards'
original relative arrangement.

## Vocabulary

- **Selectable card** — a board-child that is a note (`KIND_NOTE`), a picture
  (`mesh_ref="picture"`), or a folder (`mesh_ref="folderbook"`). Not the board itself.
- **Selection set** — the set of currently-selected card handles.
- **Anchor** — the last-clicked card; `selected_handle` continues to point at it.
- **Marquee** — the rubber-band selection rectangle.

## Decisions (from brainstorming)

- **Approach A** — a selection *set* (`sel[]` + `sel_count`) layered beside the existing
  `selected_handle`. `selected_handle` stays the anchor; when `sel_count ≤ 1` everything
  behaves exactly as today. (Approaches B "replace selected_handle everywhere" and C
  "`meta["selected"]` tag" rejected — too invasive / pollutes persistence.)
- **Board view only.** Multi-select, the marquee, group drag/delete/file all apply solely
  while `board_view != 0`. The 3D world's carry/drag is untouched.
- **Marquee = Finder touch/intersect** — any card the rectangle overlaps (even partially)
  is selected. **Live preview:** cards that would be selected highlight *while* you drag,
  before release. **Shift+marquee adds** to the existing selection (else it replaces).
- **Filing keeps the original arrangement** — on a group file, each card is restored to its
  pre-drag board-local position, then re-tagged to the folder's page, so the layout on the
  target page is identical to where the cards were.
- **No new shader** — highlight reuses the `uHighlight`/`hl` selection tint; the marquee
  reuses `ui_quad` / `ui_quad_outline` (screen-space 2D, the inventory-modal path).

## Non-Goals

- No multi-select outside board view.
- No group resize (resize stays single-selection only).
- No marquee in the 3D world or the top-down editor.
- No change to single-card behavior (`sel_count ≤ 1` paths are preserved).

## Background (current state)

- Selection today is a single `st->selected_handle`. The draw loop lights `hl = 1.0` when
  `selection_root(o) == sel_root` (main.c, the per-object draw at ~`13351`).
- The board-view press handler (`read_input`, the `} else if (fp) {` branch) does, in order:
  `do_pick` → board-click-deselect → double-click dispatch (folder navigate / note edit /
  empty spawn-note) → `try_connect` → resize-corner pick → picture-move pick → `drag_begin`.
- `drag_begin` (main.c ~5123) sets `st->drag_handle`; the board-mode drag update re-pins the
  dragged card under the cursor via `board_pin_pos`; the release handler saves (or files onto
  a folder via `drop_target_handle`, set by `folder_at_board_point`).
- Delete dispatch (main.c, Backspace/Delete) removes the single `selected_handle` per kind.
- `board_card_tag_page(st, h)` tags a board-child with its board's `active_page`.
- `ui_quad(x,y,w,h, r,g,b,a)` and `ui_quad_outline(x,y,w,h,t, r,g,b,a)` draw screen-space 2D
  (ui.h:38/42), used by the inventory modal (main.c ~13443).

## Architecture

### New module: `multiselect.c` / `.h` (scene-free pure logic, unit-tested)

```c
#define MULTISEL_CAP 256          /* max cards in one selection */

/* AABB overlap (Finder "touch"): true if rect A and rect B overlap, edges
   inclusive. Corners may be given in any order (the function normalizes). */
sol_bool msel_rect_overlap(float ax0, float ay0, float ax1, float ay1,
                           float bx0, float by0, float bx1, float by1);

/* Set ops on a handle array (len entries, cap capacity). Stable-order. */
sol_bool msel_contains(const sol_u32 *set, int len, sol_u32 h);
void     msel_add(sol_u32 *set, int *len, int cap, sol_u32 h);   /* no-op if present/full */
void     msel_remove(sol_u32 *set, int *len, sol_u32 h);         /* compacts; no-op if absent */
sol_bool msel_toggle(sol_u32 *set, int *len, int cap, sol_u32 h);/* returns new present-state */
```
Pure, depends only on `sol_types.h`. Unit-tested in `multiselect_test.c` (libc only, the
`inventory_test` pattern): overlap normalization + edge-touch + disjoint + containment;
add-idempotent, remove-compacts, toggle round-trip, contains, the cap bound.

### `AppState` additions

```c
sol_u32  sel[MULTISEL_CAP];   /* selection set; sel_count<=1 mirrors selected_handle */
int      sel_count;
sol_bool marquee_active;      /* a marquee gesture is in progress */
sol_bool marquee_dragging;    /* moved past the slop -> a real rubber-band */
sol_bool marquee_add;         /* shift held at start -> union, else replace */
double   marquee_x0, marquee_y0;   /* press point, screen px */
double   marquee_x1, marquee_y1;   /* current cursor, screen px */
float    marquee_lx0, marquee_ly0, marquee_lx1, marquee_ly1;  /* board-local rect (preview) */
sol_bool group_drag;          /* dragging the whole set together */
vec3     group_prepos[MULTISEL_CAP];  /* per-set-member pre-drag board-local pos */
```

### main.c selection wrappers (keep the anchor in sync)

- `sel_clear(st)` → `sel_count=0; selected_handle=0`.
- `sel_set_single(st, h)` → set = `{h}`, `selected_handle=h`.
- `sel_toggle_h(st, h)` → `msel_toggle`; `selected_handle = present ? h : (sel_count? sel[sel_count-1] : 0)`.
- `sel_add_h(st, h)` → `msel_add`; `selected_handle=h`.
- Invariant: `sel_count==1 ⇒ selected_handle==sel[0]`; `sel_count==0 ⇒ selected_handle==0`.
- `object_is_selectable(s, h)` = board-child AND (`KIND_NOTE` || `mesh_ref` "picture" || "folderbook").

### Gesture integration (board-view press handler)

`shift = LEFT_SHIFT || RIGHT_SHIFT`. After `do_pick` gives `hit` (0 = the board/empty):

1. **Double-click** (`is_dbl`) → unchanged (operates on the single clicked card).
2. **`hit` is a selectable card:**
   - `shift` → `sel_toggle_h(st, hit)`. No drag, no navigate.
   - no shift, `msel_contains(hit)` and `sel_count > 1` → keep the set (anchor = hit); this
     press may become a **group drag** (below).
   - no shift, otherwise → `sel_set_single(st, hit)`; this press may become a single drag.
3. **`hit` is empty/the board** → begin a marquee: `marquee_active=1`, `marquee_dragging=0`,
   `marquee_add=shift`, record `(x0,y0)`. Do NOT clear the set yet.

When `sel_count > 1`: the resize-corner pick and picture-move-slide are **suppressed**
(no group resize); the drag dispatch goes to the group path.

### Marquee (during lmb held + release)

- Each frame while `marquee_active`: update `(x1,y1)`; if cursor moved past the slop,
  `marquee_dragging=1`. Project the two screen corners `(x0,y0)`/`(x1,y1)` onto the board
  plane (a `Ray` per corner via `camera_ray` → intersect the board's plane → `scene_world_to_local`)
  to get `marquee_l*` (board-local rect).
- **Live preview:** the draw loop lights `hl=1.0` for any selectable card whose board-local
  footprint (`pos` ± `w/h`) `msel_rect_overlap`s `marquee_l*` — in addition to the set members.
- **Draw:** `ui_quad` translucent fill + `ui_quad_outline` border from `(x0,y0)` to `(x1,y1)`.
- **Release:**
  - `marquee_dragging` → build the hit set (all selectable cards overlapping the rect). If
    `marquee_add`, union into `sel`; else replace `sel`. `selected_handle = sel[sel_count-1]`
    (or 0). 
  - not dragging (a click) → if not `marquee_add`, `sel_clear(st)` (deselect all).
  - `marquee_active=0`.

### Group drag (move together)

- Starts when a press on a set member (`sel_count>1`, no shift) passes the slop. Snapshot
  every `sel[i]` board-local pos into `group_prepos[i]`; `group_drag=1`; `drag_handle` = the
  grabbed anchor (the normal drag path moves it).
- Each board-mode update frame, after the anchor's `pos` is set, compute
  `delta = anchor.pos − group_prepos[anchor_index]` and set each other `sel[i].pos =
  group_prepos[i] + delta` (relative layout preserved live). `arrows_rebuild` once.
- **Drop-target** (for filing) is `folder_at_board_point` under the cursor **excluding any
  folder in the set**; it highlights via `drop_target_handle` as today.

### Group file (onto a folder)

On release of a group drag over a drop-target folder (link non-empty): for each `sel[i]`,
**restore `sel[i].pos = group_prepos[i]`** and `scene_meta_set(sel[i], "page", folder.link)`.
The cards page-out of the current page and reappear on the target page in their original
arrangement. Then `sel_clear`, `scene_save`. (Folders in the set are filed too — the whole
selection moves. The target folder is excluded from the set, so nothing files into itself.)
On release **not** over a folder → save each card's new position (a plain group move).

### Group delete

Delete/Backspace with `sel_count > 1`: copy the set, then delete each handle through its
per-kind cleanup (note / picture / folder — release keyed mesh, clear carry/resize/drag/drop
refs, `scene_remove`), `arrows_rebuild`, `sel_clear`, `scene_save`. `sel_count ≤ 1` keeps the
existing single-delete. (Handles are stable; never hold a `SceneObject*` across a delete.)

### Highlight (draw loop)

`hl = 1.0` if the object is in `sel[]` **or** (marquee active AND its footprint overlaps the
live marquee rect). Folds in beside the existing single-selection tint; reuses `uHighlight`.

## Data Flow

```
press (board view):
  do_pick -> hit
  is_dbl                       -> single navigate/edit (unchanged)
  hit selectable + shift       -> sel_toggle
  hit selectable + in multiset -> keep set, arm group drag
  hit selectable else          -> sel_set_single, arm single drag
  hit empty                    -> begin marquee (add = shift)

drag (lmb held):
  marquee active   -> update rect, live-preview highlight, draw ui_quad
  group_drag       -> anchor follows cursor; others = prepos + delta;
                      drop_target = folder under cursor (not in set)

release:
  marquee dragging -> commit hits (replace or union)
  marquee click    -> clear set (unless shift)
  group over folder -> restore prepos + retag page (keep layout), clear set
  group elsewhere  -> save group positions

Delete: sel_count>1 -> delete all in set; else single (unchanged)
draw: hl = in sel[] OR in live marquee rect
```

## File Touch List

- **`multiselect.h` / `.c` / `_test.c`** (new): `msel_rect_overlap` + set ops + `MULTISEL_CAP`.
- **`build.sh`**: a `multiselecttest` target + `multiselect.c` on the four full-build lines.
- **`main.c`**: `AppState` fields; the `sel_*` wrappers + `object_is_selectable`; gesture
  changes in the board-view press handler; the marquee (update / board-local projection /
  draw / commit); the group-drag update + release; group file; group delete; the highlight
  fold-in. A small `board_ray_local` helper (Ray → board-local) for the marquee corners.

## Milestones (one spec / one plan)

- **M1 — Set + clicks:** the `multiselect` module, `AppState` fields + `sel_*` wrappers,
  shift-click toggle, click-to-single, multi-highlight, group delete. Result: working
  click-based multi-select + delete.
- **M2 — Marquee:** rubber-band rect (draw + board-local projection + live-preview highlight),
  commit replace/union, click-clears.
- **M3 — Group move + file:** drag-together (delta-applied) and file-onto-folder with the
  pre-drag-position restore (layout retention).

## Testing

- **Build gauntlet (all three, every task):** `./build.sh c89check`, `./build.sh`,
  `./build.sh metal`.
- **Unit suite `multiselect_test`:** `msel_rect_overlap` (overlap / edge-touch / disjoint /
  containment / unnormalized corners) and the set ops (add idempotent, remove compacts,
  toggle round-trip, contains, cap bound).
- **Human live-verify (board view), per milestone:**
  - M1: shift-click builds/removes a selection across notes+pictures+folders; all glow;
    Delete removes them all; single-select unchanged.
  - M2: drag a rectangle on empty board → covered cards glow live; release selects them;
    shift+rectangle adds; a plain empty click clears.
  - M3: drag a multi-selection → all move together keeping layout; drop on empty board saves;
    drop on a folder files them all and they appear on the target page in the same arrangement
    (navigate in to confirm); the target folder isn't one of the selected.
  - Both backends; no launch-time MSL break (no new shader).

## Risks

- **Gesture disambiguation** in the already-busy board-view press handler — the marquee
  (empty-board drag) vs group drag (selected-card drag) vs single drag vs double-click vs
  shift-toggle. The decision tree above is the contract; live-verify the feel (especially
  click-vs-drag slop and shift interactions).
- **Marquee projection** assumes board view is fronto-parallel (it is — the camera frames the
  board). Off-axis would skew the board-local rect; acceptable since marquee is board-view only.
- **Group delete** must iterate a snapshot of the set (scene mutation invalidates pointers,
  not handles). **Group file** must restore positions before re-tagging. **Use-after-realloc**:
  never hold a `SceneObject*` across `scene_add`/`scene_remove`.
- **Anchor sync**: keep the `sel_count==1 ⇒ selected_handle==sel[0]` invariant so single-item
  paths (resize, edit) keep working.
- No new shader ⇒ no MSL-twin risk.
