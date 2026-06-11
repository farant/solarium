/* rhi_metal.m — the Metal implementation of rhi.h (P4 item 10).
   The SIXTH quarantine: the ONLY file that speaks Metal/Objective-C, as
   rhi_gl.c is the only file that speaks GL. Compiled with ARC; linked with
   -framework Metal -framework QuartzCore; excluded from c89check exactly
   as platform_audio.c is. rhi.h is untouched by the backend switch — that
   untouchedness is the architectural proof this item exists to make.

   STAGE (a) of the landing plan: lifecycle (GLFW_NO_API + CAMetalLayer),
   buffers, MSL shader compile, pipelines, window passes, draws, honest
   frame pacing. Textures, render targets and uniforms are HONEST STUBS
   (id 0 / no-op). Every entry point tolerates the id-0 handles the stubs
   hand out, so the palace boots BLIND BUT ALIVE on this backend while the
   MSL twins arrive in waves (stages b-e).

   THE FIRST §1.4 FINDING: a Metal pipeline state object marries the pixel
   formats of the attachments it will render into; RhiPipelineDesc carries
   no such thing (GL keeps framebuffer state separate). Resolved HERE, not
   in rhi.h: rhi_create_pipeline stores the desc; the concrete
   MTLRenderPipelineState is built lazily at draw time against the OPEN
   pass's formats and memoized per (pipeline, color, depth) combo — the
   palace renders into ~3 format combos, so the cache stays tiny.

   Conventions (the seam's contract, mirrored by every MSL twin):
   - vertex entry point `vmain`, fragment entry point `fmain`;
   - buffer index 0 = the vertex stream, 1 = the instance stream
     (uniform blocks will take 2+ in stage b);
   - the frame is bracketed begin_pass..rhi_present: one command buffer,
     one encoder per pass, one drawable, one autorelease pool. */

#include "rhi.h"

#include <stdio.h>
#include <assert.h>

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

/* the C ABI of @autoreleasepool: the frame spans several rhi calls, so the
   pool cannot be a lexical block — push at frame start, pop after present */
extern void *objc_autoreleasePoolPush(void);
extern void  objc_autoreleasePoolPop(void *pool);

/* ---- internal resource storage (handle id -> Metal object) ----
   The same ceilings and slot discipline as rhi_gl.c. ARC manages object
   lifetimes through these static strong arrays; "delete" is nil-ing the
   slot (any in-flight command buffer holds its own reference until the
   GPU is done — Metal's retain semantics ARE the orphaning idiom). */
#define MAX_BUFFERS   1024
#define MAX_SHADERS     64
#define MAX_PIPELINES   64
#define SLOT_NONE  ((sol_u32)-1)

typedef struct {
    sol_u32       shader;          /* shader table id; 0 = invalid */
    RhiVertexAttr attrs[RHI_MAX_ATTRS];
    int           attr_count;
    size_t        stride;
    size_t        instance_stride;
    sol_bool      depth_test;
    sol_bool      depth_write_off;
    int           blend;           /* RhiBlend */
} MtPipeline;

static id<MTLBuffer>   g_buffers[MAX_BUFFERS];
static sol_u32         g_buffer_count;
static id<MTLFunction> g_shader_v[MAX_SHADERS];
static id<MTLFunction> g_shader_f[MAX_SHADERS];
static sol_u32         g_shader_count;
static MtPipeline      g_pipelines[MAX_PIPELINES];
static sol_u32         g_pipeline_count;

static sol_u32 g_buffer_free[MAX_BUFFERS];
static sol_u32 g_buffer_free_count;
static sol_u32 g_shader_free[MAX_SHADERS];
static sol_u32 g_shader_free_count;
static sol_u32 g_pipeline_free[MAX_PIPELINES];
static sol_u32 g_pipeline_free_count;

