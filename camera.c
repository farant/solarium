/* camera.c — see camera.h. Pure math + state; no GLFW, no GL. */

#include "camera.h"
#include "sol_math.h"

#include <math.h>   /* cosf, sinf, sqrtf, asinf, atan2f, expf */

#define CAMERA_ZOOM_SPEED  0.5f   /* units per scroll notch */
#define CAMERA_MIN_DIST    1.0f
#define CAMERA_MAX_DIST    50.0f
#define CAMERA_SETTLE_RATE 8.0f   /* 1/s; how fast walk height eases to eye level */

#define CAMERA_RTS_PITCH (-50.0f)   /* degrees: looking down at the rooms */
#define CAMERA_RTS_YAW   (-90.0f)   /* degrees: forward toward -Z (north up) */
#define CAMERA_RTS_BACK   80.0f     /* how far back along -forward to sit */
#define CAMERA_RTS_PAN    1.5f      /* pan units/sec = this * ortho_h (even feel at any zoom) */
#define CAMERA_RTS_ZOOM   0.1f      /* fraction of ortho_h per scroll notch */
#define CAMERA_RTS_MIN_H  5.0f
#define CAMERA_RTS_MAX_H  200.0f

static const vec3 WORLD_UP = {0.0f, 1.0f, 0.0f};

void camera_init(Camera *c, vec3 pos, float yaw, float pitch) {
    c->pos        = pos;
    c->yaw        = yaw;
    c->pitch      = pitch;
    c->fov        = sol_radians(45.0f);
    c->mode       = CAMERA_WALK;
    c->move_speed = 4.5f;        /* units/sec; a brisk indoor walk */
    c->target     = vec3_make(0.0f, 0.0f, 0.0f);
    c->distance   = 5.0f;
    c->ground_y   = 0.0f;        /* the world floor until terrain says otherwise */
    c->ortho_h    = 20.0f;       /* a sane default until camera_enter_rts frames the scene */
}

/* yaw/pitch -> a unit forward direction (spherical coordinates) */
vec3 camera_forward(const Camera *c) {
    float cp = cosf(c->pitch);
    vec3  f;
    f.x = cosf(c->yaw) * cp;
    f.y = sinf(c->pitch);
    f.z = sinf(c->yaw) * cp;
    return f;                    /* already unit length */
}

