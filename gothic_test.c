/* gothic_test.c — headless checks for the gothic kit's gate primitive
   (P6 item 1): the profile sweep. A swept square IS a box (positions,
   axis-aligned normals, winding agreeing with normals); a 90-degree
   miter stretches the joint ring by sqrt(2) and auto-creases to
   per-segment frames; rolls stay smooth and pinch-free; the molding
   table is sane and CCW-wound; the two-cap tessellation rule binds the
   right cap; and §1.8's first law — bit-determinism through the registry
   row — holds. Links gothic.c + mesh.c + sol_math.c, no GL.
   Built by `build.sh gothictest`. */

#include "gothic.h"
#include "sol_math.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

static int g_fail = 0;

static void fail(const char *msg) {
    printf("FAIL: %s\n", msg);
    g_fail = 1;
}

static vec3 v3(float x, float y, float z) {
    vec3 v; v.x = x; v.y = y; v.z = z; return v;
}

static int near3(vec3 a, float x, float y, float z, float tol) {
    return fabsf(a.x - x) < tol && fabsf(a.y - y) < tol && fabsf(a.z - z) < tol;
}

/* fetch position / normal of vertex i from the canonical 12-float layout */
static vec3 vpos(const MeshBuilder *b, sol_u32 i) {
    const sol_f32 *v = b->vertices + (size_t)i * 12;
    return v3(v[0], v[1], v[2]);
}
static vec3 vnrm(const MeshBuilder *b, sol_u32 i) {
    const sol_f32 *v = b->vertices + (size_t)i * 12;
    return v3(v[3], v[4], v[5]);
}

/* geometric (winding) normal of triangle t; zero if degenerate */
static vec3 tri_gnormal(const MeshBuilder *b, sol_u32 t) {
    vec3 p0 = vpos(b, b->indices[t * 3 + 0]);
    vec3 p1 = vpos(b, b->indices[t * 3 + 1]);
    vec3 p2 = vpos(b, b->indices[t * 3 + 2]);
    vec3 cr = vec3_cross(vec3_sub(p1, p0), vec3_sub(p2, p0));
    float l = sqrtf(vec3_dot(cr, cr));
    if (l < 1e-12f) return v3(0, 0, 0);
    return vec3_scale(cr, 1.0f / l);
}

/* every triangle: positive area, geometric normal agreeing with all three
   stored vertex normals to at least min_dot (1.0-ish for flat-creased
   geometry, lower for smooth rolls whose vertex normals average facets) */
static void check_consistency(const MeshBuilder *b, float min_dot, const char *who) {
    sol_u32 t, k;
    for (t = 0; t < b->index_count / 3; t++) {
        vec3 gn = tri_gnormal(b, t);
        if (vec3_dot(gn, gn) < 0.5f) {
            printf("FAIL: %s: degenerate triangle %u\n", who, (unsigned)t);
            g_fail = 1; return;
        }
        for (k = 0; k < 3; k++) {
            vec3 n = vnrm(b, b->indices[t * 3 + k]);
            float nl = sqrtf(vec3_dot(n, n));
            if (fabsf(nl - 1.0f) > 1e-3f) {
                printf("FAIL: %s: non-unit vertex normal on tri %u\n", who, (unsigned)t);
                g_fail = 1; return;
            }
            if (vec3_dot(gn, n) < min_dot) {
                printf("FAIL: %s: winding disagrees with normal on tri %u (dot %.3f)\n",
                       who, (unsigned)t, vec3_dot(gn, n));
                g_fail = 1; return;
            }
        }
    }
}

/* the unit test profile: a closed square, half-width h, every arris hard */
static const ProfilePt SQUARE[5] = {
    { -0.5f, -0.5f, 1 }, { 0.5f, -0.5f, 1 }, { 0.5f, 0.5f, 1 },
    { -0.5f, 0.5f, 1 }, { -0.5f, -0.5f, 1 }
};

