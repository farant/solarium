# Map View — Design

**Date:** 2026-06-30
**Status:** Approved, ready for plan
**Related:** mirrors Board View (`board_view_*`); operates on world-map objects (`mesh_ref=="map"`, the World Map Boards feature).

## Why

Board view lets you select a whiteboard and press Enter to glide the camera square-on to it. Maps deserve the same: select a map, press Enter, the camera orients to frame it for a clean full-screen look; Esc glides back. This is the camera half of board mode applied to maps. It is **view-only for now** — and is deliberately built as its own self-contained mode so that **pin/marker management** (the next feature) has a clean home to land in, rather than being bolted onto board view's card machinery.

## Decisions (from the design dialogue)

- **View-only framing now.** No cursor unlock, no map interaction. Pin management comes next as a separate feature on top of this mode.
- **Any orientation.** Frame the map from its *actual* world facing: a vertical wall/whiteboard map gets a head-on horizontal look; a flat table/floor map gets a top-down "lean over the table" look. The camera is computed from the map's real world transform, not a wall-yaw assumption.
- **Parallel mode, not a generalization of board view.** A new `st->map_view` handle alongside `st->board_view`. Board view's card stack (cursor unlock, multi-select, marquee, paging, cut/paste) is keyed on `board_view != 0` across dozens of sites; generalizing risks leaking that behavior onto maps. Map view stays isolated.
- **Reuse the glide scratch.** Map view reuses the existing `bv_*` camera-glide fields and the smoothstep advance. Board view and map view are mutually exclusive, so sharing the scratch is safe and avoids duplicating the easing math.
- **Cursor stays locked.** View-only needs no pointer. (Board view unlocks the cursor only to arrange cards.)

## Architecture

### State (AppState)

One new field, mirroring `board_view`:

```c
sol_u32 map_view;   /* map being viewed; 0 = not in map view */
```

Reuses the existing glide scratch: `bv_from_pos/yaw/pitch`, `bv_to_pos/yaw/pitch`, `bv_return_pos/yaw/pitch`, `bv_t`, `bv_dir`.

### New functions (placed beside `board_view_*` in main.c)

