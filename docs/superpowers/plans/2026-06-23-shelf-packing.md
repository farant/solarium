# Width- & Depth-Aware Shelf Packing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Filed items (cards + codices) pack on a shelf by their own thickness, spine flush at the front, re-flowing the whole shelf on add/remove.

**Architecture:** A pure `furniture_shelf_layout` (in `furniture.c`, unit-tested) packs item widths left-to-right with row wrap. `main.c`'s `shelf_repack` gathers a shelf's filed items, runs the layout, and sets each one's local pos (depth-aware z, per-item vertical anchor). It runs on every drop and pickup. The old fixed-pitch grid is retired. No new mesh, no shader.

**Tech Stack:** C89, the scene-free `furniture.c` + `furniture_test` (ASan/UBSan), main.c carry/file glue. Build gauntlet: `./build.sh c89check && ./build.sh debug && ./build.sh metal && ./build.sh furnituretest`.

---

## Context the implementer needs

- **C89:** declarations at the top of each block, `/* */` only, no `fabsf`/`sinf` C99-isms in pedantic code — use `fabs((double)x)` etc. `furniture.c` is **scene-free pure math** (no scene/GL); the scene gathering lives in `main.c`.
- **Shelf params (by index, as the existing code reads them):** `params[0]=w` (width), `[1]=h` (height), `[2]=d` (depth), `[3]=sh` (shelf-board count). Constants in `furniture.c`: `FURN_SHELF_MARGIN 0.06`, `FURN_PANEL_T 0.04`, `FURN_SPINE_PITCH 0.06`.
- **Old grid (being retired):** `furniture_shelf_slot(params,count,i)` → fixed grid; `furniture_shelf_capacity`; `shelf_free_slot` (main.c) scans for the lowest free slot. The row-height formula inside `furniture_shelf_slot` is `s.y = (h-FURN_PANEL_T)*(sh-row)/(sh+1) + FURN_PANEL_T` (row 0 = top board, row `sh` = floor).
- **Axis mapping (filed spine-out):** an item's **thickness `t`** runs along the shelf (the packing width), its **width `w`** goes into the shelf (depth, spine→fore), its **height `h`** is vertical. Both card and codex meshes carry `w`/`h`/`t` params; a codex reads them from its `book_cover` child (`codex_cover_child(s, handle)`), a card from itself. Read via `mesh_ref_param(ref, params, count, "t"/"w"/"h")`.
- **Origins:** a card is bottom-origin (`pos.y` = base). A codex anchor's origin is the book CENTRE (`pos.y` = centre = base + `h/2`).
- **Integration points in `main.c`:** the `carry_update` furniture-preview loop sets `st->file_local` (currently from `furniture_shelf_slot`); the `cmd_carry_toggle` `file_aim` drop re-parents at `file_local`; the pickup `on_furn` branch detaches a shelved item (`main.c:7025`).
- **Helper placement:** put the new `main.c` shelf helpers **before `cmd_carry_toggle`** (so the drop/pickup can call them) and before `carry_update` (so the preview can) — i.e. alongside `carry_target`/`carry_place_point`.

---

### Task 1: The pure variable-width layout (furniture.c)

**Files:** Modify `furniture.h`, `furniture.c`, `furniture_test.c`. Old grid functions are KEPT in this task (main.c still calls them).

- [ ] **Step 1: Add the layout + row_y declarations to `furniture.h`**

After the `furniture_shelf_capacity` declaration block, add:

```c
/* Pack n items of along-shelf widths widths[0..n) left-to-right across the
   bookshelf, each followed by a small gap, wrapping to the next row (0 = top
   board ... sh = floor board) when an item won't fit the remaining row width.
   Fills out_x[i] (LOCAL x of the item's CENTRE) and out_row[i]. An item wider
   than a whole row still gets placed (its own row). Returns the rows used. */
int   furniture_shelf_layout(const float *params, int count,
                             const float *widths, int n,
                             float *out_x, int *out_row);

/* the LOCAL y of a shelf row's board top (row 0 = top ... sh = floor), so a
   filed item's base can sit on it. */
float furniture_shelf_row_y(const float *params, int count, int row);
```

- [ ] **Step 2: Add the gap constant + implementations to `furniture.c`**

Near the other `FURN_*` constants add:

```c
#define FURN_SHELF_GAP    0.012f   /* the breathing room between packed spines */
```

After `furniture_shelf_capacity`'s definition, add:

