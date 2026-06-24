# Sandstone Floor Overlay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (inline) to implement this plan. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Apply the PolyHaven `red_sandstone_tiles` PBR set to room floors at constant tile size, as a reversible render-time overlay.

**Architecture:** A linear texture loader + a one-time sandstone `Material` + a `(w,d)`-keyed floor-quad cache (meter-based UVs = constant tile size) + a per-room draw over each active room's floor in the main opaque pass, through the existing lit PBR pipeline. All in `main.c` + a `.gitignore` line. No scene/persistence change, no shader, no MSL twin.

**Tech Stack:** C89, the existing `image_load`/`rhi_create_texture`/`draw_mesh`/MeshBuilder + the `Material` PBR slots.

**Branch:** `sandstone-floor-overlay` (create at start; ff-merge to `main` at the end).

**Spec:** `docs/superpowers/specs/2026-06-23-sandstone-floor-overlay-design.md`

**C89 reminders:** decls at top of block; `/* */`; `fabs((double)x)` not `fabsf`; the c89check uses `-Wall -Wextra -Werror`. **Never commit** `NOTES.stml`/`paper-picture.png`. Commit trailer: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

**Note (no unit test):** this is render-only (a material + a GPU-mesh cache + a draw loop) — there is no headless-testable logic. Verification is the build gauntlet (compiles on both backends; no shader changed) + human live-verify.

---

### Task 0: Branch

- [ ] **Step 1:** `git checkout -b sandstone-floor-overlay`

---

### Task 1: The floor overlay (all in `main.c`)

**Files:** Modify `main.c`, `.gitignore`.

- [ ] **Step 1: Add `load_texture_linear`**

`load_texture` (main.c:9559-9578) forces sRGB. Immediately AFTER its closing `}` (main.c:9578), add a linear sibling:

```c
/* like load_texture but LINEAR (RGBA8) — for normal/ORM maps that must not be
   sRGB-decoded. distinct cache key (sRGB flag false); no watcher. */
static RhiTexture load_texture_linear(const char *path) {
    Image      img;
    RhiTexture tex;
    char       key[320];
    tex.id = 0;
    tex_asset_key(path, SOL_FALSE, key);           /* linear: data maps, not colour */
    if (asset_acquire(&g_tex_assets, key, &tex, sizeof tex))
        return tex;
    if (image_load(path, &img)) {
        tex = rhi_create_texture(img.pixels, img.w, img.h, RHI_TEX_RGBA8);
        image_free(&img);
        if (tex.id) asset_store_add(&g_tex_assets, key, &tex, sizeof tex);
    } else {
        fprintf(stderr, "image load failed: %s\n", path);
    }
    return tex;
}
```

- [ ] **Step 2: Add the floor material + quad cache block**

Immediately after the `load_texture_linear` you just added, append the whole floor block:

```c
/* ---- sandstone floor overlay (sourced-texture experiment, flagged) ----
   A deliberate, reversible departure from synthesized-never-sourced: a PolyHaven
   (CC0) PBR set tiled over room floors, render-time. No scene change, no shader. */
#define FLOOR_TILE_M    1.5f     /* meters per texture-repeat (the tile-size knob) */
#define FLOOR_EPS       0.012f   /* lift above the room's own floor (anti z-fight) */
#define FLOOR_CACHE_MAX 32

static Material g_floor_mat;      /* sandstone; albedo_tex.id == 0 => overlay disabled */
static struct { float w, d; Mesh mesh; } g_floor_cache[FLOOR_CACHE_MAX];
static int      g_floor_cache_n = 0;

static void floor_mat_init(void) {
    g_floor_mat = material_default();
    g_floor_mat.albedo_tex =
        load_texture("red_sandstone_tiles/red_sandstone_tiles_diff_1k.png");   /* sRGB */
    if (g_floor_mat.albedo_tex.id == 0) return;    /* folder missing: stay disabled */
    g_floor_mat.normal_tex =
        load_texture_linear("red_sandstone_tiles/red_sandstone_tiles_nor_gl_1k.png");
    g_floor_mat.mr_tex =
        load_texture_linear("red_sandstone_tiles/red_sandstone_tiles_arm_1k.png");
    g_floor_mat.ao_tex       = g_floor_mat.mr_tex; /* ARM: R=AO, G=rough, B=metal */
    g_floor_mat.base_color   = vec3_make(1.0f, 1.0f, 1.0f);
    g_floor_mat.metallic     = 1.0f;
    g_floor_mat.roughness    = 1.0f;
    g_floor_mat.normal_scale = 1.0f;
    g_floor_mat.ao_strength  = 1.0f;
}

/* a w x d floor quad (XZ, +Y up) with meter-based tiling UVs, cached by size so a
   tile is the same physical size in every room. empty mesh on cache overflow. */
static Mesh floor_quad_for(float w, float d) {
    MeshBuilder mb;
    Mesh        m;
    int         i;
    float       uw, ud;
    sol_u32     a, b2, c, e;
    for (i = 0; i < g_floor_cache_n; i++)
        if (fabs((double)(g_floor_cache[i].w - w)) < 1e-3 &&
            fabs((double)(g_floor_cache[i].d - d)) < 1e-3)
            return g_floor_cache[i].mesh;
    memset(&m, 0, sizeof m);
    if (g_floor_cache_n >= FLOOR_CACHE_MAX) {
        static int warned = 0;
        if (!warned) { printf("floor overlay: cache full (%d sizes)\n",
                              FLOOR_CACHE_MAX); warned = 1; }
        return m;
    }
    uw = w / FLOOR_TILE_M;
    ud = d / FLOOR_TILE_M;
    mb_init(&mb);
    a  = mb_push_vertex(&mb, -w * 0.5f, 0.0f, -d * 0.5f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f);
    b2 = mb_push_vertex(&mb,  w * 0.5f, 0.0f, -d * 0.5f, 0.0f, 1.0f, 0.0f, uw,   0.0f);
    c  = mb_push_vertex(&mb,  w * 0.5f, 0.0f,  d * 0.5f, 0.0f, 1.0f, 0.0f, uw,   ud);
    e  = mb_push_vertex(&mb, -w * 0.5f, 0.0f,  d * 0.5f, 0.0f, 1.0f, 0.0f, 0.0f, ud);
    mb_push_triangle(&mb, a, c, b2);               /* CCW from +Y (matches make_grid) */
    mb_push_triangle(&mb, a, e, c);
    m = mesh_from_builder(&mb);                     /* tangents auto-computed */
    mb_free(&mb);
    g_floor_cache[g_floor_cache_n].w    = w;
    g_floor_cache[g_floor_cache_n].d    = d;
    g_floor_cache[g_floor_cache_n].mesh = m;
    g_floor_cache_n++;
    return m;
}

/* pre-build floor quads for the active rooms — called at the top of render(),
   BEFORE any rhi pass, so no GPU mesh is created mid-encoder. */
static void floor_quads_ensure(AppState *st) {
    sol_u32 i;
    if (g_floor_mat.albedo_tex.id == 0) return;
    for (i = 0; i < st->scene.count; i++) {
        SceneObject *o = &st->scene.objects[i];
        float        w, d;
        if (!o->mesh_ref || strcmp(o->mesh_ref, "room") != 0) continue;
        if (!scene_object_active(&st->scene, o->handle)) continue;
        w = mesh_ref_param("room", o->mesh_params, o->mesh_param_count, "w");
        d = mesh_ref_param("room", o->mesh_params, o->mesh_param_count, "d");
        (void)floor_quad_for(w, d);
    }
}
```

(`fabs`/`memset`/`printf`/`fprintf` are already used throughout main.c; `material_default`, `mesh_ref_param`, `scene_object_active`, the MeshBuilder trio, and `RHI_TEX_RGBA8` are all in scope.)

- [ ] **Step 3: Initialize the material at startup**

At the world-init site, after `state->albedo_tex = load_texture("paper-picture.png");` (main.c:10657), add:

```c
    floor_mat_init();   /* sandstone floor overlay (sourced experiment, flagged) */
```

- [ ] **Step 4: Pre-build the cache at the top of `render()`**

Find the `render(...)` function definition (the one with the per-object opaque loop). At its very top — before the FIRST `rhi_begin_pass` (i.e. before the shadow pass) — add:

```c
    floor_quads_ensure(state);   /* build floor quads outside any rhi pass */
```

Grep for the function signature (it takes `AppState *state`); if the parameter is named `st` rather than `state`, match it. Confirm the call lands before the first `rhi_begin_pass`.

- [ ] **Step 5: Draw the overlay in the opaque pass**

The main opaque object loop ends at main.c:11790 with `state->terrain_blend = SOL_FALSE;`. Immediately AFTER that line (and before the skinned-models block), insert:

```c
    /* sandstone floor overlay (sourced experiment): a tiled sandstone quad over
       each active room's stone floor, just above it. render-only, no scene change. */
    if (g_floor_mat.albedo_tex.id != 0) {
        sol_u32 fi;
        for (fi = 0; fi < state->scene.count; fi++) {
            SceneObject *o = &state->scene.objects[fi];
            float fw, fd;
            Mesh  fq;
            mat4  fm;
            if (!o->mesh_ref || strcmp(o->mesh_ref, "room") != 0) continue;
            if (!scene_object_active(&state->scene, o->handle)) continue;
            if (vis && !vis[o->handle]) continue;
            fw = mesh_ref_param("room", o->mesh_params, o->mesh_param_count, "w");
            fd = mesh_ref_param("room", o->mesh_params, o->mesh_param_count, "d");
            fq = floor_quad_for(fw, fd);
            if (fq.index_count == 0) continue;
            fm = mat4_mul(scene_world_matrix(&state->scene, o),
                          mat4_translate(vec3_make(0.0f, FLOOR_EPS, 0.0f)));
            draw_mesh(state, fq, fm, view, proj, eye, 0.0f, g_floor_mat);
        }
    }
```

(`vis`, `view`, `proj`, `eye`, `state` are all in scope here — the loop just above uses them. `floor_quad_for` here is a pure cache hit because `floor_quads_ensure` pre-built them.)

- [ ] **Step 6: Gitignore the texture folder**

In `.gitignore`, under the existing "Local test assets" comment, add:

```
# Sourced PBR material (PolyHaven, CC0) — kept local per synthesized-never-sourced (the floor-overlay experiment)
/red_sandstone_tiles/
```

- [ ] **Step 7: Build gauntlet (both backends)**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: c89check PASS; debug + metal build clean. **No shader changed** → the Metal risk is compile-only. If a build fails, fix minimally; if `RHI_TEX_RGBA8` / a field name doesn't match, grep the real name and adjust.

- [ ] **Step 8: Confirm the folder won't be committed**

Run: `git status --short red_sandstone_tiles/`
Expected: **no output** (the gitignore line now hides it). If it still shows `?? red_sandstone_tiles/`, the gitignore entry is wrong — fix it.

- [ ] **Step 9: Commit**

```bash
git add main.c .gitignore
git commit -m "$(cat <<'EOF'
Sandstone floor overlay (sourced-texture experiment)

A flagged, reversible departure from synthesized-never-sourced: tile the
PolyHaven red_sandstone_tiles PBR set over room floors at constant tile size, as
a render-time overlay (linear loader + g_floor_mat + a (w,d)-keyed quad cache +
a per-room draw in the opaque pass). No scene change, no shader/MSL twin. The
CC0 texture folder is gitignored.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Live-verify + finish

- [ ] **Step 1: Human live-verify (Fran)** — on `./solarium` and `./solarium-metal`: open a room; confirm the floor shows tiled red sandstone (albedo + normal relief + roughness), lit and shadowed, with a consistent tile size between differently-sized rooms; no z-fighting flicker. Tune `FLOOR_TILE_M` (tile density) / `FLOOR_EPS` if needed.
- [ ] **Step 2: Finish** — use superpowers:finishing-a-development-branch; ff-merge `sandstone-floor-overlay` to `main` (or per Fran's call). Do NOT stage `NOTES.stml`/`paper-picture.png`.

---

## Plan self-review

**Spec coverage:** linear loader (Step 1) ✓; `g_floor_mat` w/ ARM→mr+ao + disabled-flag (Step 2-3) ✓; constant-tile-size `(w,d)` cache w/ meter UVs + overflow guard (Step 2) ✓; pre-build-before-pass safety (Step 4) ✓; per-room opaque draw, workspace+frustum filtered, room-scoped, +EPS (Step 5) ✓; gitignore the CC0 folder (Step 6) ✓; no shader/MSL, render-only (whole design) ✓; gauntlet + live-verify (Steps 7, Task 2) ✓.

**Placeholder scan:** none — full code + exact anchors throughout.

**Type consistency:** `g_floor_mat` / `g_floor_cache` / `floor_mat_init` / `floor_quad_for` / `floor_quads_ensure` and the constants `FLOOR_TILE_M`/`FLOOR_EPS`/`FLOOR_CACHE_MAX` are used identically across Steps 2/4/5. `load_texture_linear` signature matches its call sites. The Mesh field `index_count` and the `Material` slots (`albedo_tex`/`normal_tex`/`mr_tex`/`ao_tex`/`base_color`/`metallic`/`roughness`/`normal_scale`/`ao_strength`) match the struct in material.h.
