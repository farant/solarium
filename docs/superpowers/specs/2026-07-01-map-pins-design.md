# Map Pins — Design

**Date:** 2026-07-01
**Status:** Approved, ready for plan
**Builds on:** [[map-view]] (the framing mode built as pins' home), [[world-map-boards]] (the map object + `mapmath`), the command palette (`palette.c`/`command.h`), and the Places catalog (the entity browser's hidden-anchor saved-locations).

## Why

Map view is a view-only camera mode that frames a world map. Pins add the payload: markers with floating text labels at geographic points on a map. You place them by clicking the map, add them from the Places catalog, get in-bounds Places auto-populated when a map is created, and delete/rename them. Interaction inside the frozen focus view is driven by a new **focus palette** — a lightweight, reusable command/picker built by extending the existing palette — which also unblocks the `:` key that map view currently suppresses.

## The unifying model

A pin is **a geographic point (lat/lon) + a name label**. Places already carry exactly this (`name`/`lat`/`lon`/`zoom`/`basemap` meta on hidden-anchor children). So a click-placed pin, a Place-added pin, and an auto-populated pin are the *same thing*. **lat/lon is canonical; a pin's position on a given map is derived** by projecting through the map's current crop window (`mapmath_lonlat_to_uv` → is it inside `[u0,u1]×[v0,v1]` → local x/y on the quad). This mirrors how the map itself is a quad derived from meta, and means future pan/zoom just re-projects pins for free.

**No new shader anywhere → no MSL twin.** The marker is an ordinary scene-object mesh; the label reuses `wtext_block`; the focus palette reuses `palette.c`.

## Scope (one feature, four components)

Built in this order:
1. **Pin entity + render** — the marker mesh on the map + floating label.
2. **Place / select / delete** in map view.
3. **Auto-populate** in-bounds Places when a map is created.
4. **Focus palette** — `:` context-sensitive + a picker level — wiring the "Add place…" and "Rename pin" commands.

## Component A — the pin entity

- A **scene-object child of the map** (`parent == map handle`, `mesh_ref == "pin"`), `KIND_PLAIN`, with meta `lat`, `lon`, and `name` (name may be empty). Being a child means a pin hides/deletes/workspace-filters *with* its map for free.
- **Canonical data is meta (lat/lon/name); geometry is derived.** A `map_pins_resolve`-style step, given a map and its crop window `(u0,v0,u1,v1)` + quad size `(w,h)`, for each pin child:
  - `mapmath_lonlat_to_uv(lon,lat,&u,&v)`; if `u∈[u0,u1] && v∈[v0,v1]` the pin is **in-bounds**: local position `x = -w/2 + w·(u-u0)/(u1-u0)`, `y = h·(v-v0)/(v1-v0)`, with a tiny `+Z` offset off the map face; build/keep the marker mesh at that pos.
  - Otherwise the pin is **out-of-bounds**: hide it by clearing its marker mesh (`index_count 0`), which also removes it from the render pass and from picking.
