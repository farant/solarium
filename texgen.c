/* texgen.c — synthesized PBR material maps (33rd TU).

   THE HEIGHT FIELD IS THE ONE AUTHOR (the meadow pattern at texel
   scale): one synthesized relief field, three readers — the normal map
   is its finite difference, the albedo darkens in its grooves, the
   roughness and occlusion read its cavities. The maps cannot disagree
   because none of them is authored separately.

   Tileable BY CONSTRUCTION, not by blending: every noise octave hashes
   a wrapped lattice, and the masonry grid quantizes courses and stones
   to integers per repeat — there is no seam to hide (the suite holds
   the wrap edge to the law of any interior edge).

   The course knob defaults to 0.4 m — quantize_keep's course constant.
   Walls carry world-scale planar UVs (v = world y), so mortar lines in
   the texture land on the same heights where ruined walls actually
   break: the texture and the ruin system agree about where the stones
   are because they share one number.

   texgen owns its noise twin (the gothic ruling): another module
   changing its hash must never silently re-dress every wall. */

#include "texgen.h"
#include <stdlib.h>   /* malloc, free */
#include <string.h>   /* memcpy */
#include <math.h>     /* floorf, sqrtf, powf */

/* ---- the knob schema: ONE shared vector, kinds are presets ---- */

static const char *const tg_names[TEXGEN_PARAMS] = {
    "seed",     /* variation selector (any value hashes) */
    "tile",     /* world meters covered by one repeat */
    "course",   /* masonry course height (m); 0 = no grid (plaster) */
    "stone",    /* target stone length (m) */
    "joint",    /* mortar joint width (m) */
    "depth",    /* relief amount 0..1 (groove recess + per-stone offset) */
    "tone",     /* base albedo value, linear */
    "warmth",   /* hue lean: 0 cool grey .. 1 warm buff */
    "weather",  /* large-scale patchiness amount 0..1 */
    "rough"     /* base roughness of dressed faces */
};

static const float tg_stone_def[TEXGEN_PARAMS] = {
    0.0f, 2.0f, 0.4f, 0.65f, 0.018f, 0.6f, 0.34f, 0.45f, 0.5f, 0.78f
};
static const float tg_plaster_def[TEXGEN_PARAMS] = {
    0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 0.25f, 0.52f, 0.4f, 0.6f, 0.9f
};

static const char *const tg_kind_names[TEXGEN_KIND_COUNT] = {
    "stone", "plaster"
};

int texgen_kind(const char *name) {
    int i;
    if (!name) return -1;
    for (i = 0; i < TEXGEN_KIND_COUNT; i++)
        if (strcmp(name, tg_kind_names[i]) == 0) return i;
    return -1;
}

const char *texgen_kind_name(int kind) {
    if (kind < 0 || kind >= TEXGEN_KIND_COUNT) return (const char *)0;
    return tg_kind_names[kind];
}

int texgen_schema(int kind, const char *const **names,
                  const float **defaults) {
    if (kind < 0 || kind >= TEXGEN_KIND_COUNT) return -1;
    if (names)    *names = tg_names;
    if (defaults) *defaults = (kind == TEXGEN_PLASTER) ? tg_plaster_def
                                                       : tg_stone_def;
    return TEXGEN_PARAMS;
}

/* ---- texgen's own noise twin ---- */

static sol_u32 tg_mix(sol_u32 h) {
    h ^= h >> 16; h *= 0x7feb352dU;
    h ^= h >> 15; h *= 0x846ca68bU;
    h ^= h >> 16;
    return h;
}

/* one float in [0,1) from (seed, lattice x, lattice y, channel) */
static float tg_hash01(sol_u32 seed, int x, int y, sol_u32 ch) {
    sol_u32 h = seed * 0x9e3779b9U + ch * 0x85ebca6bU;
    h ^= (sol_u32)x * 0xc2b2ae35U;
    h  = tg_mix(h);
    h ^= (sol_u32)y * 0x27d4eb2fU;
    h  = tg_mix(h);
    return (float)(h & 0xffffffU) / 16777216.0f;
}

static float tg_smooth(float t) { return t * t * (3.0f - 2.0f * t); }

/* periodic value noise: u,v in tile units [0,1), p lattice cells per
   tile — the lattice WRAPS, so the field tiles by construction */
