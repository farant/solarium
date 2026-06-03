#define GL_SILENCE_DEPRECATION

#include <stdio.h>
#include <stdlib.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <OpenGL/gl3.h>

/* --- shaders: plain text, compiled at runtime by the driver --- */
static const char *VERTEX_SRC =
    "#version 330 core\n"
    "layout (location = 0) in vec3 aPos;\n"   /* input slot 0: a vec3 position */
    "layout (location = 1) in vec2 aUV;\n"    /* input slot 1: texture coords */
    "uniform float u_angle;\n"                /* per-draw constant, set each frame */
    "out vec2 vUV;\n"                         /* hand UV to the fragment stage */
    "void main() {\n"
    "    float c = cos(u_angle);\n"
    "    float s = sin(u_angle);\n"
    "    vec2 r = vec2(aPos.x * c - aPos.y * s,\n"   /* 2D rotation, hand-rolled */
    "                  aPos.x * s + aPos.y * c);\n"
    "    gl_Position = vec4(r, aPos.z, 1.0);\n"      /* still NDC — no matrices yet */
    "    vUV = aUV;\n"                               /* rasterizer interpolates this */
    "}\n";

static const char *FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec2 vUV;\n"                          /* interpolated UV for this pixel */
    "out vec4 FragColor;\n"
    "uniform sampler2D uTex;\n"               /* holds a texture UNIT number */
    "void main() {\n"
    "    FragColor = texture(uTex, vUV);\n"   /* fetch the texel at this UV */
    "}\n";

typedef struct {
    int    fb_width;
    int    fb_height;
    GLuint program;       /* the linked shader logic */
    GLuint vao;           /* how to read the vertex data */
    GLuint tex;           /* the checker texture, bound to unit 0 */
    GLint  u_angle_loc;   /* where u_angle lives in the program (queried once) */
    float  angle;         /* the simulation state update() advances */
} AppState;

/* Compile one shader, and ACTUALLY CHECK it — silent failure otherwise */
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

/* Link vertex+fragment into one program, and check the link too */
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

/* One-time GPU setup: program + VBO + VAO. Runs once, before the loop. */
static int init_scene(AppState *state) {
    /* interleaved: x, y, z, u, v  — a quad = two triangles = 6 vertices */
    static const float vertices[] = {
        -0.5f, -0.5f, 0.0f,   0.0f, 0.0f,   /* tri 1 */
         0.5f, -0.5f, 0.0f,   1.0f, 0.0f,
         0.5f,  0.5f, 0.0f,   1.0f, 1.0f,

        -0.5f, -0.5f, 0.0f,   0.0f, 0.0f,   /* tri 2 — 2 verts are DUPLICATES */
         0.5f,  0.5f, 0.0f,   1.0f, 1.0f,
        -0.5f,  0.5f, 0.0f,   0.0f, 1.0f,
    };

    /* a 2x2 RGB checker, four distinct colors so the mapping is legible */
    static const unsigned char pixels[] = {
        255,  80,  80,    80, 255,  80,   /* row 0: red,   green  */
         80,  80, 255,   240, 240,  80,   /* row 1: blue,  yellow */
    };

    GLuint vs = compile_shader(GL_VERTEX_SHADER, VERTEX_SRC);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FRAGMENT_SRC);
    if (!vs || !fs) return 0;

    state->program = link_program(vs, fs);
    glDeleteShader(vs);   /* the program owns the compiled code now; */
    glDeleteShader(fs);   /* the standalone shader objects can go    */
    if (!state->program) return 0;

    state->u_angle_loc = glGetUniformLocation(state->program, "u_angle");
    if (state->u_angle_loc == -1) {
        fprintf(stderr, "warning: u_angle not found (optimized out?)\n");
    }

    /* --- geometry: VBO + VAO with TWO interleaved attributes --- */
    GLuint vbo;
    glGenVertexArrays(1, &state->vao);
    glGenBuffers(1, &vbo);

    glBindVertexArray(state->vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    /* position: slot 0, 3 floats, stride 5 floats, offset 0 */
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    /* uv: slot 1, 2 floats, SAME stride, offset 3 floats in */
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);

    /* --- texture: hardcoded pixels, unit 0 --- */
    glGenTextures(1, &state->tex);
    glActiveTexture(GL_TEXTURE0);                 /* select unit 0 */
    glBindTexture(GL_TEXTURE_2D, state->tex);     /* bind our texture into it */

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);        /* rows are 6 bytes, not a mult of 4 */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 2, 0, GL_RGB, GL_UNSIGNED_BYTE, pixels);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);  /* blocky */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    /* tell the sampler to read from unit 0 (set once; program must be bound) */
    glUseProgram(state->program);
    glUniform1i(glGetUniformLocation(state->program, "uTex"), 0);

    return 1;
}

static void update(AppState *state, double dt) {
    state->angle += (float)dt * 1.5f;   /* radians/sec; tweak to taste */
}

static void render(const AppState *state) {
    glViewport(0, 0, state->fb_width, state->fb_height);
    glClearColor(0.10f, 0.12f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(state->program);                      /* bind FIRST...       */
    glUniform1f(state->u_angle_loc, state->angle);     /* ...then set uniform */
    glBindVertexArray(state->vao);                     /* feed it this data   */
    glDrawArrays(GL_TRIANGLES, 0, 6);                  /* 6 verts = 2 tris = 1 quad */
}

static void on_key(GLFWwindow *window, int key, int scancode, int action, int mods) {
    (void)scancode;
    (void)mods;
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
}

int main(void) {
    if (!glfwInit()) {
        fprintf(stderr, "glfwInit failed\n");
        return EXIT_FAILURE;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

    GLFWwindow *window = glfwCreateWindow(960, 540, "solarium", NULL, NULL);
    if (!window) {
        fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return EXIT_FAILURE;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glfwSetKeyCallback(window, on_key);

    printf("GL_VERSION : %s\n", glGetString(GL_VERSION));
    printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));

    GLint profile = 0, flags = 0;
    glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
    glGetIntegerv(GL_CONTEXT_FLAGS, &flags);
    printf("CORE PROFILE  : %s\n", (profile & GL_CONTEXT_CORE_PROFILE_BIT) ? "yes" : "no");
    printf("FWD COMPATIBLE: %s\n", (flags & GL_CONTEXT_FLAG_FORWARD_COMPATIBLE_BIT) ? "yes" : "no");

    AppState state = {0};

    if (!init_scene(&state)) {              /* one-time setup */
        fprintf(stderr, "triangle init failed\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    double last = glfwGetTime();

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        double now = glfwGetTime();
        double dt  = now - last;
        last = now;
        if (dt > 0.1) dt = 0.1;   /* clamp: a long stall pauses motion, never lurches */

        glfwGetFramebufferSize(window, &state.fb_width, &state.fb_height);

        update(&state, dt);
        render(&state);

        glfwSwapBuffers(window);
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}
