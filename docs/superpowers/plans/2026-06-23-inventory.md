# Inventory Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A personal "bag" you stow carriable objects into and take back out, browsed through a modal grid screen that renders each item as a live 3D thumbnail; pictures are reusable stamps, the bag is global.

**Architecture:** The bag is a single mesh-less **anchor** SceneObject tagged `meta["inventory"]`. Stowing re-parents an item under it; "stowed" = the anchor is an ancestor. A new scene predicate `scene_object_stowed` is folded into the existing `scene_object_active`, so every scene reader (render, pick/BVH, collision, route, editor) excludes the bag with **one edit**. The modal screen reuses the immediate-mode 2D overlay (`ui.h`); thumbnails reuse the existing forward PBR draw + the existing post/tonemap pipeline into a small RGBA8 target shown by the existing `ui_textured_quad` — **no new shader, no MSL twin**. Pure grid/pagination/hit-test math lives in a scene-free `inventory.c` (the `furniture.c` precedent).

**Tech Stack:** C89 (Dependable-C), OpenGL + Metal via the RHI seam, GLFW. Build gauntlet: `./build.sh c89check && ./build.sh debug && ./build.sh metal && ./build.sh inventorytest`.

**Spec:** `docs/superpowers/specs/2026-06-23-inventory-design.md`.

**Project laws (apply to every task):** strict C89 — declarations at the top of each block, `/* */` comments only, no `//`, no mixed declarations/statements, no VLAs, no C99 `fabsf` (use `fabs((double)x)`), `snprintf`/`strncpy`. The RHI seam is inviolable (only `rhi_gl.c`/`rhi_metal.m` touch GL/Metal). Never `git add` `NOTES.stml` or `paper-picture.png` — stage only the files named in each commit. Commits end with the `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>` line. Branch `inventory` already exists and is checked out.

---

## Task 1: Pure inventory layout math (`inventory.c`)

A scene-free module: page counts, the grid cell rectangles, click hit-testing, and the page-arrow rects. Pixel coordinates are framebuffer pixels, origin top-left, y-down (the `ui.h` convention).

**Files:**
- Create: `inventory.h`
- Create: `inventory.c`
- Create: `inventory_test.c`
- Modify: `build.sh` (add the `inventorytest` target; add `inventory.c` to the four build source lists)

- [ ] **Step 1: Write the header**

Create `inventory.h`:

```c
/* inventory.h — the inventory grid's pure layout math. SCENE-FREE: page
   counts, cell rectangles, click hit-testing, the page-arrow rects. No GL,
   no scene graph — main.c owns the bag, the items, and the drawing. All
   coordinates are FRAMEBUFFER PIXELS, origin top-left, y-down (the ui.h
   convention). */
#ifndef SOL_INVENTORY_H
#define SOL_INVENTORY_H

#define INV_COLS     4
#define INV_ROWS     3
#define INV_PER_PAGE (INV_COLS * INV_ROWS)

/* number of pages for n items (always at least 1). */
int inv_page_count(int n_items, int per_page);

/* clamp a page index into [0, page_count-1]. */
int inv_clamp_page(int page, int n_items, int per_page);

/* the pixel rect of grid cell `slot` (0..INV_PER_PAGE-1) on a screen of
   sw x sh, laid out cols x rows. Fills x,y,w,h. Out-of-range slot -> a zero
   rect. */
void inv_cell_rect(int slot, int cols, int rows, int sw, int sh,
                   float *x, float *y, float *w, float *h);

/* which slot (0..cols*rows-1) does pixel (px,py) fall in? -1 if none (a gap
   or a margin). The caller maps slot -> item via page*per_page + slot. */
int inv_hit_slot(float px, float py, int cols, int rows, int sw, int sh);

/* the previous / next page-arrow rects (for click hit-testing). */
void inv_prev_rect(int sw, int sh, float *x, float *y, float *w, float *h);
void inv_next_rect(int sw, int sh, float *x, float *y, float *w, float *h);

#endif /* SOL_INVENTORY_H */
```

- [ ] **Step 2: Write the failing test**

Create `inventory_test.c`:

```c
#include "inventory.h"
#include <stdio.h>
#include <math.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
    /* page counts: empty still shows one page; exact fills don't spill. */
    CHECK(inv_page_count(0, 12)  == 1);
    CHECK(inv_page_count(12, 12) == 1);
    CHECK(inv_page_count(13, 12) == 2);
    CHECK(inv_page_count(24, 12) == 2);
    CHECK(inv_page_count(25, 12) == 3);

    /* clamp into [0, pages-1]. */
    CHECK(inv_clamp_page(5,  13, 12) == 1);   /* 2 pages -> max index 1 */
    CHECK(inv_clamp_page(-1, 13, 12) == 0);
    CHECK(inv_clamp_page(0,   0, 12) == 0);

    /* cell rects: grid order, on-screen, non-overlapping. */
    {
        float x0,y0,w0,h0, x3,y3,w3,h3, x4,y4,w4,h4;
        inv_cell_rect(0, INV_COLS, INV_ROWS, 1920, 1080, &x0,&y0,&w0,&h0);
        inv_cell_rect(3, INV_COLS, INV_ROWS, 1920, 1080, &x3,&y3,&w3,&h3);
        inv_cell_rect(4, INV_COLS, INV_ROWS, 1920, 1080, &x4,&y4,&w4,&h4);
        CHECK(w0 > 0.0f && h0 > 0.0f);
        CHECK(x0 >= 0.0f && y0 >= 0.0f && x0 + w0 <= 1920.0f && y0 + h0 <= 1080.0f);
        CHECK(x3 > x0 && fabs((double)(y3 - y0)) < 1e-3);   /* slot 3 = same row, further right */
        CHECK(y4 > y0 && fabs((double)(x4 - x0)) < 1e-3);   /* slot 4 = next row, back to left col */
        CHECK(x0 + w0 <= x3 + 1e-3f);                       /* no horizontal overlap within a row */
    }

    /* hit-test: a point at a cell centre returns that slot; a margin point misses. */
    {
        float x,y,w,h;
        int s;
        inv_cell_rect(5, INV_COLS, INV_ROWS, 1920, 1080, &x,&y,&w,&h);
        s = inv_hit_slot(x + w*0.5f, y + h*0.5f, INV_COLS, INV_ROWS, 1920, 1080);
        CHECK(s == 5);
        CHECK(inv_hit_slot(1.0f, 1.0f, INV_COLS, INV_ROWS, 1920, 1080) == -1);  /* top-left margin */
    }

    /* page arrows live on screen and don't overlap each other. */
    {
        float px,py,pw,ph, nx,ny,nw,nh;
        inv_prev_rect(1920, 1080, &px,&py,&pw,&ph);
        inv_next_rect(1920, 1080, &nx,&ny,&nw,&nh);
        CHECK(pw > 0.0f && ph > 0.0f && nw > 0.0f && nh > 0.0f);
        CHECK(px + pw <= 1920.0f && nx + nw <= 1920.0f);
        CHECK(px + pw <= nx + 1e-3f);   /* prev is left of next */
    }

    if (fails == 0) printf("inventory_test: OK\n");
    return fails ? 1 : 0;
}
```

