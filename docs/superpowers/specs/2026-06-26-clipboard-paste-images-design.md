# Clipboard Paste Images — Design Spec

**Date:** 2026-06-26
**Status:** Approved (brainstorm), pending implementation plan
**Author:** Fran + Claude

## Goal

In board view, **paste an image from the system clipboard** (Cmd+V): the bytes are saved
to a local `library/<nid>.png` file, and the image appears on the board as a picture —
reusing the existing board-picture display. Pasted images persist across reloads.

## Decisions (from brainstorming)

- **The "library-image entity" = the file + the board picture.** `library/<nid>.png` on
  disk, and a board picture object referencing it by `content` path (exactly how
  dropped-image pictures work today). **No separate registry/manifest.** Reuse/browse can
  come later.
- **Native Cocoa clipboard seam (Approach A).** A small Objective-C module
  `platform_clipboard.m` (the macOS quarantine, like `rhi_metal.m`) exposing a C seam that
  returns the clipboard image as PNG bytes. (GLFW clipboard is text-only; shelling out to
  `pbpaste` was rejected.)
- **Cmd+V, board view only.** Cmd (`SUPER`) avoids the existing `V` = mint-codex binding.
- **Always PNG.** Write the clipboard's PNG representation to `<nid>.png` regardless of the
  source format. Lossless, one code path.
- **`library/` is gitignored.** Pasted images are local, never committed (the
  sourced/synthesized-binary law). Note: `*.png` is NOT currently gitignored (only `*.jpg`
  and `*.hdr` are, because `paper-picture.png` is a committed asset), so the whole
  `library/` directory is ignored instead.

## Non-Goals

- No library registry, manifest, browser, dedup, or reuse-across-boards (the picture's
  content path is the only record).
- No text paste (paste-text-as-note) — images only.
- No cross-platform clipboard — macOS only (the project is macOS/M2).
- No image re-encoding (we write the clipboard's already-encoded PNG bytes raw; no
  `stb_image_write`).
- Paste applies only in board view; no world/editor paste.

## Background (current state)

- `spawn_image_picture(AppState *st, sol_u32 parent, vec3 pos, quat rot, const char *content)`
  (main.c:8670) creates a `KIND_PLAIN` `mesh_ref="picture"` object with `content` = the
  image path; the picture round-trips in `scene.stml` and reloads from the file. This is
  exactly the dropped-image-on-board path (main.c release handler ~10455).
- `image_load_from_memory(const unsigned char *data, int len, Image *out)` (image.h:23)
  validates/decodes image bytes — used here to confirm the clipboard bytes are a real
  image before writing.
- `nid_generate(char *out)` (nid.h) writes a 26-char ULID (`NID_LEN`+NUL).
- `board_card_tag_page(st, handle)` tags a board-pinned card with the board's `active_page`
  (board-pages) so a pasted picture lands on the page you're viewing.
- `board_under_ray(st, Ray, vec3 *local)` (main.c:4143) gives the board-local cursor hit;
  `board_pin_pos(scene, board, card, local, ox, oy)` pins a card proud of the face.
- `platform_fs.h/.c` is the POSIX seam (opendir/readdir/stat); it has `fs_exists(path)` but
  no directory-creation helper.
