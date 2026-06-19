/* mesh.c — CPU mesh builder + primitive emitters. Above the seam: talks only
   to rhi.h for upload, never to GL. */

#include "mesh.h"
#include "gothic.h"     /* the kit's emitters — registry rows compose them */
#include "flora.h"      /* the trees (P7 item 3): species refs over tree_plan */
#include "rock.h"       /* boulders (P7 item 6): the island's own stone */
#include "sol_math.h"   /* SOL_PI + sol_smoothstep (the codex emitters) */

#include <stdlib.h>
#include <string.h>
#include <math.h>

void mb_init(MeshBuilder *b) {
    b->vertices = NULL; b->vertex_count = 0; b->vertex_cap = 0;
    b->indices  = NULL; b->index_count  = 0; b->index_cap  = 0;
}

void mb_free(MeshBuilder *b) {
    free(b->vertices); b->vertices = NULL;
    free(b->indices);  b->indices  = NULL;
}

sol_u32 mb_push_vertex(MeshBuilder *b, sol_f32 px, sol_f32 py, sol_f32 pz,
                       sol_f32 nx, sol_f32 ny, sol_f32 nz, sol_f32 u, sol_f32 v) {
    sol_u32 base;
    if (b->vertex_count == b->vertex_cap) {
        b->vertex_cap = b->vertex_cap ? b->vertex_cap * 2 : 64;
        b->vertices = realloc(b->vertices, (size_t)b->vertex_cap * 12 * sizeof(sol_f32));
    }
    base = b->vertex_count * 12;
    b->vertices[base+0] = px; b->vertices[base+1] = py; b->vertices[base+2] = pz;
    b->vertices[base+3] = nx; b->vertices[base+4] = ny; b->vertices[base+5] = nz;
    b->vertices[base+6] = u;  b->vertices[base+7] = v;
    b->vertices[base+8] = 0.0f; b->vertices[base+9]  = 0.0f;    /* tangent: filled later */
    b->vertices[base+10]= 0.0f; b->vertices[base+11] = 0.0f;    /* by mb_compute_tangents */
    return b->vertex_count++;                 /* index of the vertex just added */
}

void mb_push_triangle(MeshBuilder *b, sol_u32 a, sol_u32 i, sol_u32 c) {
    if (b->index_count + 3 > b->index_cap) {
        sol_u32 cap = b->index_cap ? b->index_cap * 2 : 64;
        while (b->index_count + 3 > cap) cap *= 2;
        b->indices = realloc(b->indices, (size_t)cap * sizeof(sol_u32));
        b->index_cap = cap;
    }
    b->indices[b->index_count++] = a;
    b->indices[b->index_count++] = i;
    b->indices[b->index_count++] = c;
}

/* One flat quad face: 4 corners (a loop around the face) sharing one normal,
   with a 0->1 planar UV. Two triangles. Per-face vertices are what make the
   box flat-shaded (24 verts total, not 8). */
static void push_quad(MeshBuilder *b, const sol_f32 *p0, const sol_f32 *p1,
                      const sol_f32 *p2, const sol_f32 *p3,
                      sol_f32 nx, sol_f32 ny, sol_f32 nz) {
    sol_u32 i0 = mb_push_vertex(b, p0[0],p0[1],p0[2], nx,ny,nz, 0.0f, 0.0f);
    sol_u32 i1 = mb_push_vertex(b, p1[0],p1[1],p1[2], nx,ny,nz, 1.0f, 0.0f);
    sol_u32 i2 = mb_push_vertex(b, p2[0],p2[1],p2[2], nx,ny,nz, 1.0f, 1.0f);
    sol_u32 i3 = mb_push_vertex(b, p3[0],p3[1],p3[2], nx,ny,nz, 0.0f, 1.0f);
    mb_push_triangle(b, i0, i1, i2);
    mb_push_triangle(b, i0, i2, i3);
}

void make_box(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 d) {
    sol_f32 hx = w * 0.5f, hy = h * 0.5f, hz = d * 0.5f;
    sol_f32 c[8][3];      /* 8 corners, bit-indexed: bit0=x, bit1=y, bit2=z */
    int k;
    for (k = 0; k < 8; k++) {
        c[k][0] = (k & 1) ? hx : -hx;
        c[k][1] = (k & 2) ? hy : -hy;
        c[k][2] = (k & 4) ? hz : -hz;
    }
    /* 6 faces; each shares one outward normal -> per-face vertices (flat shading) */
    push_quad(b, c[4], c[5], c[7], c[6],  0.0f, 0.0f,  1.0f);   /* +Z */
    push_quad(b, c[1], c[0], c[2], c[3],  0.0f, 0.0f, -1.0f);   /* -Z */
    push_quad(b, c[1], c[3], c[7], c[5],  1.0f, 0.0f,  0.0f);   /* +X */
    push_quad(b, c[0], c[4], c[6], c[2], -1.0f, 0.0f,  0.0f);   /* -X */
    push_quad(b, c[2], c[6], c[7], c[3],  0.0f, 1.0f,  0.0f);   /* +Y */
    push_quad(b, c[0], c[1], c[5], c[4],  0.0f,-1.0f,  0.0f);   /* -Y */
}

void make_grid(MeshBuilder *b, sol_f32 w, sol_f32 d, sol_u32 subdiv) {
    sol_u32 stride = subdiv + 1;              /* fencepost: N segments, N+1 verts */
    sol_u32 base   = b->vertex_count;         /* so it composes onto existing geometry */
    sol_u32 row, col;
    sol_f32 inv = 1.0f / (sol_f32)subdiv;     /* float division — int would truncate to 0 */

    /* (subdiv+1)^2 vertices, shared across cells (one up-normal, no seams) */
    for (row = 0; row <= subdiv; row++) {
        for (col = 0; col <= subdiv; col++) {
            sol_f32 u = (sol_f32)col * inv;
            sol_f32 v = (sol_f32)row * inv;
            mb_push_vertex(b, (u - 0.5f) * w, 0.0f, (v - 0.5f) * d,   /* XZ plane, y=0 */
                           0.0f, 1.0f, 0.0f,                          /* normal: up */
                           u, v);                                     /* planar UV */
        }
    }
    /* two triangles per cell, CCW seen from above (+Y) */
    for (row = 0; row < subdiv; row++) {
        for (col = 0; col < subdiv; col++) {
            sol_u32 bl = base + row * stride + col;
            sol_u32 br = bl + 1;
            sol_u32 tl = bl + stride;
            sol_u32 tr = tl + 1;
            mb_push_triangle(b, bl, tr, br);
            mb_push_triangle(b, bl, tl, tr);
        }
    }
}

void make_plane(MeshBuilder *b, sol_f32 w, sol_f32 d) {
    make_grid(b, w, d, 1);   /* the degenerate grid: a single quad */
}

/* A page: a w x h quad standing in the XY plane, facing +Z, with upright UVs
   (uv (0,0) at bottom-left). Built directly rather than make_plane+rotation,
   which would invert V relative to world-up (a flat XZ quad stood up via
   +90 deg about X sends v to -Y). Extracted from main.c in P3 item 1. */
void make_page(MeshBuilder *b, sol_f32 w, sol_f32 h) {
    sol_f32 hw = w * 0.5f, hh = h * 0.5f;
    sol_u32 v0 = mb_push_vertex(b, -hw, -hh, 0.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f);  /* BL */
    sol_u32 v1 = mb_push_vertex(b,  hw, -hh, 0.0f,  0.0f, 0.0f, 1.0f,  1.0f, 0.0f);  /* BR */
    sol_u32 v2 = mb_push_vertex(b,  hw,  hh, 0.0f,  0.0f, 0.0f, 1.0f,  1.0f, 1.0f);  /* TR */
    sol_u32 v3 = mb_push_vertex(b, -hw,  hh, 0.0f,  0.0f, 0.0f, 1.0f,  0.0f, 1.0f);  /* TL */
    mb_push_triangle(b, v0, v1, v2);
    mb_push_triangle(b, v0, v2, v3);
}

/* One quad from 4 corners given in CCW order AS SEEN FROM the normal's side,
   each with its own UV. The shared worker for the architectural emitters. */
static void push_quad4(MeshBuilder *b,
                       sol_f32 x0, sol_f32 y0, sol_f32 z0, sol_f32 u0, sol_f32 v0,
                       sol_f32 x1, sol_f32 y1, sol_f32 z1, sol_f32 u1, sol_f32 v1,
                       sol_f32 x2, sol_f32 y2, sol_f32 z2, sol_f32 u2, sol_f32 v2,
                       sol_f32 x3, sol_f32 y3, sol_f32 z3, sol_f32 u3, sol_f32 v3,
                       sol_f32 nx, sol_f32 ny, sol_f32 nz) {
    sol_u32 a = mb_push_vertex(b, x0, y0, z0, nx, ny, nz, u0, v0);
    sol_u32 c = mb_push_vertex(b, x1, y1, z1, nx, ny, nz, u1, v1);
    sol_u32 d = mb_push_vertex(b, x2, y2, z2, nx, ny, nz, u2, v2);
    sol_u32 e = mb_push_vertex(b, x3, y3, z3, nx, ny, nz, u3, v3);
    mb_push_triangle(b, a, c, d);
    mb_push_triangle(b, a, d, e);
}


