/* glb.h — minimal glTF binary (.glb) loader: container + accessor decode into
   our Mesh(es). Static subset only (item 6); above the seam (uploads via mesh.c,
   never GL). Materials/node transforms come in 6c/6d. */
#ifndef GLB_H
#define GLB_H

#include "sol_base.h"
#include "rhi.h"
#include "mesh.h"

/* one glTF primitive: geometry + its base-color texture (albedo.id 0 = none) */
typedef struct { Mesh mesh; RhiTexture albedo; } GlbPart;

typedef struct {
    GlbPart *parts;
    sol_u32  count;
} GlbModel;

sol_bool glb_load(const char *path, GlbModel *out);   /* SOL_FALSE on any failure */
void     glb_free(GlbModel *out);                      /* frees the array (GPU meshes persist) */

#endif /* GLB_H */
