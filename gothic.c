/* gothic.c — the Gothic kit, item 1 (P6): the profile sweep & the molding
   table. Pure CPU — a client of the MeshBuilder with collide.c's
   citizenship (strict C89, headless-testable, no rhi/scene includes).
   mesh.c's registry rows compose these emitters; gothic.c knows geometry
   only (TODO6 §1.7).

   The sweep is the gate primitive: every molded element in the building —
   rib, archivolt, mullion, string course, hood-mold, shaft, base — is a
   small 2D section extruded along a path. v1 paths are PLANAR (TODO6
   item 1): the binormal is the path plane's normal, constant along the
   sweep, so framing is exact — no parallel transport, no torsion. At
   interior joints the ring lies in the MITER plane (normal = bisector of
   the adjacent segment directions) with the in-plane axis stretched by
   1/cos(half-turn), so each segment's faces remain planar continuations
   of its straight prism — the picture-frame rule: a 45-degree cut is
   wider than its molding by sqrt(2). */

#include "gothic.h"
#include "sol_math.h"

#include <math.h>

/* ---- the molding table (§1.6: the glossary is the namespace) ----
   Sections in meters at canonical scale, in (o,u): o projects out of the
   wall/core plane at o=0, u spans the section's height. All profiles are
   wound CCW (interior on the left) so the outward normal of edge (eo,eu)
   is (eu,-eo). Open profiles walk bottom -> out -> top; their o=0 faces
   are never emitted (they abut the wall — §1.2 abutment). Closed profiles
   repeat their first point, creased at the seam. */

/* chamfered vault rib: side fillets, chamfers, a roll at the soffit —
   symmetric about u=0; the web sits at o=0 */
static const ProfilePt PR_RIB[9] = {
    { 0.000f, -0.090f, 1 }, { 0.100f, -0.090f, 1 }, { 0.160f, -0.045f, 1 },
    { 0.175f, -0.030f, 0 }, { 0.190f,  0.000f, 0 }, { 0.175f,  0.030f, 0 },
    { 0.160f,  0.045f, 1 }, { 0.100f,  0.090f, 1 }, { 0.000f,  0.090f, 1 }
};

/* the standard double-chamfer mullion: a free-standing lozenge between
   two lights (closed; the glass meets it at +-u) */
static const ProfilePt PR_MULLION[7] = {
    {  0.080f,  0.000f, 1 }, {  0.030f,  0.050f, 1 }, { -0.030f,  0.050f, 1 },
    { -0.080f,  0.000f, 1 }, { -0.030f, -0.050f, 1 }, {  0.030f, -0.050f, 1 },
    {  0.080f,  0.000f, 1 }
};

/* string course: roll below, cavetto above, a weathered top shedding
   water back to the wall */
static const ProfilePt PR_STRING[11] = {
    { 0.000f, 0.000f, 1 }, { 0.050f, 0.000f, 1 }, { 0.075f, 0.020f, 0 },
    { 0.085f, 0.045f, 0 }, { 0.075f, 0.070f, 0 }, { 0.055f, 0.075f, 1 },
    { 0.060f, 0.100f, 0 }, { 0.075f, 0.130f, 0 }, { 0.095f, 0.150f, 0 },
    { 0.100f, 0.155f, 1 }, { 0.000f, 0.190f, 1 }
};

/* the attic base: lower torus, scotia, upper torus, tucking to the shaft
   at the top — the square sub-plinth is the pier emitter's (item 4) */
static const ProfilePt PR_BASE[12] = {
    { 0.100f, 0.000f, 1 }, { 0.115f, 0.020f, 0 }, { 0.120f, 0.045f, 0 },
    { 0.110f, 0.070f, 0 }, { 0.095f, 0.080f, 1 }, { 0.075f, 0.105f, 0 },
    { 0.078f, 0.130f, 0 }, { 0.090f, 0.140f, 1 }, { 0.100f, 0.160f, 0 },
    { 0.095f, 0.180f, 0 }, { 0.080f, 0.190f, 0 }, { 0.000f, 0.200f, 1 }
};

/* hood-mold: a projecting drip over an arch, weathered on top */
static const ProfilePt PR_HOOD[6] = {
    { 0.000f, 0.000f, 1 }, { 0.055f, 0.000f, 1 }, { 0.065f, 0.020f, 0 },
    { 0.070f, 0.045f, 0 }, { 0.060f, 0.070f, 1 }, { 0.000f, 0.100f, 1 }
};

/* octagonal shaft section (closed): circumradius 0.12, faces axis-aligned
   (vertices at 22.5 + k*45 degrees), every arris hard */
static const ProfilePt PR_SHAFT_OCT[9] = {
    {  0.110866f,  0.045922f, 1 }, {  0.045922f,  0.110866f, 1 },
    { -0.045922f,  0.110866f, 1 }, { -0.110866f,  0.045922f, 1 },
    { -0.110866f, -0.045922f, 1 }, { -0.045922f, -0.110866f, 1 },
    {  0.045922f, -0.110866f, 1 }, {  0.110866f, -0.045922f, 1 },
    {  0.110866f,  0.045922f, 1 }
};

const ProfilePt *gothic_profile(int prof_id, int *out_n) {
    switch (prof_id) {
    case PROF_RIB:       *out_n = 9;  return PR_RIB;
    case PROF_MULLION:   *out_n = 7;  return PR_MULLION;
    case PROF_STRING:    *out_n = 11; return PR_STRING;
    case PROF_BASE:      *out_n = 12; return PR_BASE;
    case PROF_HOOD:      *out_n = 6;  return PR_HOOD;
    case PROF_SHAFT_OCT: *out_n = 9;  return PR_SHAFT_OCT;
    default:             *out_n = 0;  return (const ProfilePt *)0;
    }
}

int gothic_arc_segments(float arc_len, float arc_angle) {
    return sweep_segments(arc_len, arc_angle, GOTHIC_MAX_SEG);
}

/* ---- the sweep (sweep.c since P7 item 1) ----
   The kit's untapered idiom: one line over the lathe — the name stayed,
   so no call site ever moved and the suite proved the extraction
   byte-identical. Taper lives in sweep_extrude for flora's branches. */
void gothic_sweep(MeshBuilder *b, const ProfilePt *prof, int prof_n,
                  const vec3 *path, int path_n, vec3 plane_n,
                  float scale, int cap0, int cap1) {
    sweep_extrude(b, prof, prof_n, path, path_n, plane_n, scale,
                  (const float *)0, cap0, cap1);
}

/* ================== item 2: the arch family ==================
   The two-arc pointed construction (see gothic.h): centers ON the
   springing line at -+c, r = c + s/2, acuteness a = 2c/s. One float,
   the whole historical family — and the level-crown solve below is
   *why* Gothic exists: arches of different spans reaching one height. */

static void arch_cr(float s, float a, float *c, float *r) {
    *c = 0.5f * a * s;
    *r = *c + 0.5f * s;
}

float gothic_arch_y(float s, float a, float x) {
    float c, r, q;
    if (s <= 0.0f || a < 0.0f) return 0.0f;
    arch_cr(s, a, &c, &r);
    if (x < 0.0f) x = -x;
    if (x >= 0.5f * s) return 0.0f;
    q = x + c;
    q = r * r - q * q;
    return q > 0.0f ? sqrtf(q) : 0.0f;
}

float gothic_arch_acuteness_for(float s, float crown_h) {
    float c;
    if (s <= 0.0f || crown_h <= 0.5f * s) return 0.0f;  /* round at flattest */
    c = (crown_h * crown_h - 0.25f * s * s) / s;
    return 2.0f * c / s;
}

/* segments per HALF arc by the two-cap rule (>= 2 so a head reads curved) */
static int arch_half_segments(float s, float a, float max_seg) {
    float c, r, hgt, phi;
    int   n;
    arch_cr(s, a, &c, &r);
    hgt = sqrtf(r * r - c * c);
    phi = atan2f(hgt, c);                 /* the half-arc's angular extent */
    n = sweep_segments(r * phi, phi, max_seg);
    return n < 2 ? 2 : n;
}

/* the polyline at a FIXED per-half count — the orders emitter forces one
   count across nested arches (same acuteness = same arc angle, so equal
   counts make every step-face ladder a clean 1:1 strip). The right half
   is computed and the left MIRRORED (bit-symmetry); springings and apex
   are then forced exact — the crown must be a vertex (item 5's bosses). */
static int arch_path_n(vec3 *out, float s, float a, int n_h) {
    float c, r, hgt, phi;
    int   k;
    arch_cr(s, a, &c, &r);
    hgt = sqrtf(r * r - c * c);
    phi = atan2f(hgt, c);
    for (k = 1; k < n_h; k++) {
        float t  = phi * (float)(n_h - k) / (float)n_h;
        float xr = -c + r * cosf(t);
        float yr = r * sinf(t);
        out[n_h + k] = vec3_make(xr, yr, 0.0f);
        out[n_h - k] = vec3_make(-xr, yr, 0.0f);
    }
    out[0]       = vec3_make(-0.5f * s, 0.0f, 0.0f);
    out[2 * n_h] = vec3_make( 0.5f * s, 0.0f, 0.0f);
    out[n_h]     = vec3_make(0.0f, hgt, 0.0f);
    return 2 * n_h + 1;
}

int gothic_arch_path(vec3 *out, int max_n, float s, float a, float max_seg) {
    int n_h;
    if (!out || s <= 0.0f || a < 0.0f || max_seg <= 0.0f) return 0;
    n_h = arch_half_segments(s, a, max_seg);
    if (2 * n_h + 1 > max_n) return 0;
    return arch_path_n(out, s, a, n_h);
}

/* ---------- the arched wall & recessed orders (§1.4 with curves) ----------
   Conventions mirror make_wall_with_opening exactly (mesh.c): exposed
   faces only, never coplanar, world-scale UVs, threshold top-only,
   panels emit tops AND bottoms, outer ends emitted, flush openings get
   the head's exposed end. mesh.c's face helpers are static, so the kit
   carries its own — same corner order, same UV scheme. */

static void g_quad4(MeshBuilder *b,
                    float x0, float y0, float z0, float u0, float v0,
                    float x1, float y1, float z1, float u1, float v1,
                    float x2, float y2, float z2, float u2, float v2,
                    float x3, float y3, float z3, float u3, float v3,
                    float nx, float ny, float nz) {
    sol_u32 a = mb_push_vertex(b, x0, y0, z0, nx, ny, nz, u0, v0);
    sol_u32 c = mb_push_vertex(b, x1, y1, z1, nx, ny, nz, u1, v1);
    sol_u32 d = mb_push_vertex(b, x2, y2, z2, nx, ny, nz, u2, v2);
    sol_u32 e = mb_push_vertex(b, x3, y3, z3, nx, ny, nz, u3, v3);
    mb_push_triangle(b, a, c, d);
    mb_push_triangle(b, a, d, e);
}

static void gz_face(MeshBuilder *b, float x0, float x1, float y0, float y1,
                    float z, int dir) {
    if (dir > 0)
        g_quad4(b, x0,y0,z, x0,y0,  x1,y0,z, x1,y0,  x1,y1,z, x1,y1,  x0,y1,z, x0,y1,
                0.0f, 0.0f, 1.0f);
    else
        g_quad4(b, x1,y0,z, x1,y0,  x0,y0,z, x0,y0,  x0,y1,z, x0,y1,  x1,y1,z, x1,y1,
                0.0f, 0.0f, -1.0f);
}
static void gx_face(MeshBuilder *b, float x, float y0, float y1,
                    float z0, float z1, int dir) {
    if (dir > 0)
        g_quad4(b, x,y0,z1, z1,y0,  x,y0,z0, z0,y0,  x,y1,z0, z0,y1,  x,y1,z1, z1,y1,
                1.0f, 0.0f, 0.0f);
    else
        g_quad4(b, x,y0,z0, z0,y0,  x,y0,z1, z1,y0,  x,y1,z1, z1,y1,  x,y1,z0, z0,y1,
                -1.0f, 0.0f, 0.0f);
}
static void gy_face(MeshBuilder *b, float x0, float x1, float y,
                    float z0, float z1, int dir) {
    if (dir > 0)
        g_quad4(b, x0,y,z0, x0,z0,  x0,y,z1, x0,z1,  x1,y,z1, x1,z1,  x1,y,z0, x1,z0,
                0.0f, 1.0f, 0.0f);
    else
        g_quad4(b, x0,y,z0, x0,z0,  x1,y,z0, x1,z0,  x1,y,z1, x1,z1,  x0,y,z1, x0,z1,
                0.0f, -1.0f, 0.0f);
}

/* one intrados strip: the reveal under the arch between consecutive
   polyline stations, across the slab's thickness — flat per strip
   (voussoir-scale faceting, deliberately not smoothed: TODO6 item 2),
   normal the in-plane arc normal pointing INTO the opening. u = arc
   length along the head, v = z: world-scale. */
static void intrados_strip(MeshBuilder *b, float xa, float ya, float xb,
                           float yb, float zb, float zf, float u0, float u1) {
    float dx = xb - xa, dy = yb - ya;
    float l  = sqrtf(dx * dx + dy * dy);
    float nx, ny;
    sol_u32 i0, i1, i2, i3;
    if (l < 1e-7f) return;
    nx = dy / l; ny = -dx / l;
    i0 = mb_push_vertex(b, xa, ya, zf, nx, ny, 0.0f, u0, zf);
    i1 = mb_push_vertex(b, xa, ya, zb, nx, ny, 0.0f, u0, zb);
    i2 = mb_push_vertex(b, xb, yb, zb, nx, ny, 0.0f, u1, zb);
    i3 = mb_push_vertex(b, xb, yb, zf, nx, ny, 0.0f, u1, zf);
    mb_push_triangle(b, i0, i1, i2);
    mb_push_triangle(b, i0, i2, i3);
}

/* one head strip: front/back wall face above the arch, curve to top */
static void head_strip(MeshBuilder *b, float xa, float ya, float xb, float yb,
                       float top, float z, int dir) {
    if (dir > 0)
        g_quad4(b, xa,ya,z, xa,ya,  xb,yb,z, xb,yb,  xb,top,z, xb,top,  xa,top,z, xa,top,
                0.0f, 0.0f, 1.0f);
    else
        g_quad4(b, xb,yb,z, xb,yb,  xa,ya,z, xa,ya,  xa,top,z, xa,top,  xb,top,z, xb,top,
                0.0f, 0.0f, -1.0f);
}

/* one step-face ladder rung: between the OUTER (wider, in front) and
   INNER order's arch stations at the slab boundary plane, facing +z —
   equal per-half counts make this an exact 1:1 strip, no T-junctions */
static void ladder_strip(MeshBuilder *b, float ox0, float oy0, float ox1,
                         float oy1, float ix0, float iy0, float ix1,
                         float iy1, float z) {
    g_quad4(b, ox0,oy0,z, ox0,oy0,  ix0,iy0,z, ix0,iy0,
               ix1,iy1,z, ix1,iy1,  ox1,oy1,z, ox1,oy1,
            0.0f, 0.0f, 1.0f);
}

/* abutment flags (item 4): a piece sitting on a plinth suppresses its
   bottom; a piece under another suppresses its top; a piece ending at a
   pier or another wall suppresses its end faces — faces where pieces
   abut are SKIPPED, the flat emitter's law at composition scale */
#define WF_NO_TOP    1
#define WF_NO_BOTTOM 2
#define WF_NO_ENDS   4
#define WF_NO_SILL   8   /* the threshold/sill ledge: a plinth or floor
                            below already claims that plane */

static void tri3(MeshBuilder *b, vec3 a, vec3 c, vec3 d);   /* item 7,
                                       defined below in reading order */
static void quad3(MeshBuilder *b, vec3 a, vec3 c, vec3 d, vec3 e);

/* the shared core: N recessed orders, widest at the FRONT (+z) face,
   sharing center cx, springing height and acuteness. Around the gap,
   slab by slab: each order owns its sub-thickness's jamb reveals,
   intrados strips, threshold/top/bottom strips; the wall's front face
   reads the widest arch, the back face the narrowest; step faces
   connect consecutive orders (jamb rectangles + the curved ladder).
   sill > 0 makes the opening a WINDOW: jamb reveals start at the sill,
   the threshold becomes the sill ledge, and a solid band fills the wall
   below (single-order only). Impossible parameters (crown above the
   top, opening past the wall) emit nothing — the plan never asks. */
static void arched_orders(MeshBuilder *b, float w, float h, float t,
                          float cx, float ow, float sill, float spring_h,
                          float a, int orders, float step, int archivolts,
                          int flags) {
    vec3  arch[GOTHIC_MAX_ORDERS][GOTHIC_ARCH_MAX_PTS];
    float x0[GOTHIC_MAX_ORDERS], x1[GOTHIC_MAX_ORDERS];
    float hw = 0.5f * w, zf, dt;
    int   n_h, np, k, j, has_l, has_r;

    if (orders < 1) orders = 1;
    if (orders > GOTHIC_MAX_ORDERS) orders = GOTHIC_MAX_ORDERS;
    if (step < 0.0f) step = 0.0f;
    if (t < 0.01f) t = 0.01f;
    if (sill < 0.0f) sill = 0.0f;
    if (sill > 0.0f) orders = 1;             /* windows are single-order */
    if (spring_h < sill + 0.05f) spring_h = sill + 0.05f;
    zf = 0.5f * t;
    dt = t / (float)orders;

    {
        float s0 = ow + 2.0f * step * (float)(orders - 1);
        n_h = arch_half_segments(s0, a, GOTHIC_MAX_SEG);
        if (2 * n_h + 1 > GOTHIC_ARCH_MAX_PTS) n_h = (GOTHIC_ARCH_MAX_PTS - 1) / 2;
        np = 2 * n_h + 1;
        for (k = 0; k < orders; k++) {
            float sk = ow + 2.0f * step * (float)(orders - 1 - k);
            arch_path_n(arch[k], sk, a, n_h);
            x0[k] = cx - 0.5f * sk;
            x1[k] = cx + 0.5f * sk;
        }
        if (spring_h + arch[0][n_h].y > h - 1e-4f) return;   /* crown clears top */
        if (x0[0] < -hw - 1e-5f || x1[0] > hw + 1e-5f) return;
        if (orders > 1 && (x0[0] < -hw + 1e-5f || x1[0] > hw - 1e-5f))
            return;                       /* flush is the single-order case */
    }
    has_l = x0[0] > -hw + 1e-5f;
    has_r = x1[0] <  hw - 1e-5f;

    /* outer ends: once, full height by full thickness */
    if (!(flags & WF_NO_ENDS)) {
        if (has_l) gx_face(b, -hw, 0.0f, h, -zf, zf, -1);
        if (has_r) gx_face(b,  hw, 0.0f, h, -zf, zf,  1);
    }

    for (k = 0; k < orders; k++) {
        float zk_f = zf - dt * (float)k;
        float zk_b = (k == orders - 1) ? -zf : zf - dt * (float)(k + 1);

        if (has_l) {
            if (!(flags & WF_NO_TOP))
                gy_face(b, -hw, x0[k], h,    zk_b, zk_f,  1);  /* panel tops */
            if (!(flags & WF_NO_BOTTOM))
                gy_face(b, -hw, x0[k], 0.0f, zk_b, zk_f, -1);
        }
        if (has_r) {
            if (!(flags & WF_NO_TOP))
                gy_face(b, x1[k], hw, h,    zk_b, zk_f,  1);
            if (!(flags & WF_NO_BOTTOM))
                gy_face(b, x1[k], hw, 0.0f, zk_b, zk_f, -1);
        }
        if (!(flags & WF_NO_TOP))
            gy_face(b, x0[k], x1[k], h, zk_b, zk_f, 1);      /* head top      */
        if (!(flags & WF_NO_SILL))
            gy_face(b, x0[k], x1[k], sill, zk_b, zk_f, 1);   /* threshold or
                                                                the sill ledge */
        /* jamb reveals — except on a FLUSH endless side, where the
           reveal IS an end face (it would z-fight the abutting wall) */
        if (has_l || !(flags & WF_NO_ENDS))
            gx_face(b, x0[k], sill, spring_h, zk_b, zk_f,  1);
        if (has_r || !(flags & WF_NO_ENDS))
            gx_face(b, x1[k], sill, spring_h, zk_b, zk_f, -1);
        if (!has_l && !(flags & WF_NO_ENDS))
            gx_face(b, x0[k], spring_h, h, zk_b, zk_f, -1);  /* flush head end */
        if (!has_r && !(flags & WF_NO_ENDS))
            gx_face(b, x1[k], spring_h, h, zk_b, zk_f,  1);
        if (sill > 0.0f) {                   /* the band below the window */
            gz_face(b, x0[k], x1[k], 0.0f, sill,  zf,  1);
            gz_face(b, x0[k], x1[k], 0.0f, sill, -zf, -1);
            if (!(flags & WF_NO_BOTTOM))
                gy_face(b, x0[k], x1[k], 0.0f, zk_b, zk_f, -1);
        }

        {                                  /* the intrados, strip by strip */
            float u = 0.0f;
            for (j = 0; j + 1 < np; j++) {
                float xa = cx + arch[k][j].x,     ya = spring_h + arch[k][j].y;
                float xb = cx + arch[k][j + 1].x, yb = spring_h + arch[k][j + 1].y;
                float dl = sqrtf((xb - xa) * (xb - xa) + (yb - ya) * (yb - ya));
                intrados_strip(b, xa, ya, xb, yb, zk_b, zk_f, u, u + dl);
                u += dl;
            }
        }
    }

    /* front face off the WIDEST arch, back face off the NARROWEST */
    if (has_l) gz_face(b, -hw, x0[0], 0.0f, h, zf, 1);
    if (has_r) gz_face(b, x1[0],  hw, 0.0f, h, zf, 1);
    for (j = 0; j + 1 < np; j++)
        head_strip(b, cx + arch[0][j].x,     spring_h + arch[0][j].y,
                      cx + arch[0][j + 1].x, spring_h + arch[0][j + 1].y,
                   h, zf, 1);
    k = orders - 1;
    if (has_l) gz_face(b, -hw, x0[k], 0.0f, h, -zf, -1);
    if (has_r) gz_face(b, x1[k],  hw, 0.0f, h, -zf, -1);
    for (j = 0; j + 1 < np; j++)
        head_strip(b, cx + arch[k][j].x,     spring_h + arch[k][j].y,
                      cx + arch[k][j + 1].x, spring_h + arch[k][j + 1].y,
                   h, -zf, -1);

    /* step faces between consecutive orders */
    for (k = 1; k < orders; k++) {
        float z = zf - dt * (float)k;
        gz_face(b, x0[k - 1], x0[k], 0.0f, spring_h, z, 1);
        gz_face(b, x1[k], x1[k - 1], 0.0f, spring_h, z, 1);
        for (j = 0; j + 1 < np; j++)
            ladder_strip(b,
                         cx + arch[k - 1][j].x,     spring_h + arch[k - 1][j].y,
                         cx + arch[k - 1][j + 1].x, spring_h + arch[k - 1][j + 1].y,
                         cx + arch[k][j].x,         spring_h + arch[k][j].y,
                         cx + arch[k][j + 1].x,     spring_h + arch[k][j + 1].y,
                         z);
    }

    /* archivolts: PROF_RIB swept along each order's arch at its front
       plane, path REVERSED so the section's o points radially INWARD —
       the roll hangs under the arris into the opening, the open o=0
       back rests against the intrados line, side faces straddle the
       step plane into the wider order's open air. No coplanar contact. */
    if (archivolts) {
        vec3 rev[GOTHIC_ARCH_MAX_PTS];
        int  pn;
        const ProfilePt *rib = gothic_profile(PROF_RIB, &pn);
        float sc = step * 3.5f;
        if (sc < 0.35f) sc = 0.35f;
        if (sc > 1.0f)  sc = 1.0f;
        for (k = 0; k < orders; k++) {
            float z = zf - dt * (float)k;
            for (j = 0; j < np; j++) {
                rev[j].x = cx + arch[k][np - 1 - j].x;
                rev[j].y = spring_h + arch[k][np - 1 - j].y;
                rev[j].z = z;
            }
            gothic_sweep(b, rib, pn, rev, np, vec3_make(0.0f, 0.0f, 1.0f),
                         sc, 1, 1);
        }
    }
}

