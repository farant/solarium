/* wtext.c — see wtext.h. Above the seam: rhi_*, never GL. Mirrors ui.c's
   SDF pipeline with one difference that is the whole point: vertices are in
   a surface's local meters and ride uMVP, instead of screen pixels riding
   the pixels->NDC remap. text_shape stays the only door to glyphs. */

#include "wtext.h"
#include "text.h"
#include "sol_math.h"

#define WT_MAX_GLYPHS 1024
#define WT_WRAP_CAP   2048
#define WT_VERT_FLOATS 4              /* x, y, u, v */

static struct {
    RhiShader   shader;
    RhiPipeline pipeline;
    RhiBuffer   vbuffer;
    sol_bool    ready;
} g_wt;

/* one block's worth of glyph quads; static, not stack (96KB) — the engine
   is single-threaded and blocks are drawn immediately */
static float g_wt_verts[WT_MAX_GLYPHS * 6 * WT_VERT_FLOATS];

static const char *WT_VERTEX_SRC =
    "#version 330 core\n"
    "layout(location = 0) in vec2 aPos;\n"
    "layout(location = 1) in vec2 aUV;\n"
    "uniform mat4 uMVP;\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);\n"
    "    vUV = aUV;\n"
    "}\n";

/* the same SDF decode as the UI text pipeline; fwidth here measures the
   PROJECTED derivative, so the edge band tracks perspective automatically.
   Fully transparent fragments discard so depth writes stay honest (an
   invisible glyph quad must not occlude ink drawn after it). */
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

sol_bool wtext_init(void) {
    RhiPipelineDesc desc = {0};

    g_wt.shader = rhi_create_shader(WT_VERTEX_SRC, WT_FRAGMENT_SRC);
    if (!g_wt.shader.id) return SOL_FALSE;

    desc.shader = g_wt.shader;
    desc.attrs[0].location = 0; desc.attrs[0].format = RHI_FORMAT_FLOAT2; desc.attrs[0].offset = 0;
    desc.attrs[1].location = 1; desc.attrs[1].format = RHI_FORMAT_FLOAT2;
    desc.attrs[1].offset = 2 * sizeof(sol_f32);
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

void wtext_block(const Font *f, mat4 viewproj, mat4 model, const char *utf8,
                 float x, float top_y, float px_to_m, float wrap_w_m,
                 float r, float g, float b) {
    ShapedGlyph shaped[WT_MAX_GLYPHS];
    char        wrapped[WT_WRAP_CAP];
    const char *src = utf8;
    mat4        mvp;
    float       baseline;
    int         n, i, vc = 0;

    if (!g_wt.ready || !f || !utf8 || px_to_m <= 0.0f) return;

    if (wrap_w_m > 0.0f) {
        if (text_wrap(f, utf8, px_to_m, wrap_w_m, wrapped, WT_WRAP_CAP) > 0)
            src = wrapped;
    }

    n = text_shape(f, src, shaped, WT_MAX_GLYPHS);
    baseline = top_y - font_ascent(f) * px_to_m;   /* top edge -> first baseline */

    for (i = 0; i < n; i++) {
        const FontGlyph *gl = font_glyph(f, shaped[i].glyph);
        float gx, gy, gw, gh, *v;
        if (!gl || gl->w <= 0.0f) continue;        /* ink-less: advance only */
        /* shaped positions are px, +y DOWN; the surface is meters, +y UP —
           flip around the first baseline */
        gx = x + (shaped[i].x + gl->xoff) * px_to_m;
        gy = baseline - (shaped[i].y + gl->yoff) * px_to_m;   /* the quad's TOP */
        gw = gl->w * px_to_m;
        gh = gl->h * px_to_m;
        v = &g_wt_verts[(size_t)vc * WT_VERT_FLOATS];
        v[0]  = gx;      v[1]  = gy;      v[2]  = gl->u0; v[3]  = gl->v0;
        v[4]  = gx + gw; v[5]  = gy;      v[6]  = gl->u1; v[7]  = gl->v0;
        v[8]  = gx + gw; v[9]  = gy - gh; v[10] = gl->u1; v[11] = gl->v1;
        v[12] = gx;      v[13] = gy;      v[14] = gl->u0; v[15] = gl->v0;
        v[16] = gx + gw; v[17] = gy - gh; v[18] = gl->u1; v[19] = gl->v1;
        v[20] = gx;      v[21] = gy - gh; v[22] = gl->u0; v[23] = gl->v1;
        vc += 6;
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
