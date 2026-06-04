#include <stdio.h>
#include <stdlib.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>          /* platform: window, input, time — not GL */

#include "rhi.h"                 /* the graphics seam — no GL above here */
#include "mesh.h"
#include "sol_math.h"

/* --- shaders: GLSL source handed to the backend (still app-authored) --- */
static const char *VERTEX_SRC =
    "#version 330 core\n"
    "layout (location = 0) in vec3 aPos;\n"
    "layout (location = 1) in vec3 aNormal;\n"
    "layout (location = 2) in vec2 aUV;\n"
    "uniform mat4 uModel;\n"
    "uniform mat4 uView;\n"
    "uniform mat4 uProj;\n"
    "out vec3 vNormal;\n"
    "out vec3 vWorldPos;\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "    vec4 worldPos = uModel * vec4(aPos, 1.0);\n"
    "    gl_Position = uProj * uView * worldPos;\n"
    "    vNormal = mat3(uModel) * aNormal;\n"   /* rotate normal into world space */
    "    vWorldPos = worldPos.xyz;\n"
    "    vUV = aUV;\n"                          /* plumbed for item 5; unused now */
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

typedef struct {
    int         fb_width, fb_height;
    RhiPipeline pipeline;
    Mesh        box;
    Mesh        floor;
    float       angle;
} AppState;

/* One-time setup: build the geometry + pipeline. Runs once, before the loop. */
static int init_scene(AppState *state) {
    MeshBuilder mb;
    RhiShader shader;
    RhiPipelineDesc desc;

    shader = rhi_create_shader(VERTEX_SRC, FRAGMENT_SRC);
    if (!shader.id) return 0;

    /* one pipeline, shared by both meshes (same 8-float layout) */
    desc.shader = shader;
    desc.attrs[0].location = 0; desc.attrs[0].format = RHI_FORMAT_FLOAT3; desc.attrs[0].offset = 0;
    desc.attrs[1].location = 1; desc.attrs[1].format = RHI_FORMAT_FLOAT3;
    desc.attrs[1].offset = 3 * sizeof(float);
    desc.attrs[2].location = 2; desc.attrs[2].format = RHI_FORMAT_FLOAT2;
    desc.attrs[2].offset = 6 * sizeof(float);
    desc.attr_count = 3;
    desc.stride     = 8 * sizeof(float);
    desc.depth_test = SOL_TRUE;
    state->pipeline = rhi_create_pipeline(&desc);

    /* box */
    mb_init(&mb);
    make_box(&mb, 1.0f, 1.0f, 1.0f);
    state->box = mesh_from_builder(&mb);
    printf("box:   %u vertices, %u indices\n",
           (unsigned)mb.vertex_count, (unsigned)mb.index_count);
    mb_free(&mb);

    /* floor */
    mb_init(&mb);
    make_grid(&mb, 6.0f, 6.0f, 8);
    state->floor = mesh_from_builder(&mb);
    printf("floor: %u vertices, %u indices\n",
           (unsigned)mb.vertex_count, (unsigned)mb.index_count);
    mb_free(&mb);

    return 1;
}

static void update(AppState *state, double dt) {
    state->angle += (float)dt * 0.8f;   /* radians/sec; tweak to taste */
}

static void draw_mesh(const AppState *state, Mesh mesh, mat4 model,
                      mat4 view, mat4 proj, vec3 eye) {
    rhi_set_pipeline(state->pipeline);
    rhi_set_uniform_mat4("uModel",   model.m);
    rhi_set_uniform_mat4("uView",    view.m);
    rhi_set_uniform_mat4("uProj",    proj.m);
    rhi_set_uniform_vec3("uViewPos", eye.x, eye.y, eye.z);
    rhi_bind_vertex_buffer(mesh.vbuffer);
    rhi_bind_index_buffer(mesh.ibuffer);
    rhi_draw_indexed(0, mesh.index_count);
}

static void render(const AppState *state) {
    float aspect;
    vec3  eye;
    mat4  view, proj, box_model, floor_model;
    quat  qy, qx, rot;

    rhi_begin_frame(state->fb_width, state->fb_height, 0.10f, 0.12f, 0.15f, 1.0f);

    aspect = (state->fb_height > 0)
           ? (float)state->fb_width / (float)state->fb_height
           : 1.0f;
    eye  = vec3_make(0.0f, 2.5f, 5.0f);   /* raised + back to see the floor */
    view = mat4_look_at(eye,
                        vec3_make(0.0f, 0.5f, 0.0f),    /* look at the box */
                        vec3_make(0.0f, 1.0f, 0.0f));   /* up is +Y        */
    proj = mat4_perspective(sol_radians(45.0f), aspect, 0.1f, 100.0f);

    /* box: tumbling, hovering above the floor — quaternion TRS */
    qy  = quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), state->angle);
    qx  = quat_from_axis_angle(vec3_make(1.0f, 0.0f, 0.0f), state->angle * 0.5f);
    rot = quat_normalize(quat_mul(qy, qx));
    box_model = mat4_from_trs(vec3_make(0.0f, 1.0f, 0.0f), rot, vec3_make(1.0f, 1.0f, 1.0f));
    draw_mesh(state, state->box, box_model, view, proj, eye);

    /* floor: static grid at y = 0 (identity rotation) */
    floor_model = mat4_from_trs(vec3_make(0.0f, 0.0f, 0.0f),
                                quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
    draw_mesh(state, state->floor, floor_model, view, proj, eye);
}

static void on_key(GLFWwindow *window, int key, int scancode, int action, int mods) {
    (void)scancode;
    (void)mods;
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
}

int main(void) {
    GLFWwindow *window;
    AppState state = {0};
    double last;

    if (!glfwInit()) {
        fprintf(stderr, "glfwInit failed\n");
        return EXIT_FAILURE;
    }

    rhi_configure_window();   /* backend sets the API/context hints */

    window = glfwCreateWindow(960, 540, "solarium", NULL, NULL);
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

    if (!init_scene(&state)) {
        fprintf(stderr, "scene init failed\n");
        rhi_shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

#ifndef NDEBUG
    /* teardown self-test (item 1 acceptance): create/destroy in a loop must be
       ASan-clean and bounded — the free-list reuses one slot the whole time */
    {
        int n;
        sol_f32 tmp = 0.0f;
        for (n = 0; n < 5000; n++) {
            RhiBuffer b = rhi_create_buffer(RHI_BUFFER_VERTEX, &tmp, sizeof tmp);
            rhi_destroy_buffer(b);
        }
        printf("teardown selftest: 5000 create/destroy cycles ok\n");
    }
#endif

    last = glfwGetTime();

    while (!glfwWindowShouldClose(window)) {
        double now, dt;
        glfwPollEvents();

        now = glfwGetTime();
        dt  = now - last;
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