/* Axis-aligned face helpers for the wall pieces: a rect facing +/-Z, +/-X,
   or +/-Y, corners ordered CCW from the normal's side, world-scale UVs. */
static void face_z(MeshBuilder *b, sol_f32 x0, sol_f32 x1, sol_f32 y0, sol_f32 y1,
                   sol_f32 z, int dir) {
    if (dir > 0)
        push_quad4(b, x0,y0,z, x0,y0,  x1,y0,z, x1,y0,  x1,y1,z, x1,y1,  x0,y1,z, x0,y1,
                   0.0f, 0.0f, 1.0f);
    else
        push_quad4(b, x1,y0,z, x1,y0,  x0,y0,z, x0,y0,  x0,y1,z, x0,y1,  x1,y1,z, x1,y1,
                   0.0f, 0.0f, -1.0f);
}
static void face_x(MeshBuilder *b, sol_f32 x, sol_f32 y0, sol_f32 y1,
                   sol_f32 z0, sol_f32 z1, int dir) {
    if (dir > 0)
        push_quad4(b, x,y0,z1, z1,y0,  x,y0,z0, z0,y0,  x,y1,z0, z0,y1,  x,y1,z1, z1,y1,
                   1.0f, 0.0f, 0.0f);
    else
        push_quad4(b, x,y0,z0, z0,y0,  x,y0,z1, z1,y0,  x,y1,z1, z1,y1,  x,y1,z0, z0,y1,
                   -1.0f, 0.0f, 0.0f);
}
static void face_y(MeshBuilder *b, sol_f32 x0, sol_f32 x1, sol_f32 y,
                   sol_f32 z0, sol_f32 z1, int dir) {
    if (dir > 0)
        push_quad4(b, x0,y,z0, x0,z0,  x0,y,z1, x0,z1,  x1,y,z1, x1,z1,  x1,y,z0, x1,z0,
                   0.0f, 1.0f, 0.0f);
    else
        push_quad4(b, x0,y,z0, x0,z0,  x1,y,z0, x1,z0,  x1,y,z1, x1,z1,  x0,y,z1, x0,z1,
                   0.0f, -1.0f, 0.0f);
}

/* The doorway wall: never wall-minus-box (§1.4: mesh CSG is a robustness tar
   pit) — the pieces are BUILT AROUND the gap, the way a framed wall is built
   around a rough opening: left panel, right panel, header. Each piece is a
   REAL-THICKNESS box emitting only its EXPOSED faces — fronts/backs, outer
   ends, tops/bottoms, and the doorway's reveal (jambs + lintel underside);
   faces where pieces abut are skipped, so nothing is ever coplanar (two
   quads at the same depth z-fight: the rasterizer picks a different winner
   per pixel per frame — the shimmering-checkerboard artifact). A future
   gothic arch is this same pattern with a segmented curved head. */
void make_wall_with_opening(MeshBuilder *b, sol_f32 w, sol_f32 h,
                            sol_f32 ox, sol_f32 ow, sol_f32 oh, sol_f32 t) {
    sol_f32 left = -w * 0.5f;
    sol_f32 zf, zb, lx0, lx1, rx0, rx1;
    int     has_left, has_right, has_header;

    if (t < 0.01f) t = 0.01f;                 /* paper-thin still must not z-fight */
    if (ox < 0.0f) ox = 0.0f;                 /* clamp the opening into the wall */
    if (ox > w)    ox = w;
    if (ow > w - ox) ow = w - ox;
    if (oh > h)      oh = h;
    zf = t * 0.5f;
    zb = -zf;

    if (ow < 1e-5f || oh < 1e-5f) {           /* no opening: one solid box */
        face_z(b, left, left + w, 0.0f, h, zf,  1);
        face_z(b, left, left + w, 0.0f, h, zb, -1);
        face_x(b, left,     0.0f, h, zb, zf, -1);
        face_x(b, left + w, 0.0f, h, zb, zf,  1);
        face_y(b, left, left + w, h,    zb, zf,  1);
        face_y(b, left, left + w, 0.0f, zb, zf, -1);
        return;
    }

    has_left   = ox > 1e-5f;
    has_right  = ox + ow < w - 1e-5f;
    has_header = oh < h - 1e-5f;
    lx0 = left;          lx1 = left + ox;          /* left panel x-range  */
    rx0 = left + ox + ow; rx1 = left + w;          /* right panel x-range */

    if (has_left) {
        face_z(b, lx0, lx1, 0.0f, h, zf,  1);
        face_z(b, lx0, lx1, 0.0f, h, zb, -1);
        face_x(b, lx0, 0.0f, h, zb, zf, -1);       /* outer end */
        face_x(b, lx1, 0.0f, oh, zb, zf,  1);      /* jamb: the reveal, below the header */
        face_y(b, lx0, lx1, h,    zb, zf,  1);     /* top */
        face_y(b, lx0, lx1, 0.0f, zb, zf, -1);     /* bottom */
    }
    if (has_right) {
        face_z(b, rx0, rx1, 0.0f, h, zf,  1);
        face_z(b, rx0, rx1, 0.0f, h, zb, -1);
        face_x(b, rx1, 0.0f, h, zb, zf,  1);       /* outer end */
        face_x(b, rx0, 0.0f, oh, zb, zf, -1);      /* jamb */
        face_y(b, rx0, rx1, h,    zb, zf,  1);
        face_y(b, rx0, rx1, 0.0f, zb, zf, -1);
    }
    if (has_header) {
        face_z(b, lx1, rx0, oh, h, zf,  1);
        face_z(b, lx1, rx0, oh, h, zb, -1);
        face_y(b, lx1, rx0, oh, zb, zf, -1);       /* lintel underside */
        face_y(b, lx1, rx0, h,  zb, zf,  1);       /* header top */
        if (!has_left)  face_x(b, lx1, oh, h, zb, zf, -1);   /* header's exposed end */
        if (!has_right) face_x(b, rx0, oh, h, zb, zf,  1);
    }

    /* the THRESHOLD: the opening's floor across the wall's thickness. The
       rooms' own floors stop at their interiors, so without this, walking
       through a doorway crosses a wall-thick strip of void. Top face only —
       an underside at the same y=0 would z-fight it. */
    face_y(b, lx1, rx0, 0.0f, zb, zf, 1);
}

/* an axis-aligned solid box, all six faces (CCW from each normal). Cheap and
   robust: abutting boxes meet back-to-back (opposite-facing coplanar quads
   never z-fight), so doored walls can be assembled from whole boxes without
   the face-skipping bookkeeping make_wall_with_opening does by hand. */
static void aabb_box(MeshBuilder *b, sol_f32 x0, sol_f32 x1, sol_f32 y0,
                     sol_f32 y1, sol_f32 z0, sol_f32 z1) {
    face_z(b, x0, x1, y0, y1, z1,  1);
    face_z(b, x0, x1, y0, y1, z0, -1);
    face_x(b, x1, y0, y1, z0, z1,  1);
    face_x(b, x0, y0, y1, z0, z1, -1);
    face_y(b, x0, x1, y1, z0, z1,  1);
    face_y(b, x0, x1, y0, z0, z1, -1);
}

/* one thick room wall, built around its gaps. runx=1: the wall runs along X
   (N/S walls), its two faces at z=f0,f1, span [s0,s1] in x. runx=0: runs
   along Z (E/W walls), faces at x=f0,f1, span [s0,s1] in z. `ops` is the full
   room list; only entries with .wall==wall_id are used (their .center is in
   the run axis). Up to ROOM_MAX_OPENINGS_PER_WALL gaps per wall. Emits piers
   + headers; the room floor (full outer footprint) fills each doorway's
   threshold strip, so no threshold face is emitted here (it would z-fight). */
static void emit_doored_wall(MeshBuilder *b, int runx, sol_f32 f0, sol_f32 f1,
                             sol_f32 s0, sol_f32 s1, sol_f32 h,
                             const RoomOpening *ops, int n_ops, int wall_id) {
    sol_f32 lo[ROOM_MAX_OPENINGS_PER_WALL];
    sol_f32 hi[ROOM_MAX_OPENINGS_PER_WALL];
    sol_f32 oy[ROOM_MAX_OPENINGS_PER_WALL];
    int     k = 0, i, j;
    sol_f32 cur;
    for (i = 0; i < n_ops; i++) {
        sol_f32 c, hwid;
        if (ops[i].wall != wall_id) continue;
        if (k >= ROOM_MAX_OPENINGS_PER_WALL) break;
        c = ops[i].center; hwid = ops[i].width * 0.5f;
        lo[k] = c - hwid; hi[k] = c + hwid; oy[k] = ops[i].height;
        k++;
    }
    for (i = 1; i < k; i++) {                 /* insertion sort by lo */
        sol_f32 pivot_lo = lo[i], pivot_hi = hi[i], pivot_oy = oy[i];
        j = i - 1;
        while (j >= 0 && lo[j] > pivot_lo) {
            lo[j + 1] = lo[j]; hi[j + 1] = hi[j]; oy[j + 1] = oy[j]; j--;
        }
        lo[j + 1] = pivot_lo; hi[j + 1] = pivot_hi; oy[j + 1] = pivot_oy;
    }
    cur = s0;
    for (i = 0; i <= k; i++) {
        sol_f32 gL = (i < k) ? lo[i] : s1;
        sol_f32 gR = (i < k) ? hi[i] : s1;
        if (gL < s0) gL = s0;
        if (gR > s1) gR = s1;
        if (gL > cur) {                        /* solid pier [cur, gL] */
            if (runx) aabb_box(b, cur, gL, 0.0f, h, f0, f1);
            else      aabb_box(b, f0, f1, 0.0f, h, cur, gL);
        }
        if (i < k) {
            if (oy[i] < h) {                   /* header above the gap */
                if (runx) aabb_box(b, gL, gR, oy[i], h, f0, f1);
                else      aabb_box(b, f0, f1, oy[i], h, gL, gR);
            }
            cur = gR;
        }
    }
}

