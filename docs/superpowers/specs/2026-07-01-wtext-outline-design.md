# World-Text SDF Outline — Design

**Date:** 2026-07-01
**Status:** Approved, ready for plan
**Context:** world text is SDF (`wtext.c`, GLSL + MSL twin). This adds a reusable **outline/halo** to the SDF fragment shader so labels read on any background — replacing the map-pin's 4-copy depth-layered halo hack with one crisp draw. Built now for pin labels; available to any world-text caller.

## Why

Pin labels currently fake an outline by drawing the text 5× (four dark offset copies behind + one light copy in front, on two depth planes). It's awkward and costs 5 draws. SDF text is *designed* for outlines: the fragment shader already has the per-pixel distance to the glyph edge, so an outline is a second distance band in ONE draw — crisp at any projected scale. This is the standard technique; it belongs in the renderer, not faked at the call site.

## Architecture — the outline in the SDF fragment shader

The wtext FS gains two fragment uniforms — `uOutlineColor` (vec3) and `uOutline` (float, the outline half-width in SDF distance units, `0` = none) — and computes body + outline from the one distance sample:

```glsl
float d    = texture(uTex, vUV).r;
float w    = fwidth(d) * 0.5 + 0.0001;
float fill = smoothstep(0.5 - w, 0.5 + w, d);   /* the glyph body (as today) */
float oe   = 0.5 - uOutline;                     /* outline reaches a lower distance -> extends outward */
float cov  = smoothstep(oe - w, oe + w, d);      /* body + outline coverage */
if (cov < 0.004) discard;                        /* (metal: discard_fragment) */
vec3  col  = mix(uOutlineColor, uColor, fill);   /* outline color outside the body, fill inside */
FragColor  = vec4(col, cov);                     /* (metal: return float4(col, cov)) */
```

**The invariant that makes this safe:** when `uOutline == 0`, `oe == 0.5`, so `cov == fill`; and if the caller passes `uOutlineColor == uColor`, then `col == uColor`. That is **byte-identical to the current shader** (`return float4(uColor, edge)`), with no branch. So every existing world-text caller is provably unchanged.

- Edited in **both** the GLSL FS and the **MSL twin** FS (the dual-backend law). The MSL `struct FU` grows to `{ float3 uColor; float3 uOutlineColor; float uOutline; }`.
- Uniforms are set **by name** (`rhi_set_uniform_vec3("uOutlineColor", …)`, `rhi_set_uniform_float("uOutline", …)`); the Metal RHI **reflects** the struct to locate the fields, so there is NO hand-packed constant buffer and field order only needs to be sensible, not manually offset-matched.
- `uOutlineColor`/`uOutline` are *used* in the FS, so neither backend strips them from reflection.

## API — `wtext_block_outlined` (a sibling)

`wtext.h` gains one function beside `wtext_block` / `wtext_block_bent`:

```c
/* Like wtext_block, but with an SDF OUTLINE: `ow` is the outline half-width in
   SDF distance units (0..~0.3; 0 = none), drawn in (or,og,ob) around the
   (r,g,b) fill. One draw — reads on any background. */
void wtext_block_outlined(const Font *f, mat4 viewproj, mat4 model, const char *utf8,
                          float x, float top_y, float px_to_m, float wrap_w_m,
                          float r, float g, float b,
                          float or_, float og, float ob, float ow);
```

`wtext_block` and `wtext_block_bent` keep their exact signatures and behavior. Internally, the uniform-set-and-draw is factored into one shared helper (`wt_draw(mvp, vc, r,g,b, or_,og,ob, ow)`) that sets `uColor`, `uOutlineColor`, `uOutline` (plus `uTex`/`uMVP`) and issues the draw. `wtext_block` and `wtext_block_bent` call it with `outlineColor = fillColor, ow = 0` (the identical-output path); `wtext_block_outlined` passes the real outline. No change to `wt_build` (the glyph-quad builder) or the glyph cache.

**Out of scope:** a bent+outlined variant (bent text is book pages; no outline need). The shared helper leaves the door open if wanted later.

## The pin label (main.c)

Replace the current 4-copy halo + second depth plane with **one** `wtext_block_outlined`: the light fill (`0.99, 0.98, 0.94`) with a near-black outline (`0.04, 0.04, 0.05`, `ow ≈ 0.2`). Delete the `hx`/`hy` offset arrays, the `face2` plane, the `ho` offset, and the halo loop. The proportional-size logic (`PIN_LABEL_FRAC`) and the `top_y` clearance stay.

## Testing

- **No pure unit** — it's a shader + GL/Metal state. Verification is the gauntlet plus live-verify.
- **Gauntlet incl. `./build.sh metal`** is load-bearing here: the MSL twin must compile AND its reflection must expose the two new uniforms (a name typo or a stripped uniform would surface at reflect/set time).
- **The critical invariant to live-verify:** all EXISTING world text is visually unchanged — card labels, note bodies, doorway/route labels, and open-book pages — on BOTH GL and Metal (the `uOutline == 0`, `uOutlineColor == uColor` degrade path). Then the pin labels show a clean, crisp outline (no more multi-copy halo) on both backends.

## Out of scope (deferred)

- Retrofitting card/doorway/note/book labels with outlines (the capability exists; they opt in later if desired).
- Outlined *bent* text.
- Drop-shadow / glow (the same SDF machinery could do these; not now).
