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

/* abutment flags (item 4): a piece sitting on a plinth suppresses its
   bottom; a piece under another suppresses its top; a piece ending at a
   pier or another wall suppresses its end faces — faces where pieces
   abut are SKIPPED, the flat emitter's law at composition scale */
#define WF_NO_TOP    1
#define WF_NO_BOTTOM 2
#define WF_NO_ENDS   4
#define WF_NO_SILL   8   /* the threshold/sill ledge: a plinth or floor
                            below already claims that plane */

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
    arched_orders(b, span, hloc, t, 0.0f, span, 0.0f, spring - ybase, a,
                  1, 0.0f, 0, flags | WF_NO_ENDS);
    mb_transform_from(b, v0, c, s, tx, ybase, tz);
}

/* the stepped buttress, emitted facing -Z with its back on z=0 (the
   wall's outer face), then placed. Stages step back, each topped by a
   WEATHERING slope (two sloped quads' worth: one quad + side
   triangles) — exposed faces only, the back never emitted. */
static void buttress(MeshBuilder *b, float h_head, float bw, float d0,
                     int stages, float c, float s, float tx, float tz) {
    sol_u32 v0 = b->vertex_count;
    float hx = 0.5f * bw;
    float fr2[2]; float fr3[3]; const float *fr;
    float y0 = 0.0f, dk = d0;
    int k;
    fr2[0] = 0.52f; fr2[1] = 0.93f;
    fr3[0] = 0.40f; fr3[1] = 0.68f; fr3[2] = 0.93f;
    fr = stages == 3 ? fr3 : fr2;
    if (stages != 3) stages = 2;
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
        {   /* side triangles under the slope */
            sol_u32 t0, t1, t2;
            t0 = mb_push_vertex(b, -hx, y1, -dk, -1.0f, 0.0f, 0.0f, -dk, y1);
            t1 = mb_push_vertex(b, -hx, y1, -dn, -1.0f, 0.0f, 0.0f, -dn, y1);
            t2 = mb_push_vertex(b, -hx, y1 + rise, -dn, -1.0f, 0.0f, 0.0f, -dn, y1 + rise);
            mb_push_triangle(b, t0, t1, t2);
            t0 = mb_push_vertex(b, hx, y1, -dk, 1.0f, 0.0f, 0.0f, -dk, y1);
            t1 = mb_push_vertex(b, hx, y1 + rise, -dn, 1.0f, 0.0f, 0.0f, -dn, y1 + rise);
            t2 = mb_push_vertex(b, hx, y1, -dn, 1.0f, 0.0f, 0.0f, -dn, y1);
            mb_push_triangle(b, t0, t1, t2);
        }
        y0 = y1 + rise;
        dk = dn;
    }
    mb_transform_from(b, v0, c, s, tx, 0.0f, tz);
}

/* the pier: octagonal shaft through an attic-base collar on a square
   sub-plinth, capital (frustum + abacus) at the impost. h_cap 0 skips
   the capital (apse buttress-piers run sheer to their flat tops). */
