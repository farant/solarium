# Board Multi-Select Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finder-style multi-select for board cards in board view — shift-click + rubber-band marquee across notes/pictures/folders, with group drag / delete / file-onto-folder that keeps the cards' arrangement.

**Architecture:** A selection set (`sel[]` + `sel_count`) layered beside the existing single `selected_handle` (the anchor). A new scene-free `multiselect.c` holds the pure rect-overlap + set-ops (unit-tested). Everything is gated on `board_view != 0`; `sel_count ≤ 1` preserves today's single-select behavior. No new shader (highlight reuses `uHighlight`; the marquee reuses `ui_quad`).

**Tech Stack:** C89 ("Dependable C"), OpenGL + Metal dual backend, hand-written `build.sh`. Pure logic in `multiselect.c`; the rest is `main.c`. Spec: `docs/superpowers/specs/2026-06-25-board-multiselect-design.md`.

**Gauntlet (run all three after every task that touches buildable code):** `./build.sh c89check` · `./build.sh` · `./build.sh metal`.

**Project laws:**
- **Strict C89**: declarations at the TOP of each block; `/* */` comments only; no C99; no C99 math in main.c (use `(float)sin((double)x)`, ternary abs). Authority: `./build.sh c89check`.
- **Use-after-realloc / remove:** `scene_add`/`scene_remove` may move the objects array. Never hold a `SceneObject*` across them; operate by handle and re-`scene_get`. Handles are stable; pointers are not.
- Commit messages end EXACTLY with: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. `git add` ONLY the files each task names. NEVER add `NOTES.stml` / `paper-picture.png`.

---

## File Structure

- **`multiselect.h` / `.c` / `_test.c`** (NEW, scene-free, libc + `sol_types.h`): `msel_rect_overlap` + set ops + `MULTISEL_CAP`. Pure, unit-tested.
- **`build.sh`**: a `multiselecttest` target + `multiselect.c` on the four full-build lines.
- **`main.c`**: `AppState` fields; `sel_*` wrappers + `object_is_selectable`; gesture changes in the board-view press handler; group delete; the marquee (state/begin/projection/update/commit/draw/preview); group drag + release.

---

# Milestone 1 — Selection set + clicks + delete

## Task 1: `multiselect` module (pure logic, TDD)

**Files:** Create `multiselect.h`, `multiselect.c`, `multiselect_test.c`; Modify `build.sh`.

- [ ] **Step 1: Header** — create `multiselect.h`:
```c
#ifndef SOL_MULTISELECT_H
#define SOL_MULTISELECT_H
#include "sol_types.h"

#define MULTISEL_CAP 256            /* max cards in one selection */

/* AABB overlap (Finder "touch"): true if rect A and rect B overlap, edges
   inclusive. Corners may be given in any order (normalized internally). */
sol_bool msel_rect_overlap(float ax0, float ay0, float ax1, float ay1,
                           float bx0, float by0, float bx1, float by1);

/* Set ops on a handle array (len entries, cap capacity), stable order. */
sol_bool msel_contains(const sol_u32 *set, int len, sol_u32 h);
void     msel_add(sol_u32 *set, int *len, int cap, sol_u32 h);    /* no-op if present/full */
void     msel_remove(sol_u32 *set, int *len, sol_u32 h);          /* compacts; no-op if absent */
sol_bool msel_toggle(sol_u32 *set, int *len, int cap, sol_u32 h); /* returns new present-state */

#endif
```

- [ ] **Step 2: Failing test** — create `multiselect_test.c`:
```c
#include "multiselect.h"
#include <stdio.h>

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static void test_overlap(void) {
    CHECK(msel_rect_overlap(0,0, 2,2,  1,1, 3,3), "overlap: corner overlap");
    CHECK(!msel_rect_overlap(0,0, 1,1, 2,2, 3,3), "overlap: disjoint");
    CHECK(msel_rect_overlap(0,0, 4,4,  1,1, 2,2), "overlap: A contains B");
    CHECK(msel_rect_overlap(2,2, 0,0,  1,1, 3,3), "overlap: unnormalized A");
    CHECK(msel_rect_overlap(0,0, 2,2,  2,2, 3,3), "overlap: edge touch inclusive");
    CHECK(!msel_rect_overlap(0,0, 1,0.5f, 0,1, 1,2), "overlap: vertical gap");
}

static void test_setops(void) {
    sol_u32 s[8];
    int     n = 0;
    msel_add(s, &n, 8, 5); msel_add(s, &n, 8, 7); msel_add(s, &n, 8, 5);
    CHECK(n == 2, "set: add dedupes");
    CHECK(msel_contains(s, n, 7) && !msel_contains(s, n, 9), "set: contains");
    msel_remove(s, &n, 5);
    CHECK(n == 1 && s[0] == 7, "set: remove compacts");
    CHECK(msel_toggle(s, &n, 8, 9) == SOL_TRUE && n == 2, "set: toggle adds");
    CHECK(msel_toggle(s, &n, 8, 9) == SOL_FALSE && n == 1, "set: toggle removes");
}

int main(void) {
    test_overlap();
    test_setops();
    if (fails == 0) printf("multiselect_test: all passed\n");
    return fails ? 1 : 0;
}
```

