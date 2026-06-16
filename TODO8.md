# Solarium — Phase 8 Execution Brief (Items 1–9): The Felt World — Light, Air, and Sound

> **Numbering & status note.** TODO5.md (the app engine) still waits at its boundary, and
> the gothic and flora phases jumped its queue twice, profitably. This is **Phase 8** by
> number — atmosphere, cast light, and sound — drawn from `engine-ideas.md`, the
> renderer-feature survey written 2026-06-16. The execution order between it and Phase 5
> remains the project owner's call. Items are numbered 1–9 *within this phase*; cite as
> "P8 · Item N".

## How to use this document

Phases 6 and 7 were a deliberate **pure-CPU holiday**: the gothic kit and the living island
touched zero shader code, zero RHI seam, while dual-backend trust accrued. That holiday paid
off — Phase 7 item 10 was Fran-verified visually identical on both backends, the frame locked
at 16.7 ms, the Metal vsync stutter dead. **Trust has accrued. This phase spends it.**

Almost every item here is a renderer feature: it lands in `rhi_gl.c` *and* as a runtime-
compiled MSL twin in `rhi_metal.m`, under the discipline the fog scar taught (§8.2). After two
phases of growing *what the world contains*, this phase changes *how the world is felt* — the
floating island reads as floating in real air; god-rays pour through the clerestory; candle-
light actually casts; the cathedral finally sounds like one. The content is already built; this
is the atmosphere it has been waiting for.

The organizing thesis, taken whole from the survey: **the difficulty in modern rendering is
almost always temporal** — reprojecting last frame's data across motion and disocclusion — and
this engine's two superpowers, **determinism** and **a slow, deliberate camera**, let it
sidestep the temporal version of nearly everything. We take the crude, honest, non-temporal
form of each technique and lean on the pilgrim's-eye camera that forgives the noise a fast
camera would not. The single most important consequence is a *refusal*, made constitutional in
§8.3: **this engine will not build TAA.** That one "no" de-risks four other items down to their
buildable forms.

**Sequencing.** Item 1 (the GPU timer instrument) is the gate — it makes every "measure first,
don't build it until a real scene blows the budget" call evidence-based instead of a vibe, and
it re-warms the RHI-twin discipline on a low-stakes feature. Items 2–5 are the **depth-reading
cluster** (fog/aerial, god-rays, soft particles, SSAO) — they share one investment, a sampleable
scene-depth texture, landed once and read four times (§8.4). Items 6–7 are the **shadow-map
cluster** (cascades for the sun, cube maps for candle-light). Item 8 is sound — the one item
that dodges the dual-backend tax entirely. Item 9 composes them into the portfolio shot and
the index doc.

**Part 1 is binding on every item.** Each item states **intent**, **spec**, and **acceptance**.
Reserved decisions are flagged inline and collected in Part 4.

---

## Part 1 — The Constitution

### 1.1 What is already established (maintain it)

- **The RHI seam is inviolable.** Only `rhi_gl.c` touches GL; only `rhi_metal.m` touches
  Metal/ObjC. Every new shader is a GLSL source string *and* an MSL twin selected at the seam
  (the stage-e conventions from the Metal backend phase). C11 atomics/threading stay quarantined
  to `platform_audio.c` (Item 8's only home for a worker).
- **The frame's pass structure**: shadow pass (sun, by the light's volume) → HDR pass (by the
  camera, with skybox/IBL/PBR/bloom) → post/tonemap (ACES, the `uFogColor`/`uFogStrength`
  uniforms, the render-target V-flip rule on Metal). New passes name where they slot in.
- **The IBL chain** (irradiance + prefilter + BRDF LUT, re-baked on the U/Z mint seam) and
  **bloom** (Karis soft-knee, K toggles). Reflection-probe and god-ray work *read* these; they
  do not replace them.
- **The HUD** (frame ms, tri/draw counts, asset counts). Item 1 extends it; every later item
  reports its cost there.
- **Strict C89** for all CPU code, `./build.sh c89check`, the forbidden-construct list,
  `sol_base.h`. Shaders are their own languages behind the seam.
