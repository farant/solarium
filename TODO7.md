# Solarium — Phase 7 Execution Brief (Items 1–10): The Living Island

> **Numbering & status note.** TODO5.md (the app engine) still waits at its boundary;
> this brief is **Phase 7** by number — flora, rock, water, and wind for the islands —
> and the execution order between it and Phase 5 is the project owner's call (the gothic
> kit already jumped that queue once, profitably). Seeded 2026-06-12, the week Phase 6
> closed, to capture the design while three of its enabling investments are hot. Items
> are numbered 1–10 *within this phase*; cite as "P7 · Item N".

## How to use this document

You have the engine, and now you have the *town*: Phase 6 proved that one deterministic
plan function can grow a structure every subsystem reads — geometry, colliders, ruin,
ground truth — and Phase 6's tail paid the instanced-ornament debt, which means the
engine can draw ten thousand copies of a small mesh through the full PBR + shadow path
for one draw call apiece. The texture side-quest made surfaces synthesizable from knobs.
The meadow has been scattering instanced grass by island seed since Phase 4.

This phase is those four investments aimed at the thing islands obviously lack: **life**.
Trees first — they are the high-reward centerpiece and the hardest discipline test —
then rock, undergrowth, water, and one unifying wind.

The organizing thesis, inherited and unbent: **a tree is a function.** `tree_plan()`
expands `(species, seed, age)` into a **branch graph** — nodes, segments, radii, twig
tips — and everything else is a reader of it: the wood emitter sweeps its segments, the
canopy instances its tips, the collider boxes its trunk, the wind sways what it lists as
swayable. Same seed, same tree, forever: the codex-mint pattern at organism scale.

The second thesis: **populations are data, not objects.** A forest is not five hundred
scene objects; it is a few shared variant meshes and a few instance buffers derived from
the island's seed — the meadow's law promoted from grass to canopy. Only HERO plants
(the churchyard yew, the orchard pair you placed by hand) are scene vocabulary.

**Sequencing.** Item 1 (taper on the sweep) is the gate — every branch rides it. Items
2–4 build the tree inward-out (plan → wood → leaves); 5 scales it to forests; 6–7 are
the cheap companions (rock, undergrowth) that make the scatter read as an ecosystem;
8 is water, the phase's one renderer feature; 9 unifies wind across every swaying
system; 10 is the capstone island and the docs.

**Part 1 is binding on every item.** Each item states **intent**, **spec**, and
**acceptance**. Reserved decisions are flagged inline and collected in Part 4.

---

## Part 1 — The Constitution

### 1.1 What is already established (maintain it)

- **Strict C89**, the forbidden-construct list, `sol_base.h`, `./build.sh c89check`.
  All plan/emitter work in this phase is pure CPU; only Item 8 (water) and the leaf
  shader variant touch the RHI/shader layer, and those carry Metal twins per the
  stage-e conventions.
- **The MeshBuilder** (12-float canonical layout, tangents at upload) and
  **params-are-identity** registry refs (acquire-first swap, release-before-params-
  change).
- **`gothic_sweep`** — miters, auto-crease, closed loops, the two-cap tessellation —
  is the proven extrusion engine this phase extends (Item 1) and rides.
- **The instanced-ornament path** (P6 item 10): per-instance `(pos, yaw, sxz, sy)`
  over a unit mesh, full FRAGMENT_SRC, shadow twins, per-carcass local-slot buffers,
  fingerprint sync. The canopy is its second client; generalizing it is Item 4's job.
- **texgen** (the texture side-quest): the height field is the one author; kinds are
  presets over one knob schema. Bark and leaf-color ramps land as new kinds/knobs —
  **zero image binaries, still**.
- **The meadow** (P4 item 3): derived-never-serialized scatter from island seed,
  slope/height gates, one draw per island. The FIELD tier (§1.3) is this law, named.
- **The step treaty** (`COLLIDE_STEP_UP`), `ground_under` + `collide_stand`,
  `mint_ground` — anything standable obeys them.