- [ ] **Step 3: Add the build target and source-list entries**

In `build.sh`, add a new mode block (place it right after the `furnituretest` block, near line 145):

```sh
# inventorytest: the inventory grid math (scene-free). Links nothing but libc.
if [ "$MODE" = "inventorytest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        inventory.c inventory_test.c \
        -o inventory_test
    echo "built ./inventory_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi
```

Then add `inventory.c` to the end of each of the four real-build source lists (every line that currently ends with `... workspace.c furniture.c`): the c89check list (line ~16), the metal list (~320), the asan list (~336), and the debug/default list (~351). Each becomes `... workspace.c furniture.c inventory.c`.

- [ ] **Step 4: Run the test to verify it fails (link error)**

Run: `./build.sh inventorytest`
Expected: FAIL — `inventory.c` has no function bodies yet, so the link fails with undefined symbols (`inv_page_count`, etc.).

- [ ] **Step 5: Implement `inventory.c`**

Create `inventory.c`:

```c
/* inventory.c — see inventory.h. Pure layout math, framebuffer pixels. */
#include "inventory.h"

/* the grid occupies a centred region inset from the screen edges; cells are
   evenly spaced with a small gap. These fractions are the only "look" knobs. */
#define INV_MX_FRAC   0.12f   /* left/right outer margin, fraction of width  */
#define INV_MY_FRAC   0.14f   /* top/bottom outer margin, fraction of height */
#define INV_GAP_FRAC  0.015f  /* gap between cells, fraction of width        */

int inv_page_count(int n_items, int per_page) {
    if (per_page < 1) per_page = 1;
    if (n_items <= 0) return 1;
    return (n_items + per_page - 1) / per_page;
}

int inv_clamp_page(int page, int n_items, int per_page) {
    int last = inv_page_count(n_items, per_page) - 1;
    if (page < 0)    page = 0;
    if (page > last) page = last;
    return page;
}

void inv_cell_rect(int slot, int cols, int rows, int sw, int sh,
                   float *x, float *y, float *w, float *h) {
    float fw = (float)sw, fh = (float)sh;
    float mx = fw * INV_MX_FRAC, my = fh * INV_MY_FRAC;
    float gap = fw * INV_GAP_FRAC;
    float gw = fw - 2.0f * mx, gh = fh - 2.0f * my;
    float cw = (cols > 0) ? (gw - (float)(cols - 1) * gap) / (float)cols : gw;
    float ch = (rows > 0) ? (gh - (float)(rows - 1) * gap) / (float)rows : gh;
    int   col, row;
    if (slot < 0 || cols < 1 || rows < 1 || slot >= cols * rows) {
        *x = *y = *w = *h = 0.0f;
        return;
    }
    col = slot % cols;
    row = slot / cols;
    *x = mx + (float)col * (cw + gap);
    *y = my + (float)row * (ch + gap);
    *w = cw;
    *h = ch;
}

int inv_hit_slot(float px, float py, int cols, int rows, int sw, int sh) {
    int slot, n = cols * rows;
    for (slot = 0; slot < n; slot++) {
        float x, y, w, h;
        inv_cell_rect(slot, cols, rows, sw, sh, &x, &y, &w, &h);
        if (px >= x && px <= x + w && py >= y && py <= y + h)
            return slot;
    }
    return -1;
}

void inv_prev_rect(int sw, int sh, float *x, float *y, float *w, float *h) {
    float fw = (float)sw, fh = (float)sh;
    *w = fw * 0.04f; *h = fh * 0.04f;
    *x = fw * 0.42f - *w * 0.5f;
    *y = fh * 0.93f - *h * 0.5f;
}

void inv_next_rect(int sw, int sh, float *x, float *y, float *w, float *h) {
    float fw = (float)sw, fh = (float)sh;
    *w = fw * 0.04f; *h = fh * 0.04f;
    *x = fw * 0.58f - *w * 0.5f;
    *y = fh * 0.93f - *h * 0.5f;
}
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `./build.sh inventorytest && ./inventory_test`
Expected: `inventory_test: OK` and no sanitizer output.

- [ ] **Step 7: Verify C89 + both backends still build**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: `c89check: PASS` and both binaries build (now linking `inventory.c`, which has no callers yet — that is fine).

- [ ] **Step 8: Commit**

```bash
git add inventory.h inventory.c inventory_test.c build.sh
git commit -m "$(cat <<'EOF'
inventory: pure grid layout math (inventory.c) + inventorytest

Page counts, cell rects, click hit-testing, page-arrow rects. Scene-free,
unit-tested. Wired into the build (inventorytest + the four source lists).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: The bag visibility predicate (`scene_object_stowed`)

Define "stowed" = the inventory anchor (any ancestor tagged `meta["inventory"]`) is above this object, and fold it into `scene_object_active` so every reader excludes the bag through the predicate they already call.

**Files:**
- Modify: `workspace.h` (declare `scene_object_stowed`)
- Modify: `workspace.c:8-25` (define it; call it from `scene_object_active`)
- Modify: `workspace_test.c` (unit-test it)

- [ ] **Step 1: Write the failing test**

In `workspace_test.c`, find `main()` and add this block just before the final `printf`/return (build a scene by hand; reuse the includes already at the top of the file — it already includes `scene.h`/`workspace.h`):

```c
    /* scene_object_stowed: true under an anchor tagged meta["inventory"]. */
    {
        Scene s;
        sol_u32 bag, card, room, shelf_card;
        Mesh    empty;
        vec3    z = vec3_make(0.0f, 0.0f, 0.0f);
        quat    q = quat_identity();
        vec3    one = vec3_make(1.0f, 1.0f, 1.0f);
        memset(&empty, 0, sizeof empty);
        scene_init(&s);
        bag  = scene_add(&s, 0, empty, z, q, one);
        scene_meta_set(&s, bag, "inventory", "1");
        card = scene_add(&s, bag, empty, z, q, one);          /* in the bag */
        room = scene_add(&s, 0, empty, z, q, one);            /* loose in the world */
        shelf_card = scene_add(&s, room, empty, z, q, one);   /* parented, not stowed */
        CHECK(scene_object_stowed(&s, card) == SOL_TRUE);
        CHECK(scene_object_stowed(&s, bag)  == SOL_TRUE);     /* the anchor itself counts */
        CHECK(scene_object_stowed(&s, room) == SOL_FALSE);
        CHECK(scene_object_stowed(&s, shelf_card) == SOL_FALSE);
        /* and the fold-in: a stowed object is never "active" */
        CHECK(scene_object_active(&s, card) == SOL_FALSE);
        CHECK(scene_object_active(&s, room) == SOL_TRUE);
        scene_free(&s);
    }
```

