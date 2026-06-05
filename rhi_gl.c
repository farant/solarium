/* rhi_gl.c — the OpenGL implementation of rhi.h.
   This is the ONLY file that includes a GL header. All the GL state,
   the bind-to-edit dance, and the silent-failure traps live in here. */

#define GL_SILENCE_DEPRECATION

#include "rhi.h"

#include <stdio.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <OpenGL/gl3.h>

/* ---- internal resource storage (handle id -> GL object) ---- */
#define MAX_BUFFERS        256
#define MAX_SHADERS         64
#define MAX_PIPELINES       64
#define MAX_TEXTURES        64
#define MAX_RENDER_TARGETS  16

typedef struct {
    GLuint        program;
    GLuint        vao;
    RhiVertexAttr attrs[RHI_MAX_ATTRS];
    int           attr_count;
    GLsizei       stride;
    sol_bool      depth_test;
} GlPipeline;

typedef struct {
    GLuint     fbo;
    GLuint     depth_rbo;   /* write-only depth; never sampled this item */
    RhiTexture color;       /* handle into g_textures, so it binds like any texture */
    int        width, height;
} GlRenderTarget;

static GLuint     g_buffers[MAX_BUFFERS];
static sol_u32    g_buffer_count;
static GLuint     g_shaders[MAX_SHADERS];
static sol_u32    g_shader_count;
static GlPipeline g_pipelines[MAX_PIPELINES];
static sol_u32    g_pipeline_count;
static GLuint     g_textures[MAX_TEXTURES];
static sol_u32    g_texture_count;
static GlRenderTarget g_render_targets[MAX_RENDER_TARGETS];
static sol_u32        g_render_target_count;

static struct GLFWwindow *g_window;
static const GlPipeline  *g_current;   /* pipeline bound by rhi_set_pipeline */

/* ---- free-lists: reused slots so create/destroy loops stay bounded ---- */
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

/* Reuse a freed slot if one exists, else extend the table. */
static sol_u32 slot_alloc(sol_u32 *count, sol_u32 *free_list, sol_u32 *free_count) {
    if (*free_count > 0) {
        *free_count -= 1;
        return free_list[*free_count];
    }
    return (*count)++;
}

static void slot_free(sol_u32 idx, sol_u32 *free_list, sol_u32 *free_count) {
    free_list[*free_count] = idx;
    *free_count += 1;
}

/* ---- debug GL error check: silent unless we introduce a real error ---- */
#ifndef NDEBUG
static void gl_check(const char *where) {
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
        fprintf(stderr, "GL error 0x%04x at %s\n", (unsigned)err, where);
    }
}
#else
#define gl_check(where) ((void)0)
#endif

/* ---- shader compile/link (moved here from main.c — it's GL) ---- */
static GLuint compile_shader(GLenum type, const char *src) {
    GLuint shader = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "shader compile failed:\n%s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint link_program(GLuint vs, GLuint fs) {
    GLuint program = glCreateProgram();
    GLint ok = 0;
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        fprintf(stderr, "program link failed:\n%s\n", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

static int format_components(RhiVertexFormat f) {
    return (f == RHI_FORMAT_FLOAT2) ? 2 : 3;
}

/* ---- lifecycle ---- */
void rhi_configure_window(void) {
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);
}

sol_bool rhi_init(struct GLFWwindow *window) {
    GLint profile = 0, flags = 0;
    g_window = window;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    printf("GL_VERSION : %s\n", glGetString(GL_VERSION));
    printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));

    glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
    glGetIntegerv(GL_CONTEXT_FLAGS, &flags);
    printf("CORE PROFILE  : %s\n", (profile & GL_CONTEXT_CORE_PROFILE_BIT) ? "yes" : "no");
    printf("FWD COMPATIBLE: %s\n", (flags & GL_CONTEXT_FLAG_FORWARD_COMPATIBLE_BIT) ? "yes" : "no");
    return SOL_TRUE;
}

void rhi_shutdown(void) {
    /* the OS reclaims GL objects on exit; nothing required for now */
    g_window = NULL;
}

/* ---- resource creation ---- */
RhiBuffer rhi_create_buffer(RhiBufferType type, const void *data, size_t size) {
    GLuint vbo;
    sol_u32 idx;
    RhiBuffer h;
    (void)type;   /* a GL buffer is typeless; the target matters at BIND time */

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_COPY_WRITE_BUFFER, vbo);   /* neutral: touches no VAO state */
    glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)size, data, GL_STATIC_DRAW);

    idx = slot_alloc(&g_buffer_count, g_buffer_free, &g_buffer_free_count);
    g_buffers[idx] = vbo;
    h.id = idx + 1;
    gl_check("rhi_create_buffer");
    return h;
}

RhiShader rhi_create_shader(const char *vertex_src, const char *fragment_src) {
    RhiShader h;
    GLuint vs, fs, program;
    sol_u32 idx;

    h.id = 0;
    vs = compile_shader(GL_VERTEX_SHADER, vertex_src);
    fs = compile_shader(GL_FRAGMENT_SHADER, fragment_src);
    if (!vs || !fs) return h;

    program = link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!program) return h;

    idx = slot_alloc(&g_shader_count, g_shader_free, &g_shader_free_count);
    g_shaders[idx] = program;
    h.id = idx + 1;
    return h;
}