- [ ] **Step 3: Add the `multiselecttest` target.** In `build.sh`, after the `inventorytest` block, add:
```sh
# multiselecttest: the board multi-select pure logic (scene-free C89). libc only.
if [ "$MODE" = "multiselecttest" ]; then
    set -x
    clang -std=c89 -pedantic-errors -Werror -g -fsanitize=address,undefined \
        multiselect.c multiselect_test.c \
        -o multiselect_test
    echo "built ./multiselect_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi
```

- [ ] **Step 4: Run it — expect a LINK failure** (no bodies yet): `./build.sh multiselecttest`.

- [ ] **Step 5: Implement `multiselect.c`:**
```c
#include "multiselect.h"

sol_bool msel_rect_overlap(float ax0, float ay0, float ax1, float ay1,
                           float bx0, float by0, float bx1, float by1) {
    float axlo = ax0 < ax1 ? ax0 : ax1, axhi = ax0 < ax1 ? ax1 : ax0;
    float aylo = ay0 < ay1 ? ay0 : ay1, ayhi = ay0 < ay1 ? ay1 : ay0;
    float bxlo = bx0 < bx1 ? bx0 : bx1, bxhi = bx0 < bx1 ? bx1 : bx0;
    float bylo = by0 < by1 ? by0 : by1, byhi = by0 < by1 ? by1 : by0;
    if (axhi < bxlo || bxhi < axlo) return SOL_FALSE;
    if (ayhi < bylo || byhi < aylo) return SOL_FALSE;
    return SOL_TRUE;
}

sol_bool msel_contains(const sol_u32 *set, int len, sol_u32 h) {
    int i;
    for (i = 0; i < len; i++) if (set[i] == h) return SOL_TRUE;
    return SOL_FALSE;
}

void msel_add(sol_u32 *set, int *len, int cap, sol_u32 h) {
    if (msel_contains(set, *len, h) || *len >= cap) return;
    set[(*len)++] = h;
}

void msel_remove(sol_u32 *set, int *len, sol_u32 h) {
    int i, j = 0;
    for (i = 0; i < *len; i++) if (set[i] != h) set[j++] = set[i];
    *len = j;
}

sol_bool msel_toggle(sol_u32 *set, int *len, int cap, sol_u32 h) {
    if (msel_contains(set, *len, h)) { msel_remove(set, len, h); return SOL_FALSE; }
    msel_add(set, len, cap, h);
    return msel_contains(set, *len, h);
}
```

- [ ] **Step 6: Run — expect pass:** `./build.sh multiselecttest && ./multiselect_test` → `multiselect_test: all passed`.

- [ ] **Step 7: Wire `multiselect.c` into the four full builds.** In `build.sh`, append ` multiselect.c` to the source list on the `c89check` line (~16), the `metal` line (~362), the `asan` line (~378), and the final debug/release line (~393) — next to `boardpage.c`.

- [ ] **Step 8: Gauntlet** — `./build.sh c89check && ./build.sh && ./build.sh metal` all pass.

