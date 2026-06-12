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
#include <stdlib.h>
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
                vec3 p0 = vpos(b, b->indices[t * 3]);
                vec3 p1 = vpos(b, b->indices[t * 3 + 1]);
                vec3 p2 = vpos(b, b->indices[t * 3 + 2]);
                printf("FAIL: %s: winding vs normal tri %u (dot %.3f)"
                       " v0(%.2f %.2f %.2f) v1(%.2f %.2f %.2f)"
                       " v2(%.2f %.2f %.2f) n(%.2f %.2f %.2f)\n",
                       who, (unsigned)t, vec3_dot(gn, n),
                       p0.x, p0.y, p0.z, p1.x, p1.y, p1.z,
                       p2.x, p2.y, p2.z, n.x, n.y, n.z);
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

/* ---- 6: §1.8 — bit-determinism, through the registry rows ---- */
static void det_check(const char *ref, const float *params, int count,
                      float min_dot) {
    MeshBuilder a, b;
    mb_init(&a); mb_init(&b);
    if (!mesh_ref_build(ref, params, count, &a) ||
        !mesh_ref_build(ref, params, count, &b)) {
        printf("FAIL: determinism: registry refused %s\n", ref);
        g_fail = 1; return;
    }
    if (a.vertex_count != b.vertex_count || a.index_count != b.index_count)
        { printf("FAIL: determinism: %s counts differ\n", ref); g_fail = 1; }
    else if (memcmp(a.vertices, b.vertices,
                    (size_t)a.vertex_count * 12 * sizeof(sol_f32)) != 0 ||
             memcmp(a.indices, b.indices,
                    (size_t)a.index_count * sizeof(sol_u32)) != 0)
        { printf("FAIL: determinism: %s identical params, different bytes\n", ref); g_fail = 1; }
    if (a.index_count == 0)
        { printf("FAIL: determinism: %s emitted nothing\n", ref); g_fail = 1; }
    check_consistency(&a, min_dot, ref);
    mb_free(&a); mb_free(&b);
}

static void test_determinism(void) {
    static const float P_BENT[5] = { 2.0f, 3.0f, 1.0f, 180.0f, 1.0f };
    det_check("molding", (const float *)0, 0, 0.5f);
    det_check("molding", P_BENT, 5, 0.5f);
    det_check("wall_arched", (const float *)0, 0, 0.99f);
    det_check("portal", (const float *)0, 0, 0.5f);
}

/* ---- 7: the arch math — closed forms & the level-crown solve ---- */
static void test_arch_math(void) {
    static const float S[5] = { 0.6f, 1.0f, 2.0f, 5.0f, 9.0f };
    static const float F[4] = { 0.55f, 0.75f, 1.0f, 1.6f };
    int i, j;
    for (i = 0; i < 5; i++) {
        float s = S[i];
        if (fabsf(gothic_arch_y(s, 1.0f, 0.0f) - 0.8660254f * s) > 1e-5f * s)
            fail("arch: equilateral crown != 0.866*s");
        if (fabsf(gothic_arch_y(s, 0.0f, 0.0f) - 0.5f * s) > 1e-6f * s)
            fail("arch: semicircular crown != s/2");
        if (gothic_arch_y(s, 1.0f, 0.5f * s) != 0.0f)
            fail("arch: head must be 0 at the springing");
        for (j = 0; j < 4; j++) {
            float hgt = F[j] * s;
            float ac  = gothic_arch_acuteness_for(s, hgt);
            if (fabsf(gothic_arch_y(s, ac, 0.0f) - hgt) >
                2e-5f * (s > 1.0f ? s : 1.0f))
                fail("arch: acuteness solve does not round-trip");
        }
    }
    if (gothic_arch_acuteness_for(2.0f, 0.5f) != 0.0f)
        fail("arch: flatter-than-round must clamp to the semicircle");
}

