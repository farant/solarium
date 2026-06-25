# Portal Material Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the portal a dark_wood frame and an animated, glowing "energy membrane" pane (a procedural shader) instead of the flat blue look.

**Architecture:** Split the `gate` mesh into frame-only (drawn with dark_wood) + a separate shared unit-quad pane. The pane is drawn in a new dedicated render pass with a new procedural energy shader (GLSL + MSL twin, modeled on the water shader) — domain-warped noise swirl + edge glow, opaque, blue, `uTime`-animated — sized to each portal's opening. No new scene objects, no scene.stml change.

**Tech Stack:** C89 (strict: `-Wall -Wextra -Werror -pedantic`), OpenGL + Metal dual backend. **This feature ADDS A SHADER → it needs a GLSL + MSL twin** (the engine compiles MSL on-device at launch, so a twin mismatch passes the build and breaks at startup). Spec: `docs/superpowers/specs/2026-06-25-portal-material-design.md`.

**Conventions the implementer MUST follow:**
- In `main.c`, no C99 math (use `(float)sin((double)x)` etc.). Declarations at the top of each block.
- Commit on the current branch, message body ending with:
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`
- Never `git add` `NOTES.stml` or `paper-picture.png`. Stage only the files each task touches.
- After each task build BOTH backends: `./build.sh` (expect `built ./solarium (debug)`) and `./build.sh metal` (expect `built ./solarium-metal ...`).
- **CRITICAL dual-backend note:** `build.sh` only compiles C — it does NOT compile the GLSL/MSL shader strings (both compile on-device at launch). So a shader-source typo will NOT fail the build; it surfaces at launch (and degrades via the id-0 contract: a failed shader → id-0 pipeline → its draws are swallowed; the failure logs to stderr). Therefore: write the two shader twins to mirror each other EXACTLY (same logic, same uniform names), model them on the water shader (main.c ~862–954), and report DONE_WITH_CONCERNS noting that shader compilation is human-launch-verified. Do not try to "fix" a build that already passes by altering the shader.

---

## File Touch List

- `mesh.c` — `emit_gate`: drop the pane slab (frame-only). (Task 1)
- `main.c`:
  - `apply_kind_materials` `KIND_PORTAL` → wood-brown, no emissive; draw-loop branch `mesh_ref=="gate"` → `g_dark_wood`. (Task 1)
  - `PORTAL_VERTEX_SRC` / `PORTAL_FRAGMENT_SRC` (GLSL + MSL twin); `AppState` fields `gate_pane` + `portal_pipeline`; create the pipeline. (Task 2)
  - the portal-energy render pass. (Task 3)

---

### Task 1: Frame — dark_wood + frame-only gate mesh

**Files:**
- Modify: `mesh.c` (`emit_gate`, ~line 519)
- Modify: `main.c` (`apply_kind_materials` KIND_PORTAL ~line 11203; the draw-loop dark_wood branch ~line 13143)

- [ ] **Step 1: Make `emit_gate` build the frame only**

In `mesh.c`, find `emit_gate` (~line 519):

```c
static void emit_gate(MeshBuilder *b, const float *p) {
    float w = p[0], h = p[1], t = p[2], pw = p[3];
    float hw = w * 0.5f, hz = t * 0.5f;
    /* left + right posts */
    aabb_box(b, -hw - pw, -hw,     0.0f, h,      -hz, hz);
    aabb_box(b,  hw,       hw + pw, 0.0f, h,      -hz, hz);
    /* lintel across the top */
    aabb_box(b, -hw - pw,  hw + pw, h - pw, h,    -hz, hz);
    /* the shimmer pane: thin slab filling the opening */
    aabb_box(b, -hw, hw, 0.0f, h - pw, -0.02f, 0.02f);
}
```

Delete the last `aabb_box` (the shimmer pane) and its comment, so the gate is frame-only:

```c
static void emit_gate(MeshBuilder *b, const float *p) {
    float w = p[0], h = p[1], t = p[2], pw = p[3];
    float hw = w * 0.5f, hz = t * 0.5f;
    /* left + right posts */
    aabb_box(b, -hw - pw, -hw,     0.0f, h,      -hz, hz);
    aabb_box(b,  hw,       hw + pw, 0.0f, h,      -hz, hz);
    /* lintel across the top (the opening's energy pane is drawn separately) */
    aabb_box(b, -hw - pw,  hw + pw, h - pw, h,    -hz, hz);
}
```

- [ ] **Step 2: KIND_PORTAL → wood-brown, no glow**

In `main.c` `apply_kind_materials` (~line 11203), find:

```c
            case KIND_PORTAL:    m.base_color = vec3_make(0.20f, 0.32f, 0.55f); m.roughness = 0.30f;
                                 m.emissive   = vec3_make(0.25f, 0.55f, 0.95f); break;
