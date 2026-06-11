/* metal_smoke.c — the backend's standing smoke test (P4 item 10). Pure C
   against the UNCHANGED rhi.h with NO GL linked: if this file needed an
   #import, the quarantine would have failed.
   Stage (a): a cleared window + the static triangle (lifecycle, buffers,
   pipelines, draws, pacing). Stage (b) adds the SPINNING TEXTURED QUAD:
   a mat4 uniform through the reflection-built tables + a checkerboard
   through the texture/sampler machinery — each subsystem proven in
   isolation before the palace leans on it.
   Build: ./build.sh metalsmoke   Run: ./metal_smoke (ESC quits).
   It prints the average frame time every 120 frames — the pacing the GL
   translation layer could never hold steady. */

#include "rhi.h"

#include <stdio.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

/* The MSL convention the backend documents: entry points vmain/fmain,
   vertex stream at buffer(0), attribute slots = RhiVertexAttr.location. */
static const char *TRI_VS =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VIn  { float2 pos [[attribute(0)]]; float3 col [[attribute(1)]]; };\n"
    "struct VOut { float4 pos [[position]]; float3 col; };\n"
    "vertex VOut vmain(VIn v [[stage_in]]) {\n"
    "    VOut o;\n"
    "    o.pos = float4(v.pos, 0.0, 1.0);\n"
    "    o.col = v.col;\n"
    "    return o;\n"
    "}\n";

static const char *TRI_FS =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float3 col; };\n"
    "fragment float4 fmain(VOut v [[stage_in]]) {\n"
    "    return float4(v.col, 1.0);\n"
    "}\n";

/* pos.xy + col.rgb, stride 20 — the simplest layout the fetcher can walk */
static const float TRI[15] = {
    -0.45f,  0.85f,   1.00f, 0.35f, 0.20f,
    -0.85f, -0.10f,   0.20f, 1.00f, 0.50f,
    -0.05f, -0.10f,   0.25f, 0.50f, 1.00f
};

/* stage (b): the quad — pos.xy + uv, stride 16; spun by a uniform mat4,
   skinned in a checkerboard. Uniforms AND textures proven in one object. */
