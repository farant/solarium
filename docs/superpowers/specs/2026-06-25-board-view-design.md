# Board View — Design Spec

**Date:** 2026-06-25
**Status:** Approved (brainstorm), pending implementation plan
**Author:** Fran + Claude

## Goal

Select a whiteboard and press **Enter** to "flip" into a **board view**: the camera
glides to frame the whole board head-on (filling the field of view to the board's
width *or* height — whichever shows the entire board — with a small margin), and the
mouse cursor unlocks so you can select / drag / resize / edit / create cards on the
board surface with the pointer, the way the cursor unlocks in the inventory screen.
**Esc** glides back to where you were standing.

This is a *view + input mode*, not new card mechanics: the on-board interactions
(select, drag-to-reposition, corner-resize a note, Enter-to-edit a note, Delete,
and `N` to spawn a note onto the board) already exist in the 3D world and are
driven today by the center crosshair. Board view reuses them, driven by the free
cursor instead.

## Decisions (from brainstorming)

- **Interaction scope:** arrange + edit + delete existing cards/notes, **plus create
  new notes (`N`)** onto the board. Out of scope for v1: pulling cards off the board
  out to the world/bag, adding from the bag, multi-select / box-select.
- **Transition:** smooth glide (~0.35 s) in on enter and back out on exit.
- **Entry:** a board is **selected** (click) and **Enter** is pressed.
- **Exit:** **Esc**. Clicking empty board space **deselects** (does not exit).
- **Boards:** any board (`mesh_ref == "board"`), wall-mounted or free-standing —
  framing is derived from the board's own world position + facing.

## Non-Goals

- No new shader (reuses `draw_mesh` / `wtext`) → **no MSL twin**.
- No new camera *mode* enum value — board view is a state flag plus input gates,
  with the camera left in `CAMERA_WALK` and movement/look suppressed.
- No persistence: board view is transient UI state, never written to `scene.stml`.

## Architecture

### State (in `AppState`)

```c
sol_u32 board_view;          /* board being viewed; 0 = not in board view */
sol_bool board_view_was;     /* edge-detect for the cursor toggle */
/* glide tween */
vec3  bv_from_pos,  bv_to_pos;
float bv_from_yaw,  bv_to_yaw;
float bv_from_pitch,bv_to_pitch;
float bv_t;                  /* 0..1 eased progress; >=1 means settled. INIT TO 1.0 */
float bv_dir;                /* +1 gliding to the board, -1 gliding back out */
/* the pose to restore on exit (where you were standing) */
vec3  bv_return_pos;
float bv_return_yaw, bv_return_pitch;
```

Mirrors how `inv_open` / `inv_was_open` live. All transient.

### Framing math — new pure camera helper

`camera_focus` already frames a surface head-on, but it fits by **height only** and
*sets* the camera immediately; it also has existing callers (the book reader at
`main.c:3998`, `camera_test.c`). So **add a new pure helper that returns a target
pose** without mutating live state:

```c
/* camera.h / camera.c */
typedef struct { vec3 pos; float yaw, pitch; } CameraPose;

/* Frame a flat upright surface head-on, fitting BOTH its half-width and
   half-height into the FOV (whichever needs the greater standoff wins), with a
   margin multiplier. `normal` points toward the viewer (the front face). */
CameraPose camera_frame_pose(vec3 center, vec3 normal,
                             float half_w, float half_h,
                             float fov, float aspect, float margin);
```

Implementation:

```
tan_v      = tanf(fov * 0.5f)
dist_h     = half_h / tan_v
dist_w     = half_w / (tan_v * aspect)
dist       = (dist_h > dist_w ? dist_h : dist_w) * margin
n          = normalize(normal)
pose.pos   = center + n * dist
dir        = -n                        (camera looks back at the surface)
pose.pitch = asinf(dir.y)              (0 for an upright board)
pose.yaw   = atan2f(dir.z, dir.x)      (matches camera_focus's convention)
```

`margin = 1.1` (a bit tighter fill than `camera_focus`'s 1.3; tunable constant
`BOARD_VIEW_MARGIN`). Unit-testable in `camera_test.c`: a board facing +Z frames
the eye on the +Z axis at the expected distance; a wide (landscape) board lands
farther back than a tall one of equal height.

### Board geometry source

The board's world center, facing normal, and half-extents come from the existing
`board_world_corners(scene, handle, corners[4], &u)` + `board_yaw`:

- `center` = average of the four world corners (robust for upright boards).
- `normal` = `(sinf(yaw), 0, cosf(yaw))` — the front face (where a viewer stands),
  matching the normal used by `picture_move_pick`.
- `half_w` = `mesh_ref_param("board", …, "w") * 0.5`, `half_h` = `… "h" * 0.5`.

### Entry / exit (key handler, `main.c` ~14455–14470)

- **Enter**, `action == GLFW_PRESS`, `selected_handle != 0`, object is a board
  (`mesh_ref == "board"`), and **not already in board view** → call
  `board_view_enter(st)`: store the return pose, compute the framed target pose via
  `camera_frame_pose`, set `bv_from_* = live camera`, `bv_to_* = target`, `bv_t = 0`,
  `bv_dir = +1`, set `st->board_view = selected_handle`. This branch is checked
  **before** the existing `reader_open` fallback so a board no longer mis-opens the
  book reader.
- Inside board view, **Enter** on a selected **note** still routes to
  `note_edit_begin` (unchanged — that branch already gates on `KIND_NOTE`).
