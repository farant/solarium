# Inventory — Design

**Date:** 2026-06-23
**Status:** Approved

## Goal

Add an **inventory**: a personal bag you can stow carriable objects into and take
them back out of, browsed through a modal grid screen that renders each item as a
**live 3D thumbnail**. `i` opens the screen; `Enter` (while carrying) stows the
held item; clicking a tile takes that item into your hands. Pictures are reusable
"stamps" (placing one leaves the original in the bag); every other item is unique.
The bag is **global** — one bag that follows you across all workspaces.

## Background — what exists today

- **Carry** (`main.c`): `E` toggles pick-up/place via `cmd_carry_toggle`
  (main.c:7033). The carried handle lives in `AppState.carried`; pick-up saves
  `carry_origin` / `carry_prev_parent` / `carry_prev_rot`; `carry_update`
  (main.c:7313) floats the carried object in front of the camera each frame and
  sets the wall/furniture aim flags. `carry_target` (main.c:6872) defines what is
  pickable (KIND_PLAIN cards, codex anchors — not room structure, not already
  parented). Drop routes to floor / furniture-file / wall-mount / door-descend.
- **Hidden objects:** the engine never deletes a hidden object — it *filters* it.
  `reader_source` is skipped in the render loop (main.c:10979); inactive-workspace
  objects are skipped in every reader (render/collision/route/BVH/editor) via
  `scene_object_active` → `workspace_of` parent-walk (workspace.c:8). Same pattern
  we reuse for the bag.
- **2D overlay UI** (`ui.h`): `ui_begin/ui_quad/ui_glyph_quad/ui_end`, drawn after
  tonemap, over the final frame. The command **palette** is the modal precedent —
  while `palette.open`, `read_input` returns early (main.c:7834) and the palette
  owns the keyboard. The **RTS editor** is the cursor-release precedent (it uses
  the mouse for direct manipulation and gates camera-look on `!editor.active`).
- **Render-to-texture** (`rhi.h`): `rhi_create_render_target(w,h,fmt)` +
  `rhi_begin_pass/rhi_end_pass` + `rhi_render_target_texture`. Shadow maps, bloom,
  god-rays, SSAO and the BRDF LUT all render offscreen this way. The forward PBR
  draw + IBL bind already exist; a thumbnail is one mesh drawn to a small target.
- **Scene model** (`scene.h`): `SceneObject{ handle, parent, nid, kind, pos/rot,
  mesh_ref, mesh_params, material, tex_ref, meta map, content, ... }`.
  `scene_add` / `scene_remove` / `scene_get` (pointer valid only until the next
  `scene_add`); `scene_meta_set/get`; `scene_save/load` round-trips STML.
- **Key bindings:** `i` / `GLFW_KEY_I` is currently the *irradiance-view* debug
  toggle (main.c:7797) — a dev-only command in `g_commands[]`. `Enter` has no
  binding in normal play (only in reader / place-furniture / note-edit / palette).

## The inventory anchor (data model)

A single **global anchor** SceneObject: `parent = 0`, no mesh, tagged
`meta["inventory"]="1"`, **untagged workspace** (so it is reachable everywhere).
Created on demand the first time something is stowed (a `inventory_anchor(Scene*)`
helper in `main.c` that finds the existing anchor by its meta key or creates one).

**"Stowed" is defined as *being in the inventory subtree*** — an object whose
parent chain reaches the inventory anchor. No separate flag. This persists in
`scene.stml` for free (the items are ordinary objects parented to the anchor).

### The bag filter (the new filter law entry)

A predicate `object_stowed(Scene*, handle)` = "the inventory anchor is an
ancestor." Every scene reader skips stowed objects, exactly as it skips
inactive-workspace objects:

- **render** (main.c render loop): skip if `object_stowed`.
- **collision / route / BVH / editor:** skip if `object_stowed`.
- The inventory anchor itself is mesh-less, so it is inert in all passes anyway.

Because the bag is global (untagged), `scene_object_active` would *show* its
contents in every workspace if we relied only on workspace filtering — the
`object_stowed` check is what actually hides them. This is the load-bearing guard.

## Interactions