- **The 8-light windowed inverse-square cap** (`brdfDirect`, ≤8 lights as object META). It
  stays until Item 1's timers prove a real scene needs more (§8.5) — not before.

### 1.2 NEW — The dual-backend tax is this phase's discipline

Every visual item costs roughly twice: a GL implementation and a hand-written MSL twin. The
**Metal-twins runtime trap** is the standing scar — `build.sh metal` compiles only C/ObjC; the
MSL shader strings compile *on-device* via `newLibraryWithSource`, so a twin struct/body field
mismatch passes the build clean and only surfaces at launch (the `uFog` → `uFogColor`/
`uFogStrength` split that broke Metal after Phase 7 item 8). **The law: when you edit a Metal
twin's uniforms, grep the struct AND every body reference for each field name before you call
the build done.** Both backends must agree visually on every item that draws (§1.7).

### 1.3 NEW — Temporal is the enemy; the slow camera is the answer (NO TAA)

This engine will not build TAA. It is the single most finicky item on the survey, its worst
case is exactly this content (thin tracery, twigs, animated leaves ghost), and its payoff is
smallest precisely where this camera lives — slow, deliberate, pilgrim's-eye. **Declining TAA
is constitutional and load-bearing**: it is what licenses every technique here to take its
crude, non-temporal form. God-rays are a raymarch, not a froxel-temporal volume (Item 3).
Reflections are probes + the planar pond, not SSR. Transparency is sorted-alpha from the plan,
not OIT. AA, when wanted, is MSAA/FXAA, not TAA. The noise these crude forms leave is the noise
a slow camera forgives.

### 1.4 NEW — One depth buffer, many readers (the one-author law, render edition)

Items 2–5 all need to know *how far away the scene is* per pixel. The HDR pass's depth becomes
a **sampleable depth texture** — one RHI addition (a depth-texture attachment + its Metal twin),
landed in Item 2 and read by fog/aerial, god-rays, soft particles, and SSAO alike. No item
re-derives scene depth; each is a reader of the one buffer. (This is the gothic plan law moved
from geometry to the framebuffer: a single source, many consumers, no second description.)

### 1.5 NEW — Measure first (the YAGNI instrument)

Item 1 exists so that the survey's "don't build it until a scene blows the cap" calls are
honest. Per-pass GPU timing turns "the 8-light cap is probably fine" and "clustered-forward is
probably unnecessary" into measured facts. The rule: **no performance-driven architecture
change (clustered/tiled forward, light culling rework) ships without a timer capture showing a
real scene exceeding budget.** Until then, the simple path stands.

### 1.6 NEW — GI is the successor phase, not an item here

The survey's highest-value idea — bounced light re-imagined as **bake-once-per-param-hash and
cache**, riding the `asset.c` params-are-identity store and the IBL-rebake-on-mint seam this
engine already fires — is deliberately **out of scope**. It is a phase, not an item: SH
irradiance probes baked into a room's air, the static/dynamic split letting trees and the fox
sample rather than bake, a worker thread resolving the light progressively. This phase builds
the two things GI will stand on — the timer instrument (Item 1) and the depth/G-buffer habits
(§1.4) — and leaves GI its own brief.

### 1.7 Definition of done (every item)

Suites green, `c89check`, all three app builds, **both backends visually agreed where the item
draws** (this phase's sharpest gate), a **timer capture** showing the item's per-pass cost in ms
on the M2 in both backends, Fran-verified live, committed, memory record.

---

## Part 2 — The Items

### Item 1 — The instrument: per-pass GPU timers + the budget HUD (gate; do first)

**Intent.** Make the rest of the phase measurable before it is built. Re-warm the RHI-twin
discipline on a feature that cannot break a frame.

**Spec.**
- A small `rhi_timer` seam: begin/end a named GPU timing scope around a pass.
  GL via `glQueryCounter`/`GL_TIMESTAMP` (double-buffered so the read is never a stall);
  Metal via the command-buffer GPU start/end timestamps (or counter sample buffers).
- The HUD gains a per-pass breakdown: shadow / HDR / post / (later) god-ray / SSAO / water,
  each in ms, plus the existing frame total and tri/draw counts.
- **Reserved decision #1 — readback latency**: accept one-to-two-frame-old timer values (the
  honest, stall-free path) **vs** force a sync for exact-frame numbers. Recommendation:
  N-frame-old; this is a HUD, not a profiler bus.

