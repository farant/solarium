# Walkways Take 2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace fs-tree walkways with single-bend right-angle L-paths that enter/exit rooms perpendicular through real doorways, driven by one shared routing TU so paths and doors never disagree (and the disappearing-walkway bug dies).

**Architecture:** A new pure-CPU `route.c` is the single author of connection geometry: given the scene's rooms + `connects` graph it computes, per edge, which wall each room opens, the door centers (spread when several share a wall), the L corner, and the rise. Two readers consume it — `main.c connections_rebuild` (builds walkway + room-shell meshes) and `collide.c` (builds the matching colliders). New mesh builders `make_room_doored` (thick walls built around N door gaps) and `make_walkway_L` (two axis-aligned stepped legs + a corner landing) do the geometry; nothing is stored, everything re-derives each load.

**Tech Stack:** Strict C89 (`build.sh c89check` is `-std=c89 -pedantic-errors -Werror`), hand-written `build.sh`, no GL in the new code (pure CPU, links into the headless `*test` builds). Verification per task = the build gauntlet `./build.sh c89check && ./build.sh debug && ./build.sh metal`, plus a new `route_test` (ASan/UBSan) for the routing math, plus `collide_test` for Task 5. There is **no per-frame GUI test harness** — interactive verification is the human's job (handoff checklist in Task 6).

**C89 discipline (applies to every code block below):** all declarations at the top of their block; no `//` comments; no mixed declaration/statement. The new code is CPU-only — **no shader/MSL twin** is involved.

---

## File Structure

- `mesh.h` / `mesh.c` — add `RoomOpening` (pure-geometry struct), `make_room_doored`, `make_walkway_L`, and two static helpers (`aabb_box`, `emit_doored_wall`, `walkway_leg`). Switch `emit_room` to build a thick closed room. *(Tasks 1–2.)*
- `route.h` / `route.c` — **new TU.** The routing author: `Route`/`route_all`/`route_for_walkway`/`route_room_openings`. Scene-level, no GL. *(Task 3.)*
- `route_test.c` — **new.** Headless asserts for the routing math (ASan/UBSan), wired as `build.sh routetest`. *(Task 3.)*
- `build.sh` — add `route.c` to the 4 full-build TU lists + the `collidetest` list; add a `routetest` mode. *(Task 3.)*
- `main.c` — replace `walkways_rebuild` with `connections_rebuild` (reads `route.c`, builds walkway + room meshes); swap the two call sites. *(Task 4.)*
- `collide.c` — rewrite the `room` case (floor + doored walls via `route_room_openings`) and the `walkway` case (L legs + landing via `route_for_walkway`); include `route.h`. *(Task 5.)*

Door constants (shared): **width 1.4 m, height 2.1 m**. Wall thickness **t = 0.20 m** (matches `COLLIDE_SHELL_T`). These live as `#define`s in `route.h` so both readers agree (Task 3).

---

## Task 1: `make_room_doored` — thick walls built around door gaps

**Files:**
- Modify: `mesh.h` (after the `make_walkway` decl, ~line 55)
- Modify: `mesh.c` (add helpers + builder near `make_wall_with_opening`, ~line 288; change `emit_room`, line 360)

- [ ] **Step 1: Declare the geometry struct + builder in `mesh.h`**

Add after the `make_walkway` declaration (mesh.h:55):

```c
/* wall ids for a doored room: N=-z, E=+x, S=+z, W=-x (matches make_room) */
#define ROOM_WALL_N 0
#define ROOM_WALL_E 1
#define ROOM_WALL_S 2
#define ROOM_WALL_W 3

/* A gap in a room wall. `center` is the door-center offset along the wall's
   run axis, in ROOM-LOCAL coords (x for N/S walls, z for E/W walls); width
   and height are the opening size. Pure geometry — no scene knowledge. */
typedef struct {
    int   wall;
    float center;
    float width;
    float height;
} RoomOpening;

/* A thick-walled room built AROUND its door gaps (never CSG): floor always,
   ceiling if `ceil`, a thick slab per present wall (wn/we/ws/ww) with the
   openings for that wall left as gaps (piers + headers + thresholds). */
void make_room_doored(MeshBuilder *b, sol_f32 w, sol_f32 d, sol_f32 h, sol_f32 t,
                      int wn, int we, int ws, int ww, int ceil,
                      const RoomOpening *ops, int n_ops);
```

- [ ] **Step 2: Add the static box + doored-wall helpers in `mesh.c`**

Insert just BELOW `make_wall_with_opening` (after mesh.c:288, before the registry comment at ~342). `face_x`/`face_y`/`face_z` already exist above this point.

```c
/* an axis-aligned solid box, all six faces (CCW from each normal). Cheap and
   robust: abutting boxes meet back-to-back (opposite-facing coplanar quads
   never z-fight), so doored walls can be assembled from whole boxes without
   the face-skipping bookkeeping make_wall_with_opening does by hand. */
static void aabb_box(MeshBuilder *b, sol_f32 x0, sol_f32 x1, sol_f32 y0,
                     sol_f32 y1, sol_f32 z0, sol_f32 z1) {
    face_z(b, x0, x1, y0, y1, z1,  1);
    face_z(b, x0, x1, y0, y1, z0, -1);
    face_x(b, x1, y0, y1, z0, z1,  1);
    face_x(b, x0, y0, y1, z0, z1, -1);
    face_y(b, x0, x1, y1, z0, z1,  1);
    face_y(b, x0, x1, y0, z0, z1, -1);
}

/* one thick room wall, built around its gaps. runx=1: the wall runs along X
   (N/S walls), its two faces at z=f0,f1, span [s0,s1] in x. runx=0: runs
   along Z (E/W walls), faces at x=f0,f1, span [s0,s1] in z. `ops` is the full
   room list; only entries with .wall==wall_id are used (their .center is in
   the run axis). Up to 8 gaps per wall. */
static void emit_doored_wall(MeshBuilder *b, int runx, sol_f32 f0, sol_f32 f1,
                             sol_f32 s0, sol_f32 s1, sol_f32 h,
                             const RoomOpening *ops, int n_ops, int wall_id) {
    sol_f32 lo[8], hi[8], oy[8];
    int     k = 0, i, j;
    sol_f32 cur;
    for (i = 0; i < n_ops; i++) {
        sol_f32 c, hwid;
        if (ops[i].wall != wall_id) continue;
        if (k >= 8) break;
        c = ops[i].center; hwid = ops[i].width * 0.5f;
        lo[k] = c - hwid; hi[k] = c + hwid; oy[k] = ops[i].height;
        k++;
    }
    for (i = 1; i < k; i++) {                 /* insertion sort by lo */
        sol_f32 a = lo[i], bb = hi[i], cc = oy[i];
        j = i - 1;
        while (j >= 0 && lo[j] > a) {
            lo[j + 1] = lo[j]; hi[j + 1] = hi[j]; oy[j + 1] = oy[j]; j--;
        }
        lo[j + 1] = a; hi[j + 1] = bb; oy[j + 1] = cc;
    }
    cur = s0;
    for (i = 0; i <= k; i++) {
        sol_f32 gL = (i < k) ? lo[i] : s1;
        sol_f32 gR = (i < k) ? hi[i] : s1;
        if (gL < s0) gL = s0;
        if (gR > s1) gR = s1;
        if (gL > cur) {                        /* solid pier [cur, gL] */
            if (runx) aabb_box(b, cur, gL, 0.0f, h, f0, f1);
            else      aabb_box(b, f0, f1, 0.0f, h, cur, gL);
        }
        if (i < k) {
            if (oy[i] < h) {                   /* header above the gap */
                if (runx) aabb_box(b, gL, gR, oy[i], h, f0, f1);
                else      aabb_box(b, f0, f1, oy[i], h, gL, gR);
            }
            if (runx) face_y(b, gL, gR, 0.0f, f0, f1, 1);   /* threshold top */
            else      face_y(b, f0, f1, 0.0f, gL, gR, 1);
            cur = gR;
        }
    }
}
```

