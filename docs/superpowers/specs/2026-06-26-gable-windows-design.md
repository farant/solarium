# Gable Windows — Design Spec (Phase 3)

**Date:** 2026-06-26
**Status:** Approved (brainstorm), pending implementation plan
**Author:** Fran + Claude

## Goal

Let windows sit **in the gable triangle** (a rose/oculus in the peak) and **span the wall→gable
seam** (a tall window rising from the wall into the gable). The gable mesh — currently a solid
triangle — learns to cut a rectangular notch for a window whose top rises above the wall. This is
**Phase 3 of 3** (Phase 1 = core windows; Phase 2 = shape styles).

## Decisions (from brainstorming)

- **Split-at-wall-top model.** A window's opening `[sill, lintel]` is split at the wall top `h`:
  the wall shell cuts `[sill, min(lintel, h)]` (it already caps the header at `h`), the gable cuts
  `[max(sill, h), lintel]`. Gable-only (`sill ≥ h`) and spanning (`sill < h < lintel`) are the same
  mechanism — just where the split falls. A window entirely below `h` is today's window, untouched.
- **The gable cuts a RECTANGLE; the window meshes make the shape.** Same no-CSG trick as Phase 1/2 —
  a rose window = a rectangular gable notch + the existing circular frame/oak-fill/glass meshes.
  **No window-mesh changes in this phase.**
- **One window per gable end (v1).** The common case (a single rose or tall window per ridge-end).
  Multiple windows on one gable end is a flagged limitation (Fran: "one seems good for now").
- **The gable notch reveal is solid gable material** (no dark_wood casing) — like the wall before
  the timber-frame polish.
- **Placement unchanged; drag/resize up into the gable.** You place a window low (default sill) and
  carry it up — the move/resize clamps are already gable-aware (apex topcap, width fit to the
  triangle) on the two ridge-end walls; only the gable *mesh* cut was missing. Eave walls unchanged.

## Non-Goals

- More than one window per gable end (v1 limitation).
- No dark_wood reveal casing inside the gable notch (solid gable material).
- No change to the window object, its frame/oak-fill/glass meshes, styles, color, persistence, or
  collision (the wall stays solid for walking; the gable hole is visual).
- No special "gable window" placement gesture — reuse place + drag/resize.

## Background (current state — verified)

