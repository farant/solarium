# World-Text SDF Outline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to execute this plan task-by-task. Fresh implementer per task, then spec + code-quality review, then human live-verify. Steps use checkbox (`- [ ]`).

**Goal:** Add a reusable SDF outline to the world-text renderer (`wtext_block_outlined` + two new fragment uniforms, GLSL + MSL twin) and use it for map-pin labels, replacing the 4-copy halo hack with one crisp draw — while leaving all existing world text byte-identical.

**Architecture:** The wtext SDF fragment shader gains `uOutlineColor` (vec3) + `uOutline` (float) uniforms and a second distance band; when `uOutline==0` and `uOutlineColor==uColor` it degrades to the exact current output, so `wtext_block`/`_bent` (which pass those defaults) are unchanged. A new `wtext_block_outlined` passes a real outline. Uniforms are set by name; the Metal RHI reflects the struct, so no hand-packed constant buffer.

**Tech Stack:** C89 (`wtext.c/.h`, `main.c`); GLSL + runtime-compiled MSL twin in `wtext.c`; RHI seam (`rhi_set_uniform_*`). Build via `build.sh` (GL + `metal`).

---

## Context the implementer needs

Reference the spec: `docs/superpowers/specs/2026-07-01-wtext-outline-design.md`.

- **C89** for `.c`/`.h`: declarations at the TOP of every block, no `//`, no C99/C11.
- **Dual-backend shader law:** `wtext.c` holds BOTH a GLSL fragment shader and a `#ifdef SOL_RHI_METAL` MSL twin. Both must gain the two uniforms and identical math, or the Metal build diverges. `./build.sh metal` is load-bearing (the MSL must compile and its reflection must expose `uOutlineColor`/`uOutline`).
- **The safety invariant:** existing callers must be visually unchanged. This is guaranteed by the shader math: at `uOutline==0`, `oe==0.5` so `cov==fill`; and `wtext_block`/`_bent` pass `uOutlineColor==uColor`, so `mix(uOutlineColor,uColor,fill)==uColor`. Do not add a branch — the math already degrades.
- **Do NOT run the engine binary.** Build only.
- **Commit discipline:** `git add` only the named files; NEVER `git add -A`/`.`; NEVER stage `NOTES.stml`/`paper-picture.png`; commit body ends EXACTLY with `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
- **Gauntlet per task:** `./build.sh`, `./build.sh c89check`, `./build.sh asan`, `./build.sh metal`.
- The uniform param name `or_` (trailing underscore) is deliberate — `or` is an `<iso646.h>` alternative-token macro; avoid it.

## File Structure

```
wtext.c   (Task 1) GLSL + MSL fragment shader (2 uniforms + outline math);
                   wt_draw sets the 2 uniforms; wt_block_flat internal;
                   wtext_block / wtext_block_outlined wrappers; bent draw call
wtext.h   (Task 1) wtext_block_outlined declaration
main.c    (Task 2) pin-label pass -> one wtext_block_outlined, halo removed
```

---

## Task 1 — the outline capability in the wtext renderer

**Files:** `wtext.c`, `wtext.h`.

### Step 1.1 — MSL fragment shader (the twin)

Find:
```c
static const char *WT_FRAGMENT_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "struct FU { float3 uColor; };\n"
    "fragment float4 fmain(VOut v [[stage_in]], constant FU &u [[buffer(0)]],\n"
    "                      texture2d<float> uTex [[texture(0)]],\n"
    "                      sampler s0 [[sampler(0)]]) {\n"
    "    float d = uTex.sample(s0, v.uv).r;\n"
    "    float w = fwidth(d) * 0.5 + 0.0001;\n"
    "    float edge = smoothstep(0.5 - w, 0.5 + w, d);\n"
    "    if (edge < 0.004) discard_fragment();\n"
    "    return float4(u.uColor, edge);\n"
    "}\n";
