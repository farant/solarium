/* rock.c — boulders and scree (36th TU; P7 item 6).

   An octahedron subdivided to a ball, every vertex pushed along its own
   direction by fBm: a believable stone. The trick that keeps it
   WATERTIGHT without an edge-midpoint map: displacement is a pure
   function of the unit DIRECTION, so two triangles sharing a midpoint
   compute the identical displaced point — no cracks, no dedup. Flat-
   shaded per triangle (rocks are faceted). World-scale planar UVs. */

#include "rock.h"
#include "sol_math.h"

#include <math.h>

#define ROCK_AMP 0.30f   /* lump depth as a fraction of the radius */

/* ---- rock's own noise twin: 3D value noise, fBm'd ---- */

static sol_u32 rk_mix(sol_u32 h) {
    h ^= h >> 16; h *= 0x7feb352dU;
    h ^= h >> 15; h *= 0x846ca68bU;
    h ^= h >> 16;
    return h;
}

static float rk_hash3(sol_u32 seed, int x, int y, int z) {
    sol_u32 h = seed * 0x9e3779b9U;
    h ^= (sol_u32)x * 0xc2b2ae35U; h = rk_mix(h);
    h ^= (sol_u32)y * 0x27d4eb2fU; h = rk_mix(h);
    h ^= (sol_u32)z * 0x165667b1U; h = rk_mix(h);
    return (float)(h & 0xffffffU) / 16777216.0f;
}

static float rk_smooth(float t) { return t * t * (3.0f - 2.0f * t); }

static float rk_vnoise3(sol_u32 seed, float x, float y, float z) {
    int   ix = (int)floorf(x), iy = (int)floorf(y), iz = (int)floorf(z);
    float fx = x - (float)ix, fy = y - (float)iy, fz = z - (float)iz;
    float sx = rk_smooth(fx), sy = rk_smooth(fy), sz = rk_smooth(fz);
    float c000 = rk_hash3(seed, ix,   iy,   iz);
    float c100 = rk_hash3(seed, ix+1, iy,   iz);
    float c010 = rk_hash3(seed, ix,   iy+1, iz);
    float c110 = rk_hash3(seed, ix+1, iy+1, iz);
    float c001 = rk_hash3(seed, ix,   iy,   iz+1);
    float c101 = rk_hash3(seed, ix+1, iy,   iz+1);
    float c011 = rk_hash3(seed, ix,   iy+1, iz+1);
    float c111 = rk_hash3(seed, ix+1, iy+1, iz+1);
    float x00 = c000 + (c100 - c000) * sx;
    float x10 = c010 + (c110 - c010) * sx;
    float x01 = c001 + (c101 - c001) * sx;
    float x11 = c011 + (c111 - c011) * sx;
    float y0  = x00 + (x10 - x00) * sy;
    float y1  = x01 + (x11 - x01) * sy;
    return y0 + (y1 - y0) * sz;
}

static float rk_fbm3(sol_u32 seed, vec3 p) {
    float sum = 0.0f, amp = 0.5f, norm = 0.0f, freq = 2.3f;
    int   i;
    for (i = 0; i < 4; i++) {
        sum  += amp * rk_vnoise3(seed + (sol_u32)i * 131u,
                                 p.x * freq, p.y * freq, p.z * freq);
        norm += amp;
        amp  *= 0.5f;
        freq *= 2.0f;
    }
    return sum / norm;
}

/* the displaced surface point for a unit direction — the ONE function
   both the emitter and the shared midpoints evaluate (watertight) */
static vec3 rock_point(vec3 dir, float size, unsigned seed, float flat) {
    float r = size * (1.0f + ROCK_AMP * (rk_fbm3(seed, dir) - 0.5f) * 2.0f);
    vec3  p = vec3_scale(dir, r);
    p.y *= (1.0f - 0.55f * flat);              /* squash to a dome */
    {   /* flatten the crown: pull the top toward a plane as flat rises */
        float top = size * (1.0f - 0.55f * flat) * (1.0f + ROCK_AMP) * 0.78f;
        if (flat > 0.0f && p.y > top)
            p.y = top + (p.y - top) * (1.0f - flat);
    }
    return p;
}

