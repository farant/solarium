# Solarium — Phase 6 Execution Brief (Items 1–10): The Gothic Kit

> **Numbering & status note.** This brief assumes Phase 5 lands as the TODO5.md
> prospectus (the app engine). The gothic kit is **Phase 6** — "the long-stated heart of
> the project" (TODO5 Part 5) finally given its own brief. Seeded 2026-06-11, drafted
> mid-Phase-4 like TODO5 was, to capture the full design while it is hot; if the
> phase-boundary brainstorm reorders Phase 5 and 6, renumber — nothing here depends on
> the app engine. Items are numbered 1–10 *within this phase*; cite as "P6 · Item N".

## How to use this document

You have the engine. Phases 1–4 built the substrate — parametric refs, the registry-as-
schema, collision derived from the same parameters the renderer reads, instancing, the
asset cache, terrain whose height function is "THE one source of truth." This phase is
the payoff those disciplines were *for*: procedural Gothic architecture, generated onto
the rectangular islands, complete or ruined or half-built, walkable by construction.

The organizing thesis: **a building is a function.** `terrain_height(params, lx, lz)`
taught the engine that geometry, normals, and physics can all evaluate one deterministic
function and agree by construction. The gothic kit elevates that lesson from a surface to
a structure: one deterministic **plan function** expands `(w, d, seed, …)` into the
building's structural truth — the bay grid, the piers, every opening, every dependency —
and *everything else is a reader of it*. The stone emitter reads it. The glass emitter
reads it. The collider derivation reads it. The ruin pass reads it. Same seed, same
church, forever: the codex-mint pattern at architectural scale.

The second thesis: **real architecture is already a procedural system, so use the real
grammar.** Gothic construction is not a style applied to boxes; it is a dependency graph
the medieval lodges themselves treated procedurally (Roriczer's 1486 pinnacle booklet
derives an entire pinnacle from one square; elevations were derived from plans *ad
quadratum*). The bay is the repeating unit; the vault scheme determines the ribs; each
rib demands a respond shaft, which is why compound piers look the way they do; the
buttress stands where the vault thrusts. A generator that models the causal chain gets
correct-looking detail *for free* and avoids the kit-bash look — and gets believable
ruins for free too, because decay that respects the dependency graph is what real ruins
look like (arcades standing, vaults gone, the east window an empty tracery frame). Tiny
Glade is the reference for *feel* (NOTES.stml reference-games); the construction
discipline is the cathedral lodges' own.

**Sequencing.** Item 1 (the profile sweep) is the gate — it is the mesh primitive every
molded element rides. Items 2–3 (arches, the plan) are the geometric and structural
cores; 4–7 build the church outward from the plan (stone shell → vaults → glass → roof
and tower); 8 is the ruin/worksite pass over the finished dependency graph; 9 grounds it
all on terrain and in the collision layer; 10 is the capstone — the standalone pieces
and the demo island that is the phase's screenshot test.

**Part 1 is binding on every item.** Each item states **intent**, **spec**, an
**interface sketch** where it helps, and **acceptance**. Reserved decisions are flagged
inline and collected in Part 4.

---

## Part 1 — The Constitution

### 1.1 What is already established (maintain it)

- **Strict C89**, `-std=c89 -pedantic-errors -Wall -Wextra`, the forbidden-construct
  list, `sol_base.h` types, `./build.sh c89check`. The entire kit is pure CPU — nothing
  in this phase touches the RHI seam at all, let alone GL.
- **The MeshBuilder is the seed of all procedural geometry** (12-float canonical
  vertices, `mb_push_vertex`/`mb_push_triangle`, tangents computed at upload). Every
  emitter this phase adds is a client of it.
- **Registry-as-schema**: every new piece of geometry is a row in mesh.c's `REGISTRY` —
  named params + defaults, the prefix-merge rule, self-describing in the scene file.
  `MESH_REF_MAX_PARAMS` stays 8 (see §1.3 for why that is enough).
- **The asset cache dedups by `m|ref|params`** (P4 item 4): two islands placing the same
  `(w,d,seed)` church share GPU buffers with zero new machinery. Rely on it.
- **§1.4 of TODO3 (never booleans)** and the around-the-gap construction with
  exposed-face emission and the coplanarity discipline. Extended, not amended, by §1.4
  below.
- **One author for the world's shape (P4 §1.3)**: collision and render geometry derive
  from the same parametric source. This phase's §1.2 is that law's largest application.
- **World-scale UVs** (1 unit per meter, the `make_room` precedent) so texel density is
  uniform across differently-sized buildings.
- **The permanence invariant**: the file stores `ref + params + TRS`, never geometry.
  A church is its parameters.
- **Headless tests are the house proof style.** This phase adds `gothictest`.
- **Multi-material assemblies are groups of objects sharing params** — the
  `book_cover`/`book_block` precedent. The church follows it exactly (§1.7).

### 1.2 NEW — The plan is the one author

The kit's center of gravity is `church_plan()`: a **pure, deterministic, allocation-free
expansion** of the ref params into the building's structural description. Every
sub-emitter (`church_stone`, `church_roof`, `church_glass`, `church_floor`), the
collider derivation in `collide_rebuild`, the ruin pass, and any future query (wayfinding
through the nave, candle placement in sconces) evaluates **the same plan function**.
There is no stored plan file, no cached layout, no second description. When stone and
collision disagree about a doorway, the bug is that there were two authors — and the fix
is in the plan, where both readers pick it up at once. This is `terrain_height`'s
contract: the plan is to the church what the height function is to the island.

Corollary: **the plan owns abutment.** A wall segment spans between *pier faces*, not
pier centers; a web meets a rib at the rib's flank; a buttress stage sits on the stage
below. Coplanar-face skipping (the z-fight defense) is computable only because one
function knows where every element ends and its neighbor begins.

