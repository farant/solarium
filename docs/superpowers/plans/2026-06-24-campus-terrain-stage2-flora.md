# Campus Terrain — Milestone 2 (Flora) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Scatter grass + trees over the campus terrain (sampling `campus_height` so they sit on the ground), skipping the room footprints and walkway corridors so plants grow on the open hills, not indoors or on the paths.

**Architecture:** A **separate campus-flora path** (NOT the existing `meadow[]`/`forest[]` scene-keyed arrays — the forest draw needs a valid scene handle for its model matrix, and `vis[]` is handle-indexed, neither of which a derived global has). A new `CampusFlora g_campus_flora` holds instance buffers; `campus_flora_rebuild` scatters grass (reusing the meadow tuft format + colors) and trees (reusing `forest_place` + `FOREST_VARIANTS`) over `g_campus`, sampling `campus_height` and skipping points inside any pad (a new pure `campus_point_blocked`); a draw block renders them by reusing the **existing meadow + ornament pipelines and meshes**, the campus grass in world space and the trees translated by `g_campus.center`. Hooked right after `campus_rebuild` (end of `connections_rebuild`) so it follows the campus; a no-op when the campus is disabled.

**Tech Stack:** C89; the existing `meadow_rnd`/`forest_rnd`, `forest_place`, `FOREST_VARIANTS`, the meadow/ornament pipelines + meshes, `campus_height`/`campus_rect_dist`, RHI instance buffers.

**C89 reminders:** decls at top of block; `/* */` only; no C99 math (`(float)sqrt((double)x)`, ternary abs); `c89check` = `-Wall -Wextra -Werror -pedantic` (co-locate statics with first caller; no unused). **Never commit** `NOTES.stml`/`paper-picture.png`. Commit trailer: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

**Only `campus_point_blocked` is unit-testable** (pure); the scatter/draw get the build gauntlet + human live-verify.

**Branch:** `campus-stage2` (create at start; ff-merge to `main` at end).

**What already exists (Milestone 1, on main):** `g_campus` (`CampusState{ enabled, center, w, d, y0, amp_range, CampusPad pads[CAMPUS_MAX_PADS], npads, mesh }`); `campus_height(pads,npads,w,d,amp,seed,lx,lz)` → world-absolute Y (mesh.h); the static `campus_rect_dist(lx,lz,pad)` (mesh.c); `CAMPUS_SEED`(7u), `CAMPUS_HILL_AMP`(2.0f) (main.c); `campus_rebuild(st, routes, n)` hooked at the end of `connections_rebuild`. The pads array contains BOTH room footprints (grown) AND walkway-corridor footprints — so "inside any pad" = "inside a room or on a path".

**What to reuse (read these first):** `meadow_rnd`/`forest_rnd` (LCG, main.c ~3086/5400); `forest_place(int v, float lx, float lz, float h, float yaw, float sc, float *wood[], int wn[], float *can[], int cn[])` (main.c ~5408 — fills `wood[v]` + `can[lk]` from the cached canopy slots `g_var_slots`, gated on `g_forest_built`; caps canopy at `FOREST_CANOPY_CAP`); `FOREST_VARIANTS[FOREST_VARIANT_COUNT]` + `FOREST_TREE_VARIANTS`(5) + `forest_bark_material()`; the meadow draw (main.c ~12695: pipeline `state->meadow_pipeline`, mesh `state->meadow_vbuf`/`meadow_ibuf`, 12 indices, instance fmt `[x,y,z,size,r,g,b,a]` world-space) and the forest draw (~12727: `state->ornament_pipeline`, `state->forest_wood[v]`, `state->orn_mesh[ORN_LEAF_BROAD|ORN_LEAF_CONIFER]`, `bind_scene_uniforms(state,pipe,model,view,proj,eye,0,mat)`, wood fmt `[lx,h,lz,yaw,sx,sy,0,0]` drawn via `uModel`, leaf material `flora_leaf_color(FLORA_OAK|FLORA_PINE)`).

