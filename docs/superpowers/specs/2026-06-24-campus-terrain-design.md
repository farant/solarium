# Campus Terrain — Design

**Date:** 2026-06-24
**Status:** Approved (brainstorm); ready for implementation planning.

## Concept

A **per-world opt-in** that repurposes the existing terrain generation. Instead of rooms (and their walkways) floating in empty space, an opted-in world gets a single **rectangular terrain** generated beneath it: graded so each room sits flush on a flat **pad** at its own floor height, with **noise hills** filling the space between rooms, plus **grass and trees**. Navigation (walkways, descent, portals, the spatial filesystem) is unchanged. Floating worlds and campus worlds coexist; toggling campus off restores floating.

This is a **terrain underlay**: it adds ground + flora beneath the existing room layout. It does NOT change how rooms or walkways are authored, and it does NOT re-lay-out rooms.

## The height mechanism (the heart)

Today `terrain_height(params, count, lx, lz)` (mesh.c:538) is **pure fBm noise** masked to a zero rim — there is no way to pin it to a specific height at a specific spot. The campus inverts authorship: **the rooms author the terrain.**

A new height field, `campus_height(lx, lz)`, blends **pads + noise**:

- **Pads.** Each room contributes a pad `{cx, cz, hw, hd, floor_y}` — its footprint center/half-extents and its floor height (the room's world Y). Gathered from the world's rooms in campus-local coordinates, reusing the existing `editor_room_rect` footprint logic (editor.c:12, `RoomRect`).
- **Inside a room footprint** → height = that room's `floor_y` (dead flat pad).
- **Apron** → across a short margin around each footprint, the height eases from `floor_y` out toward the between-rooms surface (smoothstep).
- **Between rooms** → a smooth base that interpolates the nearby pads' heights (so the ground flows from one room's height to another), **plus** fBm hills (the existing `terrain_fbm`, mesh.c:568) so the in-between is not flat.
- **Lowest pad wins** where footprints overlap in plan: the terrain grades to the LOWEST overlapping `floor_y`; higher stacked rooms simply float above the ground (un-grounded) until the author spreads them out. This makes the feature robust against today's vertically-stacked layout (home above its roots, descent sub-rooms, etc.).
- **Zero-rim mask** — the existing border mask (mesh.c:584) still applies at the rectangle's edge, so the campus rises out of its own perimeter like today's islands.

**The base + hill blend (between rooms):** the smooth base interpolates pad heights (e.g. inverse-distance-weighted or nearest-pad blend over the gathered pads — small pad count, so cheap), and fBm hills are added scaled by `(1 - pad_influence)` so hills vanish on the pads and grow between them. Exact blend math is an implementation detail for the plan; the requirement is: flat at `floor_y` on each footprint, smoothly varying + hilly between, never poking above a room's own floor on its pad.

## Architecture

The campus is **derived** (always rebuilt from the rooms, never hand-edited). The only persisted state is a per-world flag.