- [ ] **Step 3: Add `make_room_doored` in `mesh.c`** (immediately below `emit_doored_wall`)

```c
void make_room_doored(MeshBuilder *b, sol_f32 w, sol_f32 d, sol_f32 h, sol_f32 t,
                      int wn, int we, int ws, int ww, int ceil,
                      const RoomOpening *ops, int n_ops) {
    sol_f32 hw = w * 0.5f, hd = d * 0.5f;
    if (t < 0.02f) t = 0.02f;
    aabb_box(b, -hw, hw, -t, 0.0f, -hd, hd);                 /* floor, top at y=0 */
    if (ceil) aabb_box(b, -hw, hw, h, h + t, -hd, hd);
    if (wn) emit_doored_wall(b, 1, -hd - t, -hd, -hw, hw, h, ops, n_ops, ROOM_WALL_N);
    if (ws) emit_doored_wall(b, 1,  hd, hd + t, -hw, hw, h, ops, n_ops, ROOM_WALL_S);
    if (we) emit_doored_wall(b, 0,  hw, hw + t, -hd - t, hd + t, h, ops, n_ops, ROOM_WALL_E);
    if (ww) emit_doored_wall(b, 0, -hw - t, -hw, -hd - t, hd + t, h, ops, n_ops, ROOM_WALL_W);
}
```

- [ ] **Step 4: Switch `emit_room` to a thick closed room** (mesh.c:360)

Replace the body of `emit_room` so the default/registry-built shell is now thick (closed — no openings; `connections_rebuild` re-derives the doored mesh at runtime):

```c
static void emit_room(MeshBuilder *b, const float *p) {
    make_room_doored(b, p[0], p[1], p[2], 0.20f,
                     p[3] > 0.5f, p[4] > 0.5f, p[5] > 0.5f, p[6] > 0.5f,
                     p[7] > 0.5f, (const RoomOpening *)0, 0);
}
```

Leave `make_room` (the old flat builder) in place — it is still referenced by `emit_room`? No: after this change `emit_room` no longer calls `make_room`. Check for other callers: `grep -n "make_room\b" *.c *.h`. If `make_room` has no remaining callers, delete its body + decl (mesh.c:154-186, mesh.h decl) to avoid an `-Werror` unused warning is NOT triggered for an exported function, but dead code should go. If anything else still calls it, leave it.

- [ ] **Step 5: Build gauntlet**

```bash
./build.sh c89check && ./build.sh debug && ./build.sh metal
```
Expected: all three succeed with no warnings. (Rooms now render as thick solid shells; no doorways yet — those arrive in Task 4.)

- [ ] **Step 6: Commit**

```bash
git add mesh.c mesh.h
git commit -m "$(printf 'feat: make_room_doored — thick room walls built around door gaps\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 2: `make_walkway_L` — two stepped legs + a corner landing

**Files:**
- Modify: `mesh.h` (after the `make_room_doored` decl)
- Modify: `mesh.c` (below `make_walkway`, ~line 340)

- [ ] **Step 1: Declare in `mesh.h`**

```c
/* An L-shaped (or straight) walkway in LOCAL space: the lower door is the
   origin (0,0,0); the path bends at (cx,cz,cy) and ends at (ex,ez,ey). Each
   leg is axis-aligned and stepped to climb to its end height; a flat landing
   caps the corner (skipped when a leg is zero-length, i.e. a straight path). */
void make_walkway_L(MeshBuilder *b, sol_f32 cx, sol_f32 cz, sol_f32 cy,
                    sol_f32 ex, sol_f32 ez, sol_f32 ey, sol_f32 w, sol_f32 t);
```

- [ ] **Step 2: Add the leg helper + builder in `mesh.c`** (below `make_walkway`, before the registry comment)

```c
/* one axis-aligned stepped ribbon from (x0,z0,y0) to (x1,z1,y1). The run is
   along whichever of x/z changes; the other holds. Steps climb to the end y
   in WALKWAY_STEP_RISE increments (one box per step). */
