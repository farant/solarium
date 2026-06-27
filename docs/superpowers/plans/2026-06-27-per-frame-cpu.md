# Per-Frame CPU — Route Cache + O(1) scene_get — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cut per-frame CPU that grows with object (note) count by (A) throttling the per-frame `route_all` solve behind the doorway labels, and (B) making `scene_get` O(1) via a handle→index map so the per-frame O(N²) passes collapse to O(N).

**Architecture:** Two independent, sequential commits. **Part A** caches the doorway-label route solution in `AppState`, refreshing it at most ~4×/sec. **Part B** adds a `handle→index` map to `Scene`, maintained incrementally on `scene_add` (append) and rebuilt lazily on `scene_remove` (the memmove shift sets a dirty flag); `scene_get` reindexes on demand then does an O(1) lookup. No behavior or visual change; `scene_get`'s contract is preserved.

**Tech Stack:** C89 core (`scene.c`/`scene.h`/`main.c`, gauntlet `./build.sh c89check` + `./build.sh` + `./build.sh metal`), C11 test (`scene_test.c` under ASan/UBSan via a new `build.sh scenetest` target). Spec: `docs/superpowers/specs/2026-06-27-per-frame-cpu-design.md`.

**Important house rules:** strict C89 for `scene.c`/`main.c` (declarations at the top of each block, no `//` comments, no mid-block declarations). Never `git add NOTES.stml` or `paper-picture.png`. Commit bodies end with the `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>` line. Work on a feature branch off `main`.

---

## Task 1: Route-label cache (Part A) — one commit

**Files:**
- Modify: `main.c` — `AppState` struct (~2666), a new `#define` (~4367), the doorway-label block (~15659–15692).

There is no headless test for this (it is render-loop code). Verification is the build gauntlet plus human live-verify. This is a behavior-preserving throttle of an existing call.

- [ ] **Step 1: Add the route cache fields to `AppState`**

In `main.c`, find the visibility-cache fields (around line 2665–2666):

```c
    unsigned char *vis;          /* per-pass visibility, handle-indexed (piece 4) */
    sol_u32        vis_cap;
```

Insert immediately after `sol_u32 vis_cap;`:

```c
    /* doorway-label routes (per-frame CPU spec, Part A): route_all is too costly
       to run every frame, so solve at most ~4x/sec and reuse the result. Labels
       are cosmetic; a room only moves in the editor, where a <=0.25s lag is
       imperceptible. routes_last_t = 0 (zero-init AppState) forces a frame-1 solve. */
    Route   routes[ROUTE_MAX];
    int     route_count;
    double  routes_last_t;
```

(`Route` and `ROUTE_MAX` are in scope — `route.h` is included at `main.c:32`, well before `AppState`.)

- [ ] **Step 2: Add the refresh-interval constant**

In `main.c`, find:

```c
#define BOARD_VIEW_MARGIN   1.10f   /* fill the FOV to the board + a little air */
```

Insert immediately after it:

```c
#define ROUTE_LABEL_REFRESH_S 0.25  /* doorway-label routes resolve at most ~4x/sec */
```

- [ ] **Step 3: Throttle the doorway-label block to read the cache**

In `main.c`, the doorway-label block currently begins (around line 15659):

```c
        {
            Route droutes[ROUTE_MAX];
            int   dn = route_all(&state->scene, droutes, ROUTE_MAX);
            int   di, dside;
            for (di = 0; di < dn; di++) {
                Route *r = &droutes[di];
```

Replace exactly those six lines with:

```c
        {
            double rnow = glfwGetTime();   /* render already samples the clock (see 14861) */
            int    di, dside;
            if (state->routes_last_t == 0.0 ||
                rnow - state->routes_last_t > ROUTE_LABEL_REFRESH_S) {
                state->route_count   = route_all(&state->scene, state->routes, ROUTE_MAX);
                state->routes_last_t = rnow;
            }
            for (di = 0; di < state->route_count; di++) {
                Route *r = &state->routes[di];
```

Leave the rest of the loop body unchanged — `dn` and `droutes` are referenced nowhere else in the block (verified: only the loop header used them).

- [ ] **Step 4: Run the build gauntlet**

```bash
./build.sh c89check && ./build.sh && ./build.sh metal
```

Expected: `c89check: PASS …`, `built ./solarium (debug)`, `built ./solarium-metal …`. No warnings/errors.

- [ ] **Step 5: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
Perf: cache doorway-label routes (~4x/sec, not every frame)