(If `workspace_test.c` lacks `#include <string.h>`, add it. Use the scene init/free function names already used elsewhere in that file — if they differ from `scene_init`/`scene_free`, match the file.)

- [ ] **Step 2: Run the test to verify it fails**

Run: `./build.sh workspacetest && ./workspace_test`
Expected: FAIL — `scene_object_stowed` is undefined (link error), or if the compile reaches the asserts they fail.

- [ ] **Step 3: Declare the predicate**

In `workspace.h`, after the `scene_object_active` declaration (line ~15), add:

```c
/* true if `handle` (or any ancestor) is an inventory anchor (meta["inventory"]):
   the object sits in the bag and is filtered out of every scene reader. */
sol_bool scene_object_stowed(Scene *s, sol_u32 handle);
```

- [ ] **Step 4: Implement it and fold it into `scene_object_active`**

In `workspace.c`, add the definition (place it just above `scene_object_active`):

```c
sol_bool scene_object_stowed(Scene *s, sol_u32 handle) {
    sol_u32 h = handle;
    int guard = 0;
    while (h != 0 && guard++ < 4096) {        /* walk parents to the root */
        SceneObject *o = scene_get(s, h);
        if (!o) break;
        if (scene_meta_get(s, h, "inventory")) return SOL_TRUE;
        h = o->parent;
    }
    return SOL_FALSE;
}
```

Then change `scene_object_active` (currently lines 22-24) to exclude the bag first:

```c
sol_bool scene_object_active(Scene *s, sol_u32 handle) {
    if (scene_object_stowed(s, handle)) return SOL_FALSE;   /* in the bag */
    if (s->active_ws[0] == '\0') return SOL_TRUE;
    return (sol_bool)(strcmp(workspace_of(s, handle), s->active_ws) == 0);
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `./build.sh workspacetest && ./workspace_test`
Expected: the workspace test prints OK with no sanitizer output.

- [ ] **Step 6: Verify the whole gauntlet**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal && ./build.sh collidetest && ./build.sh routetest`
Expected: all pass. (`collide.c`/`route.c` link `workspace.c`; nothing is tagged `inventory` yet, so behavior is unchanged.)

- [ ] **Step 7: Commit**

```bash
git add workspace.h workspace.c workspace_test.c
git commit -m "$(cat <<'EOF'
inventory: scene_object_stowed, folded into scene_object_active

A stowed object (under a meta["inventory"] anchor) is excluded from every
scene reader through the predicate they already call — render, BVH/pick,
collision, route, editor. No-op until the bag exists. Unit-tested.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: The working bag — stow, take, and the modal screen (placeholder tiles)

The full interactive bag: AppState fields, the anchor helper, stow (`Enter` while carrying), take (click a tile — unique items leave, pictures clone), the `i` rebind, the cursor release, the input gate, and the modal overlay drawn with **placeholder colored tiles** (real thumbnails arrive in Task 4). This task is verified by the build gauntlet plus a manual GUI check (subagents can't GUI-test; a human live-verifies).

**Files:**
- Modify: `main.c` (AppState fields; the anchor/stow/take helpers; the `i` rebind in `g_commands`; `on_key`; `read_input`; the overlay draw)

- [ ] **Step 1: Add the include and AppState fields**

At the top of `main.c` with the other local headers (near `#include "furniture.h"`), add:

```c
#include "inventory.h"
```

And near the top-of-file `#define`s, add the bag's collect cap (also the size of the per-frame `items[]` buffers and the thumbnail cache in Task 4):

```c
#define INV_THUMB_CAP 64   /* max items the bag tracks / caches at once */
```

In the `AppState` struct, near the carry fields (around `main.c:2651-2699`), add:

```c
    /* inventory (the bag): a mesh-less anchor tagged meta["inventory"]; items
       re-parented under it are "stowed" (scene_object_stowed hides them). */
    sol_u32 inv_anchor;     /* cached anchor handle; 0 = not created yet     */
    sol_bool inv_open;      /* the modal screen is up                        */
    sol_bool inv_was_open;  /* edge-detect for the cursor release            */
    int      inv_page;      /* current page in the grid                      */
```

- [ ] **Step 2: Add the anchor + collect helpers**

Place these `static` helpers in `main.c` **above** `cmd_carry_toggle` (anywhere after `scene_*`/`codex_cover_child` are declared — e.g. just before `cmd_carry_toggle` at ~7033). `codex_cover_child` is already defined earlier in the file and is used here.

```c
/* The bag's anchor: a mesh-less object at the world root tagged
   meta["inventory"]. Found by its tag (survives reload) or created on demand.
   NOTE: creating it calls scene_add, which can realloc s->objects — callers
   must re-fetch any SceneObject* AFTER calling this. */
static sol_u32 inventory_anchor(AppState *st) {
    Mesh    empty;
    vec3    z   = vec3_make(0.0f, 0.0f, 0.0f);
    quat    q   = quat_identity();
    vec3    one = vec3_make(1.0f, 1.0f, 1.0f);
    int     i;
    if (st->inv_anchor != 0 && scene_get(&st->scene, st->inv_anchor) != 0)
        return st->inv_anchor;
    for (i = 0; i < (int)st->scene.count; i++) {           /* find by tag */
        sol_u32 h = st->scene.objects[i].handle;
        if (scene_meta_get(&st->scene, h, "inventory")) {
            st->inv_anchor = h;
            return h;
        }
    }
    memset(&empty, 0, sizeof empty);                        /* else create one */
    st->inv_anchor = scene_add(&st->scene, 0, empty, z, q, one);
    scene_meta_set(&st->scene, st->inv_anchor, "inventory", "1");
    return st->inv_anchor;
}

/* Gather the bag's direct children (one entry per stowed item — a card, a
   note, a picture, or a codex anchor). Returns the count (<= cap). */
static int inventory_collect(AppState *st, sol_u32 *out, int cap) {
    int i, n = 0;
    sol_u32 anchor = (st->inv_anchor != 0 &&
                      scene_get(&st->scene, st->inv_anchor) != 0)
                     ? st->inv_anchor : 0;
    if (anchor == 0) return 0;
    for (i = 0; i < (int)st->scene.count && n < cap; i++) {
        if (st->scene.objects[i].parent == anchor)
            out[n++] = st->scene.objects[i].handle;
    }
    return n;
}

/* The "world changed" rebuild after a stow/take: meshes, materials, colliders,
   pick BVH, edges, and a save. (Mirrors the load tail + the descend path.) */
static void inventory_commit(AppState *st) {
    scene_resolve_meshes(&st->scene);
    apply_kind_materials(&st->scene);
    collide_rebuild(&st->colliders, &st->scene);
    bvh_refresh(st);
    arrows_rebuild(st);
    connections_rebuild(st);
    scene_save(&st->scene, "scene.stml");
}
```

