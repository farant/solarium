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
    in->toggle_mode = SOL_FALSE;
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

    /* WALK: moving forward stays on the ground plane (y unchanged) but moves */
    clear_input(&in);
    in.forward = SOL_TRUE;
    c.pos = vec3_make(0.0f, 1.5f, 0.0f);
    camera_update(&c, &in, 1.0f);
    printf("walk forward -> pos=(%.3f, %.3f, %.3f)\n", c.pos.x, c.pos.y, c.pos.z);
    if (!approx(c.pos.y, 1.5f)) { printf("FAIL: walk changed height\n"); return 1; }
    if (approx(c.pos.z, 0.0f)) { printf("FAIL: walk did not move\n"); return 1; }

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

    printf("camera_test: OK\n");
    return 0;
}
