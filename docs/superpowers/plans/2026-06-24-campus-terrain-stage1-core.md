# Campus Terrain — Milestone 1 (Core) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** A per-world opt-in "campus": the rooms of a flagged world get a single generated terrain beneath them, graded flat under each room at its floor height with noise hills between, and you can walk on it. No flora yet (Milestone 2).

**Architecture:** A new pure `campus_height(pads, …, lx, lz)` blends per-room flat **pads** (footprint + floor height) with fBm noise — IDW interpolation of pad heights, footprint-flatten with **lowest-pad-wins**, hills between, zero-rim fade. `make_campus` builds a terrain mesh from it (mirroring `make_terrain`). The campus is **derived state** (`g_campus`, like the `RoomFrame`/`walk_trim` caches) — NOT a scene object: `campus_rebuild` gathers the active world's room pads, sizes a rubber-band rectangle, and rebuilds the mesh, hooked at the end of `connections_rebuild` (which already runs on every structural change). A `:` command toggles a persistent `meta["campus"]` flag on the world's workspace anchor. `ground_under` calls `campus_height` directly for standing (same pattern as `terrain_height` — **no separate baked grid**; the pad count is tiny, so direct evaluation stays consistent with the visible mesh).

**Tech Stack:** C89 (`-Wall -Wextra -Werror -pedantic`); the existing `terrain_fbm`/noise, `make_terrain` skirt/base, `sol_smoothstep`, `editor_room_rect`, `workspace_anchor_*`, `scene_meta_*`, the command registry.

**C89 reminders:** decls at top of block; `/* */` only; **no C99 math** — use `(float)sqrt((double)x)` and ternary `abs` (`a<0?-a:a`), NOT `sqrtf`/`fabsf`; `c89check` is `-Wall -Wextra -Werror`, so co-locate any new static with its first caller and avoid unused symbols. **Never commit** `NOTES.stml`/`paper-picture.png`. Commit trailer: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

**Render parts are not unit-testable** — only `campus_height` (pure) gets a unit test; the mesh/draw/standing/command get the build gauntlet + human live-verify.

**Branch:** `campus-stage1` (create at start; ff-merge to `main` at the end).

---

## File structure

- **mesh.h** — declare `CampusPad`, `CAMPUS_MAX_PADS`, `campus_height`, `make_campus`.
- **mesh.c** — `campus_rect_dist` (static), `campus_height` (pure, blends pads+noise; reuses the file-static `terrain_fbm`/`TERRAIN_FEATURE_M`), `make_campus` (mesh builder mirroring `make_terrain`).
- **campus_test.c** (new) — headless unit test for `campus_height`.
- **build.sh** — a `campustest` case mirroring `furnituretest`.
- **main.c** — `g_campus` state struct + globals; `campus_gather_pads`; `campus_rebuild`; the campus draw block; the `ground_under` campus branch; `cmd_toggle_campus` + its `g_commands[]` row; the `campus_rebuild(st)` hook at the end of `connections_rebuild`.

---

### Task 0: Branch

- [ ] **Step 1:** `git checkout -b campus-stage1`

---

### Task 1: `campus_height` (pure) + unit test

**Files:** Modify `mesh.h`, `mesh.c`. Create `campus_test.c`. Modify `build.sh`.

- [ ] **Step 1: Declare the API in `mesh.h`**

After the `make_terrain` declaration (grep `void    make_terrain`), add:
```c
/* A campus pad: one room's footprint + its floor height, in campus-LOCAL coords
   (relative to the campus rectangle centre). floor_y is absolute world Y. */
typedef struct { float cx, cz, hw, hd, floor_y; } CampusPad;
#define CAMPUS_MAX_PADS 256

/* Campus heightfield: per-room flat pads (footprint at floor_y) blended with
   fBm hills, lowest-pad-wins at overlaps, faded to 0 at the rim (so the skirt
   works like make_terrain). w/d = rectangle size; amp = hill amplitude. */
float campus_height(const CampusPad *pads, int npads,
                    float w, float d, float amp, unsigned seed,
                    float lx, float lz);

/* Build the campus terrain mesh (top grid + skirt + base), sampling
   campus_height. sub = tessellation (clamped 2..96). */
void  make_campus(MeshBuilder *b, const CampusPad *pads, int npads,
                  float w, float d, int sub, float amp, unsigned seed);
```

