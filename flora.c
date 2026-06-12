/* flora.c — tree_plan: the branch graph (35th TU; P7 item 2).

   THE GROWTH MODEL — botany-lite, but the causal parts:
   1) APICAL DOMINANCE, one float: high = the leader persists and
      laterals ring off it (pine, cypress — monopodial); low = the axis
      forks and forks (oak — sympodial). The knob is the leader's SHARE
      of the parent's cross-section (< 0.25 = no leader at all).
   2) DA VINCI'S RULE: children's cross-sections sum to the parent's —
      r_child = r_parent * sqrt(share), shares summing to one. Radii
      DERIVE from conservation flowing down from the trunk; the same law
      SELF-LIMITS growth (a pine's whorl twigs thin below the floor in
      two generations — no explosion, no special case).
   3) GRAVITROPISM: a signed droop blends branch direction toward or
      away from gravity, deeper generations more — old pine boughs sag,
      cypress sweeps up.

   THE AGE LAW: age scales lengths, radii, AND the generation count;
   branching decisions are lane-keyed by NODE IDENTITY (id = parent*8 +
   ordinal+1), so a sapling's topology is a PREFIX of the elder tree's —
   same seed, same tree, younger (the codex pattern gains a time axis).
   Buds expand BFS, so the arena cap also truncates youngest-first. */

#include "flora.h"
#include "sol_math.h"

#include <math.h>
#include <string.h>

/* ---- the schema: ONE shared vector, species are presets ---- */

static const char *const fl_names[FLORA_PARAMS] = {
    "seed",     /* variation selector                          */
    "age",      /* 1 = mature: scales size AND generations     */
    "height",   /* trunk-to-crown reach at age 1, meters       */
    "girth",    /* trunk radius at the base at age 1, meters   */
    "apical",   /* leader's share of the fork (< 0.25 = none)  */
    "splits",   /* lateral children per fork                   */
    "spread",   /* lateral pitch off the parent axis, degrees  */
    "droop",    /* gravitropism, signed: + sags, - sweeps up   */
    "twist",    /* azimuth advance per generation, degrees     */
    "taper",    /* radius retained along one segment           */
    "decay",    /* lateral length / parent length              */
    "gens",     /* generations at age 1 (capped FLORA_MAX_GENS)*/
    "jitter",   /* randomness amplitude 0..1                   */
    "leaf_size",     /* item 4's (reserved now: schema growth  */
    "leaf_density",  /*  is a save-format event)               */
    "reserved"
};

static const float fl_oak[FLORA_PARAMS] = {
    0.0f, 1.0f, 7.0f, 0.28f, 0.15f, 3.0f, 42.0f, 0.10f,
    25.0f, 0.80f, 0.72f, 5.0f, 0.55f, 0.55f, 0.8f, 0.0f
};
static const float fl_pine[FLORA_PARAMS] = {
    0.0f, 1.0f, 11.0f, 0.26f, 0.92f, 4.0f, 68.0f, 0.60f,
    12.0f, 0.88f, 0.38f, 6.0f, 0.30f, 0.45f, 0.7f, 0.0f
};
static const float fl_birch[FLORA_PARAMS] = {
    0.0f, 1.0f, 9.0f, 0.16f, 0.70f, 2.0f, 32.0f, -0.05f,
    40.0f, 0.82f, 0.68f, 6.0f, 0.45f, 0.40f, 0.6f, 0.0f
};
static const float fl_cypress[FLORA_PARAMS] = {
    0.0f, 1.0f, 7.0f, 0.20f, 0.90f, 5.0f, 24.0f, -0.55f,
    30.0f, 0.86f, 0.32f, 6.0f, 0.25f, 0.35f, 0.9f, 0.0f
};

static const char *const fl_species_names[FLORA_SPECIES_COUNT] = {
    "oak", "pine", "birch", "cypress"
};
static const float *const fl_defaults[FLORA_SPECIES_COUNT] = {
    fl_oak, fl_pine, fl_birch, fl_cypress
};

int flora_species(const char *name) {
    int i;
    if (!name) return -1;
    for (i = 0; i < FLORA_SPECIES_COUNT; i++)
        if (strcmp(name, fl_species_names[i]) == 0) return i;
    return -1;
}

const char *flora_species_name(int species) {
    if (species < 0 || species >= FLORA_SPECIES_COUNT)
        return (const char *)0;
    return fl_species_names[species];
}

int flora_schema(int species, const char *const **names,
                 const float **defaults) {
    if (species < 0 || species >= FLORA_SPECIES_COUNT) return -1;
    if (names)    *names = fl_names;
    if (defaults) *defaults = fl_defaults[species];
    return FLORA_PARAMS;
}

/* ---- flora's own noise twin ---- */