```

Replace with a neutral wood-brown frame (no emissive — the frame must not glow; the pane glows instead):

```c
            case KIND_PORTAL:    m.base_color = vec3_make(0.32f, 0.22f, 0.13f); m.roughness = 0.60f; break;
```

- [ ] **Step 3: Draw the frame with dark_wood when available**

In `main.c`, find the dark_wood bookshelf branch in the draw loop (~line 13143):

```c
            if (g_dark_wood.albedo_tex.id != 0 && o->mesh_ref &&
                strcmp(o->mesh_ref, "bookshelf") == 0)
                dm = g_dark_wood;                 /* dark-wood bookshelves */
```

Immediately AFTER that branch, add a parallel branch for the gate frame:

```c
            if (g_dark_wood.albedo_tex.id != 0 && o->mesh_ref &&
                strcmp(o->mesh_ref, "gate") == 0)
                dm = g_dark_wood;                 /* portal frame */
```

- [ ] **Step 4: Build both backends**

Run: `./build.sh && ./build.sh metal`
Expected: both build clean. (Visible result if launched: the portal frame is dark wood / wood-brown, and the opening is now empty — the energy pane arrives in Tasks 2–3. That empty-opening intermediate is EXPECTED.)

- [ ] **Step 5: Commit**

```bash
git add mesh.c main.c
git commit -m "Portal material 1/3: dark_wood frame + frame-only gate mesh

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: The energy shader (GLSL + MSL twin) + pipeline + AppState fields

**Files:**
- Modify: `main.c` — add the shader pair after the decal shader pair (`#endif /* SOL_RHI_METAL — the decal pair */`, ~line 1276); `AppState` fields (`gate_pane` near `resize_handle_mesh` ~line 2704; `portal_pipeline` near `decal_pipeline` ~line 2500); the pipeline creation (beside `decal_pipeline`, ~line 11553).

Context: the engine selects GLSL or MSL by `#ifdef SOL_RHI_METAL`. Metal uniforms are reflection-mapped by NAME, so the field names in the MSL `VU`/`FU` structs must match the GLSL `uniform` names exactly. Model on the water shader (main.c ~862–954): vertex uniforms `uModel/uView/uProj` in a `VU` struct at `buffer(2)`; fragment uniforms in an `FU` struct at `buffer(0)`; the Metal VS does the `o.pos.z = (o.pos.z + o.pos.w)*0.5;` depth remap.

- [ ] **Step 1: Add the shader twin**

In `main.c`, find the end of the decal shader pair: `#endif /* SOL_RHI_METAL — the decal pair */` (~line 1276). Immediately AFTER that line, insert the portal energy shader pair:

```c
/* the portal energy pane (Portal Material): an UNLIT, opaque procedural membrane
   — domain-warped value-noise swirl animated by uTime, dark->bright by the swirl,
   with a bright glow toward the frame edge. Modeled on the water twin. */
#ifdef SOL_RHI_METAL
static const char *PORTAL_VERTEX_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VIn { float3 pos [[attribute(0)]]; float3 normal [[attribute(1)]];\n"
    "             float2 uv [[attribute(2)]]; float4 tangent [[attribute(3)]]; };\n"
    "struct VU { float4x4 uModel; float4x4 uView; float4x4 uProj; };\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "vertex VOut vmain(VIn v [[stage_in]], constant VU &u [[buffer(2)]]) {\n"
    "    VOut o;\n"
    "    float4 wp = u.uModel * float4(v.pos, 1.0);\n"
    "    o.uv = v.uv;\n"
    "    o.pos = u.uProj * (u.uView * wp);\n"
    "    o.pos.z = (o.pos.z + o.pos.w) * 0.5;\n"
    "    return o;\n"
    "}\n";
static const char *PORTAL_FRAGMENT_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct FU { float3 uPortalColor; float uTime; };\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "static float phash(float2 p){ p = fract(p*float2(123.34,456.21));\n"
    "    p += dot(p, p+45.32); return fract(p.x*p.y); }\n"
    "static float pnoise(float2 p){\n"
    "    float2 i = floor(p), f = fract(p);\n"
    "    float a = phash(i), b = phash(i+float2(1.0,0.0));\n"
    "    float c = phash(i+float2(0.0,1.0)), d = phash(i+float2(1.0,1.0));\n"
    "    float2 u = f*f*(3.0-2.0*f);\n"
    "    return mix(mix(a,b,u.x), mix(c,d,u.x), u.y); }\n"
    "static float pfbm(float2 p){ return 0.6*pnoise(p) + 0.4*pnoise(p*2.03 + 7.1); }\n"
    "fragment float4 fmain(VOut v [[stage_in]], constant FU &u [[buffer(0)]]) {\n"
    "    float2 p = v.uv * 3.0;\n"
    "    float t = u.uTime * 0.3;\n"
    "    float2 warp = float2(pnoise(p + t), pnoise(p - t*0.8 + 5.2));\n"
    "    float n = pfbm(p + warp*1.5 + t*0.5);\n"
    "    float2 c = v.uv - 0.5;\n"
    "    float edge = smoothstep(0.20, 0.50, max(abs(c.x), abs(c.y)));\n"
    "    float3 col = u.uPortalColor * mix(0.25, 1.0, n);\n"
    "    col += float3(0.30, 0.50, 0.95) * edge * 0.8;\n"
    "    return float4(col, 1.0);\n"
    "}\n";
#else /* GLSL */
static const char *PORTAL_VERTEX_SRC =
    "#version 330 core\n"
    "layout (location = 0) in vec3 aPos;\n"
    "layout (location = 1) in vec3 aNormal;\n"
    "layout (location = 2) in vec2 aUV;\n"
    "layout (location = 3) in vec4 aTangent;\n"
    "uniform mat4 uModel; uniform mat4 uView; uniform mat4 uProj;\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "    vUV = aUV;\n"
    "    gl_Position = uProj * uView * uModel * vec4(aPos, 1.0);\n"
    "}\n";
static const char *PORTAL_FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "uniform vec3 uPortalColor;\n"
    "uniform float uTime;\n"
    "out vec4 FragColor;\n"
    "float phash(vec2 p){ p = fract(p*vec2(123.34,456.21));\n"
    "    p += dot(p, p+45.32); return fract(p.x*p.y); }\n"
    "float pnoise(vec2 p){\n"
    "    vec2 i = floor(p), f = fract(p);\n"
    "    float a = phash(i), b = phash(i+vec2(1.0,0.0));\n"
    "    float c = phash(i+vec2(0.0,1.0)), d = phash(i+vec2(1.0,1.0));\n"
    "    vec2 u = f*f*(3.0-2.0*f);\n"
    "    return mix(mix(a,b,u.x), mix(c,d,u.x), u.y); }\n"
    "float pfbm(vec2 p){ return 0.6*pnoise(p) + 0.4*pnoise(p*2.03 + 7.1); }\n"
    "void main() {\n"
    "    vec2 p = vUV * 3.0;\n"
    "    float t = uTime * 0.3;\n"
    "    vec2 warp = vec2(pnoise(p + t), pnoise(p - t*0.8 + 5.2));\n"
    "    float n = pfbm(p + warp*1.5 + t*0.5);\n"
    "    vec2 c = vUV - 0.5;\n"
    "    float edge = smoothstep(0.20, 0.50, max(abs(c.x), abs(c.y)));\n"
    "    vec3 col = uPortalColor * mix(0.25, 1.0, n);\n"
    "    col += vec3(0.30, 0.50, 0.95) * edge * 0.8;\n"
    "    FragColor = vec4(col, 1.0);\n"
    "}\n";
#endif /* SOL_RHI_METAL — the portal energy pane */
```

- [ ] **Step 2: Add the AppState fields**