- [ ] **Step 3: Add the stow and take helpers**

Place these `static` helpers in `main.c` immediately after the helpers from Step 2.

```c
/* Stow the carried item: re-parent it under the bag anchor and drop the carry
   state. The item becomes hidden (scene_object_stowed) on the next frame. */
static void inventory_stow(AppState *st) {
    sol_u32      item = st->carried, anchor;
    SceneObject *o;
    if (item == 0) return;
    anchor = inventory_anchor(st);          /* may scene_add -> re-fetch below */
    o = scene_get(&st->scene, item);
    if (!o) { st->carried = 0; return; }
    o->parent = anchor;
    o->pos    = vec3_make(0.0f, 0.0f, 0.0f);
    o->rot    = quat_identity();
    st->carried     = 0;
    st->plant_aim   = SOL_FALSE;
    st->file_aim    = SOL_FALSE;
    st->picture_aim = SOL_FALSE;
    inventory_commit(st);
    printf("stowed an item\n");
}

/* Is this item a reusable stamp (taking it leaves the original in the bag)? */
static sol_bool inventory_is_stamp(const SceneObject *o) {
    return (sol_bool)(o && o->mesh_ref && strcmp(o->mesh_ref, "picture") == 0);
}

/* Take an item from the bag into the hands. A stamp (picture) is CLONED — the
   original stays stowed; a unique item is re-parented to the world. Either way
   the result becomes the carried object. Closes the screen. */
static void inventory_take(AppState *st, sol_u32 item) {
    SceneObject *o = scene_get(&st->scene, item);
    vec3         w;
    if (!o) return;
    w = carry_place_point(st);              /* a point in front of the camera */
    if (inventory_is_stamp(o)) {
        /* clone like the picture-hang path (main.c:7073): mesh_ref + params +
           content, then inventory_commit's resolve builds the mesh + albedo.
           Capture the heap path + params BEFORE scene_add invalidates o. */
        char    *path = o->content;            /* heap ptr survives scene_add */
        float    params[MESH_REF_MAX_PARAMS];
        int      npar = o->mesh_param_count, k;
        Mesh     empty;
        vec3     one  = vec3_make(1.0f, 1.0f, 1.0f);
        sol_u32  clone;
        for (k = 0; k < npar && k < MESH_REF_MAX_PARAMS; k++) params[k] = o->mesh_params[k];
        memset(&empty, 0, sizeof empty);
        clone = scene_add(&st->scene, 0, empty, w, quat_identity(), one);  /* o now stale */
        scene_mesh_ref_set(&st->scene, clone, "picture");
        if (npar > 0) scene_mesh_params_set(&st->scene, clone, params, npar);
        if (path) scene_content_set(&st->scene, clone, path);
        st->carried           = clone;
        st->carry_origin      = w;
        st->carry_prev_parent = 0;
        st->carry_prev_rot    = quat_identity();
    } else {
        o->parent = 0;
        o->pos    = w;
        st->carried           = item;
        st->carry_origin      = w;
        st->carry_prev_parent = 0;
        st->carry_prev_rot    = o->rot;
    }
    st->inv_open = SOL_FALSE;
    inventory_commit(st);
    printf("took an item from the bag\n");
}
```

Notes for the implementer: `MESH_REF_MAX_PARAMS`, `scene_mesh_ref_set`, `scene_mesh_params_set`, `scene_content_set`, `carry_place_point` all already exist and are used by the picture-hang path in `cmd_carry_toggle` (main.c:7054-7083) — the stamp clone matches that path's call shapes exactly. `inventory_commit` runs `scene_resolve_meshes` (builds the picture mesh + loads its albedo from `content`) and `apply_kind_materials` (skips KIND_PLAIN, so the loaded image survives) — so the clone needs no manual material/kind/tex_ref copying; the default `scene_add` kind (KIND_PLAIN) is what a picture wants. Only pictures are stamps (`inventory_is_stamp`); other kinds take the unique branch.

- [ ] **Step 4: Rebind `i` and register the Inventory command**

In `g_commands[]`, change the irradiance row (main.c:7797) from a key binding to palette-only, and add an Inventory row. Replace:

```c
    { "Toggle irradiance view",      "I", GLFW_KEY_I, cmd_toggle_irradiance, can_toggle_irradiance, SOL_FALSE },
```

with:

```c
    { "Toggle irradiance view",      NULL, 0,         cmd_toggle_irradiance, can_toggle_irradiance, SOL_FALSE },
    { "Inventory",                   "I",  0,         cmd_inventory_open,    NULL,                  SOL_FALSE },
```

Both rows now have `key == 0` (palette-only dispatch); `i` is freed for `on_key` to own. Add the command's run function just above the `g_commands[]` array definition:

```c
/* I / palette: open the inventory screen. */
static void cmd_inventory_open(AppState *st) {
    st->inv_page = 0;
    st->inv_open = SOL_TRUE;
}
```

(`cmd_inventory_open` must be declared/defined before `g_commands[]`. If `g_commands[]` sits above these helpers, add a forward declaration `static void cmd_inventory_open(AppState *st);` near the other forward decls — match how the file already orders command funcs vs. the table.)

- [ ] **Step 5: Wire `on_key` — open, close, page, and Enter-to-stow**

In `on_key` (main.c:12175), add two blocks.

(a) Enter-to-stow — add right after the `,`/`.` carried-rotate block (~main.c:12215), before the `':'` palette-open block:

```c
    /* Enter while carrying = stow the held item into the inventory bag. */
    if (action == GLFW_PRESS && (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER)
        && st->carried != 0 && !st->place_active && !st->editor.active) {
        inventory_stow(st);
        return;
    }
```

(b) The inventory modal — add right after the palette modal block (after main.c:12195, the `if (st->palette.open) { ... return; }`):

```c
    /* The inventory screen owns the keyboard while open. */
    if (st->inv_open) {
        if (action == GLFW_PRESS) {
            sol_u32 items[INV_THUMB_CAP];
            int     n = inventory_collect(st, items, INV_THUMB_CAP);
            if (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_I) st->inv_open = SOL_FALSE;
            else if (key == GLFW_KEY_LEFT)
                st->inv_page = inv_clamp_page(st->inv_page - 1, n, INV_PER_PAGE);
            else if (key == GLFW_KEY_RIGHT)
                st->inv_page = inv_clamp_page(st->inv_page + 1, n, INV_PER_PAGE);
        }
        return;
    }

    /* 'i' opens the inventory when nothing else owns the keyboard. */
    if (action == GLFW_PRESS && key == GLFW_KEY_I && st->carried == 0 &&
        st->edit_handle == 0 && st->reader_state == READER_IDLE &&
        !st->place_active && !st->palette.open && !st->editor.active) {
        cmd_inventory_open(st);
        return;
    }
```