```c
int furniture_shelf_layout(const float *params, int count,
                           const float *widths, int n,
                           float *out_x, int *out_row) {
    float w      = (count > 0) ? params[0] : 1.0f;
    int   sh     = (count > 3) ? (int)(params[3] + 0.5f) : 2;
    float usable = w - 2.0f * FURN_SHELF_MARGIN;
    float left   = -w * 0.5f + FURN_SHELF_MARGIN;
    float cursor = 0.0f;                    /* distance from `left` along the row */
    int   row = 0, rows, i;
    if (sh < 1) sh = 1;
    rows = sh + 1;
    for (i = 0; i < n; i++) {
        float bw = (widths[i] > 0.0f) ? widths[i] : 0.0f;
        if (cursor > 0.0f && cursor + bw > usable) {   /* won't fit: next row */
            cursor = 0.0f;
            if (row < rows - 1) row++;                 /* clamp; overflow piles on last */
        }
        out_x[i]   = left + cursor + bw * 0.5f;
        out_row[i] = row;
        cursor += bw + FURN_SHELF_GAP;
    }
    return (n > 0) ? row + 1 : 0;
}

float furniture_shelf_row_y(const float *params, int count, int row) {
    float h  = (count > 1) ? params[1] : 1.8f;
    int   sh = (count > 3) ? (int)(params[3] + 0.5f) : 2;
    if (sh < 1) sh = 1;
    if (row < 0)  row = 0;
    if (row > sh) row = sh;
    return (h - FURN_PANEL_T) * (float)(sh - row) / (float)(sh + 1) + FURN_PANEL_T;
}
```

- [ ] **Step 3: Add `furniture_test` cases**

In `furniture_test.c`, before the final `return fails ? 1 : 0;` (or after the existing shelf-slot block), add:

```c
    /* variable-width layout: widths pack left-to-right, wrap a full row, row_y descends */
    {
        float p[4]; float ws[5], xs[5]; int rows[5]; int used;
        p[0] = 1.0f; p[1] = 1.8f; p[2] = 0.3f; p[3] = 2.0f;   /* w h d sh */
        /* three thin items fit one row, in order, left-to-right */
        ws[0] = 0.1f; ws[1] = 0.2f; ws[2] = 0.1f;
        used = furniture_shelf_layout(p, 4, ws, 3, xs, rows);
        CHECK(rows[0] == 0 && rows[1] == 0 && rows[2] == 0);   /* same (top) row */
        CHECK(xs[0] < xs[1] && xs[1] < xs[2]);                 /* left to right */
        CHECK(used == 1);
        /* gap-aware spacing: centre-to-centre ~= half each width + the gap */
        CHECK(fabs((double)((xs[1]-xs[0]) - (0.1f*0.5f + 0.012f + 0.2f*0.5f))) < 1e-4);
        /* a row that overflows wraps to the next (lower) row */
        ws[0] = 0.5f; ws[1] = 0.5f; ws[2] = 0.5f;              /* usable=0.88: 2 per row */
        used = furniture_shelf_layout(p, 4, ws, 3, xs, rows);
        CHECK(rows[0] == 0 && rows[1] == 0);                   /* two fit the top row */
        CHECK(rows[2] == 1);                                   /* third wraps down */
        /* row_y: the top board (0) sits above the floor board (sh) */
        CHECK(furniture_shelf_row_y(p, 4, 0) > furniture_shelf_row_y(p, 4, 2));
    }
```

- [ ] **Step 4: Build the test + the gauntlet**

Run: `./build.sh furnituretest && ./furniture_test && ./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: `furniture_test` prints no `FAIL` lines (exit 0); gauntlet PASSes (main.c still uses the old grid, unchanged).

- [ ] **Step 5: Commit**

```bash
git add furniture.h furniture.c furniture_test.c
git commit -m "$(cat <<'EOF'
shelf-packing: pure variable-width layout (furniture.c)

furniture_shelf_layout packs item widths left-to-right with a gap, wrapping
rows; furniture_shelf_row_y gives each row's board height. Unit-tested. The
old fixed grid still stands until main.c migrates.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Re-pack in main.c (migrate filing to the layout)

**Files:** Modify `main.c` — new shelf helpers before `cmd_carry_toggle`; the `carry_update` preview shelf branch; the `cmd_carry_toggle` drop + pickup; remove `shelf_free_slot`.

- [ ] **Step 1: Add the shelf helpers**

Immediately before `static void cmd_carry_toggle(` (or beside `carry_target`/`carry_place_point`), add:

```c
#define SHELF_MAX_ITEMS 64

/* a filed item on a shelf = a codex anchor or a card (NOT the shelf's own mesh). */
static int shelf_is_filed_item(AppState *st, sol_u32 h) {
    SceneObject *o = scene_get(&st->scene, h);
    if (!o) return 0;
    if (codex_cover_child(&st->scene, h) != 0) return 1;
    return (int)(o->mesh_ref && strcmp(o->mesh_ref, "card") == 0);
}

/* the item's shelf footprint: along_w = thickness (packs along the shelf),
   depth = width (into the shelf), height = h. A codex reads its cover child. */
static void shelf_item_dims(AppState *st, sol_u32 handle,
                            float *along_w, float *depth, float *height) {
    sol_u32      cov = codex_cover_child(&st->scene, handle);
    SceneObject *o   = scene_get(&st->scene, cov != 0 ? cov : handle);
    const char  *ref = (cov != 0) ? "book_cover"
                     : (o && o->mesh_ref) ? o->mesh_ref : "card";
    const float *p   = o ? o->mesh_params : (const float *)0;
    int          pc  = o ? o->mesh_param_count : 0;
    *along_w = mesh_ref_param(ref, p, pc, "t");
    *depth   = mesh_ref_param(ref, p, pc, "w");
    *height  = mesh_ref_param(ref, p, pc, "h");
}

/* the LOCAL pos `item` should take on `furniture` for layout row/x `rx`/`row`:
   depth-aware z (spine flush at the front), per-item vertical anchor. */
static vec3 shelf_item_pos(AppState *st, sol_u32 furniture, sol_u32 item,
                           float rx, int row) {
    SceneObject *fo = scene_get(&st->scene, furniture);
    float aw, dp, ht, d, ry;
    vec3  r;
    d  = (fo && fo->mesh_param_count > 2) ? fo->mesh_params[2] : 0.3f;
    ry = furniture_shelf_row_y(fo ? fo->mesh_params : (const float *)0,
                               fo ? fo->mesh_param_count : 0, row);
    shelf_item_dims(st, item, &aw, &dp, &ht);
    r.x = rx;
    r.y = ry + ((codex_cover_child(&st->scene, item) != 0) ? ht * 0.5f : 0.0f);
    r.z = (d * 0.5f - 0.02f) - dp * 0.5f;   /* spine flush at the front opening */
    return r;
}

/* Re-flow every filed item on `furniture` tight, left-to-right, no gaps. */
static void shelf_repack(AppState *st, sol_u32 furniture) {
    sol_u32      items[SHELF_MAX_ITEMS];
    float        widths[SHELF_MAX_ITEMS], xs[SHELF_MAX_ITEMS];
    int          rows[SHELF_MAX_ITEMS];
    int          n = 0, i, j;
    SceneObject *fo = scene_get(&st->scene, furniture);
    if (!fo || !fo->mesh_ref || !furniture_is_shelf(fo->mesh_ref)) return;
    for (i = 0; i < (int)st->scene.count && n < SHELF_MAX_ITEMS; i++) {
        SceneObject *o = &st->scene.objects[i];
        if (o->parent != furniture) continue;
        if (!shelf_is_filed_item(st, o->handle)) continue;
        items[n++] = o->handle;
    }
    /* stable fill order: higher row (bigger y) first, then left (smaller x);
       a just-dropped item placed at the append x sorts to the end. */
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++) {
            SceneObject *a = scene_get(&st->scene, items[i]);
            SceneObject *b = scene_get(&st->scene, items[j]);
            int swap = 0;
            if (a && b) {
                if (b->pos.y > a->pos.y + 0.01f) swap = 1;
                else if (fabs((double)(b->pos.y - a->pos.y)) <= 0.01 &&
                         b->pos.x < a->pos.x) swap = 1;
            }
            if (swap) { sol_u32 t = items[i]; items[i] = items[j]; items[j] = t; }
        }
    for (i = 0; i < n; i++) {
        float aw, dp, ht;
        shelf_item_dims(st, items[i], &aw, &dp, &ht);
        widths[i] = aw;
    }
    furniture_shelf_layout(fo->mesh_params, fo->mesh_param_count, widths, n, xs, rows);
    for (i = 0; i < n; i++) {
        SceneObject *o = scene_get(&st->scene, items[i]);
        if (o) o->pos = shelf_item_pos(st, furniture, items[i], xs[i], rows[i]);
    }
}

/* where a carried item WOULD land if filed now (append at the end of the pack) —
   for the live preview. */
static vec3 shelf_append_local(AppState *st, sol_u32 furniture, sol_u32 carried) {
    float        widths[SHELF_MAX_ITEMS + 1], xs[SHELF_MAX_ITEMS + 1];
    int          rows[SHELF_MAX_ITEMS + 1];
    int          n = 0, i;
    SceneObject *fo = scene_get(&st->scene, furniture);
    float        aw, dp, ht;
    if (!fo) { vec3 z = { 0.0f, 0.0f, 0.0f }; return z; }
    for (i = 0; i < (int)st->scene.count && n < SHELF_MAX_ITEMS; i++) {
        SceneObject *o = &st->scene.objects[i];
        if (o->parent != furniture || o->handle == carried) continue;
        if (!shelf_is_filed_item(st, o->handle)) continue;
        shelf_item_dims(st, o->handle, &aw, &dp, &ht);
        widths[n++] = aw;
    }
    shelf_item_dims(st, carried, &aw, &dp, &ht);
    widths[n] = aw;
    furniture_shelf_layout(fo->mesh_params, fo->mesh_param_count, widths, n + 1, xs, rows);
    return shelf_item_pos(st, furniture, carried, xs[n], rows[n]);
}
```