static void walkway_leg(MeshBuilder *b, sol_f32 x0, sol_f32 z0, sol_f32 y0,
                        sol_f32 x1, sol_f32 z1, sol_f32 y1, sol_f32 w, sol_f32 t) {
    sol_f32 dx = x1 - x0, dz = z1 - z0, dy = y1 - y0;
    sol_f32 len = (sol_f32)sqrt((double)(dx * dx + dz * dz));
    sol_f32 hw = w * 0.5f, ady = (dy < 0.0f) ? -dy : dy;
    int     n, i, run_x;
    if (len < 1e-4f) return;
    run_x = ((dx < 0.0f ? -dx : dx) >= (dz < 0.0f ? -dz : dz));
    n = (ady < 0.02f) ? 1 : (int)(ady / WALKWAY_STEP_RISE) + 1;
    if (n < 1) n = 1;
    if (n > 128) n = 128;
    for (i = 0; i < n; i++) {
        sol_f32 a0 = (sol_f32)i / (sol_f32)n, a1 = (sol_f32)(i + 1) / (sol_f32)n;
        sol_f32 sx = x0 + dx * a0, sz = z0 + dz * a0;
        sol_f32 tx = x0 + dx * a1, tz = z0 + dz * a1;
        sol_f32 yd = y0 + dy * a1;            /* this step's deck top */
        sol_f32 bx0, bx1, bz0, bz1;
        if (run_x) {
            bx0 = (sx < tx) ? sx : tx; bx1 = (sx < tx) ? tx : sx;
            bz0 = sz - hw; bz1 = sz + hw;
        } else {
            bz0 = (sz < tz) ? sz : tz; bz1 = (sz < tz) ? tz : sz;
            bx0 = sx - hw; bx1 = sx + hw;
        }
        aabb_box(b, bx0, bx1, -t, yd, bz0, bz1);
    }
}

void make_walkway_L(MeshBuilder *b, sol_f32 cx, sol_f32 cz, sol_f32 cy,
                    sol_f32 ex, sol_f32 ez, sol_f32 ey, sol_f32 w, sol_f32 t) {
    sol_f32 hw = w * 0.5f;
    sol_f32 l1 = (sol_f32)sqrt((double)(cx * cx + cz * cz));
    sol_f32 l2 = (sol_f32)sqrt((double)((ex - cx) * (ex - cx) + (ez - cz) * (ez - cz)));
    walkway_leg(b, 0.0f, 0.0f, 0.0f, cx, cz, cy, w, t);
    if (l1 > 1e-3f && l2 > 1e-3f)             /* landing only at a real bend */
        aabb_box(b, cx - hw, cx + hw, -t, cy, cz - hw, cz + hw);
    walkway_leg(b, cx, cz, cy, ex, ez, ey, w, t);
}
```

`<math.h>` is already included in mesh.c (used by `make_walkway`). Confirm with `grep -n "include <math.h>" mesh.c`; if absent, add it.

- [ ] **Step 3: Build gauntlet**

```bash
./build.sh c89check && ./build.sh debug && ./build.sh metal
```
Expected: all pass, no warnings. (Nothing calls `make_walkway_L` yet — that is Task 4.)

- [ ] **Step 4: Commit**

```bash
git add mesh.c mesh.h
git commit -m "$(printf 'feat: make_walkway_L — stepped L legs + corner landing\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 3: `route.c` — the routing author (TDD)

**Files:**
- Create: `route.h`, `route.c`, `route_test.c`
- Modify: `build.sh` (TU lists + a `routetest` mode)

This is the trickiest math, so it is built test-first. `route.c` is pure CPU (includes `scene.h`, `mesh.h`, `sol_math.h`, `<math.h>`); no GL.

- [ ] **Step 1: Write `route.h`**

```c
#ifndef SOL_ROUTE_H
#define SOL_ROUTE_H

#include "scene.h"
#include "sol_math.h"
#include "mesh.h"   /* RoomOpening, ROOM_WALL_* */

#define ROUTE_DOOR_W 1.4f
#define ROUTE_DOOR_H 2.1f
#define ROUTE_WALL_T 0.20f
#define ROUTE_MAX    256

/* One connection's fully-resolved geometry. World coords. The lower-Y room is
   the anchor: door_lo is the mesh origin of the walkway; the path bends at
   `corner` and ends at door_hi. `straight` => no bend (corner == door_hi). */
typedef struct {
    sol_u32 walkway;
    sol_u32 room_lo, room_hi;     /* parent (room_type) handles */
    int     wall_lo, wall_hi;     /* ROOM_WALL_* opened on each room */
    vec3    door_lo, corner, door_hi;
    int     straight;
    int     valid;                /* 0 = dangling/degenerate; skip */
} Route;

/* Compute every walkway's route. Returns the count written to out (<= max).
   Door centers are spread when multiple routes share a (room,wall). */
int  route_all(Scene *s, Route *out, int max);

/* The route for one walkway handle (recomputes route_all internally). Returns
   1 and fills *out if found+valid, else 0. */
int  route_for_walkway(Scene *s, sol_u32 walkway, Route *out);

/* The door openings on one room (parent handle), in ROOM-LOCAL coords, for
   make_room_doored / the collider. Returns the count written (<= max). */
int  route_room_openings(Scene *s, sol_u32 room, RoomOpening *out, int max);

#endif
```

- [ ] **Step 2: Write `route_test.c` (failing — `route.c` not yet implemented)**

