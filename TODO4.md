# Solarium — Phase 4 Execution Brief (Items 1–10): The Engine Beneath

> **Numbering note.** This is **Phase 4**. Phase 3 (TODO3.md, items 1–10: the spatial
> knowledge environment — persistence hardening → terrain) is complete, every item
> owner-verified. The items below are numbered 1–10 *within this phase*; cite as
> "P4 · Item N" where ambiguous.

## How to use this document

You have the current engine source. What you do **not** have is the design reasoning
behind this phase. Phases 2–3 built an *application* — the memory palace — on top of
architectural **promises**: seams and disciplines paid for early and never fully cashed.
This phase cashes them. The RHI seam's promise is a second backend (item 10). The
retained-parametric-geometry pattern's promise is collision (item 1) and triangle-precise
picking (item 2). The HDR pipeline's promise is bloom (item 5). The parent-chain + slerp
math's promise is skeletal animation (item 9). The registry-as-schema pattern's promise is
that it generalizes — to assets (item 4) and to *behavior* (item 6).

The organizing thesis: **every item lands as engine capability, app-agnostic.** The palace
is this phase's *test scene*, not its customer. No item may bake palace concepts (cards,
mirrors, codices) into a subsystem below `main.c`; `main.c` composes. Phase 5 swings back
to application work (the gothic kit and the rest of the recorded directions) on a substrate
that pushes back, scales, glows, and sounds.

**Sequencing.** Items 1–3 are ordered by dependency (collision is the felt gap; the spatial
index serves three masters; instancing gates particles). Items 4–9 are loosely ordered and
mostly independent — audio (item 8) in particular touches nothing else and can interleave
anywhere a change of texture is welcome. Item 10 is **last by design**: every RHI addition
this phase is API the Metal backend must also implement, so Metal ports a *settled* surface.

**Part 1 (the Constitution) is binding on every item.** Each item states its **intent**,
its **spec**, an **interface sketch** where it helps, and **acceptance criteria**. Decisions
reserved to the project owner are flagged inline and collected in Part 4.

---

## Part 1 — The Constitution

### 1.1 What is already established (maintain it)

These are facts about the codebase you are extending. Keep them:

- **Strict C89**, `-std=c89 -pedantic-errors -Wall -Wextra`, clean, with the established
  forbidden-construct list and `sol_base.h` types. Verified by `./build.sh c89check`.
- **The RHI seam is inviolable.** `rhi_gl.c` is the only TU that touches GL. New GPU
  capability = extend `rhi.h`, implement in the backend. This phase *ends* by proving the
  seam with a second backend — which raises the seam's standards; see §1.4.
- **The quarantine pattern** for TUs that need non-C89 surfaces: `image.c` (stb_image),
  `font.c` (stb_truetype), `platform_fs.c` (POSIX) are compiled relaxed and excluded from
  `c89check`. This phase adds quarantined TUs (audio, Metal); follow the pattern exactly.
- **The scene spine is the substrate** — stable handles + nids, TRS, parent hierarchy,
  meta/relations/content slots, byte-identical STML round-trips. Extend it; never build a
  parallel structure.
- **Registry-as-schema**: parametric things declare named params + defaults in one table
  read by writer, loader, and resolver; schema growth is backward-compatible (absent params
  take defaults). This phase applies the same pattern to assets and components.
- **The permanence invariant (P3 §1.2) still binds.** A placed object's position is sacred;
  only explicit user action moves it. Item 6's animation doctrine (§1.6 below) exists to
  keep components from violating this.
- **The file stores arrangement and references, never geometry** — and never runtime pose
  (the reader-rig precedent: transient visual state lives in AppState and dies on landing).
- **Headless tests are the house proof style** (`stmltest`/`nidtest`/`iotest`/`camtest`/
  `picktest`/`jsontest`). Every pure-CPU core this phase adds gets one.
- **Zero dependencies** beyond GLFW and the sanctioned vendored stb headers. New
  *platform* surfaces (CoreAudio, Metal) are OS frameworks, not dependencies — they live
  behind seams like GL does.

### 1.2 NEW — Engine, not app (the rule of this phase)

Every subsystem this phase ships must be usable by *any* application built on this engine.
Concretely: collision knows shapes, not rooms-as-PKM; the asset registry knows names and
refcounts, not codices; components know update functions, not whiteboards. The test for
every design: "would this API make sense if the app were a game?" Palace-specific
composition happens in `main.c`, where it always has. This is not abstraction for its own
sake — it is what makes Phase 5's gothic kit (and anything after) land on capability
instead of special cases.

### 1.3 NEW — The world's shape has one author

