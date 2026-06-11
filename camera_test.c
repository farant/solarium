/* camera_test.c — headless checks for the camera math: the forward vector for a
   known yaw/pitch, that walk-movement stays on the ground while fly-movement
   gains height, and that pitch clamps. Links camera.c + sol_math.c only — no
   GLFW/GL. Built by `build.sh camtest`. */

#include "camera.h"
#include "sol_math.h"

#include <stdio.h>
#include <math.h>

static int approx(float a, float b) { return fabsf(a - b) < 0.001f; }

static void clear_input(CameraInput *in) {
    in->forward = in->back = in->left = in->right = SOL_FALSE;
    in->up = in->down = SOL_FALSE;
    in->look_dx = in->look_dy = 0.0f;
    in->zoom = 0.0f;
}

int main(void) {
    Camera      c;
    CameraInput in;

    /* forward vector: yaw=-90deg, pitch=0 should look straight down -Z */
    camera_init(&c, vec3_make(0.0f, 0.0f, 0.0f), sol_radians(-90.0f), 0.0f);
    {
        vec3 f = camera_forward(&c);
        printf("forward(yaw=-90,pitch=0) = (%.3f, %.3f, %.3f)\n", f.x, f.y, f.z);
        if (!approx(f.x, 0.0f) || !approx(f.y, 0.0f) || !approx(f.z, -1.0f)) {
            printf("FAIL: forward vector wrong\n");
            return 1;
        }
    }

    /* WALK: moving forward travels in the ground plane and the height settles
       toward standing eye level (dt=1 closes essentially the whole gap) */
    clear_input(&in);
    in.forward = SOL_TRUE;
    c.pos = vec3_make(0.0f, 2.5f, 0.0f);
    camera_update(&c, &in, 1.0f);
    printf("walk forward -> pos=(%.3f, %.3f, %.3f)\n", c.pos.x, c.pos.y, c.pos.z);
    if (!approx(c.pos.y, CAMERA_EYE_HEIGHT)) {
        printf("FAIL: walk did not settle to eye height\n"); return 1;
    }
    if (approx(c.pos.z, 0.0f)) { printf("FAIL: walk did not move\n"); return 1; }

    /* WALK idle: no movement input -> height holds, even away from eye level.
       This is the camera_focus contract: a framed view (parked at the surface's
       height, mode=WALK) must not drift until you step away. */
    clear_input(&in);
    in.look_dx = 0.1f;                        /* looking around is not stepping */
    c.pos = vec3_make(0.0f, 0.9f, 0.0f);
    camera_update(&c, &in, 1.0f);
    printf("walk idle -> pos=(%.3f, %.3f, %.3f)\n", c.pos.x, c.pos.y, c.pos.z);
    if (!approx(c.pos.y, 0.9f)) { printf("FAIL: idle walk drifted\n"); return 1; }

    /* WALK on terrain (item 10): the settle targets ground_y + eye height —
       the doorway glide becomes hill-climbing when the ground rises */
    clear_input(&in);
    in.forward = SOL_TRUE;
    c.ground_y = 2.0f;
    c.pos = vec3_make(0.0f, CAMERA_EYE_HEIGHT, 0.0f);
    camera_update(&c, &in, 1.0f);
    printf("walk on ground_y=2 -> y=%.3f\n", c.pos.y);
    if (!approx(c.pos.y, 2.0f + CAMERA_EYE_HEIGHT)) {
        printf("FAIL: walk must settle above the GROUND, not sea level\n");
        return 1;
    }
    c.ground_y = 0.0f;

    /* FLY: looking up (pitch=45) and moving forward gains height */
    clear_input(&in);
    in.forward = SOL_TRUE;
    c.mode  = CAMERA_FLY;
    c.pitch = sol_radians(45.0f);
    c.pos   = vec3_make(0.0f, 1.5f, 0.0f);
    camera_update(&c, &in, 1.0f);
    printf("fly forward (pitch=45) -> pos=(%.3f, %.3f, %.3f)\n", c.pos.x, c.pos.y, c.pos.z);
    if (!(c.pos.y > 1.5f)) { printf("FAIL: fly did not gain height\n"); return 1; }

    /* pitch clamps at +/-89 degrees */
    clear_input(&in);
    in.look_dy = 10.0f;                       /* way past straight up */
    camera_update(&c, &in, 1.0f);
    printf("pitch after huge look_dy = %.2f deg\n", c.pitch * (180.0f / SOL_PI));
    if (c.pitch > sol_radians(89.0f) + 0.001f) { printf("FAIL: pitch not clamped\n"); return 1; }

    /* ORBIT: entering keeps the eye in place; the eye sits at target-forward*dist */
    camera_init(&c, vec3_make(0.0f, 0.0f, 5.0f), sol_radians(-90.0f), 0.0f);
    camera_enter_orbit(&c, vec3_make(0.0f, 0.0f, 0.0f));
    clear_input(&in);
    camera_update(&c, &in, 1.0f);
    printf("orbit eye=(%.3f, %.3f, %.3f) dist=%.3f\n", c.pos.x, c.pos.y, c.pos.z, c.distance);
    if (!approx(c.distance, 5.0f)) { printf("FAIL: orbit distance wrong\n"); return 1; }
    if (!approx(c.pos.z, 5.0f))   { printf("FAIL: entering orbit moved the eye\n"); return 1; }

    /* scrolling in reduces the orbit radius */
    clear_input(&in);
    in.zoom = 2.0f;                           /* scroll up = zoom in */
    camera_update(&c, &in, 1.0f);
    printf("orbit dist after zoom = %.3f\n", c.distance);
    if (!(c.distance < 5.0f)) { printf("FAIL: scroll did not dolly in\n"); return 1; }

    /* camera_ray through screen center (ndc 0,0) points straight ahead = forward */
    camera_init(&c, vec3_make(0.0f, 0.0f, 5.0f), sol_radians(-90.0f), 0.0f);
    {
        Ray  ray = camera_ray(&c, 0.0f, 0.0f, 1.777f);
        vec3 f   = camera_forward(&c);
        printf("center ray dir=(%.3f, %.3f, %.3f)\n", ray.dir.x, ray.dir.y, ray.dir.z);
        if (!approx(ray.dir.x, f.x) || !approx(ray.dir.y, f.y) || !approx(ray.dir.z, f.z)) {
            printf("FAIL: center ray should equal forward\n");
            return 1;
        }
    }

    /* camera_focus: framing a +Z-facing surface puts the eye on the +Z axis,
       looking back at the target (forward ~ -Z) */
    camera_focus(&c, vec3_make(0.0f, 0.0f, 0.0f), vec3_make(0.0f, 0.0f, 1.0f), 0.6f);
    {
        vec3 f = camera_forward(&c);
        printf("focus eye=(%.3f, %.3f, %.3f) fwd=(%.3f, %.3f, %.3f)\n",
               c.pos.x, c.pos.y, c.pos.z, f.x, f.y, f.z);
        if (!(c.pos.z > 0.0f) || !approx(c.pos.x, 0.0f) || !approx(c.pos.y, 0.0f)) {
            printf("FAIL: focus eye should sit on +Z axis\n");
            return 1;
        }
        if (!approx(f.z, -1.0f)) { printf("FAIL: focus should look back along -Z\n"); return 1; }
    }

    /* mat4_project_point: the inverse direction of camera_ray. A point dead
       ahead of the camera projects to NDC (0,0); a point behind it must be
       culled (w<=0), never mirrored onto the screen. */
    camera_init(&c, vec3_make(0.0f, 0.0f, 5.0f), sol_radians(-90.0f), 0.0f);
    {
        mat4 vp = mat4_mul(camera_proj(&c, 1.777f), camera_view(&c));
        vec3 ndc;
        if (!mat4_project_point(vp, vec3_make(0.0f, 0.0f, 0.0f), &ndc)) {
            printf("FAIL: point ahead of camera rejected\n");
            return 1;
        }
        printf("project center: ndc=(%.4f, %.4f, %.4f)\n", ndc.x, ndc.y, ndc.z);
        if (!approx(ndc.x, 0.0f) || !approx(ndc.y, 0.0f)) {
            printf("FAIL: dead-ahead point should project to ndc (0,0)\n");
            return 1;
        }
        if (mat4_project_point(vp, vec3_make(0.0f, 0.0f, 10.0f), &ndc)) {
            printf("FAIL: point behind the camera must be culled (w<=0)\n");
            return 1;
        }
        printf("behind-camera point culled: ok\n");
    }

    printf("camera_test: OK\n");
    return 0;
}