/* ---- 8: the arch polyline — odd, exact, symmetric, monotone ---- */
static void test_arch_path(void) {
    vec3 p[GOTHIC_ARCH_MAX_PTS];
    int  n = gothic_arch_path(p, GOTHIC_ARCH_MAX_PTS, 1.5f, 1.0f, GOTHIC_MAX_SEG);
    int  i;
    if (n <= 0 || (n & 1) == 0) { fail("arch path: count must be odd"); return; }
    if (p[0].x != -0.75f || p[0].y != 0.0f ||
        p[n - 1].x != 0.75f || p[n - 1].y != 0.0f)
        fail("arch path: springings must be exact");
    if (p[(n - 1) / 2].x != 0.0f)
        fail("arch path: the apex must be a vertex at x = 0");
    if (fabsf(p[(n - 1) / 2].y - 0.8660254f * 1.5f) > 1e-5f)
        fail("arch path: apex height off the closed form");
    for (i = 0; i < n; i++)
        if (p[i].x != -p[n - 1 - i].x || p[i].y != p[n - 1 - i].y)
            { fail("arch path: not bit-symmetric"); break; }
    for (i = 0; i + 1 < n; i++)
        if (p[i + 1].x <= p[i].x)
            { fail("arch path: x must be monotone"); break; }
}

/* count the triangles using positional edge (p,q), either direction */
static int edge_uses(const MeshBuilder *b, vec3 p, vec3 q) {
    sol_u32 t, k;
    int uses = 0;
    for (t = 0; t < b->index_count / 3; t++) {
        for (k = 0; k < 3; k++) {
            vec3 e0 = vpos(b, b->indices[t * 3 + k]);
            vec3 e1 = vpos(b, b->indices[t * 3 + (k + 1) % 3]);
            if ((near3(e0, p.x, p.y, p.z, 1e-5f) && near3(e1, q.x, q.y, q.z, 1e-5f)) ||
                (near3(e0, q.x, q.y, q.z, 1e-5f) && near3(e1, p.x, p.y, p.z, 1e-5f)))
                uses++;
        }
    }
    return uses;
}

/* every polyline edge at a given z-plane must be used by EXACTLY two
   triangles — the head/intrados (or intrados/ladder) seam closes with
   shared stations, no cracks, no T-junctions (TODO6 item 2 acceptance) */
static void check_arch_seams(const MeshBuilder *b, const vec3 *arc, int n,
                             float cx, float spring, float z, const char *who) {
    int j;
    for (j = 0; j + 1 < n; j++) {
        vec3 p = v3(cx + arc[j].x,     spring + arc[j].y,     z);
        vec3 q = v3(cx + arc[j + 1].x, spring + arc[j + 1].y, z);
        int uses = edge_uses(b, p, q);
        if (uses != 2) {
            printf("FAIL: %s: seam edge %d at z=%.3f used %d times (want 2)\n",
                   who, j, z, uses);
            g_fail = 1; return;
        }
    }
}

/* ---- 9: the arched wall — seams closed on both faces ---- */
static void test_wall_arched(void) {
    MeshBuilder b;
    vec3 arc[GOTHIC_ARCH_MAX_PTS];
    int  n;

    mb_init(&b);
    gothic_wall_arched(&b, 4.0f, 3.5f, 0.3f, 1.25f, 1.5f, 1.4f, 1.0f);
    if (b.index_count == 0) { fail("arched wall: emitted nothing"); mb_free(&b); return; }
    check_consistency(&b, 0.99f, "arched wall");

    n = gothic_arch_path(arc, GOTHIC_ARCH_MAX_PTS, 1.5f, 1.0f, GOTHIC_MAX_SEG);
    check_arch_seams(&b, arc, n, 0.0f, 1.4f,  0.15f, "arched wall front");
    check_arch_seams(&b, arc, n, 0.0f, 1.4f, -0.15f, "arched wall back");
    mb_free(&b);

    /* a crown above the wall top is impossible: emit nothing, loudly no */
    mb_init(&b);
    gothic_wall_arched(&b, 4.0f, 2.0f, 0.3f, 1.25f, 1.5f, 1.4f, 1.0f);
    if (b.index_count != 0) fail("arched wall: impossible crown must emit nothing");
    mb_free(&b);
}

/* ---- 10: the portal — the widest order's seams close through the
   step ladder (front face + first slab boundary); inner orders are the
   same machinery at the same shared station count ---- */
