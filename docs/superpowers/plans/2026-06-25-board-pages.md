# Board Pages Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn each whiteboard into a navigable notebook — a flat graph of folder-linked pages, each holding its own cards, browsed by double-clicking folders or arrow-cycling.

**Architecture:** Pages are emergent from `meta` tags (`meta["page"]` on each board-child, `meta["active_page"]` on the board); visibility is one O(1) gate folded into `scene_object_active`, exactly like the workspace filter. A folder is a `KIND_PLAIN` object with `mesh_ref="folderbook"` (a new randomized closed-book mesh) carrying `meta["link"]`. Creation auto-makes a backlink; navigation is one `meta` write. No new shader → no MSL twin.

**Tech Stack:** C89 ("Dependable C"), OpenGL + Metal (dual backend), hand-written `build.sh`. Pure-logic lives in a new scene-free module `boardpage.c`; the rest is `main.c` + one mesh in `mesh.c`. Spec: `docs/superpowers/specs/2026-06-25-board-pages-design.md`.

**Gauntlet (run all three after every task that touches buildable code):**
- `./build.sh c89check` — strict `-std=c89 -pedantic-errors -Werror -Wall -Wextra`
- `./build.sh` — debug GL build
- `./build.sh metal` — Metal build (compiles C/ObjC; MSL compiles on-device, but this feature adds no shader)