```c
#include "route.h"
#include "scene.h"
#include <stdio.h>
#include <math.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* add a fs-tree room (parent empty + "room" shell child) at pos, size w x d. */
static sol_u32 add_room(Scene *s, float x, float y, float z, float w, float d) {
    Mesh    empty;
    sol_u32 parent, shell;
    float   p[8];
    memset(&empty, 0, sizeof empty);
    parent = scene_add(s, 0, empty, vec3_make(x, y, z), quat_identity(),
                       vec3_make(1.0f, 1.0f, 1.0f));
    scene_meta_set(s, parent, "room_type", "mirror");
    shell = scene_add(s, parent, empty, vec3_make(0.0f, 0.0f, 0.0f),
                      quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
    scene_mesh_ref_set(s, shell, "room");
    p[0] = w; p[1] = d; p[2] = 3.0f; p[3] = 1.0f; p[4] = 1.0f; p[5] = 1.0f;
    p[6] = 1.0f; p[7] = 0.0f;
    scene_mesh_params_set(s, shell, p, 8);
    return parent;
}

static sol_u32 add_walkway(Scene *s, sol_u32 a, sol_u32 b) {
    Mesh    empty;
    sol_u32 w;
    memset(&empty, 0, sizeof empty);
    w = scene_add(s, 0, empty, vec3_make(0.0f, 0.0f, 0.0f), quat_identity(),
                  vec3_make(1.0f, 1.0f, 1.0f));
    scene_mesh_ref_set(s, w, "walkway");
    scene_rel_add(s, w, "connects", a);
    scene_rel_add(s, w, "connects", b);
    return w;
}

int main(void) {
    /* due-east: straight, A opens E, B opens W, 0 bends */
    {
        Scene s; Route r; sol_u32 home, east, wk;
        scene_init(&s);
        home = add_room(&s, 0.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        east = add_room(&s, 20.0f, 12.0f, 0.0f, 10.0f, 10.0f);
        wk   = add_walkway(&s, home, east);
        CHECK(route_for_walkway(&s, wk, &r));
        CHECK(r.valid);
        CHECK(r.straight);
        /* one room opens E, the other W (order depends on lo/hi by Y; equal
           Y => deterministic by handle, so just assert the PAIR) */
        CHECK((r.wall_lo == ROOM_WALL_E && r.wall_hi == ROOM_WALL_W) ||
              (r.wall_lo == ROOM_WALL_W && r.wall_hi == ROOM_WALL_E));
        scene_free(&s);
    }
    /* diagonal up-right (dx>dz): A opens E, B opens N, single bend */
    {
        Scene s; Route r; sol_u32 a, b, wk;
        scene_init(&s);
        a  = add_room(&s, 0.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        b  = add_room(&s, 24.0f, 12.0f, 10.0f, 10.0f, 10.0f);  /* +x dominant, +z */
        wk = add_walkway(&s, a, b);
        CHECK(route_for_walkway(&s, wk, &r));
        CHECK(r.valid);
        CHECK(!r.straight);
        /* the corner shares the exit room's z and the entry room's x */
        CHECK(fabs((double)(r.corner.z - r.door_lo.z)) < 1e-3);
        CHECK(fabs((double)(r.corner.x - r.door_hi.x)) < 1e-3);
        scene_free(&s);
    }
    /* two roots both due-east of home => two doors spread on home's E wall */
    {
        Scene s; RoomOpening op[16]; sol_u32 home, e1, e2; int n, i, eN = 0;
        scene_init(&s);
        home = add_room(&s, 0.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        e1   = add_room(&s, 20.0f, 12.0f, 6.0f, 10.0f, 10.0f);
        e2   = add_room(&s, 20.0f, 12.0f, -6.0f, 10.0f, 10.0f);
        add_walkway(&s, home, e1);
        add_walkway(&s, home, e2);
        n = route_room_openings(&s, home, op, 16);
        for (i = 0; i < n; i++) if (op[i].wall == ROOM_WALL_E) eN++;
        CHECK(eN == 2);
        /* spread => the two E-wall door centers differ */
        if (eN == 2) {
            float c0 = 1e30f, c1 = -1e30f;
            for (i = 0; i < n; i++) if (op[i].wall == ROOM_WALL_E) {
                if (op[i].center < c0) c0 = op[i].center;
                if (op[i].center > c1) c1 = op[i].center;
            }
            CHECK(c1 - c0 > 0.5f);
        }
        scene_free(&s);
    }
    if (fails == 0) printf("route_test: OK\n");
    return fails ? 1 : 0;
}
```

Add `#include <string.h>` at the top for `memset`.

- [ ] **Step 3: Add a `routetest` mode to `build.sh`**

Mirror the `collidetest` block (build.sh ~92-100). Insert after it:

```sh
# route_test: routing math over a headless scene (no GL). Links the scene
# spine + mesh.c (RoomOpening) it derives from.
if [ "$MODE" = "routetest" ]; then
    set -x
    clang -std=c89 -pedantic-errors -Werror -g -fsanitize=address,undefined \
        route.c route_test.c scene.c material.c mesh.c flora.c rock.c gothic.c sweep.c nid.c sol_math.c \
        -o route_test
    echo "built ./route_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi
```
(Match the exact flag/indentation style of the adjacent `collidetest` block; copy its `clang ...` invocation and swap the sources.)

- [ ] **Step 4: Add `route.c` to the 4 full-build TU lists + `collidetest`**

In `build.sh`, append ` route.c` to each of these source lists (right after `palette.c`, or after `collide.c` for the collidetest line):
- line ~16 (`c89check` `-fsyntax-only` list) — after `palette.c`
- line ~98 (`collidetest`) — after `collide.c` (collide.c will call route in Task 5; link it now)
- line ~253 (`metal`) — after `palette.c`
- line ~269 (`debug`) — after `palette.c`
- line ~284 (the asan/`test` full build) — after `palette.c`

- [ ] **Step 5: Write `route.c`**