static void test_portal(void) {
    MeshBuilder b;
    vec3 arc[GOTHIC_ARCH_MAX_PTS];
    int  n;

    mb_init(&b);
    gothic_wall_portal(&b, 6.0f, 5.0f, 0.9f, 1.6f, 2.2f, 1.0f, 3, 0.18f, 1);
    if (b.index_count == 0) { fail("portal: emitted nothing"); mb_free(&b); return; }
    check_consistency(&b, 0.5f, "portal");

    /* widest span = ow + 2*step*(orders-1) = 2.32; front plane z = 0.45,
       its slab boundary at z = 0.45 - 0.9/3 = 0.15 */
    n = gothic_arch_path(arc, GOTHIC_ARCH_MAX_PTS, 2.32f, 1.0f, GOTHIC_MAX_SEG);
    check_arch_seams(&b, arc, n, 0.0f, 2.2f, 0.45f, "portal front");
    check_arch_seams(&b, arc, n, 0.0f, 2.2f, 0.15f, "portal step");
    mb_free(&b);
}

/* ---- 11: the plan invariants — ~200 seeds x sizes (TODO6 item 3) ---- */
static void check_one_plan(const ChurchPlan *p, float pl_in, float pw_in) {
    float hl = 0.5f * p->plot_l, hw = 0.5f * p->plot_w;
    float x, z;
    int   i, j;

    /* the frame: plot_l is the long dimension, swapped honest */
    if (p->plot_l + 1e-5f < p->plot_w) { fail("plan: plot_l must be the long axis"); return; }
    if (!p->swapped && pw_in > pl_in && pw_in > 6.0f)
        { fail("plan: deeper-than-wide must set swapped"); return; }

    /* the module within the ad-quadratum band */
    if (p->bay_l < 0.43f * p->nave_w || p->bay_l > 0.57f * p->nave_w)
        { fail("plan: bay module out of the ad-quadratum band"); return; }
    if (p->nbays < 1 || p->nbays > PLAN_MAX_BAYS)
        { fail("plan: bay count out of range"); return; }

    /* the body and annexes fit inside the usable rect */
    if (p->west_x < -hl + p->margin - 1e-4f)
        { fail("plan: body west of the usable rect"); return; }
    if (p->east_x + p->apse_d > hl - p->margin + 1e-3f)
        { fail("plan: body+apse east of the usable rect"); return; }
    if (0.5f * p->nave_w + p->aisle_w > hw - p->margin + 1e-3f)
        { fail("plan: body wider than the usable rect"); return; }

    /* every pier inside the plot; every nave bay's four piers exist */
    for (i = 0; i <= p->nbays; i++)
        for (j = 0; j <= PIER_ROW_N_WALL; j++) {
            if (!plan_pier(p, i, j, &x, &z)) {
                if (p->aisles) { fail("plan: aisled church missing a pier"); return; }
                continue;
            }
            if (x < -hl - 1e-4f || x > hl + 1e-4f ||
                z < -hw - 1e-4f || z > hw + 1e-4f)
                { fail("plan: pier outside the plot"); return; }
        }
    if (plan_pier(p, p->nbays + 1, 0, &x, &z) ||
        plan_pier(p, -1, 0, &x, &z) || plan_pier(p, 0, 4, &x, &z))
        { fail("plan: out-of-range pier query must refuse"); return; }

    /* the apse closes: 6 vertices on one circle, mouth on the east wall */
    if (p->apse_sides == 5) {
        float r   = 0.5411961f * p->nave_w;
        float cxa = p->east_x + 0.3826834f * r;
        float zk[6], xk[6];
        for (i = 0; i < 6; i++) {
            float dx, dz;
            if (!plan_apse_pier(p, i, &xk[i], &zk[i]))
                { fail("plan: apse vertex missing"); return; }
            dx = xk[i] - cxa; dz = zk[i];
            if (fabsf(sqrtf(dx * dx + dz * dz) - r) > 1e-4f)
                { fail("plan: apse vertex off the circle"); return; }
        }
        if (fabsf(xk[0] - p->east_x) > 1e-4f ||
            fabsf(zk[0] + 0.5f * p->nave_w) > 1e-4f ||
            fabsf(zk[5] - 0.5f * p->nave_w) > 1e-4f)
            { fail("plan: apse mouth off the east wall"); return; }
        for (i = 0; i < 6; i++)
            if (fabsf(zk[i] + zk[5 - i]) > 1e-4f || fabsf(xk[i] - xk[5 - i]) > 1e-4f)
                { fail("plan: apse not symmetric"); return; }
    }

    /* openings: inside their bays, crowns under their limits */
    for (i = 0; i < p->nbays; i++) {
        GothicOpening o;
        float bx0 = p->west_x + p->tower_d + (float)i * p->bay_l;
        plan_opening(p, WALL_AISLE_S, i, &o);
        if (o.kind == GOTHIC_OPEN_WINDOW) {
            if (o.cx - 0.5f * o.w < bx0 - 1e-4f ||
                o.cx + 0.5f * o.w > bx0 + p->bay_l + 1e-4f)
                { fail("plan: window outside its bay"); return; }
            if (o.spring + gothic_arch_y(o.w, o.acute, 0.0f) >
                p->aisle_h - 0.8f + 1e-3f)
                { fail("plan: window crown into the wall head"); return; }
        }
        plan_opening(p, WALL_CLEREST_S, i, &o);
        if (o.kind == GOTHIC_OPEN_WINDOW &&
            o.spring + gothic_arch_y(o.w, o.acute, 0.0f) >
            p->clerest_h1 - 0.3f + 1e-3f)
            { fail("plan: clerestory crown out of its band"); return; }
    }
    {   /* the portal admits the capsule, the great window fits above */
        GothicOpening o;
        plan_opening(p, WALL_WEST, 0, &o);
        if (o.kind != GOTHIC_OPEN_DOOR || o.w < 1.2f - 1e-5f || o.spring < 1.9f)
            { fail("plan: portal must admit the capsule with margin"); return; }
        plan_opening(p, WALL_WEST, 1, &o);
        if (o.kind == GOTHIC_OPEN_WINDOW &&
            o.spring + gothic_arch_y(o.w, o.acute, 0.0f) > p->wall_h - 0.8f + 1e-3f)
            { fail("plan: great window into the wall head"); return; }
    }
}