**Project laws to honor in every task:**
- **Use-after-realloc:** `scene_add` may move the objects array. Never hold a `SceneObject*` across a `scene_add`; set fields via handle-based setters or re-fetch with `scene_get` after.
- **C89 declarations:** all declarations at the top of their block; no declaration-after-statement; no C99 `//` comments; no C99 math in `main.c` (use `(float)sin((double)x)`, ternary `fabs`). `c89check` is the authority.
- **Registry-shared meshes** are rebuilt via `asset_release(oldkey)` + clear + `scene_resolve_meshes` — never `mesh_destroy` a shared shape. (Not needed here; folders aren't resized.)
- **Never `git add`** `NOTES.stml` or `paper-picture.png`. Commit only the files each task names.
- Commit messages end with exactly: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

---

## File Structure

- **`boardpage.h` / `boardpage.c`** (NEW, scene-free, libc-only, C89): `boardpage_slugify` + `boardpage_collect` + the `PAGE_SLUG_CAP` / `BOARD_PAGE_MAX` constants. The pure logic, unit-tested in isolation (the `furniture.c` / `inventory.c` discipline).
- **`boardpage_test.c`** (NEW): the unit suite for the two pure functions.
- **`mesh.c` / `mesh.h`**: `make_folderbook` builder + a `"folderbook"` REGISTRY row.
- **`workspace.c`**: `scene_object_paged_out` helper folded into `scene_object_active` (the one filter seam).
- **`workspace_test.c`**: one new case asserting the page gate hides a paged-out board child.
- **`main.c`**: helpers (`object_is_folder`, `board_pages`, `add_folder`, `cycle_page`); the `d`-create flow; the double-click navigate arm; arrow-cycle; the page title + folder label render; the delete-folder branch; M2 drag-to-file (`drop_target_handle`); new `AppState` fields.
- **`build.sh`**: add `boardpage.c` to the 4 full-build link lines + a `boardpagetest` target.

---

# Milestone 1 — Navigable notebook

## Task 1: `boardpage` module — slugify + page-list (pure logic, TDD)

**Files:**
- Create: `boardpage.h`, `boardpage.c`, `boardpage_test.c`
- Modify: `build.sh`

- [ ] **Step 1: Write the header**

Create `boardpage.h`:
```c
#ifndef SOL_BOARDPAGE_H
#define SOL_BOARDPAGE_H

/* A page slug is a flat, board-scoped key: a leading '/' + lowercase
   [a-z0-9-]. PAGE_SLUG_CAP bounds its bytes incl. the '/' and NUL. */
#define PAGE_SLUG_CAP   96
/* Max distinct pages a single board's arrow-cycle enumerates. */
#define BOARD_PAGE_MAX  64

/* Slugify a user-typed page name into out[cap] (NUL-terminated):
   lowercase; every maximal run of non-[a-z0-9] characters (spaces AND
   punctuation, including any typed '/') collapses to a single '-';
   trim leading/trailing '-'; prepend exactly one leading '/'.
   Returns the length written (excl. NUL). A name with no alphanumerics
   yields just "/" (length 1) — the caller reads that as "cancel". */
int boardpage_slugify(const char *in, char *out, int cap);

/* Build a board's navigable page list from its raw page tags:
   dedupe the n input slugs, always include "/" and `active`, and sort
   "/" first then ascending strcmp, into out[cap][PAGE_SLUG_CAP].
   NULL/empty input slugs are skipped. `active` NULL/"" is treated as "/".
   Returns the row count (<= cap). */
int boardpage_collect(const char *const *pages, int n, const char *active,
                      char out[][PAGE_SLUG_CAP], int cap);

#endif
```

- [ ] **Step 2: Write the failing tests**

Create `boardpage_test.c`:
```c
#include "boardpage.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

static void test_slugify(void) {
    char b[PAGE_SLUG_CAP];
    int  n;
    n = boardpage_slugify("Chapter 1: Notes!", b, sizeof b);
    CHECK(strcmp(b, "/chapter-1-notes") == 0, "slug: spaces+punct -> dashes");
    CHECK(n == (int)strlen(b), "slug: returns length");
    boardpage_slugify("  trailing  ", b, sizeof b);
    CHECK(strcmp(b, "/trailing") == 0, "slug: trims edges, no trailing dash");
    boardpage_slugify("a///b", b, sizeof b);
    CHECK(strcmp(b, "/a-b") == 0, "slug: internal slashes -> single dash");
    boardpage_slugify("/already-slug", b, sizeof b);
    CHECK(strcmp(b, "/already-slug") == 0, "slug: idempotent on a clean slug");
    n = boardpage_slugify("!!!", b, sizeof b);
    CHECK(strcmp(b, "/") == 0 && n == 1, "slug: all-punct -> '/' (cancel)");
    n = boardpage_slugify("", b, sizeof b);
    CHECK(strcmp(b, "/") == 0 && n == 1, "slug: empty -> '/' (cancel)");
    boardpage_slugify("UPPER Mixed", b, sizeof b);
    CHECK(strcmp(b, "/upper-mixed") == 0, "slug: lowercases");
}

static void test_collect(void) {
    char out[BOARD_PAGE_MAX][PAGE_SLUG_CAP];
    const char *raw[4];
    int n;
    raw[0] = "/beta"; raw[1] = "/alpha"; raw[2] = "/beta"; raw[3] = "/";
    n = boardpage_collect(raw, 4, "/alpha", out, BOARD_PAGE_MAX);
    CHECK(n == 3, "collect: dedupes to 3 (/, /alpha, /beta)");
    CHECK(strcmp(out[0], "/") == 0, "collect: '/' sorts first");
    CHECK(strcmp(out[1], "/alpha") == 0, "collect: then ascending");
    CHECK(strcmp(out[2], "/beta") == 0, "collect: ascending");
    /* active not present in raw is still included */
    raw[0] = "/x";
    n = boardpage_collect(raw, 1, "/zzz", out, BOARD_PAGE_MAX);
    CHECK(n == 3, "collect: active + '/' always present");
    CHECK(strcmp(out[0], "/") == 0, "collect: '/' first even when not raw");
    /* empty input still yields root */
    n = boardpage_collect((const char *const *)0, 0, (const char *)0, out, BOARD_PAGE_MAX);
    CHECK(n == 1 && strcmp(out[0], "/") == 0, "collect: empty -> just '/'");
}

int main(void) {
    test_slugify();
    test_collect();
    if (fails == 0) printf("boardpage_test: all passed\n");
    return fails ? 1 : 0;
}
```

- [ ] **Step 3: Add the `boardpagetest` build target**

In `build.sh`, after the `inventorytest` block (around line 167), add:
```sh
# boardpagetest: the board-page pure logic (scene-free C89). libc only.
if [ "$MODE" = "boardpagetest" ]; then
    set -x
    clang -std=c89 -pedantic-errors -Werror -g -fsanitize=address,undefined \
        boardpage.c boardpage_test.c \
        -o boardpage_test
    echo "built ./boardpage_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi
```

- [ ] **Step 4: Run the test target to verify it fails to link**

Run: `./build.sh boardpagetest`
Expected: link error (undefined `boardpage_slugify` / `boardpage_collect`) — `boardpage.c` has no bodies yet.

- [ ] **Step 5: Implement `boardpage.c`**

Create `boardpage.c`:
```c
#include "boardpage.h"
#include <string.h>

int boardpage_slugify(const char *in, char *out, int cap) {
    int         oi   = 0;
    int         pend = 0;          /* a separator run is pending */
    const char *p;
    if (cap < 2) { if (cap > 0) out[0] = '\0'; return 0; }
    out[oi++] = '/';
    if (in) {
        for (p = in; *p; p++) {
            unsigned char c = (unsigned char)*p;
            int isnum = (c >= '0' && c <= '9');
            int isup  = (c >= 'A' && c <= 'Z');
            int islo  = (c >= 'a' && c <= 'z');
            if (isnum || isup || islo) {
                if (pend && oi > 1 && oi < cap - 1) out[oi++] = '-';
                pend = 0;
                if (oi < cap - 1)
                    out[oi++] = (char)(isup ? (c - 'A' + 'a') : c);
            } else {
                pend = 1;          /* space/punct: defer one dash, collapse runs,
                                      and never flush a trailing one */
            }
        }
    }
    out[oi] = '\0';
    return oi;
}

/* page comparator: "/" sorts before everything, else plain strcmp. */
static int page_cmp(const char *a, const char *b) {
    int ra = (strcmp(a, "/") == 0);
    int rb = (strcmp(b, "/") == 0);
    if (ra != rb) return ra ? -1 : 1;
    return strcmp(a, b);
}

static void page_add_unique(char out[][PAGE_SLUG_CAP], int *count, int cap,
                            const char *s) {
    int i;
    if (!s || !s[0]) return;
    if (*count >= cap) return;
    for (i = 0; i < *count; i++)
        if (strcmp(out[i], s) == 0) return;
    strncpy(out[*count], s, PAGE_SLUG_CAP - 1);
    out[*count][PAGE_SLUG_CAP - 1] = '\0';
    (*count)++;
}

int boardpage_collect(const char *const *pages, int n, const char *active,
                      char out[][PAGE_SLUG_CAP], int cap) {
    const char *act = (active && active[0]) ? active : "/";
    int count = 0, i, j;
    page_add_unique(out, &count, cap, "/");
    page_add_unique(out, &count, cap, act);
    for (i = 0; i < n; i++)
        page_add_unique(out, &count, cap, pages ? pages[i] : (const char *)0);
    /* insertion sort by page_cmp */
    for (i = 1; i < count; i++) {
        char key[PAGE_SLUG_CAP];
        strncpy(key, out[i], PAGE_SLUG_CAP);
        key[PAGE_SLUG_CAP - 1] = '\0';
        j = i - 1;
        while (j >= 0 && page_cmp(out[j], key) > 0) {
            strncpy(out[j + 1], out[j], PAGE_SLUG_CAP);
            out[j + 1][PAGE_SLUG_CAP - 1] = '\0';
            j--;
        }
        strncpy(out[j + 1], key, PAGE_SLUG_CAP);
        out[j + 1][PAGE_SLUG_CAP - 1] = '\0';
    }
    return count;
}
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `./build.sh boardpagetest && ./boardpage_test`
Expected: `boardpage_test: all passed`, no sanitizer output.

- [ ] **Step 7: Wire `boardpage.c` into the four full builds**

In `build.sh`, append ` boardpage.c` to the source list on each of these four `clang` invocations (the order in the list doesn't matter; add it next to `inventory.c`):
- the `c89check` line (~line 16),
- the `metal` line (~line 362),
- the `asan` line (~line 378),
- the final debug/release line (~line 393).

- [ ] **Step 8: Run the gauntlet**

Run: `./build.sh c89check && ./build.sh && ./build.sh metal`
Expected: all three succeed (`boardpage.c` now links into the app, unused so far — fine).

- [ ] **Step 9: Commit**

```bash
git add boardpage.h boardpage.c boardpage_test.c build.sh
git commit -m "$(printf 'Board pages 1/9: boardpage module — slugify + page-list (TDD)\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 2: `make_folderbook` mesh

**Files:**
- Modify: `mesh.h` (declaration), `mesh.c` (builder + REGISTRY row)

**Note on testing:** mesh geometry is visual, not unit-tested in this project (only the math-y modules have suites). Verification here is the three-target gauntlet (it compiles + registers) plus the Task 4 live-verify (a folder appears on the board). No new test file.

- [ ] **Step 1: Declare the builder in `mesh.h`**

Next to the other `make_book_*` declarations in `mesh.h`, add:
```c
/* A closed book seen cover-out, pinned to a board like a thick card:
   width along X (centered -w/2..w/2), height along Y (bottom-origin 0..h),
   depth along Z (0..d, +Z out of the board face). The big cover faces +Z;
   the spine (left edge) wears `bands` raised cords. One mesh, one material. */
void make_folderbook(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 d, int bands);
```

- [ ] **Step 2: Implement `make_folderbook` in `mesh.c`**

Add near `make_book_block` (after line ~860), using only `box_minmax` (mesh.c:752):
```c
void make_folderbook(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 d, int bands) {
    sol_f32 hw    = w * 0.5f;
    sol_f32 board = d * 0.18f;      /* cover-board thickness in z */
    sol_f32 inset = h * 0.05f;      /* page block inset from top/bottom/fore-edge */
    sol_f32 lip   = w * 0.03f;      /* covers overhang the leaves at the fore-edge */
    int     k;
    if (board < 1e-4f) board = 1e-4f;
    /* the leaf block: flush at the spine (left), inset elsewhere */
    box_minmax(b, -hw + board, hw - lip, inset, h - inset, board, d - board);
    /* back + front cover boards (full footprint) */
    box_minmax(b, -hw, hw, 0.0f, h, 0.0f, board);
    box_minmax(b, -hw, hw, 0.0f, h, d - board, d);
    /* spine slab closing the left edge across the full depth */
    box_minmax(b, -hw, -hw + board, 0.0f, h, 0.0f, d);
    /* raised bands: thin cords proud of the spine */
    for (k = 1; k <= bands; k++) {
        sol_f32 yc = h * (sol_f32)k / (sol_f32)(bands + 1);
        sol_f32 bw = h * 0.020f;
        box_minmax(b, -hw - board * 0.5f, -hw, yc - bw, yc + bw, 0.0f, d);
    }
}
```

- [ ] **Step 3: Add the emitter + REGISTRY row in `mesh.c`**

Near `emit_book_block` (mesh.c:1014), add:
```c
static void emit_folderbook(MeshBuilder *b, const float *p) {
    make_folderbook(b, p[0], p[1], p[2], (int)p[3]);
}
```
In the `REGISTRY[]` table (mesh.c:1142), after the `book_open_block` row (line ~1178), add:
```c
    { "folderbook", 4, { "w", "h", "d", "bands" },
      { 0.14f, 0.20f, 0.05f, 4.0f }, emit_folderbook },
```

- [ ] **Step 4: Run the gauntlet**

Run: `./build.sh c89check && ./build.sh && ./build.sh metal`
Expected: all three succeed.

- [ ] **Step 5: Commit**

```bash
git add mesh.h mesh.c
git commit -m "$(printf 'Board pages 2/9: make_folderbook mesh — a closed book pinned cover-out\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 3: the page gate — `scene_object_paged_out` folded into `scene_object_active`

**Files:**
- Modify: `workspace.c` (the helper + the fold-in)
- Test: `workspace_test.c` (one new case)

- [ ] **Step 1: Write the failing test in `workspace_test.c`**

Add this case and call it from `main` (mirror the existing test style — it links `scene.c` + `mesh.c`, so `scene_add` / `scene_meta_set` / `scene_object_active` are available):
```c
static void test_page_gate(void) {
    Scene s; Mesh empty; sol_u32 board, card;
    vec3 z = {0,0,0}; quat q = {0,0,0,1}; vec3 one = {1,1,1};
    scene_init(&s);
    memset(&empty, 0, sizeof empty);
    board = scene_add(&s, 0, empty, z, q, one);
    scene_mesh_ref_set(&s, board, "board");
    card  = scene_add(&s, board, empty, z, q, one);
    scene_meta_set(&s, card, "page", "/chapter");
    /* board active page defaults to "/" -> the card is paged out (hidden) */
    CHECK(scene_object_active(&s, card) == SOL_FALSE, "gate: hidden off-page");
    scene_meta_set(&s, board, "active_page", "/chapter");
    CHECK(scene_object_active(&s, card) == SOL_TRUE,  "gate: shown on-page");
    scene_meta_set(&s, board, "active_page", "/");
    CHECK(scene_object_active(&s, card) == SOL_FALSE, "gate: hidden again");
    /* a card with no page tag is on "/" -> shown when the board is on "/" */
    scene_meta_set(&s, card, "page", "/");
    CHECK(scene_object_active(&s, card) == SOL_TRUE, "gate: root default shown");
    scene_free(&s);
}
```
Match the file's existing `CHECK` macro, includes, and `scene_init`/`scene_free` conventions (the snippet above assumes the same `CHECK(cond, msg)` form the suite already uses). If `scene_object_active` returns the workspace-default for a fresh scene with `active_ws == ""`, these still hold because the page gate runs before the workspace filter. Add `test_page_gate();` to `main`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `./build.sh workspacetest && ./workspace_test`
Expected: the new `gate:` checks FAIL (the gate doesn't exist yet — the card is active regardless of page).

- [ ] **Step 3: Implement `scene_object_paged_out` and fold it in (`workspace.c`)**

Above `scene_object_active` in `workspace.c`, add the helper:
```c
/* A board-child is "paged out" (hidden) when its page tag differs from the
   board's active page. O(1): page-bearing objects pin DIRECTLY to the board,
   so we only test the direct parent — no deep walk. */
static sol_bool scene_object_paged_out(Scene *s, sol_u32 handle) {
    SceneObject *o = scene_get(s, handle);
    SceneObject *par;
    const char  *opage, *bpage;
    if (!o || o->parent == 0) return SOL_FALSE;
    par = scene_get(s, o->parent);
    if (!par || !par->mesh_ref || strcmp(par->mesh_ref, "board") != 0)
        return SOL_FALSE;
    opage = scene_meta_get(s, handle, "page");
    if (!opage) opage = "/";
    bpage = scene_meta_get(s, par->handle, "active_page");
    if (!bpage) bpage = "/";
    return (sol_bool)(strcmp(opage, bpage) != 0);
}
```
In `scene_object_active`, add the gate immediately after the stowed check and before the workspace filter:
```c
    if (scene_object_stowed(s, handle)) return SOL_FALSE;
    if (scene_object_paged_out(s, handle)) return SOL_FALSE;   /* NEW: board page */
    if (s->active_ws[0] == '\0') return SOL_TRUE;
    /* ...existing workspace filter... */
```
Ensure `<string.h>` is included in `workspace.c` (it already uses `strcmp`).

- [ ] **Step 4: Run the test to verify it passes**

Run: `./build.sh workspacetest && ./workspace_test`
Expected: all checks pass, no sanitizer output.

- [ ] **Step 5: Run the gauntlet**

Run: `./build.sh c89check && ./build.sh && ./build.sh metal`
Expected: all three succeed.

- [ ] **Step 6: Commit**

```bash
git add workspace.c workspace_test.c
git commit -m "$(printf 'Board pages 3/9: page gate folded into scene_object_active\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 4: create a folder — `d` in board view

**Files:**
- Modify: `main.c` (include, `AppState` fields, helpers, the `d` handler + callback)

- [ ] **Step 1: Include the header + add `AppState` fields**

Near the other project includes at the top of `main.c`, add:
```c
#include "boardpage.h"
```
In the `AppState` struct, add (next to `selected_handle` / the board-view fields):
```c
    vec3     folder_place_local;   /* board-local point captured at 'd' press */
    sol_bool folder_place_has;     /* 0 = no board hit at press -> use center */
    sol_bool d_was_down;           /* edge-detect for the 'd' folder key */
    sol_bool page_prev_was, page_next_was;  /* edge-detect for arrow-cycle */
    sol_u32  drop_target_handle;   /* folder under a dragged card (M2); 0 = none */
```
In the `AppState` initializer (where `drag_handle = 0` etc. are reset, ~line 11489), add:
```c
    st->folder_place_has   = SOL_FALSE;
    st->d_was_down         = SOL_FALSE;
    st->page_prev_was      = SOL_FALSE;
    st->page_next_was      = SOL_FALSE;
    st->drop_target_handle = 0;
```

- [ ] **Step 2: Add `object_is_folder`, `board_pages`, and `add_folder` helpers**

Place these in `main.c` BEFORE `read_input` (so the press handler and callbacks can call them) and after `board_under_ray` / `board_pin_pos` (which `add_folder` uses). `object_is_folder` mirrors `object_is_board` (main.c:4125):
```c
static sol_bool object_is_folder(Scene *s, sol_u32 h) {
    SceneObject *o = scene_get(s, h);
    return (o && o->mesh_ref && strcmp(o->mesh_ref, "folderbook") == 0)
               ? SOL_TRUE : SOL_FALSE;
}

/* Gather the board's navigable page list (sorted, deduped, '/' + active
   always present) into out. Returns the count. */
static int board_pages(AppState *st, sol_u32 board,
                       char out[][PAGE_SLUG_CAP], int cap) {
    const char *raw[BOARD_PAGE_MAX];
    const char *active;
    int         n = 0;
    sol_u32     i;
    for (i = 0; i < st->scene.count && n < BOARD_PAGE_MAX; i++) {
        SceneObject *o = &st->scene.objects[i];
        if (o->parent != board) continue;
        raw[n++] = scene_meta_get(&st->scene, o->handle, "page");  /* NULL -> '/' in collect */
    }
    active = scene_meta_get(&st->scene, board, "active_page");
    return boardpage_collect(raw, n, active, out, cap);
}

/* Does the target page already carry a folder linking back to `link`? */
static sol_bool folder_backlink_exists(AppState *st, sol_u32 board,
                                       const char *page, const char *link) {
    sol_u32 i;
    for (i = 0; i < st->scene.count; i++) {
        SceneObject *o = &st->scene.objects[i];
        const char *op, *ol;
        if (o->parent != board) continue;
        if (!object_is_folder(&st->scene, o->handle)) continue;
        op = scene_meta_get(&st->scene, o->handle, "page");
        ol = scene_meta_get(&st->scene, o->handle, "link");
        if (op && ol && strcmp(op, page) == 0 && strcmp(ol, link) == 0)
            return SOL_TRUE;
    }
    return SOL_FALSE;
}

/* Add a randomized folder book to `board` on `page`, linking to `link`,
   pinned at board-local hit point `blocal`. Returns the handle. */
static sol_u32 add_folder(AppState *st, sol_u32 board, const char *page,
                          const char *link, vec3 blocal) {
    Mesh    empty;
    vec3    one = vec3_make(1.0f, 1.0f, 1.0f);
    float   p[4];
    int     leather;
    sol_u32 h;
    if (g_mint_rng == 0) g_mint_rng = (unsigned)time((time_t *)0) | 1u;
    p[0] = mint_range(0.11f, 0.16f);             /* w */
    p[1] = mint_range(0.17f, 0.26f);             /* h */
    p[2] = mint_range(0.04f, 0.07f);             /* d */
    p[3] = (float)(int)mint_range(3.0f, 5.99f);  /* bands */
    leather = (int)mint_range(0.0f, 2.99f);
    memset(&empty, 0, sizeof empty);
    h = scene_add(&st->scene, board, empty, vec3_make(0.0f, 0.0f, 0.0f),
                  quat_identity(), one);
    scene_kind_set(&st->scene, h, KIND_PLAIN);
    scene_mesh_ref_set(&st->scene, h, "folderbook");
    scene_mesh_params_set(&st->scene, h, p, 4);
    scene_meta_set(&st->scene, h, "page", page);
    scene_meta_set(&st->scene, h, "link", link);
    scene_resolve_meshes(&st->scene);
    {
        SceneObject *o = scene_get(&st->scene, h);
        if (o) {
            Material m = material_default();
            m.base_color = (leather == 0) ? vec3_make(0.36f, 0.22f, 0.13f)
                         : (leather == 1) ? vec3_make(0.34f, 0.12f, 0.10f)
                                          : vec3_make(0.14f, 0.22f, 0.15f);
            m.roughness = 0.6f;
            o->material  = m;
            o->pos = board_pin_pos(&st->scene, board, h, blocal,
                                   0.0f, -0.5f * p[1]);  /* center on the point */
        }
    }
    return h;
}
```
If `g_mint_rng` / `mint_range` / `material_default` aren't visible at this point in the file, move these helpers below their definitions (they're used by `cmd_mint_codex` at main.c:7469, so define `add_folder` after that). Keep `object_is_folder` and `board_pages` before `read_input`; `add_folder` may sit anywhere before its first caller (the callback in Step 3).

