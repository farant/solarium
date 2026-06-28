# Folder Books: Blue Leather Cover + White Pages — Design Spec

**Date:** 2026-06-27
**Status:** Approved (brainstorm), pending implementation plan
**Author:** Fran + Claude

## Goal

The board "folder" books (the blue page-link books, `mesh_ref="folderbook"`) get a **blue leather
cover** (the `red_leather` PBR material, tinted blue) and a **white page block** (instead of the
current flat all-blue book).

## Decisions (from brainstorming)

- **Use the sourced `red_leather` material** (PolyHaven CC0, already on disk) for the cover, the same
  way the engine already loads `oak_veneer`/`dark_wood`/etc. via `load_pbr_material`. `red_leather` has
  **no diff/albedo map** (only normal/ARM/AO), so the **color comes from `base_color`** — "make it
  blue" = a blue `base_color` over the leather's normal/roughness/AO (uniform blue with real leather
  grain + sheen). Kept **local + gitignored** like the other material dirs.
- **White pages = a render-time derived box, NOT a separate scene object.** A separate leaves object
  would (a) keep showing when its folder is hidden on another board page (the page-filter only hides
  *direct* board-children, and the leaves would be a grandchild), and (b) be separately clickable.
  Drawing the page block as a cached unit box scaled per-folder in the draw loop gives correct
  visibility + picking for free, with no `scene.stml` clutter and no migration.
- The cover material is (re)applied by a **`folderbook_materialize` derive** — required regardless
  because textures aren't serialized (only scalar `base_color`/roughness are), so they must be
  re-assigned on every load; the derive conveniently also updates existing folders.

## Non-Goals

- Only the board `folderbook`s change — codices, the reader book, file/folder tablets, etc. are
  untouched.
