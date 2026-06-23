/* rhi_metal.m — the Metal implementation of rhi.h (P4 item 10).
   The SIXTH quarantine: the ONLY file that speaks Metal/Objective-C, as
   rhi_gl.c is the only file that speaks GL. Compiled with ARC; linked with
   -framework Metal -framework QuartzCore; excluded from c89check exactly
   as platform_audio.c is. rhi.h is untouched by the backend switch — that
   untouchedness is the architectural proof this item exists to make.

   STAGE (a) landed: lifecycle (GLFW_NO_API + CAMetalLayer), buffers, MSL
   compile, window passes, draws, honest frame pacing.
   STAGE (b) lands here: the UNIFORM machinery (reflection-built name ->
   offset tables + per-pipeline shadow blocks + a per-frame arena), 2D
   textures + samplers, and render targets (color + depth-only). Cubemaps
   remain stubs (stage d bakes them). Every entry point stays id-0
   tolerant: the palace runs with whatever twins exist.

   THE FIRST §1.4 FINDING: a Metal pipeline state object marries the pixel
   formats of the attachments it will render into; RhiPipelineDesc carries
   no such thing. Resolved HERE, not in rhi.h: the concrete PSO is built
   lazily against the OPEN pass's formats and memoized per (pipeline,
   color, depth) — the palace renders into ~4 combos.

   UNIFORMS-BY-NAME, Metal edition: GL answered rhi_set_uniform_* with
   glGetUniformLocation against the compiled program. Metal's analog is
   PIPELINE REFLECTION — the compiled PSO reports its uniform struct's
   members (name, offset, type) — so the name->offset tables build
   THEMSELVES at first PSO creation; nothing is hand-maintained and the
   twins cannot drift from the tables. Setters write a persistent CPU
   shadow block (GL's program-resident uniform semantics, mirrored); each
   draw copies the block into a per-frame ARENA buffer and binds the slice
   — one code path whether the block is 16 bytes or a 4KB joint palette.

   Conventions (the seam's contract, mirrored by every MSL twin):
   - vertex entry `vmain`, fragment entry `fmain`;
   - VERTEX buffers: 0 = vertex stream, 1 = instance stream, 2 = the
     uniform struct; FRAGMENT buffers: 0 = the uniform struct;
   - textures/samplers use rhi_bind_texture's slot numbers, fragment
     stage (no vertex textures yet);
   - ORIENTATION (stage c/d, replacing stage b's per-twin v-flips): every
     OFFSCREEN pass renders through a NEGATIVE-HEIGHT VIEWPORT, so all
     render targets (HDR, shadow, bloom, BRDF LUT, cube faces) store
     GL-identical row layouts and every sampling twin keeps GLSL-identical
     math — no flips anywhere. Only the drawable renders top-down (it
     must: row 0 IS the top of the screen). Winding is unaffected — we
     cull nothing (GL parity).
   - DEPTH RANGE: the app's projection matrices are GL-convention
     (clip z in [-w,w]); Metal clips z to [0,w]. Every PROJECTED-geometry
     VS twin remaps `pos.z = (pos.z + pos.w) * 0.5` — full near-plane and
     depth precision parity, and stored depth = GL's 0.5*ndc+0.5 mapping,
     so depth-compare math (the shadow test) stays GLSL-identical too.
   - the frame is bracketed begin_pass..rhi_present: one command buffer,
     one encoder per pass, one drawable, one autorelease pool. */

