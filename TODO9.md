# Solarium — Phase 9 Execution Brief (Items 1–8): Stained Light — Surfaces, Reflections, and the Picture

> **Numbering & status note.** TODO5.md (the app engine) still waits at its boundary; the
> gothic, flora, and felt-world phases jumped its queue three times, profitably. This is
> **Phase 9** by number — surfaces, reflections, and the finished image — drawn from the
> low-risk tail of `engine-ideas.md` (the renderer-feature survey, 2026-06-16), specifically
> the items Phase 8's Part 5 *deferred by name*. The execution order between it and Phase 5
> remains the project owner's call. Items are numbered 1–8 *within this phase*; cite as
> "P9 · Item N".

## How to use this document

Phase 8 made the world **felt** — volumetric air, cast light, sound. It spent the dual-backend
trust the two pure-CPU phases had banked, and spent it cleanly: no launch-time MSL-twin break,
the frame still locked at 16.7 ms. **This phase dresses the surfaces that light lands on and
finishes the image it forms.** After a phase about *the medium* (fog, shafts, reverb), this is a
phase about *the material* (glass, reflections, splat, stains) and *the picture* (grade, AA).

The continuity is exact, not thematic. **Phase 9 cashes the checks Phase 8 wrote.** TODO8's
Part 5 declined SSR and OIT *on the explicit promise that their substitutes would land later* —
"reflection probes & the planar pond, a reflections mini-phase of their own," "OIT substituted
by sorted-alpha derived from the plan," "decals / MSAA-FXAA, each its own later item." Those
substitutes are this phase. Declining TAA (§8.1.3) was load-bearing precisely because it
promised these crude, non-temporal, *placement-aware* forms instead — and here they ship.

The organizing thesis carries forward and extends. Phase 8's §1.4 made one depth buffer serve
many readers — **the one-author law, render edition.** Phase 9 moves that law from *depth* to
*material*: the **plan** authors the glass draw-order (sorted-alpha, not a per-pixel OIT solve);
the **height field** authors the terrain splat (texgen is the one author, as it already is for
albedo/normal/ORM); the **room** authors the reflection probe (the same containment query that
drives wind and reverb). Same law, material edition. The crude/non-temporal discipline is
unchanged — reflections are probes + a planar mirror, not SSR; transparency is sorted-alpha, not
OIT; AA is FXAA, not TAA — and the slow pilgrim camera still forgives the noise a fast one would
not.

**Sequencing.** Item 1 (color grade) is the **warm-up gate**: a frame-safe post-twin edit that
re-warms the dual-backend discipline and gives an immediate art-direction payoff (the survey's
"disproportionate work" layer). Item 2 (stained glass) is the **marquee** — it establishes the
transparent pass the later surface items lean on, and it is half of the capstone. Items 3–6 are
the surface and reflection body (decals, probes, the planar pond, terrain splat). Item 7 (FXAA)
cleans the finished picture last. Item 8 composes them — and Phase 8's volumetrics — into the
phase's one new *feature that needs both phases to exist*: colored light through the glass.

**Part 1 is binding on every item.** Each item states **intent**, **spec**, and **acceptance**.
Reserved decisions are flagged inline and collected in Part 4. Three are already ruled by the
owner: the phase name (**Stained Light**), the AA technique (**FXAA first**, MSAA deferred), and
the existence of the **Item 8 capstone**.

---

## Part 1 — The Constitution

### 1.1 What is already established (maintain it)

- **The RHI seam is inviolable.** Only `rhi_gl.c` touches GL; only `rhi_metal.m` touches
  Metal/ObjC. Every new shader is a GLSL source string *and* an MSL twin selected at the seam.
  C11 atomics/threading stay quarantined to `platform_audio.c`. This phase has **no audio item**
  and therefore no tax-free item (§1.2).