static void test_plan(void) {
    static const float SIZES[5][2] = {
        { 18.0f, 30.0f }, { 14.0f, 22.0f }, { 30.0f, 46.0f },
        { 9.0f, 12.0f },  { 40.0f, 24.0f }
    };
    int si, seed;
    for (si = 0; si < 5 && !g_fail; si++) {
        for (seed = 0; seed < 40 && !g_fail; seed++) {
            float params[8];
            ChurchPlan p, q;
            params[0] = SIZES[si][0]; params[1] = SIZES[si][1];
            params[2] = (float)seed;  params[3] = -1.0f;
            params[4] = 0.0f; params[5] = 1.0f; params[6] = 1.0f;
            params[7] = 0.0f;
            church_plan(&p, params, 8);
            church_plan(&q, params, 8);
            if (memcmp(&p, &q, sizeof p) != 0)
                { fail("plan: identical params, different structs"); return; }
            check_one_plan(&p, params[0], params[1]);
        }
    }
    {   /* forced styles honored; derive picks chapel for small plots */
        float params[8] = { 18, 30, 7, 0, 0, 1, 1, 0 };
        ChurchPlan p;
        int s;
        for (s = 0; s <= 2; s++) {
            params[3] = (float)s;
            church_plan(&p, params, 8);
            if (p.style != s) { fail("plan: forced style not honored"); return; }
        }
        params[0] = 9.0f; params[1] = 12.0f; params[3] = -1.0f;
        church_plan(&p, params, 8);
        if (p.style != CHURCH_CHAPEL)
            { fail("plan: small plot must derive chapel"); return; }
        if (p.aisles || plan_pier(&p, 0, PIER_ROW_S_ARCADE, 0, 0))
            { fail("plan: chapel must have no arcade row"); return; }
    }
    {   /* a prefix is legal (the registry merge rule); the bay clamp holds */
        float params[3] = { 20.0f, 120.0f, 11.0f };
        ChurchPlan p;
        church_plan(&p, params, 3);
        if (p.nbays != PLAN_MAX_BAYS)
            { fail("plan: huge plot must clamp to PLAN_MAX_BAYS"); return; }
        if (!p.swapped) { fail("plan: 20x120 must swap"); return; }
        church_plan(&p, (const float *)0, 0);   /* all defaults */
        if (p.nbays < 2) { fail("plan: default church too short"); return; }
    }
}