```
Replace with:
```c
static const char *WT_FRAGMENT_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "struct FU { float3 uColor; float3 uOutlineColor; float uOutline; };\n"
    "fragment float4 fmain(VOut v [[stage_in]], constant FU &u [[buffer(0)]],\n"
    "                      texture2d<float> uTex [[texture(0)]],\n"
    "                      sampler s0 [[sampler(0)]]) {\n"
    "    float d = uTex.sample(s0, v.uv).r;\n"
    "    float w = fwidth(d) * 0.5 + 0.0001;\n"
    "    float fill = smoothstep(0.5 - w, 0.5 + w, d);\n"
    "    float oe = 0.5 - u.uOutline;\n"
    "    float cov = smoothstep(oe - w, oe + w, d);\n"
    "    if (cov < 0.004) discard_fragment();\n"
    "    return float4(mix(u.uOutlineColor, u.uColor, fill), cov);\n"
    "}\n";
```

### Step 1.2 — GLSL fragment shader

Find:
```c
static const char *WT_FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec3 uColor;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "    float d = texture(uTex, vUV).r;\n"
    "    float w = fwidth(d) * 0.5 + 0.0001;\n"
    "    float edge = smoothstep(0.5 - w, 0.5 + w, d);\n"
    "    if (edge < 0.004) discard;\n"
    "    FragColor = vec4(uColor, edge);\n"
    "}\n";
```
Replace with:
```c
static const char *WT_FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec3 uColor;\n"
    "uniform vec3 uOutlineColor;\n"
    "uniform float uOutline;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "    float d = texture(uTex, vUV).r;\n"
    "    float w = fwidth(d) * 0.5 + 0.0001;\n"
    "    float fill = smoothstep(0.5 - w, 0.5 + w, d);\n"
    "    float oe = 0.5 - uOutline;\n"
    "    float cov = smoothstep(oe - w, oe + w, d);\n"
    "    if (cov < 0.004) discard;\n"
    "    FragColor = vec4(mix(uOutlineColor, uColor, fill), cov);\n"
    "}\n";