**Acceptance.** The HUD shows per-pass ms on both backends; toggling bloom (K) visibly moves the
post line; numbers are stable frame-to-frame; zero measurable overhead when the HUD timer is off.

### Item 2 — Height & exponential fog + aerial perspective

**Intent.** Make the floating island read as floating *in air*, at real scale — the cheapest
high-payoff visual on the survey, extending uniforms that already exist.

**Spec.**
- Land the **sampleable scene-depth texture** (§1.4) here — the one RHI addition (depth attach-
  ment on the HDR target + Metal twin) every later depth-reader consumes. Reconstruct world
  position from depth + inverse-viewproj in the post pass.
- Extend the post shader's existing fog (`uFogColor`/`uFogStrength`) from a flat tint to
  **exponential distance fog + a height falloff** (density decays with altitude, so the island's
  underside sinks into haze and its crown stands clear) and a mild **aerial-perspective** tint
  (distant geometry leans toward the sky/horizon color sampled from the IBL).
- All in the existing post pass — no new pass, one shader edit per backend (mind §1.2).

**Acceptance.** A floating island viewed from its own height shows depth haze that sells scale;
the underside reads as far below; turning fog off returns exactly today's image; both backends
agree; the post timer line is unmoved within noise.

### Item 3 — Volumetric god-rays (crude, non-temporal raymarch)

**Intent.** The phase's marquee visual and the survey's highest non-baking payoff: shafts of
light pouring through the clerestory and tracery. The cathedral image.

**Spec.**
- A raymarch from the camera through the scene, sampling the **sun shadow map** at N steps along
  each view ray (read the depth texture from §1.4 to stop the march at geometry), accumulating
  in-scatter where the ray is lit. Crude on purpose: a modest step count, a dithered start
  offset to trade banding for noise, **no temporal reprojection** (§1.3) — the slow camera
  forgives it.
- Its own half-resolution pass between HDR and post, composited additively (the bloom chain's
  precedent for a downsampled contributory buffer), so the cost is bounded and tunable.
- Density driven by the Item 2 fog field — god-rays and fog are the *same medium*, lit vs unlit;
  one author, two readings.
- **Reserved decision #2 — resolution & blur**: half-res + bilateral upsample (sharper, more
  code) **vs** quarter-res + simple blur (cheaper, softer). Recommendation: half-res, depth-
  aware upsample — the tracery edges are the whole point.

**Acceptance.** Standing in a church at the survival angle, light visibly shafts through the
clerestory and falls on the floor; the rays move correctly as the sun (IBL) changes; the god-ray
timer line is within the phase's per-item budget on the M2; both backends agree on the look.

### Item 4 — Soft particles

**Intent.** Dust motes that fade *against* geometry instead of slicing through it — small code,
outsized "sits in the world" payoff, and the depth machinery is already standing.

**Spec.**
- The particle FS (already an additive soft disc, depth-write off) gains one **scene-depth
  sample** (§1.4): fade alpha by the difference between the particle's depth and the scene depth
  behind it, so a mote nearing a wall softens to nothing rather than showing a hard intersection
  edge.
- One FS edit per backend + binding the depth texture into the particle pass; the CPU pool
  (`particles.c`) is untouched — this is purely a fill/shader concern.

**Acceptance.** The E-key dust near a pier or trunk fades at contact with no hard edge; distant
motes are unchanged; `parttest` still green (no CPU change); both backends agree.

### Item 5 — SSAO / GTAO (non-temporal)

**Intent.** Dynamic contact occlusion where pier meets floor, trunk meets terrain, rubble meets
ground — the grounding that baked AO can't give a player-built world.