- **The frame's pass structure, as Phase 8 left it**: sun shadow cascades (×2) → spot sconce
  shadow → HDR opaque (writes the sampleable depth) → depth copy → particles (additive-in-HDR) →
  water → god-rays (½) → SSAO (½) → bloom → post/tonemap (ACES, sRGB) → UI overlay. **New passes
  name where they slot in.** Phase 9 inserts a transparent/glass pass after opaque, a decal
  contribution reading the depth texture, a planar-reflection render feeding the water pass,
  off-frame probe captures, and grade + FXAA at the tail of post.
- **The IBL chain** (irradiance + prefilter + BRDF LUT, re-baked on the U/Z mint seam) and
  **bloom**. The reflection probes (Item 4) *reuse the prefilter machinery and ride the same mint
  seam* — they extend this chain, they do not replace it.
- **The sampleable scene-depth texture (Phase 8 §1.4).** Decals (Item 3) and the capstone's
  floor projection (Item 8) become new readers of the *same* buffer. No item re-derives depth.
- **The HUD + per-pass GPU timers (Phase 8 Item 1).** Every drawing item this phase reports its
  per-pass cost there; every "is it within budget" claim is a timer capture, not a vibe (§1.5).
- **Strict C89** for all CPU code, `./build.sh c89check`, the forbidden-construct list,
  `sol_base.h`. Shaders are their own languages behind the seam.
- **Synthesized, never sourced.** Terrain materials (Item 6) are texgen presets; decal art
  (Item 3) and LUTs (Item 1) are the one permitted exception worth flagging — see §1.6. No mesh
  CSG; build around gaps.

### 1.2 NEW — The dual-backend tax is undiluted this phase

Phase 8 had one tax-free item (audio, in the quarantine, no RHI seam). **Phase 9 has none — all
eight items draw.** Every one is a GL implementation *and* a hand-written MSL twin. The standing
scar is unchanged: `build.sh metal` compiles only C/ObjC; the MSL strings compile *on-device*,
so a twin struct/body field mismatch passes the build clean and surfaces only at launch. **The
law: when you edit a Metal twin's uniforms, grep the struct AND every body reference for each
field name before you call the build done.** Both backends must agree visually on every item.

### 1.3 NEW — The no-TAA substitutes ship here (the §8.1.3 bargain, paid)

Phase 8 made declining TAA constitutional, and that "no" licensed three substitutions it
promised but did not build. This phase builds them:

- **Transparency is sorted-alpha from the plan, not OIT** (Item 2). The stained glass is placed,
  parametric, and mostly planar — its back-to-front draw order is *computable from placement*,
  not a per-pixel linked-list solve.
- **Reflections are probes + a planar mirror, not SSR** (Items 4, 5). Few reflective surfaces;
  the general per-pixel march buys this content almost nothing.
- **AA is FXAA, not TAA** (Item 7). A single non-disruptive post pass on the tracery and twigs
  the survey names as the worst case — chosen over MSAA (§reserved, ruled) for not tangling the
  depth-texture readers with multisampled targets.

The crude forms leave noise; the slow camera forgives it. This is the same trade Phase 8 made,
applied to surfaces instead of air.

### 1.4 NEW — The plan authors the surface, too (one-author law, material edition)

Phase 8's §1.4 was "one depth buffer, many readers." Phase 9 generalizes it from the framebuffer
to the material:

- **The plan authors the glass order.** `church_plan` knows where every window is; the
  transparent pass derives its draw order from that placement, not from a runtime per-fragment
  sort.
- **The height field authors the splat.** Terrain material weights (grass/rock/dirt by slope and
  altitude) come from the same height field texgen already uses to author albedo/normal/ORM —
  one author, now feeding the blend.
- **The room authors the probe.** A reflection probe is captured *per room*; the runtime selects
  it with the same `room_containing` / containment query that already drives the wind and reverb.

No surface feature invents a second description of where things are. The placement is the author;
the shader is a reader.

### 1.5 NEW — Reflections ride the IBL/mint seam (and pre-pay for GI)

