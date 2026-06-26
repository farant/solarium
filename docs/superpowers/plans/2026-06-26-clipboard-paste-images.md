# Clipboard Paste Images Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** In board view, Cmd+V pastes the system-clipboard image to `library/<nid>.png` and shows it on the board as a picture (persisting across reloads).

**Architecture:** A native Cocoa seam `platform_clipboard.m` (the macOS quarantine, like `rhi_metal.m`) returns the clipboard image as PNG bytes; `main.c` writes `library/<nid>.png` (raw bytes, no encoding) and reuses the existing `spawn_image_picture`. A small `fs_mkdir` is added to the `platform_fs` POSIX seam. The library is the file + the board picture (no registry).

**Tech Stack:** C89/C11 ("Dependable C"), Objective-C (Cocoa/`NSPasteboard`, ARC) for the one platform TU, OpenGL + Metal dual backend, hand-written `build.sh`. Spec: `docs/superpowers/specs/2026-06-26-clipboard-paste-images-design.md`.

**Gauntlet (run all three after every task that touches buildable code):** `./build.sh c89check` (the new `.m` is excluded, like `rhi_metal.m`; the portable C must still compile clean) · `./build.sh` · `./build.sh metal`.

**Project laws:**
- **Strict C89** for the portable C (`platform_fs.c`, `main.c`): declarations at the TOP of each block; `/* */` comments; no C99. Authority: `./build.sh c89check`. The Objective-C `.m` is exempt (not in `c89check`).
- **RHI/platform seam:** the `.m` touches only Cocoa (`NSPasteboard`) — NO GL/Metal/RHI. It links into both backends because it's OS-level.
- **Use-after-realloc:** `scene_add` may move the objects array; re-`scene_get` after. Free the clipboard buffer exactly once.
- Commit messages end EXACTLY with: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. `git add` only the files each task names. NEVER add `NOTES.stml` / `paper-picture.png`.

**Testing note:** this feature is platform/GUI glue (clipboard, mkdir, paste) — there is no meaningful headless unit test (consistent with `platform_fs.c`/`image.c` being untested glue). Each task is verified by the build gauntlet; the end-to-end behavior is human live-verify.

---

## File Structure

- **`platform_fs.h` / `.c`**: add `fs_mkdir` (POSIX `mkdir` behind the seam).
- **`platform_clipboard.h`** (NEW, portable C): the `clipboard_read_image` seam.
- **`platform_clipboard.m`** (NEW, Objective-C/ARC): the macOS `NSPasteboard` implementation — the only platform-specific code.
- **`main.c`**: `library_write` helper + `cmd_paste_image` + the Cmd+V handler + `paste_was_down` field + the includes.
- **`build.sh`**: compile + link `platform_clipboard.m` into the debug/release, asan, and metal builds (not `c89check`).
- **`.gitignore`**: add `library/`.

---

## Task 1: `fs_mkdir` in the platform_fs seam

**Files:** Modify `platform_fs.h`, `platform_fs.c`.

- [ ] **Step 1: Declare in `platform_fs.h`** — next to `fs_exists`:
```c
/* Create a directory (mode 0755). Returns SOL_TRUE if it now exists (created
   or already present), SOL_FALSE on error. */
sol_bool fs_mkdir(const char *path);
```

- [ ] **Step 2: Implement in `platform_fs.c`** — it already includes `<sys/stat.h>` and `<errno.h>`? It includes `<sys/stat.h>` (for `stat`); add `#include <errno.h>` at the top if absent. Add the function (anywhere after the includes):
```c
sol_bool fs_mkdir(const char *path) {
    if (mkdir(path, 0755) == 0) return SOL_TRUE;
    return (errno == EEXIST) ? SOL_TRUE : SOL_FALSE;
}
```
(`mkdir` is declared in `<sys/stat.h>`; `errno`/`EEXIST` in `<errno.h>`.)

- [ ] **Step 3: Gauntlet** — `./build.sh c89check && ./build.sh && ./build.sh metal`. All three pass. (`platform_fs.c` is already on every full-build link line, so no build wiring needed.)

