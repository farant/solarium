/* metal_smoke.c — stage (a)'s proof (P4 item 10): a window cleared and a
   triangle drawn through the UNCHANGED rhi.h with NO GL linked — the Metal
   twin of TODO.md step 6's first triangle. Pure C against the seam: if
   this file needed an #import, the quarantine would have failed.
   Build: ./build.sh metalsmoke   Run: ./metal_smoke (ESC quits).
   It prints the average frame time every 120 frames — the first look at
   the pacing the GL translation layer could never hold steady. */

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
     0.0f,  0.6f,    1.00f, 0.35f, 0.20f,
    -0.6f, -0.5f,    0.20f, 1.00f, 0.50f,
     0.6f, -0.5f,    0.25f, 0.50f, 1.00f
};

int main(void) {
    GLFWwindow     *window;
    RhiShader       shader;
    RhiBuffer       vbuf;
    RhiPipeline     pipeline;
    RhiPipelineDesc desc = {0};
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

    if (!shader.id || !vbuf.id || !pipeline.id) {
        fprintf(stderr, "smoke setup failed (shader %u buffer %u pipeline %u)\n",
                shader.id, vbuf.id, pipeline.id);
    }

    printf("metal smoke: triangle on a cleared window — ESC quits\n");
    t0 = glfwGetTime();
    while (!glfwWindowShouldClose(window)) {
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
            glfwSetWindowShouldClose(window, GLFW_TRUE);

        rhi_begin_pass(win, RHI_CLEAR_ALL, 0.08f, 0.10f, 0.14f, 1.0f);
        rhi_set_pipeline(pipeline);
        rhi_bind_vertex_buffer(vbuf);
        rhi_draw(0, 3);
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

    rhi_destroy_pipeline(pipeline);
    rhi_destroy_buffer(vbuf);
    rhi_destroy_shader(shader);
    rhi_shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
