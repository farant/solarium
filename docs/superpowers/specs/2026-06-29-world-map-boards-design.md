# World Map Boards — Design Spec

**Date:** 2026-06-29
**Status:** Approved (design), ready for implementation plan
**Author:** brainstormed with Fran

## Goal

Place a map of the world on a wall or table as a board/picture in the palace, centered on
any lat/lon at a chosen zoom, sourced entirely from **bundled public-domain basemaps** —
**no runtime network, fully offline, no licensing strings**.

## Locked decisions (the brainstorm)

1. **Static board**, not an interactive pan/zoom widget. Re-center / re-zoom = rebuild the
   board, not live navigation.
2. **Location input = type lat/lon + zoom.** No geocoding, no place-name search (that would be
   a second network API; deferred).
3. **Offline, store-only, zero network in the engine.** The shipped binary has no HTTP/socket
   code. Tiles/basemaps are bundled assets acquired by a one-time, out-of-engine prep step.
4. **World base only, low zoom (~z6 equivalent).** Country/region scale. Street-level detail is
   explicitly out of scope (it's the deferred "region packs" idea; see Future Directions).
5. **Public-domain basemaps, two of them, selectable per map:**
   - **Natural Earth** raster — public domain, no attribution required. Relief / map aesthetic.
   - **NASA Blue Marble** (Next Generation) — public domain (NASA in-house from MODIS data),
     no attribution required. Satellite aesthetic.
   Both ship as single **equirectangular** (plate carrée, plain lon/lat) world images.
6. **Equirect-crop architecture** (the simplification the PD/equirect choice unlocks): drop the
   Web Mercator slippy-tile pyramid, the tile store, the stitcher, and the network fetcher
   entirely. A map board is a quad whose **UVs are the lon/lat window** into one shared basemap
   texture.

## Why this is small

The display half already exists (the 2026-06-22 image batch): `image_load` → RGBA8 → a quad on
the lit-albedo "picture" path, plus carry → plant-on-wall → resize. A map is just another
texture going down that same path. **No new shader. No MSL twin.**

## Architecture / components

### 1. `mapmath.c` / `mapmath.h` (+ `mapmathtest`) — NEW pure module, strict C89, headless

The brain. All equirectangular math, no GL, no IO. Equirectangular maps lon/lat **linearly** to
UV (unlike Web Mercator), so this is a handful of linear functions:

- `mapmath_lonlat_to_uv(lon, lat, *u, *v)` — `u = (lon+180)/360`, `v = (90-lat)/180`
  (v orientation matched to the image row order; see implementation note).
- `mapmath_uv_to_lonlat(u, v, *lon, *lat)` — inverse (exact; makes pin placement trivial later).
- `mapmath_window(center_lon, center_lat, zoom, aspect, *u0,*v0,*u1,*v1)` — given a center, a
  zoom level, and the board's aspect ratio, produce the UV rectangle the board displays.
  Zoom maps to angular span (e.g. span_deg = 360 / 2^zoom across the wider axis), clamped so the
  window stays within [0,1] in v and within the world in u.
- Helpers: lat clamp to ±90, zoom clamp to [0, ZMAX].

**v1 limitation:** windows are clamped to **not cross the antimeridian** (±180° lon seam).
A window that would wrap is clamped to the edge. (Antimeridian wrap = future.)

**Tested headless** in the `caret.c` / `route.c` / `furniture.c` tradition: round-trip
lon/lat→uv→lon/lat, window centering, clamping, edge cases (poles, ±180, zoom 0).

### 2. Basemap registry — small, in `main.c` (or a thin `basemap.c` if it earns its own TU)

- A basemap is `{ id, asset_path, RhiTexture (lazy) }`. Two registered: `relief` (Natural Earth),
  `satellite` (Blue Marble).
- **Lazy, refcounted load:** a basemap's texture is created on first use by a placed/visible map
  and freed when no map references it. If only relief maps exist, the satellite image never loads.
- **VRAM note:** an 8192×4096 RGBA8 texture is ~134 MB. Lazy per-style load keeps at most the
  in-use styles resident. This is the reason for the modest 8192×4096 default resolution (see
  Data acquisition) rather than 16k.

### 3. The map object — `main.c`, reuses the picture/board path

- A `KIND_MAP` (or a tagged picture) object. `meta` carries the state so it survives reload:
  - `basemap` = `"relief"` | `"satellite"`
  - `lat`, `lon`, `zoom`
- The object's mesh is a quad with **custom UVs = the lon/lat window** (a small
  `make_map_quad(w, h, u0, v0, u1, v1)` — the only mesh addition; `make_picture` is fixed 0..1).
  Re-centering / re-zooming / toggling style just rebuilds the quad UVs (and swaps the basemap
  texture reference) — **no per-map texture allocation**; many maps share one basemap texture.
- Renders on the **existing lit-albedo picture path** with the basemap as albedo. No new shader.

