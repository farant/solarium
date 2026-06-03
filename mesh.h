#ifndef MESH_H
#define MESH_H

#include "sol_base.h"
#include "rhi.h"

/* a GPU mesh: vertex buffer + index buffer + index count */
typedef struct { RhiBuffer vbuffer; RhiBuffer ibuffer; int index_count; } Mesh;

/* CPU-side builder — generalizes the FloatArray idiom into vertices (8 floats
   each, the canonical pos+normal+uv layout) plus sol_u32 indices. The seed of
   all procedural/CAD geometry: emitters are clients of it. */
typedef struct {
    sol_f32 *vertices;  sol_u32 vertex_count;  sol_u32 vertex_cap;   /* 8 floats each */
    sol_u32 *indices;   sol_u32 index_count;   sol_u32 index_cap;
} MeshBuilder;

void    mb_init(MeshBuilder *b);
void    mb_free(MeshBuilder *b);
sol_u32 mb_push_vertex(MeshBuilder *b, sol_f32 px, sol_f32 py, sol_f32 pz,
                       sol_f32 nx, sol_f32 ny, sol_f32 nz, sol_f32 u, sol_f32 v);
void    mb_push_triangle(MeshBuilder *b, sol_u32 a, sol_u32 i, sol_u32 c);

/* primitive emitters (more to come: cylinder, arch, ...) */
void    make_box(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 d);

/* upload a finished builder to GPU buffers (uses the RHI, never GL) */
Mesh    mesh_from_builder(const MeshBuilder *b);

#endif /* MESH_H */