| Input | Context | Effect |
|-------|---------|--------|
| `i` | normal play | open the inventory screen (cursor released) |
| `i` / `Esc` | screen open | close the screen |
| `Enter` | carrying an item | **stow** the carried item into the bag |
| `E` | carrying an item | place in the world (unchanged) |
| click a tile | screen open | **take** that item into your hands, close screen |
| ‹ › arrows / Left·Right | screen open, >1 page | change page |

- **Stow** (`Enter` while `carried != 0`): re-parent the carried object under the
  inventory anchor (local pose irrelevant — it is hidden), clear `carried` and the
  carry/aim state, `scene_save`. Build/refresh its thumbnail (lazy; see below).
- **Take** (click a tile): resolve the clicked cell → item handle.
  - **Unique item** (card, note, book/codex, …): re-parent to world at the carry
    position, set `carried`, close the screen. The tile disappears.
  - **Stamp item** (`mesh_ref == "picture"`): **clone** the object (new
    `scene_add` copying mesh_ref/params/material/tex_ref/content/relevant meta),
    parent the clone to the world, set `carried = clone`, close. The original stays
    under the anchor (its tile and thumbnail remain). Placing the clone on a
    wall/board is then the ordinary picture-mount path — no placement-time special
    case. (Cloning rule keyed on `mesh_ref == "picture"`; a small
    `inventory_is_stamp(mesh_ref)` predicate so the rule has one home.)
- **`i` rebind:** the irradiance-view command's `key` field becomes `0`
  (palette-only, still runnable by name via `:`); a new "Inventory" command takes
  `GLFW_KEY_I`.

### What can be stowed

Whatever `carry_target` already deems pickable (KIND_PLAIN cards, codex anchors,
pictures, notes). Stow operates on the currently-carried object, so the carry
rules define eligibility — no separate allow-list.

## The inventory screen (modal overlay)

Drawn in the post-tonemap UI phase, like the palette:

1. A dim full-screen backdrop quad (`ui_quad` with alpha).
2. A grid of cells — **v1: 4 columns × 3 rows = 12 per page** (constants, tunable).
   Each cell: the item's **thumbnail** (textured quad) + its **name label**
   (`meta["name"]`, or a kind fallback) drawn below via `ui_glyph_quad`.
3. A page indicator (`"page 2 / 4"`) and clickable ‹ › arrows when
   `item_count > per_page`.

**Modal behavior:** while open, `read_input` returns early (palette precedent) so
movement keys are suppressed; the **mouse cursor is released** (editor precedent)
so cells and arrows are clickable. Closing restores cursor capture.

### Pure layout math → `inventory.c` / `inventory.h`

Scene-free, unit-tested (the `furniture.c` precedent):

```c
/* page geometry: how many pages for n items, clamped page index */
int inv_page_count(int n_items, int per_page);
int inv_clamp_page(int page, int n_items, int per_page);

/* the pixel rect of grid cell `slot` (0..per_page-1) on the current screen,
   given cols/rows and margins. Fills x,y,w,h (framebuffer pixels, y-down). */
void inv_cell_rect(int slot, int cols, int rows,
                   int screen_w, int screen_h,
                   float *x, float *y, float *w, float *h);

/* hit-test: which slot (0..per_page-1) does pixel (px,py) fall in? -1 = none.
   Returns the slot only; the caller maps slot -> item via page*per_page+slot. */
int inv_hit_slot(float px, float py, int cols, int rows,
                 int screen_w, int screen_h);

/* the ‹ / › arrow rects, for click hit-testing (or -1 region helpers). */
void inv_prev_rect(int screen_w, int screen_h, float *x,float *y,float *w,float *h);
void inv_next_rect(int screen_w, int screen_h, float *x,float *y,float *w,float *h);
```

`main.c` owns the scene glue: gathering the anchor's children into a display list,
mapping `page*per_page + slot` → item handle, drawing tiles, dispatching clicks.

## Thumbnails (live 3D, lazily cached)

- **Render:** one small color render target (~256², SRGB8 or RGBA8). For an item,
  set a fixed 3/4 thumbnail camera framing the mesh bounds, bind the IBL
  environment, and issue the **existing forward PBR draw** for that one mesh at
  origin. No new *render* shader.
- **Cache:** the resulting `RhiTexture` is stored keyed by item handle (a small
  fixed-size cache array in `AppState`, e.g. `InvThumb { sol_u32 handle;
  RhiRenderTarget rt; } thumbs[INV_THUMB_CAP]`). Reused while the screen is open.