void gothic_wall_arched(MeshBuilder *b, float w, float h, float t,
                        float ox, float ow, float spring_h, float a) {
    float hw;
    if (!b || w <= 0.0f || h <= 0.0f) return;
    hw = 0.5f * w;
    if (t < 0.01f) t = 0.01f;
    if (ox < 0.0f) ox = 0.0f;                /* clamp into the wall, like the */
    if (ox > w)    ox = w;                   /* flat emitter                  */
    if (ow > w - ox) ow = w - ox;
    if (ow < 1e-5f) {                        /* no opening: one solid box */
        float zf = 0.5f * t;
        gz_face(b, -hw, hw, 0.0f, h,  zf,  1);
        gz_face(b, -hw, hw, 0.0f, h, -zf, -1);
        gx_face(b, -hw, 0.0f, h, -zf, zf, -1);
        gx_face(b,  hw, 0.0f, h, -zf, zf,  1);
        gy_face(b, -hw, hw, h,    -zf, zf,  1);
        gy_face(b, -hw, hw, 0.0f, -zf, zf, -1);
        return;
    }
    arched_orders(b, w, h, t, -hw + ox + 0.5f * ow, ow, 0.0f, spring_h, a,
                  1, 0.0f, 0, 0);
}

void gothic_wall_portal(MeshBuilder *b, float w, float h, float t,
                        float ow, float spring_h, float a,
                        int orders, float step, int archivolts) {
    if (!b || w <= 0.0f || h <= 0.0f || ow <= 1e-5f) return;
    arched_orders(b, w, h, t, 0.0f, ow, 0.0f, spring_h, a,
                  orders, step, archivolts, 0);
}

/* ================== item 3: the plan function ==================
   One pure expansion; everything else is a reader (§1.2). Every random
   decision draws from a NAMED LANE (§1.3) so the seed's meaning can
   only grow, never shift. */

#include <string.h>

float gothic_hash01(unsigned seed, int lane, int i, int j) {
    unsigned h = seed;
    h ^= (unsigned)lane * 0x9E3779B9u;
    h ^= (unsigned)i    * 0x85EBCA6Bu;
    h ^= (unsigned)j    * 0xC2B2AE35u;
    h ^= h >> 16; h *= 0x7FEB352Du;
    h ^= h >> 15; h *= 0x846CA68Bu;
    h ^= h >> 16;
    return (float)(h & 0xFFFFFFu) / 16777216.0f;   /* 24 bits: mantissa-exact */
}

/* the church ref schema's defaults — mesh.c's registry rows carry the
   same values (gothictest asserts the two tables agree) */
const float gothic_church_defaults[8] =
    { 18.0f, 30.0f, 7.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f };

/* the 5/8 apse: 5 sides of an octagon whose mouth chord equals the nave
   width — mouth = 2 r sin(112.5), depth = r (cos(22.5) - cos(112.5)) */
#define APSE_R_PER_NAVE   0.5411961f    /* r / nave_w                  */
#define APSE_D_PER_NAVE   0.7071068f    /* depth / nave_w              */

void church_plan(ChurchPlan *p, const float *params, int count) {
    float full[8];
    float pl, pw, ul, uw, rem, roll;
    int   k;

    memset(p, 0, sizeof *p);                /* padding too: memcmp-clean */
    for (k = 0; k < 8; k++)
        full[k] = (params && k < count) ? params[k] : gothic_church_defaults[k];

    p->seed  = full[2] > 0.0f ? (unsigned)(full[2] + 0.5f) : 0u;
    p->acute = full[6] < 0.0f ? 0.0f : full[6];
    p->ruin  = full[4] < 0.0f ? 0.0f : full[4] > 1.0f ? 1.0f : full[4];
    p->built = full[5] < 0.0f ? 0.0f : full[5] > 1.0f ? 1.0f : full[5];

    /* 1. orientation: the nave runs the LONGER dimension; east = +X */
    pl = full[0]; pw = full[1];
    if (pl < 6.0f) pl = 6.0f;
    if (pw < 6.0f) pw = 6.0f;
    p->swapped = pw > pl;
    if (p->swapped) { float tmp = pl; pl = pw; pw = tmp; }
    p->plot_l = pl;
    p->plot_w = pw;

    /* 2. style: forced, or derived from the provisional interior area
       (margin is style-dependent, so derivation uses a 1 m stand-in) */
    k = (int)full[3];
    if (k >= CHURCH_CHAPEL && k <= CHURCH_BASILICA) {
        p->style = k;
    } else if ((pl - 2.0f) * (pw - 2.0f) < 120.0f) {
        p->style = CHURCH_CHAPEL;
    } else {
        float pb = ((pw - 2.0f) - 12.0f) / 6.0f;   /* basilica wants width */
        if (pb < 0.0f)   pb = 0.0f;
        if (pb > 0.85f)  pb = 0.85f;
        roll = gothic_hash01(p->seed, LANE_STYLE, 0, 0);
        p->style = roll < pb ? CHURCH_BASILICA : CHURCH_HALL;
    }
    p->aisles = p->style != CHURCH_CHAPEL;

    /* the buttress reserve (flyers want depth) */
    roll = gothic_hash01(p->seed, LANE_ELEV, 0, 0);
    if      (p->style == CHURCH_CHAPEL) p->margin = 0.8f + 0.2f * roll;
    else if (p->style == CHURCH_HALL)   p->margin = 1.2f + 0.4f * roll;
    else                                p->margin = 1.6f + 0.6f * roll;
    ul = pl - 2.0f * p->margin;
    uw = pw - 2.0f * p->margin;

    /* 3. the bay module — ad quadratum: aisle = nave/2, bay = nave/2 */
    roll = gothic_hash01(p->seed, LANE_NAVE_W, 0, 0);
    if (p->style == CHURCH_CHAPEL) {
        p->nave_w  = 4.0f + 3.0f * roll;             /* 4..7        */
        if (p->nave_w > uw) p->nave_w = uw;
        p->aisle_w = 0.0f;
    } else {
        if (p->style == CHURCH_BASILICA) p->nave_w = 7.0f + 3.0f * roll;
        else                             p->nave_w = 5.5f + 2.5f * roll;
        p->aisle_w = 0.5f * p->nave_w;
        if (p->nave_w + 2.0f * p->aisle_w > uw) {    /* squeeze, keep ratio */
            p->nave_w  = uw * 0.5f;
            p->aisle_w = uw * 0.25f;
        }
    }
    roll = gothic_hash01(p->seed, LANE_MODULE, 0, 0);
    p->bay_l = p->nave_w * (0.5f + 0.12f * (roll - 0.5f));  /* nw/2 +-6% */

    p->nbays = (int)(ul / p->bay_l);
    if (p->nbays > PLAN_MAX_BAYS) p->nbays = PLAN_MAX_BAYS;
    if (p->nbays < 1)             p->nbays = 1;
    rem = ul - (float)p->nbays * p->bay_l;

    /* the remainder is absorbed east-first: apse, then tower, then the
       west porch. The chevet is deep — when the leftover is promising
       but short, it EATS BAYS until the polygon fits (deterministic). */
    if (p->style != CHURCH_CHAPEL && rem >= 0.6f * p->bay_l &&
        gothic_hash01(p->seed, LANE_APSE, 0, 0) > 0.25f) {
        float need = APSE_D_PER_NAVE * p->nave_w;
        while (rem < need && p->nbays > 2) { p->nbays--; rem += p->bay_l; }
        if (rem >= need) {
            p->apse_sides = 5;
            p->apse_d     = need;
            rem          -= need;
        }
    }
    if (p->style != CHURCH_CHAPEL && rem >= 0.8f * p->bay_l &&
        gothic_hash01(p->seed, LANE_TOWER, 0, 0) > 0.5f) {
        p->tower   = 1;
        p->tower_d = rem > p->bay_l ? p->bay_l : rem;
        rem       -= p->tower_d;
    }
    p->porch  = rem;
    p->west_x = -0.5f * pl + p->margin + rem;
    p->east_x = p->west_x + p->tower_d + (float)p->nbays * p->bay_l;

    /* 4. the elevation formula — drawn once per building (LANE_ELEV by
       scalar index), arcade band height DERIVED from the arch math */
    {
        float r1 = gothic_hash01(p->seed, LANE_ELEV, 1, 0);
        float r2 = gothic_hash01(p->seed, LANE_ELEV, 2, 0);
        float r3 = gothic_hash01(p->seed, LANE_ELEV, 3, 0);
        float r4 = gothic_hash01(p->seed, LANE_ELEV, 4, 0);
        float r5 = gothic_hash01(p->seed, LANE_ELEV, 5, 0);
        float r6 = gothic_hash01(p->seed, LANE_ELEV, 6, 0);
        float r7 = gothic_hash01(p->seed, LANE_ELEV, 7, 0);
        float arc_span = 0.8f * p->bay_l;       /* arcade clear span */

        float q_nave  = 0.5f * sqrtf(p->nave_w * p->nave_w +
                                     p->bay_l * p->bay_l);
        float q_aisle = 0.5f * sqrtf(p->aisle_w * p->aisle_w +
                                     p->bay_l * p->bay_l);
        p->plinth_h = 0.4f + 0.15f * r1;
        p->sill_h   = 1.1f + 0.5f  * r2;
        if (p->style == CHURCH_CHAPEL) {
            p->wall_t   = 0.6f + 0.2f * r3;
            p->impost_h = 2.2f + 0.8f * r4;     /* window springing line */
            p->arcade_h = p->impost_h;
            p->wall_h   = p->impost_h
                        + gothic_arch_y(0.55f * p->bay_l, p->acute, 0.0f)
                        + 1.0f;
            /* the vault-closure law (item 5): the wall head must clear
               the diagonal's crown — the wall ribs ride ON stone */
            if (p->wall_h < p->impost_h + q_nave + 0.35f)
                p->wall_h = p->impost_h + q_nave + 0.35f;
            p->aisle_h  = p->wall_h;
        } else if (p->style == CHURCH_HALL) {
            p->wall_t   = 0.7f + 0.25f * r3;
            p->impost_h = 3.0f + 1.0f  * r4;
            p->arcade_h = p->impost_h
                        + gothic_arch_y(arc_span, p->acute, 0.0f) + 0.5f;
            p->wall_h   = p->arcade_h + 0.8f;
            if (p->wall_h < p->arcade_h + q_nave + 0.35f)   /* vault closure */
                p->wall_h = p->arcade_h + q_nave + 0.35f;
            p->aisle_h  = p->wall_h;            /* THE hall trait        */
        } else {
            p->wall_t     = 0.8f + 0.3f * r3;
            p->impost_h   = 3.4f + 1.2f * r4;
            p->arcade_h   = p->impost_h
                          + gothic_arch_y(arc_span, p->acute, 0.0f) + 0.5f;
            p->clerest_h0 = p->arcade_h + 0.8f + 0.4f * r5;  /* triforium */
            p->clerest_h1 = p->clerest_h0 + 1.7f + 0.6f * r6;
            p->wall_h     = p->clerest_h1 + 0.5f;
            if (p->wall_h < p->clerest_h0 + q_nave + 0.35f) /* vault closure */
                p->wall_h = p->clerest_h0 + q_nave + 0.35f;
            p->aisle_h    = p->arcade_h + 0.4f;
            if (p->aisle_h < p->impost_h + q_aisle + 0.35f)
                p->aisle_h = p->impost_h + q_aisle + 0.35f;
        }
        p->parapet_h = 0.4f + 0.2f * r7;
    }
}

int plan_pier(const ChurchPlan *p, int i, int j, float *out_x, float *out_z) {
    float z;
    if (!p || i < 0 || i > p->nbays || j < 0 || j > PIER_ROW_N_WALL) return 0;
    if (!p->aisles && (j == PIER_ROW_S_ARCADE || j == PIER_ROW_N_ARCADE))
        return 0;                               /* chapel: no arcades */
    switch (j) {
    case PIER_ROW_S_WALL:   z = -(0.5f * p->nave_w + p->aisle_w); break;
    case PIER_ROW_S_ARCADE: z = -0.5f * p->nave_w;                break;
    case PIER_ROW_N_ARCADE: z =  0.5f * p->nave_w;                break;
    default:                z =  0.5f * p->nave_w + p->aisle_w;   break;
    }
    if (out_x) *out_x = p->west_x + p->tower_d + (float)i * p->bay_l;
    if (out_z) *out_z = z;
    return 1;
}

int plan_apse_pier(const ChurchPlan *p, int k, float *out_x, float *out_z) {
    float r, cxa, ang;
    if (!p || p->apse_sides != 5 || k < 0 || k > 5) return 0;
    r   = APSE_R_PER_NAVE * p->nave_w;
    cxa = p->east_x + 0.3826834f * r;     /* center east of the mouth chord */
    ang = (-112.5f + 45.0f * (float)k) * (SOL_PI / 180.0f);
    if (out_x) *out_x = cxa + r * cosf(ang);
    if (out_z) *out_z = r * sinf(ang);
    return 1;
}

int plan_bay_kind(const ChurchPlan *p, int i, int lane) {
    if (!p || i < 0 || i >= p->nbays || lane < 0 || lane > 2)
        return GOTHIC_BAY_NONE;
    if (lane == 1) return GOTHIC_BAY_NAVE;
    return p->aisles ? GOTHIC_BAY_AISLE : GOTHIC_BAY_NONE;
}

/* flatten-to-fit: if the default acuteness pushes the crown past the
   limit, re-solve via the level-crown formula; if even the SEMICIRCLE
   is too tall, drop the springing toward the sill, and narrow the
   light as the last resort. The plan guarantees the emitters'
   preconditions — an opening it returns always fits its wall. */
static void opening_fit(GothicOpening *o, float limit) {
    float room;
    if (o->spring + gothic_arch_y(o->w, o->acute, 0.0f) <= limit) return;
    o->acute = gothic_arch_acuteness_for(o->w, limit - o->spring);
    if (o->spring + gothic_arch_y(o->w, o->acute, 0.0f) <= limit + 1e-4f)
        return;
    o->acute = 0.0f;
    room = limit - 0.5f * o->w;       /* springing for an exact round fit */
    if (room >= o->sill + 0.15f) { o->spring = room; return; }
    o->spring = o->sill + 0.15f;
    o->w      = 2.0f * (limit - o->spring);
    if (o->w < 0.3f) o->kind = GOTHIC_OPEN_NONE;
}

void plan_opening(const ChurchPlan *p, int wall, int i, GothicOpening *out) {
    float clear, limit, head;
    memset(out, 0, sizeof *out);
    if (!p) return;
    out->acute = p->acute;
    switch (wall) {
    case WALL_AISLE_S:
    case WALL_AISLE_N:
        if (i < 0 || i >= p->nbays) return;
        clear = p->bay_l - p->wall_t - 0.4f;
        if (clear < 0.6f) return;
        out->kind   = GOTHIC_OPEN_WINDOW;
        out->cx     = p->west_x + p->tower_d + ((float)i + 0.5f) * p->bay_l;
        out->w      = 0.55f * clear;
        out->sill   = p->sill_h;
        limit       = p->aisle_h - 0.8f;     /* two courses above the crown */
        head        = limit - out->sill;
        if (head < 0.8f) { out->kind = GOTHIC_OPEN_NONE; return; }
        out->spring = out->sill + 0.45f * head;
        opening_fit(out, limit);
        break;
    case WALL_CLEREST_S:
    case WALL_CLEREST_N:
        if (p->style != CHURCH_BASILICA || i < 0 || i >= p->nbays) return;
        clear = p->bay_l - p->wall_t - 0.4f;
        if (clear < 0.6f) return;
        out->kind   = GOTHIC_OPEN_WINDOW;
        out->cx     = p->west_x + p->tower_d + ((float)i + 0.5f) * p->bay_l;
        out->w      = 0.55f * clear;
        out->sill   = p->clerest_h0 + 0.15f * (p->clerest_h1 - p->clerest_h0);
        limit       = p->clerest_h1 - 0.3f;
        head        = limit - out->sill;
        if (head < 0.6f) { out->kind = GOTHIC_OPEN_NONE; return; }
        out->spring = out->sill + 0.4f * head;
        opening_fit(out, limit);
        break;
    case WALL_WEST:
        if (i == 0) {                        /* the portal */
            out->kind   = GOTHIC_OPEN_DOOR;
            out->cx     = 0.0f;
            out->w      = 0.3f * p->nave_w;
            if (out->w < 1.2f) out->w = 1.2f;
            out->sill   = 0.0f;
            out->spring = 2.0f + 0.1f * p->nave_w;
            opening_fit(out, p->wall_h - 1.2f);   /* room for the window */
        } else if (i == 1) {                 /* the great window above */
            GothicOpening door;
            plan_opening(p, WALL_WEST, 0, &door);
            out->kind   = GOTHIC_OPEN_WINDOW;
            out->cx     = 0.0f;
            out->w      = 0.5f * p->nave_w;
            out->sill   = door.spring
                        + gothic_arch_y(door.w, door.acute, 0.0f) + 0.5f;
            limit       = p->wall_h - 0.8f;
            head        = limit - out->sill;
            if (head < 0.8f) { out->kind = GOTHIC_OPEN_NONE; return; }
            out->spring = out->sill + 0.4f * head;
            opening_fit(out, limit);
        }
        break;
    case WALL_EAST:                          /* the flat east end's window */
        if (p->apse_sides != 0 || i != 0) return;
        out->kind   = GOTHIC_OPEN_WINDOW;
        out->cx     = 0.0f;
        out->w      = 0.5f * p->nave_w;
        out->sill   = p->sill_h + 0.4f;
        limit       = p->wall_h - 0.8f;
        head        = limit - out->sill;
        if (head < 0.8f) { out->kind = GOTHIC_OPEN_NONE; return; }
        out->spring = out->sill + 0.4f * head;
        opening_fit(out, limit);
        break;
    case WALL_APSE:                          /* one lancet per chevet side */
        if (p->apse_sides != 5 || i < 0 || i > 4) return;
        clear = 0.7653669f * 0.5411961f * p->nave_w;     /* the side length */
        out->kind   = GOTHIC_OPEN_WINDOW;
        out->cx     = 0.0f;
        out->w      = 0.42f * clear;
        out->sill   = p->sill_h;
        limit       = p->wall_h - 0.8f;
        head        = limit - out->sill;
        if (head < 0.8f) { out->kind = GOTHIC_OPEN_NONE; return; }
        out->spring = out->sill + 0.45f * head;
        opening_fit(out, limit);
        break;
    default:
        break;
    }
}

/* ================== item 4: the stone shell ==================
   The plan's first full reader: everything load-bearing and opaque, one
   mesh. Pieces are emitted in a LOCAL frame (x along the wall, y up
   from the piece's base, thickness centered) and placed by yaw+
   translate — tangents are computed at upload, after placement, so
   rotation is safe. Abutment is the composition's whole discipline:
   walls sit on plinths bottomless, pieces meet piers endless, stacked
   bands suppress their facing tops/bottoms (§1.2: the plan owns
   abutment; faces where pieces abut are SKIPPED). */

/* rotate about +Y (cos c, sin s) then translate — positions AND normals */
static void mb_transform_from(MeshBuilder *b, sol_u32 v0, float c, float s,
                              float tx, float ty, float tz) {
    sol_u32 i;
    for (i = v0; i < b->vertex_count; i++) {
        sol_f32 *v = b->vertices + (size_t)i * 12;
        float x = v[0], z = v[2], nx = v[3], nz = v[5];
        v[0] = c * x + s * z + tx;
        v[2] = -s * x + c * z + tz;
        v[1] += ty;
        v[3] = c * nx + s * nz;
        v[5] = -s * nx + c * nz;
    }
}

/* a solid wall piece — the blank stretches between openings */
static void solid_wall(MeshBuilder *b, float w, float h, float t, int flags) {
    float hw = 0.5f * w, zf = 0.5f * t;
    if (w <= 1e-5f || h <= 1e-5f) return;
    gz_face(b, -hw, hw, 0.0f, h,  zf,  1);
    gz_face(b, -hw, hw, 0.0f, h, -zf, -1);
    if (!(flags & WF_NO_ENDS)) {
        gx_face(b, -hw, 0.0f, h, -zf, zf, -1);
        gx_face(b,  hw, 0.0f, h, -zf, zf,  1);
    }
    if (!(flags & WF_NO_TOP))    gy_face(b, -hw, hw, h,    -zf, zf,  1);
    if (!(flags & WF_NO_BOTTOM)) gy_face(b, -hw, hw, 0.0f, -zf, zf, -1);
}

/* one placed wall piece. Local heights: the piece's base is ybase in
   building coords, so opening heights arrive absolute and shed ybase
   here. cx_op = opening center in the piece's local x. */
static void place_piece(MeshBuilder *b, float w, float hloc, float t,
                        const GothicOpening *o, float cx_op, float ybase,
                        int flags, float c, float s, float tx, float tz) {
    sol_u32 v0 = b->vertex_count;
    if (o && o->kind == GOTHIC_OPEN_WINDOW)
        arched_orders(b, w, hloc, t, cx_op, o->w, o->sill - ybase,
                      o->spring - ybase, o->acute, 1, 0.0f, 0, flags);
    else if (o && o->kind == GOTHIC_OPEN_DOOR)
        arched_orders(b, w, hloc, t, cx_op, o->w, 0.0f,
                      o->spring - ybase, o->acute, 1, 0.0f, 0, flags);
    else
        solid_wall(b, w, hloc, t, flags);
    mb_transform_from(b, v0, c, s, tx, ybase, tz);
}

/* a flush arch piece (arcade / transverse): the opening IS the span */
static void place_arch(MeshBuilder *b, float span, float hloc, float t,
                       float spring, float acute, float ybase, int flags,
                       float c, float s, float tx, float tz) {
    sol_u32 v0 = b->vertex_count;
    float a = acute;
    float room = hloc - 0.4f - (spring - ybase);
    if (room < 0.5f * span)                      /* cannot even go round */
        return;
    if (spring - ybase + gothic_arch_y(span, a, 0.0f) > hloc - 0.4f)
        a = gothic_arch_acuteness_for(span, room);   /* level-crown again */
    /* WF_NO_SILL: an interior arch spans FLOOR, not a doorway — the
       pavement (or the ruin's grass) owns that plane; the threshold
       strip floated over dipping terrain once the pavement fell */
    arched_orders(b, span, hloc, t, 0.0f, span, 0.0f, spring - ybase, a,
                  1, 0.0f, 0, flags | WF_NO_ENDS | WF_NO_SILL);
    mb_transform_from(b, v0, c, s, tx, ybase, tz);
}

/* the stepped buttress, emitted facing -Z with its back on z=0 (the
   wall's outer face), then placed. Stages step back, each topped by a
   WEATHERING slope (two sloped quads' worth: one quad + side
   triangles) — exposed faces only, the back never emitted. */