static sol_u32 slot_alloc(sol_u32 *count, sol_u32 *free_list,
                          sol_u32 *free_count, sol_u32 max) {
    if (*free_count > 0) {
        *free_count -= 1;
        return free_list[*free_count];
    }
    if (*count >= max) {
        assert(0 && "RHI resource table exhausted (raise the MAX_* ceiling)");
        return SLOT_NONE;
    }
    return (*count)++;
}

static void slot_free(sol_u32 idx, sol_u32 *free_list, sol_u32 *free_count) {
    free_list[*free_count] = idx;
    *free_count += 1;
}

/* ---- the device and the frame ---- */
#define FRAMES_IN_FLIGHT 3   /* the CPU records at most this many ahead */

static struct GLFWwindow         *g_window;
static id<MTLDevice>              g_device;
static id<MTLCommandQueue>        g_queue;
static CAMetalLayer              *g_layer;
static id<MTLTexture>             g_window_depth;  /* backend-owned, tracks size */
static dispatch_semaphore_t       g_inflight;
static NSMutableDictionary       *g_pso_cache;     /* (pipeline,color,depth) -> PSO */
static id<MTLDepthStencilState>   g_ds[2][2];      /* [depth_test][write_off] */

static void                      *g_frame_pool;    /* this frame's autorelease pool */
static sol_bool                   g_frame_began;
static id<MTLCommandBuffer>       g_cmdbuf;        /* this frame's parcel */
static id<CAMetalDrawable>        g_drawable;      /* this frame's window texture */
static id<MTLRenderCommandEncoder> g_encoder;      /* the open pass; nil between */
static sol_bool                   g_pass_alive;    /* encoder usable */
static MTLPixelFormat             g_pass_color_fmt;
static MTLPixelFormat             g_pass_depth_fmt;

/* bound state, applied lazily at draw time (GL is a state machine; the app
   may set state in any order around passes — recording + late application
   makes the encoder agnostic to that order) */
static sol_u32       g_current;     /* pipeline id; 0 = none */
static id<MTLBuffer> g_vbuf;
static id<MTLBuffer> g_instbuf;
static id<MTLBuffer> g_ibuf;

/* ---- lifecycle ---- */
void rhi_configure_window(void) {
    /* no GL context at all — Metal owns the surface via the layer */
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
}

sol_bool rhi_init(struct GLFWwindow *window) {
    NSWindow *nswin;
    g_window = window;
    g_device = MTLCreateSystemDefaultDevice();
    if (!g_device) {
        fprintf(stderr, "Metal: no device\n");
        return SOL_FALSE;
    }
    g_queue = [g_device newCommandQueue];
    g_layer = [CAMetalLayer layer];
    g_layer.device      = g_device;
    g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;  /* NON-sRGB drawable: the
                              shaders' manual pow(1/2.2) encode stays, for
                              exact parity with the GL path (reserved
                              decision: revisit native sRGB after parity) */
    g_layer.framebufferOnly   = YES;
    g_layer.displaySyncEnabled = YES;                /* vsync */
    nswin = glfwGetCocoaWindow(window);
    nswin.contentView.wantsLayer = YES;
    nswin.contentView.layer      = g_layer;
    g_inflight  = dispatch_semaphore_create(FRAMES_IN_FLIGHT);
    g_pso_cache = [NSMutableDictionary new];
    printf("MTL DEVICE : %s\n", g_device.name.UTF8String);
    return SOL_TRUE;
}

void rhi_shutdown(void) {
    sol_u32 i;
    if (g_queue) {                       /* fence: drain the serial queue so
                                            nothing we release is under the GPU */
        id<MTLCommandBuffer> cb = [g_queue commandBuffer];
        [cb commit];
        [cb waitUntilCompleted];
    }
    for (i = 0; i < g_buffer_count; i++) g_buffers[i] = nil;
    for (i = 0; i < g_shader_count; i++) { g_shader_v[i] = nil; g_shader_f[i] = nil; }
    g_pso_cache = nil;
    g_ds[0][0] = nil; g_ds[0][1] = nil; g_ds[1][0] = nil; g_ds[1][1] = nil;
    g_window_depth = nil;
    g_vbuf = nil; g_instbuf = nil; g_ibuf = nil;
    g_encoder = nil; g_cmdbuf = nil; g_drawable = nil;
    g_layer = nil; g_queue = nil; g_device = nil;
    g_window = NULL;
}

