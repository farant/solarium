# Image Pictures on Boards — Design Spec

**Date:** 2026-06-25
**Status:** Approved (brainstorm), pending implementation plan
**Author:** Fran + Claude

## Goal

When an image lands on a whiteboard (`mesh_ref "board"`) — by **either** gesture — it becomes a **resizable, draggable image "picture" widget**, exactly like dropping an image onto a wall. Today the two gestures disagree and neither is fully there:

- **E-carry** an image onto a board → already drops a `"picture"`, **but** it can't be resized on a board (only slid).
- **Click-drag** an image file card onto a board → pins a filename **alias card** (the image isn't shown at all).

## Decisions (from brainstorming)

- **Both gestures behave the same:** an image on a board → a resizable picture.
- The **file tablet snaps home** (§1.3 — a mirror record never leaves its room); the picture is the visual left on the board, content = the image path.
- Resize is **aspect-locked** (preserves the image proportions), like wall pictures.
- **No new shader** — pictures use the existing lit-albedo `draw_mesh` path → no MSL twin.

## Non-Goals

- Walls already work — unchanged.
- Non-image file cards dragged onto a board still pin as alias cards (unchanged).
- No new persistence format — a board picture is an ordinary `KIND_PLAIN` `"picture"` scene object (parent = the board), persisted/reloaded like a wall picture.

## Background (current state)

- **Carry release** (main.c ~8366, the `picture_aim` branch): on drop, spawns a `"picture"` parented to `picture_target` (a room/wall OR a board) — `image_load` → `image_fit_box(…,1.6,1.2,&pw,&ph)` → `scene_add` mesh_ref `"picture"` → `scene_content_set(path)` → params `{pw,ph,0.03}`. The image card returns to `carry_prev_parent`/`carry_origin`. Board AND wall both set `picture_aim` (aim handler ~8918 board / ~8943 wall), so **carry already makes a board picture**.
- **Drag release onto a board** (main.c ~9993): a dragged `KIND_FILE`/`KIND_FOLDER` card → snaps home, then `scene_add` a **`KIND_ALIAS` `"card"`** pinned via `board_pin_pos` (a filename tablet — NOT the image).
- **Move (slide)** a board-parented picture works: the move handler's board branch (main.c ~9760) writes board-local `pos` (x centered, clamped to `±(bw/2−pw/2)`; y in `[0, bh−ph]`).
- **Resize** is gated by `board_is_mounted` (main.c) — TRUE only when the picture's parent has `room_type` (a wall). A board-parented picture (parent = the board, no `room_type`) returns FALSE → no corner handles, no resize. The resize handler's non-note `else` branch assumes a room (`editor_room_rect(resize_room)`), so it can't resize a board picture either.

## Architecture

### Part 1 — drop → picture (the click-drag path)

1. **Factor a shared picture-spawn helper.** Extract the carry-release picture build into:
   ```c
   /* spawn a "picture" of `content` parented to `parent` at local `pos`/`rot`,
      sized to the image's aspect. Returns the new handle (0 on failure). */
   static sol_u32 spawn_image_picture(AppState *st, sol_u32 parent,
                                      vec3 pos, quat rot, const char *content);
   ```
   It does `image_load` → `image_fit_box(img.w,img.h,1.6,1.2,&pw,&ph)` → `scene_add(parent,…)` → `scene_mesh_ref_set("picture")` → `scene_content_set(content)` → `scene_mesh_params_set({pw,ph,0.03})`, then `scene_resolve_meshes` + `apply_kind_materials`. The carry release is refactored to call it (behavior-preserving).

2. **Drag-release: image file → board → picture.** In the drag-release-onto-board handler (main.c ~9993, the `KIND_FILE`/`KIND_FOLDER` → board branch), when `reader_is_image_path(o->content)` is true, snap the file card home (as now) and call `spawn_image_picture(st, board, board_local_pos, identity, content)` **instead of** adding a `KIND_ALIAS` card. The board-local position comes from `board_pin_pos(&st->scene, board, newhandle, blocal, 0.0f, -0.5f*ph)` (the existing pin math; bottom-center origin). Non-image files keep the existing alias-card path.

### Part 2 — picture-on-board resize (the missing half)

3. **A resizability predicate for board pictures.** Add:
   ```c
   /* a "picture" parented to a whiteboard (not a wall): resizable in board-local space */
   static sol_bool picture_on_board(Scene *s, sol_u32 h);   /* mesh_ref=="picture" && object_is_board(parent) */
   ```
   Extend the three gates that currently read `board_is_mounted(sel) || note_resizable(sel)` to also accept `picture_on_board(sel)`:
   - the resize-press corner pick (main.c ~9721),
   - the resize-press second site (main.c ~10067),
   - the corner-handle render (main.c ~13832).
   `resize_corner_pick` itself already works for any picture (it uses `board_world_corners`), and sets `resize_room = o->parent` (= the board).

4. **Board-local resize branch in the resize handler.** The resize handler currently splits: `KIND_NOTE` (own-plane resize) / room-vanished / `else` (wall resize via `editor_room_rect`). Insert a branch **before the wall `else`**: if the resize target is a board picture (`object_is_board(st->resize_room)`), do a **board-local, aspect-locked** resize:
   - ray vs the picture's own plane (normal from `board_yaw`) → `hit` (world).
   - `aspect = pw/ph` (the picture's params); `board_resize_corner(st->resize_anchor, hit, st->resize_u, 0.3f, aspect, &nw, &nh, &origin)` — `0.3f` is the same minimum-size the wall-picture resize passes, so resize feels identical.
   - clamp `nw ≤ bw`, `nh ≤ bh` (board can't be smaller than its picture).
   - rebuild the registry-shared `"picture"` mesh at the new size — the **same dance** wall-picture resize uses: `mesh_asset_key(o,oldkey)` → `scene_mesh_params_set({nw,nh,pt})` → `asset_release(&g_mesh_assets,oldkey)` → re-fetch `o` → `memset(&o->mesh,0,…)` → `o->pos = scene_world_to_local(&st->scene, o->parent, origin)` → `scene_resolve_meshes`. Since `o->parent` is the board, the write lands in board-local coords automatically.
   - optionally re-clamp the board-local `pos.x`/`pos.y` to the board face (the move handler's clamp), so a resized picture stays on the board.

### What's already fine (no change)

- **Move/slide** a board picture (the move handler's board branch) — works; "draggable" is done.
- **Persistence** — a board picture is a `"picture"` object with `content` + params, parent = the board; `scene_resolve_meshes` rebuilds it + loads albedo on reload, like a wall picture.
- **Rendering** — the picture albedo path (main.c ~11261, `mesh_ref=="picture"` → its image texture) is parent-agnostic.

## Data Flow

```
drag image FILE card → board release:
    file card snaps home (drag_prev_*)  +  spawn_image_picture(board, board_local, content)
carry image card → board release (existing):
    image card returns home (carry_prev_*)  +  spawn_image_picture(board, picture_local, content)
both → a "picture" parented to the board (content + aspect-sized params)
select the board picture → corner handles show (picture_on_board) →
    corner-drag → board-local aspect-locked resize (rebuild shared mesh) ;
    body-drag → board-local slide (existing)
save/reload → picture rebuilt from content + params (record stays home)
```

## File Touch List

- `main.c` only:
  - `spawn_image_picture` helper + refactor the carry release to use it. (Part 1)
  - drag-release-onto-board: image → `spawn_image_picture` instead of an alias card. (Part 1)
  - `picture_on_board` predicate; extend the 3 resize gates. (Part 2)
  - the board-local resize branch in the resize handler. (Part 2)

## Testing

- **Build gauntlet (ALL THREE):** `./build.sh c89check` (strict C89 — the board-view regression lesson), `./build.sh`, `./build.sh metal`.
- **Human live-verify:**
  1. **Drag** an image file card onto a board → a resizable image picture appears; the file tablet returns to its shelf.
  2. **Carry** (E) an image onto a board → same result.
  3. **Resize** the board picture by a corner — aspect-locked, stays on the board; **slide** it by the body — still works.
  4. **Save/reload** (or revisit) → the board picture persists; the file record is still home in its room.
  5. A **non-image** file card dragged onto a board still pins as a filename alias card (unchanged).
  6. Wall pictures + furniture filing unaffected.

## Risks

- The board-local resize reuses `board_resize_corner` + the registry-shared-mesh rebuild dance verbatim from the wall path, so the main risk is the **board-local vs wall-local coordinate write** — mitigated because `o->parent` is the board and `scene_world_to_local(o->parent, origin)` already yields board-local (the move handler proves the convention).
- No new shader → no MSL twin risk.