---

## File structure

- **mesh.h / mesh.c** — `campus_point_blocked` (pure: is (lx,lz) within `clear` of any pad?).
- **campus_test.c** — extend with `campus_point_blocked` checks.
- **main.c** — `CampusFlora g_campus_flora` + constants; `campus_flora_free`; `campus_flora_rebuild` (grass + trees scatter); a forward decl + the `connections_rebuild` hook; the draw block.

---

### Task 0: Branch

- [ ] **Step 1:** `git checkout -b campus-stage2`

---

### Task 1: `campus_point_blocked` (pure) + unit test

**Files:** Modify `mesh.h`, `mesh.c`, `campus_test.c`.

- [ ] **Step 1: Declare it in `mesh.h`** (after the `campus_height` declaration, grep `float campus_height`):
```c
/* 1 if (lx,lz) is within `clear` of ANY pad's footprint (a room or a walkway
   corridor) — used to keep flora off buildings and paths. */
int campus_point_blocked(const CampusPad *pads, int npads,
                         float lx, float lz, float clear);
```

- [ ] **Step 2: Add the failing checks to `campus_test.c`** (inside `main`, after the existing campus_height checks, before the final `printf`):
```c
    /* campus_point_blocked: a 6x6 pad at origin blocks nearby points. */
    {
        CampusPad pad;
        pad.cx = 0.0f; pad.cz = 0.0f; pad.hw = 3.0f; pad.hd = 3.0f; pad.floor_y = 0.0f;
        check("blocked: inside the pad",
              (float)campus_point_blocked(&pad, 1, 0.0f, 0.0f, 0.5f), 0.5f, 1.5f);
        check("blocked: just outside within clearance",
              (float)campus_point_blocked(&pad, 1, 3.2f, 0.0f, 0.5f), 0.5f, 1.5f);
        check("clear: well outside",
              (float)campus_point_blocked(&pad, 1, 10.0f, 0.0f, 0.5f), -0.5f, 0.5f);
    }
```
(check() asserts a value in a range; `campus_point_blocked` returns 0/1, so 1→[0.5,1.5], 0→[-0.5,0.5].)

- [ ] **Step 3: Run — expect link failure** (`campus_point_blocked` undefined): `./build.sh campustest` → undefined symbol.

- [ ] **Step 4: Implement in `mesh.c`** immediately after `campus_height` (grep `float campus_height`) — it calls the file-static `campus_rect_dist`, so it must be in mesh.c after it:
```c
int campus_point_blocked(const CampusPad *pads, int npads,
                         float lx, float lz, float clear) {
    int i;
    for (i = 0; i < npads; i++)
        if (campus_rect_dist(lx, lz, &pads[i]) < clear) return 1;
    return 0;
}
```

- [ ] **Step 5: Run — expect PASS:** `./build.sh campustest && ./campus_test` → all checks `ok`, `all ok`.

