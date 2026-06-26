# Window Styles — Design Spec (Phase 2)

**Date:** 2026-06-26
**Status:** Approved (brainstorm), pending implementation plan
**Author:** Fran + Claude

## Goal

Give windows **shape styles** cycled with `←/→`: plain rectangle (today's default), round-top
**arched**, **pointed** gothic, **circular** oculus, and **french** (cross-mullion). The shape
lives entirely in the window's own frame + glass mesh; the wall opening stays a plain rectangle.
This is **Phase 2 of 3** (Phase 1 = core windows, shipped; Phase 3 = gable / wall-gable windows).

## Decisions (from brainstorming)

- **The wall hole stays a `w×h` rectangle; only the window's frame + glass mesh reshapes.** The
  opaque dark_wood frame fills the rectangle *minus* the glass aperture (corner **spandrels**),
  spanning the wall depth, so the shape reads from both faces with no see-through corners. No CSG
  (the fill is built "around the gap", consistent with the wall) → `emit_doored_wall`,
  `room_append_windows`, `room_rebuild_one`, collision, and the timber-frame reveal are all
  UNCHANGED.
- **`style` is a geometry param** (not meta), added to the `"window"` (`w,h,t,fw,style`) and
  `"window_glass"` (`w,h,style`) registry rows; default `0`. Values: **0 plain · 1 arched ·
  2 pointed · 3 circular · 4 french.** Param ⇒ cached + persisted, and a cycle just re-resolves
  the meshes. Backward-compatible: existing windows (4 params) and glass children (2 params) read
  `style` past their stored count, getting the registry default `0` = plain — they render
  unchanged until cycled.
- **Circular = a true inscribed circle** (diameter `min(w,h)`, centered); the leftover region out
  to the rectangle is opaque fill.
- **French = one rectangular pane** behind a vertical + horizontal dark_wood mullion bar (4-lite
  look), no spandrels.
- **`←/→` cycles style** while a window is selected — a direct mirror of the existing `↑/↓` color
  handler, including the "guard the arrow off camera-look while a window is selected" pattern.
- **No new shader → no MSL twin** (frame = `draw_mesh` + `g_dark_wood`; glass = `draw_glass`).
- A shaped window's rectangular wall opening behind the spandrels is accepted (a sliver may be
  glimpsable at sharp side angles) — the no-CSG simplicity is the trade we chose. Tunable later.

## Non-Goals

- No tracery / multiple sub-lights inside an arch; no figural stained-glass patterns.
- No per-style sill or opening-size change (Phase 1's sill + the `w×h` opening stand).
- No gable / wall-gable windows (Phase 3).
- No change to placement (windows still spawn plain, style 0), collision, persistence schema
  beyond the new `style` param, or the color-cycle feature (which composes with style).

## Background (current state — verified)

- **`make_window(MeshBuilder *b, sol_f32 w, h, t, fw)`** (mesh.c): a center-origin dark_wood ring
  (4 `aabb_box` stiles/rails) + sill ledge, depth `t/2 + WINDOW_PROUD`. **`make_window_glass(b,
  w, h)`**: a center-origin rectangular quad (`mb_push_vertex`/`mb_push_triangle`). Registry rows
  (mesh.c:1222-1223): `{ "window", 4, {w,h,t,fw}, {1.2,1.4,0.20,0.08}, emit_window }` and
  `{ "window_glass", 2, {w,h}, {1.2,1.4}, emit_window_glass }`. `emit_window`/`emit_window_glass`
  are the `(MeshBuilder*, const float *p)` wrappers.
- **The `↑/↓` color handler** (main.c ~12096): edge-detected via `win_color_was`, gated on a
  selected window (`window_on_wall`) and `board_view==0`, cycles `WINDOW_GLASS[]` and calls
  `window_set_glass` → which manages the `window_glass` child via the registry-rebuild law.
- **`window_glass_resize(AppState*, win)`** (main.c ~10096): the template for re-resolving a
  window/child mesh after a param change — `mesh_asset_key` → `scene_mesh_params_set` →
  `asset_release(oldkey)` → re-fetch → `memset(&mesh,0)` → `scene_resolve_meshes`.
- **`window_on_wall(Scene*, h)`** (main.c ~9017): the single "selected object is a window" test.
- **The arrow-look guard** (main.c ~10566-10569): `←/→` feed `in->look_dx`, `↑/↓` feed
  `in->look_dy`; a `win_look_free` bool already suppresses `↑/↓` when a window is selected — `←/→`
  are NOT yet guarded (they still pan the camera).
- `cmd_place_window` builds `p[4] = {w,h,t,fw}` and sets it via `scene_mesh_params_set(...,p,4)`.

## Architecture

### 1. The `style` param + dispatch

- Registry rows become `{ "window", 5, {"w","h","t","fw","style"}, {1.2,1.4,0.20,0.08,0.0}, ... }`
  and `{ "window_glass", 3, {"w","h","style"}, {1.2,1.4,0.0}, ... }`.
- `make_window` / `make_window_glass` gain a `sol_f32 style` parameter (the emit wrappers pass the
  new param: `make_window(b, p[0],p[1],p[2],p[3],p[4])`, `make_window_glass(b, p[0],p[1],p[2])`).
  Each switches on `(int)(style + 0.5f)` to dispatch per-style geometry. `mesh.h` declarations
  updated.
- `cmd_place_window` sets `p[4] = 0.0f` (plain) — a 5-element param array.

### 2. `make_window` — the shaped opaque frame

The frame is always **the opaque dark_wood fill of the `w×h` rectangle minus the glass aperture**,
center-origin, spanning the wall depth `[-ht, ht]` (`ht = t/2 + WINDOW_PROUD`). Decompose as:
- **a muntin border** of width `fw` hugging the glass aperture outline (the visible frame), and
- **spandrel infill** from the border's outer edge to the rectangle boundary (fills the corners so
  there's no see-through where the wall hole is rectangular but the aperture isn't).

