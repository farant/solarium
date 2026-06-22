# Whiteboard Resize (Corner Handles) — Design

**Goal:** When a wall-mounted whiteboard is selected, four corner handles appear;
grabbing a corner and dragging resizes the board in place on the wall (opposite
corner anchored), live, and persists. Off-wall boards don't resize.

**Context:** The mount pass put a board flat on a wall (`mesh_ref "board"`,
re-parented to the room, facing yaw from `descend_wall_mount`). This pass adds
in-place resize via the engine's existing **crosshair drag** model: in
first-person the mouse is captured, so "click and drag" means aim the
center-screen crosshair at a thing, hold the button, and look around so it
follows (the same machinery `drag_handle` already uses to move objects). The
resize geometry is one new headless function; the rest is `main.c` wiring into
the existing press/drag/release path and a small handle render. No new shader.

---

## Architecture

- **`board_resize_corner`** (`descend.c`, headless + unit-tested) — anchor corner
  + dragged point + wall axis → new `w`, `h`, bottom-center origin.
- **`board_is_mounted`** predicate (`main.c`) — `mesh_ref "board"` whose parent
  carries `room_type` (it was mounted onto a room).
- **Corner-handle hit-test + resize-drag** woven into the existing first-person
  LMB press/drag/release in `main.c`, gated so it only fires for a selected
  mounted board and takes priority over starting a carry-move.
- **Handle render** — four small quads at the corners when a mounted board is
  selected, via the existing lit-albedo `draw_mesh` path.

## The geometry — `board_resize_corner` (headless, unit-tested)

```c
/* Resize a wall-mounted board by dragging one corner. `anchor` is the OPPOSITE
   (fixed) corner in world space; `dragged` is the grabbed corner's new world
   point, already projected onto the wall plane; `u` is the wall's horizontal
   in-plane unit axis (vertical is world-up). Returns the new width/height
   (floored at `min_size`) and the board's bottom-center origin in world space.
   The board spans the rectangle from `anchor` to the (min-clamped) dragged
   corner. */
void board_resize_corner(vec3 anchor, vec3 dragged, vec3 u, float min_size,
                         float *out_w, float *out_h, vec3 *out_origin);
```

Logic: `d = dragged - anchor`; `du = dot(d, u)`, `dv = dragged.y - anchor.y`;
`w = |du|`, `h = |dv|`, each floored at `min_size`; signs `su`/`sv` preserve the
drag direction. Origin `P = anchor + u*(su*w/2)` (centered along the wall axis),
then `P.y = (sv >= 0) ? anchor.y : anchor.y - h` (the rectangle's bottom). `P`
keeps `anchor`'s depth into the wall (flush), because `u` is horizontal and the
y is set explicitly.

## Wall axes from the board

A mounted board stores its facing as a yaw quaternion. `yaw = 2*atan2(q.y, q.w)`
→ inward normal `n = (sin yaw, 0, cos yaw)`, horizontal wall axis
`u = (cos yaw, 0, -sin yaw)`, vertical = world-up. The four corners from the
board's bottom-center origin `P`, width `w`, height `h`:
`P ± (w/2)u` (bottom two) and `P ± (w/2)u + h*up` (top two).

## Interaction flow (main.c, first-person)

1. **Handles shown:** while `selected_handle` is a mounted board, render a small
   quad at each of its 4 corners (flush on the wall, slightly proud).
2. **Press:** on LMB-down in first-person, BEFORE the normal pick/drag-begin, if a
   mounted board is selected, ray-test the crosshair `pick_ray` against the 4
   corner world points (nearest within a pixel/▵-distance threshold). On a hit:
   enter resize mode — store `resize_board`, the **anchor** = the opposite
   corner, and the wall axis `u` (+ the parent room, for clamping). This
   suppresses the carry-move that a press on the board would otherwise start.
3. **Drag:** each frame while LMB held and `resize_board != 0`: project `pick_ray`
   onto the board's wall plane (`ray_vs_plane` with point `P0` on the wall and
   normal `n`), clamp the hit within the wall bounds (along `u` to the wall's run
   span from the room rect; in y to `[floor, ceil]`), call
   `board_resize_corner`, then `scene_mesh_params_set(board, {w,h,t})` + set
   `board.pos` (room-local origin) + `scene_resolve_meshes` to rebuild live.
4. **Release:** `resize_board = 0`; `scene_save`.

## State (AppState)

`sol_u32 resize_board;` (0 = not resizing), `vec3 resize_anchor;`,
`vec3 resize_u;`, `sol_u32 resize_room;` — mirrors the `drag_*` fields' style.

## Apply + persistence

Resize edits the board's `w`/`h` mesh params and `pos`; `scene_resolve_meshes`
rebuilds the mesh from the new params each drag frame (cheap — one small mesh).
On release `scene_save` writes `scene.stml`, so the size persists like any param.
The board stays a child of its room (re-parenting is untouched).

## Error handling

- Min size floor (`0.3 m`) so a board can't collapse to nothing.
- Dragged point clamped within the wall (run span + floor/ceil) so the board
  can't grow past the wall or off its edges — the same containment the mount uses.
- If the board's parent room can't be found mid-drag (deleted), end resize.

## Testing

- **`descend_test.c`**: `board_resize_corner` cases — drag a corner up-right →
  `w`/`h` equal the deltas and origin is the bottom-center; drag down-left →
  origin moves and bottom is the lower y; a tiny drag floors at `min_size`; the
  anchor's wall-depth is preserved.
- **Live verify (Fran):** select a mounted board → handles appear; grab a corner
  and look around → the board grows/shrinks live with the opposite corner pinned,
  clamped to the wall; release persists; reload keeps the new size; an off-wall
  board shows no handles.

## Scope / non-goals (YAGNI)

In: wall-mounted whiteboards only, four corner handles, crosshair drag, free
aspect (w and h move together via the corner), min-size + wall clamp, live
rebuild + save. Out: edge/midpoint handles, rotation, a free mouse cursor,
resizing off-wall or non-board objects, aspect-lock modifier, numeric entry.

## Files touched

- `descend.h` / `descend.c` — `board_resize_corner`.
- `descend_test.c` — its unit cases.
- `main.c` — `board_is_mounted` predicate; `AppState` resize fields; corner-handle
  hit-test + resize-drag in the LMB press/drag/release path; handle render in the
  draw loop.
