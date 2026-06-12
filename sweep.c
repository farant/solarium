/* sweep.c — the profile lathe (34th TU): the gothic kit's sweep, moved
   whole (P7 item 1) and grown ONE capability — taper. The move is
   provable: every gothic suite output is byte-identical through it (the
   pier-refactor precedent at TU scale). The taper obeys the equivalence
   law: scales == NULL runs the exact untapered arithmetic.

   THE CONE CORRECTION: a cylinder's surface normal is radial; a
   tapering tube's leans along the path by the taper slope. For surface
   P(s,q) = C(s) + sc(s)*V(q) the normal is n ~ m - sc'*(V.m)*t, where
   m is the section normal and (V.m) the profile point's support
   distance. One term per vertex, fed through the existing crease/roll
   machinery untouched — creases keep their semantics under taper. */

#include "sweep.h"
#include "sol_math.h"

#include <math.h>

int sweep_segments(float arc_len, float arc_angle, float max_seg) {
    float q;
    int   nl, na;
    if (arc_len   < 0.0f) arc_len   = -arc_len;
    if (arc_angle < 0.0f) arc_angle = -arc_angle;
    q  = arc_len / max_seg;         nl = (int)q;
    if ((float)nl + 1e-4f < q) nl++;
    q  = arc_angle / SWEEP_MAX_ANG; na = (int)q;
    if ((float)na + 1e-4f < q) na++;
    if (na > nl) nl = na;
    return nl < 1 ? 1 : nl;
}

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
    sol_u32 vin[SWEEP_MAX_PROF];
    sol_u32 vout[SWEEP_MAX_PROF];
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

/* the cone correction. lean == 0 RETURNS THE INPUT UNTOUCHED — not a
   multiply-by-zero (a -0 component would flip its bit and break the
   equivalence law's memcmp). */
static vec3 taper_normal(vec3 n, vec3 tdir, float lean, float support) {
    if (lean == 0.0f) return n;
    return vec3_normalize(vec3_sub(n, vec3_scale(tdir, lean * support)));
}

/* emit one ring of vertices at pos; o_pos is the (possibly miter-
   stretched) position axis, o_n the unit shading axis, v_arc the path
   arclength (the v texcoord). scale is the STATION's scale (taper
   included); tdir/lean carry the cone correction. Fills ids. */
static void emit_ring(MeshBuilder *b, const ProfilePt *prof, int prof_n,
                      const SectPt *sp, vec3 pos, vec3 o_pos, vec3 o_n,
                      vec3 up, float scale, vec3 tdir, float lean,
                      float v_arc, RingIds *ids) {
    int j;
    for (j = 0; j < prof_n; j++) {
        vec3  p, n;
        float uu = sp[j].al * scale;
        p = vec3_add(pos, vec3_add(vec3_scale(o_pos, prof[j].o * scale),
                                   vec3_scale(up,    prof[j].u * scale)));
        if (j == 0) {                              /* open start: one edge */
            n = sect_normal(o_n, up, sp[j].no_o, sp[j].no_u);
            n = taper_normal(n, tdir, lean,
                             prof[j].o * sp[j].no_o + prof[j].u * sp[j].no_u);
            ids->vout[j] = mb_push_vertex(b, p.x, p.y, p.z, n.x, n.y, n.z, uu, v_arc);
            ids->vin[j]  = ids->vout[j];
        } else if (j == prof_n - 1) {              /* open end: one edge */
            n = sect_normal(o_n, up, sp[j].ni_o, sp[j].ni_u);
            n = taper_normal(n, tdir, lean,
                             prof[j].o * sp[j].ni_o + prof[j].u * sp[j].ni_u);
            ids->vin[j]  = mb_push_vertex(b, p.x, p.y, p.z, n.x, n.y, n.z, uu, v_arc);
            ids->vout[j] = ids->vin[j];
        } else if (prof[j].crease) {               /* hard arris: two */
            n = sect_normal(o_n, up, sp[j].ni_o, sp[j].ni_u);
            n = taper_normal(n, tdir, lean,
                             prof[j].o * sp[j].ni_o + prof[j].u * sp[j].ni_u);
            ids->vin[j]  = mb_push_vertex(b, p.x, p.y, p.z, n.x, n.y, n.z, uu, v_arc);
            n = sect_normal(o_n, up, sp[j].no_o, sp[j].no_u);
            n = taper_normal(n, tdir, lean,
                             prof[j].o * sp[j].no_o + prof[j].u * sp[j].no_u);
            ids->vout[j] = mb_push_vertex(b, p.x, p.y, p.z, n.x, n.y, n.z, uu, v_arc);
        } else {                                   /* roll: one, averaged */
            float ao = sp[j].ni_o + sp[j].no_o;
            float au = sp[j].ni_u + sp[j].no_u;
            float l  = sqrtf(ao * ao + au * au);
            if (l > 1e-8f) { ao /= l; au /= l; }
            else { ao = sp[j].no_o; au = sp[j].no_u; }   /* 180: pick one */
            n = sect_normal(o_n, up, ao, au);
            n = taper_normal(n, tdir, lean,
                             prof[j].o * ao + prof[j].u * au);
            ids->vin[j]  = mb_push_vertex(b, p.x, p.y, p.z, n.x, n.y, n.z, uu, v_arc);
            ids->vout[j] = ids->vin[j];
        }
    }
}