#include "rhi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
   lifetimes through static strong arrays; "delete" is nil-ing the slot
   (any in-flight command buffer holds its own reference until the GPU is
   done — Metal's retain semantics ARE the orphaning idiom). */
#define MAX_BUFFERS        1024
#define MAX_SHADERS          64
#define MAX_PIPELINES        64
#define MAX_TEXTURES       1024
#define MAX_RENDER_TARGETS   48   /* engine ~14 + inventory thumbnail pool + scratch */
#define SLOT_NONE  ((sol_u32)-1)

/* sampler kinds — GL baked sampler state into the texture object at
   creation; Metal separates them, so each texture remembers its kind and
   the (tiny) sampler cache hands back the matching state object */
enum {
    SAMP_MIPS_REPEAT = 0,   /* create_texture: trilinear, repeat */
    SAMP_LINEAR_CLAMP,      /* render-target color + unmipped cube: linear, clamp */
    SAMP_HDR,               /* equirect HDR: linear, S repeat / T clamp */
    SAMP_DEPTH,             /* shadow map: nearest, clamp-to-WHITE-border
                               (outside the light's frustum = depth 1 = lit,
                               GL's border-color trick mirrored) */
    SAMP_CLAMP_MIPS,        /* mipmapped cube (env/prefilter): trilinear, clamp */
    SAMP_KIND_COUNT
};

/* what a fragment shader DECLARES at each texture slot (from reflection) —
   drives the fallback choice when a declared slot has no app binding
   (Metal validates binding types; a 2D white square cannot stand in for a
   cube or a depth map) */
enum { TEXDECL_NONE = 0, TEXDECL_2D, TEXDECL_CUBE, TEXDECL_DEPTH };

/* one reflected uniform-struct member */
typedef struct {
    char    name[64];
    sol_u32 offset;
    int     dtype;      /* MTLDataType of the element */
    sol_u32 count;      /* array length; 1 = scalar member */
    sol_u32 stride;     /* array element stride; 0 = scalar member */
} MtUniform;

#define MAX_UNIFORMS 64

typedef struct {
    sol_u32       shader;          /* shader table id; 0 = invalid */
    RhiVertexAttr attrs[RHI_MAX_ATTRS];
    int           attr_count;
    size_t        stride;
    size_t        instance_stride;
    sol_bool      depth_test;
    sol_bool      depth_write_off;
    int           blend;           /* RhiBlend */
    /* reflection-built (first successful PSO): */
    sol_bool      reflected;
    MtUniform     vu[MAX_UNIFORMS];  int vu_count;  sol_u32 vu_size;
    MtUniform     fu[MAX_UNIFORMS];  int fu_count;  sol_u32 fu_size;
    unsigned char *vblock;         /* persistent shadow storage (calloc) */
    unsigned char *fblock;
    unsigned char ftex_decl[16];   /* TEXDECL_* per fragment texture slot */
} MtPipeline;

typedef struct {
    int      kind;                 /* SAMP_* */
    sol_bool cpu;                  /* CPU-uploaded (update_texture legal) */
    RhiTextureFormat fmt;          /* creation format (identity, per rhi.h) */
} MtTexInfo;

typedef struct {
    RhiTexture color;              /* handle into the texture table; 0 = depth-only */
    RhiTexture depth;              /* samplable depth (depth targets); 0 = color target */
    RhiTextureFormat color_fmt;
    int width, height;
} MtRenderTarget;

static id<MTLBuffer>   g_buffers[MAX_BUFFERS];
static sol_u32         g_buffer_count;
static id<MTLFunction> g_shader_v[MAX_SHADERS];
static id<MTLFunction> g_shader_f[MAX_SHADERS];
static sol_u32         g_shader_count;
static MtPipeline      g_pipelines[MAX_PIPELINES];
static sol_u32         g_pipeline_count;
static id<MTLTexture>  g_textures[MAX_TEXTURES];
static MtTexInfo       g_tex_info[MAX_TEXTURES];
static sol_u32         g_texture_count;
static MtRenderTarget  g_render_targets[MAX_RENDER_TARGETS];
static id<MTLTexture>  g_rt_depth_obj[MAX_RENDER_TARGETS];  /* color targets'
                                       private (never sampled) depth texture */
static sol_u32         g_render_target_count;

static sol_u32 g_buffer_free[MAX_BUFFERS];
static sol_u32 g_buffer_free_count;
static sol_u32 g_shader_free[MAX_SHADERS];
static sol_u32 g_shader_free_count;
static sol_u32 g_pipeline_free[MAX_PIPELINES];
static sol_u32 g_pipeline_free_count;
static sol_u32 g_texture_free[MAX_TEXTURES];
static sol_u32 g_texture_free_count;
static sol_u32 g_render_target_free[MAX_RENDER_TARGETS];
static sol_u32 g_render_target_free_count;

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
#define FRAMES_IN_FLIGHT 3        /* the CPU records at most this many ahead */
#define ARENA_BYTES (2u * 1024u * 1024u)   /* per-frame uniform arena */
#define MAX_TEX_SLOTS 16

static struct GLFWwindow         *g_window;
static id<MTLDevice>              g_device;
static id<MTLCommandQueue>        g_queue;
static CAMetalLayer              *g_layer;
static id<MTLTexture>             g_window_depth;  /* backend-owned, tracks size */
static dispatch_semaphore_t       g_inflight;
static NSMutableDictionary       *g_pso_cache;     /* (pipeline,color,depth) -> PSO */
static id<MTLDepthStencilState>   g_ds[2][2];      /* [depth_test][write_off] */
static id<MTLSamplerState>        g_samplers[SAMP_KIND_COUNT];
static id<MTLTexture>             g_white;         /* fallbacks for declared-but- */
static id<MTLTexture>             g_white_cube;    /* unbound slots, one per      */
static id<MTLTexture>             g_depth_one;     /* declared texture type       */
static id<MTLBuffer>              g_arena[FRAMES_IN_FLIGHT];
static sol_u32                    g_arena_off;
static int                        g_frame_slot;

static void                      *g_frame_pool;    /* this frame's autorelease pool */
static sol_bool                   g_frame_began;
static id<MTLCommandBuffer>       g_cmdbuf;        /* this frame's parcel */
static id<CAMetalDrawable>        g_drawable;      /* this frame's window texture */
static id<MTLRenderCommandEncoder> g_encoder;      /* the open pass; nil between */
static sol_bool                   g_pass_alive;    /* encoder usable */
static MTLPixelFormat             g_pass_color_fmt;
static MTLPixelFormat             g_pass_depth_fmt;
static volatile double            g_frame_gpu_ms = -1.0;  /* P8 item 1: whole-frame
                                        GPU ms, set on the completion queue
                                        (GPUEndTime-GPUStartTime), read on the
                                        main thread next frame — a benign race */

/* bound state, applied lazily at draw time (GL is a state machine; the app
   may set state in any order around passes — recording + late application
   makes the encoder agnostic to that order) */
static sol_u32       g_current;                  /* pipeline id; 0 = none */
static id<MTLBuffer> g_vbuf;
static id<MTLBuffer> g_instbuf;
static id<MTLBuffer> g_ibuf;
static sol_u32       g_bound_tex[MAX_TEX_SLOTS]; /* texture handles by slot */

static void clear_target_once(id<MTLTexture> color, id<MTLTexture> depth);

/* ---- samplers ---- */
static id<MTLSamplerState> sampler_for(int kind) {
    if (!g_samplers[kind]) {
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = MTLSamplerMinMagFilterLinear;
        sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
        sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        switch (kind) {
        case SAMP_MIPS_REPEAT:
            sd.mipFilter    = MTLSamplerMipFilterLinear;
            sd.sAddressMode = MTLSamplerAddressModeRepeat;
            sd.tAddressMode = MTLSamplerAddressModeRepeat;
            break;
        case SAMP_HDR:
            sd.sAddressMode = MTLSamplerAddressModeRepeat;  /* longitude wraps */
            break;                                          /* T clamps (poles) */
        case SAMP_DEPTH:
            sd.minFilter    = MTLSamplerMinMagFilterNearest;
            sd.magFilter    = MTLSamplerMinMagFilterNearest;
            sd.sAddressMode = MTLSamplerAddressModeClampToBorderColor;
            sd.tAddressMode = MTLSamplerAddressModeClampToBorderColor;
            sd.borderColor  = MTLSamplerBorderColorOpaqueWhite;  /* outside the
                                 light's frustum reads depth 1.0 = lit (GL's
                                 border trick, mirrored) */
            break;
        case SAMP_CLAMP_MIPS:
            sd.mipFilter = MTLSamplerMipFilterLinear;
            break;
        default: break;                 /* SAMP_LINEAR_CLAMP is the base desc */
        }
        g_samplers[kind] = [g_device newSamplerStateWithDescriptor:sd];
    }
    return g_samplers[kind];
}

/* ---- lifecycle ---- */
void rhi_configure_window(void) {
    /* no GL context at all — Metal owns the surface via the layer */
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
}

sol_bool rhi_init(struct GLFWwindow *window) {
    NSWindow *nswin;
    int i;
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
    for (i = 0; i < FRAMES_IN_FLIGHT; i++)
        g_arena[i] = [g_device newBufferWithLength:ARENA_BYTES
                                           options:MTLResourceStorageModeShared];
    {   /* the fallbacks: a declared-but-unbound texture slot samples opaque
           white (a stale GL unit would have sampled garbage; we can do
           better than parity here because Metal VALIDATES bindings). One
           per declared TYPE — a 2D square cannot stand in for a cube or a
           depth map. */
        static const unsigned char px[4] = { 255, 255, 255, 255 };
        MTLTextureDescriptor *td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                         width:1 height:1 mipmapped:NO];
        int s;
        g_white = [g_device newTextureWithDescriptor:td];
        [g_white replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                   mipmapLevel:0 withBytes:px bytesPerRow:4];
        td = [MTLTextureDescriptor
            textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                            size:1 mipmapped:NO];
        g_white_cube = [g_device newTextureWithDescriptor:td];
        for (s = 0; s < 6; s++)
            [g_white_cube replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                            mipmapLevel:0 slice:(NSUInteger)s
                              withBytes:px bytesPerRow:4 bytesPerImage:4];
        td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                         width:1 height:1 mipmapped:NO];
        td.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        g_depth_one = [g_device newTextureWithDescriptor:td];
        clear_target_once(nil, g_depth_one);     /* reads as "far" = lit */
    }
    printf("MTL DEVICE : %s\n", g_device.name.UTF8String);
    return SOL_TRUE;
}