`INV_THUMB_CAP` is the `#define` added in Step 1. `st->reader_state`, `st->place_active`, `st->edit_handle`, and `READER_IDLE` already exist and gate the other modal owners.

- [ ] **Step 6: Wire `read_input` — cursor release + click-to-take**

(a) Cursor edge — add at the very top of `read_input` (main.c:7825, after the local declarations, BEFORE the `if (st->edit_handle ... ) { ... return; }` gate so it runs every frame):

```c
    /* inventory: release the cursor for clicking on open, restore on close. */
    if (st->inv_open && !st->inv_was_open) {
        glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        st->mouse_skip = 2;
    } else if (!st->inv_open && st->inv_was_open && !st->editor.active) {
        glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        st->mouse_skip = 2;
    }
    st->inv_was_open = st->inv_open;
```

(b) Suppress movement + handle clicks — extend the modal gate. Change the gate condition (main.c:7834) from:

```c
    if (st->edit_handle != 0 || st->palette.open || reader_is_editing(st)) {
```

to:

```c
    if (st->edit_handle != 0 || st->palette.open || reader_is_editing(st) || st->inv_open) {
```

and inside that block, alongside the existing `if (st->edit_handle != 0) {...} else if (reader_is_editing(st)) {...}` chain, add a branch for the inventory (it click-tests page arrows first, then a cell):

```c
        } else if (st->inv_open) {
            sol_bool lmb = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            if (lmb && !st->lmb_was_down) {
                int   ww, wh;
                float scale, px, py, rx, ry, rw, rh;
                sol_u32 items[INV_THUMB_CAP];
                int   n = inventory_collect(st, items, INV_THUMB_CAP);
                int   slot;
                glfwGetWindowSize(w, &ww, &wh);
                scale = (ww > 0) ? (float)st->fb_width / (float)ww : 1.0f;
                px = (float)mx * scale;
                py = (float)my * scale;
                inv_prev_rect(st->fb_width, st->fb_height, &rx, &ry, &rw, &rh);
                if (px >= rx && px <= rx + rw && py >= ry && py <= ry + rh) {
                    st->inv_page = inv_clamp_page(st->inv_page - 1, n, INV_PER_PAGE);
                } else {
                    inv_next_rect(st->fb_width, st->fb_height, &rx, &ry, &rw, &rh);
                    if (px >= rx && px <= rx + rw && py >= ry && py <= ry + rh) {
                        st->inv_page = inv_clamp_page(st->inv_page + 1, n, INV_PER_PAGE);
                    } else {
                        slot = inv_hit_slot(px, py, INV_COLS, INV_ROWS,
                                            st->fb_width, st->fb_height);
                        if (slot >= 0) {
                            int idx = st->inv_page * INV_PER_PAGE + slot;
                            if (idx < n) inventory_take(st, items[idx]);
                        }
                    }
                }
            }
            st->lmb_was_down = lmb;
        }
```

The block above must slot into the existing `if/else if` chain inside the gate (the chain currently has `if (st->edit_handle != 0)` then `else if (reader_is_editing(st))`). Add this `else if (st->inv_open)` as a further branch. `mx`/`my` are already fetched by `glfwGetCursorPos` at the top of the gate. `st->lmb_was_down` and `st->fb_width`/`st->fb_height` already exist.

- [ ] **Step 7: Draw the modal overlay (placeholder tiles)**

Add the overlay function above the frame's UI phase (place it near `editor_draw_overlay`, main.c:10792). It draws a dim backdrop, colored tiles per item, name labels, and the page indicator + arrows. Real thumbnails replace the colored tile in Task 4.

Text is scaled by the UI scale `us = fb_height / 1080` (the established overlay idiom, main.c:11875); `ts = 0.40f * us` is a readable label size and `font_ascent(font) * ts` converts a top edge to the baseline. `font_ascent` is the only font metric used (font.h:44).

```c
/* a stable-ish tile color from a kind, so the placeholder grid is legible. */
static void inv_kind_color(const SceneObject *o, sol_bool is_codex,
                           float *r, float *g, float *b) {
    const char *mr = o && o->mesh_ref ? o->mesh_ref : "";
    *r = 0.35f; *g = 0.38f; *b = 0.45f;                 /* default slate */
    if (is_codex) { *r = 0.45f; *g = 0.32f; *b = 0.22f; }                    /* book brown */
    else if (strcmp(mr, "picture") == 0) { *r = 0.25f; *g = 0.40f; *b = 0.50f; }
    else if (o && o->kind == KIND_NOTE)  { *r = 0.55f; *g = 0.52f; *b = 0.30f; }
}

static void inventory_draw_overlay(AppState *st) {
    sol_u32 items[INV_THUMB_CAP];
    int     n, pages, slot;
    float   sw = (float)st->fb_width, sh = (float)st->fb_height;
    float   us = sh / 1080.0f;
    char    pagebuf[32];
    if (!st->inv_open) return;
    n     = inventory_collect(st, items, INV_THUMB_CAP);
    st->inv_page = inv_clamp_page(st->inv_page, n, INV_PER_PAGE);
    pages = inv_page_count(n, INV_PER_PAGE);

    ui_quad(0.0f, 0.0f, sw, sh, 0.04f, 0.04f, 0.06f, 0.82f);   /* dim backdrop */

    for (slot = 0; slot < INV_PER_PAGE; slot++) {
        int   idx = st->inv_page * INV_PER_PAGE + slot;
        float x, y, w, h, cr, cg, cb;
        SceneObject *o;
        sol_bool is_codex;
        if (idx >= n) break;
        o = scene_get(&st->scene, items[idx]);
        is_codex = (sol_bool)(codex_cover_child(&st->scene, items[idx]) != 0);
        inv_cell_rect(slot, INV_COLS, INV_ROWS, st->fb_width, st->fb_height, &x, &y, &w, &h);
        inv_kind_color(o, is_codex, &cr, &cg, &cb);
        ui_quad(x, y, w, h, cr, cg, cb, 1.0f);                 /* placeholder tile */
        ui_quad_outline(x, y, w, h, 2.0f, 0.85f, 0.88f, 0.95f, 1.0f);
        {   /* the item's name, centred under the tile */
            const char *nm = o ? scene_meta_get(&st->scene, items[idx], "name") : NULL;
            const char *content = o ? o->content : NULL;
            const char *label = nm ? nm : (content ? content : "item");
            float ts = 0.40f * us;
            float lw, lh;
            text_measure(st->ui_font, label, ts, &lw, &lh);
            ui_text(st->ui_font, label, x + (w - lw) * 0.5f,
                    y + h + font_ascent(st->ui_font) * ts + 4.0f * us, ts,
                    0.92f, 0.94f, 0.98f, 1.0f);
        }
    }

    if (pages > 1) {                                           /* page arrows + label */
        float rx, ry, rw, rh, ts = 0.45f * us, lw, lh;
        inv_prev_rect(st->fb_width, st->fb_height, &rx, &ry, &rw, &rh);
        ui_quad(rx, ry, rw, rh, 0.20f, 0.22f, 0.28f, 1.0f);
        ui_text(st->ui_font, "<", rx + rw * 0.32f, ry + rh * 0.5f + font_ascent(st->ui_font) * ts * 0.5f, ts, 1.0f, 1.0f, 1.0f, 1.0f);
        inv_next_rect(st->fb_width, st->fb_height, &rx, &ry, &rw, &rh);
        ui_quad(rx, ry, rw, rh, 0.20f, 0.22f, 0.28f, 1.0f);
        ui_text(st->ui_font, ">", rx + rw * 0.32f, ry + rh * 0.5f + font_ascent(st->ui_font) * ts * 0.5f, ts, 1.0f, 1.0f, 1.0f, 1.0f);
        snprintf(pagebuf, sizeof pagebuf, "page %d / %d", st->inv_page + 1, pages);
        ts = 0.36f * us;
        text_measure(st->ui_font, pagebuf, ts, &lw, &lh);
        ui_text(st->ui_font, pagebuf, sw * 0.5f - lw * 0.5f, sh * 0.965f, ts, 0.85f, 0.88f, 0.95f, 1.0f);
    }
}
```