```c
#include "route.h"
#include "mesh.h"
#include <math.h>
#include <string.h>

/* world translation of an object (parent chain applied) */
static vec3 obj_pos(Scene *s, sol_u32 h) {
    SceneObject *o = scene_get(s, h);
    if (o) { mat4 w = scene_world_matrix(s, o); return vec3_make(w.m[12], w.m[13], w.m[14]); }
    return vec3_make(0.0f, 0.0f, 0.0f);
}

/* a room's interior half-extents (its "room" shell child's w/d, halved) */
static void room_half(Scene *s, sol_u32 room, float *hw, float *hd) {
    sol_u32 i;
    *hw = 4.0f; *hd = 4.0f;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        if (o->parent == room && o->mesh_ref && strcmp(o->mesh_ref, "room") == 0) {
            *hw = 0.5f * mesh_ref_param("room", o->mesh_params, o->mesh_param_count, "w");
            *hd = 0.5f * mesh_ref_param("room", o->mesh_params, o->mesh_param_count, "d");
            return;
        }
    }
}

/* the two rooms a walkway connects (its first two `connects` targets) */
static void walkway_rooms(SceneObject *o, sol_u32 *a, sol_u32 *b) {
    sol_u32 j;
    *a = 0; *b = 0;
    for (j = 0; j < o->rel_count; j++) {
        if (strcmp(o->relations[j].type, "connects") != 0) continue;
        if (*a == 0) *a = o->relations[j].target;
        else if (*b == 0) *b = o->relations[j].target;
    }
}

/* pass 1: per walkway, fill room_lo/hi (by Y) + wall_lo/hi (by geometry).
   door centers + corner are filled in pass 2/3. */
static int routes_pass1(Scene *s, Route *out, int max) {
    sol_u32 i;
    int     n = 0;
    for (i = 0; i < s->count && n < max; i++) {
        SceneObject *o = &s->objects[i];
        sol_u32 ra, rb, lo, hi;
        vec3    pa, pb, plo, phi;
        float   dx, dz, adx, adz;
        Route  *r;
        if (!o->mesh_ref || strcmp(o->mesh_ref, "walkway") != 0) continue;
        walkway_rooms(o, &ra, &rb);
        r = &out[n];
        memset(r, 0, sizeof *r);
        r->walkway = o->handle;
        if (ra == 0 || rb == 0 || !scene_get(s, ra) || !scene_get(s, rb)) { n++; continue; }
        pa = obj_pos(s, ra); pb = obj_pos(s, rb);
        lo = (pa.y <= pb.y) ? ra : rb;
        hi = (lo == ra) ? rb : ra;
        r->room_lo = lo; r->room_hi = hi;
        plo = obj_pos(s, lo); phi = obj_pos(s, hi);
        dx = phi.x - plo.x; dz = phi.z - plo.z;        /* hi relative to lo */
        adx = dx < 0.0f ? -dx : dx; adz = dz < 0.0f ? -dz : dz;
        if (adx < 1e-3f && adz < 1e-3f) { n++; continue; }   /* overlapping: invalid */
        /* wall facing the OTHER room. North=-z, South=+z, East=+x, West=-x. */
        if (adz < 0.5f) {                               /* aligned in z: straight x */
            r->wall_lo = (dx > 0.0f) ? ROOM_WALL_E : ROOM_WALL_W;
            r->wall_hi = (dx > 0.0f) ? ROOM_WALL_W : ROOM_WALL_E;
            r->straight = 1;
        } else if (adx < 0.5f) {                        /* aligned in x: straight z */
            r->wall_lo = (dz > 0.0f) ? ROOM_WALL_S : ROOM_WALL_N;
            r->wall_hi = (dz > 0.0f) ? ROOM_WALL_N : ROOM_WALL_S;
            r->straight = 1;
        } else if (adx >= adz) {                        /* L: lo exits x, hi enters z */
            r->wall_lo = (dx > 0.0f) ? ROOM_WALL_E : ROOM_WALL_W;
            r->wall_hi = (dz > 0.0f) ? ROOM_WALL_N : ROOM_WALL_S;
            r->straight = 0;
        } else {                                        /* L: lo exits z, hi enters x */
            r->wall_lo = (dz > 0.0f) ? ROOM_WALL_S : ROOM_WALL_N;
            r->wall_hi = (dx > 0.0f) ? ROOM_WALL_W : ROOM_WALL_E;
            r->straight = 0;
        }
        r->valid = 1;
        n++;
    }
    return n;
}

/* the door center (signed offset along the wall's run axis from room center)
   for route index `idx`, given how many routes share its (room,wall) and this
   route's order among them. side==0 => the lo room, side==1 => the hi room. */
static float spread_center(Scene *s, Route *out, int n, int idx, int side) {
    sol_u32 room = side ? out[idx].room_hi : out[idx].room_lo;
    int     wall = side ? out[idx].wall_hi : out[idx].wall_lo;
    float   hw, hd, span, margin, lo, hiend;
    int     i, count = 0, rank = 0;
    /* count members of this (room,wall) group + this route's 1-based rank
       (stable by array order). A route touches `room` as its lo XOR hi side. */
    for (i = 0; i < n; i++) {
        int hit_lo, hit_hi;
        if (!out[i].valid) continue;
        hit_lo = (out[i].room_lo == room && out[i].wall_lo == wall);
        hit_hi = (out[i].room_hi == room && out[i].wall_hi == wall);
        if (hit_lo) { if (i < idx || (i == idx && side == 0)) rank++; count++; }
        if (hit_hi) { if (i < idx || (i == idx && side == 1)) rank++; count++; }
    }
    if (count < 1) count = 1;
    if (rank < 1) rank = 1;
    room_half(s, room, &hw, &hd);
    margin = ROUTE_DOOR_W;                       /* keep doors off the corners */
    span   = (wall == ROOM_WALL_N || wall == ROOM_WALL_S) ? (2.0f * hw) : (2.0f * hd);
    lo     = -0.5f * span + margin;
    hiend  =  0.5f * span - margin;
    if (hiend < lo) { lo = 0.0f; hiend = 0.0f; }
    return lo + (hiend - lo) * ((float)rank / (float)(count + 1));
}

/* world door-center on `room`'s `wall` at run-axis offset `center` */
static vec3 door_world(Scene *s, sol_u32 room, int wall, float center) {
    vec3  p = obj_pos(s, room);
    float hw, hd;
    room_half(s, room, &hw, &hd);
    if      (wall == ROOM_WALL_N) return vec3_make(p.x + center, p.y, p.z - hd);
    else if (wall == ROOM_WALL_S) return vec3_make(p.x + center, p.y, p.z + hd);
    else if (wall == ROOM_WALL_E) return vec3_make(p.x + hw, p.y, p.z + center);
    else                          return vec3_make(p.x - hw, p.y, p.z + center);  /* W */
}

int route_all(Scene *s, Route *out, int max) {
    int n, i;
    n = routes_pass1(s, out, max);
    for (i = 0; i < n; i++) {
        Route *r = &out[i];
        float  clo, chi;
        vec3   dlo, dhi, cor;
        if (!r->valid) continue;
        clo = spread_center(s, out, n, i, 0);
        chi = spread_center(s, out, n, i, 1);
        dlo = door_world(s, r->room_lo, r->wall_lo, clo);
        dhi = door_world(s, r->room_hi, r->wall_hi, chi);
        if (r->straight) {
            cor = dhi;                            /* no bend */
        } else if (r->wall_lo == ROOM_WALL_E || r->wall_lo == ROOM_WALL_W) {
            cor = vec3_make(dhi.x, 0.0f, dlo.z);  /* lo exits along x */
        } else {
            cor = vec3_make(dlo.x, 0.0f, dhi.z);  /* lo exits along z */
        }
        /* distribute the rise as a steady climb over the total run length */
        {
            float l1 = (float)sqrt((double)((cor.x - dlo.x) * (cor.x - dlo.x) +
                                            (cor.z - dlo.z) * (cor.z - dlo.z)));
            float l2 = (float)sqrt((double)((dhi.x - cor.x) * (dhi.x - cor.x) +
                                            (dhi.z - cor.z) * (dhi.z - cor.z)));
            float tot = l1 + l2;
            cor.y = (tot > 1e-4f) ? dlo.y + (dhi.y - dlo.y) * (l1 / tot) : dlo.y;
        }
        r->door_lo = dlo; r->corner = cor; r->door_hi = dhi;
    }
    return n;
}

int route_for_walkway(Scene *s, sol_u32 walkway, Route *out) {
    Route all[ROUTE_MAX];
    int   n = route_all(s, all, ROUTE_MAX), i;
    for (i = 0; i < n; i++) {
        if (all[i].walkway == walkway && all[i].valid) { *out = all[i]; return 1; }
    }
    return 0;
}

int route_room_openings(Scene *s, sol_u32 room, RoomOpening *out, int max) {
    Route all[ROUTE_MAX];
    int   n = route_all(s, all, ROUTE_MAX), i, m = 0;
    vec3  rp = obj_pos(s, room);
    for (i = 0; i < n && m < max; i++) {
        Route *r = &all[i];
        if (!r->valid) continue;
        if (r->room_lo == room) {
            int   wall = r->wall_lo;
            float c = (wall == ROOM_WALL_N || wall == ROOM_WALL_S)
                        ? r->door_lo.x - rp.x : r->door_lo.z - rp.z;
            out[m].wall = wall; out[m].center = c;
            out[m].width = ROUTE_DOOR_W; out[m].height = ROUTE_DOOR_H; m++;
        }
        if (m >= max) break;
        if (r->room_hi == room) {
            int   wall = r->wall_hi;
            float c = (wall == ROOM_WALL_N || wall == ROOM_WALL_S)
                        ? r->door_hi.x - rp.x : r->door_hi.z - rp.z;
            out[m].wall = wall; out[m].center = c;
            out[m].width = ROUTE_DOOR_W; out[m].height = ROUTE_DOOR_H; m++;
        }
    }
    return m;
}
```