static void buttress(MeshBuilder *b, float h_head, float bw, float d0,
                     int stages, float wall_head, float c, float s,
                     float tx, float tz) {
    sol_u32 v0 = b->vertex_count;
    float hx = 0.5f * bw;
    float y_peak = 0.0f;
    float fr2[2]; float fr3[3]; const float *fr;
    float y0 = 0.0f, dk = d0;
    int k;
    fr2[0] = 0.52f; fr2[1] = 0.93f;
    fr3[0] = 0.40f; fr3[1] = 0.68f; fr3[2] = 0.93f;
    fr = stages == 3 ? fr3 : fr2;
    if (stages != 3) stages = 2;
    y0 = -GOTHIC_FOUNDATION;           /* the skirt (item 9) */
    for (k = 0; k < stages; k++) {
        float y1 = fr[k] * h_head;
        float dn = (k == stages - 1) ? 0.0f : dk * 0.65f;
        float rise = (dk - dn) * 1.1f;
        sol_u32 ia, ib, ic, id;
        /* the stage box: front + sides (no back, no bottom, no top —
           the weathering closes it) */
        gz_face(b, -hx, hx, y0, y1, -dk, -1);
        gx_face(b, -hx, y0, y1, -dk, 0.0f, -1);
        gx_face(b,  hx, y0, y1, -dk, 0.0f,  1);
        /* the weathering: slope from the front-top edge back-up */
        {
            float ny = dk - dn, nz = -rise;
            float nl = sqrtf(ny * ny + nz * nz);
            ny /= nl; nz /= nl;
            ia = mb_push_vertex(b,  hx, y1, -dk, 0.0f, ny, nz, hx, 0.0f);
            ib = mb_push_vertex(b, -hx, y1, -dk, 0.0f, ny, nz, -hx, 0.0f);
            ic = mb_push_vertex(b, -hx, y1 + rise, -dn, 0.0f, ny, nz, -hx, 1.0f);
            id = mb_push_vertex(b,  hx, y1 + rise, -dn, 0.0f, ny, nz, hx, 1.0f);
            mb_push_triangle(b, ia, ib, ic);
            mb_push_triangle(b, ia, ic, id);
        }
        {   /* the side closure: the FULL trapezoid from the slope line
               back to the wall — the old wedge triangle stopped at the
               next stage's front plane and left the band behind it
               open (Fran's punch list, error6). Degenerates to the
               triangle on the top stage, where dn = 0. */
            sol_u32 t0, t1, t2, t3;
            t0 = mb_push_vertex(b, -hx, y1, -dk, -1.0f, 0.0f, 0.0f, -dk, y1);
            t1 = mb_push_vertex(b, -hx, y1, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, y1);
            t2 = mb_push_vertex(b, -hx, y1 + rise, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, y1 + rise);
            mb_push_triangle(b, t0, t1, t2);
            if (dn > 1e-5f) {
                t3 = mb_push_vertex(b, -hx, y1 + rise, -dn, -1.0f, 0.0f, 0.0f, -dn, y1 + rise);
                mb_push_triangle(b, t0, t2, t3);
            }
            t0 = mb_push_vertex(b, hx, y1, -dk, 1.0f, 0.0f, 0.0f, -dk, y1);
            t1 = mb_push_vertex(b, hx, y1 + rise, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, y1 + rise);
            t2 = mb_push_vertex(b, hx, y1, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, y1);
            mb_push_triangle(b, t0, t1, t2);
            if (dn > 1e-5f) {
                t3 = mb_push_vertex(b, hx, y1 + rise, -dn, 1.0f, 0.0f, 0.0f, -dn, y1 + rise);
                mb_push_triangle(b, t0, t3, t1);
            }
        }
        y0 = y1 + rise;
        dk = dn;
        if (y0 > y_peak) y_peak = y0;
    }
    /* the back face ABOVE the wall head: a back buried in its wall may
       legally not exist — a back standing proud of it must (a raised
       buttress without one is a window into the hollow interior) */
    if (y_peak > wall_head + 0.02f)
        gz_face(b, -hx, hx, wall_head - 0.05f, y_peak, -0.01f, 1);
    mb_transform_from(b, v0, c, s, tx, 0.0f, tz);
}

/* the pier: octagonal shaft through an attic-base collar on a square
   sub-plinth, capital (frustum + abacus) at the impost. h_cap 0 skips
   the capital (apse buttress-piers run sheer to their flat tops). */
static void pier(MeshBuilder *b, float plinth_h, float impost_h,
                 float px, float pz, float rp, float top, int capital,
                 float yaw_c, float yaw_s, float keep) {
    sol_u32 v0 = b->vertex_count;
    float sp = rp / 0.12f;             /* shaft profile scale            */
    float ap = rp * 0.9238795f;        /* the octagon's apothem          */
    int   pn;
    const ProfilePt *oct = gothic_profile(PROF_SHAFT_OCT, &pn);
    vec3  path[2];
    float shaft_top = capital ? impost_h - 0.45f : top;
    if (keep < 0.999f) {               /* the BROKEN COLUMN: truncated,
                                          capped flat at a course line */
        shaft_top = plinth_h + keep * (shaft_top - plinth_h);
        capital   = capital ? 2 : 0;   /* base yes, frustum/abacus no */
    }

    if (capital) {                     /* square sub-plinth, skirted */
        float hs = rp + 0.10f;
        float yb = -GOTHIC_FOUNDATION;
        gz_face(b, -hs, hs, yb, plinth_h,  hs,  1);
        gz_face(b, -hs, hs, yb, plinth_h, -hs, -1);
        gx_face(b, -hs, yb, plinth_h, -hs, hs, -1);
        gx_face(b,  hs, yb, plinth_h, -hs, hs,  1);
        gy_face(b, -hs, hs, plinth_h, -hs, hs, 1);   /* full top: the
                                          shaft and base stand ON it */
        {   /* the attic base: PR_BASE around the shaft on a square loop,
               seam mid-side (item 1's closed-loop lesson) */
            int bn;
            const ProfilePt *bp = gothic_profile(PROF_BASE, &bn);
            float sb = sp * 0.85f;
            vec3 loop[6];
            /* walk +X first so o = cross(+Y, d) points OUTWARD; the
               seam closes mid-side (item 1's closed-loop lesson) */
            loop[0] = vec3_make(0.0f, plinth_h, -ap);
            loop[1] = vec3_make( ap,  plinth_h, -ap);
            loop[2] = vec3_make( ap,  plinth_h,  ap);
            loop[3] = vec3_make(-ap,  plinth_h,  ap);
            loop[4] = vec3_make(-ap,  plinth_h, -ap);
            loop[5] = vec3_make(0.0f, plinth_h, -ap);
            gothic_sweep(b, bp, bn, loop, 6, vec3_make(0.0f, 1.0f, 0.0f),
                         sb, 0, 0);
        }
    }
    path[0] = vec3_make(0.0f, capital ? plinth_h : -GOTHIC_FOUNDATION,
                        0.0f);
    path[1] = vec3_make(0.0f, shaft_top, 0.0f);
    gothic_sweep(b, oct, pn, path, 2, vec3_make(0.0f, 0.0f, 1.0f),
                 sp, 0, capital == 1 ? 0 : 1);
    if (capital == 1) {
        float aa = rp + 0.12f;                 /* abacus half-size  */
        float yf0 = shaft_top, yf1 = impost_h - 0.15f;
        int k;
        for (k = 0; k < 8; k++) {              /* the frustum: 8 quads */
            float a0 = (22.5f + 45.0f * (float)k) * (SOL_PI / 180.0f);
            float a1 = (22.5f + 45.0f * (float)(k + 1)) * (SOL_PI / 180.0f);
            float x0 = rp * cosf(a0), z0 = rp * sinf(a0);
            float x1 = rp * cosf(a1), z1 = rp * sinf(a1);
            float xu0 = aa * cosf(a0), zu0 = aa * sinf(a0);
            float xu1 = aa * cosf(a1), zu1 = aa * sinf(a1);
            float mx = cosf(0.5f * (a0 + a1)), mz = sinf(0.5f * (a0 + a1));
            float fl = sqrtf((yf1 - yf0) * (yf1 - yf0) + 0.05f);
            sol_u32 q0, q1, q2, q3;
            vec3 n = vec3_normalize(vec3_make(mx * (yf1 - yf0), 0.22f, mz * (yf1 - yf0)));
            (void)fl;
            q0 = mb_push_vertex(b, x0, yf0, z0, n.x, n.y, n.z, 0.0f, 0.0f);
            q1 = mb_push_vertex(b, x1, yf0, z1, n.x, n.y, n.z, 1.0f, 0.0f);
            q2 = mb_push_vertex(b, xu1, yf1, zu1, n.x, n.y, n.z, 1.0f, 1.0f);
            q3 = mb_push_vertex(b, xu0, yf1, zu0, n.x, n.y, n.z, 0.0f, 1.0f);
            mb_push_triangle(b, q0, q2, q1);
            mb_push_triangle(b, q0, q3, q2);
        }
        /* the abacus slab: full bottom and top (anything stacked above
           is thinner — smaller-on-larger keeps both legal) */
        gz_face(b, -aa, aa, yf1, impost_h,  aa,  1);
        gz_face(b, -aa, aa, yf1, impost_h, -aa, -1);
        gx_face(b, -aa, yf1, impost_h, -aa, aa, -1);
        gx_face(b,  aa, yf1, impost_h, -aa, aa,  1);
        gy_face(b, -aa, aa, impost_h, -aa, aa,  1);
        gy_face(b, -aa, aa, yf1, -aa, aa, -1);
    }
    mb_transform_from(b, v0, yaw_c, yaw_s, px, 0.0f, pz);
}

/* the plinth strip: a footing box projecting beyond both wall faces —
   full top (walls stand on it bottomless), no bottom. zrun says the
   strip RUNS along z: long faces are then the x pair, end caps the z
   pair. Ends optional (suppressed where it abuts another strip). */
static void plinth_strip(MeshBuilder *b, float x0, float x1, float z0,
                         float z1, float h, int ends, int zrun) {
    float yb = -GOTHIC_FOUNDATION;     /* the skirt (item 9): buried
                                          uphill, exposed footing down */
    gy_face(b, x0, x1, h, z0, z1, 1);
    if (!zrun) {
        gz_face(b, x0, x1, yb, h, z1,  1);
        gz_face(b, x0, x1, yb, h, z0, -1);
        if (ends) {
            gx_face(b, x0, yb, h, z0, z1, -1);
            gx_face(b, x1, yb, h, z0, z1,  1);
        }
    } else {
        gx_face(b, x0, yb, h, z0, z1, -1);
        gx_face(b, x1, yb, h, z0, z1,  1);
        if (ends) {
            gz_face(b, x0, x1, yb, h, z1,  1);
            gz_face(b, x0, x1, yb, h, z0, -1);
        }
    }
}

/* a parapet band: a thin solid strip ON a wall head (no bottom);
   zrun as above */
static void parapet_strip(MeshBuilder *b, float x0, float x1, float z0,
                          float z1, float y0, float y1, int ends, int zrun) {
    gy_face(b, x0, x1, y1, z0, z1, 1);
    if (!zrun) {
        gz_face(b, x0, x1, y0, y1, z1,  1);
        gz_face(b, x0, x1, y0, y1, z0, -1);
        if (ends) {
            gx_face(b, x0, y0, y1, z0, z1, -1);
            gx_face(b, x1, y0, y1, z0, z1,  1);
        }
    } else {
        gx_face(b, x0, y0, y1, z0, z1, -1);
        gx_face(b, x1, y0, y1, z0, z1,  1);
        if (ends) {
            gz_face(b, x0, x1, y0, y1, z1,  1);
            gz_face(b, x0, x1, y0, y1, z0, -1);
        }
    }
}

/* pier sizing shared by walls (spans stop at pier faces) and piers */
static float stone_pier_r(const ChurchPlan *p) {
    return 0.18f + 0.035f * p->nave_w;
}

/* one aisle-wall run (side -1 = south, +1 = north): per-bay pieces with
   their windows, a blank stretch over the tower flank, west/east corner
   extensions — all bottomless on the plinth, endless between bays */
static void stone_wall_run(MeshBuilder *b, const ChurchPlan *p, int side) {
    float wt   = p->wall_t;
    float hwid = 0.5f * p->nave_w + p->aisle_w;
    float zc   = (float)side * (hwid + 0.5f * wt);
    float yawc = side > 0 ? 1.0f : -1.0f;        /* outward = +-Z */
    float hloc = p->aisle_h - p->plinth_h;
    int   i, wallq = side > 0 ? WALL_AISLE_N : WALL_AISLE_S;
    int   jl = side > 0 ? 2 : 0;

    if (p->tower) {
        float tk;
        if (church_survives(p, ELEM_WALL, 0, jl, &tk))
            place_piece(b, p->tower_d + wt,
                        tk * hloc < 0.5f ? 0.5f : tk * hloc, wt,
                        (const GothicOpening *)0,
                        0.0f, p->plinth_h, WF_NO_BOTTOM | WF_NO_ENDS, yawc,
                        0.0f, p->west_x + 0.5f * (p->tower_d - wt), zc);
    }
    for (i = 0; i < p->nbays; i++) {
        GothicOpening o;
        float x0 = p->west_x + p->tower_d + (float)i * p->bay_l;
        float xc = x0 + 0.5f * p->bay_l, w = p->bay_l;
        float tk, hk;
        if (!church_survives(p, ELEM_WALL, i, jl, &tk)) continue;
        hk = tk * hloc;
        if (hk < 0.5f) continue;
        if (i == 0 && !p->tower) { w += wt; xc -= 0.5f * wt; }   /* corner */
        if (i == p->nbays - 1)   { w += wt; xc += 0.5f * wt; }
        plan_opening(p, wallq, i, &o);
        /* a window whose crown the break took is no window */
        if (o.kind == GOTHIC_OPEN_WINDOW &&
            o.spring + gothic_arch_y(o.w, o.acute, 0.0f)
                > p->plinth_h + hk - 0.25f)
            o.kind = GOTHIC_OPEN_NONE;
        place_piece(b, w, hk, wt,
                    o.kind != GOTHIC_OPEN_NONE ? &o : (const GothicOpening *)0,
                    yawc > 0.0f ? o.cx - xc : xc - o.cx,
                    p->plinth_h, WF_NO_BOTTOM | WF_NO_ENDS |
                    (tk < 0.999f ? 0 : 0), yawc, 0.0f, xc, zc);
    }
    /* the corner end faces, each tied to its extreme bay's kept height */
    {
        float za = (float)side * hwid, zb = (float)side * (hwid + wt);
        float z0 = za < zb ? za : zb, z1 = za < zb ? zb : za;
        float tk;
        if (church_survives(p, ELEM_WALL, 0, jl, &tk) && tk * hloc >= 0.5f)
            gx_face(b, p->west_x - wt, p->plinth_h,
                    p->plinth_h + tk * hloc, z0, z1, -1);
        if (church_survives(p, ELEM_WALL, p->nbays - 1, jl, &tk) &&
            tk * hloc >= 0.5f)
            gx_face(b, p->east_x + wt, p->plinth_h,
                    p->plinth_h + tk * hloc, z0, z1,  1);
    }
}

/* the west facade: stepped — a nave-width center (portal below the
   springline course, great window above, both from the plan) rising to
   wall_h, blank aisle-width flanks rising to aisle_h */
static void stone_facade(MeshBuilder *b, const ChurchPlan *p) {
    float wt = p->wall_t;
    float xc = p->west_x - 0.5f * wt;
    GothicOpening door, win;
    float h_split;
    float tkf, kept_top;
    int   whole;

    if (!church_survives(p, ELEM_WALL, 0, 1, &tkf)) return;
    kept_top = p->plinth_h + tkf * (p->wall_h - p->plinth_h);
    whole    = tkf >= 0.96f;

    {   /* the split must clear the WIDEST order's crown, not the
           door's — three stepped orders crown half a meter above it,
           and the crown-above-top guard silently refused the whole
           portal wall on basilicas (Fran's punch list, error9) */
        int ords0 = p->style == CHURCH_BASILICA ? 3 :
                    p->style == CHURCH_HALL ? 2 : 1;
        float wide;
        plan_opening(p, WALL_WEST, 0, &door);
        plan_opening(p, WALL_WEST, 1, &win);
        wide = door.w + 2.0f * 0.15f * (float)(ords0 - 1);
        h_split = door.spring + gothic_arch_y(wide, door.acute, 0.0f) + 0.3f;
        if (win.kind == GOTHIC_OPEN_WINDOW && win.sill < h_split + 0.15f) {
            win.sill = h_split + 0.15f;
            if (win.spring < win.sill + 0.3f) win.spring = win.sill + 0.3f;
            opening_fit(&win, p->wall_h - 0.8f);
        }
    }

    {   /* the portal: recessed orders by style, archivolt-dressed.
           A chapel's facade ends abut the long walls' inner faces (no
           flanks stand beside it) — ends suppressed; aisled facades
           keep theirs, exposed above the flanks. */
        sol_u32 v0 = b->vertex_count;
        int ords = p->style == CHURCH_BASILICA ? 3 :
                   p->style == CHURCH_HALL ? 2 : 1;
        int ef = p->aisles ? 0 : WF_NO_ENDS;
        if (kept_top > h_split + 0.3f) {     /* the portal zone stands */
            arched_orders(b, p->nave_w, h_split - p->plinth_h, wt, 0.0f,
                          door.w, 0.0f, door.spring - p->plinth_h, door.acute,
                          ords, 0.15f, 1,
                          WF_NO_TOP | WF_NO_BOTTOM | WF_NO_SILL | ef);
            mb_transform_from(b, v0, 0.0f, -1.0f, xc, p->plinth_h, 0.0f);
            if (win.kind == GOTHIC_OPEN_WINDOW &&
                (win.spring + gothic_arch_y(win.w, win.acute, 0.0f)
                     > kept_top - 0.25f))
                win.kind = GOTHIC_OPEN_NONE;
            place_piece(b, p->nave_w, kept_top - h_split, wt,
                        win.kind == GOTHIC_OPEN_WINDOW ? &win : (const GothicOpening *)0,
                        0.0f, h_split,
                        WF_NO_BOTTOM | (whole ? 0 : WF_NO_TOP * 0) | ef,
                        0.0f, -1.0f, xc, 0.0f);
        } else {                             /* broken low: one stump */
            arched_orders(b, p->nave_w, kept_top - p->plinth_h, wt, 0.0f,
                          door.w, 0.0f,
                          door.spring - p->plinth_h, door.acute,
                          1, 0.0f, 0,
                          WF_NO_BOTTOM | WF_NO_SILL | ef);
            mb_transform_from(b, v0, 0.0f, -1.0f, xc, p->plinth_h, 0.0f);
        }
    }
    if (p->aisles) {
        float fz = 0.5f * (0.5f * p->nave_w + (0.5f * p->nave_w + p->aisle_w));
        float ts, tn;
        if (church_survives(p, ELEM_WALL, 0, 0, &ts) &&
            ts * (p->aisle_h - p->plinth_h) >= 0.5f)
            place_piece(b, p->aisle_w, ts * (p->aisle_h - p->plinth_h), wt,
                        (const GothicOpening *)0, 0.0f, p->plinth_h,
                        WF_NO_BOTTOM | WF_NO_ENDS, 0.0f, -1.0f, xc, -fz);
        if (church_survives(p, ELEM_WALL, 0, 2, &tn) &&
            tn * (p->aisle_h - p->plinth_h) >= 0.5f)
            place_piece(b, p->aisle_w, tn * (p->aisle_h - p->plinth_h), wt,
                        (const GothicOpening *)0, 0.0f, p->plinth_h,
                        WF_NO_BOTTOM | WF_NO_ENDS, 0.0f, -1.0f, xc,  fz);
    }
    if (kept_top > h_split + 0.3f)
    {   /* item 7: the LINTEL and TYMPANUM fill the innermost order's
           head — the slab that has carried sculpture nine centuries;
           ours takes the normal map and waits. Recessed from the back
           face so nothing is coplanar. */
        float zi0 = p->west_x - wt + 0.06f;        /* slab band (x)   */
        float zi1 = zi0 + 0.16f;
        float ly  = door.spring - 0.05f + p->plinth_h * 0.0f;
        float hw2 = 0.5f * door.w;
        /* the lintel: ends ABUT the jamb reveals (no end faces — the
           audit's lesson, again) */
        gy_face(b, zi0, zi1, ly - 0.22f, -hw2, hw2, -1);   /* underside */
        gx_face(b, zi0, ly - 0.22f, ly, -hw2, hw2, -1);    /* west face */
        gx_face(b, zi1, ly - 0.22f, ly, -hw2, hw2,  1);    /* east face */
        {   /* the tympanum: the arch-head polygon as a thin slab */
            vec3 hp[GOTHIC_ARCH_MAX_PTS];
            int  hn = gothic_arch_path(hp, GOTHIC_ARCH_MAX_PTS, door.w,
                                       door.acute, GOTHIC_MAX_SEG);
            int  j;
            for (j = 0; j + 1 < hn; j++) {
                vec3 a0 = vec3_make(zi0, door.spring + hp[j].y,     -hp[j].x);
                vec3 a1 = vec3_make(zi0, door.spring + hp[j + 1].y, -hp[j + 1].x);
                vec3 b0 = vec3_make(zi0, ly, -hp[j].x);
                vec3 b1 = vec3_make(zi0, ly, -hp[j + 1].x);
                vec3 c0 = vec3_make(zi1, door.spring + hp[j].y,     -hp[j].x);
                vec3 c1 = vec3_make(zi1, door.spring + hp[j + 1].y, -hp[j + 1].x);
                vec3 d0 = vec3_make(zi1, ly, -hp[j].x);
                vec3 d1 = vec3_make(zi1, ly, -hp[j + 1].x);
                quad3(b, a1, a0, b0, b1);          /* west: the carved face */
                quad3(b, c0, c1, d1, d0);          /* east face — the head
                      strip is NOT emitted: the portal's own intrados
                      already owns that surface (one author per plane) */
            }
        }
    }
    {   /* item 7: porch steps from the plan's west residue — risers a
           STEP_UP treaty away from being climbable (item 9's colliders) */
        float riser = 0.16f, tread = 0.34f;
        int   nstep = (int)(p->plinth_h / riser);
        float sw    = 0.5f * door.w + 0.7f;
        int   k;
        if (nstep > 0 && p->porch > tread * (float)nstep * 0.5f) {
            for (k = 1; k <= nstep; k++) {
                float top = p->plinth_h - riser * (float)k;
                float xs0 = p->west_x - wt - tread * (float)k;
                float xs1 = p->west_x - wt - tread * (float)(k - 1);
                if (top <= 0.02f) break;
                gy_face(b, xs0, xs1, top, -sw, sw, 1);
                gx_face(b, xs0, -1.2f, top, -sw, sw, -1);
                gz_face(b, xs0, xs1, -1.2f, top, -sw, -1);
                gz_face(b, xs0, xs1, -1.2f, top,  sw,  1);
            }
        }
    }
}

/* the east end: a flat wall with its great window, or the aisle
   closures + the five chevet sides with their lancets */