- [ ] **Step 2: Migrate the `carry_update` preview to the append position**

In the furniture-preview loop, replace the shelf/codex/card position logic. The block currently sets `st->file_local` from `shelf_free_slot`+`furniture_shelf_slot` (with a `bh*0.5` lift for codices). Replace the whole `if (carry_is_codex) {...} else if (furniture_is_shelf(...)) {...} else {...}` chain with:

```c
            if (furniture_is_shelf(f->mesh_ref)) {
                st->file_local = shelf_append_local(st, f->handle, st->carried);
                st->file_rot   = carry_is_codex
                    ? quat_mul(quat_from_axis_angle(vec3_make(0.0f,1.0f,0.0f), sol_radians(90.0f)),
                               quat_from_axis_angle(vec3_make(1.0f,0.0f,0.0f), sol_radians(-90.0f)))
                    : quat_from_axis_angle(vec3_make(0.0f,1.0f,0.0f), sol_radians(90.0f));
            } else if (carry_is_codex) {
                st->file_local = furniture_table_point(f->mesh_params, f->mesh_param_count, loc);
                st->file_rot   = quat_mul(
                    quat_from_axis_angle(vec3_make(0.0f,1.0f,0.0f), sol_radians(90.0f)),
                    quat_from_axis_angle(vec3_make(1.0f,0.0f,0.0f), sol_radians(-90.0f)));
            } else {
                st->file_local = furniture_table_point(f->mesh_params, f->mesh_param_count, loc);
                st->file_rot   = quat_mul(quat_from_axis_angle(vec3_make(0.0f,1.0f,0.0f), st->place_yaw),
                                          quat_from_axis_angle(vec3_make(1.0f,0.0f,0.0f), sol_radians(-90.0f)));
            }
```

(The codex table-rotation keeps the upright-ish lay used before; the previous `bh`/`shelf_free_slot`/`furniture_shelf_slot` code is gone — `shelf_append_local` does the vertical anchor and depth.)

- [ ] **Step 3: Re-pack on the drop**

In `cmd_carry_toggle`'s `file_aim` branch, after `o->rot = st->file_rot;` and before its `scene_save`, add:

```c
                {
                    SceneObject *ft = scene_get(&st->scene, st->file_target);
                    if (ft && ft->mesh_ref && furniture_is_shelf(ft->mesh_ref))
                        shelf_repack(st, st->file_target);
                }
```

- [ ] **Step 4: Re-pack on pickup from a shelf**

In the pickup `on_furn` detach (`main.c:7025` block), capture the shelf and repack after detaching. Replace:

```c
                    if (on_furn || mounted) {
                        vec3 wp = object_world_pos(&st->scene, t);   /* world pos before detach */
                        co->parent = 0;                               /* leave the wall/furniture */
                        co->pos    = wp;
                    }
```

with:

```c
                    if (on_furn || mounted) {
                        vec3    wp  = object_world_pos(&st->scene, t);  /* world pos before detach */
                        sol_u32 src = co->parent;                       /* the shelf, if any */
                        sol_bool was_shelf = (sol_bool)(par && par->mesh_ref &&
                                                        furniture_is_shelf(par->mesh_ref));
                        co->parent = 0;                                 /* leave the wall/furniture */
                        co->pos    = wp;
                        if (was_shelf) shelf_repack(st, src);           /* reflow the rest */
                    }
```