static sol_u32 fl_mix(sol_u32 h) {
    h ^= h >> 16; h *= 0x21f0aaadU;
    h ^= h >> 15; h *= 0x735a2d97U;
    h ^= h >> 15;
    return h;
}

float flora_hash01(unsigned seed, int lane, int i, int j) {
    sol_u32 h = (sol_u32)seed * 0x9e3779b9U + (sol_u32)lane * 0x85ebca6bU;
    h ^= (sol_u32)i * 0xc2b2ae35U;
    h  = fl_mix(h);
    h ^= (sol_u32)j * 0x27d4eb2fU;
    h  = fl_mix(h);
    return (float)(h & 0xffffffU) / 16777216.0f;
}

/* ---- the plan ---- */

/* a bud waiting to grow: everything one segment needs to exist */
typedef struct {
    vec3  pos, dir;
    float len, r;
    int   node_id;      /* stable identity: parent*8 + ordinal + 1 */
    short parent_seg;
    unsigned char depth;
} FloraBud;

#define FLORA_RADIUS_FLOOR 0.008f
#define FLORA_LENGTH_FLOOR 0.05f
#define FLORA_GOLDEN_DEG   137.50776f

/* an orthonormal frame around d (deterministic fallback axis) */
static void fl_frame(vec3 d, vec3 *t1, vec3 *t2) {
    vec3 ref = (d.y > 0.9f || d.y < -0.9f) ? vec3_make(1.0f, 0.0f, 0.0f)
                                           : vec3_make(0.0f, 1.0f, 0.0f);
    *t1 = vec3_normalize(vec3_cross(ref, d));
    *t2 = vec3_cross(d, *t1);
}