- [ ] **Step 2b: A board-center fallback helper**

Add a tiny helper to compute a board-local default point (used when `d` is pressed with the cursor off the board, and for backlink placement):
```c
/* A board-local point: fx in [-w/2, w/2], fy in [0, h], from fractions of
   the board's own dimensions. */
static vec3 board_local_frac(AppState *st, sol_u32 board, float fx, float fy) {
    SceneObject *o = scene_get(&st->scene, board);
    float bw = o ? mesh_ref_param("board", o->mesh_params, o->mesh_param_count, "w") : 1.8f;
    float bh = o ? mesh_ref_param("board", o->mesh_params, o->mesh_param_count, "h") : 1.2f;
    return vec3_make(fx * bw, fy * bh, 0.0f);
}
```

- [ ] **Step 3: Add the `create_folder_from_name` callback**

Place it before the `d` handler (and after `add_folder`). It reads the board, slugifies, resolves new-vs-existing, and lays the forward + backlink folders:
```c
static void create_folder_from_name(AppState *st, const char *typed) {
    sol_u32     board = st->board_view;
    char        target[PAGE_SLUG_CAP];
    const char *src;
    vec3        fwd_local, back_local;
    char        pages[BOARD_PAGE_MAX][PAGE_SLUG_CAP];
    int         np, i;
    sol_bool    exists = SOL_FALSE;
    if (board == 0) return;
    if (boardpage_slugify(typed, target, sizeof target) <= 1) return;  /* '/' = cancel */
    src = scene_meta_get(&st->scene, board, "active_page");
    if (!src) src = "/";
    if (strcmp(target, src) == 0) {                 /* self-link: ignore */
        printf("folder: already on %s\n", target);
        return;
    }
    np = board_pages(st, board, pages, BOARD_PAGE_MAX);
    for (i = 0; i < np; i++)
        if (strcmp(pages[i], target) == 0) { exists = SOL_TRUE; break; }
    fwd_local  = st->folder_place_has ? st->folder_place_local
                                      : board_local_frac(st, board, 0.0f, 0.6f);
    back_local = board_local_frac(st, board, -0.32f, 0.85f);  /* top-left */
    /* forward folder on the current page */
    st->selected_handle = add_folder(st, board, src, target, fwd_local);
    /* backlink on the target page — idempotent */
    if (!folder_backlink_exists(st, board, target, src))
        (void)add_folder(st, board, target, src, back_local);
    st->folder_place_has = SOL_FALSE;
    scene_save(&st->scene, "scene.stml");
    printf("folder %s -> %s%s\n", src, target, exists ? " (link to existing)" : " (new page)");
}
```

