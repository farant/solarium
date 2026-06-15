/* flora.h — the living island's plans (P7 item 2). tree_plan is to a
   tree what church_plan is to the church (§1.2, organism edition): a
   pure, deterministic, allocation-bounded expansion of (species, seed,
   age) into the BRANCH GRAPH — and everything else is a reader of it:
   the wood emitter sweeps its segments (item 3), the canopy instances
   its tips (item 4), the collider boxes its trunk, the wind sways what
   it lists. Same seed, same tree, forever.

   Pure CPU, strict C89, headless-testable; no rhi/scene includes. */
#ifndef FLORA_H
#define FLORA_H

#include "sol_types.h"   /* vec3 */
#include "mesh.h"        /* MeshBuilder (the wood emitter, item 3) */

#define FLORA_PARAMS  16     /* one shared knob vector (species = presets) */
#define FLORA_MAX_SEG 256    /* the arena cap: a budget enforced, not hoped */
#define FLORA_MAX_GENS 7

/* species are PRESETS over the one schema (the synth lesson, fourth
   verse): a shrub is a species whose trunk knobs shrink, not new code */
enum { FLORA_OAK = 0, FLORA_PINE, FLORA_BIRCH, FLORA_CYPRESS,
       FLORA_SPECIES_COUNT };

/* lanes — APPEND-ONLY (the gothic law). Keys are (node_id, ordinal):
   node identity, never a sequential stream, so adding a knob can never
   reshuffle an existing tree — and the AGE LAW holds (a sapling's
   topology is a PREFIX of the elder tree's, same seed). */
enum {
    LANE_FLORA_SPLIT = 0,   /* fork count jitter                  */
    LANE_FLORA_AZIMUTH,     /* azimuth jitter about the golden angle */
    LANE_FLORA_LENGTH,      /* segment length jitter              */
    LANE_FLORA_PITCH,       /* spread angle jitter                */
    LANE_FLORA_LEAN,        /* the whole tree's lean              */
    LANE_FLORA_COUNT
};

/* flora owns its hash twin: no other module's change may regrow a wood */
float flora_hash01(unsigned seed, int lane, int i, int j);

int         flora_species(const char *name);        /* -1 unknown */
const char *flora_species_name(int species);        /* NULL out of range */

/* The knob schema: names shared across species, defaults per species.
   {seed, age, height, girth, apical, splits, spread, droop, twist,
    taper, decay, gens, jitter, leaf_size, leaf_density, reserved}
   — leaf knobs are item 4's, reserved now so the schema never grows.
   Returns FLORA_PARAMS, or -1 for an unknown species. */
int flora_schema(int species, const char *const **names,
                 const float **defaults);

/* One branch segment. Radii obey DA VINCI'S RULE by construction: at
   every fork the children's cross-sections sum to the parent's end
   cross-section (the leader takes the apical share, laterals split the
   rest) — radii are DERIVED, never authored. tip marks twig ends (the
   canopy's seats). Base local space, y up, trunk base at the origin. */
typedef struct {
    vec3          p0, p1;
    float         r0, r1;
    short         parent;       /* segment index; -1 = the root */
    unsigned char depth;        /* generation                   */
    unsigned char tip;
} FloraSeg;

/* The plan. params is a PREFIX over the species defaults (the mesh-ref
   rule; seed and age ride the vector as knobs 0 and 1). Fills out (at
   most max_seg segments, BFS generation order — the cap truncates the
   YOUNGEST growth first and never orphans a child) and returns the
   count. 0 on a bad species or arena. */
int flora_tree_plan(int species, const float *params, int count,
                    FloraSeg *out, int max_seg);

/* ---- item 3: the wood ----
   THE SAUSAGE RULING: every segment is its own 2-station tapered sweep
   — a straight line is planar by definition, so the lathe's planarity
   law holds with zero new machinery. Children sink slightly INTO their
   parents (the overlap hides the seam under bark); the trunk sinks
   FLORA_ROOT_SINK below origin so trees plant convincingly on slopes
   (the foundation-skirt law, organically). Caps at tips only. The
   profile is flora's own uncreased unit octagon — roll normals read
   round; the one inherent seam (the closed profile's endpoint pair)
   hides under bark, flagged for the 3D-frame refinement. UVs come from
   the lathe world-scale: ridges run with the grain trunk-to-twig free. */
#define FLORA_ROOT_SINK 0.45f
void flora_tree_wood(MeshBuilder *b, int species, const float *params,
                     int count);

/* the trunk the COLLIDERS read (one formula, two readers — the §1.2
   law): radius at the base and the top of the climbable mass (the
   thickest-child chain walked while it still blocks a capsule) */
void flora_trunk_dims(int species, const float *params, int count,
                      float *r, float *top);

/* ---- item 4: the canopy ----
   Leaves read the TIPS the branch graph already flagged (§1.2 again).
   One cluster seat per tip; leaf_density (knob 9) GATES tips by a
   per-tip hash threshold — MONOTONE, a tip bared at 0.3 stays bared at
   0.6 (the baluster membership law, the ruin-dial of trees). The
   canopy is per-instance data over a unit leaf mesh: the item-10
   ornament pool is its renderer (one draw per tree). */
typedef struct {
    vec3  pos;      /* the tip, in the tree's base frame */
    float yaw;      /* golden-angle splay about Y        */
    float scale;    /* leaf_size x per-tip jitter        */
    float phase;    /* sway phase (item 4 wind down-payment) */
} FloraLeaf;

/* fill out (at most max) with one seat per surviving tip; returns the
   count. params is the species-default-merged prefix. */
int flora_canopy(int species, const float *params, int count,
                 FloraLeaf *out, int max);

/* which unit mesh a species wears: 0 = broadleaf puff, 1 = conifer
   spray. Cluster SHAPE is species data, not new machinery. */
enum { FLORA_LEAF_BROAD = 0, FLORA_LEAF_CONIFER };
int  flora_leaf_kind(int species);

/* one unit leaf cluster, base at y=0 growing up (the sway pins the
   root), double-sided cards. leaf_kind picks puff vs spray. */
void flora_leafcard_unit(MeshBuilder *b, int leaf_kind);

/* the canopy's green (linear), per species — the leaf material's base
   color (the wood wears bark; the leaves can't inherit it) */
vec3 flora_leaf_color(int species);

#endif /* FLORA_H */