/* ---- 12: item 4 — the stone shell ---- */

/* the coplanarity audit (TODO6 item 4): no two triangles may be
   coplanar AND overlapping — the shimmering-checkerboard defense,
   automated. Triangles bucket by their canonical plane; within a
   bucket, each pair is SAT-tested in 2D after shrinking toward their
   centroids (shared edges and adjacency pass; true overlap fails). */
typedef struct { long kx, ky, kz, kd; sol_u32 tri; } AuditEnt;

static int audit_cmp(const void *pa, const void *pb) {
    const AuditEnt *a = (const AuditEnt *)pa, *b = (const AuditEnt *)pb;
    if (a->kx != b->kx) return a->kx < b->kx ? -1 : 1;
    if (a->ky != b->ky) return a->ky < b->ky ? -1 : 1;
    if (a->kz != b->kz) return a->kz < b->kz ? -1 : 1;
    if (a->kd != b->kd) return a->kd < b->kd ? -1 : 1;
    return 0;
}

static int sat_separated(float A[3][2], float B[3][2]) {
    int i, j;
    for (i = 0; i < 3; i++) {
        float ex = A[(i + 1) % 3][0] - A[i][0];
        float ey = A[(i + 1) % 3][1] - A[i][1];
        float nx = -ey, ny = ex;
        float minA = 1e30f, maxA = -1e30f, minB = 1e30f, maxB = -1e30f;
        for (j = 0; j < 3; j++) {
            float pa = nx * A[j][0] + ny * A[j][1];
            float pb = nx * B[j][0] + ny * B[j][1];
            if (pa < minA) minA = pa;
            if (pa > maxA) maxA = pa;
            if (pb < minB) minB = pb;
            if (pb > maxB) maxB = pb;
        }
        if (maxB <= minA || maxA <= minB) return 1;
    }
    return 0;
}

/* project tri t onto the plane's dominant axes, shrunk toward centroid */
static void audit_project(const MeshBuilder *b, sol_u32 t, vec3 n,
                          float out[3][2]) {
    int ax0 = 0, ax1 = 1, k;
    float cx = 0.0f, cy = 0.0f;
    float anx = fabsf(n.x), any = fabsf(n.y), anz = fabsf(n.z);
    if (anx >= any && anx >= anz)      { ax0 = 1; ax1 = 2; }
    else if (any >= anx && any >= anz) { ax0 = 0; ax1 = 2; }
    for (k = 0; k < 3; k++) {
        vec3 p = vpos(b, b->indices[t * 3 + k]);
        const float *pp = &p.x;
        out[k][0] = pp[ax0]; out[k][1] = pp[ax1];
        cx += out[k][0] / 3.0f; cy += out[k][1] / 3.0f;
    }
    for (k = 0; k < 3; k++) {           /* shrink: adjacency passes */
        float dx = cx - out[k][0], dy = cy - out[k][1];
        float l = sqrtf(dx * dx + dy * dy);
        if (l > 1e-3f) { out[k][0] += dx / l * 1e-3f; out[k][1] += dy / l * 1e-3f; }
    }
}

