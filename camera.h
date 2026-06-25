/* camera.h — a movable camera with first-person walk/fly. Platform-free: it
   consumes a CameraInput struct (filled by main.c from GLFW) and produces view/
   projection matrices, so it never touches windowing or GL and stays
   headless-testable. The view matrix is the inverse of the camera's placement;
   we build it with sol_math's look_at. */
#ifndef CAMERA_H
#define CAMERA_H

#include "sol_base.h"
#include "sol_types.h"

/* Standing eye height above the floor. Walk mode settles toward it while you
   move (doorway openings default to 2.2, so this clears them comfortably);
   main.c also spawns the camera here. */
#define CAMERA_EYE_HEIGHT 1.65f

typedef enum {
    CAMERA_WALK = 0,   /* first-person: movement locked to the ground plane;
                          height settles to CAMERA_EYE_HEIGHT while moving */
    CAMERA_FLY,        /* first-person: full-3D movement + vertical */
    CAMERA_ORBIT,      /* orbit a target: drag to rotate, scroll to dolly */
    CAMERA_RTS         /* top-down editor: angled orthographic, pan + zoom,
                          fixed orientation (the spatial-tree editor view) */
} CameraMode;

typedef struct {
    vec3       pos;
    float      yaw;          /* radians; rotation about world up   */
    float      pitch;        /* radians; clamped to +/-89 degrees  */
    float      fov;          /* radians, vertical                  */
    CameraMode mode;
    float      move_speed;   /* units per second                   */
    vec3       target;       /* orbit pivot                        */
    float      distance;     /* orbit radius                       */
    float      ground_y;     /* the floor under the camera (item 10: terrain
                                feeds this per frame); walk settles toward
                                ground_y + CAMERA_EYE_HEIGHT */
    float      ortho_h;      /* CAMERA_RTS: half-height of the ortho view box,
                                world units; zoom changes this */
} Camera;

/* Per-frame input, filled by the platform layer (main.c polls GLFW). look_dx/dy
   are the angular delta to apply THIS frame, in radians — the caller bakes in
   dt*look_speed (keyboard now) or pixels*sensitivity (mouse, in 3c). */
typedef struct {
    sol_bool forward, back, left, right;   /* WASD                      */
    sol_bool up, down;                     /* fly vertical (Space/Ctrl) */
    float    look_dx, look_dy;             /* yaw/pitch delta, radians  */
    float    zoom;                         /* scroll delta this frame (orbit dolly) */
} CameraInput;

void camera_init(Camera *c, vec3 pos, float yaw, float pitch);
void camera_update(Camera *c, const CameraInput *in, float dt);
vec3 camera_forward(const Camera *c);            /* full-3D unit look direction */
mat4 camera_view(const Camera *c);
mat4 camera_proj(const Camera *c, float aspect);
Ray  camera_ray(const Camera *c, float ndc_x, float ndc_y, float aspect);  /* picking ray through a screen point */

/* mode transitions (kept continuous so the view never jumps) */
void camera_enter_orbit(Camera *c, vec3 target);   /* aim at target, remember radius */
void camera_enter_fp(Camera *c);                    /* back to first-person walk */
void camera_focus(Camera *c, vec3 target, vec3 normal, float half_height);  /* framed head-on view */
void camera_enter_rts(Camera *c, vec3 center, float radius);  /* angled top-down, framing a disc of `radius` about `center` */

/* A camera pose without mutating live state — used by callers that want to
   animate toward a framed view rather than snap to it. */
typedef struct { vec3 pos; float yaw, pitch; } CameraPose;

/* Frame a flat, upright surface head-on, fitting BOTH its half-width and
   half-height into the FOV (whichever needs the greater standoff wins), scaled
   by `margin`. `normal` points toward the viewer (the surface's front face);
   must be non-zero. `fov` is the vertical FOV in radians; `aspect` = width/height. */
CameraPose camera_frame_pose(vec3 center, vec3 normal,
                             float half_w, float half_h,
                             float fov, float aspect, float margin);

#endif /* CAMERA_H */
