# Plank Walls Overlay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Tile the PolyHaven `weathered_brown_planks` PBR set over room inner-wall faces, split around doorways (piers + headers), as a reversible render-time overlay.

**Architecture:** A shared `load_pbr_material` helper + a `g_wall_mat`; a `wall_panels` helper that mirrors `emit_doored_wall`'s pier/header split but emits flat inner-face quads with position-based UVs; a per-room overlay mesh built in `connections_rebuild`/`_focus` (where the doorway openings are already computed) and stored in a handle-keyed cache; a per-room draw in the opaque pass. All in `main.c` + `.gitignore`. No scene change, no shader, no MSL twin.

**Tech Stack:** C89, the existing `load_texture`/`load_texture_linear` + MeshBuilder + the `Material` PBR slots + `RoomOpening` geometry.

**Branch:** `plank-walls-overlay` (create at start; ff-merge to `main` at the end).

**C89 reminders:** decls at top of block; `/* */`; `fabs((double)x)`; c89check is `-Wall -Wextra -Werror`. **Critically, `-Wunused-function`** — each static is added in the same task as its first caller (the tasks are grouped accordingly). **Never commit** `NOTES.stml`/`paper-picture.png`. Commit trailer: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

**No unit test:** render-only (a material + per-room GPU meshes + a draw) — verification is the gauntlet (`c89check && debug && metal`; no shader → Metal risk is compile-only) + human live-verify.

**Reference (do not modify): `emit_doored_wall` in mesh.c** — `wall_panels` replicates its opening-walk. `RoomOpening` (mesh.h:58) = `{ int wall; sol_f32 center, width, height; }`; `ROOM_WALL_N=0,E=1,S=2,W=3`; `ROOM_MAX_OPENINGS_PER_WALL=8`. Walls in `make_room_doored`: N at z=−hd (faces +z), S at z=+hd (faces −z), E at x=+hw (faces −x), W at x=−hw (faces +x); `hw=w/2`, `hd=d/2`. Opening `center` is along the wall's run axis (x for N/S, z for E/W).

---

### Task 0: Branch

- [ ] **Step 1:** `git checkout -b plank-walls-overlay`

---

### Task 1: DRY material loader + the plank material

**Files:** Modify `main.c`, `.gitignore`.

- [ ] **Step 1: Add `load_pbr_material` + refactor `floor_mat_init` + add `wall_mat_init`**

In `main.c`, the floor block currently has `floor_mat_init` (right after `load_texture_linear`). Replace the existing `floor_mat_init` function with the shared helper + both inits:

```c
/* a 3-map PBR material (sourced-texture experiments): albedo sRGB, normal +
   ORM linear (ARM packs AO/Rough/Metal). albedo id 0 = load failed = disabled. */
static Material load_pbr_material(const char *diff, const char *nor, const char *arm) {
    Material m = material_default();
    m.albedo_tex = load_texture(diff);             /* sRGB */
    if (m.albedo_tex.id == 0) return m;            /* missing: stay disabled */
    m.normal_tex   = load_texture_linear(nor);
    m.mr_tex       = load_texture_linear(arm);
    m.ao_tex       = m.mr_tex;                     /* ARM: R=AO, G=rough, B=metal */
    m.base_color   = vec3_make(1.0f, 1.0f, 1.0f);
    m.metallic     = 1.0f;
    m.roughness    = 1.0f;
    m.normal_scale = 1.0f;
    m.ao_strength  = 1.0f;
    return m;
}

static void floor_mat_init(void) {
    g_floor_mat = load_pbr_material(
        "red_sandstone_tiles/red_sandstone_tiles_diff_1k.png",
        "red_sandstone_tiles/red_sandstone_tiles_nor_gl_1k.png",
        "red_sandstone_tiles/red_sandstone_tiles_arm_1k.png");
}

static void wall_mat_init(void) {
    g_wall_mat = load_pbr_material(
        "weathered_brown_planks/weathered_brown_planks_diff_1k.png",
        "weathered_brown_planks/weathered_brown_planks_nor_gl_1k.png",
        "weathered_brown_planks/weathered_brown_planks_arm_1k.png");
}
```

