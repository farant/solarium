# Per-Frame CPU — Route Cache + O(1) scene_get — Design Spec

**Date:** 2026-06-27
**Status:** Approved (brainstorm), pending implementation plan
**Author:** Fran + Claude

## Goal

Cut per-frame CPU that grows as the scene grows. Symptom: CPU sits ~75% at start and
climbs to ~81–82% as notes are added. Research (see "Background") traced this to two
classes of per-frame waste:

1. **`route_all` re-solved every frame** for doorway labels (a fixed per-frame cost).
2. **`scene_get` is O(N)** and the per-frame hot loops call it O(N) times → several
   **O(N²)** passes per frame, so cost grows quadratically with object (note) count.

This spec covers the two fixes Fran chose to do first — **#3 (route cache)** and
**#1 (O(1) scene_get)** — producing **two independent commits**. A third opportunity
(caching `wtext` glyph geometry) is deferred to a later pass after measuring these.

## Non-Goals

- **No `wtext` glyph-geometry caching** (the deferred #2 — bigger change to the wtext
  module's single shared immediate-mode vbuffer). Revisit after measuring #1+#3.
- **No `bvh_refresh` dirty-skip** (the #4 from research). The O(1)-`scene_get` change
  already removes the quadratic part of `bvh_refresh`; the remaining per-frame box
  recompute for static objects is left for a later pass.
- **No behavioral/visual change.** Doorway labels look identical (≤0.25 s position lag
  only while a room is dragged); `scene_get` keeps its exact contract.
- No change to handle semantics: handles stay monotonic and never reused (`scene.h`).

## Background (current state — verified)

- **`scene_get(s, handle)`** (`scene.c:110`) is a linear scan over `s->objects[0..count)`.
  **`scene_meta_get`** (`scene.c:326`) calls `scene_get` then scans meta.
- **`workspace_of`** (`workspace.c:8`) and **`scene_object_stowed`** (`workspace.c:22`)
  parent-walk, each step doing `scene_meta_get` + `scene_get` (both O(N)).
  **`scene_object_paged_out`** (`workspace.c:48`) does 2× `scene_get`.
  **`scene_object_active`** (`workspace.c:63`) chains stowed + paged_out + (when a named
  workspace is active) workspace_of.
- **`scene_world_matrix`** (`scene.c:145`) parent-walks via `scene_get` each step.
- These are called **once per object, per frame**, in: `bvh_refresh` (`main.c:3187`),
  the main draw loop (`main.c:14954`, `scene_object_active` at 14959 + per-object world
  matrix), and the world-text loops (`main.c:15546+`). Each loop is O(N) iterations ×
  O(N) lookups = **O(N²) per frame**, several times over.
- **The only two mutators of the `objects[]` array layout** are `scene_add`
  (`scene.c:54`, append, `o->handle = next_handle++`) and `scene_remove` (`scene.c:92`,
  `memmove` shift-down preserving order). No `qsort`/reorder exists anywhere (grep-verified).
- **`route_all`** (`route.c:284`) = `collect_rooms` (O(N)) + `routes_pass1` + per-route
  `solve_path`. The code comment (`route.c:314`) states routes should be "computed ONCE
  per rebuild, then queried." The **only per-frame caller** is the doorway-label block
  (`main.c:15661`); all other callers (`main.c:5102, 5202, 10159`) run at rebuild time
  inside mesh resolution.
- `vis_fill` (`main.c:14422`) is already efficient — it queries the BVH frustum, not a
  per-object scan. Not in scope.

---

## Part A — #3: Route cache (first commit)

### Approach: throttle the per-frame solve

Add to `AppState`:

```c
Route   routes[ROUTE_MAX];   /* last doorway-label route solve (cosmetic labels only) */
int     route_count;
double  routes_last_t;       /* glfwGetTime() of the last solve; 0 = never */
```

In the doorway-label block (`main.c` ~15659, currently `route_all(... droutes ...)`):

```c
double now = glfwGetTime();     /* render already samples the clock — see below */
if (state->routes_last_t == 0.0 || now - state->routes_last_t > ROUTE_LABEL_REFRESH_S) {
    state->route_count   = route_all(&state->scene, state->routes, ROUTE_MAX);
    state->routes_last_t = now;
}
/* draw labels from state->routes[0..route_count) */
```

with `#define ROUTE_LABEL_REFRESH_S 0.25` (≈4 solves/sec, mirroring the 0.5 s watcher
cadence at `main.c:16903`). The render function already samples `glfwGetTime()` in
several places (e.g. `resolve_spot_caster(state, (float)glfwGetTime())` at `main.c:14861`),
so calling it once more in the label block is consistent and needs **no new frame-time
plumbing**. Replace the local `droutes[]`/`dn` with the cached
`state->routes`/`state->route_count`.

### Why throttle, not cache-at-rebuild

There is exactly one per-frame consumer, and a throttle can never go *permanently* stale
(always refreshes within 0.25 s). A rebuild-time cache would need invalidation wired into
every route-mutating site (room add/move/connect/descend/resize); a missed site = labels
frozen wrong. The throttle is lower effort and lower risk, with the only visible effect
being ≤0.25 s label-position lag *while a room is actively dragged* in the editor (labels
are static during normal play).

### Data flow

```
render doorway labels: now = glfwGetTime(); if (now - routes_last_t > 0.25) resolve+stamp; draw from cache
room dragged/added/connected (editor/descend): next label refresh (≤0.25 s) picks it up
```

`routes_last_t` starts 0 (zero-initialized AppState), so the first frame solves once and
stamps. No frame-loop changes are needed — the throttle lives entirely in the label block.

### Testing (Part A)

- **Gauntlet:** `./build.sh c89check`, `./build.sh`, `./build.sh metal`.
- **Human live-verify:** doorway labels still render correctly above doors; walking
  between rooms shows labels unchanged; dragging a room in the editor, the label catches
  up within ~0.25 s on release; CPU at a room with doorways is lower than before.
- No new unit test (pure throttle of an existing call; the route solver itself is unchanged).

---

## Part B — #1: O(1) scene_get (second commit)

### Approach: a handle→index map on `Scene`

Add to `Scene` (`scene.h`):

```c
sol_u32 *handle_index;       /* handle -> index into objects[]; (sol_u32)-1 = absent */
sol_u32  handle_index_cap;   /* allocated length (indexable handles [0, cap)) */
sol_bool handle_index_dirty; /* a remove shifted indices; rebuild on next scene_get */
```

`#define SCENE_NO_INDEX ((sol_u32)-1)` in scene.c.

- **`scene_init`** (`scene.c:~20`): `handle_index = NULL; handle_index_cap = 0;
  handle_index_dirty = SOL_FALSE;`.
- **`scene_free`**: `free(s->handle_index);` (with the other frees).
- **`scene_add`** (`scene.c:54`): after `o->handle = s->next_handle++` and `s->count++`,
  ensure `handle_index_cap > o->handle` (grow via realloc — geometric, like `objects`;
  fill new slots with `SCENE_NO_INDEX`), then set `handle_index[o->handle] = s->count - 1`.
  Append never shifts, so this stays incremental — **bulk load is O(N), not O(N²)**.
  (If the realloc fails, set `handle_index_dirty = SOL_TRUE` and continue — `scene_get`
  falls back to a rebuild/scan; never crash.)
- **`scene_remove`** (`scene.c:92`): the `memmove` shifts every later index, so simply set
  `handle_index_dirty = SOL_TRUE` after the shift. (Also fine to leave the removed handle's
  slot; the rebuild overwrites everything.) Removes are rare and already O(N) via memmove.
- **`scene_get`** (`scene.c:110`): 
  ```c
  if (s->handle_index_dirty) scene_reindex(s);   /* O(N), once after a remove */
  if (handle == 0 || handle >= s->handle_index_cap) return NULL;
  { sol_u32 ix = s->handle_index[handle];
    if (ix == SCENE_NO_INDEX || ix >= s->count) return NULL;
    return &s->objects[ix]; }
  ```
- **`scene_reindex(s)`** (new static): grow `handle_index` to ≥ `next_handle` if needed,
  `memset` all to `SCENE_NO_INDEX`, then `for i in [0,count): handle_index[objects[i].handle] = i`.
  Clear `handle_index_dirty`.

### Why dirty-on-remove (not incremental shift-fixup)

A remove memmoves the tail, changing many indices. Patching each shifted entry is fiddly
and easy to get subtly wrong; a single lazy O(N) rebuild on the next `scene_get` is simpler
and provably correct. Removes are infrequent (and never per-frame), so the amortized cost
is negligible. Adds stay incremental (append → one index write) so the load path — thousands
of `scene_add` calls — never degrades to O(N²).

### Safety / invariants

- The map stores **indices, not pointers** → `objects[]` realloc (order-preserving) does
  **not** invalidate it. Only `scene_add`/`scene_remove` change layout; both are handled.
- `scene_get`'s **contract is unchanged**: same return pointer, same "valid until the next
  `scene_add`" lifetime — pure speedup. No call sites change.
- Handle 0 (none/root) → `NULL`, matching today.
- A handle for a removed object → `NULL` (its slot is `SCENE_NO_INDEX` after reindex, or
  the dirty rebuild drops it), matching today's "scan finds nothing."
- `scene_handle_for_nid` (`scene.c:120`, the loader's nid→handle scan) is **out of scope** —
  it keys on `nid`, not `handle`, and runs only at load.

### Effect

Every O(N²) per-frame pass (`bvh_refresh`, the draw loop, the text loops, all the
`scene_object_active`/`scene_world_matrix` parent-walks) collapses to O(N) with **zero
call-site edits** — they all bottom out in `scene_get`, now O(1).

### Testing (Part B)

- **New unit test** `scene_test.c` + a `scenetest` `build.sh` target (reusing the `iotest`
  link set — `scene.c material.c scene_io.c mesh.c flora.c rock.c gothic.c sweep.c texgen.c
  mirror.c platform_fs.c component.c particles.c nid.c stml.c sol_math.c` — under ASan/UBSan
  like the other `*test` targets), GL-free:
  - add N objects → `scene_get(h)` returns the object whose `->handle == h` for every h.
  - force a realloc (add past initial capacity) → all prior handles still resolve.
  - remove a middle object → every surviving handle still resolves to the right object;
    the removed handle returns `NULL`; a subsequent add resolves correctly.
  - `scene_get(0)` and an out-of-range/never-issued handle return `NULL`.
  - interleave add/remove/add and assert `scene_get` agrees with a linear-scan oracle.
- **Gauntlet:** `./build.sh c89check`, `./build.sh`, `./build.sh metal`.
- **Human live-verify:** the palace loads and renders identically; place/drag/delete
  notes, switch workspaces, descend, board-view — all behave exactly as before; CPU at a
  note-dense board is lower and **no longer climbs** as notes are added.

## Risks

- **A missed layout mutator.** Mitigation: grep confirmed only `scene_add`/`scene_remove`
  touch `objects[]` order (no `qsort`/swap). If a future path reorders in place, it must set
  `handle_index_dirty`. Documented at the struct field.
- **`handle_index_cap` growth.** Sized to `next_handle` (monotonic), like `vis[]` already
  is (`main.c:14423`). Over a long churny session it tracks total-ever-created, not live
  count — same trade-off the existing `vis[]` accepts; memory is one `sol_u32` per handle.
- **Throttle staleness (Part A).** Bounded at 0.25 s and only visible during an active room
  drag; acceptable for cosmetic labels.
- **Two independent commits.** Part A and Part B don't depend on each other; either can land
  first. Order: A then B (A is smaller, lets us measure the route-cache delta in isolation).

## File Touch List

- **Part A:** `main.c` — AppState fields (`routes`, `route_count`, `routes_last_t`);
  rewrite the doorway-label block to throttle (`glfwGetTime()` in-block) + read the cache;
  `#define ROUTE_LABEL_REFRESH_S`.
- **Part B:** `scene.h` (3 Scene fields), `scene.c` (`scene_init`, `scene_free`,
  `scene_add`, `scene_remove`, `scene_get`, new `scene_reindex`, `SCENE_NO_INDEX`),
  new `scene_test.c` + `scenetest` target in `build.sh`.

## Testing Summary

Both parts: the three-build gauntlet (`c89check` / GL / `metal`). Part B adds a scene-level
unit test. Human live-verify after each commit — measure CPU at app start and as notes are
added (the headline metric: the climb should flatten).