- [ ] **Step 6: Commit:**
```bash
git add mesh.h mesh.c campus_test.c
git commit -m "$(cat <<'EOF'
Campus flora: campus_point_blocked (skip pads when scattering) + test

Pure helper: is (lx,lz) within `clear` of any pad's footprint (room or walkway
corridor). Used by the campus flora scatter to keep grass/trees off buildings
and paths. Unit-tested (inside / clearance band / clear).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: `g_campus_flora` state + `campus_flora_rebuild` (scatter grass + trees)

**Files:** Modify `main.c`.

- [ ] **Step 1: Constants + state.** Near the campus constants (grep `#define CAMPUS_SUB`), add:
```c
#define CAMPUS_GRASS_PER_M2 2.0f      /* grass density on the campus */
#define CAMPUS_GRASS_MAX    16000
#define CAMPUS_TREE_PER_M2  0.012f    /* tree density */
#define CAMPUS_TREE_MAX     128
#define CAMPUS_GRASS_CLEAR  0.4f      /* keep grass this far off pads (m) */
#define CAMPUS_TREE_CLEAR   1.6f      /* keep trees this far off pads (m) */
```
Near the `CampusState g_campus` declaration (grep `static CampusState g_campus`), add:
```c
typedef struct {
    RhiBuffer grass;                          int grass_n;
    RhiBuffer wood[FOREST_VARIANT_COUNT];      int wood_n[FOREST_VARIANT_COUNT];
    RhiBuffer canopy[2];                       int canopy_n[2];
} CampusFlora;
static CampusFlora g_campus_flora;             /* derived, drawn explicitly when g_campus.enabled */
```
NOTE: `RhiBuffer`, `FOREST_VARIANT_COUNT`, `meadow_rnd`, `forest_rnd`, `forest_place`, `FOREST_VARIANTS`, `FOREST_TREE_VARIANTS`, `g_forest_built`, `FOREST_CANOPY_CAP` must all be in scope at the point where `campus_flora_rebuild` is DEFINED. They are all defined in the forest region (~5249–5440), so DEFINE `campus_flora_rebuild` immediately AFTER `forest_rebuild` (grep `static void forest_rebuild`), and forward-declare it for the earlier `connections_rebuild` hook (Step 4). The `CampusFlora` typedef + `g_campus_flora` global, being near `g_campus` (~4205), are before that — fine.