/* emit one flat-shaded triangle from three directions */
static void rock_tri(MeshBuilder *b, vec3 da, vec3 db, vec3 dc,
                     float size, unsigned seed, float flat) {
    vec3 a = rock_point(da, size, seed, flat);
    vec3 c = rock_point(db, size, seed, flat);
    vec3 d = rock_point(dc, size, seed, flat);
    vec3 n = vec3_cross(vec3_sub(c, a), vec3_sub(d, a));
    float l = sqrtf(vec3_dot(n, n));
    sol_u32 i0, i1, i2;
    if (l < 1e-9f) return;
    n = vec3_scale(n, 1.0f / l);
    i0 = mb_push_vertex(b, a.x, a.y, a.z, n.x, n.y, n.z, a.x, a.z);
    i1 = mb_push_vertex(b, c.x, c.y, c.z, n.x, n.y, n.z, c.x, c.z);
    i2 = mb_push_vertex(b, d.x, d.y, d.z, n.x, n.y, n.z, d.x, d.z);
    mb_push_triangle(b, i0, i1, i2);
}

/* recursively split a spherical triangle, midpoints renormalized onto
   the sphere; at depth 0 emit the displaced face */
static void rock_subdiv(MeshBuilder *b, vec3 a, vec3 c, vec3 d, int depth,
                        float size, unsigned seed, float flat) {
    if (depth == 0) {
        rock_tri(b, a, c, d, size, seed, flat);
        return;
    }
    {
        vec3 ac = vec3_normalize(vec3_add(a, c));
        vec3 cd = vec3_normalize(vec3_add(c, d));
        vec3 da = vec3_normalize(vec3_add(d, a));
        rock_subdiv(b, a,  ac, da, depth - 1, size, seed, flat);
        rock_subdiv(b, ac, c,  cd, depth - 1, size, seed, flat);
        rock_subdiv(b, da, cd, d,  depth - 1, size, seed, flat);
        rock_subdiv(b, ac, cd, da, depth - 1, size, seed, flat);
    }
}

/* the 8 octahedron faces, wound CCW seen from outside */
static const float OCTA[8][3][3] = {
    {{ 1,0,0},{0, 1,0},{0,0, 1}}, {{0, 1,0},{-1,0,0},{0,0, 1}},
    {{-1,0,0},{0,-1,0},{0,0, 1}}, {{0,-1,0},{ 1,0,0},{0,0, 1}},
    {{0, 1,0},{ 1,0,0},{0,0,-1}}, {{-1,0,0},{0, 1,0},{0,0,-1}},
    {{0,-1,0},{-1,0,0},{0,0,-1}}, {{ 1,0,0},{0,-1,0},{0,0,-1}}
};

static void rock_build(MeshBuilder *b, float size, unsigned seed,
                       float flat, int depth) {
    int f;
    if (!b || size <= 0.0f) return;
    for (f = 0; f < 8; f++) {
        vec3 a = vec3_make(OCTA[f][0][0], OCTA[f][0][1], OCTA[f][0][2]);
        vec3 c = vec3_make(OCTA[f][1][0], OCTA[f][1][1], OCTA[f][1][2]);
        vec3 d = vec3_make(OCTA[f][2][0], OCTA[f][2][1], OCTA[f][2][2]);
        rock_subdiv(b, a, c, d, depth, size, seed, flat);
    }
}

void rock_boulder(MeshBuilder *b, float size, unsigned seed, float flat) {
    if (flat < 0.0f) flat = 0.0f;
    if (flat > 1.0f) flat = 1.0f;
    rock_build(b, size, seed, flat, 2);    /* 8 * 4^2 = 128 faces */
}

void rock_pebble_unit(MeshBuilder *b) {
    rock_build(b, 1.0f, 5u, 0.2f, 1);      /* 32 faces — scree is distant */
}

void rock_boulder_dims(float size, float flat, float *half, float *top) {
    if (flat < 0.0f) flat = 0.0f;
    if (flat > 1.0f) flat = 1.0f;
    if (half) *half = size * (1.0f + ROCK_AMP);
    /* the standable top: the crown-flatten plane (where high lumps land) */
    if (top)  *top  = size * (1.0f - 0.55f * flat) * (1.0f + ROCK_AMP) * 0.78f;
}
