#include <stdio.h>
#include <stdlib.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>          /* platform: window, input, time — not GL */

#include "rhi.h"                 /* the graphics seam — no GL above here */
#include "mesh.h"
#include "scene.h"
#include "sol_math.h"
#include "camera.h"
#include "image.h"
#include "glb.h"

#define LOOK_SPEED        1.5f     /* radians/sec for keyboard look           */
#define MOUSE_SENSITIVITY 0.0025f  /* radians per pixel; NOT dt-scaled        */

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
    "in vec2 vUV;\n"
    "uniform vec3 uViewPos;\n"
    "uniform float uHighlight;\n"                            /* 0 = normal, 1 = selected */
    "uniform sampler2D uTexture;\n"
    "uniform float uUseTexture;\n"                           /* 0 = base color, 1 = sample uTexture */
    "out vec4 FragColor;\n"
    "void main() {\n"
    "    vec3 N = normalize(vNormal);\n"                       /* renormalize after interp */
    "    vec3 L = normalize(vec3(0.4, 1.0, 0.6));\n"           /* direction TO the light */
    "    vec3 V = normalize(uViewPos - vWorldPos);\n"          /* direction TO the camera */
    "    vec3 H = normalize(L + V);\n"                         /* half-vector */
    "\n"
    "    vec3 lightColor = vec3(1.0, 0.98, 0.92);\n"
    "    vec3 baseColor  = vec3(0.85, 0.45, 0.35);\n"          /* the object's color */
    "    vec3 albedo = (uUseTexture > 0.5)\n"
    "                ? texture(uTexture, vUV).rgb\n"           /* sRGB tex -> linear on sample */
    "                : baseColor;\n"
    "    float ambient   = 0.15;\n"
    "    float shininess = 48.0;\n"
    "\n"
    "    float diff = max(dot(N, L), 0.0);\n"                  /* Lambert diffuse */
    "    float spec = (diff > 0.0) ? pow(max(dot(N, H), 0.0), shininess) : 0.0;\n"
    "    vec3 color = albedo * ambient\n"
    "               + albedo * lightColor * diff\n"
    "               + lightColor * spec;\n"                    /* Blinn-Phong highlight */
    "    color = pow(color, vec3(1.0 / 2.2));\n"              /* linear -> sRGB for display */
    "    color = mix(color, vec3(1.0, 0.85, 0.30), uHighlight * 0.5);\n"  /* gold when selected */
    "    FragColor = vec4(color, 1.0);\n"
    "}\n";

typedef struct {
    int         fb_width, fb_height;
    RhiPipeline pipeline;
    RhiTexture  albedo_tex;     /* decoded page image (item 5b); 0 if load failed */
    Scene       scene;
    sol_u32     box_handle;     /* so update() can animate the box object */
    sol_u32     anchor_handle;  /* the empty the box is parented to */
    sol_u32     page_handle;    /* the readable parchment surface (item 5d) */
    float       angle;
    Camera      camera;
    sol_bool    f_was_down;     /* edge-detect the walk/fly toggle key */
    /* mouse-look / cursor state (item 3c/3d) */
    double      mouse_last_x, mouse_last_y;
    int         mouse_skip;        /* swallow N frames of delta after a cursor-mode change */
    sol_bool    tab_was_down;
    double      scroll_accum;      /* scroll events accumulate here, drained per frame */
    /* picking / selection (item 4) */
    sol_u32     selected_handle;   /* 0 = none */
    sol_bool    lmb_was_down;
    double      press_x, press_y;  /* left-press position, for orbit tap-vs-drag */
} AppState;

/* Union AABB over all of a model's meshes (local space) — for auto-fitting an
   arbitrary-scale imported asset to the room. */
static Aabb union_bounds(const GlbModel *model) {
    Aabb    b;
    sol_u32 m;
    b.min = vec3_make( 1e30f,  1e30f,  1e30f);
    b.max = vec3_make(-1e30f, -1e30f, -1e30f);
    for (m = 0; m < model->count; m++) {
        Aabb e = model->parts[m].mesh.bounds;
        if (e.min.x < b.min.x) b.min.x = e.min.x;
        if (e.min.y < b.min.y) b.min.y = e.min.y;
        if (e.min.z < b.min.z) b.min.z = e.min.z;
        if (e.max.x > b.max.x) b.max.x = e.max.x;
        if (e.max.y > b.max.y) b.max.y = e.max.y;
        if (e.max.z > b.max.z) b.max.z = e.max.z;
    }
    return b;
}

/* Load a glTF .glb, auto-fit it (longest side ~1.2 units), and stand it on the
   floor centered at (x,z). Node transforms are already baked by the loader. */
