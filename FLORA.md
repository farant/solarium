# FLORA.md — the living island's index

Phase 7 ("The Living Island", TODO7.md) gave the islands life: trees,
rock, undergrowth, water, and one wind. Like GOTHIC.md, this is the
**index**, not a manual — every table restates one in the source; the
source wins.

## The two theses

- **A tree is a function.** `flora_tree_plan(species, seed, age)` expands
  into a flat **branch graph**; the wood emitter sweeps its segments, the
  canopy instances its tips, the collider boxes its trunk, the wind sways
  what it lists — every consumer a reader of one deterministic plan (the
  §1.2 law from gothic, at organism scale). Same seed, same tree, forever.
- **Populations are data, not objects.** A forest is not 500 scene
  objects; it is a few shared variant meshes and a few instance buffers
  derived from the island's seed — the meadow's law promoted. Only HERO
  plants are scene vocabulary.

## The HERO / FIELD tier law (§1.3)

| tier | what it is | placed how | pickable? | collider? |
|---|---|---|---|---|
| HERO | a scene object with a ref | hand / composer / mint key | yes | trunk / boulder box |
| FIELD | derived per-island scatter | the island seed, never saved | no (LAND policy) | ghost (v1) |

A `tree`/`shrub`/`boulder`/`pond` ref is HERO. The forest, scree, and
meadow are FIELD — rebuilt on load/mint, swept never saved, riding the
island's visibility bit, drawn through the ornament PBR pipeline (wood +
canopy) or the meadow pipeline (grass + flowers).

## The flora knob schema (flora.h — 16 knobs, species are presets)

The synth lesson, fourth verse: one schema, a species is a default
vector, a shrub is a species whose trunk knob shrinks (§1.4).

| # | knob | meaning |
|---|---|---|
| 0 | seed | variation selector |
| 1 | age | 1 = mature; scales size AND generation count |
| 2 | height | trunk-to-crown reach at age 1 (m) |
| 3 | girth | trunk base radius at age 1 (m) |
| 4 | apical | the leader's SHARE of the fork (< 0.25 = pure forking) |
| 5 | splits | lateral children per fork |
| 6 | spread | lateral pitch off the parent axis (deg) |
| 7 | droop | gravitropism, signed (+ sags, − sweeps up) |
| 8 | leaf_size | cluster scale at each tip |
| 9 | leaf_density | the shedding dial (monotone; 1 full, 0 winter) |
| 10 | twist | azimuth advance per generation (deg) |
| 11 | taper | radius retained along one segment |
| 12 | decay | lateral length / parent length |
| 13 | gens | generations at age 1 (capped FLORA_MAX_GENS 7) |
| 14 | jitter | randomness amplitude 0..1 |
| 15 | reserved | — |

The tree **registry refs expose the first 10** (silhouette + leaf knobs);
the deep structural knobs stay species-fixed. Leaf knobs ride at 8/9 so
the contiguous-prefix rule reaches `leaf_density` (the reorder was free
exactly once — at item 4, before any tree persisted a deep knob).

**Three causal growth rules:** apical dominance (one float spans oak↔
cypress), **da Vinci's rule** (child cross-sections sum to the parent's —
radii derive, and the law self-limits growth), gravitropism by HEIGHT on
the tree. **The age law**: a sapling's topology is a bitwise PREFIX of the
elder's, same seed.

## Species (flora.c presets)

| ref | character | leaf |
|---|---|---|
| `oak` | low apical, wide forking crown, stout | broadleaf |
| `pine` | high apical, drooping whorls | conifer spray |
| `birch` | slender, leaning, fine twigs | broadleaf |
| `cypress` | columnar, sweeping up | conifer spray |
| `shrub` | trunk ~0 (apical 0.05), low & dense | broadleaf |

## The flora lanes (flora.h — APPEND-ONLY)

`flora_hash01(seed, lane, i, j)`, keyed by NODE IDENTITY so a new knob
never reshuffles an existing tree (the gothic lane law):
`LANE_FLORA_SPLIT, _AZIMUTH, _LENGTH, _PITCH, _LEAN`.

## The texgen kinds (texgen.h — the third "never sourced")

Textures are synthesized from knobs, zero image binaries. The height
field is the one author; normal/albedo/ORM are readers.

| kind | use | note |
|---|---|---|
| `stone` | masonry, boulders | course 0 = granular, no grid |
| `plaster` | smooth interior | stone with course 0 + trowel |
| `bark` | trunks | first ANISOTROPIC kind (ridges along v) |
| `water` | the pond ripples | only the normal is read; shader scrolls 2 copies |

## The other vocabulary

- `boulder {size, seed, flat}` — fBm-displaced octahedron, watertight by
  construction; `flat` → a standable table-rock.
- `pond {r, depth, seed}` — a disc the WATER PASS draws (fresnel × the
  prefilter IBL + scrolled ripples + rim fade); ghost to walking.
- The **ornament pool** (from gothic item 10) is the universal "instance
  a PBR mesh" tool: balusters, leaf clusters, FIELD tree wood, scree —
  all ride it.

## One wind (item 9)

`wind_at(t, x, z)` → a prevailing direction + a gust scalar, evaluated
once per frame at the camera. ONE GUST, FOUR SENSES: the meadow + canopy
sway (lean downwind, swell with the gust, travel as a wave), particle
drift, and the audio wind gain all read it — a gust visibly crosses the
island and you hear it rise.

## Budgets (item 10)

A dressed island stays interactive: forest ≤ 8 draws/island, the abbey
island < ~550k tris, load < 2 s, frame ~16.7 ms on the M2 in both
backends. `FOREST_PER_M2` is the thinning lever if it climbs.

## Keys

- **H** — mint a wild island (forests, rocks, a pond in its hollow)
- **Z** — compose the abbey (church + cross + colonnade + yew + orchard +
  pond + sconces, grown over, night falls)
- **Q** — mint a pond into the hollow ahead
- **U** church · **O** lantern · **E** dust · **B** board · **J** plan
  overlay · **L** reload · **K** bloom · **X** delete

## Deliberately deferred (TODO7 Part 5)

Full seasons & weather; birds & creatures (wander-in-3D, its own phase);
rivers / waterfalls / ocean (the pond is a plane); branch skeletal
animation; ivy on architecture (wants plan + tree_plan married); planar
water reflections; a day/night cycle (hdr_reload is the lever).