route_all (collect_rooms + per-route solve_path) was re-solved every
frame in render just to place doorway labels — the code's own comment
(route.c:314) says routes should be computed once per rebuild and
queried. Cache the solve in AppState and refresh it at most every
0.25s (ROUTE_LABEL_REFRESH_S). Labels are cosmetic; a room only moves
in the editor, where a <=0.25s position lag is imperceptible. No
visual change in normal play.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Characterization test for `scene_get` (Part B, step 1 of 2) — one commit

**Files:**
- Create: `scene_test.c`
- Modify: `build.sh` — add a `scenetest` target after the `iotest` block (~line 65).

This test pins down `scene_get`'s exact contract **against the current O(N) implementation**, so it guards the Task 3 refactor. It is expected to PASS on the current code (that proves the oracle is right); it must keep passing after Task 3.

- [ ] **Step 1: Create `scene_test.c`**

Create `scene_test.c` with this exact content:

```c
/* scene_test.c — characterization + regression test for scene_get. Pins its
   contract (handle -> the object whose ->handle matches; NULL for 0, removed,
   or never-issued handles) across add / realloc / remove / churn, so the O(1)
   handle->index refactor stays behavior-preserving. Built by
   `build.sh scenetest` under ASan/UBSan; GL-free (empties only, zero Mesh). */

#include "scene.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

static vec3 v3(float x, float y, float z) { vec3 v; v.x = x; v.y = y; v.z = z; return v; }
static quat qid(void) { quat q; q.x = 0.0f; q.y = 0.0f; q.z = 0.0f; q.w = 1.0f; return q; }

static sol_u32 add_empty(Scene *s) {
    Mesh m;
    memset(&m, 0, sizeof m);
    return scene_add(s, 0, m, v3(0.0f, 0.0f, 0.0f), qid(), v3(1.0f, 1.0f, 1.0f));
}

static void test_add_get(void) {
    Scene   s;
    sol_u32 h[5];
    int     i;
    scene_init(&s);
    for (i = 0; i < 5; i++) h[i] = add_empty(&s);
    for (i = 0; i < 5; i++) {
        SceneObject *o = scene_get(&s, h[i]);
        CHECK(o != NULL && o->handle == h[i], "add/get: each handle resolves to its object");
    }
    scene_free(&s);
}

static void test_realloc(void) {
    Scene   s;
    sol_u32 h[40];
    int     i;
    scene_init(&s);
    for (i = 0; i < 40; i++) h[i] = add_empty(&s);   /* past the initial cap (16) */
    for (i = 0; i < 40; i++) {
        SceneObject *o = scene_get(&s, h[i]);
        CHECK(o != NULL && o->handle == h[i], "realloc: handles survive objects[] growth");
    }
    scene_free(&s);
}

static void test_remove_middle(void) {
    Scene   s;
    sol_u32 h[5];
    int     i;
    scene_init(&s);
    for (i = 0; i < 5; i++) h[i] = add_empty(&s);    /* handles 1..5 */
    scene_remove(&s, h[2]);                          /* drop the middle one */
    CHECK(scene_get(&s, h[2]) == NULL, "remove: removed handle -> NULL");
    for (i = 0; i < 5; i++) {
        SceneObject *o;
        if (i == 2) continue;
        o = scene_get(&s, h[i]);
        CHECK(o != NULL && o->handle == h[i], "remove: survivors still resolve correctly");
    }
    {
        sol_u32      hn = add_empty(&s);             /* add after a remove */
        SceneObject *o  = scene_get(&s, hn);
        CHECK(o != NULL && o->handle == hn, "remove+add: new handle resolves");
    }
    scene_free(&s);
}

static void test_zero_and_oob(void) {
    Scene s;
    scene_init(&s);
    (void)add_empty(&s);
    CHECK(scene_get(&s, 0) == NULL, "get(0) -> NULL (root/none)");
    CHECK(scene_get(&s, 99999u) == NULL, "get(never-issued) -> NULL");
    scene_free(&s);
}

/* a linear-scan oracle: does the scene contain `handle`, and at what index? */
static int oracle_present(Scene *s, sol_u32 handle, sol_u32 *out_index) {
    sol_u32 i;
    for (i = 0; i < s->count; i++)
        if (s->objects[i].handle == handle) {
            if (out_index) *out_index = i;
            return 1;
        }
    return 0;
}

/* churn: an interleave of add + remove; after every step, scene_get must agree
   with the oracle for EVERY handle ever issued (present -> exact pointer; absent
   -> NULL). This is the strong guard on the index map's correctness. */
static void test_churn_oracle(void) {
    Scene   s;
    sol_u32 hs[64];
    int     n = 0, step;
    scene_init(&s);
    for (step = 0; step < 200; step++) {
        if ((step % 3) != 0 || n == 0) {             /* mostly add, sometimes remove */
            if (n < 64) hs[n++] = add_empty(&s);
        } else {
            int k = step % n;                        /* deterministic victim */
            scene_remove(&s, hs[k]);
            hs[k] = hs[--n];                         /* compact the shadow list */
        }
        {
            sol_u32 h;
            for (h = 1; h < s.next_handle; h++) {
                sol_u32      oi = 0;
                int          present = oracle_present(&s, h, &oi);
                SceneObject *o = scene_get(&s, h);
                if (present)
                    CHECK(o == &s.objects[oi] && o->handle == h,
                          "churn: get matches oracle (present)");
                else
                    CHECK(o == NULL, "churn: get matches oracle (absent)");
            }
        }
    }
    scene_free(&s);
}

int main(void) {
    test_add_get();
    test_realloc();
    test_remove_middle();
    test_zero_and_oob();
    test_churn_oracle();
    if (fails == 0) printf("scene_test: all passed\n");
    return fails ? 1 : 0;
}
```

