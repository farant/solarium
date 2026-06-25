# Portal Material — Design Spec

**Date:** 2026-06-25
**Status:** Approved (brainstorm), pending implementation plan
**Author:** Fran + Claude

## Goal

Replace the portal's flat-blue look with: a **dark_wood frame** and an **animated, glowing "energy membrane"** in the opening — a procedural shader (swirling domain-warped noise + edge glow) instead of a flat emissive pane.

## Decisions (from brainstorming)

- **Frame → `dark_wood`** material (PBR lit), wood-brown fallback if the textures are absent.
- **Pane → an animated procedural energy shader** (a new GLSL + MSL twin).
- **Opaque** energy (no see-through, no alpha sorting, no scene-color sampling).
- **Keep the blue** palette (current portal cyan-blue), tunable later.
- The pane is a **derived visual** (like the stained-glass / decal passes) — **no new scene objects, no scene.stml change**; portal creation and travel (`portal_update`) untouched.

## Non-Goals

- No translucency / refraction (no scene-color texture sampling).
- No particles (the "membrane + particles" option was not chosen).
- No per-workspace color theming.
- Portals are not resizable (placed at the registry default), so the pane is sized from the gate's `w`/`h` params each frame — no per-portal mesh cache needed.

## Background (current state)

- `mesh.c` `emit_gate` builds ONE mesh: two posts + a lintel (the frame) **plus** a thin "shimmer pane" slab (`aabb_box(b, -hw, hw, 0.0f, h - pw, -0.02f, 0.02f)`). Registry row: `{ "gate", 4, {w,h,t,post}, {1.6,2.4,0.18,0.16}, emit_gate }`.
- `main.c` `apply_kind_materials` `KIND_PORTAL`: blue base `(0.20,0.32,0.55)` + blue emissive `(0.25,0.55,0.95)`, applied to the whole gate mesh.
- Portals are `KIND_PORTAL` scene objects with `mesh_ref == "gate"`, drawn in the main opaque object loop. `portal_update` triggers travel by proximity — independent of any pane geometry.

## Architecture

### 1. Split the gate: frame mesh + separate pane