void make_room_doored(MeshBuilder *b, sol_f32 w, sol_f32 d, sol_f32 h, sol_f32 t,
                      int wn, int we, int ws, int ww, int ceil,
                      const RoomOpening *ops, int n_ops) {
    sol_f32 hw = w * 0.5f, hd = d * 0.5f;
    if (t < 0.02f) t = 0.02f;
    aabb_box(b, -hw - t, hw + t, -t, 0.0f, -hd - t, hd + t);   /* full outer footprint: no corner toe-notch, fills every doorway strip */
    if (ceil) aabb_box(b, -hw, hw, h, h + t, -hd, hd);
    if (wn) emit_doored_wall(b, 1, -hd - t, -hd, -hw, hw, h, ops, n_ops, ROOM_WALL_N);
    if (ws) emit_doored_wall(b, 1,  hd, hd + t, -hw, hw, h, ops, n_ops, ROOM_WALL_S);
    if (we) emit_doored_wall(b, 0,  hw, hw + t, -hd - t, hd + t, h, ops, n_ops, ROOM_WALL_E);
    if (ww) emit_doored_wall(b, 0, -hw - t, -hw, -hd - t, hd + t, h, ops, n_ops, ROOM_WALL_W);
}

/* An index card (P3 item 6): a small upright slab standing on its bottom
   edge — a FILE/ALIAS/NOTE's body in a room. Faces +/-Z, x centered, y in
   [0,h], thickness t (a future size cue: fat manuscript, thin note).
   Outward normals; built from the same exposed-face helpers as the wall. */
void make_card(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 t) {
    sol_f32 hw = w * 0.5f, ht = t * 0.5f;
    face_z(b, -hw, hw, 0.0f, h,  ht,  1);     /* front / back */
    face_z(b, -hw, hw, 0.0f, h, -ht, -1);
    face_x(b, -hw, 0.0f, h, -ht, ht, -1);     /* edges */
    face_x(b,  hw, 0.0f, h, -ht, ht,  1);
    face_y(b, -hw, hw, h,    -ht, ht,  1);    /* top */
    face_y(b, -hw, hw, 0.0f, -ht, ht, -1);    /* bottom */
}

/* A walkable slab — the second EMBODIMENT of a room-graph edge (a path
   between distant rooms; the shared doorway wall is the first). Length runs
   along X, width along Z, the walking surface at y=0 (the slab hangs below,
   y in [-t, 0]) so it places flush with room floors. Outward normals — it
   is seen from outside, unlike a room. */
void make_path(MeshBuilder *b, sol_f32 len, sol_f32 w, sol_f32 t) {
    sol_f32 hl = len * 0.5f, hw = w * 0.5f;
    face_y(b, -hl, hl, 0.0f, -hw, hw,  1);    /* deck */
    face_y(b, -hl, hl, -t,   -hw, hw, -1);    /* underside */
    face_z(b, -hl, hl, -t, 0.0f,  hw,  1);    /* sides */
    face_z(b, -hl, hl, -t, 0.0f, -hw, -1);
    face_x(b, -hl, -t, 0.0f, -hw, hw, -1);    /* ends */
    face_x(b,  hl, -t, 0.0f, -hw, hw,  1);
}

void make_walkway(MeshBuilder *b, sol_f32 len, sol_f32 w, sol_f32 t, sol_f32 dy) {
    sol_f32 hl = len * 0.5f, hw = w * 0.5f;
    sol_f32 ady = (dy < 0.0f) ? -dy : dy;
    sol_f32 tread, rise;
    int     n, i;
    n = (ady < 0.02f) ? 1 : (int)(ady / WALKWAY_STEP_RISE) + 1;
    if (n < 1)   n = 1;
    if (n > 128) n = 128;
    tread = len / (sol_f32)n;
    rise  = dy  / (sol_f32)n;
    for (i = 0; i < n; i++) {                       /* each step is a full box */
        sol_f32 x0 = -hl + (sol_f32)i * tread;
        sol_f32 x1 = x0 + tread;
        sol_f32 yd = (sol_f32)(i + 1) * rise;       /* this step's deck height */
        face_y(b, x0, x1, yd,  -hw, hw,  1);        /* deck (top) */
        face_y(b, x0, x1, -t,  -hw, hw, -1);        /* underside */
        face_z(b, x0, x1, -t, yd,  hw,  1);         /* +z side */
        face_z(b, x0, x1, -t, yd, -hw, -1);         /* -z side */
        face_x(b, x1, -t, yd, -hw, hw,  1);         /* +x riser/end */
        face_x(b, x0, -t, yd, -hw, hw, -1);         /* -x riser/end */
    }
}

/* one axis-aligned stepped ribbon from (x0,z0,y0) to (x1,z1,y1). The run is
   along whichever of x/z changes; the other holds. Steps climb to the end y
   in WALKWAY_STEP_RISE increments (one box per step). The caller must ensure
   only one of |dx|,|dz| is significant per leg (axis-aligned); a diagonal leg
   would produce a wrong box. */
static void walkway_leg(MeshBuilder *b, sol_f32 x0, sol_f32 z0, sol_f32 y0,
                        sol_f32 x1, sol_f32 z1, sol_f32 y1, sol_f32 w, sol_f32 t) {
    sol_f32 dx = x1 - x0, dz = z1 - z0, dy = y1 - y0;
    sol_f32 hw = w * 0.5f, ady = (dy < 0.0f) ? -dy : dy;
    int     n, i, run_x;
    if (dx * dx + dz * dz < 1e-8f) return;
    run_x = ((dx < 0.0f ? -dx : dx) >= (dz < 0.0f ? -dz : dz));
    n = (ady < 0.02f) ? 1 : (int)(ady / WALKWAY_STEP_RISE) + 1;
    if (n < 1) n = 1;
    if (n > 128) n = 128;
    for (i = 0; i < n; i++) {
        sol_f32 a0 = (sol_f32)i / (sol_f32)n, a1 = (sol_f32)(i + 1) / (sol_f32)n;
        sol_f32 sx = x0 + dx * a0, sz = z0 + dz * a0;
        sol_f32 tx = x0 + dx * a1, tz = z0 + dz * a1;
        sol_f32 yd = y0 + dy * a1;            /* this step's deck top */
        sol_f32 bx0, bx1, bz0, bz1;
        if (run_x) {
            bx0 = (sx < tx) ? sx : tx; bx1 = (sx < tx) ? tx : sx;
            bz0 = sz - hw; bz1 = sz + hw;
        } else {
            bz0 = (sz < tz) ? sz : tz; bz1 = (sz < tz) ? tz : sz;
            bx0 = sx - hw; bx1 = sx + hw;
        }
        aabb_box(b, bx0, bx1, y0 - t, yd, bz0, bz1);
    }
}

void make_walkway_L(MeshBuilder *b, sol_f32 cx, sol_f32 cz, sol_f32 cy,
                    sol_f32 ex, sol_f32 ez, sol_f32 ey, sol_f32 w, sol_f32 t) {
    sol_f32 hw = w * 0.5f;
    sol_f32 l1sq = cx * cx + cz * cz;
    sol_f32 l2sq = (ex - cx) * (ex - cx) + (ez - cz) * (ez - cz);
    walkway_leg(b, 0.0f, 0.0f, 0.0f, cx, cz, cy, w, t);
    if (l1sq > 1e-6f && l2sq > 1e-6f)         /* landing only at a real bend */
        aabb_box(b, cx - hw, cx + hw, cy - t, cy, cz - hw, cz + hw);
    walkway_leg(b, cx, cz, cy, ex, ez, ey, w, t);
}

/* ------------------------------------------------------------ the registry */
/* THE single source of truth for what each ref name means (P3 item 1) — the
   scene built at startup and the scene loaded from disk both come through
   here, so they cannot drift. Item 5 grew it into a SCHEMA: parameter names
   feed the scene file's self-describing attributes, defaults make absent
   attributes legal, and new kit pieces (arch, column, vault...) are one entry
   + one emitter each. Pure CPU — headless-testable, linkable by scene_io. */
typedef struct {
    const char *name;
    int         param_count;
    const char *param_names[MESH_REF_MAX_PARAMS];
    float       defaults[MESH_REF_MAX_PARAMS];
    void      (*emit)(MeshBuilder *b, const float *p);
} MeshRefEntry;

