/* bvh.c — see bvh.h. Pure boxes-and-ids; no scene, no GL. C89. */

#include "bvh.h"
#include "sol_math.h"   /* ray_vs_aabb */

#include <stdlib.h>     /* malloc, realloc, free, qsort */
#include <string.h>     /* memcpy */

#define BVH_LEAF_MAX 4  /* items per leaf: smaller = deeper tree, more node
                           tests; larger = fatter leaves, more callback work */

typedef struct {
    float key;          /* centroid along the split axis */
    int   idx;          /* position in the original arrays */
} BvhSortKey;

void bvh_init(Bvh *b) {
    b->nodes = NULL;  b->node_count = 0;  b->node_cap = 0;
    b->order = NULL;  b->boxes = NULL;    b->ids = NULL;
    b->count = 0;     b->cap = 0;
}

void bvh_free(Bvh *b) {
    free(b->nodes);
    free(b->order);
    free(b->boxes);
    free(b->ids);
    bvh_init(b);
}

static Aabb aabb_union(Aabb a, Aabb b) {
    Aabb u;
    u.min.x = a.min.x < b.min.x ? a.min.x : b.min.x;
    u.min.y = a.min.y < b.min.y ? a.min.y : b.min.y;
    u.min.z = a.min.z < b.min.z ? a.min.z : b.min.z;
    u.max.x = a.max.x > b.max.x ? a.max.x : b.max.x;
    u.max.y = a.max.y > b.max.y ? a.max.y : b.max.y;
    u.max.z = a.max.z > b.max.z ? a.max.z : b.max.z;
    return u;
}

/* allocate a node, growing the array; returns its INDEX — never hold a
   node POINTER across a child build (the realloc moves the array) */
static int node_alloc(Bvh *b) {
    if (b->node_count == b->node_cap) {
        int      ncap = b->node_cap ? b->node_cap * 2 : 64;
        BvhNode *nn   = (BvhNode *)realloc(b->nodes, (size_t)ncap * sizeof *nn);
        if (nn == NULL) return -1;
        b->nodes    = nn;
        b->node_cap = ncap;
    }
    return b->node_count++;
}

static int key_cmp(const void *pa, const void *pb) {
    const BvhSortKey *a = (const BvhSortKey *)pa;
    const BvhSortKey *kb = (const BvhSortKey *)pb;
    if (a->key < kb->key) return -1;
    if (a->key > kb->key) return  1;
    return 0;
}

static float box_centroid_axis(const Aabb *box, int axis) {
    if (axis == 0) return (box->min.x + box->max.x) * 0.5f;
    if (axis == 1) return (box->min.y + box->max.y) * 0.5f;
    return (box->min.z + box->max.z) * 0.5f;
}

/* recursive median-split: parents get LOWER indices than their children,
   which is what lets bvh_refit walk the array backward (children first) */
static int build_node(Bvh *b, int start, int count, BvhSortKey *scratch) {
    int  ni = node_alloc(b);
    Aabb box;
    int  i;
    if (ni < 0) return -1;

    box = b->boxes[b->order[start]];
    for (i = 1; i < count; i++)
        box = aabb_union(box, b->boxes[b->order[start + i]]);
    b->nodes[ni].box = box;

    if (count <= BVH_LEAF_MAX) {
        b->nodes[ni].start = start;
        b->nodes[ni].count = count;
        b->nodes[ni].left  = -1;
        b->nodes[ni].right = -1;
        return ni;
    }

    {
        /* split the longest axis of the CENTROID spread (the box spread can
           be dominated by one large object; centroids measure distribution) */
        float cmin[3], cmax[3];
        int   axis, mid, l, r;
        for (i = 0; i < 3; i++) { cmin[i] = 1e30f; cmax[i] = -1e30f; }
        for (i = 0; i < count; i++) {
            int a;
            for (a = 0; a < 3; a++) {
                float c = box_centroid_axis(&b->boxes[b->order[start + i]], a);
                if (c < cmin[a]) cmin[a] = c;
                if (c > cmax[a]) cmax[a] = c;
            }
        }
        axis = 0;
        if (cmax[1] - cmin[1] > cmax[axis] - cmin[axis]) axis = 1;
        if (cmax[2] - cmin[2] > cmax[axis] - cmin[axis]) axis = 2;

        for (i = 0; i < count; i++) {
            scratch[start + i].idx = b->order[start + i];
            scratch[start + i].key =
                box_centroid_axis(&b->boxes[b->order[start + i]], axis);
        }
        qsort(scratch + start, (size_t)count, sizeof(BvhSortKey), key_cmp);
        for (i = 0; i < count; i++)
            b->order[start + i] = scratch[start + i].idx;

        mid = count / 2;                     /* median: halves stay balanced */
        l = build_node(b, start, mid, scratch);
        r = build_node(b, start + mid, count - mid, scratch);
        if (l < 0 || r < 0) return -1;
        b->nodes[ni].left  = l;              /* re-index AFTER the children:
                                                their builds may have moved
                                                the nodes array */
        b->nodes[ni].right = r;
        b->nodes[ni].start = 0;
        b->nodes[ni].count = 0;
        return ni;
    }
}

