# Timber-Frame Halls — design

**Status:** design approved 2026-06-24, ready for implementation plan(s).
**Nature:** the procedural-architecture ("follies"/gothic-kit) vision applied to rooms — turning plain box rooms into timber-framed halls. Builds directly on the sourced-texture floor/wall overlays (`load_pbr_material`, the per-room derived-mesh cache built in `connections_rebuild`). Adds two more sourced PBR materials (`dark_wood`, `distressed_painted_planks`, CC0) — the same flagged, gitignored sourced-texture exception, now carrying procedural timber geometry.

## Goal

Each room becomes a hall: **taller walls**, **corner columns**, **scissor trusses** spaced down its length, and a **pitched plank roof** on the trusses — all per-room procedural geometry derived from a shared per-room **frame plan**, reversible (render-time derived, no scene-data churn beyond the one height migration).

## Scope

**In:** a one-time room-height migration (3.0→4.5); a per-room derived `RoomFrame` (generalizing the just-merged wall-overlay cache) holding the wall + wood + roof + gable meshes; a `frame_beam` oriented-box primitive; the geometry for columns / scissor trusses / pitched roof / gable ends; two new materials (`dark_wood`, `distressed_painted_planks`; gables reuse the wall material); tuning knobs.

**Out / unchanged:** no scene-object persistence of the frame (it's derived like the overlays); **no new shader, no MSL twin**; the `disp`/`nor_dx`/redundant maps; gable windows or other decoration; non-room structures (church/terrain); textured-collision (the frame is visual — collision stays the room box, see below).

## Materials

`dark_wood/` and `distressed_painted_planks/` (PNG, CC0) — `…_diff_1k.png` (albedo sRGB) + `…_nor_gl_1k.png` (normal linear) + `…_arm_1k.png` (ORM linear), loaded via the existing `load_pbr_material`. Two new file-scope materials `g_dark_wood`, `g_roof_mat`; gable triangles reuse `g_wall_mat`. Both folders **gitignored** (CC0, local).

## The frame plan (the shared authority — `church_plan` for rooms)

Per room, from `w`, `d`, `h` (h = the migrated 4.5):
- **ridge axis** = the longer of `w`/`d` (ties → X). The ridge runs the full length of that axis; the roof slopes across the shorter axis (the **span**).
- **pitch** = `FRAME_PITCH` (35°, tunable) → **ridge_y** = `h + (span/2)·tan(pitch)` (peak height above the floor).
- **bents** = `max(2, round(ridge_len / FRAME_BENT_SPACING))` scissor trusses, evenly spaced along the ridge axis (first/last near the gable ends).
- **beam thickness** `FRAME_BEAM_T` (0.14 m); **column** `FRAME_COL_T` (0.24 m); **scissor crossing** at `FRAME_SCISSOR_FRAC` (0.5) of the rise; **flush eaves** (no overhang — rooms are discrete floating boxes, so an overhang would intersect walkways/neighbors).

Every piece reads this plan, so the roof pitch, truss rafters, and gable triangle agree by construction. The plan can be a small `room_frame_plan(w,d,h, RoomFramePlan*)` helper or computed inline in the builder; either is fine.

## Architecture — the per-room `RoomFrame` (generalizes the wall overlay)

The wall planks are already a per-room cached mesh built in `connections_rebuild` (`g_wall_cache` + `wall_overlay_store` + `wall_cache_flush` + `wall_overlay_get`). Generalize that single cache into a `RoomFrame` holding several meshes per room handle:

```
struct RoomFrame { sol_u32 handle; Mesh wall, wood, roof, gable; }
```
- `wall` — the existing plank panels (`g_wall_mat`).
- `wood` — columns + trusses (`g_dark_wood`).
- `roof` — the two pitched planes (`g_roof_mat`).
- `gable` — the two end triangles (`g_wall_mat`).

`room_frame_build(shell, ops, no)` (the renamed/extended `wall_overlay_store`) builds all of a room's meshes from the frame plan + the wall openings, replacing+`mesh_destroy`-ing the prior entry. `room_frame_flush` (the renamed `wall_cache_flush`) destroys all four meshes per entry. The draw pass iterates active rooms and draws each non-empty mesh with its material (4 draws/room max). Hooked in `connections_rebuild` + `_focus` exactly as the wall overlay is today.

**Geometry helpers** (direct-vertex, like `wall_panel_quad`): `frame_beam(mb, ax,ay,az, bx,by,bz, t)` — a square-section (t×t) box swept from A to B (handles vertical columns, sloped rafters, crossing scissor chords); plus roof quads and gable triangles. Tangents auto-computed by `mesh_from_builder`; the engine never culls. Position-based tiling UVs (constant texel size) for the roof/gables; beams get simple length-wise UVs.

## Build stages (one spec, four plans — each merged + eyeballed before the next)

### Stage 1 — Taller walls (foundational; no frame geometry)
- **Migration:** on scene load (`load_palace`/the room-load path), for each `"room"` shell, if `h == 3.0` (the old default) set `h = 4.5` — **idempotent** (4.5 ≠ 3.0; a deliberately-resized height is left alone). New-room defaults → 4.5 (`home_p[2]`; descend/add-room creation sites).
- **Ripples:** bump the hardcoded `3.0` default in `room_interior_height` and the `3.5` "close in Y" headroom heuristic (collide) to follow (4.5 / 5.0). Collision/ceiling otherwise read the room height and auto-follow. Doorways/wall-planks read `h` and auto-adapt. The flat `ceil` stays through stages 1–3 (suppressed in stage 4 when the roof replaces it).
- **Verify:** all rooms 50% taller; walls + plank overlay adapt; headroom/collision correct; doorways intact; reload is idempotent.

### Stage 2 — Corner columns
- **Refactor** `g_wall_cache`/`wall_overlay_store`/`wall_cache_flush`/`wall_overlay_get` into the `RoomFrame` struct + `room_frame_build`/`_flush`/`_get` (carry the wall mesh through unchanged; add the `wood` mesh).
- `g_dark_wood = load_pbr_material(dark_wood …)`.
- `frame_beam` helper.
- 4 columns: vertical `frame_beam`s at the interior corners (inset by ~`FRAME_COL_T/2`), floor (y=0) → wall-top (y=h), into the `wood` mesh; drawn with `g_dark_wood`.
- **Verify:** four dark-wood posts in each room's corners, full height; the wall planks still correct (refactor preserved them).

### Stage 3 — Scissor trusses
- Extend `room_frame_build` to add trusses to the `wood` mesh: per bent (positions from the plan, along the ridge axis), build the scissor truss from `frame_beam`s — two **rafters** (eave-top → ridge each side), two **scissor lower chords** (each eave-top up to the crossing point past center), and a short **king post** (ridge down to the crossing). dark_wood.
- **Verify:** scissor trusses span across the room, spaced down its length, sitting on the wall-tops; pitch reads as ~35°.

### Stage 4 — Pitched roof + gable ends
- `g_roof_mat = load_pbr_material(distressed_painted_planks …)`.
- `roof` mesh: two sloped quads from the eave-tops (y=h at ±span/2) to the ridge (y=ridge_y, center), running the ridge length, flush eaves, tiling UVs; drawn with `g_roof_mat`.
- `gable` mesh: two triangles at the ridge ends (above the short walls), wall-top → ridge, filling the gable; drawn with `g_wall_mat` (planks continue up).
- **Suppress the flat ceiling:** pass `ceil = 0` to `make_room_doored` (or skip the ceiling quad) so you look up into the timber.
- **Verify:** pitched plank roof on the trusses, gable ends closed, no flat ceiling, the hall reads top-to-bottom.

## Collision note

The frame is **visual only** — collision stays the existing room box (you don't bonk on a truss). The taller walls do change the headroom (stage 1), which is the only collision change. Columns/trusses/roof are not colliders (consistent with how the wall-plank overlay isn't a collider either).

## Knobs (tuned live, like the overlay `TILE_M`)

`FRAME_PITCH` (35°), `FRAME_BENT_SPACING` (2.5 m), `FRAME_BEAM_T` (0.14 m), `FRAME_COL_T` (0.24 m), `FRAME_SCISSOR_FRAC` (0.5), plus a roof `TILE_M` and the gable inheriting the wall `TILE_M`.

## Error handling / edges

- A material that fails to load (`albedo_tex.id == 0`) → that piece's mesh is skipped/empty (per-material guard at build + draw). Missing folder = that timber piece silently absent; the rest stand.
- Square room → ridge axis is the deterministic tie-break (X when `w >= d`).
- The `RoomFrame` cache reuses the wall overlay's flush-on-full-rebuild, so stale entries can't accumulate (already fixed).
- Workspace filter: built only for active rooms (connections_rebuild filters), drawn only for active rooms.
- A room too small for `>=2` bents → clamp to 2.

## Testing

Render-only geometry + draws — verification is the **build gauntlet** (`c89check && debug && metal`; no shader → Metal risk is compile-only) + **human live-verify per stage** (each stage is a distinct visual checkpoint). The frame-plan arithmetic (`ridge_y`, bent positions, scissor crossing) is simple and verified visually; the height-migration idempotency is verified by reload. No new headless unit target.

## Constraints honored

- **C89**; **no new shader / no MSL twin**; RHI seam intact (materials via the existing loaders, geometry via MeshBuilder).
- **Synthesized-never-sourced knowingly bent** (the ongoing flagged sourced-texture experiment) — `dark_wood/` + `distressed_painted_planks/` gitignored (CC0, local).
- Never stage `NOTES.stml`/`paper-picture.png`; commits end with the `Co-Authored-By: Claude Opus 4.8 (1M context)` line.

## File structure

```
main.c       modify — the height migration + default bump + headroom-fallback fixes;
             the RoomFrame generalization of the wall cache; room_frame_build/_flush/_get;
             frame_beam + the column/truss/roof/gable geometry; g_dark_wood + g_roof_mat;
             the per-material draws; the ceiling suppression; the FRAME_* knobs
.gitignore   modify — /dark_wood/ and /distressed_painted_planks/ (CC0, local)
```
