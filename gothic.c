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

/* the two-cap rule against an explicit linear cap (the arch path takes
   a caller max_seg per the brief's sketch); ceil with a dust epsilon —
   an arc that is EXACTLY k segments must not flip to k+1 on the last
   bit of a float division */
static int seg_two_caps(float arc_len, float arc_angle, float max_seg) {
    float q;
    int   nl, na;
    if (arc_len   < 0.0f) arc_len   = -arc_len;
    if (arc_angle < 0.0f) arc_angle = -arc_angle;
    q  = arc_len / max_seg;          nl = (int)q;
    if ((float)nl + 1e-4f < q) nl++;
    q  = arc_angle / GOTHIC_MAX_ANG; na = (int)q;
    if ((float)na + 1e-4f < q) na++;
    if (na > nl) nl = na;
    return nl < 1 ? 1 : nl;
}

int gothic_arc_segments(float arc_len, float arc_angle) {
    return seg_two_caps(arc_len, arc_angle, GOTHIC_MAX_SEG);
}

/* ---- the sweep ---- */

/* per-profile-point precomputed section data: cumulative arclength and
   the section normals of the incoming (edge j-1) and outgoing (edge j)
   profile edges — CCW winding makes (eu,-eo) the outward normal */
typedef struct {
    float al;            /* arclength from point 0, unscaled       */
    float ni_o, ni_u;    /* incoming edge's outward section normal */
    float no_o, no_u;    /* outgoing edge's outward section normal */
} SectPt;

/* the vertex ids one ring contributed, per profile point: vin carries the
   incoming edge's normal side, vout the outgoing — equal when the point
   is smooth (one shared, averaged vertex) */
typedef struct {
    sol_u32 vin[GOTHIC_SWEEP_MAX_PROF];
    sol_u32 vout[GOTHIC_SWEEP_MAX_PROF];
} RingIds;

static void sect_build(const ProfilePt *prof, int prof_n, SectPt *sp) {
    int j;
    sp[0].al = 0.0f;
    for (j = 0; j + 1 < prof_n; j++) {
        float eo = prof[j + 1].o - prof[j].o;
        float eu = prof[j + 1].u - prof[j].u;
        float l  = sqrtf(eo * eo + eu * eu);
        if (l > 1e-8f) { eo /= l; eu /= l; }
        sp[j].no_o     = eu;  sp[j].no_u     = -eo;
        sp[j + 1].ni_o = eu;  sp[j + 1].ni_u = -eo;
        sp[j + 1].al   = sp[j].al + l;
    }
    /* endpoints: define the unused side so every read is initialized */
    sp[0].ni_o = sp[0].no_o;                  sp[0].ni_u = sp[0].no_u;
    sp[prof_n - 1].no_o = sp[prof_n - 1].ni_o; sp[prof_n - 1].no_u = sp[prof_n - 1].ni_u;
}

/* map a section-space normal through the ring's frame (o_n is the UNIT
   in-plane axis used for shading — positions may ride a stretched one) */
static vec3 sect_normal(vec3 o_n, vec3 up, float so, float su) {
    return vec3_normalize(vec3_add(vec3_scale(o_n, so), vec3_scale(up, su)));
}

/* emit one ring of vertices at pos; o_pos is the (possibly miter-
   stretched) position axis, o_n the unit shading axis, v_arc the path
   arclength (the v texcoord). Fills ids. */
static void emit_ring(MeshBuilder *b, const ProfilePt *prof, int prof_n,
                      const SectPt *sp, vec3 pos, vec3 o_pos, vec3 o_n,
                      vec3 up, float scale, float v_arc, RingIds *ids) {
    int j;
    for (j = 0; j < prof_n; j++) {
        vec3  p, n;
        float uu = sp[j].al * scale;
        p = vec3_add(pos, vec3_add(vec3_scale(o_pos, prof[j].o * scale),
                                   vec3_scale(up,    prof[j].u * scale)));
        if (j == 0) {                              /* open start: one edge */
            n = sect_normal(o_n, up, sp[j].no_o, sp[j].no_u);
            ids->vout[j] = mb_push_vertex(b, p.x, p.y, p.z, n.x, n.y, n.z, uu, v_arc);
            ids->vin[j]  = ids->vout[j];
        } else if (j == prof_n - 1) {              /* open end: one edge */
            n = sect_normal(o_n, up, sp[j].ni_o, sp[j].ni_u);
            ids->vin[j]  = mb_push_vertex(b, p.x, p.y, p.z, n.x, n.y, n.z, uu, v_arc);
            ids->vout[j] = ids->vin[j];
        } else if (prof[j].crease) {               /* hard arris: two */
            n = sect_normal(o_n, up, sp[j].ni_o, sp[j].ni_u);
            ids->vin[j]  = mb_push_vertex(b, p.x, p.y, p.z, n.x, n.y, n.z, uu, v_arc);
            n = sect_normal(o_n, up, sp[j].no_o, sp[j].no_u);
            ids->vout[j] = mb_push_vertex(b, p.x, p.y, p.z, n.x, n.y, n.z, uu, v_arc);
        } else {                                   /* roll: one, averaged */
            float ao = sp[j].ni_o + sp[j].no_o;
            float au = sp[j].ni_u + sp[j].no_u;
            float l  = sqrtf(ao * ao + au * au);
            if (l > 1e-8f) { ao /= l; au /= l; }
            else { ao = sp[j].no_o; au = sp[j].no_u; }   /* 180: pick one */
            n = sect_normal(o_n, up, ao, au);
            ids->vin[j]  = mb_push_vertex(b, p.x, p.y, p.z, n.x, n.y, n.z, uu, v_arc);
            ids->vout[j] = ids->vin[j];
        }
    }
}