- The marker mesh is a small **per-pin OWNED mesh** (a co-planar disc/ring, orientation-agnostic so it reads on a wall map or a flat table map), built/destroyed per the in-bounds test — mirroring the map's own per-object owned quad. `"pin"` is therefore **excluded from `mesh_asset_key`** (owned → `mesh_destroy` on delete, like map/arrow).
- This resolve runs wherever the map quad is (re)built — **creation, scene load, and re-crop on resize** — so pins reposition with their map; and directly at **placement time** for a newly added pin (the map's current window is known then).
- **Label render:** a new pin-label pass modeled on the existing card-label pass (the `wtext_block` loop): for each in-bounds pin, draw its `name` as depth-tested world text floating just off the marker toward the viewer. Skip empty names. Labels are shown **always** (declutter/fade is a later concern). Reuses `label_in_view` culling.
- The marker itself renders through the **normal scene pass** (it's a scene object with a mesh + material) — no special marker pass.

## Component B — place / select / delete (map view)

Map view's press handler currently has an inert no-op branch (clicks do nothing). Replace it with pin interaction. This requires **picking at the cursor in map view** (the cursor is now free): add `map_view` to `pick_ray`'s cursor branch (previously deliberately board-view-only — that deferral ends here).

- **Single-click a pin** → select it (`selected_handle`, standard highlight).
- **Single-click empty map** → deselect.
- **Double-click empty map** → place a pin at that lat/lon: raycast the map quad's plane (the `board_ray_local` pattern — intersect the face plane, `scene_world_to_local` → local `(x,y)`, bounds-check against `[-w/2,w/2]×[0,h]`), convert local → uv → `mapmath_uv_to_lonlat` → lat/lon; create a pin child with that lat/lon and an **empty name**; resolve it (in-bounds by construction) and select it. Mirrors double-click-empty-board → new note.
- **Delete/Backspace** on a selected pin → remove it: a new `"pin"` branch in the first-person delete ladder (owned mesh → `mesh_destroy`, then `scene_remove` + `scene_save`), mirroring the map/pond branches.

## Component C — auto-populate Places at creation

At the end of `spawn_map_board` (after the map's meta is set and its window is computable), iterate the Places anchor's children; for each Place whose lat/lon falls inside the new map's window, create a pin child carrying that Place's `name` + `lat`/`lon`. **Snapshot semantics** — the pin is an independent, deletable copy; it does not track later Place edits.

## Component D — the focus palette

A lightweight command/picker for focus modes, built by **reusing the existing `Palette`** (which already parameterizes on a `Command[]` and already supports a command→sub-input flow via its prompt mode).

- **Per-mode command tables.** A `Command[]` for map view (`g_mapview_cmds[]`), holding at least:
  - **"Add place…"** — `run()` enumerates the Places catalog into a `BrowserItem[]` and calls `palette_pick(...)` (below) to choose one; the callback creates a pin on the current map (`st->map_view`) at the Place's lat/lon with its name. (A Place outside the map's bounds still creates a pin — hidden until in view; harmless.)
  - **"Rename pin"** — `can_run` = a pin is selected; `run()` calls `palette_prompt(p, "pin name", …)` and the callback sets the selected pin's `name` meta (label rebuilds on next resolve/label pass).
  - Deletion stays on the Delete key (no separate command needed).
- **`:` becomes context-sensitive** (in `on_key`): in **map view** it opens the palette over the map-view command array; everywhere else, the global registry as today. This **replaces the `map_view == 0` suppression** on `:` added in the map-view MVP — `:` now *routes* in map view instead of being blocked. Board view is **left unchanged** (its `:` stays suppressed) — the palette primitive is built reusable, but only map view is wired to it now; board view adopting it is a later feature.
- **New picker level** — `palette_pick(Palette *p, const char *label, const BrowserItem *items, int n, void (*cb)(struct AppState *, const char *ref))`, a **sibling of `palette_prompt`**: it copies up to a capped number of items into the palette, fuzzy-filters over their names (reusing `fuzzy.c`), and fires `cb(st, ref)` on Enter (Esc cancels). Adds one new mode flag to `Palette` alongside `prompt`.
- **Wiring the active command set:** `palette_input_key`/`palette_draw` already take `(cmds, ncmds)`; main.c passes the *active* focus set (or the global registry) — stored as `AppState` fields set when the palette opens — so the palette module gains only the picker mode, nothing about "which mode."

## Testing

- **Pure headless unit:** the pin projection helper — given a map window `(u0,v0,u1,v1,w,h)` and a pin lon/lat, return in-bounds + local `(x,y)`. Lives in `mapmath.c` (scene-free) and is exercised by `mapmathtest`: a point at the window centre maps to the quad centre; a point at a known edge maps to the edge; an out-of-window point reports out-of-bounds. Round-trips against `mapmath_uv_to_lonlat` for the click-to-place path.
- **`palette_pick`** nav/filter is thin over the already-tested `fuzzy.c`; covered by the gauntlet build + live-verify.
- Full gauntlet per task: `./build.sh`, `./build.sh c89check`, `./build.sh asan`, `./build.sh metal` (+ `mapmathtest`). **No shader change → no MSL twin.**
- **Live-verify:** double-click a map to drop a pin (unlabeled marker appears); single-click select + Delete removes it; `:` → "Add place…" → pick a Place → named pin appears where that place is; `:` → "Rename pin" names the selected pin; create a new map over a region with seeded Places → those Places auto-appear as pins; pins hidden when their lat/lon is outside the map.

## Out of scope (deferred)

- Pan/zoom re-crop of a map (pins already re-project when it lands).
- Drag-to-move a pin.
- Live (non-snapshot) Place overlay; two-way pin↔Place sync.
- Declutter/fade of dense labels; edge-clamped off-map indicators.
- Board view's focus-palette command set (the primitive is built; population is a later feature).