Then call it in the frame's UI phase, right before `palette_draw` (main.c:12088):

```c
        inventory_draw_overlay(state);
```

(It is inside the `ui_begin(...) ... ui_end()` block, so its `ui_quad`/`ui_text` calls batch correctly.)

- [ ] **Step 8: Build the gauntlet**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal && ./build.sh inventorytest`
Expected: `c89check: PASS`, both binaries build, `inventory_test: OK`. Fix any C89 / MSL-twin / declaration-order issues before proceeding. (No new shader was added, so there is no MSL twin to check — but confirm `./build.sh metal` links clean.)

- [ ] **Step 9: Manual live-verify (human)**

Run `./solarium` (or `./solarium-metal`). Verify:
1. Pick up a card with `E`, press `Enter` → it disappears from the world (stowed); the console prints `stowed an item`.
2. Press `i` → the inventory screen opens, the cursor appears, a colored tile with the item's name shows.
3. Click the tile → the screen closes and the item is back in your hands; place it with `E`.
4. Stow a *picture* card, open the bag, click it → you're holding a picture **and** the bag still shows the original (take it again to confirm it persists).
5. Stow >12 items → a second page appears; the `<`/`>` arrows and Left/Right keys change pages.
6. Press `i` or `Esc` to close → the cursor re-locks and first-person look resumes.
7. Reload (`L`) → the bag's items persist (stay hidden in the world, still listed in the screen); nothing from the bag appears loose in the world or blocks walking.
8. Hop through a portal to another workspace, open the bag → the same items are there (global bag).

- [ ] **Step 10: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
inventory: the working bag — stow, take, modal screen (placeholder tiles)

Mesh-less anchor + inventory_anchor/collect/commit; Enter stows the carried
item, clicking a tile takes it (pictures clone, the rest are unique). 'i'
rebound from the irradiance debug toggle (now palette-only) to the bag; cursor
released while open; modal input gate. Grid drawn with placeholder kind-colored
tiles + names + pagination (live 3D thumbnails follow).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Live 3D thumbnails

Replace the placeholder tiles with cached 3D renders of each item's mesh. Each thumbnail is rendered once (lazily) with the existing forward PBR draw into a small HDR target, tonemapped+encoded by the existing post pipeline (driven with neutral inputs) into a per-item RGBA8 target, and shown with the existing `ui_textured_quad`. **No new shader, no MSL twin.**

**Files:**
- Modify: `main.c` (thumbnail cache fields; init of the scratch target + two 1×1 neutral textures; the build/cache/evict functions; `inventory_ensure_thumbs` called at the top of the frame; swap the placeholder tile for the thumbnail)

- [ ] **Step 1: Add the cache fields + constants**

Near the inventory `#define`s at the top of `main.c`, add:

```c
#define INV_THUMB_SIZE 256     /* per-item thumbnail render-target edge, px */
```

(`INV_THUMB_CAP` already defined in Task 3.) In `AppState`, near the inventory fields from Task 3, add:

```c
    /* inventory thumbnails: one cached RGBA8 target per stowed item, plus a
       shared HDR scratch + two 1x1 neutral textures for the post reuse. */
    struct { sol_u32 handle; RhiRenderTarget rt; } inv_thumbs[INV_THUMB_CAP];
    int             inv_thumb_count;
    RhiRenderTarget inv_thumb_hdr;   /* shared HDR scratch (RGBA16F)          */
    RhiTexture      inv_white_tex;   /* 1x1 white: neutral AO input           */
    RhiTexture      inv_black_tex;   /* 1x1 black: neutral godray/bloom/depth  */
```

- [ ] **Step 2: Create the scratch target + neutral textures at init**

Find where the other render targets / IBL resources are created at startup (near `build_brdf_lut`/the pipeline setup, e.g. after `ensure_render_target` is first called or in the init path around main.c:9700-9860). Add, once:

```c
    {   /* inventory thumbnail scratch + neutral post inputs (created once) */
        unsigned char wpx[4] = { 255, 255, 255, 255 };
        unsigned char bpx[4] = { 0, 0, 0, 255 };
        state->inv_thumb_hdr = rhi_create_render_target(INV_THUMB_SIZE, INV_THUMB_SIZE, RHI_TEX_RGBA16F);
        state->inv_white_tex = rhi_create_texture(wpx, 1, 1, RHI_TEX_RGBA8);
        state->inv_black_tex = rhi_create_texture(bpx, 1, 1, RHI_TEX_RGBA8);
    }
```

(`rhi_create_texture(pixels, w, h, fmt)` and `rhi_create_render_target` are the exact signatures from `rhi.h:108,136`.)

- [ ] **Step 3: Add the thumbnail render + cache helpers**

Place these `static` helpers in `main.c` above `inventory_draw_overlay`. They depend on `draw_mesh`, `bind_scene_uniforms`, `state->post_pipeline`, and `state->exposure` (all defined earlier).