The `terrain_height` / `BOOK_GUTTER_FRAC` lesson, elevated to law: **collision geometry,
render geometry, and spatial queries must derive from the same parametric source.** A
room's collision slabs come from the same `w,d,h` + wall flags the emitter reads; a
doorway is passable because collision derives from the same around-the-gap pieces the
renderer draws — the hole is real *by construction*, not by a hand-placed "no-collide
zone." Never hand-maintain a parallel collision shape. When geometry and physics disagree,
the bug is that there were two authors.

### 1.4 NEW — Every RHI addition is Metal debt

Item 10 implements the entire `rhi.h` surface a second time. Therefore, this phase:
additions to `rhi.h` are deliberate, minimal, and **expressible in Metal** (check the
mapping before committing the API — e.g. per-instance vertex data maps to Metal step
functions; uniform-by-name maps to a name→offset table). Prefer extending an existing
call's descriptor over adding a new entry point. Item 10 is the audit that makes this
real; a GL-ism that leaks into `rhi.h` between now and then is a bug *now*, not then.

### 1.5 NEW — OS surfaces get the quarantine pattern

Audio means CoreAudio (`AudioToolbox`/`AudioUnit` — a C API); it lives behind
`platform_audio.c`/`.h` exactly as dirent lives behind `platform_fs.c`. Metal means
Objective-C; `rhi_metal.m` is quarantined the way `rhi_gl.c` quarantines GL. **Threading
enters the engine for the first time in item 8** (the audio render callback runs on a
real-time thread) and it is confined *inside* the quarantine: the engine-facing API is
called from the main thread only; the quarantined TU owns all cross-thread communication.
No locks, threads, or atomics above the seam.

### 1.6 NEW — Animation never contaminates persisted state (the overlay doctrine)

Components (item 6) and animation clips (item 9) produce motion as a **function of time
applied on top of the persisted base TRS** — they never write the animated instantaneous
value back into the object's stored transform. Saving writes the *base*. A scene saved
mid-spin and reloaded resumes spinning from the base pose, not from a baked frame. This is
the permanence invariant's corollary: the file records what the *user* placed, and what
behavior is *attached* — never a snapshot of the dance.

### 1.7 NEW — Measure before optimizing

Frame instrumentation (CPU frame time, per-pass breakdown, draw/instance/cull counts in
the debug HUD) lands with item 2, *before* the culling and instancing claims it exists to
verify. Performance work in this phase is demonstrated in the HUD, not asserted. (GPU
timer queries on macOS GL are historically unreliable; CPU timings + draw counts are the
honest metrics, and the Metal backend can revisit GPU timing properly.)

### 1.8 Definition of done (every item)

Builds clean under `-std=c89 -pedantic-errors -Wall -Wextra` (quarantined TUs excepted as
documented); runs clean under ASan+UBSan; no GL symbol above the seam; persisted state
round-trips byte-identically; new pure-CPU cores have headless tests; the stated checkpoint
demonstrably passes; no new dependency beyond those approved in Part 4.

---

## Part 2 — Item 1: Collision & the character controller (gate; do first)

