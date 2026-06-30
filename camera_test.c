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

    /* mat4_inverse (P8 item 2): inverse(proj*view) * (proj*view) ~ identity.
       The post-pass fog reconstructs world position with exactly this inverse. */
    {
        mat4 vp  = mat4_mul(camera_proj(&c, 1.777f), camera_view(&c));
        mat4 inv = mat4_inverse(vp);
        mat4 id  = mat4_mul(inv, vp);
        mat4 I   = mat4_identity();
        int  k;
        for (k = 0; k < 16; k++) {
            if (!approx(id.m[k], I.m[k])) {
                printf("FAIL: mat4_inverse roundtrip not identity at [%d]\n", k);
                return 1;
            }
        }
        printf("mat4_inverse roundtrip: ok\n");
    }

    /* mat4_ortho (P8 item 6): the box corners map onto the NDC cube. The sun's
       cascaded shadow maps project casters with exactly this. View-space near
       plane sits at z = -near, far at z = -far. */
    {
        mat4  o = mat4_ortho(-3.0f, 5.0f, -2.0f, 6.0f, 1.0f, 21.0f);
        /* (x,y,z,1) -> column-major M*v; check three known mappings. */
        struct { float x, y, z, ex, ey, ez; } cases[3];
        int  ci;
        cases[0].x=-3.0f; cases[0].y=-2.0f; cases[0].z= -1.0f;  /* l,b,-near */
        cases[0].ex=-1.0f; cases[0].ey=-1.0f; cases[0].ez=-1.0f;
        cases[1].x= 5.0f; cases[1].y= 6.0f; cases[1].z=-21.0f;  /* r,t,-far  */
        cases[1].ex= 1.0f; cases[1].ey= 1.0f; cases[1].ez= 1.0f;
        cases[2].x= 1.0f; cases[2].y= 2.0f; cases[2].z=-11.0f;  /* center    */
        cases[2].ex= 0.0f; cases[2].ey= 0.0f; cases[2].ez= 0.0f;
        for (ci = 0; ci < 3; ci++) {
            float x = cases[ci].x, y = cases[ci].y, z = cases[ci].z;
            float nx = o.m[0]*x + o.m[4]*y + o.m[8] *z + o.m[12];
            float ny = o.m[1]*x + o.m[5]*y + o.m[9] *z + o.m[13];
            float nz = o.m[2]*x + o.m[6]*y + o.m[10]*z + o.m[14];
            float nw = o.m[3]*x + o.m[7]*y + o.m[11]*z + o.m[15];
            if (!approx(nw, 1.0f) || !approx(nx, cases[ci].ex) ||
                !approx(ny, cases[ci].ey) || !approx(nz, cases[ci].ez)) {
                printf("FAIL: mat4_ortho corner %d -> (%f,%f,%f,%f)\n",
                       ci, (double)nx, (double)ny, (double)nz, (double)nw);
                return 1;
            }
        }
        printf("mat4_ortho NDC corners: ok\n");
    }

    /* CAMERA_RTS enter: frames the disc — mode set, ortho_h = radius, eye sits
       BACK along -forward (so the disc is in front of the camera). */
    camera_enter_rts(&c, vec3_make(0.0f, 12.0f, 0.0f), 30.0f);
    printf("rts enter: mode=%d ortho_h=%.2f pos=(%.2f,%.2f,%.2f)\n",
           (int)c.mode, c.ortho_h, c.pos.x, c.pos.y, c.pos.z);
    if (c.mode != CAMERA_RTS) { printf("FAIL: enter_rts did not set mode\n"); return 1; }
    if (!approx(c.ortho_h, 30.0f)) { printf("FAIL: enter_rts ortho_h\n"); return 1; }
    {
        vec3 f = camera_forward(&c);
        /* the framed center should be ~BACK*forward ahead of the eye */
        vec3 ahead = vec3_add(c.pos, vec3_scale(f, 80.0f));
        if (!approx(ahead.x, 0.0f) || !approx(ahead.y, 12.0f) || !approx(ahead.z, 0.0f)) {
            printf("FAIL: enter_rts did not frame the center\n"); return 1;
        }
    }

    /* RTS pan: W slides the eye in the ground plane, height unchanged. */
    clear_input(&in);
    in.forward = SOL_TRUE;
    {
        vec3 before = c.pos;
        camera_update(&c, &in, 1.0f);
        printf("rts pan -> pos=(%.2f,%.2f,%.2f)\n", c.pos.x, c.pos.y, c.pos.z);
        if (!approx(c.pos.y, before.y)) { printf("FAIL: rts pan changed height\n"); return 1; }
        if (approx(c.pos.x, before.x) && approx(c.pos.z, before.z)) {
            printf("FAIL: rts pan did not move\n"); return 1;
        }
        if (!(c.pos.z < before.z)) { printf("FAIL: rts pan W should go -Z\n"); return 1; }
    }

    /* RTS zoom: scroll up shrinks the ortho extent (zoom in). */
    clear_input(&in);
    in.zoom = 1.0f;
    {
        float before = c.ortho_h;
        camera_update(&c, &in, 1.0f);
        printf("rts zoom -> ortho_h=%.3f\n", c.ortho_h);
        if (!(c.ortho_h < before)) { printf("FAIL: rts zoom did not zoom in\n"); return 1; }
    }

    /* RTS zoom clamps: many scroll-in notches floor at MIN_H, many scroll-out
       notches cap at MAX_H. */
    {
        int k;
        clear_input(&in);
        in.zoom = 5.0f;                       /* scroll in hard */
        for (k = 0; k < 200; k++) camera_update(&c, &in, 1.0f);
        printf("rts zoom floor -> ortho_h=%.3f\n", c.ortho_h);
        if (!approx(c.ortho_h, 5.0f)) { printf("FAIL: rts zoom did not floor at MIN_H\n"); return 1; }
        clear_input(&in);
        in.zoom = -5.0f;                      /* scroll out hard */
        for (k = 0; k < 200; k++) camera_update(&c, &in, 1.0f);
        printf("rts zoom cap -> ortho_h=%.3f\n", c.ortho_h);
        if (!approx(c.ortho_h, 200.0f)) { printf("FAIL: rts zoom did not cap at MAX_H\n"); return 1; }
    }

    /* RTS ray is PARALLEL: two different NDC points share a direction but have
       different origins (perspective rays would share an origin instead). */
    {
        Ray ra = camera_ray(&c, -0.5f, 0.0f, 1.777f);
        Ray rb = camera_ray(&c,  0.5f, 0.0f, 1.777f);
        printf("rts rays: dirA=(%.3f,%.3f,%.3f) origA.x=%.3f origB.x=%.3f\n",
               ra.dir.x, ra.dir.y, ra.dir.z, ra.origin.x, rb.origin.x);
        if (!approx(ra.dir.x, rb.dir.x) || !approx(ra.dir.y, rb.dir.y) ||
            !approx(ra.dir.z, rb.dir.z)) {
            printf("FAIL: rts rays should be parallel\n"); return 1;
        }
        if (approx(ra.origin.x, rb.origin.x) && approx(ra.origin.z, rb.origin.z)) {
            printf("FAIL: rts ray origins should differ across the view rect\n"); return 1;
        }
    }

    /* camera_frame_pose: a board facing +Z parks the eye on the +Z axis, looking
       back (-Z), pitch 0, yaw -90deg. A TALL board is framed by its height; a
       WIDE (landscape) board is framed by its width (greater standoff wins). */
    {
        CameraPose p;
        float fov = sol_radians(45.0f), aspect = 16.0f / 9.0f, margin = 1.1f;
        float tanv = tanf(fov * 0.5f);
        float dist_tall, dist_wide;

        /* tall: half_w=0.5, half_h=1.0 -> height controls */
        p = camera_frame_pose(vec3_make(0.0f, 0.0f, 0.0f), vec3_make(0.0f, 0.0f, 1.0f),
                              0.5f, 1.0f, fov, aspect, margin);
        dist_tall = (1.0f / tanv) * margin;          /* height-limited standoff */
        printf("frame tall -> pos=(%.3f,%.3f,%.3f) yaw=%.3f pitch=%.3f\n",
               p.pos.x, p.pos.y, p.pos.z, p.yaw, p.pitch);
        if (!approx(p.pos.x, 0.0f) || !approx(p.pos.y, 0.0f) ||
            fabsf(p.pos.z - dist_tall) > 0.01f) {
            printf("FAIL: tall board framing\n"); return 1;
        }
        if (!approx(p.pitch, 0.0f) || !approx(p.yaw, sol_radians(-90.0f))) {
            printf("FAIL: tall board pose orientation\n"); return 1;
        }

        /* wide: half_w=2.0, half_h=0.3 -> width controls (farther back than tall) */
        p = camera_frame_pose(vec3_make(0.0f, 0.0f, 0.0f), vec3_make(0.0f, 0.0f, 1.0f),
                              2.0f, 0.3f, fov, aspect, margin);
        dist_wide = (2.0f / (tanv * aspect)) * margin;   /* width-limited standoff */
        printf("frame wide -> pos.z=%.3f (expect %.3f)\n", p.pos.z, dist_wide);
        if (fabsf(p.pos.z - dist_wide) > 0.01f) {
            printf("FAIL: wide board framing (width should control)\n"); return 1;
        }
    }

    /* camera_frame_pose_up: for an upright (+Z-facing) surface it must match
       camera_frame_pose exactly; for a flat (face-up/face-down) surface it
       frames top-down WITHOUT the WORLD_UP look_at degenerating — pitch stays
       strictly inside +/-90 deg and yaw follows the surface's up edge so its
       top lands toward screen-top. */
    {
        CameraPose pu, pf;
        float fov = sol_radians(45.0f), aspect = 16.0f / 9.0f, margin = 1.1f;

        /* upright +Z surface, up=+Y: identical to camera_frame_pose */
        pf = camera_frame_pose   (vec3_make(0.0f, 0.0f, 0.0f), vec3_make(0.0f, 0.0f, 1.0f),
                                  0.5f, 1.0f, fov, aspect, margin);
        pu = camera_frame_pose_up(vec3_make(0.0f, 0.0f, 0.0f), vec3_make(0.0f, 0.0f, 1.0f),
                                  vec3_make(0.0f, 1.0f, 0.0f), 0.5f, 1.0f, fov, aspect, margin);
        printf("frame_up upright -> pos=(%.3f,%.3f,%.3f) yaw=%.3f pitch=%.3f\n",
               pu.pos.x, pu.pos.y, pu.pos.z, pu.yaw, pu.pitch);
        if (!approx(pu.pos.x, pf.pos.x) || !approx(pu.pos.y, pf.pos.y) ||
            !approx(pu.pos.z, pf.pos.z) ||
            !approx(pu.yaw, pf.yaw) || !approx(pu.pitch, pf.pitch)) {
            printf("FAIL: frame_up should match frame_pose for an upright surface\n");
            return 1;
        }

        /* flat FACE-UP surface (normal=+Y), up edge toward +X: eye sits ABOVE,
           pitch ~ -89 (looks down), yaw follows up edge (atan2(0,1)=0). */
        pu = camera_frame_pose_up(vec3_make(0.0f, 0.0f, 0.0f), vec3_make(0.0f, 1.0f, 0.0f),
                                  vec3_make(1.0f, 0.0f, 0.0f), 0.5f, 1.0f, fov, aspect, margin);
        printf("frame_up flat-up -> pos=(%.3f,%.3f,%.3f) yaw=%.3f pitch=%.3f\n",
               pu.pos.x, pu.pos.y, pu.pos.z, pu.yaw, pu.pitch);
        if (!(pu.pos.y > 0.0f)) { printf("FAIL: face-up map eye should sit ABOVE\n"); return 1; }
        if (!(fabsf(pu.pitch) < sol_radians(90.0f))) {
            printf("FAIL: flat map pitch must stay inside +/-90 (no degeneracy)\n"); return 1;
        }
        if (!approx(pu.pitch, sol_radians(-89.0f))) {
            printf("FAIL: face-up map should look ~straight down\n"); return 1;
        }
        if (!approx(pu.yaw, atan2f(0.0f, 1.0f))) {
            printf("FAIL: flat map yaw should follow the up edge\n"); return 1;
        }

        /* flat FACE-DOWN surface (normal=-Y): eye BELOW, pitch ~ +89 (looks up) */
        pu = camera_frame_pose_up(vec3_make(0.0f, 0.0f, 0.0f), vec3_make(0.0f, -1.0f, 0.0f),
                                  vec3_make(1.0f, 0.0f, 0.0f), 0.5f, 1.0f, fov, aspect, margin);
        printf("frame_up flat-down -> pos.y=%.3f pitch=%.3f\n", pu.pos.y, pu.pitch);
        if (!(pu.pos.y < 0.0f)) { printf("FAIL: face-down map eye should sit BELOW\n"); return 1; }
        if (!approx(pu.pitch, sol_radians(89.0f))) {
            printf("FAIL: face-down map should look ~straight up\n"); return 1;
        }

        /* flat FACE-UP with a NON-trivial up edge (up=+Z): yaw must follow it,
           not default to 0 — guards a regression that hardcodes the flat yaw. */
        pu = camera_frame_pose_up(vec3_make(0.0f, 0.0f, 0.0f), vec3_make(0.0f, 1.0f, 0.0f),
                                  vec3_make(0.0f, 0.0f, 1.0f), 0.5f, 1.0f, fov, aspect, margin);
        printf("frame_up flat-up rotated -> yaw=%.3f (expect %.3f)\n",
               pu.yaw, atan2f(1.0f, 0.0f));
        if (!approx(pu.yaw, atan2f(1.0f, 0.0f))) {
            printf("FAIL: flat map yaw must follow a non-axis-aligned up edge\n"); return 1;
        }
    }

    printf("camera_test: OK\n");
    return 0;
}