static float tg_vnoise(sol_u32 seed, float u, float v, int p, sol_u32 ch) {
    float x, y, fx, fy, a, b, c, d, sx, sy, ab, cd;
    int   ix, iy, x0, y0, x1, y1;
    x  = u * (float)p;  y = v * (float)p;
    ix = (int)floorf(x); iy = (int)floorf(y);
    fx = x - (float)ix;  fy = y - (float)iy;
    x0 = ((ix % p) + p) % p;  y0 = ((iy % p) + p) % p;
    x1 = (x0 + 1) % p;        y1 = (y0 + 1) % p;
    a  = tg_hash01(seed, x0, y0, ch);
    b  = tg_hash01(seed, x1, y0, ch);
    c  = tg_hash01(seed, x0, y1, ch);
    d  = tg_hash01(seed, x1, y1, ch);
    sx = tg_smooth(fx);  sy = tg_smooth(fy);
    ab = a + (b - a) * sx;
    cd = c + (d - c) * sx;
    return ab + (cd - ab) * sy;
}

/* octaves double the lattice period — each stays integer, so the sum
   tiles exactly like its parts */
static float tg_fbm(sol_u32 seed, float u, float v, int p, int oct,
                    sol_u32 ch) {
    float sum = 0.0f, amp = 0.5f, norm = 0.0f;
    int   i, pp = p;
    for (i = 0; i < oct; i++) {
        sum  += amp * tg_vnoise(seed, u, v, pp, ch + (sol_u32)i * 101u);
        norm += amp;
        amp  *= 0.5f;
        pp   *= 2;
    }
    return sum / norm;
}

