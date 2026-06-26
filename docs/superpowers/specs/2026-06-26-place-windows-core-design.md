# Place Windows — Design Spec (Phase 1: Core)

**Date:** 2026-06-26
**Status:** Approved (brainstorm), pending implementation plan
**Author:** Fran + Claude

## Goal

Place **rectangular windows** in flat room walls: a command drops a window on the wall in front
of you; the window is a selectable, draggable, resizable, deletable object; it cuts a real hole
in the wall (visible see-through), wears a **dark_wood frame**, and can take a **colored glass
pane** (default: open hole, no glass). This is **Phase 1 of 3** — styles (french/arched/pointed/
circular) and gable/wall-gable spanning are separate later specs.

## Decisions (from brainstorming)

- **Approach A — the window is a scene object; the wall reads it.** A window is a normal
  `SceneObject` parented to its room, storing everything a wall opening needs (wall-index,
  along-wall center, sill, width, height). The wall emitter and renderer are READERS of it — the
  project's one-author/many-readers law. **Persistence is free** (it saves/loads as a normal
  object; no separate meta serialization). Rejected: B (window as room-meta, baked into derived
  geometry — loses the free pick/drag/resize machinery) and C (one object split across opaque +
  transparent passes — the renderer draws one mesh per object per pass).
- **The wall hole stays rectangular; the FRAME carries any future shape.** A window opening is a
  rectangle, cut by extending the existing doorway "build around the gap" machinery with a 4th
  **sill panel** below the hole. Arched/circular looks (Phase 2) will come from opaque dark_wood
  spandrels inside the rectangular frame — the wall never cuts a non-rectangular hole, honoring
  the NO-CSG law.
- **Drop on the wall in front.** A palette command raycasts the camera-forward ray at the room
  wall planes and drops a default window at the hit. No ghost-aim mode in v1; fine-tune by
  select + drag + resize.
- **Default = open hole (no pane).** The default look is dark_wood frame + a clean see-through
  hole. `↑/↓` cycles color presets; the first preset is none/clear, the rest add a tinted
  translucent pane. `←/→` is reserved for Phase-2 styles.
- **Plain rectangular frame** (border ring + sill ledge), no mullions/muntins in v1.
- **No new shader** → no MSL twin (reuses `draw_mesh` for the frame, `draw_glass` for the pane).

## Non-Goals (Phase 1)

- No arched / circular / pointed / french styles (Phase 2).
- No gable-triangle windows or wall+gable spanning (Phase 3).
- No real light transport — the hole is **visually** see-through; colored light shafts / floor
  pools are the deferred P9 capstone (item 8). Glass only tints what you see through it.
- **Collision is unchanged.** The wall stays solid for walking — a window does not open a
  walkable gap.
- No window in a free-standing (non-room) wall; windows attach to room shells only.

## Background (current state — verified)

