# Plank Walls Overlay — sourced-texture experiment (with doorway splits)

**Status:** design approved 2026-06-23, ready for implementation plan.
**Nature:** the wall companion to the [sandstone floor overlay](2026-06-23-sandstone-floor-overlay-design.md) — another flagged, reversible sourced-texture experiment (PolyHaven `weathered_brown_planks`, CC0). Same departure from synthesized-never-sourced, same gitignore-and-flag treatment.

## Goal

Plank the **inner faces of room walls** with the weathered-brown-planks PBR set, at constant tile size, **splitting the planking around doorways** (panels flank each opening, a header sits above it) so the openings you walk through stay clear. Lit and shadowed like everything else.

## Scope

**In:** a DRY material-loader helper (shared with the floor); a `g_wall_mat`; a per-room **wall-overlay mesh** of inner-face panels (split around doorways) built where the doorway openings are already computed; a handle-keyed cache; a per-room draw; gitignore the folder.

**Out / unchanged:** no scene/`scene.stml` change; **no new shader / no MSL twin** (reuses the lit PBR pipeline); not the church/terrain; the displacement + `nor_dx` + redundant `ao`/`rough` maps (ARM + `nor_gl` supersede them); the synthesized texgen path (this is the explicit exception).

## What's on disk

`weathered_brown_planks/` (PNG, CC0): `…_diff_1k.png` (albedo), `…_nor_gl_1k.png` (GL normal), `…_arm_1k.png` (AO/Rough/Metal). Same minimal set as the sandstone; loaded the same way (`load_texture` sRGB + `load_texture_linear` for normal/ARM, both already on `main`).

## Why doorways are the crux

A room's walls are one monolithic mesh, and they carry **doorway cutouts** — ~1.2 m openings the engine computes from the route graph. `mesh.c`'s `emit_doored_wall` builds each wall by walking its (sorted) openings and emitting **solid piers** between them plus a **header** above each opening when the opening's height is below the wall height. A naive full-wall plank quad would cover those openings. So the overlay must replicate that pier-and-header split — but as flat inner-face panels.

## Architecture (all in `main.c` + `.gitignore`)

### 1. DRY the material load
Extract a helper used by both surfaces:
```
static Material load_pbr_material(const char *diff, const char *nor, const char *arm);
```
It returns a `Material` with `albedo_tex` = `load_texture(diff)` (sRGB), `normal_tex` = `load_texture_linear(nor)`, `mr_tex` = `ao_tex` = `load_texture_linear(arm)` (ARM = ORM), `base_color` = white, `metallic`/`roughness`/`normal_scale`/`ao_strength` = 1; if `diff` fails to load, `albedo_tex.id == 0` (the disabled flag). Refactor `floor_mat_init` to call it; add `g_wall_mat = load_pbr_material(...planks diff/nor/arm...)`.

### 2. The wall-overlay mesh (the new geometry)
A helper that mirrors `emit_doored_wall`'s pier-and-header walk but emits **flat inner-face quads**:
```
static void wall_panels(MeshBuilder *b, int runx, float f_inner, float nsign,
                        float s0, float s1, float h,
                        const RoomOpening *ops, int n_ops, int wall_id);
```
For one wall it walks the openings (filtered to `wall_id`, sorted by start, exactly as `emit_doored_wall`) and emits a flat quad for each **solid pier** `[cur, gL] × [0, h]` and each **header** `[gL, gR] × [oy, h]` (only when `oy < h`). Each quad lies on the wall's inner face, displaced inward by `WALL_EPS`:
- `runx == 1` (N/S walls, run along X): vertices `(s, y, f_inner)`, normal `(0,0,nsign)`.
- `runx == 0` (E/W walls, run along Z): vertices `(f_inner, y, s)`, normal `(nsign,0,0)`.

**UVs are position-based** for constant tile size *and* cross-doorway alignment: `u = s / WALL_TILE_M`, `v = y / WALL_TILE_M`. (So planks run along the wall length and tile continuously past the door gaps.)

