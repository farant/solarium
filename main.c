#include <stdio.h>
#include <stdlib.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>          /* platform: window, input, time — not GL */

#include "rhi.h"                 /* the graphics seam — no GL above here */
#include "mesh.h"
#include "scene.h"
#include "sol_math.h"
#include "camera.h"

#define LOOK_SPEED 1.5f          /* radians/sec for keyboard look */

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
    Scene       scene;
    sol_u32     box_handle;     /* so update() can animate the box object */
    sol_u32     anchor_handle;  /* the empty the box is parented to */
    float       angle;
    Camera      camera;
    sol_bool    f_was_down;     /* edge-detect the walk/fly toggle key */
} AppState;

/* Poll GLFW into a CameraInput (the platform layer; camera.c stays GLFW-free).
   Movement/look are level-triggered (held keys); the mode toggle is edge-
   triggered so it fires once per press. */
static void read_input(GLFWwindow *w, CameraInput *in, double dt, AppState *st) {
    float    look = (float)dt * LOOK_SPEED;
    sol_bool f_now;

    in->forward = glfwGetKey(w, GLFW_KEY_W) == GLFW_PRESS;
    in->back    = glfwGetKey(w, GLFW_KEY_S) == GLFW_PRESS;
    in->left    = glfwGetKey(w, GLFW_KEY_A) == GLFW_PRESS;
    in->right   = glfwGetKey(w, GLFW_KEY_D) == GLFW_PRESS;
    in->up      = glfwGetKey(w, GLFW_KEY_SPACE)        == GLFW_PRESS;
    in->down    = glfwGetKey(w, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS;

    in->look_dx = 0.0f;
    in->look_dy = 0.0f;
    if (glfwGetKey(w, GLFW_KEY_RIGHT) == GLFW_PRESS) in->look_dx += look;
    if (glfwGetKey(w, GLFW_KEY_LEFT)  == GLFW_PRESS) in->look_dx -= look;
    if (glfwGetKey(w, GLFW_KEY_UP)    == GLFW_PRESS) in->look_dy += look;
    if (glfwGetKey(w, GLFW_KEY_DOWN)  == GLFW_PRESS) in->look_dy -= look;

    f_now = glfwGetKey(w, GLFW_KEY_F) == GLFW_PRESS;
    in->toggle_mode = (f_now && !st->f_was_down);   /* fire once per press */
    st->f_was_down  = f_now;
}

/* One-time setup: build the pipeline + meshes, populate the scene. */
static int init_scene(AppState *state) {
    MeshBuilder mb;
    RhiShader shader;
    RhiPipelineDesc desc;
    Mesh box_mesh, floor_mesh;
    Mesh empty = {0};            /* zero mesh -> an empty (transform-only) */
    sol_u32 anchor, floor;

    shader = rhi_create_shader(VERTEX_SRC, FRAGMENT_SRC);
    if (!shader.id) return 0;

    /* one pipeline, shared by all objects (same 8-float layout) */
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

    /* meshes (shared assets the scene objects reference) */
    mb_init(&mb);
    make_box(&mb, 1.0f, 1.0f, 1.0f);
    box_mesh = mesh_from_builder(&mb);
    mb_free(&mb);

    mb_init(&mb);
    make_grid(&mb, 6.0f, 6.0f, 8);
    floor_mesh = mesh_from_builder(&mb);
    mb_free(&mb);

    /* scene: floor (root), an empty anchor (root), and the box as the
       anchor's child — so spinning the anchor makes the box orbit it */
    scene_init(&state->scene);
    floor = scene_add(&state->scene, 0, floor_mesh,
              vec3_make(0.0f, 0.0f, 0.0f), quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
    anchor = scene_add(&state->scene, 0, empty,
              vec3_make(0.0f, 0.0f, 0.0f), quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
    state->anchor_handle = anchor;
    state->box_handle = scene_add(&state->scene, anchor, box_mesh,
              vec3_make(1.5f, 1.0f, 0.0f), quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));

    /* geometry by reference: the asset name regenerates the mesh on load */
    scene_mesh_ref_set(&state->scene, floor, "grid");
    scene_mesh_ref_set(&state->scene, state->box_handle, "box");

    /* overbuilt slots demo (mostly empty this phase) */
    scene_meta_set(&state->scene, state->box_handle, "title",  "Test Box");
    scene_meta_set(&state->scene, state->box_handle, "author", "Solarium");
    scene_rel_add(&state->scene, state->box_handle, "orbits", state->anchor_handle);
    scene_content_set(&state->scene, state->box_handle, "notes/box.txt");

    /* persist the room once at startup (item 2.5c) — inspect ./scene.stml */
    if (scene_save(&state->scene, "scene.stml"))
        printf("saved scene -> scene.stml\n");

    /* place the camera where the old fixed view sat: back + raised, facing the
       scene (-Z) with a slight downward tilt (item 3b) */
    camera_init(&state->camera, vec3_make(0.0f, 2.5f, 5.0f),
                sol_radians(-90.0f), sol_radians(-20.0f));
    state->f_was_down = SOL_FALSE;

    printf("scene: %u objects (1 empty anchor)\n", (unsigned)state->scene.count);
    printf("box meta: title=\"%s\", author=\"%s\"; %u relations\n",
           scene_meta_get(&state->scene, state->box_handle, "title"),
           scene_meta_get(&state->scene, state->box_handle, "author"),
           (unsigned)scene_get(&state->scene, state->box_handle)->rel_count);
    return 1;
}

static void update(AppState *state, double dt) {
    SceneObject *box, *anchor;
    quat qy, qx;

    state->angle += (float)dt * 0.8f;

    /* spin the anchor -> its child (the box) orbits the origin */
    anchor = scene_get(&state->scene, state->anchor_handle);
    if (anchor) anchor->rot = quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), state->angle);

    /* the box's OWN tumble (local rotation), faster than the orbit */
    box = scene_get(&state->scene, state->box_handle);
    if (box) {
        qy = quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), state->angle * 1.5f);
        qx = quat_from_axis_angle(vec3_make(1.0f, 0.0f, 0.0f), state->angle * 0.75f);
        box->rot = quat_normalize(quat_mul(qy, qx));
    }
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