- [ ] **Step 6: Build + run the route test**

```bash
./build.sh routetest && ./route_test
```
Expected: compiles clean, prints `route_test: OK`, no sanitizer output.

- [ ] **Step 7: Build gauntlet (route.c now in every list)**

```bash
./build.sh c89check && ./build.sh debug && ./build.sh metal
```
Expected: all pass.

- [ ] **Step 8: Commit**

```bash
git add route.c route.h route_test.c build.sh
git commit -m "$(printf 'feat: route.c — the connection routing author (L-paths, doorways)\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 4: `connections_rebuild` — the mesh reader

**Files:**
- Modify: `main.c` — replace `walkways_rebuild` (main.c:4088-4138) with `connections_rebuild`; add `#include "route.h"` near the other includes; swap call sites at main.c:6593 and main.c:7927.

- [ ] **Step 1: Add the include**

At the top of `main.c` with the other project headers (e.g. after `#include "collide.h"` / wherever `mesh.h` is included), add:
```c
#include "route.h"
```

- [ ] **Step 2: Replace `walkways_rebuild` with `connections_rebuild`**

Replace the entire `walkways_rebuild` function (main.c:4085-4138, including its comment) with:

```c
/* Derive ALL connection geometry from the route author (route.c): each
   walkway becomes an L (or straight) ribbon anchored at its lower door, and
   each room's shell is rebuilt with the doorways its routes pierce. Rooms and
   walkways store no geometry — this runs on load and after every edit, so a
   new root can never leave a sibling's path stale (the take-1 bug). */
static void connections_rebuild(AppState *st) {
    Scene *s = &st->scene;
    Route  routes[ROUTE_MAX];
    int    n = route_all(s, routes, ROUTE_MAX), i;
    sol_u32 j;

    /* 1. walkways: anchor at the lower door, mesh in local coords */
    for (i = 0; i < n; i++) {
        Route       *r = &routes[i];
        SceneObject *o = scene_get(s, r->walkway);
        MeshBuilder  mb;
        if (!o) continue;
        mesh_destroy(&o->mesh);
        if (!r->valid) continue;
        o->pos = r->door_lo;
        o->rot = quat_identity();
        mb_init(&mb);
        make_walkway_L(&mb,
                       r->corner.x - r->door_lo.x, r->corner.z - r->door_lo.z,
                       r->corner.y - r->door_lo.y,
                       r->door_hi.x - r->door_lo.x, r->door_hi.z - r->door_lo.z,
                       r->door_hi.y - r->door_lo.y,
                       ROUTE_DOOR_W + 0.4f, 0.15f);   /* deck a touch wider than the door */
        if (mb.index_count > 0) o->mesh = mesh_from_builder(&mb);
        mb_free(&mb);
    }

    /* 2. rooms: rebuild each shell child with its doorways */
    for (j = 0; j < s->count; j++) {
        SceneObject *room = &s->objects[j];
        const char  *rt;
        sol_u32      k;
        if (room->mesh_ref) continue;                 /* room parents are empties */
        rt = scene_meta_get(s, room->handle, "room_type");
        if (!rt) continue;
        if (strcmp(rt, "home") != 0 && strcmp(rt, "mirror") != 0) continue;
        for (k = 0; k < s->count; k++) {
            SceneObject *shell = &s->objects[k];
            RoomOpening  ops[16];
            int          no;
            float        w, d, h;
            MeshBuilder  mb;
            if (shell->parent != room->handle || !shell->mesh_ref ||
                strcmp(shell->mesh_ref, "room") != 0) continue;
            w = mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "w");
            d = mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "d");
            h = mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "h");
            no = route_room_openings(s, room->handle, ops, 16);
            mesh_destroy(&shell->mesh);
            mb_init(&mb);
            make_room_doored(&mb, w, d, h, ROUTE_WALL_T,
                             mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "wn") > 0.5f,
                             mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "we") > 0.5f,
                             mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "ws") > 0.5f,
                             mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "ww") > 0.5f,
                             mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "ceil") > 0.5f,
                             ops, no);
            if (mb.index_count > 0) shell->mesh = mesh_from_builder(&mb);
            mb_free(&mb);
        }
    }
}
```

Note `scene_get` returns a pointer "valid until the next `scene_add`" — `connections_rebuild` does no `scene_add`, so the `o`/`room`/`shell` pointers stay valid through the loops. `apply_kind_materials` is still called by the sites after this, so the rebuilt shells keep their materials (materials live on the SceneObject, not the mesh).

- [ ] **Step 3: Swap the two call sites**

`create_root_from_path` (main.c:6593): change `walkways_rebuild(st);` to `connections_rebuild(st);`.