/* ---- 1: a swept square IS a box ---- */
static void test_square_is_box(void) {
    MeshBuilder b;
    vec3 path[2];
    sol_u32 i;

    mb_init(&b);
    path[0] = v3(0, 0, 0);
    path[1] = v3(2, 0, 0);
    gothic_sweep(&b, SQUARE, 5, path, 2, v3(0, 1, 0), 1.0f, 1, 1);

    /* 2 rings x 8 verts (2 endpoints + 3 creased interior x 2) + 2 caps
       x 4 (the closed profile's seam copy deduped) = 24 vertices; 4 side
       quads (8 tris) + 2 earclipped tris per cap = 12 */
    if (b.vertex_count != 24) fail("square sweep: vertex count != 24");
    if (b.index_count != 12 * 3) fail("square sweep: triangle count != 12");

    /* every position is one of the 8 box corners */
    for (i = 0; i < b.vertex_count; i++) {
        vec3 p = vpos(&b, i);
        if ((fabsf(p.x) > 1e-6f && fabsf(p.x - 2.0f) > 1e-6f) ||
            fabsf(fabsf(p.y) - 0.5f) > 1e-6f ||
            fabsf(fabsf(p.z) - 0.5f) > 1e-6f) {
            fail("square sweep: vertex off the box corners"); break;
        }
    }
    /* every normal is an axis, winding agrees exactly */
    for (i = 0; i < b.vertex_count; i++) {
        vec3 n = vnrm(&b, i);
        float ax = fabsf(n.x), ay = fabsf(n.y), az = fabsf(n.z);
        if (fabsf(ax + ay + az - 1.0f) > 1e-4f)
            { fail("square sweep: normal not axis-aligned"); break; }
    }
    check_consistency(&b, 0.999f, "square sweep");
    mb_free(&b);

    /* abutting ends: caps off emits the 4 side quads only */
    mb_init(&b);
    gothic_sweep(&b, SQUARE, 5, path, 2, v3(0, 1, 0), 1.0f, 0, 0);
    if (b.index_count != 8 * 3) fail("capless sweep: triangle count != 8");
    mb_free(&b);
}

/* ---- 2: the 90-degree miter — picture-frame stretch + auto-crease ---- */
static void test_miter(void) {
    MeshBuilder b;
    vec3 path[3];
    sol_u32 i;
    int outer = 0, axis_ok = 1;

    mb_init(&b);
    path[0] = v3(0, 0, 0);
    path[1] = v3(1, 0, 0);
    path[2] = v3(1, 0, 1);
    gothic_sweep(&b, SQUARE, 5, path, 3, v3(0, 1, 0), 1.0f, 1, 1);

    /* the outer miter corner: profile o=+0.5 stretched by sqrt(2) along
       O_m = (1,0,-1)/sqrt2 -> exactly (1.5, +-0.5, -0.5): each segment's
       straight prism extended to the cut, the picture-frame rule */
    for (i = 0; i < b.vertex_count; i++)
        if (near3(vpos(&b, i), 1.5f, 0.5f, -0.5f, 1e-5f)) outer = 1;
    if (!outer) fail("miter: outer corner not at the sqrt(2) stretch");

    /* the 90-degree joint auto-creases: every stored normal stays a pure
       axis (per-segment frames, no smeared bisector shading) */
    for (i = 0; i < b.vertex_count; i++) {
        vec3 n = vnrm(&b, i);
        if (fabsf(fabsf(n.x) + fabsf(n.y) + fabsf(n.z) - 1.0f) > 1e-4f)
            axis_ok = 0;
    }
    if (!axis_ok) fail("miter: joint normals not per-segment axes (auto-crease)");

    check_consistency(&b, 0.99f, "miter");
    mb_free(&b);
}