- [ ] **Step 2: `campus_flora_free` + `campus_flora_rebuild`.** Immediately AFTER `forest_rebuild`'s closing brace, add:
```c
static void campus_flora_free(void) {
    int v;
    if (g_campus_flora.grass_n) rhi_destroy_buffer(g_campus_flora.grass);
    for (v = 0; v < FOREST_VARIANT_COUNT; v++)
        if (g_campus_flora.wood_n[v]) rhi_destroy_buffer(g_campus_flora.wood[v]);
    if (g_campus_flora.canopy_n[0]) rhi_destroy_buffer(g_campus_flora.canopy[0]);
    if (g_campus_flora.canopy_n[1]) rhi_destroy_buffer(g_campus_flora.canopy[1]);
    memset(&g_campus_flora, 0, sizeof g_campus_flora);
}

/* scatter grass + trees over the active campus, sampling campus_height and
   skipping the pads (rooms + walkway corridors). Reuses the meadow tuft format
   and forest_place; drawn explicitly (campus is a derived global, not a scene
   object). No-op when the campus is disabled. */
static void campus_flora_rebuild(AppState *st) {
    float   cx, cz, w, d, amp;
    int     target, placed, k;
    sol_u32 rng;
    float  *buf;
    (void)st;
    campus_flora_free();
    if (!g_campus.enabled || g_campus.npads == 0) return;
    cx = g_campus.center.x; cz = g_campus.center.z;
    w = g_campus.w; d = g_campus.d; amp = CAMPUS_HILL_AMP;

    /* --- grass (world-space instances; reuse the meadow tuft format/colors) --- */
    rng    = CAMPUS_SEED * 2654435761u + 7u;
    target = (int)(w * d * CAMPUS_GRASS_PER_M2);
    if (target > CAMPUS_GRASS_MAX) target = CAMPUS_GRASS_MAX;
    buf = (float *)malloc((size_t)(target > 0 ? target : 1) * 8 * sizeof(float));
    placed = 0;
    for (k = 0; buf && k < target; k++) {
        float lx, lz, h, sx, sz, slope, r1, r2, r3;
        lx = (meadow_rnd(&rng) - 0.5f) * (w - 1.5f);
        lz = (meadow_rnd(&rng) - 0.5f) * (d - 1.5f);
        if (campus_point_blocked(g_campus.pads, g_campus.npads, lx, lz, CAMPUS_GRASS_CLEAR)) continue;
        h  = campus_height(g_campus.pads, g_campus.npads, w, d, amp, CAMPUS_SEED, lx, lz);
        sx = campus_height(g_campus.pads, g_campus.npads, w, d, amp, CAMPUS_SEED, lx + 0.5f, lz);
        sz = campus_height(g_campus.pads, g_campus.npads, w, d, amp, CAMPUS_SEED, lx, lz + 0.5f);
        slope = (float)sqrt((double)((sx - h) * (sx - h) + (sz - h) * (sz - h))) * 2.0f;
        if (slope > 0.5f) continue;                       /* steep hillside: bare */
        r1 = meadow_rnd(&rng); r2 = meadow_rnd(&rng); r3 = meadow_rnd(&rng);
        buf[placed * 8 + 0] = cx + lx;                    /* world space */
        buf[placed * 8 + 1] = h;
        buf[placed * 8 + 2] = cz + lz;
        buf[placed * 8 + 3] = 0.16f + 0.20f * r1;
        buf[placed * 8 + 4] = 0.10f + 0.10f * r2;
        buf[placed * 8 + 5] = 0.22f + 0.16f * r3;
        buf[placed * 8 + 6] = 0.05f + 0.06f * r2;
        buf[placed * 8 + 7] = 1.0f;
        placed++;
    }
    if (buf && placed > 0) {
        g_campus_flora.grass = rhi_create_buffer(RHI_BUFFER_VERTEX, buf,
                                   (size_t)placed * 8 * sizeof(float));
        g_campus_flora.grass_n = placed;
    }
    free(buf);

    /* --- trees (local-to-centre via forest_place; needs the canopy slots) --- */
    if (g_forest_built) {
        float *wood[FOREST_VARIANT_COUNT];
        float *can[2];
        int    wn[FOREST_VARIANT_COUNT], cn[2], v, ok = 1, darts;
        rng    = CAMPUS_SEED * 2246822519u + 13u;
        target = (int)(w * d * CAMPUS_TREE_PER_M2);
        if (target > CAMPUS_TREE_MAX) target = CAMPUS_TREE_MAX;
        for (v = 0; v < FOREST_VARIANT_COUNT; v++) {
            wood[v] = (float *)malloc((size_t)(target > 0 ? target : 1) * 8 * sizeof(float));
            wn[v] = 0;
            if (!wood[v]) ok = 0;
        }
        can[0] = (float *)malloc((size_t)FOREST_CANOPY_CAP * 8 * sizeof(float));
        can[1] = (float *)malloc((size_t)FOREST_CANOPY_CAP * 8 * sizeof(float));
        cn[0] = cn[1] = 0;
        if (!can[0] || !can[1]) ok = 0;
        if (ok && target > 0) {
            placed = 0;
            for (darts = 0; darts < target * 6 && placed < target; darts++) {
                float lx, lz, h, sx, sz, slope, yaw, sc;
                lx = (forest_rnd(&rng) - 0.5f) * (w - 2.0f);
                lz = (forest_rnd(&rng) - 0.5f) * (d - 2.0f);
                if (campus_point_blocked(g_campus.pads, g_campus.npads, lx, lz, CAMPUS_TREE_CLEAR)) continue;
                h  = campus_height(g_campus.pads, g_campus.npads, w, d, amp, CAMPUS_SEED, lx, lz);
                sx = campus_height(g_campus.pads, g_campus.npads, w, d, amp, CAMPUS_SEED, lx + 0.5f, lz);
                sz = campus_height(g_campus.pads, g_campus.npads, w, d, amp, CAMPUS_SEED, lx, lz + 0.5f);
                slope = (float)sqrt((double)((sx - h) * (sx - h) + (sz - h) * (sz - h))) * 2.0f;
                if (slope > 0.45f) continue;
                v   = (int)(forest_rnd(&rng) * (float)FOREST_TREE_VARIANTS);
                if (v >= FOREST_TREE_VARIANTS) v = FOREST_TREE_VARIANTS - 1;
                yaw = forest_rnd(&rng) * 6.2831853f;
                sc  = FOREST_VARIANTS[v].scale * (0.82f + 0.36f * forest_rnd(&rng));
                forest_place(v, lx, lz, h, yaw, sc, wood, wn, can, cn);
                placed++;
            }
            for (v = 0; v < FOREST_VARIANT_COUNT; v++) {
                g_campus_flora.wood_n[v] = wn[v];
                if (wn[v] > 0)
                    g_campus_flora.wood[v] = rhi_create_buffer(RHI_BUFFER_VERTEX,
                                                 wood[v], (size_t)wn[v] * 8 * sizeof(float));
            }
            for (k = 0; k < 2; k++) {
                g_campus_flora.canopy_n[k] = cn[k];
                if (cn[k] > 0)
                    g_campus_flora.canopy[k] = rhi_create_buffer(RHI_BUFFER_VERTEX,
                                                   can[k], (size_t)cn[k] * 8 * sizeof(float));
            }
        }
        for (v = 0; v < FOREST_VARIANT_COUNT; v++) free(wood[v]);
        free(can[0]); free(can[1]);
    }
}
```
NOTE: `forest_place` stores wood as `[lx, h, lz, yaw, sc, sc, 0, 0]` (LOCAL lx/lz, but `h` is whatever you pass — here `campus_height` = world-absolute Y) and canopy as local offsets + `h`. The draw (Task 3) uses `uModel = translate(g_campus.center)` (XZ only, center.y=0), so `(center.x+lx, h, center.z+lz)` = correct world position (h already world Y). The grass is stored fully world-space and drawn with no model matrix (the meadow shader is view/proj only), matching the existing meadow path.