- [ ] **Step 4: Commit:**
```bash
git add platform_fs.h platform_fs.c
git commit -m "$(printf 'Clipboard paste 1/3: fs_mkdir in the platform_fs seam\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 2: the clipboard seam + build wiring

**Files:** Create `platform_clipboard.h`, `platform_clipboard.m`; Modify `build.sh`.

- [ ] **Step 1: The header** — create `platform_clipboard.h`:
```c
#ifndef SOL_PLATFORM_CLIPBOARD_H
#define SOL_PLATFORM_CLIPBOARD_H
#include "sol_base.h"

/* Read the system clipboard's image as PNG bytes. On success returns SOL_TRUE,
   sets *out_bytes (malloc'd — caller frees with free()) and *out_len. Returns
   SOL_FALSE (and leaves the outputs untouched) when the clipboard holds no
   image (text / empty / unsupported). */
sol_bool clipboard_read_image(unsigned char **out_bytes, int *out_len);

#endif
```
(Confirm `sol_base.h` is the header that defines `sol_bool`/`SOL_TRUE`/`SOL_FALSE` — it is, per the rest of the codebase.)

- [ ] **Step 2: The Objective-C implementation** — create `platform_clipboard.m`:
```objc
#import <Cocoa/Cocoa.h>
#include "platform_clipboard.h"
#include <stdlib.h>
#include <string.h>

sol_bool clipboard_read_image(unsigned char **out_bytes, int *out_len) {
    @autoreleasepool {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        NSData       *png = nil;

        /* Prefer an existing PNG representation. */
        NSData *direct = [pb dataForType:NSPasteboardTypePNG];
        if (direct && [direct length] > 0) {
            png = direct;
        } else {
            /* Fall back: read an NSImage (or TIFF) and re-encode to PNG. */
            NSData *tiff = [pb dataForType:NSPasteboardTypeTIFF];
            if (!tiff || [tiff length] == 0) {
                NSArray *imgs = [pb readObjectsForClasses:@[[NSImage class]] options:nil];
                if (imgs && [imgs count] > 0) {
                    NSImage *img = [imgs objectAtIndex:0];
                    tiff = [img TIFFRepresentation];
                }
            }
            if (tiff && [tiff length] > 0) {
                NSBitmapImageRep *rep = [NSBitmapImageRep imageRepWithData:tiff];
                if (rep)
                    png = [rep representationUsingType:NSBitmapImageFileTypePNG
                                            properties:@{}];
            }
        }

        if (!png || [png length] == 0) return SOL_FALSE;

        {
            NSUInteger     n   = [png length];
            unsigned char *buf = (unsigned char *)malloc(n);
            if (!buf) return SOL_FALSE;
            memcpy(buf, [png bytes], n);
            *out_bytes = buf;
            *out_len   = (int)n;
        }
        return SOL_TRUE;
    }
}
```
Notes: `@autoreleasepool` bounds the temporary Cocoa objects (the paste happens mid-frame; no ambient pool is guaranteed). The function references NO GL/Metal/RHI. If `readObjectsForClasses:` or any symbol is unavailable on the deployment SDK, the simpler `dataForType:NSPasteboardTypePNG`/`NSPasteboardTypeTIFF` path is enough — keep both.

- [ ] **Step 3: Build wiring.** `platform_clipboard.m` must be compiled (ARC) to a `.o` and linked into the three full-app builds (NOT `c89check`). Mirror how `rhi_metal.m` is compiled in the metal build.

  **(a) Metal build** (the `if [ "$MODE" = "metal" ]` block): it already does `clang -fobjc-arc … -c rhi_metal.m … -o rhi_metal.o`. Right after that line add:
  ```sh
    clang -fobjc-arc -g -O0 -Wall -Wextra \
        -c platform_clipboard.m $(pkg-config --cflags glfw3) -o platform_clipboard.o
  ```
  Add ` platform_clipboard.o` to the `solarium-metal` link line's object list (next to `rhi_metal.o`), and change the trailing `rm -f rhi_metal.o` to `rm -f rhi_metal.o platform_clipboard.o`.

  **(b) Asan build** (the `if [ "$MODE" = "asan" ]` block): before its `clang -std=c11 …` line, add a sanitizer-matched ObjC compile (so the asan binary links clean):
  ```sh
    clang -fobjc-arc -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined \
        -Wall -Wextra \
        -c platform_clipboard.m $(pkg-config --cflags glfw3) -o platform_clipboard.o
  ```
  Append ` platform_clipboard.o` to that build's source/object list (after the `.c` files, before the frameworks), and add `rm -f platform_clipboard.o` immediately after the link command. (The asan build is a manual sanitizer tool, not part of the gauntlet, but leave it linkable.)

  **(c) Debug/release build** (the final unconditional `clang -std=c11 $FLAGS …` at the bottom): immediately BEFORE it add:
  ```sh