In `main.c` `AppState`, find `RhiPipeline decal_pipeline; /* P9 item 3: church_decals — unlit alpha quads */` (~line 2500). Immediately AFTER it add:

```c
    RhiPipeline portal_pipeline;/* Portal Material: the energy-pane shader */
```

Then find `Mesh        resize_handle_mesh;/* small corner quad; built once on first use */` (~line 2704). Immediately AFTER it add:

```c
    Mesh        gate_pane;        /* portal energy pane: a unit quad, built once on first use */
```

- [ ] **Step 3: Create the pipeline**

In `main.c`, find the decal pipeline creation block (~line 11546):

```c
    {   /* P9 item 3: the decal pipeline — same vertex layout, the unlit decal
           shader, alpha-blend + depth-write-off (weathering quads). */
        RhiPipelineDesc dd = desc;
        dd.shader          = rhi_create_shader(DECAL_VERTEX_SRC, DECAL_FRAGMENT_SRC);
        dd.blend           = RHI_BLEND_ALPHA;
        dd.depth_write_off = SOL_TRUE;
        if (dd.shader.id) state->decal_pipeline = rhi_create_pipeline(&dd);
    }
```

Immediately AFTER that block, add the portal pipeline (same vertex layout, the energy shader, OPAQUE — `desc` defaults are depth-test on / blend off / depth-write on):

```c
    {   /* Portal Material: the energy-pane pipeline — same vertex layout, the
           procedural energy shader, opaque (depth-tested, writes depth). */
        RhiPipelineDesc pp = desc;
        pp.shader = rhi_create_shader(PORTAL_VERTEX_SRC, PORTAL_FRAGMENT_SRC);
        if (pp.shader.id) state->portal_pipeline = rhi_create_pipeline(&pp);
    }
```

- [ ] **Step 4: Build both backends**

Run: `./build.sh && ./build.sh metal`
Expected: both build clean. (The shader strings are NOT compiled by the build — they compile on-device at launch. Nothing is drawn with this pipeline yet; that's Task 3.)

- [ ] **Step 5: Commit**

```bash
git add main.c
git commit -m "Portal material 2/3: energy-pane shader twin + pipeline + fields

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

Report **DONE_WITH_CONCERNS** noting that the GLSL/MSL twins compile on-device at launch and are therefore human-launch-verified (the build cannot validate them).

---

### Task 3: The portal-energy render pass

**Files:**
- Modify: `main.c` — add the pass right after the decal render pass (the block that begins `if (state->decal_pipeline.id && state->decal_atlas.id)` and ends with its closing `}`, ~line 13923–13943).

Context: the decal pass (main.c ~13923) is the exact model — `rhi_set_pipeline` once, set view/proj uniforms, then per matching object set `uModel` + draw the mesh via `rhi_bind_vertex_buffer`/`rhi_bind_index_buffer`/`rhi_draw_indexed`. We draw the shared `gate_pane` (not the object's own mesh) scaled to each portal's opening. `make_page(b, w, h)` builds an upright XY quad centered at origin with 0..1 UVs (mesh.h: "upright XY quad, +Z, upright UVs"). `mesh_from_builder`, `mb_init`, `mb_free` are the build helpers (see the resize-handle lazy-build at ~13841). `mat4_scale(vec3)`, `mat4_translate(vec3)`, `mat4_mul` exist. `mesh_ref_param(ref, params, count, name)` reads a gate param. `scene_object_active` is the workspace filter (MANDATORY). `vis` is the frustum-cull array used by the decal pass.

- [ ] **Step 1: Add the render pass**

In `main.c`, find the END of the decal pass (the closing `}` of `if (state->decal_pipeline.id && state->decal_atlas.id) { ... }`, ~line 13943 — it's immediately before the `/* particles (P4 item 7): LAST in the HDR pass ... */` comment). Insert this block right AFTER the decal pass's closing `}` (and before the particles comment). The block is a self-contained, brace-balanced `if (...) { ... }`, so it compiles wherever it lands — just re-indent it to match the decal pass's indent level (the render-function-body level) for cleanliness; whitespace doesn't affect correctness. `view`, `proj`, `vis`, and `state` are all in scope here (the decal pass uses them).

```c
        /* Portal Material: the energy pane — one shared unit quad, drawn per
           active portal scaled to its opening, with the procedural swirl
           shader (uTime-animated). Opaque, in the HDR pass so it blooms. */
        if (state->portal_pipeline.id) {
            sol_u32 pi;
            if (state->gate_pane.index_count == 0) {          /* lazy-build the quad */
                MeshBuilder mb;
                mb_init(&mb);
                make_page(&mb, 1.0f, 1.0f);                   /* unit XY quad, 0..1 UV */
                state->gate_pane = mesh_from_builder(&mb);
                mb_free(&mb);
            }
            rhi_set_pipeline(state->portal_pipeline);
            rhi_set_uniform_mat4 ("uView", view.m);
            rhi_set_uniform_mat4 ("uProj", proj.m);
            rhi_set_uniform_float("uTime", (float)glfwGetTime());
            rhi_set_uniform_vec3 ("uPortalColor", 0.20f, 0.45f, 0.95f);
            for (pi = 0; pi < state->scene.count; pi++) {
                const SceneObject *o = &state->scene.objects[pi];
                float w, h, pw, oh;
                mat4  model;
                if (o->kind != KIND_PORTAL) continue;
                if (!scene_object_active(&state->scene, o->handle)) continue;  /* workspace filter */
                if (vis && !vis[o->handle]) continue;                          /* frustum cull */
                w  = mesh_ref_param("gate", o->mesh_params, o->mesh_param_count, "w");
                h  = mesh_ref_param("gate", o->mesh_params, o->mesh_param_count, "h");
                pw = mesh_ref_param("gate", o->mesh_params, o->mesh_param_count, "post");
                oh = h - pw;                                   /* opening height */
                model = mat4_mul(scene_world_matrix(&state->scene, o),
                          mat4_mul(mat4_translate(vec3_make(0.0f, oh * 0.5f, 0.0f)),
                                   mat4_scale(vec3_make(w, oh, 1.0f))));
                rhi_set_uniform_mat4("uModel", model.m);
                rhi_bind_vertex_buffer(state->gate_pane.vbuffer);
                rhi_bind_index_buffer (state->gate_pane.ibuffer);
                rhi_draw_indexed(0, state->gate_pane.index_count);
            }
        }