```

### Step 1.3 — `wt_draw` sets the two new uniforms

Find:
```c
static void wt_draw(RhiBuffer buffer, int vc, mat4 viewproj, mat4 model,
                    const Font *f, float r, float g, float b) {
    mat4 mvp = mat4_mul(viewproj, model);
    rhi_set_pipeline(g_wt.pipeline);
    rhi_bind_vertex_buffer(buffer);
    rhi_bind_texture(font_atlas(f), 0);
    rhi_set_uniform_int("uTex", 0);
    rhi_set_uniform_mat4("uMVP", mvp.m);
    rhi_set_uniform_vec3("uColor", r, g, b);
    rhi_draw(0, vc);
}
```
Replace with:
```c
static void wt_draw(RhiBuffer buffer, int vc, mat4 viewproj, mat4 model,
                    const Font *f, float r, float g, float b,
                    float or_, float og, float ob, float ow) {
    mat4 mvp = mat4_mul(viewproj, model);
    rhi_set_pipeline(g_wt.pipeline);
    rhi_bind_vertex_buffer(buffer);
    rhi_bind_texture(font_atlas(f), 0);
    rhi_set_uniform_int("uTex", 0);
    rhi_set_uniform_mat4("uMVP", mvp.m);
    rhi_set_uniform_vec3("uColor", r, g, b);
    rhi_set_uniform_vec3("uOutlineColor", or_, og, ob);
    rhi_set_uniform_float("uOutline", ow);
    rhi_draw(0, vc);
}
```

### Step 1.4 — factor `wtext_block` into `wt_block_flat` + two wrappers

Find the whole current `wtext_block` function:
```c
void wtext_block(const Font *f, mat4 viewproj, mat4 model, const char *utf8,
                 float x, float top_y, float px_to_m, float wrap_w_m,
                 float r, float g, float b) {
    char        wrapped[WT_WRAP_CAP];
    const char *src = utf8;
    int         len, slot, vc;
    RhiBuffer   buf, evicted;

    if (!g_wt.ready || !f || !utf8 || px_to_m <= 0.0f) return;
    len = (int)strlen(utf8);

    if (len < WTCACHE_TEXT) {                       /* cacheable: the common case */
        slot = wtcache_find(&g_wt_cache, (const void *)f, utf8, len,
                            px_to_m, wrap_w_m, x, top_y, g_wt_frame);
        if (slot >= 0) {                            /* HIT — no shape, no upload */
            g_wt_blocks++;
            wt_draw(g_wt_cache.e[slot].buffer, g_wt_cache.e[slot].vc,
                    viewproj, model, f, r, g, b);
            return;
        }
        if (wrap_w_m > 0.0f) {                      /* MISS — build once, store */
            if (text_wrap(f, utf8, px_to_m, wrap_w_m, wrapped, WT_WRAP_CAP) > 0)
                src = wrapped;
        }
        vc = wt_build(f, src, x, top_y, px_to_m, (WtextBend)0, (void *)0, 0.0f);
        if (vc == 0) return;                        /* whitespace-only: nothing to cache */
        slot = wtcache_claim(&g_wt_cache, (const void *)f, utf8, len,
                             px_to_m, wrap_w_m, x, top_y, g_wt_frame, &evicted);
        if (evicted.id) rhi_destroy_buffer(evicted);
        buf = rhi_create_buffer(RHI_BUFFER_VERTEX, g_wt_verts,
                                (size_t)vc * WT_VERT_FLOATS * sizeof(sol_f32));
        wtcache_set(&g_wt_cache, slot, buf, vc);
        g_wt_blocks++; g_wt_uploads++; g_wt_misses++;
        wt_draw(buf, vc, viewproj, model, f, r, g, b);
        return;
    }

    /* uncacheable (huge string, e.g. a big reader page): the immediate path on
       the shared scratch buffer — one block, no scaling concern */
    if (wrap_w_m > 0.0f) {
        if (text_wrap(f, utf8, px_to_m, wrap_w_m, wrapped, WT_WRAP_CAP) > 0)
            src = wrapped;
    }
    vc = wt_build(f, src, x, top_y, px_to_m, (WtextBend)0, (void *)0, 0.0f);
    if (vc == 0) return;
    rhi_update_buffer(g_wt.vbuffer, g_wt_verts,
                      (size_t)vc * WT_VERT_FLOATS * sizeof(sol_f32));
    g_wt_blocks++; g_wt_uploads++;
    wt_draw(g_wt.vbuffer, vc, viewproj, model, f, r, g, b);
}
```
Replace it with the internal `wt_block_flat` (same body, 4 extra params threaded through each `wt_draw`) plus two thin public wrappers:
```c
/* The flat (cacheable) text path, with an optional SDF outline. wtext_block
   passes the fill colour as the outline colour and ow=0, which the shader
   collapses to the exact no-outline output. wtext_block_outlined passes a real
   outline. The glyph cache keys on geometry (font/text/size/pos), not colour,
   so an outlined and a plain block of the same text share cached vertices. */
