# Sandstone Floor Overlay — sourced-texture experiment

**Status:** design approved 2026-06-23, ready for implementation plan.
**Nature:** a **deliberate, flagged sourced-texture experiment** — it applies a PolyHaven (CC0) PBR texture set to room floors. This intentionally departs from the engine's **synthesized-never-sourced** law (texgen.c is normally the one author of albedo/normal/ORM). It is render-time and fully reversible; if kept, a later pass could either bless it as a real floor mesh-kind or replace it with a synthesized texgen floor tuned to match.

## Goal

Apply the `red_sandstone_tiles` map set to **normal room floors** at a **constant physical tile size**, lit and shadowed like everything else, so Fran can evaluate the look in-world.

## Scope

**In:** a linear texture loader; a one-time sandstone `Material`; a tiny `(w,d)`-keyed floor-quad cache (meter-based UVs for constant tile size); a per-room draw in the main opaque pass for `"room"` shells (workspace-filtered); gitignoring the texture folder.

**Out / unchanged:** no scene or `scene.stml` changes (render-time only); **no new shader, no MSL twin** (reuses the lit PBR pipeline); not terrain islands (their own texgen ground) or the church; the displacement map (no tessellation/parallax); the synthesized texgen path (this is the explicit exception).

## What's on disk

`red_sandstone_tiles/` (PNG, CC0): `…_diff_1k.png` (albedo), `…_nor_gl_1k.png` (GL normal), `…_arm_1k.png` (AO/Rough/Metal packed), plus redundant `ao`/`rough`/`disp` (unused — ARM supersedes ao/rough; disp needs tessellation we don't have). The engine reads PNG via `image_load` (stb); REPEAT wrap + mips are the `rhi_create_texture` default, so tiling works.

## Architecture (all in `main.c` + `.gitignore`)

### 1. Linear texture loader
`load_texture` forces sRGB (`RHI_TEX_SRGB8`). Add a sibling `load_texture_linear(path)` identical except `RHI_TEX_RGBA8` (linear) and a distinct linear cache key (`tex_asset_key(path, SOL_FALSE, …)`). Used for the normal + ARM maps (the diffuse stays sRGB via `load_texture`). No file-watcher entry (the experiment doesn't need hot-reload of these).

### 2. The sandstone material (built once)
A file-scope `Material g_floor_mat`, populated during world init where the RHI + textures are available (alongside the existing `load_texture("paper-picture.png")`):
- `albedo_tex` ← `load_texture("red_sandstone_tiles/red_sandstone_tiles_diff_1k.png")` (sRGB)
- `normal_tex` ← `load_texture_linear(".../red_sandstone_tiles_nor_gl_1k.png")`
- `mr_tex` and `ao_tex` ← `load_texture_linear(".../red_sandstone_tiles_arm_1k.png")` (ARM: R=AO, G=Rough, B=Metal — exactly the engine's ORM convention)
- `base_color` = (1,1,1); `metallic` = `roughness` = `normal_scale` = `ao_strength` = 1.0 (the maps fully drive the look)

**Disabled flag / graceful fallback:** if the diffuse fails to load, `g_floor_mat.albedo_tex.id == 0` — the render step treats that as "overlay off" and rooms keep their plain stone floor. Nothing else breaks.

### 3. Constant-tile-size floor-quad cache
A small fixed cache `{ float w, d; Mesh mesh; }[FLOOR_CACHE_MAX]` (e.g. 32) + `floor_quad_for(w, d)`:
- On hit (matching `w,d` within an epsilon) → return the cached mesh.
- On miss → build a horizontal `w × d` quad (XZ plane, +Y normal) with **UVs `(0,0)`→`(w/TILE_M, d/TILE_M)`** — so one texture-repeat spans `TILE_M` meters regardless of room size — via 4 `mb_push_vertex` + 2 `mb_push_triangle`, then `mesh_from_builder` (tangents auto-computed → normal mapping works). Insert and return.
- On overflow (more than `FLOOR_CACHE_MAX` distinct sizes — not expected in practice) → skip the overlay for that room and `printf` once. (No transient per-frame meshes — that would leak GPU buffers.)

`TILE_M` (meters per texture-repeat) is the tuning knob; default ≈ 1.5.

### 4. The render step
In the main opaque pass (the world's per-object draw, after rooms are drawn), iterate scene objects; for each `o` with `mesh_ref == "room"` **and** `scene_object_active(&scene, o->handle)` **and** `g_floor_mat.albedo_tex.id != 0`:
- read `w`, `d` from the room params (`mesh_ref_param("room", …, "w"/"d")`)
- `q = floor_quad_for(w, d)` (skip if it returned an empty mesh)
- `model = scene_world_matrix(&scene, o) · translate(0, FLOOR_EPS, 0)` (no scale — the cached quad is already `w×d`)
- `draw_mesh(state, q, model, view, proj, eye, 0.0f, g_floor_mat)`

`FLOOR_EPS` (≈ 0.012 m) lifts the overlay just above the room's own floor (origin at floor center, y=0) to avoid z-fighting; the opaque overlay hides the stone beneath. It rides the lit PBR pipeline, so the sun + cascaded shadows fall on it for free.

## Data flow

```
startup ───► load 3 maps ───► g_floor_mat (or albedo.id==0 = disabled)
each frame, main opaque pass:
  for each active "room" shell ─► floor_quad_for(w,d) [cache] ─► draw_mesh over the floor (+EPS)
```

## Error handling / edges

- Texture folder missing → `albedo_tex.id == 0` → overlay disabled, rooms unchanged.
- Cache overflow → skip + one-time log (no leak).
- Workspace filter: only active-workspace rooms draw (no cross-world leak).
- Room resized in the editor → its `(w,d)` changes → a different/new cache entry; the stale one lingers harmlessly (tiny).

## Testing

This is render-only (a material + a GPU-mesh cache + a draw loop) — no pure logic worth a headless unit test. Verification is the **build gauntlet** (`c89check && debug && metal` — confirming it compiles on both backends; no shader changed so the Metal risk is compile-only) plus **human live-verify**: open a room and confirm the floor shows tiled red sandstone (albedo + normal relief + roughness), lit and shadowed, with a consistent tile size between differently-sized rooms.

## Constraints honored

- **C89**; **no new shader / no MSL twin** (reuses `draw_mesh`'s PBR pipeline); the RHI seam intact (`load_texture_linear` calls `rhi_create_texture` from `main.c` exactly as `load_texture` does).
- **Synthesized-never-sourced is knowingly bent** — flagged here and in code as the experiment it is; `red_sandstone_tiles/` is gitignored (CC0, kept local).
- Never stage `NOTES.stml` / `paper-picture.png`; commits end with the `Co-Authored-By: Claude Opus 4.8 (1M context)` line.

## File structure

```
main.c       modify — load_texture_linear; g_floor_mat + init; the floor-quad cache
             + floor_quad_for; the per-room overlay draw in the opaque pass
.gitignore   modify — red_sandstone_tiles/ (CC0, local-only, the sourced exception)
```