clang -fobjc-arc $FLAGS -Wall -Wextra \
    -c platform_clipboard.m $(pkg-config --cflags glfw3) -o platform_clipboard.o
  ```
  Append ` platform_clipboard.o` to its object list (after the `.c` files, before `$(pkg-config …)`), and add `rm -f platform_clipboard.o` immediately after that final link command (after the `echo "built ./solarium …"` is fine too).

  All three already link `-framework Cocoa` (the umbrella that includes AppKit/`NSPasteboard`), so no new framework flag. Do NOT add `platform_clipboard.m` to the `c89check` source list.

- [ ] **Step 4: Gauntlet** — `./build.sh c89check && ./build.sh && ./build.sh metal`. All three pass. (`c89check` is unchanged/clean since the `.m` isn't listed; the debug and metal builds now compile + link `platform_clipboard.o`.) Verify the link has no undefined symbols and no leftover `platform_clipboard.o` after (the `rm -f`).

- [ ] **Step 5: Commit:**
```bash
git add platform_clipboard.h platform_clipboard.m build.sh
git commit -m "$(printf 'Clipboard paste 2/3: Cocoa clipboard seam (platform_clipboard.m) + build wiring\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 3: the paste handler + .gitignore

**Files:** Modify `main.c`, `.gitignore`.

- [ ] **Step 1: Includes + AppState field.** Near the top `#include`s of `main.c`, add:
```c
#include "platform_clipboard.h"
```
Confirm `nid.h` and `platform_fs.h` are already included (they are used elsewhere); if either is missing, add it. In `AppState` (next to `d_was_down`), add:
```c
    sol_bool    paste_was_down;      /* edge-detect for Cmd+V paste */
```
In the AppState reset (where `st->d_was_down = SOL_FALSE;` is set), add:
```c
    st->paste_was_down     = SOL_FALSE;
```

- [ ] **Step 2: The `library_write` helper** — define before `cmd_paste_image` (and before `read_input`), near the other board helpers:
```c
/* Write `len` bytes to library/<nid>.png and fill out_path (cap bytes) with the
   relative path. Returns SOL_TRUE on success. */
static sol_bool library_write(const unsigned char *bytes, int len,
                              char *out_path, int cap) {
    char  nid[NID_LEN + 1];
    FILE *f;
    nid_generate(nid);
    fs_mkdir("library");
    snprintf(out_path, (size_t)cap, "library/%s.png", nid);
    f = fopen(out_path, "wb");
    if (!f) return SOL_FALSE;
    if (fwrite(bytes, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        return SOL_FALSE;
    }
    fclose(f);
    return SOL_TRUE;
}
```
(`NID_LEN`/`nid_generate` from `nid.h`; `fs_mkdir` from `platform_fs.h`; `FILE`/`fopen`/`fwrite` from `<stdio.h>`, already included.)

