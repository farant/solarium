/* glb.h — minimal glTF binary (.glb) loader: container + accessor decode into
   our Mesh(es). Static subset only (item 6); above the seam (uploads via mesh.c,
   never GL). Materials/node transforms come in 6c/6d. */
#ifndef GLB_H
#define GLB_H

#include "sol_base.h"
#include "rhi.h"
#include "mesh.h"
#include "material.h"
#include "skel.h"

/* one glTF primitive: geometry + its resolved PBR material */
typedef struct { Mesh mesh; Material material; } GlbPart;

typedef struct {
    GlbPart *parts;
    sol_u32  count;
} GlbModel;

sol_bool glb_load(const char *path, GlbModel *out);   /* SOL_FALSE on any failure */
void     glb_free(GlbModel *out);                      /* frees the array (GPU meshes persist) */

/* skin 0 + every animation -> compact SkelData (P4 item 9). Pure CPU —
   no upload happens on this path. Caller owns the result
   (skel_data_free). SOL_FALSE if the file has no skin (or > 64 joints,
   or is corrupt) — out is left freed-and-zeroed. */
sol_bool glb_load_skeleton(const char *path, SkelData *out);

/* the whole rigged model (item 9 piece 2): the skinned primitive UNBAKED
   (20-float layout, mesh-local bind space) + its material + the skeleton.
   Caller owns: mesh_destroy + skel_data_free. */
typedef struct {
    Mesh     mesh;
    Material material;
    SkelData rig;        /* rig.skel = the Skeleton, rig.clips = the dances */
} SkinnedModel;

sol_bool glb_load_skinned(const char *path, SkinnedModel *out);

#endif /* GLB_H */
