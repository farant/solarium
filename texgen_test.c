/* texgen_test.c — headless checks for synthesized material maps (the
   texture side-quest, 14th suite): the shared knob schema, kind names,
   bit-determinism (same knobs = memcmp-identical maps — §1.8 applied to
   a pixel buffer), defaults-are-identity (an explicit-default prefix
   renders the same bytes as no prefix), seed/kind divergence, normal
   vectors decoding to unit length, and the TILE-SEAM LAW: the wrap edge
   is held to the same bound as the roughest interior edge — tileable by
   proof, not by eyeball. No GL: texgen is pure pixel arithmetic.
   `build.sh texgentest`. */

#include "texgen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAP_BYTES (TEXGEN_SIZE * TEXGEN_SIZE * 4)

/* the largest per-channel difference between horizontally adjacent
   texels, interior columns only */
static int max_adj_interior(const unsigned char *m) {
    int y, x, c, best = 0;
    for (y = 0; y < TEXGEN_SIZE; y++)
        for (x = 0; x < TEXGEN_SIZE - 1; x++)
            for (c = 0; c < 3; c++) {
                int a = m[(y * TEXGEN_SIZE + x) * 4 + c];
                int b = m[(y * TEXGEN_SIZE + x + 1) * 4 + c];
                int d = a > b ? a - b : b - a;
                if (d > best) best = d;
            }
    return best;
}

/* the largest per-channel difference across the wrap edge (last column
   against the first) */
static int max_adj_wrap(const unsigned char *m) {
    int y, c, best = 0;
    for (y = 0; y < TEXGEN_SIZE; y++)
        for (c = 0; c < 3; c++) {
            int a = m[(y * TEXGEN_SIZE + TEXGEN_SIZE - 1) * 4 + c];
            int b = m[(y * TEXGEN_SIZE + 0) * 4 + c];
            int d = a > b ? a - b : b - a;
            if (d > best) best = d;
        }
    return best;
}