Per style:
- **0 plain:** the current 4-rail ring (border only; no spandrel). Unchanged geometry.
- **1 arched:** straight side stiles + bottom rail up to a **springline** `ys = hh - r` where
  `r = min(hw, hh)` (clamped so `ys ≥ -hh`); above `ys`, a tessellated **semicircle** (radius `r`,
  center `(0, ys)`) as the muntin band + the **two top spandrels** filling between the arc and the
  box's top corners. Fixed arc segment count (e.g. `WINDOW_ARC_SEG 20`).
- **2 pointed:** like arched but the top is **two arcs** (each radius ~`w`, centers offset) meeting
  at an apex `(0, hh)`; two top spandrels.
- **3 circular:** an inscribed **circle** radius `r = min(hw, hh)`, center origin; muntin annulus
  (`r-fw..r`) + opaque fill from the circle out to the rectangle (the 4 corners, plus any straight
  margin when `w≠h`). Fixed segment count (e.g. `WINDOW_CIRC_SEG 32`).
- **4 french:** the plain ring **plus** a vertical bar (`x∈[-bw/2,bw/2]`, full inner height) and a
  horizontal bar (`y∈[-bw/2,bw/2]`, full inner width), `bw≈fw`, opaque, spanning the depth — over
  the (rectangular) glass. No spandrel.