void camera_update(Camera *c, const CameraInput *in, float dt) {
    vec3  fwd, right, move;
    float limit = sol_radians(89.0f);

    if (c->mode == CAMERA_RTS) {          /* top-down editor: pan + zoom, fixed angle */
        vec3 pfwd, pright, pmove;
        c->ortho_h -= in->zoom * CAMERA_RTS_ZOOM * c->ortho_h;   /* scroll up = zoom in */
        if (c->ortho_h < CAMERA_RTS_MIN_H) c->ortho_h = CAMERA_RTS_MIN_H;
        if (c->ortho_h > CAMERA_RTS_MAX_H) c->ortho_h = CAMERA_RTS_MAX_H;
        pfwd   = camera_forward(c); pfwd.y = 0.0f; pfwd = vec3_normalize(pfwd);
        pright = vec3_normalize(vec3_cross(pfwd, WORLD_UP));
        pmove  = vec3_make(0.0f, 0.0f, 0.0f);
        if (in->forward) pmove = vec3_add(pmove, pfwd);
        if (in->back)    pmove = vec3_sub(pmove, pfwd);
        if (in->right)   pmove = vec3_add(pmove, pright);
        if (in->left)    pmove = vec3_sub(pmove, pright);
        if (vec3_dot(pmove, pmove) > 0.0f) {
            pmove  = vec3_normalize(pmove);
            c->pos = vec3_add(c->pos, vec3_scale(pmove, CAMERA_RTS_PAN * c->ortho_h * dt));
        }
        return;
    }

    /* look applies in every mode (FP mouse/keys, or orbit drag); clamp pitch so
       look_at never degenerates */
    c->yaw   += in->look_dx;
    c->pitch += in->look_dy;
    if (c->pitch >  limit) c->pitch =  limit;
    if (c->pitch < -limit) c->pitch = -limit;

    if (c->mode == CAMERA_ORBIT) {          /* sit on a sphere around the target */
        c->distance -= in->zoom * CAMERA_ZOOM_SPEED;   /* scroll up -> closer */
        if (c->distance < CAMERA_MIN_DIST) c->distance = CAMERA_MIN_DIST;
        if (c->distance > CAMERA_MAX_DIST) c->distance = CAMERA_MAX_DIST;
        c->pos = vec3_sub(c->target, vec3_scale(camera_forward(c), c->distance));
        return;                             /* no WASD movement in orbit */
    }

    /* first-person: movement basis depends on walk vs fly */
    fwd = camera_forward(c);
    if (c->mode == CAMERA_WALK) {           /* lock to the ground plane */
        fwd.y = 0.0f;
        fwd   = vec3_normalize(fwd);
    }
    right = vec3_normalize(vec3_cross(fwd, WORLD_UP));

    move = vec3_make(0.0f, 0.0f, 0.0f);
    if (in->forward) move = vec3_add(move, fwd);
    if (in->back)    move = vec3_sub(move, fwd);
    if (in->right)   move = vec3_add(move, right);
    if (in->left)    move = vec3_sub(move, right);
    if (c->mode == CAMERA_FLY) {            /* vertical only when flying */
        if (in->up)   move = vec3_add(move, WORLD_UP);
        if (in->down) move = vec3_sub(move, WORLD_UP);
    }

    if (vec3_dot(move, move) > 0.0f) {      /* normalize so diagonals aren't faster */
        move   = vec3_normalize(move);
        c->pos = vec3_add(c->pos, vec3_scale(move, c->move_speed * dt));
        if (c->mode == CAMERA_WALK) {
            /* settle toward standing eye height ABOVE THE GROUND (terrain
               feeds ground_y per frame; item 10) — but only while moving,
               so a camera_focus framing (which parks the eye at the
               surface's height and leaves us in walk mode) holds until you
               step away. The exponential form makes the ease framerate-
               independent — and it is also the climb up a hillside and the
               feather-fall off an island rim: the same glide, new ground. */
            float k = 1.0f - expf(-CAMERA_SETTLE_RATE * dt);
            c->pos.y += (c->ground_y + CAMERA_EYE_HEIGHT - c->pos.y) * k;
        }
    }
}

/* Enter orbit aiming at `target`: keep the eye where it is (no jump) by setting
   distance = |target - pos| and deriving yaw/pitch from the look-at direction. */
void camera_enter_orbit(Camera *c, vec3 target) {
    vec3  d   = vec3_sub(target, c->pos);
    float len = sqrtf(vec3_dot(d, d));
    c->mode     = CAMERA_ORBIT;
    c->target   = target;
    c->distance = (len > 0.0001f) ? len : 5.0f;
    if (len > 0.0001f) {
        vec3 dir = vec3_scale(d, 1.0f / len);
        c->pitch = asinf(dir.y);
        c->yaw   = atan2f(dir.z, dir.x);
    }
}

void camera_enter_fp(Camera *c) {
    c->mode = CAMERA_WALK;   /* keep pos/yaw/pitch -> looks the same, just controls change */
}

/* Enter the top-down editor view: a fixed angled-orthographic vantage that
   frames a disc of `radius` about `center`. Sits BACK along -forward so the
   whole disc is in front, within the ortho far plane. */
void camera_enter_rts(Camera *c, vec3 center, float radius) {
    c->mode    = CAMERA_RTS;
    c->yaw     = sol_radians(CAMERA_RTS_YAW);
    c->pitch   = sol_radians(CAMERA_RTS_PITCH);
    c->ortho_h = radius;
    if (c->ortho_h < CAMERA_RTS_MIN_H) c->ortho_h = CAMERA_RTS_MIN_H;
    if (c->ortho_h > CAMERA_RTS_MAX_H) c->ortho_h = CAMERA_RTS_MAX_H;
    c->pos     = vec3_sub(center, vec3_scale(camera_forward(c), CAMERA_RTS_BACK));
}

/* Frame a surface head-on: sit on its normal at a distance that fills the view,
   looking back at its center. Leaves the camera in first-person walk so you can
   step away afterward. */
