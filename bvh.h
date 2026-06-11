/* bvh.h — bounding volume hierarchy (P4 item 2 piece 3): the spatial index.
   A tree of nested AABBs over a flat set of (box, id) pairs; one query rule
   pays for everything: if the query misses a node's box, the entire subtree
   vanishes from consideration. The structure follows the OBJECTS, not space
   — a packed room becomes a deep local subtree, the void between islands
   costs nothing — which is why it fits the palace where a uniform grid
   would not.

   Deliberately PURE: this module knows boxes and ids, never the Scene (the
   ids are the caller's — scene handles in practice). Picking traverses it
   here (bvh_ray_query); frustum culling (piece 4) adds its own walk. Build
   is median-split over the longest centroid axis — the simple choice; SAH
   is a refinement thousands of objects don't need. When objects MOVE,
   REFIT: recompute node bounds bottom-up without restructuring (cheap; the
   tree degrades only slowly as things migrate — a fresh build on load
   resets it). Strict C89, no GL, headless-tested in pick_test. */

#ifndef BVH_H
#define BVH_H

#include "sol_base.h"
#include "sol_types.h"

typedef struct {
    Aabb box;
    int  left, right;    /* children, internal nodes only */
    int  start, count;   /* leaf: a range of order[]; count > 0 marks a leaf */
} BvhNode;

typedef struct {
    BvhNode *nodes;
    int      node_count, node_cap;
    int     *order;      /* permutation of [0, count): leaves index through it */
    Aabb    *boxes;      /* the caller's boxes, copied (refit updates them)  */
    sol_u32 *ids;
    int      count, cap;
} Bvh;

void bvh_init(Bvh *b);
void bvh_free(Bvh *b);

/* Build from scratch over count (box, id) pairs (copied in). Call when the
   SET changes (load, add, remove); for pure motion prefer bvh_refit. */
void bvh_build(Bvh *b, const Aabb *boxes, const sol_u32 *ids, int count);

/* Refit: the same items in the same order as the last build, with new
   boxes — node bounds recompute bottom-up, topology untouched. */
void bvh_refit(Bvh *b, const Aabb *boxes, int count);

/* Ray query, nearest-first: walks the tree visiting the closer child before
   the farther one, pruning any node whose entry distance can't beat best_t,
   and calls fn for each candidate id. fn does the caller's own precise test
   (triangles, a tighter box, anything) and returns the possibly-improved
   best_t — the traversal prunes harder as the caller's answer sharpens.
   Returns the final best_t. */
typedef float (*BvhRayFn)(sol_u32 id, float best_t, void *ctx);
float bvh_ray_query(const Bvh *b, Ray ray, float best_t, BvhRayFn fn, void *ctx);

#endif /* BVH_H */