/* loop-closure state: the sweep is single-threaded like the engine */
static RingIds g_loop_first;
static int     g_loop_on = 0;

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
   each pass (determinism); input is CCW (the table test asserts it).
   Bounded, allocation-free. */
static int cap_earclip(const ProfilePt *prof, int m, int *tris) {
    int idx[SWEEP_MAX_PROF];
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
    sol_u32 vid[SWEEP_MAX_PROF];
    int     tris[(SWEEP_MAX_PROF - 2) * 3];
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

/* the station's effective scale: base x the taper array (NULL = base —
   the same VALUE through the same ops as the untapered sweep) */
static float st_scale(float scale, const float *scales, int i) {
    return scales ? scale * scales[i] : scale;
}

void sweep_extrude(MeshBuilder *b, const ProfilePt *prof, int prof_n,
                   const vec3 *path, int path_n, vec3 plane_n,
                   float scale, const float *scales, int cap0, int cap1) {
    SectPt  sp[SWEEP_MAX_PROF];
    RingIds prev, cur;
    vec3    up, last_pos;
    float   arc, prev_sc;
    int     i;
    /* joints turning past 30 degrees auto-crease: two rings at the same
       miter positions, per-segment shading frames — a sharp stone arris
       instead of a smeared normal */
    static const float COS_CREASE = 0.86602540f;

    if (!b || !prof || !path) return;
    if (prof_n < 2 || prof_n > SWEEP_MAX_PROF || path_n < 2) return;
    g_loop_on = 0;                  /* never inherit a stale loop seam */

    up = vec3_normalize(plane_n);
    sect_build(prof, prof_n, sp);

    {   /* a CLOSED LOOP (first == last point) gets a mitered seam ring
           stitched back to itself — no caps, no gap. Sharp-seam loops
           fall back to the open path. */
        vec3  d0 = vec3_normalize(vec3_sub(path[1], path[0]));
        vec3  dl = vec3_sub(path[path_n - 1], path[path_n - 2]);
        vec3  dc = vec3_sub(path[path_n - 1], path[0]);
        int   closed = vec3_dot(dc, dc) < 1e-10f && path_n >= 4;
        float sc0 = st_scale(scale, scales, 0);
        float lean0 = 0.0f;
        if (scales) {               /* the start's slope: outgoing segment */
            vec3  s01 = vec3_sub(path[1], path[0]);
            float l01 = sqrtf(vec3_dot(s01, s01));
            if (l01 > 1e-7f)
                lean0 = (st_scale(scale, scales, 1) - sc0) / l01;
        }
        if (closed) {
            float c2;
            dl = vec3_normalize(dl);
            c2 = vec3_dot(dl, d0);
            if (c2 >= 0.86602540f) {
                vec3 m   = vec3_normalize(vec3_add(dl, d0));
                vec3 o_u = vec3_normalize(vec3_cross(up, m));
                vec3 o_p = vec3_scale(o_u, 1.0f / sqrtf((1.0f + c2) * 0.5f));
                emit_ring(b, prof, prof_n, sp, path[0], o_p, o_u, up,
                          sc0, m, lean0, 0.0f, &prev);
                g_loop_first = prev;
                g_loop_on    = 1;
            } else {
                closed = 0;
            }
        }
        if (!closed) {
            vec3 o0 = vec3_normalize(vec3_cross(up, d0));
            g_loop_on = 0;
            emit_ring(b, prof, prof_n, sp, path[0], o0, o0, up, sc0,
                      d0, lean0, 0.0f, &prev);
            if (cap0)
                cap_emit(b, prof, prof_n, path[0], o0, up, sc0,
                         vec3_scale(d0, -1.0f), 1);
        }
        prev_sc = sc0;
    }
    last_pos = path[0];
    arc = 0.0f;

    for (i = 1; i < path_n; i++) {
        vec3  seg = vec3_sub(path[i], last_pos);
        float slen = sqrtf(vec3_dot(seg, seg));
        vec3  d_s;
        float sc_i, lean;
        if (slen <= 1e-7f) continue;               /* doubled point: drop */
        d_s = vec3_scale(seg, 1.0f / slen);
        arc += slen;
        last_pos = path[i];
        sc_i = st_scale(scale, scales, i);
        lean = scales ? (sc_i - prev_sc) / slen : 0.0f;

        if (i == path_n - 1) {                     /* the end ring */
            if (g_loop_on) {                       /* close the loop */
                stitch(b, &prev, &g_loop_first, prof_n);
                g_loop_on = 0;
                continue;
            }
            {
                vec3 oe = vec3_normalize(vec3_cross(up, d_s));
                emit_ring(b, prof, prof_n, sp, path[i], oe, oe, up, sc_i,
                          d_s, lean, arc, &cur);
                stitch(b, &prev, &cur, prof_n);
                if (cap1)
                    cap_emit(b, prof, prof_n, path[i], oe, up, sc_i, d_s, 0);
            }
        } else {                                   /* an interior joint */
            vec3  d_n = vec3_normalize(vec3_sub(path[i + 1], path[i]));
            float c   = vec3_dot(d_s, d_n);
            vec3  m, o_u, o_p;
            if (c < -0.999f) return;               /* doubled back: refuse */
            m   = vec3_normalize(vec3_add(d_s, d_n));
            o_u = vec3_normalize(vec3_cross(up, m));
            o_p = vec3_scale(o_u, 1.0f / sqrtf((1.0f + c) * 0.5f));
            if (c >= COS_CREASE) {                 /* gentle: one ring */
                emit_ring(b, prof, prof_n, sp, path[i], o_p, o_u, up, sc_i,
                          m, lean, arc, &cur);
                stitch(b, &prev, &cur, prof_n);
                prev = cur;
            } else {                               /* sharp: crease it */
                vec3 oa = vec3_normalize(vec3_cross(up, d_s));
                vec3 ob = vec3_normalize(vec3_cross(up, d_n));
                emit_ring(b, prof, prof_n, sp, path[i], o_p, oa, up, sc_i,
                          d_s, lean, arc, &cur);
                stitch(b, &prev, &cur, prof_n);
                emit_ring(b, prof, prof_n, sp, path[i], o_p, ob, up, sc_i,
                          d_n, lean, arc, &cur);
                prev = cur;
            }
        }
        prev_sc = sc_i;
    }
}