static void wt_block_flat(const Font *f, mat4 viewproj, mat4 model, const char *utf8,
                          float x, float top_y, float px_to_m, float wrap_w_m,
                          float r, float g, float b,
                          float or_, float og, float ob, float ow) {
    char        wrapped[WT_WRAP_CAP];
    const char *src = utf8;
    int         len, slot, vc;
    RhiBuffer   buf, evicted;

    if (!g_wt.ready || !f || !utf8 || px_to_m <= 0.0f) return;
    len = (int)strlen(utf8);

    if (len < WTCACHE_TEXT) {                       /* cacheable: the common case */
        slot = wtcache_find(&g_wt_cache, (const void *)f, utf8, len,
                            px_to_m, wrap_w_m, x, top_y, g_wt_frame);
        if (slot >= 0) {                            /* HIT — no shape, no upload */
            g_wt_blocks++;
            wt_draw(g_wt_cache.e[slot].buffer, g_wt_cache.e[slot].vc,
                    viewproj, model, f, r, g, b, or_, og, ob, ow);
            return;
        }
        if (wrap_w_m > 0.0f) {                      /* MISS — build once, store */
            if (text_wrap(f, utf8, px_to_m, wrap_w_m, wrapped, WT_WRAP_CAP) > 0)
                src = wrapped;
        }
        vc = wt_build(f, src, x, top_y, px_to_m, (WtextBend)0, (void *)0, 0.0f);
        if (vc == 0) return;                        /* whitespace-only: nothing to cache */
        slot = wtcache_claim(&g_wt_cache, (const void *)f, utf8, len,
                             px_to_m, wrap_w_m, x, top_y, g_wt_frame, &evicted);
        if (evicted.id) rhi_destroy_buffer(evicted);
        buf = rhi_create_buffer(RHI_BUFFER_VERTEX, g_wt_verts,
                                (size_t)vc * WT_VERT_FLOATS * sizeof(sol_f32));
        wtcache_set(&g_wt_cache, slot, buf, vc);
        g_wt_blocks++; g_wt_uploads++; g_wt_misses++;
        wt_draw(buf, vc, viewproj, model, f, r, g, b, or_, og, ob, ow);
        return;
    }

    /* uncacheable (huge string, e.g. a big reader page): the immediate path on
       the shared scratch buffer — one block, no scaling concern */
    if (wrap_w_m > 0.0f) {
        if (text_wrap(f, utf8, px_to_m, wrap_w_m, wrapped, WT_WRAP_CAP) > 0)
            src = wrapped;
    }
    vc = wt_build(f, src, x, top_y, px_to_m, (WtextBend)0, (void *)0, 0.0f);
    if (vc == 0) return;
    rhi_update_buffer(g_wt.vbuffer, g_wt_verts,
                      (size_t)vc * WT_VERT_FLOATS * sizeof(sol_f32));
    g_wt_blocks++; g_wt_uploads++;
    wt_draw(g_wt.vbuffer, vc, viewproj, model, f, r, g, b, or_, og, ob, ow);
}

void wtext_block(const Font *f, mat4 viewproj, mat4 model, const char *utf8,
                 float x, float top_y, float px_to_m, float wrap_w_m,
                 float r, float g, float b) {
    wt_block_flat(f, viewproj, model, utf8, x, top_y, px_to_m, wrap_w_m,
                  r, g, b, r, g, b, 0.0f);          /* outline off: identical output */
}

void wtext_block_outlined(const Font *f, mat4 viewproj, mat4 model, const char *utf8,
                          float x, float top_y, float px_to_m, float wrap_w_m,
                          float r, float g, float b,
                          float or_, float og, float ob, float ow) {
    wt_block_flat(f, viewproj, model, utf8, x, top_y, px_to_m, wrap_w_m,
                  r, g, b, or_, og, ob, ow);
}
```

### Step 1.5 — update `wtext_block_bent`'s draw call

Find (the last line of `wtext_block_bent`):
```c
    g_wt_blocks++; g_wt_uploads++;
    wt_draw(g_wt.vbuffer, vc, viewproj, model, f, r, g, b);
}
```
Replace with (outline off — bent text keeps its exact behavior):
```c
    g_wt_blocks++; g_wt_uploads++;
    wt_draw(g_wt.vbuffer, vc, viewproj, model, f, r, g, b, r, g, b, 0.0f);
}
```

### Step 1.6 — declare `wtext_block_outlined` in `wtext.h`

Find:
```c
void wtext_block(const Font *f, mat4 viewproj, mat4 model, const char *utf8,
                 float x, float top_y, float px_to_m, float wrap_w_m,
                 float r, float g, float b);
