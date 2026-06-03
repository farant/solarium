/* mesh.c — CPU mesh builder + primitive emitters. Above the seam: talks only
   to rhi.h for upload, never to GL. */

#include "mesh.h"

#include <stdlib.h>

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
        b->vertices = realloc(b->vertices, (size_t)b->vertex_cap * 8 * sizeof(sol_f32));
    }
    base = b->vertex_count * 8;
    b->vertices[base+0] = px; b->vertices[base+1] = py; b->vertices[base+2] = pz;
    b->vertices[base+3] = nx; b->vertices[base+4] = ny; b->vertices[base+5] = nz;
    b->vertices[base+6] = u;  b->vertices[base+7] = v;
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

Mesh mesh_from_builder(const MeshBuilder *b) {
    Mesh m;
    m.vbuffer = rhi_create_buffer(RHI_BUFFER_VERTEX, b->vertices,
                    (size_t)b->vertex_count * 8 * sizeof(sol_f32));
    m.ibuffer = rhi_create_buffer(RHI_BUFFER_INDEX, b->indices,
                    (size_t)b->index_count * sizeof(sol_u32));
    m.index_count = (int)b->index_count;
    return m;
}
