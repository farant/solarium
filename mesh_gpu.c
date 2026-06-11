/* mesh_gpu.c — the GPU half of the mesh module: upload a finished builder.
   Split from mesh.c in P3 item 5 so the emitters + ref registry stay pure
   CPU (headless-testable; scene_io links them for the param schema without
   dragging the RHI into the tests). Above the seam: rhi_*, never GL. */

#include "mesh.h"

void mesh_destroy(Mesh *m) {
    if (m->vbuffer.id) {
        mesh_geom_drop(m->vbuffer.id);       /* retained triangles go with it */
        rhi_destroy_buffer(m->vbuffer);
    }
    if (m->ibuffer.id) rhi_destroy_buffer(m->ibuffer);
    m->vbuffer.id  = 0;
    m->ibuffer.id  = 0;
    m->index_count = 0;
}

Mesh mesh_from_builder(MeshBuilder *b) {
    Mesh m;
    mb_compute_tangents(b);                  /* finalize tangents before upload */
    m.vbuffer = rhi_create_buffer(RHI_BUFFER_VERTEX, b->vertices,
                    (size_t)b->vertex_count * 12 * sizeof(sol_f32));
    m.ibuffer = rhi_create_buffer(RHI_BUFFER_INDEX, b->indices,
                    (size_t)b->index_count * sizeof(sol_u32));
    m.index_count = (int)b->index_count;
    mesh_geom_register(m.vbuffer.id, b);     /* P4 item 2: triangles retained
                                                at THE upload chokepoint —
                                                every path goes through here */

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

Mesh mesh_from_skinned(const sol_f32 *verts, sol_u32 vert_count,
                       const sol_u32 *indices, sol_u32 index_count) {
    Mesh    m;
    sol_u32 i;
    m.vbuffer = rhi_create_buffer(RHI_BUFFER_VERTEX, verts,
                    (size_t)vert_count * 20 * sizeof(sol_f32));
    m.ibuffer = rhi_create_buffer(RHI_BUFFER_INDEX, indices,
                    (size_t)index_count * sizeof(sol_u32));
    m.index_count = (int)index_count;
    /* NO mesh_geom_register: a deforming mesh has no one triangle truth —
       picking falls back to this BIND-pose AABB if it's ever asked */
    if (vert_count == 0) {
        m.bounds.min.x = m.bounds.min.y = m.bounds.min.z = 0.0f;
        m.bounds.max = m.bounds.min;
    } else {
        sol_f32 minx, miny, minz, maxx, maxy, maxz;
        minx = maxx = verts[0];
        miny = maxy = verts[1];
        minz = maxz = verts[2];
        for (i = 1; i < vert_count; i++) {
            sol_f32 x = verts[i*20+0], y = verts[i*20+1], z = verts[i*20+2];
            if (x < minx) minx = x;  if (x > maxx) maxx = x;
            if (y < miny) miny = y;  if (y > maxy) maxy = y;
            if (z < minz) minz = z;  if (z > maxz) maxz = z;
        }
        m.bounds.min.x = minx; m.bounds.min.y = miny; m.bounds.min.z = minz;
        m.bounds.max.x = maxx; m.bounds.max.y = maxy; m.bounds.max.z = maxz;
    }
    return m;
}