```
Replace with:
```c
void wtext_block(const Font *f, mat4 viewproj, mat4 model, const char *utf8,
                 float x, float top_y, float px_to_m, float wrap_w_m,
                 float r, float g, float b);

/* Like wtext_block, but with an SDF OUTLINE: `ow` is the outline half-width in
   SDF distance units (0..~0.3; 0 = none), drawn in (or_,og,ob) around the
   (r,g,b) fill. One draw — reads on any background. */
void wtext_block_outlined(const Font *f, mat4 viewproj, mat4 model, const char *utf8,
                          float x, float top_y, float px_to_m, float wrap_w_m,
                          float r, float g, float b,
                          float or_, float og, float ob, float ow);
```

### Step 1.7 — gauntlet build

- [ ] `./build.sh` — expect `built ./solarium (debug)`.
- [ ] `./build.sh c89check` — expect `c89check: PASS — all sources are C89-pedantic clean`.
- [ ] `./build.sh asan` — links (pre-existing `sprintf` deprecation warning is OK).
- [ ] `./build.sh metal` — **must** build clean (the MSL twin compiles; its reflection exposes `uOutlineColor`/`uOutline`). If it fails, the MSL FS or `FU` struct is wrong — fix per step 1.1.

### Step 1.8 — commit

- [ ] `git add wtext.c wtext.h`
- [ ] Commit:
```
World-text SDF outline: wtext_block_outlined + shader uniforms

The wtext SDF fragment shader (GLSL + MSL twin) gains uOutlineColor/uOutline
and a second distance band; wt_draw sets them. wt_block_flat is the shared
path; wtext_block/_bent pass outline-off (uOutlineColor==uColor, ow=0), which
the shader collapses to byte-identical current output. New wtext_block_outlined
passes a real outline. No existing caller changes.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
```

---

## Task 2 — use the outline for map-pin labels

**Files:** `main.c`.

### Step 2.1 — replace the pin label's 4-copy halo with one outlined draw

In the pin-label pass, find:
```c
            const char  *nm;
            float        px2m, nw, top_y, mw2, ho, bx;
            mat4         face, face2;
            SceneObject *pm;
            int          d;
            static const float hx[4] = { -1.0f, 1.0f,  0.0f, 0.0f };
            static const float hy[4] = {  0.0f, 0.0f, -1.0f, 1.0f };
            if (!o->mesh_ref || strcmp(o->mesh_ref, "pin") != 0) continue;
            if (o->mesh.index_count == 0) continue;         /* out-of-window: hidden */
            if (vis && !vis[o->handle]) continue;           /* frustum cull (as cards) */
            nm = scene_meta_get(&state->scene, o->handle, "name");
            if (!nm || !nm[0]) continue;                    /* unnamed: no label */
            pm  = scene_get(&state->scene, o->parent);      /* the map: sizes the label */
            mw2 = pm ? mesh_ref_param("map", pm->mesh_params,
                                      pm->mesh_param_count, "w") : MAP_BOARD_W;
            if (mw2 <= 0.0f) mw2 = MAP_BOARD_W;
            px2m  = PIN_LABEL_FRAC * mw2 / lh;              /* line height = frac of map width */
            text_measure_cached(uf, nm, 1.0f, &nw, (float *)0);
            top_y = (PIN_HEAD_CY + PIN_HEAD_R + 0.02f) * mw2 + lh * px2m;  /* clear the head */
            bx    = -nw * px2m * 0.5f;                      /* centre the label */
            ho    = 0.09f * lh * px2m;                      /* dark-halo offset ~ line height */
            face  = mat4_mul(scene_world_matrix(&state->scene, o),
                             mat4_translate(vec3_make(0.0f, 0.0f, 0.002f)));   /* halo plane */
            face2 = mat4_mul(scene_world_matrix(&state->scene, o),
                             mat4_translate(vec3_make(0.0f, 0.0f, 0.004f)));   /* text, nearer */
            for (d = 0; d < 4; d++)                         /* dark halo: reads on any basemap */
                wtext_block(uf, vp, face, nm, bx + hx[d] * ho, top_y + hy[d] * ho,
                            px2m, 0.0f, 0.04f, 0.04f, 0.05f);
            wtext_block(uf, vp, face2, nm, bx, top_y, px2m, 0.0f,
                        0.99f, 0.98f, 0.94f);               /* light text, in front */
