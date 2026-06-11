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
    float q;
    int   nl, na;
    if (arc_len   < 0.0f) arc_len   = -arc_len;
    if (arc_angle < 0.0f) arc_angle = -arc_angle;
    /* ceil with a dust epsilon: an arc that is EXACTLY k segments must
       not flip to k+1 on the last bit of a float division */
    q  = arc_len / GOTHIC_MAX_SEG;   nl = (int)q;
    if ((float)nl + 1e-4f < q) nl++;
    q  = arc_angle / GOTHIC_MAX_ANG; na = (int)q;
    if ((float)na + 1e-4f < q) na++;
    if (na > nl) nl = na;
    return nl < 1 ? 1 : nl;
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