/* world matrix, by walking up the parent chain: parent.world * ... * local.
   Iterative (no recursion), capped against cycles, NULL-guarded. */
static mat4 object_world_matrix(Scene *scene, const SceneObject *o) {
    mat4    world = mat4_from_trs(o->pos, o->rot, o->scale);
    sol_u32 p     = o->parent;
    int     depth = 0;
    while (p != 0 && depth < 64) {
        SceneObject *par = scene_get(scene, p);
        if (!par) break;                                   /* dangling parent -> stop */
        world = mat4_mul(mat4_from_trs(par->pos, par->rot, par->scale), world);
        p     = par->parent;                               /* climb */
        depth++;                                           /* cycle guard */
    }
    return world;
}

static void render(AppState *state) {
    float   aspect;
    vec3    eye;
    mat4    view, proj;
    sol_u32 i;

    rhi_begin_frame(state->fb_width, state->fb_height, 0.10f, 0.12f, 0.15f, 1.0f);

    aspect = (state->fb_height > 0)
           ? (float)state->fb_width / (float)state->fb_height
           : 1.0f;
    eye  = state->camera.pos;                       /* camera drives the view now */
    view = camera_view(&state->camera);
    proj = camera_proj(&state->camera, aspect);

    /* iterate the scene — each object's WORLD matrix (parent * local) */
    for (i = 0; i < state->scene.count; i++) {
        const SceneObject *o = &state->scene.objects[i];
        mat4 model;
        if (o->mesh.index_count == 0) continue;   /* empty: transform-only, don't draw */
        model = object_world_matrix(&state->scene, o);
        draw_mesh(state, o->mesh, model, view, proj, eye);
    }
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
        double      now, dt;
        CameraInput in;
        glfwPollEvents();

        now = glfwGetTime();
        dt  = now - last;
        last = now;
        if (dt > 0.1) dt = 0.1;   /* clamp: a long stall pauses motion, never lurches */

        glfwGetFramebufferSize(window, &state.fb_width, &state.fb_height);

        read_input(window, &in, dt, &state);          /* poll GLFW -> CameraInput */
        camera_update(&state.camera, &in, (float)dt);
        update(&state, dt);                           /* animate the scene */
        render(&state);

        rhi_present();
    }

    scene_free(&state.scene);
    rhi_shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}
