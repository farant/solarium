# Map Pins — Iteration 1 Design

**Date:** 2026-07-01
**Status:** Approved, ready for plan
**Builds on:** the map-pins feature (branch `map-pins`, spec `2026-07-01-map-pins-design.md`). Three live-verify refinements.

## Why

Live-verify of map pins surfaced three changes: (1) clicking a pin highlights the whole map; (2) the marker should look like a classic map pin (circle + downward triangle, tip on the point); (3) "Add place…" should also let you create a *new* place via a lat/lon/label form. No new shader.

## Item 1 — a pin selects individually (bug fix)

`do_pick` already sets `selected_handle` to the pin (the leaf). The bug is in the highlight: the render resolves the selected object through `selection_root`, which for a `KIND_PLAIN` object returns its `group_root` — and a pin's group root is its parent map, so the whole map (and all its pins) light up.

**Fix:** add a `"pin"` exception to `selection_root` (main.c), exactly like the existing `"arrow"` one — return the pin's own handle so it selects/highlights individually:

```c
if (o->mesh_ref && strcmp(o->mesh_ref, "pin") == 0) return handle;
```

Combined with the map-view press-branch filter (which already refuses to keep the map selected), the map is unselectable in map view and a clicked pin highlights alone.

## Item 2 — the map-pin marker shape

Rewrite `pin_marker_mesh` (main.c) from a flat disc into a **classic map marker**: a circle **head** with a downward **triangle** whose **tip is at local origin `(0,0)`**, the head sitting above it (+Y). Because the tip is at the origin, `resolve_pin`'s existing `pos = (lx, ly, PIN_Z_OFFSET)` anchors the **tip** on the projected lat/lon (the marker rises upward from the exact point). Still a co-planar 2D icon on the map's +Z plane (orientation follows the map's local up, like the disc did).

- Geometry (all scaled to `mw`, tunable at live-verify): head radius `r ≈ 0.02*mw`; head centre at `(0, tri_len + r)` with `tri_len ≈ 2*r`; the triangle runs from the tip `(0,0)` up to the head's lower-left/lower-right. Built via `MeshBuilder` (fan for the head + one/two triangles for the point), `+Z` normals.
- The **label pass** raises its `top_y` offset so the name clears the head (currently it clears a disc of radius `0.03*mw`; now it must clear the full marker height `≈ tri_len + 2r`).

## Item 3 — "New place" via a form

### A reusable palette FORM mode (palette.h/.c)

A fourth palette mode alongside command / prompt / pick: **N labeled fields**, each an editable line.

- New `Palette` fields: `sol_bool form; char form_labels[PALETTE_FORM_FIELDS][32]; char form_vals[PALETTE_FORM_FIELDS][PALETTE_FIELD_CAP]; int form_n; int form_field; void (*form_cb)(struct AppState *, const char *const *vals, int n);` with `#define PALETTE_FORM_FIELDS 4`, `#define PALETTE_FIELD_CAP 64`.
- `void palette_form(Palette *p, const char *const *labels, int nfields, void (*cb)(struct AppState *, const char *const *vals, int n));` — opens the form, clears command/prompt/pick, copies up to `PALETTE_FORM_FIELDS` labels + zeroes the values, `form_field = 0`, stores `cb`.
- **Input:** `palette_input_char` appends to `form_vals[form_field]` (not `query`) when `p->form`. In `palette_input_key`, a `form` branch: **Tab / ↓** → next field (wrap), **↑** → previous field, **Backspace** → edit the current field's value, **Enter** → gather `const char *vals[]` pointing at each `form_vals[i]`, close + clear, `cb(st, vals, form_n)`; **Cancel** → clear form + close. This needs a **new `PALETTE_KEY_TAB`** in the `PaletteKey` enum, mapped from `GLFW_KEY_TAB` in main.c's palette-key routing (the palette owns the keyboard while open).
- **Draw:** `palette_draw` gets a `form` branch — draw each field as a `label: value` row, the current field highlighted with a trailing `_` cursor; the other modes are unchanged.
- All three opens (`palette_open_now`, `palette_prompt`, `palette_pick`) set `p->form = SOL_FALSE`; Cancel/Enter clear `form` + `form_cb`.

### The "New place" wiring (main.c)

- **`cmd_add_place`** prepends a sentinel row `{ name: "+ New place", ref: "+new" }` (ASCII) atop the existing Places rows in the picker.
- **`add_place_pin_cb(st, ref)`** branches: if `ref == "+new"` → open the form `palette_form(&st->palette, {"lat","lon","label"}, 3, new_place_form_cb)`; else the existing behavior (pin from the chosen Place handle).
- **`new_place_form_cb(st, vals, n)`**: `lat = atof(vals[0]); lon = atof(vals[1]); label = vals[2]`. Create a **new Places-catalog entry** — `scene_add` a child of `places_anchor(st)` with meta `name`=label, `lat`, `lon`, `zoom`="5", `basemap`="relief" (matching the seeded Place shape). Then **drop a pin** on the current map (`st->map_view`) at that lat/lon/label; resolve + select; save.
- **Shared helper `add_pin_to_map(AppState *st, sol_u32 map, double lat, double lon, const char *name)`** — creates a `"pin"` child of `map` with lat/lon/name meta, `resolve_pin`s it against `map_window_of`, selects it, saves. Used by both `add_place_pin_cb` (existing-place branch) and `new_place_form_cb`, removing the duplicated pin-creation block. (The double-click placement in the press handler stays as-is.)
- **Validation (light):** blank/garbage lat/lon parse via `atof` to `0.0` — a pin at (0,0), visible and deletable. No hard validation in v1; note it. `st->map_view == 0` guard as in the existing callback.

## Slice & testing

Two tasks:
1. **Pin polish** (main.c): the `selection_root` `"pin"` exception + the teardrop `pin_marker_mesh` + the label-offset bump. Verifiable by clicking a pin (only the pin highlights) and by look (teardrop, tip on the point).
2. **New place** (palette.h, palette.c, main.c): the palette form mode (+ `PALETTE_KEY_TAB`) + the `cmd_add_place` sentinel + `new_place_form_cb` + the `add_pin_to_map` helper. Verifiable end-to-end: `:` → Add place → "+ New place" → fill lat/lon/label → a new catalog place + its pin appear.

- No new pure-logic module → no new unit test (the form field-nav is UI; the reshape/selection are visual). Full gauntlet per task: `./build.sh`, `c89check`, `asan`, `metal`. **No shader change → no MSL twin.**
- **Live-verify:** click a pin → only that pin highlights, never the map; markers are tip-anchored teardrops; `:` → Add place shows "+ New place" atop the list; picking it opens a lat/lon/label form (Tab/↑↓ between fields); submitting creates a catalog place (visible in Add-place next time and the `;` browser) and a pin on the current map.

## Out of scope (deferred)

- Editing an existing Place's fields (this only adds new ones).
- Hard numeric validation / inline error messages on the form.
- Applying the form mode elsewhere (it's built reusable; only New-place uses it now).
- A distinct pin color/icon per source (all pins look the same).