int main(void) {
    unsigned char *a1, *n1, *o1, *a2, *n2, *o2;
    int i;

    a1 = (unsigned char *)malloc(MAP_BYTES);
    n1 = (unsigned char *)malloc(MAP_BYTES);
    o1 = (unsigned char *)malloc(MAP_BYTES);
    a2 = (unsigned char *)malloc(MAP_BYTES);
    n2 = (unsigned char *)malloc(MAP_BYTES);
    o2 = (unsigned char *)malloc(MAP_BYTES);
    if (!a1 || !n1 || !o1 || !a2 || !n2 || !o2) {
        printf("FAIL: out of memory\n"); return 1;
    }

    /* ---- the schema: shared names, per-kind defaults ---- */
    {
        const char *const *ns, *const *np;
        const float *ds, *dp;
        if (texgen_schema(TEXGEN_STONE, &ns, &ds) != TEXGEN_PARAMS ||
            texgen_schema(TEXGEN_PLASTER, &np, &dp) != TEXGEN_PARAMS) {
            printf("FAIL: schema count\n"); return 1;
        }
        if (ns != np) { printf("FAIL: kinds must share one name table\n"); return 1; }
        if (memcmp(ds, dp, TEXGEN_PARAMS * sizeof(float)) == 0) {
            printf("FAIL: kind defaults identical\n"); return 1;
        }
        if (texgen_schema(-1, &ns, &ds) != -1 ||
            texgen_schema(TEXGEN_KIND_COUNT, &ns, &ds) != -1) {
            printf("FAIL: bad kind not rejected\n"); return 1;
        }
    }
    /* kind names round-trip; unknown is loudly -1 */
    for (i = 0; i < TEXGEN_KIND_COUNT; i++) {
        if (texgen_kind(texgen_kind_name(i)) != i) {
            printf("FAIL: kind name round-trip %d\n", i); return 1;
        }
    }
    if (texgen_kind("velvet") != -1 || texgen_kind((const char *)0) != -1) {
        printf("FAIL: unknown kind not -1\n"); return 1;
    }
    printf("schema + kinds: ok\n");

    /* ---- bit-determinism: same knobs, identical bytes ---- */
    if (!texgen_render(TEXGEN_STONE, (const float *)0, 0, a1, n1, o1) ||
        !texgen_render(TEXGEN_STONE, (const float *)0, 0, a2, n2, o2)) {
        printf("FAIL: render failed\n"); return 1;
    }
    if (memcmp(a1, a2, MAP_BYTES) != 0 || memcmp(n1, n2, MAP_BYTES) != 0 ||
        memcmp(o1, o2, MAP_BYTES) != 0) {
        printf("FAIL: render not deterministic\n"); return 1;
    }
    printf("bit-determinism: ok\n");

    /* ---- defaults are identity: explicit defaults == no prefix ---- */
    {
        const float *ds;
        float p[TEXGEN_PARAMS];
        texgen_schema(TEXGEN_STONE, (const char *const **)0, &ds);
        memcpy(p, ds, sizeof p);
        if (!texgen_render(TEXGEN_STONE, p, TEXGEN_PARAMS, a2, n2, o2)) {
            printf("FAIL: explicit-default render failed\n"); return 1;
        }
        if (memcmp(a1, a2, MAP_BYTES) != 0 || memcmp(n1, n2, MAP_BYTES) != 0 ||
            memcmp(o1, o2, MAP_BYTES) != 0) {
            printf("FAIL: explicit defaults render differently\n"); return 1;
        }
    }
    printf("defaults-are-identity: ok\n");

    /* ---- the knobs do something: seeds differ, kinds differ ---- */
    {
        float p[1];
        p[0] = 9.0f;
        if (!texgen_render(TEXGEN_STONE, p, 1, a2, n2, o2)) {
            printf("FAIL: seeded render failed\n"); return 1;
        }
        if (memcmp(a1, a2, MAP_BYTES) == 0) {
            printf("FAIL: seed change changed nothing\n"); return 1;
        }
    }
    if (!texgen_render(TEXGEN_PLASTER, (const float *)0, 0, a2, n2, o2)) {
        printf("FAIL: plaster render failed\n"); return 1;
    }
    if (memcmp(a1, a2, MAP_BYTES) == 0) {
        printf("FAIL: stone and plaster identical\n"); return 1;
    }
    {   /* and the field is alive — not a constant fill */
        int lo = 255, hi = 0;
        for (i = 0; i < MAP_BYTES; i += 4) {
            if (a1[i] < lo) lo = a1[i];
            if (a1[i] > hi) hi = a1[i];
        }
        if (hi - lo < 8) { printf("FAIL: albedo nearly flat\n"); return 1; }
    }
    printf("knob divergence: ok\n");

    /* ---- normals decode to unit length, z out of the surface ---- */
    {
        int bad = 0;
        for (i = 0; i < TEXGEN_SIZE * TEXGEN_SIZE; i += 7) {
            float x = (float)n1[i * 4 + 0] / 255.0f * 2.0f - 1.0f;
            float y = (float)n1[i * 4 + 1] / 255.0f * 2.0f - 1.0f;
            float z = (float)n1[i * 4 + 2] / 255.0f * 2.0f - 1.0f;
            float l = sqrtf(x * x + y * y + z * z);
            if (l < 0.94f || l > 1.06f || z <= 0.0f) bad++;
        }
        if (bad) { printf("FAIL: %d non-unit normals\n", bad); return 1; }
    }
    printf("normal map: ok\n");

    /* ---- THE TILE-SEAM LAW: the wrap edge obeys the interior's bound.
       Stone may step hard at a mortar line, but the seam must never
       step harder than the roughest interior edge — tileable by proof. */
    {
        static const int kinds[4] = { TEXGEN_STONE, TEXGEN_PLASTER,
                                      TEXGEN_BARK, TEXGEN_WATER };
        int k;
        for (k = 0; k < 4; k++) {
            if (!texgen_render(kinds[k], (const float *)0, 0, a1, n1, o1)) {
                printf("FAIL: seam render failed\n"); return 1;
            }
            if (max_adj_wrap(a1) > max_adj_interior(a1) ||
                max_adj_wrap(n1) > max_adj_interior(n1) ||
                max_adj_wrap(o1) > max_adj_interior(o1)) {
                printf("FAIL: %s seam rougher than interior\n",
                       texgen_kind_name(kinds[k]));
                return 1;
            }
        }
    }
    printf("tile-seam law: ok\n");

    free(a1); free(n1); free(o1);
    free(a2); free(n2); free(o2);
    printf("texgen_test: ALL OK\n");
    return 0;
}
