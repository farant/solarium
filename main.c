#define GL_SILENCE_DEPRECATION

#include <stdio.h>
#include <stdlib.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <OpenGL/gl3.h>

#include "sol_math.h"

/* --- shaders: plain text, compiled at runtime by the driver --- */
static const char *VERTEX_SRC =
    "#version 330 core\n"
    "layout (location = 0) in vec3 aPos;\n"
    "layout (location = 1) in vec3 aColor;\n"
    "uniform mat4 uModel;\n"
    "uniform mat4 uView;\n"
    "uniform mat4 uProj;\n"
    "out vec3 vColor;\n"
    "void main() {\n"
    "    gl_Position = uProj * uView * uModel * vec4(aPos, 1.0);\n"  /* the pipeline */
    "    vColor = aColor;\n"
    "}\n";

static const char *FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec3 vColor;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "    FragColor = vec4(vColor, 1.0);\n"
    "}\n";

typedef struct {
    int    fb_width;
    int    fb_height;
    GLuint  program;      /* the linked shader logic */
    GLuint  vao;          /* how to read the vertex data */
    GLsizei index_count;  /* how many indices to draw */
    GLint   u_model_loc, u_view_loc, u_proj_loc;
    float   angle;        /* the simulation state update() advances */
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
    /* 8 corners: x,y,z  then  r,g,b (color = position + 0.5 -> the RGB color cube) */
    static const float vertices[] = {
        -0.5f,-0.5f,-0.5f,  0,0,0,   /* 0 */
         0.5f,-0.5f,-0.5f,  1,0,0,   /* 1 */
         0.5f, 0.5f,-0.5f,  1,1,0,   /* 2 */
        -0.5f, 0.5f,-0.5f,  0,1,0,   /* 3 */
        -0.5f,-0.5f, 0.5f,  0,0,1,   /* 4 */
         0.5f,-0.5f, 0.5f,  1,0,1,   /* 5 */
         0.5f, 0.5f, 0.5f,  1,1,1,   /* 6 */
        -0.5f, 0.5f, 0.5f,  0,1,1,   /* 7 */
    };
    /* 12 triangles (2 per face). 8 verts reused 36 times — the index payoff. */
    static const unsigned int indices[] = {
        0,1,2, 2,3,0,   /* back   */
        4,5,6, 6,7,4,   /* front  */
        0,3,7, 7,4,0,   /* left   */
        1,5,6, 6,2,1,   /* right  */
        0,4,5, 5,1,0,   /* bottom */
        3,2,6, 6,7,3,   /* top    */
    };
    state->index_count = sizeof(indices) / sizeof(indices[0]);

    GLuint vs = compile_shader(GL_VERTEX_SHADER, VERTEX_SRC);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FRAGMENT_SRC);
    if (!vs || !fs) return 0;

    state->program = link_program(vs, fs);
    glDeleteShader(vs);   /* the program owns the compiled code now; */
    glDeleteShader(fs);   /* the standalone shader objects can go    */
    if (!state->program) return 0;

    state->u_model_loc = glGetUniformLocation(state->program, "uModel");
    state->u_view_loc  = glGetUniformLocation(state->program, "uView");
    state->u_proj_loc  = glGetUniformLocation(state->program, "uProj");

    GLuint vbo, ebo;
    glGenVertexArrays(1, &state->vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);

    glBindVertexArray(state->vao);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    /* the index buffer — bound while the VAO is bound, so the VAO records it */
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    /* position: slot 0, 3 floats, stride 6 floats, offset 0 */
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    /* color: slot 1, 3 floats, SAME stride, offset 3 floats in */
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                          (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);   /* unbind VAO (NOT the EBO first — VAO has it recorded) */

    glEnable(GL_DEPTH_TEST);  /* nearer fragments win */
    return 1;
}

static void update(AppState *state, double dt) {
    state->angle += (float)dt * 0.8f;   /* radians/sec; tweak to taste */
}

static void render(const AppState *state) {
    glViewport(0, 0, state->fb_width, state->fb_height);
    glClearColor(0.10f, 0.12f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);   /* clear BOTH every frame */

    float aspect = (state->fb_height > 0)
                 ? (float)state->fb_width / (float)state->fb_height
                 : 1.0f;

    mat4 model = mat4_mul(mat4_rotate_y(state->angle),
                          mat4_rotate_x(state->angle * 0.5f));
    mat4 view  = mat4_look_at(vec3_make(0, 0, 3),    /* camera pushed back on +Z */
                              vec3_make(0, 0, 0),    /* looking at the origin     */
                              vec3_make(0, 1, 0));   /* up is +Y                  */
    mat4 proj  = mat4_perspective(sol_radians(45.0f), aspect, 0.1f, 100.0f);

    glUseProgram(state->program);
    glUniformMatrix4fv(state->u_model_loc, 1, GL_FALSE, model.m);
    glUniformMatrix4fv(state->u_view_loc,  1, GL_FALSE, view.m);
    glUniformMatrix4fv(state->u_proj_loc,  1, GL_FALSE, proj.m);

    glBindVertexArray(state->vao);
    glDrawElements(GL_TRIANGLES, state->index_count, GL_UNSIGNED_INT, 0);
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
    glfwWindowHint(GLFW_DEPTH_BITS, 24);   /* we depend on a depth buffer */

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