/* ---- resource creation ---- */
RhiBuffer rhi_create_buffer(RhiBufferType type, const void *data, size_t size) {
    RhiBuffer h;
    id<MTLBuffer> buf;
    sol_u32 idx;
    (void)type;   /* a Metal buffer is typeless too; usage decides at bind */
    h.id = 0;
    if (!g_device || size == 0) return h;
    buf = data ? [g_device newBufferWithBytes:data length:size
                          options:MTLResourceStorageModeShared]
               : [g_device newBufferWithLength:size
                          options:MTLResourceStorageModeShared];
    if (!buf) return h;
    idx = slot_alloc(&g_buffer_count, g_buffer_free, &g_buffer_free_count, MAX_BUFFERS);
    if (idx == SLOT_NONE) return h;
    g_buffers[idx] = buf;
    h.id = idx + 1;
    return h;
}

void rhi_update_buffer(RhiBuffer buffer, const void *data, size_t size) {
    /* full re-specification, the per-frame stream idiom: a NEW MTLBuffer
       replaces the slot's. Any in-flight command buffer keeps its own
       reference to the old storage until the GPU finishes — Metal's retain
       semantics do the orphaning GL's driver did. */
    if (!buffer.id || !g_device || size == 0 || !data) return;
    g_buffers[buffer.id - 1] = [g_device newBufferWithBytes:data length:size
                                        options:MTLResourceStorageModeShared];
}

RhiShader rhi_create_shader(const char *vertex_src, const char *fragment_src) {
    RhiShader h;
    NSError *err = nil;
    id<MTLLibrary>  vlib, flib;
    id<MTLFunction> vfn, ffn;
    sol_u32 idx;
    h.id = 0;
    if (!g_device || !vertex_src || !fragment_src) return h;
    vlib = [g_device newLibraryWithSource:@(vertex_src) options:nil error:&err];
    if (!vlib) {
        fprintf(stderr, "MSL vertex compile failed:\n%.300s\n",
                err.localizedDescription.UTF8String);
        return h;
    }
    err = nil;
    flib = [g_device newLibraryWithSource:@(fragment_src) options:nil error:&err];
    if (!flib) {
        fprintf(stderr, "MSL fragment compile failed:\n%.300s\n",
                err.localizedDescription.UTF8String);
        return h;
    }
    vfn = [vlib newFunctionWithName:@"vmain"];
    ffn = [flib newFunctionWithName:@"fmain"];
    if (!vfn || !ffn) {
        fprintf(stderr, "MSL entry point missing (vmain/fmain by convention)\n");
        return h;
    }
    idx = slot_alloc(&g_shader_count, g_shader_free, &g_shader_free_count, MAX_SHADERS);
    if (idx == SLOT_NONE) return h;
    g_shader_v[idx] = vfn;
    g_shader_f[idx] = ffn;
    h.id = idx + 1;
    return h;
}

RhiPipeline rhi_create_pipeline(const RhiPipelineDesc *desc) {
    RhiPipeline h;
    MtPipeline *p;
    sol_u32 idx;
    int i;
    h.id = 0;
    if (desc->shader.id == 0) return h;   /* a twin that failed to compile:
                                             the id-0 convention propagates */
    idx = slot_alloc(&g_pipeline_count, g_pipeline_free, &g_pipeline_free_count, MAX_PIPELINES);
    if (idx == SLOT_NONE) return h;
    p = &g_pipelines[idx];
    p->shader     = desc->shader.id;
    p->attr_count = desc->attr_count;
    for (i = 0; i < desc->attr_count; i++) p->attrs[i] = desc->attrs[i];
    p->stride          = desc->stride;
    p->instance_stride = desc->instance_stride;
    p->depth_test      = desc->depth_test;
    p->depth_write_off = desc->depth_write_off;
    p->blend           = desc->blend;
    h.id = idx + 1;
    return h;
}