(`g_wall_mat` is declared in Task 2's early block, which is earlier in the file, so it is in scope here. If Task 2 hasn't run yet, this won't compile — so **do Task 1's build step after Task 2's declaration exists**, OR add the `g_wall_mat` declaration now; see Step 2.)

- [ ] **Step 2: Declare `g_wall_mat` early (before `connections_rebuild`)**

`g_wall_mat` is read by the geometry code (Task 2) which sits *before* `connections_rebuild`, so the variable must be declared there too. Grep for the `connections_rebuild` function definition (`static void connections_rebuild(`, ~main.c:4180-4200) and immediately ABOVE it add:

```c
/* ---- plank walls overlay (sourced-texture experiment, flagged) ----
   Tiled planks on room inner-wall faces, split around doorways. Render-time;
   built where the doorway openings are already computed (connections_rebuild). */
#define WALL_TILE_M    1.5f      /* meters per texture-repeat (the plank-size knob) */
#define WALL_EPS       0.01f     /* inward lift off the wall face (anti z-fight) */
#define WALL_CACHE_MAX 128

static Material g_wall_mat;       /* planks; albedo_tex.id == 0 => overlay disabled */
```

- [ ] **Step 3: Call `wall_mat_init` at startup**

After `floor_mat_init();` (the line you added in the floor work, ~main.c:10658), add:

```c
    wall_mat_init();    /* plank walls overlay (sourced experiment, flagged) */
```

- [ ] **Step 4: Gitignore the texture folder**

In `.gitignore`, after the `/red_sandstone_tiles/` line, add:

```
/weathered_brown_planks/
```

- [ ] **Step 5: Build**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: all PASS. (`g_wall_mat` is written by `wall_mat_init` but not yet read — a written-unread file-scope static does not warn under `-Wall -Wextra`. If it somehow does, proceed to Task 2 which reads it.)

Run: `git status --short weathered_brown_planks/` → expect **no output** (gitignored).

- [ ] **Step 6: Commit**

```bash
git add main.c .gitignore
git commit -m "$(cat <<'EOF'
Plank walls: DRY load_pbr_material + the plank material

Extract load_pbr_material (albedo sRGB + normal/ARM linear); floor_mat_init now
uses it, and g_wall_mat loads the weathered_brown_planks set the same way.
Declared early (before connections_rebuild) for the geometry that follows.
CC0 folder gitignored.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: The doorway-split panels + the build hooks

**Files:** Modify `main.c`.

This task adds the geometry helpers (in the early block, just below the Task 1 `g_wall_mat` declaration) **and** their callers (the two rebuild hooks), so no function is unused.

- [ ] **Step 1: Add the cache + panel + overlay-build helpers**

Immediately AFTER the `static Material g_wall_mat;` line (from Task 1 Step 2), add:

```c
static struct { sol_u32 handle; Mesh mesh; } g_wall_cache[WALL_CACHE_MAX];
static int g_wall_cache_n = 0;

/* one flat inner-face quad; position-based UVs (u = s/TILE, v = y/TILE) so planks
   tile at constant size and align across doorway gaps. runx: 1 = wall runs along
   X (s=X, face plane at z=f); 0 = runs along Z (s=Z, face plane at x=f). */
static void wall_panel_quad(MeshBuilder *b, int runx, float f, float ns,
                            float slo, float shi, float y0, float y1) {
    float   u0 = slo / WALL_TILE_M, u1 = shi / WALL_TILE_M;
    float   v0 = y0  / WALL_TILE_M, v1 = y1  / WALL_TILE_M;
    sol_u32 a, c, e, g;
    if (runx) {
        a = mb_push_vertex(b, slo, y0, f, 0.0f, 0.0f, ns, u0, v0);
        c = mb_push_vertex(b, shi, y0, f, 0.0f, 0.0f, ns, u1, v0);
        e = mb_push_vertex(b, shi, y1, f, 0.0f, 0.0f, ns, u1, v1);
        g = mb_push_vertex(b, slo, y1, f, 0.0f, 0.0f, ns, u0, v1);
    } else {
        a = mb_push_vertex(b, f, y0, slo, ns, 0.0f, 0.0f, u0, v0);
        c = mb_push_vertex(b, f, y0, shi, ns, 0.0f, 0.0f, u1, v0);
        e = mb_push_vertex(b, f, y1, shi, ns, 0.0f, 0.0f, u1, v1);
        g = mb_push_vertex(b, f, y1, slo, ns, 0.0f, 0.0f, u0, v1);
    }
    mb_push_triangle(b, a, c, e);                  /* consistent winding (engine */
    mb_push_triangle(b, a, e, g);                  /* never culls; normals are set) */
}

/* one wall's inner-face panels: mirror emit_doored_wall's opening-walk, emitting
   a flat pier between openings and a header above each opening (oy < h). */
static void wall_panels(MeshBuilder *b, int runx, float f, float ns,
                        float s0, float s1, float h,
                        const RoomOpening *ops, int n_ops, int wall_id) {
    float lo[ROOM_MAX_OPENINGS_PER_WALL];
    float hi[ROOM_MAX_OPENINGS_PER_WALL];
    float oy[ROOM_MAX_OPENINGS_PER_WALL];
    int   k = 0, i, j;
    float cur;
    for (i = 0; i < n_ops; i++) {
        float c, hwid;
        if (ops[i].wall != wall_id) continue;
        if (k >= ROOM_MAX_OPENINGS_PER_WALL) break;
        c = ops[i].center; hwid = ops[i].width * 0.5f;
        lo[k] = c - hwid; hi[k] = c + hwid; oy[k] = ops[i].height;
        k++;
    }
    for (i = 1; i < k; i++) {                       /* insertion sort by lo */
        float plo = lo[i], phi = hi[i], poy = oy[i];
        j = i - 1;
        while (j >= 0 && lo[j] > plo) {
            lo[j + 1] = lo[j]; hi[j + 1] = hi[j]; oy[j + 1] = oy[j]; j--;
        }
        lo[j + 1] = plo; hi[j + 1] = phi; oy[j + 1] = poy;
    }
    cur = s0;
    for (i = 0; i <= k; i++) {
        float gL = (i < k) ? lo[i] : s1;
        float gR = (i < k) ? hi[i] : s1;
        if (gL < s0) gL = s0;
        if (gR > s1) gR = s1;
        if (gL > cur)                              /* solid pier [cur, gL] x [0, h] */
            wall_panel_quad(b, runx, f, ns, cur, gL, 0.0f, h);
        if (i < k) {
            if (oy[i] < h)                         /* header [gL, gR] x [oy, h] */
                wall_panel_quad(b, runx, f, ns, gL, gR, oy[i], h);
            cur = gR;
        }
    }
}

/* build a room shell's wall-overlay mesh from its openings and store it in the
   handle-keyed cache (replacing any prior entry). no-op if planks disabled. */
static void wall_overlay_store(SceneObject *shell, const RoomOpening *ops, int no) {
    MeshBuilder mb;
    Mesh        m;
    float       w, d, h, hw, hd;
    int         i;
    if (g_wall_mat.albedo_tex.id == 0) return;
    w  = mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "w");
    d  = mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "d");
    h  = mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "h");
    hw = w * 0.5f; hd = d * 0.5f;
    memset(&m, 0, sizeof m);
    mb_init(&mb);
    if (mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "wn") > 0.5f)
        wall_panels(&mb, 1, -hd + WALL_EPS,  1.0f, -hw, hw, h, ops, no, ROOM_WALL_N);
    if (mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "ws") > 0.5f)
        wall_panels(&mb, 1,  hd - WALL_EPS, -1.0f, -hw, hw, h, ops, no, ROOM_WALL_S);
    if (mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "we") > 0.5f)
        wall_panels(&mb, 0,  hw - WALL_EPS, -1.0f, -hd, hd, h, ops, no, ROOM_WALL_E);
    if (mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "ww") > 0.5f)
        wall_panels(&mb, 0, -hw + WALL_EPS,  1.0f, -hd, hd, h, ops, no, ROOM_WALL_W);
    if (mb.index_count > 0) m = mesh_from_builder(&mb);
    mb_free(&mb);
    for (i = 0; i < g_wall_cache_n; i++)
        if (g_wall_cache[i].handle == shell->handle) {
            mesh_destroy(&g_wall_cache[i].mesh);
            g_wall_cache[i].mesh = m;
            return;
        }
    if (g_wall_cache_n >= WALL_CACHE_MAX) {
        static int warned = 0;
        if (!warned) { printf("wall overlay: cache full (%d rooms)\n",
                              WALL_CACHE_MAX); warned = 1; }
        mesh_destroy(&m);
        return;
    }
    g_wall_cache[g_wall_cache_n].handle = shell->handle;
    g_wall_cache[g_wall_cache_n].mesh   = m;
    g_wall_cache_n++;
}
```

- [ ] **Step 2: Hook `connections_rebuild`**

In `connections_rebuild`, the shell loop builds the room mesh (main.c:4234): `if (mb.index_count > 0) shell->mesh = mesh_from_builder(&mb);` then `mb_free(&mb);`. Immediately AFTER the `mb_free(&mb);` (and before the loop's closing `}`), add:

```c
            wall_overlay_store(shell, ops, no);   /* matching plank overlay */