void bvh_build(Bvh *b, const Aabb *boxes, const sol_u32 *ids, int count) {
    BvhSortKey *scratch;
    int         i;

    b->node_count = 0;
    b->count      = 0;
    if (count <= 0) return;

    if (count > b->cap) {
        Aabb    *nb = (Aabb *)realloc(b->boxes, (size_t)count * sizeof *nb);
        sol_u32 *ni = (sol_u32 *)realloc(b->ids, (size_t)count * sizeof *ni);
        int     *no = (int *)realloc(b->order, (size_t)count * sizeof *no);
        if (nb) b->boxes = nb;
        if (ni) b->ids   = ni;
        if (no) b->order = no;
        if (!nb || !ni || !no) return;       /* OOM: empty tree, callers scan */
        b->cap = count;
    }
    memcpy(b->boxes, boxes, (size_t)count * sizeof *boxes);
    memcpy(b->ids,   ids,   (size_t)count * sizeof *ids);
    for (i = 0; i < count; i++) b->order[i] = i;
    b->count = count;

    scratch = (BvhSortKey *)malloc((size_t)count * sizeof *scratch);
    if (scratch == NULL) { b->count = 0; return; }
    if (build_node(b, 0, count, scratch) < 0) b->count = 0;
    free(scratch);
}

void bvh_refit(Bvh *b, const Aabb *boxes, int count) {
    int n;
    if (count != b->count || count == 0) return;   /* contract: same set */
    memcpy(b->boxes, boxes, (size_t)count * sizeof *boxes);

    /* children always sit at higher indices than their parents (build
       order), so one backward sweep refits everything bottom-up */
    for (n = b->node_count - 1; n >= 0; n--) {
        BvhNode *nd = &b->nodes[n];
        if (nd->count > 0) {
            int  i;
            Aabb box = b->boxes[b->order[nd->start]];
            for (i = 1; i < nd->count; i++)
                box = aabb_union(box, b->boxes[b->order[nd->start + i]]);
            nd->box = box;
        } else {
            nd->box = aabb_union(b->nodes[nd->left].box,
                                 b->nodes[nd->right].box);
        }
    }
}

static float ray_walk(const Bvh *b, int ni, Ray ray, float best,
                      BvhRayFn fn, void *ctx) {
    const BvhNode *nd = &b->nodes[ni];
    if (nd->count > 0) {
        int i;
        for (i = 0; i < nd->count; i++)
            best = fn(b->ids[b->order[nd->start + i]], best, ctx);
        return best;
    }
    {
        /* nearer child first; re-check the prune between the two, because
           the first visit may have sharpened best */
        float    tl = 0.0f, tr = 0.0f;
        sol_bool hl = ray_vs_aabb(ray, b->nodes[nd->left].box,  &tl);
        sol_bool hr = ray_vs_aabb(ray, b->nodes[nd->right].box, &tr);
        int      first  = nd->left,  second = nd->right;
        float    tfirst = tl,        tsecond = tr;
        sol_bool hfirst = hl,        hsecond = hr;
        if (hl && hr && tr < tl) {
            first  = nd->right;  second = nd->left;
            tfirst = tr;         tsecond = tl;
        }
        if (hfirst && tfirst < best)
            best = ray_walk(b, first, ray, best, fn, ctx);
        if (hsecond && tsecond < best)
            best = ray_walk(b, second, ray, best, fn, ctx);
        return best;
    }
}

float bvh_ray_query(const Bvh *b, Ray ray, float best_t, BvhRayFn fn, void *ctx) {
    float t;
    if (b->count == 0 || b->node_count == 0) return best_t;
    if (!ray_vs_aabb(ray, b->nodes[0].box, &t) || t >= best_t) return best_t;
    return ray_walk(b, 0, ray, best_t, fn, ctx);
}
