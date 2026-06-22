# Images Open as Books — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Opening a `.png`/`.jpg`/`.jpeg` file as a book shows the image on the right page (aspect-fit) with the filename on the left, instead of typesetting the binary as text.

**Architecture:** A `reader_is_image` flag splits the reader into two content modes that share one book shell (cover+block mesh, rise/return animation, the page plane). Image mode decodes the file with `image_load`, makes an aspect-fitted `make_page` quad, and draws it via `draw_mesh`'s existing lit-albedo path (the same one `paper-picture.png` uses) — no new shader, no MSL twin. The page-turn input is already gated on `reader_text`, so an image book is naturally a single, un-turnable spread.

**Tech Stack:** C89 (strict), the `image.h` stb seam, the RHI (`rhi_create_texture`/`rhi_destroy_texture`, `RHI_TEX_SRGB8`), `MeshBuilder`/`make_page`, `draw_mesh`.

**Spec:** `docs/superpowers/specs/2026-06-22-images-as-books-design.md`

---

## Conventions for every task

- **Strict C89:** declarations at the top of each block, `/* */` comments only (no `//`), no mixed declarations, no VLAs, `snprintf`/`strncpy` (never `sprintf`/`strcpy`).
- **Build gauntlet (must stay green):** `./build.sh c89check && ./build.sh debug && ./build.sh metal`.
- **The reader render code is GUI** — it can't be unit-tested headless; Fran live-verifies after Task 3. Only the pure aspect-fit math (Task 1) gets a unit test.
- Commit after each task. Stage only the files the task names — **never** `git add` `NOTES.stml`, `paper-picture.png`, or `scene*.stml`.
- Commit trailer: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.

## File map

- `image.h` / `image.c` — add the pure `image_fit_box` aspect-fit helper.
- `image_test.c` (new) + `build.sh` — the fit-math unit test target.
- `main.c` — `AppState` image fields; `reader_is_image_path`; `reader_free_image`; the image branch in `reader_load_content`; the image draw branch in the reader render block.

---

### Task 1: `image_fit_box` aspect-fit math + unit test

**Files:**
- Modify: `image.h` (declaration), `image.c` (definition)
- Create: `image_test.c`
- Modify: `build.sh` (new `imagetest` target)

- [ ] **Step 1: Write the failing test** — create `image_test.c`:

```c
/* image_test.c — the pure aspect-fit math (image_fit_box). No GL, no stb call
   (image.c is linked only for the symbol; the decoder is never run). */
#include "image.h"
#include <stdio.h>
#include <math.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define NEAR(a,b) (fabs((double)((a) - (b))) < 1e-4)

int main(void) {
    float w, h;
    /* a WIDE image (2:1) in a square field is width-bound: w fills, h is half */
    image_fit_box(200, 100, 1.0f, 1.0f, &w, &h);
    CHECK(NEAR(w, 1.0f)); CHECK(NEAR(h, 0.5f));
    /* a TALL image (1:2) in a square field is height-bound */
    image_fit_box(100, 200, 1.0f, 1.0f, &w, &h);
    CHECK(NEAR(h, 1.0f)); CHECK(NEAR(w, 0.5f));
    /* a SQUARE image in a WIDE field fits the smaller (height) dimension */
    image_fit_box(100, 100, 2.0f, 1.0f, &w, &h);
    CHECK(NEAR(w, 1.0f)); CHECK(NEAR(h, 1.0f));
    /* aspect preserved + stays inside the field */
    image_fit_box(300, 200, 1.4f, 0.8f, &w, &h);
    CHECK(NEAR(w / h, 1.5f));
    CHECK(w <= 1.4f + 1e-4f && h <= 0.8f + 1e-4f);
    /* degenerate dims -> 0,0, no divide-by-zero */
    image_fit_box(0, 100, 1.0f, 1.0f, &w, &h);
    CHECK(NEAR(w, 0.0f) && NEAR(h, 0.0f));
    image_fit_box(100, 0, 1.0f, 1.0f, &w, &h);
    CHECK(NEAR(w, 0.0f) && NEAR(h, 0.0f));
    if (fails == 0) printf("image_test: OK\n");
    return fails ? 1 : 0;
}
```

- [ ] **Step 2: Add the `imagetest` build target** — in `build.sh`, immediately after the `furnituretest` block (the `fi` ending it, ~line 146), insert (mirrors `skeltest`, which already links `image.c` under `-std=c11` because stb is not C89):