- [ ] **Step 2: Write the failing unit test `campus_test.c`**

Create `campus_test.c`:
```c
/* campus_test: pure checks on campus_height (no GL, no scene). */
#include "mesh.h"
#include <stdio.h>
#include <math.h>

static int fails = 0;
static void check(const char *what, float got, float lo, float hi) {
    int ok = (got >= lo && got <= hi);
    printf("%-40s got=%7.3f  [%.3f, %.3f]  %s\n",
           what, got, lo, hi, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

int main(void) {
    /* two pads in a 60x60 campus: A at origin floor 0, B at +x20 floor 10.
       amp=0 so there are no hills -> deterministic. */
    CampusPad two[2];
    CampusPad overlap[2];
    two[0].cx = 0.0f;  two[0].cz = 0.0f;  two[0].hw = 3.0f; two[0].hd = 3.0f; two[0].floor_y = 0.0f;
    two[1].cx = 20.0f; two[1].cz = 0.0f;  two[1].hw = 3.0f; two[1].hd = 3.0f; two[1].floor_y = 10.0f;

    check("on pad A centre -> A floor",
          campus_height(two, 2, 60.0f, 60.0f, 0.0f, 7u, 0.0f, 0.0f),  -0.05f, 0.05f);
    check("on pad B centre -> B floor",
          campus_height(two, 2, 60.0f, 60.0f, 0.0f, 7u, 20.0f, 0.0f),  9.95f, 10.05f);
    check("midpoint -> between the two floors",
          campus_height(two, 2, 60.0f, 60.0f, 0.0f, 7u, 10.0f, 0.0f),  4.0f, 6.0f);

    /* overlap: two pads at the same spot, floors 0 and 8 -> lowest (0) wins. */
    overlap[0].cx = 0.0f; overlap[0].cz = 0.0f; overlap[0].hw = 5.0f; overlap[0].hd = 5.0f; overlap[0].floor_y = 0.0f;
    overlap[1].cx = 0.0f; overlap[1].cz = 0.0f; overlap[1].hw = 5.0f; overlap[1].hd = 5.0f; overlap[1].floor_y = 8.0f;
    check("overlap -> lowest pad wins",
          campus_height(overlap, 2, 60.0f, 60.0f, 0.0f, 7u, 0.0f, 0.0f), -0.05f, 0.05f);

    /* outside the rectangle -> 0 (rim). */
    check("outside rect -> 0",
          campus_height(two, 2, 60.0f, 60.0f, 0.0f, 7u, 40.0f, 0.0f), -0.001f, 0.001f);

    printf(fails ? "\n%d FAIL\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
```

- [ ] **Step 3: Add the `campustest` build case**

