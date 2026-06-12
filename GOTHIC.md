# GOTHIC.md — the kit's index

Phase 6 ("The Gothic Kit", TODO6.md) built procedural Gothic architecture
as pure-CPU registry vocabulary. The code is self-describing (§1.6 — the
glossary is the namespace); this document is the **index**, not a manual.
Every table here restates a table in the source; the source wins.

## The constitution (TODO6.md part 1, abridged)

- **§1.2 — one author.** `church_plan()` is to the church what
  `terrain_height` is to the island: a pure, deterministic expansion of
  the ref params into structural truth. The stone emitter, the glass
  emitter, the colliders, the ruin pass — every one a READER. There is
  no stored plan.
- **§1.3 — lanes, not a stream.** Every architectural decision draws
  `gothic_hash01(seed, LANE, i, j)` from a NAMED lane. The lane enum is
  APPEND-ONLY: reordering is a save-breaking change.
- **§1.4 — around the gap, with curves.** Openings are emitted as jambs,
  reveals, and heads off the arch polyline. Mesh booleans / CSG are
  permanently banned.
- **§1.5 — ruin and built are queries.** `church_survives(plan, elem,
  i, j, &keep)` — there is no ruin pass; emitters ask before emitting,
  and the same seed keeps its exact collapse forever.
- **§1.8 — bit-determinism.** Same params, memcmp-identical meshes
  (`gothictest` holds every ref to it).

## The lane enum (gothic.h, append-only)

| lane | decided by it |
|---|---|
| `LANE_STYLE` | hall vs basilica |
| `LANE_MODULE` | bay length jitter about nave_w/2 |
| `LANE_NAVE_W` | nave width within the style range |
| `LANE_APSE` | polygonal vs flat east end |
| `LANE_TOWER` | west tower bay |
| `LANE_ELEV` | elevation formula scalars (j = which) |
| `LANE_TOWER_H` | tower height |
| `LANE_PITCH` | roof pitch |
| `LANE_TRACERY` | lights divisor jitter |
| `LANE_RUIN` | the decay field |
| `LANE_RUIN_DIR` | which end collapses first |
| `LANE_SPIRE` | broach vs parapet-and-needle |
| `LANE_FOLLY` | standalone pieces' variation |

## The ruin threshold ladder (gothic.c `RUIN_T`, fragility-ordered)

Pressure = `ruin * (0.35 + 0.65 * (0.55*noise + 0.45*gradient))`. An
element falls when pressure crosses its threshold; **within a cell the
ladder order alone decides** (same pressure everywhere in the cell), and
the dependency graph binds across cells (a web cannot outlive its ribs).

| element | threshold | note |
|---|---|---|
| ROOF | 0.12 | first to go |
| WEB | 0.28 | vault cells; the floor query reads it too |
| GLASS | 0.30 | |
| TRACERY | 0.42 | outlives its glass: the empty east frame |
| CLEREST | 0.45 | |
| RIB | 0.50 | partial: springer stubs on standing piers |
| FLYER | 0.55 | |
| SPIRE | 0.58 | ladder position by VALUE (enum appended) |
| ARCADE | 0.62 | |
| PINNACLE | 0.65 | |
| WALL | 0.70 | partial, course-quantized; lower courses hold to 0.95 |
| PIER | 0.80 | partial: the broken column |
| BUTTRESS | 0.80 | pier-tough |

Partial keeps are quantized to the **0.4 m course** (`quantize_keep`) —
broken walls snap to masonry joints. The same constant is texgen's
default `course` knob, so the synthesized mortar lines land where ruined
walls actually break.

`built` runs the same query against east-to-west construction stages
(choir first); the masks compose by intersection.

## The profile table (gothic.h `PROF_*`, used by `molding` and the sweeps)

| profile | section | open/closed |
|---|---|---|
| `PROF_RIB` | chamfered vault rib, roll between fillets | open |
| `PROF_MULLION` | double-chamfer window bar | closed |
| `PROF_STRING` | cavetto over roll, weathered top | open |
| `PROF_BASE` | attic base: torus, scotia, torus | open |
| `PROF_HOOD` | hood-mold drip | open |
| `PROF_SHAFT_OCT` | octagonal shaft section | closed |

