#ifndef RHI_H
#define RHI_H

#include <stddef.h>   /* size_t, NULL */
#include "sol_base.h"

/* The platform window, owned by the app (GLFW), not by the RHI.
   Forward-declared so rhi.h pulls in neither GLFW nor any GL header. */
struct GLFWwindow;

/* ---- opaque resource handles (id 0 == invalid) ---- */
typedef struct { sol_u32 id; } RhiBuffer;
typedef struct { sol_u32 id; } RhiShader;
typedef struct { sol_u32 id; } RhiPipeline;
typedef struct { sol_u32 id; } RhiTexture;   /* stubbed — for when step-3 textures return */

/* ---- enums ---- */
typedef enum { RHI_BUFFER_VERTEX, RHI_BUFFER_INDEX } RhiBufferType;
typedef enum { RHI_FORMAT_FLOAT2, RHI_FORMAT_FLOAT3 } RhiVertexFormat;

/* ---- vertex layout + pipeline description ---- */
typedef struct {
    int             location;   /* shader input slot (layout location) */
    RhiVertexFormat format;     /* float2 / float3 */
    size_t          offset;     /* byte offset within one vertex */
} RhiVertexAttr;

enum { RHI_MAX_ATTRS = 8 };

typedef struct {
    RhiShader     shader;
    RhiVertexAttr attrs[RHI_MAX_ATTRS];
    int           attr_count;
    size_t        stride;        /* bytes per vertex */
    sol_bool      depth_test;    /* render state, bundled into the pipeline */
} RhiPipelineDesc;

/* ---- lifecycle (this is where the window/context split lives) ---- */
void     rhi_configure_window(void);          /* BEFORE glfwCreateWindow: set API/context hints */
sol_bool rhi_init(struct GLFWwindow *window); /* AFTER: make context current; SOL_FALSE on failure */
void rhi_shutdown(void);

/* ---- resource creation (up front) ---- */
RhiBuffer   rhi_create_buffer(RhiBufferType type, const void *data, size_t size);
RhiShader   rhi_create_shader(const char *vertex_src, const char *fragment_src);
RhiPipeline rhi_create_pipeline(const RhiPipelineDesc *desc);

/* ---- per frame ---- */
void rhi_begin_frame(int fb_width, int fb_height,
                     float r, float g, float b, float a);  /* viewport + clear color & depth */
void rhi_set_pipeline(RhiPipeline pipeline);               /* binds shader + layout + state */
void rhi_bind_vertex_buffer(RhiBuffer buffer);
void rhi_bind_index_buffer(RhiBuffer buffer);
void rhi_set_uniform_mat4(const char *name, const float *m);     /* on the bound pipeline */
void rhi_set_uniform_vec3(const char *name, float x, float y, float z);
void rhi_draw(int first_vertex, int vertex_count);
void rhi_draw_indexed(int first_index, int index_count);
void rhi_present(void);

/* ---- resource teardown ---- */
void rhi_destroy_buffer(RhiBuffer buffer);
void rhi_destroy_shader(RhiShader shader);
void rhi_destroy_pipeline(RhiPipeline pipeline);
void rhi_destroy_texture(RhiTexture texture);

#endif /* RHI_H */