static void add_glb_to_scene(AppState *state, const char *path, float x, float z) {
    GlbModel model;
    Aabb     b;
    vec3     center, ext, pos;
    float    maxdim, scale;
    sol_u32  m;

    if (!glb_load(path, &model)) { fprintf(stderr, "glb load failed: %s\n", path); return; }

    b      = union_bounds(&model);
    center = vec3_scale(vec3_add(b.min, b.max), 0.5f);
    ext    = vec3_sub(b.max, b.min);
    maxdim = ext.x;
    if (ext.y > maxdim) maxdim = ext.y;
    if (ext.z > maxdim) maxdim = ext.z;
    scale  = (maxdim > 0.0001f) ? (1.2f / maxdim) : 1.0f;
    pos    = vec3_make(x - scale * center.x, -scale * b.min.y, z - scale * center.z);  /* base on floor */

    printf("glb: %s -> %u part(s), autoscale %.4f\n", path, (unsigned)model.count, scale);
    for (m = 0; m < model.count; m++) {
        sol_u32 h = scene_add(&state->scene, 0, model.parts[m].mesh, pos,
                              quat_identity(), vec3_make(scale, scale, scale));
        scene_texture_set(&state->scene, h, model.parts[m].albedo);
    }
    glb_free(&model);
}

/* World-space position of an object (its world matrix's translation column),
   falling back to the scene focus if the handle is gone. */
static vec3 object_world_pos(Scene *s, sol_u32 handle) {
    SceneObject *o = scene_get(s, handle);
    if (o) {
        mat4 w = scene_world_matrix(s, o);
        return vec3_make(w.m[12], w.m[13], w.m[14]);
    }
    return vec3_make(0.0f, 0.5f, 0.0f);
}

/* Cast a pick ray through a screen point (NDC) and select the nearest object,
   reporting its stable handle + nid. In orbit, a hit re-targets the pivot. */
static void do_pick(AppState *st, GLFWwindow *w, float ndc_x, float ndc_y) {
    int     ww, wh;
    float   aspect, t;
    Ray     ray;
    sol_u32 hit;

    glfwGetWindowSize(w, &ww, &wh);                 /* cursor is in window coords */
    aspect = (wh > 0) ? (float)ww / (float)wh : 1.0f;
    ray = camera_ray(&st->camera, ndc_x, ndc_y, aspect);
    hit = scene_pick(&st->scene, ray, &t);
    st->selected_handle = hit;
    if (hit) {
        SceneObject *o = scene_get(&st->scene, hit);
        printf("picked: handle %u, nid %s, t=%.2f\n",
               (unsigned)hit, (o && o->nid) ? o->nid : "?", t);
        if (hit == st->page_handle) {                   /* click the page -> read it */
            camera_focus(&st->camera, object_world_pos(&st->scene, st->page_handle),
                         vec3_make(0.0f, 0.0f, 1.0f), 0.6f);   /* page faces +Z, half-height 0.6 */
            glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_DISABLED);  /* first-person look */
            st->mouse_skip = 2;                         /* no jump across the cursor-mode change */
        } else if (st->camera.mode == CAMERA_ORBIT) {   /* "click a shelf to zoom" */
            camera_enter_orbit(&st->camera, object_world_pos(&st->scene, hit));
        }
    } else {
        printf("picked: nothing\n");
    }
}

/* Scroll arrives only as events (no poll), so it needs a callback; the window
   user-pointer bridges back to our state. read_input drains scroll_accum. */
static void on_scroll(GLFWwindow *w, double xoff, double yoff) {
    AppState *st = (AppState *)glfwGetWindowUserPointer(w);
    (void)xoff;
    if (st) st->scroll_accum += yoff;
}

/* Poll GLFW into a CameraInput (the platform layer; camera.c stays GLFW-free).
   Movement/look are level-triggered (held keys); the mode toggle is edge-
   triggered so it fires once per press. */