- **Bit-determinism** (`memcmp`) and the suite-per-item discipline; the audit where
  coplanar faces can sin.

### 1.2 NEW — The plan is the one author (organism edition)

`tree_plan(species, seed, age)` is a pure, allocation-bounded expansion into a flat
branch-graph array (parent index, endpoints, radii, depth, tip flag). No stored tree,
no second description. The wood emitter, the canopy filler, the collider, the wind
table, the scatter variant builder — every one a READER. Knobs that change the graph
change the identity; knobs that only re-dress it (leaf tint) do not.

### 1.3 NEW — Two tiers: HERO and FIELD

- **HERO** plants/rocks are registry vocabulary: scene objects with refs, params,
  colliders, pickable, draggable, persisted. The churchyard yew is a HERO.
- **FIELD** populations are DERIVED data on the meadow/arrows law: rebuilt from the
  island seed at load/mint, never serialized, drawn instanced, sharing a small set of
  variant meshes through the registry. A forest is FIELD. The file never grows because
  the island is wooded.

### 1.4 NEW — Species are presets (the synth lesson, fourth verse)

ONE shared knob schema for all woody plants (height, splits, spread, droop, twist,
taper, leaf size/density, color lean, …). A species is a default vector; a shrub is a
species whose trunk knob is ~0 (plaster's lesson). The schema stays introspectable —
a future grimoire-of-plants book app reads it for free.

### 1.5 NEW — Lanes again, own noise twin

`flora_hash01(seed, lane, i, j)` with an APPEND-ONLY lane enum, and flora owns its
hash twin (the gothic ruling): no other module's change may silently regrow a forest.

### 1.6 NEW — Instancing first

If a thing appears more than a handful of times, it is an instance population, not a
mesh emission: leaves, FIELD trunks, pebbles, flowers. The budget law: an island fully
dressed stays interactive — each item states its triangle/draw budget and the HUD
verifies it.

### 1.7 Definition of done (every item)

Suites green (`floratest` joins as the 15th), c89check, all three app builds, both
backends visually agreed where the item draws, memcmp determinism for every new ref,
Fran-verified live, committed, memory record.

---

## Part 2 — Item 1: Taper on the sweep (gate; do first)

**Intent.** Branches taper; the sweep cannot. Add per-station scale, prove the old
callers byte-identical, and settle the sweep's home.

**Spec.**
- `gothic_sweep_tapered(b, prof, prof_n, path, path_n, plane_n, scales[], cap0, cap1)`
  — `scales[path_n]` multiplies the profile per station (miter math unchanged: the
  stretch applies to the ring as today, scale applied before it). A NULL or constant
  array must reproduce `gothic_sweep` exactly (memcmp — the suite proves it).
- Section normals account for the taper's slope (the cone correction: a tapering
  cylinder's normal leans; for the gentle tapers trees use, the per-segment correction
  is one term).
- **Reserved decision #1 — the sweep's home**: extract sweep + two-cap tessellation
  into `sweep.c` (own TU, gothic and flora both clients; `gothic_test` pins the move
  byte-identical) **vs** flora includes gothic.h. Recommendation: extract — a forest
  should not include a cathedral.

**Acceptance.** Existing gothic suite untouched-green (bytes identical through the
refactor); a tapered test sweep's radii measured at stations; crease/cap behavior
unchanged.

### Item 2 — `tree_plan()`: the branch graph

**Intent.** The phase centerpiece: `(species, seed, age)` → the structural truth of a
tree, every consumer a reader.