- [ ] **Step 2: Add the `scenetest` target to `build.sh`**

In `build.sh`, find the end of the `iotest` block (around line 63–65):

```sh
    echo "built ./scene_io_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi
```

Insert immediately after that `fi`:

```sh

# Build + run the headless scene handle-map test under the sanitizers. Same link
# set as iotest (scene.c + deps, no GL): it churns the graph and checks scene_get.
if [ "$MODE" = "scenetest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        scene.c material.c scene_io.c mesh.c flora.c rock.c gothic.c sweep.c texgen.c mirror.c platform_fs.c component.c particles.c nid.c stml.c sol_math.c scene_test.c \
        -o scene_test
    echo "built ./scene_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi
```

- [ ] **Step 3: Build and run the test against the CURRENT (unchanged) `scene.c`**

```bash
./build.sh scenetest && ./scene_test
```

Expected: builds clean under ASan/UBSan, and prints `scene_test: all passed` with no sanitizer output. (This passes on the current O(N) `scene_get` — that is the point: it establishes the oracle is correct and will guard the Task 3 refactor.)

- [ ] **Step 4: Confirm the main gauntlet is unaffected**

```bash
./build.sh c89check
```

Expected: `c89check: PASS …` (the test TU is C11 and not part of c89check; this just confirms nothing else broke).

- [ ] **Step 5: Commit**

```bash
git add scene_test.c build.sh
git commit -m "$(cat <<'EOF'
scenetest: characterize scene_get's contract (add/realloc/remove/churn)

A GL-free ASan/UBSan test that pins scene_get to: handle -> the object
whose ->handle matches; NULL for 0, removed, or never-issued handles.
Includes a churn-vs-linear-scan-oracle check. Passes on the current
O(N) scene_get; guards the O(1) handle->index refactor that follows.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: O(1) `scene_get` via a handle→index map (Part B, step 2 of 2) — one commit

**Files:**
- Modify: `scene.h` — 3 new `Scene` fields (~line 99).
- Modify: `scene.c` — `SCENE_NO_INDEX` macro, `scene_index_reserve` + `scene_reindex` helpers, and edits to `scene_init`, `scene_free`, `scene_add`, `scene_remove`, `scene_get`.
- The Task 2 `scene_test` is the regression guard (must still pass).

- [ ] **Step 1: Add the map fields to the `Scene` struct**

In `scene.h`, the `Scene` struct currently ends:

```c
    char     active_ws[SOL_WS_NAME_CAP];  /* runtime view filter: the workspace
                          currently shown; "" = unfiltered (show all). NEVER
                          serialized — reset on load, set by the app. */
} Scene;
```

Insert the three fields before the closing `} Scene;`:

```c
    char     active_ws[SOL_WS_NAME_CAP];  /* runtime view filter: the workspace
                          currently shown; "" = unfiltered (show all). NEVER
                          serialized — reset on load, set by the app. */
    /* handle -> objects[] index, for O(1) scene_get (per-frame CPU spec, Part B).
       Sized >= next_handle; SCENE_NO_INDEX = absent. scene_add keeps it current
       (append never shifts); scene_remove sets _dirty (its memmove shifts every
       later index) and the next scene_get rebuilds once. ONLY scene_add/
       scene_remove reorder objects[] — a future in-place reorder must set _dirty. */
    sol_u32 *handle_index;
    sol_u32  handle_index_cap;
    sol_bool handle_index_dirty;
} Scene;
```

- [ ] **Step 2: Add the `SCENE_NO_INDEX` macro and the two helpers to `scene.c`**

In `scene.c`, immediately after the `#include` lines at the top of the file, add:

```c
#define SCENE_NO_INDEX ((sol_u32)-1)   /* "no object for this handle" sentinel */
```

Then, immediately **above** the `scene_get` definition (currently at `scene.c:110`), add both helpers:

```c
/* Ensure handle_index can address handles in [0, cap_needed), filling new slots
   with SCENE_NO_INDEX. On OOM, marks the map dirty and returns 0 so the caller
   skips the incremental write (scene_get then falls back to a linear scan). */
static int scene_index_reserve(Scene *s, sol_u32 cap_needed) {
    sol_u32  ncap, k;
    sol_u32 *ni;
    if (cap_needed <= s->handle_index_cap) return 1;
    ncap = s->handle_index_cap ? s->handle_index_cap : 16;
    while (ncap < cap_needed) ncap *= 2;
    ni = (sol_u32 *)realloc(s->handle_index, (size_t)ncap * sizeof *ni);
    if (!ni) { s->handle_index_dirty = SOL_TRUE; return 0; }
    for (k = s->handle_index_cap; k < ncap; k++) ni[k] = SCENE_NO_INDEX;
    s->handle_index     = ni;
    s->handle_index_cap = ncap;
    return 1;
}

/* Rebuild handle_index from objects[] (O(N)); called lazily after a remove. */
static void scene_reindex(Scene *s) {
    sol_u32 i;
    if (!scene_index_reserve(s, s->next_handle)) return;  /* OOM: stay dirty (scan path) */
    for (i = 0; i < s->handle_index_cap; i++) s->handle_index[i] = SCENE_NO_INDEX;
    for (i = 0; i < s->count; i++) s->handle_index[s->objects[i].handle] = i;
    s->handle_index_dirty = SOL_FALSE;
}
```

- [ ] **Step 3: Initialize and free the map**

In `scene.c`, `scene_init` currently ends:

```c
    s->next_handle = 1;       /* 0 reserved for "none" / root */
    s->active_ws[0] = '\0';
}
```

Add the three field inits before the closing brace:

```c
    s->next_handle = 1;       /* 0 reserved for "none" / root */
    s->active_ws[0] = '\0';
    s->handle_index       = NULL;   /* O(1) scene_get map: lazily grown */
    s->handle_index_cap   = 0;
    s->handle_index_dirty = SOL_FALSE;
}
```

In `scene.c`, `scene_free` currently ends:

```c
    free(s->objects);
    s->objects = NULL;
    s->count = 0;
    s->capacity = 0;
}
```

Add the map teardown before the closing brace:

```c
    free(s->objects);
    s->objects = NULL;
    s->count = 0;
    s->capacity = 0;
    free(s->handle_index);
    s->handle_index       = NULL;
    s->handle_index_cap   = 0;
    s->handle_index_dirty = SOL_FALSE;
}
```

- [ ] **Step 4: Maintain the map incrementally in `scene_add`**

In `scene.c`, `scene_add` begins:

```c
sol_u32 scene_add(Scene *s, sol_u32 parent, Mesh mesh, vec3 pos, quat rot, vec3 scale) {
    SceneObject *o;
    if (s->count == s->capacity) {
```

Add an `h` local to the declarations:

```c
sol_u32 scene_add(Scene *s, sol_u32 parent, Mesh mesh, vec3 pos, quat rot, vec3 scale) {
    SceneObject *o;
    sol_u32      h;
    if (s->count == s->capacity) {
```

`scene_add` ends:

```c
    o->overlay_clip  = -1;                          /* -1 = no animation override */
    o->overlay_speed = 1.0f;
    return o->handle;
}
```

Replace those last three lines with:

```c
    o->overlay_clip  = -1;                          /* -1 = no animation override */
    o->overlay_speed = 1.0f;
    /* keep the O(1) map current: an append never shifts existing indices, so a
       single write suffices (so bulk load stays O(N), never O(N^2)). If the map
       is already dirty, skip — the next scene_get reindexes everything. */
    h = o->handle;
    if (!s->handle_index_dirty && scene_index_reserve(s, h + 1))
        s->handle_index[h] = s->count - 1;
    return o->handle;
}
```

- [ ] **Step 5: Mark the map dirty in `scene_remove`**

In `scene.c`, `scene_remove`'s inner block currently is:

```c
            scene_object_free(&s->objects[i]);
            if (i + 1 < s->count) {
                memmove(&s->objects[i], &s->objects[i + 1],
                        (size_t)(s->count - i - 1) * sizeof(SceneObject));
            }
            s->count--;
            return;
```

