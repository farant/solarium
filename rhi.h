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
typedef struct { sol_u32 id; } RhiTexture;
typedef struct { sol_u32 id; } RhiRenderTarget;

/* ---- enums ---- */
typedef enum { RHI_BUFFER_VERTEX, RHI_BUFFER_INDEX } RhiBufferType;
typedef enum { RHI_FORMAT_FLOAT2, RHI_FORMAT_FLOAT3 } RhiVertexFormat;

/* Color space / precision is part of a texture's identity (decided at creation,
   per §1.5):
   SRGB8   — color/albedo/page images (GPU converts sRGB->linear on sample);
   RGBA8   — linear data maps (normal/roughness/masks; sampled raw);
   RGBA16F — float HDR render-target color: values may exceed 1.0 without
             clipping (the point of the offscreen pass; item 7). */
typedef enum { RHI_TEX_SRGB8, RHI_TEX_RGBA8, RHI_TEX_RGBA16F } RhiTextureFormat;

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
RhiTexture  rhi_create_texture(const void *pixels, int width, int height, RhiTextureFormat fmt);  /* RGBA8 source */

/* An offscreen render target: a framebuffer wiring a samplable color texture
   (color_format, typically RHI_TEX_RGBA16F) + a write-only depth renderbuffer.
   Created at a fixed size; recreate on window resize. All framebuffer GL lives
   in the backend (§1.2). Returns id 0 on failure. */
RhiRenderTarget rhi_create_render_target(int width, int height, RhiTextureFormat color_format);

/* ---- per frame ---- */
/* Begin a render pass targeting `target`: bind its framebuffer, set the viewport
   to its size, and clear color+depth. A zero target ({0}) means the default
   framebuffer (the window) — the backend queries the window's size itself.
   rhi_end_pass restores the default framebuffer. */
void rhi_begin_pass(RhiRenderTarget target, float r, float g, float b, float a);
void rhi_end_pass(void);
void rhi_set_pipeline(RhiPipeline pipeline);               /* binds shader + layout + state */
void rhi_bind_vertex_buffer(RhiBuffer buffer);
void rhi_bind_index_buffer(RhiBuffer buffer);
void rhi_bind_texture(RhiTexture texture, int slot);            /* to texture unit `slot` */
void rhi_set_uniform_mat4(const char *name, const float *m);     /* on the bound pipeline */
void rhi_set_uniform_mat3(const char *name, const float *m);     /* e.g. the normal matrix */
void rhi_set_uniform_vec3(const char *name, float x, float y, float z);
void rhi_set_uniform_float(const char *name, float v);
void rhi_set_uniform_int(const char *name, int v);               /* e.g. a sampler's texture unit */
void rhi_draw(int first_vertex, int vertex_count);
void rhi_draw_indexed(int first_index, int index_count);

/* The render target's color attachment as a bindable texture, so a later pass
   can sample what was just rendered (the fullscreen tonemap pass samples this). */
RhiTexture rhi_render_target_texture(RhiRenderTarget rt);

void rhi_present(void);

/* ---- resource teardown ---- */
void rhi_destroy_buffer(RhiBuffer buffer);
void rhi_destroy_shader(RhiShader shader);
void rhi_destroy_pipeline(RhiPipeline pipeline);
void rhi_destroy_texture(RhiTexture texture);
void rhi_destroy_render_target(RhiRenderTarget rt);

#endif /* RHI_H */
