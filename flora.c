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
#include "sweep.h"      /* the lathe: the wood rides it (item 3) */
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
    "leaf_size",     /* item 4: cluster scale at each tip      */
    "leaf_density",  /* item 4: the shedding dial (monotone)   */
    "twist",    /* azimuth advance per generation, degrees     */
    "taper",    /* radius retained along one segment           */
    "decay",    /* lateral length / parent length              */
    "gens",     /* generations at age 1 (capped FLORA_MAX_GENS)*/
    "jitter",   /* randomness amplitude 0..1                   */
    "reserved"
};

/* THE LEAF KNOBS RIDE AT 8/9 (P7 item 4): the registry's prefix rule is
   contiguous from knob 0, so a 10-param tree ref reaches leaf_density
   (the shedding dial) while the deep structural knobs stay species-
   fixed at 10-14. The reorder was free exactly once — before any tree
   persisted a knob past the 8th, which is this item. */
static const float fl_oak[FLORA_PARAMS] = {
    0.0f, 1.0f, 7.0f, 0.28f, 0.15f, 3.0f, 42.0f, 0.10f,
    0.55f, 0.8f, 25.0f, 0.80f, 0.72f, 5.0f, 0.55f, 0.0f
};
static const float fl_pine[FLORA_PARAMS] = {
    0.0f, 1.0f, 11.0f, 0.26f, 0.92f, 4.0f, 68.0f, 0.60f,
    0.45f, 0.7f, 12.0f, 0.88f, 0.38f, 6.0f, 0.30f, 0.0f
};
static const float fl_birch[FLORA_PARAMS] = {
    0.0f, 1.0f, 9.0f, 0.16f, 0.70f, 2.0f, 32.0f, -0.05f,
    0.40f, 0.6f, 40.0f, 0.82f, 0.68f, 6.0f, 0.45f, 0.0f
};
static const float fl_cypress[FLORA_PARAMS] = {
    0.0f, 1.0f, 7.0f, 0.20f, 0.90f, 5.0f, 24.0f, -0.55f,
    0.35f, 0.9f, 30.0f, 0.86f, 0.32f, 6.0f, 0.25f, 0.0f
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
    /* p[8] leaf_size, p[9] leaf_density — the canopy's, read by flora_canopy */
    twist  = p[10] * (SOL_PI / 180.0f);
    taper  = p[11];
    if (taper < 0.5f)  taper = 0.5f;
    if (taper > 0.98f) taper = 0.98f;
    decay  = p[12];
    jitter = p[14];
    gens   = (int)(p[13] * (age < 1.0f ? age : 1.0f) + 0.5f);
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

/* ================== item 3: the wood ================== */

/* flora's own profile: a unit octagon, UNCREASED (roll normals read
   round under bark), closed by repeating point 0. The endpoint pair is
   the lathe's one inherent normal seam — bark masks it; the 3D-frame
   sweep is the flagged refinement. */
static const ProfilePt FL_ROUND[9] = {
    { 1.0f,        0.0f,        0 },
    { 0.7071068f,  0.7071068f,  0 },
    { 0.0f,        1.0f,        0 },
    { -0.7071068f, 0.7071068f,  0 },
    { -1.0f,       0.0f,        0 },
    { -0.7071068f, -0.7071068f, 0 },
    { 0.0f,        -1.0f,       0 },
    { 0.7071068f,  -0.7071068f, 0 },
    { 1.0f,        0.0f,        0 }
};

void flora_tree_wood(MeshBuilder *b, int species, const float *params,
                     int count) {
    FloraSeg s[FLORA_MAX_SEG];
    int      n, i;
    if (!b) return;
    n = flora_tree_plan(species, params, count, s, FLORA_MAX_SEG);
    for (i = 0; i < n; i++) {
        vec3  d, t1, t2, path[2];
        float scales[2], len, sink;
        d   = vec3_sub(s[i].p1, s[i].p0);
        len = sqrtf(vec3_dot(d, d));
        if (len < 1e-6f) continue;
        d = vec3_scale(d, 1.0f / len);
        if (s[i].parent < 0) {
            sink = FLORA_ROOT_SINK;     /* planted, not perched */
        } else {
            sink = s[s[i].parent].r1 * 0.9f;
            if (sink > len * 0.3f) sink = len * 0.3f;
        }
        path[0] = vec3_sub(s[i].p0, vec3_scale(d, sink));
        path[1] = s[i].p1;
        scales[0] = s[i].r0;
        scales[1] = s[i].r1;
        fl_frame(d, &t1, &t2);
        (void)t2;
        sweep_extrude(b, FL_ROUND, 9, path, 2, t1, 1.0f, scales,
                      0, s[i].tip ? 1 : 0);
    }
}

void flora_trunk_dims(int species, const float *params, int count,
                      float *r, float *top) {
    FloraSeg s[FLORA_MAX_SEG];
    int      n, cur, i;
    float    block;
    if (r)   *r = 0.0f;
    if (top) *top = 0.0f;
    n = flora_tree_plan(species, params, count, s, FLORA_MAX_SEG);
    if (n < 1) return;
    if (r) *r = s[0].r0;
    block = s[0].r0 * 0.45f;        /* thinner than this stops nobody */
    cur = 0;
    for (;;) {                      /* the thickest-child chain */
        int   next = -1;
        float best = 0.0f;
        for (i = cur + 1; i < n; i++)
            if (s[i].parent == cur && s[i].r0 > best) {
                best = s[i].r0;
                next = i;
            }
        if (next < 0 || s[next].r0 < block) break;
        cur = next;
    }
    if (top) *top = s[cur].p1.y;
}

/* ================== item 4: the canopy ================== */

vec3 flora_leaf_color(int species) {
    switch (species) {
    case FLORA_OAK:     return vec3_make(0.16f, 0.32f, 0.10f);  /* deep green */
    case FLORA_BIRCH:   return vec3_make(0.34f, 0.50f, 0.16f);  /* bright, light */
    case FLORA_PINE:    return vec3_make(0.10f, 0.22f, 0.12f);  /* blue-green */
    case FLORA_CYPRESS: return vec3_make(0.12f, 0.26f, 0.16f);  /* darker still */
    default:            return vec3_make(0.18f, 0.34f, 0.14f);
    }
}

int flora_leaf_kind(int species) {
    return (species == FLORA_PINE || species == FLORA_CYPRESS)
        ? FLORA_LEAF_CONIFER : FLORA_LEAF_BROAD;
}

int flora_canopy(int species, const float *params, int count,
                 FloraLeaf *out, int max) {
    FloraSeg s[FLORA_MAX_SEG];
    float    p[FLORA_PARAMS];
    const float *defs;
    unsigned seed;
    float    leaf_size, density, twig;
    int      n, i, m = 0, seat_i = 0;

    if (flora_schema(species, (const char *const **)0, &defs) < 0) return 0;
    if (!out || max < 1) return 0;
    for (i = 0; i < FLORA_PARAMS; i++)
        p[i] = (params && i < count) ? params[i] : defs[i];
    seed      = (unsigned)(sol_u32)(long)floorf(p[0] + 0.5f);
    leaf_size = p[8];
    density   = p[9];
    if (density < 0.0f) density = 0.0f;
    if (density > 1.0f) density = 1.0f;

    n = flora_tree_plan(species, params, count, s, FLORA_MAX_SEG);
    /* the TWIG threshold: leaves grow where the wood is thin enough to
       bear them, never on the trunk — botanically true, and it fills
       high-apical conifers (whose thin laterals never reach the radius
       floor as tips) the way childless-tips alone could not */
    twig = 0.18f * (n > 0 ? s[0].r0 : 0.1f);
    if (twig < 0.035f) twig = 0.035f;
    for (i = 0; i < n && m < max; i++) {
        float keep, jit, t;
        vec3  along;
        if (!s[i].tip && s[i].r1 > twig) continue;   /* trunk/limb: bare */
        /* the shedding dial: hash by SEAT ORDINAL (stable identity);
           KEEP while the roll lands under density — MONOTONE (a seat
           bared at 0.3 stays bared at 0.6 because the threshold only
           rises). density 1 = full, 0 = winter. */
        keep = flora_hash01(seed, LANE_FLORA_LENGTH, 9000 + seat_i, 0);
        seat_i++;
        if (keep >= density) continue;
        jit   = flora_hash01(seed, LANE_FLORA_PITCH, 9000 + seat_i, 1);
        along = vec3_sub(s[i].p1, s[i].p0);
        t     = s[i].tip ? 1.0f : 0.6f;              /* tip caps; twig rides */
        out[m].pos   = vec3_add(s[i].p0, vec3_scale(along, t));
        out[m].yaw   = FLORA_GOLDEN_DEG * (SOL_PI / 180.0f) * (float)seat_i;
        out[m].scale = leaf_size * (0.7f + 0.6f * jit);
        out[m].phase = flora_hash01(seed, LANE_FLORA_AZIMUTH,
                                    9000 + seat_i, 2) * 6.2831853f;
        m++;
    }
    return m;
}

/* one double-sided card in the plane spun to `yaw` and tilted `pitch`
   from vertical, base at the origin, reaching `up_len` with half-width
   `hw` at the base tapering to a point — a leaf blade. Emitted both
   windings so it lights from either face (leaves are thin). */
static void fl_card(MeshBuilder *b, float yaw, float pitch, float up_len,
                    float hw, vec3 col) {
    float cy = cosf(yaw), sy = sinf(yaw);
    float cp = cosf(pitch), sp = sinf(pitch);
    /* the blade's own axes: u across (in the yaw plane), w up-and-out */
    vec3  uax = vec3_make(cy, 0.0f, sy);
    vec3  wax = vec3_make(-sy * sp, cp, cy * sp);
    vec3  nrm = vec3_normalize(vec3_cross(wax, uax));
    vec3  b0  = vec3_scale(uax, -hw);
    vec3  b1  = vec3_scale(uax,  hw);
    vec3  tipv = vec3_scale(wax, up_len);
    vec3  mid0 = vec3_add(vec3_scale(wax, up_len * 0.55f),
                          vec3_scale(uax, -hw * 0.5f));
    vec3  mid1 = vec3_add(vec3_scale(wax, up_len * 0.55f),
                          vec3_scale(uax,  hw * 0.5f));
    sol_u32 v[5];
    int     side;
    (void)col;
    for (side = 0; side < 2; side++) {
        vec3 nn = side ? vec3_scale(nrm, -1.0f) : nrm;
        v[0] = mb_push_vertex(b, b0.x, b0.y, b0.z, nn.x, nn.y, nn.z, 0.0f, 0.0f);
        v[1] = mb_push_vertex(b, b1.x, b1.y, b1.z, nn.x, nn.y, nn.z, 1.0f, 0.0f);
        v[2] = mb_push_vertex(b, mid1.x, mid1.y, mid1.z, nn.x, nn.y, nn.z, 0.85f, 0.55f);
        v[3] = mb_push_vertex(b, tipv.x, tipv.y, tipv.z, nn.x, nn.y, nn.z, 0.5f, 1.0f);
        v[4] = mb_push_vertex(b, mid0.x, mid0.y, mid0.z, nn.x, nn.y, nn.z, 0.15f, 0.55f);
        if (!side) {
            mb_push_triangle(b, v[0], v[1], v[2]);
            mb_push_triangle(b, v[0], v[2], v[3]);
            mb_push_triangle(b, v[0], v[3], v[4]);
        } else {
            mb_push_triangle(b, v[0], v[2], v[1]);
            mb_push_triangle(b, v[0], v[3], v[2]);
            mb_push_triangle(b, v[0], v[4], v[3]);
        }
    }
}

void flora_leafcard_unit(MeshBuilder *b, int leaf_kind) {
    vec3 col = vec3_make(1.0f, 1.0f, 1.0f);
    int  i;
    if (!b) return;
    if (leaf_kind == FLORA_LEAF_CONIFER) {
        /* a drooping spray: many blades fanned around and angled DOWN-
           and-out, denser, longer — the needle frond reads from afar */
        int blades = 10;
        for (i = 0; i < blades; i++) {
            float yaw   = FLORA_GOLDEN_DEG * (SOL_PI / 180.0f) * (float)i;
            float t     = (float)i / (float)blades;
            float pitch = 1.15f + 0.30f * t;       /* past vertical: droops */
            fl_card(b, yaw, pitch, 0.95f - 0.3f * t, 0.06f, col);
        }
    } else {
        /* a broadleaf puff: blades fanned up-and-out into a rough dome */
        int blades = 7;
        for (i = 0; i < blades; i++) {
            float yaw   = FLORA_GOLDEN_DEG * (SOL_PI / 180.0f) * (float)i;
            float pitch = 0.35f + 0.5f
                        * flora_hash01(1u, LANE_FLORA_PITCH, i, 0);
            fl_card(b, yaw, pitch, 0.85f, 0.34f, col);
        }
    }
}