```

- [ ] **Step 2: Build both backends**

Run: `./build.sh && ./build.sh metal`
Expected: both build clean.

- [ ] **Step 3: Commit**

```bash
git add main.c
git commit -m "Portal material 3/3: the energy-pane render pass

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Final verification (after all tasks)

- [ ] **Full gauntlet:** `./build.sh && ./build.sh metal`
  Expected: both build clean.
- [ ] **Human live-verify (GUI — the implementer cannot do this; hand back to Fran):**
  1. Launch `./solarium` (GL) AND `./solarium-metal` (Metal) and walk up to a portal.
  2. Frame reads as **dark wood**; the opening shows an **animated blue energy swirl** that glows brighter toward the frame and moves over time.
  3. **Both backends look the same** (the MSL twin matches the GLSL — watch the launch stderr for any shader-compile error; a broken twin shows an empty opening + an error log).
  4. Walk through the portal (travel still works — unchanged).
  5. A portal in another workspace does not show a pane while you're elsewhere (the `scene_object_active` filter).

---

## Self-Review notes (plan author)

- **Spec coverage:** frame split + dark_wood + wood-brown (T1); shader twin + pipeline + fields (T2); the per-portal pass with workspace filter + HDR/bloom (T3). All spec sections covered.
- **Type consistency:** `gate_pane` (Mesh) and `portal_pipeline` (RhiPipeline) declared in T2, used in T3. Uniform names match across the GLSL/MSL twins and the pass: `uModel`, `uView`, `uProj` (vertex), `uTime`, `uPortalColor` (fragment). `make_page(1,1)` matches the model-matrix scale (unit ±0.5 quad → `scale(w,oh,1)` → `translate(0,oh/2,0)`).
- **Shader caveat surfaced:** the build does not validate shader strings; both twins compile on-device at launch → human-launch-verified (called out in T2 and the conventions). The id-0 contract means a bad shader degrades (empty opening), not a crash.