### 1.3 NEW — The seed is the building (lanes, not a stream)

Eight params are enough because **the seed is the rest of the parameter vector** — the
codex-mint pattern as law. But a single sequential PRNG stream is forbidden: if drawing
the tracery style consumed stream position 7, then adding a new decision before it would
reshuffle every church in every saved scene. Instead, every architectural decision draws
from a **named lane**: `gothic_hash(seed, LANE_ID, i, j)` — `noise_hash`'s pattern with
the lane enum as one lattice coordinate. Decisions are independent by construction;
per-bay decisions key on the bay coordinates; adding a lane never disturbs an existing
one. The lane enum is **append-only** — reordering it is a save-breaking change and is
treated like reordering the STML schema.

### 1.4 NEW — Around-the-gap, now with curves (still never booleans)

The doorway wall's promise comes due: "a future gothic arch is this same pattern with a
segmented curved head" (mesh.c). The law: every opening — pointed door, lancet window,
arcade arch, belfry louvre — is built as the pieces **around** it: jamb panels below the
springing, a **segmented head** above it whose strip bottoms follow the arch polyline,
and reveal faces (jambs + intrados) across the wall's real thickness. Curved surfaces
are emitted as strips/fans of real quads at a stated tessellation (§ Item 2's
`GOTHIC_MAX_SEG`); abutting faces are skipped exactly as `make_wall_with_opening` does.
Mesh CSG remains permanently out of scope.

### 1.5 NEW — Structure before ornament (the dependency graph is real)

Every element the plan describes knows what supports it: web → its four ribs → their two
piers; flyer → its outer buttress *and* its clerestory bay; roof truss line → its wall
heads; pinnacle → its buttress. The ruin pass (Item 8) and the worksite pass (the
`built` param) are **graph traversals, never random deletion**: an element may exist
only if its supports exist. This is simultaneously the realism law (it is why ruins read
as ruins), the determinism law (survival is a pure function of plan + ruin value), and
the safety law (no floating masonry, ever, at any parameter combination).

### 1.6 NEW — The glossary is the namespace

The kit speaks the real terminology, in code: `springing`, `impost`, `intrados`/
`extrados`, `voussoir` (as the head-strip comment term), `respond`, `bay`, `web`,
`boss`, `tas-de-charge`, `clerestory`, `triforium`, `weathering` (the buttress slope),
`broach` (the spire transition). Functions, struct fields, lane names, and comments use
these words; a contributor with Toman's glossary open can read the code, and a
contributor reading the code learns the architecture. This is not decoration — shared
precise vocabulary is what lets the plan function's comments cite *why* (e.g. "transverse
arches are stilted to level the crowns — the standard High Gothic solution").

### 1.7 NEW — The kit is a vocabulary; main.c composes