- [ ] **Step 9: Commit:**
```bash
git add multiselect.h multiselect.c multiselect_test.c build.sh
git commit -m "$(printf 'Board multi-select 1/8: multiselect module — rect-overlap + set ops (TDD)\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 2: AppState fields, `sel_*` wrappers, `object_is_selectable`, set highlight

**Files:** Modify `main.c`.

- [ ] **Step 1: Include + AppState fields.** Add near the other includes:
```c
#include "multiselect.h"
```
In the `AppState` struct (next to `selected_handle` / `drop_target_handle`), add:
```c
    sol_u32  sel[MULTISEL_CAP];    /* board multi-select set; <=1 mirrors selected_handle */
    int      sel_count;
    sol_bool marquee_active;       /* a marquee gesture is underway (M2) */
    sol_bool marquee_dragging;     /* moved past the slop -> a real rubber-band (M2) */
    sol_bool marquee_add;          /* shift held -> union, else replace (M2) */
    double   marquee_x0, marquee_y0, marquee_x1, marquee_y1;   /* screen px (M2) */
    float    marquee_lx0, marquee_ly0, marquee_lx1, marquee_ly1;  /* board-local rect (M2) */
    sol_bool group_drag;           /* dragging the whole set together (M3) */
    vec3     group_prepos[MULTISEL_CAP];   /* per-member pre-drag board-local pos (M3) */
```
In the AppState init (where `st->drop_target_handle = 0;` is set), add:
```c
    st->sel_count       = 0;
    st->marquee_active  = SOL_FALSE;
    st->marquee_dragging= SOL_FALSE;
    st->marquee_add     = SOL_FALSE;
    st->group_drag      = SOL_FALSE;
```

- [ ] **Step 2: Helpers** — define before `read_input` (near `object_is_folder` / `is_fileable_card`):
```c
/* A board-child note/picture/folder is multi-selectable. */
static sol_bool object_is_selectable(Scene *s, sol_u32 h) {
    SceneObject *o = scene_get(s, h);
    SceneObject *par;
    if (!o || o->parent == 0) return SOL_FALSE;
    par = scene_get(s, o->parent);
    if (!par || !par->mesh_ref || strcmp(par->mesh_ref, "board") != 0) return SOL_FALSE;
    if (o->kind == KIND_NOTE) return SOL_TRUE;
    if (o->mesh_ref && strcmp(o->mesh_ref, "picture") == 0)    return SOL_TRUE;
    if (o->mesh_ref && strcmp(o->mesh_ref, "folderbook") == 0) return SOL_TRUE;
    return SOL_FALSE;
}

/* selection-set wrappers that keep `selected_handle` (the anchor) in sync:
   sel_count==0 -> selected_handle==0; sel_count==1 -> selected_handle==sel[0]. */
static void sel_clear(AppState *st) {
    st->sel_count = 0;
    st->selected_handle = 0;
}
static void sel_set_single(AppState *st, sol_u32 h) {
    st->sel[0] = h;
    st->sel_count = 1;
    st->selected_handle = h;
}
static void sel_toggle_h(AppState *st, sol_u32 h) {
    sol_bool now = msel_toggle(st->sel, &st->sel_count, MULTISEL_CAP, h);
    st->selected_handle = now ? h
                        : (st->sel_count ? st->sel[st->sel_count - 1] : 0);
}
```

- [ ] **Step 3: Set highlight in the draw loop.** After the existing drop-target highlight (main.c ~13691, the `o->handle == state->drop_target_handle` block), add:
```c
        if (state->sel_count > 1 &&
            msel_contains(state->sel, state->sel_count, o->handle))
            hl = 1.0f;                            /* a member of the multi-selection */
```
(`sel_count == 1` already lights via the existing `sel_root` path, so this only adds the multi case.)

- [ ] **Step 4: Gauntlet** — all three pass.

- [ ] **Step 5: Commit:**
```bash
git add main.c
git commit -m "$(printf 'Board multi-select 2/8: sel set + wrappers + selectable predicate + set highlight\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 3: shift-click + click-to-single gestures (press handler)

**Files:** Modify `main.c` (the board-view press dispatch, ~10064–10096).