/* quads between consecutive rings, one per profile edge; winding chosen
   so the geometric normal matches the edge's section normal (asserted by
   gothic_test against a known box sweep) */
static void stitch(MeshBuilder *b, const RingIds *a, const RingIds *c, int prof_n) {
    int j;
    for (j = 0; j + 1 < prof_n; j++) {
        mb_push_triangle(b, a->vout[j], a->vin[j + 1], c->vin[j + 1]);
        mb_push_triangle(b, a->vout[j], c->vin[j + 1], c->vout[j]);
    }
}

/* cap triangulation: a deterministic earclip in SECTION space — a fan is
   not enough, the string course's cavetto is concave. Lowest-index ear
   each pass (determinism §1.8); input is CCW (the table test asserts it).
   Bounded, allocation-free. Item 6's glass panels will want this too. */
static int cap_earclip(const ProfilePt *prof, int m, int *tris) {
    int idx[GOTHIC_SWEEP_MAX_PROF];
    int k, j, nt = 0;
    for (k = 0; k < m; k++) idx[k] = k;
    while (m > 3) {
        int found = 0;
        for (k = 0; k < m && !found; k++) {
            int ia = idx[(k + m - 1) % m], ib = idx[k], ic = idx[(k + 1) % m];
            float ax = prof[ia].o, ay = prof[ia].u;
            float bx = prof[ib].o, by = prof[ib].u;
            float cx = prof[ic].o, cy = prof[ic].u;
            float cr = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
            int blocked = 0;
            if (cr <= 1e-9f) continue;              /* reflex or flat corner */
            for (j = 0; j < m; j++) {
                float px, py, d1, d2, d3;
                int ip = idx[j];
                if (ip == ia || ip == ib || ip == ic) continue;
                px = prof[ip].o; py = prof[ip].u;
                d1 = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
                d2 = (cx - bx) * (py - by) - (cy - by) * (px - bx);
                d3 = (ax - cx) * (py - cy) - (ay - cy) * (px - cx);
                if (d1 > -1e-9f && d2 > -1e-9f && d3 > -1e-9f) { blocked = 1; break; }
            }
            if (blocked) continue;
            tris[nt * 3] = ia; tris[nt * 3 + 1] = ib; tris[nt * 3 + 2] = ic;
            nt++;
            for (j = k; j + 1 < m; j++) idx[j] = idx[j + 1];
            m--;
            found = 1;
        }
        if (!found) return nt;                      /* malformed: degrade */
    }
    tris[nt * 3] = idx[0]; tris[nt * 3 + 1] = idx[1]; tris[nt * 3 + 2] = idx[2];
    return nt + 1;
}

/* flat end cap over the profile polygon (closed profiles drop their seam
   copy). flip=1 for the start cap (facing -path_dir): section-CCW
   triangles face +path_dir in the right-handed (o,u,dir) frame, so the
   start cap reverses. UVs are the scaled section coords. */
static void cap_emit(MeshBuilder *b, const ProfilePt *prof, int prof_n,
                     vec3 pos, vec3 o_pos, vec3 up, float scale,
                     vec3 n, int flip) {
    sol_u32 vid[GOTHIC_SWEEP_MAX_PROF];
    int     tris[(GOTHIC_SWEEP_MAX_PROF - 2) * 3];
    int     m = prof_n, k, nt;
    if (prof[0].o == prof[prof_n - 1].o && prof[0].u == prof[prof_n - 1].u)
        m = prof_n - 1;                             /* closed: drop the seam */
    if (m < 3) return;
    nt = cap_earclip(prof, m, tris);
    for (k = 0; k < m; k++) {
        vec3 p = vec3_add(pos, vec3_add(vec3_scale(o_pos, prof[k].o * scale),
                                        vec3_scale(up,    prof[k].u * scale)));
        vid[k] = mb_push_vertex(b, p.x, p.y, p.z, n.x, n.y, n.z,
                                prof[k].o * scale, prof[k].u * scale);
    }
    for (k = 0; k < nt; k++) {
        if (flip) mb_push_triangle(b, vid[tris[k * 3]], vid[tris[k * 3 + 2]],
                                   vid[tris[k * 3 + 1]]);
        else      mb_push_triangle(b, vid[tris[k * 3]], vid[tris[k * 3 + 1]],
                                   vid[tris[k * 3 + 2]]);
    }
}

