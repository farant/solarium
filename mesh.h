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

/* scale every vertex's UV by s (post-build tiling control: smaller s => the
   texture covers more world space => bigger apparent tiles) */
void    mb_scale_uvs(MeshBuilder *b, sol_f32 s);

/* primitive emitters (more to come: arch, column, vault, stair — the gothic kit) */
void    make_box(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 d);
void    make_plane(MeshBuilder *b, sol_f32 w, sol_f32 d);
void    make_grid(MeshBuilder *b, sol_f32 w, sol_f32 d, sol_u32 subdiv);
void    make_page(MeshBuilder *b, sol_f32 w, sol_f32 h);   /* upright XY quad, +Z, upright UVs */
void    make_picture(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 t);  /* bottom-origin quad, +Z, 0..1 UVs (no tile) */
/* like make_picture but with an explicit UV crop window (for map boards that
   sample a sub-rectangle of a shared equirectangular basemap). Bottom-origin
   +Z quad, w x h meters, UVs (u0,v0)=bottom-left .. (u1,v1)=top-right. */
void    make_map_quad(MeshBuilder *b, sol_f32 w, sol_f32 h,
                      sol_f32 u0, sol_f32 v0, sol_f32 u1, sol_f32 v1);
#define WINDOW_FRAME_W  0.08f   /* window frame border; the "window" registry "fw" default must match this */
void    make_window(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 t, sol_f32 fw, sol_f32 style);
void    make_window_fill(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 t, sol_f32 fw, sol_f32 style);
void    make_window_glass(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 style);

/* A walkable slab (P3 item 7): a room-graph edge embodied — length along X,
   width along Z, deck at y=0 (places flush with room floors). */
void    make_path(MeshBuilder *b, sol_f32 len, sol_f32 w, sol_f32 t);

/* A connector that climbs: a ribbon of N step-boxes from y=0 up to y=dy over
   `len` along X (width `w` along Z, slabs `t` thick). dy~0 => one flat box (a
   walkway); larger dy => stairs (each step's rise stays climbable). The
   per-step rise lives here so the collider mirrors the emitter exactly. */
#define WALKWAY_STEP_RISE 0.18f
void    make_walkway(MeshBuilder *b, sol_f32 len, sol_f32 w, sol_f32 t, sol_f32 dy);

/* wall ids for a doored room: N=-z, E=+x, S=+z, W=-x (matches make_room_doored) */
#define ROOM_WALL_N 0
#define ROOM_WALL_E 1
#define ROOM_WALL_S 2
#define ROOM_WALL_W 3

/* A gap in a room wall. `center` is the door-center offset along the wall's
   run axis, in ROOM-LOCAL coords (x for N/S walls, z for E/W walls); width
   and height are the opening size. Pure geometry -- no scene knowledge. */
typedef struct {
    int     wall;
    sol_f32 center;
    sol_f32 width;
    sol_f32 height;    /* top of the opening (lintel) above the floor */
    sol_f32 sill;      /* bottom of the opening above the floor; 0 = door (reaches floor) */
} RoomOpening;

/* The most gaps one wall can carry (the doored-wall scratch arrays size to it). */
#define ROOM_MAX_OPENINGS_PER_WALL 8

/* A thick-walled room built AROUND its door gaps (never CSG): floor always,
   ceiling if `ceil`, a thick slab per present wall (wn/we/ws/ww) with the
   openings for that wall cut as solid minus the union of opening holes (a
   vertical slab sweep), so overlapping/stacked openings don't fill each
   other; the floor fills each doorway's threshold strip). */
void make_room_doored(MeshBuilder *b, sol_f32 w, sol_f32 d, sol_f32 h, sol_f32 t,
                      int wn, int we, int ws, int ww, int ceil,
                      const RoomOpening *ops, int n_ops);

/* An L-shaped (or straight) walkway in LOCAL space: the lower door is the
   origin (0,0,0); the path bends at (cx,cz,cy) and ends at (ex,ez,ey). Each
   leg is axis-aligned and stepped to climb to its end height; a flat landing
   caps the corner (skipped when a leg is zero-length, i.e. a straight path). */
void make_walkway_L(MeshBuilder *b, sol_f32 cx, sol_f32 cz, sol_f32 cy,
                    sol_f32 ex, sol_f32 ez, sol_f32 ey, sol_f32 w, sol_f32 t);

/* An edge fascia/curb running along both sides of the same L-walkway (same
   local space + leg/step structure as make_walkway_L): it juts `over` past the
   deck edge, rises `ch` above the deck top, and wraps DOWN the full block side
   (deck thickness `dt`) so it covers the deck's side faces. cw = rail cross-
   section thickness. Its OWN mesh, so it can wear a different (dark-wood)
   material than the marble deck. */