- [ ] **Step 5: Remove `shelf_free_slot`**

Delete the entire `static int shelf_free_slot(...) { ... }` function from `main.c` — it has no callers after Step 2.

- [ ] **Step 6: Build the gauntlet**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: all PASS (no `-Wunused-function` — `shelf_free_slot` is gone; `furniture_shelf_slot`/`capacity` are public so they don't warn while unused).

- [ ] **Step 7: Human live-verify (hand to Fran)**

Hand Fran:
1. File several different-sized books on one shelf → they pack tight side-by-side, no overlap, spines flush at the front; large books overhang the back.
2. Remove a mid-shelf book (`E`) → the remaining books slide to close the gap.
3. Fill a row → the next book wraps to the row below.
4. Mix cards and books on one shelf → all lay out without overlap.
5. The carry preview shows the book at the spot it actually lands.
6. Reload → the shelved layout persists.

- [ ] **Step 8: Commit (after Fran confirms)**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
shelf-packing: re-pack filed items by width on every drop/pickup

shelf_repack reflows a shelf's items (cards + codices) tight via
furniture_shelf_layout — thickness packs along the shelf, spine flush at the
front (depth = width), per-item vertical anchor. The carry preview shows the
append position; pickup reflows the rest. shelf_free_slot retired.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Retire the old fixed grid

**Files:** Modify `furniture.h`, `furniture.c`, `furniture_test.c`.

- [ ] **Step 1: Remove the old grid functions**

Delete `furniture_shelf_slot` and `furniture_shelf_capacity` (both the declarations in `furniture.h` and the definitions in `furniture.c`). Leave `furn_shelf_cols`/`FURN_SPINE_PITCH` only if still referenced; if nothing references them after the removal, delete them too (the compiler will flag an unused static `furn_shelf_cols`).

- [ ] **Step 2: Remove the old shelf-slot test cases**

In `furniture_test.c`, delete the two blocks that call `furniture_shelf_slot` (the "shelf slots: fill a shelf left-to-right" block and the nested "once a shelf fills" sub-block). The new layout block from Task 1 stays.

- [ ] **Step 3: Build the test + gauntlet**

Run: `./build.sh furnituretest && ./furniture_test && ./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: `furniture_test` clean (no FAIL), gauntlet PASSes, no unused-function warnings.

- [ ] **Step 4: Commit**

```bash
git add furniture.h furniture.c furniture_test.c
git commit -m "$(cat <<'EOF'
shelf-packing: retire the fixed-pitch shelf grid

Remove furniture_shelf_slot/furniture_shelf_capacity (+ furn_shelf_cols if now
unused) and their tests; all filing routes through furniture_shelf_layout.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-review

- **Spec coverage:**
  - Footprint (along=t, depth=w, height=h; codex via cover child) → `shelf_item_dims` (Task 2 Step 1).
  - Pure variable-width layout + row_y + gap → Task 1.
  - `shelf_repack` reflow on drop + pickup → Task 2 Steps 1,3,4.
  - Depth-aware spine-flush z + per-item vertical anchor → `shelf_item_pos` (Task 2 Step 1).
  - Preview = append position → `shelf_append_local` + Task 2 Step 2.
  - Unified cards + books → `shelf_is_filed_item` accepts both; preview/repack are kind-agnostic.
  - Retire fixed grid → Task 3 (+ remove `shelf_free_slot` in Task 2 Step 5).
  - Unit tests + build gauntlet incl. `furnituretest` → Task 1 Step 3/4, Task 3 Step 3.
- **Placeholder scan:** none — every code step is complete. `fabs((double)…)` used (no C99 `fabsf`). No `furniture.c` scene access.
- **Type/name consistency:** `furniture_shelf_layout(params,count,widths,n,out_x,out_row)→int` and `furniture_shelf_row_y(params,count,row)→float` match between `.h`, `.c`, tests, and the `main.c` callers; `shelf_item_dims`/`shelf_item_pos`/`shelf_repack`/`shelf_append_local`/`shelf_is_filed_item` consistent; `codex_cover_child`, `furniture_is_shelf`, `mesh_ref_param`, `furniture_table_point` match existing signatures. Helpers defined before `cmd_carry_toggle`/`carry_update` (their callers).
- **Ordering:** Task 1 adds (old grid still used → builds); Task 2 migrates main.c + drops `shelf_free_slot`; Task 3 deletes the now-dead grid. Each task builds; human verify after Task 2.
