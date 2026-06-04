/* camera.c — see camera.h. Pure math + state; no GLFW, no GL. */

#include "camera.h"
#include "sol_math.h"

#include <math.h>   /* cosf, sinf, sqrtf, asinf, atan2f */

#define CAMERA_ZOOM_SPEED 0.5f   /* units per scroll notch */
#define CAMERA_MIN_DIST   1.0f
#define CAMERA_MAX_DIST   50.0f

static const vec3 WORLD_UP = {0.0f, 1.0f, 0.0f};

void camera_init(Camera *c, vec3 pos, float yaw, float pitch) {
    c->pos        = pos;
    c->yaw        = yaw;
    c->pitch      = pitch;
    c->fov        = sol_radians(45.0f);
    c->mode       = CAMERA_WALK;
    c->move_speed = 3.0f;        /* units/sec */
    c->target     = vec3_make(0.0f, 0.0f, 0.0f);
    c->distance   = 5.0f;
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

mat4 camera_view(const Camera *c) {
    vec3 target = vec3_add(c->pos, camera_forward(c));
    return mat4_look_at(c->pos, target, WORLD_UP);
}

mat4 camera_proj(const Camera *c, float aspect) {
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
    float th    = tanf(c->fov * 0.5f);
    Ray   r;
    r.origin = c->pos;
    r.dir = vec3_normalize(vec3_add(vec3_add(fwd,
                vec3_scale(right, ndc_x * th * aspect)),
                vec3_scale(up,    ndc_y * th)));
    return r;
}