static void pier(MeshBuilder *b, const ChurchPlan *p, float px, float pz,
                 float rp, float top, int capital, float yaw_c, float yaw_s) {
    sol_u32 v0 = b->vertex_count;
    float sp = rp / 0.12f;             /* shaft profile scale            */
    float ap = rp * 0.9238795f;        /* the octagon's apothem          */
    int   pn;
    const ProfilePt *oct = gothic_profile(PROF_SHAFT_OCT, &pn);
    vec3  path[2];
    float shaft_top = capital ? p->impost_h - 0.45f : top;

    if (capital) {                     /* square sub-plinth */
        float hs = rp + 0.10f;
        gz_face(b, -hs, hs, 0.0f, p->plinth_h,  hs,  1);
        gz_face(b, -hs, hs, 0.0f, p->plinth_h, -hs, -1);
        gx_face(b, -hs, 0.0f, p->plinth_h, -hs, hs, -1);
        gx_face(b,  hs, 0.0f, p->plinth_h, -hs, hs,  1);
        gy_face(b, -hs, hs, p->plinth_h, -hs, hs, 1);   /* full top: the
                                          shaft and base stand ON it */
        {   /* the attic base: PR_BASE around the shaft on a square loop,
               seam mid-side (item 1's closed-loop lesson) */
            int bn;
            const ProfilePt *bp = gothic_profile(PROF_BASE, &bn);
            float sb = sp * 0.85f;
            vec3 loop[6];
            /* walk +X first so o = cross(+Y, d) points OUTWARD; the
               seam closes mid-side (item 1's closed-loop lesson) */
            loop[0] = vec3_make(0.0f, p->plinth_h, -ap);
            loop[1] = vec3_make( ap,  p->plinth_h, -ap);
            loop[2] = vec3_make( ap,  p->plinth_h,  ap);
            loop[3] = vec3_make(-ap,  p->plinth_h,  ap);
            loop[4] = vec3_make(-ap,  p->plinth_h, -ap);
            loop[5] = vec3_make(0.0f, p->plinth_h, -ap);
            gothic_sweep(b, bp, bn, loop, 6, vec3_make(0.0f, 1.0f, 0.0f),
                         sb, 0, 0);
        }
    }
    path[0] = vec3_make(0.0f, capital ? p->plinth_h : 0.0f, 0.0f);
    path[1] = vec3_make(0.0f, shaft_top, 0.0f);
    gothic_sweep(b, oct, pn, path, 2, vec3_make(0.0f, 0.0f, 1.0f),
                 sp, 0, capital ? 0 : 1);
    if (capital) {
        float aa = rp + 0.12f;                 /* abacus half-size  */
        float yf0 = shaft_top, yf1 = p->impost_h - 0.15f;
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
        gz_face(b, -aa, aa, yf1, p->impost_h,  aa,  1);
        gz_face(b, -aa, aa, yf1, p->impost_h, -aa, -1);
        gx_face(b, -aa, yf1, p->impost_h, -aa, aa, -1);
        gx_face(b,  aa, yf1, p->impost_h, -aa, aa,  1);
        gy_face(b, -aa, aa, p->impost_h, -aa, aa,  1);
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
    gy_face(b, x0, x1, h, z0, z1, 1);
    if (!zrun) {
        gz_face(b, x0, x1, 0.0f, h, z1,  1);
        gz_face(b, x0, x1, 0.0f, h, z0, -1);
        if (ends) {
            gx_face(b, x0, 0.0f, h, z0, z1, -1);
            gx_face(b, x1, 0.0f, h, z0, z1,  1);
        }
    } else {
        gx_face(b, x0, 0.0f, h, z0, z1, -1);
        gx_face(b, x1, 0.0f, h, z0, z1,  1);
        if (ends) {
            gz_face(b, x0, x1, 0.0f, h, z1,  1);
            gz_face(b, x0, x1, 0.0f, h, z0, -1);
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

    if (p->tower || p->porch > 0.0f) { /* nothing west of west_x but air */ }
    if (p->tower)                       /* the blank flank beside the tower */
        place_piece(b, p->tower_d + wt, hloc, wt, (const GothicOpening *)0,
                    0.0f, p->plinth_h, WF_NO_BOTTOM | WF_NO_ENDS, yawc, 0.0f,
                    p->west_x + 0.5f * (p->tower_d - wt), zc);
    for (i = 0; i < p->nbays; i++) {
        GothicOpening o;
        float x0 = p->west_x + p->tower_d + (float)i * p->bay_l;
        float xc = x0 + 0.5f * p->bay_l, w = p->bay_l;
        if (i == 0 && !p->tower) { w += wt; xc -= 0.5f * wt; }   /* corner */
        if (i == p->nbays - 1)   { w += wt; xc += 0.5f * wt; }
        plan_opening(p, wallq, i, &o);
        place_piece(b, w, hloc, wt,
                    o.kind != GOTHIC_OPEN_NONE ? &o : (const GothicOpening *)0,
                    yawc > 0.0f ? o.cx - xc : xc - o.cx,
                    p->plinth_h, WF_NO_BOTTOM | WF_NO_ENDS, yawc, 0.0f, xc, zc);
    }
    /* the corner end faces (the exposed strips beside the west/east
       walls — coplanar-ADJACENT with their faces, never overlapping);
       the face helpers want ORDERED ranges, so sort the side's span */
    {
        float za = (float)side * hwid, zb = (float)side * (hwid + wt);
        float z0 = za < zb ? za : zb, z1 = za < zb ? zb : za;
        gx_face(b, p->west_x - wt, p->plinth_h, p->aisle_h, z0, z1, -1);
        gx_face(b, p->east_x + wt, p->plinth_h, p->aisle_h, z0, z1,  1);
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

    plan_opening(p, WALL_WEST, 0, &door);
    plan_opening(p, WALL_WEST, 1, &win);
    h_split = door.spring + gothic_arch_y(door.w, door.acute, 0.0f) + 0.3f;

    {   /* the portal: recessed orders by style, archivolt-dressed.
           A chapel's facade ends abut the long walls' inner faces (no
           flanks stand beside it) — ends suppressed; aisled facades
           keep theirs, exposed above the flanks. */
        sol_u32 v0 = b->vertex_count;
        int ords = p->style == CHURCH_BASILICA ? 3 :
                   p->style == CHURCH_HALL ? 2 : 1;
        int ef = p->aisles ? 0 : WF_NO_ENDS;
        arched_orders(b, p->nave_w, h_split - p->plinth_h, wt, 0.0f,
                      door.w, 0.0f, door.spring - p->plinth_h, door.acute,
                      ords, 0.15f, 1,
                      WF_NO_TOP | WF_NO_BOTTOM | WF_NO_SILL | ef);
        mb_transform_from(b, v0, 0.0f, -1.0f, xc, p->plinth_h, 0.0f);
        place_piece(b, p->nave_w, p->wall_h - h_split, wt,
                    win.kind == GOTHIC_OPEN_WINDOW ? &win : (const GothicOpening *)0,
                    0.0f, h_split, WF_NO_BOTTOM | ef, 0.0f, -1.0f, xc, 0.0f);
    }
    if (p->aisles) {
        float fz = 0.5f * (0.5f * p->nave_w + (0.5f * p->nave_w + p->aisle_w));
        place_piece(b, p->aisle_w, p->aisle_h - p->plinth_h, wt,
                    (const GothicOpening *)0, 0.0f, p->plinth_h,
                    WF_NO_BOTTOM | WF_NO_ENDS, 0.0f, -1.0f, xc, -fz);
        place_piece(b, p->aisle_w, p->aisle_h - p->plinth_h, wt,
                    (const GothicOpening *)0, 0.0f, p->plinth_h,
                    WF_NO_BOTTOM | WF_NO_ENDS, 0.0f, -1.0f, xc,  fz);
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
        plan_opening(p, WALL_EAST, 0, &win);
        place_piece(b, p->nave_w, p->wall_h - p->plinth_h, wt,
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
        if (cw > 0.3f) {
            place_piece(b, cw, p->aisle_h - p->plinth_h, wt,
                        (const GothicOpening *)0, 0.0f, p->plinth_h,
                        WF_NO_BOTTOM | WF_NO_ENDS, 0.0f, 1.0f, xc, -fz);
            place_piece(b, cw, p->aisle_h - p->plinth_h, wt,
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
            place_piece(b, len, p->wall_h, wt,
                        win.kind == GOTHIC_OPEN_WINDOW ? &win : (const GothicOpening *)0,
                        0.0f, 0.0f, WF_NO_ENDS,
                        nz, nx, mx + nx * 0.5f * wt, mz + nz * 0.5f * wt);
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
        for (i = 0; i < p->nbays; i++) {
            float x0 = p->west_x + p->tower_d + (float)i * p->bay_l;
            /* end bays die INTO the walls (engaged responds — no
               freestanding pier collides with the wall footings);
               an apse mouth keeps its own radial piers instead */
            float rap = 0.4f > 0.7f * wt ? 0.4f : 0.7f * wt;
            float xs = x0 + ((i == 0 && !p->tower) ? 0.0f : pfh);
            float xe = x0 + p->bay_l -
                       (i == p->nbays - 1
                            ? (p->apse_sides ? rap * 0.9238795f : 0.0f)
                            : pfh);
            int   fl = p->style == CHURCH_BASILICA ? WF_NO_TOP : 0;
            place_arch(b, xe - xs, top, wt, p->impost_h,
                       p->acute, 0.0f, fl, yawc, 0.0f,
                       0.5f * (xs + xe), zc);
        }
        for (i = 1; i < p->nbays; i++) {     /* spandrels over the piers */
            float x = p->west_x + p->tower_d + (float)i * p->bay_l;
            int   fl = WF_NO_BOTTOM | WF_NO_ENDS |
                       (p->style == CHURCH_BASILICA ? WF_NO_TOP : 0);
            place_piece(b, 2.0f * pfh, top - p->impost_h, wt,
                        (const GothicOpening *)0, 0.0f, p->impost_h, fl,
                        yawc, 0.0f, x, zc);
        }
        if (p->style == CHURCH_BASILICA) {   /* the clerestory band */
            for (i = 0; i < p->nbays; i++) {
                GothicOpening o;
                float x0 = p->west_x + p->tower_d + (float)i * p->bay_l;
                float xc = x0 + 0.5f * p->bay_l;
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
        /* south half walks -z first; the north half mirrors */
        float hs = (float)half;
        int   n  = 0;
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
            float x, z;
            if (plan_pier(p, i, PIER_ROW_S_ARCADE, &x, &z))
                pier(b, p, x, z, rp, 0.0f, 1, 1.0f, 0.0f);
            if (plan_pier(p, i, PIER_ROW_N_ARCADE, &x, &z))
                pier(b, p, x, z, rp, 0.0f, 1, 1.0f, 0.0f);
        }
    if (p->apse_sides == 5) {
        float rap = 0.4f > 0.7f * wt ? 0.4f : 0.7f * wt;
        for (i = 0; i < 6; i++) {
            float x, z, ang;
            plan_apse_pier(p, i, &x, &z);
            ang = (-112.5f + 45.0f * (float)i) * (SOL_PI / 180.0f);
            pier(b, p, x, z, rap, p->wall_h + 0.25f, 0,
                 cosf(ang), sinf(ang));
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
            buttress(b, bh, bw, bd, st, 1.0f, 0.0f,
                     x, -(hwid + wt));   /* south: the body already
                                            extends -z; back on the face */
            buttress(b, bh, bw, bd, st, -1.0f, 0.0f,
                     x, hwid + wt);      /* north: flipped 180 */
        }
        if (p->aisles) {
            buttress(b, p->wall_h, bw, bd, st, 0.0f, 1.0f,
                     p->west_x - wt, -0.5f * p->nave_w);
            buttress(b, p->wall_h, bw, bd, st, 0.0f, 1.0f,
                     p->west_x - wt, 0.5f * p->nave_w);
            if (p->apse_sides == 0) {
                buttress(b, p->wall_h, bw, bd, st, 0.0f, -1.0f,
                         p->east_x + wt, -0.5f * p->nave_w);
                buttress(b, p->wall_h, bw, bd, st, 0.0f, -1.0f,
                         p->east_x + wt, 0.5f * p->nave_w);
            }
        }
    }

    stone_vaults(b, p);             /* part 2: the crown of the system */
    stone_course(b, p);

    {   /* parapets: aisle walls full length, facade and flat east
           trimmed between them, the clerestory band's own */
        float pw = 0.5f * wt;
        float zs = hwid + 0.5f * wt;
        parapet_strip(b, p->west_x - wt, p->east_x + wt,
                      -zs - 0.5f * pw, -zs + 0.5f * pw,
                      p->aisle_h, p->aisle_h + p->parapet_h, 1, 0);
        parapet_strip(b, p->west_x - wt, p->east_x + wt,
                      zs - 0.5f * pw, zs + 0.5f * pw,
                      p->aisle_h, p->aisle_h + p->parapet_h, 1, 0);
        parapet_strip(b, p->west_x - wt, p->west_x - wt + pw,
                      -(zs - 0.5f * pw), zs - 0.5f * pw,
                      p->wall_h, p->wall_h + p->parapet_h, 0, 1);
        if (p->apse_sides == 0)
            parapet_strip(b, p->east_x + wt - pw, p->east_x + wt,
                          -(zs - 0.5f * pw), zs - 0.5f * pw,
                          p->wall_h, p->wall_h + p->parapet_h, 0, 1);
        if (p->style == CHURCH_BASILICA) {
            float x0 = p->west_x + p->tower_d, x1 = p->east_x;
            parapet_strip(b, x0, x1, -0.5f * p->nave_w - 0.5f * pw,
                          -0.5f * p->nave_w + 0.5f * pw,
                          p->wall_h, p->wall_h + p->parapet_h, 1, 0);
            parapet_strip(b, x0, x1, 0.5f * p->nave_w - 0.5f * pw,
                          0.5f * p->nave_w + 0.5f * pw,
                          p->wall_h, p->wall_h + p->parapet_h, 1, 0);
        }
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
                   float s, float dome, float lift) {
    float t = (float)k / (float)(n - 1);
    vec3  p = vlerp(ra[k], rb[k], s);
    p.y += dome * sinf(SOL_PI * t) * sinf(SOL_PI * s) + lift;
    return p;
}

static void web_quad(MeshBuilder *b, vec3 p00, vec3 p10, vec3 p11, vec3 p01,
                     vec3 ref) {
    /* oriented toward ref (the room below for the intrados, the sky
       for the extrados). The boss row's rules CONVERGE: detect the
       collapsed pair FIRST, before any orientation swap relocates it,
       and emit the triangle of the three distinct corners. */
    vec3    e = vec3_sub(p11, p10);
    vec3    nm, cen;
    float   l;
    sol_u32 i0, i1, i2, i3;
    int     tri = vec3_dot(e, e) < 1e-6f;

    nm  = vec3_cross(vec3_sub(p10, p00), vec3_sub(p01, p00));
    l   = sqrtf(vec3_dot(nm, nm));
    if (l < 1e-10f) return;
    nm  = vec3_scale(nm, 1.0f / l);
    cen = vec3_scale(vec3_add(vec3_add(p00, p10), vec3_add(p11, p01)), 0.25f);
    if (vec3_dot(nm, vec3_sub(ref, cen)) < 0.0f) {
        vec3 tswap = p10; p10 = p01; p01 = tswap;
        nm = vec3_scale(nm, -1.0f);
        tri = tri ? 2 : 0;            /* the collapsed pair moved */
    } else if (tri) {
        tri = 1;
    }
    i0 = mb_push_vertex(b, p00.x, p00.y, p00.z, nm.x, nm.y, nm.z, 0, 0);
    i1 = mb_push_vertex(b, p10.x, p10.y, p10.z, nm.x, nm.y, nm.z, 1, 0);
    if (tri == 1) {                   /* unswapped: (p00, p10, p01) */
        i3 = mb_push_vertex(b, p01.x, p01.y, p01.z, nm.x, nm.y, nm.z, 0, 1);
        mb_push_triangle(b, i0, i1, i3);
        return;
    }
    if (tri == 2) {                   /* swapped: pair sits at (p11,p01) */
        i2 = mb_push_vertex(b, p11.x, p11.y, p11.z, nm.x, nm.y, nm.z, 1, 1);
        mb_push_triangle(b, i0, i1, i2);
        return;
    }
    i2 = mb_push_vertex(b, p11.x, p11.y, p11.z, nm.x, nm.y, nm.z, 1, 1);
    i3 = mb_push_vertex(b, p01.x, p01.y, p01.z, nm.x, nm.y, nm.z, 0, 1);
    mb_push_triangle(b, i0, i1, i2);
    mb_push_triangle(b, i0, i2, i3);
}

static void web_cell(MeshBuilder *b, const vec3 *ra, const vec3 *rb,
                     int n, float dome) {
    vec3  chord = vec3_sub(rb[0], ra[0]);
    float clen  = sqrtf(vec3_dot(chord, chord));
    vec3  boss  = ra[n - 1];
    vec3  ref_lo, ref_hi;
    int   m, k, j, side;
    if (n < 2 || clen < 0.2f) return;
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
                         web_pt(ra, rb, n, k,     s0, dome, lift),
                         web_pt(ra, rb, n, k + 1, s0, dome, lift),
                         web_pt(ra, rb, n, k + 1, s1, dome, lift),
                         web_pt(ra, rb, n, k,     s1, dome, lift),
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
                         float spring, float rib_sc, int formerets) {
    vec3  d1[GOTHIC_ARCH_MAX_PTS], d2[GOTHIC_ARCH_MAX_PTS];
    vec3  h1r[GOTHIC_ARCH_MAX_PTS], h2r[GOTHIC_ARCH_MAX_PTS];
    vec3  wr[GOTHIC_ARCH_MAX_PTS];
    float w = z1 - z0, l = p->bay_l;
    float hgt = 0.5f * sqrtf(w * w + l * l);
    float a_t = gothic_arch_acuteness_for(w, hgt);
    float a_l = gothic_arch_acuteness_for(l, hgt);
    float dome = 0.08f * hgt;
    int   i, k, n;

    for (i = 0; i < nbays; i++) {
        float xa = x_w + (float)i * l, xb = xa + l;
        int   nh;
        n = vault_path(d1, xa, z0, xb, z1, spring, 0.0f);
        if (n < 3) continue;
        vault_path(d2, xb, z0, xa, z1, spring, 0.0f);
        nh = (n - 1) / 2;
        vault_rib(b, d1, n, rib_sc);
        vault_rib(b, d2, n, rib_sc);
        boss_knob(b, d1[nh], 0.16f + 0.05f * rib_sc);
        for (k = 0; k <= nh; k++) {            /* reversed halves */
            h1r[k] = d1[n - 1 - k];            /* from (xa, z1)   */
            h2r[k] = d2[n - 1 - k];            /* from (xb, z1)   */
        }
        web_cell(b, d1, d2, nh + 1, dome);     /* S: (xa,z0)+(xb,z0) */
        web_cell(b, h1r, h2r, nh + 1, dome);   /* N — wait: h1r from
              (xb,z1), h2r from (xa,z1): both rise to the boss */
        web_cell(b, d1, h2r, nh + 1, dome);    /* W: (xa,z0)+(xa,z1) */
        web_cell(b, d2, h1r, nh + 1, dome);    /* E: (xb,z0)+(xb,z1) */
        if (formerets) {   /* wall ribs, slimmer, half-buried — the
                              aisles skip theirs (budget; barely seen) */
            n = vault_path(wr, xa, z0, xb, z0, spring, a_l);
            vault_rib(b, wr, n, rib_sc * 0.65f);
            n = vault_path(wr, xa, z1, xb, z1, spring, a_l);
            vault_rib(b, wr, n, rib_sc * 0.65f);
        }
    }
    /* transverse arches at the stations (shared between bays) */
    for (i = 0; i <= nbays; i++) {
        float x = x_w + (float)i * l;
        n = vault_path(wr, x, z0, x, z1, spring, a_t);
        vault_rib(b, wr, n, rib_sc * 1.1f);
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
                 spring_n, 1.0f, 1);
    if (p->aisles) {
        vault_vessel(b, p, x_w, p->nbays, -hwid, -0.5f * p->nave_w,
                     p->impost_h, 0.7f, 0);
        vault_vessel(b, p, x_w, p->nbays, 0.5f * p->nave_w, hwid,
                     p->impost_h, 0.7f, 0);
    }

    if (p->apse_sides == 5) {       /* the radial half-vault */
        vec3  rr[6][65];   /* half paths: (GOTHIC_ARCH_MAX_PTS-1)/2+1 */
        int   rn = 0, k;
        float r   = 0.5411961f * p->nave_w;
        float cxa = p->east_x + 0.3826834f * r;
        for (k = 0; k < 6; k++) {
            vec3  full[GOTHIC_ARCH_MAX_PTS];
            float vx, vz;
            int   n, j;
            plan_apse_pier(p, k, &vx, &vz);
            n = vault_path(full, vx, vz, 2.0f * cxa - vx, -vz,
                           spring_n, 0.0f);
            if (n < 3) { rn = 0; break; }
            rn = (n - 1) / 2 + 1;
            for (j = 0; j < rn; j++) rr[k][j] = full[j];
            /* the rib stops a station short too: six sweeps converging
               at one point stack anti-parallel side faces through the
               center — everything inside the half-boss yields to it */
            vault_rib(b, rr[k], rn - 1, 0.8f);
        }
        if (rn >= 3) {
            /* the radial cells stop one station short of the center —
               five shallow fans converging at one point congest into
               near-coplanar slivers; the half-boss is FOR this */
            for (k = 0; k < 5; k++)
                web_cell(b, rr[k], rr[k + 1], rn - 1, 0.05f * r);
            boss_knob(b, rr[0][rn - 1], 0.62f);
        }
    }

    /* responds: the bundle's landing shafts (item 4's deferral cashed) */
    if (p->aisles) {
        for (i = p->tower ? 0 : 1; i < p->nbays; i++) {
            float x, z;
            if (plan_pier(p, i, PIER_ROW_S_ARCADE, &x, &z))
                respond(b, x, z, p->impost_h, spring_n,
                        0.4f * rp / 0.12f);
            if (plan_pier(p, i, PIER_ROW_N_ARCADE, &x, &z))
                respond(b, x, z, p->impost_h, spring_n,
                        0.4f * rp / 0.12f);
        }
        for (i = 1; i < p->nbays; i++) {       /* aisle wall responds */
            float x = x_w + (float)i * p->bay_l;
            respond(b, x, -hwid, p->plinth_h, p->impost_h, 0.33f * rp / 0.12f);
            respond(b, x,  hwid, p->plinth_h, p->impost_h, 0.33f * rp / 0.12f);
        }
    } else {
        for (i = 1; i < p->nbays; i++) {       /* the chapel's wall shafts */
            float x = x_w + (float)i * p->bay_l;
            respond(b, x, -0.5f * p->nave_w, p->plinth_h, spring_n,
                    0.33f * rp / 0.12f);
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
                flyer(b, x, z_in, z_out, y_high, y_low);
            }
        }
    }
}