`load_palace` (main.c:7927): change `walkways_rebuild(st);` to `connections_rebuild(st);`. (The comment on that line can read `/* rooms + walkway connectors re-derive */`.)

Then `grep -n "walkways_rebuild" main.c` — expect **zero** matches. If any remain, swap them too.

- [ ] **Step 4: Build gauntlet**

```bash
./build.sh c89check && ./build.sh debug && ./build.sh metal
```
Expected: all pass, no warnings. Rooms now render with doorway openings and walkways render as L-ribbons. (Collision still uses the OLD room/walkway colliders until Task 5 — so you may clip walls / not yet walk through doors when running; that is expected and fixed next.)

- [ ] **Step 5: Commit**

```bash
git add main.c
git commit -m "$(printf 'feat: connections_rebuild — rooms + L-walkways from the route author\n\nReplaces walkways_rebuild; reads route.c to build doored room shells and\nL-shaped walkway meshes. Fixes the disappearing-walkway bug (all routes\nrebuilt every pass).\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 5: `collide.c` — the collider reader

**Files:**
- Modify: `collide.c` — `#include "route.h"`; rewrite the `room` case (collide.c:314-336) and the `walkway` case (collide.c:573-596).

- [ ] **Step 1: Add the include**

At the top of `collide.c` (with the other includes), add `#include "route.h"`.

- [ ] **Step 2: Rewrite the `room` case** (collide.c:314-336)

Replace the whole `if (strcmp(o->mesh_ref, "room") == 0) { ... }` body with floor + doored walls derived from the SAME openings the mesh used:

```c
        if (strcmp(o->mesh_ref, "room") == 0) {
            /* floor (top at y=0) + a thick slab per present wall, built around
               the SAME door gaps the mesh uses (route.c is the one author). */
            float w  = ref_p(o, "w"), d = ref_p(o, "d"), h = ref_p(o, "h");
            float hw = w * 0.5f, hd = d * 0.5f;
            float t  = ROUTE_WALL_T, ht = t * 0.5f;
            mat4  m  = scene_world_matrix(s, o);
            RoomOpening ops[16];
            int   no = (o->parent != 0) ? route_room_openings(s, o->parent, ops, 16) : 0;
            int   walls[4]; int wi;
            emit_local_box(cs, m, o->handle, 0.0f, 0.0f, hw, hd, -t, 0.0f);   /* floor */
            if (ref_p(o, "ceil") > 0.5f)
                emit_local_box(cs, m, o->handle, 0.0f, 0.0f, hw, hd, h, h + t);
            walls[0] = ref_p(o, "wn") > 0.5f; walls[1] = ref_p(o, "we") > 0.5f;
            walls[2] = ref_p(o, "ws") > 0.5f; walls[3] = ref_p(o, "ww") > 0.5f;
            for (wi = 0; wi < 4; wi++) {
                /* run axis span + fixed offset for this wall (mirror make_room_doored) */
                float s0, s1, fixc, fhalf;       /* run [s0,s1]; box centered at fixc +/- fhalf in the fixed axis */
                int   runx = (wi == ROOM_WALL_N || wi == ROOM_WALL_S);
                float lo[8], hi[8]; int k = 0, gi, a;
                float cur;
                if (!walls[wi]) continue;
                if (wi == ROOM_WALL_N)      { s0 = -hw; s1 = hw;       fixc = -hd - ht; fhalf = ht; }
                else if (wi == ROOM_WALL_S) { s0 = -hw; s1 = hw;       fixc =  hd + ht; fhalf = ht; }
                else if (wi == ROOM_WALL_E) { s0 = -hd - t; s1 = hd + t; fixc =  hw + ht; fhalf = ht; }
                else                        { s0 = -hd - t; s1 = hd + t; fixc = -hw - ht; fhalf = ht; }
                for (gi = 0; gi < no; gi++) {            /* this wall's gaps, sorted */
                    float c, gh;
                    if (ops[gi].wall != wi) continue;
                    if (k >= 8) break;
                    c = ops[gi].center; gh = ops[gi].width * 0.5f;
                    lo[k] = c - gh; hi[k] = c + gh; k++;
                }
                for (gi = 1; gi < k; gi++) {
                    float la = lo[gi], ha = hi[gi]; a = gi - 1;
                    while (a >= 0 && lo[a] > la) { lo[a+1]=lo[a]; hi[a+1]=hi[a]; a--; }
                    lo[a+1]=la; hi[a+1]=ha;
                }
                cur = s0;
                for (gi = 0; gi <= k; gi++) {
                    float gL = (gi < k) ? lo[gi] : s1;
                    float gR = (gi < k) ? hi[gi] : s1;
                    if (gL < s0) gL = s0; if (gR > s1) gR = s1;
                    if (gL > cur) {                       /* solid pier collider */
                        float mid = (cur + gL) * 0.5f, half = (gL - cur) * 0.5f;
                        if (runx) emit_local_box(cs, m, o->handle, mid, fixc, half, fhalf, 0.0f, h);
                        else      emit_local_box(cs, m, o->handle, fixc, mid, fhalf, half, 0.0f, h);
                    }
                    if (gi < k) cur = gR;                 /* header omitted: you duck/pass under, gap is walkable */
                }
            }
        }
```

Note: the header above a door is not collided (a ~2.1 m opening under a 3 m wall — the player passes through; omitting the header collider keeps doorways cleanly walkable and matches the old behavior of not blocking openings). Piers + floor are the collision that matters.

- [ ] **Step 3: Rewrite the `walkway` case** (collide.c:573-596)

Replace the whole `else if (strcmp(o->mesh_ref, "walkway") == 0) { ... }` body with an L derived from the route (two stepped legs + landing), mirroring `make_walkway_L`:

```c
        } else if (strcmp(o->mesh_ref, "walkway") == 0) {
            /* mirror make_walkway_L: per-step boxes along each leg + a landing.
               The walkway sits at door_lo with identity rot, so the mesh's
               local frame == this object's frame; we recompute the route and
               emit the same boxes in local coords. */
            Route r;
            mat4  m = scene_world_matrix(s, o);
            float ww = ROUTE_DOOR_W + 0.4f, hw2 = ww * 0.5f, tt = 0.15f;
            if (route_for_walkway(s, o->handle, &r) && r.valid) {
                float lx = r.corner.x - r.door_lo.x, lz = r.corner.z - r.door_lo.z;
                float ly = r.corner.y - r.door_lo.y;
                float ex = r.door_hi.x - r.door_lo.x, ez = r.door_hi.z - r.door_lo.z;
                float ey = r.door_hi.y - r.door_lo.y;
                float l1 = (float)sqrt((double)(lx * lx + lz * lz));
                float l2 = (float)sqrt((double)((ex - lx) * (ex - lx) + (ez - lz) * (ez - lz)));
                int   seg;
                for (seg = 0; seg < 2; seg++) {
                    float x0 = seg ? lx : 0.0f, z0 = seg ? lz : 0.0f, y0 = seg ? ly : 0.0f;
                    float x1 = seg ? ex : lx,   z1 = seg ? ez : lz,   y1 = seg ? ey : ly;
                    float dx = x1 - x0, dz = z1 - z0, dy = y1 - y0;
                    float len = (float)sqrt((double)(dx * dx + dz * dz));
                    float ady = dy < 0.0f ? -dy : dy;
                    int   nstep, i2, rx;
                    if (len < 1e-4f) continue;
                    rx = ((dx < 0.0f ? -dx : dx) >= (dz < 0.0f ? -dz : dz));
                    nstep = (ady < 0.02f) ? 1 : (int)(ady / WALKWAY_STEP_RISE) + 1;
                    if (nstep < 1) nstep = 1; if (nstep > 128) nstep = 128;
                    for (i2 = 0; i2 < nstep; i2++) {
                        float a0 = (float)i2 / (float)nstep, a1 = (float)(i2 + 1) / (float)nstep;
                        float cxs = x0 + dx * (a0 + a1) * 0.5f;
                        float czs = z0 + dz * (a0 + a1) * 0.5f;
                        float yd  = y0 + dy * a1;
                        float halfrun = len / (float)nstep * 0.5f;
                        if (rx) emit_local_box(cs, m, o->handle, cxs, czs, halfrun, hw2, -tt, yd);
                        else    emit_local_box(cs, m, o->handle, cxs, czs, hw2, halfrun, -tt, yd);
                    }
                }
                if (l1 > 1e-3f && l2 > 1e-3f)            /* corner landing */
                    emit_local_box(cs, m, o->handle, lx, lz, hw2, hw2, -tt, ly);
            }
```

(Keep the existing closing `}` structure of the `else if` chain intact.)

- [ ] **Step 4: Build gauntlet + the collide & route tests**

```bash
./build.sh c89check && ./build.sh debug && ./build.sh metal && ./build.sh collidetest && ./collide_test && ./build.sh routetest && ./route_test
```
Expected: all builds pass; `collide_test` and `route_test` print OK with no sanitizer output.

- [ ] **Step 5: Commit**

```bash
git add collide.c
git commit -m "$(printf 'feat: collide.c reads route.c — doored room + L-walkway colliders\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 6: Final gauntlet + human live-verify handoff

**Files:** none (verification only).

- [ ] **Step 1: Full gauntlet + all affected tests**

```bash
./build.sh c89check && ./build.sh debug && ./build.sh metal && ./build.sh routetest && ./route_test && ./build.sh collidetest && ./collide_test
```
Expected: every command exits 0; both tests print OK.

- [ ] **Step 2: Confirm no leftovers**

```bash
grep -n "walkways_rebuild\|make_room\b" main.c mesh.c mesh.h collide.c
```
Expected: no `walkways_rebuild` anywhere; `make_room` only if a real remaining caller exists (otherwise it was deleted in Task 1 Step 4).

- [ ] **Step 3: Hand off the live-verify checklist to Fran** (subagents cannot drive the GLFW window)

Present this checklist; do NOT merge until Fran confirms:
1. `rm scene.stml`, launch → spawn in the home room (now a thick-walled open-topped box).
2. `:` → New root → a path due-east of home enters/exits perpendicular through a **visible doorway with reveals** (straight, 0 bends).
3. New root up-and-to-the-side → a **single-bend L**, both ends perpendicular through doorways.
4. **Create a 2nd root — the 1st root's walkway MUST still be there** (the take-1 bug).
5. Two roots that land on the same wall of home → **two spread doorways**, two paths, no overlap.
6. Walk a path: doorways, stairs, and the corner landing are all walkable; no fall-through; no clipping into a wall where there is no door.
7. Backtick `` ` `` / reload (or relaunch) → everything re-derives identically.

---

## Self-Review

**Spec coverage (against `2026-06-18-walkways-take-2-design.md`):**
- Shared route / one-author-two-readers → `route.c` + Tasks 4 (mesh reader) & 5 (collider reader). ✓
- `connections_rebuild` single pass → Task 4. ✓
- Single-bend L + straight-when-aligned + axis-of-greater-separation + door spreading → `route.c` pass1/spread_center/route_all + `route_test`. ✓
- Thick reveal doorways, multi-opening, derived-each-load rooms → `make_room_doored` (Task 1) + Task 4 room loop. ✓
- Corner landing, door size, stairs distribution → `make_walkway_L` (Task 2) + `route_all` rise split. ✓
- Disappearing-bug folded in → Task 4 rebuilds ALL routes every pass (note in commit). ✓
- Collision: thick walls with gaps + L legs + landing → Task 5. ✓
- Migration free (derive each load) → load site calls `connections_rebuild` (Task 4 Step 3). ✓

**Placeholder scan:** the only intentional "leftover" is the flagged stray loop in `spread_center` (Task 3 Step 5) with an explicit instruction to delete it — not a placeholder, a correction note. No TBDs.

**Type consistency:** `RoomOpening` (mesh.h) used identically in mesh.c, route.c, collide.c. `Route` fields (`door_lo/corner/door_hi/wall_lo/wall_hi/room_lo/room_hi/straight/valid/walkway`) used consistently across route.c, main.c, collide.c. `ROUTE_DOOR_W/H`, `ROUTE_WALL_T`, `ROOM_WALL_*` shared via the headers. Walkway deck width (`ROUTE_DOOR_W + 0.4f`) and thickness (`0.15f`) match between Task 4 (mesh) and Task 5 (collider).

**Known limitations (in spec's out-of-scope):** very close rooms whose L-corner would land inside a room can clip (ring placement keeps rooms well separated); >8 doors on one wall are dropped (cap). Both acceptable for v1.
