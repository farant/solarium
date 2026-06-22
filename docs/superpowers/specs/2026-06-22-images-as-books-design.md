# Images Open as Books — Design

**Goal:** When a `.png`/`.jpg`/`.jpeg` file is opened as a book, show the image on
the right page (aspect-fit) with the filename on the left page — instead of the
current text typesetting, which only makes sense for text files.

**Context:** Today `reader_open` → `reader_load_content` reads any file as text,
expands tabs, wraps to the page field, and paginates; the render loop draws each
page's text via `wtext_block`. Binary files (images) read as garbage. This adds
an image branch alongside the text branch. It reuses existing machinery
end-to-end — `image_load` (the stb seam already used by `load_texture` /
`paper-picture.png`), `rhi_create_texture` with `RHI_TEX_SRGB8`, the
`MeshBuilder` quad, and `draw_mesh`'s lit-albedo path. **No new shader, no MSL
twin.**

---

## Architecture

A new `reader_is_image` flag on `AppState` splits the reader into two content
modes that share the same book shell (cover + block mesh, rise/return animation,
the page plane):

- **Text mode** (today, unchanged): `reader_text` typeset and paginated.
- **Image mode** (new): one decoded texture + its pixel dimensions, drawn as a
  fitted quad on the right page, with the filename as a caption on the left.

The book shell, poses, and open/close lifecycle are identical in both modes. Only
content load and content draw branch.

## Components & Data Flow

### 1. Detection — `reader_is_image_path()`
A pure helper mirroring `reader_wants_mono`'s extension test: take the basename,
find the last `.`, and case-insensitively compare the extension against
`{ "png", "jpg", "jpeg" }`. Returns `SOL_TRUE` for an image path.

### 2. New `AppState` fields
```c
sol_bool   reader_is_image;   /* this book is showing an image, not text */
RhiTexture reader_image_tex;  /* decoded image (id==0 if none/failed) */
int        reader_image_w;    /* source pixel dims, for aspect-fit */
int        reader_image_h;
```

### 3. Loading — branch in `reader_load_content`
At the top of `reader_load_content`, after `reader_free_text`, also reset the
image state (see lifecycle). Then:

- If `reader_is_image_path(path)`:
  - `image_load(path, &img)`. On success:
    `reader_image_tex = rhi_create_texture(img.pixels, img.w, img.h, RHI_TEX_SRGB8)`
    (sRGB8 matches `load_texture` — photos are sRGB); store `img.w`/`img.h`;
    `image_free(&img)`; set `reader_is_image = SOL_TRUE`.
  - On `image_load` failure (corrupt / unreadable / too large for stb): fall
    through to the **text branch** with `reader_is_image = SOL_FALSE`, so the
    existing `"(the file would not open)"` message renders. The reader always
    opens to *something*.
- Else: the existing text path, unchanged.

The image branch does **not** touch `reader_text`/`reader_line_off` (they stay
NULL), and does not compute `reader_px2m`/`lines_per_page` for the body.

