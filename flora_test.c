/* flora_test.c — the branch graph's invariant sweep (P7 item 2, 15th
   suite): schema shape, bit-determinism, connectivity (parents precede
   children, sprouting exactly at their ends), DA VINCI'S LAW at every
   fork (cross-sections never exceed the parent's — radii are derived,
   not authored), taper monotone down every chain, the height envelope,
   tips by the childless test, THE AGE LAW (a sapling's topology is a
   prefix of the elder tree's), graceful arena truncation, and
   defaults-are-identity. Pure CPU — `build.sh floratest`. */

#include "flora.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

static int g_fail = 0;
static void fail(const char *msg) { printf("FAIL: %s\n", msg); g_fail = 1; }

static FloraSeg A[FLORA_MAX_SEG], B[FLORA_MAX_SEG];

/* every structural law, on one planned tree */
static void invariants(const FloraSeg *s, int n, float envelope,
                       const char *who) {
    int   i, roots = 0, tips = 0;
    char  msg[96];
    float child_area[FLORA_MAX_SEG];
    int   child_n[FLORA_MAX_SEG];
    if (n < 1) { snprintf(msg, sizeof msg, "%s: empty plan", who); fail(msg); return; }
    for (i = 0; i < n; i++) { child_area[i] = 0.0f; child_n[i] = 0; }
    for (i = 0; i < n; i++) {
        if (s[i].parent < 0) {
            roots++;
        } else {
            int pa = s[i].parent;
            if (pa >= i) { snprintf(msg, sizeof msg, "%s: parent after child", who); fail(msg); return; }
            if (s[i].depth != s[pa].depth + 1)
                { snprintf(msg, sizeof msg, "%s: depth not parent+1", who); fail(msg); return; }
            if (memcmp(&s[i].p0, &s[pa].p1, sizeof(vec3)) != 0)
                { snprintf(msg, sizeof msg, "%s: child not at parent's end", who); fail(msg); return; }
            if (s[i].r0 > s[pa].r1 + 1e-6f)
                { snprintf(msg, sizeof msg, "%s: child thicker than parent end", who); fail(msg); return; }
            child_area[pa] += s[i].r0 * s[i].r0;
            child_n[pa]++;
        }
        if (s[i].r1 >= s[i].r0)
            { snprintf(msg, sizeof msg, "%s: segment fattens along itself", who); fail(msg); return; }
        if (fabsf(s[i].p1.x) > envelope || s[i].p1.y > envelope ||
            s[i].p1.y < -0.01f || fabsf(s[i].p1.z) > envelope)
            { snprintf(msg, sizeof msg, "%s: growth outside the envelope", who); fail(msg); return; }
        if (s[i].tip) tips++;
    }
    if (roots != 1) { snprintf(msg, sizeof msg, "%s: not exactly one root", who); fail(msg); }
    if (tips < 1)   { snprintf(msg, sizeof msg, "%s: no tips", who); fail(msg); }
    for (i = 0; i < n; i++) {
        /* da Vinci: children never out-mass the parent's end (under-sum
           is legal — the radius floor prunes shares) */
        if (child_area[i] > s[i].r1 * s[i].r1 * (1.0f + 1e-4f))
            { snprintf(msg, sizeof msg, "%s: da Vinci broken at a fork", who); fail(msg); return; }
        /* tips are exactly the childless */
        if ((child_n[i] == 0) != (s[i].tip == 1))
            { snprintf(msg, sizeof msg, "%s: tip flag disagrees with childlessness", who); fail(msg); return; }
    }
}