```sh
# imagetest: the pure aspect-fit math in image.c (image_fit_box). image.c pulls
# stb, so this rides -std=c11 like skeltest, not c89-pedantic.
if [ "$MODE" = "imagetest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        image.c image_test.c \
        -o image_test
    echo "built ./image_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi
```

- [ ] **Step 3: Run the test to verify it FAILS to build** (the function doesn't exist yet)

Run: `./build.sh imagetest`
Expected: link error — `undefined symbol: _image_fit_box`.

- [ ] **Step 4: Declare `image_fit_box`** — in `image.h`, after the `image_free` declaration:

```c
/* Largest (w,h) in METERS that fits inside (field_w x field_h) at the source
   image's aspect ratio (letterbox: one axis meets the field, the other is
   smaller). Any non-positive input yields *out_w = *out_h = 0. Pure math —
   no stb, no GL. */
void image_fit_box(int src_w, int src_h, float field_w, float field_h,
                   float *out_w, float *out_h);
```

- [ ] **Step 5: Define `image_fit_box`** — in `image.c`, after `image_free` (keep it ABOVE the stb `#include` is not required; anywhere in the file is fine since it doesn't use stb):

```c
void image_fit_box(int src_w, int src_h, float field_w, float field_h,
                   float *out_w, float *out_h) {
    float aspect;
    *out_w = 0.0f;
    *out_h = 0.0f;
    if (src_w <= 0 || src_h <= 0 || field_w <= 0.0f || field_h <= 0.0f) return;
    aspect = (float)src_w / (float)src_h;          /* width per height */
    if (field_w / field_h > aspect) {              /* field wider than image: height-bound */
        *out_h = field_h;
        *out_w = field_h * aspect;
    } else {                                       /* width-bound */
        *out_w = field_w;
        *out_h = field_w / aspect;
    }
}
```

- [ ] **Step 6: Run the test to verify it PASSES**

Run: `./build.sh imagetest && ./image_test`
Expected: `image_test: OK` (no sanitizer output).

- [ ] **Step 7: Confirm the gauntlet is still green** (image.c changed)

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: `c89check: PASS`, both binaries build. (`image.c` is excluded from c89check; the gauntlet just confirms nothing else broke.)

- [ ] **Step 8: Commit**

```bash
git add image.h image.c image_test.c build.sh
git commit -m "$(printf 'image: image_fit_box aspect-fit helper + unit test\n\nPure letterbox math (largest w*h at the source aspect inside a field), the\none testable piece of images-as-books. New imagetest target rides -std=c11\nlike skeltest since image.c pulls stb.\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

### Task 2: image load + lifecycle (decode on open, free on close — not drawn yet)

**Files:**
- Modify: `main.c` — `AppState` fields (~line 2723); `reader_is_image_path` (~line 5466, by `reader_wants_mono`); `reader_free_image` (~line 5495, by `reader_free_text`); the image branch in `reader_load_content` (~line 5515); two `reader_free_image` call sites.

After this task an image file opens to a blank book (loaded + freed cleanly, but the draw branch comes in Task 3). That is a valid, green intermediate.

- [ ] **Step 1: Add the `AppState` image fields** — in `main.c`, immediately after `float reader_px2m;` (the `reader_px2m` line, ~2723):

```c
    sol_bool    reader_is_image;       /* this book shows an image, not text */
    Mesh        reader_image_quad;     /* the aspect-fitted right-page quad */
    RhiTexture  reader_image_tex;      /* decoded image; .id==0 if none */
    int         reader_image_w, reader_image_h;  /* source pixel dims */
```

- [ ] **Step 2: Add the extension predicate** — in `main.c`, directly after the `reader_wants_mono` function (it ends ~line 5482):

```c
/* png/jpg/jpeg open AS a picture on the page, not as typeset text. */
static sol_bool reader_is_image_path(const char *path) {
    static const char *img[] = { "png", "jpg", "jpeg", (const char *)0 };
    const char *dot, *base;
    char        ext[8];
    int         i;
    if (!path) return SOL_FALSE;
    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    dot  = strrchr(base, '.');
    if (!dot || dot == base) return SOL_FALSE;
    for (i = 0; dot[i + 1] != '\0' && i < (int)sizeof ext - 1; i++) {
        char c = dot[i + 1];                       /* lowercase for the compare */
        ext[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    ext[i] = '\0';
    for (i = 0; img[i] != (const char *)0; i++)
        if (strcmp(ext, img[i]) == 0) return SOL_TRUE;
    return SOL_FALSE;
}
```

- [ ] **Step 3: Add the image-free helper** — in `main.c`, directly after `reader_free_text` (it ends ~line 5495):

```c
static void reader_free_image(AppState *st) {
    if (st->reader_image_tex.id) rhi_destroy_texture(st->reader_image_tex);
    st->reader_image_tex.id = 0;
    mesh_destroy(&st->reader_image_quad);
    st->reader_image_w = 0;
    st->reader_image_h = 0;
    st->reader_is_image = SOL_FALSE;
}
```

- [ ] **Step 4: Clear image state at the top of `reader_load_content`** — find the existing `reader_free_text(st);` at the top of `reader_load_content` (~line 5506) and add the image free right after it:

```c
    reader_free_text(st);
    reader_free_image(st);
```

- [ ] **Step 5: Add the image branch** — in `reader_load_content`, find the two lines that compute the page field:

```c
    field_w = wb - xf - 2.0f * mg;
    field_h = 2.0f * zh - 2.0f * mg;
```

Insert this block immediately AFTER them (before the `st->reader_px2m = ...` line):

```c
    /* IMAGE BRANCH: a picture file opens AS a picture — decode it, size the
       page quad to its aspect, and skip typesetting. Any decode/upload failure
       falls through to the text path below (which renders a graceful message). */
    if (reader_is_image_path(path)) {
        Image img;
        if (image_load(path, &img)) {
            st->reader_image_tex = rhi_create_texture(img.pixels, img.w, img.h,
                                                      RHI_TEX_SRGB8);
            st->reader_image_w = img.w;
            st->reader_image_h = img.h;
            image_free(&img);
            if (st->reader_image_tex.id) {
                float       qw, qh;
                MeshBuilder mb;
                image_fit_box(st->reader_image_w, st->reader_image_h,
                              field_w, field_h, &qw, &qh);
                mb_init(&mb);
                make_page(&mb, qw, qh);
                st->reader_image_quad = mesh_from_builder(&mb);
                mb_free(&mb);
                st->reader_is_image = SOL_TRUE;
                return;                            /* image ready; no typeset */
            }
            st->reader_image_w = 0;                /* upload failed: drop dims */
            st->reader_image_h = 0;
        }
        /* image_load failed -> fall through to the text fallback below */
    }
```

- [ ] **Step 6: Free image state in the reader teardown** — find the IDLE-landing block (~line 5696) that frees the reader meshes; it currently ends with:

```c
            mesh_destroy(&st->reader_leaf);
            reader_free_text(st);
```

Add the image free right after `reader_free_text(st);`:

```c
            mesh_destroy(&st->reader_leaf);
            reader_free_text(st);
            reader_free_image(st);
```

- [ ] **Step 7: Build the gauntlet**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: `c89check: PASS`, both binaries build. (No new shader, so Metal links clean.)

- [ ] **Step 8: Commit**

```bash
git add main.c
git commit -m "$(printf 'reader: decode image files on open (load + lifecycle)\n\nA reader_is_image branch in reader_load_content decodes png/jpg/jpeg via\nimage_load, uploads an sRGB texture, and builds an aspect-fitted page quad;\nreader_free_image tears the texture+quad down wherever reader_free_text is\ncalled. Decode failure falls through to the text path. Not drawn yet (Task 3).\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

### Task 3: render the image on the right page + filename on the left

**Files:**
- Modify: `main.c` — the reader page render block (~line 10235), converting the text-only `if` into an image-or-text branch.

- [ ] **Step 1: Branch the page render** — in the reader render block, find:

```c
            if (state->reader_text) {
                int L      = state->reader_lines_per_page;
```

Change the opening line to make the existing text block the `else if`, and add the image branch BEFORE it:

```c
            if (state->reader_is_image && state->reader_image_tex.id) {
                /* the image on the RIGHT page (fitted), filename on the LEFT */
                float field_w_left = wb - xf - 2.0f * mg;
                if (state->reader_image_quad.index_count > 0) {
                    float    cx = (xf + wb) * 0.5f;        /* right-page center x */
                    mat4     im = mat4_mul(page,
                                  mat4_translate(vec3_make(cx, 0.0f, 0.0008f)));
                    Material pm = material_default();
                    pm.base_color = vec3_make(1.0f, 1.0f, 1.0f);  /* show as-is */
                    pm.roughness  = 0.95f;
                    pm.albedo_tex = state->reader_image_tex;
                    draw_mesh(state, state->reader_image_quad, im,
                              view, proj, eye, 0.0f, pm);
                }
                {   /* the filename, shrunk to the left page width */
                    SceneObject *src  = scene_get(&state->scene,
                                                  state->reader_source);
                    const char  *path = (src && src->content) ? src->content
                                                              : (const char *)0;
                    const char  *nm;
                    char         lbuf[16];
                    float        cpx, nw;
                    if (path) { nm = strrchr(path, '/'); nm = nm ? nm + 1 : path; }
                    else        nm = object_label(&state->scene,
                                                  state->reader_source, lbuf);
                    cpx = (bp[1] * 0.020f) / lh;
                    text_measure(uf, nm, 1.0f, &nw, (float *)0);
                    if (nw * cpx > field_w_left && nw > 0.0f)
                        cpx = field_w_left / nw;
                    wtext_block(uf, vp, page, nm, -wb + mg, zh - mg, cpx, 0.0f,
                                0.13f, 0.10f, 0.08f);
                }
            } else if (state->reader_text) {
                int L      = state->reader_lines_per_page;
```

Leave the rest of the text block (its body and closing brace) exactly as-is.

- [ ] **Step 2: Verify `material_default` exposes `albedo_tex` and `base_color`** — confirm the Material struct has `albedo_tex` (RhiTexture), `base_color` (vec3), and `roughness` (the `paper-picture.png` page sets `material.albedo_tex` the same way). If a field name differs, match the struct. Run a quick grep:

Run: `grep -n "albedo_tex\|base_color\|} Material" material.h`
Expected: the three fields exist on `Material` (no code change if so).

- [ ] **Step 3: Build the gauntlet**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: `c89check: PASS`, both binaries build.

- [ ] **Step 4: Run the fit unit test once more (regression)**

Run: `./build.sh imagetest && ./image_test`
Expected: `image_test: OK`.

- [ ] **Step 5: Commit**

```bash
git add main.c
git commit -m "$(printf 'reader: draw the opened image on the right page, filename on the left\n\nImage mode draws the aspect-fitted quad via draw_mesh (albedo_tex = the\ndecoded image, the paper-picture.png lit path) on the right page and the\nfilename via wtext on the left; the existing text block becomes the else-if.\nPage-turn input is already gated on reader_text, so an image is one spread.\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

- [ ] **Step 6: Hand to Fran for live verify** — he opens a `.png` and a `.jpg` from a mirror room and confirms on BOTH backends: the image shows upright + aspect-correct on the right page, the filename reads on the left, the page-turn arrows do nothing, and opening a text file afterward still typesets (no leak, no stale image). Note any tuning (lift/scale/caption position) for a follow-up.

---

## Self-review (against the spec)

- **Detection** (spec §1) → Task 2 Step 2 (`reader_is_image_path`, case-insensitive png/jpg/jpeg). ✓
- **AppState fields** (spec §2) → Task 2 Step 1 (matches the spec's names: `reader_is_image`, `reader_image_tex`, `reader_image_w/h`; plus `reader_image_quad` from spec §5). ✓
- **Loading branch + sRGB + fallback** (spec §3) → Task 2 Step 5. ✓
- **Render: image right via draw_mesh lit-albedo, caption left** (spec §4) → Task 3 Step 1. ✓
- **Single un-turnable spread** (spec §4) → no code; the existing `reader_text` gate at the turn input handles it (noted Task 3 Step 6). ✓
- **Lifecycle: build quad on load, free on close** (spec §5) → Task 2 Steps 3/5/6. ✓
- **`image_fit_box` pure + unit test** (spec, Testing) → Task 1. ✓
- **Live verify both backends** (spec, Testing) → Task 3 Step 6. ✓

**Type/name consistency:** `image_fit_box(int,int,float,float,float*,float*)` (Task 1) is called identically in Task 2 Step 5. `reader_image_tex`/`_quad`/`_w`/`_h`/`reader_is_image` defined in Task 2 Step 1 are used identically in `reader_free_image` and the render branch. `make_page`, `mb_init`, `mesh_from_builder`, `mb_free`, `draw_mesh`, `material_default`, `text_measure`, `wtext_block`, `object_label`, `rhi_create_texture`, `rhi_destroy_texture`, `RHI_TEX_SRGB8`, `image_load`, `image_free`, `Image` all exist in the codebase as used. No placeholders.