void rhi_shutdown(void) {
    sol_u32 i;
    int k;
    if (g_queue) {                       /* fence: drain the serial queue so
                                            nothing we release is under the GPU */
        id<MTLCommandBuffer> cb = [g_queue commandBuffer];
        [cb commit];
        [cb waitUntilCompleted];
    }
    for (i = 0; i < g_buffer_count; i++) g_buffers[i] = nil;
    for (i = 0; i < g_shader_count; i++) { g_shader_v[i] = nil; g_shader_f[i] = nil; }
    for (i = 0; i < g_texture_count; i++) g_textures[i] = nil;
    for (i = 0; i < g_render_target_count; i++) g_rt_depth_obj[i] = nil;
    for (i = 0; i < g_pipeline_count; i++) {
        free(g_pipelines[i].vblock); g_pipelines[i].vblock = NULL;
        free(g_pipelines[i].fblock); g_pipelines[i].fblock = NULL;
    }
    g_pso_cache = nil;
    g_ds[0][0] = nil; g_ds[0][1] = nil; g_ds[1][0] = nil; g_ds[1][1] = nil;
    for (k = 0; k < SAMP_KIND_COUNT; k++) g_samplers[k] = nil;
    for (k = 0; k < FRAMES_IN_FLIGHT; k++) g_arena[k] = nil;
    g_white = nil; g_white_cube = nil; g_depth_one = nil;
    g_window_depth = nil;
    g_vbuf = nil; g_instbuf = nil; g_ibuf = nil;
    g_encoder = nil; g_cmdbuf = nil; g_drawable = nil;
    g_layer = nil; g_queue = nil; g_device = nil;
    g_window = NULL;
}

/* ---- resource creation: buffers + shaders ---- */
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

/* harvest one stage's reflected bindings into a pipeline's tables */
static void reflect_stage(NSArray<id<MTLBinding>> *bindings, int uniform_index,
                          MtUniform *tab, int *count, sol_u32 *size,
                          unsigned char **block, unsigned char *tex_decl) {
    for (id<MTLBinding> b in bindings) {
        if (b.type == MTLBindingTypeTexture && tex_decl) {
            if (b.index < MAX_TEX_SLOTS) {
                id<MTLTextureBinding> tb = (id<MTLTextureBinding>)b;
                tex_decl[b.index] =
                      tb.depthTexture                              ? TEXDECL_DEPTH
                    : (tb.textureType == MTLTextureTypeCube)       ? TEXDECL_CUBE
                    :                                                TEXDECL_2D;
            }
            continue;
        }
        if (b.type != MTLBindingTypeBuffer || (int)b.index != uniform_index)
            continue;
        {
            id<MTLBufferBinding> bb = (id<MTLBufferBinding>)b;
            MTLStructType *st = bb.bufferStructType;
            if (!st) continue;
            *size = (sol_u32)bb.bufferDataSize;
            for (MTLStructMember *m in st.members) {
                MtUniform *u;
                if (*count >= MAX_UNIFORMS) {
                    fprintf(stderr, "Metal: uniform table full (raise MAX_UNIFORMS)\n");
                    break;
                }
                u = &tab[(*count)++];
                strncpy(u->name, m.name.UTF8String, sizeof u->name - 1);
                u->name[sizeof u->name - 1] = '\0';
                u->offset = (sol_u32)m.offset;
                if (m.dataType == MTLDataTypeArray) {
                    MTLArrayType *at = m.arrayType;
                    u->dtype  = (int)at.elementType;
                    u->count  = (sol_u32)at.arrayLength;
                    u->stride = (sol_u32)at.stride;
                } else {
                    u->dtype  = (int)m.dataType;
                    u->count  = 1;
                    u->stride = 0;
                }
            }
            if (*size && !*block) *block = calloc(1, *size);
        }
    }
}

