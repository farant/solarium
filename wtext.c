/* wtext.c — see wtext.h. Above the seam: rhi_*, never GL. Mirrors ui.c's
   SDF pipeline with one difference that is the whole point: vertices are in
   a surface's local meters and ride uMVP, instead of screen pixels riding
   the pixels->NDC remap. text_shape stays the only door to glyphs. */

#include "wtext.h"
#include "text.h"
#include "sol_math.h"

#define WT_MAX_GLYPHS 4096   /* a full book page of code (item 9) fits */
#define WT_VERT_FLOATS 5              /* x, y, z, u, v — z carries the bend */
/* WT_WRAP_CAP now comes from wtext.h, so callers wrap identically */

static struct {
    RhiShader   shader;
    RhiPipeline pipeline;
    RhiBuffer   vbuffer;
    sol_bool    ready;
} g_wt;

/* one block's worth of glyph quads; static, not stack (96KB) — the engine
   is single-threaded and blocks are drawn immediately */
static float g_wt_verts[WT_MAX_GLYPHS * 6 * WT_VERT_FLOATS];

#ifdef SOL_RHI_METAL

static const char *WT_VERTEX_SRC =     /* item 10 stage (e): the MSL twin */
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VIn { float3 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; };\n"
    "struct VU { float4x4 uMVP; };\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "vertex VOut vmain(VIn v [[stage_in]], constant VU &u [[buffer(2)]]) {\n"
    "    VOut o;\n"
    "    o.pos = u.uMVP * float4(v.pos, 1.0);\n"
    "    o.pos.z = (o.pos.z + o.pos.w) * 0.5;\n"   /* GL clip z -> Metal */
    "    o.uv = v.uv;\n"
    "    return o;\n"
    "}\n";

#else /* GLSL */

static const char *WT_VERTEX_SRC =
    "#version 330 core\n"
    "layout(location = 0) in vec3 aPos;\n"
    "layout(location = 1) in vec2 aUV;\n"
    "uniform mat4 uMVP;\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "    gl_Position = uMVP * vec4(aPos, 1.0);\n"
    "    vUV = aUV;\n"
    "}\n";

#endif /* SOL_RHI_METAL — world-text VS */

/* the same SDF decode as the UI text pipeline; fwidth here measures the
   PROJECTED derivative, so the edge band tracks perspective automatically.
   Fully transparent fragments discard so depth writes stay honest (an
   invisible glyph quad must not occlude ink drawn after it). */
#ifdef SOL_RHI_METAL

static const char *WT_FRAGMENT_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "struct FU { float3 uColor; };\n"
    "fragment float4 fmain(VOut v [[stage_in]], constant FU &u [[buffer(0)]],\n"
    "                      texture2d<float> uTex [[texture(0)]],\n"
    "                      sampler s0 [[sampler(0)]]) {\n"
    "    float d = uTex.sample(s0, v.uv).r;\n"
    "    float w = fwidth(d) * 0.5 + 0.0001;\n"
    "    float edge = smoothstep(0.5 - w, 0.5 + w, d);\n"
    "    if (edge < 0.004) discard_fragment();\n"
    "    return float4(u.uColor, edge);\n"
    "}\n";

#else /* GLSL */

static const char *WT_FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec3 uColor;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "    float d = texture(uTex, vUV).r;\n"
    "    float w = fwidth(d) * 0.5 + 0.0001;\n"
    "    float edge = smoothstep(0.5 - w, 0.5 + w, d);\n"
    "    if (edge < 0.004) discard;\n"
    "    FragColor = vec4(uColor, edge);\n"
    "}\n";

#endif /* SOL_RHI_METAL — world-text FS */

sol_bool wtext_init(void) {
    RhiPipelineDesc desc = {0};

    g_wt.shader = rhi_create_shader(WT_VERTEX_SRC, WT_FRAGMENT_SRC);
    if (!g_wt.shader.id) return SOL_FALSE;

    desc.shader = g_wt.shader;
    desc.attrs[0].location = 0; desc.attrs[0].format = RHI_FORMAT_FLOAT3; desc.attrs[0].offset = 0;
    desc.attrs[1].location = 1; desc.attrs[1].format = RHI_FORMAT_FLOAT2;
    desc.attrs[1].offset = 3 * sizeof(sol_f32);
    desc.attr_count = 2;
    desc.stride     = WT_VERT_FLOATS * sizeof(sol_f32);
    desc.depth_test = SOL_TRUE;      /* ink sits ON surfaces: walls occlude it */
    desc.blend      = SOL_TRUE;      /* the SDF edge is an alpha ramp */
    g_wt.pipeline = rhi_create_pipeline(&desc);
    if (!g_wt.pipeline.id) return SOL_FALSE;

    g_wt.vbuffer = rhi_create_buffer(RHI_BUFFER_VERTEX, (const void *)0,
                                     256 * WT_VERT_FLOATS * sizeof(sol_f32));
    if (!g_wt.vbuffer.id) return SOL_FALSE;

    g_wt.ready = SOL_TRUE;
    return SOL_TRUE;
}

void wtext_shutdown(void) {
    if (!g_wt.ready) return;
    rhi_destroy_buffer(g_wt.vbuffer);
    rhi_destroy_pipeline(g_wt.pipeline);
    rhi_destroy_shader(g_wt.shader);
    g_wt.ready = SOL_FALSE;
}

/* one vertex into the batch: position already 3D */
static int wt_vert(int vc, float px, float py, float pz, float u, float v) {
    float *o = &g_wt_verts[(size_t)vc * WT_VERT_FLOATS];
    o[0] = px; o[1] = py; o[2] = pz; o[3] = u; o[4] = v;
    return vc + 1;
}

/* the shared emitter: flat when bend is NULL (z = 0), else each glyph's
   left and right edges ride the curve — a piecewise-flat chord per glyph */
static void wt_emit(const Font *f, mat4 viewproj, mat4 model, const char *src,
                    float x, float top_y, float px_to_m,
                    WtextBend bend, void *user, float lift,
                    float r, float g, float b) {
    ShapedGlyph shaped[WT_MAX_GLYPHS];
    mat4        mvp;
    float       baseline;
    int         n, i, vc = 0;

    n = text_shape(f, src, shaped, WT_MAX_GLYPHS);
    baseline = top_y - font_ascent(f) * px_to_m;   /* top edge -> first baseline */

    for (i = 0; i < n; i++) {
        const FontGlyph *gl = font_glyph(f, shaped[i].glyph);
        float gx, gy, gw, gh;
        if (!gl || gl->w <= 0.0f) continue;        /* ink-less: advance only */
        /* shaped positions are px, +y DOWN; the surface is meters, +y UP —
           flip around the first baseline */
        gx = x + (shaped[i].x + gl->xoff) * px_to_m;
        gy = baseline - (shaped[i].y + gl->yoff) * px_to_m;   /* the quad's TOP */
        gw = gl->w * px_to_m;
        gh = gl->h * px_to_m;
        if (!bend) {
            vc = wt_vert(vc, gx,      gy,      0.0f, gl->u0, gl->v0);
            vc = wt_vert(vc, gx + gw, gy,      0.0f, gl->u1, gl->v0);
            vc = wt_vert(vc, gx + gw, gy - gh, 0.0f, gl->u1, gl->v1);
            vc = wt_vert(vc, gx,      gy,      0.0f, gl->u0, gl->v0);
            vc = wt_vert(vc, gx + gw, gy - gh, 0.0f, gl->u1, gl->v1);
            vc = wt_vert(vc, gx,      gy - gh, 0.0f, gl->u0, gl->v1);
        } else {
            float ax, az, atx, atz, bx, bz, btx, btz;
            float anx, anz, bnx, bnz, x0, z0, x1, z1;
            bend(gx,      user, &ax, &az, &atx, &atz);
            bend(gx + gw, user, &bx, &bz, &btx, &btz);
            anx = -atz; anz = atx;                 /* the curve's 2D normal */
            bnx = -btz; bnz = btx;
            x0 = ax + anx * lift;  z0 = az + anz * lift;
            x1 = bx + bnx * lift;  z1 = bz + bnz * lift;
            vc = wt_vert(vc, x0, gy,      z0, gl->u0, gl->v0);
            vc = wt_vert(vc, x1, gy,      z1, gl->u1, gl->v0);
            vc = wt_vert(vc, x1, gy - gh, z1, gl->u1, gl->v1);
            vc = wt_vert(vc, x0, gy,      z0, gl->u0, gl->v0);
            vc = wt_vert(vc, x1, gy - gh, z1, gl->u1, gl->v1);
            vc = wt_vert(vc, x0, gy - gh, z0, gl->u0, gl->v1);
        }
    }
    if (vc == 0) return;

    rhi_update_buffer(g_wt.vbuffer, g_wt_verts,
                      (size_t)vc * WT_VERT_FLOATS * sizeof(sol_f32));
    mvp = mat4_mul(viewproj, model);
    rhi_set_pipeline(g_wt.pipeline);
    rhi_bind_vertex_buffer(g_wt.vbuffer);
    rhi_bind_texture(font_atlas(f), 0);
    rhi_set_uniform_int("uTex", 0);
    rhi_set_uniform_mat4("uMVP", mvp.m);
    rhi_set_uniform_vec3("uColor", r, g, b);
    rhi_draw(0, vc);
}

void wtext_block(const Font *f, mat4 viewproj, mat4 model, const char *utf8,
                 float x, float top_y, float px_to_m, float wrap_w_m,
                 float r, float g, float b) {
    char        wrapped[WT_WRAP_CAP];
    const char *src = utf8;

    if (!g_wt.ready || !f || !utf8 || px_to_m <= 0.0f) return;
    if (wrap_w_m > 0.0f) {
        if (text_wrap(f, utf8, px_to_m, wrap_w_m, wrapped, WT_WRAP_CAP) > 0)
            src = wrapped;
    }
    wt_emit(f, viewproj, model, src, x, top_y, px_to_m,
            (WtextBend)0, (void *)0, 0.0f, r, g, b);
}

void wtext_block_bent(const Font *f, mat4 viewproj, mat4 model,
                      const char *utf8, float x, float top_y, float px_to_m,
                      WtextBend bend, void *user, float lift,
                      float r, float g, float b) {
    if (!g_wt.ready || !f || !utf8 || px_to_m <= 0.0f || !bend) return;
    wt_emit(f, viewproj, model, utf8, x, top_y, px_to_m,
            bend, user, lift, r, g, b);
}