The per-room overlay = `wall_panels` for each existing wall (gated by `wn/we/ws/ww`), with the inner-face coordinates from the room geometry:
| wall | runx | f_inner | nsign | s0 | s1 |
|------|------|---------|-------|----|----|
| N | 1 | `-hd + WALL_EPS` | +1 | `-hw` | `hw` |
| S | 1 | ` hd - WALL_EPS` | −1 | `-hw` | `hw` |
| E | 0 | ` hw - WALL_EPS` | −1 | `-hd` | `hd` |
| W | 0 | `-hw + WALL_EPS` | +1 | `-hd` | `hd` |

where `hw = w/2`, `hd = d/2`. (The opening `center` values are in the same `s` coordinate `emit_doored_wall` uses, so the splits line up with the real doorways.)

### 3. Build it where the openings already exist (perf-safe)
`connections_rebuild` (main.c:4203) already, per room shell, computes `route_room_openings_in(routes, n, …, ops, no)` and rebuilds the doored room mesh via `make_room_doored(…, ops, no)`. **Immediately after** that mesh build, build the matching wall-overlay mesh from the same `w,d,h`, wall flags, and `ops/no`, and store it in a **handle-keyed cache** (`{ sol_u32 handle; Mesh mesh; }`, replacing + `mesh_destroy`-ing any prior entry for that shell). Do the same at the analogous shell-rebuild in `connections_rebuild_focus` (the editor's incident rebuild). This keeps the overlay in sync with the room mesh and adds **zero per-frame route work** (the alternative — re-deriving openings each frame at render — is the engine's known perf trap, so it's rejected).

### 4. The draw
In the main opaque pass, after the room objects draw (alongside the floor overlay), iterate active `"room"` shells; look up the shell's cached wall-overlay mesh; if present and non-empty, `draw_mesh(state, overlay, scene_world_matrix(shell), view, proj, eye, 0.0f, g_wall_mat)`. The `WALL_EPS` lift is already baked into the mesh (room-local), so no extra transform. Lit PBR pipeline → sun + shadows for free, **no new shader**.

### 5. Constants / knobs
`WALL_TILE_M` (meters per texture-repeat, default 1.5 — the plank-size knob), `WALL_EPS` (≈ 0.01 m inward, anti z-fight), `WALL_CACHE_MAX` (e.g. 128 shells; overflow → skip + one-time log).

## Data flow

```
startup ──► g_wall_mat = load_pbr_material(planks) (or albedo.id==0 = disabled)
connections_rebuild / _focus (on load + every room/doorway change):
   per room shell ─► make_room_doored (existing) ─► wall_panels over ops ─► cache[shell] = overlay mesh
main opaque pass:
   per active room shell ─► draw cached overlay with g_wall_mat
```

## Error handling / edges

- Folder missing → `g_wall_mat.albedo_tex.id == 0` → overlay built/drawn never (guarded); rooms keep plain stone walls.
- Cache overflow (> `WALL_CACHE_MAX` shells) → skip that shell + one-time log (no leak).
- A deleted room leaves a stale cache entry (small, harmless); not pruned in this experiment.
- Workspace filter: only active rooms are built (connections_rebuild already filters `scene_object_active`) and drawn.
- Headerless openings (`oy >= h`, full-height) → no header panel, just flanking piers.

## Testing

Render-only (a material + per-room GPU meshes + a draw) — no headless-testable logic. Verification: the **build gauntlet** (`c89check && debug && metal`; no shader changed → Metal risk is compile-only) + **human live-verify**: walls show tiled weathered planks split cleanly around doorways (panels flank each door, a header bridges above it, the opening stays clear), consistent tile size between rooms, no z-fighting.

## Constraints honored

- **C89**; **no new shader / no MSL twin**; RHI seam intact (`load_pbr_material` only calls the existing loaders).
- **Synthesized-never-sourced knowingly bent** — flagged; `weathered_brown_planks/` gitignored (CC0, local).
- Never stage `NOTES.stml`/`paper-picture.png`; commits end with the `Co-Authored-By: Claude Opus 4.8 (1M context)` line.

## File structure

```
main.c       modify — load_pbr_material (refactor floor + add g_wall_mat); wall_panels +
             the per-room overlay build + handle-keyed cache, hooked in connections_rebuild
             and connections_rebuild_focus; the per-room overlay draw in the opaque pass
.gitignore   modify — weathered_brown_planks/ (CC0, local — the sourced exception)
```