int flora_tree_plan(int species, const float *params, int count,
                    FloraSeg *out, int max_seg) {
    float        p[FLORA_PARAMS];
    const float *defs;
    FloraBud     queue[FLORA_MAX_SEG];
    int          q_head = 0, q_tail = 0, n_seg = 0;
    unsigned     seed;
    float        age, height, girth, apical, spread, droop, twist,
                 taper, decay, jitter, sizef;
    int          splits, gens, k;

    if (flora_schema(species, (const char *const **)0, &defs) < 0)
        return 0;
    if (!out || max_seg < 1) return 0;
    for (k = 0; k < FLORA_PARAMS; k++)
        p[k] = (params && k < count) ? params[k] : defs[k];

    seed   = (unsigned)(sol_u32)(long)floorf(p[0] + 0.5f);
    age    = p[1];
    if (age < 0.15f) age = 0.15f;
    if (age > 2.0f)  age = 2.0f;
    sizef  = powf(age, 0.75f);          /* size grows sublinearly */
    height = p[2] * sizef;
    girth  = p[3] * sizef;
    apical = p[4];
    splits = (int)(p[5] + 0.5f);
    if (splits < 1) splits = 1;
    if (splits > 6) splits = 6;
    spread = p[6] * (SOL_PI / 180.0f);
    droop  = p[7];
    twist  = p[8] * (SOL_PI / 180.0f);
    taper  = p[9];
    if (taper < 0.5f)  taper = 0.5f;
    if (taper > 0.98f) taper = 0.98f;
    decay  = p[10];
    jitter = p[12];
    gens   = (int)(p[11] * (age < 1.0f ? age : 1.0f) + 0.5f);
    if (gens < 1) gens = 1;
    if (gens > FLORA_MAX_GENS) gens = FLORA_MAX_GENS;

    {   /* the root bud: up, with the tree's lean (birches lean). The
           first reach SOLVES the dominant chain's geometric series so
           the height knob is the tree's true reach, not a lie the
           leader compounds past (an 11 m pine is 11 m tall). */
        FloraBud *b = &queue[q_tail++];
        float la = (flora_hash01(seed, LANE_FLORA_LEAN, 0, 0) - 0.5f)
                   * jitter * 0.22f;
        float lz = (flora_hash01(seed, LANE_FLORA_LEAN, 0, 1) - 0.5f)
                   * jitter * 0.22f;
        float f  = (apical >= 0.25f) ? (0.62f + 0.30f * apical) : decay;
        float series;
        if (f > 0.98f) f = 0.98f;
        series = (1.0f - powf(f, (float)(gens + 1))) / (1.0f - f);
        b->pos = vec3_make(0.0f, 0.0f, 0.0f);
        b->dir = vec3_normalize(vec3_make(la, 1.0f, lz));
        b->len = height / series;
        b->r   = girth;
        b->node_id    = 0;
        b->parent_seg = -1;
        b->depth      = 0;
    }

    while (q_head < q_tail && n_seg < max_seg) {
        FloraBud  bud = queue[q_head++];
        FloraSeg *s   = &out[n_seg];
        int       seg_index = n_seg;
        int       grow;
        s->p0     = bud.pos;
        s->p1     = vec3_add(bud.pos, vec3_scale(bud.dir, bud.len));
        s->r0     = bud.r;
        s->r1     = bud.r * taper;
        s->parent = (short)bud.parent_seg;
        s->depth  = (unsigned char)bud.depth;
        s->tip    = 0;                  /* the post-pass decides */
        n_seg++;

        grow = bud.depth + 1 <= gens
            && s->r1 >= FLORA_RADIUS_FLOOR
            && bud.len * decay >= FLORA_LENGTH_FLOOR;
        if (grow) {
            int   nlat = splits, c, spawned = 0;
            float leader = (apical >= 0.25f) ? apical : 0.0f;
            float lat_share, jl;
            jl = flora_hash01(seed, LANE_FLORA_SPLIT, bud.node_id, 0);
            if (jitter > 0.0f && jl < jitter * 0.4f && nlat > 1) nlat--;
            else if (jitter > 0.0f && jl > 1.0f - jitter * 0.25f) nlat++;
            if (nlat > 6) nlat = 6;
            lat_share = (1.0f - leader) / (float)nlat;

            if (leader > 0.0f && q_tail < FLORA_MAX_SEG) {
                FloraBud *b = &queue[q_tail++];
                float jx = (flora_hash01(seed, LANE_FLORA_PITCH,
                                         bud.node_id, 7) - 0.5f)
                           * jitter * 0.20f;
                float jz = (flora_hash01(seed, LANE_FLORA_PITCH,
                                         bud.node_id, 8) - 0.5f)
                           * jitter * 0.20f;
                b->pos = s->p1;
                b->dir = vec3_normalize(vec3_add(bud.dir,
                                                 vec3_make(jx, 0.0f, jz)));
                b->len = bud.len * (0.62f + 0.30f * apical);
                b->r   = s->r1 * sqrtf(leader);
                b->node_id    = bud.node_id * 8 + 1;
                b->parent_seg = (short)seg_index;
                b->depth      = (unsigned char)(bud.depth + 1);
                spawned = 1;
            }
            for (c = 0; c < nlat && q_tail < FLORA_MAX_SEG; c++) {
                FloraBud *b = &queue[q_tail++];
                vec3  t1, t2, d;
                float az, pitch, dr, rl;
                rl = s->r1 * sqrtf(lat_share);
                if (rl < FLORA_RADIUS_FLOOR * 0.75f) { q_tail--; continue; }
                az = FLORA_GOLDEN_DEG * (SOL_PI / 180.0f) * (float)c
                   + twist * (float)bud.depth
                   + (flora_hash01(seed, LANE_FLORA_AZIMUTH,
                                   bud.node_id, c) - 0.5f)
                     * jitter * 1.2f;
                pitch = spread
                      + (flora_hash01(seed, LANE_FLORA_PITCH,
                                      bud.node_id, c) - 0.5f)
                        * jitter * 0.5f;
                fl_frame(bud.dir, &t1, &t2);
                d = vec3_add(vec3_scale(bud.dir, cosf(pitch)),
                             vec3_add(vec3_scale(t1, sinf(pitch) * cosf(az)),
                                      vec3_scale(t2, sinf(pitch) * sinf(az))));
                /* gravitropism weights by HEIGHT ON THE TREE, not by
                   generation: a pine's lowest boughs sag hardest, a
                   cypress sweeps up hardest at its base — true for
                   every species at once */
                dr = 1.0f - s->p1.y / height;
                if (dr < 0.15f) dr = 0.15f;
                if (dr > 1.0f)  dr = 1.0f;
                dr *= droop;
                d  = vec3_normalize(vec3_add(d, vec3_make(0.0f, -dr, 0.0f)));
                b->pos = s->p1;
                b->dir = d;
                b->len = bud.len * decay
                       * (1.0f + (flora_hash01(seed, LANE_FLORA_LENGTH,
                                               bud.node_id, c) - 0.5f)
                                 * jitter * 0.5f);
                b->r   = rl;
                b->node_id    = bud.node_id * 8 + 2 + c;
                b->parent_seg = (short)seg_index;
                b->depth      = (unsigned char)(bud.depth + 1);
                spawned = 1;
            }
            (void)spawned;
        }
    }

    {   /* tips by POST-PASS: a segment no child points at is a twig end.
           Correct even under arena truncation — a branch whose children
           the cap cut IS the canopy's seat now. */
        for (k = 0; k < n_seg; k++)
            if (out[k].parent >= 0) out[out[k].parent].tip = 2;
        for (k = 0; k < n_seg; k++)
            out[k].tip = (out[k].tip == 2) ? 0 : 1;
    }
    return n_seg;
}