- [ ] **Step 1: Read shift + the picked hit, and route the gestures.** The press dispatch currently reads:
```c
                if (is_dbl) {                           /* navigate a folder, edit a note, or create on the board */
                    ...unchanged...
                } else if (try_connect(st, st->selected_handle)) {
                    /* the press completed a connection — no drag */
                } else if (st->selected_handle != 0 &&
                           (board_is_mounted(&st->scene, st->selected_handle) ||
                            note_resizable(&st->scene, st->selected_handle) ||
                            picture_on_board(&st->scene, st->selected_handle)) &&
                           resize_corner_pick(st, w)) {
                    /* grabbed a corner handle — resize, not carry */
                } else if (st->selected_handle != 0 &&
                           picture_move_pick(st, w)) {
                    /* grabbed a picture's body — slide it on its wall or board */
                } else if (st->selected_handle != 0 && st->selected_handle != st->page_handle)
                    drag_begin(st, w, st->selected_handle);
```
Replace from the `} else if (try_connect...` line down to the `drag_begin(...)` line with:
```c
                } else if (try_connect(st, st->selected_handle)) {
                    /* the press completed a connection — no drag */
                } else if (st->board_view != 0 &&
                           (glfwGetKey(w, GLFW_KEY_LEFT_SHIFT)  == GLFW_PRESS ||
                            glfwGetKey(w, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS) &&
                           st->selected_handle != 0 &&
                           object_is_selectable(&st->scene, st->selected_handle)) {
                    sel_toggle_h(st, st->selected_handle);   /* shift-click: toggle, no drag */
                } else if (st->sel_count <= 1 && st->selected_handle != 0 &&
                           (board_is_mounted(&st->scene, st->selected_handle) ||
                            note_resizable(&st->scene, st->selected_handle) ||
                            picture_on_board(&st->scene, st->selected_handle)) &&
                           resize_corner_pick(st, w)) {
                    /* grabbed a corner handle — resize (single selection only) */
                } else if (st->sel_count <= 1 && st->selected_handle != 0 &&
                           picture_move_pick(st, w)) {
                    /* grabbed a picture's body — slide it (single selection only) */
                } else if (st->selected_handle != 0 && st->selected_handle != st->page_handle) {
                    /* a plain click on a card: keep an existing multi-selection it
                       belongs to (so a drag moves the group — M3); otherwise select
                       just this card. */
                    if (st->board_view != 0 && st->sel_count > 1 &&
                        msel_contains(st->sel, st->sel_count, st->selected_handle)) {
                        /* in the set: leave the set; M3 turns the drag into a group drag */
                        drag_begin(st, w, st->selected_handle);
                    } else {
                        if (st->board_view != 0 &&
                            object_is_selectable(&st->scene, st->selected_handle))
                            sel_set_single(st, st->selected_handle);
                        drag_begin(st, w, st->selected_handle);
                    }
                }
```
Notes: a board-view plain-click on a selectable card now updates `sel` to `{card}` via `sel_set_single` (a single click selects). `picture_move_pick` / `resize_corner_pick` are suppressed while a multi-selection is active. The empty-board / marquee case is added in Task 5 (today an empty board click just leaves `selected_handle==0`).

- [ ] **Step 2: Gauntlet** — all three pass.

- [ ] **Step 3: Live-verify (human) + commit.** In board view: shift-click several notes/pictures/folders → each toggles in/out and all glow; a plain click on one card selects just it; single-select resize/edit unchanged. Then:
```bash
git add main.c
git commit -m "$(printf 'Board multi-select 3/8: shift-click toggle + click-to-single gestures\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 4: group delete

**Files:** Modify `main.c` (the Backspace/Delete dispatch, ~11088–11161).

- [ ] **Step 1: A per-card delete helper** — define before `read_input`:
```c
/* Delete one selectable board card (note/picture/folder): release its keyed
   mesh, clear transient refs, remove it. Does NOT save or rebuild arrows. */