void camera_focus(Camera *c, vec3 target, vec3 normal, float half_height) {
    float dist = (half_height / tanf(c->fov * 0.5f)) * 1.3f;   /* frame + margin */
    vec3  eye  = vec3_add(target, vec3_scale(vec3_normalize(normal), dist));
    vec3  d    = vec3_sub(target, eye);
    float len  = sqrtf(vec3_dot(d, d));
    c->mode = CAMERA_WALK;
    c->pos  = eye;
    if (len > 0.0001f) {
        vec3 dir = vec3_scale(d, 1.0f / len);
        c->pitch = asinf(dir.y);
        c->yaw   = atan2f(dir.z, dir.x);
    }
}

CameraPose camera_frame_pose(vec3 center, vec3 normal,
                             float half_w, float half_h,
                             float fov, float aspect, float margin) {
    CameraPose p;
    float tanv   = tanf(fov * 0.5f);
    float dist_h = half_h / tanv;
    float dist_w = half_w / (tanv * aspect);
    float dist   = (dist_h > dist_w ? dist_h : dist_w) * margin;
    vec3  n      = vec3_normalize(normal);
    vec3  dir;
    p.pos = vec3_add(center, vec3_scale(n, dist));
    dir   = vec3_scale(n, -1.0f);                /* camera looks back at center */
    p.pitch = asinf(dir.y);
    p.yaw   = atan2f(dir.z, dir.x);
    return p;
}

CameraPose camera_frame_pose_up(vec3 center, vec3 normal, vec3 up_axis,
                                float half_w, float half_h,
                                float fov, float aspect, float margin) {
    CameraPose p;
    float tanv   = tanf(fov * 0.5f);
    float dist_h = half_h / tanv;
    float dist_w = half_w / (tanv * aspect);
    float dist   = (dist_h > dist_w ? dist_h : dist_w) * margin;
    vec3  n      = vec3_normalize(normal);
    vec3  dir    = vec3_scale(n, -1.0f);          /* camera looks back at center */
    p.pos = vec3_add(center, vec3_scale(n, dist));
    if (fabsf(dir.y) < 0.999f) {                  /* normal ~horizontal: upright surface */
        p.pitch = asinf(dir.y);
        p.yaw   = atan2f(dir.z, dir.x);
    } else {                                       /* normal ~vertical: flat surface, top-down */
        float s = (dir.y < 0.0f) ? -1.0f : 1.0f;   /* down for face-up, up for face-down */
        p.pitch = s * sol_radians(89.0f);          /* clamp shy of straight down/up */
        p.yaw   = atan2f(up_axis.z, up_axis.x);    /* surface's up edge -> screen-top */
    }
    return p;
}

mat4 camera_view(const Camera *c) {
    vec3 target = vec3_add(c->pos, camera_forward(c));
    return mat4_look_at(c->pos, target, WORLD_UP);
}

mat4 camera_proj(const Camera *c, float aspect) {
    if (c->mode == CAMERA_RTS) {
        float hh = c->ortho_h, hw = c->ortho_h * aspect;
        return mat4_ortho(-hw, hw, -hh, hh, 0.1f, 1000.0f);
    }
    return mat4_perspective(c->fov, aspect, 0.1f, 100.0f);
}

/* Picking ray for a screen point in NDC ([-1,1], y up). Analytic from the
   frustum: shoot from the eye through that spot on the near plane. Exact for our
   symmetric perspective camera (the Ray interface isolates this if we ever need
   the general inverse-projection unproject). ndc (0,0) -> straight ahead. */
Ray camera_ray(const Camera *c, float ndc_x, float ndc_y, float aspect) {
    vec3  fwd   = camera_forward(c);
    vec3  right = vec3_normalize(vec3_cross(fwd, WORLD_UP));
    vec3  up    = vec3_cross(right, fwd);
    Ray   r;
    if (c->mode == CAMERA_RTS) {          /* parallel projection: dir constant, origin slides */
        float hh = c->ortho_h, hw = c->ortho_h * aspect;
        r.origin = vec3_add(c->pos,
                     vec3_add(vec3_scale(right, ndc_x * hw),
                              vec3_scale(up,    ndc_y * hh)));
        r.dir = fwd;                      /* already unit length */
        return r;
    }
    {
        float th = tanf(c->fov * 0.5f);
        r.origin = c->pos;
        r.dir = vec3_normalize(vec3_add(vec3_add(fwd,
                    vec3_scale(right, ndc_x * th * aspect)),
                    vec3_scale(up,    ndc_y * th)));
    }
    return r;
}