void gothic_sweep(MeshBuilder *b, const ProfilePt *prof, int prof_n,
                  const vec3 *path, int path_n, vec3 plane_n,
                  float scale, int cap0, int cap1) {
    SectPt  sp[GOTHIC_SWEEP_MAX_PROF];
    RingIds prev, cur;
    vec3    up, last_pos;
    float   arc;
    int     i;
    /* joints turning past 30 degrees auto-crease: two rings at the same
       miter positions, per-segment shading frames — a sharp stone arris
       instead of a smeared normal */
    static const float COS_CREASE = 0.86602540f;

    if (!b || !prof || !path) return;
    if (prof_n < 2 || prof_n > GOTHIC_SWEEP_MAX_PROF || path_n < 2) return;

    up = vec3_normalize(plane_n);
    sect_build(prof, prof_n, sp);

    {
        vec3 d0 = vec3_normalize(vec3_sub(path[1], path[0]));
        vec3 o0 = vec3_normalize(vec3_cross(up, d0));
        emit_ring(b, prof, prof_n, sp, path[0], o0, o0, up, scale, 0.0f, &prev);
        if (cap0)
            cap_emit(b, prof, prof_n, path[0], o0, up, scale,
                     vec3_scale(d0, -1.0f), 1);
    }
    last_pos = path[0];
    arc = 0.0f;

    for (i = 1; i < path_n; i++) {
        vec3  seg = vec3_sub(path[i], last_pos);
        float slen = sqrtf(vec3_dot(seg, seg));
        vec3  d_s;
        if (slen <= 1e-7f) continue;               /* doubled point: drop */
        d_s = vec3_scale(seg, 1.0f / slen);
        arc += slen;
        last_pos = path[i];

        if (i == path_n - 1) {                     /* the end ring */
            vec3 oe = vec3_normalize(vec3_cross(up, d_s));
            emit_ring(b, prof, prof_n, sp, path[i], oe, oe, up, scale, arc, &cur);
            stitch(b, &prev, &cur, prof_n);
            if (cap1)
                cap_emit(b, prof, prof_n, path[i], oe, up, scale, d_s, 0);
        } else {                                   /* an interior joint */
            vec3  d_n = vec3_normalize(vec3_sub(path[i + 1], path[i]));
            float c   = vec3_dot(d_s, d_n);
            vec3  m, o_u, o_p;
            if (c < -0.999f) return;               /* doubled back: refuse */
            m   = vec3_normalize(vec3_add(d_s, d_n));
            o_u = vec3_normalize(vec3_cross(up, m));
            o_p = vec3_scale(o_u, 1.0f / sqrtf((1.0f + c) * 0.5f));
            if (c >= COS_CREASE) {                 /* gentle: one ring */
                emit_ring(b, prof, prof_n, sp, path[i], o_p, o_u, up, scale, arc, &cur);
                stitch(b, &prev, &cur, prof_n);
                prev = cur;
            } else {                               /* sharp: crease it */
                vec3 oa = vec3_normalize(vec3_cross(up, d_s));
                vec3 ob = vec3_normalize(vec3_cross(up, d_n));
                emit_ring(b, prof, prof_n, sp, path[i], o_p, oa, up, scale, arc, &cur);
                stitch(b, &prev, &cur, prof_n);
                emit_ring(b, prof, prof_n, sp, path[i], o_p, ob, up, scale, arc, &cur);
                prev = cur;
            }
        }
    }
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
    n = seg_two_caps(r * phi, phi, max_seg);
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

/* the shared core: N recessed orders, widest at the FRONT (+z) face,
   sharing center cx, springing height and acuteness. Around the gap,
   slab by slab: each order owns its sub-thickness's jamb reveals,
   intrados strips, threshold/top/bottom strips; the wall's front face
   reads the widest arch, the back face the narrowest; step faces
   connect consecutive orders (jamb rectangles + the curved ladder).
   Impossible parameters (crown above the top, opening past the wall)
   emit nothing — the plan (item 3) never asks for them. */
static void arched_orders(MeshBuilder *b, float w, float h, float t,
                          float cx, float ow, float spring_h, float a,
                          int orders, float step, int archivolts) {
    vec3  arch[GOTHIC_MAX_ORDERS][GOTHIC_ARCH_MAX_PTS];
    float x0[GOTHIC_MAX_ORDERS], x1[GOTHIC_MAX_ORDERS];
    float hw = 0.5f * w, zf, dt;
    int   n_h, np, k, j, has_l, has_r;

    if (orders < 1) orders = 1;
    if (orders > GOTHIC_MAX_ORDERS) orders = GOTHIC_MAX_ORDERS;
    if (step < 0.0f) step = 0.0f;
    if (t < 0.01f) t = 0.01f;
    if (spring_h < 0.0f) spring_h = 0.0f;
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
    if (has_l) gx_face(b, -hw, 0.0f, h, -zf, zf, -1);
    if (has_r) gx_face(b,  hw, 0.0f, h, -zf, zf,  1);

    for (k = 0; k < orders; k++) {
        float zk_f = zf - dt * (float)k;
        float zk_b = (k == orders - 1) ? -zf : zf - dt * (float)(k + 1);

        if (has_l) {
            gy_face(b, -hw, x0[k], h,    zk_b, zk_f,  1);    /* panel tops    */
            gy_face(b, -hw, x0[k], 0.0f, zk_b, zk_f, -1);    /* panel bottoms */
        }
        if (has_r) {
            gy_face(b, x1[k], hw, h,    zk_b, zk_f,  1);
            gy_face(b, x1[k], hw, 0.0f, zk_b, zk_f, -1);
        }
        gy_face(b, x0[k], x1[k], h,    zk_b, zk_f, 1);       /* head top      */
        gy_face(b, x0[k], x1[k], 0.0f, zk_b, zk_f, 1);       /* the THRESHOLD */
        gx_face(b, x0[k], 0.0f, spring_h, zk_b, zk_f,  1);   /* jamb reveals  */
        gx_face(b, x1[k], 0.0f, spring_h, zk_b, zk_f, -1);
        if (!has_l) gx_face(b, x0[k], spring_h, h, zk_b, zk_f, -1);  /* flush */
        if (!has_r) gx_face(b, x1[k], spring_h, h, zk_b, zk_f,  1);

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
    arched_orders(b, w, h, t, -hw + ox + 0.5f * ow, ow, spring_h, a,
                  1, 0.0f, 0);
}

void gothic_wall_portal(MeshBuilder *b, float w, float h, float t,
                        float ow, float spring_h, float a,
                        int orders, float step, int archivolts) {
    if (!b || w <= 0.0f || h <= 0.0f || ow <= 1e-5f) return;
    arched_orders(b, w, h, t, 0.0f, ow, spring_h, a, orders, step, archivolts);
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

/* the church ref schema's defaults — item 4's registry rows must carry
   these same values (gothictest will assert the two tables agree) */
static const float CHURCH_DEFAULTS[8] =
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
        full[k] = (params && k < count) ? params[k] : CHURCH_DEFAULTS[k];

    p->seed  = full[2] > 0.0f ? (unsigned)(full[2] + 0.5f) : 0u;
    p->acute = full[6] < 0.0f ? 0.0f : full[6];

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
        float arc_span = 0.8f * p->bay_l;       /* arcade clear span */

        p->plinth_h = 0.4f + 0.15f * r1;
        p->sill_h   = 1.1f + 0.5f  * r2;
        if (p->style == CHURCH_CHAPEL) {
            p->wall_t   = 0.6f + 0.2f * r3;
            p->impost_h = 2.2f + 0.8f * r4;     /* window springing line */
            p->arcade_h = p->impost_h;
            p->wall_h   = p->impost_h
                        + gothic_arch_y(0.55f * p->bay_l, p->acute, 0.0f)
                        + 1.0f;
            p->aisle_h  = p->wall_h;
        } else if (p->style == CHURCH_HALL) {
            p->wall_t   = 0.7f + 0.25f * r3;
            p->impost_h = 3.0f + 1.0f  * r4;
            p->arcade_h = p->impost_h
                        + gothic_arch_y(arc_span, p->acute, 0.0f) + 0.5f;
            p->wall_h   = p->arcade_h + 0.8f;
            p->aisle_h  = p->wall_h;            /* THE hall trait        */
        } else {
            p->wall_t     = 0.8f + 0.3f * r3;
            p->impost_h   = 3.4f + 1.2f * r4;
            p->arcade_h   = p->impost_h
                          + gothic_arch_y(arc_span, p->acute, 0.0f) + 0.5f;
            p->clerest_h0 = p->arcade_h + 0.8f + 0.4f * r5;  /* triforium */
            p->clerest_h1 = p->clerest_h0 + 1.7f + 0.6f * r6;
            p->wall_h     = p->clerest_h1 + 0.5f;
            p->aisle_h    = p->arcade_h + 0.4f;
        }
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
    default:
        break;
    }
}