The reflection probes (Item 4) are captured and prefiltered on the **U/Z mint seam** the IBL
chain already fires — a probe re-bakes when its room is minted or edited, and is static between
edits (no per-frame capture). This is not a new "when to bake" system; it is the existing one,
extended. And it is a **down-payment on the GI successor phase** (§1.6): a per-room captured,
prefiltered, parallax-corrected probe is one step from the SH irradiance probes GI will bake into
a room's air. The reflection infrastructure built here is the reflection half of GI's probe
infrastructure.

### 1.6 NEW — The two permitted binaries, named honestly

This engine's pride is *synthesized, never sourced*. Two items brush that line and must do so on
purpose, not by drift:

- **LUTs (Item 1)** are small data textures (a neutral identity LUT plus a handful of grade
  presets). They are *parameters as a texture*, the same spirit as a synth preset — and the
  identity LUT must return today's image bit-for-bit, proving the grade is opt-in.
- **Decal art (Item 3)** wants a stain/moss/streak source. The discipline: prefer a **texgen-
  synthesized** decal atlas (stains and moss are noise fields texgen can already author) before
  reaching for any external image. If a sourced decal is ever admitted, it is gitignored like the
  HDRs and flagged in the commit — not smuggled into the synthesized canon.

### 1.7 GI is still the successor phase, not an item here

Bounced light re-imagined as bake-once-per-param-hash-and-cache remains **out of scope** — a
phase, not an item. Phase 9 builds the *reflection* half of its probe machinery (§1.5) and
nothing more. GI keeps its own brief.

### 1.8 Definition of done (every item)

Suites green, `c89check`, all three app builds, **both backends visually agreed where the item
draws** (this phase's sharpest gate, as it was Phase 8's), a **timer capture** showing the item's
per-pass cost in ms on the M2 in both backends, Fran-verified live, committed, memory record.

---

## Part 2 — The Items

### Item 1 — Color grading / LUT + vignette (the post warm-up; gate; do first)

**Intent.** The cheap, frame-safe re-warm of the dual-backend post twin, and the art-direction
layer the survey calls "disproportionate work" (the Witness / Eastshade lesson). Establish the
grading slot every later item's color flows through.

**Spec.**
- A grading stage at the **tail of the post pass**, after the ACES tonemap and sRGB encode
  (**reserved decision R8** — after-tonemap LDR grade vs a pre-tonemap HDR grade; rec: after).
- A **2D-strip LUT** (a 16³ cube unwrapped to a flat strip texture — chosen over a 3D texture to
  dodge GL/Metal 3D-sampler portability concerns) sampled per pixel; a neutral **identity LUT
  returns today's image bit-for-bit** (§1.6).
- Plus analytic knobs — contrast / saturation / a lift-gamma-gain trio — and a parametric
  **vignette** (radial corner darkening).
- One shader edit per backend + a LUT load through the existing texture path. Mind §1.2: grep the
  new uniforms in struct AND body on the Metal twin.

**Acceptance.** Identity LUT returns exactly today's image on both backends; a warm-dusk LUT
visibly regrades the abbey; the vignette darkens the corners; the post timer line is unmoved
within noise.

### Item 2 — Stained-glass translucency (sorted alpha) — the marquee

**Intent.** Colored translucent glass in the clerestory — the survey's stained-glass image, and
the **OIT substitute** Phase 8 promised (§1.3): sorted-alpha from the plan, not a per-pixel solve.

**Spec.**
- A **transparent pass after the opaque HDR pass**: alpha-blended, depth-test on, depth-write
  off, so the glass reads against the lit scene and the sky/IBL behind it.
- **Draw order from the plan** (§1.4): the windows are placed and parametric, so their
  back-to-front order per view is computable from `church_plan` placement (**reserved decision
  R2** — plan-derived order vs a runtime per-draw depth sort; rec: plan-derived).
- The glass material: a tinted, semi-transparent surface carrying its existing PBR/IBL response,
  with per-window color from the plan. (The *colored light it throws* is the capstone, Item 8 —
  here the concern is the glass **surface** blending correctly.)
- GLSL + MSL twin.