```

- [ ] **Step 3: Hook `connections_rebuild_focus`**

Same edit in `connections_rebuild_focus` (the analogous spot at main.c:4315-4316, after `if (mb.index_count > 0) shell->mesh = mesh_from_builder(&mb);` / `mb_free(&mb);`):

```c
            wall_overlay_store(shell, ops, no);   /* matching plank overlay */
```

- [ ] **Step 4: Build**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: all PASS. (`wall_panel_quad`←`wall_panels`←`wall_overlay_store`←the two hooks — all called; no unused-function. Overlays are now BUILT into the cache but not yet drawn — no visible change.)

- [ ] **Step 5: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
Plank walls: doorway-split inner-face panels + build hooks

wall_panels mirrors emit_doored_wall's pier/header opening-walk but emits flat
inner-face quads with position-based tiling UVs; wall_overlay_store builds a
room's combined overlay mesh and caches it by shell handle. Hooked into
connections_rebuild and _focus where the openings are already computed.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Draw the overlay

**Files:** Modify `main.c`.

- [ ] **Step 1: Add `wall_overlay_get`**

Immediately AFTER `wall_overlay_store` (from Task 2), add the lookup:

```c
static Mesh *wall_overlay_get(sol_u32 handle) {
    int i;
    for (i = 0; i < g_wall_cache_n; i++)
        if (g_wall_cache[i].handle == handle) return &g_wall_cache[i].mesh;
    return (Mesh *)0;
}
```

- [ ] **Step 2: Draw it in the opaque pass**

The floor overlay draw block ends just after main.c:11895 (`/* sandstone floor overlay ... */` ... its closing `}`). Immediately AFTER the floor overlay block's closing `}`, add the wall draw:

```c
    /* plank walls overlay (sourced experiment): each active room's cached
       wall-panel mesh, drawn over its inner wall faces. render-only. */
    if (g_wall_mat.albedo_tex.id != 0) {
        sol_u32 wi;
        for (wi = 0; wi < state->scene.count; wi++) {
            SceneObject *o = &state->scene.objects[wi];
            Mesh        *wm;
            if (!o->mesh_ref || strcmp(o->mesh_ref, "room") != 0) continue;
            if (!scene_object_active(&state->scene, o->handle)) continue;
            if (vis && !vis[o->handle]) continue;
            wm = wall_overlay_get(o->handle);
            if (!wm || wm->index_count == 0) continue;
            draw_mesh(state, *wm, scene_world_matrix(&state->scene, o),
                      view, proj, eye, 0.0f, g_wall_mat);
        }
    }