**Intent.** The loudest engine-shaped hole: the world has ground (the P3 item-10 seam) but
no sides — you can walk through every wall. Movement that the world honestly resists is the
foundation everything else this phase *feels* through (footsteps that stop when you stop,
a character that can't phase through masonry), and Phase 5's gothic kit is architecture —
walls that don't work would make it stage dressing. Do this first; it touches no other
item's territory and pays off every session thereafter.

**Spec.**
- **A collision layer in a new pure-CPU TU** (`collide.c`/`.h`, strict C89, in `c89check`,
  headless-testable). The world's *static* colliders are **derived from the scene's
  parametric refs** (§1.3): a `room` contributes a slab per present wall (+ ceiling, for
  fly mode) computed from the same `w,d,h` + flags the emitter reads; a `wall` (with
  opening) contributes a box per around-the-gap piece — so the doorway is passable by
  construction; a `path` contributes its deck box; `terrain` contributes its existing
  height function (already the vertical authority). Rebuild the collider set on
  load/edit — the `arrows_rebuild` pattern: derived data, never serialized.
- **Props do not obstruct by default** (cards, books, notes are decoration). Whether large
  furniture (boards) should obstruct is a reserved decision; default no.
- **The mover: move-and-slide.** The player is a vertical **capsule** (recommended over a
  sphere — a sphere either floats you up walls or fits through nothing; radius ~0.30 m,
  total height matching `CAMERA_EYE_HEIGHT` + headroom). Per frame: attempt the desired
  lateral move; for each penetrated collider, project the remaining motion onto the
  contact plane and re-test (≤ 3 iterations — the classic slide loop). Sliding along a
  wall toward a doorway carries you *through* the opening smoothly.
- **Vertical stays the ground seam's job.** `ground_under` + the settle glide remain the
  height authority; collision adds the lateral resolve before the settle. Reuse the
  existing +0.6 step-up constant — thresholds and (future) stairs pass through the same
  tolerance, one constant, two consumers (§1.3 in miniature).
- **Walk collides; fly is a reserved decision** (recommend: fly collides too, with a
  debug ghost toggle — building/inspection wants to pass through things on purpose, but
  flight that respects architecture preserves the inhabited feeling).
- **Headless test (`coltest`):** slide along a wall preserves tangential speed; a corner
  stops you dead; a doorway-width gap admits the capsule; a sub-step-up ledge is climbed,
  an over-step-up wall is not; the same room params that build the mesh build the
  colliders (assert agreement at sampled points).

**Interface sketch.**
```c
void collide_rebuild(ColliderSet *cs, const Scene *s);       /* derived, like arrows */
vec3 collide_slide(const ColliderSet *cs, vec3 pos, vec3 move, float radius, float height);
/* camera integration: lateral move runs through collide_slide before the y settle */
```

**Acceptance.** You cannot walk through any wall of any room; every doorway admits you;
sliding along a wall is smooth (no sticking, no jitter); the threshold step-up still works;
island rims still feather-fall (no regression — same glide, same constants); `coltest`
passes; ASan clean.

**Decisions flagged:** capsule vs sphere (capsule recommended); fly-mode collision
(collide + ghost toggle recommended); whether boards/furniture obstruct (default no).

---

## Part 3 — Items 2–10

### Item 2 — The spatial index (cull, pick, and the triangle-precise debt)

**Intent.** Every frame draws every object; every pick linearly scans every AABB; picking
through a doorway grabs the wall (the AABB is solid). One structure pays all three debts:
frustum culling, pick acceleration, and **triangle-precise picking** — the longest-deferred
item in the project, nearly free once geometry is retained. Plus the instrumentation
(§1.7) that makes the wins demonstrable.

**Spec.**
- **Instrumentation first**: CPU frame ms (smoothed), per-pass ms, and drawn/total object
  counts in the debug HUD via the existing mono readout. This is the yardstick for
  everything below.
- **Retained CPU geometry.** Parametric meshes are rebuilt from emitters at every load —
  keep the positions + indices (a `CpuGeom` on the mesh or alongside it) instead of
  discarding after upload. glb-derived parts retain theirs at import. Memory cost is
  modest at palace scale; this is also what item 1 may consult for exact-shape cases and
  what makes triangle picking possible at all.
- **The structure**: a BVH over object world-AABBs (recommended over a uniform grid — the
  palace's density is wildly nonuniform: a packed room, then a void, then an island).
  Median-split build is sufficient; **refit** (recompute node bounds bottom-up) on drag
  rather than rebuild; rebuild on load. Object count is thousands at most — simplicity
  over asymptotics.
- **Frustum culling**: extract the 6 planes from the view-proj matrix (Gribb–Hartmann),
  test BVH nodes, draw only intersecting leaves. The skybox, UI, and reader rig are
  exempt by construction. HUD shows drawn/total.
- **Pick acceleration + precision**: `scene_pick` traverses the BVH; leaf candidates run
  **ray-vs-triangle** (Möller–Trumbore, in `sol_math.c` with the other intersectors)
  against retained geometry, nearest triangle hit wins. The "can't pick what you're
  inside" AABB rule retires — interior wall faces are honestly hittable now (their
  normals face you). Whether room-shell children become *selectable* (useful later for
  kit editing) or stay pick-transparent is a reserved decision; default pick-transparent
  so empty-room clicks still mean "deselect."
- **Headless test**: BVH query equivalence vs brute force (random rays over a generated
  scene — identical nearest hits); frustum plane extraction against known matrices;
  Möller–Trumbore hit/miss/edge cases.

**Acceptance.** The HUD shows frame timings and cull counts; facing away from the palace
demonstrably drops draws; **clicking through a doorway selects the object beyond it** (the
acceptance moment of the item); pick results match brute force in tests; drags and reloads
regress nothing.

**Decisions flagged:** BVH vs grid (BVH recommended); wall selectability (pick-transparent
default).

---

### Item 3 — Instancing

**Intent.** One draw, N transforms — the canonical engine RHI feature. Particles (item 7)
require it; Phase 5's arcades and balustrades will quietly thank it; and it is the first
real test of §1.4 (design RHI surface Metal-first).

**Spec.**
- **RHI surface**: per-instance vertex attributes + an instanced draw. The pipeline
  descriptor's attribute entries gain a **step rate** (per-vertex / per-instance — exactly
  Metal's `MTLVertexStepFunctionPerInstance`, implemented in GL as
  `glVertexAttribDivisor`), and `rhi_draw_indexed` gains an instanced sibling
  (`rhi_draw_indexed_instanced(count, instances)`). **Audit every existing pipeline-desc
  site when the descriptor grows** — the established garbage-field defense: set the new
  field explicitly everywhere (`= {0}` init is already universal; verify).