Replace with (add the dirty flag before `return`):

```c
            scene_object_free(&s->objects[i]);
            if (i + 1 < s->count) {
                memmove(&s->objects[i], &s->objects[i + 1],
                        (size_t)(s->count - i - 1) * sizeof(SceneObject));
            }
            s->count--;
            s->handle_index_dirty = SOL_TRUE;   /* the memmove shifted every later index */
            return;
```

- [ ] **Step 6: Make `scene_get` O(1)**

In `scene.c`, replace the entire `scene_get` body:

```c
SceneObject *scene_get(Scene *s, sol_u32 handle) {
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        if (s->objects[i].handle == handle) return &s->objects[i];
    }
    return NULL;
}
```

with:

```c
SceneObject *scene_get(Scene *s, sol_u32 handle) {
    if (handle == 0) return NULL;                    /* 0 = none/root */
    if (s->handle_index_dirty) scene_reindex(s);     /* O(N), once after a remove */
    if (!s->handle_index_dirty && handle < s->handle_index_cap) {
        sol_u32 ix = s->handle_index[handle];
        if (ix == SCENE_NO_INDEX || ix >= s->count) return NULL;
        return &s->objects[ix];
    }
    {   /* fallback: reindex OOM (still dirty) or a handle beyond the map */
        sol_u32 i;
        for (i = 0; i < s->count; i++)
            if (s->objects[i].handle == handle) return &s->objects[i];
    }
    return NULL;
}
```

- [ ] **Step 7: Run the regression test (must still pass)**

```bash
./build.sh scenetest && ./scene_test
```

Expected: `scene_test: all passed`, no ASan/UBSan output. (Same test as Task 2; it now exercises the O(1) path including the dirty-reindex-after-remove and the churn oracle.)

- [ ] **Step 8: Run the full build gauntlet**

```bash
./build.sh c89check && ./build.sh && ./build.sh metal
```

Expected: `c89check: PASS …`, `built ./solarium (debug)`, `built ./solarium-metal …`. No warnings/errors. (Watch for any C89/`-pedantic-errors` issue in the new `scene.c` code — all declarations are at block tops, no `//` comments.)

- [ ] **Step 9: Commit**

```bash
git add scene.h scene.c
git commit -m "$(cat <<'EOF'
Perf: O(1) scene_get via a handle->index map

scene_get was an O(N) linear scan, and the per-frame hot loops
(bvh_refresh, the draw loop, the world-text loops) call it — and the
helpers built on it (scene_meta_get, scene_world_matrix,
scene_object_active) — O(N) times each, making several O(N^2) passes
per frame. Cost grew quadratically with object (note) count.

Add a handle->index map to Scene: maintained incrementally on
scene_add (append never shifts, so one write keeps bulk load O(N)),
and rebuilt lazily on scene_remove (its memmove shift sets a dirty
flag; the next scene_get reindexes once). scene_get becomes O(1) with
an unchanged contract. Guarded by scene_test (add/realloc/remove/churn
vs a linear-scan oracle). No call sites change.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-Review notes (for the implementer)

- **Spec coverage:** Task 1 = spec Part A (route throttle, `ROUTE_LABEL_REFRESH_S` 0.25s, cache in AppState, `glfwGetTime()` in-block). Tasks 2–3 = spec Part B (handle→index map: incremental add, dirty-on-remove, lazy reindex, O(1) get, sentinel `(sol_u32)-1`, init/free, `scenetest` reusing the iotest link set). Non-goals (wtext cache, bvh dirty-skip) are untouched.
- **Type consistency:** field names are exactly `routes` / `route_count` / `routes_last_t` (Task 1) and `handle_index` / `handle_index_cap` / `handle_index_dirty` (Task 3); helpers `scene_index_reserve` / `scene_reindex`; macros `ROUTE_LABEL_REFRESH_S` / `SCENE_NO_INDEX`. Used identically wherever referenced.
- **Order independence:** Task 1 (main.c) and Tasks 2–3 (scene.*) touch disjoint files; either part can land first, but do Task 1 → 2 → 3 so each commit is measurable on its own.
- **C89:** every new declaration sits at the top of its block; no `//` comments; `(sol_u32)-1` and `realloc` casts are C89-clean. `scene_test.c` is C11 (its own target), not part of c89check.
- **Human live-verify (after the branch is built):** (1) doorway labels render and read correctly while walking between rooms; (2) place/drag/delete notes, switch workspaces, descend, enter board view — all behave exactly as before; (3) measure CPU at app start and as notes are added — the climb should flatten. Report the before/after CPU numbers.
