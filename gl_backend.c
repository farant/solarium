/* gl_backend.c — the OpenGL implementation of rhi.h.
   This is the ONLY file that includes a GL header. All the GL state,
   the bind-to-edit dance, and the silent-failure traps live in here. */

#define GL_SILENCE_DEPRECATION

#include "rhi.h"

#include <stdio.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <OpenGL/gl3.h>

/* ---- internal resource storage (handle id -> GL object) ---- */
#define MAX_BUFFERS   256
#define MAX_SHADERS    64
#define MAX_PIPELINES  64

typedef struct {
    GLuint        program;
    GLuint        vao;
    RhiVertexAttr attrs[RHI_MAX_ATTRS];
    int           attr_count;
    GLsizei       stride;
    bool          depth_test;
} GlPipeline;

static GLuint     g_buffers[MAX_BUFFERS];
static uint32_t   g_buffer_count;
static GLuint     g_shaders[MAX_SHADERS];
static uint32_t   g_shader_count;
static GlPipeline g_pipelines[MAX_PIPELINES];
static uint32_t   g_pipeline_count;

static struct GLFWwindow *g_window;
static const GlPipeline  *g_current;   /* pipeline bound by rhi_set_pipeline */

/* ---- shader compile/link (moved here from main.c — it's GL) ---- */
static GLuint compile_shader(GLenum type, const char *src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint ok = 0;
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
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    GLint ok = 0;
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

bool rhi_init(struct GLFWwindow *window) {
    g_window = window;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    printf("GL_VERSION : %s\n", glGetString(GL_VERSION));
    printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));

    GLint profile = 0, flags = 0;
    glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
    glGetIntegerv(GL_CONTEXT_FLAGS, &flags);
    printf("CORE PROFILE  : %s\n", (profile & GL_CONTEXT_CORE_PROFILE_BIT) ? "yes" : "no");
    printf("FWD COMPATIBLE: %s\n", (flags & GL_CONTEXT_FLAG_FORWARD_COMPATIBLE_BIT) ? "yes" : "no");
    return true;
}

void rhi_shutdown(void) {
    /* the OS reclaims GL objects on exit; nothing required for now */
    g_window = NULL;
}

/* ---- resource creation ---- */
RhiBuffer rhi_create_buffer(RhiBufferType type, const void *data, size_t size) {
    GLenum target = (type == RHI_BUFFER_INDEX) ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(target, vbo);
    glBufferData(target, (GLsizeiptr)size, data, GL_STATIC_DRAW);

    uint32_t idx = g_buffer_count++;
    g_buffers[idx] = vbo;
    RhiBuffer h = { .id = idx + 1 };
    return h;
}

RhiShader rhi_create_shader(const char *vertex_src, const char *fragment_src) {
    RhiShader h = { .id = 0 };
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_src);
    if (!vs || !fs) return h;

    GLuint program = link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!program) return h;

    uint32_t idx = g_shader_count++;
    g_shaders[idx] = program;
    h.id = idx + 1;
    return h;
}

RhiPipeline rhi_create_pipeline(const RhiPipelineDesc *desc) {
    uint32_t idx = g_pipeline_count++;
    GlPipeline *p = &g_pipelines[idx];

    p->program    = g_shaders[desc->shader.id - 1];
    p->attr_count = desc->attr_count;
    for (int i = 0; i < desc->attr_count; i++) p->attrs[i] = desc->attrs[i];
    p->stride     = (GLsizei)desc->stride;
    p->depth_test = desc->depth_test;

    glGenVertexArrays(1, &p->vao);   /* attribs are bound when a buffer is set */

    RhiPipeline h = { .id = idx + 1 };
    return h;
}

/* ---- per frame ---- */
void rhi_begin_frame(int fb_width, int fb_height, float r, float g, float b, float a) {
    glViewport(0, 0, fb_width, fb_height);
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
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
    glBindBuffer(GL_ARRAY_BUFFER, g_buffers[buffer.id - 1]);

    /* GL 4.1: the VAO bundles format + buffer, so (re)specify the pipeline's
       layout against the just-bound buffer. This is the VAO fiction the
       interface papers over — set_pipeline + bind_buffer being separate. */
    for (int i = 0; i < p->attr_count; i++) {
        const RhiVertexAttr *a = &p->attrs[i];
        glVertexAttribPointer(a->location, format_components(a->format),
                              GL_FLOAT, GL_FALSE, p->stride, (const void *)a->offset);
        glEnableVertexAttribArray(a->location);
    }
}

void rhi_set_uniform_mat4(const char *name, const float *m) {
    GLint loc = glGetUniformLocation(g_current->program, name);
    glUniformMatrix4fv(loc, 1, GL_FALSE, m);
}

void rhi_set_uniform_vec3(const char *name, float x, float y, float z) {
    GLint loc = glGetUniformLocation(g_current->program, name);
    glUniform3f(loc, x, y, z);
}

void rhi_draw(int first_vertex, int vertex_count) {
    glDrawArrays(GL_TRIANGLES, first_vertex, vertex_count);
}

void rhi_present(void) {
    glfwSwapBuffers(g_window);
}