static void emit_box(MeshBuilder *b, const float *p)  { (void)p; make_box(b, 1.0f, 1.0f, 1.0f); }
static void emit_grid(MeshBuilder *b, const float *p) { (void)p; make_grid(b, 6.0f, 6.0f, 8); }
static void emit_page(MeshBuilder *b, const float *p) { (void)p; make_page(b, 0.9f, 1.2f); }
static void emit_room(MeshBuilder *b, const float *p) {
    make_room_doored(b, p[0], p[1], p[2], 0.20f, /* == ROUTE_WALL_T */
                     p[3] > 0.5f, p[4] > 0.5f, p[5] > 0.5f, p[6] > 0.5f,
                     p[7] > 0.5f, (const RoomOpening *)0, 0);
}
static void emit_wall(MeshBuilder *b, const float *p) { make_wall_with_opening(b, p[0], p[1], p[2], p[3], p[4], p[5]); }
static void emit_path(MeshBuilder *b, const float *p) { make_path(b, p[0], p[1], p[2]); }
static void emit_walkway(MeshBuilder *b, const float *p) {
    make_walkway(b, p[0], p[1], p[2], p[3]);
}
/* ---- terrain (item 10): the floating plot ----
   A heightfield top over a skirt and base slab — an ISLAND, not a world:
   heights come from seeded fBm value noise, masked to a ZERO RIM at the
   plot border, so every island rises out of its own edge. The SEED is the
   island's identity (the codex-mint pattern): same seed, same hills,
   forever. terrain_height below is THE one source of truth — emitter
   vertices, their finite-difference normals, and the standing query all
   evaluate it, so geometry and physics agree by construction. */

/* integer lattice -> stable [0,1): the noise's randomness */
static sol_f32 noise_hash(int x, int z, unsigned seed) {
    unsigned h = (unsigned)x * 374761393u + (unsigned)z * 668265263u
               + seed * 2246822519u;
    h  = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (sol_f32)(h & 0xFFFFFFu) / (sol_f32)0x1000000u;
}

static sol_f32 noise_mix(sol_f32 a, sol_f32 b, sol_f32 t) {
    return a + (b - a) * t;
}

/* value noise: lattice hashes, smoothstep-blended between them */
static sol_f32 value_noise(sol_f32 x, sol_f32 z, unsigned seed) {
    int     ix = (int)x, iz = (int)z;
    sol_f32 sx, sz, a, b, c, d;
    if (x < (sol_f32)ix) ix--;                 /* portable floor */
    if (z < (sol_f32)iz) iz--;
    sx = sol_smoothstep(x - (sol_f32)ix);
    sz = sol_smoothstep(z - (sol_f32)iz);
    a = noise_hash(ix,     iz,     seed);
    b = noise_hash(ix + 1, iz,     seed);
    c = noise_hash(ix,     iz + 1, seed);
    d = noise_hash(ix + 1, iz + 1, seed);
    return noise_mix(noise_mix(a, b, sx), noise_mix(c, d, sx), sz);
}

/* fBm: octaves at doubling frequency, halving amplitude — one octave is
   a swell, the sum is terrain (large forms with detail riding them) */
#define TERRAIN_FEATURE_M 8.0f   /* base feature size, meters */
static sol_f32 terrain_fbm(sol_f32 x, sol_f32 z, unsigned seed) {
    sol_f32 sum = 0.0f, amp = 0.5f, freq = 1.0f;
    int     o;
    for (o = 0; o < 5; o++) {
        sum  += amp * value_noise(x * freq, z * freq, seed + (unsigned)o * 101u);
        freq *= 2.0f;
        amp  *= 0.5f;
    }
    return sum;                                /* ~[0, 0.97) */
}

float terrain_height(const float *params, int count, float lx, float lz) {
    float w    = mesh_ref_param("terrain", params, count, "w");
    float d    = mesh_ref_param("terrain", params, count, "d");
    float amp  = mesh_ref_param("terrain", params, count, "amp");
    unsigned seed = (unsigned)mesh_ref_param("terrain", params, count, "seed");
    float hw = w * 0.5f, hd = d * 0.5f, edge, ez, margin, mask;
    if (lx < -hw || lx > hw || lz < -hd || lz > hd) return 0.0f;
    edge = hw - (lx < 0.0f ? -lx : lx);        /* distance to the border */
    ez   = hd - (lz < 0.0f ? -lz : lz);
    if (ez < edge) edge = ez;
    margin = (w < d ? w : d) * 0.18f;
    mask   = sol_smoothstep(edge / margin);    /* the island fade: rim = 0 */
    return amp * mask * terrain_fbm(lx / TERRAIN_FEATURE_M,
                                    lz / TERRAIN_FEATURE_M, seed);
}

void make_terrain(MeshBuilder *b, sol_f32 w, sol_f32 d, int sub,
                  sol_f32 amp, unsigned seed) {
    float   p[5];
    sol_f32 hw = w * 0.5f, hd = d * 0.5f, bt;
    int     i, j;
    if (sub < 2)  sub = 2;
    if (sub > 96) sub = 96;
    p[0] = w; p[1] = d; p[2] = (float)sub; p[3] = amp; p[4] = (float)seed;
    bt = 0.4f + amp * 0.2f;                    /* base slab depth */

    /* the top: a shared-vertex grid; per-vertex normals are CENTRAL
       DIFFERENCES of the same height function — under IBL this is the
       difference between a hillside that catches sky light from the
       direction it faces and painted-flat ground */
    for (j = 0; j <= sub; j++) {
        for (i = 0; i <= sub; i++) {
            sol_f32 x = -hw + w * (sol_f32)i / (sol_f32)sub;
            sol_f32 z = -hd + d * (sol_f32)j / (sol_f32)sub;
            sol_f32 e = w / (sol_f32)sub;
            sol_f32 h  = terrain_height(p, 5, x, z);
            sol_f32 nx = terrain_height(p, 5, x - e, z)
                       - terrain_height(p, 5, x + e, z);
            sol_f32 nz = terrain_height(p, 5, x, z - e)
                       - terrain_height(p, 5, x, z + e);
            sol_f32 ny = 2.0f * e;
            sol_f32 nl = sqrtf(nx * nx + ny * ny + nz * nz);
            mb_push_vertex(b, x, h, z, nx / nl, ny / nl, nz / nl, x, z);
        }
    }
    for (j = 0; j < sub; j++) {
        for (i = 0; i < sub; i++) {
            sol_u32 v0 = (sol_u32)(j * (sub + 1) + i);
            sol_u32 v1 = v0 + 1;
            sol_u32 v2 = v0 + (sol_u32)(sub + 1);
            sol_u32 v3 = v2 + 1;
            mb_push_triangle(b, v0, v2, v3);
            mb_push_triangle(b, v0, v3, v1);
        }
    }

    /* the rim is masked to zero, so the skirt is four flat walls + a base:
       a deliberate PLATFORM edge, kin to the path slabs (the torn-earth
       underside is future sculpting) */
    face_x(b, -hw, -bt, 0.0f, -hd, hd, -1);
    face_x(b,  hw, -bt, 0.0f, -hd, hd,  1);
    face_z(b, -hw, hw, -bt, 0.0f, -hd, -1);
    face_z(b, -hw, hw, -bt, 0.0f,  hd,  1);
    face_y(b, -hw, hw, -bt, -hd, hd, -1);
}

/* ---- the codex (item 9): geometry from real bookbinding ----
   A bound book parameterized by its CONSTRUCTION, not by guesswork: sewn
   signatures form the text BLOCK; the sewing cords show through the leather
   as RAISED BANDS across the spine; the spine is ROUNDED AND BACKED
   (round 0 = a cheap flat binding, 1 = fully rounded); wooden BOARDS
   overhang the block by the SQUARES; vellum wants a CLASP. Real ranges
   make variation non-arbitrary (formats run h:w ~1.35-1.6; bands 3-5).

   Local frame: the book LIES on its back cover — x: 0 = spine edge ->
   w = fore-edge; y: 0..t thickness; z: +-h/2 head/tail. Stand it up with
   the object's rotation. COVER and BLOCK are separate refs so each part
   wears its own material (leather vs cream) with no new machinery — a
   book is a small GROUP, the way a room is. */
#define BOOK_SEG 6   /* spine arc segments */

static void box_minmax(MeshBuilder *b, sol_f32 x0, sol_f32 x1,
                       sol_f32 y0, sol_f32 y1, sol_f32 z0, sol_f32 z1) {
    face_x(b, x0, y0, y1, z0, z1, -1);
    face_x(b, x1, y0, y1, z0, z1,  1);
    face_y(b, x0, x1, y0, z0, z1, -1);
    face_y(b, x0, x1, y1, z0, z1,  1);
    face_z(b, x0, x1, y0, y1, z0, -1);
    face_z(b, x0, x1, y0, y1, z1,  1);
}

/* spine cross-section sample i of [0..BOOK_SEG]: from the back board edge
   (0,0) over the bulge (-depth, t/2) to the front board edge (0,t) */
static void spine_point(int i, sol_f32 t, sol_f32 depth, sol_f32 *x, sol_f32 *y) {
    float a = SOL_PI * ((float)i / (float)BOOK_SEG) - SOL_PI * 0.5f;
    *x = -depth * cosf(a);
    *y = t * 0.5f + t * 0.5f * sinf(a);
}

/* one lofted strip of the spine arc over [z0,z1] at the given bulge depth;
   per-segment normals from the cross-section tangent, rotated outward */