static void delete_board_card(AppState *st, sol_u32 h) {
    SceneObject *o = scene_get(&st->scene, h);
    char akey[160];
    if (!o) return;
    if (mesh_asset_key(o, akey)) asset_release(&g_mesh_assets, akey);
    if (st->resize_board       == h) st->resize_board       = 0;
    if (st->move_board         == h) st->move_board         = 0;
    if (st->carried            == h) st->carried            = 0;
    if (st->drag_handle        == h) st->drag_handle        = 0;
    if (st->drop_target_handle == h) st->drop_target_handle = 0;
    if (st->selected_handle    == h) st->selected_handle    = 0;
    scene_remove(&st->scene, h);
}
```

- [ ] **Step 2: Group-delete arm.** The dispatch opens:
```c
        if (bs_now && !st->bs_was_down && st->selected_handle != 0) {
            SceneObject *o = scene_get(&st->scene, st->selected_handle);
            if (o && o->kind == KIND_TOMBSTONE) {
```
Change the opening guard + add the group arm in front of the single dispatch:
```c
        if (bs_now && !st->bs_was_down && st->board_view != 0 && st->sel_count > 1) {
            sol_u32 doomed[MULTISEL_CAP];
            int     i, n = st->sel_count;
            for (i = 0; i < n; i++) doomed[i] = st->sel[i];   /* snapshot: handles are stable */
            sel_clear(st);
            for (i = 0; i < n; i++) delete_board_card(st, doomed[i]);
            arrows_rebuild(st);
            scene_save(&st->scene, "scene.stml");
            printf("deleted %d cards\n", n);
        } else if (bs_now && !st->bs_was_down && st->selected_handle != 0) {
            SceneObject *o = scene_get(&st->scene, st->selected_handle);
            if (o && o->kind == KIND_TOMBSTONE) {
```
(The existing single-delete body and its trailing `}` stay unchanged; you are only adding the new `if (... sel_count > 1) { ... } else if (...)` in front, converting the old `if` to an `else if`.)

- [ ] **Step 3: Gauntlet** — all three pass.

- [ ] **Step 4: Live-verify (human) + commit.** Shift-select several cards → Delete removes them all; the page is otherwise intact; single delete still works. Then:
```bash
git add main.c
git commit -m "$(printf 'Board multi-select 4/8: group delete (delete_board_card over the set)\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

**End of Milestone 1 — click-based multi-select + group delete.**

---

# Milestone 2 — Marquee

## Task 5: marquee — begin, projection, update, commit

**Files:** Modify `main.c`.

- [ ] **Step 1: `board_ray_local` helper** — project a ray onto a specific board's face plane (unclamped), defined after `board_under_ray` (main.c ~4169):
```c
/* Project ray `r` onto board `board`'s front-face plane; writes the board-local
   point (UNCLAMPED). Returns SOL_FALSE if the ray misses the plane. */
static sol_bool board_ray_local(AppState *st, sol_u32 board, Ray r, vec3 *out) {
    SceneObject *o = scene_get(&st->scene, board);
    float bt, t;
    vec3  n, face;
    if (!o) return SOL_FALSE;
    bt   = mesh_ref_param("board", o->mesh_params, o->mesh_param_count, "t");
    n    = quat_rotate(scene_world_rotation(&st->scene, board), vec3_make(0.0f, 0.0f, 1.0f));
    face = mat4_mul_point(scene_world_matrix(&st->scene, o), vec3_make(0.0f, 0.0f, bt * 0.5f));
    if (!ray_vs_plane(r, face, n, &t)) return SOL_FALSE;
    *out = scene_world_to_local(&st->scene, board, vec3_add(r.origin, vec3_scale(r.dir, t)));
    return SOL_TRUE;
}

/* The board-local footprint of a selectable card (x centered on pos.x, y
   bottom-origin pos.y..pos.y+h). */
static void card_footprint(Scene *s, sol_u32 h, float *x0, float *y0,
                           float *x1, float *y1) {
    SceneObject *o = scene_get(s, h);
    float cw = o ? mesh_ref_param(o->mesh_ref ? o->mesh_ref : "card",
                                  o->mesh_params, o->mesh_param_count, "w") : 0.0f;
    float ch = o ? mesh_ref_param(o->mesh_ref ? o->mesh_ref : "card",
                                  o->mesh_params, o->mesh_param_count, "h") : 0.0f;
    *x0 = o ? o->pos.x - cw * 0.5f : 0.0f;
    *x1 = o ? o->pos.x + cw * 0.5f : 0.0f;
    *y0 = o ? o->pos.y : 0.0f;
    *y1 = o ? o->pos.y + ch : 0.0f;
}
```

- [ ] **Step 2: Begin the marquee on an empty-board press.** In the press dispatch (Task 3's restructured chain), add a new arm — place it right after the shift-toggle arm and before the `resize_corner_pick` arm:
```c
                } else if (st->board_view != 0 && st->selected_handle == 0) {
                    /* empty board: begin a marquee (shift => add to the set) */
                    st->marquee_active   = SOL_TRUE;
                    st->marquee_dragging = SOL_FALSE;
                    st->marquee_add      = (sol_bool)(
                        glfwGetKey(w, GLFW_KEY_LEFT_SHIFT)  == GLFW_PRESS ||
                        glfwGetKey(w, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);
                    st->marquee_x0 = st->marquee_x1 = mx;
                    st->marquee_y0 = st->marquee_y1 = my;
```
(`st->selected_handle == 0` here means the press hit the board or empty space — the board-click-deselect at ~10054 already zeroed it.)

- [ ] **Step 3: Per-frame marquee update (while lmb held).** Add a block in `read_input` alongside the other `if (lmb && ...)` drag blocks (e.g. right before the `if (lmb && st->move_board != 0)` slide block):
```c
        if (lmb && st->marquee_active) {
            vec3 c0, c1;
            int  mw, mh;
            glfwGetWindowSize(w, &mw, &mh);
            st->marquee_x1 = mx;
            st->marquee_y1 = my;
            if ((mx - st->marquee_x0) * (mx - st->marquee_x0) +
                (my - st->marquee_y0) * (my - st->marquee_y0) >= 25.0)
                st->marquee_dragging = SOL_TRUE;
            if (mw > 0 && mh > 0 &&
                board_ray_local(st, st->board_view, camera_ray(&st->camera,
                    2.0f * (float)st->marquee_x0 / (float)mw - 1.0f,
                    1.0f - 2.0f * (float)st->marquee_y0 / (float)mh,
                    (float)mw / (float)mh), &c0) &&
                board_ray_local(st, st->board_view, camera_ray(&st->camera,
                    2.0f * (float)st->marquee_x1 / (float)mw - 1.0f,
                    1.0f - 2.0f * (float)st->marquee_y1 / (float)mh,
                    (float)mw / (float)mh), &c1)) {
                st->marquee_lx0 = c0.x; st->marquee_ly0 = c0.y;
                st->marquee_lx1 = c1.x; st->marquee_ly1 = c1.y;
            }
        }
```

- [ ] **Step 4: Commit / clear on release.** In the release handler (`if (!lmb && st->lmb_was_down)`), add a new FIRST arm before the `if (st->resize_board != 0)` arm:
```c
            if (st->marquee_active) {
                if (st->marquee_dragging) {       /* rubber-band: select the covered cards */
                    sol_u32 i;
                    if (!st->marquee_add) sel_clear(st);
                    for (i = 0; i < st->scene.count; i++) {
                        sol_u32 h = st->scene.objects[i].handle;
                        float   fx0, fy0, fx1, fy1;
                        if (st->scene.objects[i].parent != st->board_view) continue;
                        if (!object_is_selectable(&st->scene, h)) continue;
                        if (!scene_object_active(&st->scene, h)) continue;   /* on this page */
                        card_footprint(&st->scene, h, &fx0, &fy0, &fx1, &fy1);
                        if (msel_rect_overlap(st->marquee_lx0, st->marquee_ly0,
                                              st->marquee_lx1, st->marquee_ly1,
                                              fx0, fy0, fx1, fy1))
                            msel_add(st->sel, &st->sel_count, MULTISEL_CAP, h);
                    }
                    st->selected_handle = st->sel_count ? st->sel[st->sel_count - 1] : 0;
                } else if (!st->marquee_add) {     /* a plain click on empty board: clear */
                    sel_clear(st);
                }
                st->marquee_active   = SOL_FALSE;
                st->marquee_dragging = SOL_FALSE;
            } else if (st->resize_board != 0) {     /* finished a resize */
```
(The existing `if (st->resize_board != 0)` becomes the `else if`. Everything below it is unchanged.)

- [ ] **Step 5: Gauntlet** — all three pass. (No visible rectangle yet — Task 6 draws it; selection should still work on release.)

- [ ] **Step 6: Commit:**
```bash
git add main.c
git commit -m "$(printf 'Board multi-select 5/8: marquee gesture — projection, update, commit\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 6: marquee draw + live-preview highlight

**Files:** Modify `main.c`.

- [ ] **Step 1: Live-preview highlight in the draw loop.** After the Task-2 set-highlight block (the `sel_count > 1 && msel_contains` line, ~13691+), add:
```c
        if (state->marquee_active && state->marquee_dragging &&
            o->parent == state->board_view &&
            object_is_selectable(&state->scene, o->handle)) {
            float fw = mesh_ref_param(o->mesh_ref ? o->mesh_ref : "card",
                                      o->mesh_params, o->mesh_param_count, "w");
            float fh = mesh_ref_param(o->mesh_ref ? o->mesh_ref : "card",
                                      o->mesh_params, o->mesh_param_count, "h");
            if (msel_rect_overlap(state->marquee_lx0, state->marquee_ly0,
                                  state->marquee_lx1, state->marquee_ly1,
                                  o->pos.x - fw * 0.5f, o->pos.y,
                                  o->pos.x + fw * 0.5f, o->pos.y + fh))
                hl = 1.0f;                        /* live marquee preview */
        }
```

- [ ] **Step 2: Draw the rectangle.** The frame's UI overlay opens with `ui_begin(state->fb_width, state->fb_height)` at main.c ~14878, and the HUD draws after it through the `} /* show_hud */` at ~14982 — an unconditional `ui_quad` scope. Place the marquee draw immediately AFTER that `} /* show_hud */` line (still inside the same `ui_begin` overlay, `state` in scope):
```c
        if (state->marquee_active && state->marquee_dragging) {
            float rx = (float)(state->marquee_x0 < state->marquee_x1 ?
                               state->marquee_x0 : state->marquee_x1);
            float ry = (float)(state->marquee_y0 < state->marquee_y1 ?
                               state->marquee_y0 : state->marquee_y1);
            float rw = (float)(state->marquee_x0 < state->marquee_x1 ?
                               state->marquee_x1 - state->marquee_x0 :
                               state->marquee_x0 - state->marquee_x1);
            float rh = (float)(state->marquee_y0 < state->marquee_y1 ?
                               state->marquee_y1 - state->marquee_y0 :
                               state->marquee_y0 - state->marquee_y1);
            ui_quad(rx, ry, rw, rh, 0.45f, 0.62f, 0.95f, 0.18f);            /* fill */
            ui_quad_outline(rx, ry, rw, rh, 1.5f, 0.55f, 0.72f, 1.0f, 0.9f);/* border */
        }
```
(`ui_quad`/`ui_quad_outline` are declared in `ui.h`, already included; coordinates are screen pixels, matching `marquee_x0/y0/x1/y1`.)

- [ ] **Step 3: Gauntlet** — all three pass.

- [ ] **Step 4: Live-verify (human) + commit.** Drag a rectangle on the empty board → a translucent blue rect draws; covered cards glow live as you drag; release selects them; shift+rectangle adds to the set; a plain empty click clears. Then:
```bash
git add main.c
git commit -m "$(printf 'Board multi-select 6/8: marquee rectangle draw + live-preview highlight\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

**End of Milestone 2 — the rubber-band marquee.**

---

# Milestone 3 — Group move + file

## Task 7: group drag (begin + delta update + drop-target exclusion)

**Files:** Modify `main.c`.

- [ ] **Step 1: `group_drag_begin`** — define before `read_input` (after `drag_begin`):
```c
/* Begin dragging the whole selection together: snapshot every member's board-
   local position, then drag the anchor on the normal path; the update applies
   the anchor's delta to the rest. */
static void group_drag_begin(AppState *st, GLFWwindow *w) {
    int i;
    for (i = 0; i < st->sel_count; i++) {
        SceneObject *o = scene_get(&st->scene, st->sel[i]);
        st->group_prepos[i] = o ? o->pos : vec3_make(0.0f, 0.0f, 0.0f);
    }
    st->group_drag = SOL_TRUE;
    drag_begin(st, w, st->selected_handle);     /* the anchor leads */
}
```

- [ ] **Step 2: Use it in the press dispatch.** In Task 3's drag arm, replace the in-set branch's `drag_begin(...)` with `group_drag_begin`:
```c
                    if (st->board_view != 0 && st->sel_count > 1 &&
                        msel_contains(st->sel, st->sel_count, st->selected_handle)) {
                        group_drag_begin(st, w);            /* drag the whole set (M3) */
                    } else {
```
(Leave the `else { ... sel_set_single ... drag_begin ... }` branch as-is.)

- [ ] **Step 3: Apply the delta + exclude selected folders in the board-mode drag update.** In the board-mode branch (main.c ~10350–10358), after `o->pos = board_pin_pos(...)` (the anchor move) and `arrows_rebuild(st)`, REPLACE the existing drop-target line:
```c
                        st->drop_target_handle =
                            is_fileable_card(&st->scene, st->drag_handle)
                                ? folder_at_board_point(st, board, blocal) : 0;
```
with:
```c
                        if (st->group_drag) {
                            int  gi, ai = -1;
                            vec3 gd;
                            for (gi = 0; gi < st->sel_count; gi++)
                                if (st->sel[gi] == st->drag_handle) { ai = gi; break; }
                            if (ai >= 0) {
                                gd = vec3_sub(o->pos, st->group_prepos[ai]);
                                for (gi = 0; gi < st->sel_count; gi++) {
                                    SceneObject *si;
                                    if (st->sel[gi] == st->drag_handle) continue;
                                    si = scene_get(&st->scene, st->sel[gi]);
                                    if (si) si->pos = vec3_add(st->group_prepos[gi], gd);
                                }
                            }
                            st->drop_target_handle =
                                folder_at_board_point(st, board, blocal);
                            if (msel_contains(st->sel, st->sel_count,
                                              st->drop_target_handle))
                                st->drop_target_handle = 0;   /* not a selected folder */
                        } else {
                            st->drop_target_handle =
                                is_fileable_card(&st->scene, st->drag_handle)
                                    ? folder_at_board_point(st, board, blocal) : 0;
                        }
```

- [ ] **Step 4: Gauntlet** — all three pass.

- [ ] **Step 5: Live-verify (human) + commit.** Shift-select several cards, then drag one of them → all move together keeping their relative layout; a folder under the cursor (not selected) highlights. (Drop behavior lands in Task 8.) Then:
```bash
git add main.c
git commit -m "$(printf 'Board multi-select 7/8: group drag — snapshot + delta move + drop-target exclusion\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 8: group release — move or file (layout preserved)

**Files:** Modify `main.c` (the release handler).

- [ ] **Step 1: Add a group-release arm BEFORE the single drag arm.** In the release handler, find `} else if (st->drag_handle != 0 && st->drag_moved) {` (main.c ~10415) and insert a new arm immediately before it:
```c
            } else if (st->group_drag) {                /* finished a group move/file */
                sol_u32     tgt  = st->drop_target_handle;
                const char *link = (tgt != 0 &&
                                    !msel_contains(st->sel, st->sel_count, tgt))
                                   ? scene_meta_get(&st->scene, tgt, "link")
                                   : (const char *)0;
                int i;
                if (link && link[0]) {                  /* dropped on a folder: file all */
                    for (i = 0; i < st->sel_count; i++) {
                        SceneObject *si = scene_get(&st->scene, st->sel[i]);
                        if (si) si->pos = st->group_prepos[i];   /* keep the arrangement */
                        scene_meta_set(&st->scene, st->sel[i], "page", link);
                    }
                    printf("filed %d cards onto %s\n", st->sel_count, link);
                    sel_clear(st);
                }                                       /* else: a plain group move, positions kept */
                st->group_drag = SOL_FALSE;
                arrows_rebuild(st);
                scene_save(&st->scene, "scene.stml");
            } else if (st->drag_handle != 0 && st->drag_moved) {
```
The trailing `st->drag_handle = 0; st->drag_moved = SOL_FALSE; st->drop_target_handle = 0;` at the end of the release block run for this arm too (they clear the anchor's drag state) — leave them.

- [ ] **Step 2: Gauntlet** — all three pass.

- [ ] **Step 3: Live-verify (human) + commit.** Group-drag onto empty board → the group's new arrangement saves. Group-drag onto a folder (not one of the selected) → all cards file onto that folder's page; navigate in → they appear in the same relative layout they had. The target folder must not be part of the selection. Then:
```bash
git add main.c
git commit -m "$(printf 'Board multi-select 8/8: group release — move or file with layout preserved\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

**End of Milestone 3.**

---

## Final verification (after all tasks)

- [ ] **Gauntlet:** `./build.sh c89check && ./build.sh && ./build.sh metal` all green.
- [ ] **Unit suite:** `./build.sh multiselecttest && ./multiselect_test` → all passed (ASan/UBSan clean).
- [ ] **Human live-verify the full flow** (board view): shift-click builds a mixed selection (notes+pictures+folders), all glow; marquee selects by touch with live preview; shift+marquee adds; plain empty click clears; group-drag moves all together; Delete removes all; drag-onto-folder files them all keeping layout (navigate in to confirm); single-select (resize/edit/navigate/single-file) all unchanged; **both backends**, no launch-time MSL break (no new shader).
- [ ] **Perf sanity:** idle CPU unchanged; the marquee per-frame projection only runs while dragging.

## Notes for the implementer

- **Read the spec:** `docs/superpowers/specs/2026-06-25-board-multiselect-design.md`.
- **Board view only:** every new behavior is gated on `st->board_view != 0`. `sel_count <= 1` keeps single-select identical.
- **Anchor invariant:** `sel_count==0 ⇒ selected_handle==0`; `sel_count==1 ⇒ selected_handle==sel[0]`. The `sel_*` wrappers enforce it; don't set `selected_handle` directly when the set changes.
- **Handles are stable across `scene_remove`; pointers are not.** Group delete snapshots the set first. Group file restores positions before re-tagging.
- **No new shader** — the highlight reuses `uHighlight`/`hl`; the marquee reuses `ui_quad`/`ui_quad_outline`. No MSL twin.
