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

/* primitive emitters (more to come: arch, column, vault, stair — the gothic kit) */
void    make_box(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 d);
void    make_plane(MeshBuilder *b, sol_f32 w, sol_f32 d);
void    make_grid(MeshBuilder *b, sol_f32 w, sol_f32 d, sol_u32 subdiv);
void    make_page(MeshBuilder *b, sol_f32 w, sol_f32 h);   /* upright XY quad, +Z, upright UVs */

/* A room shell (P3 items 5+7): floor + optional walls/ceiling, all faces
   INTERIOR (inward normals, inward winding — the viewer is inside), origin at
   the floor's center, y in [0,h]. World-scale UVs (1 unit per meter) so texel
   density is uniform across differently-sized rooms. The presence flags are
   the follies vocabulary: a sealed cell, a three-walled stage, or a bare
   platform are all the same emitter. The floor is always present. */
void    make_room(MeshBuilder *b, sol_f32 w, sol_f32 d, sol_f32 h,
                  int wall_n, int wall_e, int wall_s, int wall_w, int ceil);

/* A walkable slab (P3 item 7): a room-graph edge embodied — length along X,
   width along Z, deck at y=0 (places flush with room floors). */
void    make_path(MeshBuilder *b, sol_f32 len, sol_f32 w, sol_f32 t);

/* An index card (P3 item 6): a FILE/ALIAS/NOTE's body — an upright slab
   standing on its bottom edge, facing +/-Z. */
void    make_card(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 t);

/* The codex (item 9) — geometry from real bookbinding; see the construction
   notes in mesh.c. Closed frame: the book LIES on its back cover (x: 0 =
   spine -> w = fore-edge; y: 0..t; z: +-h/2) — stand it with the object's
   rotation. Cover and block are separate refs (separate materials). */
void    make_book_cover(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 t,
                        sol_f32 board, sol_f32 sq, sol_f32 round_,
                        int bands, int clasp);
void    make_book_block(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 t,
                        sol_f32 board, sol_f32 sq);
/* Open frame: gutter at x=0, spread spans -w..+w; each page fan rises out
   of the gutter pinch to the flat TEXT FIELD where wtext sets the page.
   The rise occupies this fraction of the page width — geometry (the fan
   profile) and layout (where text may sit) must agree, so it lives here. */
#define BOOK_GUTTER_FRAC 0.15f
void    make_book_open_cover(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 t,
                             sol_f32 board, sol_f32 sq);
void    make_book_open_block(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 t,
                             sol_f32 board, sol_f32 sq);

/* Board ink (item 8): a flat arrow in the XY plane (z=0), single-sided
   facing +Z. DERIVED geometry — endpoints come from two cards' positions,
   so it is rebuilt as they move, never serialized. Emits nothing when the
   segment is shorter than 2w. */
void    make_arrow(MeshBuilder *b, sol_f32 x0, sol_f32 y0,
                   sol_f32 x1, sol_f32 y1, sol_f32 w);

/* A wall with a doorway, built as the pieces AROUND the gap — left panel,
   right panel, header — never a boolean cut (§1.4). Real thickness t (centered
   on the XY plane): each piece is a box emitting only its EXPOSED faces, incl.
   the doorway's jambs + lintel underside, with abutting faces skipped so
   nothing is coplanar (coplanar quads z-fight). x in [-w/2, w/2], y in [0,h];
   the opening starts ox from the LEFT edge, ow wide, oh tall from the floor.
   Degenerate pieces (an opening flush with an edge) are skipped, not emitted. */
void    make_wall_with_opening(MeshBuilder *b, sol_f32 w, sol_f32 h,
                               sol_f32 ox, sol_f32 ow, sol_f32 oh, sol_f32 t);

/* The mesh-ref registry, CPU half (P3 items 1+5): ref name -> emitter, THE
   single source of truth for what each name means. Item 5 made it a SCHEMA:
   each entry declares its parameter names + defaults, so the scene file can
   carry them as self-describing attributes (<mesh ref="room" w="6" .../>)
   and the writer/loader/resolver all read one table. */
#define MESH_REF_MAX_PARAMS 8

/* Schema lookup: the entry's parameter names + defaults (borrowed pointers,
   static lifetime). Returns the parameter count, or -1 for an unknown ref. */
int mesh_ref_schema(const char *ref, const char *const **names, const float **defaults);

/* Build the named geometry into b. params[0..count) is a PREFIX of the
   schema's parameters; the tail fills from the defaults (so files written
   before a schema grew keep working — pass NULL/0 for all-defaults).
   SOL_FALSE = unknown ref; the caller keeps the object as an empty (placed
   data is never destroyed). */
sol_bool mesh_ref_build(const char *ref, const float *params, int count, MeshBuilder *b);

/* One effective parameter by name, same prefix+defaults merge as _build —
   for reading a dimension without emitting (a board's w/h for the drag
   rect-test). Unknown ref/name -> 0. */
float mesh_ref_param(const char *ref, const float *params, int count, const char *name);

/* upload a finished builder to GPU buffers (computes tangents first, then
   uploads via the RHI, never GL). Mutates b (writes the tangents); b is freed
   after. Lives in mesh_gpu.c so everything above stays pure CPU — emitters
   and the registry are headless-testable and linkable by scene_io. */
Mesh    mesh_from_builder(MeshBuilder *b);

/* Release a mesh's GPU buffers and zero it — for DERIVED meshes (arrows)
   that rebuild as their sources move; everything else lives for the session. */
void    mesh_destroy(Mesh *m);

#endif /* MESH_H */