/* ---- stubs: textures + render targets arrive in stages b-d ---- */
RhiTexture rhi_create_texture(const void *pixels, int width, int height, RhiTextureFormat fmt) {
    RhiTexture h;
    (void)pixels; (void)width; (void)height; (void)fmt;
    h.id = 0;
    return h;
}

void rhi_update_texture(RhiTexture texture, const void *pixels,
                        int width, int height, RhiTextureFormat fmt) {
    (void)texture; (void)pixels; (void)width; (void)height; (void)fmt;
}

RhiTexture rhi_create_texture_hdr(const float *pixels, int width, int height) {
    RhiTexture h;
    (void)pixels; (void)width; (void)height;
    h.id = 0;
    return h;
}

RhiTexture rhi_create_cubemap(int size, sol_bool mipmapped) {
    RhiTexture h;
    (void)size; (void)mipmapped;
    h.id = 0;
    return h;
}

void rhi_begin_cubemap_face(RhiTexture cube, int face, int mip, int size) {
    /* stage d. A dead pass: close any encoder so the bake's draws no-op. */
    (void)cube; (void)face; (void)mip; (void)size;
    if (g_encoder) { [g_encoder endEncoding]; g_encoder = nil; }
    g_pass_alive = SOL_FALSE;
}

void rhi_cubemap_generate_mips(RhiTexture cube) { (void)cube; }

void rhi_bind_texture(RhiTexture texture, int slot) {
    (void)texture; (void)slot;            /* stage b */
}

RhiRenderTarget rhi_create_render_target(int width, int height, RhiTextureFormat color_format) {
    RhiRenderTarget h;
    (void)width; (void)height; (void)color_format;
    h.id = 0;
    return h;
}

RhiRenderTarget rhi_create_depth_target(int width, int height) {
    RhiRenderTarget h;
    (void)width; (void)height;
    h.id = 0;
    return h;
}

RhiTexture rhi_render_target_texture(RhiRenderTarget rt) {
    RhiTexture t;
    (void)rt;
    t.id = 0;
    return t;
}

RhiTexture rhi_render_target_depth_texture(RhiRenderTarget rt) {
    RhiTexture t;
    (void)rt;
    t.id = 0;
    return t;
}

/* ---- per frame ---- */
static void frame_begin(void) {
    if (g_frame_began) return;
    g_frame_pool = objc_autoreleasePoolPush();
    dispatch_semaphore_wait(g_inflight, DISPATCH_TIME_FOREVER);
    g_cmdbuf = [g_queue commandBuffer];
    g_frame_began = SOL_TRUE;
}

static void ensure_drawable(void) {
    int w, h;
    if (g_drawable) return;
    glfwGetFramebufferSize(g_window, &w, &h);
    if (w <= 0 || h <= 0) return;                    /* minimized: skip the frame */
    if ((int)g_layer.drawableSize.width != w || (int)g_layer.drawableSize.height != h)
        g_layer.drawableSize = CGSizeMake(w, h);
    g_drawable = [g_layer nextDrawable];             /* may block: this wait IS the
                                                        honest vsync pacing GL's
                                                        translation layer fumbled */
    if (!g_drawable) return;
    if (!g_window_depth || (int)g_window_depth.width != w
                        || (int)g_window_depth.height != h) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                         width:(NSUInteger)w
                                        height:(NSUInteger)h
                                     mipmapped:NO];
        td.usage       = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        g_window_depth = [g_device newTextureWithDescriptor:td];
    }
}