- **`static int map_view_pose(AppState *st, sol_u32 map, CameraPose *out)`** — thin glue. Returns 0 (leaving `*out` untouched) unless the handle resolves to a `mesh_ref=="map"` object. Otherwise:
  - `w`/`h` from `mesh_ref_param("map", ...)`; `half_w = w*0.5f`, `half_h = h*0.5f`.
  - `q   = scene_world_rotation(&st->scene, map)` (world quaternion).
  - `p0  = object_world_pos(&st->scene, map)` (world position of the local origin = the quad's bottom-center, per `make_map_quad`).
  - `center = p0 + quat_rotate(q, (0, half_h, 0))` (bottom-origin → mid-height).
  - `normal = quat_rotate(q, (0, 0, 1))` (the textured face).
  - `up     = quat_rotate(q, (0, 1, 0))` (the quad's up edge).
  - `aspect` from `fb_width/fb_height` (fallback 16:9, as in `board_view_pose`).
  - `*out = camera_frame_pose_up(center, normal, up, half_w, half_h, st->camera.fov, aspect, BOARD_VIEW_MARGIN)`; return 1.

- **`static int map_view_enter(AppState *st)`** — gated exactly like `board_view_enter`: returns 0 if `map_view != 0`; selection must resolve to a `mesh_ref=="map"`; bails if `carried != 0 || place_active || editor.active || palette.open || inv_open || edit_handle != 0 || reader_state != READER_IDLE || board_view != 0`. On success: stash `bv_return_*` from the current camera, set the forward glide (`bv_from_* = camera`, `bv_to_* = pose`, `bv_t = 0`, `bv_dir = 1`), set `map_view = selected_handle`, clear `selected_handle`.

- **`static void map_view_exit(AppState *st)`** — returns early if `map_view == 0`. Sets the reverse glide (`bv_from_* = camera`, `bv_to_* = bv_return_*`, `bv_t = 0`, `bv_dir = -1`) and `map_view = 0`. No card/marquee/sel cleanup (view-only).

- **Vanish guard** — in the existing per-frame glide advance (`board_view_update`), add a sibling check beside the board one: if `map_view != 0 && scene_get(&st->scene, map_view) == 0`, call `map_view_exit` (map deleted → glide out). The shared `bv_t` glide already animates the camera for both modes.

### The any-orientation pose math (pure, in camera.c)

A new pure function beside `camera_frame_pose`, headless-testable (camera.c is GL-free):

```c
CameraPose camera_frame_pose_up(vec3 center, vec3 normal, vec3 up_axis,
                                float half_w, float half_h,
                                float fov, float aspect, float margin);
```

- Distance fit identical to `camera_frame_pose`: `dist = max(half_h/tanv, half_w/(tanv*aspect)) * margin`, where `tanv = tan(fov*0.5)`. Camera sits at `center + n*dist` (n = normalized normal), looking back along `-n`.
- **Vertical map** (`|dir.y|` below a threshold, e.g. `0.999`): identical to `camera_frame_pose` — `pitch = asin(dir.y)`, `yaw = atan2(dir.z, dir.x)`. Correct for a wall map facing any compass direction.
- **Flat map** (`|dir.y|` at/above threshold — normal ≈ ±WORLD_UP, the degenerate top-down case): clamp pitch just shy of vertical (`sign(dir.y) * MAX_FRAME_PITCH`, `MAX_FRAME_PITCH ≈ 89° in radians`) so the `WORLD_UP`-based `look_at` stays well-defined, and set `yaw = atan2(up_axis.z, up_axis.x)` so the map's top edge lands toward screen-top.

`board_view_pose` keeps calling the existing `camera_frame_pose` unchanged — the new function is additive.

**Known limitation (accepted):** the engine's yaw/pitch + `WORLD_UP` camera has no roll, so a flat map is framed ~1° off true straight-down rather than a perfect orthographic top-down. Imperceptible for a view-only look, and the right tradeoff vs. adding camera roll.

### Input integration (the freeze)

- `read_input` `bv_active` (the walk/look freeze flag): extend to `(st->board_view != 0 || st->map_view != 0 || st->bv_t < 1.0f)`. This suppresses WASD movement and keyboard/mouse look while framed and during the glide.
- **No cursor toggle for map view** — the cursor stays `GLFW_CURSOR_DISABLED`. The board-view cursor-unlock edge-detect block stays keyed on `board_view` only.
- Tab toggle gate (first-person ↔ orbit): add `&& !st->map_view` so Tab is inert while framed (matches the existing `!st->board_view`).

### Enter / Esc dispatch (on_key)

- **Enter** (the "enter the thing" handler): add a branch before the `reader_open` fallthrough —
  `else if (o->mesh_ref && strcmp(o->mesh_ref, "map") == 0) { if (st->map_view == 0) map_view_enter(st); }`.
  (Today a map falls through to `reader_open`, which silently ignores unreadable kinds — a clean no-op to take over.)
- **Esc**: extend the ladder to —
  1. `reader_state != READER_IDLE` → `reader_close`
  2. else `map_view != 0` → `map_view_exit`
  3. else `board_view != 0` → `board_view_exit`
  4. else `bv_t < 1.0f` → absorb (no-op; a mode's glide-out is mid-flight)
  5. else → close the window.

  Step 4 is the new explicit absorb: after `map_view_exit`, both `map_view` and `board_view` are 0 while `bv_t < 1`; without it a second Esc during the glide-out would close the window.

## Testing

- **Headless unit test** for `camera_frame_pose_up` (new small target in build.sh, e.g. `frameposetest`):
  - Horizontal normal (e.g. `(0,0,1)`) ⇒ result matches `camera_frame_pose` (same pos, yaw, pitch within epsilon).
  - Vertical-up normal (`(0,1,0)`, a flat face-up map) ⇒ pitch strictly inside ±90° (no degeneracy) and `yaw` aligned to the up-axis heading.
  - Vertical-down normal (`(0,-1,0)`) ⇒ pitch strictly inside ±90° with the opposite sign.
- `map_view_pose` itself is thin glue over `scene_world_rotation`/`object_world_pos` (both GL-free but Scene-bound); covered by the gauntlet build, not a separate unit test.
- Full gauntlet per task: `build.sh` (GL), `c89check`, `asan`, `metal`.
- **No new shader → no MSL twin.**
- Human live-verify: select a wall map + Enter (head-on frame), Esc (glide back); a whiteboard-pinned map; a flat map if one exists (top-down); confirm walking/look are frozen while framed and restored on exit; confirm Esc mid-glide doesn't close the window; confirm deleting the framed map glides you out.

## Out of scope (deferred)

- **Pin/marker management** — the next feature; map view is being built as its home.
- **Pan/zoom re-crop** while framed (live lat/lon/zoom edit).
- **Entering map view from inside board view** for a board-pinned map — stays a no-op for now (modes don't nest).
- Applying camera roll for a perfect orthographic top-down on flat maps.