- **`emit_gate` (mesh.c) → frame only.** Remove the shimmer-pane `aabb_box` line; keep the two posts + lintel.
- **The pane = a shared unit quad** built once and cached in `AppState` (the `resize_handle_mesh` lazy-build pattern at main.c ~13841): a quad in the **XY plane, spanning ±0.5 in X and ±0.5 in Y, centered at origin, 0..1 UVs**, normal ±Z (matching the gate's "opening faces local ±Z"). Produced via the mesh builder so it carries the standard 12-float vertex layout (pos3/normal3/uv2/tangent4) the pipelines expect. Field: `RhiMesh`-style `Mesh gate_pane;` (mirrors `Mesh resize_handle_mesh;`).

### 2. Frame material → dark_wood

- **`apply_kind_materials` `KIND_PORTAL`** → a neutral **wood-brown** base (e.g. `(0.32,0.22,0.13)`, roughness ~0.6), **no emissive** (the frame must not glow). This is the fallback when the `dark_wood` PBR textures aren't loaded.
- **Draw-loop branch** (the established material-override pattern, main.c ~12826 where the `g_dark_wood`/`g_stone_mat`/… branches live): `if (g_dark_wood.albedo_tex.id != 0 && o->mesh_ref && strcmp(o->mesh_ref, "gate") == 0) dm = g_dark_wood;` — the frame draws with dark_wood when available, wood-brown otherwise. Frame stays on the normal PBR `draw_mesh` path (lit).

### 3. The energy shader — a new GLSL + MSL twin (`PORTAL_VERTEX_SRC` / `PORTAL_FRAGMENT_SRC`)

Modeled on the **water shader** (main.c ~862–954): a procedural animated surface driven by `uTime`. Placed beside the other shader pairs under one `#ifdef SOL_RHI_METAL … #else … #endif` comment (the dual-backend law: GLSL and the MSL twin stay together; **grep struct AND body for every uniform field**).

**Vertex** (both twins): standard MVP. Uniforms `uModel`, `uView`, `uProj` (MSL: a `VU` struct at `buffer(2)`, matching the water VS). Pass `uv` to the fragment. (MSL: the `o.pos.z = (o.pos.z + o.pos.w)*0.5;` depth remap, as in every Metal VS here.)

**Fragment** (both twins): unlit, opaque. Uniforms (MSL: an `FU` struct at `buffer(0)`):
- `float uTime`
- `vec3 uPortalColor` (the blue, fed from the pass)

Procedural recipe (structurally identical in GLSL and MSL — a hash-based value `noise(vec2)` + a 2-octave `fbm`):
```
// uv = 0..1 across the opening
p   = uv * 3.0
t   = uTime * 0.3
warp = vec2( noise(p + t), noise(p - t*0.8 + 5.2) )     // domain warp -> swirl
n    = fbm( p + warp*1.5 + t*0.5 )                       // 0..1 energy field
c    = uv - 0.5
edge = smoothstep(0.20, 0.50, max(abs(c.x), abs(c.y)))   // 0 centre -> 1 at frame
col  = uPortalColor * mix(0.25, 1.0, n)                  // dark->bright by swirl
col += vec3(0.30, 0.50, 0.95) * edge * 0.8               // bright rim glow
FragColor = vec4(col, 1.0)                               // OPAQUE
```
(Brightness/scroll-rate/warp constants are the obvious tuning knobs — Fran live-tunes.) Aspect: the opening is portrait (≈1.6×2.28); if the swirl looks stretched, multiply the UV by the opening aspect in the shader (a one-line tweak) — noted, not load-bearing.

### 4. The pipeline

In the pipeline-setup block (main.c ~11515, beside `glass_pipeline`/`decal_pipeline`): copy the base `desc` (same 12-float vertex layout), swap in the portal shader, keep it **opaque** (`depth_test` on, `blend` off, depth-write on — the `desc` defaults):
```c
{   RhiPipelineDesc pd = desc;
    pd.shader = rhi_create_shader(PORTAL_VERTEX_SRC, PORTAL_FRAGMENT_SRC);
    if (pd.shader.id) state->portal_pipeline = rhi_create_pipeline(&pd);
}
```
Field: `RhiPipeline portal_pipeline;` (beside `glass_pipeline`/`decal_pipeline` ~main.c 2500/2634). A failed shader degrades to id 0 (the id-0 contract: a zero pipeline swallows its draws) — the launch still boots.

### 5. The render pass

A dedicated loop modeled on the **decal pass** (main.c ~13923), drawn in the opaque region (after the main objects; the pane is opaque so order vs. glass/decals doesn't matter — depth-tested). Per frame:
```c
if (state->portal_pipeline.id && state->gate_pane.index_count > 0) {
    rhi_set_pipeline(state->portal_pipeline);
    rhi_set_uniform_mat4 ("uView", view.m);
    rhi_set_uniform_mat4 ("uProj", proj.m);
    rhi_set_uniform_float("uTime", (float)glfwGetTime());
    rhi_set_uniform_vec3 ("uPortalColor", 0.20f, 0.45f, 0.95f);
    for (each scene object o) {
        if (o->kind != KIND_PORTAL) continue;
        if (!scene_object_active(&state->scene, o->handle)) continue;   /* workspace filter */
        w  = mesh_ref_param("gate", o->mesh_params, o->mesh_param_count, "w");
        h  = mesh_ref_param("gate", o->mesh_params, o->mesh_param_count, "h");
        pw = mesh_ref_param("gate", o->mesh_params, o->mesh_param_count, "post");
        oh = h - pw;                                       /* opening height */
        model = scene_world_matrix(o)
              * translate(0, oh*0.5, 0) * scale(w, oh, 1); /* unit quad -> opening */
        rhi_set_uniform_mat4("uModel", model.m);
        rhi_bind_vertex_buffer(state->gate_pane.vbuffer);  /* the decal-pass draw call */
        rhi_bind_index_buffer (state->gate_pane.ibuffer);
        rhi_draw_indexed(0, state->gate_pane.index_count);
    }
}
```
The draw call is exactly the decal pass's per-object draw (main.c ~13938–13941), but on the shared `gate_pane` mesh instead of `o->mesh`. The **workspace filter** (`scene_object_active`) is mandatory (the FILTER LAW — a portal in another workspace must not draw its pane). This pass runs in the **HDR region** (like the decals), so the bright swirl + rim feed bloom + tonemap + the color grade for free — no extra work.

## Data Flow

```
emit_gate (frame only) ─┐
state->gate_pane (unit quad, lazy-built) ─┘ → two separable surfaces
load/draw: frame → draw_mesh (dark_wood / wood-brown, lit)
           pane  → portal_pipeline pass: per active KIND_PORTAL, draw gate_pane
                   scaled to the opening, uTime-animated swirl + edge glow
portal_update (travel) — unchanged, geometry-independent
```

## File Touch List

- `mesh.c` — `emit_gate`: drop the pane `aabb_box` (frame-only).
- `main.c`:
  - `PORTAL_VERTEX_SRC` / `PORTAL_FRAGMENT_SRC` (GLSL + MSL twin).
  - `AppState`: `Mesh gate_pane;` + `RhiPipeline portal_pipeline;`.
  - pipeline setup: create `portal_pipeline`.
  - `apply_kind_materials` `KIND_PORTAL`: wood-brown base, no emissive.
  - draw-loop material branch: `mesh_ref=="gate"` → `g_dark_wood`.
  - the portal-energy render pass (lazy-build `gate_pane`, draw per portal).

## Testing

- **Geometry:** `emit_gate` frame-only + the unit-quad pane build cleanly; a quick visual smoke (frame has a hole, pane fills it).
- **Dual-backend (the real risk):** the MSL twin compiles **on-device**, so a struct/body mismatch passes `build.sh metal` and breaks at launch → **grep the `FU`/`VU` struct AND the body for every uniform** (`uTime`, `uPortalColor`, `uModel/uView/uProj`). Build gauntlet: `./build.sh` (GL strict) + `./build.sh metal`, then **launch BOTH** `./solarium` and `./solarium-metal` to confirm the portal shader compiles and the pane renders (no on-device MSL error).
- **Human live-verify:** frame is dark wood; the pane swirls + glows blue and animates; both backends match; other workspaces' portals don't leak a pane.

## Risks

- **MSL twin sync** is the primary risk (the engine's standing dual-backend tax). Mitigation: model exactly on the water twin; grep struct + body; launch both.
- Shader-compile failure degrades (id-0 contract) rather than crashing — the pane just won't draw; the frame still works.