int main(void) {
    int sp, n;

    /* ---- schema: shared names, per-species defaults, round-trips ---- */
    {
        const char *const *n0, *const *n1;
        const float *d0, *d1;
        if (flora_schema(FLORA_OAK, &n0, &d0) != FLORA_PARAMS ||
            flora_schema(FLORA_PINE, &n1, &d1) != FLORA_PARAMS)
            fail("schema count");
        else {
            if (n0 != n1) fail("species must share one name table");
            if (memcmp(d0, d1, FLORA_PARAMS * sizeof(float)) == 0)
                fail("species defaults identical");
        }
        if (flora_schema(-1, &n0, &d0) != -1 ||
            flora_schema(FLORA_SPECIES_COUNT, &n0, &d0) != -1)
            fail("bad species not rejected");
        for (sp = 0; sp < FLORA_SPECIES_COUNT; sp++)
            if (flora_species(flora_species_name(sp)) != sp)
                fail("species name round-trip");
        if (flora_species("mallorn") != -1 || flora_species((const char *)0) != -1)
            fail("unknown species not -1");
    }
    printf("schema + species: ok\n");

    /* ---- determinism + invariants, every species, seeds x ages ---- */
    {
        static const float SEEDS[5] = { 0.0f, 1.0f, 7.0f, 42.0f, 999.0f };
        static const float AGES[4]  = { 0.3f, 0.6f, 1.0f, 1.5f };
        int se, ag, total = 0;
        for (sp = 0; sp < FLORA_SPECIES_COUNT; sp++) {
            const float *defs;
            flora_schema(sp, (const char *const **)0, &defs);
            for (se = 0; se < 5; se++)
                for (ag = 0; ag < 4; ag++) {
                    float prm[2];
                    float env;
                    int   n2;
                    prm[0] = SEEDS[se]; prm[1] = AGES[ag];
                    n  = flora_tree_plan(sp, prm, 2, A, FLORA_MAX_SEG);
                    n2 = flora_tree_plan(sp, prm, 2, B, FLORA_MAX_SEG);
                    if (n != n2 ||
                        memcmp(A, B, (size_t)n * sizeof(FloraSeg)) != 0)
                        fail("plan not deterministic");
                    env = defs[2] * powf(AGES[ag] > 1.0f ? AGES[ag] : 1.0f,
                                         0.75f) * 2.2f;
                    invariants(A, n, env, flora_species_name(sp));
                    total += n;
                }
        }
        if (total < 4 * 5 * 4 * 8)
            fail("the sweep grew suspiciously little wood");
        printf("invariant sweep: %d trees, %d segments: ok\n",
               4 * 5 * 4, total);
    }

    /* ---- the defaults are real trees: non-trivial, uncapped ---- */
    for (sp = 0; sp < FLORA_SPECIES_COUNT; sp++) {
        n = flora_tree_plan(sp, (const float *)0, 0, A, FLORA_MAX_SEG);
        if (n < 30) fail("a default tree is a twig");
        if (n >= FLORA_MAX_SEG) fail("a default tree hit the arena cap");
    }
    printf("default species: ok\n");

    /* ---- THE AGE LAW: the sapling's topology prefixes the elder's ---- */
    {
        float young[2], old[2];
        int   ny, no2, i;
        young[0] = 7.0f; young[1] = 0.4f;
        old[0]   = 7.0f; old[1]   = 1.0f;
        for (sp = 0; sp < FLORA_SPECIES_COUNT; sp++) {
            ny  = flora_tree_plan(sp, young, 2, A, FLORA_MAX_SEG);
            no2 = flora_tree_plan(sp, old, 2, B, FLORA_MAX_SEG);
            if (ny > no2) { fail("age law: the sapling outgrew the elder"); continue; }
            for (i = 0; i < ny; i++)
                if (A[i].parent != B[i].parent || A[i].depth != B[i].depth) {
                    fail("age law: topology not a prefix");
                    break;
                }
        }
    }
    printf("the age law: ok\n");

    /* ---- seeds and species diverge ---- */
    {
        float s1[1], s2[1];
        int   n1, n2;
        s1[0] = 1.0f; s2[0] = 2.0f;
        n1 = flora_tree_plan(FLORA_OAK, s1, 1, A, FLORA_MAX_SEG);
        n2 = flora_tree_plan(FLORA_OAK, s2, 1, B, FLORA_MAX_SEG);
        if (n1 == n2 && memcmp(A, B, (size_t)n1 * sizeof(FloraSeg)) == 0)
            fail("seed change changed nothing");
        n1 = flora_tree_plan(FLORA_OAK, (const float *)0, 0, A, FLORA_MAX_SEG);
        n2 = flora_tree_plan(FLORA_PINE, (const float *)0, 0, B, FLORA_MAX_SEG);
        if (n1 == n2 && memcmp(A, B, (size_t)n1 * sizeof(FloraSeg)) == 0)
            fail("oak and pine identical");
    }
    printf("divergence: ok\n");

    /* ---- defaults are identity: explicit defaults == bare ---- */
    {
        const float *defs;
        float prm[FLORA_PARAMS];
        int   n1, n2;
        flora_schema(FLORA_BIRCH, (const char *const **)0, &defs);
        memcpy(prm, defs, sizeof prm);
        n1 = flora_tree_plan(FLORA_BIRCH, (const float *)0, 0, A, FLORA_MAX_SEG);
        n2 = flora_tree_plan(FLORA_BIRCH, prm, FLORA_PARAMS, B, FLORA_MAX_SEG);
        if (n1 != n2 || memcmp(A, B, (size_t)n1 * sizeof(FloraSeg)) != 0)
            fail("explicit defaults plan differently");
    }
    printf("defaults-are-identity: ok\n");

    /* ---- graceful truncation: a tiny arena keeps every law ---- */
    {
        n = flora_tree_plan(FLORA_OAK, (const float *)0, 0, A, 16);
        if (n > 16) fail("arena cap ignored");
        invariants(A, n, 7.0f * 2.2f, "capped oak");
        if (flora_tree_plan(FLORA_OAK, (const float *)0, 0, A, 0) != 0)
            fail("zero arena not refused");
        if (flora_tree_plan(99, (const float *)0, 0, A, 16) != 0)
            fail("bad species not refused");
    }
    printf("truncation: ok\n");

    if (g_fail) { printf("flora_test: FAILED\n"); return 1; }
    printf("flora_test: OK\n");
    return 0;
}