Sweeps tessellate by the **two caps**: 0.25 m max chord
(`GOTHIC_MAX_SEG`) and 22.5° max turn (`GOTHIC_MAX_ANG`), whichever is
finer, dust-epsilon ceil. Joints turning > ~30° auto-crease.

## The arch family (one float)

Acuteness `a = 2c/s`: 0 = semicircular, 1 = equilateral, > 1 = lancet.
The **level-crown solve** `gothic_arch_acuteness_for(s, H)` is the
formula Gothic exists for — arches of different spans meeting at one
crown. It pays everywhere: vault ribs, window flattening-to-fit, web
lunettes, the arch fragment folly.

## Plan invariants (held across 200 seeds in `gothictest`)

- The module is ad-quadratum; the east-first remainder lets a deep
  chevet eat bays to fit; the apse mouth equals the nave width exactly.
- Elevation derives from the arch math; halls have `aisle_h == wall_h`.
- **Vault closure:** `wall_h >= spring + q/2` — the wall is always tall
  enough for its diagonal rib's round crown.
- `plan_opening` returns openings that ALWAYS fit (flatten via the
  level-crown solve, drop springing, narrow — in that order).
- Returned plans are scalars-only, memset-zeroed: memcmp is identity.

## The registry vocabulary (mesh.c rows)

**The church** — a GROUP of four refs sharing one 8-param schema
`{w, d, seed, style, ruin, built, acute, reserved}`:
`church_stone`, `church_glass`, `church_roof`, `church_floor`.
Defaults are asserted equal to `gothic_church_defaults`.

**The follies (item 10)** — standalone vocabulary; ruin is a per-piece
truncation param (a single piece has no dependency graph). All stand on
their local origin with the 2.5 m `GOTHIC_FOUNDATION` skirt below grade,
and all have colliders that read the same truncation formulas
(`gothic_column_top`, `gothic_arch_frag_dims`):

| ref | params | note |
|---|---|---|
| `molding` | prof, len, scale, bend, vert | item 1's first row |
| `wall_arched` | w, h, ox, ow, spring, acute, t | |
| `portal` | w, h, t, ow, spring, acute, orders, step | recessed orders |
| `pinnacle` | h, seed | Roriczer simplified |
| `column` | h, style, broken, seed | style 1 = bare drum; broken plants the snapped core |
| `arch_frag` | span, acute, depth, h, ruin | one jamb + sprung half-arch at high ruin |
| `stair` | w, rise, run, steps | every tread standable (the treaty) |
| `balustrade` | len, h, seed, ruin | balusters are the instance pool's |
| `cross` | h | steps, socket, tapered shaft, head |

**The instanced ornament**: balusters render as ONE canonical unit mesh
(`gothic_baluster_unit`, height exactly 1) through a per-instance PBR
pipeline — one draw per balustrade, shadow twin included.
`gothic_balusters` enumerates the slots (monotone in ruin: a baluster
lost at 0.3 stays lost at 0.6); the sill/rail constants live in gothic.h
so carcass and pool cannot disagree.

## Materials (the texture side-quest)

Synthesized stone/plaster maps via texgen.c: `<tex kind="stone" .../>`
beside an object's `<mat>`; knobs in materials.stml on the watcher.
The height field is the one author — normal, albedo, and ORM are
readers. Course default 0.4 m: see the threshold ladder above.

## Keys (the demo vocabulary)

- **J** — floor-plan overlay on the island underfoot
- **U** — mint the island's church; press again to walk the ruin dial
  (0 → .3 → .6 → .9 → whole)
- **Z** — compose THE ABBEY: a large island ahead, hall church at ruin
  0.55, churchyard cross, broken colonnade, porch balustrades, choir
  sconces — and the night sky falls (rogland HDR + full IBL re-bake)

## Deliberately deferred (TODO6.md part 5)

CSG (permanently); curvilinear/perpendicular tracery, rose windows, fan
vaults; sexpartite vaults, crypts, walkable galleries; figure sculpture;
interior furnishing (a Phase 7 with the app-engine books); visible roof
carpentry; LOD; towns and secular buildings; the cloister (reserved
decision #9, ruled: defer).