**Acceptance.** The clerestory shows colored translucent panes the sky/IBL reads through;
overlapping panes blend in correct order with no popping as the camera orbits slowly; the opaque
scene behind reads correctly; both backends agree; within the transparent pass's budget slice.

### Item 3 — Decals

**Intent.** Ruin stains, moss, water streaks, grounding shadow patches — the gothic-ruin grammar
the abbey and the islands are built for; and the next reader of Phase 8's depth texture.

**Spec.**
- **Projected/deferred decals** (**reserved decision R5** — screen-space projected reading the
  §1.4 depth texture vs mesh decals; rec: projected, for the §1.4 reuse): reconstruct world
  position from the depth texture, project an oriented decal box, and blend its albedo / normal /
  roughness contribution into the HDR target where the projection hits, **normal-rejected** so it
  only lands on surfaces facing the projector and fades at grazing angles.
- **Placement via the scene/plan** (§1.4): decals are placed objects (handles/nids) or plan-
  emitted by the `church_plan` / terrain readers — the §1.2 placement doctrine ("a placed
  object's position is sacred") holds.
- **Art is synthesized first** (§1.6): a texgen stain/moss/streak atlas before any sourced image.
- GLSL + MSL twin.

**Acceptance.** A stain/moss decal projects onto a pier and the floor, following the surface,
fading at steep angles and rejecting back-facing geometry; placed decals persist via the scene
format; distant geometry is untouched; both backends agree; within budget.

### Item 4 — Reflection probes (per-room local cubemaps)

**Intent.** Polished stone, wet floor, and metal inside the abbey should reflect *their room*,
not the global skybox — the reflection mini-phase Phase 8's Part 5 named, and the reflection half
of GI's probe machinery (§1.5).

**Spec.**
- Capture a **cubemap at a probe point per room/area** and prefilter it into the same GGX-mip
  chain the IBL prefilter already produces — **reuse the prefilter machinery** (one author, the
  bake).
- **Bake on the U/Z mint seam** (§1.5): a probe re-captures when its room is minted/edited, and
  is static between edits — no per-frame capture.
- Runtime: **select the probe by the fragment's room** (the `room_containing` containment query),
  apply **box-parallax correction** (**reserved decision R3** — box-parallax-corrected vs
  global/infinite; rec: box-parallax) so the reflection aligns to the room's walls; **fall back to
  the global IBL prefilter** outside any room. The PBR specular-IBL lookup gains a probe path.
- GLSL + MSL twin.

**Acceptance.** A reflective surface inside the abbey reflects the abbey interior, not the open
sky; crossing a room boundary swaps probes without an objectionable pop; minting a room bakes its
probe; open-island surfaces still use the global IBL; both backends agree; capture cost is a mint-
time event (not a per-frame line) and the runtime lookup is within budget.

### Item 5 — Planar pond reflection (the true mirror)

**Intent.** Finish the P7 pond — the real single-plane mirror (the yews and the abbey inverted in
the water), replacing today's fresnel × prefilter-IBL approximation.

**Spec.**
- A **reflection render**: mirror the camera across the pond plane and render the scene into a
  reflection target with an **oblique near-plane clip** at the water surface (**reserved decision
  R4** — oblique-clip planar re-render vs keep/extend the fresnel-IBL; rec: planar re-render,
  since you have one dominant plane and want the sharp version). Sample that target in the water
  shader, fresnel-blended with the existing refraction/tint and **distorted by the P7 ripple
  normal**.
- **Bounded**: only the nearest/active pond, only when on-screen, at reduced resolution; timer-
  verified (§1.5) — one extra scene render is the cost, paid where expected.
- Reuses the P7 water surface and ripple. GLSL + MSL twin.

**Acceptance.** The pond mirrors the yews and the abbey; the reflection ripples with the surface;
fresnel mirrors at grazing angles and clears looking straight down; the reflection render shows on
the timer within budget; the fresnel-IBL path remains a clean fallback; both backends agree.

### Item 6 — Terrain splatting / triplanar

**Intent.** Grass, rock, dirt, and moss blended across the islands by slope and altitude —
material richness authored by the **same height field texgen already owns** (§1.4).

**Spec.**
- The terrain material samples multiple synthesized albedo / normal / ORM sets and blends by
  **slope (normal.y) and height**, with the weights derived from the height field — flat + low →
  grass, steep → rock, the band between → dirt/moss.
- **Triplanar projection on steep faces** where planar UVs stretch (**reserved decision R6** —
  slope-threshold triplanar, planar where flat and triplanar only on cliffs, vs triplanar
  everywhere; rec: slope-threshold, for the cost).
- **Materials are texgen presets** — zero new texture binaries (§1.6, kinds-are-presets). GLSL +
  MSL twin in the terrain shader.

**Acceptance.** An island shows grass on its crown, rock on its cliffs, and dirt/moss between,
blended without seams and without UV stretching on steep faces; the blend follows the height field
(re-deriving correctly on a new island seed); both backends agree; terrain shader cost within
budget.

### Item 7 — FXAA (the final anti-alias)

**Intent.** Clean the tracery, twigs, and high-contrast silhouettes the survey names as the
aliasing worst case — the non-TAA way (§1.3). FXAA is **ruled** (reserved R7) over MSAA: a single
non-disruptive post pass that does not tangle the depth-texture readers with multisampled targets.

**Spec.**
- A standard **FXAA pass as the final step after grading** (luma-based edge detect + directional
  blend) on the tonemapped LDR image. One full-screen pass; **drawn before the UI overlay** so the
  HUD/text stay crisp. GLSL + MSL twin.
- **Toggleable** (a key) for an honest before/after.
- **MSAA flagged as the heavier geometry-true upgrade** (Part 5), not built now.

**Acceptance.** Tracery edges, twigs, and silhouettes show visibly reduced crawl/stair-stepping
under the slow camera; the HUD/text (drawn after) is unaffected; toggling FXAA off returns the
aliased image; cost is one post pass within budget; both backends agree.

### Item 8 — The capstone: colored light — the glass projects

**Intent.** The phase's portfolio shot and the proof the items compose — and the one *feature*
that cannot exist until Phase 9's glass meets Phase 8's volumetrics: **the stained glass tints the
light that passes through it**, throwing colored shafts in the air and colored pools of light
across the nave floor. The signature cathedral image (Sainte-Chapelle).

**Spec.**
- The stained-glass windows (Item 2), being placed / parametric / planar, carry a **known color
  per window from the plan.** The light through them takes that color, two ways:
  - **Colored shafts**: where the Phase 8 god-ray march crosses a window's plane along the sun
    direction, **tint the accumulated in-scatter by that window's color** — the glass is a colored
    filter on the volumetric shaft.
  - **Colored pools**: project the window's color along the sun direction onto the receiving
    floor and walls — a **colored projective gobo / light-cookie** keyed to the window + sun,
    reusing Item 3's projection-and-depth-read machinery, so a pool of stained color lands where
    the window's light falls.
- **Crude / non-temporal** per the constitution: a modest projection, no caustics, no per-frame
  recapture — the slow camera forgives it. (**Reserved decision R9** — shaft-tint only vs shaft +
  floor-pool; rec: both, the floor pool is the iconic half.)
- **The composer (extend Z)**: the abbey with a low sun through the south clerestory — colored
  shafts in the dusty air (Item 8 × P8 god-rays × P8 particles), colored pools on a decaled floor
  (Item 8 × Item 3), the pond outside mirroring the structure (Item 5), every surface graded
  (Item 1) and FXAA-cleaned (Item 7). A **daylight counter-shot** proves the surfaces — splat,
  decals, reflections — read in the open, without the church.
- **`SURFACE.md`** (the GOTHIC.md / FLORA.md / ATMOS.md sibling index, **reserved decision R10**
  on the name — rec: SURFACE.md): the transparent pass order, the plan-authors-the-surface law,
  the reflection-bake-on-mint seam, the FXAA-not-MSAA decision, the budgets, the keys.
- **All budgets timer-verified**: the dressed scene holds **16.7 ms** on the M2 in *both*
  backends. The phase's whole point is that the finished picture is still vsync.

**Acceptance.** The screenshot — colored light shafting through the stained glass, pools of
stained color on the nave floor, the pond mirroring the abbey, every surface graded and clean —
and a daylight wild island proving the surfaces stand on their own. Every suite green; both
backends agree; `SURFACE.md` written; the phase closes.

---

## Part 4 — Decisions reserved to the project owner

1. **Selection & sequence of items** — the meta-decision. These eight, in this order
   (grade gate → glass marquee → decals → probes → planar pond → splat → FXAA → colored-light
   capstone)? Drop, add, or reorder before any code. *(Name, AA, and the capstone's existence are
   ruled; the rest of this list is open.)*
2. **Color-grade placement** (Item 1, R8): after-tonemap LDR LUT vs pre-tonemap HDR grade.
   Rec: after-tonemap, 2D-strip LUT.
3. **Stained-glass draw order** (Item 2, R2): plan-derived sorted alpha vs runtime depth sort.
   Rec: plan-derived.
4. **Reflection-probe parallax** (Item 4, R3): box-parallax-corrected per room vs global/infinite.
   Rec: box-parallax, bake-on-mint.
5. **Planar pond technique** (Item 5, R4): oblique-clip planar re-render vs extend the fresnel-IBL.
   Rec: planar re-render.
6. **Decal type** (Item 3, R5): screen-space projected reading the depth texture vs mesh decals.
   Rec: projected (reuses §1.4).
7. **Terrain splat** (Item 6, R6): slope-threshold triplanar vs triplanar everywhere.
   Rec: slope-threshold.
8. **AA technique** (Item 7, R7): **RULED — FXAA first**; MSAA deferred as the geometry-true
   upgrade (Part 5).
9. **Capstone scope** (Item 8, R9): shaft-tint + floor-pool vs shaft-tint only. Rec: both.
10. **Index doc name** (Item 8, R10): `SURFACE.md` vs `GLASS.md` / `FINISH.md`. Rec: `SURFACE.md`.
11. **Ordering vs Phase 5** — this phase before or after the TODO5 app engine. The standing
    meta-decision, unchanged since Phase 6.

## Part 5 — Deliberately deferred (not this phase)

- **MSAA** — the geometry-true AA upgrade. FXAA ships first (§1.3, reserved R7); MSAA waits until
  the eye and the timer agree FXAA's softening on the tracery isn't enough. Its cost — multisampled
  HDR *and* depth targets + resolve steps, tangling the depth-texture readers — is exactly why it
  is not the v1.
- **Omni point-light cube shadows** — Phase 8 Item 7 cast a *spot* (one 2D map); the omni cube-map
  generalization for a bare candle flame remains the flagged follow-up.
- **The character / movement cluster** — cloth via PBD (banners, tapestries, vestments), two-bone
  IK foot-planting, animation crossfade + a small state machine, navmesh + A*. All sound survey
  items; none belongs to a surfaces-and-picture phase. They are their own later phase, due when
  creatures drive the world.
- **The authoring / interactivity cluster** — in-world transform gizmos + object inspector,
  trigger volumes. Clean given the handle/nid identity and overlay doctrine, but ergonomics, not
  picture; their own later item set.
- **Clustered / tiled forward** — still measure-first (§8.1.5). The timers exist now; the 8-light
  cap stands until a real scene's capture proves it must fall. Very possibly never.
- **GI / bounced light** — still the marquee successor phase (§1.7). Phase 9's reflection probes
  are a down-payment on its probe infrastructure (§1.5), nothing more.
- **TAA** — declined on principle (§1.3), permanently. Not deferred — refused.
- **SSR** — substituted by the reflection probes + the planar pond, both built this phase.
- **OIT** — substituted by sorted-alpha from the plan (Item 2), built this phase.
- **A true day/night cycle** — render-architecture that brushes GI; the `hdr_reload` swap
  (Phase 8 Item 9, the backtick lever) remains the v1.