```c
/* The representative mesh + material for an item's thumbnail: a codex shows
   its cover; everything else shows its own mesh. Returns SOL_FALSE if the item
   has no drawable mesh. */
static sol_bool inventory_thumb_mesh(AppState *st, sol_u32 item, Mesh *mesh, Material *mat) {
    sol_u32      cov = codex_cover_child(&st->scene, item);
    SceneObject *o   = scene_get(&st->scene, cov != 0 ? cov : item);
    if (!o || o->mesh.index_count == 0) return SOL_FALSE;
    *mesh = o->mesh;
    *mat  = o->material;
    return SOL_TRUE;
}

/* Tonemap+encode the HDR scratch into `dst` using the EXISTING post pipeline,
   driven with neutral inputs (no bloom/godray/fog/grade/vignette) so the
   thumbnail is a clean sRGB-encoded RGBA8 that ui_textured_quad can show. */
static void inventory_thumb_tonemap(AppState *st, RhiRenderTarget dst) {
    mat4 ident = mat4_identity();
    rhi_begin_pass(dst, RHI_CLEAR_ALL, 0.0f, 0.0f, 0.0f, 1.0f);
    rhi_set_pipeline(st->post_pipeline);
    rhi_bind_texture(rhi_render_target_texture(st->inv_thumb_hdr), 0);
    rhi_bind_texture(st->inv_black_tex, 1);   /* uBloom (gated to 0 anyway)   */
    rhi_bind_texture(st->inv_black_tex, 3);   /* uGodray = black (added raw)  */
    rhi_bind_texture(st->inv_white_tex, 4);   /* uAO = white (multiplied)     */
    rhi_bind_texture(st->inv_black_tex, 2);   /* uDepth (fog gated off)       */
    rhi_bind_texture(st->inv_black_tex, 5);   /* uLut (gated to 0)            */
    rhi_set_uniform_int("uHdr", 0);    rhi_set_uniform_int("uBloom", 1);
    rhi_set_uniform_int("uDepth", 2);  rhi_set_uniform_int("uGodray", 3);
    rhi_set_uniform_int("uAO", 4);     rhi_set_uniform_int("uLut", 5);
    rhi_set_uniform_float("uBloomStrength", 0.0f);
    rhi_set_uniform_float("uExposure", st->exposure);
    rhi_set_uniform_float("uFogDensity", 0.0f);
    rhi_set_uniform_float("uFogFalloff", 1.0f);
    rhi_set_uniform_float("uFogHeight", 0.0f);
    rhi_set_uniform_float("uFogStrength", 0.0f);
    rhi_set_uniform_vec3 ("uAerialColor", 0.0f, 0.0f, 0.0f);
    rhi_set_uniform_vec3 ("uFogColor", 0.0f, 0.0f, 0.0f);
    rhi_set_uniform_vec3 ("uCamPos", 0.0f, 0.0f, 0.0f);
    rhi_set_uniform_mat4 ("uInvViewProj", ident.m);
    rhi_set_uniform_vec3 ("uGradeTint", 1.0f, 1.0f, 1.0f);
    rhi_set_uniform_float("uGradeContrast", 1.0f);
    rhi_set_uniform_float("uGradeSaturation", 1.0f);
    rhi_set_uniform_float("uLutMix", 0.0f);
    rhi_set_uniform_float("uVignetteStrength", 0.0f);
    rhi_set_uniform_float("uVignetteRadius", 1.0f);
    rhi_draw(0, 3);
    rhi_end_pass();
}

/* Render an item's thumbnail into `dst`: the representative mesh, framed by a
   fixed 3/4 view, into the HDR scratch, then tonemapped into dst. */
static void inventory_thumb_render(AppState *st, sol_u32 item, RhiRenderTarget dst) {
    Mesh     mesh;
    Material mat;
    mat4     model, view, proj;
    vec3     eye, ctr, ext;
    float    rad, dist;
    if (!inventory_thumb_mesh(st, item, &mesh, &mat)) return;
    /* frame the mesh by its bounds (centre + radius), 3/4 from above-right */
    ctr = vec3_scale(vec3_add(mesh.bounds.min, mesh.bounds.max), 0.5f);
    ext = vec3_sub(mesh.bounds.max, mesh.bounds.min);
    rad = 0.5f * (float)sqrt((double)vec3_dot(ext, ext));   /* no vec3_length in sol_math */
    if (rad < 1e-3f) rad = 0.5f;
    dist = rad * 2.6f;
    eye  = vec3_add(ctr, vec3_make(dist * 0.7f, dist * 0.6f, dist * 0.7f));
    model = mat4_identity();
    view  = mat4_look_at(eye, ctr, vec3_make(0.0f, 1.0f, 0.0f));
    proj  = mat4_perspective(sol_radians(35.0f), 1.0f, 0.05f, dist * 4.0f + 10.0f);
    rhi_begin_pass(st->inv_thumb_hdr, RHI_CLEAR_ALL, 0.10f, 0.12f, 0.15f, 1.0f);
    draw_mesh(st, mesh, model, view, proj, eye, 0.0f, mat);
    rhi_end_pass();
    inventory_thumb_tonemap(st, dst);
}

/* Look up (or build) the cached thumbnail target for an item. Returns {0} if
   the item has no mesh. Simple eviction: when full, recycle slot 0. */
static RhiRenderTarget inventory_thumb_get(AppState *st, sol_u32 item) {
    int i;
    for (i = 0; i < st->inv_thumb_count; i++)
        if (st->inv_thumbs[i].handle == item) return st->inv_thumbs[i].rt;
    if (st->inv_thumb_count >= INV_THUMB_CAP) {     /* evict slot 0 */
        rhi_destroy_render_target(st->inv_thumbs[0].rt);
        for (i = 1; i < st->inv_thumb_count; i++) st->inv_thumbs[i - 1] = st->inv_thumbs[i];
        st->inv_thumb_count--;
    }
    i = st->inv_thumb_count++;
    st->inv_thumbs[i].handle = item;
    st->inv_thumbs[i].rt     = rhi_create_render_target(INV_THUMB_SIZE, INV_THUMB_SIZE, RHI_TEX_RGBA8);
    inventory_thumb_render(st, item, st->inv_thumbs[i].rt);
    return st->inv_thumbs[i].rt;
}

/* Build any missing thumbnails for the items currently in the bag, and drop
   cached targets whose item left the bag. Call at the top of the frame while
   the screen is open (inside the frame's command stream — no rhi_flush). */
static void inventory_ensure_thumbs(AppState *st) {
    sol_u32 items[INV_THUMB_CAP];
    int     n = inventory_collect(st, items, INV_THUMB_CAP), i, j;
    for (i = 0; i < st->inv_thumb_count; ) {        /* evict departed items */
        sol_bool present = SOL_FALSE;
        for (j = 0; j < n; j++) if (items[j] == st->inv_thumbs[i].handle) { present = SOL_TRUE; break; }
        if (!present) {
            rhi_destroy_render_target(st->inv_thumbs[i].rt);
            for (j = i + 1; j < st->inv_thumb_count; j++) st->inv_thumbs[j - 1] = st->inv_thumbs[j];
            st->inv_thumb_count--;
        } else i++;
    }
    for (i = 0; i < n; i++) (void)inventory_thumb_get(st, items[i]);   /* build missing */
}
```