```

(Find the floor block by grepping `sandstone floor overlay (sourced experiment): a tiled`; insert after its closing brace. `vis`/`view`/`proj`/`eye`/`state` are in scope — the floor block just above uses them.)

- [ ] **Step 3: Build both backends**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: all PASS. `wall_overlay_get`←the draw (called); no unused-function. **No shader changed** → Metal risk is compile-only.

- [ ] **Step 4: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
Plank walls: draw the cached wall overlay

Per active room, draw its cached wall-panel mesh through the lit PBR pipeline
over the inner wall faces (no new shader). Planks now visible, split around
doorways.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Gauntlet, live-verify, finish

- [ ] **Step 1: Full gauntlet**

```bash
./build.sh c89check && ./build.sh debug && ./build.sh metal
```
Expected: c89check PASS; debug + metal clean.

- [ ] **Step 2: Human live-verify (Fran)** — on `./solarium` and `./solarium-metal`: walls show tiled weathered planks; planking **splits cleanly around each doorway** (panels flank the opening, a header bridges above it, the doorway stays clear to walk through); constant tile size between rooms; no z-fighting shimmer; the floor (sandstone) still looks right. Tune `WALL_TILE_M` / `WALL_EPS` if needed. Check a descended/mirror sub-room (its doorways too).

- [ ] **Step 3: Finish** — use superpowers:finishing-a-development-branch; ff-merge `plank-walls-overlay` to `main` (or per Fran's call). Do NOT stage `NOTES.stml`/`paper-picture.png`.

---

## Plan self-review

**Spec coverage:** DRY `load_pbr_material` shared with floor (Task 1) ✓; `g_wall_mat` + disabled flag (Task 1) ✓; `wall_panels` mirroring `emit_doored_wall` with flat inner-face panels + position-based UVs + the N/S/E/W table (Task 2) ✓; per-room cache built in `connections_rebuild` + `_focus` where openings exist (Task 2) ✓; per-room opaque draw, workspace+frustum filtered (Task 3) ✓; gitignore (Task 1) ✓; no shader/MSL, render-only (whole plan) ✓; gauntlet + live-verify (Task 4) ✓.

**Placeholder scan:** none — full code + exact anchors.

**Type consistency:** `g_wall_mat`/`g_wall_cache`/`wall_panel_quad`/`wall_panels`/`wall_overlay_store`/`wall_overlay_get` and the constants `WALL_TILE_M`/`WALL_EPS`/`WALL_CACHE_MAX` are used identically across Tasks 1-3. `RoomOpening` fields (`wall`/`center`/`width`/`height`), `ROOM_WALL_*`, `ROOM_MAX_OPENINGS_PER_WALL`, and the `Material`/`Mesh`/`MeshBuilder` APIs match the headers. The unused-function ordering is handled: every static is introduced with its caller (panels+store+hooks in Task 2; get+draw in Task 3).

**Build-order note for the executor:** Task 1 Step 1 (`wall_mat_init` referencing `g_wall_mat`) and Step 2 (declaring `g_wall_mat`) must both be in place before Task 1's build — do both edits, then build. The plan lists them in that order.