**Spec.**
- Output: a flat array of segments `{parent, p0, p1, r0, r1, depth, tip}` in a
  caller-provided arena with a hard cap (`FLORA_MAX_SEG`, ~256 — the budget is the
  schema's enforcement, not a hope). Scalars-only header struct, memset-zeroed,
  memcmp-able (§1.8 of P6, inherited).
- Growth model: recursive splitting with apical dominance — per node, `splits` children
  at `spread` angle fanned by golden-angle azimuth + `twist`, radii by the taper knob
  under a **conservation law** (∑ child cross-sections ≤ parent's, da Vinci's rule —
  asserted), `droop` blending branch direction toward gravity with depth, length decay
  per depth, `age` scaling overall size AND depth count (a sapling is the same tree
  younger — same seed, fewer generations).
- ONE knob schema (`flora_schema`, names + per-species defaults), species v1 =
  **reserved decision #2** (recommendation: oak, pine, birch, cypress — pine proves
  whorls/apical dominance, birch proves lean+light canopy, cypress proves columnar).
- Lanes: `LANE_FLORA_SPLIT`, `_AZIMUTH`, `_LENGTH`, `_DROOP`, … append-only.
- `floratest` (15th suite): 200-tree invariant sweep — connectivity (every parent
  precedes its child), conservation law, bounds (no segment beyond `height` knob's
  envelope), tip count > 0, age monotonicity (older ⇒ ≥ segments), bit-determinism.

**Acceptance.** The invariant sweep green across species × seeds × ages; the schema
introspectable; zero allocations beyond the caller's arena.

### Item 3 — The wood: the `tree` ref, bark, the hero collider

**Intent.** The graph becomes a placeable, walkable-around, pickable thing.

**Spec.**
- Registry row `tree {species, seed, age}` — emitter walks the graph, sweeps each
  segment chain with the tapered sweep (round profile, low station count; merge
  parent-into-child chains so a branch is ONE sweep, not per-segment stitching),
  octagonal-to-round profile at trunk scale. Budget: HERO tree ≤ ~6k tris wood.
- Bark: a texgen kind (grid off, directional ridge noise along v — the generator grows
  one anisotropy knob). The HERO mint dresses the tree in it (tex ref, the abbey
  pattern). Leaf-color ramp knobs reserved to Item 4.
- Collider: trunk box from the plan's trunk radius/height (`flora_trunk_dims` — the
  shared-formula law from P6 item 10); canopy NOT solid. Pickable via retained CPU
  geom like every mesh ref.
- **Y-mint?** No new key (the keyboard is full): hero trees mint via the capstone's
  composer and hand-edit; **reserved decision #3** if a mint key is wanted (sacrifice
  Q?).

**Acceptance.** Determinism + audit; a placed tree blocks at the trunk, admits under
the boughs; bark reads as bark at arm's length both backends.

### Item 4 — The leaves: the canopy through the ornament pool

**Intent.** Pay the instancing investment forward: the canopy is per-instance data
over one leaf-card mesh; zero or near-zero renderer additions.

**Spec.**
- Generalize the ornament pool from "balustrade-only" to ORNAMENT KINDS: a small
  table `{ref-name → slot enumerator + unit mesh}`; balusters become row 0, leaf
  clusters row 1. Same fingerprint sync, same per-carcass local buffers.
- `flora_canopy(plan, out_slots, max)` — a cluster slot per tip (pos from the graph,
  yaw by golden angle, scale by depth/age, slight droop tilt encodable in yaw+scale
  v1), MONOTONE under a `leaf_density` knob (the baluster membership law).
- The unit mesh: `flora_leafcard_unit` — crossed diamond cards, a handful of tris,
  vertex-level color variation baked (two greens). **Reserved decision #4 — leaf
  style**: pure geometry cards (zero renderer work, stylized-clean, matches the
  palace) **vs** alpha-tested textured cards (texgen leaf kind + FS `discard` +
  pipeline flag + Metal twins — softer look, real renderer addition).
  Recommendation: geometry v1; alpha-test flagged as the refinement.
