/* mesh.c — CPU mesh builder + primitive emitters. Above the seam: talks only
   to rhi.h for upload, never to GL. */

#include "mesh.h"

#include <stdlib.h>
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

Mesh mesh_from_builder(MeshBuilder *b) {
    Mesh m;
    mb_compute_tangents(b);                  /* finalize tangents before upload */
    m.vbuffer = rhi_create_buffer(RHI_BUFFER_VERTEX, b->vertices,
                    (size_t)b->vertex_count * 12 * sizeof(sol_f32));
    m.ibuffer = rhi_create_buffer(RHI_BUFFER_INDEX, b->indices,
                    (size_t)b->index_count * sizeof(sol_u32));
    m.index_count = (int)b->index_count;

    /* local-space AABB over the vertex positions (floats [i*12 + 0..2]) */
    if (b->vertex_count == 0) {
        m.bounds.min.x = m.bounds.min.y = m.bounds.min.z = 0.0f;
        m.bounds.max = m.bounds.min;
    } else {
        sol_f32 minx, miny, minz, maxx, maxy, maxz;
        sol_u32 i;
        minx = maxx = b->vertices[0];
        miny = maxy = b->vertices[1];
        minz = maxz = b->vertices[2];
        for (i = 1; i < b->vertex_count; i++) {
            sol_f32 x = b->vertices[i*12+0], y = b->vertices[i*12+1], z = b->vertices[i*12+2];
            if (x < minx) minx = x;  if (x > maxx) maxx = x;
            if (y < miny) miny = y;  if (y > maxy) maxy = y;
            if (z < minz) minz = z;  if (z > maxz) maxz = z;
        }
        m.bounds.min.x = minx; m.bounds.min.y = miny; m.bounds.min.z = minz;
        m.bounds.max.x = maxx; m.bounds.max.y = maxy; m.bounds.max.z = maxz;
    }
    return m;
}