- **The gable** is built in `room_frame_build` (main.c) when `g_roof_mat` + `g_wall_mat` textures
  are loaded: a `gi` loop over the two ridge ends emits, per end, a **solid** triangle per face via
  `gable_tri(mb, a, b, c, n, ...)` (main.c:4552 — one triangle, 3 verts). Geometry (main.c:4706-):
  - `rax` = ridge axis (1 ⇒ ridge along X, gable ends at `x = ±along_h`; 0 ⇒ ridge along Z, ends at
    `z = ±along_h`). `along_h` = half ridge length.
  - Per end `gi`: `ge = ±along_h` (the end's plane), `gout = ±1` (outward), `off = bent_pt(rax,
    gout*ROUTE_WALL_T, 0, 0)` (thickness extrude), `nin`/`nout` (inner/outer normals).
  - Base corners `eL = bent_pt(rax, ge, -sh, h)`, `eR = bent_pt(rax, ge, sh, h)`, apex
    `ap = bent_pt(rax, ge, 0, ridge_y)`. So in the gable's 2D `(s, y)` plane: base `[-sh, sh]` at
    `y = h` (wall top), apex `(0, ridge_y)`; `sh` = base half-width; `bent_pt(rax, ge, s, y)` maps
    `(s, y)` to 3D on the inner face, `+ off` to the outer face.
  - `ridge_y = h + dy` (peak above floor). `room_frame_build` ALREADY receives `ops` (the openings,
    including windows) — currently only used for `emit_door_reveal`, not for cutting the gable.
- **`emit_doored_wall`** (mesh.c): per opening, a sill box `[0, sl[i]]`, piers, and a header
  `if (oy[i] < h)` → `[oy[i], h]` (so a `lintel ≥ h` opening reaches the wall top with no header).
  The sill box is NOT capped at `h` — an opening with `sill > h` would emit a too-tall sill box.
- **`room_append_windows`** (main.c) derives each window's `RoomOpening`: `center` (room-local
  along-wall), `width`, `height` = lintel = `pos.y + h/2`, `sill` = `pos.y − h/2` (≥ 0). No clamp to
  the wall top — so a high window already reports `lintel > h`.
- **`room_rebuild_one`** (main.c) re-cuts a room's shell (`make_room_doored`) AND frame
  (`room_frame_build`, which builds the gable) from `ops`. So the gable re-cuts on any window
  change (place / drag-release / resize-release / style / delete) — the existing hook.
- **Move/resize clamps:** the window drag uses `wall_clamp_run_cy` and resize uses
  `wall_gable_geom` (main.c:9182) → `topcap = is_gable ? apex_y : wall_top`, and the along-wall
  limit narrows inside the triangle. So a window can already be positioned into the gable; the gable
  just renders solid behind it today.

## Architecture

### 1. The wall-shell cap (`emit_doored_wall`, mesh.c)

Cap each opening's sill at the wall height so a window above the wall doesn't make a too-tall sill
box and an entirely-above window reads as solid wall:
```c
/* in the gather loop, after sl[k] = ops[i].sill; */
if (sl[k] > h) sl[k] = h;   /* a window rising into the gable: the wall part stops at the top */
```
With this, `sill ≥ h` → sill box `[0, h]` fills the strip (solid, no wall hole); `sill < h < lintel`
→ sill box `[0, sill]` + opening `[sill, h]` + no header (lintel ≥ h) = the wall part of a spanning
window. (The header guard `oy[i] < h` already handles `lintel ≥ h`.)

### 2. The gable notch (the hard part — `room_frame_build`, main.c)

Replace the per-end solid `gable_tri` pair with a **triangle-minus-rectangle** builder that reads
the window opening for that gable's wall. Add a helper:
```c
/* emit one gable end face (inner OR outer) as a triangle minus a rectangular
   notch [s0,s1]x[yb,yt] in the (s,y) plane, mapped to 3D by bent_pt(rax, ge, .)
   + offv. halfw(y) = sh*(ridge_y - y)/(ridge_y - h). */
gable_notched_face(mb, rax, ge, offv, n, sh, h_wall, ridge_y, s0, s1, yb, yt, reversed);
```
For a notch (`yb = max(sill, h_wall)`, `yt = min(lintel, ridge_y)`, `s0 = center − w/2`,
`s1 = center + w/2`, all clamped so the top corners fit `|s| ≤ halfw(yt)`), the face decomposes into
four regions, each one/two triangles (winding chosen so the normal matches `n`; reversed for the
outer face):
- **bottom band** (if `yb > h_wall`): the full triangle slab `y ∈ [h_wall, yb]` — a trapezoid
  `(−sh,h_wall)`,`(sh,h_wall)`,`(halfw(yb),yb)`,`(−halfw(yb),yb)`.
- **left strip** `y ∈ [yb, yt]`: quad `(−halfw(yb),yb)`,`(s0,yb)`,`(s0,yt)`,`(−halfw(yt),yt)`.
- **right strip** `y ∈ [yb, yt]`: quad `(s1,yb)`,`(halfw(yb),yb)`,`(halfw(yt),yt)`,`(s1,yt)`.
- **top piece** (if `yt < ridge_y`): the triangle `(−halfw(yt),yt)`,`(halfw(yt),yt)`,`(0,ridge_y)`.

Then the **notch reveal walls** (so you don't see through the gable thickness `off`) — solid gable
material, spanning inner→outer at each notch edge: left `s=s0`, right `s=s1`, top `y=yt`, and bottom
`y=yb` **only if** `yb > h_wall` (an interior/gable-only notch; a spanning notch's bottom meets the
wall hole at `h`).

In the `gi` loop: determine this end's **wall index** from `rax`+`gi` (rax=1 ⇒ `ge=+along_h` → E,
`−along_h` → W; rax=0 ⇒ `+along_h` → S, `−along_h` → N), find the (one) window opening with
`ops[i].wall == that wall && ops[i].height > h_wall`; if found, build the two **notched** faces;
else build the existing **solid** `gable_tri` pair. UVs position-based, matching `gable_tri`'s scale
(`s/WALL_TILE_M`, `y/WALL_TILE_M` or the existing convention). Keep the rake caps as they are.

### 3. Placement / drag / resize (verify, minimal change)

Placement lands the window low (default `WINDOW_DEF_SILL`) as today. **Verify** the move
(`wall_clamp_run_cy`) and resize (`wall_gable_geom`) clamps already let a window reach the apex on
ridge-end walls and fit its width to the triangle — they do (Phase 1). The gable re-cuts on
drag-release/resize-release because those already call `room_rebuild_one`. No new placement gesture.
(If a clamp turns out to cap at the wall top, raise it to the gable apex via `wall_gable_geom` — but
this is expected to already work.)

### 4. Window meshes — unchanged

The window object, its `make_window`/`make_window_fill`/`make_window_glass` meshes, styles, oak
fill, color, persistence, and per-room rebuild are untouched. The gable cuts the rectangular
bounding box; the window meshes provide the shape. A circular/arched rose window in the gable works
for free.

## Data Flow

```
Window placed/dragged/resized up into the gable
  -> room_rebuild_one(room)
       -> room_append_windows: opening {center, sill, lintel = pos.y +/- h/2}
       -> make_room_doored / emit_doored_wall: wall cut [sill, min(lintel,h)] (sill capped at h)
       -> room_frame_build: gable end whose wall matches -> notched faces [max(sill,h), lintel]
  -> the window's own frame/oak-fill/glass meshes render the shape across the seam
