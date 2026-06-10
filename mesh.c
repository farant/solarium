/* mesh.c — CPU mesh builder + primitive emitters. Above the seam: talks only
   to rhi.h for upload, never to GL. */

#include "mesh.h"

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

/* A room shell: every present face INTERIOR — normals point inward and the
   winding is CCW from inside, so the geometry stays correct if back-face
   culling is ever enabled. Origin at the floor's center, y in [0,h].
   World-scale UVs: u/v run in meters, so a texture tiles at the same density
   on a 2m wall and an 8m wall. Each wall and the ceiling is OPTIONAL (the
   follies direction: three-walled stages, roofless cells, bare platforms);
   the floor is always present — a room you can't stand in isn't a room. */
void make_room(MeshBuilder *b, sol_f32 w, sol_f32 d, sol_f32 h,
               int wall_n, int wall_e, int wall_s, int wall_w, int ceil) {
    sol_f32 hw = w * 0.5f, hd = d * 0.5f;

    /* floor (normal +Y); ceiling (normal -Y) if present */
    push_quad4(b, -hw, 0.0f, -hd, 0.0f, 0.0f,   -hw, 0.0f,  hd, 0.0f, d,
                   hw, 0.0f,  hd, w,    d,       hw, 0.0f, -hd, w,    0.0f,
               0.0f, 1.0f, 0.0f);
    if (ceil)
        push_quad4(b, -hw, h,    -hd, 0.0f, 0.0f,    hw, h,    -hd, w,    0.0f,
                       hw, h,     hd, w,    d,      -hw, h,     hd, 0.0f, d,
                   0.0f, -1.0f, 0.0f);

    /* north wall (z = -hd, faces +Z into the room) / south (z = +hd, faces -Z) */
    if (wall_n)
        push_quad4(b, -hw, 0.0f, -hd, 0.0f, 0.0f,    hw, 0.0f, -hd, w,    0.0f,
                       hw, h,    -hd, w,    h,      -hw, h,    -hd, 0.0f, h,
                   0.0f, 0.0f, 1.0f);
    if (wall_s)
        push_quad4(b,  hw, 0.0f,  hd, 0.0f, 0.0f,   -hw, 0.0f,  hd, w,    0.0f,
                      -hw, h,     hd, w,    h,       hw, h,     hd, 0.0f, h,
                   0.0f, 0.0f, -1.0f);

    /* west wall (x = -hw, faces +X) / east (x = +hw, faces -X) */
    if (wall_w)
        push_quad4(b, -hw, 0.0f,  hd, 0.0f, 0.0f,   -hw, 0.0f, -hd, d,    0.0f,
                      -hw, h,    -hd, d,    h,      -hw, h,     hd, 0.0f, h,
                   1.0f, 0.0f, 0.0f);
    if (wall_e)
        push_quad4(b,  hw, 0.0f, -hd, 0.0f, 0.0f,    hw, 0.0f,  hd, d,    0.0f,
                       hw, h,     hd, d,    h,       hw, h,    -hd, 0.0f, h,
                   -1.0f, 0.0f, 0.0f);
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
    make_room(b, p[0], p[1], p[2],
              p[3] > 0.5f, p[4] > 0.5f, p[5] > 0.5f, p[6] > 0.5f, p[7] > 0.5f);
}
static void emit_wall(MeshBuilder *b, const float *p) { make_wall_with_opening(b, p[0], p[1], p[2], p[3], p[4], p[5]); }
static void emit_path(MeshBuilder *b, const float *p) { make_path(b, p[0], p[1], p[2]); }

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
    { "path", 3, { "len", "w", "t" }, { 6.0f, 1.5f, 0.15f }, emit_path }
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

sol_bool mesh_ref_build(const char *ref, const float *params, int count, MeshBuilder *b) {
    const MeshRefEntry *e = registry_find(ref);
    float               full[MESH_REF_MAX_PARAMS];
    int                 k;
    if (!e) return SOL_FALSE;
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