void rhi_begin_pass(RhiRenderTarget target, int clear_flags,
                    float r, float g, float b, float a) {
    MTLRenderPassDescriptor *pd;
    if (g_encoder) { [g_encoder endEncoding]; g_encoder = nil; }
    g_pass_alive = SOL_FALSE;
    if (!g_queue) return;
    frame_begin();
    if (!g_cmdbuf) return;
    if (target.id != 0) return;     /* offscreen targets: stage c — the stubs
                                       only hand out id 0, so a dead pass here
                                       just swallows that pass's draws */
    ensure_drawable();
    if (!g_drawable) return;
    pd = [MTLRenderPassDescriptor renderPassDescriptor];
    pd.colorAttachments[0].texture = g_drawable.texture;
    pd.colorAttachments[0].loadAction =
        (clear_flags & RHI_CLEAR_COLOR) ? MTLLoadActionClear : MTLLoadActionLoad;
    pd.colorAttachments[0].clearColor  = MTLClearColorMake(r, g, b, a);
    pd.colorAttachments[0].storeAction = MTLStoreActionStore;
    pd.depthAttachment.texture = g_window_depth;
    pd.depthAttachment.loadAction =
        (clear_flags & RHI_CLEAR_DEPTH) ? MTLLoadActionClear : MTLLoadActionLoad;
    pd.depthAttachment.clearDepth  = 1.0;
    pd.depthAttachment.storeAction = MTLStoreActionStore;  /* later same-frame
                                       passes (RHI_CLEAR_NONE composites) LOAD it */
    g_encoder = [g_cmdbuf renderCommandEncoderWithDescriptor:pd];
    g_pass_color_fmt = MTLPixelFormatBGRA8Unorm;
    g_pass_depth_fmt = MTLPixelFormatDepth32Float;
    g_pass_alive = (g_encoder != nil);
}

void rhi_end_pass(void) {
    if (g_encoder) { [g_encoder endEncoding]; g_encoder = nil; }
    g_pass_alive = SOL_FALSE;
}

void rhi_set_pipeline(RhiPipeline pipeline) {
    g_current = pipeline.id;        /* applied lazily at draw time */
}

void rhi_bind_vertex_buffer(RhiBuffer buffer) {
    g_vbuf = buffer.id ? g_buffers[buffer.id - 1] : nil;
}

void rhi_bind_instance_buffer(RhiBuffer buffer) {
    g_instbuf = buffer.id ? g_buffers[buffer.id - 1] : nil;
}

void rhi_bind_index_buffer(RhiBuffer buffer) {
    g_ibuf = buffer.id ? g_buffers[buffer.id - 1] : nil;
}

/* ---- uniforms: the name->offset shadow machinery is stage b ---- */
void rhi_set_uniform_mat4(const char *name, const float *m) { (void)name; (void)m; }
void rhi_set_uniform_mat4_array(const char *name, const float *m, int count) {
    (void)name; (void)m; (void)count;
}
void rhi_set_uniform_mat3(const char *name, const float *m) { (void)name; (void)m; }
void rhi_set_uniform_vec3(const char *name, float x, float y, float z) {
    (void)name; (void)x; (void)y; (void)z;
}
void rhi_set_uniform_float(const char *name, float v) { (void)name; (void)v; }
void rhi_set_uniform_int(const char *name, int v) { (void)name; (void)v; }

/* ---- the lazy pipeline: desc + pass formats -> memoized PSO ---- */
static id<MTLDepthStencilState> ds_for(sol_bool test, sol_bool write_off) {
    int ti = test ? 1 : 0, wi = write_off ? 1 : 0;
    if (!g_ds[ti][wi]) {
        MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
        /* GL semantics, mirrored exactly: depth test DISABLED also disables
           depth writes; enabled tests with LESS (GL's default func) and
           writes unless the pipeline opted out (item 7's particles) */
        dd.depthCompareFunction = test ? MTLCompareFunctionLess
                                       : MTLCompareFunctionAlways;
        dd.depthWriteEnabled = (test && !write_off) ? YES : NO;
        g_ds[ti][wi] = [g_device newDepthStencilStateWithDescriptor:dd];
    }
    return g_ds[ti][wi];
}