static float tg_sstep(float e0, float e1, float x) {
    float t;
    if (e1 <= e0) return (x < e0) ? 0.0f : 1.0f;
    t = (x - e0) / (e1 - e0);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

static float tg_clamp01(float x) {
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

static unsigned char tg_byte(float x) {
    return (unsigned char)(tg_clamp01(x) * 255.0f + 0.5f);
}

/* ---- the generator ---- */

sol_bool texgen_render(int kind, const float *params, int count,
                       unsigned char *albedo, unsigned char *normal,
                       unsigned char *orm) {
    float       p[TEXGEN_PARAMS];
    const float *defs;
    float       *hf;
    sol_u32      seed;
    float        tile, course, stone, joint, depth, tone, warmth,
                 weather, rough;
    int          grid_on, nrows, ncols;
    float        row_h, col_w, shoulder, texel_m, groove_m, relief_m;
    int          px, py, i;

    if (texgen_schema(kind, (const char *const **)0, &defs) < 0)
        return SOL_FALSE;
    if (!albedo || !normal || !orm) return SOL_FALSE;
    for (i = 0; i < TEXGEN_PARAMS; i++)
        p[i] = (params && i < count) ? params[i] : defs[i];

    hf = (float *)malloc((size_t)TEXGEN_SIZE * TEXGEN_SIZE * sizeof(float));
    if (!hf) return SOL_FALSE;

    seed    = (sol_u32)(long)floorf(p[0] + 0.5f);
    tile    = (p[1] > 1e-3f) ? p[1] : 1.0f;
    course  = p[2];
    stone   = p[3];
    joint   = p[4];
    depth   = tg_clamp01(p[5]);
    tone    = p[6];
    warmth  = tg_clamp01(p[7]);
    weather = tg_clamp01(p[8]);
    rough   = p[9];

    texel_m = tile / (float)TEXGEN_SIZE;

    /* quantize the grid to the tile: integer courses and stones per
       repeat — running bond's half-stone offset then wraps exactly */
    grid_on = (course > 1e-4f && stone > 1e-4f);
    nrows = 1; ncols = 1; row_h = tile; col_w = tile;
    if (grid_on) {
        nrows = (int)(tile / course + 0.5f); if (nrows < 1) nrows = 1;
        ncols = (int)(tile / stone  + 0.5f); if (ncols < 1) ncols = 1;
        row_h = tile / (float)nrows;
        col_w = tile / (float)ncols;
    }
    shoulder = joint * 1.5f;                       /* the chamfered arris */
    if (shoulder < 2.0f * texel_m) shoulder = 2.0f * texel_m;
    groove_m = 0.014f * depth;                     /* ~8 mm at default */
    relief_m = 0.008f * depth;                     /* proud/shy stones */

    /* pass 1: the height field, and its first two readers (albedo, ORM
       need only local values, so they ride along) */
    for (py = 0; py < TEXGEN_SIZE; py++) {
        float v = ((float)py + 0.5f) / (float)TEXGEN_SIZE;
        for (px = 0; px < TEXGEN_SIZE; px++) {
            float u = ((float)px + 0.5f) / (float)TEXGEN_SIZE;
            float h = 0.0f, face = 1.0f;
            float fine, wfbm, sval = 1.0f, shue = 0.5f, srgh = 0.5f;
            float cr, cg, cb, ao, rg;
            int   idx = (py * TEXGEN_SIZE + px) * 4;

            if (grid_on) {                          /* the masonry grid */
                float ry, ly, dh, cx, lx, dv, d;
                int   row, col;
                ry  = v * (float)nrows;
                row = (int)ry;  if (row >= nrows) row = nrows - 1;
                ly  = (ry - (float)row) * row_h;
                dh  = (ly < row_h - ly) ? ly : row_h - ly;
                cx  = u * (float)ncols + ((row & 1) ? 0.5f : 0.0f);
                col = (int)cx;
                lx  = (cx - (float)col) * col_w;
                dv  = (lx < col_w - lx) ? lx : col_w - lx;
                col = col % ncols;                  /* bond offset wraps */
                d   = (dh < dv) ? dh : dv;
                face = tg_sstep(joint * 0.5f, joint * 0.5f + shoulder, d);
                h += groove_m * (face - 1.0f);      /* joints recessed */
                h += (tg_hash01(seed, row, col, 7u) - 0.5f)
                     * relief_m * face;             /* proud/shy, face only */
                sval = 0.82f + 0.36f * tg_hash01(seed, row, col, 21u);
                shue = tg_hash01(seed, row, col, 22u);
                srgh = tg_hash01(seed, row, col, 23u);
            }

            /* tooling grain + trowel undulation; plaster leans on it */
            fine = tg_fbm(seed, u, v, grid_on ? 24 : 6, 3, 11u);
            h   += (fine - 0.5f) * (grid_on ? 0.0035f : 0.006f)
                   * (0.5f + depth);
            wfbm = tg_fbm(seed, u, v, 3, 3, 17u);   /* the weather wash */
            hf[py * TEXGEN_SIZE + px] = h;

            /* albedo (linear, then sRGB-encoded at the write) */
            {
                float wm = tg_clamp01(warmth + (shue - 0.5f) * 0.3f);
                float val = tone * sval
                          * (0.96f + 0.08f * fine)
                          * (1.0f - 0.30f * weather * wfbm);
                cr = val * (0.93f + wm * 0.13f);
                cg = val * (0.95f + wm * 0.02f);
                cb = val * (1.02f - wm * 0.18f);
                if (grid_on) {                       /* mortar reads warm-grey */
                    float mv = tone * 0.78f * (0.92f + 0.16f * fine);
                    cr = cr * face + mv * 1.00f * (1.0f - face);
                    cg = cg * face + mv * 0.97f * (1.0f - face);
                    cb = cb * face + mv * 0.90f * (1.0f - face);
                }
            }
            albedo[idx + 0] = tg_byte(powf(tg_clamp01(cr), 1.0f / 2.2f));
            albedo[idx + 1] = tg_byte(powf(tg_clamp01(cg), 1.0f / 2.2f));
            albedo[idx + 2] = tg_byte(powf(tg_clamp01(cb), 1.0f / 2.2f));
            albedo[idx + 3] = 255;

            /* ORM: cavities occlude, joints and weather roughen */
            ao = (0.55f + 0.45f * face) * (1.0f - 0.18f * weather * wfbm);
            rg = rough + (1.0f - face) * 0.18f + weather * 0.12f * wfbm
               + (srgh - 0.5f) * 0.08f;
            orm[idx + 0] = tg_byte(ao);
            orm[idx + 1] = tg_byte(rg);
            orm[idx + 2] = 0;
            orm[idx + 3] = 255;
        }
    }

    /* pass 2: the normal map IS the height field's finite difference —
       wrapped, so the seam carries the same slopes as any interior edge */
    for (py = 0; py < TEXGEN_SIZE; py++) {
        int ym = (py + TEXGEN_SIZE - 1) % TEXGEN_SIZE;
        int yp = (py + 1) % TEXGEN_SIZE;
        for (px = 0; px < TEXGEN_SIZE; px++) {
            int   xm = (px + TEXGEN_SIZE - 1) % TEXGEN_SIZE;
            int   xp = (px + 1) % TEXGEN_SIZE;
            int   idx = (py * TEXGEN_SIZE + px) * 4;
            float dhdx = (hf[py * TEXGEN_SIZE + xp] - hf[py * TEXGEN_SIZE + xm])
                       / (2.0f * texel_m);
            float dhdy = (hf[yp * TEXGEN_SIZE + px] - hf[ym * TEXGEN_SIZE + px])
                       / (2.0f * texel_m);
            float nx = -dhdx, ny = -dhdy, nz = 1.0f;
            float il = 1.0f / sqrtf(nx * nx + ny * ny + nz * nz);
            normal[idx + 0] = tg_byte(nx * il * 0.5f + 0.5f);
            normal[idx + 1] = tg_byte(ny * il * 0.5f + 0.5f);
            normal[idx + 2] = tg_byte(nz * il * 0.5f + 0.5f);
            normal[idx + 3] = 255;
        }
    }

    free(hf);
    return SOL_TRUE;
}