static id<MTLRenderPipelineState> pso_for(sol_u32 pipe_id,
                                          MTLPixelFormat color_fmt,
                                          MTLPixelFormat depth_fmt,
                                          sol_bool quiet) {
    MtPipeline *p = &g_pipelines[pipe_id - 1];
    NSNumber *key = @(((unsigned long long)pipe_id << 32)
                    | ((unsigned long long)color_fmt << 16)
                    |  (unsigned long long)depth_fmt);
    id<MTLRenderPipelineState> pso = g_pso_cache[key];
    MTLRenderPipelineDescriptor *rd;
    MTLAutoreleasedRenderPipelineReflection refl = nil;
    NSError *err = nil;
    int i;
    if (pso) return pso;
    if (p->shader == 0) return nil;
    rd = [MTLRenderPipelineDescriptor new];
    rd.vertexFunction   = g_shader_v[p->shader - 1];
    rd.fragmentFunction = g_shader_f[p->shader - 1];
    if (p->attr_count > 0) {
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
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
    }
    if (color_fmt != MTLPixelFormatInvalid) {
        rd.colorAttachments[0].pixelFormat = color_fmt;
        if (p->blend == RHI_BLEND_ALPHA) {
            rd.colorAttachments[0].blendingEnabled = YES;  /* straight-alpha "over" */
            rd.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorSourceAlpha;
            rd.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOneMinusSourceAlpha;
            rd.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorSourceAlpha;
            rd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        } else if (p->blend == RHI_BLEND_ADD) {
            rd.colorAttachments[0].blendingEnabled = YES;  /* pure accumulation */
            rd.colorAttachments[0].sourceRGBBlendFactor        = MTLBlendFactorOne;
            rd.colorAttachments[0].destinationRGBBlendFactor   = MTLBlendFactorOne;
            rd.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorOne;
            rd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
        }
    }
    rd.depthAttachmentPixelFormat = depth_fmt;
    pso = [g_device newRenderPipelineStateWithDescriptor:rd
                                                 options:MTLPipelineOptionBindingInfo
                                              reflection:&refl
                                                   error:&err];
    if (!pso) {
        if (!quiet)
            fprintf(stderr, "Metal pipeline build failed:\n%.300s\n",
                    err.localizedDescription.UTF8String);
        return nil;
    }
    if (!p->reflected && refl) {       /* the tables build THEMSELVES — the
                                          struct layout is format-independent,
                                          so any variant's reflection serves */
        reflect_stage(refl.vertexBindings, 2,
                      p->vu, &p->vu_count, &p->vu_size, &p->vblock, NULL);
        reflect_stage(refl.fragmentBindings, 0,
                      p->fu, &p->fu_count, &p->fu_size, &p->fblock, p->ftex_decl);
        p->reflected = SOL_TRUE;
    }
    g_pso_cache[key] = pso;
    return pso;
}

RhiPipeline rhi_create_pipeline(const RhiPipelineDesc *desc) {
    RhiPipeline h;
    MtPipeline *p;
    sol_u32 idx;
    int i;
    h.id = 0;
    if (desc->shader.id == 0) return h;   /* the id-0 contract: a failed twin
                                             propagates a zero pipeline */
    idx = slot_alloc(&g_pipeline_count, g_pipeline_free, &g_pipeline_free_count, MAX_PIPELINES);
    if (idx == SLOT_NONE) return h;
    p = &g_pipelines[idx];
    memset(p, 0, sizeof *p);
    p->shader     = desc->shader.id;
    p->attr_count = desc->attr_count;
    for (i = 0; i < desc->attr_count; i++) p->attrs[i] = desc->attrs[i];
    p->stride          = desc->stride;
    p->instance_stride = desc->instance_stride;
    p->depth_test      = desc->depth_test;
    p->depth_write_off = desc->depth_write_off;
    p->blend           = desc->blend;
    h.id = idx + 1;
    /* probe a PSO NOW so reflection (the uniform tables) exists before the
       app's first set_uniform — try the window combo, then depth-only (a
       void-fragment shadow twin rejects a color attachment); both quiet,
       real usage reports loudly later if genuinely broken */
    if (!pso_for(h.id, MTLPixelFormatBGRA8Unorm, MTLPixelFormatDepth32Float, SOL_TRUE))
        (void)pso_for(h.id, MTLPixelFormatInvalid, MTLPixelFormatDepth32Float, SOL_TRUE);
    return h;
}

/* ---- textures ---- */
static sol_u32 register_texture(id<MTLTexture> tex, int kind, sol_bool cpu,
                                RhiTextureFormat fmt) {
    sol_u32 idx = slot_alloc(&g_texture_count, g_texture_free,
                             &g_texture_free_count, MAX_TEXTURES);
    if (idx == SLOT_NONE) return 0;
    g_textures[idx]      = tex;
    g_tex_info[idx].kind = kind;
    g_tex_info[idx].cpu  = cpu;
    g_tex_info[idx].fmt  = fmt;
    return idx + 1;
}

static void generate_mips(id<MTLTexture> tex) {
    id<MTLCommandBuffer> cb;
    id<MTLBlitCommandEncoder> blit;
    if (tex.mipmapLevelCount <= 1) return;
    cb   = [g_queue commandBuffer];          /* one-shot; the serial queue
                                                orders it before any frame
                                                that samples the texture */
    blit = [cb blitCommandEncoder];
    [blit generateMipmapsForTexture:tex];
    [blit endEncoding];
    [cb commit];
}

static id<MTLTexture> make_2d_texture(const void *pixels, int width, int height,
                                      RhiTextureFormat fmt) {
    MTLPixelFormat pf = (fmt == RHI_TEX_SRGB8) ? MTLPixelFormatRGBA8Unorm_sRGB
                      : (fmt == RHI_TEX_R8)    ? MTLPixelFormatR8Unorm
                      :                          MTLPixelFormatRGBA8Unorm;
    int bpp = (fmt == RHI_TEX_R8) ? 1 : 4;
    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:pf
                                     width:(NSUInteger)width
                                    height:(NSUInteger)height
                                 mipmapped:YES];
    id<MTLTexture> tex;
    td.usage = MTLTextureUsageShaderRead;
    if (fmt == RHI_TEX_R8)            /* GL swizzled G/B to RED so debug views
                                         show grayscale; mirror it (A reads 1) */
        td.swizzle = MTLTextureSwizzleChannelsMake(
            MTLTextureSwizzleRed, MTLTextureSwizzleRed,
            MTLTextureSwizzleRed, MTLTextureSwizzleOne);
    tex = [g_device newTextureWithDescriptor:td];
    if (!tex) return nil;
    [tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)width, (NSUInteger)height)
           mipmapLevel:0
             withBytes:pixels
           bytesPerRow:(NSUInteger)(width * bpp)];
    generate_mips(tex);
    return tex;
}

RhiTexture rhi_create_texture(const void *pixels, int width, int height,
                              RhiTextureFormat fmt) {
    RhiTexture h;
    id<MTLTexture> tex;
    h.id = 0;
    if (!g_device || !pixels || width <= 0 || height <= 0) return h;
    tex = make_2d_texture(pixels, width, height, fmt);
    if (!tex) return h;
    h.id = register_texture(tex, SAMP_MIPS_REPEAT, SOL_TRUE, fmt);
    return h;
}