static void stone_east(MeshBuilder *b, const ChurchPlan *p) {
    float wt = p->wall_t;
    float xc = p->east_x + 0.5f * wt;

    if (p->apse_sides == 0) {
        GothicOpening win;
        float wfull = p->aisles ? p->nave_w + 2.0f * p->aisle_w : p->nave_w;
        float tke, hke;
        if (!church_survives(p, ELEM_WALL, p->nbays, 1, &tke)) return;
        hke = tke * (p->wall_h - p->plinth_h);
        if (hke < 0.5f) return;
        plan_opening(p, WALL_EAST, 0, &win);
        if (win.kind == GOTHIC_OPEN_WINDOW &&
            win.spring + gothic_arch_y(win.w, win.acute, 0.0f)
                > p->plinth_h + hke - 0.25f)
            win.kind = GOTHIC_OPEN_NONE;
        place_piece(b, p->nave_w, hke, wt,
                    win.kind == GOTHIC_OPEN_WINDOW ? &win : (const GothicOpening *)0,
                    0.0f, p->plinth_h,
                    WF_NO_BOTTOM | (p->aisles ? 0 : WF_NO_ENDS),
                    0.0f, 1.0f, xc, 0.0f);
        if (p->aisles) {
            float fz = 0.5f * (0.5f * p->nave_w + 0.5f * wfull);
            place_piece(b, p->aisle_w, p->aisle_h - p->plinth_h, wt,
                        (const GothicOpening *)0, 0.0f, p->plinth_h,
                        WF_NO_BOTTOM | WF_NO_ENDS, 0.0f, 1.0f, xc, -fz);
            place_piece(b, p->aisle_w, p->aisle_h - p->plinth_h, wt,
                        (const GothicOpening *)0, 0.0f, p->plinth_h,
                        WF_NO_BOTTOM | WF_NO_ENDS, 0.0f, 1.0f, xc,  fz);
        }
        return;
    }
    if (p->aisles) {                    /* the aisle east closures stop
           where the apse mouth side's slab begins (its corner is the
           slab's own; §1.2 — one author per region) */
        float cw = p->aisle_w - wt;
        float fz = 0.5f * p->nave_w + wt + 0.5f * cw;
        float ts2, tn2;
        if (cw > 0.3f) {
            if (church_survives(p, ELEM_WALL, p->nbays, 0, &ts2) &&
                ts2 * (p->aisle_h - p->plinth_h) >= 0.5f)
                place_piece(b, cw, ts2 * (p->aisle_h - p->plinth_h), wt,
                            (const GothicOpening *)0, 0.0f, p->plinth_h,
                            WF_NO_BOTTOM | WF_NO_ENDS, 0.0f, 1.0f, xc, -fz);
            if (church_survives(p, ELEM_WALL, p->nbays, 2, &tn2) &&
                tn2 * (p->aisle_h - p->plinth_h) >= 0.5f)
                place_piece(b, cw, tn2 * (p->aisle_h - p->plinth_h), wt,
                            (const GothicOpening *)0, 0.0f, p->plinth_h,
                            WF_NO_BOTTOM | WF_NO_ENDS, 0.0f, 1.0f, xc,  fz);
        }
    }
    {   /* the chevet: five sides, a lancet each — walls span PIER FACE
           to pier face (§1.2: trimmed at both ends, the radial piers
           cover the joints; untrimmed slabs overlap in a kite at every
           vertex, the audit's first real catch) */
        float rap   = 0.4f > 0.7f * wt ? 0.4f : 0.7f * wt;
        float inset = 0.83f * rap;
        int k;
        for (k = 0; k < 5; k++) {
            float ax, az, bx, bz, mx, mz, dx, dz, len, nx, nz, nl;
            GothicOpening win;
            plan_apse_pier(p, k,     &ax, &az);
            plan_apse_pier(p, k + 1, &bx, &bz);
            mx = 0.5f * (ax + bx); mz = 0.5f * (az + bz);
            dx = bx - ax; dz = bz - az;
            nl = sqrtf(dx * dx + dz * dz);
            len = nl - 2.0f * inset;
            if (len < 0.5f || nl < 1e-6f) continue;
            nx = dz / nl; nz = -dx / nl;     /* the chord's outward
                perpendicular — the vertex walk is south-to-north around
                the east, so (dz,-dx) faces away from the apse */
            plan_opening(p, WALL_APSE, k, &win);
            if (win.kind == GOTHIC_OPEN_WINDOW && win.w > len - 0.6f) {
                win.w = len - 0.6f;
                if (win.w < 0.3f) win.kind = GOTHIC_OPEN_NONE;
            }
            {   /* survival per side (i = nbays, j = the side index) */
                float tka, hka;
                if (!church_survives(p, ELEM_WALL, p->nbays, k, &tka))
                    continue;
                hka = tka * p->wall_h;
                if (hka < 0.5f) continue;
                if (win.kind == GOTHIC_OPEN_WINDOW &&
                    win.spring + gothic_arch_y(win.w, win.acute, 0.0f)
                        > hka - 0.25f)
                    win.kind = GOTHIC_OPEN_NONE;
                /* ends EMITTED (punch list, error8): the capped end
                   buries in the pier and surfaces as a reveal */
                place_piece(b, len, hka, wt,
                            win.kind == GOTHIC_OPEN_WINDOW ? &win : (const GothicOpening *)0,
                            0.0f, 0.0f, 0,
                            nz, nx, mx + nx * 0.5f * wt, mz + nz * 0.5f * wt);
            }
        }
    }
}

/* arcades, the clerestory band, spandrels over the piers, and the
   tower's transverse arch — the nave boundary planes at +-nave_w/2 */
static void stone_arcades(MeshBuilder *b, const ChurchPlan *p) {
    float wt  = p->wall_t;
    float rp  = stone_pier_r(p);
    float pfh = rp * 0.9238795f;
    float top = p->style == CHURCH_BASILICA ? p->arcade_h : p->wall_h;
    int   side, i;

    if (!p->aisles) return;
    for (side = -1; side <= 1; side += 2) {
        float zc = (float)side * 0.5f * p->nave_w;
        float yawc = side > 0 ? 1.0f : -1.0f;
        int ja = side > 0 ? 2 : 0;
        for (i = 0; i < p->nbays; i++) {
            float x0 = p->west_x + p->tower_d + (float)i * p->bay_l;
            float rap = 0.4f > 0.7f * wt ? 0.4f : 0.7f * wt;
            float xs = x0 + ((i == 0 && !p->tower) ? 0.0f : pfh);
            float xe = x0 + p->bay_l -
                       (i == p->nbays - 1
                            ? (p->apse_sides ? rap * 0.9238795f : 0.0f)
                            : pfh);
            int   fl = p->style == CHURCH_BASILICA ? WF_NO_TOP : 0;
            if (!church_survives(p, ELEM_ARCADE, i, ja, (float *)0))
                continue;
            place_arch(b, xe - xs, top, wt, p->impost_h,
                       p->acute, 0.0f, fl, yawc, 0.0f,
                       0.5f * (xs + xe), zc);
        }
        for (i = 1; i < p->nbays; i++) {     /* spandrels over the piers */
            float x = p->west_x + p->tower_d + (float)i * p->bay_l;
            int   fl = WF_NO_BOTTOM | WF_NO_ENDS |
                       (p->style == CHURCH_BASILICA ? WF_NO_TOP : 0);
            if (!church_survives(p, ELEM_ARCADE, i, ja, (float *)0) &&
                !church_survives(p, ELEM_ARCADE, i - 1, ja, (float *)0))
                continue;                    /* both neighbors fell    */
            place_piece(b, 2.0f * pfh, top - p->impost_h, wt,
                        (const GothicOpening *)0, 0.0f, p->impost_h, fl,
                        yawc, 0.0f, x, zc);
        }
        if (p->style == CHURCH_BASILICA) {   /* the clerestory band */
            for (i = 0; i < p->nbays; i++) {
                GothicOpening o;
                float x0 = p->west_x + p->tower_d + (float)i * p->bay_l;
                float xc = x0 + 0.5f * p->bay_l;
                if (!church_survives(p, ELEM_CLEREST, i, ja, (float *)0))
                    continue;
                plan_opening(p, side > 0 ? WALL_CLEREST_N : WALL_CLEREST_S,
                             i, &o);
                place_piece(b, p->bay_l, p->wall_h - p->arcade_h, wt,
                            o.kind != GOTHIC_OPEN_NONE ? &o : (const GothicOpening *)0,
                            yawc > 0.0f ? o.cx - xc : xc - o.cx,
                            p->arcade_h, WF_NO_BOTTOM | WF_NO_ENDS,
                            yawc, 0.0f, xc, zc);
            }
        }
        if (p->tower)                        /* the tower-bay side arch */
            place_arch(b, p->tower_d - 2.0f * pfh,
                       p->style == CHURCH_BASILICA ? p->wall_h : p->wall_h,
                       wt, p->impost_h, p->acute, 0.0f,
                       p->tower ? WF_NO_TOP : 0, yawc, 0.0f,
                       p->west_x + 0.5f * p->tower_d, zc);
    }
    if (p->tower)                            /* the transverse arch east
                                                of the tower bay */
        place_arch(b, p->nave_w - 2.0f * pfh, p->wall_h, wt, p->impost_h,
                   p->acute, 0.0f, WF_NO_TOP, 0.0f, 1.0f,
                   p->west_x + p->tower_d, 0.0f);
}

/* the string course: PROF_STRING swept just below the sills, jogging
   OUT and around every buttress (the miter machinery earning its keep)
   — an open run from the portal's south jamb around the building to
   its north jamb, ending early at the apse mouth when there is one */
#define COURSE_MAX_PTS 192
static int course_jog(vec3 *pt, int n, float along0, float along1,
                      float depth, float y, int axis, float fixed, int neg) {
    /* four points stepping out around a buttress; axis 0 = run along x
       at z = fixed (depth grows -z when neg), axis 1 = run along z */
    float d = neg ? -depth : depth;
    if (n + 4 > COURSE_MAX_PTS) return n;
    if (axis == 0) {
        pt[n++] = vec3_make(along0, y, fixed);
        pt[n++] = vec3_make(along0, y, fixed + d);
        pt[n++] = vec3_make(along1, y, fixed + d);
        pt[n++] = vec3_make(along1, y, fixed);
    } else {
        pt[n++] = vec3_make(fixed, y, along0);
        pt[n++] = vec3_make(fixed + d, y, along0);
        pt[n++] = vec3_make(fixed + d, y, along1);
        pt[n++] = vec3_make(fixed, y, along1);
    }
    return n;
}

static void stone_course(MeshBuilder *b, const ChurchPlan *p) {
    vec3  pt[COURSE_MAX_PTS];
    float wt   = p->wall_t;
    float hwid = 0.5f * p->nave_w + p->aisle_w;
    float y    = p->sill_h - 0.25f;
    float xf   = p->west_x - wt;            /* facade outer face        */
    float xe   = p->east_x + wt;            /* east outer face          */
    float bd   = (p->style == CHURCH_BASILICA ? 1.05f :
                  p->style == CHURCH_HALL ? 0.75f : 0.55f);
    float bw   = wt + 0.3f;
    GothicOpening door;
    int   half, i, n_str;
    const ProfilePt *prof = gothic_profile(PROF_STRING, &n_str);

    plan_opening(p, WALL_WEST, 0, &door);

    for (half = -1; half <= 1; half += 2) {
        /* south half walks -z first; the north half mirrors. v1 ruin:
           the whole half stands only while every wall it rides is
           whole (a broken run drops its course — runs later) */
        float hs = (float)half;
        int   n  = 0, jc = half > 0 ? 2 : 0, bb, whole = 1;
        float tkc;
        for (bb = 0; bb < p->nbays; bb++)
            if (!church_survives(p, ELEM_WALL, bb, jc, &tkc) || tkc < 0.96f)
                whole = 0;
        if (!church_survives(p, ELEM_WALL, 0, 1, &tkc) || tkc < 0.96f)
            whole = 0;
        if (!whole) continue;
        pt[n++] = vec3_make(xf, y, hs * (0.5f * door.w + 0.35f));
        if (p->aisles)                        /* facade buttress jog */
            n = course_jog(pt, n, hs * (0.5f * p->nave_w - 0.5f * bw),
                           hs * (0.5f * p->nave_w + 0.5f * bw),
                           bd, y, 1, xf, 1);
        pt[n++] = vec3_make(xf, y, hs * (hwid + wt));     /* the corner */
        for (i = 1; i < p->nbays; i++) {      /* along the aisle wall */
            float x = p->west_x + p->tower_d + (float)i * p->bay_l;
            n = course_jog(pt, n, x - 0.5f * bw, x + 0.5f * bw,
                           bd, y, 0, hs * (hwid + wt), half < 0);
        }
        if (p->apse_sides == 0) {
            pt[n++] = vec3_make(xe, y, hs * (hwid + wt)); /* east corner */
            if (p->aisles)        /* jog ONLY around a buttress that was
                                     built — chapels have none here, and
                                     a jog around nothing frames a hole */
                n = course_jog(pt, n, hs * (0.5f * p->nave_w + 0.5f * bw),
                               hs * (0.5f * p->nave_w - 0.5f * bw),
                               bd, y, 1, xe, 0);
            if (n + 1 <= COURSE_MAX_PTS)
                pt[n++] = vec3_make(xe, y, hs * 0.18f);   /* meet mid-east */
        } else {
            pt[n++] = vec3_make(xe, y, hs * (hwid + wt));
            if (n + 1 <= COURSE_MAX_PTS)      /* end at the apse mouth */
                pt[n++] = vec3_make(xe, y, hs * (0.5f * p->nave_w + 0.3f));
        }
        if (half < 0) {                       /* south runs portal->east */
            gothic_sweep(b, prof, n_str, pt, n,
                         vec3_make(0.0f, 1.0f, 0.0f), 0.8f, 1, 1);
        } else {                              /* north mirrors: REVERSE so
                                                 o still points outward */
            vec3 rev[COURSE_MAX_PTS];
            for (i = 0; i < n; i++) rev[i] = pt[n - 1 - i];
            gothic_sweep(b, prof, n_str, rev, n,
                         vec3_make(0.0f, 1.0f, 0.0f), 0.8f, 1, 1);
        }
    }
}

static void stone_vaults(MeshBuilder *b, const ChurchPlan *p);  /* item 5,
                                       defined below in reading order */
static void church_windows(MeshBuilder *stone, MeshBuilder *glass,
                           const ChurchPlan *p);            /* item 6 */
static void stone_pinnacles(MeshBuilder *b, const ChurchPlan *p);
static void stone_rubble(MeshBuilder *b, const ChurchPlan *p);

void church_stone(MeshBuilder *b, const float *params, int count) {
    ChurchPlan plan;
    const ChurchPlan *p = &plan;
    float wt, hwid, rp, pfh, proj;
    int   i;

    if (!b) return;
    church_plan(&plan, params, count);
    wt   = p->wall_t;
    hwid = 0.5f * p->nave_w + p->aisle_w;
    rp   = stone_pier_r(p);
    pfh  = rp * 0.9238795f;
    proj = 0.12f;

    /* plinths first: S/N full length, west/east trimmed between them
       (their end faces abut and are skipped — the S/N inner faces win) */
    plinth_strip(b, p->west_x - wt - proj, p->east_x + wt + proj,
                 -(hwid + wt + proj), -(hwid - proj), p->plinth_h, 1, 0);
    plinth_strip(b, p->west_x - wt - proj, p->east_x + wt + proj,
                 hwid - proj, hwid + wt + proj, p->plinth_h, 1, 0);
    plinth_strip(b, p->west_x - wt - proj, p->west_x + proj,
                 -(hwid - proj), hwid - proj, p->plinth_h, 0, 1);
    if (p->apse_sides == 0)
        plinth_strip(b, p->east_x - proj, p->east_x + wt + proj,
                     -(hwid - proj), hwid - proj, p->plinth_h, 0, 1);

    stone_wall_run(b, p, -1);
    stone_wall_run(b, p,  1);
    stone_facade(b, p);
    stone_east(b, p);
    stone_arcades(b, p);

    /* freestanding arcade piers at INTERIOR stations; the end arches
       die into the walls (station 0 stands only behind a tower, the
       east station only as the apse's own) */
    if (p->aisles)
        for (i = p->tower ? 0 : 1; i < p->nbays; i++) {
            float x, z, tk;
            if (plan_pier(p, i, PIER_ROW_S_ARCADE, &x, &z) &&
                church_survives(p, ELEM_PIER, i, 0, &tk))
                pier(b, p->plinth_h, p->impost_h, x, z, rp, 0.0f, 1, 1.0f, 0.0f, tk);
            if (plan_pier(p, i, PIER_ROW_N_ARCADE, &x, &z) &&
                church_survives(p, ELEM_PIER, i, 2, &tk))
                pier(b, p->plinth_h, p->impost_h, x, z, rp, 0.0f, 1, 1.0f, 0.0f, tk);
        }
    if (p->apse_sides == 5) {
        float rap = 0.4f > 0.7f * wt ? 0.4f : 0.7f * wt;
        for (i = 0; i < 6; i++) {
            float x, z, ang, tk;
            if (!church_survives(p, ELEM_PIER, p->nbays, i, &tk))
                continue;
            plan_apse_pier(p, i, &x, &z);
            ang = (-112.5f + 45.0f * (float)i) * (SOL_PI / 180.0f);
            pier(b, p->plinth_h, p->impost_h, x, z, rap,
                 tk * (p->wall_h + 0.25f), 0,
                 cosf(ang), sinf(ang), 1.0f);
        }
    }

    {   /* buttresses: interior stations on both aisle walls, a pair
           flanking the facade, a pair on a flat east end */
        float bd = (p->style == CHURCH_BASILICA ? 1.05f :
                    p->style == CHURCH_HALL ? 0.75f : 0.55f);
        float bw = wt + 0.3f;
        int   st = p->style == CHURCH_BASILICA ? 3 : 2;
        /* basilica aisle buttresses RAISED to receive the flyers */
        float bh = p->style == CHURCH_BASILICA ? p->aisle_h + 0.6f
                                               : p->aisle_h;
        for (i = 1; i < p->nbays; i++) {
            float x = p->west_x + p->tower_d + (float)i * p->bay_l;
            if (church_survives(p, ELEM_BUTTRESS, i, 0, (float *)0))
                buttress(b, bh, bw, bd, st, p->aisle_h, 1.0f, 0.0f,
                         x, -(hwid + wt));
            if (church_survives(p, ELEM_BUTTRESS, i, 2, (float *)0))
                buttress(b, bh, bw, bd, st, p->aisle_h, -1.0f, 0.0f,
                         x, hwid + wt);
        }
        if (p->aisles) {
            if (church_survives(p, ELEM_BUTTRESS, 0, 0, (float *)0))
                buttress(b, p->wall_h, bw, bd, st, p->wall_h, 0.0f, 1.0f,
                         p->west_x - wt, -0.5f * p->nave_w);
            if (church_survives(p, ELEM_BUTTRESS, 0, 2, (float *)0))
                buttress(b, p->wall_h, bw, bd, st, p->wall_h, 0.0f, 1.0f,
                         p->west_x - wt, 0.5f * p->nave_w);
            if (p->apse_sides == 0) {
                if (church_survives(p, ELEM_BUTTRESS, p->nbays, 0, (float *)0))
                    buttress(b, p->wall_h, bw, bd, st, p->wall_h, 0.0f, -1.0f,
                             p->east_x + wt, -0.5f * p->nave_w);
                if (church_survives(p, ELEM_BUTTRESS, p->nbays, 2, (float *)0))
                    buttress(b, p->wall_h, bw, bd, st, p->wall_h, 0.0f, -1.0f,
                             p->east_x + wt, 0.5f * p->nave_w);
            }
        }
    }

    stone_pinnacles(b, p);          /* item 7: every buttress crowned */
    stone_vaults(b, p);             /* part 2: the crown of the system */
    church_windows(b, (MeshBuilder *)0, p);   /* item 6: the bars are
                                                 masonry; the glass is
                                                 church_glass's */
    stone_course(b, p);
    stone_rubble(b, p);             /* item 9: debris where vaults fell */

    {   /* parapets: aisle walls full length, facade and flat east
           trimmed between them, the clerestory band's own */
        float pw = 0.5f * wt;
        float zs = hwid + 0.5f * wt;
        float tkp;
        int   sd2, rr0, ii2;
        for (sd2 = -1; sd2 <= 1; sd2 += 2) {   /* aisle strips, per RUN */
            float zc0 = (float)sd2 * zs;
            int   jc2 = sd2 > 0 ? 2 : 0;
            rr0 = -1;
            for (ii2 = 0; ii2 <= p->nbays; ii2++) {
                int up = ii2 < p->nbays &&
                         church_survives(p, ELEM_WALL, ii2, jc2, &tkp) &&
                         tkp >= 0.96f;
                if (up && rr0 < 0) rr0 = ii2;
                if (!up && rr0 >= 0) {
                    float rx0 = p->west_x + p->tower_d
                              + (float)rr0 * p->bay_l - (rr0 == 0 ? wt : 0.0f);
                    float rx1 = p->west_x + p->tower_d
                              + (float)ii2 * p->bay_l
                              + (ii2 == p->nbays ? wt : 0.0f);
                    parapet_strip(b, rx0, rx1, zc0 - 0.5f * pw,
                                  zc0 + 0.5f * pw,
                                  p->aisle_h, p->aisle_h + p->parapet_h,
                                  1, 0);
                    rr0 = -1;
                }
            }
        }
        if (church_survives(p, ELEM_WALL, 0, 1, &tkp) && tkp >= 0.96f)
            parapet_strip(b, p->west_x - wt, p->west_x - wt + pw,
                          -(zs - 0.5f * pw), zs - 0.5f * pw,
                          p->wall_h, p->wall_h + p->parapet_h, 0, 1);
        if (p->apse_sides == 0 &&
            church_survives(p, ELEM_WALL, p->nbays, 1, &tkp) && tkp >= 0.96f)
            parapet_strip(b, p->east_x + wt - pw, p->east_x + wt,
                          -(zs - 0.5f * pw), zs - 0.5f * pw,
                          p->wall_h, p->wall_h + p->parapet_h, 0, 1);
        if (p->style == CHURCH_BASILICA) {
            /* run forward to MEET the facade parapet (12mm shy: the
               abutting end must not share its plane) — ending at the
               wall line left a beam-end floating over the aisle roof */
            float x0 = p->tower ? p->west_x + p->tower_d
                                : p->west_x - wt + pw + 0.012f;
            float x1 = p->apse_sides ? p->east_x
                                     : p->east_x + wt - pw - 0.012f;
            int rrc, iic;
            for (sd2 = -1; sd2 <= 1; sd2 += 2) {
                float zc0 = (float)sd2 * 0.5f * p->nave_w;
                int   jc2 = sd2 > 0 ? 2 : 0;
                rrc = -1;
                for (iic = 0; iic <= p->nbays; iic++) {
                    int up = iic < p->nbays &&
                             church_survives(p, ELEM_CLEREST, iic, jc2,
                                             (float *)0);
                    if (up && rrc < 0) rrc = iic;
                    if (!up && rrc >= 0) {
                        float rx0 = p->west_x + p->tower_d
                                  + (float)rrc * p->bay_l;
                        float rx1 = p->west_x + p->tower_d
                                  + (float)iic * p->bay_l;
                        if (rrc == 0) rx0 = x0;
                        if (iic == p->nbays) rx1 = x1;
                        parapet_strip(b, rx0, rx1, zc0 - 0.5f * pw,
                                      zc0 + 0.5f * pw,
                                      p->wall_h, p->wall_h + p->parapet_h,
                                      1, 0);
                        rrc = -1;
                    }
                }
            }
        }
    }

    {
        float tkt;
        if (p->tower &&
            (!church_survives(p, ELEM_WALL, 0, 1, &tkt) || tkt < 0.96f))
            goto tower_done;
    }
    if (p->tower) {   /* the shaft above the nave walls, belfry pairs */
        float th = p->wall_h * (2.0f + 0.4f *
                   gothic_hash01(p->seed, LANE_TOWER_H, 0, 0));
        float x0 = p->west_x - wt, x1 = p->west_x + p->tower_d + 0.5f * wt;
        float sill = th - 2.2f, spring = th - 1.2f;
        float hloc = th - p->wall_h;
        int   f;
        for (f = -1; f <= 1; f += 2) {        /* the side faces */
            float zc = (float)f * 0.5f * p->nave_w;
            float half_w = 0.5f * (x1 - x0);
            float xm = 0.5f * (x0 + x1);
            GothicOpening bo;
            bo.kind = GOTHIC_OPEN_WINDOW; bo.cx = 0.0f;
            bo.w = 0.4f * half_w; bo.sill = sill; bo.spring = spring;
            bo.acute = p->acute;
            opening_fit(&bo, th - 0.4f);
            place_piece(b, half_w, hloc, wt, &bo, 0.0f, p->wall_h,
                        WF_NO_BOTTOM | WF_NO_ENDS, (float)f, 0.0f,
                        xm - 0.25f * (x1 - x0), zc);
            place_piece(b, half_w, hloc, wt, &bo, 0.0f, p->wall_h,
                        WF_NO_BOTTOM | WF_NO_ENDS, (float)f, 0.0f,
                        xm + 0.25f * (x1 - x0), zc);
        }
        for (f = 0; f < 2; f++) {             /* west & east faces */
            float xc = f ? p->west_x + p->tower_d : p->west_x - 0.5f * wt;
            float yc = f ? 1.0f : -1.0f;
            float half_w = 0.5f * p->nave_w - 0.5f * wt;
            GothicOpening bo;
            bo.kind = GOTHIC_OPEN_WINDOW; bo.cx = 0.0f;
            bo.w = 0.4f * half_w; bo.sill = sill; bo.spring = spring;
            bo.acute = p->acute;
            opening_fit(&bo, th - 0.4f);
            place_piece(b, half_w, hloc, wt, &bo, 0.0f, p->wall_h,
                        WF_NO_BOTTOM | WF_NO_ENDS, 0.0f, yc,
                        xc, -0.25f * p->nave_w);
            place_piece(b, half_w, hloc, wt, &bo, 0.0f, p->wall_h,
                        WF_NO_BOTTOM | WF_NO_ENDS, 0.0f, yc,
                        xc, 0.25f * p->nave_w);
        }
        {   /* the tower's parapet ring */
            float pw = 0.5f * wt;
            float zs = 0.5f * p->nave_w + 0.5f * wt;
            parapet_strip(b, x0, x1, -zs - 0.5f * pw, -zs + 0.5f * pw,
                          th, th + p->parapet_h, 1, 0);
            parapet_strip(b, x0, x1, zs - 0.5f * pw, zs + 0.5f * pw,
                          th, th + p->parapet_h, 1, 0);
            parapet_strip(b, x0, x0 + pw, -(zs - 0.5f * pw), zs - 0.5f * pw,
                          th, th + p->parapet_h, 0, 1);
            parapet_strip(b, x1 - pw, x1, -(zs - 0.5f * pw), zs - 0.5f * pw,
                          th, th + p->parapet_h, 0, 1);
        }
    }
tower_done:
    (void)pfh;
}

