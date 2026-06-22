# Whiteboard on the Wall — Design

**Goal:** While carrying a whiteboard (`E`), aim at a room wall and place it — the
board mounts flat on that wall, centered on the point you're looking at, clamped
to stay fully on the wall, facing into the room.

**Context:** The carry system already has two aim-branches: a folder tablet aims
at a wall to *descend* (open it as a sub-room, `descend_wall_aim` →
`descend_plant`), and a file card aims at furniture to *file* (re-parent onto it).
This adds a third: a whiteboard (`mesh_ref "board"`) aims at a wall to *mount*. It
reuses the wall-ray machinery in `descend.c` and the re-parent drop path in
`cmd_carry_toggle` — no new shader, no new render path, no new AppState carry
fields. Resize is explicitly a **separate later pass**, not in scope here.

---

## Architecture

One new headless geometry function plus three small `main.c` wirings:

- **`descend_wall_mount`** (`descend.c`) — the height-unconstrained sibling of
  `descend_wall_aim`: ray vs. the 4 interior walls → nearest wall + the clamped,
  flush mount **center**.
- **`carry_update`** gains a `"board"` branch that computes the mount and sets the
  existing `file_*` fields (so the existing drop re-parents it).
- **`cmd_carry_toggle`** pickup detaches a wall-mounted board (the furniture-detach
  pattern, extended to boards).
- The existing **drop branch** in `cmd_carry_toggle` is reused unchanged.

## The geometry — `descend_wall_mount` (headless, unit-tested)

```c
/* Aim `ray` at the room's 4 interior walls to MOUNT a flat board (half-width
   w_half, half-height h_half, thickness t) flush on the nearest wall. Unlike
   descend_wall_aim (door height, returns offset) this is height-UNCONSTRAINED
   and returns the board's world-space CENTER: the ray/wall hit CLAMPED so the
   board stays fully on the wall (along-wall within +/-(wall_half - w_half),
   vertically within [floor_y + h_half, ceil_y - h_half]) then pushed off the
   wall surface by t/2 so the back sits flush. Returns 1 + *out_wall
   (ROOM_WALL_*) + *out_center; 0 if no wall is hit or the board is wider/taller
   than the wall. */
int descend_wall_mount(RoomRect r, Ray ray, float ceil_y,
                       float w_half, float h_half, float t,
                       int *out_wall, vec3 *out_center);
```

Implementation mirrors `descend_wall_aim`'s wall loop (same 4 planes + inward
normals + `ray_vs_plane`, nearest forward hit), but: no door-height filter
(accept any hit with `floor_y <= hit.y <= ceil_y`); clamp the along-wall coord to
`+/-(wall_half - w_half)` and the height to `[floor_y + h_half, ceil_y - h_half]`;
reject (return 0) if either usable span is negative (board bigger than wall); set
`*out_center = clamped_hit + inward_normal * (t * 0.5f)`.

## main.c wiring

### `carry_update` — the board branch
After the folder branch and the card-filing branch, add a branch for a carried
board (`o->mesh_ref == "board"`, not a folder):

1. `room = descend_room_at(scene, camera.pos)`; if 0, skip (carry normally).
2. `r = editor_room_rect(scene, room)`; `ceil_y = r.floor_y + roomHeight` where
   `roomHeight` is the room shell's `"room"` mesh `"h"` param (a small scan, the
   way `route.c`'s `room_half` reads `"w"/"d"`).
3. Board half-extents from its `"board"` params: `w_half = w/2`, `h_half = h/2`,
   `t` the thickness.
4. `ray = {camera.pos, camera_forward()}`. If
   `descend_wall_mount(r, ray, ceil_y, w_half, h_half, t, &wall, &center)`:
   - `yaw = WALL_YAW[wall]` where `WALL_YAW[N,E,S,W] = {0, -90, 180, 90}` degrees
     (so the board's +Z front faces the wall's inward normal; +Z front is the
     spawn convention — `cmd_mint_whiteboard`'s `"board +Z looks back at you"`).
   - **Origin offset for bottom-origin geometry:** `make_card` builds the board
     with its origin at the bottom-center (y in `[0,h]`), so the geometric center
     is local `(0, h_half, 0)`. The object origin in world is
     `P = center - (0, h_half, 0)` (yaw rotates only about Y, so local-up == world-up).
   - Set the carry preview + drop target exactly like the card-filing branch:
     `st->file_aim = TRUE; st->file_target = room;`
     `st->file_rot = quat_from_axis_angle(+Y, radians(yaw));`
     `st->file_local = scene_world_to_local(scene, room, P);`
     and preview `o->pos = scene_world_to_local(scene, o->parent, P); o->rot = st->file_rot;`
   - `return;` (mount preview wins this frame, like the other aim branches).

### `cmd_carry_toggle` — drop (reused, unchanged)
The existing branch already does the mount:
```c
} else if (st->file_aim && st->file_target != 0 && scene_get(...) != 0) {
    o->parent = st->file_target;   /* the room */
    o->pos    = st->file_local;    /* room-local P */
    o->rot    = st->file_rot;      /* the wall yaw */
    ...
}
```
The board becomes a child of the room (rides it, inherits its workspace), like a
filed card rides its shelf. (Its `printf` may be generalized from "filed onto
furniture" to "placed" — cosmetic.)

### `cmd_carry_toggle` — pickup detach
When picking up (`E` with nothing carried), the existing code detaches an object
parented to furniture by capturing its world pose and clearing `parent`. Extend
that detach to fire for a **board with any parent** (a wall-mounted board is a
room child), so re-picking a mounted board lifts it cleanly back into the hand.

## Data flow

carry a board → `carry_update` each frame: in a room + aimed at a wall →
`descend_wall_mount` → clamped center → preview pose + `file_*` set. Press `E` →
`cmd_carry_toggle` drop branch re-parents onto the room at the mount pose +
`scene_save`. Not in a room / not aimed at a wall → `file_aim` stays false, the
board places in front as today. Re-pick with `E` → detach to world.

## Error handling

- No room / no wall hit / board bigger than the wall → `descend_wall_mount`
  returns 0 (or the room lookup fails) → no mount aim, normal carry/place.
- Degenerate `roomHeight` (missing shell) → ceil falls back to `floor_y + 3.0`
  (the default room height) so the clamp still has a sane span.

## Testing

- **`descend_test.c`** gains `descend_wall_mount` cases (headless, ASan/UBSan):
  a centered horizontal aim at the north wall returns `ROOM_WALL_N` with
  `center.z == (cz - hd) + t/2` (flush + pushed out) and `center` on the wall;
  a high aim clamps `center.y` to `ceil_y - h_half`; a corner aim clamps the
  along-wall coord to `wall_half - w_half`; a board wider than the wall returns 0.
- **Live verify (Fran):** carry a board, aim high and low on a wall — it previews
  flat and follows the aim, clamped on the wall edges; `E` mounts it flush + upright
  facing the room; it persists across reload; re-picking with `E` lifts it back.

## Scope / non-goals (YAGNI)

In: whiteboards (`"board"`) only, via the carry flow, on the 4 axis-aligned room
walls, mounted where you aim (clamped). Out: **resize** (separate next pass),
angled/curved walls, mounting on furniture or non-board objects, collision changes
(boards stay non-solid), snapping/aligning multiple boards.

## Files touched

- `descend.h` / `descend.c` — `descend_wall_mount`.
- `descend_test.c` — its unit cases.
- `main.c` — `carry_update` board branch; `cmd_carry_toggle` pickup-detach
  extension (drop branch reused as-is, printf optionally generalized).
