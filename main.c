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
    "void main() {\n"
    "    gl_Position = vec4(aPos, 1.0);\n"     /* already in NDC — no matrices */
    "}\n";

static const char *FRAGMENT_SRC =
    "#version 330 core\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "    FragColor = vec4(1.0, 0.5, 0.2, 1.0);\n"  /* one constant warm orange */
    "}\n";

typedef struct {
    int    fb_width;
    int    fb_height;
    GLuint program;   /* the linked shader logic */
    GLuint vao;       /* how to read the vertex data */
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
static int init_triangle(AppState *state) {
    static const float vertices[] = {
        -0.5f, -0.5f, 0.0f,   /* bottom-left  */
         0.5f, -0.5f, 0.0f,   /* bottom-right */
         0.0f,  0.5f, 0.0f,   /* top          */
    };

    GLuint vs = compile_shader(GL_VERTEX_SHADER, VERTEX_SRC);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FRAGMENT_SRC);
    if (!vs || !fs) return 0;

    state->program = link_program(vs, fs);
    glDeleteShader(vs);   /* the program owns the compiled code now; */
    glDeleteShader(fs);   /* the standalone shader objects can go    */
    if (!state->program) return 0;

    GLuint vbo;
    glGenVertexArrays(1, &state->vao);
    glGenBuffers(1, &vbo);

    glBindVertexArray(state->vao);              /* record into THIS vao... */

    glBindBuffer(GL_ARRAY_BUFFER, vbo);         /* ...the data lives here... */
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    /* ...and THIS is how to read it: slot 0, 3 floats, stride 12, offset 0 */
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);

    glBindVertexArray(0);                        /* done recording */
    return 1;
}

static void update(AppState *state, double dt) {
    (void)state;
    (void)dt;
}

static void render(const AppState *state) {
    glViewport(0, 0, state->fb_width, state->fb_height);
    glClearColor(0.10f, 0.12f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(state->program);          /* use this logic     */
    glBindVertexArray(state->vao);         /* feed it this data  */
    glDrawArrays(GL_TRIANGLES, 0, 3);      /* pull the trigger   */
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

    if (!init_triangle(&state)) {           /* one-time setup */
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

        glfwGetFramebufferSize(window, &state.fb_width, &state.fb_height);

        update(&state, dt);
        render(&state);

        glfwSwapBuffers(window);
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}