- **Per-instance layout**: recommend compact — position (3) + scale (1) + color (4) for
  particles and scatter; a full per-instance mat4 (4×vec4 attributes) is the general
  variant, added only when something needs rotation per instance. Reserved decision.
- **Scope discipline**: scene objects stay one-draw-each (they are unique; batching the
  scene graph is not this item). Instancing is for genuinely repeated geometry: particles,
  and a demo scatter (a few thousand tufts/stones on an island) to prove the path.
- The instance buffer is a stream: `rhi_update_buffer` (exists, orphaning) refilled per
  frame for dynamic users (particles), static for scatter.

**Acceptance.** ~10k instanced quads/blades render at interactive rates as **one draw**
(HUD draw count proves it); the same scene without instancing visibly costs more draws;
ASan clean; every pipeline-desc site sets the new field explicitly.

**Decisions flagged:** per-instance layout (compact recommended, mat4 variant deferred
until needed).

---

### Item 4 — The asset registry (ownership, dedup, hot reload)

**Intent.** Three recorded debts come due: duplicate mesh uploads are tolerated ("correct
but wasteful"), `L`-reload deliberately leaks GPU buffers ("registry owns it later" —
this is later), and textures have no dedup. Name → asset with refcounts closes all three,
and hot reload falls out nearly free because `platform_fs` already stats mtimes.