In `build.sh`, find the `furnituretest)` case (grep `furnituretest`) and add an analogous case right after it (copy its compile line; swap the test source + output name):
```sh
  campustest)
    clang -std=c89 -pedantic-errors -Werror -g -fsanitize=address,undefined \
      campus_test.c mesh.c flora.c rock.c gothic.c sweep.c sol_math.c \
      -o campus_test && ./campus_test
    ;;
```
(Match the exact flags/structure of the real `furnituretest)` case; the source list mirrors `furniture_test`'s minus `furniture.c` plus `campus_test.c`.)

- [ ] **Step 4: Run it to confirm it FAILS to link** (campus_height not defined yet)

Run: `./build.sh campustest`
Expected: link error — `undefined symbol: _campus_height` (and `make_campus`).

- [ ] **Step 5: Implement `campus_rect_dist` + `campus_height` in `mesh.c`**

`campus_height` calls the file-static `terrain_fbm` and uses `TERRAIN_FEATURE_M`, so it MUST live in `mesh.c` AFTER `terrain_fbm` (grep `static sol_f32 terrain_fbm`) and after `terrain_height`. Add, immediately after `terrain_height`'s closing brace (grep `float terrain_height(`):
```c
#define CAMPUS_APRON 3.0f     /* m: flatten radius around each room footprint */
#define CAMPUS_RIM   0.12f    /* rim-fade fraction of min(w,d) */

/* distance from (lx,lz) to a pad's footprint rectangle; 0 if inside. */
static float campus_rect_dist(float lx, float lz, const CampusPad *p) {
    float ax = (lx > p->cx ? lx - p->cx : p->cx - lx) - p->hw;   /* >0 outside in x */
    float az = (lz > p->cz ? lz - p->cz : p->cz - lz) - p->hd;
    float ex = ax > 0.0f ? ax : 0.0f;
    float ez = az > 0.0f ? az : 0.0f;
    return (float)sqrt((double)(ex * ex + ez * ez));
}

float campus_height(const CampusPad *pads, int npads,
                    float w, float d, float amp, unsigned seed,
                    float lx, float lz) {
    float hw = w * 0.5f, hd = d * 0.5f;
    float sumw = 0.0f, sumwh = 0.0f, infl = 0.0f;
    float base, floor_ctrl, flat, hills, edge, ez2, margin, rim, inside_min = 0.0f;
    int   i, any_inside = 0;
    if (lx < -hw || lx > hw || lz < -hd || lz > hd) return 0.0f;
    for (i = 0; i < npads; i++) {
        float dr = campus_rect_dist(lx, lz, &pads[i]);
        float wi = 1.0f / (dr * dr + 0.01f);                  /* inverse-distance weight */
        float fi = 1.0f - sol_smoothstep(dr / CAMPUS_APRON);  /* 1 on footprint -> 0 past apron */
        sumw  += wi;
        sumwh += wi * pads[i].floor_y;
        if (fi > infl) infl = fi;
        if (dr <= 0.0f) {                                     /* inside this footprint */
            if (!any_inside || pads[i].floor_y < inside_min) inside_min = pads[i].floor_y;
            any_inside = 1;                                   /* lowest pad wins */
        }
    }
    base       = (sumw > 0.0f) ? sumwh / sumw : 0.0f;         /* smooth pad-height interpolation */
    floor_ctrl = any_inside ? inside_min : base;
    flat       = base + (floor_ctrl - base) * infl;           /* -> floor near rooms, base between */
    hills      = amp * terrain_fbm(lx / TERRAIN_FEATURE_M, lz / TERRAIN_FEATURE_M, seed);
    edge = hw - (lx < 0.0f ? -lx : lx);                       /* rim fade (mirror terrain_height) */
    ez2  = hd - (lz < 0.0f ? -lz : lz);
    if (ez2 < edge) edge = ez2;
    margin = (w < d ? w : d) * CAMPUS_RIM;
    rim    = sol_smoothstep(edge / margin);
    return (flat + hills * (1.0f - infl)) * rim;              /* fade whole height to 0 at the rim */
}
```

- [ ] **Step 6: Add a temporary `make_campus` stub so the test links**

The test links `mesh.c` which declares `make_campus` in the header; the linker needs the symbol even though the test doesn't call it. Add a minimal real `make_campus` now (Task 2 already needs it — implement it fully here to avoid a throwaway). Immediately after `campus_height`, add:
```c
void make_campus(MeshBuilder *b, const CampusPad *pads, int npads,
                 float w, float d, int sub, float amp, unsigned seed) {
    sol_f32 hw = w * 0.5f, hd = d * 0.5f, bt;
    int     i, j;
    if (sub < 2)  sub = 2;
    if (sub > 96) sub = 96;
    bt = 0.4f + amp * 0.2f;
    for (j = 0; j <= sub; j++) {
        for (i = 0; i <= sub; i++) {
            sol_f32 x = -hw + w * (sol_f32)i / (sol_f32)sub;
            sol_f32 z = -hd + d * (sol_f32)j / (sol_f32)sub;
            sol_f32 e = w / (sol_f32)sub;
            sol_f32 h  = campus_height(pads, npads, w, d, amp, seed, x, z);
            sol_f32 nx = campus_height(pads, npads, w, d, amp, seed, x - e, z)
                       - campus_height(pads, npads, w, d, amp, seed, x + e, z);
            sol_f32 nz = campus_height(pads, npads, w, d, amp, seed, x, z - e)
                       - campus_height(pads, npads, w, d, amp, seed, x, z + e);
            sol_f32 ny = 2.0f * e;
            sol_f32 nl = (sol_f32)sqrt((double)(nx * nx + ny * ny + nz * nz));
            if (nl < 1e-6f) nl = 1.0f;
            mb_push_vertex(b, x, h, z, nx / nl, ny / nl, nz / nl, x, z);
        }
    }
    for (j = 0; j < sub; j++) {
        for (i = 0; i < sub; i++) {
            sol_u32 v0 = (sol_u32)(j * (sub + 1) + i);
            sol_u32 v1 = v0 + 1;
            sol_u32 v2 = v0 + (sol_u32)(sub + 1);
            sol_u32 v3 = v2 + 1;
            mb_push_triangle(b, v0, v2, v3);
            mb_push_triangle(b, v0, v3, v1);
        }
    }
    face_x(b, -hw, -bt, 0.0f, -hd, hd, -1);     /* skirt + base (rim is 0, like make_terrain) */
    face_x(b,  hw, -bt, 0.0f, -hd, hd,  1);
    face_z(b, -hw, hw, -bt, 0.0f, -hd, -1);
    face_z(b, -hw, hw, -bt, 0.0f,  hd,  1);
    face_y(b, -hw, hw, -bt, -hd, hd, -1);
}
```
(`face_x`/`face_z`/`face_y` and `mb_push_vertex`/`mb_push_triangle` are already file-static/available in `mesh.c` — `make_terrain` right above uses them identically.)

- [ ] **Step 7: Run the test — expect PASS**

Run: `./build.sh campustest`
Expected: all five checks print `ok`, final line `all ok`, exit 0. If a check fails, fix `campus_height` (most likely the IDW/rim constants) until green.

- [ ] **Step 8: Commit**
```bash
git add mesh.h mesh.c campus_test.c build.sh
git commit -m "$(cat <<'EOF'
Campus terrain: campus_height (pads + noise) + make_campus + unit test

Pure pad-blend heightfield: IDW interpolation of per-room floor heights,
footprint-flatten with lowest-pad-wins, fBm hills between, zero-rim fade so the
skirt works like make_terrain. campus_test covers on-pad / between / overlap /
outside. make_campus mirrors make_terrain reading campus_height.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: `g_campus` state + `campus_rebuild` (gather pads, size the rectangle, build the mesh)

**Files:** Modify `main.c`.

- [ ] **Step 1: Add the campus state + tuning constants**

Near the other surfacing/derived constants (grep `#define ROOM_FRAME_MAX`), add:
```c
#define CAMPUS_SUB      72        /* campus tessellation (clamped 2..96 in make_campus) */
#define CAMPUS_HILL_AMP 2.0f      /* hill amplitude between rooms (m) */
#define CAMPUS_SEED     7u        /* campus noise identity */
```
Near the `g_room_frame` cache declaration (grep `static RoomFrame g_room_frame`), add the campus state:
```c
typedef struct {
    int       enabled;
    vec3      center;             /* world XZ centre of the rectangle (y unused) */
    float     w, d;               /* rectangle size */
    float     y0, amp_range;      /* for the terrain slope/height palette */
    CampusPad pads[CAMPUS_MAX_PADS];
    int       npads;
    Mesh      mesh;
} CampusState;
static CampusState g_campus;       /* derived: the active world's campus, or disabled */
```

- [ ] **Step 2: Gather the active world's room pads**

`editor_room_rect` (editor.c) already returns `{cx,cz,hw,hd,floor_y}` for a room anchor or island. Add, immediately BEFORE `connections_rebuild` (grep `static void connections_rebuild`), a gatherer that fills `g_campus.pads` from active room anchors and reports the footprint bounding box:
```c
/* fill g_campus.pads from the active world's ROOM anchors (not islands), and
   return the footprint bounding box via lo/hi. Returns the pad count. */
static int campus_gather_pads(AppState *st, vec3 *lo, vec3 *hi) {
    Scene  *s = &st->scene;
    int     n = 0;
    sol_u32 i;
    float   minx = 0, maxx = 0, minz = 0, maxz = 0;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        const char  *rt;
        RoomRect     r;
        if (o->mesh_ref) continue;                       /* room parents are empties */
        rt = scene_meta_get(s, o->handle, "room_type");
        if (!rt) continue;
        if (strcmp(rt, "home") != 0 && strcmp(rt, "mirror") != 0) continue;
        if (!scene_object_active(s, o->handle)) continue; /* this world only */
        if (n >= CAMPUS_MAX_PADS) break;
        r = editor_room_rect(s, o->handle);
        g_campus.pads[n].cx = r.cx; g_campus.pads[n].cz = r.cz;
        g_campus.pads[n].hw = r.hw; g_campus.pads[n].hd = r.hd;
        g_campus.pads[n].floor_y = r.floor_y;
        if (n == 0) {
            minx = r.cx - r.hw; maxx = r.cx + r.hw;
            minz = r.cz - r.hd; maxz = r.cz + r.hd;
        } else {
            if (r.cx - r.hw < minx) minx = r.cx - r.hw;
            if (r.cx + r.hw > maxx) maxx = r.cx + r.hw;
            if (r.cz - r.hd < minz) minz = r.cz - r.hd;
            if (r.cz + r.hd > maxz) maxz = r.cz + r.hd;
        }
        n++;
    }
    *lo = vec3_make(minx, 0.0f, minz);
    *hi = vec3_make(maxx, 0.0f, maxz);
    return n;
}
```
Note: `RoomRect` / `editor_room_rect` come from `editor.h` (already included by main.c — confirm the `#include "editor.h"` is present; it is, since the editor lives in main.c's includes).

- [ ] **Step 3: `campus_rebuild` — flag check, rectangle, mesh**

Immediately after `campus_gather_pads`, add:
```c
/* (re)build the active world's campus terrain into g_campus, or disable it.
   Derived: called at the end of connections_rebuild (every structural change). */
static void campus_rebuild(AppState *st) {
    const char *wsname = st->scene.active_ws[0] ? st->scene.active_ws : "home";
    sol_u32     anchor = workspace_anchor_find(&st->scene, wsname);
    const char *flag   = anchor ? scene_meta_get(&st->scene, anchor, "campus") : (const char *)0;
    vec3        lo, hi;
    float       margin, miny, maxy;
    int         k;
    MeshBuilder mb;
    mesh_destroy(&g_campus.mesh);
    g_campus.enabled = (flag && strcmp(flag, "1") == 0);
    g_campus.npads   = 0;
    if (!g_campus.enabled) return;
    g_campus.npads = campus_gather_pads(st, &lo, &hi);
    if (g_campus.npads == 0) { g_campus.enabled = 0; return; }    /* nothing to ground */
    /* rubber-band rectangle: footprint bbox + a margin proportional to its size
       (so rooms stay inside the un-faded rim zone). */
    margin = 0.15f * ((hi.x - lo.x) > (hi.z - lo.z) ? (hi.x - lo.x) : (hi.z - lo.z));
    if (margin < 8.0f) margin = 8.0f;
    g_campus.center = vec3_make((lo.x + hi.x) * 0.5f, 0.0f, (lo.z + hi.z) * 0.5f);
    g_campus.w = (hi.x - lo.x) + 2.0f * margin;
    g_campus.d = (hi.z - lo.z) + 2.0f * margin;
    /* pads to campus-local (centre at origin); track the height range for the palette */
    miny = maxy = g_campus.pads[0].floor_y;
    for (k = 0; k < g_campus.npads; k++) {
        g_campus.pads[k].cx -= g_campus.center.x;
        g_campus.pads[k].cz -= g_campus.center.z;
        if (g_campus.pads[k].floor_y < miny) miny = g_campus.pads[k].floor_y;
        if (g_campus.pads[k].floor_y > maxy) maxy = g_campus.pads[k].floor_y;
    }
    g_campus.y0        = miny;
    g_campus.amp_range = (maxy - miny) + CAMPUS_HILL_AMP;
    if (g_campus.amp_range < 0.001f) g_campus.amp_range = 0.001f;
    mb_init(&mb);
    make_campus(&mb, g_campus.pads, g_campus.npads, g_campus.w, g_campus.d,
                CAMPUS_SUB, CAMPUS_HILL_AMP, CAMPUS_SEED);
    if (mb.index_count > 0) g_campus.mesh = mesh_from_builder(&mb);
    mb_free(&mb);
}
```
(`workspace_anchor_find` is declared in `workspace.h` — confirm main.c includes it; it does, since the `:` portal/workspace commands use it.)

- [ ] **Step 4: Hook it at the end of `connections_rebuild`**

`connections_rebuild` (grep `static void connections_rebuild`) runs on every structural change (load, `world_rebuild`, editor commit, room add/move/delete). Add `campus_rebuild(st);` as the LAST statement before its closing brace:
```c
    /* ... existing room-shell + frame build loop ends ... */
    campus_rebuild(st);   /* the campus follows the rooms */
}
```

- [ ] **Step 5: Build** — `./build.sh c89check && ./build.sh debug && ./build.sh metal` — all PASS. (No draw yet; the mesh is built but unused, so expect possibly an unused-warning ONLY if `g_campus.mesh` is never read — it is read by `mesh_destroy`, so no warning. If c89check flags anything, fix minimally.)

- [ ] **Step 6: Commit**
```bash
git add main.c
git commit -m "$(cat <<'EOF'
Campus terrain: g_campus state + campus_rebuild (gather pads, size, mesh)

Derived per-world campus: gathers the active world's room pads (editor_room_rect),
sizes a rubber-band rectangle (footprint bbox + proportional margin), and builds
the campus mesh via make_campus. Gated on meta["campus"]=="1" on the world's
workspace anchor; hooked at the end of connections_rebuild.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Draw the campus

**Files:** Modify `main.c`.

- [ ] **Step 1: Draw `g_campus.mesh` with the terrain palette**

Find the room-frame overlay draw block (grep `rf->door_trim` — the campus draw goes right after that `}` so it's in the same opaque pass, using the same `view`/`proj`/`eye`). Terrain is drawn with the slope/height palette via `state->terrain_blend` + `terrain_y0` + `terrain_amp` (grep `state->terrain_blend = (o->mesh_ref` to see how the object loop sets them). Add:
```c
    /* the campus ground (derived; drawn with the terrain slope/height palette) */
    if (g_campus.enabled && g_campus.mesh.index_count > 0) {
        mat4     cm = mat4_translate(g_campus.center);   /* heights are world-absolute */
        Material cmat = material_default();
        state->terrain_blend = SOL_TRUE;
        state->terrain_y0    = g_campus.y0;
        state->terrain_amp   = g_campus.amp_range;
        draw_mesh(state, g_campus.mesh, cm, view, proj, eye, 0.0f, cmat);
        state->terrain_blend = SOL_FALSE;
    }
```
Confirm the field names against the object loop: it sets `state->terrain_blend`, `state->terrain_y0`, `state->terrain_amp` (grep them) — match exactly. `mat4_translate` and `material_default` are already used in main.c.

- [ ] **Step 2: Build + live-smoke** — `./build.sh c89check && ./build.sh debug && ./build.sh metal` all PASS. (Visual verify deferred to Task 6, after the toggle command exists — there's no way to enable a campus yet.)

- [ ] **Step 3: Commit**
```bash
git add main.c
git commit -m "$(cat <<'EOF'
Campus terrain: draw the campus mesh with the terrain palette

Draws g_campus.mesh in the opaque pass at the rectangle centre (heights are
world-absolute), using the slope/height terrain_blend palette keyed to the
campus's own height range. No-op when disabled.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Stand on the campus

**Files:** Modify `main.c`.

- [ ] **Step 1: Sample `campus_height` in `ground_under`**

`ground_under` (grep `static float ground_under`) loops terrain scene objects and keeps the highest ground within the step treaty. The campus is NOT a scene object, so add an explicit branch just before `if (out_plot) *out_plot = best_plot;` (the end of the function):
```c
    if (g_campus.enabled) {                              /* the derived campus ground */
        float lx = p.x - g_campus.center.x;
        float lz = p.z - g_campus.center.z;
        float hw = g_campus.w * 0.5f, hd = g_campus.d * 0.5f;
        if (lx >= -hw && lx <= hw && lz >= -hd && lz <= hd) {
            float gy = campus_height(g_campus.pads, g_campus.npads, g_campus.w,
                                     g_campus.d, CAMPUS_HILL_AMP, CAMPUS_SEED, lx, lz);
            if (gy <= p.y + COLLIDE_STEP_UP && gy > best) { best = gy; best_plot = 0; }
        }
    }
```
(`campus_height` returns world-absolute Y, so no transform is needed — only the XZ offset by `center`.)

- [ ] **Step 2: Build** — `./build.sh c89check && ./build.sh debug && ./build.sh metal` all PASS.

- [ ] **Step 3: Commit**
```bash
git add main.c
git commit -m "$(cat <<'EOF'
Campus terrain: stand on the campus (ground_under samples campus_height)

ground_under adds a campus branch: when the player is over the active campus
rectangle, campus_height gives the world-Y ground, folded into the same
step-treaty max as terrain/architecture. Mirrors the terrain_height path.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: The "Campus mode" toggle command

**Files:** Modify `main.c`.

- [ ] **Step 1: Add the command handler**

Near the other `cmd_*` handlers (grep `static void cmd_mint_island`), add:
```c
/* toggle the active world's persistent campus flag, then re-derive. */
static void cmd_toggle_campus(AppState *st) {
    const char *wsname = st->scene.active_ws[0] ? st->scene.active_ws : "home";
    sol_u32     anchor = workspace_anchor_add(&st->scene, wsname);   /* find-or-create */
    const char *cur    = scene_meta_get(&st->scene, anchor, "campus");
    int         on     = !(cur && strcmp(cur, "1") == 0);
    scene_meta_set(&st->scene, anchor, "campus", on ? "1" : "0");
    scene_save(&st->scene, "scene.stml");
    world_rebuild(st);                                  /* re-derive incl. campus_rebuild */
    printf("campus %s for world '%s'\n", on ? "on" : "off", wsname);
}
```
(`workspace_anchor_add` is declared in `workspace.h`; `world_rebuild` calls `connections_rebuild`, which now calls `campus_rebuild`.)

- [ ] **Step 2: Register it in `g_commands[]`**

In the `g_commands[]` array (grep `static Command g_commands`), add a palette-only row (no key — letter keys are taken; this is a mode toggle):
```c
    { "Campus mode",                 NULL, 0,          cmd_toggle_campus,     NULL,                  SOL_FALSE },
```

- [ ] **Step 3: Build** — `./build.sh c89check && ./build.sh debug && ./build.sh metal` all PASS.

- [ ] **Step 4: Commit**
```bash
git add main.c
git commit -m "$(cat <<'EOF'
Campus terrain: ":" Campus mode toggle (persistent per-world flag)

cmd_toggle_campus flips meta["campus"] on the active world's workspace anchor,
saves, and world_rebuilds. Palette-only command (mode toggle). Persists across
loads; floating worlds are untouched.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: Gauntlet, live-verify, finish

- [ ] **Step 1: Full gauntlet** — `./build.sh c89check && ./build.sh debug && ./build.sh metal && ./build.sh campustest` — all PASS.

- [ ] **Step 2: Human live-verify (Fran)** — on `./solarium` and `./solarium-metal`:
  - In a world with a few rooms, run `:` → **"Campus mode"**. A terrain rectangle appears beneath the rooms, flat under each room at its floor height, with noise hills between, fading to a skirt at the rim.
  - Each room sits flush on its pad (no floating gap, no terrain poking through the floor); walking out of a room you step onto graded ground and can walk the hills between rooms.
  - A room at a different height sits on its own raised/lowered pad with the ground sloping to meet it.
  - Stacked rooms: the ground grades to the lowest; higher ones still float (expected).
  - Walkways still bridge the rooms over the terrain (not buried — if a hill swallows a walkway, lower `CAMPUS_HILL_AMP`).
  - `:` → "Campus mode" again removes it (floating restored). Reload (`L`) keeps the campus (flag persisted).
  - A different (non-campus) world is unaffected.
  - Tune `CAMPUS_HILL_AMP` / `CAMPUS_APRON` / `CAMPUS_RIM` / `CAMPUS_SUB` / the margin if proportions are off.

- [ ] **Step 3: Finish** — superpowers:finishing-a-development-branch; ff-merge `campus-stage1` to `main`. Do NOT stage `NOTES.stml`/`paper-picture.png`. (Then Milestone 2: flora.)

---

## Plan self-review

**Spec coverage (Milestone 1 scope):** `campus_height` pads+noise+lowest-wins+rim ✓ (T1); pad gathering reusing `editor_room_rect` ✓ (T2 S2); rubber-band rectangle ✓ (T2 S3); campus mesh reusing `make_terrain`'s skirt/base/normals ✓ (T1 S6 `make_campus`); opt-in `:` command + persistent workspace-meta flag ✓ (T5); rebuild hook on structural events via `connections_rebuild` ✓ (T2 S4); standing via `ground_under` ✓ (T4); coexists via `active_ws`/`scene_object_active` filter in `campus_gather_pads` ✓ (T2 S2). **Deviation from spec:** no separate baked grid — `campus_height` is evaluated directly (mesh vertices + `ground_under`), matching the existing `terrain_height` pattern; justified because the pad count is tiny and it keeps standing consistent with the visible mesh. Flora is explicitly Milestone 2 (not here). ✓

**Placeholder scan:** none — every step has full code or an exact grep anchor + the surrounding pattern to match.

**Type consistency:** `CampusPad{cx,cz,hw,hd,floor_y}`, `campus_height(pads,npads,w,d,amp,seed,lx,lz)`, `make_campus(...,sub,...)`, `CampusState g_campus`, `campus_gather_pads(st,*lo,*hi)`, `campus_rebuild(st)`, `cmd_toggle_campus`, and the constants `CAMPUS_SUB/HILL_AMP/SEED/APRON/RIM` are used identically across tasks. `ground_under` and `make_campus` and `campus_rebuild` all pass `CAMPUS_HILL_AMP`/`CAMPUS_SEED` so the standing height matches the drawn mesh exactly. The campus draw reads `g_campus.y0`/`amp_range` set in `campus_rebuild`.