All emitters live in a new pure-CPU TU pair `gothic.c`/`gothic.h` (strict C89, in
`c89check`, headless-testable, no scene/GL includes — exactly `collide.c`'s citizenship).
mesh.c's `REGISTRY` remains **the** single table; it gains rows whose emit functions call
into gothic.c. The church is **not one object**: it is a small group, the book pattern —
`church_stone` + `church_roof` + `church_glass` + `church_floor`, all carrying identical
params, each wearing its own material, parented to one empty group object the spawner
mints. Spawning, terrain placement, and material assignment are main.c composition
(Item 9); gothic.c knows geometry only.

### 1.8 Definition of done (every item)

Builds clean under `-std=c89 -pedantic-errors -Wall -Wextra`; ASan+UBSan clean; no
include of rhi.h/scene.h in gothic.c; **bit-determinism** — two builds with identical
params produce byte-identical MeshBuilder contents (memcmp; this is gothictest's first
assertion and every item's last); new plan capability is exercised through at least one
registry row; the stated acceptance demonstrably passes; no new dependency.

---

## Part 2 — Item 1: The profile sweep & the molding table (gate; do first)

**Intent.** One operation underlies the entire molded vocabulary of Gothic architecture:
a small 2D section extruded along a path. Ribs, archivolts, mullions, string courses,
hood-molds, column shafts, bases, balusters — all are profile sweeps; the glossary's
*cavetto*, *bead moulding*, *bevel* are literally rows in the profile table. Build this
first, alone, with its own tests: every later item is its client, and a subtle framing
or winding bug here becomes a hundred haunted moldings later.

**Spec.**
- **The profile**: a polyline in section space — `o` (outward, away from the wall/core)
  and `u` (up in the section's frame), plus a per-point `crease` flag. Creased points
  duplicate the ring vertex so the two adjacent faces get hard normals (a fillet edge);
  uncreased points share one averaged normal (a roll/torus reads round).
- **The path**: a world-space polyline. **v1 paths are planar** — every Gothic sweep this
  phase needs (arch curves, straight runs, vault rib arcs) lies in a plane, so framing is
  exact and simple: the binormal is the path plane's normal (passed as `up_hint` crossed
  into the dominant direction), constant along the sweep; at interior joints the ring
  plane is the **miter plane** (normal = the angle bisector of adjacent segment
  directions) so rings never pinch. No parallel transport, no torsion — deliberately
  dodged, the §1.4-of-TODO3 instinct applied to differential geometry.
- **Emission**: one vertex ring per path point (profile points × crease duplication),
  quads stitched between consecutive rings as two triangles each, outward winding
  (`cross(path_dir, profile_tangent)` convention, asserted in tests against a known
  cube-like sweep). Optional flat end caps (a triangle fan over the profile polygon) for
  free-standing pieces; abutting ends (a rib dying into a springer) skip caps per §1.2.
- **UVs world-scale**: `u` = profile arclength in meters, `v` = path arclength in meters
  — moldings pick up the stone texture at the same density as the walls.
- **The molding table**: `static const` profiles in gothic.c with an append-only enum:
  `PROF_RIB` (a chamfered rib: roll + fillets), `PROF_MULLION` (the standard
  double-chamfer), `PROF_STRING` (cavetto over roll — the string course), `PROF_BASE`
  (the attic base: torus, scotia, torus over a square plinth), `PROF_HOOD` (the
  hood-mold drip), `PROF_SHAFT_OCT` (octagonal shaft section). 6–12 points each; sized
  in meters at canonical scale with a uniform scale factor at the call site (a nave rib
  and a window mullion are the same profile at different scales — historically true).
- **Tessellation — two caps, take the finer**: `GOTHIC_MAX_SEG` (0.25 m) bounds chord
  length and `GOTHIC_MAX_ANG` (22.5°) bounds turn per segment;
  `n = max(ceil(arc_angle / MAX_ANG), ceil(arc_len / MAX_SEG))`. The linear cap governs
  big rib arcs (voussoir-scale faceting, cheap); the angular cap rescues small circles —
  a 0.3 m tracery foil under the linear cap alone would be a 4-gon. Predictable budgets,
  and the foils don't embarrass the windows.

**Interface sketch.**
```c
typedef struct { float o, u; unsigned char crease; } ProfilePt;

/* Sweep `prof` along the PLANAR polyline `path` (plane normal `plane_n`).
   scale multiplies the profile; cap0/cap1 emit flat end caps. Rings are
   mitered at joints. Winding: profile CCW as seen looking down path_dir. */
void gothic_sweep(MeshBuilder *b, const ProfilePt *prof, int prof_n,
                  const vec3 *path, int path_n, vec3 plane_n,
                  float scale, int cap0, int cap1);

const ProfilePt *gothic_profile(int prof_id, int *out_n);
```

**Acceptance.** A swept square profile along a straight path memcmp-equals a hand-built
box's positions; a swept `PROF_STRING` along a 90° two-segment path shows a clean miter
(no pinched ring, no inverted quad — assert all quad normals agree with winding);
determinism memcmp; visual: a test scene row of all table profiles swept along a straight
run and along an arch (Item 2 path), inspected under the IBL.

**Decisions flagged:** the v1 profile set (the six above recommended); GOTHIC_MAX_SEG
value.

---

## Part 3 — Items 2–10

### Item 2 — The arch family & the arched opening

**Intent.** The pointed arch is the kit's signature curve and the around-the-gap rule's
first curved test. One closed-form construction covers every arch in the building —
door, window, arcade, vault rib, flyer — differing only in span and one shape parameter.

**Spec.**
- **The two-arc construction.** An opening of span `s` (springing points at ±s/2, y=0
  local). Each half is a circular arc whose center sits on the springing line at ∓c,
  radius `r = c + s/2`, rising to the crown at `y = sqrt(r² − c²)`. Parametrize by
  **acuteness** `a = 2c/s`: `a = 0` → semicircular (Romanesque survivals), `a = 1` →
  equilateral (the High Gothic default), `a > 1` → lancet, and a *drop* arch for
  `0 < a < 1`. One float, the whole historical family.
- **`gothic_arch_path()`** emits the polyline: both arcs subdivided to `GOTHIC_MAX_SEG`,
  symmetric about the crown (an odd point count so the apex vertex exists exactly —
  the crown must be a vertex or every rib boss floats).
- **The level-crown solve** (used by Item 5 but it belongs to the arch math): given a
  required crown height `H` over span `s`, the acuteness that achieves it:
  `c = (H² − s²/4) / s` (from `sqrt((c+s/2)² − c²) = H`), valid for `H ≥ s/2`. This
  single formula is *why* Gothic exists — arches of different spans reaching one height —
  and it is what makes the vault item nearly trivial.
- **`gothic_wall_arched()`** — `make_wall_with_opening`'s sibling with a pointed head:
  jamb panels (full wall pieces left/right of the opening, exposed faces only, jamb
  reveals up to the springing); above the springing, the head emitted as **vertical
  strips**: for each consecutive pair of arch-polyline x-stations, front and back quads
  from `arch_y(x)` up to the wall top, and an **intrados strip** (the reveal under the
  arch) across the wall thickness — each strip a quad pair between consecutive polyline
  points, normals along the local arc normal (creased per-strip: real voussoir-scale
  faceting reads correctly in stone, do not fake smooth). Threshold face across the
  thickness at y=0 exactly as the flat-headed wall does. Degenerate handling (opening
  flush to an edge) follows the existing emitter.
- **Recessed orders** (for Item 7's portal, but the mechanism lands here): the same
  opening repeated N times through the wall thickness at stepped sizes — each order a
  narrower arched opening, its jamb/intrados strips connected to the next order's by
  step faces; archivolt sweeps (`PROF_RIB` along the arch path) dress each arris.

**Interface sketch.**
```c
/* Fill `out` (cap `max_n`) with the pointed-arch polyline in the XY plane:
   span s at y=0, acuteness a. Returns the point count (odd; apex exact). */
int  gothic_arch_path(vec3 *out, int max_n, float s, float a, float max_seg);
float gothic_arch_y(float s, float a, float x);            /* head height at x   */
float gothic_arch_acuteness_for(float s, float crown_h);   /* the level-crown solve */

void gothic_wall_arched(MeshBuilder *b, float w, float h, float t,
                        float ox, float ow, float spring_h, float a);
```

**Acceptance.** `gothic_arch_y(s, 1, 0) == 0.8660·(s/2)·2…` — crown heights match the
closed forms to 1e-5 across a parameter sweep; the acuteness solve round-trips
(`arch_y(s, acuteness_for(s,H), 0) == H`); a wall with a pointed doorway shows no
z-fighting, no cracks at strip seams (shared vertices along the polyline), and the
doorway admits the player capsule once Item 9 lands colliders; determinism memcmp.

---

### Item 3 — The plan function (`church_plan`): the seed becomes a building

**Intent.** The constitutional centerpiece (§1.2, §1.3). Everything before this item is
geometry; everything after it is a reader of this.

**Spec.**
- **The ref schema** (shared verbatim by all four sub-refs, §1.7):
  `{ "w", "d", "seed", "style", "ruin", "built", "acute", "reserved" }` with defaults
  `{ 18, 30, 7, -1, 0, 1, 1.0, 0 }`. `style`: −1 = derive from size (the default), else
  force 0 = **chapel** (single vessel, no aisles), 1 = **hall** (aisled, one roof,
  aisle = nave height), 2 = **basilica** (aisled, clerestory, flyers). `ruin`/`built` ∈
  [0,1] are Item 8's. One slot spare under the cap of 8.
- **The expansion, in order** (every draw a named lane, §1.3):
  1. **Orientation & margins.** The nave axis runs along the *longer* plot dimension
     ("east" = local +X by convention; real orientation is the spawner's rotation).
     A perimeter margin reserves buttress depth (style-dependent, 0.8–2.2 m).
  2. **Style** (if −1): interior area < ~120 m² → chapel; else hall vs basilica from
     LANE_STYLE weighted by width (basilica wants ≥ 14 m clear).
  3. **The bay module.** Draw the nave width `nw` from style range (chapel 4–7 m,
     basilica nave 7–10 m); aisle width `aw ≈ nw/2` (the *ad quadratum* ratio). Bay
     length `bl` = nw/2 ± LANE_MODULE jitter (the doubled-square bay of High Gothic);
     fit `nbays = floor(usable_length / bl)`, clamped to `PLAN_MAX_BAYS` (16);
     remainder absorbed: ≥ 0.6·bl at the east end → **apse** (5/8 polygon for
     basilica/hall, flat for chapel, LANE_APSE may force flat); ≥ 0.8·bl at the west →
     a **tower bay** (LANE_TOWER, basilica/hall only); residue feeds the west wall
     thickness and porch steps.
  4. **The elevation formula.** Springing/impost height, arcade height, clerestory band,
     wall thickness `wt` (0.6–1.1 m by style), parapet height — drawn once per building
     from lanes within style ranges; **string courses** at impost and sill lines.
  5. **The structural table — computed, not stored.** The plan struct holds only the
     scalars above; every *element* is answered by query functions: `plan_pier(p,i,j)`
     (position + which respond shafts it carries), `plan_bay_kind(p,i,j)`
     (nave/aisle/apse/tower/porch), `plan_opening(p, wall_edge)` (the window/door
     rectangle + arch params for any wall segment), `plan_supports(p, elem)` (the
     dependency edges, §1.5). Bounded, allocation-free, pure — `terrain_height`'s
     citizenship.
- **Windows by rule, not by draw**: aisle and clerestory bays get one window each,
  width = 0.55·bay clear span, sill at the string course, springing at a fixed fraction
  of the band — so windows *rhythm* with the bays automatically. The west front gets the
  portal (Item 7) and a great window above it. Tower bays get paired belfry openings.
- **`gothictest` plan invariants** (property tests across ~200 seeds × sizes): every
  pier inside the plot; every opening inside its wall segment with ≥ 2 courses of
  masonry above the crown; every nave bay's four piers exist; apse sides close (vertex
  loop sums to 2π); module within style range; determinism.

**Interface sketch.**
```c
#define PLAN_MAX_BAYS 16
typedef struct {
    int   style, nbays, aisles, apse_sides, tower_bay;  /* -1 = none */
    float nave_w, aisle_w, bay_l, wall_t;
    float impost_h, arcade_h, clerest_h0, clerest_h1, wall_h;
    float plinth_h, acute;
    unsigned seed;
    /* … scalars only; ELEMENTS are answered by the queries below */
} ChurchPlan;

void church_plan(ChurchPlan *p, const float *params, int count);
/* lanes */
float gothic_hash01(unsigned seed, int lane, int i, int j);
```

**Acceptance.** The invariant suite passes across the seed sweep; the same params
expand identically across two calls (struct memcmp); a debug overlay (main.c, the HUD
pattern) can draw the plan as floor-plan lines on an island — the visual sanity check
before any stone exists.

**Decisions flagged:** style ranges and the area thresholds; whether `style` stays an
override param or becomes derive-only.

---

### Item 4 — The stone shell: walls, piers, buttresses (`church_stone`, part 1)

**Intent.** The first full reader of the plan: everything load-bearing and opaque, one
mesh, one material. After this item a church *stands* — roofless, glassless, but
unmistakably a church.

**Spec.**
- **Walls**: for each perimeter segment between pier stations, `gothic_wall_arched`
  pieces per the plan's opening query — spanning **pier face to pier face** (§1.2
  abutment), sitting on a **plinth** course (a stepped footing emitted as box faces,
  `PROF_BASE`-topped), topped by a parapet over the wall head. Interior arcades
  (nave/aisle boundaries, basilica & hall): open arched walls — the same emitter with
  opening = the full bay between piers, springing at arcade impost.
- **Piers**: octagonal core (`PROF_SHAFT_OCT` swept vertically pier-height) on an attic
  base over a square sub-plinth, with a **capital** at the impost: an octagonal frustum
  + abacus slab (box faces) — the molded-capital simplification of v1. **Responds**: at
  each rib the plan assigns (Item 5), a half-shaft (the same octagonal profile at 0.4
  scale, swept up the pier flank to the springing) — the compound pier emerges from the
  dependency graph exactly as §1.5 promises. Apse piers follow the polygon stations.
- **Buttresses**: at every perimeter pier station, a stepped buttress: 2–3 box stages
  stepping back, each stage topped by a **weathering** slope (two sloped quads + side
  triangles — real exposed-face emission, no boolean); depth/stages by style. Basilica
  **flyers** wait for Item 5 (they need the vault's thrust line to aim at).
- **String courses**: `PROF_STRING` swept along the perimeter at impost and sill heights,
  jogging around buttresses (planar rectangular path loops — the miter joints earn their
  keep).
- **Towers** (if `tower_bay ≥ 0`): the tower shaft is the same wall machinery on the
  tower bay's square, run to tower height (≈ 2.2× wall_h, LANE_TOWER_H), belfry
  openings paired per face; the spire is Item 7's.
- **Coplanarity audit**: wall meets buttress, plinth meets wall, stage meets stage —
  every abutting face skipped; gothictest samples random seeds and asserts no two emitted
  quads are coplanar-overlapping (the shimmering-checkerboard defense, now automated).

**Acceptance.** A chapel, a hall, and a basilica (forced styles, fixed seeds) each build
clean and read correctly from outside and inside; piers carry visibly correct responds
once Item 5's rib assignment exists; tri-count for a 10-bay basilica shell within budget
(~60k tris; HUD-verified); coplanarity audit and determinism pass.

---

### Item 5 — Vaults: quadripartite ribs and webs (`church_stone`, part 2)

**Intent.** The crown of the system, literally — and the item where the level-crown
solve (Item 2) pays for the whole arch family. v1 is the quadripartite rib vault, the
workhorse of High Gothic; everything fancier is deferred.

**Spec.**
- **The geometry, per bay** (interior clear span `w` × `l`, springing at impost):
  - **Diagonal ribs**: semicircular (`a = 0`) over the bay diagonal — span
    `q = sqrt(w² + l²)`, so the **crown height is `H = q/2`** above springing. This is
    the historical datum: the diagonals are round, and everything else stretches to meet
    them.
  - **Transverse arches** (across the nave, span `w`) and **wall/longitudinal ribs**
    (span `l`): pointed, acuteness from `gothic_arch_acuteness_for(span, H)` — the
    level-crown solve, applied exactly as the lodges applied it. All five rib crowns
    meet at one point: the **boss**.
  - **Ribs are `PROF_RIB` sweeps** along the arch paths in their (vertical, planar)
    planes; at the springer the four-to-five rib bundle converges on the pier's
    respond cluster (v1: ribs simply terminate on the abacus — the solid *tas-de-charge*
    block is a flagged refinement). The **boss**: a small octahedral knob at the crown
    (a future instanced ornament slot).
  - **Webs**: four cells per bay, each a **ruled loft** between two adjacent ribs:
    parametrize each rib's path by normalized arclength `t ∈ [0,1]` springer→crown;
    connect equal-`t` stations with straight rules subdivided to `GOTHIC_MAX_SEG`;
    emit the resulting quad strip grid **twice** (intrados faces down/inward, extrados
    up — the extrados is what a roofless ruin shows, so it is not optional). A gentle
    doming bias (lift interior points along +Y by `0.08·H·sin(πt)·sin(πs)`) breaks the
    dead-flat ruled look; one constant, deliberately subtle.
- **Aisle vaults**: the same machinery per aisle bay at aisle spans; the apse gets a
  radial half-vault (ribs from each polygon pier to a half-boss — same code path, the
  polygon stations just are the springer set).
- **Flyers** (basilica): now the thrust line exists — from each clerestory pier head,
  a quadrant arc (`a = 0` half-arch) swept with a rectangular section down to the outer
  aisle buttress's top stage, plus the buttress's upper stage raised to receive it.
  Dependency edges registered (§1.5): flyer ← {clerestory pier, outer buttress}.
- **The ceiling/collision question**: vault webs are render geometry only — the collide
  layer's fly-mode clamp uses the wall-head boxes; per-triangle vault collision is not
  needed (reserved if fly mode feels wrong under the vault).

**Acceptance.** All five rib crowns of every bay meet at one vertex to 1e-4 (asserted in
gothictest across the seed sweep — this single assertion validates the entire arch
stack); webs show no cracks against ribs (shared parametrization, not proximity
welding); a basilica's flyers land on their buttresses across the seed sweep; interior
screenshot under the spot+IBL reads as a vaulted nave; determinism.

---

### Item 6 — Windows & tracery: the geometric grammar (`church_glass` + stone bars)

**Intent.** The element that reads as "Gothic" from a hundred meters — and a famously
clean subgrammar: v1 ships **Geometric** tracery (circles and pointed sub-arches only),
which covers ~1250–1310 and is entirely constructible from the arch math in hand.

**Spec.**
- **The grammar, per window** (opening span `s`, springing `h`, acuteness `a` from the
  plan):
  1. **Lights**: `n = clamp(round(s / 0.7 m), 1, 4)` mullion-divided vertical lights
     (LANE_TRACERY jitters the divisor slightly per building, not per window — a
     building's windows are a family).
  2. Each light heads in a **pointed sub-arch** springing at the main arch's springing,
     acuteness inherited.
  3. **The spandrel** (between sub-arch crowns and the main arch): `n = 1` → nothing
     (a lancet); `n = 2` → one circled **foil** (a circle tangent to both sub-arch
     extrados and the main intrados — centers computable in closed form from the three
     arc centers/radii; v1 may bisect numerically, 10 iterations, deterministic);
     `n = 3–4` → a large foiled circle over the center pair + small circles in the
     remaining spandrels (the Lincoln/Amiens scheme).
  4. **Cusping** (the trefoil lobes inside circles) is a flagged stretch: three arcs
     inside each foil circle; geometry is the same arc machinery, budget is the question.
- **Emission**: all bars are `PROF_MULLION` sweeps along the construction's arc/segment
  paths, in the window plane, into **`church_stone`** (bars are masonry); jamb-to-bar
  abutments per §1.2. The **glass** is `church_glass`: one flat polygon panel per light
  and per foil, inset to mid-wall depth, earclipped (a tiny deterministic earclip for
  convex-ish window polygons lives in gothic.c), **double-sided** (two windings) since
  it is seen from both sides.
- **The glass material** (main.c composition, Item 9's spawner): v1 is **opaque glossy
  dark glass** — low roughness, near-black base color, slight emissive at dusk
  composition time (`emissive` is already in Material; bloom bites on it — the stained-
  glass-at-night shot for free). True transparency is a renderer feature this phase does
  not buy (reserved; noted in Part 4).
- **The west window**: the same grammar at `n = 4` over the portal; the **rose** window
  (radial grammar) is deferred — the Geometric grammar's circled spandrels carry the
  facade meanwhile.

**Acceptance.** A window sweep across `s ∈ [0.8, 4.5]` m produces 1–4 lights with no
bar/jamb intersections (sampled-point clearance test); foil circles tangent to their
three bounding arcs within 5 mm; glass polygons triangulate without slivers (min-angle
assertion); a basilica's clerestory ribbon reads correctly at distance; determinism.

**Decisions flagged:** cusping in or out of v1; the dusk emissive composition.

---

### Item 7 — Roof, tower & spire, the west portal (`church_roof` + facade)

**Intent.** The silhouette items: everything above the parapet and the one facade
element visitors walk through. After this item the complete (ruin = 0) church is *done*.

**Spec.**
- **Roofs** (`church_roof`, own material — lead/slate, reserved): the nave gets a gabled
  prism at the Gothic pitch (~55°, LANE_PITCH ±5°) bearing on the wall heads, gable
  triangles closing east/west (or dying into the tower); aisles get lean-to roofs from
  nave wall to aisle parapet (basilica) or share the main roof (hall — the defining
  difference, and it falls out of the plan's style scalar); the apse gets a polygonal
  half-cone. Exposed-face boxes and sloped quads; eaves overhang one wall-thickness.
  Roof *structure* (trusses) is not modeled — the roof is its skin (visible truss-work
  is a ruin-interior refinement, deferred).
- **The spire** (towered styles): an octagonal pyramid over the square tower head via
  **broaches** — the four corner half-pyramids that effect the square→octagon transition
  (pure closed-form vertex math, ~40 triangles, and the single most silhouette-defining
  element in the kit); parapet-and-needle as the LANE_SPIRE alternative.
- **Pinnacles**: Roriczer's derivation, simplified — a square shaft, gabled on four
  faces, topped by a slender pyramid + finial knob; emitted once per *params* into a
  small standalone ref (`pinnacle`) and **instanced** (P4 item 3's path, compact layout)
  at buttress heads and tower corners by the spawner. The first real architecture
  client of the instancing item, as TODO4 predicted ("Phase 5's arcades and balustrades
  will quietly thank it").
- **The west portal**: recessed orders (Item 2's mechanism) — 2–3 stepped arched
  openings through a thickened west wall, archivolt sweeps on each order, a **tympanum**
  (the flat slab filling the head above the door lintel — relief carving is far-future;
  v1's tympanum takes the material's normal map gracefully), and a doorway sized for the
  capsule with margin. **Porch steps**: risers emitted from the plan's west residue —
  they become Item 9's climbable colliders via the STEP_UP treaty.

**Acceptance.** Forced chapel/hall/basilica show three distinct, correct silhouettes
(the hall's single great roof vs the basilica's stepped section is the visual proof the
style scalar works); the spire broach closes watertight (edge-manifold assertion over
the tower head); the portal admits the capsule between its orders; pinnacle instancing
shows one draw for all pinnacles in the HUD; determinism.

---

### Item 8 — The ruin & the worksite: `ruin` and `built` as graph traversals

**Intent.** The kit's poetic payoff and §1.5's proof. Identical machinery, two
directions: `ruin` tears the dependency graph down along a spatial decay field; `built`
stops its construction partway along the historical build order. Both are pure functions
of the plan — a ruined abbey keeps its exact collapse forever, the codex-mint promise
extended to entropy.

**Spec.**
- **The decay field**: per bay,
  `d(i,j) = 0.55·value_noise(i·0.7, j·0.7, lane(LANE_RUIN)) + 0.45·grad(i)` where
  `grad` rises toward one building end chosen by LANE_RUIN_DIR — collapse is spatially
  *correlated* (one end rubble, the other near-intact: the Tintern/Fountains reading),
  never salt-and-pepper. Local pressure: `p(i,j) = ruin · (0.35 + 0.65·d(i,j))`.
- **Class thresholds** (an element class falls where `p` exceeds its `T`), ordered by
  real-world fragility — append-only table:
  `T_roof .12 < T_web .28 < T_glass .30 < T_tracery .42 < T_clerestory_wall .45 <
  T_rib .50 < T_flyer .55 < T_spire .58 < T_arcade .62 < T_pinnacle .65 <
  T_upper_wall .70 < T_pier .80 < T_lower_wall .95` (lower courses essentially always
  survive — ground plans outlive everything, which is also why archaeologists have
  jobs).
- **Then the graph pass** (§1.5, the part that makes it *true*): survival is
  intersected with support — a web with a fallen pier falls regardless of `p`; a rib
  whose web fell **survives as a stub**: the sweep truncated at
  `t = 0.15 + 0.25·hash01` of its path (springer stubs on standing piers — the single
  most recognizable ruin signature); a flyer falls unless **both** ends stand; a fallen
  pier becomes a **broken column**: the shaft sweep truncated at
  `plinth + hash01·0.6·shaft_h` with a jagged cap (the top ring perturbed per-vertex by
  course-quantized noise); a wall under pressure keeps only its lower courses, its top
  **quantized to the course height** (0.4 m) and stepped per strip — quantization is
  what makes a broken wall read as masonry rather than torn paper.
- **Consequences, not extra rules**: where the vault above a bay is gone,
  `church_floor` omits that bay's pavement → the terrain island shows through as the
  grass floor (Item 9 sinks the pavement datum to make this literal); window bars whose
  glass fell stay as **empty tracery frames** (T_tracery > T_glass — deliberate, it is
  the east-window-at-Tintern shot); `church_roof` over fallen bays is simply absent,
  exposing web extrados (Item 5 emitted them for exactly this).
- **Rubble**: per fallen element, a debris contribution — v1 is 1–3 low fBm mounds per
  heavily-ruined bay (gothic.c's own value-noise twin — mesh.c's is `static`, and §1.3's
  lane hash wants gothic-owned noise anyway; a 6×6 heightfield patch into church_stone);
  instanced stone-block scatter is the stretch (the instancing path is ready).
- **`built`** runs the same survival query against **construction stage** instead of
  pressure: stages {0 plinth, 1 piers+lower walls, 2 arcade+aisle vaults, 3 clerestory,
  4 high vaults, 5 roof, 6 ornament}, awarded **east-to-west** (choir first —
  historically how it was done, so a half-built church has a roofed east end and open
  western foundations, which is exactly what Cologne looked like for 600 years):
  `stage_available(i) = floor(built · (6 + nbays) − distance_from_east(i))`, clamped.
  `ruin` and `built` compose (a ruined abandoned worksite is two multiplied survival
  masks) — for free, because both are masks over the same graph.
- **gothictest**: monotonicity (raising `ruin` never *adds* an element; raising `built`
  never removes one — element-set inclusion across a parameter ladder); no surviving
  element with a fallen support (exhaustive graph check per seed); determinism at every
  ladder step.

**Acceptance.** `ruin` slider 0→1 over a fixed seed (a debug binding) shows a continuous
believable collapse with no popping floaters and no inside-out geometry at any value;
the 0.6-ruin basilica delivers the phase's *second* screenshot test: standing arcade,
sky where the vault was, springer stubs, an empty traceried east window.

**Decisions flagged:** threshold table values (tunable, append-only); rubble v1 scope.

---

### Item 9 — Ground truth: terrain, foundations, collision, the spawner

**Intent.** The church meets the two systems that make it *real*: the island it stands
on and the capsule that walks it. One author (§1.2) is enforced here or nowhere.

**Spec.**
- **The datum**: builders leveled a platform; so does the spawner. It samples
  `terrain_height` at the plan's pier stations and takes the **maximum** (the building
  must nowhere float) — that is the floor datum; the group object's y. The emitters stay
  terrain-ignorant (they build to local y=0); grounding is composition.
- **The foundation skirt**: every perimeter wall and buttress extends
  `GOTHIC_FOUNDATION` (2.5 m) *below* local y=0 — buried in the hill on the high side,
  exposed footing on the low side, kin to terrain's own skirt. Where datum − terrain at
  the portal exceeds one riser, the porch steps (Item 7) multiply to span it.
- **Sinking the floor**: pavement top sits at datum; in ruined bays where pavement is
  omitted, the spawner's datum choice (max of pier samples) means terrain is at-or-below
  pavement everywhere — the grass floor is the island itself showing through, walkable
  via `ground_under` with **zero new ground code**. (Where terrain dips well below
  pavement inside a ruined bay, the gap reads as the floor robbed to its make-up layers
  — acceptable, even good; flagged for taste review.)
- **Colliders — the plan read a third time**: `collide_rebuild` gains the church refs:
  per around-the-gap wall piece a `ColliderBox` (jambs full-height — springing heights
  ≥ 2.1 m clear the 1.8 m capsule, asserted in the plan invariants); piers, buttress
  stages, plinths as boxes; pavement and steps as thin top-claiming slabs (the STEP_UP
  treaty makes stairs climbable with **no stair-specific code** — riser 0.17 m ≪
  0.6 m); ruined stubs/broken columns contribute their truncated boxes. The yaw field
  handles the apse polygon's angled wall boxes (collide.h already stores cyaw/syaw —
  it was built for this day). **Derivation calls the same `church_plan` + survival
  queries the emitters call** — gothictest asserts render/collide agreement by sampling:
  every doorway admits the capsule, every wall rejects it, at multiple ruin values.
- **The spawner** (main.c): mints the group — empty parent + the four sub-ref objects
  with identical params, materials assigned (stone albedo set, roof, glass, pavement;
  the texture pipeline already exists), pinnacle instance buffer filled from plan
  stations. A dev command (the codex-spawn precedent): "mint a church on this island,"
  seed from the mint counter.

**Acceptance.** A basilica on a default 32 m island: stands level, foundation reads
correctly on the slope, portal steps climb, every doorway passable, every wall solid,
walking the ruined nave puts grass underfoot and sky overhead; save/reload reproduces
the building byte-identically (params round-trip; the cache key dedups); `coltest`-
style agreement suite green.

**Decisions flagged:** datum = max vs 90th-percentile of pier samples; whether glass
panels get colliders (recommend no — ruins want walk-through window voids, intact
windows sit above reach).

---

### Item 10 — The follies capstone: standalone pieces & the abbey island

**Intent.** The phase's stated heart was never only churches — it was **follies**:
architectural pieces composed freely on islands. The capstone turns the kit's internals
into registry vocabulary and dresses the demo island that proves the phase.

**Spec.**
- **Standalone registry rows** (each a thin wrapper over machinery items 1–8 built;
  every one ruin-capable via its own truncation param):
  - `column { h, style, broken, seed }` — base/shaft/capital; `broken > 0` truncates
    with the jagged cap. The "three broken columns on a hillside" piece.
  - `arch_frag { span, acute, depth, h, ruin }` — a freestanding arched wall fragment
    (two jambs + head, or one jamb + a sprung half-arch at high ruin).
  - `stair { w, rise, run, steps }` — risers + stringers; colliders via the treaty.
  - `pinnacle { h, seed }` — Item 7's, now placeable alone.
  - `balustrade { len, h, seed }` — `PROF_MULLION` balusters (instanced) under a rail
    sweep; the cloister-walk and parapet dressing.
  - `cross { h }` — a churchyard cross: steps, socket, shaft, cross-head. Small, and
    the islands want it.
- **The composition demo** (main.c, the phase's portfolio): one large island — a
  0.55-ruin hall church, the churchyard cross, a broken colonnade descending the hill,
  grass-tuft instancing in the roofless bays (TODO4 item 3's scatter demo, re-aimed),
  candle sconces in the intact choir (P4 item 5's lights), dusk IBL with the glass
  emissive composition. This is the screenshot that has been the point since the
  reference-games list went into NOTES.stml.
- **The numbers**: HUD-verified budgets — the demo island under ~400k total triangles,
  instanced ornament in single-digit draws, load-to-interactive under 2 s (all emitters
  are pure CPU; if a basilica build exceeds ~80 ms, profile before optimizing — §1.7 of
  TODO4 still binds).
- **Docs**: a `GOTHIC.md` — the lane enum, the threshold table, the profile table, the
  plan invariants, and a glossary cross-reference (§1.6 made the code self-describing;
  the doc is the index).

**Acceptance.** The demo island exists as a saved scene, loads byte-identically, walks
end-to-end (terrain → steps → nave → choir) with collision honest throughout; every
standalone ref places and ruins independently; gothictest's full suite (determinism,
invariants, monotonicity, dependency, agreement) green across the seed sweep; the
screenshot.

---

## Part 4 — Decisions reserved to the project owner

1. **Profile set & tessellation** (Item 1) — *RULED 2026-06-11*: the six profiles as
   listed; `GOTHIC_MAX_SEG` 0.25 m plus the `GOTHIC_MAX_ANG` 22.5° angular cap.
2. **Style ranges / area thresholds; `style` as override vs derive-only** (Item 3).
3. **Tas-de-charge springer blocks vs ribs-die-on-abacus** (Item 5) — v1 recommends the
   simple termination; the solid springer is the flagged refinement.
4. **Cusping in v1 tracery** (Item 6) — geometry is ready; budget is the question.
5. **Glass: opaque-glossy + dusk emissive vs buying renderer transparency** (Item 6) —
   recommend opaque this phase; transparency is an RHI/pipeline feature with §1.4-of-
   TODO4 (Metal debt) implications and deserves its own deliberation.
6. **Roof material identity** (lead grey vs slate vs tile — a palette choice, Item 7).
7. **Ruin threshold table tuning & rubble scope** (Item 8) — values are taste; the
   *ordering* is structural and not really negotiable.
8. **Datum policy and glass colliders** (Item 9).
9. **Whether `church` should also mint a bare `cloister` variant this phase** (a square
   court + arcaded walk is ~90% existing machinery) or defer (Part 5 default: defer).

## Part 5 — Deliberately deferred (not this phase)

- **Mesh booleans / CSG** — permanently, per the standing law; restated for emphasis.
- **Flowing (Curvilinear) and Perpendicular tracery; rose windows; fan vaults** — the
  Geometric grammar and quadripartite vault are v1; later styles are table/grammar
  extensions on the same machinery, and they deserve their own item when wanted.
- **Sexpartite vaults, crypts, galleries/triforium passages as walkable volumes** — the
  triforium band is elevation articulation only this phase.
- **Figure sculpture** (tympanum reliefs, statuary, gargoyles-as-sculpture) — a
  different artform; the slots (tympanum slab, label stops, buttress niches) exist.
- **Interior furnishing** — altar, ambo, choirscreen, antependium: the glossary's
  liturgical layer. Wants the church first; a natural Phase 7 with the app-engine books
  (a sacristy inventory is a very Solarium idea).
- **Roof carpentry / visible trusses** in ruin interiors.
- **LOD / imposters** — one detail level; the budgets in Item 10 are sized for it.
- **Towns, walls, secular buildings** — the kit's machinery generalizes (the *alcázar*
  is sitting right there in the glossary), later.
- **Bells, sound** — the synth book exists; a tolling bell is one saved patch away,
  but composition belongs to the app-engine phase.

The cross-cutting disciplines pinned in this Constitution — the plan as one author
(§1.2), lanes not streams (§1.3), around-the-gap with curves (§1.4), structure before
ornament (§1.5), the glossary as namespace (§1.6) — are pinned because they are painful
to retrofit, which has been the project's own rule since Phase 2. The phase ends where
the project's heart always was: an island, a ruin, grass in the nave, and every stone of
it the deterministic consequence of eight floats.