- **Walls** are thick solid slabs emitted as boxes. `emit_doored_wall(MeshBuilder*, int runx,
  f0, f1, s0, s1, h, const RoomOpening *ops, int n_ops, int wall_id)` (mesh.c:270) walks the wall
  left→right and emits solid `aabb_box`es as **piers** (between gaps) and **headers** (above a
  gap that doesn't reach the wall top). `make_room_doored(...)` (mesh.c:315) is the room shell.
  Wall thickness `ROUTE_WALL_T = 0.20` (route.h). The emitter currently has **no sill** — doors
  reach the floor.
- **`RoomOpening { int wall; sol_f32 center; sol_f32 width; sol_f32 height; }`** (mesh.h:62).
  Door openings are **derived from the route graph** (`route_room_openings_in`, route.c:330),
  never persisted. `ROUTE_DOOR_W 1.4`, `ROUTE_DOOR_H 2.1`.
- **`RoomFrame { sol_u32 handle; Mesh wall, wood, roof, gable, door_floor, door_trim; }`**
  (main.c:4386), cached per room in `g_room_frame[]`, (re)built by
  `room_frame_build(SceneObject *shell, const RoomOpening *ops, int no)` (main.c:4591) — invoked
  during the connections/room rebuild. Replace-by-handle; destroyed on room delete / full rebuild.
- **Wall mounting:** `descend_wall_mount` (descend.c:77) raycasts the room's 4 interior wall
  planes and returns the wall index + hit; pictures are pushed proud by `n · t/2`. Wall facing
  yaw table `wyaw[4]` (main.c:9434).
- **Drag along wall:** the `move_board` state (main.c:10468) projects the ray on the wall plane
  and clamps via `wall_clamp_run_cy` (main.c:9109) / `wall_gable_geom` (main.c:9092).
- **Corner resize:** `resize_corner_pick` (main.c:9017), `board_corners` (descend.c:122),
  `board_resize_corner(anchor, dragged, u, min, aspect, *w, *h, *origin)` (descend.c:131). The
  **registry-rebuild law:** capture old key → `scene_mesh_params_set` → `asset_release(old key)`
  → `memset(&o->mesh,0,...)` (clear the borrow) → `scene_resolve_meshes` (NEVER `mesh_destroy` a
  registry-shared shape).
- **Delete:** `delete_board_card(st, h)` (main.c:10016) releases the mesh key + clears state
  pointers + `scene_remove`.
- **Glass:** `draw_glass(state, mesh, ...)` (main.c:13455) on `glass_pipeline`
  (`RHI_BLEND_ALPHA`, `depth_write_off`) with `uOpacity = GLASS_OPACITY (0.6)`. The transparent
  pass (main.c:~14990) collects `mesh_ref=="church_glass"` objects, **caps at 16**, sorts
  far→near, draws each via `draw_glass`. `material.base_color` tints; `material.emissive *
  overlay_glow` glows.
- **Arrows:** `↑/↓` free when no palette/board-page mode; `←/→` cycle board pages in board view
  (free elsewhere). Place mode owns the keyboard and cycles a catalog via `[`/`]`
  (`place_realize_ghost` precedent).

## Architecture

### 1. The window object (the author)

A window is `KIND_PLAIN`, `mesh_ref="window"`, **parented to the room** whose wall it sits on.
Its geometry fields ARE a window opening, stored in `mesh_params` (room-local):
`wall` (N/E/S/W index), `center` (along-wall offset), `sill`, `width`, `height`. (`wall` may
live in `meta["wall"]` if an int param is awkward; the plan picks.) The window's **world
transform** for rendering derives from `(room transform, wall, center, sill, height)` — the same
way `RoomFrame` derives from its room — so the frame mesh and the wall hole always agree. The
window sits **centered in the wall thickness** (NOT pushed proud like a picture; the frame
straddles the wall).

Color preset is `meta["glass"]` (e.g. `none`/`blue`/`green`/`amber`/`red`). `none` = open hole.

### 2. The wall hole (the wall reads the window)

- **Extend `RoomOpening` with `sol_f32 sill;`** — `0` = door (floor-reaching, unchanged);
  `> 0` = window.
- **`emit_doored_wall` gains a sill panel:** for any opening with `sill > 0`, additionally emit
  a solid box `[lo, hi, 0, sill, f0, f1]` below the gap (the existing header box already closes
  the top). The 4-way split becomes sill / header / left pier / right pier.
- **The room rebuild gathers windows:** in `room_frame_build` (or the caller that assembles
  `ops`), after collecting route-derived door openings, **scan the room's child objects for
  `mesh_ref=="window"`** and append a `RoomOpening{wall, center, width, height, sill}` for each
  (workspace-filtered like every other reader). The wall is emitted with doors + windows.

### 3. Frame + glass meshes

- **New registry mesh-gen `"window"`** (mesh.c, REGISTRY row): the dark_wood **frame ring +
  sill ledge**, parametric on width/height/thickness/frameWidth. Spans the wall thickness, flush
  on both faces, hollow center (the hole). Opaque, drawn with `draw_mesh` + `g_dark_wood`.
- **Glass pane:** when `glass != none`, a **child object** `mesh_ref="window_glass"` (a thin
  quad sized to the inner opening) carries the translucent tinted pane. `material.base_color` =
  the preset tint; drawn via `draw_glass`. Default `none` → no child object.
- **Resize rebuild:** the frame mesh follows the registry-rebuild law on resize (capture key →
  params set → `asset_release` → clear borrow → `scene_resolve_meshes`); the glass child resizes
  the same way.

### 4. Placement — "Place window"

A palette command `cmd_place_window` (plus optionally a free hotkey): raycast camera-forward at
the active room's wall planes (`descend_wall_mount` machinery). On a hit:
- determine room + wall index + along-wall center + a sill so the default window sits at a
  pleasant height (e.g. default `width 1.2`, `height 1.4`, `sill 0.9`, clamped within the wall),
- create the window object parented to that room with those params, `glass=none`,
  workspace-tagged (`mint_tag_ws`),
- **rebuild the owning room's wall** so the hole appears,
- select the new window. No ghost-aim mode.

### 5. Interaction (reused, plus one new coupling)

- **Select:** pick the frame like any wall object.
- **Drag along wall:** reuse `move_board` + `wall_clamp_run_cy`, clamped so the whole hole stays
  inside the wall (`center ± width/2` within span; `sill ≥ 0`, `sill+height ≤ wall_h`).
- **Corner resize:** `board_corners` + `board_resize_corner`, **free aspect**. Bottom corners
  move the sill; top corners move the header.
- **Delete:** `delete_board_card` (releases the frame mesh, removes the glass child).
- **`↑/↓` while selected (or while the new window is fresh-placed and selected):** cycle the
  color preset → add/retint/remove the `window_glass` child.
- **★ The one new coupling — rebuild the wall when a window changes.** A window's
  move / resize / delete / color change must re-run the owning room's wall rebuild so the hole
  tracks. **Perf:** per the editor perf lesson (don't rebuild heavy geometry every drag frame),
  the FRAME (a light object) updates live during a drag, but the **WALL HOLE rebuilds on
  release** (mouse-up / end of resize), not every frame. Color changes (discrete) rebuild
  immediately.

### 6. Rendering

- Frame: normal opaque pass (`draw_mesh`, `g_dark_wood`).
- Glass: **generalize the transparent pass** from `mesh_ref=="church_glass"` only to also accept
  `mesh_ref=="window_glass"`, and **raise the 16-object cap** (a small fixed bump, e.g. 64, or
  size it to the live count) so many windows don't starve the church glass.

### 7. Persistence

Free. Windows are normal scene objects (`mesh_ref="window"` + params + `meta["glass"]`), saved
in `scene.stml` and re-resolved on load via `scene_resolve_meshes`; the glass child saves too.
On load, the room rebuild re-reads child windows and re-cuts the holes — same as runtime.

## Data Flow

```
Place window (palette)
  -> raycast camera-forward at room wall planes (descend_wall_mount)
  -> create window object (mesh_ref="window", params {wall,center,sill,w,h}, glass=none),
     parent = room, workspace-tagged
  -> room wall rebuild: gather route doors + child windows -> emit_doored_wall (sill panel)
  -> select the window

Drag/resize the window
  -> frame object updates live (cheap)
  -> on release: recompute center/sill/w/h -> room wall rebuild (hole tracks)

Up/Down (selected)
  -> cycle meta["glass"]; none<->tinted -> add/retint/remove window_glass child -> redraw

Delete
  -> remove glass child + window object -> room wall rebuild (hole closes)

Reload
  -> window objects load from scene.stml -> room rebuild re-cuts holes
```

## File Touch List

- **`mesh.h`**: add `sol_f32 sill;` to `RoomOpening`; declare the `make_window` mesh-gen.
- **`mesh.c`**: sill panel in `emit_doored_wall`; the `make_window` frame mesh-gen + REGISTRY
  row (and a `make_window_glass` quad, or reuse an existing thin quad for the pane).
- **`main.c`**: `cmd_place_window` + palette row; gather child windows into the room opening list
  in `room_frame_build`/its caller; the move/resize/delete → wall-rebuild-on-release hook; the
  `↑/↓` color cycle; generalize the transparent glass pass + raise the cap; default-size
  constants.
- **`descend.c` / `.h`**: reuse `descend_wall_mount` (likely no change; possibly a thin helper to
  return room + wall + along-wall center for placement).
- **`build.sh`**: a `meshtest`/`gothictest`-style headless assertion target if we add a window
  mesh unit test (see Testing).

## Testing

- **Build gauntlet (all three):** `./build.sh c89check`, `./build.sh`, `./build.sh metal`.
- **Pure-logic (headless, the mesh builder is testable):** assert `emit_doored_wall` with a
  `sill > 0` opening emits the **sill panel** (a box below the gap) — e.g. a vertex-count / AABB
  assertion in an existing mesh test, mirroring the gothic/room tests. Optionally assert the
  window→`RoomOpening` derivation (center/width/sill/height) from a known window object.
- **Human live-verify (both backends):**
  - "Place window" drops a framed open hole in the wall ahead; you can see through it.
  - Select → drag along the wall (stays within the wall; hole follows on release) → corner-resize
    (free aspect; bottom moves sill, top moves header; hole follows on release).
  - `↑/↓` cycles glass color (none → blue → green → amber → red …); a tinted pane appears/changes;
    back to none = open hole.
  - Delete → the hole closes and the wall is solid again.
  - Reload (`L`) → windows persist and the holes re-cut.
  - Place several windows + keep the church stained glass visible (the cap raise works).

## Risks

- **Wall-rebuild coupling cost.** Rebuilding a room shell on every drag frame would stutter (the
  editor perf lesson). Mitigation: live-move the light frame object, rebuild the heavy wall on
  release only.
- **Opening↔object representation drift.** The window's params and its derived world transform
  must stay consistent (single source of truth = the room-local params; world transform derived).
  A drag edits params, not a free world pos, to avoid drift.
- **Glass pass cap + sorting.** Window panes join the sorted transparent set; raise the 16 cap
  and confirm windows + church glass still sort correctly (far→near).
- **Workspace filtering.** The room rebuild must only read windows in the active workspace (every
  derive-reader filters — the workspace LAW).
- **Default sill within wall.** Clamp the default window so `sill ≥ 0` and `sill+height ≤
  wall_h` for short walls; reject placement if the wall is too small.
- **No new shader** ⇒ no MSL twin.