- No separate leaves scene object; no scene.stml migration; no shader change (reuses the PBR path).
- The leather files stay LOCAL (gitignored) — no binaries committed.
- Per-folder blue **shade variation is preserved** (the derive keeps each folder's `base_color`), not
  flattened to one blue.

## Background (current state — verified)

- `make_folderbook(b, w, h, d, bands)` (mesh.c:1123) builds ONE mesh: a **leaf block**
  (`box_minmax(-hw+board, hw-lip, inset, h-inset, board, d-board)` where `hw=w/2`, `board=d*0.18`
  [clamped ≥1e-4], `inset=h*0.05`, `lip=w*0.03`), back+front cover boards, a spine slab, and raised
  bands. Registry row: `{ "folderbook", 4, {w,h,d,bands}, {0.14,0.20,0.05,4} }` (mesh.c:1471).
- `add_folder` (main.c:8084) creates the folder object (`KIND_PLAIN`, `mesh_ref="folderbook"`, params
  `p[4]`) and sets a flat blue material: `base_color = (0.08+0.14·blue, 0.16+0.22·blue, 0.42+0.34·blue)`,
  `roughness 0.6` (8111-8116). `blue` is a per-folder LCG shade.
- `apply_kind_materials` (main.c:13055) sets materials by KIND but **`continue`s on `KIND_PLAIN`**
  (13061) — so it never touches folderbooks (theirs is set in `add_folder`).
- **scene_io serializes only the scalar PBR factors** (`base_color`/metallic/roughness/emissive when
  they differ from default) — **NOT the texture handles** (scene_io.c:152). So a folder's blue
  `base_color` persists, but any textures must be re-applied at load.
- `load_pbr_material(diff, nor, arm)` (main.c:12613) builds a 3-map material; `load_texture_linear`
  loads normal/ORM maps. The `*_mat_init()` calls run at startup (main.c:13875-13883). `red_leather/`
  holds `leather_red_02_nor_gl_1k.png`, `_arm_1k.png`, `_ao_1k.png`, etc. — **no `_diff_`**.
- `material_default()` = white `base_color (1,1,1)`, roughness 0.6, no textures. `face_x/y/z`
  (via `box_minmax`) use **meter-range UVs**, so a tiled material renders at a realistic physical scale.
- The main draw loop (main.c ~14954) computes each object's world `model` and calls
  `draw_mesh(state, o->mesh, model, view, proj, eye, hl, mat)`. `caret_mesh`/`resize_handle_mesh` are
  the precedent for a lazily-built, AppState-cached unit mesh.

## Architecture

### 1. `g_book_leather` material (main.c)

A new file-static `Material g_book_leather;` + `book_leather_mat_init()` called beside the other
`*_mat_init()`s (~main.c:13883):
```
m = material_default();
m.normal_tex = load_texture_linear("red_leather/leather_red_02_nor_gl_1k.png");
if (m.normal_tex.id == 0) { g_book_leather = m; return; }   /* missing -> disabled (flat) */
m.mr_tex = load_texture_linear("red_leather/leather_red_02_arm_1k.png");   /* ARM: R=AO,G=rough,B=metal */
if (m.mr_tex.id != 0) { m.ao_tex = m.mr_tex; m.metallic = 1.0f; m.roughness = 1.0f; m.ao_strength = 1.0f; }
m.normal_scale = 1.0f;
/* base_color stays white — folderbook_materialize sets the per-folder blue */
g_book_leather = m;
```
Graceful: if the files are missing, `g_book_leather` is the default (no textures) → folders stay flat
blue (today's look).

### 2. `make_folderbook` → cover only (mesh.c)

Remove the leaf-block box (mesh.c:1131) from `make_folderbook`; it now builds only the cover boards +
spine + bands. The leaf block becomes the render-derived white box (§3). The registry row + params are
unchanged (still `w,h,d,bands`).

### 3. The white page block — render-derived (main.c, the draw loop)

A lazily-built, AppState-cached **unit box** mesh `Mesh folderbook_leaves_mesh` (a centered unit cube,
`box_minmax(-0.5,0.5,-0.5,0.5,-0.5,0.5)` via a `MeshBuilder`, like `caret_mesh`). In the draw loop,
after the normal `draw_mesh` of an object, when `o->mesh_ref == "folderbook"`:
```
w/h/d = mesh_ref_param("folderbook", ...);   board = d*0.18 (>=1e-4); inset = h*0.05; lip = w*0.03;
lw = w - lip - board;  lh = h - 2*inset;  ld = d - 2*board;
if (lw>0 && lh>0 && ld>0):
    center = ((board - lip)*0.5, h*0.5, d*0.5)           /* matches the make_folderbook leaf block */
    leaves_model = model * mat4_from_trs(center, identity, (lw,lh,ld))
    draw_mesh(state, folderbook_leaves_mesh, leaves_model, view, proj, eye, hl, material_default())
```
Drawn with the SAME `model` + `hl` as the cover, so the pages follow the folder's transform, visibility
(it only draws when the folder draws), and selection highlight. White = `material_default()`. The
leaves mesh is freed at shutdown (beside `caret_mesh`).

### 4. `folderbook_materialize(AppState *st)` derive (main.c)

```
for each object o with mesh_ref == "folderbook":
    keep = o->material.base_color;                 /* the per-folder blue */
    o->material.albedo_tex/normal_tex/mr_tex/ao_tex = g_book_leather's;
    o->material.metallic/roughness/normal_scale/ao_strength = g_book_leather's;
    o->material.base_color = keep;                 /* preserve the blue shade */
```
Re-applies the leather textures + scalars to every folder while keeping its blue tint. Called in
`world_rebuild` and the `load_palace` tail (the load-derive law) and at the end of `add_folder` (so a
freshly created folder gets the leather immediately). Material-only — no objects added.

`add_folder` keeps setting its blue `base_color` (the LCG shade) as today (that's the tint
`folderbook_materialize` preserves); just call `folderbook_materialize(st)` after the existing
`scene_resolve_meshes`.

### 5. `.gitignore`

Add `/red_leather/` (local sourced material, like the other `*_mat_init` dirs).

## Data Flow

```
startup -> book_leather_mat_init() loads g_book_leather (or stays disabled if files absent)
add_folder -> create folderbook (blue base_color) -> folderbook_materialize (apply leather, keep blue)
load/rebuild -> folderbook_materialize re-applies leather textures (not serialized) + keeps base_color
draw loop -> draw the folderbook cover (leather-blue) -> draw the derived white leaves box (same model/hl)
```

## File Touch List

- **`mesh.c`**: `make_folderbook` drops the leaf-block box (cover only).
- **`main.c`**: `g_book_leather` + `book_leather_mat_init()` (called at startup); `folderbook_leaves_mesh`
  AppState field + the draw-loop white-leaves branch + its shutdown free; `folderbook_materialize` +
  calls in `world_rebuild`/`load_palace`/`add_folder`.
- **`.gitignore`**: `/red_leather/`.
- No `build.sh` change.

## Testing

- **Build gauntlet**: `c89check` / GL / Metal (no shader change; `carettest` unaffected).
- **Human live-verify** (both backends): a folder book shows a **blue leather** cover (grain/sheen
  visible) with a **white** page block; existing folders (from `scene.stml`) update on load AND keep
  their individual blue shades; a folder hidden on another board page hides its pages too (no orphan
  white box); clicking a folder still selects/navigates it (the pages aren't separately clickable);
  with `red_leather/` absent, folders fall back to flat blue (no crash).
  - **Tuning note:** the leather grain scale rides the cover's meter UVs (realistic by default); if the
    grain reads too large/small, it's a UV-scale tweak — confirm in live-verify.

## Risks

- **Leaf-block dimensions must match `make_folderbook` exactly** (the same `board/inset/lip` formulas)
  or the white box won't line up inside the cover — single source of the formula, mirrored in the draw
  branch; verified visually.
- **Missing `red_leather/`** → graceful flat-blue fallback (the `normal_tex.id==0` guard).
- **Leather grain scale** (UV tiling) — meter UVs give a realistic default; tune if needed (live-verify).
- **Per-folder cost** — the leaves box is one extra `draw_mesh` per visible folder (a unit cube,
  negligible); the mesh is built once and cached.
- No serialized children, no migration, no shader/MSL twin, no change to other books.