static void read_input(GLFWwindow *w, CameraInput *in, double dt, AppState *st) {
    float    look = (float)dt * LOOK_SPEED;
    sol_bool f_now, tab_now, dragging, fp;
    double   mx, my;

    fp = (st->camera.mode != CAMERA_ORBIT);

    /* movement (held; ignored by the camera in orbit) */
    in->forward = glfwGetKey(w, GLFW_KEY_W) == GLFW_PRESS;
    in->back    = glfwGetKey(w, GLFW_KEY_S) == GLFW_PRESS;
    in->left    = glfwGetKey(w, GLFW_KEY_A) == GLFW_PRESS;
    in->right   = glfwGetKey(w, GLFW_KEY_D) == GLFW_PRESS;
    in->up      = glfwGetKey(w, GLFW_KEY_SPACE)        == GLFW_PRESS;
    in->down    = glfwGetKey(w, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS;

    /* keyboard look (held -> a rate -> dt-scaled) */
    in->look_dx = 0.0f;
    in->look_dy = 0.0f;
    if (glfwGetKey(w, GLFW_KEY_RIGHT) == GLFW_PRESS) in->look_dx += look;
    if (glfwGetKey(w, GLFW_KEY_LEFT)  == GLFW_PRESS) in->look_dx -= look;
    if (glfwGetKey(w, GLFW_KEY_UP)    == GLFW_PRESS) in->look_dy += look;
    if (glfwGetKey(w, GLFW_KEY_DOWN)  == GLFW_PRESS) in->look_dy -= look;

    /* Tab toggles first-person <-> orbit (edge); cursor mode follows */
    tab_now = glfwGetKey(w, GLFW_KEY_TAB) == GLFW_PRESS;
    if (tab_now && !st->tab_was_down) {
        if (st->camera.mode == CAMERA_ORBIT) {
            camera_enter_fp(&st->camera);
            glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        } else {
            camera_enter_orbit(&st->camera, object_world_pos(&st->scene, st->selected_handle));  /* pivot on the selection (fallback to scene focus) */
            glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
        st->mouse_skip = 2;                         /* cursor pos is discontinuous across the switch */
        fp = (st->camera.mode != CAMERA_ORBIT);
    }
    st->tab_was_down = tab_now;

    /* mouse look (displacement -> sensitivity-scaled, NOT dt-scaled). Always
       track the last position so a drag never starts with a jump; apply the
       delta in first-person (captured) or while left-dragging in orbit — but
       swallow a couple of frames after a cursor-mode change, since GLFW's
       virtual cursor position jumps (one frame later) when the mode flips. */
    glfwGetCursorPos(w, &mx, &my);
    dragging = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    if (st->mouse_skip > 0) {
        st->mouse_skip--;                           /* reseed only; don't apply the delta */
    } else if (fp || dragging) {
        float dx = (float)(mx - st->mouse_last_x);
        float dy = (float)(my - st->mouse_last_y);
        in->look_dx += dx * MOUSE_SENSITIVITY;
        in->look_dy -= dy * MOUSE_SENSITIVITY;      /* screen-y grows down -> negate */
    }
    st->mouse_last_x = mx;
    st->mouse_last_y = my;

    /* scroll: drain the accumulator into this frame (orbit dolly) */
    in->zoom = (float)st->scroll_accum;
    st->scroll_accum = 0.0;

    /* left button -> pick. FP: a press picks through screen center. Orbit: a
       tap (release with little movement) picks at the cursor; a drag rotates. */
    {
        sol_bool lmb = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        if (fp) {
            if (lmb && !st->lmb_was_down) do_pick(st, w, 0.0f, 0.0f);
        } else {
            if (lmb && !st->lmb_was_down) {
                st->press_x = mx;
                st->press_y = my;
            } else if (!lmb && st->lmb_was_down) {
                double ddx = mx - st->press_x;
                double ddy = my - st->press_y;
                if (ddx*ddx + ddy*ddy < 25.0) {         /* moved < 5px -> a tap */
                    int ww, wh;
                    glfwGetWindowSize(w, &ww, &wh);
                    do_pick(st, w, 2.0f*(float)mx/(float)ww - 1.0f,
                                   1.0f - 2.0f*(float)my/(float)wh);
                }
            }
        }
        st->lmb_was_down = lmb;
    }

    /* F toggles walk/fly in first person (edge) */
    f_now = glfwGetKey(w, GLFW_KEY_F) == GLFW_PRESS;
    if (f_now && !st->f_was_down && st->camera.mode != CAMERA_ORBIT)
        st->camera.mode = (st->camera.mode == CAMERA_WALK) ? CAMERA_FLY : CAMERA_WALK;
    st->f_was_down = f_now;
}

/* Decode a page image with stb (via image.c) and upload it as an sRGB texture.
   Returns a zero handle if the file is missing/undecodable. */
static RhiTexture load_texture(const char *path) {
    Image      img;
    RhiTexture tex;
    tex.id = 0;
    if (image_load(path, &img)) {
        tex = rhi_create_texture(img.pixels, img.w, img.h, RHI_TEX_SRGB8);
        image_free(&img);
    } else {
        fprintf(stderr, "image load failed: %s\n", path);
    }
    return tex;
}

/* One-time setup: build the pipeline + meshes, populate the scene. */
static int init_scene(AppState *state) {
    MeshBuilder mb;
    RhiShader shader;
    RhiPipelineDesc desc;
    Mesh box_mesh, floor_mesh, page_mesh;
    Mesh empty = {0};            /* zero mesh -> an empty (transform-only) */
    sol_u32 anchor, floor, page;

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

    state->albedo_tex = load_texture("paper-picture.png");   /* item 5b: decode via stb */

    /* meshes (shared assets the scene objects reference) */
    mb_init(&mb);
    make_box(&mb, 1.0f, 1.0f, 1.0f);
    box_mesh = mesh_from_builder(&mb);
    mb_free(&mb);

    mb_init(&mb);
    make_grid(&mb, 6.0f, 6.0f, 8);
    floor_mesh = mesh_from_builder(&mb);
    mb_free(&mb);

    /* a page-aspect quad standing in the XY plane, facing +Z, with upright UVs
       (uv (0,0) at bottom-left). Built directly rather than make_plane+rotation,
       which would invert V relative to world-up. ~3:4 to match paper-picture.png */
    mb_init(&mb);
    {
        sol_f32 hw = 0.45f, hh = 0.60f;   /* half of 0.9 x 1.2 */
        sol_u32 a = mb_push_vertex(&mb, -hw, -hh, 0.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f);  /* BL */
        sol_u32 b = mb_push_vertex(&mb,  hw, -hh, 0.0f,  0.0f, 0.0f, 1.0f,  1.0f, 0.0f);  /* BR */
        sol_u32 c = mb_push_vertex(&mb,  hw,  hh, 0.0f,  0.0f, 0.0f, 1.0f,  1.0f, 1.0f);  /* TR */
        sol_u32 d = mb_push_vertex(&mb, -hw,  hh, 0.0f,  0.0f, 0.0f, 1.0f,  0.0f, 1.0f);  /* TL */
        mb_push_triangle(&mb, a, b, c);
        mb_push_triangle(&mb, a, c, d);
    }
    page_mesh = mesh_from_builder(&mb);
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

    /* the parchment reading surface: the page quad, already upright and facing +Z */
    page = scene_add(&state->scene, 0, page_mesh,
              vec3_make(-2.0f, 1.0f, 0.0f), quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
    scene_texture_set(&state->scene, page, state->albedo_tex);
    state->page_handle = page;

    /* item 6: real glTF models standing in the room */
    add_glb_to_scene(state, "book.glb",   2.0f,  0.0f);
    add_glb_to_scene(state, "candle.glb", 0.0f,  2.0f);   /* front-centre, clear of the page */

    /* geometry by reference: the asset name regenerates the mesh on load */
    scene_mesh_ref_set(&state->scene, floor, "grid");
    scene_mesh_ref_set(&state->scene, state->box_handle, "box");
    scene_mesh_ref_set(&state->scene, page, "page");

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
                      mat4 view, mat4 proj, vec3 eye, float highlight,
                      RhiTexture tex, float use_texture) {
    rhi_set_pipeline(state->pipeline);
    rhi_set_uniform_mat4("uModel",      model.m);
    rhi_set_uniform_mat4("uView",       view.m);
    rhi_set_uniform_mat4("uProj",       proj.m);
    rhi_set_uniform_vec3("uViewPos",    eye.x, eye.y, eye.z);
    rhi_set_uniform_float("uHighlight",  highlight);
    rhi_set_uniform_float("uUseTexture", use_texture);
    if (use_texture > 0.5f) {
        rhi_bind_texture(tex, 0);
        rhi_set_uniform_int("uTexture", 0);     /* sampler -> texture unit 0 */
    }
    rhi_bind_vertex_buffer(mesh.vbuffer);
    rhi_bind_index_buffer(mesh.ibuffer);
    rhi_draw_indexed(0, mesh.index_count);
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
        mat4       model;
        float      hl, use;
        RhiTexture tex;
        if (o->mesh.index_count == 0) continue;   /* empty: transform-only, don't draw */
        model = scene_world_matrix(&state->scene, o);
        hl    = (o->handle == state->selected_handle) ? 1.0f : 0.0f;
        tex = o->texture;                      /* per-object material (item 5c) */
        use = tex.id ? 1.0f : 0.0f;
        draw_mesh(state, o->mesh, model, view, proj, eye, hl, tex, use);
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

    glfwSetWindowUserPointer(window, &state);   /* bridge callbacks -> state */
    glfwSetKeyCallback(window, on_key);         /* platform: input */
    glfwSetScrollCallback(window, on_scroll);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);  /* first-person: capture */
    state.mouse_skip = 2;                /* swallow the first deltas (no baseline yet) */

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