RhiPipeline rhi_create_pipeline(const RhiPipelineDesc *desc) {
    RhiPipeline h;
    GlPipeline *p;
    sol_u32 idx;
    int i;

    idx = slot_alloc(&g_pipeline_count, g_pipeline_free, &g_pipeline_free_count);
    p = &g_pipelines[idx];

    p->program    = g_shaders[desc->shader.id - 1];
    p->attr_count = desc->attr_count;
    for (i = 0; i < desc->attr_count; i++) p->attrs[i] = desc->attrs[i];
    p->stride     = (GLsizei)desc->stride;
    p->depth_test = desc->depth_test;

    glGenVertexArrays(1, &p->vao);   /* attribs are bound when a buffer is set */

    h.id = idx + 1;
    return h;
}

RhiTexture rhi_create_texture(const void *pixels, int width, int height, RhiTextureFormat fmt) {
    GLuint     tex;
    sol_u32    idx;
    RhiTexture h;
    GLint      internal = (fmt == RHI_TEX_SRGB8) ? GL_SRGB8_ALPHA8 : GL_RGBA8;

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, (GLsizei)width, (GLsizei)height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);     /* source bytes are RGBA8 */
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    idx = slot_alloc(&g_texture_count, g_texture_free, &g_texture_free_count);
    g_textures[idx] = tex;
    h.id = idx + 1;
    gl_check("rhi_create_texture");
    return h;
}

void rhi_bind_texture(RhiTexture texture, int slot) {
    glActiveTexture((GLenum)(GL_TEXTURE0 + slot));
    glBindTexture(GL_TEXTURE_2D, texture.id ? g_textures[texture.id - 1] : 0);
}

RhiRenderTarget rhi_create_render_target(int width, int height, RhiTextureFormat color_format) {
    RhiRenderTarget h;
    GlRenderTarget *rt;
    GLuint          fbo, color_tex, depth_rbo;
    GLint           internal;
    sol_u32         rt_idx, tex_idx;
    GLenum          status;

    h.id = 0;
    internal = (color_format == RHI_TEX_RGBA16F) ? GL_RGBA16F
             : (color_format == RHI_TEX_SRGB8)   ? GL_SRGB8_ALPHA8
             :                                      GL_RGBA8;

    /* color attachment: a samplable texture. NULL data = allocate, don't upload.
       No mipmaps (it's a render target); clamp + linear for the sampling pass. */
    glGenTextures(1, &color_tex);
    glBindTexture(GL_TEXTURE_2D, color_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, (GLsizei)width, (GLsizei)height, 0,
                 GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    /* depth attachment: a renderbuffer — write-only, cheaper than a texture,
       and we never sample the camera's depth in this item. */
    glGenRenderbuffers(1, &depth_rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24,
                          (GLsizei)width, (GLsizei)height);

    /* the framebuffer object: wire color + depth into its slots */
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, color_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, depth_rbo);

    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);            /* restore default before any return */
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "render target incomplete: 0x%04x\n", (unsigned)status);
        glDeleteTextures(1, &color_tex);
        glDeleteRenderbuffers(1, &depth_rbo);
        glDeleteFramebuffers(1, &fbo);
        return h;                                    /* id 0 = failure */
    }

    /* register the color texture in the texture table so rhi_bind_texture and
       rhi_render_target_texture treat it as an ordinary texture handle */
    tex_idx = slot_alloc(&g_texture_count, g_texture_free, &g_texture_free_count);
    g_textures[tex_idx] = color_tex;

    rt_idx = slot_alloc(&g_render_target_count, g_render_target_free, &g_render_target_free_count);
    rt = &g_render_targets[rt_idx];
    rt->fbo       = fbo;
    rt->depth_rbo = depth_rbo;
    rt->color.id  = tex_idx + 1;
    rt->width     = width;
    rt->height    = height;

    h.id = rt_idx + 1;
    gl_check("rhi_create_render_target");
    return h;
}

RhiTexture rhi_render_target_texture(RhiRenderTarget rt) {
    RhiTexture t;
    if (!rt.id) { t.id = 0; return t; }
    return g_render_targets[rt.id - 1].color;
}

/* ---- per frame ---- */
void rhi_begin_pass(RhiRenderTarget target, float r, float g, float b, float a) {
    int w, h;
    if (target.id != 0) {
        const GlRenderTarget *rt = &g_render_targets[target.id - 1];
        glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
        w = rt->width;
        h = rt->height;
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);        /* the window */
        glfwGetFramebufferSize(g_window, &w, &h);    /* backend owns the window ptr */
    }
    glViewport(0, 0, w, h);                          /* viewport does NOT follow the bind */
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void rhi_end_pass(void) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);            /* next begin_pass re-sets viewport */
}