static id<MTLRenderPipelineState> pso_for(sol_u32 pipe_id,
                                          MTLPixelFormat color_fmt,
                                          MTLPixelFormat depth_fmt) {
    const MtPipeline *p = &g_pipelines[pipe_id - 1];
    NSNumber *key = @(((unsigned long long)pipe_id << 32)
                    | ((unsigned long long)color_fmt << 16)
                    |  (unsigned long long)depth_fmt);
    id<MTLRenderPipelineState> pso = g_pso_cache[key];
    MTLRenderPipelineDescriptor *rd;
    MTLVertexDescriptor *vd;
    NSError *err = nil;
    int i;
    if (pso) return pso;
    if (p->shader == 0) return nil;
    rd = [MTLRenderPipelineDescriptor new];
    rd.vertexFunction   = g_shader_v[p->shader - 1];
    rd.fragmentFunction = g_shader_f[p->shader - 1];
    vd = [MTLVertexDescriptor vertexDescriptor];
    for (i = 0; i < p->attr_count; i++) {
        const RhiVertexAttr *a = &p->attrs[i];
        vd.attributes[a->location].format =
              (a->format == RHI_FORMAT_FLOAT2) ? MTLVertexFormatFloat2
            : (a->format == RHI_FORMAT_FLOAT4) ? MTLVertexFormatFloat4
            :                                    MTLVertexFormatFloat3;
        vd.attributes[a->location].offset      = a->offset;
        vd.attributes[a->location].bufferIndex = a->per_instance ? 1 : 0;
    }
    if (p->stride) {
        vd.layouts[0].stride       = p->stride;
        vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
    }
    if (p->instance_stride) {
        vd.layouts[1].stride       = p->instance_stride;
        vd.layouts[1].stepFunction = MTLVertexStepFunctionPerInstance;
    }
    rd.vertexDescriptor = vd;
    rd.colorAttachments[0].pixelFormat = color_fmt;
    if (p->blend == RHI_BLEND_ALPHA) {
        rd.colorAttachments[0].blendingEnabled = YES;     /* straight-alpha "over" */
        rd.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorSourceAlpha;
        rd.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
        rd.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorSourceAlpha;
        rd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    } else if (p->blend == RHI_BLEND_ADD) {
        rd.colorAttachments[0].blendingEnabled = YES;     /* pure accumulation */
        rd.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorOne;
        rd.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOne;
        rd.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorOne;
        rd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
    }
    rd.depthAttachmentPixelFormat = depth_fmt;
    pso = [g_device newRenderPipelineStateWithDescriptor:rd error:&err];
    if (!pso) {
        fprintf(stderr, "Metal pipeline build failed:\n%.300s\n",
                err.localizedDescription.UTF8String);
        return nil;
    }
    g_pso_cache[key] = pso;
    return pso;
}

/* Materialize the recorded state onto the open encoder. SOL_FALSE = this
   draw cannot happen (dead pass, id-0 pipeline, failed twin) — skip it,
   the blind-but-alive contract. */
static sol_bool draw_ready(void) {
    const MtPipeline *p;
    id<MTLRenderPipelineState> pso;
    if (!g_pass_alive || !g_encoder || g_current == 0) return SOL_FALSE;
    p = &g_pipelines[g_current - 1];
    pso = pso_for(g_current, g_pass_color_fmt, g_pass_depth_fmt);
    if (!pso) return SOL_FALSE;
    [g_encoder setRenderPipelineState:pso];
    [g_encoder setDepthStencilState:ds_for(p->depth_test, p->depth_write_off)];
    /* GL default: no face culling — match it (parity first, culling later) */
    [g_encoder setCullMode:MTLCullModeNone];
    if (g_vbuf)    [g_encoder setVertexBuffer:g_vbuf    offset:0 atIndex:0];
    if (g_instbuf) [g_encoder setVertexBuffer:g_instbuf offset:0 atIndex:1];
    return SOL_TRUE;
}

