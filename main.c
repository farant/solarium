#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>          /* platform: window, input, time — not GL */

#include "rhi.h"                 /* the graphics seam — no GL above here */
#include "sol_math.h"

/* --- shaders: GLSL source handed to the backend (still app-authored) --- */
static const char *VERTEX_SRC =
    "#version 330 core\n"
    "layout (location = 0) in vec3 aPos;\n"
    "layout (location = 1) in vec3 aNormal;\n"
    "uniform mat4 uModel;\n"
    "uniform mat4 uView;\n"
    "uniform mat4 uProj;\n"
    "out vec3 vNormal;\n"
    "out vec3 vWorldPos;\n"
    "void main() {\n"
    "    vec4 worldPos = uModel * vec4(aPos, 1.0);\n"
    "    gl_Position = uProj * uView * worldPos;\n"
    "    vNormal = mat3(uModel) * aNormal;\n"   /* rotate normal into world space */
    "    vWorldPos = worldPos.xyz;\n"
    "}\n";

static const char *FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec3 vNormal;\n"
    "in vec3 vWorldPos;\n"
    "uniform vec3 uViewPos;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "    vec3 N = normalize(vNormal);\n"                       /* renormalize after interp */
    "    vec3 L = normalize(vec3(0.4, 1.0, 0.6));\n"           /* direction TO the light */
    "    vec3 V = normalize(uViewPos - vWorldPos);\n"          /* direction TO the camera */
    "    vec3 H = normalize(L + V);\n"                         /* half-vector */
    "\n"
    "    vec3 lightColor = vec3(1.0, 0.98, 0.92);\n"
    "    vec3 baseColor  = vec3(0.85, 0.45, 0.35);\n"          /* the object's color */
    "    float ambient   = 0.15;\n"
    "    float shininess = 48.0;\n"
    "\n"
    "    float diff = max(dot(N, L), 0.0);\n"                  /* Lambert diffuse */
    "    float spec = (diff > 0.0) ? pow(max(dot(N, H), 0.0), shininess) : 0.0;\n"
    "    vec3 color = baseColor * ambient\n"
    "               + baseColor * lightColor * diff\n"
    "               + lightColor * spec;\n"                    /* Blinn-Phong highlight */
    "    color = pow(color, vec3(1.0 / 2.2));\n"              /* linear -> sRGB for display */
    "    FragColor = vec4(color, 1.0);\n"
    "}\n";

typedef struct { RhiBuffer buffer; int vertex_count; } Mesh;

typedef struct {
    int         fb_width, fb_height;
    RhiPipeline pipeline;
    Mesh        mesh;
    float       angle;
} AppState;

/* --- a growable float array (doubles capacity on demand) --- */
typedef struct { float *data; size_t count, capacity; } FloatArray;

static void fa_push(FloatArray *a, float v) {
    if (a->count == a->capacity) {
        a->capacity = a->capacity ? a->capacity * 2 : 64;   /* double */
        a->data = realloc(a->data, a->capacity * sizeof(float));
    }
    a->data[a->count++] = v;
}

/* Append one OBJ corner (1-based position p, normal n) as 6 floats. */
static void push_vertex(FloatArray *out, const FloatArray *pos, const FloatArray *nrm,
                        int p, int n) {
    int pi = (p - 1) * 3;                       /* 1-based -> 0-based */
    fa_push(out, pos->data[pi+0]);
    fa_push(out, pos->data[pi+1]);
    fa_push(out, pos->data[pi+2]);
    if (n > 0) {
        int ni = (n - 1) * 3;
        fa_push(out, nrm->data[ni+0]);
        fa_push(out, nrm->data[ni+1]);
        fa_push(out, nrm->data[ni+2]);
    } else {                                    /* no normal -> zero */
        fa_push(out, 0); fa_push(out, 0); fa_push(out, 0);
    }
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
        } else if (line[0] == 'f' && line[1] == ' ') {   /* face: tri / quad / n-gon */
            int cp[16], cn[16], nc = 0;                  /* per-corner pos/normal idx */
            char *tok = strtok(line + 1, " \t\r\n");
            while (tok && nc < 16) {
                int p = 0, t = 0, n = 0;
                if      (sscanf(tok, "%d/%d/%d", &p, &t, &n) == 3) {}   /* p/t/n */
                else if (sscanf(tok, "%d//%d",  &p, &n)      == 2) {}   /* p//n  */
                else if (sscanf(tok, "%d/%d",   &p, &t)      == 2) {}   /* p/t   */
                else     sscanf(tok, "%d", &p);                        /* p     */
                cp[nc] = p; cn[nc] = n; nc++;
                tok = strtok(NULL, " \t\r\n");
            }
            /* fan-triangulate: (0,1,2), (0,2,3), ... handles tris and quads */
            for (int i = 2; i < nc; i++) {
                push_vertex(&out, &positions, &normals, cp[0],   cn[0]);
                push_vertex(&out, &positions, &normals, cp[i-1], cn[i-1]);
                push_vertex(&out, &positions, &normals, cp[i],   cn[i]);
            }
        }
        /* comments / blank / o,g,usemtl,s -> skipped */
    }
    fclose(f);

    free(positions.data);   /* scaffolding — done with it */
    free(normals.data);

    /* recenter on the bounding-box center so the mesh rotates in place,
       regardless of where it was authored in model space */
    if (out.count >= 6) {
        float lo[3], hi[3];
        for (int k = 0; k < 3; k++) lo[k] = hi[k] = out.data[k];
        for (size_t v = 0; v < out.count; v += 6) {     /* positions are first 3 of 6 */
            for (int k = 0; k < 3; k++) {
                float c = out.data[v + k];
                if (c < lo[k]) lo[k] = c;
                if (c > hi[k]) hi[k] = c;
            }
        }
        float center[3] = { (lo[0]+hi[0])*0.5f, (lo[1]+hi[1])*0.5f, (lo[2]+hi[2])*0.5f };
        for (size_t v = 0; v < out.count; v += 6)
            for (int k = 0; k < 3; k++) out.data[v + k] -= center[k];
    }

    *out_count = (int)(out.count / 6);   /* 6 floats per vertex */
    return out.data;
}