/* ================== item 5: vaults — quadripartite ribs & webs =====
   The crown of the system. Per bay: the DIAGONALS are semicircular
   (a = 0) over the bay diagonal, so the crown is exactly q/2 above the
   springing — the historical datum. Transverse and wall ribs are
   pointed, their acuteness from the LEVEL-CROWN SOLVE — item 2's
   formula applied exactly as the lodges applied it. Ribs die on the
   abacus (ruled v1; the tas-de-charge block is the flagged
   refinement). Webs are ruled lofts between half-ribs, emitted as
   thin SLABS (intrados + extrados, ruins show the back), domed
   gently. */

#define VAULT_SEG 0.6f       /* rib chords: big smooth arcs, coarser
                                than molding scale, shared by the webs */
#define WEB_T     0.12f      /* the web slab's thickness              */

static vec3 vlerp(vec3 a, vec3 b, float t) {
    return vec3_add(a, vec3_scale(vec3_sub(b, a), t));
}

/* the arch path between two plan points, stood in its vertical plane,
   springer -> crown -> springer; crown forced exact by arch_path_n */
static int vault_path(vec3 *out, float x0, float z0, float x1, float z1,
                      float spring, float a) {
    vec3  tmp[GOTHIC_ARCH_MAX_PTS];
    float dx = x1 - x0, dz = z1 - z0;
    float s  = sqrtf(dx * dx + dz * dz);
    float mx = 0.5f * (x0 + x1), mz = 0.5f * (z0 + z1);
    int   n_h, n, i;
    if (s < 0.2f) return 0;
    dx /= s; dz /= s;
    n_h = arch_half_segments(s, a, VAULT_SEG);
    if (2 * n_h + 1 > GOTHIC_ARCH_MAX_PTS) n_h = (GOTHIC_ARCH_MAX_PTS - 1) / 2;
    n = arch_path_n(tmp, s, a, n_h);
    for (i = 0; i < n; i++) {
        out[i].x = mx + dx * tmp[i].x;
        out[i].y = spring + tmp[i].y;
        out[i].z = mz + dz * tmp[i].x;
    }
    return n;
}

/* sweep PROF_RIB along a vault path — REVERSED so the section's o
   points into the room: the roll hangs below the web (the archivolt
   lesson), the open o=0 back rests against the web's intrados line */
static void vault_rib(MeshBuilder *b, const vec3 *path, int n, float scale) {
    vec3 rev[GOTHIC_ARCH_MAX_PTS];
    vec3 d, pn;
    int  i, pn_n;
    const ProfilePt *rib = gothic_profile(PROF_RIB, &pn_n);
    if (n < 2) return;
    for (i = 0; i < n; i++) rev[i] = path[n - 1 - i];
    d   = vec3_sub(path[n - 1], path[0]);
    d.y = 0.0f;
    if (vec3_dot(d, d) < 1e-8f) return;
    pn = vec3_normalize(vec3_cross(d, vec3_make(0.0f, 1.0f, 0.0f)));
    gothic_sweep(b, rib, pn_n, rev, n, pn, scale, 1, 1);
}

/* the boss: a small octahedral knob at a crown — the future ornament
   slot, and the cover for the rib ends that converge there */
static void boss_knob(MeshBuilder *b, vec3 c, float r) {
    vec3 v[6];
    int  f;
    static const int F[8][3] = {
        { 0, 2, 4 }, { 2, 1, 4 }, { 1, 3, 4 }, { 3, 0, 4 },
        { 2, 0, 5 }, { 1, 2, 5 }, { 3, 1, 5 }, { 0, 3, 5 }
    };
    v[0] = vec3_add(c, vec3_make( r, 0.0f, 0.0f));
    v[1] = vec3_add(c, vec3_make(-r, 0.0f, 0.0f));
    v[2] = vec3_add(c, vec3_make(0.0f, 0.0f,  r));
    v[3] = vec3_add(c, vec3_make(0.0f, 0.0f, -r));
    v[4] = vec3_add(c, vec3_make(0.0f, -1.3f * r, 0.0f));   /* hangs */
    v[5] = vec3_add(c, vec3_make(0.0f,  1.3f * r, 0.0f));
    for (f = 0; f < 8; f++) {
        vec3 a = v[F[f][0]], bb = v[F[f][1]], cc = v[F[f][2]];
        vec3 nm = vec3_normalize(vec3_cross(vec3_sub(bb, a), vec3_sub(cc, a)));
        sol_u32 i0 = mb_push_vertex(b, a.x, a.y, a.z, nm.x, nm.y, nm.z, 0, 0);
        sol_u32 i1 = mb_push_vertex(b, bb.x, bb.y, bb.z, nm.x, nm.y, nm.z, 1, 0);
        sol_u32 i2 = mb_push_vertex(b, cc.x, cc.y, cc.z, nm.x, nm.y, nm.z, 0, 1);
        mb_push_triangle(b, i0, i1, i2);
    }
}

/* one web cell: the ruled loft between two half-ribs (springer->boss,
   EQUAL station counts — same span, same subdivision: equal index is
   equal arclength), domed by 0.08H sin(pi t) sin(pi s); the slab's two
   surfaces share the rib stations so adjacent cells meet seamlessly */
static vec3 web_pt(const vec3 *ra, const vec3 *rb, int n, int k,
                   float s, float dome, float lift,
                   float clen, float a_bnd) {
    /* the LUNETTE: the cell's wall-side edge follows its BOUNDARY ARCH
       (which crowns at the same H as the diagonals — the level-crown
       law pays again), fading toward the boss as (1-t)^2: the ridge
       from wall-crown to boss runs level with a real ridge's slight
       sag, and windows live inside the lunettes instead of behind a
       hanging chord canopy (Fran's punch list, error4) */
    float t  = (float)k / (float)(n - 1);
    float ft = (1.0f - t) * (1.0f - t);
    vec3  p  = vlerp(ra[k], rb[k], s);
    p.y += ft * gothic_arch_y(clen, a_bnd, (s - 0.5f) * clen)
         + dome * sinf(SOL_PI * t) * sinf(SOL_PI * s) + lift;
    return p;
}

static void web_quad(MeshBuilder *b, vec3 p00, vec3 p10, vec3 p11, vec3 p01,
                     vec3 ref) {
    /* the lunette lift twists quads near the wall edges — each triangle
       carries its OWN geometric normal, oriented toward ref (the room
       for the intrados, the sky for the extrados). The boss row's
       collapsed pair drops to one triangle. */
    vec3 e = vec3_sub(p11, p10);
    int  tri = vec3_dot(e, e) < 1e-6f;
    vec3 tp[2][3];
    int  nt = 0, q;
    if (tri) {
        tp[0][0] = p00; tp[0][1] = p10; tp[0][2] = p01; nt = 1;
    } else {
        tp[0][0] = p00; tp[0][1] = p10; tp[0][2] = p11;
        tp[1][0] = p00; tp[1][1] = p11; tp[1][2] = p01; nt = 2;
    }
    for (q = 0; q < nt; q++) {
        vec3 a = tp[q][0], c = tp[q][1], d = tp[q][2];
        vec3 nm = vec3_cross(vec3_sub(c, a), vec3_sub(d, a));
        vec3 cen = vec3_scale(vec3_add(vec3_add(a, c), d), 1.0f / 3.0f);
        float l = sqrtf(vec3_dot(nm, nm));
        sol_u32 i0, i1, i2;
        if (l < 1e-10f) continue;
        nm = vec3_scale(nm, 1.0f / l);
        if (vec3_dot(nm, vec3_sub(ref, cen)) < 0.0f) {
            vec3 sw = c; c = d; d = sw;
            nm = vec3_scale(nm, -1.0f);
        }
        i0 = mb_push_vertex(b, a.x, a.y, a.z, nm.x, nm.y, nm.z, a.x + a.z, a.y);
        i1 = mb_push_vertex(b, c.x, c.y, c.z, nm.x, nm.y, nm.z, c.x + c.z, c.y);
        i2 = mb_push_vertex(b, d.x, d.y, d.z, nm.x, nm.y, nm.z, d.x + d.z, d.y);
        mb_push_triangle(b, i0, i1, i2);
    }
}

static void web_cell(MeshBuilder *b, const vec3 *ra, const vec3 *rb,
                     int n, float dome) {
    vec3  chord = vec3_sub(rb[0], ra[0]);
    float clen  = sqrtf(vec3_dot(chord, chord));
    vec3  boss  = ra[n - 1];
    float a_bnd;
    vec3  ref_lo, ref_hi;
    int   m, k, j, side;
    if (n < 2 || clen < 0.2f) return;
    a_bnd = gothic_arch_acuteness_for(clen, boss.y - ra[0].y);
    m = (int)(clen / 0.5f);
    if (m < 2) m = 2;
    if (m > 5) m = 5;
    ref_lo = vec3_make(boss.x, ra[0].y - 4.0f, boss.z);   /* the room  */
    ref_hi = vec3_make(boss.x, boss.y + 6.0f, boss.z);    /* the sky   */
    for (side = 0; side < 2; side++) {
        float lift = side ? WEB_T : 0.0f;
        vec3  ref  = side ? ref_hi : ref_lo;
        for (k = 0; k + 1 < n; k++)
            for (j = 0; j < m; j++) {
                float s0 = (float)j / (float)m;
                float s1 = (float)(j + 1) / (float)m;
                web_quad(b,
                         web_pt(ra, rb, n, k,     s0, dome, lift, clen, a_bnd),
                         web_pt(ra, rb, n, k + 1, s0, dome, lift, clen, a_bnd),
                         web_pt(ra, rb, n, k + 1, s1, dome, lift, clen, a_bnd),
                         web_pt(ra, rb, n, k,     s1, dome, lift, clen, a_bnd),
                         ref);
            }
    }
}

/* a respond: the half-shaft a rib bundle lands on, half-buried in its
   wall plane (item 4's deferral cashed — the rib assignment exists) */
static void respond(MeshBuilder *b, float x, float z, float y0, float y1,
                    float scale) {
    int pn;
    const ProfilePt *oct = gothic_profile(PROF_SHAFT_OCT, &pn);
    vec3 path[2];
    if (y1 - y0 < 0.3f) return;
    path[0] = vec3_make(x, y0, z);
    path[1] = vec3_make(x, y1, z);
    gothic_sweep(b, oct, pn, path, 2, vec3_make(0.0f, 0.0f, 1.0f),
                 scale, 0, 1);
}

/* the flyer: a quarter-ellipse from the clerestory wall head down onto
   the (raised) outer buttress, swept with a rectangular section; both
   ends buried in their stone — §1.5's first registered dependency:
   flyer <- {clerestory wall, outer buttress} */
static const ProfilePt FLYER_RECT[5] = {
    { -0.15f, -0.13f, 1 }, { 0.15f, -0.13f, 1 }, { 0.15f, 0.13f, 1 },
    { -0.15f,  0.13f, 1 }, { -0.15f, -0.13f, 1 }
};

static void flyer(MeshBuilder *b, float x, float z_in, float z_out,
                  float y_high, float y_low) {
    vec3  pt[12];
    float run = z_out - z_in, rise = y_high - y_low;
    int   i, n = 9;
    if (rise < 0.4f) return;
    for (i = 0; i < n; i++) {
        float th = 0.5f * SOL_PI * (float)i / (float)(n - 1);
        pt[i] = vec3_make(x, y_low + rise * cosf(th), z_in + run * sinf(th));
    }
    gothic_sweep(b, FLYER_RECT, 5, pt, n,
                 vec3_make(run > 0.0f ? -1.0f : 1.0f, 0.0f, 0.0f),
                 1.0f, 1, 1);
}

/* one vaulted vessel: the four-cell quadripartite bays of a rectangle
   strip (z0..z1 wide), diagonals round, everything else level-crowned */
static void vault_vessel(MeshBuilder *b, const ChurchPlan *p,
                         float x_w, int nbays, float z0, float z1,
                         float spring, float rib_sc, int formerets,
                         int lane) {
    vec3  d1[GOTHIC_ARCH_MAX_PTS], d2[GOTHIC_ARCH_MAX_PTS];
    vec3  h1r[GOTHIC_ARCH_MAX_PTS], h2r[GOTHIC_ARCH_MAX_PTS];
    vec3  wr[GOTHIC_ARCH_MAX_PTS];
    float w = z1 - z0, l = p->bay_l;
    float hgt = 0.5f * sqrtf(w * w + l * l);
    float a_t = gothic_arch_acuteness_for(w, hgt);
    float a_l = gothic_arch_acuteness_for(l, hgt);
    float dome = 0.04f * hgt;
    int   i, k, n;

    for (i = 0; i < nbays; i++) {
        float xa = x_w + (float)i * l, xb = xa + l;
        float tk;
        int   nh, web_up, rib_up;
        web_up = church_survives(p, ELEM_WEB, i, lane, (float *)0);
        rib_up = church_survives(p, ELEM_RIB, i, lane, &tk);
        if (!web_up && !rib_up) continue;
        n = vault_path(d1, xa, z0, xb, z1, spring, 0.0f);
        if (n < 3) continue;
        vault_path(d2, xb, z0, xa, z1, spring, 0.0f);
        nh = (n - 1) / 2;
        if (web_up) {
            vault_rib(b, d1, n, rib_sc);
            vault_rib(b, d2, n, rib_sc);
            boss_knob(b, d1[nh], 0.16f + 0.05f * rib_sc);
        } else {
            /* SPRINGER STUBS on standing piers — the single most
               recognizable ruin signature: each diagonal keeps tk of
               its run from BOTH springers, ragged where it broke */
            int ns = (int)(tk * (float)nh);
            if (ns >= 2) {
                vec3 st1[GOTHIC_ARCH_MAX_PTS];
                int  q;
                vault_rib(b, d1, ns + 1, rib_sc);
                vault_rib(b, d2, ns + 1, rib_sc);
                for (q = 0; q <= ns; q++) st1[q] = d1[n - 1 - q];
                vault_rib(b, st1, ns + 1, rib_sc);
                for (q = 0; q <= ns; q++) st1[q] = d2[n - 1 - q];
                vault_rib(b, st1, ns + 1, rib_sc);
            }
        }
        if (web_up) {
            for (k = 0; k <= nh; k++) {        /* reversed halves */
                h1r[k] = d1[n - 1 - k];        /* from (xa, z1)   */
                h2r[k] = d2[n - 1 - k];        /* from (xb, z1)   */
            }
            web_cell(b, d1, d2, nh + 1, dome);
            web_cell(b, h1r, h2r, nh + 1, dome);
            web_cell(b, d1, h2r, nh + 1, dome);
            web_cell(b, d2, h1r, nh + 1, dome);
            if (formerets) {
                n = vault_path(wr, xa, z0, xb, z0, spring, a_l);
                vault_rib(b, wr, n, rib_sc * 0.65f);
                n = vault_path(wr, xa, z1, xb, z1, spring, a_l);
                vault_rib(b, wr, n, rib_sc * 0.65f);
            }
        }
    }
    /* transverse arches at the stations (whole with EITHER adjacent
       web, a stub on standing piers when both fell) */
    for (i = 0; i <= nbays; i++) {
        float x = x_w + (float)i * l;
        float tk;
        int   bay = i < nbays ? i : nbays - 1;
        int   up  = church_survives(p, ELEM_WEB, bay, lane, (float *)0) ||
                    (i > 0 && church_survives(p, ELEM_WEB, i - 1, lane,
                                              (float *)0));
        if (!church_survives(p, ELEM_RIB, bay, lane, &tk)) continue;
        n = vault_path(wr, x, z0, x, z1, spring, a_t);
        if (up) {
            vault_rib(b, wr, n, rib_sc * 1.1f);
        } else {
            int ns = (int)(tk * (float)((n - 1) / 2));
            if (ns >= 2) {
                vec3 st1[GOTHIC_ARCH_MAX_PTS];
                int  q;
                vault_rib(b, wr, ns + 1, rib_sc * 1.1f);
                for (q = 0; q <= ns; q++) st1[q] = wr[n - 1 - q];
                vault_rib(b, st1, ns + 1, rib_sc * 1.1f);
            }
        }
    }
}

/* nave/aisle/apse vaulting + responds + flyers — church_stone part 2 */
static void stone_vaults(MeshBuilder *b, const ChurchPlan *p) {
    float spring_n = p->style == CHURCH_BASILICA ? p->clerest_h0 :
                     p->style == CHURCH_HALL ? p->arcade_h : p->impost_h;
    float x_w  = p->west_x + p->tower_d;
    float hwid = 0.5f * p->nave_w + p->aisle_w;
    float rp   = stone_pier_r(p);
    int   i, side;

    /* skip the tower bay's high vault (the shaft is open to the top) */
    vault_vessel(b, p, x_w, p->nbays, -0.5f * p->nave_w, 0.5f * p->nave_w,
                 spring_n, 1.0f, 1, 1);
    if (p->aisles) {
        vault_vessel(b, p, x_w, p->nbays, -hwid, -0.5f * p->nave_w,
                     p->impost_h, 0.7f, 0, 0);
        vault_vessel(b, p, x_w, p->nbays, 0.5f * p->nave_w, hwid,
                     p->impost_h, 0.7f, 0, 2);
    }

    if (p->apse_sides == 5) {       /* the radial half-vault */
        vec3  rr[6][65];   /* half paths: (GOTHIC_ARCH_MAX_PTS-1)/2+1 */
        int   rn = 0, k;
        float r   = 0.5411961f * p->nave_w;
        float cxa = p->east_x + 0.3826834f * r;
        int apse_web = church_survives(p, ELEM_WEB, p->nbays, 1, (float *)0);
        for (k = 0; k < 6; k++) {
            vec3  full[GOTHIC_ARCH_MAX_PTS];
            float vx, vz, tk;
            int   n, j, ns;
            plan_apse_pier(p, k, &vx, &vz);
            n = vault_path(full, vx, vz, 2.0f * cxa - vx, -vz,
                           spring_n, 0.0f);
            if (n < 3) { rn = 0; break; }
            rn = (n - 1) / 2 + 1;
            for (j = 0; j < rn; j++) rr[k][j] = full[j];
            if (!church_survives(p, ELEM_RIB, p->nbays, k, &tk)) continue;
            /* whole (a station short of the half-boss), or a stub */
            ns = apse_web ? rn - 1 : (int)(tk * (float)(rn - 1));
            if (ns >= 2) vault_rib(b, rr[k], ns, 0.8f);
        }
        if (!apse_web) rn = 0;             /* no webs, no half-boss */
        if (rn >= 3) {
            /* the radial cells stop one station short of the center —
               five shallow fans converging at one point congest into
               near-coplanar slivers; the half-boss is FOR this */
            for (k = 0; k < 5; k++)
                web_cell(b, rr[k], rr[k + 1], rn - 1, 0.025f * r);
            boss_knob(b, rr[0][rn - 1], 0.62f);
        }
    }

    /* responds: the bundle's landing shafts (item 4's deferral cashed) */
    if (p->aisles) {
        for (i = p->tower ? 0 : 1; i < p->nbays; i++) {
            float x, z, tk;
            if (plan_pier(p, i, PIER_ROW_S_ARCADE, &x, &z) &&
                church_survives(p, ELEM_PIER, i, 0, &tk) && tk >= 0.999f)
                respond(b, x, z, p->impost_h, spring_n,
                        0.4f * rp / 0.12f);
            if (plan_pier(p, i, PIER_ROW_N_ARCADE, &x, &z) &&
                church_survives(p, ELEM_PIER, i, 2, &tk) && tk >= 0.999f)
                respond(b, x, z, p->impost_h, spring_n,
                        0.4f * rp / 0.12f);
        }
        for (i = 1; i < p->nbays; i++) {       /* aisle wall responds */
            float x = x_w + (float)i * p->bay_l;
            float tk;
            if (church_survives(p, ELEM_WALL, i, 0, &tk) && tk >= 0.96f)
                respond(b, x, -hwid, p->plinth_h, p->impost_h,
                        0.33f * rp / 0.12f);
            if (church_survives(p, ELEM_WALL, i, 2, &tk) && tk >= 0.96f)
                respond(b, x,  hwid, p->plinth_h, p->impost_h,
                        0.33f * rp / 0.12f);
        }
    } else {
        for (i = 1; i < p->nbays; i++) {       /* the chapel's wall shafts */
            float x = x_w + (float)i * p->bay_l;
            float tk;
            if (church_survives(p, ELEM_WALL, i, 0, &tk) && tk >= 0.96f)
                respond(b, x, -0.5f * p->nave_w, p->plinth_h, spring_n,
                        0.33f * rp / 0.12f);
            if (church_survives(p, ELEM_WALL, i, 2, &tk) && tk >= 0.96f)
                respond(b, x,  0.5f * p->nave_w, p->plinth_h, spring_n,
                        0.33f * rp / 0.12f);
        }
    }

    /* flyers: basilica only — the thrust line exists now (§1.5: the
       flyer depends on its clerestory bay AND its outer buttress) */
    if (p->style == CHURCH_BASILICA) {
        float wt = p->wall_t;
        for (i = 1; i < p->nbays; i++) {
            float x = x_w + (float)i * p->bay_l;
            float y_high = p->wall_h - 0.45f;
            float y_low  = 0.78f * (p->aisle_h + 0.6f);
            for (side = -1; side <= 1; side += 2) {
                float z_in  = (float)side * 0.5f * p->nave_w;
                float z_out = (float)side * (hwid + wt + 0.25f);
                if (!church_survives(p, ELEM_FLYER, i,
                                     side > 0 ? 2 : 0, (float *)0))
                    continue;
                flyer(b, x, z_in, z_out, y_high, y_low);
            }
        }
    }
}