void rhi_update_texture(RhiTexture texture, const void *pixels,
                        int width, int height, RhiTextureFormat fmt) {
    /* the hot-reload primitive: same handle, new image. A FRESH MTLTexture
       replaces the slot's (the buffer idiom again — in-flight frames keep
       their own reference); every material holding the handle sees the new
       image with zero rebinding. */
    id<MTLTexture> tex;
    if (!texture.id || !pixels) return;
    if (!g_tex_info[texture.id - 1].cpu) return;   /* render targets don't hot-reload */
    tex = make_2d_texture(pixels, width, height, fmt);
    if (!tex) return;
    g_textures[texture.id - 1] = tex;
    g_tex_info[texture.id - 1].fmt = fmt;
}

RhiTexture rhi_create_texture_hdr(const float *pixels, int width, int height) {
    RhiTexture h;
    MTLTextureDescriptor *td;
    id<MTLTexture> tex;
    h.id = 0;
    if (!g_device || !pixels || width <= 0 || height <= 0) return h;
    /* float source uploads as RGBA32Float (replaceRegion does no conversion;
       GL converted to 16F on upload — this stores MORE precision, sampled
       identically). Linear radiance: no mips, S repeats, T clamps. */
    td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                     width:(NSUInteger)width
                                    height:(NSUInteger)height
                                 mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    tex = [g_device newTextureWithDescriptor:td];
    if (!tex) return h;
    [tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)width, (NSUInteger)height)
           mipmapLevel:0
             withBytes:pixels
           bytesPerRow:(NSUInteger)width * 16];
    h.id = register_texture(tex, SAMP_HDR, SOL_FALSE, RHI_TEX_RGBA16F);
    return h;
}

/* cubemaps (stage d): the IBL bakes' canvas. Metal is CLEANER than GL
   here — no shared FBO dance; a pass descriptor just names slice + level. */
static void frame_begin(void);

RhiTexture rhi_create_cubemap(int size, sol_bool mipmapped) {
    RhiTexture h;
    MTLTextureDescriptor *td;
    id<MTLTexture> tex;
    h.id = 0;
    if (!g_device || size <= 0) return h;
    td = [MTLTextureDescriptor
        textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                        size:(NSUInteger)size
                                   mipmapped:(mipmapped ? YES : NO)];
    td.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    tex = [g_device newTextureWithDescriptor:td];
    if (!tex) return h;
    h.id = register_texture(tex, mipmapped ? SAMP_CLAMP_MIPS : SAMP_LINEAR_CLAMP,
                            SOL_FALSE, RHI_TEX_RGBA16F);
    return h;
}

void rhi_begin_cubemap_face(RhiTexture cube, int face, int mip, int size) {
    MTLRenderPassDescriptor *pd;
    if (g_encoder) { [g_encoder endEncoding]; g_encoder = nil; }
    g_pass_alive = SOL_FALSE;
    if (!cube.id || !g_queue) return;            /* the id-0 contract */
    frame_begin();
    if (!g_cmdbuf) return;
    pd = [MTLRenderPassDescriptor renderPassDescriptor];
    pd.colorAttachments[0].texture     = g_textures[cube.id - 1];
    pd.colorAttachments[0].slice       = (NSUInteger)face;
    pd.colorAttachments[0].level       = (NSUInteger)mip;
    pd.colorAttachments[0].loadAction  = MTLLoadActionClear;  /* GL's glClear */
    pd.colorAttachments[0].clearColor  = MTLClearColorMake(0, 0, 0, 1);
    pd.colorAttachments[0].storeAction = MTLStoreActionStore;
    g_encoder = [g_cmdbuf renderCommandEncoderWithDescriptor:pd];
    if (!g_encoder) return;
    {   /* the negative-height viewport: faces store GL row layouts, so the
           app's GL-compensating face bases work unchanged */
        MTLViewport vp;
        vp.originX = 0.0;             vp.originY = (double)size;
        vp.width   = (double)size;    vp.height  = -(double)size;
        vp.znear   = 0.0;             vp.zfar    = 1.0;
        [g_encoder setViewport:vp];
    }
    g_pass_color_fmt = MTLPixelFormatRGBA16Float;
    g_pass_depth_fmt = MTLPixelFormatInvalid;    /* no depth, like GL's cube FBO */
    g_pass_alive = SOL_TRUE;
}

void rhi_cubemap_generate_mips(RhiTexture cube) {
    id<MTLTexture> tex;
    if (!cube.id) return;
    tex = g_textures[cube.id - 1];
    if (!tex || tex.mipmapLevelCount <= 1) return;
    if (g_encoder) { [g_encoder endEncoding]; g_encoder = nil; g_pass_alive = SOL_FALSE; }
    if (g_frame_began && g_cmdbuf) {
        /* ride the FRAME's command buffer: encode order = execution order,
           so the mips compute AFTER the face renders that feed them (a
           one-shot here would commit FIRST and average undefined texels) */
        id<MTLBlitCommandEncoder> blit = [g_cmdbuf blitCommandEncoder];
        [blit generateMipmapsForTexture:tex];
        [blit endEncoding];
    } else {
        generate_mips(tex);
    }
}

void rhi_bind_texture(RhiTexture texture, int slot) {
    if (slot < 0 || slot >= MAX_TEX_SLOTS) return;
    g_bound_tex[slot] = texture.id;          /* applied lazily at draw time */
}

/* ---- render targets ---- */
/* clear a fresh target once so a pass that LOADS before anything drew
   (bloom's accumulating up-walk, a skipped extract) reads defined zeroes —
   GL framebuffers gave that for free, Metal textures start undefined */
static void clear_target_once(id<MTLTexture> color, id<MTLTexture> depth) {
    id<MTLCommandBuffer> cb = [g_queue commandBuffer];
    MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor renderPassDescriptor];
    id<MTLRenderCommandEncoder> enc;
    if (color) {
        pd.colorAttachments[0].texture     = color;
        pd.colorAttachments[0].loadAction  = MTLLoadActionClear;
        pd.colorAttachments[0].clearColor  = MTLClearColorMake(0, 0, 0, 1);
        pd.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    if (depth) {
        pd.depthAttachment.texture     = depth;
        pd.depthAttachment.loadAction  = MTLLoadActionClear;
        pd.depthAttachment.clearDepth  = 1.0;
        pd.depthAttachment.storeAction = MTLStoreActionStore;
    }
    enc = [cb renderCommandEncoderWithDescriptor:pd];
    [enc endEncoding];
    [cb commit];
}

