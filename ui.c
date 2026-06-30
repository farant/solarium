/* ui.c — immediate-mode 2D overlay (P3 item 2). See ui.h for the model.

   The batch is MeshBuilder's transient cousin: accumulate CPU vertices, flush
   once — but rebuilt from nothing every frame, so the GPU buffer is a STREAM
   (re-specified via rhi_update_buffer, never rebuilt as a new handle).

   One shader path for everything: "untextured" quads sample an internal 1x1
   white texture (multiply by 1 = pure vertex color), so the batch only breaks
   when the texture actually changes — the dear-imgui trick. */

#include "ui.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* vertex: pos2 + uv2 + rgba4 = 8 floats (32 bytes) */
#define UI_VERT_FLOATS 8
#define UI_VERT_BYTES  (UI_VERT_FLOATS * sizeof(sol_f32))

/* a contiguous run of vertices drawn with one texture + one pipeline
   (sdf -> the text pipeline's smoothstep decode; else the flat shader) */
typedef struct { RhiTexture tex; sol_bool sdf; int first; int count; } UiSpan;

static struct {
    sol_f32    *verts;                       /* CPU batch, realloc-doubling */
    sol_u32     vert_count, vert_cap;        /* in vertices */
    UiSpan     *spans;
    sol_u32     span_count, span_cap;
    RhiBuffer   vbuffer;                     /* persistent stream buffer handle;
                                                storage is re-specified per frame */
    RhiShader   shader;
    RhiPipeline pipeline;
    RhiShader   text_shader;                 /* SDF decode (P3 item 3b) */
    RhiPipeline text_pipeline;
    RhiTexture  white;                       /* 1x1 white for untextured draws */
    float       screen_w, screen_h;
    sol_bool    ready;
} g_ui;

#ifdef SOL_RHI_METAL

/* Full-fidelity MSL twins (item 10 stage b). The pixel->clip remap is
   IDENTICAL: UI's top-left origin agrees with Metal's top-left raster
   origin the same way it agreed with GL's bottom-left through the y flip
   in the math. The atlas is a CPU-uploaded texture — no v-flip (that rule
   is for render targets only). */
static const char *UI_VERTEX_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VIn {\n"
    "    float2 pos [[attribute(0)]];\n"
    "    float2 uv  [[attribute(1)]];\n"
    "    float4 col [[attribute(2)]];\n"
    "};\n"
    "struct VU { float uScreenW; float uScreenH; };\n"
    "struct VOut { float4 pos [[position]]; float2 uv; float4 col; };\n"
    "vertex VOut vmain(VIn v [[stage_in]], constant VU &u [[buffer(2)]]) {\n"
    "    VOut o;\n"
    "    float x = v.pos.x / u.uScreenW * 2.0 - 1.0;\n"
    "    float y = 1.0 - v.pos.y / u.uScreenH * 2.0;\n"
    "    o.pos = float4(x, y, 0.0, 1.0);\n"
    "    o.uv  = v.uv;\n"
    "    o.col = v.col;\n"
    "    return o;\n"
    "}\n";

static const char *UI_FRAGMENT_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float2 uv; float4 col; };\n"
    "fragment float4 fmain(VOut v [[stage_in]],\n"
    "                      texture2d<float> uTex [[texture(0)]],\n"
    "                      sampler smp [[sampler(0)]]) {\n"
    "    return v.col * uTex.sample(smp, v.uv);\n"
    "}\n";

static const char *UI_TEXT_FRAGMENT_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float2 uv; float4 col; };\n"
    "fragment float4 fmain(VOut v [[stage_in]],\n"
    "                      texture2d<float> uTex [[texture(0)]],\n"
    "                      sampler smp [[sampler(0)]]) {\n"
    "    float d = uTex.sample(smp, v.uv).r;\n"
    "    float w = fwidth(d) * 0.5 + 0.0001;\n"
    "    float edge = smoothstep(0.5 - w, 0.5 + w, d);\n"
    "    return float4(v.col.rgb, v.col.a * edge);\n"
    "}\n";

#else /* GLSL */

/* pixels in, NDC out: the orthographic projection collapsed to one remap.
   Top-left origin / y-down (UI convention) to GL's centered y-up. */