void make_walkway_trim(MeshBuilder *b, sol_f32 cx, sol_f32 cz, sol_f32 cy,
                       sol_f32 ex, sol_f32 ez, sol_f32 ey,
                       sol_f32 w, sol_f32 cw, sol_f32 ch, sol_f32 over, sol_f32 dt);

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

/* A closed book seen cover-out, pinned to a board like a thick card:
   width along X (centered -w/2..w/2), height along Y (bottom-origin 0..h),
   depth along Z (0..d, +Z out of the board face). The big cover faces +Z;
   the spine (left edge) wears `bands` raised cords. One mesh, one material. */
void make_folderbook(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 d, int bands);

/* Terrain (item 10): a floating plot — seeded-fBm heightfield top, masked
   to a ZERO RIM at the border (an island, not a world), over a skirt and
   base slab. Centered: local x in [-w/2,w/2], z in [-d/2,d/2]. */
void    make_terrain(MeshBuilder *b, sol_f32 w, sol_f32 d, int sub,
                     sol_f32 amp, unsigned seed);

/* A campus pad: one room's footprint + its floor height, in campus-LOCAL coords
   (relative to the campus rectangle centre). floor_y is absolute world Y. */
typedef struct { float cx, cz, hw, hd, floor_y; } CampusPad;
#define CAMPUS_MAX_PADS 1024

/* Campus heightfield: per-room flat pads (footprint at floor_y) blended with
   fBm hills, lowest-pad-wins at overlaps, faded to 0 at the rim (so the skirt
   works like make_terrain). w/d = rectangle size; amp = hill amplitude. */
float campus_height(const CampusPad *pads, int npads,
                    float w, float d, float amp, unsigned seed,
                    float lx, float lz);

/* 1 if (lx,lz) is within `clear` of ANY pad's footprint (a room or a walkway
   corridor) — used to keep flora off buildings and paths. */
int campus_point_blocked(const CampusPad *pads, int npads,
                         float lx, float lz, float clear);

/* Build the campus terrain mesh (top grid + skirt + base), sampling
   campus_height. sub = tessellation (clamped 2..96). */
void  make_campus(MeshBuilder *b, const CampusPad *pads, int npads,
                  float w, float d, int sub, float amp, unsigned seed);

/* THE height function — the one source of truth the emitter's vertices,
   their finite-difference normals, AND the standing/placement queries all
   evaluate (geometry and physics agree by construction). Params follow
   the registry's prefix+defaults merge; outside the plot returns 0. */
float   terrain_height(const float *params, int count, float lx, float lz);

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
#define MESH_REF_MAX_PARAMS 10   /* P7 item 4: the tree refs expose
                                    leaf_size/leaf_density at 8/9 — the
                                    cap rose with them; church/room stay 8 */

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
   that rebuild as their sources move; everything else lives for the session.
   Also drops the retained CPU geometry below. */
void    mesh_destroy(Mesh *m);

/* upload SKINNED vertex data (P4 item 9): 20 floats per vertex — the
   canonical 12 (pos3+normal3+uv2+tangent4) plus joints4 (indices as
   floats) + weights4. No retained CPU geometry: a deforming mesh has no
   single triangle truth; picking sees the bind-pose AABB. */
Mesh    mesh_from_skinned(const sol_f32 *verts, sol_u32 vert_count,
                          const sol_u32 *indices, sol_u32 index_count);

/* ---- retained CPU geometry (P4 item 2): triangles kept at upload ----
   Picking needs actual triangles (a doorway is a hole in the geometry but
   not in the AABB), so mesh_from_builder registers positions + indices
   here, keyed by the mesh's vbuffer id (shared meshes share one entry; the
   RHI's slot reuse means a destroyed id re-registers cleanly). Pure CPU,
   lives in mesh.c — headless tests register fake ids by hand. Positions
   are packed xyz (a third of the full vertex), in LOCAL space like
   mesh.bounds. id 0 (no buffer) never registers and never resolves. */
typedef struct {
    sol_f32 *pos;          /* xyz per vertex, packed */
    sol_u32  vert_count;
    sol_u32 *idx;
    sol_u32  idx_count;
} CpuGeom;

void           mesh_geom_register(sol_u32 vbuffer_id, const MeshBuilder *b);
void           mesh_geom_drop(sol_u32 vbuffer_id);
const CpuGeom *mesh_geom_get(sol_u32 vbuffer_id);

#endif /* MESH_H */