**Spec.**
- A half-res AO pass reading the depth texture (§1.4) and reconstructed normals (from depth
  derivatives v1, or a normals attachment if §1.4 grows one — **reserved decision #3**):
  GTAO-style horizon sampling, a handful of directions, a bilateral blur, **no temporal
  accumulation** (§1.3 again — the blur and the slow camera stand in for it). Modulates the
  ambient/IBL term only (never direct light), composited before bloom.
- Bounded by the timer (Item 1): if the AO pass exceeds its slice of the frame, drop directions
  before resolution.

**Acceptance.** Contact darkening appears at pier/floor and trunk/terrain seams, stable as the
camera moves slowly (no boiling), gone when toggled off; ambient-only (direct highlights
unaffected); both backends agree; within budget.

### Item 6 — Cascaded shadow maps (the sun, sharpened)

**Intent.** Tight sun shadows across a whole island instead of one stretched map — and, for
free, a sharper march for the god-rays that already read the sun map (§1.4's law: improve the
author, every reader benefits).

**Spec.**
- Split the view frustum into 2–3 cascades, a shadow map each, selected per-fragment by depth;
  stabilize the texel snap so slow camera motion doesn't shimmer the edges (the one temporal-ish
  concern, solved the classic non-TAA way — texel-grid snapping, not reprojection).
- The HDR pass shadow lookup and the Item 3 god-ray march both switch to cascade selection; the
  shadow pass grows from one render to N (timer-verified the cost is paid where expected).
- **Reserved decision #4 — cascade count**: 2 (cheaper, fine for island scale) **vs** 3 (tighter
  near shadows). Recommendation: start at 2, measure, raise only if the timer and the eye agree
  it's worth the third render.

**Acceptance.** Near shadows (a baluster, a twig) are crisp while a whole island still shadows
correctly; no visible cascade seam at the splits; god-rays inherit the sharper map; shadow timer
reflects N renders; both backends agree.

### Item 7 — Shadow-casting point/spot lights (candle-light that casts)

**Intent.** The night-abbey look — sconces and lanterns that throw real shadows down the nave,
the Amnesia/Doom-3 image. The lights already exist as META; here they cast.

**Spec.**
- Cube shadow maps for point lights (or a single 2D map for a spot/`cutoff` light — **reserved
  decision #5 — which first**: spot-only v1, far cheaper, one 2D map, and a directed sconce is
  the dramatic case **vs** full omni cube maps). Recommendation: **spot first** — the dramatic
  light is directional, the cube map is the expensive generalization.
- A small fixed budget of *shadow-casting* lights (e.g. ≤2–4, the brightest/nearest), distinct
  from the 8-light shading cap; the rest light without casting. The selection is by the timer's
  permission, not by hope (§1.5).
- Reuses the shadow-map machinery and bias discipline from Item 6; the lantern's existing flicker
  (the component channel) now flickers a *cast* shadow.

**Acceptance.** A lit sconce in a dark church throws the pier shadows across the floor and they
move with the flicker; turning the caster off returns to today's un-cast lighting; the per-caster
shadow render shows on the timer; both backends agree; the 8-light shading path is unregressed.

### Item 8 — Reverb sends per room + spatial audio

**Intent.** The cathedral that *sounds* like one — and the phase's one item that dodges the
dual-backend tax entirely, because it lives in the audio quarantine, not the RHI seam.

**Spec.**
- **Spatial:** distance attenuation + a constant-power auto-pan from the listener (the camera)
  for located sources — a fountain, a bell, the lantern's crackle gains a *place* (the synth
  voices already carry position-adjacent data).
- **Reverb:** a Schroeder/FDN reverb in the mixer, with **per-room sends keyed off the existing
  room/KIND data** — a church reverberates long and bright, a small room is dry. The containment
  query that already drives the wind audio picks the room; the room picks the reverb preset
  (kinds-are-presets, the synth lesson, applied to acoustics).
- Pure C89 in `synth.c`/`mixer.c`; the SPSC ring and the audio worker are the only threading,
  already quarantined. **Zero audio binaries** — reverb is parameters, like every other sound.
- **Reserved decision #6 — reverb topology**: Freeverb-style Schroeder comb+allpass bank
  (simple, classic, cheap) **vs** a small FDN (richer tails, more knobs). Recommendation:
  Schroeder v1, introspectable for the future synth-book app; FDN flagged as the refinement.