/* ================== item 6: windows & tracery ==================
   The GEOMETRIC grammar: circles and pointed sub-arches only — every
   element compass-constructible from what is already there, which is
   why this is the most algorithmic period in architectural history,
   and why it is v1. */

/* internal tangency to the main arch's intrados pins cy for a centered
   foil of radius rf; defined when the foil fits under the crown */
static int foil_cy_main(float c_m, float r_m, float spring, float cx,
                        float rf, float *cy) {
    float rr = r_m - rf;
    float dx = cx - (cx < 0.0f ? c_m : -c_m);   /* the arc on this side */
    float q  = rr * rr - dx * dx;
    if (q <= 0.0f) return 0;
    *cy = spring + sqrtf(q);
    return 1;
}

/* the centered foil: internal to the main arcs (symmetric), external
   to the two nearest sub-arch arcs — one unknown after the main pins
   cy(rf); the residual is monotone, bisection is exact enough for the
   5 mm law and deterministic (the brief's own permission) */
static int foil_center(float c_m, float r_m, float sub_cx, float c_s,
                       float r_s, float spring, float *out_y, float *out_r) {
    float lo = 0.03f, hi = 0.45f * r_m, cy = 0.0f;
    float tx = sub_cx - c_s;            /* the sub-arch's inner arc center */
    int   it;
    for (it = 0; it < 36; it++) {
        float rf = 0.5f * (lo + hi);
        float dx, dy, res;
        if (!foil_cy_main(c_m, r_m, spring, 0.0f, rf, &cy)) { hi = rf; continue; }
        dx = 0.0f - tx; dy = cy - spring;
        res = sqrtf(dx * dx + dy * dy) - (r_s + rf);
        if (res > 0.0f) lo = rf; else hi = rf;
    }
    if (lo < 0.05f) return 0;
    if (!foil_cy_main(c_m, r_m, spring, 0.0f, lo, &cy)) return 0;
    *out_y = cy;
    *out_r = lo;
    return 1;
}

/* the off-center foil (n = 4's outer spandrels): cx joins the unknowns
   — nested bisection, the inner solving rf for left-sub tangency at a
   given cx, the outer balancing the right-sub residual */
static int foil_outer(float c_m, float r_m, float t0x, float t1x,
                      float c_s_unused, float r_s, float spring, float mx,
                      float half_range, float *ox, float *oy, float *orr) {
    float clo = mx - 1.3f * half_range, chi = mx + 1.3f * half_range;
    float cy = 0.0f, rf = 0.0f;
    int   oit, iit;
    (void)c_s_unused;
    for (oit = 0; oit < 30; oit++) {
        float cx = 0.5f * (clo + chi);
        float lo = 0.03f, hi = 0.9f * r_m;   /* generous: the main-arc
                                  constraint self-limits via foil_cy_main */
        float dx, dy, res1;
        for (iit = 0; iit < 30; iit++) {
            float rt = 0.5f * (lo + hi);
            float r0;
            if (!foil_cy_main(c_m, r_m, spring, cx, rt, &cy)) { hi = rt; continue; }
            dx = cx - t0x; dy = cy - spring;
            r0 = sqrtf(dx * dx + dy * dy) - (r_s + rt);
            if (r0 > 0.0f) lo = rt; else hi = rt;
        }
        rf = lo;
        if (!foil_cy_main(c_m, r_m, spring, cx, rf, &cy)) return 0;
        dx = cx - t1x; dy = cy - spring;
        res1 = sqrtf(dx * dx + dy * dy) - (r_s + rf);
        if (res1 > 0.0f) clo = cx; else chi = cx;
    }
    if (rf < 0.045f) return 0;
    *ox = clo; *oy = cy; *orr = rf;
    return 1;
}

int gothic_tracery(const GothicOpening *o, float divisor, GothicTracery *t) {
    float s, c_m, r_m, c_s, r_s, pl;
    int   n;
    if (!o || !t || o->kind != GOTHIC_OPEN_WINDOW) return 0;
    memset(t, 0, sizeof *t);
    s = o->w;
    if (s < 0.35f || divisor < 0.3f) return 0;
    n = (int)(s / divisor + 0.5f);
    if (n < 1) n = 1;
    if (n > 4) n = 4;
    t->n_lights  = n;
    t->pitch     = s / (float)n;
    t->sub_span  = t->pitch - (n > 1 ? 1.5f * 0.16f * 0.55f : 0.0f);
    t->sub_acute = o->acute;
    if (n == 1) { t->sub_span = t->pitch; return 1; }

    arch_cr(s, o->acute, &c_m, &r_m);
    pl = t->pitch;
    arch_cr(t->sub_span, o->acute, &c_s, &r_s);

    {   /* the center foil: n=2 between the pair, n=3 over the middle
           light, n=4 over the center pair — one symmetric solve */
        float sub_cx = (n == 3) ? 0.0f : -0.5f * pl;
        float fy, fr;
        if (foil_center(c_m, r_m, sub_cx, c_s, r_s, o->spring, &fy, &fr)) {
            t->foil_x[t->n_foils] = 0.0f;
            t->foil_y[t->n_foils] = fy;
            t->foil_r[t->n_foils] = fr;
            t->n_foils++;
        }
    }
    if (n == 4) {   /* small foils in the outer spandrels */
        float mx = -0.5f * s + pl;        /* the outer-left mullion line */
        float t0x = (mx - 0.5f * pl) - c_s;   /* left light's inner arc  */
        float t1x = (mx + 0.5f * pl) + c_s;   /* center-left's outer arc */
        float fx, fy, fr;
        if (foil_outer(c_m, r_m, t0x, t1x, c_s, r_s, o->spring, mx,
                       0.35f * pl, &fx, &fy, &fr)) {
            t->foil_x[t->n_foils] = fx;
            t->foil_y[t->n_foils] = fy;
            t->foil_r[t->n_foils] = fr;
            t->n_foils++;
            t->foil_x[t->n_foils] = -fx;      /* mirrored               */
            t->foil_y[t->n_foils] = fy;
            t->foil_r[t->n_foils] = fr;
            t->n_foils++;
        }
    }
    return 1;
}

/* a bigger earclip for the glass polygons (the cap version is sized
   for profiles; a traceried light's boundary runs longer) */
#define GLASS_MAX_PTS 28
static int glass_earclip(const float *px, const float *py, int m, int *tris) {
    int idx[GLASS_MAX_PTS];
    int k, j, nt = 0;
    for (k = 0; k < m; k++) idx[k] = k;
    while (m > 3) {
        /* the BEST ear, not the first: lowest-index clipping carves
           long slivers from elongated panels; score = fatness
           (area over squared perimeter), ties to the lower index —
           still fully deterministic */
        int   best = -1;
        float best_score = -1.0f;
        for (k = 0; k < m; k++) {
            int ia = idx[(k + m - 1) % m], ib = idx[k], ic = idx[(k + 1) % m];
            float ex0 = px[ib] - px[ia], ey0 = py[ib] - py[ia];
            float ex1 = px[ic] - px[ib], ey1 = py[ic] - py[ib];
            float ex2 = px[ia] - px[ic], ey2 = py[ia] - py[ic];
            float cr = ex0 * (py[ic] - py[ia]) - ey0 * (px[ic] - px[ia]);
            float per, score;
            int   blocked = 0;
            if (cr <= 1e-9f) continue;
            for (j = 0; j < m; j++) {
                float d1, d2, d3;
                int ip = idx[j];
                if (ip == ia || ip == ib || ip == ic) continue;
                d1 = ex0 * (py[ip] - py[ia]) - ey0 * (px[ip] - px[ia]);
                d2 = ex1 * (py[ip] - py[ib]) - ey1 * (px[ip] - px[ib]);
                d3 = ex2 * (py[ip] - py[ic]) - ey2 * (px[ip] - px[ic]);
                if (d1 > -1e-9f && d2 > -1e-9f && d3 > -1e-9f) { blocked = 1; break; }
            }
            if (blocked) continue;
            per = ex0 * ex0 + ey0 * ey0 + ex1 * ex1 + ey1 * ey1
                + ex2 * ex2 + ey2 * ey2;
            score = per > 1e-12f ? cr / per : 0.0f;
            if (score > best_score + 1e-9f) { best_score = score; best = k; }
        }
        if (best < 0) return nt;
        k = best;
        {
            int ia = idx[(k + m - 1) % m], ib = idx[k], ic = idx[(k + 1) % m];
            tris[nt * 3] = ia; tris[nt * 3 + 1] = ib; tris[nt * 3 + 2] = ic;
            nt++;
            for (j = k; j + 1 < m; j++) idx[j] = idx[j + 1];
            m--;
        }
    }
    tris[nt * 3] = idx[0]; tris[nt * 3 + 1] = idx[1]; tris[nt * 3 + 2] = idx[2];
    return nt + 1;
}

/* one glass panel: a flat polygon in window-local (x, y), emitted
   double-sided 3 mm apart (coincident sides are their own z-fight) */
static void glass_panel(MeshBuilder *g, const float *px, const float *py,
                        int m) {
    int  tris[(GLASS_MAX_PTS - 2) * 3];
    int  nt, k, side;
    if (!g || m < 3 || m > GLASS_MAX_PTS) return;
    nt = glass_earclip(px, py, m, tris);
    for (side = 0; side < 2; side++) {
        float z = side ? 0.003f : -0.003f;
        float nz = side ? 1.0f : -1.0f;
        for (k = 0; k < nt; k++) {
            int a = tris[k * 3], bb = tris[k * 3 + 1], cc = tris[k * 3 + 2];
            sol_u32 i0 = mb_push_vertex(g, px[a], py[a], z, 0, 0, nz, px[a], py[a]);
            sol_u32 i1, i2;
            if (side) {
                i1 = mb_push_vertex(g, px[bb], py[bb], z, 0, 0, nz, px[bb], py[bb]);
                i2 = mb_push_vertex(g, px[cc], py[cc], z, 0, 0, nz, px[cc], py[cc]);
            } else {
                i1 = mb_push_vertex(g, px[cc], py[cc], z, 0, 0, nz, px[cc], py[cc]);
                i2 = mb_push_vertex(g, px[bb], py[bb], z, 0, 0, nz, px[bb], py[bb]);
            }
            mb_push_triangle(g, i0, i1, i2);
        }
    }
}

/* dress ONE window: bars into `stone`, panels into `glass` (either may
   be NULL — church_stone and church_glass call the SAME walker, §1.2),
   all in window-local coords, then placed by the wall's own frame */
#define TR_BAR_SCALE 0.55f
static void emit_tracery(MeshBuilder *stone, MeshBuilder *glass,
                         const GothicOpening *o, float divisor,
                         float c, float s_, float tx, float tz) {
    GothicTracery t;
    vec3   path[GOTHIC_ARCH_MAX_PTS];
    int    pn_n, i, n;
    const ProfilePt *bar = gothic_profile(PROF_MULLION, &pn_n);
    sol_u32 v0s = stone ? stone->vertex_count : 0;
    sol_u32 v0g = glass ? glass->vertex_count : 0;

    if (!gothic_tracery(o, divisor, &t)) return;

    if (stone && t.n_lights > 1) {
        for (i = 1; i < t.n_lights; i++) {       /* the mullions */
            float mx = -0.5f * o->w + (float)i * t.pitch;
            path[0] = vec3_make(mx, o->sill + 0.004f, 0.0f);
            path[1] = vec3_make(mx, o->spring, 0.0f);
            gothic_sweep(stone, bar, pn_n, path, 2,
                         vec3_make(0.0f, 0.0f, 1.0f), TR_BAR_SCALE, 1, 0);
        }
        for (i = 0; i < t.n_lights; i++) {       /* the sub-arch heads */
            float lc = -0.5f * o->w + ((float)i + 0.5f) * t.pitch;
            int   j;
            n = gothic_arch_path(path, GOTHIC_ARCH_MAX_PTS, t.sub_span,
                                 t.sub_acute, GOTHIC_MAX_SEG);
            for (j = 0; j < n; j++) {
                path[j].x += lc;
                path[j].y += o->spring;
            }
            if (i == 0)              path[0].x     += 0.004f;  /* off the jamb */
            if (i == t.n_lights - 1) path[n - 1].x -= 0.004f;
            gothic_sweep(stone, bar, pn_n, path, n,
                         vec3_make(0.0f, 0.0f, 1.0f), TR_BAR_SCALE,
                         i == 0, i == t.n_lights - 1);
        }
        for (i = 0; i < t.n_foils; i++) {        /* the foiled circles */
            int   nc = gothic_arc_segments(2.0f * SOL_PI * t.foil_r[i],
                                           2.0f * SOL_PI);
            int   j;
            if (nc < 12) nc = 12;
            if (nc > 24) nc = 24;
            for (j = 0; j <= nc; j++) {
                float th = 2.0f * SOL_PI * (float)j / (float)nc;
                path[j] = vec3_make(t.foil_x[i] + t.foil_r[i] * sinf(th),
                                    t.foil_y[i] + t.foil_r[i] * cosf(th),
                                    0.0f);
            }
            path[nc] = path[0];                  /* closed: bit-equal seam */
            /* slimmer than the bars it kisses, and STAGGERED per foil:
               at a tangency the bodies interpenetrate (real tracery
               MERGES there) — equal scales would lay face on face, so
               every kissing pair differs by a step */
            gothic_sweep(stone, bar, pn_n, path, nc + 1,
                         vec3_make(0.0f, 0.0f, 1.0f),
                         TR_BAR_SCALE * (0.88f - 0.11f * (float)i),
                         0, 0);
        }
    }

    if (glass) {
        /* ONE full-aperture panel per window — the glass field is
           CONTINUOUS behind the tracery, as real glazing is; the bars
           sit proud of it on both sides. (Replaces the per-light
           patchwork, whose spandrels were open holes — Fran's punch
           list, error5. Item 8 shatters per window.) */
        float px[GLASS_MAX_PTS], py[GLASS_MAX_PTS];
        vec3  hp[GOTHIC_ARCH_MAX_PTS];
        int   m = 0, hn, j;
        hn = gothic_arch_path(hp, GOTHIC_ARCH_MAX_PTS, o->w, o->acute,
                              0.45f * o->w);
        if (hn >= 3 && hn + 4 <= GLASS_MAX_PTS) {
            px[m] = -0.5f * o->w; py[m] = o->sill; m++;
            px[m] =  0.5f * o->w; py[m] = o->sill; m++;
            for (j = hn - 1; j >= 0; j--) {   /* the head, right to left */
                px[m] = hp[j].x;
                py[m] = o->spring + hp[j].y;
                m++;
            }
            glass_panel(glass, px, py, m);
        }
    }

    if (stone) mb_transform_from(stone, v0s, c, s_, tx, 0.0f, tz);
    if (glass) mb_transform_from(glass, v0g, c, s_, tx, 0.0f, tz);
}

/* the window walk — the SAME windows, the SAME frames as the wall
   emitters; bars land in church_stone, panels in church_glass */
static void church_windows(MeshBuilder *stone, MeshBuilder *glass,
                           const ChurchPlan *p) {
    float divisor = 0.7f * (0.9f + 0.2f *
                    gothic_hash01(p->seed, LANE_TRACERY, 0, 0));
    float wt   = p->wall_t;
    float hwid = 0.5f * p->nave_w + p->aisle_w;
    GothicOpening o;
    int   i;

    for (i = 0; i < p->nbays; i++) {
        /* glass falls at .30 inside tracery standing to .42 — gate
           each independently and the walker's NULLs do the rest */
        MeshBuilder *st_s = church_survives(p, ELEM_TRACERY, i, 0, (float *)0) ? stone : (MeshBuilder *)0;
        MeshBuilder *gl_s = church_survives(p, ELEM_GLASS,   i, 0, (float *)0) ? glass : (MeshBuilder *)0;
        MeshBuilder *st_n = church_survives(p, ELEM_TRACERY, i, 2, (float *)0) ? stone : (MeshBuilder *)0;
        MeshBuilder *gl_n = church_survives(p, ELEM_GLASS,   i, 2, (float *)0) ? glass : (MeshBuilder *)0;
        plan_opening(p, WALL_AISLE_S, i, &o);
        if (o.kind == GOTHIC_OPEN_WINDOW && (st_s || gl_s))
            emit_tracery(st_s, gl_s, &o, divisor, 1.0f, 0.0f,
                         o.cx, -(hwid + 0.5f * wt));
        plan_opening(p, WALL_AISLE_N, i, &o);
        if (o.kind == GOTHIC_OPEN_WINDOW && (st_n || gl_n))
            emit_tracery(st_n, gl_n, &o, divisor, 1.0f, 0.0f,
                         o.cx, hwid + 0.5f * wt);
        if (p->style == CHURCH_BASILICA) {
            plan_opening(p, WALL_CLEREST_S, i, &o);
            if (o.kind == GOTHIC_OPEN_WINDOW && (st_s || gl_s))
                emit_tracery(st_s, gl_s, &o, divisor, 1.0f, 0.0f,
                             o.cx, -0.5f * p->nave_w);
            plan_opening(p, WALL_CLEREST_N, i, &o);
            if (o.kind == GOTHIC_OPEN_WINDOW && (st_n || gl_n))
                emit_tracery(st_n, gl_n, &o, divisor, 1.0f, 0.0f,
                             o.cx, 0.5f * p->nave_w);
        }
    }
    {
        MeshBuilder *st_w = church_survives(p, ELEM_TRACERY, 0, 1, (float *)0) ? stone : (MeshBuilder *)0;
        MeshBuilder *gl_w = church_survives(p, ELEM_GLASS,   0, 1, (float *)0) ? glass : (MeshBuilder *)0;
        MeshBuilder *st_e = church_survives(p, ELEM_TRACERY, p->nbays, 1, (float *)0) ? stone : (MeshBuilder *)0;
        MeshBuilder *gl_e = church_survives(p, ELEM_GLASS,   p->nbays, 1, (float *)0) ? glass : (MeshBuilder *)0;
        plan_opening(p, WALL_WEST, 1, &o);       /* the great window */
        if (o.kind == GOTHIC_OPEN_WINDOW && (st_w || gl_w))
            emit_tracery(st_w, gl_w, &o, divisor, 0.0f, -1.0f,
                         p->west_x - 0.5f * wt, 0.0f);
        plan_opening(p, WALL_EAST, 0, &o);
        if (o.kind == GOTHIC_OPEN_WINDOW && (st_e || gl_e))
            emit_tracery(st_e, gl_e, &o, divisor, 0.0f, 1.0f,
                         p->east_x + 0.5f * wt, 0.0f);
    }
    if (p->apse_sides == 5) {
        float rap   = 0.4f > 0.7f * wt ? 0.4f : 0.7f * wt;
        float inset = 0.83f * rap;
        int   k;
        for (k = 0; k < 5; k++) {                /* mirror stone_east */
            float ax, az, bx, bz, mx, mz, dx, dz, nl, nx, nz, len;
            plan_apse_pier(p, k,     &ax, &az);
            plan_apse_pier(p, k + 1, &bx, &bz);
            mx = 0.5f * (ax + bx); mz = 0.5f * (az + bz);
            dx = bx - ax; dz = bz - az;
            nl = sqrtf(dx * dx + dz * dz);
            len = nl - 2.0f * inset;
            if (len < 0.5f || nl < 1e-6f) continue;
            nx = dz / nl; nz = -dx / nl;
            plan_opening(p, WALL_APSE, k, &o);
            if (o.kind == GOTHIC_OPEN_WINDOW && o.w > len - 0.6f) {
                o.w = len - 0.6f;
                if (o.w < 0.3f) continue;
            }
            if (o.kind == GOTHIC_OPEN_WINDOW) {
                MeshBuilder *st_a = church_survives(p, ELEM_TRACERY,
                    p->nbays, k, (float *)0) ? stone : (MeshBuilder *)0;
                MeshBuilder *gl_a = church_survives(p, ELEM_GLASS,
                    p->nbays, k, (float *)0) ? glass : (MeshBuilder *)0;
                if (st_a || gl_a)
                    emit_tracery(st_a, gl_a, &o, divisor, nz, nx,
                                 mx + nx * 0.5f * wt, mz + nz * 0.5f * wt);
            }
        }
    }
}

void church_glass(MeshBuilder *b, const float *params, int count) {
    ChurchPlan plan;
    if (!b) return;
    church_plan(&plan, params, count);
    church_windows((MeshBuilder *)0, b, &plan);
}

/* ================== item 7: roof, tower & spire ==================
   The silhouette: after this item the ruin = 0 church is COMPLETE.
   Roofs are skins behind the parapets; the spire crosses square to
   octagon by broaches — ad quadratum from foundation to tip. */

/* one push for a flat triangle with its own normal */
static void tri3(MeshBuilder *b, vec3 a, vec3 c, vec3 d) {
    vec3 nm = vec3_cross(vec3_sub(c, a), vec3_sub(d, a));
    float l = sqrtf(vec3_dot(nm, nm));
    sol_u32 i0, i1, i2;
    if (l < 1e-10f) return;
    nm = vec3_scale(nm, 1.0f / l);
    i0 = mb_push_vertex(b, a.x, a.y, a.z, nm.x, nm.y, nm.z, a.x + a.z, a.y);
    i1 = mb_push_vertex(b, c.x, c.y, c.z, nm.x, nm.y, nm.z, c.x + c.z, c.y);
    i2 = mb_push_vertex(b, d.x, d.y, d.z, nm.x, nm.y, nm.z, d.x + d.z, d.y);
    mb_push_triangle(b, i0, i1, i2);
}
static void quad3(MeshBuilder *b, vec3 a, vec3 c, vec3 d, vec3 e) {
    tri3(b, a, c, d);
    tri3(b, a, d, e);
}

/* one roof slope: a thin slab — the weather face, the underside the
   nave looks up at, and the eave strip closing the low edge */
#define ROOF_T 0.12f
static void roof_slope(MeshBuilder *b, float x0, float x1,
                       float z_eave, float y_eave, float z_ridge,
                       float y_ridge) {
    vec3 e0 = vec3_make(x0, y_eave, z_eave);
    vec3 e1 = vec3_make(x1, y_eave, z_eave);
    vec3 r0 = vec3_make(x0, y_ridge, z_ridge);
    vec3 r1 = vec3_make(x1, y_ridge, z_ridge);
    vec3 dn = vec3_make(0.0f, -ROOF_T, 0.0f);
    /* top: wound so the normal points up-and-out */
    if (z_eave > z_ridge) quad3(b, e0, e1, r1, r0);
    else                  quad3(b, e1, e0, r0, r1);
    /* underside */
    if (z_eave > z_ridge)
        quad3(b, vec3_add(e1, dn), vec3_add(e0, dn),
              vec3_add(r0, dn), vec3_add(r1, dn));
    else
        quad3(b, vec3_add(e0, dn), vec3_add(e1, dn),
              vec3_add(r1, dn), vec3_add(r0, dn));
    /* the eave strip */
    if (z_eave > z_ridge)
        quad3(b, e1, e0, vec3_add(e0, dn), vec3_add(e1, dn));
    else
        quad3(b, e0, e1, vec3_add(e1, dn), vec3_add(e0, dn));
}