- **Esc**, `action == GLFW_PRESS`: if `st->board_view != 0` → `board_view_exit(st)`:
  `bv_from_* = live camera`, `bv_to_* = return pose`, `bv_t = 0`, `bv_dir = -1`,
  and clear `st->board_view = 0` (the glide-back still runs from the stored tween).
  This is checked **before** the existing reader-close / window-close Esc handling.

### Cursor unlock (read_input, near the inventory toggle ~9270–9277)

Add a board-view edge mirroring the inventory toggle:

```c
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

### Movement / look suppression (read_input)

Board view differs from inventory: inventory **early-returns** at the modal gate
(it replaces interaction with screen-space tiles), but board view must **fall
through** so the existing 3D pick / drag / resize handlers run from the cursor.
So board view is **not** added to the `9300` early-return gate. Instead, define a
**board-view-active** predicate that also covers the outbound glide (so movement /
look stay frozen until the camera has fully settled back, never fighting the tween):

```c
sol_bool bv_active = (sol_bool)(st->board_view != 0 || st->bv_t < 1.0f);
```

- After movement is polled (~9357–9362), if `bv_active`, zero
  `forward/back/left/right/up/down`.
- Keyboard look (~9394–9397): skip when `bv_active` (no effect anyway since
  movement is gone, but keeps arrow keys free for future use).
- Mouse look (~9424): change the apply condition to
  `… && !st->editor.active && !bv_active`, so moving the free cursor never rotates
  the framed camera, and the glide-back is never perturbed by mouse motion.

`N` (spawn note), select, drag, resize, and Delete are all polled / handled *after*
the gate and are left to run — they already target the board under the pick ray.

### Cursor-driven picking (`pick_ray`, ~3016; selection `pick_at`)

`pick_ray` currently reads the real cursor only in `CAMERA_ORBIT`. Extend the
condition to also read it in board view:

```c
if ((st->camera.mode == CAMERA_ORBIT || st->board_view != 0) && ww > 0 && wh > 0) { … }
```

The non-editor selection path that calls `pick_at` with screen-center NDC in
first-person must, in board view, pass the **cursor NDC**
(`2*mx/ww - 1`, `1 - 2*my/wh`) instead — same NDC the editor path already computes.
(The drag/resize handlers use `pick_ray`, so extending `pick_ray` covers them.)

### Glide tween (advanced once per frame, in the update path)

While `bv_t < 1`, advance `bv_t += dt / BOARD_VIEW_GLIDE_S` (clamp to 1), ease with
smoothstep `e = bv_t*bv_t*(3 - 2*bv_t)`, and write the camera:

```
camera.pos   = lerp(bv_from_pos, bv_to_pos, e)
camera.yaw   = bv_from_yaw   + shortest_angle(bv_to_yaw - bv_from_yaw) * e
camera.pitch = bv_from_pitch + (bv_to_pitch - bv_from_pitch) * e
```

`shortest_angle` wraps the yaw delta into `[-π, π]` so the glide never spins the
long way around. `BOARD_VIEW_GLIDE_S = 0.35`. The tween advances whenever `bv_t < 1`,
independent of `board_view` (so the outbound glide keeps running after `board_view`
is cleared). `bv_t` is initialized to `1.0` at `AppState` setup so no tween runs at
startup. The cursor unlocks at the *start* of the inbound glide (interaction is live
immediately; the easing is purely visual). On exit, `bv_active` (above) keeps
movement/look frozen until `bv_t` reaches 1, at which point normal first-person
resumes with the camera at the restored return pose.

### Robustness

- If the viewed board is deleted while in board view (`scene_get(board_view) == 0`),
  call `board_view_exit` (snap or glide back) — mirrors the `move_board` / `resize_board`
  vanish guards.
- Entering board view while carrying / placing / in the editor / palette / inventory
  is disallowed (guard `board_view_enter` on those flags being clear), since Enter is
  already claimed by those modes.

## Data Flow

```
click board → selected_handle = board
Enter       → board_view_enter: store return pose; target = camera_frame_pose(board);
              begin inbound glide; board_view = handle; cursor unlocks next frame
per frame   → glide tween eases camera to target; movement/look suppressed;
              pick_ray + selection read the cursor
mouse/keys  → existing select / drag (board_pin_pos) / resize / Enter-edit / Delete / N
Esc         → board_view_exit: begin outbound glide to return pose; board_view = 0;
              cursor re-locks; once settled, normal first-person resumes
```

## File Touch List

- `camera.h` / `camera.c` — `CameraPose` + `camera_frame_pose` (pure).
- `camera_test.c` — unit tests for `camera_frame_pose` (fit-by-height, fit-by-width,
  margin, +Z-facing eye position).
- `main.c` — `AppState` board-view fields; `board_view_enter` / `board_view_exit`;
  cursor toggle; movement/look gates; `pick_ray` + selection cursor NDC; glide tween
  in the update path; Enter/Esc interception in the key handler.

## Testing

- **Unit (pure):** `camera_frame_pose` distance/pose for tall vs. wide boards + margin.
- **Build gauntlet:** GL (`-Wall -Wextra -Werror -pedantic`), Metal link, `cameratest`.
- **Human live-verify (GUI):** enter/exit glide framing for a portrait and a landscape
  board, on a wall and free-standing; cursor-driven select/drag/resize/edit/delete;
  `N` spawns a note under the cursor onto the board; Esc returns to the prior pose;
  movement/look correctly frozen while in board view.
