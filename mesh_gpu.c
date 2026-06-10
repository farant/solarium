/* mesh_gpu.c — the GPU half of the mesh module: upload a finished builder.
   Split from mesh.c in P3 item 5 so the emitters + ref registry stay pure
   CPU (headless-testable; scene_io links them for the param schema without
   dragging the RHI into the tests). Above the seam: rhi_*, never GL. */

#include "mesh.h"

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