Implementer notes (all verified): `mesh.bounds` is the `Mesh` struct's `Aabb` (`bvh_refresh` uses `o->mesh.bounds` at main.c:3032; `Aabb` has `.min`/`.max`). `mat4_look_at(eye, center, up)`, `mat4_perspective(fovy_rad, aspect, near, far)`, `mat4_identity()`, `vec3_scale`, `vec3_dot`, `sol_radians` are in `sol_math.h` (lines 39, 40, 26, 20, 21). `vec3_add`/`vec3_sub` are the standard helpers used throughout main.c. There is no `vec3_length` — the radius uses `sqrt(vec3_dot(ext,ext))` (needs `<math.h>`, already included).

- [ ] **Step 4: Call `inventory_ensure_thumbs` at the top of the frame**

In the render path, at the very start of the frame's drawing (the function that begins with the shadow passes, just before `rhi_begin_pass(state->shadow_rt[0] ...)` at main.c:10879 — but after `ensure_render_target`), add:

```c
    if (state->inv_open) inventory_ensure_thumbs(state);
```

This runs inside `frame_begin`/`present`, so the offscreen passes need no `rhi_flush` (that is only for GPU work outside the frame loop). The stale-by-one-frame shadow maps it reuses never contain the origin-framed thumbnail object, so they don't matter.

- [ ] **Step 5: Swap the placeholder tile for the thumbnail**

In `inventory_draw_overlay` (Task 3, Step 7), replace the placeholder `ui_quad(x, y, w, h, cr, cg, cb, 1.0f);` line with the thumbnail draw (keep the outline + label):

```c
        ui_quad(x, y, w, h, 0.08f, 0.09f, 0.11f, 1.0f);        /* matte behind */
        {
            RhiRenderTarget rt = inventory_thumb_get(st, items[idx]);
            if (rt.id != 0) ui_textured_quad(rhi_render_target_texture(rt), x, y, w, h);
        }
```

`inv_kind_color` and its call can be removed (no longer needed), or kept as a fallback for items whose mesh failed to render. Simplest: drop `inv_kind_color` and the `cr/cg/cb` locals.

- [ ] **Step 6: Build the gauntlet**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal && ./build.sh inventorytest`
Expected: all green. On `./build.sh metal`, double-check the post pipeline's uniform names match what `inventory_thumb_tonemap` sets — the MSL post fragment shader (main.c, the `#ifdef SOL_RHI_METAL` post pair near line 1100-1170) must declare the same `uHdr/uBloom/uDepth/uGodray/uAO/uLut` samplers and the `uFog*`/`uGrade*`/`uVignette*` fields. **Grep both the GLSL and MSL post sources for every uniform name set above** (the dual-backend law: a struct/body mismatch passes the build and breaks at launch).

- [ ] **Step 7: Manual live-verify (human)**

Run `./solarium` and `./solarium-metal`. Verify:
1. Open the bag → each tile shows a recognizable 3D render of the item (a card face, a book, a picture, a note), not a flat color.
2. Take an item → its tile disappears; the remaining thumbnails are intact.
3. Stow a new item → its thumbnail appears (built lazily).
4. Reload (`L`) with items in the bag, open it → thumbnails rebuild and render correctly (nothing persisted to disk).
5. Thumbnails look reasonably lit (IBL ambient). If an item is too dark/bright or poorly framed, note it — framing (`dist` multiplier, eye angle) and exposure are the tuning knobs; adjust and rebuild.
6. Run with >12 items across pages → thumbnails on page 2 render too.

- [ ] **Step 8: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
inventory: live 3D thumbnails (cached, lazy)

Each stowed item renders once via the forward PBR draw into a small HDR
target, tonemapped by the existing post pipeline (neutral inputs, two 1x1
textures) into a per-item RGBA8 shown by ui_textured_quad. Cached by handle,
built lazily at frame top while open, evicted when the item leaves the bag.
No new shader, no MSL twin.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Finish the development branch

- [ ] **Step 1: Full gauntlet, both backends**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal && ./build.sh inventorytest && ./build.sh workspacetest && ./inventory_test && ./workspace_test`
Expected: `c89check: PASS`, both binaries build, both tests print `OK`.

- [ ] **Step 2: Confirm the working tree**

Run: `git status --short`
Expected: clean except `NOTES.stml` / `paper-picture.png` (Fran's — never staged) and any gitignored artifacts. No inventory source left unstaged.

- [ ] **Step 3: Finish the branch**

Announce: "I'm using the finishing-a-development-branch skill to complete this work." Then follow `superpowers:finishing-a-development-branch` — verify the tests, present the merge options, and on Fran's choice ff-merge `inventory` into `main` (in-place branch, no worktree) and delete the branch.

---

## Self-review notes (for the implementer)

- **Declaration order (C89):** every helper must be defined (or forward-declared) before its first use. `cmd_inventory_open` is referenced by `g_commands[]` and by `on_key`; `inventory_stow`/`inventory_take`/`inventory_collect`/`inventory_anchor` are referenced by `on_key`/`read_input`. If the command table or the input callbacks sit above these definitions, add forward declarations near the file's other `static` prototypes.
- **Never deref across `scene_add`:** `inventory_anchor` and the picture clone in `inventory_take` call `scene_add`, which can realloc `scene.objects`. Re-fetch every `SceneObject*` after such a call (the code above does — keep it that way).
- **`INV_THUMB_CAP` doubles as the collect cap:** the `items[]` buffers in `on_key`, `read_input`, and `inventory_draw_overlay` are sized `INV_THUMB_CAP`; `inventory_collect` clamps to the cap. Keep them consistent.
- **No new shader:** Task 4 reuses `state->pipeline` (via `draw_mesh`) and `state->post_pipeline`. If the post-uniform neutralization proves fragile on Metal, the fallback is a tiny dedicated thumbnail tonemap pipeline (GLSL + MSL twin) — surface that to Fran before adding it (the spec flags a twin as the escape hatch, not the default).
- **Cursor restore:** closing the bag restores `GLFW_CURSOR_DISABLED` unless the editor is active (the edge block guards on `!st->editor.active`).
```
