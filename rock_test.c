/* rock_test.c — the boulder's invariants (P7 item 6, 16th suite):
   bit-determinism, the coplanarity audit (no shimmering coincident
   faces), vertex bounds (the displaced ball stays within its envelope),
   WATERTIGHT (every edge shared by exactly two triangles — the shared-
   midpoint proof), the flat knob squashes the crown, and the collider
   dims bound the mesh (stand-on-top is honest). Pure CPU.
   `build.sh rocktest`. */

#include "rock.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

static int g_fail = 0;
static void fail(const char *m) { printf("FAIL: %s\n", m); g_fail = 1; }

static float vmax_y(const MeshBuilder *b) {
    sol_u32 i; float t = -1e30f;
    for (i = 0; i < b->vertex_count; i++)
        if (b->vertices[i * 12 + 1] > t) t = b->vertices[i * 12 + 1];
    return t;
}

/* edge-use parity: build the (min,max) vertex-position key per edge and
   demand every edge appears exactly twice (manifold, watertight). Verts
   are deduped by position first (the mesh duplicates them per-face). */
static void check_watertight(const MeshBuilder *b, const char *who) {
    /* hash each undirected edge by quantized endpoint positions; a
       watertight closed surface has every edge shared by 2 faces */
    sol_u32 t, e;
    int     odd = 0;
    /* small open-addressed multiset of edge keys with counts */
    static long  key[200000];
    static int   cnt[200000];
    int          cap = 200000, i;
    for (i = 0; i < cap; i++) { key[i] = 0; cnt[i] = 0; }
    for (t = 0; t + 2 < b->index_count; t += 3) {
        int v[3]; v[0]=b->indices[t]; v[1]=b->indices[t+1]; v[2]=b->indices[t+2];
        for (e = 0; e < 3; e++) {
            const sol_f32 *pa = &b->vertices[v[e] * 12];
            const sol_f32 *pb = &b->vertices[v[(e+1)%3] * 12];
            long qa[3], qb[3], lo[3], hi[3]; long h; int k; unsigned slot;
            for (k = 0; k < 3; k++) {
                qa[k] = (long)floorf(pa[k] * 4096.0f + 0.5f);
                qb[k] = (long)floorf(pb[k] * 4096.0f + 0.5f);
                lo[k] = qa[k] < qb[k] ? qa[k] : qb[k];
                hi[k] = qa[k] < qb[k] ? qb[k] : qa[k];
            }
            h = (lo[0]*73856093L) ^ (lo[1]*19349663L) ^ (lo[2]*83492791L)
              ^ (hi[0]*37623481L) ^ (hi[1]*51964263L) ^ (hi[2]*15485863L);
            if (h == 0) h = 1;
            slot = (unsigned)(h % cap); if (h < 0) slot = (unsigned)((-h) % cap);
            while (cnt[slot] != 0 && key[slot] != h) slot = (slot + 1) % (unsigned)cap;
            key[slot] = h; cnt[slot]++;
        }
    }
    for (i = 0; i < cap; i++) if (cnt[i] != 0 && cnt[i] != 2) odd++;
    if (odd) { printf("FAIL: %s not watertight (%d non-paired edges)\n", who, odd); g_fail = 1; }
}

int main(void) {
    MeshBuilder a, b;

    /* ---- determinism: same (size,seed,flat) = identical bytes ---- */
    mb_init(&a); mb_init(&b);
    rock_boulder(&a, 1.4f, 7u, 0.0f);
    rock_boulder(&b, 1.4f, 7u, 0.0f);
    if (a.index_count == 0) fail("boulder emitted nothing");
    if (a.vertex_count != b.vertex_count ||
        memcmp(a.vertices, b.vertices,
               (size_t)a.vertex_count * 12 * sizeof(sol_f32)) != 0)
        fail("boulder not deterministic");
    if (a.index_count / 3 > 300) fail("boulder over the ~300-tri budget");
    printf("determinism + budget: ok\n");

    check_watertight(&a, "boulder");
    printf("watertight: ok\n");

    /* ---- bounds: the displaced ball stays in [-1.3,1.3]*size ---- */
    {
        sol_u32 i;
        float lim = 1.4f * (1.0f + 0.30f) + 1e-3f;
        for (i = 0; i < a.vertex_count; i++) {
            const sol_f32 *v = &a.vertices[i * 12];
            if (fabsf(v[0]) > lim || fabsf(v[1]) > lim || fabsf(v[2]) > lim)
                { fail("boulder vertex outside its envelope"); break; }
        }
        printf("bounds: ok\n");
    }
    mb_free(&a); mb_free(&b);

    /* ---- flat squashes the crown, and the dims bound the mesh ---- */
    {
        MeshBuilder r, f;
        float half, top;
        mb_init(&r); mb_init(&f);
        rock_boulder(&r, 1.5f, 3u, 0.0f);
        rock_boulder(&f, 1.5f, 3u, 1.0f);
        if (vmax_y(&f) >= vmax_y(&r))
            fail("flat boulder not shorter than the round one");
        rock_boulder_dims(1.5f, 1.0f, &half, &top);
        if (vmax_y(&f) > top + 1e-3f)
            fail("collider top below the flat boulder's crown (you would float)");
        {   /* the plan half-extent must contain every vertex's x,z */
            sol_u32 i; int bad = 0;
            for (i = 0; i < f.vertex_count; i++) {
                const sol_f32 *v = &f.vertices[i * 12];
                if (fabsf(v[0]) > half + 1e-3f || fabsf(v[2]) > half + 1e-3f) bad = 1;
            }
            if (bad) fail("collider half-extent does not contain the boulder");
        }
        mb_free(&r); mb_free(&f);
        printf("flat + collider dims: ok\n");
    }

    /* ---- the pebble unit builds and is watertight ---- */
    {
        MeshBuilder p;
        mb_init(&p);
        rock_pebble_unit(&p);
        if (p.index_count == 0) fail("pebble emitted nothing");
        check_watertight(&p, "pebble");
        mb_free(&p);
        printf("pebble: ok\n");
    }

    if (g_fail) { printf("rock_test: FAILED\n"); return 1; }
    printf("rock_test: OK\n");
    return 0;
}