/* a gable: the triangular end wall as a thin slab, faces both ways */
static void roof_gable(MeshBuilder *b, float x, float dir, float z0,
                       float z1, float y_eave, float y_ridge) {
    float zr = 0.5f * (z0 + z1);
    vec3 a0 = vec3_make(x, y_eave, z0);
    vec3 a1 = vec3_make(x, y_eave, z1);
    vec3 ap = vec3_make(x, y_ridge, zr);
    vec3 off = vec3_make(0.12f * dir, 0.0f, 0.0f);
    if (dir > 0.0f) { tri3(b, a0, a1, ap); }
    else            { tri3(b, a1, a0, ap); }
    if (dir > 0.0f)
        tri3(b, vec3_add(a1, vec3_scale(off, -1.0f)),
             vec3_add(a0, vec3_scale(off, -1.0f)),
             vec3_add(ap, vec3_scale(off, -1.0f)));
    else
        tri3(b, vec3_add(a0, vec3_scale(off, -1.0f)),
             vec3_add(a1, vec3_scale(off, -1.0f)),
             vec3_add(ap, vec3_scale(off, -1.0f)));
}

void gothic_spire(MeshBuilder *b, float cx, float cz, float half,
                  float base_y, float apex_h, float broach_f) {
    /* the octagon inscribed in the square — ad quadratum: its flush
       faces ride the square's sides, its diagonal faces cross the
       corners, and the BROACHES fill what the crossing cut away */
    float t = half * 0.4142136f;          /* tan(22.5)*half: face half */
    vec3  apex = vec3_make(cx, base_y + apex_h, cz);
    vec3  v[8];
    int   k;
    if (!b || half < 0.05f || apex_h < 0.1f) return;
    v[0] = vec3_make(cx + half, base_y, cz - t);
    v[1] = vec3_make(cx + half, base_y, cz + t);
    v[2] = vec3_make(cx + t,    base_y, cz + half);
    v[3] = vec3_make(cx - t,    base_y, cz + half);
    v[4] = vec3_make(cx - half, base_y, cz + t);
    v[5] = vec3_make(cx - half, base_y, cz - t);
    v[6] = vec3_make(cx - t,    base_y, cz - half);
    v[7] = vec3_make(cx + t,    base_y, cz - half);
    for (k = 0; k < 8; k += 2)            /* the flush faces */
        tri3(b, v[k], v[k + 1], apex);
    for (k = 1; k < 8; k += 2) {          /* diagonal faces + broaches */
        vec3  va = v[k], vb = v[(k + 1) % 8];
        vec3  mid = vec3_scale(vec3_add(va, vb), 0.5f);
        vec3  p   = vlerp(mid, apex, broach_f);   /* the broach point */
        float sx  = (va.x - cx) + (vb.x - cx);
        float sz  = (va.z - cz) + (vb.z - cz);
        vec3  corner = vec3_make(cx + (sx > 0.0f ? half : -half), base_y,
                                 cz + (sz > 0.0f ? half : -half));
        /* the diagonal face SPLIT at p (edge-manifold by construction) */
        tri3(b, va, p, apex);
        tri3(b, p, vb, apex);
        /* the broach: a half-pyramid from the corner onto the face */
        tri3(b, corner, va, p);
        tri3(b, vb, corner, p);
    }
}

void gothic_pinnacle(MeshBuilder *b, float h, unsigned seed) {
    /* Roriczer simplified: shaft to 0.55h, gablets to 0.7h, the
       needle to h, a finial knob — derived from one square */
    float hw = 0.16f * h * (0.9f + 0.2f * gothic_hash01(seed, LANE_SPIRE, 1, 0));
    float y_sh = 0.55f * h, y_gb = 0.72f * h;
    int   k;
    if (!b || h < 0.3f) return;
    /* the base cap: an open shaft bottom reads as a hole the moment
       the seat is not flush (Fran's punch list, error2) */
    quad3(b, vec3_make(-hw, 0.0f, -hw), vec3_make(hw, 0.0f, -hw),
          vec3_make(hw, 0.0f, hw), vec3_make(-hw, 0.0f, hw));
    for (k = 0; k < 4; k++) {
        float c = (k == 0) ? 1.0f : (k == 2) ? -1.0f : 0.0f;
        float s = (k == 1) ? 1.0f : (k == 3) ? -1.0f : 0.0f;
        /* shaft face, gablet triangle leaning out, needle face */
        vec3 a = vec3_make(c * hw - s * hw, 0.0f,    s * hw + c * hw);
        vec3 d = vec3_make(c * hw + s * hw, 0.0f,    s * hw - c * hw);
        vec3 a1 = vec3_make(a.x, y_sh, a.z), d1 = vec3_make(d.x, y_sh, d.z);
        vec3 gp = vec3_make(c * 1.5f * hw, y_gb, s * 1.5f * hw);
        vec3 ap = vec3_make(0.0f, h, 0.0f);
        quad3(b, d, a, a1, d1);
        tri3(b, d1, a1, gp);                /* the gablet */
        tri3(b, a1, ap, gp);                /* needle flanks */
        tri3(b, gp, ap, d1);
    }
    boss_knob(b, vec3_make(0.0f, h, 0.0f), 0.10f * h);
}

/* the pinnacle ring on buttress heads + tower corners (needle mode) —
   emitted into church_stone (masonry); item 10's standalone ref and
   instanced scatter reuse gothic_pinnacle directly */
static void stone_pinnacles(MeshBuilder *b, const ChurchPlan *p) {
    float wt   = p->wall_t;
    float hwid = 0.5f * p->nave_w + p->aisle_w;
    float bh   = p->style == CHURCH_BASILICA ? p->aisle_h + 0.6f : p->aisle_h;
    float bd   = (p->style == CHURCH_BASILICA ? 1.05f :
                  p->style == CHURCH_HALL ? 0.75f : 0.55f);
    float ph   = 1.4f + 0.25f * p->wall_t * 4.0f;
    float ytop = 0.93f * bh + 0.30f;
    float seat = 0.30f * ph;             /* the coped seat's half-size */
    int   i, sd;
    for (i = 1; i < p->nbays; i++) {
        float x = p->west_x + p->tower_d + (float)i * p->bay_l;
        for (sd = -1; sd <= 1; sd += 2) {
            /* clear of the buttress back plane AND the parapet face —
               0.35bd landed the shaft face a centimeter off the back */
            float zc = (float)sd * (hwid + wt + 0.35f * bd + 0.07f);
            sol_u32 v0;
            /* the coped seat: buried into the weathering below, the
               pinnacle stands ON it — never hovers over the slope */
            gz_face(b, x - seat, x + seat, ytop - 0.45f, ytop, zc + seat, 1);
            gz_face(b, x - seat, x + seat, ytop - 0.45f, ytop, zc - seat, -1);
            gx_face(b, x - seat, ytop - 0.45f, ytop, zc - seat, zc + seat, -1);
            gx_face(b, x + seat, ytop - 0.45f, ytop, zc - seat, zc + seat, 1);
            gy_face(b, x - seat, x + seat, ytop, zc - seat, zc + seat, 1);
            v0 = b->vertex_count;
            gothic_pinnacle(b, ph, p->seed + (unsigned)i +
                            (sd > 0 ? 100u : 0u));
            mb_transform_from(b, v0, 1.0f, 0.0f, x, ytop - 0.05f, zc);
        }
    }
}

void church_roof(MeshBuilder *b, const float *params, int count) {
    ChurchPlan plan;
    const ChurchPlan *p = &plan;
    float pitch_t, wt, hwid, x_w, x_e, zn, y0;
    int   i;

    if (!b) return;
    church_plan(&plan, params, count);
    wt   = p->wall_t;
    hwid = 0.5f * p->nave_w + p->aisle_w;
    /* the High Gothic pitch, jittered per building */
    pitch_t = tanf((52.0f + 6.0f * gothic_hash01(p->seed, LANE_PITCH, 0, 0))
                   * (SOL_PI / 180.0f));
    x_w = p->west_x + p->tower_d + (p->tower ? 0.05f : 0.0f);
    x_e = p->east_x;
    y0  = p->wall_h + 0.08f;            /* behind the parapet: the gutter */

    /* the main roof: hall = ONE great roof over everything (the style
       made visible); basilica/chapel = the nave's gable. Per-bay RUNS:
       fallen bays open the vault's extrados to the sky, and each run
       break gets its slab edge bands. */
    zn = (p->style == CHURCH_HALL ? hwid : 0.5f * p->nave_w) + 0.1f * wt;
    {
        float yr = y0 + zn * pitch_t;
        int   r0 = -1;
        for (i = 0; i <= p->nbays; i++) {
            int up = i < p->nbays &&
                     church_survives(p, ELEM_ROOF, i, 1, (float *)0);
            if (up && r0 < 0) r0 = i;
            if (!up && r0 >= 0) {
                float sx0 = x_w + (float)r0 * p->bay_l
                          + ((r0 == 0 && !p->tower) ? 0.02f : 0.0f);
                float sx1 = x_w + (float)i * p->bay_l
                          - (i == p->nbays ? 0.02f : 0.0f);
                int   gw = r0 == 0 && !p->tower;
                int   ge = i == p->nbays && p->apse_sides == 0;
                roof_slope(b, sx0, sx1, -zn, y0, 0.0f, yr);
                roof_slope(b, sx0, sx1,  zn, y0, 0.0f, yr);
                if (gw) roof_gable(b, x_w, -1.0f, -zn, zn, y0, yr);
                else {                        /* the run-break band */
                    vec3 e0 = vec3_make(sx0, y0, -zn);
                    vec3 rr = vec3_make(sx0, yr, 0.0f);
                    vec3 e1 = vec3_make(sx0, y0, zn);
                    vec3 dn = vec3_make(0.0f, -ROOF_T, 0.0f);
                    quad3(b, e0, rr, vec3_add(rr, dn), vec3_add(e0, dn));
                    quad3(b, rr, e1, vec3_add(e1, dn), vec3_add(rr, dn));
                }
                if (ge) roof_gable(b, x_e, 1.0f, -zn, zn, y0, yr);
                else if (i < p->nbays || p->apse_sides != 0) {
                    vec3 e0 = vec3_make(sx1, y0, -zn);
                    vec3 rr = vec3_make(sx1, yr, 0.0f);
                    vec3 e1 = vec3_make(sx1, y0, zn);
                    vec3 dn = vec3_make(0.0f, -ROOF_T, 0.0f);
                    quad3(b, rr, e0, vec3_add(e0, dn), vec3_add(rr, dn));
                    quad3(b, e1, rr, vec3_add(rr, dn), vec3_add(e1, dn));
                }
                r0 = -1;
            }
        }
        if (p->apse_sides != 0 &&
            church_survives(p, ELEM_ROOF, p->nbays, 1, (float *)0))
        {                               /* the apse half-cone: its APEX
               tucks INTO the gable, so the mouth-side facet edges lie
               flat against the gable face — an apex over the apse
               center leaves an open wedge of sky at the junction
               (Fran's punch list, error7) */
            float r   = 0.5411961f * p->nave_w;
            float ya  = p->wall_h + 0.05f;
            float ridge = y0 + zn * pitch_t;
            float ay  = ya + 0.9f * r * pitch_t;
            vec3  apex;
            int   k;
            if (ay > ridge - 0.3f) ay = ridge - 0.3f;
            apex = vec3_make(x_e + 0.02f, ay, 0.0f);
            if (church_survives(p, ELEM_ROOF, p->nbays - 1, 1, (float *)0))
                roof_gable(b, x_e, 1.0f, -zn, zn, y0, ridge);
            for (k = 0; k < 5; k++) {
                float ax, az, bx, bz;
                plan_apse_pier(p, k,     &ax, &az);
                plan_apse_pier(p, k + 1, &bx, &bz);
                tri3(b, vec3_make(bx, ya, bz), vec3_make(ax, ya, az), apex);
            }
        }
    }

    if (p->style == CHURCH_BASILICA) {  /* the lean-to aisle roofs */
        float z_in  = 0.5f * p->nave_w + 0.5f * wt + 0.02f;
        float z_out = hwid + 0.4f * wt;
        float y_out = p->aisle_h + 0.06f;
        float y_in  = p->clerest_h0 - 0.15f;
        int   side;
        if (y_in < y_out + 0.3f) y_in = y_out + 0.3f;
        for (side = -1; side <= 1; side += 2) {
            float zo = (float)side * z_out, zi = (float)side * z_in;
            {   /* per-bay runs like the nave; each run carries its
                   own end closures (edge band + attic triangle) */
                int r0 = -1, ii;
                float yo_u = y_out - ROOF_T, yi_u = y_in - ROOF_T;
                for (ii = 0; ii <= p->nbays; ii++) {
                    int up = ii < p->nbays &&
                             church_survives(p, ELEM_ROOF, ii,
                                             side > 0 ? 2 : 0, (float *)0);
                    if (up && r0 < 0) r0 = ii;
                    if (!up && r0 >= 0) {
                        float rx0 = x_w + (float)r0 * p->bay_l;
                        float rx1 = x_w + (float)ii * p->bay_l;
                        vec3 t0w = vec3_make(rx0, y_out, zo);
                        vec3 t1w = vec3_make(rx0, y_in, zi);
                        vec3 u0w = vec3_make(rx0, yo_u, zo);
                        vec3 u1w = vec3_make(rx0, yi_u, zi);
                        vec3 t0e = vec3_make(rx1, y_out, zo);
                        vec3 t1e = vec3_make(rx1, y_in, zi);
                        vec3 u0e = vec3_make(rx1, yo_u, zo);
                        vec3 u1e = vec3_make(rx1, yi_u, zi);
                        vec3 w2 = vec3_make(rx0, yo_u, zi);
                        vec3 e2 = vec3_make(rx1, yo_u, zi);
                        roof_slope(b, rx0, rx1, zo, y_out, zi, y_in);
                        if (side > 0) {
                            quad3(b, t1w, t0w, u0w, u1w);
                            quad3(b, t0e, t1e, u1e, u0e);
                            tri3(b, u1w, u0w, w2);
                            tri3(b, u0e, u1e, e2);
                        } else {
                            quad3(b, t0w, t1w, u1w, u0w);
                            quad3(b, t1e, t0e, u0e, u1e);
                            tri3(b, u0w, u1w, w2);
                            tri3(b, u1e, u0e, e2);
                        }
                        r0 = -1;
                    }
                }
            }
        }
    }

    if (p->tower &&
        church_survives(p, ELEM_SPIRE, 0, 1, (float *)0)) {
        float th   = p->wall_h * (2.0f + 0.4f *
                     gothic_hash01(p->seed, LANE_TOWER_H, 0, 0));
        float half = 0.5f * p->nave_w * 0.82f;
        float cx   = p->west_x + 0.5f * p->tower_d - 0.25f * wt;
        float roll = gothic_hash01(p->seed, LANE_SPIRE, 0, 0);
        if (roll < 0.65f)               /* the broach spire */
            gothic_spire(b, cx, 0.0f, half, th + 0.05f,
                         half * 2.6f, 0.34f);
        else {                          /* parapet-and-needle */
            gothic_spire(b, cx, 0.0f, half * 0.45f, th + 0.05f,
                         half * 1.7f, 0.30f);
            for (i = 0; i < 4; i++) {
                sol_u32 v0 = b->vertex_count;
                float sx = (i & 1) ? half : -half;
                float sz = (i & 2) ? half : -half;
                gothic_pinnacle(b, 1.6f, p->seed + (unsigned)i + 40u);
                mb_transform_from(b, v0, 1.0f, 0.0f, cx + sx, th + 0.05f, sz);
            }
        }
    }
    (void)i;
}

/* ================== item 8: the ruin & the worksite ==================
   Survival is a QUERY (see gothic.h): pure, deterministic, consulted
   at every emitter site. No ruin pass, no mesh surgery, ever. */

/* gothic's own value-noise twin (mesh.c's is static; §1.3's lanes
   want gothic-owned noise anyway): smooth bilinear over a lattice
   hashed through the lane system */
static float gothic_vnoise(float x, float z, unsigned seed, int lane) {
    int   ix = (int)floorf(x), iz = (int)floorf(z);
    float fx = x - (float)ix, fz = z - (float)iz;
    float sx = fx * fx * (3.0f - 2.0f * fx);
    float sz = fz * fz * (3.0f - 2.0f * fz);
    float a = gothic_hash01(seed, lane, ix,     iz);
    float b = gothic_hash01(seed, lane, ix + 1, iz);
    float c = gothic_hash01(seed, lane, ix,     iz + 1);
    float d = gothic_hash01(seed, lane, ix + 1, iz + 1);
    float ab = a + (b - a) * sx;
    float cd = c + (d - c) * sx;
    return ab + (cd - ab) * sz;
}

/* the decay field: noise BLENDED with a gradient toward one end
   (LANE_RUIN_DIR) — collapse is spatially correlated, never
   salt-and-pepper; ruins have plots */
static float ruin_pressure(const ChurchPlan *p, int i, int j) {
    float fi, grad, d, ruin = p->ruin;
    if (ruin <= 0.0f) return 0.0f;
    fi = (float)i / (float)(p->nbays > 1 ? p->nbays : 1);
    grad = gothic_hash01(p->seed, LANE_RUIN_DIR, 0, 0) < 0.5f
               ? fi : 1.0f - fi;
    d = 0.55f * gothic_vnoise(0.7f * (float)i, 0.7f * (float)j,
                              p->seed, LANE_RUIN)
      + 0.45f * grad;
    return ruin * (0.35f + 0.65f * d);
}

/* the ladder: fragility-ordered, APPEND-ONLY (TODO6 item 8, ruled) */
static const float RUIN_T[ELEM_COUNT] = {
    0.12f,   /* ROOF                                            */
    0.28f,   /* WEB                                             */
    0.30f,   /* GLASS                                           */
    0.42f,   /* TRACERY  (> glass: the empty east frame)        */
    0.45f,   /* CLEREST                                         */
    0.50f,   /* RIB                                             */
    0.55f,   /* FLYER                                           */
    0.62f,   /* ARCADE                                          */
    0.65f,   /* PINNACLE                                        */
    0.70f,   /* WALL (upper; the lower courses hold to .95)     */
    0.80f,   /* PIER                                            */
    0.58f,   /* SPIRE (ladder position by VALUE, enum appended) */
    0.80f    /* BUTTRESS (pier-tough)                           */
};
#define RUIN_T_WALL_LOW 0.95f       /* ground plans outlive everything */

/* construction stages, east-to-west: 0 plinth, 1 piers+lower walls,
   2 arcade+aisles, 3 clerestory, 4 high vaults, 5 roof, 6 ornament */
static const int BUILT_STAGE[ELEM_COUNT] = {
    5,       /* ROOF      */
    4,       /* WEB       */
    6,       /* GLASS     */
    6,       /* TRACERY   */
    3,       /* CLEREST   */
    4,       /* RIB       */
    4,       /* FLYER     */
    2,       /* ARCADE    */
    6,       /* PINNACLE  */
    1,       /* WALL      */
    1,       /* PIER      */
    6,       /* SPIRE     */
    1        /* BUTTRESS  */
};

static int built_allows(const ChurchPlan *p, int elem, int i) {
    float stages, built = p->built;
    int   avail;
    if (built >= 1.0f) return 1;
    if (built <= 0.0f) return 0;
    stages = built * (6.0f + (float)p->nbays);
    avail  = (int)(stages - (float)(p->nbays - 1 - i));  /* east first */
    return BUILT_STAGE[elem] <= avail;
}

/* course-quantize a kept-height fraction (broken walls break along
   their joints — snapped tops read as masonry, not torn paper) */
static float quantize_keep(const ChurchPlan *p, float keep, float full_h) {
    float courses = full_h / 0.4f;
    float kept    = floorf(keep * courses + 0.5f);
    (void)p;
    if (kept < 1.0f) kept = 1.0f;
    return kept / courses;
}

int church_survives(const ChurchPlan *p, int elem, int i, int j,
                    float *t_keep) {
    float pr, tk = 1.0f;
    int   ok = 1;
    if (t_keep) *t_keep = 1.0f;
    if (!p || elem < 0 || elem >= ELEM_COUNT) return 0;
    if (!built_allows(p, elem, i)) return 0;
    pr = ruin_pressure(p, i, j);

    switch (elem) {
    case ELEM_WALL:
        /* partial: whole under .70, lower courses hold to .95 */
        if (pr >= RUIN_T_WALL_LOW) { tk = 0.12f; }
        else if (pr >= RUIN_T[ELEM_WALL]) {
            tk = 1.0f - 0.85f * (pr - RUIN_T[ELEM_WALL])
                       / (RUIN_T_WALL_LOW - RUIN_T[ELEM_WALL]);
        }
        if (tk < 1.0f) {
            tk = quantize_keep(p, tk, p->aisle_h);
            if (tk > 0.96f) tk = 0.96f;     /* a broken wall stays broken */
        }
        break;
    case ELEM_PIER:
        if (pr >= RUIN_T[ELEM_PIER]) {      /* the broken column */
            tk = 0.15f + 0.45f *
                 gothic_hash01(p->seed, LANE_RUIN, 100 + i, j);
            tk = quantize_keep(p, tk, p->impost_h);
        }
        break;
    case ELEM_RIB: {
        /* whole with its web; a springer stub on standing piers when
           the web fell; nothing on fallen piers */
        float dummy;
        int piers = church_survives(p, ELEM_PIER, i, j, &dummy)
                 && church_survives(p, ELEM_PIER,
                                    i + 1 > p->nbays ? p->nbays : i + 1,
                                    j, &dummy);
        if (!piers) return 0;
        if (!church_survives(p, ELEM_WEB, i, j, &dummy))
            tk = 0.15f + 0.25f *
                 gothic_hash01(p->seed, LANE_RUIN, 200 + i, j);
        break;
    }
    case ELEM_WEB: {
        float dummy;
        ok = pr < RUIN_T[ELEM_WEB]
          && pr < RUIN_T[ELEM_RIB]          /* its ribs' own rolls    */
          && church_survives(p, ELEM_PIER, i, j, &dummy)
          && church_survives(p, ELEM_PIER,
                             i + 1 > p->nbays ? p->nbays : i + 1, j, &dummy);
        break;
    }
    case ELEM_ROOF: {
        float dummy;
        ok = pr < RUIN_T[ELEM_ROOF]
          && church_survives(p, ELEM_WEB, i, 1, &dummy);
        break;
    }
    case ELEM_FLYER: {
        float dummy;
        int bay = i < p->nbays ? i : p->nbays - 1;
        ok = pr < RUIN_T[ELEM_FLYER]
          && church_survives(p, ELEM_CLEREST, bay, j, &dummy)
          && church_survives(p, ELEM_BUTTRESS, i, j, &dummy);
        break;
    }
    case ELEM_PINNACLE: {
        float dummy;
        ok = pr < RUIN_T[ELEM_PINNACLE]
          && church_survives(p, ELEM_BUTTRESS, i, j, &dummy);
        break;
    }
    case ELEM_ARCADE: {
        float dummy;
        ok = pr < RUIN_T[ELEM_ARCADE]
          && church_survives(p, ELEM_PIER, i, j, &dummy)
          && church_survives(p, ELEM_PIER,
                             i + 1 > p->nbays ? p->nbays : i + 1, j, &dummy);
        break;
    }
    case ELEM_GLASS: {
        float dummy;
        ok = pr < RUIN_T[ELEM_GLASS]
          && church_survives(p, ELEM_TRACERY, i, j, &dummy);
        break;
    }
    case ELEM_TRACERY: {
        float tw;
        ok = pr < RUIN_T[ELEM_TRACERY]
          && church_survives(p, ELEM_WALL, i, j, &tw)
          && tw >= 0.96f;                   /* its wall stands whole  */
        break;
    }
    case ELEM_SPIRE: {
        float tw;
        ok = pr < RUIN_T[ELEM_SPIRE]
          && church_survives(p, ELEM_WALL, 0, j, &tw)
          && tw >= 0.96f;
        break;
    }
    default:
        ok = pr < RUIN_T[elem];
        break;
    }
    if (!ok) return 0;
    if (t_keep) *t_keep = tk;
    return 1;
}