- [ ] **Step 4: Add the `d` key handler**

In `read_input`, next to the Backspace/Delete block (main.c:10809), add a board-view-only edge-detected handler. It captures the board-local placement at press, then opens the name prompt:
```c
    {
        sol_bool d_now = (sol_bool)(glfwGetKey(w, GLFW_KEY_D) == GLFW_PRESS);
        if (d_now && !st->d_was_down && st->board_view != 0) {
            vec3 bl;
            if (board_under_ray(st, pick_ray(st, w), &bl) != 0) {
                st->folder_place_local = bl;
                st->folder_place_has   = SOL_TRUE;
            } else {
                st->folder_place_has   = SOL_FALSE;
            }
            palette_prompt(&st->palette, "/folder name", create_folder_from_name);
        }
        st->d_was_down = d_now;
    }
```
Place this where other discrete keys are read (after `read_input`'s early-returns for note-edit / palette-open, so it can't fire while typing). Movement is frozen in board view, so the WASD `D` strafe (main.c:9681) is already inert here — no conflict.

- [ ] **Step 5: Run the gauntlet**

Run: `./build.sh c89check && ./build.sh && ./build.sh metal`
Expected: all three succeed.

- [ ] **Step 6: Live-verify (human) + commit**

Human check: enter board view (select a board, Enter); press `d`; type `chapter 1 notes`; a randomized book appears near the cursor. (Navigation to it lands in Task 5; for now confirm the forward book is present and the scene saves without error.) Then commit:
```bash
git add main.c
git commit -m "$(printf 'Board pages 4/9: d creates a folder (prompt, slugify, link-existing, backlink)\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 5: navigate — double-click a folder + arrow-cycle

**Files:**
- Modify: `main.c` (the board-view double-click arm + an arrow-cycle block + `cycle_page`)

- [ ] **Step 1: Add the `cycle_page` helper**

Before `read_input`, add:
```c
/* Step the focused board's active_page through its page list by `dir`
   (+1 next, -1 prev), wrapping. '/' is always reachable. */