static void audit_coplanar(const MeshBuilder *b, const char *who) {
    sol_u32 nt = b->index_count / 3, t;
    AuditEnt *ents = (AuditEnt *)malloc((size_t)nt * sizeof(AuditEnt));
    sol_u32 i0, i1;
    int bad = 0;
    if (!ents) { fail("audit: out of memory"); return; }
    for (t = 0; t < nt; t++) {
        vec3 n = tri_gnormal(b, t);
        float d = vec3_dot(n, vpos(b, b->indices[t * 3]));
        /* canonical sign: opposite-facing coincident planes must share
           a bucket — that is exactly the z-fight pair */
        if (n.x < -1e-6f || (fabsf(n.x) <= 1e-6f && n.y < -1e-6f) ||
            (fabsf(n.x) <= 1e-6f && fabsf(n.y) <= 1e-6f && n.z < 0.0f)) {
            n = vec3_scale(n, -1.0f); d = -d;
        }
        ents[t].kx = (long)floor(n.x * 512.0 + 0.5);
        ents[t].ky = (long)floor(n.y * 512.0 + 0.5);
        ents[t].kz = (long)floor(n.z * 512.0 + 0.5);
        ents[t].kd = (long)floor(d * 512.0 + 0.5);
        ents[t].tri = t;
    }
    qsort(ents, (size_t)nt, sizeof(AuditEnt), audit_cmp);
    for (i0 = 0; i0 < nt && !bad; i0 = i1) {
        sol_u32 a, c;
        i1 = i0 + 1;
        while (i1 < nt && audit_cmp(&ents[i0], &ents[i1]) == 0) i1++;
        for (a = i0; a < i1 && !bad; a++)
            for (c = a + 1; c < i1 && !bad; c++) {
                float A[3][2], B[3][2];
                vec3 n = tri_gnormal(b, ents[a].tri);
                audit_project(b, ents[a].tri, n, A);
                audit_project(b, ents[c].tri, n, B);
                if (!sat_separated(A, B) && !sat_separated(B, A)) {
                    int q;
                    printf("FAIL: %s: coplanar overlap, tris %u and %u\n",
                           who, (unsigned)ents[a].tri, (unsigned)ents[c].tri);
                    for (q = 0; q < 3; q++) {
                        vec3 pa = vpos(b, b->indices[ents[a].tri * 3 + q]);
                        vec3 pc = vpos(b, b->indices[ents[c].tri * 3 + q]);
                        printf("  A(%.3f %.3f %.3f)  B(%.3f %.3f %.3f)\n",
                               pa.x, pa.y, pa.z, pc.x, pc.y, pc.z);
                    }
                    g_fail = 1; bad = 1;
                }
            }
    }
    free(ents);
}

static void test_church_stone(void) {
    static const float SEEDS[3] = { 1.0f, 7.0f, 23.0f };
    int s, st;

    {   /* the schema-agreement law: the registry row's defaults ARE the
           plan's (two tables, one truth — asserted, not hoped) */
        const char *const *names; const float *defs;
        int n = mesh_ref_schema("church_stone", &names, &defs);
        if (n != 8 || memcmp(defs, gothic_church_defaults, 8 * sizeof(float)) != 0)
            fail("church_stone: registry defaults drifted from the plan's");
    }
    det_check("church_stone", (const float *)0, 0, 0.4f);

    for (st = 0; st <= 2; st++)
        for (s = 0; s < 3; s++) {
            float params[8];
            MeshBuilder b;
            params[0] = 18.0f; params[1] = 30.0f; params[2] = SEEDS[s];
            params[3] = (float)st; params[4] = 0.0f; params[5] = 1.0f;
            params[6] = 1.0f; params[7] = 0.0f;
            mb_init(&b);
            church_stone(&b, params, 8);
            if (b.index_count == 0) { fail("church_stone: emitted nothing"); mb_free(&b); return; }
            check_consistency(&b, 0.4f, "church_stone");
            audit_coplanar(&b, st == 0 ? "chapel" : st == 1 ? "hall" : "basilica");
            mb_free(&b);
            if (g_fail) return;
        }

    {   /* the budget: a long basilica shell within ~60k triangles */
        float params[8] = { 26.0f, 58.0f, 3.0f, 2.0f, 0.0f, 1.0f, 1.0f, 0.0f };
        MeshBuilder b;
        mb_init(&b);
        church_stone(&b, params, 8);
        if (b.index_count / 3 >= 60000)
            fail("church_stone: basilica shell over the 60k budget");
        if (b.index_count / 3 < 1000)
            fail("church_stone: basilica suspiciously empty");
        printf("church_stone: basilica shell %u tris\n",
               (unsigned)(b.index_count / 3));
        mb_free(&b);
    }
}

int main(void) {
    test_square_is_box();
    test_miter();
    test_string_course();
    test_table();
    test_arc_segments();
    test_determinism();
    test_arch_math();
    test_arch_path();
    test_wall_arched();
    test_portal();
    test_plan();
    test_church_stone();
    if (g_fail) { printf("gothic_test: FAILED\n"); return 1; }
    printf("gothic_test: OK\n");
    return 0;
}