static const char *UI_VERTEX_SRC =
    "#version 330 core\n"
    "layout(location = 0) in vec2 aPos;\n"
    "layout(location = 1) in vec2 aUV;\n"
    "layout(location = 2) in vec4 aColor;\n"
    "uniform float uScreenW;\n"
    "uniform float uScreenH;\n"
    "out vec2 vUV;\n"
    "out vec4 vColor;\n"
    "void main() {\n"
    "    float x = aPos.x / uScreenW * 2.0 - 1.0;\n"
    "    float y = 1.0 - aPos.y / uScreenH * 2.0;\n"
    "    gl_Position = vec4(x, y, 0.0, 1.0);\n"
    "    vUV    = aUV;\n"
    "    vColor = aColor;\n"
    "}\n";

/* display-referred passthrough: no tonemap, no gamma — the framebuffer is
   already sRGB-encoded after the post pass and we are compositing onto it */
static const char *UI_FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "in vec4 vColor;\n"
    "uniform sampler2D uTex;\n"
    "out vec4 FragColor;\n"
    "void main() { FragColor = vColor * texture(uTex, vUV); }\n";

/* the SDF decode: the atlas stores DISTANCE (0.5 = the glyph contour), and
   thresholding it reconstructs the edge analytically per screen pixel — why
   one atlas is crisp at every zoom. fwidth(d) is the screen-space derivative,
   so the smoothstep band is always ~one display pixel of anti-aliasing
   whatever the glyph's scale. */
static const char *UI_TEXT_FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "in vec4 vColor;\n"
    "uniform sampler2D uTex;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "    float d = texture(uTex, vUV).r;\n"
    "    float w = fwidth(d) * 0.5 + 0.0001;\n"
    "    float edge = smoothstep(0.5 - w, 0.5 + w, d);\n"
    "    FragColor = vec4(vColor.rgb, vColor.a * edge);\n"
    "}\n";

#endif /* SOL_RHI_METAL — the UI trio */

sol_bool ui_init(void) {
    RhiPipelineDesc desc = {0};
    unsigned char   white_px[4];

    g_ui.shader = rhi_create_shader(UI_VERTEX_SRC, UI_FRAGMENT_SRC);
    if (!g_ui.shader.id) return SOL_FALSE;

    desc.shader = g_ui.shader;
    desc.attrs[0].location = 0; desc.attrs[0].format = RHI_FORMAT_FLOAT2; desc.attrs[0].offset = 0;
    desc.attrs[1].location = 1; desc.attrs[1].format = RHI_FORMAT_FLOAT2;
    desc.attrs[1].offset = 2 * sizeof(sol_f32);
    desc.attrs[2].location = 2; desc.attrs[2].format = RHI_FORMAT_FLOAT4;
    desc.attrs[2].offset = 4 * sizeof(sol_f32);
    desc.attr_count = 3;
    desc.stride     = UI_VERT_BYTES;
    desc.depth_test = SOL_FALSE;     /* paint order, not depth; 3D depth untouched */
    desc.blend      = SOL_TRUE;      /* the whole point: straight-alpha over */
    g_ui.pipeline = rhi_create_pipeline(&desc);
    if (!g_ui.pipeline.id) return SOL_FALSE;

    /* the text pipeline: same vertex format and state, SDF-decoding fragment */
    g_ui.text_shader = rhi_create_shader(UI_VERTEX_SRC, UI_TEXT_FRAGMENT_SRC);
    if (!g_ui.text_shader.id) return SOL_FALSE;
    desc.shader = g_ui.text_shader;
    g_ui.text_pipeline = rhi_create_pipeline(&desc);
    if (!g_ui.text_pipeline.id) return SOL_FALSE;

    white_px[0] = 255; white_px[1] = 255; white_px[2] = 255; white_px[3] = 255;
    g_ui.white = rhi_create_texture(white_px, 1, 1, RHI_TEX_RGBA8);   /* linear: sampled raw */
    if (!g_ui.white.id) return SOL_FALSE;

    g_ui.vbuffer = rhi_create_buffer(RHI_BUFFER_VERTEX, (const void *)0,
                                     256 * UI_VERT_BYTES);   /* re-sized per frame */
    if (!g_ui.vbuffer.id) return SOL_FALSE;

    g_ui.ready = SOL_TRUE;
    return SOL_TRUE;
}

void ui_shutdown(void) {
    if (!g_ui.ready) return;
    rhi_destroy_buffer(g_ui.vbuffer);
    rhi_destroy_texture(g_ui.white);
    rhi_destroy_pipeline(g_ui.text_pipeline);
    rhi_destroy_shader(g_ui.text_shader);
    rhi_destroy_pipeline(g_ui.pipeline);
    rhi_destroy_shader(g_ui.shader);
    free(g_ui.verts);
    free(g_ui.spans);
    memset(&g_ui, 0, sizeof g_ui);
}