void rhi_draw(int first_vertex, int vertex_count) {
    if (!draw_ready()) return;
    [g_encoder drawPrimitives:MTLPrimitiveTypeTriangle
                  vertexStart:(NSUInteger)first_vertex
                  vertexCount:(NSUInteger)vertex_count];
}

void rhi_draw_indexed(int first_index, int index_count) {
    if (!draw_ready() || !g_ibuf) return;
    [g_encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                          indexCount:(NSUInteger)index_count
                           indexType:MTLIndexTypeUInt32
                         indexBuffer:g_ibuf
                   indexBufferOffset:(NSUInteger)first_index * sizeof(sol_u32)];
}

void rhi_draw_indexed_instanced(int first_index, int index_count,
                                int instance_count) {
    if (!draw_ready() || !g_ibuf) return;
    [g_encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                          indexCount:(NSUInteger)index_count
                           indexType:MTLIndexTypeUInt32
                         indexBuffer:g_ibuf
                   indexBufferOffset:(NSUInteger)first_index * sizeof(sol_u32)
                       instanceCount:(NSUInteger)instance_count];
}

void rhi_present(void) {
    if (g_encoder) { [g_encoder endEncoding]; g_encoder = nil; }
    g_pass_alive = SOL_FALSE;
    if (g_cmdbuf) {
        dispatch_semaphore_t sem = g_inflight;
        if (g_drawable) [g_cmdbuf presentDrawable:g_drawable];
        [g_cmdbuf addCompletedHandler:^(id<MTLCommandBuffer> cb) {
            (void)cb;
            dispatch_semaphore_signal(sem);
        }];
        [g_cmdbuf commit];
    } else if (g_frame_began) {
        dispatch_semaphore_signal(g_inflight);   /* nothing committed: hand the
                                                    in-flight slot straight back */
    }
    g_cmdbuf   = nil;
    g_drawable = nil;
    if (g_frame_began) {
        objc_autoreleasePoolPop(g_frame_pool);
        g_frame_pool  = NULL;
        g_frame_began = SOL_FALSE;
    }
}

/* ---- resource teardown ---- */
void rhi_destroy_buffer(RhiBuffer buffer) {
    sol_u32 idx;
    if (!buffer.id) return;
    idx = buffer.id - 1;
    g_buffers[idx] = nil;
    slot_free(idx, g_buffer_free, &g_buffer_free_count);
}

void rhi_destroy_shader(RhiShader shader) {
    sol_u32 idx;
    if (!shader.id) return;
    idx = shader.id - 1;
    g_shader_v[idx] = nil;
    g_shader_f[idx] = nil;
    slot_free(idx, g_shader_free, &g_shader_free_count);
}

void rhi_destroy_pipeline(RhiPipeline pipeline) {
    sol_u32 idx;
    if (!pipeline.id) return;
    idx = pipeline.id - 1;
    g_pipelines[idx].shader = 0;
    /* its memoized PSO variants stay in the cache, keyed by an id that may
       be reused — purge them so a reused slot never wears stale state */
    {
        NSMutableArray *stale = [NSMutableArray new];
        for (NSNumber *key in g_pso_cache) {
            if ((sol_u32)(key.unsignedLongLongValue >> 32) == pipeline.id)
                [stale addObject:key];
        }
        [g_pso_cache removeObjectsForKeys:stale];
    }
    slot_free(idx, g_pipeline_free, &g_pipeline_free_count);
}

void rhi_destroy_texture(RhiTexture texture) {
    (void)texture;                 /* stage b: the stubs hand out id 0 only */
}

void rhi_destroy_render_target(RhiRenderTarget rt) {
    (void)rt;                      /* stage c */
}