/* One-time setup: load the mesh, build the pipeline. Runs once, before the loop. */
static int init_scene(AppState *state) {
    const char *model_path = "suzanne.obj";   /* swap to "cube.obj" to test the cube */
    int vertex_count = 0;
    float *verts = load_obj(model_path, &vertex_count);
    if (!verts || vertex_count == 0) {
        fprintf(stderr, "init_scene: failed to load %s\n", model_path);
        free(verts);
        return 0;
    }
    printf("loaded %s: %d vertices\n", model_path, vertex_count);

    RhiShader shader = rhi_create_shader(VERTEX_SRC, FRAGMENT_SRC);
    if (!shader.id) { free(verts); return 0; }

    RhiPipelineDesc desc = {
        .shader = shader,
        .attrs = {
            { .location = 0, .format = RHI_FORMAT_FLOAT3, .offset = 0 },
            { .location = 1, .format = RHI_FORMAT_FLOAT3, .offset = 3 * sizeof(float) },
        },
        .attr_count = 2,
        .stride     = 6 * sizeof(float),
        .depth_test = true,
    };
    state->pipeline = rhi_create_pipeline(&desc);

    state->mesh.buffer       = rhi_create_buffer(RHI_BUFFER_VERTEX, verts,
                                   (size_t)vertex_count * 6 * sizeof(float));
    state->mesh.vertex_count = vertex_count;
    free(verts);   /* GPU has its own copy now — CPU array is done */
    return 1;
}

static void update(AppState *state, double dt) {
    state->angle += (float)dt * 0.8f;   /* radians/sec; tweak to taste */
}

static void render(const AppState *state) {
    rhi_begin_frame(state->fb_width, state->fb_height, 0.10f, 0.12f, 0.15f, 1.0f);

    float aspect = (state->fb_height > 0)
                 ? (float)state->fb_width / (float)state->fb_height
                 : 1.0f;

    vec3 eye = vec3_make(0, 0, 5);   /* pushed back to fit Suzanne while tumbling */

    mat4 model = mat4_mul(mat4_rotate_y(state->angle),
                          mat4_rotate_x(state->angle * 0.5f));
    mat4 view  = mat4_look_at(eye,
                              vec3_make(0, 0, 0),    /* looking at the origin */
                              vec3_make(0, 1, 0));   /* up is +Y              */
    mat4 proj  = mat4_perspective(sol_radians(45.0f), aspect, 0.1f, 100.0f);

    rhi_set_pipeline(state->pipeline);
    rhi_set_uniform_mat4("uModel",   model.m);
    rhi_set_uniform_mat4("uView",    view.m);
    rhi_set_uniform_mat4("uProj",    proj.m);
    rhi_set_uniform_vec3("uViewPos", eye.x, eye.y, eye.z);

    rhi_bind_vertex_buffer(state->mesh.buffer);
    rhi_draw(0, state->mesh.vertex_count);
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

    rhi_configure_window();   /* backend sets the API/context hints */

    GLFWwindow *window = glfwCreateWindow(960, 540, "solarium", NULL, NULL);
    if (!window) {
        fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return EXIT_FAILURE;
    }

    glfwSetKeyCallback(window, on_key);   /* platform: input */

    if (!rhi_init(window)) {              /* backend: context + GL info */
        fprintf(stderr, "rhi_init failed\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    AppState state = {0};
    if (!init_scene(&state)) {
        fprintf(stderr, "scene init failed\n");
        rhi_shutdown();
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

        rhi_present();
    }

    rhi_shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}
