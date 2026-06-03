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
    "layout (location = 1) in vec3 aNormal;\n"
    "uniform mat4 uModel;\n"
    "uniform mat4 uView;\n"
    "uniform mat4 uProj;\n"
    "out vec3 vNormal;\n"
    "void main() {\n"
    "    gl_Position = uProj * uView * uModel * vec4(aPos, 1.0);\n"
    "    vNormal = aNormal;\n"                /* object-space, for debug viz */
    "}\n";

static const char *FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec3 vNormal;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "    FragColor = vec4(vNormal * 0.5 + 0.5, 1.0);\n"   /* [-1,1] -> [0,1] RGB */
    "}\n";

typedef struct {
    GLuint  vao;
    GLsizei vertex_count;
} Mesh;

typedef struct {
    int    fb_width;
    int    fb_height;
    GLuint program;
    Mesh   cube;
    GLint  u_model_loc, u_view_loc, u_proj_loc;
    float  angle;
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

/* --- a growable float array (doubles capacity on demand) --- */
typedef struct { float *data; size_t count, capacity; } FloatArray;

static void fa_push(FloatArray *a, float v) {
    if (a->count == a->capacity) {
        a->capacity = a->capacity ? a->capacity * 2 : 64;   /* double */
        a->data = realloc(a->data, a->capacity * sizeof(float));
    }
    a->data[a->count++] = v;
}

/* Parse an OBJ into an interleaved [px,py,pz, nx,ny,nz] vertex array.
   Returns a malloc'd array (caller frees), vertex count via out_count.
   NULL on failure. Pure CPU — no GL here. */
static float *load_obj(const char *path, int *out_count) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "load_obj: cannot open %s\n", path); return NULL; }

    FloatArray positions = {0}, normals = {0}, out = {0};
    char line[256];

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == 'v' && line[1] == ' ') {                 /* position */
            float x, y, z;
            sscanf(line, "v %f %f %f", &x, &y, &z);
            fa_push(&positions, x); fa_push(&positions, y); fa_push(&positions, z);
        } else if (line[0] == 'v' && line[1] == 'n') {          /* normal */
            float x, y, z;
            sscanf(line, "vn %f %f %f", &x, &y, &z);
            fa_push(&normals, x); fa_push(&normals, y); fa_push(&normals, z);
        } else if (line[0] == 'f' && line[1] == ' ') {          /* face (3 corners) */
            char tok[3][64];
            if (sscanf(line, "f %63s %63s %63s", tok[0], tok[1], tok[2]) != 3) continue;
            for (int i = 0; i < 3; i++) {
                int p = 0, t = 0, n = 0;
                if      (sscanf(tok[i], "%d/%d/%d", &p, &t, &n) == 3) {}   /* p/t/n */
                else if (sscanf(tok[i], "%d//%d",  &p, &n)      == 2) {}   /* p//n  */
                else if (sscanf(tok[i], "%d/%d",   &p, &t)      == 2) {}   /* p/t   */
                else     sscanf(tok[i], "%d", &p);                        /* p     */

                int pi = (p - 1) * 3;                  /* 1-based -> 0-based */
                fa_push(&out, positions.data[pi+0]);
                fa_push(&out, positions.data[pi+1]);
                fa_push(&out, positions.data[pi+2]);
                if (n > 0) {
                    int ni = (n - 1) * 3;
                    fa_push(&out, normals.data[ni+0]);
                    fa_push(&out, normals.data[ni+1]);
                    fa_push(&out, normals.data[ni+2]);
                } else {                               /* no normal -> zero */
                    fa_push(&out, 0); fa_push(&out, 0); fa_push(&out, 0);
                }
            }
        }
        /* comments / blank / o,g,usemtl,s -> skipped */
    }
    fclose(f);

    free(positions.data);   /* scaffolding — done with it */
    free(normals.data);

    *out_count = (int)(out.count / 6);   /* 6 floats per vertex */
    return out.data;
}

/* Upload an interleaved pos+normal vertex array to a VAO/VBO. */
static Mesh mesh_create(const float *verts, int vertex_count) {
    Mesh mesh = { .vertex_count = vertex_count };

    GLuint vbo;
    glGenVertexArrays(1, &mesh.vao);
    glGenBuffers(1, &vbo);

    glBindVertexArray(mesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(vertex_count * 6 * sizeof(float)),
                 verts, GL_STATIC_DRAW);

    /* position: slot 0, 3 floats, stride 6 floats, offset 0 */
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    /* normal: slot 1, 3 floats, SAME stride, offset 3 floats in */
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                          (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);
    return mesh;
}

/* One-time setup: load the mesh, build the program. Runs once, before the loop. */
static int init_scene(AppState *state) {
    int vertex_count = 0;
    float *verts = load_obj("cube.obj", &vertex_count);
    if (!verts || vertex_count == 0) {
        fprintf(stderr, "init_scene: failed to load cube.obj\n");
        free(verts);
        return 0;
    }
    printf("loaded cube.obj: %d vertices\n", vertex_count);   /* expect 36 */

    GLuint vs = compile_shader(GL_VERTEX_SHADER, VERTEX_SRC);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FRAGMENT_SRC);
    if (!vs || !fs) { free(verts); return 0; }

    state->program = link_program(vs, fs);
    glDeleteShader(vs);   /* the program owns the compiled code now; */
    glDeleteShader(fs);   /* the standalone shader objects can go    */
    if (!state->program) { free(verts); return 0; }

    state->u_model_loc = glGetUniformLocation(state->program, "uModel");
    state->u_view_loc  = glGetUniformLocation(state->program, "uView");
    state->u_proj_loc  = glGetUniformLocation(state->program, "uProj");

    state->cube = mesh_create(verts, vertex_count);
    free(verts);   /* GPU has its own copy now — CPU array is done */

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

    glBindVertexArray(state->cube.vao);
    glDrawArrays(GL_TRIANGLES, 0, state->cube.vertex_count);
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
        fprintf(stderr, "scene init failed\n");
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