**Spec.**
- **A registry TU** (`asset.c`/`.h`, strict C89): keyed stores with refcounts.
  - **Parametric meshes**: key = ref name + canonical param string (the params *are* the
    identity — two rooms with identical params share one GPU mesh; the codex's "params
    persist" doctrine already made identity explicit).
  - **glb parts**: key = path + part index. **Textures**: key = path + colorspace flag
    (the sRGB-vs-linear distinction is part of identity — the same file as albedo and as
    data are *different assets*).
  - `asset_acquire_*` / `asset_release` API; `mesh_destroy`/`rhi_destroy_texture` fire at
    refcount zero. **Scene-derived meshes (arrows) stay outside** — per-object, rebuilt,
    already owned; document the boundary.
- **`L`-reload and scene transitions** become release-all → re-acquire; the deliberate
  leak retires. (macOS ASan has no leak detection — the HUD's live buffer/texture counts
  are the observable metric; steady state across many reloads is the proof.)
- **Hot reload**: per-asset mtime checks, throttled (every ~0.5 s, a handful of stats per
  frame round-robin — not a thundering stat herd), re-decode + re-upload in place on
  change (same handle, new contents — holders don't re-resolve). Scope: textures + the
  `.hdr` + glbs. Shaders are inline C strings, naturally out of scope. Trigger (polled vs
  a key) is a reserved decision; recommend polled-with-throttle — the magic is editing a
  texture in another app and watching the palace change.
- HUD gains live asset counts (meshes/textures, refs held).

**Acceptance.** Two same-param rooms share one mesh (HUD buffer count proves it); many
consecutive `L`-reloads hold GPU counts flat; editing a texture file on disk updates
in-app within a second without restart; releasing the last ref demonstrably frees (counts
drop); ASan clean.

**Decisions flagged:** hot-reload trigger (polled recommended); reload scope (textures +
hdr + glb recommended).

---

### Item 5 — Lights and glow (point lights + bloom)

**Intent.** One spot light + IBL since Phase 2. Candles in sconces — the gothic kit's
lighting story — want point lights; and bloom is the HDR pipeline's most visible unpaid
dividend: the engine has computed brightness > 1.0 for two phases and never let it glow.
Paired deliberately: candles, then candlelight.

**Spec.**
- **Lights are scene objects**: a light is data on an object (recommend meta-based
  vocabulary like `room_type` — e.g. `light=point` + `light_color`/`light_intensity`/
  `light_radius` metas, or a dedicated `<light>` element; the STML form is a reserved
  decision). Lights therefore parent, persist, and **drag** — a carryable lantern is the
  demo that proves the design. Renderer collects up to **N point lights** per frame
  (N = 8 recommended; nearest-to-camera wins beyond that) into uniform arrays.
- **The PBR loop** adds the point-light sum to the existing direct term — same BRDF as the
  spot, attenuation by **windowed inverse-square** (physical falloff, smooth cutoff at
  `light_radius`, no hard pop at the boundary). The spot light and IBL are untouched.
- **Point lights cast no shadows this phase** — omnidirectional shadows are six faces per
  light and are explicitly deferred. The one spot keeps its map. State this in the HUD
  docs/comments so nobody chases a "missing shadow" bug.
- **Emissive** joins the material model: an emissive color/strength factor on `Material`
  + `<mat>` (schema-compatible growth — absent = 0, old files unchanged). Without
  emitters, bloom only catches specular glints; a candle flame *is* an emissive surface.
- **Bloom**: post-chain on the existing HDR target before tonemap — threshold/extract,
  progressive half-res downsample (~5 levels, bilinear), blur on the upsample walk back,
  additive combine at low strength. All existing RHI machinery (render targets, fullscreen
  passes) suffices; no new surface expected. A debug toggle key for A/B.
- Flicker (a candle's life) arrives via item 6's `flicker` component — note the
  composition, don't build it here.

**Acceptance.** Several colored point lights illuminate a room with believable falloff and
no radius pop; a lantern object lights its surroundings *while being carried*; an emissive
flame quad blooms convincingly; the bloom toggle shows stable exposure (no
double-brightening, no flicker); scenes saved before this item load unchanged; ASan clean.

**Decisions flagged:** the light STML vocabulary (meta-based vs element); N max lights
(8 recommended); emissive in the material model (recommended yes).

---

### Item 6 — The component model (behavior without a language)

**Intent.** Everything that moves is hardcoded in `update()` — the tumbling box, the sword
precession, bound by name at load. A general engine needs *attachable* behavior. This is
the registry-as-schema pattern applied to behavior: components declare named params +
defaults in a table; the scene file attaches them by name. **Explicitly not a scripting
language** — the interpreter is deferred to its own future phase (Part 5); this item
covers the engine need (attachable, persistent, data-driven behavior) and teaches the
architecture lesson at a fraction of the cost.

**Spec.**
- **A component registry** (`component.c`/`.h` or within scene's orbit, strict C89):
  entries declare a type name, a param schema (names + defaults — the mesh-registry table
  shape exactly), an update function pointer, and optional per-instance runtime state
  size. `SceneObject` gains a small components array (the rels pattern: grow-by-realloc,
  owned strings/params).
- **Serialization**: `<component type="spin" axis="y" speed="0.8"/>` — named attrs against
  the schema, absent params take defaults, byte-identical round-trip, unknown types
  preserved-and-ignored (forward compatibility, the format's standing rule).
- **The overlay doctrine (§1.6) is the item's design center**: a component's update
  computes motion **from the base TRS + time** and contributes an overlay applied at
  world-matrix build, never writing back into persisted `pos`/`rot`/`scale`. Saving
  mid-animation writes the base; reload resumes the motion, not a baked frame. Dragging an
  animated object moves its *base*; the overlay rides along.
- **First components** (each small, each proving a channel): `spin` (rotation overlay —
  the box tumble, migrated), `orbit` (the sword precession, migrated — retiring those
  hardcoded `update()` blocks and most of `bind_runtime_handles`' reason to exist),
  `bob` (position overlay), `flicker` (animates a *light's* intensity — item 5's candles
  come alive; proves components can drive non-transform channels), and item 7's emitter.
- **Update pass**: iterate objects' components once per frame with
  `(scene, handle, params, state, t, dt)`; components are engine-level (no palace
  concepts, §1.2).

**Interface sketch.**
```c
void component_register(const char *type, const CompSchema *schema, CompUpdateFn fn);
void scene_component_add(Scene *s, sol_u32 h, const char *type, const float *params, int n);
void components_update(Scene *s, float t, float dt);     /* overlays, never write-back */
```

**Acceptance.** The box tumbles via a `spin` component declared in scene.stml — the
hardcoded animation code is *gone*; save→load→save is byte-identical with components
attached; deleting the component stops the motion; saving mid-spin and reloading resumes
from the base pose (the overlay doctrine, demonstrated); `flicker` makes a candle live;
unknown component types in a file survive a round-trip untouched.

**Decisions flagged:** the component STML vocabulary (the `<component>` element form);
confirmation of the overlay doctrine; which channels overlays may drive in v1 (transform +
light recommended).

---

### Item 7 — Particles

**Intent.** Instancing's first real customer, and the atmosphere item: dust motes in the
light shafts, sparks above a candle, mist off an island rim. Cheap, visible, and entirely
composed of parts this phase already built (instancing, components, bloom).

**Spec.**
- **Emitters are components** (`emit` with schema params: rate, lifetime, velocity +
  spread, size range, color/alpha curve endpoints, drift/gravity). The emitter's object
  gives position/parenting for free — a candle's spark emitter parents to the candle.
- **Particles are runtime-only** (the reader-rig doctrine: view state, never scene state;
  nothing serializes). A pooled buffer (fixed cap, recycle oldest) in a `particles.c` TU;
  CPU simulation in C89: integrate position/velocity, age, fade — simple Euler is fine.
- **Render**: camera-facing quads via the instanced path (item 3's compact per-instance
  layout), drawn in the **HDR pass** after opaques — so bloom catches sparks — with
  depth-test ON, depth-write OFF. **Additive blending recommended** for v1 (dust, sparks,
  mist all read well additive, and additive needs no sorting); sorted alpha is the
  deferred variant. A small procedural soft-disc texture (generated at init — no new
  asset) serves all of v1.
- **Budget honesty**: particle sim cost appears in the HUD's per-pass timings (§1.7).

**Acceptance.** A dust emitter fills a room's light shaft with drifting motes that fade in
and out, thousands at trivial cost, one draw (HUD proves both); a candle's sparks glow via
bloom; emitters round-trip as components and resume on reload; no depth artifacts against
geometry; ASan clean (the pool neither grows nor leaks).

**Decisions flagged:** additive-only v1 (recommended) vs sorted alpha; pool cap.

---

### Item 8 — Audio

**Intent.** Total silence is the most un-engine thing about solarium. The from-scratch
version is pleasingly shaped like the project's founding story: a platform seam over
CoreAudio (the quarantine pattern), a hand-rolled WAV parser (pure C89, headless-tested),
and a small 3D mixer. Footsteps on stone, the page-turn whisper, wind on the islands —
enormous atmosphere per line of code. Fully independent of every other item; schedule it
wherever a change of texture is welcome.

**Spec.**
- **`platform_audio.c`/`.h` — the fifth quarantine.** CoreAudio's C API (AudioToolbox:
  default-output AudioUnit + render callback, 48 kHz stereo float). The render callback
  runs on a real-time thread; **all threading is confined inside this TU** (§1.5):
  the engine-facing API (play, set listener, set voice params, stop) is main-thread-only
  and communicates with the callback via a lock-free single-producer/single-consumer
  command ring (C11 atomics are fine *inside* the quarantine, like stb headers are).
  The callback never allocates, never locks, never touches engine state directly.
- **The mixer core is pure C89 and headless-tested** (`mixtest`): voice advance/loop/end,
  gain application, constant-power stereo pan, windowed inverse-square distance
  attenuation (the *same falloff shape* as item 5's lights — one perceptual law, two
  senses), final mix-and-clip. The quarantined callback calls this pure core.
- **WAV parsing, hand-rolled** (`wav.c` or within the audio TU's C89 side): RIFF chunk
  walk, PCM16 + float32, mono/stereo, resample-on-load to the mixer rate if needed
  (linear interpolation is sufficient). Headless-tested against tiny synthesized fixtures.
  Compressed formats (Vorbis/MP3) are a reserved decision — recommend **WAV-only this
  phase** (sounds are short; music/streaming is a later concern, and stb_vorbis would be
  a *third* stb to sanction).
- **Sounds are assets** through item 4's registry (path → decoded buffer, refcounted).
- **3D voices**: position from a scene object (a parented voice follows its object) or
  fixed; listener = the camera (position + forward for pan). A `sound_loop` component
  (item 6) attaches ambient loops to objects — a candle's crackle, wind at an island's
  anchor.
- **First wirings in `main.c`** (composition, not engine): footsteps from walk-distance
  accumulation (stride length ÷ speed; silent when still — item 1 makes "moving" honest);
  the page-turn whoosh on leaf launch; the book's thump on landing; wind outdoors,
  crossfaded by the existing containment query when you step inside.
- **Asset sourcing is a reserved decision**: CC0 packs (freesound et al. — assets are not
  code; the `.hdr` precedent applies) vs synthesizing placeholders. Recommend CC0.

**Acceptance.** Footsteps track actual walking and stop when you stop; pages whoosh and
books thump; wind plays outdoors and crossfades away indoors (containment-driven); walking
past a placed source pans and attenuates audibly and smoothly; no glitches, pops, or
underruns in normal play; `mixtest` + WAV tests pass; ASan clean; no thread primitives
above the seam.

**Decisions flagged:** WAV-only vs stb_vorbis (WAV-only recommended); asset sourcing (CC0
recommended); footstep surface-awareness scope (single stone sound recommended for v1).

---

### Item 9 — Skeletal animation

**Intent.** The classic engine subsystem, and the most "promise cashed" item of the phase:
the glb parser is hand-rolled and ours to extend, `quat_slerp` was built and tested for
item 9 of *last* phase, and joint hierarchies are the parent-chain math the scene has used
since 2.3. After this, a character can walk through the palace — whatever Phase 5+ decides
that means.

**Spec.**
- **glb parser growth**: parse `skins` (joint node lists, `inverseBindMatrices` accessor)
  and `animations` (channels/samplers: translation/rotation/scale targets; LINEAR and STEP
  interpolation; CUBICSPLINE deferred). The existing accessor machinery carries most of
  this; it is parser surface, not new concepts.
- **The skeleton is asset data, not scene objects** (recommended, reserved): a compact
  runtime struct — joints as arrays (parent index, local TRS, inverse bind), owned by the
  imported asset via item 4's registry. The *math* reuses `sol_math`'s TRS/quat functions;
  the scene graph is not polluted with armature nodes, and the persistence doctrine stays
  clean (a rig is geometry-adjacent data, and the file stores arrangement, never
  geometry). The alternative — joints as SceneObjects — buys hierarchy reuse at the cost
  of serialization questions; the brief recommends against.
- **Sampling**: clip time → per-channel keyframe pair → lerp/slerp → joint local TRS →
  world joint matrices (one parent-chain walk over the compact arrays) → skinning palette
  (joint world × inverse bind). Pure CPU, strict C89, **headless-tested** (`animtest`:
  interpolation at known times, a two-bone chain against hand-computed poses, STEP vs
  LINEAR).
- **GPU skinning**: a *second* vertex layout for skinned meshes (the canonical 12 floats
  + joints ×4 + weights ×4) and a skinned vertex-shader variant applying a **matrix
  palette** uniform array. Joint cap 64 (fits comfortably in GL 3.3's guaranteed uniform
  budget at mat4×64; check the real limit at init and fail loudly). New RHI surface:
  a mat4-array uniform setter — design Metal-first per §1.4 (it maps to a buffer bind).
- **The animator is a component** (item 6): `play` with clip name, speed, loop — clip
  *names* persist, pose is runtime (§1.6: a save mid-stride stores which clip plays, never
  the bent knee).
- **Test asset**: a standard rigged glTF sample (e.g. the Khronos Fox — CC-BY, an asset
  not a dependency) or a Blender-rigged export; committed like suzanne/sword.

**Acceptance.** A rigged character glb plays its walk/idle clips in the palace, lit and
shadowed like everything else; two instances play the same clip at different phases
(independent pose state, shared asset — item 4 proves its sharing here); the animator
component round-trips by clip name and resumes after reload; `animtest` passes; switching
clips doesn't pop (a short crossfade is the stretch goal, flagged not required).

**Decisions flagged:** skeleton as compact asset struct (recommended) vs scene objects;
joint cap (64 recommended); crossfade in scope (stretch).

---

### Item 10 — The Metal backend (the capstone)

**Intent.** The RHI seam's entire reason for existing, waiting since step 6 of TODO.md.
macOS GL is deprecated, capped at 4.1, and carries the documented vsync-cadence stutter we
accepted as a platform artifact — the artifact dies here. More than any feature, this item
is the *architectural graduation*: the proof that two phases of "no GL above the seam"
discipline bought a real, swappable backend. It is last **by design**: it ports the
surface the rest of the phase settled (§1.4).

**Spec.**
- **`rhi_metal.m`** — Objective-C, quarantined exactly as `rhi_gl.c` quarantines GL; links
  the Metal + QuartzCore frameworks. **Backend selection is compile-time** (the step-6
  plan): `build.sh metal` links `rhi_metal.m` instead of `rhi_gl.c`; one backend per
  binary; `rhi.h` is untouched by the switch — that untouchedness *is* the proof.
- **Windowing**: GLFW with `GLFW_NO_API` (no GL context), a `CAMetalLayer` installed on
  the Cocoa content view. The existing `rhi_configure_window`/`rhi_init` lifecycle was
  designed for exactly this split; the GL-specific window hints move behind it.
- **Shaders: hand-written MSL twins** (recommended; a GLSL→MSL translation step is out
  under zero-deps). The program inventory is bounded (~a dozen: object/PBR, post, shadow,
  skybox + the three IBL bakes, BRDF LUT, UI, SDF text, wtext, instanced/skinned
  variants) and each is small. Uniform-by-name maps to a per-shader name→offset table
  into an argument buffer — hand-maintained, like the GL uniform lookups already are.
- **Land it in stages, each verifiable**: (a) cleared window via Metal; (b) the unlit
  mesh path (buffers, pipelines, depth); (c) render targets + the post chain; (d) full
  PBR + shadows + IBL bakes; (e) UI/text/instancing/skinning parity. The GL backend
  remains the daily driver until (e); the build switch keeps both honest.
- **Parity bar**: the same scene renders visually identically (allow LSB-level numeric
  differences); every feature and key works; performance is at-or-better; **the vsync
  stutter is measurably gone** (the HUD frame graph goes flat — the payoff line).
- **sRGB**: keep the manual `pow(1/2.2)` encode + a non-sRGB drawable for exact parity
  with the GL path (reserved decision; revisit native sRGB drawables after parity).
- The §1.4 audit happens here in practice: any rhi.h entry that proves awkward to express
  in Metal gets redesigned *now*, in both backends, rather than worked around.

**Acceptance.** `build.sh metal` produces a binary that renders the palace visually
identical to the GL build — every pass, every feature, every key; the teardown selftest
and resource-table behavior hold; frame pacing is demonstrably smoother than GL (the
stutter retired); no Metal/Obj-C symbol outside `rhi_metal.m`; the GL build still passes
everything (the seam swaps, nothing above it noticed).

**Decisions flagged:** MSL twins (recommended) vs translation; staged landing plan
approval; sRGB drawable choice (manual-encode parity first recommended).

---

## Part 4 — Decisions reserved to the project owner

Set these before delegating; an agent guessing them is the real risk.

1. **Item 1:** capsule vs sphere (capsule recommended); fly-mode collision (collide +
   ghost toggle recommended); do boards/furniture obstruct (no recommended).
2. **Item 2:** BVH vs uniform grid (BVH recommended); are room-shell walls selectable or
   pick-transparent (transparent recommended for now).
3. **Item 3:** per-instance data layout (compact pos+scale+color recommended; mat4 variant
   deferred until something needs per-instance rotation).
4. **Item 4:** hot-reload trigger (throttled polling recommended) and scope (textures +
   hdr + glb recommended).
5. **Item 5:** light vocabulary in STML (meta-based vs a `<light>` element); max
   simultaneous point lights (8 recommended); emissive material factor (yes recommended).
6. **Item 6:** the `<component>` STML form; the overlay doctrine (§1.6) confirmation;
   which channels overlays may drive in v1 (transform + light recommended).
7. **Item 7:** additive-only particles in v1 (recommended) vs sorted alpha; pool cap.
8. **Item 8:** WAV-only vs sanctioning stb_vorbis as a third stb (WAV-only recommended);
   sound asset sourcing (CC0 recommended); footstep surface awareness (single stone sound
   recommended).
9. **Item 9:** skeleton as compact asset data (recommended) vs scene objects; joint cap
   (64); clip crossfade in scope (stretch).
10. **Item 10:** hand-written MSL twins (recommended) vs a translation step; the staged
    landing plan; sRGB drawable strategy (manual-encode parity recommended first).

---

## Part 5 — Deliberately deferred (not this phase)

- **The scripting language / interpreter.** Item 6's component model covers attachable
  behavior; a hand-rolled VM is a phase-sized project wearing an item's name tag, and it
  deserves its own phase if wanted.
- **Rigid-body dynamics.** Item 1 is collision *queries and response* (the world pushes
  back), not physics simulation (stacking, tumbling, joints). A different, much larger
  subsystem.
- **Omnidirectional (point-light) shadows** — six faces per light; the spot keeps the
  only shadow map this phase.
- **Threading beyond the audio callback's quarantine** — no job system, no parallel
  render prep. The engine stays single-threaded above the seams.
- **Networking** — nothing here wants it yet.
- **MSAA / TAA / SSAO / GI** — renderer polish beyond bloom waits; bloom was chosen
  because the HDR pipeline already paid for it.
- **Soft particles, sorted-alpha particles, particle collision** — v1 is additive and
  collision-free.
- **CUBICSPLINE animation channels, morph targets, animation blending trees** — LINEAR/
  STEP and a single playing clip (+ optional crossfade) is v1.
- **Music / streaming audio / compressed formats** — short WAV sounds only (unless
  Part 4 #8 decides otherwise).
- **Cross-platform windowing/GL-loader, second OS targets** — the Metal backend is the
  second *backend*, not a second *platform*; Windows/Linux remain future.
- **All application work** — the gothic kit, wayfinding, book-binds-a-file, per-room
  files, HarfBuzz complex scripts: Phase 5 and beyond, on the engine this phase builds.

The cross-cutting disciplines of *this* phase — one author for the world's shape (§1.3),
RHI-additions-as-Metal-debt (§1.4), threading confined to quarantine (§1.5), the overlay
doctrine (§1.6), measure-before-optimizing (§1.7) — are pinned in the Constitution
precisely because they are painful to retrofit, which is the same design discipline the
project has applied to itself since Phase 2.