void ui_begin(int screen_w, int screen_h) {
    g_ui.screen_w   = (float)screen_w;
    g_ui.screen_h   = (float)screen_h;
    g_ui.vert_count = 0;             /* drop any stale batch */
    g_ui.span_count = 0;
}

float ui_vw(float pct) { return g_ui.screen_w * pct * 0.01f; }
float ui_vh(float pct) { return g_ui.screen_h * pct * 0.01f; }

/* ---------------------------------------------------------------- batching */

/* Continue the current span if it uses this texture + pipeline, else start a
   new one (a "batch break" — each span is one draw call). */
static UiSpan *span_for(RhiTexture tex, sol_bool sdf) {
    UiSpan *s;
    if (g_ui.span_count > 0 &&
        g_ui.spans[g_ui.span_count - 1].tex.id == tex.id &&
        g_ui.spans[g_ui.span_count - 1].sdf    == sdf) {
        return &g_ui.spans[g_ui.span_count - 1];
    }
    if (g_ui.span_count == g_ui.span_cap) {
        sol_u32 cap   = g_ui.span_cap ? g_ui.span_cap * 2 : 8;
        UiSpan *grown = (UiSpan *)realloc(g_ui.spans, (size_t)cap * sizeof(UiSpan));
        if (!grown) return (UiSpan *)0;
        g_ui.spans    = grown;
        g_ui.span_cap = cap;
    }
    s = &g_ui.spans[g_ui.span_count++];
    s->tex   = tex;
    s->sdf   = sdf;
    s->first = (int)g_ui.vert_count;
    s->count = 0;
    return s;
}

static void push_vert(float x, float y, float u, float v,
                      float r, float g, float b, float a) {
    sol_f32 *p;
    if (g_ui.vert_count == g_ui.vert_cap) {
        sol_u32  cap   = g_ui.vert_cap ? g_ui.vert_cap * 2 : 256;
        sol_f32 *grown = (sol_f32 *)realloc(g_ui.verts,
                                            (size_t)cap * UI_VERT_BYTES);
        if (!grown) return;
        g_ui.verts    = grown;
        g_ui.vert_cap = cap;
    }
    p = g_ui.verts + (size_t)g_ui.vert_count * UI_VERT_FLOATS;
    p[0] = x; p[1] = y; p[2] = u; p[3] = v;
    p[4] = r; p[5] = g; p[6] = b; p[7] = a;
    g_ui.vert_count++;
}

/* Two triangles for an axis-aligned rect; the caller picks which texture v
   lands on the top edge (data-order textures want 0, GL-flipped images 1). */
static void push_rect(RhiTexture tex, float x, float y, float w, float h,
                      float v_top, float v_bottom,
                      float r, float g, float b, float a) {
    UiSpan *s = span_for(tex, SOL_FALSE);
    if (!s) return;
    push_vert(x,     y,     0.0f, v_top,    r, g, b, a);
    push_vert(x + w, y,     1.0f, v_top,    r, g, b, a);
    push_vert(x + w, y + h, 1.0f, v_bottom, r, g, b, a);
    push_vert(x,     y,     0.0f, v_top,    r, g, b, a);
    push_vert(x + w, y + h, 1.0f, v_bottom, r, g, b, a);
    push_vert(x,     y + h, 0.0f, v_bottom, r, g, b, a);
    s->count += 6;
}

/* ---------------------------------------------------------------- draw API */

void ui_quad(float x, float y, float w, float h,
             float r, float g, float b, float a) {
    if (!g_ui.ready) return;
    push_rect(g_ui.white, x, y, w, h, 0.0f, 1.0f, r, g, b, a);
}

void ui_quad_outline(float x, float y, float w, float h, float t,
                     float r, float g, float b, float a) {
    if (!g_ui.ready) return;
    ui_quad(x,         y,         w, t,           r, g, b, a);   /* top    */
    ui_quad(x,         y + h - t, w, t,           r, g, b, a);   /* bottom */
    ui_quad(x,         y + t,     t, h - 2.0f * t, r, g, b, a);  /* left   */
    ui_quad(x + w - t, y + t,     t, h - 2.0f * t, r, g, b, a);  /* right  */
}