- [ ] **Step 3: Free at shutdown (optional but clean).** Find where the app frees meadow/forest buffers on exit (grep `rhi_destroy_buffer(st->meadow` near a cleanup/`main` teardown — if there's a `flora`/`world` teardown). If a clear teardown exists, add `campus_flora_free();` there. If there is NO obvious flora teardown (the app leaks-on-exit by design), SKIP this step and note it. (Do not invent a teardown path.)

- [ ] **Step 4: Forward-declare + hook.** Near the `world_rebuild` forward declaration (grep `static void world_rebuild(AppState \*st);`), add:
```c
static void campus_flora_rebuild(AppState *st);
```
Then in `connections_rebuild`, immediately AFTER the existing `campus_rebuild(st, routes, n);` line, add:
```c
    campus_flora_rebuild(st);   /* the campus's grass + trees follow it */
```
(This makes campus flora re-scatter on every structural rebuild that already rebuilds the campus — load, world_rebuild, editor commit, room add/move/delete — and it's a no-op for non-campus worlds. The flora is rebuilt with the campus mesh; both are derived together.)

- [ ] **Step 5: Build:** `./build.sh c89check && ./build.sh debug && ./build.sh metal` — all PASS. (Nothing draws the new buffers yet — Task 3 — so the campus flora is scattered but invisible; confirm no unused-symbol/`-Werror` issues. `g_campus_flora` is read by `campus_flora_free`; `campus_flora_rebuild` is called via the hook.) If c89check flags an unused `st` param, the `(void)st;` covers it; remove `(void)st;` only if `st` ends up used.

- [ ] **Step 6: Commit:**
```bash
git add main.c
git commit -m "$(cat <<'EOF'
Campus flora: campus_flora_rebuild (scatter grass + trees, mask pads)

g_campus_flora holds campus-specific instance buffers. campus_flora_rebuild
scatters grass (meadow tuft format/colors, world-space) and trees (forest_place
+ FOREST_VARIANTS, local-to-centre), sampling campus_height and skipping points
inside any pad (rooms + walkway corridors), gated on slope. Hooked after
campus_rebuild in connections_rebuild; no-op when the campus is off.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Draw the campus grass + trees

**Files:** Modify `main.c`.

- [ ] **Step 1: Draw block.** Find the forest draw block in the render function (grep `state->ornament_pipeline.id && state->forest_count`). Immediately AFTER that whole forest `if (...) { ... }` block (so it shares the same render state + `view`/`proj`/`eye`/`state`), add the campus-flora draw. READ the real meadow draw (grep `state->meadow_pipeline.id`) and forest draw to match the exact RHI calls/uniform names; the intended block:
```c
    /* the campus flora: grass (meadow pipeline, world-space) + trees (ornament
       pipeline, translated by the campus centre). Drawn whenever the campus is
       on -- no per-island vis cull (the campus IS the active world). */
    if (g_campus.enabled && g_campus_flora.grass_n > 0 && state->meadow_pipeline.id) {
        rhi_set_pipeline(state->meadow_pipeline);
        rhi_set_uniform_mat4("uView", view.m);
        rhi_set_uniform_mat4("uProj", proj.m);
        rhi_set_uniform_float("uTime", (float)glfwGetTime());
        rhi_set_uniform_vec3("uWind", state->wind_dx, state->wind_dz, state->wind_gust);
        rhi_bind_vertex_buffer(state->meadow_vbuf);
        rhi_bind_index_buffer(state->meadow_ibuf);
        rhi_bind_instance_buffer(g_campus_flora.grass);
        rhi_draw_indexed_instanced(0, 12, g_campus_flora.grass_n);
    }
    if (g_campus.enabled && state->ornament_pipeline.id) {
        mat4     cmodel = mat4_translate(g_campus.center);
        Material leafmat[2];
        int      v, lk;
        leafmat[0] = material_default(); leafmat[0].base_color = flora_leaf_color(FLORA_OAK);  leafmat[0].roughness = 0.85f;
        leafmat[1] = material_default(); leafmat[1].base_color = flora_leaf_color(FLORA_PINE); leafmat[1].roughness = 0.85f;
        for (v = 0; v < FOREST_VARIANT_COUNT; v++) {
            Mesh *um = &state->forest_wood[v];
            if (g_campus_flora.wood_n[v] == 0 || um->index_count == 0) continue;
            bind_scene_uniforms(state, state->ornament_pipeline, cmodel, view, proj, eye, 0.0f, forest_bark_material());
            rhi_set_uniform_float("uTime", (float)glfwGetTime());
            rhi_set_uniform_vec3("uWind", state->wind_dx, state->wind_dz, state->wind_gust);
            rhi_bind_vertex_buffer(um->vbuffer);
            rhi_bind_instance_buffer(g_campus_flora.wood[v]);
            rhi_bind_index_buffer(um->ibuffer);
            rhi_draw_indexed_instanced(0, um->index_count, g_campus_flora.wood_n[v]);
        }
        for (lk = 0; lk < 2; lk++) {
            Mesh *um = &state->orn_mesh[lk == 0 ? ORN_LEAF_BROAD : ORN_LEAF_CONIFER];
            if (g_campus_flora.canopy_n[lk] == 0 || um->index_count == 0) continue;
            bind_scene_uniforms(state, state->ornament_pipeline, cmodel, view, proj, eye, 0.0f, leafmat[lk]);
            rhi_set_uniform_float("uTime", (float)glfwGetTime());
            rhi_set_uniform_vec3("uWind", state->wind_dx, state->wind_dz, state->wind_gust);
            rhi_bind_vertex_buffer(um->vbuffer);
            rhi_bind_instance_buffer(g_campus_flora.canopy[lk]);
            rhi_bind_index_buffer(um->ibuffer);
            rhi_draw_indexed_instanced(0, um->index_count, g_campus_flora.canopy_n[lk]);
        }
    }
```
VERIFY against the real draws: the meadow `rhi_draw_indexed_instanced(0, 12, ...)` index count (12) and the buffer/pipeline names; the forest's `bind_scene_uniforms(...)` arg order, `state->forest_wood[v]`, `state->orn_mesh[ORN_LEAF_BROAD|ORN_LEAF_CONIFER]`, `forest_bark_material()`, `flora_leaf_color`, `FLORA_OAK/FLORA_PINE`, `material_default()`. If any name/shape differs, MATCH THE REAL ONE.

- [ ] **Step 2: Build:** `./build.sh c89check && ./build.sh debug && ./build.sh metal` — all PASS. (`leafmat`/`cmodel`/`v`/`lk`/`um` all consumed.)

- [ ] **Step 3: Commit:**
```bash
git add main.c
git commit -m "$(cat <<'EOF'
Campus flora: draw the campus grass + trees

Reuses the meadow pipeline/mesh (grass, world-space) and the ornament pipeline +
forest wood/canopy meshes (trees, translated by the campus centre). Drawn when
the campus is enabled, no per-island vis cull. No new shader.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Gauntlet, live-verify, finish

- [ ] **Step 1: Full gauntlet** — `./build.sh c89check && ./build.sh debug && ./build.sh metal && ./build.sh campustest && ./campus_test` — all PASS / `all ok`.

- [ ] **Step 2: Human live-verify (Fran)** — on `./solarium` and `./solarium-metal`, in a Campus-mode world:
  - Grass covers the open hills between buildings, swaying in the wind; trees dot the open areas.
  - **No grass/trees inside the rooms or on the walkways/stairs** (the pads + a clearance keep them off).
  - Plants sit ON the ground (sampling the same `campus_height` you stand on), including on the graded slopes.
  - Steep hillsides (big height changes between rooms) stay relatively bare (slope gate).
  - Toggle Campus mode OFF → grass+trees vanish with the terrain; ON again → they return. A non-campus world has none. Reload persists.
  - Tune if needed: `CAMPUS_GRASS_PER_M2`/`CAMPUS_TREE_PER_M2` (density), `CAMPUS_GRASS_CLEAR`/`CAMPUS_TREE_CLEAR` (how far off rooms/paths), the slope thresholds (0.5 grass / 0.45 trees), `CAMPUS_GRASS_MAX`/`CAMPUS_TREE_MAX` (caps).

- [ ] **Step 3: Finish** — superpowers:finishing-a-development-branch; ff-merge `campus-stage2` to `main`. Do NOT stage `NOTES.stml`/`paper-picture.png`. (Campus terrain COMPLETE: terrain + grading + flora. Note it in memory.)

---

## Plan self-review

**Spec coverage (Milestone 2):** grass + trees on the campus ✓ (T2 scatter, T3 draw); sample `campus_height` to sit on the ground ✓ (T2, same fn as standing); mask room footprints AND walkway corridors ✓ (`campus_point_blocked` over `g_campus.pads`, which holds both — T1/T2); reuse meadow/forest scatter + instancing + draw ✓ (meadow_rnd/forest_rnd/forest_place/FOREST_VARIANTS + the meadow/ornament pipelines+meshes); campus-aware path since flora keys on scene terrain objects ✓ (separate `g_campus_flora` + explicit draw, no scene handle / no `vis[]`); coexist / only when enabled ✓ (every path gated on `g_campus.enabled`, hook is a no-op when off). Flowers/scree are intentionally OUT of scope (trivial later adds, same pattern).

**Placeholder scan:** none — full code; the few "verify against the real X" notes are confirmations of names that already exist (meadow/forest draw), not deferred work.

**Type consistency:** `CampusFlora{grass,grass_n,wood[FOREST_VARIANT_COUNT],wood_n[],canopy[2],canopy_n[2]}`, `campus_point_blocked(pads,npads,lx,lz,clear)`, `campus_flora_free()`, `campus_flora_rebuild(st)`, and the constants `CAMPUS_GRASS_PER_M2/_MAX/_CLEAR`, `CAMPUS_TREE_PER_M2/_MAX/_CLEAR` are used identically across tasks. The scatter writes the 8-float instance records the existing meadow/ornament pipelines already consume; the draw binds the same meshes/pipelines those formats were authored for. `forest_place`'s wood/canopy local coords + the `translate(center)` draw model agree (h is world-Y, center.y=0). Grass is world-space + drawn with no model, matching the meadow path.