static const char *QUAD_VS =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VIn  { float2 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; };\n"
    "struct VU   { float4x4 uMVP; };\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "vertex VOut vmain(VIn v [[stage_in]], constant VU &u [[buffer(2)]]) {\n"
    "    VOut o;\n"
    "    o.pos = u.uMVP * float4(v.pos, 0.0, 1.0);\n"
    "    o.uv  = v.uv;\n"
    "    return o;\n"
    "}\n";

static const char *QUAD_FS =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "fragment float4 fmain(VOut v [[stage_in]],\n"
    "                      texture2d<float> tex [[texture(0)]],\n"
    "                      sampler smp [[sampler(0)]]) {\n"
    "    return tex.sample(smp, v.uv);\n"
    "}\n";

static const float QUAD[24] = {
    -0.4f, -0.4f,  0.0f, 1.0f,
     0.4f, -0.4f,  1.0f, 1.0f,
     0.4f,  0.4f,  1.0f, 0.0f,
    -0.4f, -0.4f,  0.0f, 1.0f,
     0.4f,  0.4f,  1.0f, 0.0f,
    -0.4f,  0.4f,  0.0f, 0.0f
};

#include <math.h>

int main(void) {
    GLFWwindow     *window;
    RhiShader       shader, qshader;
    RhiBuffer       vbuf, qbuf;
    RhiPipeline     pipeline, qpipeline;
    RhiPipelineDesc desc = {0}, qdesc = {0};
    RhiTexture      checker;
    RhiRenderTarget win = {0};
    double          t0;
    int             frames = 0;

    if (!glfwInit()) {
        fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    rhi_configure_window();                       /* GLFW_NO_API: no GL context */
    window = glfwCreateWindow(960, 640, "solarium — metal smoke (stage a)",
                              NULL, NULL);
    if (!window) {
        fprintf(stderr, "window creation failed\n");
        glfwTerminate();
        return 1;
    }
    if (!rhi_init(window)) {
        fprintf(stderr, "rhi_init failed\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    shader = rhi_create_shader(TRI_VS, TRI_FS);
    vbuf   = rhi_create_buffer(RHI_BUFFER_VERTEX, TRI, sizeof TRI);

    desc.shader     = shader;
    desc.attr_count = 2;
    desc.attrs[0].location = 0;  desc.attrs[0].format = RHI_FORMAT_FLOAT2;
    desc.attrs[0].offset   = 0;  desc.attrs[0].per_instance = 0;
    desc.attrs[1].location = 1;  desc.attrs[1].format = RHI_FORMAT_FLOAT3;
    desc.attrs[1].offset   = 8;  desc.attrs[1].per_instance = 0;
    desc.stride     = 20;
    desc.depth_test = SOL_FALSE;
    desc.blend      = RHI_BLEND_NONE;             /* explicit, the standing rule */
    pipeline = rhi_create_pipeline(&desc);

    /* stage (b): the spinning checkered quad */
    qshader = rhi_create_shader(QUAD_VS, QUAD_FS);
    qbuf    = rhi_create_buffer(RHI_BUFFER_VERTEX, QUAD, sizeof QUAD);
    qdesc.shader     = qshader;
    qdesc.attr_count = 2;
    qdesc.attrs[0].location = 0;  qdesc.attrs[0].format = RHI_FORMAT_FLOAT2;
    qdesc.attrs[0].offset   = 0;  qdesc.attrs[0].per_instance = 0;
    qdesc.attrs[1].location = 1;  qdesc.attrs[1].format = RHI_FORMAT_FLOAT2;
    qdesc.attrs[1].offset   = 8;  qdesc.attrs[1].per_instance = 0;
    qdesc.stride     = 16;
    qdesc.depth_test = SOL_FALSE;
    qdesc.blend      = RHI_BLEND_NONE;
    qpipeline = rhi_create_pipeline(&qdesc);
    {
        unsigned char px[8 * 8 * 4];
        int x, y;
        for (y = 0; y < 8; y++)
            for (x = 0; x < 8; x++) {
                unsigned char v = ((x + y) & 1) ? 235 : 40;
                unsigned char *q = px + (y * 8 + x) * 4;
                q[0] = v; q[1] = (unsigned char)(v / 2 + 90); q[2] = 235 - v; q[3] = 255;
            }
        checker = rhi_create_texture(px, 8, 8, RHI_TEX_RGBA8);
    }

    if (!shader.id || !vbuf.id || !pipeline.id ||
        !qshader.id || !qbuf.id || !qpipeline.id || !checker.id) {
        fprintf(stderr, "smoke setup failed (tri %u/%u/%u quad %u/%u/%u tex %u)\n",
                shader.id, vbuf.id, pipeline.id,
                qshader.id, qbuf.id, qpipeline.id, checker.id);
    }

    printf("metal smoke: static triangle + spinning checkered quad — ESC quits\n");
    t0 = glfwGetTime();
    while (!glfwWindowShouldClose(window)) {
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
            glfwSetWindowShouldClose(window, GLFW_TRUE);

        rhi_begin_pass(win, RHI_CLEAR_ALL, 0.08f, 0.10f, 0.14f, 1.0f);
        rhi_set_pipeline(pipeline);
        rhi_bind_vertex_buffer(vbuf);
        rhi_draw(0, 3);
        {   /* spin about Z + shift right, column-major like sol_math */
            float a = (float)glfwGetTime();
            float c = cosf(a), s = sinf(a);
            float mvp[16];
            int i;
            for (i = 0; i < 16; i++) mvp[i] = 0.0f;
            mvp[0] = c;  mvp[1] = s;
            mvp[4] = -s; mvp[5] = c;
            mvp[10] = 1.0f;
            mvp[12] = 0.45f; mvp[13] = -0.35f; mvp[15] = 1.0f;
            rhi_set_pipeline(qpipeline);
            rhi_set_uniform_mat4("uMVP", mvp);
            rhi_bind_vertex_buffer(qbuf);
            rhi_draw(0, 6);
        }
        rhi_end_pass();
        rhi_present();
        glfwPollEvents();

        frames++;
        if (frames % 120 == 0) {
            double t1 = glfwGetTime();
            printf("avg frame: %.2f ms over 120 frames\n",
                   (t1 - t0) * 1000.0 / 120.0);
            t0 = t1;
        }
    }

    rhi_destroy_texture(checker);
    rhi_destroy_pipeline(qpipeline);
    rhi_destroy_buffer(qbuf);
    rhi_destroy_shader(qshader);
    rhi_destroy_pipeline(pipeline);
    rhi_destroy_buffer(vbuf);
    rhi_destroy_shader(shader);
    rhi_shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
