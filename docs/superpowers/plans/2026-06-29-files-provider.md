# Files Provider (Disk / Filesystem Browser) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a third provider — "Files" — to the entity browser (`;`) that lets the user drill through the real live filesystem starting at `~/` and, on a chosen folder/file, Mount as root / Carry into their grasp.

**Architecture:** A new `TypeProvider` row in `main.c`'s `g_providers[]`, backed by a new pure path-math module (`diskpath.c`, headless-tested) and two new POSIX helpers in the `platform_fs.c` quarantine (`fs_home_dir`, `fs_is_dir`). Mount reuses `create_root_from_path`; Carry reuses the `inventory_anchor`/`inventory_take` card-spawn pattern (verbatim from `pictures_run`); the descend-on-drop flow then takes over with no new wiring. One contained shared-shell change makes a provider's `run` return stay/close so the "Open" command re-lists a directory without closing the HUD.

**Tech Stack:** C89 (engine sources are `-std=c89 -pedantic-errors -Werror -Wall -Wextra`; `*_test.c` may be c11/c89), OpenGL + Metal dual backend (no new shader here, so no MSL twin), hand-written `build.sh`.

**Conventions (apply to every task):**
- Engine `.c` is **strict C89**: declarations at the top of each block, no `//` comments, no C99/C11 constructs.
- **Never** `git add` `NOTES.stml` or `paper-picture.png` (Fran's files). Stage only the files each task names.
- Commit message bodies end with exactly: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`
- Work happens on the **`files-provider`** branch (already created; the spec is committed there at `82f1bfc`).
- **Do not run** `./solarium` or `./solarium-metal` (no display). The build gauntlet is the automated check; Fran does the GUI live-verify after merge.

**Full build gauntlet** (run the relevant subset per task as noted):
```
./build.sh diskpathtest && ./diskpath_test    # the new pure unit test
./build.sh browsertest   && ./browser_test     # regression for the browser nav module
./build.sh                                      # GL debug build (lenient)
./build.sh c89check                             # strict -std=c89 -pedantic-errors -Werror -Wall -Wextra
./build.sh asan                                 # GL + AddressSanitizer
./build.sh metal                                # Metal backend (compiles C/ObjC; MSL compiles on-device)
```

---

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `diskpath.h` / `diskpath.c` | Pure path string math (parent, join, basename, is-root). No GL/scene/POSIX. | **Create** |
| `diskpath_test.c` | Headless unit tests for `diskpath`. | **Create** |
| `platform_fs.h` / `platform_fs.c` | POSIX quarantine. Adds `fs_home_dir`, `fs_is_dir`. | Modify |
| `main.c` | New `AppState.browser_disk_cwd`; the Files provider (`disk_*`); `run`-returns-int shell tweak; `g_providers[]` row. | Modify |
| `build.sh` | `diskpathtest` target; `diskpath.c` added to the 4 engine source lists. | Modify |

---

## Task 1: The `diskpath` pure module + test + build wiring

**Files:**
- Create: `diskpath.h`, `diskpath.c`, `diskpath_test.c`
- Modify: `build.sh` (new `diskpathtest` target; add `diskpath.c` to the 4 engine source lists)

- [ ] **Step 1: Write the header**

Create `diskpath.h`:

```c
#ifndef DISKPATH_H
#define DISKPATH_H
/* diskpath.h — pure path string math for the Files browser provider. No GL,
   no scene, no POSIX; depends only on sol_base + libc string. The risky bit of
   the filesystem browser (parent/join/basename edge cases) lives here so it can
   be headless-tested (the caret.c / route.c "split pure logic" law). All output
   writers always NUL-terminate and clamp to cap. Paths are expected absolute. */
#include "sol_base.h"

/* Parent directory of `path` into out[cap]. "/a/b/c"->"/a/b"; "/a"->"/";
   "/a/b/"->"/a"; "/"->"/"; ""->"". Trailing slashes ignored. */
void        diskpath_parent(const char *path, char *out, int cap);

/* Join dir + name with exactly one '/'. "/a"+"b"->"/a/b"; "/"+"b"->"/b";
   "/a/"+"b"->"/a/b". Always NUL-terminates; clamps to cap. */
void        diskpath_join(const char *dir, const char *name, char *out, int cap);

/* Pointer to the last path segment (basename), borrowed from `path`.
   "/a/b"->"b"; "foo"->"foo"; "/"->"". */
const char *diskpath_basename(const char *path);

/* SOL_TRUE iff path is exactly the filesystem root "/". */
sol_bool    diskpath_is_root(const char *path);

#endif /* DISKPATH_H */
```

- [ ] **Step 2: Write the failing test**

Create `diskpath_test.c`:

```c
/* diskpath_test.c — headless unit tests for the pure path module. C89. */
#include "diskpath.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

static void t_parent(void) {
    char b[256];
    diskpath_parent("/Users/fran/Documents", b, (int)sizeof b); assert(strcmp(b, "/Users/fran") == 0);
    diskpath_parent("/a/b", b, (int)sizeof b);                  assert(strcmp(b, "/a") == 0);
    diskpath_parent("/a",   b, (int)sizeof b);                  assert(strcmp(b, "/") == 0);
    diskpath_parent("/a/b/",b, (int)sizeof b);                  assert(strcmp(b, "/a") == 0);
    diskpath_parent("/",    b, (int)sizeof b);                  assert(strcmp(b, "/") == 0);
    diskpath_parent("",     b, (int)sizeof b);                  assert(b[0] == '\0');
    diskpath_parent("/a/bcdef", b, 4);                          assert(strlen(b) <= 3); /* clamp */
}
static void t_join(void) {
    char b[256];
    diskpath_join("/Users/fran", "Documents", b, (int)sizeof b); assert(strcmp(b, "/Users/fran/Documents") == 0);
    diskpath_join("/",  "Users", b, (int)sizeof b);              assert(strcmp(b, "/Users") == 0);
    diskpath_join("/a/","b",     b, (int)sizeof b);              assert(strcmp(b, "/a/b") == 0);
    diskpath_join("/aaaa", "bbbb", b, 6);                        assert(strlen(b) <= 5); /* clamp */
}
static void t_basename(void) {
    assert(strcmp(diskpath_basename("/a/b"), "b") == 0);
    assert(strcmp(diskpath_basename("/a/b/c.txt"), "c.txt") == 0);
    assert(strcmp(diskpath_basename("foo"), "foo") == 0);
    assert(strcmp(diskpath_basename("/"), "") == 0);
}
static void t_isroot(void) {
    assert(diskpath_is_root("/"));
    assert(!diskpath_is_root("/a"));
    assert(!diskpath_is_root(""));
}
int main(void) {
    t_parent(); t_join(); t_basename(); t_isroot();
    printf("diskpath: all tests passed\n");
    return 0;
}
```

- [ ] **Step 3: Add the `diskpathtest` build target**

In `build.sh`, add this block immediately after the `browsertest` block (after its `fi` at line ~109), modeled on `mapmathtest`:

```sh
# diskpathtest: the Files-browser pure path module (scene-free C89). libc only.
if [ "$MODE" = "diskpathtest" ]; then
    set -x
    clang -std=c89 -pedantic-errors -Werror -g -fsanitize=address,undefined \
        diskpath.c diskpath_test.c \
        -o diskpath_test
    echo "built ./diskpath_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi
```

- [ ] **Step 4: Run the test to verify it fails (no implementation yet)**

Run: `./build.sh diskpathtest`
Expected: FAIL — linker error `Undefined symbols ... _diskpath_parent` (etc.), because `diskpath.c` has no bodies yet.

- [ ] **Step 5: Write the implementation**

Create `diskpath.c`:

```c
/* diskpath.c — pure path string math (see diskpath.h). C89, libc string only. */
#include "diskpath.h"
#include <string.h>

/* copy `len` bytes of src into out[cap], always NUL-terminated, clamped. */
static void dp_copy(char *out, int cap, const char *src, int len) {
    int n;
    if (cap <= 0) return;
    n = len;
    if (n > cap - 1) n = cap - 1;
    if (n > 0) memcpy(out, src, (size_t)n);
    out[n] = '\0';
}

sol_bool diskpath_is_root(const char *path) {
    if (!path) return SOL_FALSE;
    return (path[0] == '/' && path[1] == '\0') ? SOL_TRUE : SOL_FALSE;
}

void diskpath_parent(const char *path, char *out, int cap) {
    int len, i;
    if (cap <= 0) return;
    out[0] = '\0';
    if (!path || !path[0]) return;
    len = (int)strlen(path);
    while (len > 1 && path[len - 1] == '/') len--;     /* ignore trailing slashes */
    i = len - 1;
    while (i > 0 && path[i] != '/') i--;
    if (i <= 0) { dp_copy(out, cap, "/", 1); return; } /* "/foo" -> "/" */
    dp_copy(out, cap, path, i);                        /* up to (excluding) the slash */
}

void diskpath_join(const char *dir, const char *name, char *out, int cap) {
    int dl, n, i;
    if (cap <= 0) return;
    out[0] = '\0';
    if (!dir)  dir  = "";
    if (!name) name = "";
    dl = (int)strlen(dir);
    while (dl > 1 && dir[dl - 1] == '/') dl--;          /* trim trailing '/', keep root */
    if (dl == 1 && dir[0] == '/') dl = 0;               /* root: sep alone gives "/name" */
    n = 0;
    for (i = 0; i < dl && n < cap - 1; i++) out[n++] = dir[i];
    if (n < cap - 1) out[n++] = '/';
    for (i = 0; name[i] && n < cap - 1; i++) out[n++] = name[i];
    out[n] = '\0';
}

const char *diskpath_basename(const char *path) {
    const char *slash;
    if (!path) return "";
    slash = strrchr(path, '/');
    if (!slash) return path;
    return slash[1] ? slash + 1 : "";
}
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `./build.sh diskpathtest && ./diskpath_test`
Expected: builds clean; prints `diskpath: all tests passed`; exit 0; no sanitizer output on stderr.

- [ ] **Step 7: Wire `diskpath.c` into the 4 engine source lists**

In `build.sh`, use a replace-all of the exact substring `caret.c multiselect.c` with `caret.c diskpath.c multiselect.c`. This occurs on exactly 4 lines (the c89check `-fsyntax-only` list ~line 16, the metal list ~454, and the two GL lists ~476/~496), adding the pure module to every engine build.

- [ ] **Step 8: Verify the engine still builds with the new TU**

Run: `./build.sh c89check`
Expected: PASS (no diagnostics). `diskpath.c` compiles as a strict-C89, `-Wall -Wextra` clean unused TU for now.

- [ ] **Step 9: Commit**

```bash
git add diskpath.h diskpath.c diskpath_test.c build.sh
git commit -m "$(cat <<'EOF'
files-provider: pure diskpath module + diskpathtest

Parent/join/basename/is-root path math, headless-tested (the caret.c
"split pure logic" law). Wired into the 4 engine source lists + a
diskpathtest target. Pure C89, libc only.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: `fs_home_dir` + `fs_is_dir` in the POSIX quarantine

**Files:**
- Modify: `platform_fs.h`, `platform_fs.c`

These are thin `getenv`/`stat` wrappers in the quarantined POSIX TU (excluded from c89check). The project has no `platform_fs` test harness — the helpers are exercised by the feature's live-verify; the risky logic (path math) is already tested in Task 1. No dedicated test here is consistent with the existing `platform_fs` posture.

- [ ] **Step 1: Declare the two helpers**

In `platform_fs.h`, add after the `fs_exists` declaration (after line 30):

```c
/* The user's home directory ($HOME), or "/" if unset. Borrowed, stable for the
   process lifetime — the Files browser's default starting directory. */
const char *fs_home_dir(void);

/* Is `path` a directory specifically (not just existing)? SOL_FALSE on a file,
   a broken path, or any stat failure. Lets a bare path ref be classified. */
sol_bool fs_is_dir(const char *path);
```

- [ ] **Step 2: Implement them**

In `platform_fs.c`, add after `fs_exists` (after line 70). `getenv` needs `<stdlib.h>` (already included at line 9); `stat`/`S_ISDIR` need `<sys/stat.h>` (already included at line 13):

```c
const char *fs_home_dir(void) {
    const char *h = getenv("HOME");
    return (h && h[0]) ? h : "/";
}

sol_bool fs_is_dir(const char *path) {
    struct stat st;
    if (!path || !path[0]) return SOL_FALSE;
    if (stat(path, &st) != 0) return SOL_FALSE;
    return S_ISDIR(st.st_mode) ? SOL_TRUE : SOL_FALSE;
}
```

- [ ] **Step 3: Verify it builds**

Run: `./build.sh`
Expected: GL debug build succeeds (`platform_fs.c` compiles; the new symbols link even though nothing calls them yet).

- [ ] **Step 4: Commit**

```bash
git add platform_fs.h platform_fs.c
git commit -m "$(cat <<'EOF'
files-provider: fs_home_dir + fs_is_dir in the POSIX quarantine

Thin getenv/stat wrappers for the Files browser: the default start dir
and a directory classifier for bare path refs.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: `browser_disk_cwd` field + the `run`-returns-int shell tweak

**Files:**
- Modify: `main.c` (AppState struct ~2888; `TypeProvider` struct ~15150; `pictures_run` ~15220; `places_run` ~15311; `browser_handle_key` ~15441)

This makes a provider's `run` return whether to **stay open** (1) or **close** (0), so the Files provider's "Open" command can re-list a directory in place. The two existing providers keep closing (`return 0`).

- [ ] **Step 1: Add the cwd field to AppState**

In `main.c`, in `struct AppState`, immediately after the `browser_preview_aspect` line (~2888), add:

```c
    char        browser_disk_cwd[1024];      /* Files provider cwd; session-persistent, not serialized */
```

- [ ] **Step 2: Change the `run` function-pointer type**

In the `TypeProvider` struct (~15150), change:

```c
    void       (*run)(AppState *st, const char *ref, int cmd);
```
to:
```c
    int        (*run)(AppState *st, const char *ref, int cmd);  /* return 1 = stay open + refresh, 0 = close */
```

- [ ] **Step 3: Make `pictures_run` return int**

Change the `pictures_run` signature and all three return points. The whole function becomes:

```c
static int pictures_run(AppState *st, const char *ref, int cmd) {
    Mesh    empty;
    vec3    p   = carry_place_point(st);
    quat    q   = quat_identity();
    vec3    one = vec3_make(1.0f, 1.0f, 1.0f);
    sol_u32 anchor, h;
    if (cmd != 0) return 0;                  /* only "Place" */
    if (!ref || !ref[0]) return 0;           /* nothing to carry */
    memset(&empty, 0, sizeof empty);
    anchor = inventory_anchor(st);           /* may scene_add -> realloc; handle only */
    h = scene_add(&st->scene, anchor, empty, p, q, one);
    scene_kind_set(&st->scene, h, KIND_PLAIN);     /* keeps the image albedo */
    scene_mesh_ref_set(&st->scene, h, "card");
    scene_content_set(&st->scene, h, ref);
    scene_resolve_meshes(&st->scene);              /* builds the card mesh + sRGB albedo */
    inventory_take(st, h);                         /* image-card carry setup */
    return 0;
}
```

- [ ] **Step 4: Make `places_run` return int**

Change the `places_run` signature and all four return points. The whole function becomes:

```c
static int places_run(AppState *st, const char *ref, int cmd) {
    sol_u32     h;
    const char *slat, *slon, *szoom, *style;
    if (cmd != 0) return 0;                   /* only "Place" */
    if (!ref || !ref[0]) return 0;
    h = (sol_u32)atoi(ref);
    if (!scene_get(&st->scene, h)) return 0;
    slat  = scene_meta_get(&st->scene, h, "lat");
    slon  = scene_meta_get(&st->scene, h, "lon");
    szoom = scene_meta_get(&st->scene, h, "zoom");
    style = scene_meta_get(&st->scene, h, "basemap");
    spawn_map_board(st, slat ? atof(slat) : 0.0, slon ? atof(slon) : 0.0,
                    szoom ? atoi(szoom) : 0, style ? style : "relief");
    scene_save(&st->scene, "scene.stml");
    return 0;
}
```

- [ ] **Step 5: Honor the stay/close return in `browser_handle_key`**

Replace the `BROWSER_ACTIVATE` branch (~15441-15449) with:

```c
    if (a == BROWSER_ACTIVATE) {
        int ti  = (st->browser_type_n > 0) ? st->browser_type_order[st->browser.sel[0]] : -1;
        const char *ref = (st->browser_ent_n > 0)
            ? st->browser_items[st->browser_ent_order[st->browser.sel[1]]].ref : (const char *)0;
        int cmd = (st->browser_cmd_n > 0) ? st->browser_cmd_order[st->browser.sel[2]] : -1;
        int stay = 0;
        if (ti >= 0 && ref && cmd >= 0) stay = g_providers[ti].run(st, ref, cmd);
        if (stay) { st->browser.focus = 1; browser_refresh(st); }   /* re-list, stay on Entities */
        else      { st->browser_open = SOL_FALSE; }
        return;
    }
```

- [ ] **Step 6: Verify all builds + the browser regression**

Run, expecting each to pass:
```
./build.sh
./build.sh c89check
./build.sh asan
./build.sh metal
./build.sh browsertest && ./browser_test
```
Expected: all build clean; `./browser_test` prints its pass line (it links `browser.c`+`fuzzy.c`, not `main.c`, so the signature change can't affect it — it's the regression guard that the pure nav module is untouched).

- [ ] **Step 7: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
files-provider: provider run() returns stay/close

TypeProvider.run now returns 1 (stay open + re-focus Entities + refresh)
or 0 (close), so a provider command can re-list in place. Pictures/Places
return 0 (unchanged behavior). Adds AppState.browser_disk_cwd.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: The Files provider

**Files:**
- Modify: `main.c` (add `#include "diskpath.h"`; add the `disk_*` functions in the provider block before `g_providers[]` ~15381; add the registry row ~15384)

- [ ] **Step 1: Include the path module**

Find where `main.c` includes `platform_fs.h` (it already does — `fs_scan_dir` is used by `pictures_enumerate`). Add directly beneath that include:

```c
#include "diskpath.h"
```

- [ ] **Step 2: Add the Files provider block**

In `main.c`, immediately before `static const TypeProvider g_providers[]` (~15382), insert the provider. It reuses `create_root_from_path` (defined ~9940, in scope), `inventory_anchor`/`inventory_take` (~8995/9079), `carry_place_point`, `apply_kind_materials`/`folderbook_materialize` (forward-declared ~5510/5511), and the new `fs_home_dir`/`fs_is_dir`/`diskpath_*`:

```c
/* --- Files provider --- live drill-down of the real filesystem. The Entities
   column lists browser_disk_cwd (a "../" entry first, then folders, then files);
   Open re-lists a directory in place (run returns 1 = stay); Mount as root reuses
   create_root_from_path; Carry spawns a folder/file tablet into the grasp exactly
   as pictures_run does, then the existing carry+descend flow takes over. cwd is
   session-persistent (remember-last), seeded to ~ on first use. */

/* spawn a folder/file card onto the cursor (the pictures_run pattern). */
static void disk_carry(AppState *st, const char *ref, sol_bool is_dir) {
    Mesh        empty;
    vec3        p   = carry_place_point(st);
    quat        q   = quat_identity();
    vec3        one = vec3_make(1.0f, 1.0f, 1.0f);
    const char *base;
    sol_u32     anchor, h;
    if (!ref || !ref[0]) return;
    memset(&empty, 0, sizeof empty);
    base   = diskpath_basename(ref);
    anchor = inventory_anchor(st);                 /* may scene_add -> realloc; handle only */
    h = scene_add(&st->scene, anchor, empty, p, q, one);
    scene_kind_set(&st->scene, h, is_dir ? KIND_FOLDER : KIND_FILE);
    scene_mesh_ref_set(&st->scene, h, "card");
    scene_content_set(&st->scene, h, ref);
    scene_meta_set(&st->scene, h, "name", (base && base[0]) ? base : ref);
    scene_resolve_meshes(&st->scene);              /* builds the card mesh */
    apply_kind_materials(&st->scene);              /* folder/file kind color */
    folderbook_materialize(&st->scene);            /* leather cover for a folder card */
    inventory_take(st, h);                         /* carry setup */
}

static int disk_enumerate(AppState *st, BrowserItem *out, int cap) {
    FsListing l;
    int       n = 0, pass, i, total;
    if (cap < 1) return 0;
    if (st->browser_disk_cwd[0] == '\0') {                       /* seed to ~ on first use */
        strncpy(st->browser_disk_cwd, fs_home_dir(), sizeof st->browser_disk_cwd - 1);
        st->browser_disk_cwd[sizeof st->browser_disk_cwd - 1] = '\0';
    }
    if (!diskpath_is_root(st->browser_disk_cwd)) {               /* parent entry, unless at / */
        strcpy(out[n].name, "../");
        diskpath_parent(st->browser_disk_cwd, out[n].ref, BROWSER_REF_CAP);
        n++;
    }
    if (!fs_scan_dir(st->browser_disk_cwd, &l)) return n;        /* unreadable: just ".." */
    for (pass = 0; pass < 2 && n < cap; pass++) {                /* pass 0 = folders, 1 = files */
        for (i = 0; i < l.count && n < cap; i++) {
            sol_bool d = l.entries[i].is_dir;
            int      ln;
            if (pass == 0 && !d) continue;
            if (pass == 1 &&  d) continue;
            strncpy(out[n].name, l.entries[i].name, BROWSER_NAME_CAP - 2);
            out[n].name[BROWSER_NAME_CAP - 2] = '\0';
            if (d) {                                             /* folders shown with a trailing '/' */
                ln = (int)strlen(out[n].name);
                out[n].name[ln] = '/';
                out[n].name[ln + 1] = '\0';
            }
            diskpath_join(st->browser_disk_cwd, l.entries[i].name, out[n].ref, BROWSER_REF_CAP);
            n++;
        }
    }
    total = l.count + (diskpath_is_root(st->browser_disk_cwd) ? 0 : 1);
    if (total > cap)
        printf("Files: '%s' has %d entries, showing %d (truncated)\n",
               st->browser_disk_cwd, total, cap);
    fs_listing_free(&l);
    return n;
}

static int disk_commands(AppState *st, const char *ref, const char **out, int cap) {
    int n = 0;
    (void)st;
    if (cap < 1 || !ref || !ref[0]) return 0;
    if (fs_is_dir(ref)) {
        out[n++] = "Open";
        if (n < cap) out[n++] = "Mount as root";
        if (n < cap) out[n++] = "Carry";
    } else {
        out[n++] = "Carry";
    }
    return n;
}

static int disk_run(AppState *st, const char *ref, int cmd) {
    sol_bool is_dir;
    if (!ref || !ref[0]) return 0;
    is_dir = fs_is_dir(ref);
    if (is_dir) {
        if (cmd == 0) {                          /* Open: descend; keep the HUD open */
            strncpy(st->browser_disk_cwd, ref, sizeof st->browser_disk_cwd - 1);
            st->browser_disk_cwd[sizeof st->browser_disk_cwd - 1] = '\0';
            st->browser_items_type   = -1;        /* force re-enumerate (same provider) */
            st->browser.sel[1]       = 0;         /* back to the top ("../") */
            st->browser.filter[1][0] = '\0';
            st->browser.flen[1]      = 0;
            return 1;                             /* stay open */
        }
        if (cmd == 1) { create_root_from_path(st, ref); return 0; }  /* Mount as root */
        disk_carry(st, ref, SOL_TRUE);            /* cmd 2: Carry folder */
        return 0;
    }
    disk_carry(st, ref, SOL_FALSE);               /* file: Carry */
    return 0;
}

static RhiTexture disk_preview(AppState *st, const char *ref, float *out_aspect) {
    RhiTexture none;
    int        w, h;
    none.id = 0;
    (void)st;
    if (!ref || !ref[0]) return none;
    if (fs_is_dir(ref)) return none;              /* no folder preview in v1 */
    if (!reader_is_image_path(ref)) return none;
    if (out_aspect && image_dims(ref, &w, &h) && h > 0) *out_aspect = (float)w / (float)h;
    return load_texture(ref);                     /* the actual image, cached sRGB */
}
```

- [ ] **Step 3: Register the provider**

Change `g_providers[]` (~15382) to:

```c
static const TypeProvider g_providers[] = {
    { "Pictures", pictures_enumerate, pictures_commands, pictures_run, pictures_preview },
    { "Places",   places_enumerate,   places_commands,   places_run,   places_preview },
    { "Files",    disk_enumerate,     disk_commands,     disk_run,     disk_preview }
};
```

(Still 3 ≤ 8, so the `browser_provider_cap_check` static-assert and the `typenames[8]`/`browser_type_order[8]` arrays are unaffected.)

- [ ] **Step 4: Run the full build gauntlet**

Run, expecting each to pass clean:
```
./build.sh
./build.sh c89check
./build.sh asan
./build.sh metal
./build.sh diskpathtest && ./diskpath_test
./build.sh browsertest  && ./browser_test
```
Expected: all build with no diagnostics; both tests print their pass lines. Watch `c89check` especially for decl-after-statement (the `int ln;` and `sol_bool d;` are declared at the top of their blocks — verify no mixed decl/code crept in).

- [ ] **Step 5: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
files-provider: the Files disk-browser provider

Third entity-browser provider: live filesystem drill-down from ~ (".."
+ folders + files), remember-last cwd. Folder commands Open (re-list in
place) / Mount as root (create_root_from_path) / Carry (tablet into the
grasp, then the existing descend flow); files are Carry-able; image files
preview. No new shader.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Final holistic review + live-verify handoff

**Files:** none (verification only)

- [ ] **Step 1: Full holistic re-read**

Re-read the four `disk_*` functions and the shell tweak together as one feature (the per-task reviews look at slices; cross-task seam bugs hide between them — a recurring lesson in this codebase). Confirm specifically:
- `disk_commands` order matches `disk_run`'s `cmd` indices (0 Open / 1 Mount / 2 Carry for dirs; 0 Carry for files).
- `disk_run` "Open" sets `browser_items_type = -1` (without it, `browser_refresh` won't re-enumerate because the provider index didn't change).
- The `inventory_anchor` handle is captured before the `scene_add` in `disk_carry` (realloc discipline).
- `browser_open_now` does **not** clear `browser_disk_cwd` (remember-last relies on that).

- [ ] **Step 2: Final gauntlet**

Run the full gauntlet once more from a clean state:
```
./build.sh && ./build.sh c89check && ./build.sh asan && ./build.sh metal \
  && ./build.sh diskpathtest && ./diskpath_test \
  && ./build.sh browsertest && ./diskpath_test && ./browser_test
```
Expected: every step exits 0; both tests print their pass lines.

- [ ] **Step 3: Hand off to Fran for GUI live-verify**

Subagents cannot GUI-test. Present this checklist for Fran to verify in `./solarium` and `./solarium-metal`:
1. `;` → select **Files**: the Entities column lists `~/` (`../` first, folders with trailing `/`, then files).
2. Select a folder → `→` → **Open** → the column re-lists that folder *without* closing the HUD; selection returns to the top.
3. `../` → **Open** repeatedly walks up toward `/` (no `../` shown at `/`).
4. Close (`;`/Esc) and reopen → it reopens at the last directory (remember-last); restart the app → back to `~/`.
5. On a folder → **Mount as root** → a new room appears near Home, walkway-connected, filled with the folder's cards; walk in.
6. On a folder → **Carry** → a folder tablet is in your grasp; aim at a room wall and drop → it opens as a sub-room (descend).
7. On a file → **Carry** → a file tablet in your grasp; read it / shelve it.
8. Highlight an image file → its real image previews above the command list, right-side-up and aspect-correct.

After Fran confirms, use **superpowers:finishing-a-development-branch** to ff-merge `files-provider` to `main` (verify the gauntlet, then merge; never stage `NOTES.stml`/`paper-picture.png`).

---

## Self-Review

**Spec coverage:** Live drill-down (Task 4 `disk_enumerate`) ✓ · all-via-Commands navigation (Task 4 `disk_commands`/`disk_run` Open) ✓ · Mount as root (Task 4 `disk_run` cmd 1 → `create_root_from_path`) ✓ · Carry folder & file (Task 4 `disk_carry`) ✓ · files shown, carryable, image preview (Task 4 `disk_enumerate`/`disk_preview`) ✓ · remember-last start `~/` (Task 3 field + Task 4 lazy seed; `browser_open_now` doesn't reset) ✓ · `../` parent / no `../` at root (Task 4 + `diskpath_is_root`) ✓ · "Open stays open" shell tweak (Task 3) ✓ · pure `diskpath` + test (Task 1) ✓ · `fs_home_dir`/`fs_is_dir` (Task 2) ✓ · no new shader/MSL twin (no shader touched) ✓ · error edges: unreadable dir keeps `../` (Task 4), root has no `../` (Task 4), >2048 logged (Task 4 `total>cap`), long-path clamp (`diskpath_*` cap), realloc discipline (Task 4 `disk_carry`) ✓.

**Placeholder scan:** No TBD/TODO; every code step shows complete code; every build step gives an exact command + expected output. None found.

**Type consistency:** `run` returns `int` everywhere it's defined (TypeProvider field, `pictures_run`, `places_run`, `disk_run`) and is consumed as `int stay` in `browser_handle_key` ✓. `browser_disk_cwd[1024]` declared in Task 3, read/written in Task 4 ✓. `diskpath_*`/`fs_home_dir`/`fs_is_dir` signatures match between header (Tasks 1/2) and call sites (Task 4) ✓. `disk_commands` cmd ordering matches `disk_run` dispatch ✓.