RhiRenderTarget rhi_create_render_target(int width, int height,
                                         RhiTextureFormat color_format) {
    RhiRenderTarget h;
    MTLPixelFormat pf = (color_format == RHI_TEX_RGBA16F) ? MTLPixelFormatRGBA16Float
                      : (color_format == RHI_TEX_SRGB8)   ? MTLPixelFormatRGBA8Unorm_sRGB
                      :                                     MTLPixelFormatRGBA8Unorm;
    MTLTextureDescriptor *td;
    id<MTLTexture> color, depth;
    MtRenderTarget *rt;
    sol_u32 rt_idx, tex_id, depth_id;
    h.id = 0;
    if (!g_device || width <= 0 || height <= 0) return h;

    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf
            width:(NSUInteger)width height:(NSUInteger)height mipmapped:NO];
    td.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    color = [g_device newTextureWithDescriptor:td];

    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
            width:(NSUInteger)width height:(NSUInteger)height mipmapped:NO];
    td.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;  /* P8 item 2: post samples depth */
    td.storageMode = MTLStorageModePrivate;
    depth = [g_device newTextureWithDescriptor:td];
    if (!color || !depth) return h;

    tex_id = register_texture(color, SAMP_LINEAR_CLAMP, SOL_FALSE, color_format);
    if (!tex_id) return h;
    depth_id = register_texture(depth, SAMP_DEPTH, SOL_FALSE, RHI_TEX_RGBA8);  /* P8 item 2: samplable */
    if (!depth_id) {
        g_textures[tex_id - 1] = nil;
        slot_free(tex_id - 1, g_texture_free, &g_texture_free_count);
        return h;
    }
    rt_idx = slot_alloc(&g_render_target_count, g_render_target_free,
                        &g_render_target_free_count, MAX_RENDER_TARGETS);
    if (rt_idx == SLOT_NONE) {
        g_textures[tex_id - 1]   = nil;
        g_textures[depth_id - 1] = nil;
        slot_free(tex_id - 1,   g_texture_free, &g_texture_free_count);
        slot_free(depth_id - 1, g_texture_free, &g_texture_free_count);
        return h;
    }
    rt = &g_render_targets[rt_idx];
    rt->color.id  = tex_id;
    rt->depth.id  = depth_id;       /* P8 item 2: color targets expose samplable depth */
    rt->color_fmt = color_format;
    rt->width     = width;
    rt->height    = height;
    g_rt_depth_obj[rt_idx] = depth;
    clear_target_once(color, depth);
    h.id = rt_idx + 1;
    return h;
}

RhiRenderTarget rhi_create_depth_target(int width, int height) {
    RhiRenderTarget h;
    MTLTextureDescriptor *td;
    id<MTLTexture> depth;
    MtRenderTarget *rt;
    sol_u32 rt_idx, tex_id;
    h.id = 0;
    if (!g_device || width <= 0 || height <= 0) return h;
    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
            width:(NSUInteger)width height:(NSUInteger)height mipmapped:NO];
    td.usage       = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    depth = [g_device newTextureWithDescriptor:td];
    if (!depth) return h;
    tex_id = register_texture(depth, SAMP_DEPTH, SOL_FALSE, RHI_TEX_RGBA8);
    if (!tex_id) return h;
    rt_idx = slot_alloc(&g_render_target_count, g_render_target_free,
                        &g_render_target_free_count, MAX_RENDER_TARGETS);
    if (rt_idx == SLOT_NONE) {
        g_textures[tex_id - 1] = nil;
        slot_free(tex_id - 1, g_texture_free, &g_texture_free_count);
        return h;
    }
    rt = &g_render_targets[rt_idx];
    rt->color.id  = 0;
    rt->depth.id  = tex_id;
    rt->color_fmt = RHI_TEX_RGBA8;     /* unused */
    rt->width     = width;
    rt->height    = height;
    g_rt_depth_obj[rt_idx] = nil;
    clear_target_once(nil, depth);
    h.id = rt_idx + 1;
    return h;
}

RhiTexture rhi_render_target_texture(RhiRenderTarget rt) {
    RhiTexture t;
    if (!rt.id) { t.id = 0; return t; }
    return g_render_targets[rt.id - 1].color;
}

RhiTexture rhi_render_target_depth_texture(RhiRenderTarget rt) {
    RhiTexture t;
    if (!rt.id) { t.id = 0; return t; }
    return g_render_targets[rt.id - 1].depth;
}