- **Lazy build:** a thumbnail is rendered on first need — when an item is stowed,
  or when the screen opens and an item has no cached thumbnail (covers reload,
  since nothing is persisted to disk). Freed when the item leaves the bag (and an
  LRU/evict if the bag exceeds `INV_THUMB_CAP`).
- **Offscreen-outside-frame caution:** if a thumbnail is rendered between frames
  (e.g. at stow time, not inside the main pass), it needs `rhi_flush()` on Metal —
  the day/night re-bake lesson (a GPU op outside the frame loop must flush). The
  simplest safe choice: render any missing thumbnails **at the top of the frame**
  in which the screen is open, inside the normal command stream, before the UI
  phase. The plan will use that ordering.

### Drawing a thumbnail texture in the overlay

The UI batch today binds only the SDF font atlas + solid color. Drawing an
arbitrary texture as a 2D quad needs a textured-quad draw. **Reuse the existing
blit/post path** (the tonemap/post passes already sample a texture to a quad and
have their GLSL+MSL twin) by drawing each thumbnail as a sub-rect (viewport or a
positioned quad) during the UI phase, with the `ui_quad` panels/labels composited
around them. **Goal: no new shader, no new MSL twin.** The plan picks the exact
mechanism (extend `ui.h` with a `ui_image` that reuses the blit shader vs. a small
dedicated textured-quad helper) and documents whichever avoids a new twin.

## Files

- **Create:** `inventory.c` / `inventory.h` — pure layout/hit-test/pagination math.
- **Create:** `inventory_test.c` — unit tests for the above.
- **Modify:** `main.c` — the anchor helper, `object_stowed` + the reader-skip
  guards, stow/take in `cmd_carry_toggle` + the `Enter` path, the screen overlay
  draw + click dispatch, the thumbnail cache + render, the `i` rebind.
- **Modify:** `command.h` / `g_commands[]` — drop `i` from irradiance, add the
  Inventory command.
- **Modify:** `build.sh` — an `inventorytest` target (the `furnituretest`
  precedent).
- Possibly **Modify:** `ui.h` / `ui.c` — only if a `ui_image` helper is the chosen
  thumbnail-draw mechanism (must reuse an existing shader → no new MSL twin).

## Scope notes (YAGNI)

- **No item reordering / sorting / search in v1** — natural fill order, paginated.
- **No stack/quantity** — each tile is one object.
- **No dedupe on stow** — stowing the same picture twice makes two bag entries
  (harmless; a possible later refinement).
- **Thumbnails are not persisted** — always rebuilt lazily from the live mesh.

## Testing

- **Pure math** (`inventory.c`) → unit-tested in `inventory_test`: page counts,
  page clamping, cell rects (non-overlapping, inside the screen, grid order),
  hit-test (a pixel inside cell k returns k; gaps return -1), arrow rects.
- **Visual / interactive** → human live-verify. Build gauntlet must be green:
  `./build.sh c89check && ./build.sh debug && ./build.sh metal &&
  ./build.sh inventorytest`.
- Manual checks: stow several different objects (`E` then `Enter`) → `i` shows them
  as 3D thumbnails; click one → it lands in your hands; place it; a picture taken
  from the bag stays in the bag after mounting; pagination appears past 12 items;
  reload → bag persists, thumbnails rebuild; the bag's contents never appear in the
  world or block movement; the bag is the same across a portal hop.

## Constraints (project laws)

- Strict C89; build gauntlet (incl. `inventorytest`) green on **both** backends.
- `inventory.c` stays pure (no scene/GL); scene/GL glue lives in `main.c`.
- **No new shader → no new MSL twin** (reuse the forward PBR draw for thumbnail
  *render* and the blit path for thumbnail *display*). If a twin becomes
  unavoidable, surface it before building.
- Offscreen GPU work outside the frame loop needs `rhi_flush()` on Metal — avoided
  by rendering thumbnails at the top of the open frame.
- Never stage/commit `NOTES.stml` or `paper-picture.png`.
- Feature branch in-place → ff-merge to main; commits end with the
  `Co-Authored-By: Claude Opus 4.8 (1M context)` line.