void ui_line(float x0, float y0, float x1, float y1, float t,
             float r, float g, float b, float a) {
    float   dx, dy, len, px, py;
    UiSpan *s;
    if (!g_ui.ready) return;
    dx  = x1 - x0;
    dy  = y1 - y0;
    len = (float)sqrt((double)(dx * dx + dy * dy));
    if (len < 0.0001f) return;
    px = -dy / len * t * 0.5f;       /* the perpendicular, half a thickness long */
    py =  dx / len * t * 0.5f;
    s = span_for(g_ui.white, SOL_FALSE);
    if (!s) return;
    push_vert(x0 + px, y0 + py, 0.0f, 0.0f, r, g, b, a);
    push_vert(x1 + px, y1 + py, 1.0f, 0.0f, r, g, b, a);
    push_vert(x1 - px, y1 - py, 1.0f, 1.0f, r, g, b, a);
    push_vert(x0 + px, y0 + py, 0.0f, 0.0f, r, g, b, a);
    push_vert(x1 - px, y1 - py, 1.0f, 1.0f, r, g, b, a);
    push_vert(x0 - px, y0 - py, 0.0f, 1.0f, r, g, b, a);
    s->count += 6;
}

void ui_textured_quad(RhiTexture tex, float x, float y, float w, float h) {
    if (!g_ui.ready || !tex.id) return;
    /* data row order: texture row 0 at the quad's top (matches the font
       atlas). An stb-loaded image (flipped for GL at decode) shows inverted
       here — add a flip flag when such a caller exists. */
    push_rect(tex, x, y, w, h, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
}

void ui_textured_quad_flip(RhiTexture tex, float x, float y, float w, float h) {
    if (!g_ui.ready || !tex.id) return;
    /* v swapped: texture row 0 at the quad's BOTTOM (stb images + GL RTs). */
    push_rect(tex, x, y, w, h, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
}

void ui_glyph_quad(RhiTexture atlas, float x, float y, float w, float h,
                   float u0, float v0, float u1, float v1,
                   float r, float g, float b, float a) {
    UiSpan *s;
    if (!g_ui.ready) return;
    s = span_for(atlas, SOL_TRUE);       /* the SDF-decode pipeline */
    if (!s) return;
    push_vert(x,     y,     u0, v0, r, g, b, a);
    push_vert(x + w, y,     u1, v0, r, g, b, a);
    push_vert(x + w, y + h, u1, v1, r, g, b, a);
    push_vert(x,     y,     u0, v0, r, g, b, a);
    push_vert(x + w, y + h, u1, v1, r, g, b, a);
    push_vert(x,     y + h, u0, v1, r, g, b, a);
    s->count += 6;
}

/* ------------------------------------------------------------------- flush */

void ui_end(void) {
    RhiRenderTarget screen = {0};    /* the window (declaration init) */
    size_t          bytes;
    sol_u32         i;

    if (!g_ui.ready || g_ui.vert_count == 0) return;

    /* full re-specification sizes the buffer to the batch every frame —
       there is no separate "capacity" to grow (the driver orphans + allocs) */
    bytes = (size_t)g_ui.vert_count * UI_VERT_BYTES;
    rhi_update_buffer(g_ui.vbuffer, g_ui.verts, bytes);

    rhi_begin_pass(screen, RHI_CLEAR_NONE, 0.0f, 0.0f, 0.0f, 0.0f);  /* load, don't clear */
    {
        int cur = -1;        /* which pipeline is bound: 0 flat, 1 sdf */
        for (i = 0; i < g_ui.span_count; i++) {
            int want = g_ui.spans[i].sdf ? 1 : 0;
            if (want != cur) {           /* pipeline switch: rebind buffer + uniforms */
                rhi_set_pipeline(want ? g_ui.text_pipeline : g_ui.pipeline);
                rhi_bind_vertex_buffer(g_ui.vbuffer);
                rhi_set_uniform_float("uScreenW", g_ui.screen_w);
                rhi_set_uniform_float("uScreenH", g_ui.screen_h);
                rhi_set_uniform_int("uTex", 0);
                cur = want;
            }
            rhi_bind_texture(g_ui.spans[i].tex, 0);
            rhi_draw(g_ui.spans[i].first, g_ui.spans[i].count);
        }
    }
    rhi_end_pass();

    g_ui.vert_count = 0;
    g_ui.span_count = 0;
}