### 4. Rendering — branch in the reader page block (render loop, ~main.c:10235)
The `page` matrix (book-local, `R_x(-90°)` so a 2D layout lies flat on the page,
text-up toward the book's head) is already computed. Today the block is
`if (state->reader_text) { ...draw pages... }`. Add an
`else if (state->reader_is_image && state->reader_image_tex.id) { ... }` branch:

- **Right page — the image.** Compute the fitted quad extents from the source
  dims and the page field via `image_fit_box` (below). Build a quad in the page's
  2D frame (XY at z=0, normal +z, UVs 0..1 — `mb_push_vertex`/`mb_push_triangle`,
  the existing centered-quad recipe), centered in the right page's field
  (the right page spans `[xf+mg, wb-mg]` in layout x, `[-zh+mg, zh-mg]` in y,
  matching `reader_draw_page(right_pg, xf+mg, zh-mg)`). Draw it with `draw_mesh`
  using the `page` model matrix and a material whose `albedo_tex =
  reader_image_tex` (`base_color` white, high roughness, no metal) — the same lit
  path `paper-picture.png` uses. Rebuild the quad each frame is unnecessary; build
  it once on load (see below) and store it as `reader_image_quad` (a `Mesh`),
  freed on close like the cover/block meshes.
- **Left page — the caption.** Draw the filename (basename of the source's
  content path, or the object label) via `wtext_block` on the left page at
  `(-wb+mg, zh-mg)` using `ui_font`, shrunk to fit the page width like the card
  label.

Because `reader_text` is NULL in image mode, the page-turn input (gated on
`reader_state == READER_OPEN && reader_text`, ~main.c:7134) is naturally inert —
an image book is a single, un-turnable spread with no extra code.

### 5. Lifecycle
- **Build the quad on load:** in the image branch of `reader_load_content`, after
  the texture is created, build `reader_image_quad` from the fitted extents (so the
  fit is computed once). Store the `Mesh`.
- **Free on close:** add a `reader_free_image(st)` helper that
  `rhi_destroy_texture(reader_image_tex)` (if id), `mesh_destroy(&reader_image_quad)`,
  and zeroes `reader_image_*` + `reader_is_image`. Call it wherever `reader_free_text`
  is called (open/reload of new content, and `reader_close`/teardown), so a texture
  never leaks and reopening a different file starts clean.

## The aspect-fit math (pure, unit-tested)

`image_fit_box` in `image.c` / `image.h` — no stb, no GL, just arithmetic:

```c
/* Largest WxH (meters) that fits inside (field_w x field_h) at the source's
   aspect ratio. Letterbox: one dimension hits the field, the other is <= it. */
void image_fit_box(int src_w, int src_h, float field_w, float field_h,
                   float *out_w, float *out_h);
```

Logic: `aspect = src_w / src_h`. If `field_w / field_h > aspect` the fit is
height-bound (`*out_h = field_h; *out_w = field_h * aspect`), else width-bound
(`*out_w = field_w; *out_h = field_w / aspect`). Degenerate inputs
(`src_h <= 0`, `src_w <= 0`, non-positive field) yield `*out_w = *out_h = 0`.

## Error handling

- `image_load` fails → text fallback (graceful "would not open" message), no
  image state set.
- `rhi_create_texture` returns id 0 → treat as load failure (text fallback);
  free any partial state.
- Zero/degenerate dims → `image_fit_box` returns 0 extents → the draw branch
  skips the quad (still shows the caption), never divides by zero.

## Testing

- **`image_test.c`** (new, added to `build.sh`): unit-tests `image_fit_box` —
  a wide image is width-bound, a tall image is height-bound, a square fits the
  smaller field dimension, and degenerate dims return 0. Pure, headless, ASan/UBSan
  like the other `*_test` binaries.
- **Live verify (Fran):** open a `.png` and a `.jpg` from a mirror room — the
  image shows on the right page upright and aspect-correct on both GL and Metal
  (`image_load` flips rows for GL; Metal rides the same `albedo_tex` path as
  `paper-picture.png`), the filename reads on the left, page-turn arrows are inert,
  and reopening a text file afterward still typesets (no texture leak / no stale
  image).

## Scope / non-goals (YAGNI)

In: still PNG/JPG/JPEG, one image per book, on the right page, filename on the
left. Out: zoom/pan, multi-image galleries, animated/other formats, EXIF
rotation, drawing the image into the codex thumbnail/cover. Non-image files keep
today's exact text behavior.

## Files touched

- `main.c` — `AppState` fields; `reader_is_image_path`; image branch in
  `reader_load_content`; `reader_free_image` + its call sites; image draw branch
  in the reader render block.
- `image.h` / `image.c` — `image_fit_box`.
- `image_test.c` (new) + `build.sh` — the fit-math test target.