/* ---- 3: a real molding around the corner — smooth, pinch-free ---- */
static void test_string_course(void) {
    MeshBuilder b;
    vec3 path[3];
    int n;
    const ProfilePt *prof = gothic_profile(PROF_STRING, &n);

    mb_init(&b);
    path[0] = v3(0, 0, 0);
    path[1] = v3(1.2f, 0, 0);
    path[2] = v3(1.2f, 0, 0.8f);
    gothic_sweep(&b, prof, n, path, 3, v3(0, 1, 0), 1.0f, 1, 1);
    if (b.index_count == 0) fail("string course: emitted nothing");
    /* rolls average facet normals; the bar is consistency, not flatness */
    check_consistency(&b, 0.5f, "string course");
    mb_free(&b);
}

/* ---- 4: the molding table ---- */
static void test_table(void) {
    int id, n;
    for (id = 0; id < PROF_COUNT; id++) {
        const ProfilePt *p = gothic_profile(id, &n);
        float area = 0.0f;
        int j;
        if (!p || n < 6 || n > 12) { fail("table: profile out of spec"); return; }
        /* CCW check: shoelace over the polygon (open profiles close along
           the o=0 wall line, which IS their un-emitted back) */
        for (j = 0; j < n; j++) {
            const ProfilePt *a = &p[j], *c = &p[(j + 1) % n];
            area += a->o * c->u - c->o * a->u;
        }
        if (area <= 0.0f) { fail("table: profile not CCW"); return; }
    }
    /* closed sections close; the seam is creased */
    {
        const ProfilePt *p = gothic_profile(PROF_MULLION, &n);
        if (p[0].o != p[n - 1].o || p[0].u != p[n - 1].u)
            fail("table: mullion does not close");
        p = gothic_profile(PROF_SHAFT_OCT, &n);
        if (p[0].o != p[n - 1].o || p[0].u != p[n - 1].u)
            fail("table: shaft does not close");
    }
    if (gothic_profile(PROF_COUNT, &n) != (const ProfilePt *)0)
        fail("table: unknown id must return NULL");
}

/* ---- 5: tessellation — two caps, take the finer ---- */
static void test_arc_segments(void) {
    /* a 0.3 m tracery foil: the linear cap alone would say 4 — the
       angular cap rescues it to a 16-gon */
    if (gothic_arc_segments(0.94248f, 6.28319f) != 16)
        fail("arc segments: angular cap must bind on small circles");
    /* a big quarter arc: 6.283 m / 0.25 = 25.13 -> the linear cap binds */
    if (gothic_arc_segments(6.28319f, 1.5708f) != 26)
        fail("arc segments: linear cap must bind on big arcs");
    if (gothic_arc_segments(0.1f, 0.0f) != 1)
        fail("arc segments: minimum is 1");
}

/* ---- 6: §1.8 — bit-determinism, through the registry row ---- */
static void test_determinism(void) {
    static const float P_BENT[5] = { 2.0f, 3.0f, 1.0f, 180.0f, 1.0f };
    MeshBuilder a, b;
    int k;
    for (k = 0; k < 2; k++) {
        const float *params = k ? P_BENT : (const float *)0;
        int count = k ? 5 : 0;
        mb_init(&a); mb_init(&b);
        if (!mesh_ref_build("molding", params, count, &a) ||
            !mesh_ref_build("molding", params, count, &b)) {
            fail("determinism: registry refused the molding"); return;
        }
        if (a.vertex_count != b.vertex_count || a.index_count != b.index_count)
            fail("determinism: counts differ between identical builds");
        else if (memcmp(a.vertices, b.vertices,
                        (size_t)a.vertex_count * 12 * sizeof(sol_f32)) != 0 ||
                 memcmp(a.indices, b.indices,
                        (size_t)a.index_count * sizeof(sol_u32)) != 0)
            fail("determinism: identical params, different bytes");
        if (a.index_count == 0) fail("determinism: molding emitted nothing");
        check_consistency(&a, 0.5f, k ? "bent molding" : "default molding");
        mb_free(&a); mb_free(&b);
    }
}

int main(void) {
    test_square_is_box();
    test_miter();
    test_string_course();
    test_table();
    test_arc_segments();
    test_determinism();
    if (g_fail) { printf("gothic_test: FAILED\n"); return 1; }
    printf("gothic_test: OK\n");
    return 0;
}
