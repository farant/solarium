# Furniture & Filing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Place labeled bookshelves + tables via a translucent preview you aim/rotate/drop, and file file-tablets onto them by carrying (tables hold tablets flat with rotation; bookshelves auto-arrange them as upright spines; both are true containers).

**Architecture:** A transient **place mode** previews catalog meshes through the existing alpha `glass_pipeline` and `scene_add`s the chosen one on confirm. **Filing** extends the carry mechanic (the `descend_wall_aim` aim→snap→attach pattern) to re-parent a carried tablet onto the furniture under your aim — exactly as the whiteboard re-parents dragged cards (`o->parent = board`, main.c:7141). A new **scene-free** geometry module `furniture.c` owns the testable math (catalog, shelf-slot layout, table point, surface-aim); `main.c` owns the glue.

**Tech Stack:** C89 (`build.sh c89check` = `-std=c89 -pedantic-errors -Werror -Wall -Wextra`); `mesh.c` registry; command palette (`palette_prompt`); the carry system; `glass_pipeline`/`draw_glass`/`uOpacity` (P9 item 2); `wtext` SDF labels; workspace tagging (`Scene.active_ws`). No new shader → no MSL twin.

**Spec:** `docs/superpowers/specs/2026-06-20-furniture-and-filing-design.md`

---

## Conventions for every task

- **Branch:** work on `furniture-filing` off `main` (do NOT commit to `main`). Create it once before Task 1: `git switch -c furniture-filing`.
- **NEVER stage `NOTES.stml`, `paper-picture.png`, or `scene.stml`** — they're Fran's / gitignored. Stage only the exact files each task names.
- **C89 only:** declarations at the top of each block; `/* */` comments only; no `//`, no mixed declarations, no VLAs, no `for (int i...)`. Use `snprintf` not `sprintf`, `strncpy`+explicit-NUL not `strcpy` (macOS `-Werror` deprecates the unbounded ones).
- **Read-only git** for any subagent except its one task commit: `git diff`/`log`/`show` only; never `checkout`/`reset`/`stash`/`switch`/`branch`/`rebase`/amend.
- **Build gauntlet** before every commit touching compiled code: `./build.sh c89check && ./build.sh debug && ./build.sh metal`. Plus the task's headless suite.
- Commit messages end with: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

---

## File Structure

| File | Responsibility |
|------|----------------|
| `furniture.h` / `furniture.c` (NEW) | scene-free geometry: the catalog (names + cycle), `furniture_is_table`/`_is_shelf`, `furniture_shelf_slot`, `furniture_table_point`, `furniture_surface_aim`. Only `sol_math.h`/`sol_base.h`; no GL, no scene. |
| `furniture_test.c` (NEW) | the headless suite (the `descend_test` mold) |
| `mesh.c` | `"table"` + `"bookshelf"` procedural meshes + registry rows |
| `main.c` | place-mode `AppState` state; the `"Place furniture"` command; place-mode key input; ghost realize + draw; the carry furniture-aim hook; the attach/detach re-parent; the bookshelf label prompt + render |
| `build.sh` | `furnituretest` mode; `furniture.c` into the 4 main builds |
| `.gitignore` | `/furniture_test` |

**Catalog ↔ mesh constants** (used across tasks — keep identical):
- `"table"` params `{ "w", "d", "h" }` defaults `{ 1.4f, 0.9f, 0.75f }`.
- `"bookshelf"` params `{ "w", "h", "d", "shelves" }` defaults `{ 1.0f, 1.8f, 0.3f, 4.0f }`.
- `FURN_SPINE_PITCH 0.06f`, `FURN_SHELF_MARGIN 0.06f`, `FURN_TOP_T 0.05f` (table top thickness), `FURN_PANEL_T 0.04f` (shelf board/panel thickness).

---

## Task 1: `furniture.c`/`.h` skeleton — catalog + kind predicates

**Files:** Create `furniture.h`, `furniture.c`, `furniture_test.c`; modify `build.sh`, `.gitignore`.

- [ ] **Step 1: Write the failing test.** Create `furniture_test.c`:

```c
#include "furniture.h"
#include "sol_math.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
    /* catalog: two kinds, names correct, cycling wraps both ways */
    {
        CHECK(furniture_catalog_count() == 2);
        CHECK(strcmp(furniture_catalog_name(0), "table") == 0);
        CHECK(strcmp(furniture_catalog_name(1), "bookshelf") == 0);
        CHECK(furniture_catalog_name(2) == (const char *)0);   /* out of range */
        CHECK(furniture_catalog_cycle(0,  1) == 1);
        CHECK(furniture_catalog_cycle(1,  1) == 0);            /* wrap forward */
        CHECK(furniture_catalog_cycle(0, -1) == 1);            /* wrap back */
    }
    /* kind predicates */
    {
        CHECK(furniture_is_table("table"));
        CHECK(!furniture_is_table("bookshelf"));
        CHECK(furniture_is_shelf("bookshelf"));
        CHECK(!furniture_is_shelf("table"));
        CHECK(!furniture_is_table((const char *)0));
        CHECK(!furniture_is_shelf("card"));
    }
    if (fails == 0) printf("furniture_test: OK\n");
    return fails ? 1 : 0;
}
```

- [ ] **Step 2: Create the header.** `furniture.h`:

```c
/* furniture.h — placeable furniture geometry (bookshelves + tables). SCENE-FREE
   pure math: the catalog, kind predicates, shelf-slot layout, table point, and
   the surface-aim hit test. No GL, no scene graph — main.c owns the glue. */
#ifndef SOL_FURNITURE_H
#define SOL_FURNITURE_H

#include "sol_base.h"   /* sol_bool */
#include "sol_math.h"   /* vec3, Ray */

/* the placeable catalog (v1: table, bookshelf), index-ordered for cycling. */
int         furniture_catalog_count(void);
const char *furniture_catalog_name(int i);          /* NULL if out of range */
int         furniture_catalog_cycle(int i, int dir);/* +/-1, wraps */

sol_bool    furniture_is_table(const char *mesh_ref);
sol_bool    furniture_is_shelf(const char *mesh_ref);

/* the i-th spine's LOCAL position on a bookshelf (fills a shelf left-to-right,
   then the next shelf down). `params` = the bookshelf mesh_params. */
vec3 furniture_shelf_slot(const float *params, int count, int i);

/* a tablet's LOCAL resting position on a table top given a LOCAL hit point on
   the top surface (clamped inside the top; y = top height). */
vec3 furniture_table_point(const float *params, int count, vec3 local_hit);

/* camera `ray` (WORLD) vs the furniture's filing surface (table top / shelf
   front), given the furniture's world position `fpos` + yaw `fyaw` + params.
   SOL_TRUE on hit; *out_local = the hit point in furniture LOCAL space. */
sol_bool furniture_surface_aim(const char *mesh_ref, const float *params, int count,
                               vec3 fpos, float fyaw, Ray ray, vec3 *out_local);

#endif /* SOL_FURNITURE_H */
```

- [ ] **Step 3: Create the implementation (catalog + predicates only for now).** `furniture.c`:

```c
/* furniture.c — see furniture.h. */
#include "furniture.h"
#include <string.h>
#include <math.h>

static const char *const FURN_CATALOG[] = { "table", "bookshelf" };
#define FURN_COUNT ((int)(sizeof FURN_CATALOG / sizeof FURN_CATALOG[0]))

int furniture_catalog_count(void) { return FURN_COUNT; }

const char *furniture_catalog_name(int i) {
    if (i < 0 || i >= FURN_COUNT) return (const char *)0;
    return FURN_CATALOG[i];
}

int furniture_catalog_cycle(int i, int dir) {
    int n = FURN_COUNT;
    return ((i + dir) % n + n) % n;
}

sol_bool furniture_is_table(const char *mesh_ref) {
    return (sol_bool)(mesh_ref && strcmp(mesh_ref, "table") == 0);
}

sol_bool furniture_is_shelf(const char *mesh_ref) {
    return (sol_bool)(mesh_ref && strcmp(mesh_ref, "bookshelf") == 0);
}

/* furniture_shelf_slot / furniture_table_point / furniture_surface_aim land in
   Tasks 3-5; leave them out until then (their tests arrive with them). */
```

- [ ] **Step 4: Add the build mode + wiring.** In `build.sh`, after the `descendtest` block, add:

```sh
# furnituretest: the furniture geometry module (scene-free). Links mesh.c for
# the mesh-build assertion (Task 2) + sol_math.
if [ "$MODE" = "furnituretest" ]; then
    set -x
    clang -std=c89 -pedantic-errors -Werror -g -fsanitize=address,undefined \
        furniture.c furniture_test.c mesh.c flora.c rock.c gothic.c sweep.c sol_math.c \
        -o furniture_test
    echo "built ./furniture_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi
```

Also add `furniture.c` to the FOUR main-app source lists (each ends `... editor.c descend.c workspace.c`; append ` furniture.c`): the `c89check` block, the `metal` block, the `asan` block, and the default/`debug` build.

- [ ] **Step 5: Run.** `./build.sh furnituretest && ./furniture_test` → `furniture_test: OK`. Then `./build.sh c89check` → PASS.

- [ ] **Step 6: Ignore the binary + commit.** Add `/furniture_test` to `.gitignore`.

```bash
git add furniture.h furniture.c furniture_test.c build.sh .gitignore
git commit -m "$(printf 'Furniture: scene-free module skeleton + catalog\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 2: the `"table"` + `"bookshelf"` meshes

**Files:** Modify `mesh.c`; modify `furniture_test.c`.

- [ ] **Step 1: Write the failing test.** Append to `furniture_test.c`'s `main()` (before the OK line); add `#include "mesh.h"` at the top:

```c
    /* both furniture meshes build with geometry */
    {
        MeshBuilder b;
        mb_init(&b);
        CHECK(mesh_ref_build("table", (const float *)0, 0, &b) == SOL_TRUE);
        CHECK(b.vertex_count > 0 && b.index_count > 0);
        mb_free(&b);
        mb_init(&b);
        CHECK(mesh_ref_build("bookshelf", (const float *)0, 0, &b) == SOL_TRUE);
        CHECK(b.vertex_count > 0 && b.index_count > 0);
        mb_free(&b);
    }
```

- [ ] **Step 2: Run to confirm fail.** `./build.sh furnituretest && ./furniture_test` → the new CHECKs fail (`mesh_ref_build` returns `SOL_FALSE` for unknown refs).

- [ ] **Step 3: Implement the meshes.** In `mesh.c`, add two emit functions near the other `emit_*` (above the registry table), using the existing `aabb_box(b, x0,x1, y0,y1, z0,z1)` helper:

```c
/* a table: a top slab + 4 legs. params: w (width X), d (depth Z), h (height).
   origin at floor center; the top's upper face (y=h) is the filing surface. */
static void emit_table(MeshBuilder *b, const float *p) {
    float w = p[0], d = p[1], h = p[2];
    float hw = w * 0.5f, hd = d * 0.5f, tt = 0.05f, lt = 0.06f;
    float top0 = h - tt;
    aabb_box(b, -hw, hw, top0, h, -hd, hd);                       /* top slab */
    aabb_box(b, -hw,      -hw + lt, 0.0f, top0, -hd,      -hd + lt); /* legs */
    aabb_box(b,  hw - lt,  hw,      0.0f, top0, -hd,      -hd + lt);
    aabb_box(b, -hw,      -hw + lt, 0.0f, top0,  hd - lt,  hd);
    aabb_box(b,  hw - lt,  hw,      0.0f, top0,  hd - lt,  hd);
}

/* a bookshelf: two side panels + a back + `shelves` horizontal boards. params:
   w (width X), h (height Y), d (depth Z), shelves (board count). origin at floor
   center; the front opening (+Z) faces the reader. */
static void emit_bookshelf(MeshBuilder *b, const float *p) {
    float w = p[0], h = p[1], d = p[2];
    int   sh = (int)(p[3] + 0.5f), k;
    float hw = w * 0.5f, hd = d * 0.5f, pt = 0.04f;
    if (sh < 1) sh = 1;
    aabb_box(b, -hw, -hw + pt, 0.0f, h, -hd, hd);                 /* left panel  */
    aabb_box(b,  hw - pt, hw,  0.0f, h, -hd, hd);                 /* right panel */
    aabb_box(b, -hw, hw, 0.0f, h, -hd, -hd + pt);                 /* back panel  */
    aabb_box(b, -hw, hw, 0.0f, pt, -hd, hd);                      /* floor board */
    for (k = 1; k <= sh; k++) {                                   /* shelf boards */
        float y = (h - pt) * (float)k / (float)(sh + 1);
        aabb_box(b, -hw, hw, y, y + pt, -hd, hd);
    }
}
```

Add the registry rows near the `"card"`/`"board"` rows (NOT near the gothic `"portal"`):

```c
    { "table", 3, { "w", "d", "h" }, { 1.4f, 0.9f, 0.75f }, emit_table },
    { "bookshelf", 4, { "w", "h", "d", "shelves" }, { 1.0f, 1.8f, 0.3f, 4.0f }, emit_bookshelf },
```