- [ ] **Step 3: `cmd_paste_image`** — define after `library_write` and after `spawn_image_picture`/`board_local_frac`/`board_under_ray`/`board_pin_pos` (all exist):
```c
/* Cmd+V in board view: paste the clipboard image -> library/<nid>.png -> a
   picture on the board at the cursor, on the current page. */
static void cmd_paste_image(AppState *st, GLFWwindow *w) {
    unsigned char *bytes = (unsigned char *)0;
    int            len   = 0;
    Image          img;
    char           path[256];
    sol_u32        board, a;
    vec3           blocal;
    if (st->board_view == 0) return;
    if (!clipboard_read_image(&bytes, &len) || !bytes || len <= 0) {
        printf("paste: no image on the clipboard\n");
        return;
    }
    if (!image_load_from_memory(bytes, len, &img)) {   /* validate it decodes */
        printf("paste: clipboard image not decodable\n");
        free(bytes);
        return;
    }
    image_free(&img);
    if (!library_write(bytes, len, path, (int)sizeof path)) {
        printf("paste: could not write the library file\n");
        free(bytes);
        return;
    }
    free(bytes);
    blocal = vec3_make(0.0f, 0.0f, 0.0f);
    board  = board_under_ray(st, pick_ray(st, w), &blocal);
    if (board == 0) {                                  /* cursor off the board: center */
        board  = st->board_view;
        blocal = board_local_frac(st, board, 0.0f, 0.5f);
    }
    a = spawn_image_picture(st, board, vec3_make(0.0f, 0.0f, 0.0f),
                            quat_identity(), path);     /* tags the page internally */
    {
        SceneObject *ao = scene_get(&st->scene, a);
        if (ao) {
            float ph = mesh_ref_param("picture", ao->mesh_params,
                                      ao->mesh_param_count, "h");
            ao->pos = board_pin_pos(&st->scene, board, a, blocal, 0.0f, -0.5f * ph);
        }
    }
    st->selected_handle = a;
    scene_save(&st->scene, "scene.stml");
    printf("pasted image -> %s\n", path);
}
```
Notes: `spawn_image_picture` already calls `board_card_tag_page` internally, so the pasted picture lands on the current page — no extra tag call. The `Image img` validation decodes the bytes (catches non-image clipboard data) then frees; the raw clipboard bytes (already PNG) are what gets written. `free(bytes)` happens exactly once on every path.

- [ ] **Step 4: The Cmd+V handler.** Among the discrete-key edge-detect blocks (next to the `d`-key handler — search `st->d_was_down = d_now;`), add:
```c
    {
        sol_bool paste_now = (sol_bool)(
            (glfwGetKey(w, GLFW_KEY_LEFT_SUPER)  == GLFW_PRESS ||
             glfwGetKey(w, GLFW_KEY_RIGHT_SUPER) == GLFW_PRESS) &&
            glfwGetKey(w, GLFW_KEY_V) == GLFW_PRESS);
        if (paste_now && !st->paste_was_down && st->board_view != 0)
            cmd_paste_image(st, w);
        st->paste_was_down = paste_now;
    }
```
(Place it where the other discrete keys are read — after `read_input`'s early-returns for note-edit / palette-open, so paste can't fire while typing. Registered command hotkeys like `V`=mint-codex are already disabled in board view at main.c:10799, so Cmd+V won't also mint a codex.)

- [ ] **Step 5: `.gitignore`** — add a line:
```
library/
```

- [ ] **Step 6: Gauntlet** — `./build.sh c89check && ./build.sh && ./build.sh metal`. All three pass.

- [ ] **Step 7: Live-verify (human) + commit.** Copy an image (a screenshot or a browser image) to the clipboard; run `./solarium` (or `./solarium-metal`); enter board view; press **Cmd+V** → the image appears on the board at the cursor, on the current page; a second Cmd+V makes a second picture; check `library/` holds the `<nid>.png` files; quit and relaunch → the pasted pictures persist (loaded from `library/`); Cmd+V with text or an empty clipboard → nothing happens (a console note). Confirm `git status` shows `library/` ignored (untracked, not staged). Then:
```bash
git add main.c .gitignore
git commit -m "$(printf 'Clipboard paste 3/3: Cmd+V paste handler + library write + gitignore\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Final verification (after all tasks)

- [ ] **Gauntlet:** `./build.sh c89check && ./build.sh && ./build.sh metal` all green; no `platform_clipboard.o` left in the tree.
- [ ] **Human live-verify the full flow** (above), both backends; the pasted picture is draggable/resizable/deletable like any board picture (it reuses the picture object), and persists across reload.

## Notes for the implementer

- **Read the spec:** `docs/superpowers/specs/2026-06-26-clipboard-paste-images-design.md`.
- The `.m` is the ONLY Objective-C; keep it tiny and Cocoa-only. Compile it with `-fobjc-arc` to a `.o` and link — exactly the `rhi_metal.m` pattern.
- We write the clipboard's already-encoded PNG bytes **raw** — no `stb_image_write`. `image_load_from_memory` is used only to validate (and is freed immediately).
- `library/<nid>.png` is local + gitignored; deleting a pasted picture orphans its file on disk (no GC — acceptable for v1).
- No new shader → no MSL twin; the picture reuses `draw_mesh`.