1. **Campus generator** (new). On a rebuild it:
   - Gathers the active campus-world's room pads (footprint + floor_y), filtered by workspace (`scene_object_active`).
   - Computes the **rubber-band rectangle** = bounding box of all pad footprints + a margin `M`, giving the campus terrain's center and `w`/`d`.
   - **Bakes the height field into a grid** (an NxN sampling of `campus_height`) stored with the campus terrain. Baking once and sampling cheaply keeps the pad-blend off the per-query hot path. All consumers — the terrain mesh, flora scatter, the standing query, the editor — read this one baked grid (bilinear sample). Grid resolution chosen to resolve room footprints (the flat pads) and the hills.
   - Builds the campus terrain **mesh** from the grid (reusing `make_terrain`'s skirt/base/normals approach, but reading the baked grid instead of pure noise).

2. **Campus terrain object** — one per campus-world. Like the existing `terrain` mesh object but its geometry comes from the baked grid. Carries (or references) the baked grid so consumers can sample it by handle.

3. **Consumers sample the baked grid:**
   - **Standing** — `ground_under` (main.c:5762) already takes `max(terrain, architecture floor)`; it samples the campus grid where the player is over the campus.
   - **Flora** — meadow (grass, main.c:3137) and forest (trees, main.c:5303) already scatter per-terrain, sampling height to sit on the ground; point them at the campus and sample the grid, **masking out the room footprints** so grass grows on the hills, not indoors.
   - **Editor** — terrain footprint/snap math samples the grid.

## Lifecycle & coexistence

- **Opt-in:** a `:` command **"Campus mode"** sets a persistent `campus` flag on the world (workspace anchor `meta`, like the existing `workspace` partition flags). Toggling off removes the campus terrain (and flora) and restores the floating look.
- **Auto-rebuild:** when the active world has the flag, the campus generator runs on the **same structural events that already rebuild routes/flora** — `load_palace`, `world_rebuild`, room add/move/delete, editor commit — **never per-frame.**
- **Coexistence:** worlds without the flag are completely untouched (no campus terrain). Campus worlds and floating worlds live in the same scene, isolated by the existing workspace filter (`active_ws` + `scene_object_active`).
- **Existing standalone islands/abbeys** (the editor's draggable terrain islands and plot-linked churches) are separate scenery and are unaffected — the campus is the ground generated under the *rooms*, not a replacement for those.

## New vs reused

**New:**
- `campus_height` pad+noise blend and the bake-to-grid step.
- Room-pad gathering (reusing `editor_room_rect`).
- Rubber-band rectangle sizing.
- The grid-sampling seam for consumers (mesh, flora, standing, editor).
- The opt-in command + persistent flag + the rebuild hook.

**Reused:**
- Noise / `terrain_fbm`, the zero-rim mask, `make_terrain`'s skirt/base/normals.
- The slope/height shader palette (`uTerrainBlend`).
- Flora scatter (meadow + forest).
- The standing query (`ground_under`).
- Editor footprint math (`RoomRect`, `editor_room_rect`).
- Workspace filtering (`active_ws`, `scene_object_active`).

## Non-goals (scope control)

- No terrain collision walls — standing stays a read-only height query (rooms already do the blocking).
- No automatic re-layout of rooms — positions are sacred; the campus grounds whatever footprints already exist.
- No water, paths, biomes, or new walkway behavior — walkways stay as the existing stepped bridges between rooms, now over real terrain.
- No change to room/walkway authoring.

## Build order — two milestones

Each milestone is its own plan → build → human live-verify → ff-merge.

**Milestone 1 — Core campus (no plants):**
- `campus_height` (pads + noise + lowest-wins + rim), pad gathering, rubber-band rectangle, bake-to-grid, the campus terrain mesh.
- The opt-in `:` command + persistent flag + rebuild hook on the structural events.
- Standing on the campus (`ground_under` samples the grid).
- Verify: an opted-in world shows rooms grounded on a graded terrain with hills between; you can walk the surface; toggling off restores floating.

**Milestone 2 — Flora:**
- Grass (meadow) + trees (forest) scattered on the campus, sampling the grid, masking out room footprints.
- Verify: campus reads as grounds with grass on the hills and trees between rooms.

## Key implementation decisions to settle in the plan

- **Grid resolution & the bake** — high enough to resolve flat pads (room footprints) without staircasing the hills; bounded memory per campus.
- **Base interpolation between pads** — the exact scheme (IDW vs nearest-pad-with-falloff) that keeps slopes between rooms sane.
- **Hill amplitude / apron width** — tuning knobs (defaults reasonable, exposed like the other `*_TILE_M`/`*_T` constants). Hills between rooms must stay modest enough that they don't bury the walkways bridging rooms (or carve a low corridor under each walkway route) — the walkways still need to read as stepped bridges, not tunnels.
- **Where the baked grid lives** — on the campus terrain object vs a per-world campus state keyed by handle; whichever lets flora/standing/editor sample it cleanly.
- **Mask for grass** — exclude room footprints (+ apron) from meadow/forest scatter.