(Like the portal "gate", neither name is in `collide_rebuild`'s dispatch → furniture is non-solid in v1, matching the spec §9.)

- [ ] **Step 4: Run.** `./build.sh furnituretest && ./furniture_test` → `furniture_test: OK`. Then the gauntlet `./build.sh c89check && ./build.sh debug && ./build.sh metal` → all clean.

- [ ] **Step 5: Commit**

```bash
git add mesh.c furniture_test.c
git commit -m "$(printf 'Furniture: table + bookshelf procedural meshes\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 3: `furniture_shelf_slot` — the auto-arrange spine layout

**Files:** Modify `furniture.c`, `furniture_test.c`.

- [ ] **Step 1: Write the failing test.** Append to `furniture_test.c` `main()`:

```c
    /* shelf slots: fill a shelf left-to-right, wrap to the next shelf down */
    {
        float p[4]; vec3 s0, s1, sN;
        p[0] = 1.0f; p[1] = 1.8f; p[2] = 0.3f; p[3] = 4.0f;   /* w h d shelves */
        s0 = furniture_shelf_slot(p, 4, 0);
        s1 = furniture_shelf_slot(p, 4, 1);
        CHECK(s1.x > s0.x);                       /* second spine is to the right */
        CHECK(fabs((double)(s1.y - s0.y)) < 1e-4);/* same shelf -> same height */
        CHECK(fabs((double)s0.x) <= 0.5);         /* inside the width */
        sN = furniture_shelf_slot(p, 4, 100);     /* far index: still finite + in-bounds-ish */
        CHECK(fabs((double)sN.x) <= 0.5 && sN.y >= 0.0f && sN.y <= 1.8f);
        /* once a shelf fills, the next index drops to a lower shelf */
        {
            int cols = (int)((1.0f - 2.0f * 0.06f) / 0.06f);   /* (w-2*margin)/pitch */
            vec3 a = furniture_shelf_slot(p, 4, 0);
            vec3 b = furniture_shelf_slot(p, 4, cols);          /* first of next shelf */
            CHECK(b.y < a.y - 1e-4f);
        }
    }
```

- [ ] **Step 2: Run to confirm fail** — link error (`furniture_shelf_slot` undefined).

- [ ] **Step 3: Implement.** Replace the placeholder comment in `furniture.c` with:

```c
#define FURN_SPINE_PITCH  0.06f
#define FURN_SHELF_MARGIN 0.06f
#define FURN_PANEL_T      0.04f

/* columns that fit across a shelf of width w */
static int furn_shelf_cols(float w) {
    int c = (int)((w - 2.0f * FURN_SHELF_MARGIN) / FURN_SPINE_PITCH);
    return c < 1 ? 1 : c;
}

vec3 furniture_shelf_slot(const float *params, int count, int i) {
    float w  = (count > 0) ? params[0] : 1.0f;
    float h  = (count > 1) ? params[1] : 1.8f;
    float d  = (count > 2) ? params[2] : 0.3f;
    int   sh = (count > 3) ? (int)(params[3] + 0.5f) : 4;
    int   cols = furn_shelf_cols(w);
    int   col, row;
    float x0 = -w * 0.5f + FURN_SHELF_MARGIN + FURN_SPINE_PITCH * 0.5f;
    vec3  s;
    if (sh < 1) sh = 1;
    if (i < 0) i = 0;
    col = i % cols;
    row = (i / cols) % sh;                 /* wrap within the shelf rows (cap) */
    s.x = x0 + (float)col * FURN_SPINE_PITCH;
    /* shelf board k=row+1 sits at this y (matches emit_bookshelf); a spine
       stands ON it, centered on its half-height of free space below the next */
    s.y = (h - FURN_PANEL_T) * (float)(sh - row) / (float)(sh + 1) + FURN_PANEL_T;
    s.z = d * 0.5f - 0.04f;                 /* just inside the front opening */
    return s;
}
```

(Top shelf = row 0 gets the HIGHEST y, so `i` fills the top shelf first then descends — `sh - row` in the height formula.)

- [ ] **Step 4: Run.** `./build.sh furnituretest && ./furniture_test` → `furniture_test: OK`. `./build.sh c89check` → PASS.

- [ ] **Step 5: Commit**

```bash
git add furniture.c furniture_test.c
git commit -m "$(printf 'Furniture: bookshelf shelf-slot auto-arrange layout\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 4: `furniture_table_point` — flat placement on a table top

**Files:** Modify `furniture.c`, `furniture_test.c`.

- [ ] **Step 1: Write the failing test.** Append to `furniture_test.c` `main()`:

```c
    /* table point: clamps inside the top, sits at the top height */
    {
        float p[3]; vec3 q;
        p[0] = 1.4f; p[1] = 0.9f; p[2] = 0.75f;   /* w d h */
        q = furniture_table_point(p, 3, vec3_make(0.3f, 0.75f, 0.2f));
        CHECK(fabs((double)(q.y - 0.75f)) < 1e-4); /* on the top surface (y=h) */
        CHECK(fabs((double)(q.x - 0.3f)) < 1e-4);  /* in-bounds: unchanged */
        q = furniture_table_point(p, 3, vec3_make(5.0f, 0.0f, 5.0f));  /* way off */
        CHECK(q.x <= 0.7f + 1e-4f && q.x >= -0.7f - 1e-4f);            /* clamped to +/-w/2 */
        CHECK(q.z <= 0.45f + 1e-4f && q.z >= -0.45f - 1e-4f);          /* clamped to +/-d/2 */
    }
```

- [ ] **Step 2: Run to confirm fail** — link error.

- [ ] **Step 3: Implement.** Add to `furniture.c`:

```c
vec3 furniture_table_point(const float *params, int count, vec3 local_hit) {
    float w  = (count > 0) ? params[0] : 1.4f;
    float d  = (count > 1) ? params[1] : 0.9f;
    float h  = (count > 2) ? params[2] : 0.75f;
    float hw = w * 0.5f - 0.05f, hd = d * 0.5f - 0.05f;   /* keep a small inset */
    vec3  q  = local_hit;
    if (hw < 0.0f) hw = 0.0f;
    if (hd < 0.0f) hd = 0.0f;
    if (q.x >  hw) q.x =  hw;  if (q.x < -hw) q.x = -hw;
    if (q.z >  hd) q.z =  hd;  if (q.z < -hd) q.z = -hd;
    q.y = h;                                              /* on the top */
    return q;
}
```

(The lying-flat ORIENTATION — pitch −90° + the carry yaw — is composed by the caller in main.c; this returns only the position.)

- [ ] **Step 4: Run.** `./build.sh furnituretest && ./furniture_test` → `furniture_test: OK`. `./build.sh c89check` → PASS.

- [ ] **Step 5: Commit**

```bash
git add furniture.c furniture_test.c
git commit -m "$(printf 'Furniture: table flat-placement point\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 5: `furniture_surface_aim` — the carry aim hit test

**Files:** Modify `furniture.c`, `furniture_test.c`.

- [ ] **Step 1: Write the failing test.** Append to `furniture_test.c` `main()`. (Confirm `Ray` has `.origin`/`.dir` vec3 fields and `ray_vs_plane` exists — `grep -n "ray_vs_plane\|} Ray" sol_math.h`. The test below builds rays by hand.)

```c
    /* surface-aim: a downward ray onto a table top hits; aiming away misses */
    {
        float p[3]; Ray ray; vec3 loc;
        p[0] = 1.4f; p[1] = 0.9f; p[2] = 0.75f;
        ray.origin = vec3_make(0.0f, 2.0f, 0.0f);          /* above a table at origin */
        ray.dir    = vec3_make(0.0f, -1.0f, 0.0f);
        CHECK(furniture_surface_aim("table", p, 3, vec3_make(0,0,0), 0.0f, ray, &loc));
        CHECK(fabs((double)(loc.y - 0.75f)) < 1e-3);       /* hit at the top height */
        ray.dir = vec3_make(0.0f, 1.0f, 0.0f);             /* aim up: miss */
        CHECK(!furniture_surface_aim("table", p, 3, vec3_make(0,0,0), 0.0f, ray, &loc));
    }
    /* surface-aim: a forward ray into a bookshelf's front face hits */
    {
        float p[4]; Ray ray; vec3 loc;
        p[0]=1.0f; p[1]=1.8f; p[2]=0.3f; p[3]=4.0f;
        ray.origin = vec3_make(0.0f, 0.9f, 2.0f);          /* in front (+Z) of a shelf */
        ray.dir    = vec3_make(0.0f, 0.0f, -1.0f);
        CHECK(furniture_surface_aim("bookshelf", p, 4, vec3_make(0,0,0), 0.0f, ray, &loc));
        ray.origin = vec3_make(0.0f, 0.9f, -2.0f);         /* behind: ray points away */
        ray.dir    = vec3_make(0.0f, 0.0f, -1.0f);
        CHECK(!furniture_surface_aim("bookshelf", p, 4, vec3_make(0,0,0), 0.0f, ray, &loc));
    }
```

- [ ] **Step 2: Run to confirm fail** — link error.

- [ ] **Step 3: Implement.** Add to `furniture.c`. The ray is transformed into furniture-local space (the furniture is pure-yaw + translate), then intersected with the table top plane (y=h) within the footprint, or the bookshelf front plane (z=+d/2) within the face:

```c
/* rotate a vector by -yaw about +Y (world->local for a yaw-only frame) */
static vec3 furn_unrotate_y(vec3 v, float yaw) {
    float c = (float)cos((double)(-yaw)), s = (float)sin((double)(-yaw));
    vec3  r;
    r.x = v.x * c - v.z * s;
    r.y = v.y;
    r.z = v.x * s + v.z * c;
    return r;
}

sol_bool furniture_surface_aim(const char *mesh_ref, const float *params, int count,
                               vec3 fpos, float fyaw, Ray ray, vec3 *out_local) {
    Ray   lr;
    float t;
    vec3  pt, pnorm, hit;
    /* world ray -> furniture local (translate then un-yaw) */
    lr.origin = furn_unrotate_y(vec3_sub(ray.origin, fpos), fyaw);
    lr.dir    = furn_unrotate_y(ray.dir, fyaw);
    if (furniture_is_table(mesh_ref)) {
        float w = (count > 0) ? params[0] : 1.4f;
        float d = (count > 1) ? params[1] : 0.9f;
        float h = (count > 2) ? params[2] : 0.75f;
        pt    = vec3_make(0.0f, h, 0.0f);
        pnorm = vec3_make(0.0f, 1.0f, 0.0f);
        if (!ray_vs_plane(lr, pt, pnorm, &t) || t <= 0.0f) return SOL_FALSE;
        hit = vec3_add(lr.origin, vec3_scale(lr.dir, t));
        if (hit.x < -w*0.5f || hit.x > w*0.5f || hit.z < -d*0.5f || hit.z > d*0.5f)
            return SOL_FALSE;
        *out_local = hit;
        return SOL_TRUE;
    }
    if (furniture_is_shelf(mesh_ref)) {
        float w = (count > 0) ? params[0] : 1.0f;
        float h = (count > 1) ? params[1] : 1.8f;
        float d = (count > 2) ? params[2] : 0.3f;
        pt    = vec3_make(0.0f, 0.0f, d * 0.5f);
        pnorm = vec3_make(0.0f, 0.0f, 1.0f);
        if (!ray_vs_plane(lr, pt, pnorm, &t) || t <= 0.0f) return SOL_FALSE;
        hit = vec3_add(lr.origin, vec3_scale(lr.dir, t));
        if (hit.x < -w*0.5f || hit.x > w*0.5f || hit.y < 0.0f || hit.y > h)
            return SOL_FALSE;
        *out_local = hit;
        return SOL_TRUE;
    }
    return SOL_FALSE;
}
```

NOTE: confirm `ray_vs_plane(Ray, vec3 point, vec3 normal, float *t)` and `vec3_sub`/`vec3_add`/`vec3_scale` signatures in `sol_math.h` (carry_place_point at main.c:6566 uses `ray_vs_plane` the same way). Adjust if a name differs.

- [ ] **Step 4: Run.** `./build.sh furnituretest && ./furniture_test` → `furniture_test: OK`. `./build.sh c89check` → PASS.

- [ ] **Step 5: Commit**

```bash
git add furniture.c furniture_test.c
git commit -m "$(printf 'Furniture: surface-aim hit test (table top / shelf front)\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 6: place mode — state, command, input, ghost realize

**Files:** Modify `main.c`.

Place mode is a transient input-owning mode. `main.c` only; gauntlet-verified.

- [ ] **Step 1: Add the state.** Add `#include "furniture.h"` to `main.c`'s includes. In the `AppState` struct (near the carry fields `plant_aim`/`carry_origin`, ~main.c:2653), add:

```c
    sol_bool    place_active;     /* place-furniture preview mode */
    int         place_index;      /* catalog index being previewed */
    float       place_yaw;        /* ghost yaw (radians) */
    Mesh        place_ghost;      /* realized ghost mesh (rebuilt on enter/cycle) */
```

Initialize them where other AppState handles are nulled at startup (`st->place_active = SOL_FALSE; st->place_index = 0; st->place_yaw = 0.0f;` and zero `place_ghost` via the struct's existing memset, or `memset(&st->place_ghost, 0, sizeof st->place_ghost);`).

- [ ] **Step 2: Add a ghost-realize helper + the command.** Near `cmd_new_root` / the other `cmd_*` (above `g_commands[]`), add:

```c
/* (re)build the translucent ghost mesh for the current catalog item. */
static void place_realize_ghost(AppState *st) {
    MeshBuilder mb;
    mesh_destroy(&st->place_ghost);
    mb_init(&mb);
    if (mesh_ref_build(furniture_catalog_name(st->place_index), (const float *)0, 0, &mb))
        st->place_ghost = mesh_from_builder(&mb);
    mb_free(&mb);
}

static void cmd_place_furniture(AppState *st) {
    st->place_active = SOL_TRUE;
    st->place_index  = 0;
    st->place_yaw    = 0.0f;
    place_realize_ghost(st);
    printf("place mode: [ ] cycle, , . rotate, Enter place, Esc cancel\n");
}
```

Register in `g_commands[]` (palette-only):

```c
    { "Place furniture",             NULL, 0, cmd_place_furniture,  NULL,                  SOL_FALSE },
```

- [ ] **Step 3: Handle place-mode keys.** In the GLFW key callback (the `static void on_key`/key-callback around main.c:10880, right AFTER the `if (st->palette.open) {...}` block and BEFORE the `:`-opens-palette block), add:

```c
    /* Place mode owns the keyboard while previewing furniture. */
    if (st->place_active) {
        if (action == GLFW_PRESS || action == GLFW_REPEAT) {
            if      (key == GLFW_KEY_ESCAPE) { st->place_active = SOL_FALSE; mesh_destroy(&st->place_ghost); }
            else if (key == GLFW_KEY_LEFT_BRACKET)  { st->place_index = furniture_catalog_cycle(st->place_index, -1); place_realize_ghost(st); }
            else if (key == GLFW_KEY_RIGHT_BRACKET) { st->place_index = furniture_catalog_cycle(st->place_index,  1); place_realize_ghost(st); }
            else if (key == GLFW_KEY_COMMA)  { st->place_yaw -= sol_radians(15.0f); }
            else if (key == GLFW_KEY_PERIOD) { st->place_yaw += sol_radians(15.0f); }
            else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) { place_confirm(st); }
        }
        return;
    }
```

`place_confirm(st)` is Task 8 — for THIS task, define a temporary stub above the callback so it links: `static void place_confirm(AppState *st) { (void)st; }` (Task 8 fills it in). (Declare a forward `static void place_confirm(AppState *st);` near the other forward decls if needed.)

- [ ] **Step 4: Avoid the `[`/`]` exposure conflict.** Do NOT add `place_active` to the movement/look gate (main.c:6952) — that gate zeroes *look* too, and place mode needs look + walk to aim the ghost. The only continuous-poll conflict is the exposure brackets: `[`/`]` adjust exposure in `read_input` (main.c:7837-7838) and the place callback also owns them for cycling. Suppress the exposure poll while placing — wrap those two lines:

```c
        if (!st->place_active) {
            if (glfwGetKey(w, GLFW_KEY_RIGHT_BRACKET) == GLFW_PRESS) st->exposure += erate;
            if (glfwGetKey(w, GLFW_KEY_LEFT_BRACKET)  == GLFW_PRESS) st->exposure -= erate;
        }
```

(`,`/`.` are not polled in `read_input`, so they need no guard. Movement + look stay live in place mode — you walk and look to position the ghost; the `[ ] , . Enter Esc` discrete actions are owned by the key callback from Step 3.)

- [ ] **Step 5: Build the gauntlet.** `./build.sh c89check && ./build.sh debug && ./build.sh metal` → all clean. (No visible ghost yet — Task 7. Here: entering place mode via `:Place furniture` prints the hint and Esc exits, without crashing.)

- [ ] **Step 6: Commit**

```bash
git add main.c
git commit -m "$(printf 'Furniture: place-mode state, command, input, ghost realize\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 7: place mode — render the translucent ghost

**Files:** Modify `main.c`.

- [ ] **Step 1: Compute the ghost transform + draw it.** In `render()`, AFTER the stained-glass sub-pass (search the `church_glass` sort loop / `draw_glass` call, ~main.c:10280) and before the decal sub-pass, add a ghost draw. It reuses `draw_glass` (translucent, `GLASS_OPACITY=0.6`) at the ground-aim point + `place_yaw`:

```c
    /* the place-mode ghost: the catalog item previewed translucent at the
       ground-aim point, spun by place_yaw. Reuses the glass (alpha) path. */
    if (state->place_active && state->place_ghost.index_count > 0) {
        vec3     gp = carry_place_point(state);          /* camera-aim ground point */
        mat4     model = mat4_mul(mat4_translate(gp),
                            quat_to_mat4(quat_from_axis_angle(
                                vec3_make(0.0f, 1.0f, 0.0f), state->place_yaw)));
        Material gm = material_default();
        gm.base_color = vec3_make(0.55f, 0.62f, 0.78f);  /* cool preview tint */
        draw_glass(state, state->place_ghost, model, view, proj, eye, gm);
    }
```

NOTE: `carry_place_point` takes `AppState*` (non-const) — `render(AppState *state)` passes `state`. Confirm `mat4_translate`, `quat_to_mat4`, `quat_from_axis_angle`, `material_default` are in scope (all used elsewhere in main.c). `draw_glass(const AppState*, Mesh, mat4 model, mat4 view, mat4 proj, vec3 eye, Material)` — match it (main.c:~9320).

- [ ] **Step 2: Build the gauntlet.** `./build.sh c89check && ./build.sh debug && ./build.sh metal` → all clean.

- [ ] **Step 3: Commit**

```bash
git add main.c
git commit -m "$(printf 'Furniture: render the translucent place-mode ghost\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 8: place mode — confirm places the furniture (+ bookshelf label)

**Files:** Modify `main.c`.

- [ ] **Step 1: Implement `place_confirm` + the label callback.** Replace the Task-6 stub with the real `place_confirm` (place it above the key callback / `g_commands[]`):

```c
/* drop the previewed furniture as a real object, tagged into the active
   workspace; a bookshelf then prompts for its label. */
static void place_confirm(AppState *st) {
    Mesh     empty;
    vec3     pos = carry_place_point(st);
    quat     rot = quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), st->place_yaw);
    const char *kind = furniture_catalog_name(st->place_index);
    sol_u32  h;
    memset(&empty, 0, sizeof empty);
    h = scene_add(&st->scene, 0, empty, pos, rot, vec3_make(1.0f, 1.0f, 1.0f));
    scene_mesh_ref_set(&st->scene, h, kind);
    scene_meta_set(&st->scene, h, "name", kind);
    scene_meta_set(&st->scene, h, "workspace",
                   st->scene.active_ws[0] ? st->scene.active_ws : "home");
    scene_resolve_meshes(&st->scene);
    apply_kind_materials(&st->scene);
    st->place_active = SOL_FALSE;
    mesh_destroy(&st->place_ghost);
    if (furniture_is_shelf(kind)) {
        st->place_label_target = h;                       /* see Step 2 */
        palette_prompt(&st->palette, "bookshelf label", place_set_label);
    } else {
        scene_save(&st->scene, "scene.stml");
    }
    printf("placed %s\n", kind);
}
```

- [ ] **Step 2: The label callback + the target field.** Add a `sol_u32 place_label_target;` field to `AppState` (near the place fields), and define the callback (matches `palette_prompt`'s `void(AppState*, const char*)`):

```c
static void place_set_label(AppState *st, const char *label) {
    if (st->place_label_target != 0 && label && label[0])
        scene_meta_set(&st->scene, st->place_label_target, "label", label);
    st->place_label_target = 0;
    scene_save(&st->scene, "scene.stml");
}
```

(Add forward decls `static void place_confirm(AppState *st);` and `static void place_set_label(AppState *st, const char *label);` near the other forward declarations so the key callback + the prompt resolve.)

- [ ] **Step 3: Build the gauntlet.** `./build.sh c89check && ./build.sh debug && ./build.sh metal` → all clean.

- [ ] **Step 4: Commit**

```bash
git add main.c
git commit -m "$(printf 'Furniture: confirm places the furniture + bookshelf label prompt\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 9: render the bookshelf label

**Files:** Modify `main.c`.

- [ ] **Step 1: Draw each bookshelf's label.** In `render()`, near the doorway-label block (search `doorway labels`, ~main.c:10202, which uses `uf`, `vp`, `lh`, `text_measure`, `wtext_block`), add a pass over active-workspace bookshelves. It mirrors the doorway-label transform but anchors at the shelf's top rail facing out (the shelf's own yaw):

```c
    /* bookshelf labels: meta["label"] across the top rail, facing the shelf's
       front (+Z local), workspace-filtered like everything else. */
    {
        sol_u32 bi;
        for (bi = 0; bi < state->scene.count; bi++) {
            const SceneObject *o = &state->scene.objects[bi];
            const char *lbl, *mr = o->mesh_ref;
            float lpx, nw, x0, h;
            mat4  m;
            if (!mr || strcmp(mr, "bookshelf") != 0) continue;
            if (!scene_object_active(&state->scene, o->handle)) continue;
            lbl = scene_meta_get(&state->scene, o->handle, "label");
            if (!lbl || !lbl[0]) continue;
            h   = mesh_ref_param("bookshelf", o->mesh_params, o->mesh_param_count, "h");
            lpx = 0.18f / lh;                          /* ~18 cm letters */
            text_measure(uf, lbl, 1.0f, &nw, (float *)0);
            x0  = -nw * lpx * 0.5f;                    /* centered on the shelf */
            m   = mat4_mul(scene_world_matrix(&state->scene, o),
                           mat4_translate(vec3_make(0.0f, h + 0.02f, 0.16f)));
            wtext_block(uf, vp, m, lbl, x0, 0.0f, lpx, 0.0f, 0.95f, 0.92f, 0.82f);
        }
    }
```

NOTE: `wtext_block(font, vp, model, text, x0, y0, px, wrap, r, g, b)` — match the doorway-label call's argument order exactly (read main.c:10226-ish). `scene_world_matrix` already bakes the shelf's yaw, so the label inherits its facing; the `+Z 0.16` offset sits it just in front of the top rail. `lh` is the font line height in scope; `uf`/`vp` are the UI font + view*proj already set up for the label block.

- [ ] **Step 2: Build the gauntlet.** `./build.sh c89check && ./build.sh debug && ./build.sh metal` → all clean.

- [ ] **Step 3: Commit**

```bash
git add main.c
git commit -m "$(printf 'Furniture: render bookshelf labels (wtext)\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 10: filing — the carry surface-aim hook

**Files:** Modify `main.c`.

- [ ] **Step 1: Add filing state.** In `AppState` (near `plant_aim`), add:

```c
    sol_bool    file_aim;        /* carried tablet is aimed at furniture this frame */
    sol_u32     file_target;     /* the furniture handle */
    vec3        file_local;      /* the tablet's resting local pos on it */
    quat        file_rot;        /* the resting local orientation */
```

Null `file_aim`/`file_target` at startup with the other handles.

- [ ] **Step 2: Hook `carry_update`.** In `carry_update` (main.c:6634), after the existing `KIND_FOLDER` wall-aim block and BEFORE the "float in front" fallback, add a tablet→furniture aim. A "tablet" here is a carried card that is NOT a folder (folders keep wall-aim): `o->mesh_ref == "card"` and `o->kind != KIND_FOLDER`. Scan active-workspace furniture, take the first surface hit:

```c
    st->file_aim = SOL_FALSE;
    if (o->mesh_ref && strcmp(o->mesh_ref, "card") == 0 && o->kind != KIND_FOLDER) {
        sol_u32 fi;
        for (fi = 0; fi < st->scene.count; fi++) {
            SceneObject *f = &st->scene.objects[fi];
            vec3  fpos, loc; float fyaw; quat fq;
            Ray   ray;
            if (!f->mesh_ref) continue;
            if (!furniture_is_table(f->mesh_ref) && !furniture_is_shelf(f->mesh_ref)) continue;
            if (!scene_object_active(&st->scene, f->handle)) continue;
            fpos = object_world_pos(&st->scene, f->handle);
            fq   = f->rot;
            fyaw = 2.0f * (float)atan2((double)fq.y, (double)fq.w);
            ray.origin = st->camera.pos;
            ray.dir    = camera_forward(&st->camera);
            if (!furniture_surface_aim(f->mesh_ref, f->mesh_params, f->mesh_param_count,
                                       fpos, fyaw, ray, &loc)) continue;
            st->file_aim    = SOL_TRUE;
            st->file_target = f->handle;
            if (furniture_is_shelf(f->mesh_ref)) {
                int idx = furniture_child_count(st, f->handle);   /* Step 3 helper */
                st->file_local = furniture_shelf_slot(f->mesh_params, f->mesh_param_count, idx);
                st->file_rot   = quat_from_axis_angle(vec3_make(0.0f,1.0f,0.0f), sol_radians(90.0f)); /* spine: edge-out */
            } else {
                st->file_local = furniture_table_point(f->mesh_params, f->mesh_param_count, loc);
                st->file_rot   = quat_mul(quat_from_axis_angle(vec3_make(0.0f,1.0f,0.0f), st->place_yaw),
                                          quat_from_axis_angle(vec3_make(1.0f,0.0f,0.0f), sol_radians(-90.0f))); /* flat + carry yaw */
            }
            /* preview: hover the carried tablet at the resting spot, in furniture-local-to-world */
            {
                mat4 fm = scene_world_matrix(&st->scene, f);
                vec3 wp = mat4_mul_point(fm, st->file_local);
                o->pos = scene_world_to_local(&st->scene, o->parent, wp);
                o->rot = st->file_rot;   /* show the resting orientation while aimed */
            }
            return;
        }
    }
```

(`st->place_yaw` is the shared rotate value; Step 1c wires `,`/`.` to drive it while carrying, giving the requested table-tablet rotation.)

- [ ] **Step 1c: Rotate keys while carrying.** In the GLFW key callback, after the place-mode block (Task 6 Step 3) and before the other handlers, add a carry-rotation block so `,`/`.` spin a carried tablet (mirrors place mode but for carry):

```c
    /* ',' / '.' rotate a carried tablet (the table-filing yaw control). */
    if (st->place_active == SOL_FALSE && st->carried != 0 &&
        (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        if      (key == GLFW_KEY_COMMA)  st->place_yaw -= sol_radians(15.0f);
        else if (key == GLFW_KEY_PERIOD) st->place_yaw += sol_radians(15.0f);
    }
```

(Don't `return` — let other handlers still run for non-rotate keys while carrying.)

- [ ] **Step 2b: The child-count helper.** Add near `carry_update`:

```c
/* how many objects are parented to `furniture` (its current contents). */
static int furniture_child_count(AppState *st, sol_u32 furniture) {
    sol_u32 i; int n = 0;
    for (i = 0; i < st->scene.count; i++)
        if (st->scene.objects[i].parent == furniture) n++;
    return n;
}
```

(Forward-declare it above `carry_update` if needed.)

- [ ] **Step 3: Build the gauntlet.** `./build.sh c89check && ./build.sh debug && ./build.sh metal` → all clean. (Filing doesn't attach yet — Task 11; here the carried tablet just previews at the resting spot when you aim at furniture.) Confirm `mat4_mul_point`, `object_world_pos`, `quat_mul` are in scope (all used elsewhere).

- [ ] **Step 4: Commit**

```bash
git add main.c
git commit -m "$(printf 'Furniture: carry surface-aim previews a tablet on furniture\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 11: filing — attach on place, detach on pickup

**Files:** Modify `main.c`.

- [ ] **Step 1: Attach on place.** In `cmd_carry_toggle` (main.c:6592), in the carrying branch, add a `file_aim` case BEFORE the existing ground-place `else`. It re-parents the tablet onto the furniture (the whiteboard `o->parent = board` move):

```c
            } else if (st->file_aim && st->file_target != 0 &&
                       scene_get(&st->scene, st->file_target) != 0) {
                o->parent = st->file_target;          /* re-parent: the furniture owns it now */
                o->pos    = st->file_local;           /* furniture-local resting pos */
                o->rot    = st->file_rot;
                scene_resolve_meshes(&st->scene);
                apply_kind_materials(&st->scene);
                scene_save(&st->scene, "scene.stml");
                printf("filed onto furniture\n");
            } else {
```

(i.e. the existing `else { vec3 w = carry_place_point... }` becomes `} else if (file_aim) {...} else { ground place }`.)

Also reset the flag where `cmd_carry_toggle` already resets `plant_aim` (its end: `st->carried = 0; st->plant_aim = SOL_FALSE;`) — add `st->file_aim = SOL_FALSE;` there too.

- [ ] **Step 2: Detach on pickup.** Still in `cmd_carry_toggle`, in the ELSE branch (pick up), when the picked tablet is parented to a furniture object, re-parent it to the world so it leaves the collection cleanly. After `st->carried = t;` capture and BEFORE returning, add:

```c
        if (t != 0) {
            SceneObject *co = scene_get(&st->scene, t);
            st->carried = t;
            if (co) {
                st->carry_origin = co->pos;
                {
                    SceneObject *par = scene_get(&st->scene, co->parent);
                    if (par && par->mesh_ref &&
                        (furniture_is_table(par->mesh_ref) || furniture_is_shelf(par->mesh_ref))) {
                        vec3 wp = object_world_pos(&st->scene, t);      /* world pos before detach */
                        co->parent = 0;                                  /* leave the furniture */
                        co->pos    = wp;
                    }
                }
            }
        }
```

(Replace the existing pickup block with this; keep behavior identical for non-filed objects — only filed tablets re-parent to world.)

- [ ] **Step 3: Build the gauntlet.** `./build.sh c89check && ./build.sh debug && ./build.sh metal` → all clean.

- [ ] **Step 4: Commit**

```bash
git add main.c
git commit -m "$(printf 'Furniture: file a tablet onto furniture (attach) + detach on pickup\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Final verification (human live-verify — NOT a subagent step)

After Task 11, Fran verifies on BOTH backends (`./build.sh debug && ./solarium`; `./build.sh metal && ./solarium-metal`):

1. `:` → "Place furniture" → a translucent ghost tracks your aim; `[`/`]` cycle table↔bookshelf; `,`/`.` rotate; `Enter` drops a solid object; `Esc` cancels.
2. A placed bookshelf prompts for a label; the label renders across its top rail.
3. Carry a file tablet (`E`) → aim at a table → it previews flat (with `,`/`.` rotation) → drop → it lands flat and attaches. Aim at a bookshelf → it previews as a spine in the next slot → drop → it shelves.
4. Move the furniture (carry it) → its filed tablets ride along.
5. Pick a filed tablet back up (`E`) → it detaches to the world.
6. Place furniture inside a non-home workspace → it stays there; persists across save/reload.

Then: `superpowers:finishing-a-development-branch` to merge `furniture-filing` → `main`, and update memory.

---

## Notes carried from spec §12 (resolved here)

- **Keys:** `[`/`]` cycle, `,`/`.` rotate, `Enter` confirm, `Esc` cancel — all handled inside the place-mode key block (it owns input), so no global-hotkey collision. The same `place_yaw` gives table-tablet rotation during carry.
- **Material:** furniture uses `material_default` (a neutral prop look) in v1; a `mesh_ref`-keyed wood material is an easy later polish.
- **Carried-kind gate for filing:** `mesh_ref=="card" && kind != KIND_FOLDER` (FILE/ALIAS/NOTE file onto furniture; FOLDER keeps descent wall-aim).
- **Non-solid:** furniture gets no collider (no `collide_rebuild` case), matching spec §9.
- **Detach:** picking up a filed tablet re-parents it to world at its pre-detach world position.
```