### 4. Authoring UX

- Palette command **"Place map"** → prompt **lat / lon / zoom** (and basemap style; default relief).
- Ghost-place + plant like the existing image/picture flow (carry → aim wall/table → drop;
  resizable via the existing corner-handle path the wall pictures already use).
- Workspace-tagged on placement (the `mint_tag_ws` discipline).
- **Persistence:** state lives in `meta`; on reload the object's UVs are re-derived from
  `lat/lon/zoom` and the basemap texture is re-resolved. Deterministic and fully offline
  (the basemap is a local asset).

## Data acquisition (one-time, out of engine, manual prep)

- Two bundled assets, **equirectangular, downsampled to a common 8192×4096**:
  - `basemap_relief.jpg`  — from Natural Earth raster (public domain).
  - `basemap_satellite.jpg` — from NASA Blue Marble Next Generation (public domain).
- These are the **flagged sourced-binary exception** (the P9 LUT / decal-atlas category):
  **gitignored** like `*.glb` / `*.hdr` / `*.jpg` / `*.wav`, never committed, acquired by Fran
  on his machine. A short `docs/` or `tools/` note records the exact source URLs + the macOS
  downsample command (`sips` / ImageMagick) so the pack is reproducible.
- The engine **never** downloads these. If an asset is missing the map renders a placeholder
  (flat color + a "basemap not installed" label), exactly the offline-cold-cache behavior.

## Constraints (engine laws)

- **Strict C89** for all engine `.c` (`-std=c89 -pedantic-errors -Werror -Wextra`); `*_test.c`
  may be c11. `mapmath.c` is pure C89; `mapmathtest.c` may be c11.
- **RHI seam inviolable** — only `rhi_gl.c` touches GL, only `rhi_metal.m` touches Metal.
- **Dual-backend:** no new shader → **no MSL twin** to write. (This is a texture on an existing
  path.)
- **§1.2** a placed map's position is sacred; **no mesh CSG**; workspace filter law applies
  (a map is a normal scene object — render/BVH/editor already filter by workspace).

## Out of scope / Future Directions

- **Pins** (sub-project C): lon/lat markers on a placed map. Trivial with this architecture —
  `mapmath_lonlat_to_uv` → board-local pixel → a marker. Likely 3D pin objects parented to the
  board (the filing/whiteboard parent precedent), selectable + labelable like notes. Not in v1.
- **Region packs / street detail:** high-zoom detail for specific areas. This is where the OSM
  cartography path lives — **ODbL data, self-rendered tiles, "© OpenStreetMap contributors"
  attribution required, cannot mirror the OSM tile server.** Re-introduces a tile store (the
  shelved Web Mercator design). Deferred.
- **3D contour maps on tables/walls (elevation):** a DEM is *another equirectangular raster*,
  sampled for **height** instead of color, displacing a tabletop mesh — feeds straight into the
  engine's existing heightfield machinery (terrain_height / texgen). Public-domain data:
  **ETOPO 2022 / ETOPO1** (NOAA, PD, land + ocean — the elevation twin of Blue Marble) for world
  scale; **SRTM / NASADEM** (PD) or **Copernicus DEM GLO-30** (permissive + attribution) for
  regional detail. Needs a **16-bit / float height-load path** (8-bit terraces); precedent
  exists in `image_load_hdr`.
- **Interactive pan/zoom**, place-name geocoding, antimeridian wrap.

## Testing

- `mapmathtest` (headless): round-trip lon/lat↔uv, window math, clamps, pole/±180/zoom-0 edges.
- Build gauntlet (gl + metal + asan + c89check + the `*_test` suite).
- **Human live-verify** (subagents can't GUI-test): place a map, confirm the right region shows,
  re-center/re-zoom/toggle style, reload-persistence, wall + table placement, resize, missing-
  asset placeholder.

## Risks

- **VRAM** — 8192×4096 RGBA8 ≈ 134 MB/texture; mitigated by lazy per-style load + the modest
  default resolution. Watch if both styles are placed at once.
- **Data acquisition is a manual step** — documented, reproducible, but Fran must run it before
  maps render (graceful placeholder otherwise).
- **Equirect distortion** at high latitudes (horizontal pole smear) — inherent to plate carrée,
  acceptable for a world poster; called out so it's expected.

## Decomposition for the plan

A single cohesive feature (no longer "multiple subsystems"), built in this order:
1. `mapmath` + `mapmathtest` (headless, pure).
2. Basemap registry + lazy texture cache + missing-asset placeholder.
3. `make_map_quad` + the map object (meta state → UVs).
4. Authoring (palette "Place map", prompt, ghost-place, resize) + workspace tag.
5. Persistence (reload re-derives UVs / re-resolves basemap).
6. Data-acquisition prep note (source URLs + downsample command).

Pins, region packs, and elevation each get their own later spec.