A small shared helper emits an **arc band / fan** (tessellated) so arched/pointed/circular reuse
one tessellator. Winding + normals must match the existing boxes (outward on each face) so the
frame lights correctly (the engine doesn't backface-cull — single-sided flats must face out).

### 3. `make_window_glass` — the shaped pane

A center-origin triangle-fan/polygon at `z=0` matching the **aperture** (slightly inset by the
muntin so glass sits inside the frame):
- plain / french → rectangle (french keeps one pane behind the bars),
- arched → rectangle below `ys` + semicircle fan above,
- pointed → the pointed polygon,
- circular → a disc fan (radius `r-fw`).
Normals `+z` (matches the existing pane). Drawn unchanged in the sorted transparent pass, tinted by
`material.base_color`. Reuses the same arc tessellator (segment counts) as the frame so the glass
edge meets the muntin.

### 4. `←/→` style cycle (mirror of `↑/↓` color)

- New AppState `sol_bool win_style_was;` (beside `win_color_was`), init `SOL_FALSE`.
- Extend the arrow-look guard so `win_look_free` ALSO gates `←/→` → `in->look_dx` (so a selected
  window's `←/→` don't pan the camera), exactly as it already gates `↑/↓`.
- A handler mirroring the color one: when a window is selected and `board_view==0`, edge-detect
  `←/→` via `win_style_was`; read the window's current `style` param, step it `(idx ± 1) mod 5`,
  and `window_set_style(st, win, style)` + `scene_save` + `printf("window style: <name>\n")`.
- **`window_set_style(AppState*, win, int style)`** rewrites the `style` param on BOTH the window
  (params `{w,h,t,fw,style}` — read the current w/h/t/fw first, write all 5) and its
  `window_glass` child (params `{w,h,style}`), each via the registry-rebuild law
  (`mesh_asset_key`→`scene_mesh_params_set`→`asset_release`→`memset(&mesh,0)`→`scene_resolve_meshes`),
  re-fetching pointers after each realloc. (No wall rebuild — the hole is unchanged.)

### 5. Unchanged (the elegance)

The wall hole is the `w×h` rectangle: `emit_doored_wall`, `RoomOpening`, `room_append_windows`,
`room_rebuild_one`, collision, and the timber-frame reveal are untouched. **Resize** still works —
the aperture re-tessellates from the new `w/h` on the registry rebuild (a window resize already
re-resolves both meshes; it must carry the current `style` through). **Color** composes — the
shaped pane is tinted by the existing color child path.

## Data Flow

```
Place window           -> style param = 0 (plain), as today
Left/Right (selected)  -> window_set_style(win, style±1)
                          -> rewrite window param[4]=style + glass child param[2]=style
                          -> registry-rebuild both meshes (no wall rebuild)
make_window(style)     -> opaque frame = rect minus aperture (border + spandrels), per style
make_window_glass(style) -> tinted pane matching the aperture, drawn in the sorted glass pass
Resize (selected)      -> re-resolve both meshes carrying the current style (aperture re-tessellates)
Reload                 -> windows load with their style param; meshes resolve to shape
```

## File Touch List

- **`mesh.h`**: update `make_window` / `make_window_glass` declarations (add `style`); maybe a
  declared arc-tessellator helper if shared across TUs (likely static in mesh.c).
- **`mesh.c`**: style-aware `make_window` + `make_window_glass`; the arc/fan tessellator helper;
  the spandrel/infill builders; update the two emit wrappers + the two REGISTRY rows (5 / 3
  params); add `WINDOW_ARC_SEG` / `WINDOW_CIRC_SEG` constants.
- **`main.c`**: `cmd_place_window` writes a 5-element param array (style 0); the `←/→` handler +
  `window_set_style` helper + `win_style_was` field + init; extend the arrow-look guard to
  `look_dx`. The existing window resize path must carry `style` when it rewrites params.
- **`route_test.c`**: a headless assertion (shaped meshes have more geometry than plain — see
  Testing).

## Testing

- **Build gauntlet (all three):** `./build.sh c89check`, `./build.sh`, `./build.sh metal`.
- **Pure-logic (headless, links mesh.c — extend `routetest`):** build `make_window_glass` at
  style 3 (circle) and assert its `index_count` > the plain quad's (a disc fan has many tris);
  build `make_window` at style 1/2/3 and assert `index_count` > style 0 (spandrels add geometry).
  Deterministic, mirrors the existing sill-panel test.
- **Human live-verify (both backends):** place a window, `←/→` to cycle plain → arched → pointed →
  circular → french → plain; each shows the shaped frame + matching (tinted, if colored) glass; no
  see-through corners; resize a shaped window (aperture follows); color a shaped window (pane
  tints); reload persists the style. Eyeball arc smoothness, spandrel coverage, normals/lighting.

## Risks

- **The "rectangle minus aperture" fill without CSG is the real mesh-gen work** — the spandrel /
  annulus-to-rectangle tessellation must fully cover the corners (no see-through gaps) and wind
  correctly. Highest-effort, highest-risk part; budget the most care here.
- **Winding / normals on the tessellated arcs + fans** — the engine never backface-culls, so a
  single-sided flat lit from the wrong side goes dark; the frame faces and glass pane must face
  outward/`+z` consistently (the gable-lighting law).
- **Springline clamp** for wide windows (`w > h`): `r = min(hw,hh)` keeps the arch within the box;
  verify arched/pointed still look sane when `w ≈ h` or `w > h`.
- **Param-count migration:** existing windows/glass read `style` past their stored count → default
  0 (plain). `window_set_style` and the resize path must write the FULL param array (5 / 3) so the
  style sticks. Confirm no path writes a 4-element window array after style is introduced (would
  drop style back to default).
- **No new shader** ⇒ no MSL twin.