static void spine_strip(MeshBuilder *b, sol_f32 t, sol_f32 depth,
                        sol_f32 z0, sol_f32 z1) {
    int i;
    for (i = 0; i < BOOK_SEG; i++) {
        sol_f32 x0, y0, x1, y1, nx, ny, nl;
        spine_point(i,     t, depth, &x0, &y0);
        spine_point(i + 1, t, depth, &x1, &y1);
        nx = -(y1 - y0);
        ny =  (x1 - x0);
        nl = sqrtf(nx * nx + ny * ny);
        if (nl > 1e-9f) { nx /= nl; ny /= nl; }
        push_quad4(b,
                   x0, y0, z0, 0.0f, (float)i,
                   x0, y0, z1, 1.0f, (float)i,
                   x1, y1, z1, 1.0f, (float)(i + 1),
                   x1, y1, z0, 0.0f, (float)(i + 1),
                   nx, ny, 0.0f);
    }
}

void make_book_cover(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 t,
                     sol_f32 board, sol_f32 sq, sol_f32 round_,
                     int bands, int clasp) {
    sol_f32 hh    = h * 0.5f;
    sol_f32 depth = round_ * t * 0.42f;
    int     k;
    (void)sq;                                   /* the block insets; boards don't */

    box_minmax(b, 0.0f, w, 0.0f, board, -hh, hh);          /* back board  */
    box_minmax(b, 0.0f, w, t - board, t, -hh, hh);         /* front board */

    if (depth < 1e-4f) {
        face_x(b, 0.0f, 0.0f, t, -hh, hh, -1);             /* flat binding */
    } else {
        int i;
        spine_strip(b, t, depth, -hh, hh);                 /* the rounded wrap */
        for (i = 0; i < BOOK_SEG; i++) {                   /* head/tail caps */
            sol_f32 x0, y0, x1, y1;
            sol_u32 a, c, d;
            spine_point(i,     t, depth, &x0, &y0);
            spine_point(i + 1, t, depth, &x1, &y1);
            a = mb_push_vertex(b, 0.0f, t * 0.5f, hh, 0.0f, 0.0f, 1.0f, 0.5f, 0.5f);
            c = mb_push_vertex(b, x1,   y1,       hh, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f);
            d = mb_push_vertex(b, x0,   y0,       hh, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
            mb_push_triangle(b, a, c, d);
            a = mb_push_vertex(b, 0.0f, t * 0.5f, -hh, 0.0f, 0.0f, -1.0f, 0.5f, 0.5f);
            c = mb_push_vertex(b, x0,   y0,       -hh, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f);
            d = mb_push_vertex(b, x1,   y1,       -hh, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f);
            mb_push_triangle(b, a, c, d);
        }
    }

    /* raised bands: the sewing cords under the leather — a deeper arc strip
       plus two side rings closing it back to the spine surface */
    for (k = 1; k <= bands; k++) {
        sol_f32 zc = -hh + h * (sol_f32)k / (sol_f32)(bands + 1);
        sol_f32 bw = h * 0.018f;
        sol_f32 db = depth + t * 0.07f;   /* cord raise scales with the block */
        int     i;
        spine_strip(b, t, db, zc - bw, zc + bw);
        for (i = 0; i < BOOK_SEG; i++) {
            sol_f32 ix0, iy0, ix1, iy1, ox0, oy0, ox1, oy1;
            spine_point(i,     t, depth, &ix0, &iy0);
            spine_point(i + 1, t, depth, &ix1, &iy1);
            spine_point(i,     t, db,    &ox0, &oy0);
            spine_point(i + 1, t, db,    &ox1, &oy1);
            push_quad4(b, ix0, iy0, zc + bw, 0.0f, 0.0f,
                          ix1, iy1, zc + bw, 1.0f, 0.0f,
                          ox1, oy1, zc + bw, 1.0f, 1.0f,
                          ox0, oy0, zc + bw, 0.0f, 1.0f,
                          0.0f, 0.0f, 1.0f);
            push_quad4(b, ix1, iy1, zc - bw, 0.0f, 0.0f,
                          ix0, iy0, zc - bw, 1.0f, 0.0f,
                          ox0, oy0, zc - bw, 1.0f, 1.0f,
                          ox1, oy1, zc - bw, 0.0f, 1.0f,
                          0.0f, 0.0f, -1.0f);
        }
    }

    if (clasp)                                              /* vellum fights back */
        box_minmax(b, w * 0.94f, w * 1.03f,
                   t * 0.40f, t * 0.60f, -h * 0.05f, h * 0.05f);
}

void make_book_block(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 t,
                     sol_f32 board, sol_f32 sq) {
    sol_f32 hh = h * 0.5f;
    box_minmax(b, 0.0f, w - sq, board, t - board, -(hh - sq), hh - sq);
}

/* ---- the OPEN codex (the reader's book) ----
   Frame: gutter (the sewn fold) at x=0, the spread spans -w..+w, y up,
   z +-h/2. The boards lie flat as one slab; each page fan rises out of
   the gutter pinch along a smoothstep profile to the flat TEXT FIELD —
   which is where wtext puts the page text, clear of the gutter curve,
   like a typesetter would. */
#define BOOK_FAN_SEG 8

void make_book_open_cover(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 t,
                          sol_f32 board, sol_f32 sq) {
    (void)t; (void)sq;
    box_minmax(b, -w, w, 0.0f, board, -h * 0.5f, h * 0.5f);
}

void make_book_open_block(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 t,
                          sol_f32 board, sol_f32 sq) {
    sol_f32 wb    = w - sq;                     /* page width  */
    sol_f32 zh    = h * 0.5f - sq;              /* page height */
    sol_f32 stack = (t - 2.0f * board) * 0.5f;  /* one side's leaf pile */
    sol_f32 yb, xf, y0c, y1c;
    sol_f32 px[BOOK_FAN_SEG + 1], py[BOOK_FAN_SEG + 1];
    int     i, side;

    if (stack < 0.004f) stack = 0.004f;
    yb = board + stack * 0.25f;                 /* the gutter pinch (sewing) */
    y0c = board;                                /* fans rest ON the slab; their
                                                   underside is omitted (coplanar
                                                   with the slab top = z-fight) */
    y1c = board + stack;
    xf  = wb * BOOK_GUTTER_FRAC;                /* where the rise flattens */

    /* samples go where the curvature is: 6 of the 8 strips over the rise,
       2 across the flat field (uniform sampling spent most segments on a
       plane and left the curve chunky — Fran's catch) */
    for (i = 0; i <= BOOK_FAN_SEG; i++) {
        sol_f32 x = (i <= 6)
                  ? xf * (sol_f32)i / 6.0f
                  : xf + (wb - xf) * (sol_f32)(i - 6) / 2.0f;
        px[i] = x;
        py[i] = yb + (y1c - yb) * sol_smoothstep(x / xf);
    }

    for (side = 0; side < 2; side++) {
        sol_f32 s = (side == 0) ? 1.0f : -1.0f;
        for (i = 0; i < BOOK_FAN_SEG; i++) {    /* the top surface */
            sol_f32 nx = -(py[i + 1] - py[i]);
            sol_f32 ny =  (px[i + 1] - px[i]);
            sol_f32 nl = sqrtf(nx * nx + ny * ny);
            if (nl > 1e-9f) { nx /= nl; ny /= nl; }
            if (s > 0.0f)
                push_quad4(b, px[i],   py[i],   -zh, (float)i, 0.0f,
                              px[i],   py[i],    zh, (float)i, 1.0f,
                              px[i+1], py[i+1],  zh, (float)(i+1), 1.0f,
                              px[i+1], py[i+1], -zh, (float)(i+1), 0.0f,
                              nx, ny, 0.0f);
            else
                push_quad4(b, -px[i],   py[i],    zh, (float)i, 1.0f,
                              -px[i],   py[i],   -zh, (float)i, 0.0f,
                              -px[i+1], py[i+1], -zh, (float)(i+1), 0.0f,
                              -px[i+1], py[i+1],  zh, (float)(i+1), 1.0f,
                              -nx, ny, 0.0f);
        }
        for (i = 0; i < BOOK_FAN_SEG; i++) {    /* head/tail silhouette walls */
            sol_f32 ax = s * px[i], bx = s * px[i + 1];
            push_quad4(b, ax, y0c,  zh, 0.0f, 0.0f,
                          bx, y0c,  zh, 1.0f, 0.0f,
                          bx, py[i+1], zh, 1.0f, 1.0f,
                          ax, py[i],   zh, 0.0f, 1.0f,
                          0.0f, 0.0f, 1.0f);
            push_quad4(b, bx, y0c, -zh, 0.0f, 0.0f,
                          ax, y0c, -zh, 1.0f, 0.0f,
                          ax, py[i],   -zh, 1.0f, 1.0f,
                          bx, py[i+1], -zh, 0.0f, 1.0f,
                          0.0f, 0.0f, -1.0f);
        }
        face_x(b, s * wb, y0c, y1c, -zh, zh, s > 0.0f ? 1 : -1);   /* fore-edge */
        face_x(b, 0.0f,   y0c, yb,  -zh, zh, s > 0.0f ? -1 : 1);   /* gutter    */
    }
}

/* A flat arrow in the XY plane at z=0, from (x0,y0) to (x1,y1): a shaft quad
   plus a triangular head, single-sided, facing +Z. Board ink (item 8) — the
   endpoints come from two CARDS' positions, so this geometry is DERIVED and
   rebuilt as they move, never stored: the rel is the data, the arrow is its
   picture. Too-short segments emit nothing (overlapping cards have no
   visible edge — the relation still persists). */
void make_arrow(MeshBuilder *b, sol_f32 x0, sol_f32 y0,
                sol_f32 x1, sol_f32 y1, sol_f32 w) {
    sol_f32 dx = x1 - x0, dy = y1 - y0;
    sol_f32 len = (sol_f32)sqrt((double)(dx * dx + dy * dy));
    sol_f32 ux, uy, px, py, hw, hl, hww, sx, sy;
    sol_u32 a, c, d;
    if (len < w * 2.0f) return;
    ux = dx / len;  uy = dy / len;            /* along the shaft */
    px = -uy;       py = ux;                  /* its perpendicular */
    hw  = w * 0.5f;                           /* shaft half-width */
    hl  = w * 3.5f;                           /* head length */
    if (hl > len * 0.5f) hl = len * 0.5f;
    hww = w * 1.4f;                           /* head half-width */
    sx = x1 - ux * hl;  sy = y1 - uy * hl;    /* where the shaft hands over */
    push_quad4(b,
               x0 + px * hw, y0 + py * hw, 0.0f, 0.0f, 0.0f,
               x0 - px * hw, y0 - py * hw, 0.0f, 0.0f, 1.0f,
               sx - px * hw, sy - py * hw, 0.0f, 1.0f, 1.0f,
               sx + px * hw, sy + py * hw, 0.0f, 1.0f, 0.0f,
               0.0f, 0.0f, 1.0f);
    a = mb_push_vertex(b, sx + px * hww, sy + py * hww, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
    c = mb_push_vertex(b, sx - px * hww, sy - py * hww, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f);
    d = mb_push_vertex(b, x1,            y1,            0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.5f);
    mb_push_triangle(b, a, c, d);
}

static void emit_card(MeshBuilder *b, const float *p) { make_card(b, p[0], p[1], p[2]); }
static void emit_board(MeshBuilder *b, const float *p) { make_card(b, p[0], p[1], p[2]); }
static void emit_book_cover(MeshBuilder *b, const float *p) {
    make_book_cover(b, p[0], p[1], p[2], p[3], p[4], p[5],
                    (int)(p[6] + 0.5f), p[7] > 0.5f);
}
static void emit_book_block(MeshBuilder *b, const float *p) {
    make_book_block(b, p[0], p[1], p[2], p[3], p[4]);
}
static void emit_book_open_cover(MeshBuilder *b, const float *p) {
    make_book_open_cover(b, p[0], p[1], p[2], p[3], p[4]);
}
static void emit_book_open_block(MeshBuilder *b, const float *p) {
    make_book_open_block(b, p[0], p[1], p[2], p[3], p[4]);
}
static void emit_terrain(MeshBuilder *b, const float *p) {
    make_terrain(b, p[0], p[1], (int)(p[2] + 0.5f), p[3], (unsigned)p[4]);
}

/* ---- molding (P6 item 1): the gothic kit's first registry row ----
   A section from the molding table swept along a run of `len` meters:
   straight, or bent `bend` degrees along a circular arc. vert=0 runs in
   the ground plane (origin, heading +X, curving toward +Z — a string
   course following a plan); vert=1 runs in a vertical plane (origin,
   heading +Y, curving toward +X — a standing shaft, or at bend=180 a
   semicircular band; profile o faces radially outward). Free ends are
   capped; placement is main.c composition (TODO6 §1.7). */
#define MOLDING_MAX_PTS 128
static void emit_molding(MeshBuilder *b, const float *p) {
    vec3  path[MOLDING_MAX_PTS];
    int   pn, n, i;
    int   vert  = p[4] > 0.5f;
    float len   = p[1], scale = p[2];
    float bend  = p[3] * (SOL_PI / 180.0f);
    const ProfilePt *prof = gothic_profile((int)(p[0] + 0.5f), &pn);
    vec3  plane = vert ? vec3_make(0.0f, 0.0f, 1.0f)
                       : vec3_make(0.0f, 1.0f, 0.0f);

    if (!prof || len <= 0.0f || scale <= 0.0f) return;
    if (bend < 0.0f) bend = -bend;
    if (bend < 1e-4f) {                                    /* straight */
        path[0] = vec3_make(0.0f, 0.0f, 0.0f);
        path[1] = vert ? vec3_make(0.0f, len, 0.0f)
                       : vec3_make(len, 0.0f, 0.0f);
        gothic_sweep(b, prof, pn, path, 2, plane, scale, 1, 1);
        return;
    }
    n = gothic_arc_segments(len, bend);                    /* the two caps */
    if (n > MOLDING_MAX_PTS - 1) n = MOLDING_MAX_PTS - 1;
    {
        float r = len / bend;                              /* arc radius */
        for (i = 0; i <= n; i++) {
            float t = bend * (float)i / (float)n;
            if (vert) path[i] = vec3_make(r - r * cosf(t), r * sinf(t), 0.0f);
            else      path[i] = vec3_make(r * sinf(t), 0.0f, r - r * cosf(t));
        }
        gothic_sweep(b, prof, pn, path, n + 1, plane, scale, 1, 1);
    }
}

static void emit_wall_arched(MeshBuilder *b, const float *p) {
    gothic_wall_arched(b, p[0], p[1], p[6], p[2], p[3], p[4], p[5]);
}
static void emit_portal(MeshBuilder *b, const float *p) {
    gothic_wall_portal(b, p[0], p[1], p[2], p[3], p[4], p[5],
                       (int)(p[6] + 0.5f), p[7], 1);
}
static void emit_church_stone(MeshBuilder *b, const float *p) {
    church_stone(b, p, 8);   /* the church schema is exactly 8 wide */
}
static void emit_church_glass(MeshBuilder *b, const float *p) {
    church_glass(b, p, 8);
}
static void emit_church_roof(MeshBuilder *b, const float *p) {
    church_roof(b, p, 8);
}
static void emit_church_floor(MeshBuilder *b, const float *p) {
    church_floor(b, p, 8);
}
static void emit_church_decals(MeshBuilder *b, const float *p) {
    church_decals(b, p, 8);
}
static void emit_pinnacle(MeshBuilder *b, const float *p) {
    gothic_pinnacle(b, p[0], (unsigned)(p[1] + 0.5f));
}
static void emit_column(MeshBuilder *b, const float *p) {
    gothic_column(b, p[0], p[1], p[2], (unsigned)(p[3] + 0.5f));
}
static void emit_arch_frag(MeshBuilder *b, const float *p) {
    gothic_arch_frag(b, p[0], p[1], p[2], p[3], p[4]);
}
static void emit_stair(MeshBuilder *b, const float *p) {
    gothic_stair(b, p[0], p[1], p[2], (int)(p[3] + 0.5f));
}
static void emit_balustrade(MeshBuilder *b, const float *p) {
    gothic_balustrade(b, p[0], p[1], (unsigned)(p[2] + 0.5f), p[3]);
}
static void emit_cross(MeshBuilder *b, const float *p) {
    gothic_cross(b, p[0]);
}
static void emit_oak(MeshBuilder *b, const float *p) {
    flora_tree_wood(b, FLORA_OAK, p, 10);
}
static void emit_pine(MeshBuilder *b, const float *p) {
    flora_tree_wood(b, FLORA_PINE, p, 10);
}
static void emit_birch(MeshBuilder *b, const float *p) {
    flora_tree_wood(b, FLORA_BIRCH, p, 10);
}
static void emit_cypress(MeshBuilder *b, const float *p) {
    flora_tree_wood(b, FLORA_CYPRESS, p, 10);
}
static void emit_shrub(MeshBuilder *b, const float *p) {
    flora_tree_wood(b, FLORA_SHRUB, p, 10);
}
static void emit_boulder(MeshBuilder *b, const float *p) {
    rock_boulder(b, p[0], (unsigned)(p[1] + 0.5f), p[2]);
}
/* a pond's surface (P7 item 8): a flat disc at y=0, normal up, world-
   scale UVs for the ripple normal. depth/seed are the SHADER's (the
   water pass reads them off the object) — the geometry is just r. */
static void emit_pond(MeshBuilder *b, const float *p) {
    float   r = p[0] > 0.1f ? p[0] : 0.1f;
    int     n = 48, k;
    sol_u32 c = mb_push_vertex(b, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f);
    for (k = 0; k < n; k++) {
        float a = (float)k / (float)n * 6.2831853f;
        float x = r * cosf(a), z = r * sinf(a);
        mb_push_vertex(b, x, 0.0f, z, 0.0f, 1.0f, 0.0f, x, z);
    }
    for (k = 0; k < n; k++)
        mb_push_triangle(b, c, (sol_u32)(1 + k), (sol_u32)(1 + (k + 1) % n));
}

static const MeshRefEntry REGISTRY[] = {
    { "box",  0, { 0 }, { 0.0f }, emit_box  },
    { "grid", 0, { 0 }, { 0.0f }, emit_grid },
    { "page", 0, { 0 }, { 0.0f }, emit_page },
    /* room: dims + per-wall/ceiling presence flags (1 = present; absent
       attrs default to a sealed shell, so older files keep their meaning) */
    { "room", 8, { "w", "d", "h", "wn", "we", "ws", "ww", "ceil" },
                 { 6.0f, 4.0f, 3.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f }, emit_room },
    { "wall", 6, { "w", "h", "ox", "ow", "oh", "t" },
                 { 4.0f, 3.0f, 1.5f, 1.0f, 2.2f, 0.15f }, emit_wall },
    { "path", 3, { "len", "w", "t" }, { 6.0f, 1.5f, 0.15f }, emit_path },
    { "walkway", 4, { "len", "w", "t", "dy" }, { 4.0f, 1.6f, 0.15f, 0.0f }, emit_walkway },
    { "card", 3, { "w", "h", "t" },   { 0.35f, 0.5f, 0.03f }, emit_card },
    /* board: a card grown to furniture scale (item 8) — same slab geometry,
       bottom-origin, front face toward local +Z. Its OWN ref name is its
       identity: the drag code recognizes a pinboard by mesh_ref "board". */
    { "board", 3, { "w", "h", "t" }, { 1.8f, 1.2f, 0.05f }, emit_board },
    /* the codex (item 9): cover and block are SEPARATE refs so each part
       wears its own material — a book is a small group, like a room.
       Defaults are a sane quarto; the spawner varies within real ranges. */
    { "book_cover", 8, { "w", "h", "t", "board", "sq", "round", "bands", "clasp" },
      { 0.40f, 0.56f, 0.10f, 0.014f, 0.008f, 0.8f, 4.0f, 0.0f }, emit_book_cover },
    { "book_block", 5, { "w", "h", "t", "board", "sq" },
      { 0.40f, 0.56f, 0.10f, 0.014f, 0.008f }, emit_book_block },
    { "book_open_cover", 5, { "w", "h", "t", "board", "sq" },
      { 0.40f, 0.56f, 0.10f, 0.014f, 0.008f }, emit_book_open_cover },
    { "book_open_block", 5, { "w", "h", "t", "board", "sq" },
      { 0.40f, 0.56f, 0.10f, 0.014f, 0.008f }, emit_book_open_block },
    /* terrain (item 10): a floating plot — the SEED is its identity, so a
       minted island keeps its hills forever (the codex pattern at
       landscape scale) */
    { "terrain", 5, { "w", "d", "sub", "amp", "seed" },
      { 32.0f, 32.0f, 48.0f, 2.5f, 7.0f }, emit_terrain },
    /* molding (P6 item 1): prof indexes gothic.h's append-only table —
       RIB 0, MULLION 1, STRING 2, BASE 3, HOOD 4, SHAFT_OCT 5 */
    { "molding", 5, { "prof", "len", "scale", "bend", "vert" },
      { 0.0f, 2.0f, 1.0f, 0.0f, 0.0f }, emit_molding },
    /* the arch family (P6 item 2): wall_arched = "wall"'s param family
       with a pointed head (springing height + acuteness replace oh);
       portal = the same opening as centered RECESSED ORDERS stepping
       through the thickness, archivolt-dressed */
    { "wall_arched", 7, { "w", "h", "ox", "ow", "spring", "acute", "t" },
      { 4.0f, 3.5f, 1.25f, 1.5f, 1.4f, 1.0f, 0.3f }, emit_wall_arched },
    { "portal", 8, { "w", "h", "t", "ow", "spring", "acute", "orders", "step" },
      { 6.0f, 5.0f, 0.9f, 1.6f, 2.2f, 1.0f, 3.0f, 0.18f }, emit_portal },
    /* the church (P6 §1.7): a GROUP of sub-refs sharing this schema —
       stone lands with item 4; floor/glass/roof follow their items.
       Defaults MUST equal gothic_church_defaults (gothictest asserts). */
    { "church_stone", 8,
      { "w", "d", "seed", "style", "ruin", "built", "acute", "reserved" },
      { 18.0f, 30.0f, 7.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f }, emit_church_stone },
    { "church_glass", 8,
      { "w", "d", "seed", "style", "ruin", "built", "acute", "reserved" },
      { 18.0f, 30.0f, 7.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f }, emit_church_glass },
    { "church_roof", 8,
      { "w", "d", "seed", "style", "ruin", "built", "acute", "reserved" },
      { 18.0f, 30.0f, 7.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f }, emit_church_roof },
    { "church_floor", 8,
      { "w", "d", "seed", "style", "ruin", "built", "acute", "reserved" },
      { 18.0f, 30.0f, 7.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f }, emit_church_floor },
    { "church_decals", 8,
      { "w", "d", "seed", "style", "ruin", "built", "acute", "reserved" },
      { 18.0f, 30.0f, 7.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f }, emit_church_decals },
    /* the standalone pinnacle (P6 item 7; item 10 instances it) */
    { "pinnacle", 2, { "h", "seed" }, { 1.8f, 7.0f }, emit_pinnacle },
    /* the follies (P6 item 10): the kit's vocabulary, placeable alone —
       each a thin wrapper over machinery the church proved; ruin is a
       per-piece truncation param (no plan, no graph) */
    { "column", 4, { "h", "style", "broken", "seed" },
      { 4.5f, 0.0f, 0.0f, 7.0f }, emit_column },
    { "arch_frag", 5, { "span", "acute", "depth", "h", "ruin" },
      { 2.2f, 1.0f, 0.6f, 4.5f, 0.5f }, emit_arch_frag },
    { "stair", 4, { "w", "rise", "run", "steps" },
      { 2.0f, 0.16f, 0.34f, 8.0f }, emit_stair },
    { "balustrade", 4, { "len", "h", "seed", "ruin" },
      { 3.0f, 1.0f, 7.0f, 0.0f }, emit_balustrade },
    { "cross", 1, { "h" }, { 3.2f }, emit_cross },
    /* the trees (P7 item 3): species are REFS — the registry's per-ref
       defaults express the presets (the church-group pattern, your
       ruling). The 10-param prefix exposes the silhouette knobs PLUS
       leaf_size/leaf_density (item 4: the shedding dial reaches the
       file); the deeper structural schema (twist/taper/gens...) stays
       species-fixed. Defaults MUST equal flora_schema's first 10
       (floratest asserts). */
    { "oak", 10, { "seed", "age", "height", "girth", "apical",
                   "splits", "spread", "droop", "leaf_size", "leaf_density" },
      { 0.0f, 1.0f, 7.0f, 0.28f, 0.15f, 3.0f, 42.0f, 0.10f, 0.55f, 0.8f },
      emit_oak },
    { "pine", 10, { "seed", "age", "height", "girth", "apical",
                    "splits", "spread", "droop", "leaf_size", "leaf_density" },
      { 0.0f, 1.0f, 11.0f, 0.26f, 0.92f, 4.0f, 68.0f, 0.60f, 0.45f, 0.7f },
      emit_pine },
    { "birch", 10, { "seed", "age", "height", "girth", "apical",
                     "splits", "spread", "droop", "leaf_size", "leaf_density" },
      { 0.0f, 1.0f, 9.0f, 0.16f, 0.70f, 2.0f, 32.0f, -0.05f, 0.40f, 0.6f },
      emit_birch },
    { "cypress", 10, { "seed", "age", "height", "girth", "apical",
                       "splits", "spread", "droop", "leaf_size", "leaf_density" },
      { 0.0f, 1.0f, 7.0f, 0.20f, 0.90f, 5.0f, 24.0f, -0.55f, 0.35f, 0.9f },
      emit_cypress },
    /* the shrub (P7 item 7): a trunk-zero species — the §1.4 proof,
       placeable as a hero bush and scattered as undergrowth */
    { "shrub", 10, { "seed", "age", "height", "girth", "apical",
                     "splits", "spread", "droop", "leaf_size", "leaf_density" },
      { 0.0f, 1.0f, 1.8f, 0.06f, 0.05f, 4.0f, 55.0f, 0.10f, 0.30f, 0.85f },
      emit_shrub },
    /* the island's own stone (P7 item 6): an fBm-displaced octahedron;
       flat squashes it to a standable table-rock */
    { "boulder", 3, { "size", "seed", "flat" },
      { 1.2f, 7.0f, 0.0f }, emit_boulder },
    /* the pond surface (P7 item 8): a flat disc the WATER PASS draws — the
       scene pass skips it. depth/seed feed the shader (tint + ripple) */
    { "pond", 3, { "r", "depth", "seed" },
      { 4.0f, 1.5f, 7.0f }, emit_pond }
};
#define REGISTRY_COUNT (sizeof(REGISTRY) / sizeof(REGISTRY[0]))

static const MeshRefEntry *registry_find(const char *ref) {
    sol_u32 i;
    for (i = 0; i < REGISTRY_COUNT; i++) {
        if (strcmp(REGISTRY[i].name, ref) == 0) return &REGISTRY[i];
    }
    return (const MeshRefEntry *)0;
}

int mesh_ref_schema(const char *ref, const char *const **names, const float **defaults) {
    const MeshRefEntry *e = registry_find(ref);
    if (!e) return -1;
    if (names)    *names    = e->param_names;
    if (defaults) *defaults = e->defaults;
    return e->param_count;
}

/* Effective value of ONE schema parameter by name: the object's own saved
   prefix if it reaches that far, else the registry default — the same merge
   rule as mesh_ref_build, for callers that need a dimension without emitting
   (item 8: the drag code reads a board's w/h to rect-test the cursor hit).
   Unknown ref or name returns 0 — a visibly degenerate dimension. */
float mesh_ref_param(const char *ref, const float *params, int count, const char *name) {
    const MeshRefEntry *e = registry_find(ref);
    int                 k;
    if (!e) return 0.0f;
    for (k = 0; k < e->param_count; k++) {
        if (strcmp(e->param_names[k], name) == 0)
            return (params && k < count) ? params[k] : e->defaults[k];
    }
    return 0.0f;
}

sol_bool mesh_ref_build(const char *ref, const float *params, int count, MeshBuilder *b) {
    const MeshRefEntry *e = registry_find(ref);
    float               full[MESH_REF_MAX_PARAMS];
    int                 k;
    if (!e) return SOL_FALSE;
    for (k = 0; k < MESH_REF_MAX_PARAMS; k++) full[k] = 0.0f;  /* clean tail */
    /* merge the caller's prefix with the defaults: a file written before a
       schema grew (a 3-param room from before the presence flags) must get
       defaults for the new tail, never an uninitialized read */
    for (k = 0; k < e->param_count; k++)
        full[k] = (params && k < count) ? params[k] : e->defaults[k];
    e->emit(b, full);
    return SOL_TRUE;
}

/* Per-vertex tangents from the UV gradient: for each triangle, solve for the
   world direction of +U (tangent) and +V (bitangent), accumulate onto its 3
   vertices, then per vertex Gram-Schmidt against the normal, normalize, and
   record handedness w (= sign of dot(cross(N,T), bitangent)) for B=cross(N,T)*w. */
void mb_compute_tangents(MeshBuilder *b) {
    sol_f32 *tan, *bitan;
    sol_u32  i, v;

    if (b->vertex_count == 0) return;
    tan   = (sol_f32 *)calloc((size_t)b->vertex_count * 3, sizeof(sol_f32));
    bitan = (sol_f32 *)calloc((size_t)b->vertex_count * 3, sizeof(sol_f32));
    if (!tan || !bitan) { free(tan); free(bitan); return; }

    for (i = 0; i + 2 < b->index_count; i += 3) {
        sol_u32  ix[3];
        int      k;
        sol_f32 *p0 = &b->vertices[b->indices[i]   * 12];
        sol_f32 *p1 = &b->vertices[b->indices[i+1] * 12];
        sol_f32 *p2 = &b->vertices[b->indices[i+2] * 12];
        sol_f32  e1x=p1[0]-p0[0], e1y=p1[1]-p0[1], e1z=p1[2]-p0[2];   /* edges */
        sol_f32  e2x=p2[0]-p0[0], e2y=p2[1]-p0[1], e2z=p2[2]-p0[2];
        sol_f32  du1=p1[6]-p0[6], dv1=p1[7]-p0[7];                    /* UV deltas */
        sol_f32  du2=p2[6]-p0[6], dv2=p2[7]-p0[7];
        sol_f32  det=du1*dv2 - du2*dv1, f, tx,ty,tz, bx,by,bz;
        if (det > -1e-12f && det < 1e-12f) continue;                 /* degenerate UVs */
        f = 1.0f / det;
        tx=f*(dv2*e1x - dv1*e2x); ty=f*(dv2*e1y - dv1*e2y); tz=f*(dv2*e1z - dv1*e2z);
        bx=f*(du1*e2x - du2*e1x); by=f*(du1*e2y - du2*e1y); bz=f*(du1*e2z - du2*e1z);
        ix[0]=b->indices[i]; ix[1]=b->indices[i+1]; ix[2]=b->indices[i+2];
        for (k = 0; k < 3; k++) {
            sol_u32 j = ix[k];
            tan[j*3+0]+=tx;   tan[j*3+1]+=ty;   tan[j*3+2]+=tz;
            bitan[j*3+0]+=bx; bitan[j*3+1]+=by; bitan[j*3+2]+=bz;
        }
    }

    for (v = 0; v < b->vertex_count; v++) {
        sol_f32 *vert = &b->vertices[v*12];
        sol_f32  nx=vert[3], ny=vert[4], nz=vert[5];
        sol_f32  tx=tan[v*3+0], ty=tan[v*3+1], tz=tan[v*3+2];
        sol_f32  ndt, len, cx, cy, cz, w;
        ndt = nx*tx + ny*ty + nz*tz;                                 /* Gram-Schmidt vs N */
        tx -= nx*ndt; ty -= ny*ndt; tz -= nz*ndt;
        len = (sol_f32)sqrt((double)(tx*tx + ty*ty + tz*tz));
        if (len > 1e-8f) { tx/=len; ty/=len; tz/=len; }
        else {                                                       /* no usable UVs: any perp to N */
            if (nx*nx < 0.9f) { tx=1.0f-nx*nx; ty=-nx*ny; tz=-nx*nz; }
            else              { tx=-ny*nx; ty=1.0f-ny*ny; tz=-ny*nz; }
            len = (sol_f32)sqrt((double)(tx*tx + ty*ty + tz*tz));
            if (len > 1e-8f) { tx/=len; ty/=len; tz/=len; }
        }
        cx = ny*tz - nz*ty; cy = nz*tx - nx*tz; cz = nx*ty - ny*tx;  /* cross(N,T) */
        w  = (cx*bitan[v*3+0] + cy*bitan[v*3+1] + cz*bitan[v*3+2]) < 0.0f ? -1.0f : 1.0f;
        vert[8]=tx; vert[9]=ty; vert[10]=tz; vert[11]=w;
    }

    free(tan); free(bitan);
}

/* mesh_from_builder lives in mesh_gpu.c (the one function here that uploads):
   everything in THIS file is pure CPU, so the emitters + registry stay
   headless-testable and linkable by scene_io for the param schema. */

/* ---------------------------------------- retained CPU geometry (P4 item 2) */
/* The store behind mesh_geom_register/get/drop (mesh.h): a flat id->geometry
   table, linear-scanned (picking visits it per object per CLICK — clicks are
   rare; the spatial index, not this table, is where per-frame speed lives).
   Re-registering an id replaces its entry (the RHI free-list reuses slots);
   L-reload's deliberately-undestroyed meshes keep their entries alive the
   same bounded way they keep their buffers — the asset registry (P4 item 4)
   inherits both debts together. */

typedef struct {
    sol_u32 id;
    CpuGeom g;
} GeomEntry;

static GeomEntry *g_geom       = NULL;
static int        g_geom_count = 0;
static int        g_geom_cap   = 0;

static void geom_free_payload(CpuGeom *g) {
    free(g->pos);
    free(g->idx);
    g->pos = NULL;  g->idx = NULL;
    g->vert_count = 0;  g->idx_count = 0;
}

void mesh_geom_register(sol_u32 vbuffer_id, const MeshBuilder *b) {
    GeomEntry *e = NULL;
    sol_f32   *pos;
    sol_u32   *idx;
    sol_u32    v;
    int        i;
    if (vbuffer_id == 0 || b->vertex_count == 0 || b->index_count == 0) return;

    /* allocate-first, commit-last: an OOM leaves the table untouched and
       picking simply falls back to the AABB for this mesh */
    pos = (sol_f32 *)malloc((size_t)b->vertex_count * 3 * sizeof(sol_f32));
    idx = (sol_u32 *)malloc((size_t)b->index_count * sizeof(sol_u32));
    if (pos == NULL || idx == NULL) {
        free(pos);
        free(idx);
        return;
    }
    for (v = 0; v < b->vertex_count; v++) {  /* positions only: floats [v*12+0..2] */
        pos[v * 3 + 0] = b->vertices[v * 12 + 0];
        pos[v * 3 + 1] = b->vertices[v * 12 + 1];
        pos[v * 3 + 2] = b->vertices[v * 12 + 2];
    }
    memcpy(idx, b->indices, (size_t)b->index_count * sizeof(sol_u32));

    for (i = 0; i < g_geom_count; i++) {
        if (g_geom[i].id == vbuffer_id) { e = &g_geom[i]; break; }
    }
    if (e) {
        geom_free_payload(&e->g);            /* slot reuse: replace in place */
    } else {
        if (g_geom_count == g_geom_cap) {
            int        ncap = g_geom_cap ? g_geom_cap * 2 : 32;
            GeomEntry *ng   = (GeomEntry *)realloc(g_geom,
                                  (size_t)ncap * sizeof *ng);
            if (ng == NULL) { free(pos); free(idx); return; }
            g_geom     = ng;
            g_geom_cap = ncap;
        }
        e = &g_geom[g_geom_count++];
        e->id = vbuffer_id;
    }
    e->g.pos        = pos;
    e->g.idx        = idx;
    e->g.vert_count = b->vertex_count;
    e->g.idx_count  = b->index_count;
}

void mesh_geom_drop(sol_u32 vbuffer_id) {
    int i;
    for (i = 0; i < g_geom_count; i++) {
        if (g_geom[i].id == vbuffer_id) {
            geom_free_payload(&g_geom[i].g);
            g_geom[i] = g_geom[--g_geom_count];   /* order-free swap-remove */
            return;
        }
    }
}

const CpuGeom *mesh_geom_get(sol_u32 vbuffer_id) {
    int i;
    if (vbuffer_id == 0) return NULL;
    for (i = 0; i < g_geom_count; i++) {
        if (g_geom[i].id == vbuffer_id) return &g_geom[i].g;
    }
    return NULL;
}