/* the pavement — the group's fourth member: per-bay slabs that the
   ruin OMITS where the vault above fell (item 9 sinks the datum so
   the island's grass becomes the floor of roofless bays) */
void church_floor(MeshBuilder *b, const float *params, int count) {
    ChurchPlan plan;
    const ChurchPlan *p = &plan;
    float y, hwid;
    int   i, lane;

    if (!b) return;
    church_plan(&plan, params, count);
    y    = p->plinth_h - 0.02f;        /* a course lip below the plinth */
    hwid = 0.5f * p->nave_w + p->aisle_w;

    for (i = 0; i < p->nbays; i++) {
        float x0 = p->west_x + p->tower_d + (float)i * p->bay_l;
        float x1 = x0 + p->bay_l;
        for (lane = 0; lane <= 2; lane++) {
            float z0, z1;
            if (lane == 1) { z0 = -0.5f * p->nave_w; z1 = 0.5f * p->nave_w; }
            else if (!p->aisles) continue;
            else if (lane == 0) { z0 = -hwid; z1 = -0.5f * p->nave_w; }
            else                { z0 = 0.5f * p->nave_w; z1 = hwid; }
            if (!church_survives(p, ELEM_WEB, i, lane, (float *)0))
                continue;
            gy_face(b, x0, x1, y, z0, z1, 1);
        }
    }
    if (p->tower &&
        church_survives(p, ELEM_WEB, 0, 1, (float *)0))
        gy_face(b, p->west_x, p->west_x + p->tower_d, y,
                -0.5f * p->nave_w, 0.5f * p->nave_w, 1);
    if (p->apse_sides == 5 &&
        church_survives(p, ELEM_WEB, p->nbays, 1, (float *)0)) {
        int k;
        for (k = 0; k < 5; k++) {       /* a fan over the chevet floor */
            float ax, az, bx, bz;
            plan_apse_pier(p, k,     &ax, &az);
            plan_apse_pier(p, k + 1, &bx, &bz);
            tri3(b, vec3_make(p->east_x, y, 0.0f),
                 vec3_make(ax, y, az), vec3_make(bx, y, bz));
        }
    }
}

/* rubble (item 8's deferral, landing with the ground): low fBm mounds
   in heavily-ruined bays, based below the pavement line so they meet
   whatever slope the datum left — debris is masonry, into the stone */
static void stone_rubble(MeshBuilder *b, const ChurchPlan *p) {
    int i, lane;
    float hwid = 0.5f * p->nave_w + p->aisle_w;
    for (i = 0; i < p->nbays; i++)
        for (lane = 0; lane <= 2; lane++) {
            float pr, cx, cz, rad, hgt;
            int   gx, gz;
            if (lane != 1 && !p->aisles) continue;
            if (church_survives(p, ELEM_WEB, i, lane, (float *)0)) continue;
            pr = ruin_pressure(p, i, lane);
            if (pr < 0.45f) continue;
            cx = p->west_x + p->tower_d + ((float)i + 0.35f
               + 0.3f * gothic_hash01(p->seed, LANE_RUIN, 300 + i, lane))
               * p->bay_l;
            cz = lane == 1 ? 0.0f
               : (lane == 0 ? -1.0f : 1.0f) * 0.5f * (0.5f * p->nave_w + hwid);
            cz += (gothic_hash01(p->seed, LANE_RUIN, 400 + i, lane) - 0.5f)
                * 0.4f * p->bay_l;
            rad = 0.7f + 0.9f * gothic_hash01(p->seed, LANE_RUIN, 500 + i, lane);
            hgt = 0.35f + 0.35f * pr;
            for (gx = 0; gx < 5; gx++)
                for (gz = 0; gz < 5; gz++) {
                    float u0 = -1.0f + 0.4f * (float)gx;
                    float v0 = -1.0f + 0.4f * (float)gz;
                    vec3  q[4];
                    int   k2;
                    for (k2 = 0; k2 < 4; k2++) {
                        float uu = u0 + ((k2 == 1 || k2 == 2) ? 0.4f : 0.0f);
                        float vv = v0 + (k2 >= 2 ? 0.4f : 0.0f);
                        float rr = sqrtf(uu * uu + vv * vv);
                        float fall = rr >= 1.0f ? 0.0f
                                   : 0.5f * (1.0f + cosf(SOL_PI * rr));
                        float n = gothic_vnoise(2.0f * uu + (float)i,
                                                2.0f * vv + (float)lane,
                                                p->seed, LANE_RUIN);
                        /* based at the FOUNDATION depth, the walls' own
                           law — a mound rim above the slope floats;
                           one that climbs from the skirt emerges */
                        q[k2] = vec3_make(cx + uu * rad,
                                          -GOTHIC_FOUNDATION
                                          + (GOTHIC_FOUNDATION
                                             + hgt * (0.4f + 0.6f * n))
                                              * fall,
                                          cz + vv * rad);
                    }
                    web_quad(b, q[0], q[1], q[2], q[3],
                             vec3_make(cx, 8.0f, cz));
                }
        }
}

/* ================== item 10: the follies ==================
   Standalone vocabulary: every emitter below composes machinery the
   church already proved — pier(), the sweep, the arch path, the face
   helpers, quantize_keep. No plan: a single piece has no dependency
   graph, so ruin is a truncation parameter, snapped to the same
   course lines the church breaks on. */

/* a box from the face helpers, abutment-aware like the kit's walls */
static void folly_box(MeshBuilder *b, float x0, float x1, float y0,
                      float y1, float z0, float z1, int top, int bottom,
                      int ends) {
    gz_face(b, x0, x1, y0, y1, z1,  1);
    gz_face(b, x0, x1, y0, y1, z0, -1);
    if (ends) {
        gx_face(b, x0, y0, y1, z0, z1, -1);
        gx_face(b, x1, y0, y1, z0, z1,  1);
    }
    if (top)    gy_face(b, x0, x1, y1, z0, z1,  1);
    if (bottom) gy_face(b, x0, x1, y0, z0, z1, -1);
}

/* the shared derivation: proportions + the kept fraction — emitter and
   collider read the SAME truncation (one formula, two readers) */
static void column_dims(float h, float style, float broken, float *plinth,
                        float *rp, float *st0, float *keep) {
    int capital;
    if (h < 0.6f) h = 0.6f;
    capital = (style < 0.5f) ? 1 : 0;
    *plinth = 0.45f;
    if (*plinth > 0.25f * h) *plinth = 0.25f * h;
    *rp = 0.06f * h;                           /* shaft ~8 diameters tall */
    if (*rp < 0.09f) *rp = 0.09f;
    if (*rp > 0.45f) *rp = 0.45f;
    *st0  = capital ? (h - 0.45f) : h;
    *keep = 1.0f;
    if (broken > 0.001f) {
        float k = 1.0f - broken;
        if (k < 0.08f) k = 0.08f;
        k = quantize_keep((const ChurchPlan *)0, k, h);
        if (k > 0.93f) k = 0.93f;              /* a broken column stays broken */
        *keep = k;
    }
}

float gothic_column_top(float h, float style, float broken) {
    float plinth, rp, st0, keep;
    if (h < 0.6f) h = 0.6f;
    column_dims(h, style, broken, &plinth, &rp, &st0, &keep);
    return (keep < 0.999f) ? plinth + keep * (st0 - plinth) : h;
}

void gothic_column(MeshBuilder *b, float h, float style, float broken,
                   unsigned seed) {
    float plinth, rp, st0, keep;
    int   capital;
    if (h < 0.6f) h = 0.6f;
    capital = (style < 0.5f) ? 1 : 0;          /* 1 = bare drum */
    column_dims(h, style, broken, &plinth, &rp, &st0, &keep);
    pier(b, plinth, h, 0.0f, 0.0f, rp, h, capital, 1.0f, 0.0f, keep);
    if (keep < 0.999f) {   /* the snapped core: a slimmer course-tall stub
                              riding the break, hash-set off center — the
                              jagged cap without new machinery */
        float bt  = plinth + keep * (st0 - plinth);
        float sh  = 0.4f * (0.5f + gothic_hash01(seed, LANE_FOLLY, 0, 2));
        if (bt + sh > st0) sh = st0 - bt;
        if (sh > 0.12f) {
            float ox = (gothic_hash01(seed, LANE_FOLLY, 0, 0) - 0.5f)
                       * rp * 0.6f;
            float oz = (gothic_hash01(seed, LANE_FOLLY, 0, 1) - 0.5f)
                       * rp * 0.6f;
            sol_u32 v0 = b->vertex_count;
            int pn;
            const ProfilePt *oct = gothic_profile(PROF_SHAFT_OCT, &pn);
            vec3 path[2];
            path[0] = vec3_make(0.0f, bt, 0.0f);
            path[1] = vec3_make(0.0f, bt + sh, 0.0f);
            gothic_sweep(b, oct, pn, path, 2, vec3_make(0.0f, 0.0f, 1.0f),
                         0.55f * rp / 0.12f, 0, 1);
            mb_transform_from(b, v0, 1.0f, 0.0f, ox, 0.0f, oz);
        }
    }
}

/* the masonry ring of an arch head from station 0 to i1 (of n), lifted
   to spring height: front/back ring faces, intrados, extrados, and a
   cap on the broken end. The outer curve rides each station's in-plane
   outward normal (central difference over the FULL path, so a truncated
   ring keeps the curve's own direction at its broken end). The ring's
   springing end dies into the jamb below — no cap, the abutment law. */
static void arch_band(MeshBuilder *b, const vec3 *path, int n, int i1,
                      float band, float t, float spring, int cap_end) {
    float nx[GOTHIC_ARCH_MAX_PTS], ny[GOTHIC_ARCH_MAX_PTS];
    float al[GOTHIC_ARCH_MAX_PTS];
    float zf = 0.5f * t, zb = -0.5f * t;
    int   k;
    if (n < 2 || i1 < 1) return;
    if (i1 > n - 1) i1 = n - 1;
    al[0] = 0.0f;
    for (k = 0; k <= i1; k++) {
        int   ka = (k > 0) ? k - 1 : 0;
        int   kb = (k < n - 1) ? k + 1 : n - 1;
        float dx = path[kb].x - path[ka].x;
        float dy = path[kb].y - path[ka].y;
        float l  = sqrtf(dx * dx + dy * dy);
        if (l < 1e-7f) { dx = 0.0f; dy = 1.0f; l = 1.0f; }
        nx[k] = -dy / l;                  /* left of travel = outward */
        ny[k] =  dx / l;
        if (k > 0) {
            float sx = path[k].x - path[k - 1].x;
            float sy = path[k].y - path[k - 1].y;
            al[k] = al[k - 1] + sqrtf(sx * sx + sy * sy);
        }
    }
    for (k = 0; k < i1; k++) {
        float ix0 = path[k].x,     iy0 = spring + path[k].y;
        float ix1 = path[k + 1].x, iy1 = spring + path[k + 1].y;
        float ox0 = ix0 + band * nx[k],     oy0 = iy0 + band * ny[k];
        float ox1 = ix1 + band * nx[k + 1], oy1 = iy1 + band * ny[k + 1];
        float sx = ix1 - ix0, sy = iy1 - iy0;
        float sl = sqrtf(sx * sx + sy * sy);
        float snx, sny;
        if (sl < 1e-7f) continue;
        snx = -sy / sl; sny = sx / sl;    /* this segment's flat normal */
        /* front + back ring faces */
        g_quad4(b, ix0,iy0,zf, al[k],iy0,   ix1,iy1,zf, al[k+1],iy1,
                   ox1,oy1,zf, al[k+1],oy1, ox0,oy0,zf, al[k],oy0,
                0.0f, 0.0f, 1.0f);
        g_quad4(b, ix1,iy1,zb, al[k+1],iy1, ix0,iy0,zb, al[k],iy0,
                   ox0,oy0,zb, al[k],oy0,   ox1,oy1,zb, al[k+1],oy1,
                0.0f, 0.0f, -1.0f);
        /* intrados (faces the opening) + extrados (faces the sky) —
           flat per strip: voussoir faceting, the item-2 ruling */
        g_quad4(b, ix0,iy0,zf, al[k],zf,  ix0,iy0,zb, al[k],zb,
                   ix1,iy1,zb, al[k+1],zb, ix1,iy1,zf, al[k+1],zf,
                -snx, -sny, 0.0f);
        g_quad4(b, ox0,oy0,zb, al[k],zb,  ox0,oy0,zf, al[k],zf,
                   ox1,oy1,zf, al[k+1],zf, ox1,oy1,zb, al[k+1],zb,
                snx, sny, 0.0f);
    }
    if (cap_end) {   /* the broken end: faces along the travel direction */
        float ix = path[i1].x, iy = spring + path[i1].y;
        float ox = ix + band * nx[i1], oy = iy + band * ny[i1];
        float tx = ny[i1], ty = -nx[i1];  /* tangent = outward rotated -90 */
        g_quad4(b, ix,iy,zf, zf,iy,  ix,iy,zb, zb,iy,
                   ox,oy,zb, zb,oy,  ox,oy,zf, zf,oy,
                tx, ty, 0.0f);
    }
}

void gothic_arch_frag_dims(float span, float a, float h, float ruin,
                           float *jw, float *spring, float *aa,
                           float *hl, float *hr) {
    float crown, band = 0.35f;
    if (span < 0.5f) span = 0.5f;
    if (h < 1.2f) h = 1.2f;
    *jw = 0.35f + 0.10f * span;
    if (*jw > 0.9f) *jw = 0.9f;
    crown   = gothic_arch_y(span, a, 0.0f);
    *spring = h - crown - band;
    if (*spring < 0.3f) {             /* flatten to fit — the plan's move */
        *spring = 0.3f;
        a = gothic_arch_acuteness_for(span, h - band - *spring);
    }
    *aa = a;
    if (ruin < 0.35f) { *hl = h; *hr = h; return; }
    {   /* the ladder's kept jamb heights, course-quantized */
        float kl = 1.0f - ((ruin > 0.75f) ? 1.8f * (ruin - 0.75f) : 0.0f);
        float kr = 1.0f - 2.2f * (ruin - 0.35f);
        if (kl < 0.4f) kl = 0.4f;
        if (kr < 0.12f) kr = 0.12f;
        kl = quantize_keep((const ChurchPlan *)0, kl, *spring);
        kr = quantize_keep((const ChurchPlan *)0, kr, *spring);
        if (kl > 1.0f) kl = 1.0f;     /* the round-up trap: a jamb must
                                         never outgrow its springing */
        if (kr > 1.0f) kr = 1.0f;
        *hl = *spring * kl;
        *hr = *spring * kr;
    }
}

void gothic_arch_frag(MeshBuilder *b, float span, float a, float depth,
                      float h, float ruin) {
    vec3  path[GOTHIC_ARCH_MAX_PTS];
    float jw, w, hw, spring, band, zf, zb, hl, hr2;
    int   n;
    if (span  < 0.5f) span  = 0.5f;
    if (depth < 0.2f) depth = 0.2f;
    if (h     < 1.2f) h     = 1.2f;
    band = 0.35f;
    gothic_arch_frag_dims(span, a, h, ruin, &jw, &spring, &a, &hl, &hr2);
    w  = span + 2.0f * jw;
    hw = 0.5f * w;
    zf = 0.5f * depth; zb = -zf;
    plinth_strip(b, -hw - 0.10f, hw + 0.10f, zb - 0.10f, zf + 0.10f,
                 0.12f, 1, 0);        /* the doorstep IS the plinth */
    if (ruin < 0.35f) {               /* whole: the existing emitter */
        gothic_wall_arched(b, w, h, depth, jw, span, spring, a);
        return;
    }
    n = gothic_arch_path(path, GOTHIC_ARCH_MAX_PTS, span, a,
                         GOTHIC_MAX_SEG);
    {   /* the ladder: the ring erodes from the right springing back
           toward the surviving jamb; the right jamb falls fast, the
           left holds until deep ruin (heights from the dims read) */
        float fr = 1.0f - 1.55f * (ruin - 0.35f);
        if (fr < 0.0f) fr = 0.0f;
        folly_box(b, -hw, -hw + jw, 0.0f, hl, zb, zf, 1, 0, 1);
        folly_box(b,  hw - jw, hw, 0.0f, hr2, zb, zf, 1, 0, 1);
        if (n >= 2 && fr > 0.02f && hl > spring - 0.001f) {
            int i1 = (int)(fr * (float)(n - 1) + 0.5f);
            if (i1 >= 1)
                arch_band(b, path, n, i1, band, depth, spring, 1);
        }
    }
}

void gothic_stair(MeshBuilder *b, float w, float rise, float run,
                  int steps) {
    float hw, yb = -GOTHIC_FOUNDATION;
    int   k, n = steps;
    if (n < 1)  n = 1;
    if (n > 64) n = 64;
    if (w    < 0.4f)  w    = 0.4f;
    if (rise < 0.05f) rise = 0.05f;
    if (run  < 0.12f) run  = 0.12f;
    hw = 0.5f * w;
    for (k = 0; k < n; k++) {
        float x0  = run * (float)k, x1 = run * (float)(k + 1);
        float top = rise * (float)(k + 1);
        gy_face(b, x0, x1, top, -hw, hw, 1);                  /* tread */
        gx_face(b, x0, (k == 0) ? yb : rise * (float)k, top,
                -hw, hw, -1);                                 /* riser */
        gz_face(b, x0, x1, yb, top, -hw, -1);                 /* flanks */
        gz_face(b, x0, x1, yb, top,  hw,  1);
    }
    gx_face(b, run * (float)n, yb, rise * (float)n, -hw, hw, 1);
}

#define BALUSTER_POST 0.20f         /* end post width */

int gothic_balusters(float len, float h, unsigned seed, float ruin,
                     float *out_x, int max_n) {
    float inner, sp;
    int   n, i, kept = 0;
    (void)h;
    if (len < 0.8f) len = 0.8f;
    inner = len - 2.0f * (BALUSTER_POST + 0.06f);
    if (inner < 0.30f) return 0;
    n = (int)(inner / 0.32f);
    if (n < 1) n = 1;
    sp = inner / (float)n;
    for (i = 0; i < n; i++) {
        if (gothic_hash01(seed, LANE_FOLLY, i, 7) < ruin) continue;
        if (kept >= max_n) break;
        out_x[kept++] = -0.5f * inner + sp * ((float)i + 0.5f);
    }
    return kept;
}

void gothic_balustrade(MeshBuilder *b, float len, float h, unsigned seed,
                       float ruin) {
    float hl, yb = -GOTHIC_FOUNDATION;
    (void)seed;                    /* the balusters' — the pool reads it */
    if (len < 0.8f) len = 0.8f;
    if (h < 0.5f) h = 0.5f;
    hl = 0.5f * len;
    folly_box(b, -hl, -hl + BALUSTER_POST, yb, h + 0.08f,
              -0.11f, 0.11f, 1, 0, 1);                    /* end posts */
    folly_box(b,  hl - BALUSTER_POST, hl, yb, h + 0.08f,
              -0.11f, 0.11f, 1, 0, 1);
    folly_box(b, -hl + BALUSTER_POST, hl - BALUSTER_POST, yb,
              GOTHIC_BALUSTER_SILL, -0.09f, 0.09f, 1, 0, 0);   /* the sill —
                                       ends die into the posts */
    if (ruin <= 0.65f)             /* past that the rail has nothing to
                                      stand on and fell with them */
        folly_box(b, -hl + BALUSTER_POST, hl - BALUSTER_POST,
                  h - GOTHIC_BALUSTER_RAIL, h, -0.075f, 0.075f, 1, 1, 0);
}

void gothic_baluster_unit(MeshBuilder *b) {
    int pn;
    const ProfilePt *oct = gothic_profile(PROF_SHAFT_OCT, &pn);
    vec3 path[2];
    folly_box(b, -0.075f, 0.075f, 0.0f, 0.08f, -0.075f, 0.075f, 1, 0, 1);
    path[0] = vec3_make(0.0f, 0.06f, 0.0f);   /* ends hide in the blocks */
    path[1] = vec3_make(0.0f, 0.94f, 0.0f);
    gothic_sweep(b, oct, pn, path, 2, vec3_make(0.0f, 0.0f, 1.0f),
                 0.05f / 0.12f, 0, 0);
    folly_box(b, -0.075f, 0.075f, 0.92f, 1.0f, -0.075f, 0.075f, 1, 1, 1);
}

void gothic_cross(MeshBuilder *b, float h) {
    float yb = -GOTHIC_FOUNDATION, ht, harm0, harm1;
    if (h < 1.5f) h = 1.5f;
    ht    = h - 0.62f;                        /* shaft top: head begins */
    harm0 = ht + 0.26f;
    harm1 = harm0 + 0.16f;
    folly_box(b, -0.85f, 0.85f, yb,    0.18f, -0.85f, 0.85f, 1, 0, 1);
    folly_box(b, -0.55f, 0.55f, 0.18f, 0.36f, -0.55f, 0.55f, 1, 0, 1);
    folly_box(b, -0.26f, 0.26f, 0.36f, 0.81f, -0.26f, 0.26f, 1, 0, 1);
    {   /* the tapered shaft: four trapezoid sides, near-flat normals
           (3 cm of lean over two meters) */
        float r0 = 0.09f, r1 = 0.06f, y0 = 0.81f;
        g_quad4(b, -r0,y0,r0, -r0,y0,  r0,y0,r0, r0,y0,
                    r1,ht,r1,  r1,ht, -r1,ht,r1, -r1,ht, 0.0f,0.0f,1.0f);
        g_quad4(b,  r0,y0,-r0, r0,y0, -r0,y0,-r0, -r0,y0,
                   -r1,ht,-r1, -r1,ht,  r1,ht,-r1, r1,ht, 0.0f,0.0f,-1.0f);
        g_quad4(b,  r0,y0,r0,  r0,y0,  r0,y0,-r0, -r0,y0,
                    r1,ht,-r1, -r1,ht,  r1,ht,r1,  r1,ht, 1.0f,0.0f,0.0f);
        g_quad4(b, -r0,y0,-r0, -r0,y0, -r0,y0,r0,  r0,y0,
                   -r1,ht,r1,   r1,ht, -r1,ht,-r1, -r1,ht, -1.0f,0.0f,0.0f);
    }
    folly_box(b, -0.075f, 0.075f, ht, h, -0.075f, 0.075f, 1, 1, 1);
    folly_box(b, -0.34f, 0.34f, harm0, harm1,
              -0.070f, 0.070f, 1, 1, 1);      /* the arm: 5 mm shy of the
                                       upright's faces (the stagger law) */
}