**Acceptance.** Walking from open island air into the nave, the same footstep/voice audibly
gains a long bright tail; stepping out, it dries; a located source pans and attenuates with
distance and angle; `synthtest`/mixer suites green; presets Fran-approved by ear via `afplay`.

### Item 9 — The capstone: the night abbey, felt

**Intent.** The phase's portfolio shot and the proof that the items compose — atmosphere, cast
light, and sound in one scene.

**Spec.**
- The composer (extend Z): the abbey at dusk/night — god-rays through the clerestory under a low
  sun, candle-shadows from lit sconces down the nave, depth haze settling the island into the
  air, dust in the light shafts fading at the stone, AO grounding every pier, and the long bright
  reverb when you stand inside. A daylight counter-shot proves the fog/aerial/AO read in the
  open too.
- All budgets timer-verified: the dressed night-abbey frame holds interactive on the M2 in
  **both** backends (the phase's whole point is that the felt world is still 16.7 ms); each new
  pass within its stated slice.
- **`ATMOS.md`** (the index, GOTHIC.md / FLORA.md's sibling): the pass order, the no-TAA
  constitution, the depth-texture readers, the timer budgets, the per-item de-risking decisions,
  the keys.
- **Reserved decision #7 — a day/night lever**: leave dusk as an `hdr_reload` swap (today's
  manual path, no new system) **vs** a small sun-angle/IBL-blend control. Recommendation: swap
  only — a true day/night cycle is render-architecture (and brushes GI), deferred with it.

**Acceptance.** The screenshot: the abbey at night, light shafting through the glass, candle-
shadows on the floor, the island haze-settled in the air, and the nave's reverb in your ears —
and a daylight wild island proving the atmosphere stands without the church. Every suite green;
both backends; the phase closes.

---

## Part 4 — Decisions reserved to the project owner

1. **Selection & sequence of items** — the meta-decision. These nine, in this order
   (instrument → depth-cluster → shadow-cluster → sound → capstone)? Drop, add, or reorder
   before any code.
2. **Timer readback latency** (Item 1): N-frame-old stall-free vs forced sync. Rec: N-frame-old.
3. **God-ray resolution & upsample** (Item 3): half-res depth-aware vs quarter-res simple.
   Rec: half-res.
4. **AO normals source** (Item 5): reconstructed-from-depth vs a normals attachment in §1.4.
   Rec: reconstructed v1.
5. **Cascade count** (Item 6): 2 vs 3. Rec: 2, measure up.
6. **First casting light type** (Item 7): spot-only vs omni cube. Rec: spot first.
7. **Reverb topology** (Item 8): Schroeder vs FDN. Rec: Schroeder v1.
8. **Day/night lever** (Item 9): hdr_reload swap vs a sun/IBL blend control. Rec: swap only.
9. **Ordering vs Phase 5** — this phase before or after the app engine.

## Part 5 — Deliberately deferred (not this phase)

- **Global illumination / bounced light** — the marquee successor phase (§1.6): bake-on-mint-
  cache, SH irradiance probes, the static/dynamic split, a worker that resolves the light. This
  phase builds its instrument (timers) and its depth habits, nothing more.
- **TAA** — declined on principle (§1.3), permanently. Not deferred — refused.
- **SSR** — substituted by reflection probes + the planar pond; the general per-pixel march
  buys this content almost nothing.
- **OIT** — substituted by sorted-alpha derived from the plan (stained glass is placed and
  mostly planar; the draw order is computable, not a per-pixel solve).
- **Clustered / tiled forward** — not until Item 1's timers prove a real scene blows the 8-light
  cap (§1.5). Measure first; very possibly never.
- **Reflection probes & planar pond reflection** — strong, low-risk, and adjacent, but a
  reflections mini-phase of their own (the pond is a fresnel-IBL plane today; true mirroring is
  the next step there).
- **Decals, cloth (PBD banners), two-bone IK foot-planting, MSAA/FXAA** — all sound low-risk
  survey items, none in this phase's atmosphere/light/sound spine; each its own later item.
- **A true day/night cycle** — render-architecture that brushes GI; the `hdr_reload` swap is the
  v1 lever (Item 9, §reserved #7).