reload: same derive on load (room rebuild re-cuts wall + gable)
```

## File Touch List

- **`mesh.c`**: `emit_doored_wall` sill cap (one line) + a headless test in `route_test.c`.
- **`main.c`**: the `gable_notched_face` helper; the `gi`-loop change in `room_frame_build` (wall
  mapping + find-window + notched-vs-solid dispatch + reveal walls); verify the move/resize clamps.
- **`route_test.c`**: assert `emit_doored_wall` with a `sill ≥ h` opening produces the SAME geometry
  as no opening (the gable-only window leaves the wall solid).

## Testing

- **Build gauntlet (all three):** `./build.sh c89check`, `./build.sh`, `./build.sh metal`.
- **Pure-logic (headless, `routetest`):** `make_room_doored` with a window opening whose `sill ≥ h`
  (entirely in the gable) emits the SAME index count as the same room with NO opening (the wall is
  solid — the cap works). And a spanning opening (`sill < h < lintel`) differs from a fully-below
  one. (The gable notch itself is in `room_frame_build` in main.c, not headlessly linkable — covered
  by live-verify.)
- **Human live-verify (both backends):**
  - Place a window on a ridge-end (gable) wall, drag/resize it up so it crosses the wall top → the
    gable opens a matching notch; the window reads continuous across the seam; no see-through gap and
    no solid gable behind the upper part.
  - A small window dragged fully into the gable peak → a clean notch in the triangle (gable-only);
    the wall below stays solid.
  - Cycle it to circular/arched (a rose window) → the shape + oak fill render in the gable; the
    notch is the rectangle behind.
  - Reload → persists and re-cuts. Delete → the gable closes back to a solid triangle.
  - An eave (long) wall window is unaffected (no gable there).

## Risks

- **★ The triangle-minus-rectangle tessellation** is the real work: correct winding/normals on BOTH
  faces (the engine never backface-culls — a wrong-normal face goes dark), no gaps between the four
  regions, the reveal walls fully closing the thickness, and the width clamped so the top corners
  stay inside the triangle. Highest-care part.
- **Gable-end → wall-index mapping** (`rax`+`gi`) must be exact, or the notch lands on the wrong end
  / doesn't appear. Verify against `bent_pt`'s axis convention.
- **The wall↔gable seam at `h`** is a butt joint (wall hole top meets gable notch bottom) — a
  hairline could show; acceptable, tunable.
- **Window must fit the triangle:** clamp `s0,s1` to `±halfw(yt)`; the move/resize clamps should
  prevent an oversized window, but the builder clamps defensively.
- **One window per gable end** (v1): if two windows are on one gable end, the builder cuts the first
  matching one and ignores the rest (documented limitation).
- **No new shader** ⇒ no MSL twin (the gable uses the existing lit path).
