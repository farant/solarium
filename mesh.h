#ifndef MESH_H
#define MESH_H

#include "sol_base.h"
#include "sol_types.h"
#include "rhi.h"

/* a GPU mesh: vertex/index buffers, index count, and a local-space AABB (for
   the item-4 picker; computed at build time since CPU verts are freed after) */
typedef struct { RhiBuffer vbuffer; RhiBuffer ibuffer; int index_count; Aabb bounds; } Mesh;

/* CPU-side builder — generalizes the FloatArray idiom into vertices (12 floats
   each: pos3 + normal3 + uv2 + tangent4, the canonical layout) plus sol_u32
   indices. The seed of all procedural/CAD geometry: emitters are clients of it.
   mb_push_vertex still takes pos/normal/uv; the tangent is filled later by
   mb_compute_tangents (item 8d). */
typedef struct {
    sol_f32 *vertices;  sol_u32 vertex_count;  sol_u32 vertex_cap;   /* 12 floats each */
    sol_u32 *indices;   sol_u32 index_count;   sol_u32 index_cap;
} MeshBuilder;

void    mb_init(MeshBuilder *b);
void    mb_free(MeshBuilder *b);
sol_u32 mb_push_vertex(MeshBuilder *b, sol_f32 px, sol_f32 py, sol_f32 pz,
                       sol_f32 nx, sol_f32 ny, sol_f32 nz, sol_f32 u, sol_f32 v);
void    mb_push_triangle(MeshBuilder *b, sol_u32 a, sol_u32 i, sol_u32 c);

/* fill each vertex's tangent (xyz + handedness w) from positions + UVs + topology */
void    mb_compute_tangents(MeshBuilder *b);

/* primitive emitters (more to come: cylinder, arch, ...) */
void    make_box(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 d);
void    make_plane(MeshBuilder *b, sol_f32 w, sol_f32 d);
void    make_grid(MeshBuilder *b, sol_f32 w, sol_f32 d, sol_u32 subdiv);
void    make_page(MeshBuilder *b, sol_f32 w, sol_f32 h);   /* upright XY quad, +Z, upright UVs */

/* mesh-ref resolver, CPU half: ref name -> emitter call into b (the single
   source of truth for what "box"/"grid"/"page" mean). SOL_FALSE = unknown ref;
   the caller keeps the object as an empty (placed data is never destroyed). */
sol_bool mesh_ref_build(const char *ref, MeshBuilder *b);

/* upload a finished builder to GPU buffers (computes tangents first, then uploads
   via the RHI, never GL). Mutates b (writes the tangents); b is freed after. */
Mesh    mesh_from_builder(MeshBuilder *b);

#endif /* MESH_H */