- **Quarantine pattern:** `rhi_metal.m` is the only Objective-C TU; it is compiled
  separately (`clang -fobjc-arc -c rhi_metal.m -o rhi_metal.o`) and linked. The **GL build
  compiles no `.m`** today (it's pure C + `rhi_gl.c`), but already links `-framework Cocoa`.
  Objective-C TUs are excluded from `c89check` (which lists only the portable C TUs).
- `.gitignore` ignores `*.jpg`, `*.hdr` (not `*.png`).

## Architecture

### 1. The clipboard seam — `platform_clipboard.h` / `.m`

`platform_clipboard.h` (portable C):
```c
/* Read the system clipboard's image as PNG bytes. On success returns SOL_TRUE
   and sets *out_bytes (malloc'd, caller frees with free()) + *out_len. Returns
   SOL_FALSE if the clipboard holds no image (text/empty/none). */
sol_bool clipboard_read_image(unsigned char **out_bytes, int *out_len);
```

`platform_clipboard.m` (macOS, ARC) — the only platform-specific code:
- `NSPasteboard *pb = [NSPasteboard generalPasteboard];`
- Prefer an existing PNG representation (`NSPasteboardTypePNG`); else read the image
  (`NSImage`/TIFF) and convert to PNG via `NSBitmapImageRep` `representationUsingType:NSBitmapImageFileTypePNG`.
- `malloc` a C buffer, `memcpy` the PNG bytes in, set `*out_bytes`/`*out_len`, return
  `SOL_TRUE`. Return `SOL_FALSE` (and leave outputs untouched) when there's no image.
- No GL/Metal/RHI references — pure OS clipboard. Works identically under both backends.

### 2. `fs_mkdir` — `platform_fs.h` / `.c`

Keep POSIX behind the seam:
```c
/* Create a directory (mode 0755). Returns SOL_TRUE if it now exists (created or
   already present), SOL_FALSE on error. */
sol_bool fs_mkdir(const char *path);
```
Impl: `mkdir(path, 0755)`; treat `EEXIST` as success.

### 3. The paste handler — `main.c`

A new `cmd_paste_image(AppState *st, GLFWwindow *w)` (or inline edge-detected block), and a
small `library_write(const unsigned char *bytes, int len, char *out_path, int cap)` helper
that does nid + mkdir + write:

```
clipboard_read_image(&bytes, &len)         -- no image -> printf, return
image_load_from_memory(bytes, len, &img)   -- invalid -> free(bytes), printf, return
image_free(&img)                           -- validation only; we write the raw bytes
nid_generate(nid)
fs_mkdir("library")
path = "library/<nid>.png"
fopen(path,"wb") + fwrite(bytes,len) + fclose   -- write failure -> free, printf, return
free(bytes)
board = board_under_ray(cursor) (else board_view, center)
a = spawn_image_picture(st, board, (0,0,0), quat_identity(), path)
ao->pos = board_pin_pos(scene, board, a, blocal, 0, -0.5*picture_h)
board_card_tag_page(st, a); st->selected_handle = a; scene_save
```

**Trigger:** edge-detected `(LEFT_SUPER||RIGHT_SUPER) && KEY_V`, gated `board_view != 0`,
placed among the discrete-key handlers (after the read_input early-returns for
edit/palette so it can't fire while typing). New `AppState` field `sol_bool paste_was_down;`.

### 4. Build wiring — `build.sh`

- Compile `platform_clipboard.m` like `rhi_metal.m`: `clang -fobjc-arc -c platform_clipboard.m … -o platform_clipboard.o`.
- Link `platform_clipboard.o` into the **debug/release** line, the **asan** line, and the
  **metal** line (all already link `-framework Cocoa`). The GL lines get their first `.o`
  from a `.m` — mirror the metal line's compile-then-link pattern, or compile inline.
- Add `platform_fs.c`'s `fs_mkdir` — `platform_fs.c` is already on every link line, so no
  new wiring for it.
- `platform_clipboard.m` is **not** added to `c89check` (ObjC, like `rhi_metal.m`).

### 5. `.gitignore`

Add `library/` (the directory) so all pasted images stay local.

## Data Flow

```
Cmd+V (board view)
  -> clipboard_read_image  --no image--> note + stop
  -> image_load_from_memory (validate)  --invalid--> free + note + stop
  -> nid_generate -> fs_mkdir("library") -> write library/<nid>.png (raw PNG bytes)
  -> spawn_image_picture(board_view, library/<nid>.png) -> pin at cursor -> tag page -> select -> save
reload: the picture object (content=library/<nid>.png) loads from the local file
```

## File Touch List

- **`platform_clipboard.h` / `.m`** (new): the Cocoa clipboard seam.
- **`platform_fs.h` / `.c`**: add `fs_mkdir`.
- **`main.c`**: `library_write` helper + `cmd_paste_image` + the Cmd+V handler + the
  `paste_was_down` field + `#include "platform_clipboard.h"` (and `nid.h` if not already
  included).
- **`build.sh`**: compile + link `platform_clipboard.m` on the GL (debug/release/asan) and
  metal lines.
- **`.gitignore`**: add `library/`.

## Testing

- **Build gauntlet (all three):** `./build.sh c89check` (the `.m` is excluded; the portable
  C still compiles clean), `./build.sh`, `./build.sh metal`.
- **Pure-logic:** thin. `fs_mkdir`'s EEXIST handling and `library_write`'s path formatting
  are the only testable bits; a small assertion in an existing headless test (or a manual
  check) suffices — clipboard reading itself is platform/GUI and not headlessly testable.
- **Human live-verify:** copy an image (a screenshot, a browser image) → enter board view →
  Cmd+V → the image appears on the board at the cursor on the current page; a second paste
  makes a second picture; reload the app → the pasted pictures persist (loaded from
  `library/`); Cmd+V with text/empty clipboard → nothing happens (a console note). Both
  backends.

## Risks

- **First `.m` in the GL build.** The GL link lines have never compiled Objective-C; verify
  `platform_clipboard.o` links cleanly there (Cocoa is already framework-linked). Low risk,
  but it's the main build-wiring novelty.
- **Pasteboard format variety.** Some sources put TIFF (not PNG) on the pasteboard; the
  `.m` must fall back to converting via `NSBitmapImageRep`. Handle the no-image and
  conversion-failure cases by returning `SOL_FALSE`.
- **Library file lifecycle.** Deleting a pasted picture leaves its `library/<nid>.png`
  orphaned on disk (no GC). Acceptable for v1 (local, gitignored); note as a future
  cleanup. A missing `library/` file on reload degrades like any missing image path
  (the picture mesh loads with no albedo).
- **No new shader** (reuses `draw_mesh` picture path) ⇒ no MSL twin.