- Wind: the instanced VS gains a sway term (uTime + per-instance phase from slot
  index — the meadow's exact trick); roots pinned: sway scales with `sy`.
  Conifers (pine) take whorl-cone clusters from the same enumerator — cluster shape
  is species data, not new machinery.
- Canopy color: a `lean` knob (spring/summer/autumn tint) on the material/tex path —
  full seasons stay deferred.

**Acceptance.** A HERO oak reads as an oak at 30 m and at 3 m, in both backends, ONE
draw for its whole canopy + one for wood; shadow pass carries the canopy; the
density knob sheds leaves monotonically (the ruin-dial of trees).

### Item 5 — The forest: the FIELD tier

**Intent.** Islands grow woods the way they grow grass: by being themselves.

**Spec.**
- Per-island scatter (flora lanes off the ISLAND seed): Poisson-ish dart throwing with
  slope/height gates (no trees on crag or rim — the meadow's palette logic), a
  **plan-footprint gate** when the island wears a church (the survival query's
  sibling: `church_occupies(plan, x, z)` — trees never grow through the nave, but DO
  crowd a ruin's fallen bays at high ruin: the forest reclaims).
- Variants: ~4 per island (species mix by a moisture/size lane), shared through the
  mesh registry (params-are-identity does the dedup); instance buffers per island per
  variant — wood AND canopy both instanced (the ornament pool grows an island-FIELD
  client beside the per-carcass one).
- FIELD trees: not pickable (LAND policy), **reserved decision #5 — field colliders**:
  none v1 (recommendation: trunk circles only within some radius of the camera is the
  flagged refinement; ghosting through a distant forest is invisible, through a near
  trunk is cheap to prevent later).
- Budgets: ≤ ~40 trees/island default knob, ≤ 8 draws per island for all flora;
  load-to-interactive stays < 2 s.

**Acceptance.** H-minted islands grow consistent woods (same island, same forest,
forever); the abbey island's ruined east end has saplings in the fallen bays; HUD
budgets hold; L-reload regrows identically.

### Item 6 — Rock: boulders and scree

**Intent.** The cheap companion that makes scatter read as terrain, not decoration.

**Spec.**
- Registry row `boulder {size, seed, flat}` — fBm-displaced subdivided octahedron
  (pure CPU, ~300 tris), the stone texgen kind, course-free. HERO-placeable; FIELD
  scree via the scatter pass (small, instanced, the pebble unit mesh through the
  ornament pool).
- Collider: HERO boulders box (standable — the treaty; a flat-topped boulder is a
  viewpoint); FIELD scree ghost.
- The existing church rubble stays church-owned; this is the island's own stone.

**Acceptance.** Determinism, audit, stand-on-top, both backends.

### Item 7 — Undergrowth: shrubs, flowers, the meadow grown up

**Intent.** Close the ground layer: the gap between grass blade and tree.

**Spec.**
- Shrub = species preset with trunk ~0 (no new machinery — the schema proves §1.4).
- Flowers: the meadow gains a flower variant (tint from the existing instance layout's
  color floats; a `flowers` lane picks patches) — heads as a second tuft mesh.
- Association: undergrowth density biased UNDER canopy (the scatter reads tree
  positions — one pass feeding the next, plan-as-author chained).

**Acceptance.** An island reads as layered (grass → flowers → shrubs → trees) from
one seed; budgets hold.

### Item 8 — Water: the pond (the phase's one renderer feature)

**Intent.** Hollows hold water. Modest, honest v1 — no planar reflections.

**Spec.**
- `pond {r, depth, seed}` HERO ref: a disc at a height, placed by the composer/hand
  into terrain hollows (the mint samples `terrain_height` minima — the plan-reads-
  the-ground pattern from the datum).
- Shader: ONE new pipeline + Metal twin — fresnel against the existing sky/prefilter
  cubemaps (the IBL pays again), normal ripples from two scrolling synthesized
  normal maps (texgen `water` kind — the height-field author animates by phase
  offset, not by re-render), depth fade by distance below the surface plane,
  alpha blend over the already-drawn HDR pass (one transparent draw, LAST — the
  particle slot's precedent).
- Walking: water is not floor — `ground_under` ignores it; wading = you sink to
  terrain (honest v1); the camera under the plane gets a tint (cheap fog uniform).
- **Reserved decision #6 — scope**: this item in v1 **vs** deferred to its own
  later deliberation. Recommendation: in, at exactly this modesty — the abbey pond
  at dusk with sconce-light fresnel is the capstone shot.

**Acceptance.** A pond in a hollow at night reflects the moon-sky plausibly; no
sorting artifacts against grass/trees at the rim; both backends agree.

### Item 9 — One wind

**Intent.** Three systems sway (meadow, canopy, particles) and one hums (audio wind).
Give them ONE field so the island gusts together.

**Spec.**
- `wind_at(t, x, z)` — a tiny pure function (two octaves of time-scrolled noise →
  direction + gust scalar), CPU-evaluated per frame into a handful of uniforms
  (strength, dir, gust phase) shared by the meadow VS, the canopy sway, particle
  drift acceleration, and the audio wind gain (ONE GUST, FOUR SENSES — the lantern's
  law at weather scale).
- Falling leaves: an `emit` preset (the component system already does this — autumn
  trees get a slow leaf-fall emitter in the composer).

**Acceptance.** A gust visibly crosses the island: grass, canopy, motes, and the
audio swell move together; calm is calm.

### Item 10 — The capstone: the island that lives

**Intent.** The phase's portfolio, and the abbey completed.

**Spec.**
- The composer (extend Z or its own pass): the abbey island grows its churchyard yew
  (HERO, by the cross), an orchard pair, the forest ring with saplings in the ruined
  bays, boulders, the pond in the western hollow, flowers in the meadow, the wind.
  A second WILD island (no church) proves the system stands without architecture.
- Budgets HUD-verified: the dressed abbey island < ~550k tris total, flora ≤ 8 draws
  per island, load < 2 s, frame holds 16.7 on the M2 in both backends.
- `FLORA.md`: the index — schema knobs, species table, lanes, tier law, budgets.
- Reserved #7: whether the capstone also retro-dresses existing saved islands
  (recommendation: no — new mints only; old islands are Fran's as-built world).

**Acceptance.** The screenshot: the abbey on its hill at night, glass glowing, wind
in the trees, the pond catching the sconces — and a daylight wild island that needed
no church to feel inhabited. Every suite green; the phase closes.

---

## Part 4 — Decisions reserved to the project owner

1. **The sweep's home** (Item 1): extract `sweep.c` vs flora includes gothic.h.
   Recommendation: extract.
2. **Species v1 set** (Item 2). Recommendation: oak, pine, birch, cypress.
3. **A hero-tree mint key** (Item 3): none (composer + hand-edit) vs sacrifice Q.
4. **Leaf style** (Item 4): geometry cards vs alpha-tested textured cards.
   Recommendation: geometry v1, alpha-test flagged.
5. **FIELD colliders** (Item 5): ghost v1 vs near-camera trunk circles.
   Recommendation: ghost v1, flagged.
6. **Water in v1** (Item 8): in at stated modesty vs deferred whole.
   Recommendation: in.
7. **Retro-dressing old islands** (Item 10). Recommendation: no.
8. **Ordering**: this phase before or after Phase 5 (the app engine).

## Part 5 — Deliberately deferred (not this phase)

- **Full seasons & weather** — rain, snow, seasonal state machines; the color-lean
  knob and falling leaves are the v1 gesture.
- **Birds & creatures** — the wander component generalizes to 3D flight, and it
  deserves its own deliberation (flocking is a phase, not an item).
- **Rivers, waterfalls, ocean** — the pond is a plane; moving water is not.
- **Branch skeletal animation** — canopy sway is vertex-level; bending boughs are
  not v1.
- **Ivy & vegetation ON architecture** — the gothic kit's surfaces as growth
  substrate is gorgeous and its own item, later (it wants the plan + tree_plan
  married).
- **Roots displacing terrain, fruit, harvesting, growth-over-real-time.**
- **Planar water reflections / SSR** — the fresnel-IBL pond is the line.
- **A day/night cycle** — the sky is still one IBL at a time (hdr_reload is the
  lever; a cycle is render-architecture, not flora).
