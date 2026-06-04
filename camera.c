/* camera.c — see camera.h. Pure math + state; no GLFW, no GL. */

#include "camera.h"
#include "sol_math.h"

#include <math.h>   /* cosf, sinf */

static const vec3 WORLD_UP = {0.0f, 1.0f, 0.0f};

void camera_init(Camera *c, vec3 pos, float yaw, float pitch) {
    c->pos        = pos;
    c->yaw        = yaw;
    c->pitch      = pitch;
    c->fov        = sol_radians(45.0f);
    c->mode       = CAMERA_WALK;
    c->move_speed = 3.0f;        /* units/sec */
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

    if (in->toggle_mode)
        c->mode = (c->mode == CAMERA_WALK) ? CAMERA_FLY : CAMERA_WALK;

    /* look: deltas are already radians; clamp pitch so look_at never degenerates */
    c->yaw   += in->look_dx;
    c->pitch += in->look_dy;
    if (c->pitch >  limit) c->pitch =  limit;
    if (c->pitch < -limit) c->pitch = -limit;

    /* movement basis depends on the mode */
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

mat4 camera_view(const Camera *c) {
    vec3 target = vec3_add(c->pos, camera_forward(c));
    return mat4_look_at(c->pos, target, WORLD_UP);
}

mat4 camera_proj(const Camera *c, float aspect) {
    return mat4_perspective(c->fov, aspect, 0.1f, 100.0f);
}