```
Replace with:
```c
            const char  *nm;
            float        px2m, nw, top_y, mw2, bx;
            mat4         face;
            SceneObject *pm;
            if (!o->mesh_ref || strcmp(o->mesh_ref, "pin") != 0) continue;
            if (o->mesh.index_count == 0) continue;         /* out-of-window: hidden */
            if (vis && !vis[o->handle]) continue;           /* frustum cull (as cards) */
            nm = scene_meta_get(&state->scene, o->handle, "name");
            if (!nm || !nm[0]) continue;                    /* unnamed: no label */
            pm  = scene_get(&state->scene, o->parent);      /* the map: sizes the label */
            mw2 = pm ? mesh_ref_param("map", pm->mesh_params,
                                      pm->mesh_param_count, "w") : MAP_BOARD_W;
            if (mw2 <= 0.0f) mw2 = MAP_BOARD_W;
            px2m  = PIN_LABEL_FRAC * mw2 / lh;              /* line height = frac of map width */
            text_measure_cached(uf, nm, 1.0f, &nw, (float *)0);
            top_y = (PIN_HEAD_CY + PIN_HEAD_R + 0.02f) * mw2 + lh * px2m;  /* clear the head */
            bx    = -nw * px2m * 0.5f;                      /* centre the label */
            face  = mat4_mul(scene_world_matrix(&state->scene, o),
                             mat4_translate(vec3_make(0.0f, 0.0f, 0.002f)));
            wtext_block_outlined(uf, vp, face, nm, bx, top_y, px2m, 0.0f,
                                 0.99f, 0.98f, 0.94f,       /* light fill */
                                 0.04f, 0.04f, 0.05f, 0.20f);  /* dark SDF outline */
```

### Step 2.2 — gauntlet build

- [ ] `./build.sh`
- [ ] `./build.sh c89check`
- [ ] `./build.sh asan`
- [ ] `./build.sh metal`

### Step 2.3 — commit

- [ ] `git add main.c`
- [ ] Commit:
```
Map pins: pin labels use the SDF outline (halo hack removed)

Replace the 4-copy depth-layered halo with one wtext_block_outlined draw
(light fill + near-black SDF outline). Crisper and 1 draw instead of 5;
the proportional-size logic is unchanged.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
```

---

## After both tasks: human live-verify

- [ ] **The invariant (most important):** all EXISTING world text is visually unchanged on **both** GL and Metal — whiteboard card labels, note bodies, doorway/route labels, and open-book pages. (This proves the `uOutline==0` degrade path.)
- [ ] **Pin labels** now show a clean, crisp outline (no more multi-copy fuzz), readable over bright and dark basemaps, on **both** `./solarium` and `./solarium-metal`.
- [ ] The pin outline dials are easy to tune: `ow` (0.20 — outline thickness) and the fill/outline colors, in the `wtext_block_outlined` call.

## Self-review notes (spec coverage)

- Shader outline (2 uniforms, both twins, degrade-to-identical) → Task 1 steps 1.1–1.2.
- `wt_draw` sets the uniforms → step 1.3. Shared `wt_block_flat` + `wtext_block`/`_outlined` wrappers → step 1.4. `wtext_block_bent` unchanged behavior → step 1.5. Header decl → step 1.6.
- Pin swap (one outlined draw, halo removed) → Task 2 step 2.1.
- Dual-backend / metal-reflection verification → each task's gauntlet (`./build.sh metal`).
- Out-of-scope (retrofitting other labels, bent-outlined, drop-shadow) intentionally not implemented.