static void cycle_page(AppState *st, int dir) {
    sol_u32     board = st->board_view;
    char        pages[BOARD_PAGE_MAX][PAGE_SLUG_CAP];
    int         np, i, cur = 0, ni;
    const char *active;
    if (board == 0) return;
    np = board_pages(st, board, pages, BOARD_PAGE_MAX);
    if (np <= 1) return;
    active = scene_meta_get(&st->scene, board, "active_page");
    if (!active) active = "/";
    for (i = 0; i < np; i++)
        if (strcmp(pages[i], active) == 0) { cur = i; break; }
    ni = (cur + dir + np) % np;
    scene_meta_set(&st->scene, board, "active_page", pages[ni]);
    scene_save(&st->scene, "scene.stml");
}
```

- [ ] **Step 2: Add the folder-navigate arm to the double-click dispatch**

In the `if (is_dbl)` block (main.c:9836), make the folder case the FIRST arm (a folder is `KIND_PLAIN`, so it falls through today). Change:
```c
                if (is_dbl) {                           /* edit a note, or create+edit on the empty board */
                    SceneObject *so = scene_get(&st->scene, st->selected_handle);
                    if (so && so->kind == KIND_NOTE) {
                        note_edit_begin(st, st->selected_handle);
                    } else if (st->selected_handle == 0) {
```
to:
```c
                if (is_dbl) {                           /* navigate a folder, edit a note, or create on the board */
                    SceneObject *so = scene_get(&st->scene, st->selected_handle);
                    if (so && object_is_folder(&st->scene, st->selected_handle)) {
                        const char *link = scene_meta_get(&st->scene, so->handle, "link");
                        if (link && link[0]) {
                            scene_meta_set(&st->scene, st->board_view, "active_page", link);
                            st->selected_handle = 0;
                            scene_save(&st->scene, "scene.stml");
                        }
                    } else if (so && so->kind == KIND_NOTE) {
                        note_edit_begin(st, st->selected_handle);
                    } else if (st->selected_handle == 0) {
```
(Leave the rest of the block — the empty-board spawn-note arm and the closing comment — unchanged.)

- [ ] **Step 3: Add the arrow-cycle block**

Next to the `d` handler (Task 4 Step 4), add:
```c
    {
        sol_bool left_now  = (sol_bool)(glfwGetKey(w, GLFW_KEY_LEFT)  == GLFW_PRESS);
        sol_bool right_now = (sol_bool)(glfwGetKey(w, GLFW_KEY_RIGHT) == GLFW_PRESS);
        if (st->board_view != 0 && st->selected_handle == 0 &&
            st->reader_state == READER_IDLE) {
            if (right_now && !st->page_next_was) cycle_page(st, +1);
            if (left_now  && !st->page_prev_was) cycle_page(st, -1);
        }
        st->page_prev_was = left_now;
        st->page_next_was = right_now;
    }
```

- [ ] **Step 4: Confirm the camera-look arrow handler is inert in board view**

Check the orbit/normal arrow-look block (main.c:~9719). If it is not already gated against board view, add `st->board_view == 0 &&` to its condition so `←/→` don't also pan the camera while cycling pages. (Board view freezes movement; verify look is likewise suppressed.)

- [ ] **Step 5: Run the gauntlet**

Run: `./build.sh c89check && ./build.sh && ./build.sh metal`
Expected: all three succeed.

- [ ] **Step 6: Live-verify (human) + commit**

Human check: in board view, double-click the folder from Task 4 → the board swaps to that page (root cards gone), and a backlink book is present; double-click the backlink → back to root. With nothing selected, `←/→` cycle through all pages and always reach `/`. Then:
```bash
git add main.c
git commit -m "$(printf 'Board pages 5/9: navigate — double-click folder + arrow-cycle pages\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 6: the page title + folder labels

**Files:**
- Modify: `main.c` (two render loops in the label section, ~main.c:14037)

- [ ] **Step 1: Add the folder-label loop**

In the render label section (right after the bookshelf-label loop, main.c:14062), add a loop that floats each folder's target path above it (mirrors the bookshelf-label math; `lh` and `uf` are in scope there):
```c
        /* folder labels: meta["link"] (the target path) floating above each
           folderbook, workspace+page filtered like everything else. */
        {
            sol_u32 fi;
            for (fi = 0; fi < state->scene.count; fi++) {
                const SceneObject *o = &state->scene.objects[fi];
                const char *lnk, *mr = o->mesh_ref;
                float lpx, nw, x0, fh;
                mat4  m;
                if (!mr || strcmp(mr, "folderbook") != 0) continue;
                if (!scene_object_active(&state->scene, o->handle)) continue;
                lnk = scene_meta_get(&state->scene, o->handle, "link");
                if (!lnk || !lnk[0]) continue;
                fh  = mesh_ref_param("folderbook", o->mesh_params, o->mesh_param_count, "h");
                lpx = 0.045f / lh;                       /* ~4.5 cm letters */
                text_measure(uf, lnk, 1.0f, &nw, (float *)0);
                x0  = -nw * lpx * 0.5f;
                m   = mat4_mul(scene_world_matrix(&state->scene, o),
                               mat4_translate(vec3_make(0.0f,
                                   fh + 2.0f * lpx * lh, 0.06f)));
                wtext_block(uf, vp, m, lnk, x0, 0.0f, lpx, 0.0f,
                            0.225f, 0.325f, 0.5f);       /* deep blue */
            }
        }
```

- [ ] **Step 2: Add the board page-title loop**

After the folder-label loop, add the board title — the active page path above the board, shown in board view always and out in the world when off-root:
```c
        /* board page title: the active_page path above a board, shown in
           board view, and in the world whenever the board is off its root. */
        {
            sol_u32 ti;
            for (ti = 0; ti < state->scene.count; ti++) {
                const SceneObject *o = &state->scene.objects[ti];
                const char *ap, *mr = o->mesh_ref;
                float lpx, nw, x0, bh;
                mat4  m;
                if (!mr || strcmp(mr, "board") != 0) continue;
                if (!scene_object_active(&state->scene, o->handle)) continue;
                ap = scene_meta_get(&state->scene, o->handle, "active_page");
                if (!ap) ap = "/";
                if (state->board_view != o->handle && strcmp(ap, "/") == 0)
                    continue;                            /* plain root board: stay clean */
                bh  = mesh_ref_param("board", o->mesh_params, o->mesh_param_count, "h");
                lpx = 0.12f / lh;                        /* ~12 cm letters */
                text_measure(uf, ap, 1.0f, &nw, (float *)0);
                x0  = -nw * lpx * 0.5f;
                m   = mat4_mul(scene_world_matrix(&state->scene, o),
                               mat4_translate(vec3_make(0.0f,
                                   bh + 0.04f + 2.0f * lpx * lh, 0.04f)));
                wtext_block(uf, vp, m, ap, x0, 0.0f, lpx, 0.0f,
                            0.20f, 0.20f, 0.24f);         /* near-black ink */
            }
        }
```

- [ ] **Step 3: Run the gauntlet**

Run: `./build.sh c89check && ./build.sh && ./build.sh metal`
Expected: all three succeed.

- [ ] **Step 4: Live-verify (human) + commit**

Human check: each folder shows its `/target` path above it; the board shows the current page path as a title in board view; out in the world a board turned to a sub-page is labeled, a plain `/` board is not. Then:
```bash
git add main.c
git commit -m "$(printf 'Board pages 6/9: page title + folder path labels (wtext)\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 7: delete a folder (orphan, never the page)

**Files:**
- Modify: `main.c` (a branch in the Backspace/Delete dispatch, ~main.c:10830)

- [ ] **Step 1: Add the folder-delete branch**

In the delete dispatch, after the `"picture"` branch (main.c:10845) and before the `KIND_NOTE` branch, add:
```c
            } else if (o && o->mesh_ref != NULL &&
                       strcmp(o->mesh_ref, "folderbook") == 0) {
                /* delete ONLY this folder link. The target page and its
                   contents survive (still reachable by arrow-cycle); the
                   backlink on the other page is left intact. */
                char    akey[160];
                sol_u32 doomed = st->selected_handle;
                if (mesh_asset_key(o, akey))
                    asset_release(&g_mesh_assets, akey);
                st->selected_handle = 0;
                if (st->resize_board       == doomed) st->resize_board       = 0;
                if (st->move_board         == doomed) st->move_board         = 0;
                if (st->drag_handle        == doomed) st->drag_handle        = 0;
                if (st->drop_target_handle == doomed) st->drop_target_handle = 0;
                scene_remove(&st->scene, doomed);
                scene_save(&st->scene, "scene.stml");
                printf("deleted folder #%u — its page survives\n", (unsigned)doomed);
            }
```

- [ ] **Step 2: Run the gauntlet**

Run: `./build.sh c89check && ./build.sh && ./build.sh metal`
Expected: all three succeed.

- [ ] **Step 3: Live-verify (human) + commit**

Human check: select a folder, press Delete → the book is gone; its target page still exists (cycle to it; the backlink folder is still there). Deleting the only forward link orphans the page but it's still reachable by `←/→`. Then:
```bash
git add main.c
git commit -m "$(printf 'Board pages 7/9: delete a folder orphans the page, never deletes it\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

**End of Milestone 1 — a fully navigable, persistent board notebook.**

---

# Milestone 2 — Drag-to-file

## Task 8: drop-target detection + indicator

**Files:**
- Modify: `main.c` (drag-update hook + the draw-loop highlight override)

- [ ] **Step 1: Add a folder-under-point helper**

Before `read_input`, add (finds the nearest folder on the board near a board-local point, within a folder-width radius):
```c
/* The folder pinned on `board` nearest board-local point `bl`, within a
   small radius; 0 if none. Used by the drag-to-file drop test. */
static sol_u32 folder_at_board_point(AppState *st, sol_u32 board, vec3 bl) {
    sol_u32 i, best = 0;
    float   bestd = 0.10f * 0.10f;   /* ~10 cm radius (board-local, squared) */
    for (i = 0; i < st->scene.count; i++) {
        SceneObject *o = &st->scene.objects[i];
        float dx, dy, dd;
        if (o->parent != board) continue;
        if (!object_is_folder(&st->scene, o->handle)) continue;
        if (!scene_object_active(&st->scene, o->handle)) continue;  /* on this page */
        dx = o->pos.x - bl.x;
        dy = o->pos.y - bl.y;
        dd = dx * dx + dy * dy;
        if (dd < bestd) { bestd = dd; best = o->handle; }
    }
    return best;
}
```

- [ ] **Step 2: Set `drop_target_handle` while dragging a note over the board**

In the board-mode branch of the carry/drag update (main.c:10099, where `board != 0` and the card is being pinned), after `o->pos = board_pin_pos(...)` (line ~10112), add:
```c
                        st->drop_target_handle =
                            (o->kind == KIND_NOTE)
                                ? folder_at_board_point(st, board, blocal) : 0;
```
And clear it when the drag leaves board mode — in the `else` (ground mode) branch (main.c:10115), at the top add:
```c
                        st->drop_target_handle = 0;
```
Also clear it on release: in the release branch (main.c:10152), set `st->drop_target_handle = 0;` right after `st->drag_handle = 0;` (line ~10239). (Task 9 reads it before clearing — order handled there.)

- [ ] **Step 3: Light the drop target in the draw loop**

In the per-object draw loop, right after the `hl = (...)` selection computation (main.c:13352), add:
```c
        if (state->drop_target_handle != 0 &&
            o->handle == state->drop_target_handle)
            hl = 1.0f;                            /* folder under a dragged card */
```
This reuses the existing `uHighlight` selection tint — no new shader.

- [ ] **Step 4: Run the gauntlet**

Run: `./build.sh c89check && ./build.sh && ./build.sh metal`
Expected: all three succeed.

- [ ] **Step 5: Live-verify (human) + commit**

Human check: in board view, drag a note over a folder → the folder lights up (gold tint); drag away → it un-lights. (The actual filing lands in Task 9.) Then:
```bash
git add main.c
git commit -m "$(printf 'Board pages 8/9: drag-to-file drop-target detection + highlight indicator\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 9: file the card on release

**Files:**
- Modify: `main.c` (the drag-release branch, ~main.c:10159)

- [ ] **Step 1: Re-tag the card's page when released over a folder**

The release branch `else if (st->drag_handle != 0 && st->drag_moved)` (main.c:10159) opens with `SceneObject *o = scene_get(&st->scene, st->drag_handle);` (line ~10160) and then runs ~68 lines of ordinary placement logic (unplaced-clear, the `KIND_FILE/KIND_FOLDER` alias/image drop, `scene_save`, `collide_rebuild`) before the next `} else if (!fp) {` arm (line ~10229).

Detect the filing FIRST with a local flag, then GUARD the existing placement body so it's skipped when filed. Immediately after `SceneObject *o = scene_get(&st->scene, st->drag_handle);` (line ~10160), insert:
```c
                sol_bool filed = SOL_FALSE;
                if (st->drop_target_handle != 0) {
                    SceneObject *fold = scene_get(&st->scene, st->drop_target_handle);
                    const char  *link = fold ? scene_meta_get(&st->scene,
                                                   st->drop_target_handle, "link")
                                             : (const char *)0;
                    if (o && o->kind == KIND_NOTE && link && link[0]) {
                        scene_meta_set(&st->scene, st->drag_handle, "page", link);
                        scene_save(&st->scene, "scene.stml");
                        printf("filed note #%u onto %s\n",
                               (unsigned)st->drag_handle, link);
                        filed = SOL_TRUE;            /* skip the ordinary placement save */
                    }
                    st->drop_target_handle = 0;
                }
                if (!filed) {
```
Then add the matching closing brace `}` at the END of the existing placement body — immediately BEFORE the `} else if (!fp) {` arm (line ~10229) — so the unplaced-clear / alias-drop / `scene_save` / `collide_rebuild` all live inside `if (!filed) { ... }`. Net effect: released-over-a-folder re-tags `page` + saves + clears `drop_target_handle` and runs none of the normal floor/board placement; released anywhere else behaves exactly as before. (`st->drag_handle = 0;` and `st->drag_moved = SOL_FALSE;` at line ~10239 still run for both paths — leave them.)

- [ ] **Step 2: Run the gauntlet**

Run: `./build.sh c89check && ./build.sh && ./build.sh metal`
Expected: all three succeed.

- [ ] **Step 3: Live-verify (human) + commit**

Human check: in board view, drag a note onto a folder and release → the note disappears from the current page (filed onto the target); navigate (double-click the folder) → the note is there on that page. Dragging a note and releasing on the empty board still just moves it (no filing). Then:
```bash
git add main.c
git commit -m "$(printf 'Board pages 9/9: file a note onto a folder by drag-release (re-tag page)\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

**End of Milestone 2.**

---

## Final verification (after all tasks)

- [ ] **Full gauntlet:** `./build.sh c89check && ./build.sh && ./build.sh metal` all green.
- [ ] **Unit suites:** `./build.sh boardpagetest && ./boardpage_test`; `./build.sh workspacetest && ./workspace_test` — both pass clean under ASan/UBSan.
- [ ] **Human live-verify the full flow** (board view): create folders (`d` + name); navigate (double-click + `←/→`); titles + labels show; add notes per page (they belong to the page); re-using a path links to the existing page with no duplicate backlink; delete a folder orphans-not-deletes; drag-file a note onto a folder; legacy boards still show one `/` page; reload restores the active page; **both backends** with no launch-time MSL break (no new shader).
- [ ] **Perf sanity:** idle CPU unchanged (the page gate is O(1) per object) — watch the HUD, no regression from the workspace-only baseline.

---

## Notes for the implementer

- **Read the spec first:** `docs/superpowers/specs/2026-06-25-board-pages-design.md`.
- **Don't reuse `KIND_FOLDER`** for folders — they are `KIND_PLAIN` + `mesh_ref="folderbook"`, deliberately, to stay clear of the filesystem-tree / `room_mirror_scan` logic (the board-picture precedent).
- **Pages are emergent** — there are no page objects. A page exists because something carries its `meta["page"]`; the backlink folder guarantees every linked page is non-empty.
- **One filter seam:** the page gate lives only in `scene_object_active`. Do not scatter page checks into individual readers; that's the workspace FILTER LAW and the whole point of Approach A.
- **`scene_meta_set` copies its strings** — passing a `scene_meta_get` pointer from the same object into `scene_meta_set` on another is fine, but never hold a `SceneObject*` across `scene_add`/`scene_meta_set` that may realloc; re-fetch by handle.
- **Folder color** is set on `o->material` exactly as `cmd_mint_codex` (main.c:7469) colors its leather — the draw loop uses `o->material` for any mesh_ref not in its override list (main.c:13362), and `folderbook` isn't, so the random color renders. Its persistence across reload follows the same path as the codex; if a codex keeps its color after reload, so will a folder. If colors are found NOT to persist, that's a pre-existing material-serialization gap shared with codices — out of scope here, flag it as a follow-up rather than special-casing folders.