void rhi_set_pipeline(RhiPipeline pipeline) {
    const GlPipeline *p = &g_pipelines[pipeline.id - 1];
    g_current = p;

    glUseProgram(p->program);
    glBindVertexArray(p->vao);
    if (p->depth_test) glEnable(GL_DEPTH_TEST);
    else               glDisable(GL_DEPTH_TEST);
}

void rhi_bind_vertex_buffer(RhiBuffer buffer) {
    const GlPipeline *p = g_current;
    int i;

    glBindBuffer(GL_ARRAY_BUFFER, g_buffers[buffer.id - 1]);

    /* GL 4.1: the VAO bundles format + buffer, so (re)specify the pipeline's
       layout against the just-bound buffer. This is the VAO fiction the
       interface papers over — set_pipeline + bind_buffer being separate. */
    for (i = 0; i < p->attr_count; i++) {
        const RhiVertexAttr *a = &p->attrs[i];
        glVertexAttribPointer(a->location, format_components(a->format),
                              GL_FLOAT, GL_FALSE, p->stride, (const void *)a->offset);
        glEnableVertexAttribArray(a->location);
    }
}

void rhi_bind_index_buffer(RhiBuffer buffer) {
    /* element-buffer binding is VAO state; the pipeline's VAO is current */
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_buffers[buffer.id - 1]);
}

void rhi_set_uniform_mat4(const char *name, const float *m) {
    GLint loc = glGetUniformLocation(g_current->program, name);
    glUniformMatrix4fv(loc, 1, GL_FALSE, m);
}

void rhi_set_uniform_mat3(const char *name, const float *m) {
    GLint loc = glGetUniformLocation(g_current->program, name);
    glUniformMatrix3fv(loc, 1, GL_FALSE, m);   /* column-major, no transpose */
}

void rhi_set_uniform_vec3(const char *name, float x, float y, float z) {
    GLint loc = glGetUniformLocation(g_current->program, name);
    glUniform3f(loc, x, y, z);
}

void rhi_set_uniform_float(const char *name, float v) {
    GLint loc = glGetUniformLocation(g_current->program, name);
    glUniform1f(loc, v);
}

void rhi_set_uniform_int(const char *name, int v) {
    GLint loc = glGetUniformLocation(g_current->program, name);
    glUniform1i(loc, v);
}

void rhi_draw(int first_vertex, int vertex_count) {
    glDrawArrays(GL_TRIANGLES, first_vertex, vertex_count);
}

void rhi_draw_indexed(int first_index, int index_count) {
    const void *offset = (const void *)(first_index * sizeof(sol_u32));
    glDrawElements(GL_TRIANGLES, index_count, GL_UNSIGNED_INT, offset);
    gl_check("rhi_draw_indexed");
}

void rhi_present(void) {
    glfwSwapBuffers(g_window);
}

/* ---- resource teardown (free-list keeps create/destroy loops bounded) ---- */
void rhi_destroy_buffer(RhiBuffer buffer) {
    sol_u32 idx = buffer.id - 1;
    glDeleteBuffers(1, &g_buffers[idx]);
    g_buffers[idx] = 0;
    slot_free(idx, g_buffer_free, &g_buffer_free_count);
    gl_check("rhi_destroy_buffer");
}

void rhi_destroy_shader(RhiShader shader) {
    sol_u32 idx = shader.id - 1;
    glDeleteProgram(g_shaders[idx]);
    g_shaders[idx] = 0;
    slot_free(idx, g_shader_free, &g_shader_free_count);
    gl_check("rhi_destroy_shader");
}

void rhi_destroy_pipeline(RhiPipeline pipeline) {
    sol_u32 idx = pipeline.id - 1;
    glDeleteVertexArrays(1, &g_pipelines[idx].vao);
    g_pipelines[idx].vao = 0;
    g_pipelines[idx].program = 0;   /* not owned here — the shader owns the program */
    slot_free(idx, g_pipeline_free, &g_pipeline_free_count);
    gl_check("rhi_destroy_pipeline");
}

void rhi_destroy_texture(RhiTexture texture) {
    sol_u32 idx;
    if (!texture.id) return;
    idx = texture.id - 1;
    glDeleteTextures(1, &g_textures[idx]);
    slot_free(idx, g_texture_free, &g_texture_free_count);
    gl_check("rhi_destroy_texture");
}

void rhi_destroy_render_target(RhiRenderTarget rt) {
    GlRenderTarget *r;
    sol_u32 idx, tex_idx;
    if (!rt.id) return;
    idx = rt.id - 1;
    r = &g_render_targets[idx];

    glDeleteFramebuffers(1, &r->fbo);
    glDeleteRenderbuffers(1, &r->depth_rbo);

    if (r->color.id) {                               /* release the color texture slot too */
        tex_idx = r->color.id - 1;
        glDeleteTextures(1, &g_textures[tex_idx]);
        g_textures[tex_idx] = 0;
        slot_free(tex_idx, g_texture_free, &g_texture_free_count);
    }

    r->fbo = 0; r->depth_rbo = 0; r->color.id = 0;
    slot_free(idx, g_render_target_free, &g_render_target_free_count);
    gl_check("rhi_destroy_render_target");
}