/* ---- per frame ---- */
static void frame_begin(void) {
    if (g_frame_began) return;
    g_frame_pool = objc_autoreleasePoolPush();
    dispatch_semaphore_wait(g_inflight, DISPATCH_TIME_FOREVER);
    g_cmdbuf     = [g_queue commandBuffer];
    g_frame_slot = (g_frame_slot + 1) % FRAMES_IN_FLIGHT;
    g_arena_off  = 0;                /* the slot's GPU work finished (the
                                        semaphore guarantees it): reuse */
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
    pd = [MTLRenderPassDescriptor renderPassDescriptor];
    if (target.id != 0) {
        const MtRenderTarget *rt = &g_render_targets[target.id - 1];
        id<MTLTexture> depth = rt->depth.id ? g_textures[rt->depth.id - 1]
                                            : g_rt_depth_obj[target.id - 1];
        if (rt->color.id) {
            pd.colorAttachments[0].texture     = g_textures[rt->color.id - 1];
            pd.colorAttachments[0].loadAction  =
                (clear_flags & RHI_CLEAR_COLOR) ? MTLLoadActionClear : MTLLoadActionLoad;
            pd.colorAttachments[0].clearColor  = MTLClearColorMake(r, g, b, a);
            pd.colorAttachments[0].storeAction = MTLStoreActionStore;
            g_pass_color_fmt =
                  (rt->color_fmt == RHI_TEX_RGBA16F) ? MTLPixelFormatRGBA16Float
                : (rt->color_fmt == RHI_TEX_SRGB8)   ? MTLPixelFormatRGBA8Unorm_sRGB
                :                                      MTLPixelFormatRGBA8Unorm;
        } else {
            g_pass_color_fmt = MTLPixelFormatInvalid;   /* depth-only (shadow) */
        }
        pd.depthAttachment.texture     = depth;
        pd.depthAttachment.loadAction  =
            (clear_flags & RHI_CLEAR_DEPTH) ? MTLLoadActionClear : MTLLoadActionLoad;
        pd.depthAttachment.clearDepth  = 1.0;
        pd.depthAttachment.storeAction = MTLStoreActionStore;
        g_pass_depth_fmt = MTLPixelFormatDepth32Float;
        g_encoder = [g_cmdbuf renderCommandEncoderWithDescriptor:pd];
        if (g_encoder) {       /* offscreen renders through the NEGATIVE-
                                  height viewport: GL row layout, see top */
            MTLViewport vp;
            vp.originX = 0.0;                  vp.originY = (double)rt->height;
            vp.width   = (double)rt->width;    vp.height  = -(double)rt->height;
            vp.znear   = 0.0;                  vp.zfar    = 1.0;
            [g_encoder setViewport:vp];
        }
        g_pass_alive = (g_encoder != nil);
        return;
    } else {
        ensure_drawable();
        if (!g_drawable) return;
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
        g_pass_color_fmt = MTLPixelFormatBGRA8Unorm;
        g_pass_depth_fmt = MTLPixelFormatDepth32Float;
    }
    g_encoder = [g_cmdbuf renderCommandEncoderWithDescriptor:pd];
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

/* ---- uniforms: name -> reflected member -> shadow block ----
   GL's program-resident uniform semantics, mirrored: values persist on the
   pipeline across frames; an unknown name is silently ignored (GL location
   -1); "name[i]" indexes an array member, GL-style. */
static const MtUniform *find_member(const MtUniform *tab, int n,
                                    const char *name, sol_u32 *elem) {
    char base[64];
    const char *br = strchr(name, '[');
    int i;
    *elem = 0;
    if (br) {
        size_t len = (size_t)(br - name);
        if (len >= sizeof base) return NULL;
        memcpy(base, name, len);
        base[len] = '\0';
        *elem = (sol_u32)atoi(br + 1);
        name = base;
    }
    for (i = 0; i < n; i++)
        if (strcmp(tab[i].name, name) == 0) return &tab[i];
    return NULL;
}

/* write `bytes` of `src` at the member (+ array element), if the pipeline
   has it in either stage and the element type matches `want` */
static void uni_write(const char *name, int want, const void *src, sol_u32 bytes) {
    MtPipeline *p;
    int stage;
    if (!g_current) return;
    p = &g_pipelines[g_current - 1];
    for (stage = 0; stage < 2; stage++) {
        const MtUniform *tab = stage ? p->fu : p->vu;
        int            n     = stage ? p->fu_count : p->vu_count;
        unsigned char *block = stage ? p->fblock : p->vblock;
        sol_u32        size  = stage ? p->fu_size : p->vu_size;
        sol_u32        elem;
        const MtUniform *u;
        sol_u32 off;
        if (!block) continue;
        u = find_member(tab, n, name, &elem);
        if (!u || u->dtype != want || elem >= u->count) continue;
        off = u->offset + elem * u->stride;
        if (off + bytes > size) continue;
        memcpy(block + off, src, bytes);
    }
}

void rhi_set_uniform_mat4(const char *name, const float *m) {
    uni_write(name, (int)MTLDataTypeFloat4x4, m, 64);
}

void rhi_set_uniform_mat4_array(const char *name, const float *m, int count) {
    MtPipeline *p;
    int stage;
    if (!g_current || count <= 0) return;
    p = &g_pipelines[g_current - 1];
    for (stage = 0; stage < 2; stage++) {
        const MtUniform *tab = stage ? p->fu : p->vu;
        int            n     = stage ? p->fu_count : p->vu_count;
        unsigned char *block = stage ? p->fblock : p->vblock;
        sol_u32        size  = stage ? p->fu_size : p->vu_size;
        sol_u32        elem;
        const MtUniform *u;
        sol_u32 c, i;
        if (!block) continue;
        u = find_member(tab, n, name, &elem);
        if (!u || u->dtype != (int)MTLDataTypeFloat4x4 || u->count < 2) continue;
        c = (sol_u32)count;
        if (c > u->count) c = u->count;
        for (i = 0; i < c; i++) {
            sol_u32 off = u->offset + i * u->stride;   /* float4x4 stride = 64 */
            if (off + 64 > size) break;
            memcpy(block + off, m + i * 16, 64);
        }
    }
}

void rhi_set_uniform_mat3(const char *name, const float *m) {
    /* the MSL alignment trap, defused: float3x3 columns stride 16 bytes;
       the GL-side source is 9 packed floats */
    float cols[12];
    int c;
    for (c = 0; c < 3; c++) {
        cols[c * 4 + 0] = m[c * 3 + 0];
        cols[c * 4 + 1] = m[c * 3 + 1];
        cols[c * 4 + 2] = m[c * 3 + 2];
        cols[c * 4 + 3] = 0.0f;
    }
    uni_write(name, (int)MTLDataTypeFloat3x3, cols, 48);
}

void rhi_set_uniform_vec3(const char *name, float x, float y, float z) {
    float v[3];
    v[0] = x; v[1] = y; v[2] = z;
    uni_write(name, (int)MTLDataTypeFloat3, v, 12);
}

void rhi_set_uniform_float(const char *name, float v) {
    uni_write(name, (int)MTLDataTypeFloat, &v, 4);
}

void rhi_set_uniform_int(const char *name, int v) {
    /* sampler-unit ints (GL's uTex = 0) name no struct member in MSL —
       slots are explicit in the twins — so those sets fall through to the
       silent-ignore path, exactly as intended */
    uni_write(name, (int)MTLDataTypeInt, &v, 4);
}

/* ---- draws: materialize the recorded state onto the open encoder ---- */
static sol_bool arena_push(const unsigned char *src, sol_u32 bytes, sol_u32 *out_off) {
    sol_u32 off = (g_arena_off + 255u) & ~255u;      /* constant-buffer alignment */
    static sol_bool warned = SOL_FALSE;
    if (off + bytes > ARENA_BYTES) {
        if (!warned) {
            fprintf(stderr, "Metal: uniform arena exhausted — draws dropped "
                            "(raise ARENA_BYTES)\n");
            warned = SOL_TRUE;
        }
        return SOL_FALSE;
    }
    memcpy((unsigned char *)g_arena[g_frame_slot].contents + off, src, bytes);
    g_arena_off = off + bytes;
    *out_off = off;
    return SOL_TRUE;
}

static sol_bool draw_ready(void) {
    MtPipeline *p;
    id<MTLRenderPipelineState> pso;
    sol_u32 off;
    int slot;
    if (!g_pass_alive || !g_encoder || g_current == 0) return SOL_FALSE;
    p = &g_pipelines[g_current - 1];
    pso = pso_for(g_current, g_pass_color_fmt, g_pass_depth_fmt, SOL_FALSE);
    if (!pso) return SOL_FALSE;
    [g_encoder setRenderPipelineState:pso];
    [g_encoder setDepthStencilState:ds_for(p->depth_test, p->depth_write_off)];
    [g_encoder setCullMode:MTLCullModeNone];   /* GL default: no culling (parity) */
    if (g_vbuf)    [g_encoder setVertexBuffer:g_vbuf    offset:0 atIndex:0];
    if (g_instbuf) [g_encoder setVertexBuffer:g_instbuf offset:0 atIndex:1];
    if (p->vblock && p->vu_size) {             /* the shadow block rides the arena */
        if (!arena_push(p->vblock, p->vu_size, &off)) return SOL_FALSE;
        [g_encoder setVertexBuffer:g_arena[g_frame_slot] offset:off atIndex:2];
    }
    if (p->fblock && p->fu_size) {
        if (!arena_push(p->fblock, p->fu_size, &off)) return SOL_FALSE;
        [g_encoder setFragmentBuffer:g_arena[g_frame_slot] offset:off atIndex:0];
    }
    for (slot = 0; slot < MAX_TEX_SLOTS; slot++) {
        id<MTLTexture> tex = nil;
        int kind = SAMP_LINEAR_CLAMP;
        int decl = p->ftex_decl[slot];
        if (decl == TEXDECL_NONE) continue;          /* only declared slots */
        if (g_bound_tex[slot] && g_textures[g_bound_tex[slot] - 1]) {
            tex  = g_textures[g_bound_tex[slot] - 1];
            kind = g_tex_info[g_bound_tex[slot] - 1].kind;
        }
        if (!tex) {                /* declared but unbound: the TYPED fallback
                                      (Metal validates; a 2D white square
                                      cannot stand in for a cube or depth) */
            tex  = (decl == TEXDECL_CUBE)  ? g_white_cube
                 : (decl == TEXDECL_DEPTH) ? g_depth_one : g_white;
            kind = (decl == TEXDECL_DEPTH) ? SAMP_DEPTH : SAMP_LINEAR_CLAMP;
        }
        [g_encoder setFragmentTexture:tex atIndex:(NSUInteger)slot];
        [g_encoder setFragmentSamplerState:sampler_for(kind) atIndex:(NSUInteger)slot];
    }
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

/* GPU timers (P8 item 1). v1: whole-frame only — cb.GPUStartTime/EndTime
   captured in the completion handler below, free and universally supported.
   The per-pass path (MTLCounterSampleBuffer sampled at stage boundaries) is
   the gated follow-up; until then rhi_timer_ms reports unavailable and the
   scopes no-op. */
void   rhi_timer_begin(int slot) { (void)slot; }
void   rhi_timer_end(void)       { }
double rhi_timer_ms(int slot)    { (void)slot; return -1.0; }
double rhi_timer_frame_ms(void)  { return g_frame_gpu_ms; }

void rhi_present(void) {
    if (g_encoder) { [g_encoder endEncoding]; g_encoder = nil; }
    g_pass_alive = SOL_FALSE;
    if (g_cmdbuf) {
        dispatch_semaphore_t sem = g_inflight;
        if (g_drawable) [g_cmdbuf presentDrawable:g_drawable];
        [g_cmdbuf addCompletedHandler:^(id<MTLCommandBuffer> cb) {
            g_frame_gpu_ms = (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;  /* P8 item 1 */
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

/* see rhi.h — commit + DRAIN offscreen work (the IBL re-bake) that ran outside
   the frame loop, so its passes don't "adopt" the next frame's pool + in-flight
   slot. The bake's first rhi_begin_cubemap_face calls frame_begin (pushing the
   pool, taking a slot, opening g_cmdbuf) but never presents; this closes it
   synchronously and resets the state so the next real frame starts clean. */
void rhi_flush(void) {
    if (g_encoder) { [g_encoder endEncoding]; g_encoder = nil; }
    g_pass_alive = SOL_FALSE;
    if (!g_frame_began) return;              /* nothing began — nothing to drain */
    if (g_cmdbuf) {
        [g_cmdbuf commit];
        [g_cmdbuf waitUntilCompleted];       /* the bake's GPU work is done on return */
    }
    dispatch_semaphore_signal(g_inflight);   /* balance frame_begin's wait (no present did) */
    g_cmdbuf = nil;
    objc_autoreleasePoolPop(g_frame_pool);
    g_frame_pool  = NULL;
    g_frame_began = SOL_FALSE;
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
    free(g_pipelines[idx].vblock); g_pipelines[idx].vblock = NULL;
    free(g_pipelines[idx].fblock); g_pipelines[idx].fblock = NULL;
    /* purge memoized PSO variants so a reused slot never wears stale state */
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
    sol_u32 idx;
    if (!texture.id) return;
    idx = texture.id - 1;
    g_textures[idx] = nil;
    slot_free(idx, g_texture_free, &g_texture_free_count);
}

void rhi_destroy_render_target(RhiRenderTarget rt) {
    MtRenderTarget *r;
    sol_u32 idx;
    if (!rt.id) return;
    idx = rt.id - 1;
    r = &g_render_targets[idx];
    if (r->color.id) rhi_destroy_texture(r->color);
    if (r->depth.id) rhi_destroy_texture(r->depth);
    g_rt_depth_obj[idx] = nil;
    r->color.id = 0; r->depth.id = 0;
    slot_free(idx, g_render_target_free, &g_render_target_free_count);
}
