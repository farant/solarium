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
typedef enum { RHI_FORMAT_FLOAT2, RHI_FORMAT_FLOAT3, RHI_FORMAT_FLOAT4 } RhiVertexFormat;

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
    sol_bool      blend;         /* straight-alpha "over": src*a + dst*(1-a).
                                    Set EXPLICITLY at every desc site (a stack
                                    desc's unset field is garbage, silently
                                    enabling blend on an opaque pass). */
} RhiPipelineDesc;

/* What rhi_begin_pass clears before drawing. RHI_CLEAR_NONE is a "load" pass
   (Vulkan/Metal loadOp=LOAD): draw OVER what the previous pass produced —
   how the 2D overlay composites onto the tonemapped frame (item 2), and how
   the whiteboard accumulates ink (item 8). */
enum {
    RHI_CLEAR_NONE  = 0,
    RHI_CLEAR_COLOR = 1,
    RHI_CLEAR_DEPTH = 2,
    RHI_CLEAR_ALL   = 3        /* color | depth */
};

/* ---- lifecycle (this is where the window/context split lives) ---- */
void     rhi_configure_window(void);          /* BEFORE glfwCreateWindow: set API/context hints */
sol_bool rhi_init(struct GLFWwindow *window); /* AFTER: make context current; SOL_FALSE on failure */
void rhi_shutdown(void);

/* ---- resource creation (up front) ---- */
RhiBuffer   rhi_create_buffer(RhiBufferType type, const void *data, size_t size);

/* Re-specify a buffer's entire contents (may also grow it). For per-frame
   vertex STREAMS (the 2D overlay batch): re-specifying rather than writing
   in place lets the driver hand back fresh storage ("orphaning") instead of
   stalling until the GPU finishes reading last frame's data. Meshes built
   once should keep using rhi_create_buffer. */
void        rhi_update_buffer(RhiBuffer buffer, const void *data, size_t size);
RhiShader   rhi_create_shader(const char *vertex_src, const char *fragment_src);
RhiPipeline rhi_create_pipeline(const RhiPipelineDesc *desc);
RhiTexture  rhi_create_texture(const void *pixels, int width, int height, RhiTextureFormat fmt);  /* RGBA8 source */

/* An HDR texture from linear float (RGBA) source -> RHI_TEX_RGBA16F. For
   equirectangular environment maps: wraps in S (longitude), clamps in T
   (poles), LINEAR, no mipmaps. The source stays linear radiance (never sRGB). */
RhiTexture  rhi_create_texture_hdr(const float *pixels, int width, int height);

/* A cubemap (6-face RGBA16F): bind it with rhi_bind_texture like any handle (the
   backend tracks the target). Render into a face/mip with rhi_begin_cubemap_face
   (+ rhi_end_pass); regenerate mips after filling face 0 with generate_mips.
   The foundation for environment maps / IBL (items B/C). */
RhiTexture  rhi_create_cubemap(int size, sol_bool mipmapped);
void        rhi_begin_cubemap_face(RhiTexture cube, int face, int mip, int size);
void        rhi_cubemap_generate_mips(RhiTexture cube);

/* An offscreen render target: a framebuffer wiring a samplable color texture
   (color_format, typically RHI_TEX_RGBA16F) + a write-only depth renderbuffer.
   Created at a fixed size; recreate on window resize. All framebuffer GL lives
   in the backend (§1.2). Returns id 0 on failure. */
RhiRenderTarget rhi_create_render_target(int width, int height, RhiTextureFormat color_format);

/* A depth-only render target: a framebuffer whose sole attachment is a
   *samplable* depth texture (no color buffer). The shadow map (item 9b)
   renders the scene's depth from the light's POV into this; a later pass
   samples its depth texture. Created at a fixed size. Returns id 0 on failure. */
RhiRenderTarget rhi_create_depth_target(int width, int height);

/* ---- per frame ---- */
/* Begin a render pass targeting `target`: bind its framebuffer, set the viewport
   to its size, and clear per `clear_flags` (RHI_CLEAR_*; the r,g,b,a are the
   clear color, ignored unless RHI_CLEAR_COLOR is set). A zero target ({0})
   means the default framebuffer (the window) — the backend queries the
   window's size itself. rhi_end_pass restores the default framebuffer. */
void rhi_begin_pass(RhiRenderTarget target, int clear_flags, float r, float g, float b, float a);
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

/* A depth target's depth attachment as a bindable texture (the shadow-map
   sample source for the lighting pass). Returns id 0 for a color target. */
RhiTexture rhi_render_target_depth_texture(RhiRenderTarget rt);

void rhi_present(void);

/* ---- resource teardown ---- */
void rhi_destroy_buffer(RhiBuffer buffer);
void rhi_destroy_shader(RhiShader shader);
void rhi_destroy_pipeline(RhiPipeline pipeline);
void rhi_destroy_texture(RhiTexture texture);
void rhi_destroy_render_target(RhiRenderTarget rt);

#endif /* RHI_H */
