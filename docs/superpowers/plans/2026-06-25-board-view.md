# Board View Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Select a whiteboard and press Enter to glide the camera to frame it head-on with the cursor unlocked, so you can select/drag/resize/edit/create cards on the board with the mouse; Esc glides back.

**Architecture:** A transient `board_view` state flag on `AppState` (no new camera-mode enum). A new pure `camera_frame_pose` helper computes the framed camera pose (fit board width-or-height + margin). Entry/exit intercept Enter/Esc, set up a smooth glide tween that overrides `camera_update` each frame, unlock the cursor (inventory-style), and freeze movement/look until the glide settles. The existing 3D on-board interactions (select/drag/resize/edit/Delete/N) are reused, driven by the free cursor via an extended `pick_ray` and a cursor-NDC selection pick.

**Tech Stack:** C89 (strict: `-Wall -Wextra -Werror -pedantic`), OpenGL + Metal dual backend (this feature reuses `draw_mesh` → **no new shader, no MSL twin**), GLFW input. Spec: `docs/superpowers/specs/2026-06-25-board-view-design.md`.

**Conventions the implementer MUST follow:**
- In `main.c`, no C99 math: use `(float)sin((double)x)`, ternary for abs, etc. In `camera.c`, **match the file's existing style** (`tanf`/`asinf`/`atan2f`/`sqrtf` are already used there by `camera_focus`).
- Commits go on `main`, message body ending with the line:
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`
- Never `git add` `NOTES.stml` or `paper-picture.png`. Stage only the files each task touches.
- After each task, build BOTH backends (`./build.sh` and `./build.sh metal`) — a struct/uniform mismatch passes the C build and breaks at launch, so never skip Metal even though this feature adds no shader.

---

## File Touch List

- `camera.h` — add `CameraPose` struct + `camera_frame_pose` declaration. (Task 1)
- `camera.c` — implement `camera_frame_pose` (pure). (Task 1)
- `camera_test.c` — unit tests for `camera_frame_pose`. (Task 1)
- `main.c` — `AppState` board-view fields + init (Task 2); `board_view_enter`/`board_view_exit` + Enter/Esc interception (Task 3); `board_view_update` glide + frame-loop call (Task 4); cursor unlock + movement/look freeze in `read_input` (Task 5); cursor-driven picking — `pick_ray` extension + FP press cursor-NDC + board-click deselect (Task 6).

---

### Task 1: `camera_frame_pose` — the pure framing helper

**Files:**
- Modify: `camera.h` (add struct + declaration near `camera_focus` at line ~62)
- Modify: `camera.c` (implement near `camera_focus` at line ~154)
- Test: `camera_test.c` (add cases in `main`, before the final `return 0`)

- [ ] **Step 1: Write the failing test**

In `camera_test.c`, find the end of `main()` (the `printf("camera_test: OK\n"); return 0;` area, or the last check before `return 0;`) and insert these checks just before the final success print:

```c
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
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build.sh camtest`
Expected: FAIL — link/compile error, `camera_frame_pose` undefined (and `CameraPose` unknown type).

- [ ] **Step 3: Declare the type + function in `camera.h`**

Immediately after the `camera_focus` declaration (line ~62, the `/* framed head-on view */` line), add:

```c

/* A camera pose without mutating live state — used by callers that want to
   animate toward a framed view rather than snap to it. */
typedef struct { vec3 pos; float yaw, pitch; } CameraPose;

/* Frame a flat, upright surface head-on, fitting BOTH its half-width and
   half-height into the FOV (whichever needs the greater standoff wins), scaled
   by `margin`. `normal` points toward the viewer (the surface's front face).
   `fov` is the vertical FOV in radians; `aspect` = width/height. */
CameraPose camera_frame_pose(vec3 center, vec3 normal,
                             float half_w, float half_h,
                             float fov, float aspect, float margin);
```

- [ ] **Step 4: Implement in `camera.c`**

Immediately after the `camera_focus` function (after its closing `}` at line ~166), add:

```c

CameraPose camera_frame_pose(vec3 center, vec3 normal,
                             float half_w, float half_h,
                             float fov, float aspect, float margin) {
    CameraPose p;
    float tanv   = tanf(fov * 0.5f);
    float dist_h = half_h / tanv;
    float dist_w = half_w / (tanv * aspect);
    float dist   = (dist_h > dist_w ? dist_h : dist_w) * margin;
    vec3  n      = vec3_normalize(normal);
    vec3  dir;                                   /* camera looks back at center */
    p.pos = vec3_add(center, vec3_scale(n, dist));
    dir   = vec3_scale(n, -1.0f);
    p.pitch = asinf(dir.y);
    p.yaw   = atan2f(dir.z, dir.x);
    return p;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `./build.sh camtest && ./camera_test`
Expected: PASS — prints the `frame tall`/`frame wide` lines and ends `camera_test: OK` (exit 0), no `FAIL`.

- [ ] **Step 6: Build both backends (the helper is referenced only by the test so far, but keep the gauntlet green)**

Run: `./build.sh && ./build.sh metal`
Expected: `built ./solarium (debug)` and `built ./solarium-metal ...`.

- [ ] **Step 7: Commit**

```bash
git add camera.h camera.c camera_test.c
git commit -m "Board view 1/6: camera_frame_pose (fit width-or-height + margin)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: `AppState` board-view fields + init + constants

**Files:**
- Modify: `main.c` — `AppState` struct (the `move_board`/`move_grab` region, ~line 2724); `state` init in `main()` (~line 14475); constants near the other gameplay constants.

- [ ] **Step 1: Add the constants**

Near the top of `main.c` with the other `#define`d gameplay constants (anywhere above `read_input`; e.g. just below the `FRAME_*` block around line 4191 is fine, OR immediately above the `AppState` struct), add:

```c
#define BOARD_VIEW_GLIDE_S  0.35f   /* seconds for the enter/exit camera glide */
#define BOARD_VIEW_MARGIN   1.10f   /* fill the FOV to the board + a little air */
```

- [ ] **Step 2: Add the fields to `AppState`**

In the `AppState` struct, right after the `move_grab` field (line ~2725, `vec3 move_grab; /* origin - cursor-hit ... */`), add:

```c
    /* board view: frame a whiteboard head-on, cursor unlocked, to arrange cards.
       Transient UI state (never persisted). bv_t starts at 1.0 (settled). */
    sol_u32  board_view;          /* board being viewed; 0 = not in board view  */
    sol_bool board_view_was;      /* edge-detect for the cursor toggle           */
    vec3     bv_from_pos,  bv_to_pos;
    float    bv_from_yaw,  bv_to_yaw;
    float    bv_from_pitch, bv_to_pitch;
    float    bv_t;                /* 0..1 eased glide progress; >=1 = settled     */
    float    bv_dir;             /* +1 gliding to the board, -1 gliding back out  */
    vec3     bv_return_pos;       /* pose to restore on exit (where you stood)    */
    float    bv_return_yaw, bv_return_pitch;
```

- [ ] **Step 3: Initialize `bv_t = 1.0` so no glide runs at startup**

`AppState state = {0};` zero-inits everything (so `board_view == 0`, good), but `bv_t` must start settled at 1.0, not 0.0. In `main()`, immediately after `AppState state = {0};` (line ~14475), add:

```c
    state.bv_t = 1.0f;   /* board-view glide starts settled (no tween at boot) */
```

- [ ] **Step 4: Build both backends**

Run: `./build.sh && ./build.sh metal`
Expected: both build clean (no behavior change yet; fields are unused — that is fine, they are struct members, not unused locals).

- [ ] **Step 5: Commit**

```bash
git add main.c
git commit -m "Board view 2/6: AppState fields + glide/margin constants

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: `board_view_enter` / `board_view_exit` + Enter/Esc interception

**Files:**
- Modify: `main.c` — add the two helpers (place them just AFTER `room_interior_height` and the `wall_gable_geom` helpers, near line ~8540, so they can see `board_world_corners`, `board_yaw`, `mesh_ref_param`, `editor_room_rect`; all of those are defined earlier in the file). Then edit the Enter and Esc handlers in `on_key` (~line 14455–14470).

Context: `board_world_corners(Scene*, sol_u32 handle, vec3 out[4], vec3 *out_u)` and `board_yaw(Scene*, sol_u32)` already exist (used by `picture_move_pick`). `st->fb_width`/`st->fb_height` are the framebuffer dims (used elsewhere, e.g. line ~9325). `READER_IDLE` is the idle reader state. `CameraPose`/`camera_frame_pose` come from Task 1.

- [ ] **Step 1: Add `board_view_enter` and `board_view_exit`**

Place this block right after the `wall_mount_gable` function (the gable helper added earlier, ends near line ~8540, just before `note_text_size`'s `NOTE_CARD_*` defines) — anywhere after `board_world_corners`/`board_yaw` are defined and before `read_input`/`on_key`:

```c
/* Enter board view: frame the selected whiteboard head-on and begin the glide.
   Returns 0 (and does nothing) if the selection isn't a board, board view is
   already active, or another mode owns the keyboard/cursor. */
static int board_view_enter(AppState *st) {
    SceneObject *o = scene_get(&st->scene, st->selected_handle);
    vec3  cor[4], u, center, normal;
    float yaw, half_w, half_h, aspect;
    CameraPose pose;
    if (st->board_view != 0) return 0;
    if (!o || !o->mesh_ref || strcmp(o->mesh_ref, "board") != 0) return 0;
    if (st->carried != 0 || st->place_active || st->editor.active ||
        st->palette.open || st->inv_open || st->edit_handle != 0 ||
        st->reader_state != READER_IDLE) return 0;
    board_world_corners(&st->scene, st->selected_handle, cor, &u);
    center = vec3_scale(vec3_add(vec3_add(cor[0], cor[1]),
                                 vec3_add(cor[2], cor[3])), 0.25f);
    yaw    = board_yaw(&st->scene, st->selected_handle);
    normal = vec3_make((float)sin((double)yaw), 0.0f, (float)cos((double)yaw));
    half_w = mesh_ref_param("board", o->mesh_params, o->mesh_param_count, "w") * 0.5f;
    half_h = mesh_ref_param("board", o->mesh_params, o->mesh_param_count, "h") * 0.5f;
    aspect = (st->fb_height > 0) ? (float)st->fb_width / (float)st->fb_height : 1.7778f;
    pose   = camera_frame_pose(center, normal, half_w, half_h,
                               st->camera.fov, aspect, BOARD_VIEW_MARGIN);
    st->bv_return_pos   = st->camera.pos;
    st->bv_return_yaw   = st->camera.yaw;
    st->bv_return_pitch = st->camera.pitch;
    st->bv_from_pos = st->camera.pos;   st->bv_to_pos = pose.pos;
    st->bv_from_yaw = st->camera.yaw;   st->bv_to_yaw = pose.yaw;
    st->bv_from_pitch = st->camera.pitch; st->bv_to_pitch = pose.pitch;
    st->bv_t   = 0.0f;
    st->bv_dir = 1.0f;
    st->board_view = st->selected_handle;
    return 1;
}

/* Leave board view: glide the camera back to the stored return pose. Safe to
   call when already out. */
static void board_view_exit(AppState *st) {
    st->bv_from_pos = st->camera.pos;   st->bv_to_pos = st->bv_return_pos;
    st->bv_from_yaw = st->camera.yaw;   st->bv_to_yaw = st->bv_return_yaw;
    st->bv_from_pitch = st->camera.pitch; st->bv_to_pitch = st->bv_return_pitch;
    st->bv_t   = 0.0f;
    st->bv_dir = -1.0f;
    st->board_view = 0;
}
```

- [ ] **Step 2: Intercept Enter in `on_key`**

Find the Enter dispatch at line ~14463:

```c
    if (st && key == GLFW_KEY_ENTER && action == GLFW_PRESS &&
        st->selected_handle != 0) {
        SceneObject *o = scene_get(&st->scene, st->selected_handle);
        if (o && o->kind == KIND_NOTE)
            note_edit_begin(st, st->selected_handle);
        else if (o)
            reader_open(st, st->selected_handle);
    }
```

Replace the body so a selected board enters board view (and never falls through to the book reader):

```c
    if (st && key == GLFW_KEY_ENTER && action == GLFW_PRESS &&
        st->selected_handle != 0) {
        SceneObject *o = scene_get(&st->scene, st->selected_handle);
        if (o && o->kind == KIND_NOTE)
            note_edit_begin(st, st->selected_handle);
        else if (o && o->mesh_ref && strcmp(o->mesh_ref, "board") == 0) {
            if (st->board_view == 0) board_view_enter(st);
            /* already in board view: Enter on the board itself does nothing */
        }
        else if (o)
            reader_open(st, st->selected_handle);
    }
```

- [ ] **Step 3: Intercept Esc in `on_key`**

Find the Esc handler at line ~14455:

```c
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        if (st && st->reader_state != READER_IDLE)
            reader_close(st);                   /* put the book back first */
        else
            glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
```

Replace with (board view exits first):

```c
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        if (st && st->board_view != 0)
            board_view_exit(st);                /* leave board view first */
        else if (st && st->reader_state != READER_IDLE)
            reader_close(st);                   /* put the book back first */
        else
            glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
```

- [ ] **Step 4: Build both backends**

Run: `./build.sh && ./build.sh metal`
Expected: both build clean. (Behavior: pressing Enter on a selected board now sets `board_view` and the glide target, and Esc clears it — but the camera does not move yet, that is Task 4. Movement/cursor are not yet handled, so the view will be odd until Tasks 4–6; that is expected mid-plan.)

- [ ] **Step 5: Commit**

```bash
git add main.c
git commit -m "Board view 3/6: enter/exit helpers + Enter/Esc interception

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: `board_view_update` glide tween + frame-loop call + vanish guard

**Files:**
- Modify: `main.c` — add `board_view_update` (place it right after `board_view_exit` from Task 3); call it in the frame loop right after `camera_update` (~line 14678).

Context: `sol_smoothstep(float)` and `SOL_PI` already exist (used by `reader_update`). The glide must run AFTER `camera_update` so it overrides the camera that frame (this is exactly how the reader's camera swing works). `scene_get` returns NULL for a deleted handle.

- [ ] **Step 1: Add `board_view_update`**

Right after `board_view_exit`:

```c
/* Advance the board-view camera glide (runs AFTER camera_update so it overrides
   it). Also bails out of board view if the viewed board was deleted. */
static void board_view_update(AppState *st, float dt) {
    float e, dyaw;
    if (st->board_view != 0 && scene_get(&st->scene, st->board_view) == 0)
        board_view_exit(st);                 /* board vanished: glide back out */
    if (st->bv_t >= 1.0f) return;            /* settled: nothing to animate */
    st->bv_t += dt / BOARD_VIEW_GLIDE_S;
    if (st->bv_t > 1.0f) st->bv_t = 1.0f;
    e = sol_smoothstep(st->bv_t);
    st->camera.pos = vec3_add(st->bv_from_pos,
                     vec3_scale(vec3_sub(st->bv_to_pos, st->bv_from_pos), e));
    dyaw = st->bv_to_yaw - st->bv_from_yaw;
    while (dyaw >  SOL_PI) dyaw -= 2.0f * SOL_PI;   /* shortest arc */
    while (dyaw < -SOL_PI) dyaw += 2.0f * SOL_PI;
    st->camera.yaw   = st->bv_from_yaw + dyaw * e;
    st->camera.pitch = st->bv_from_pitch +
                       (st->bv_to_pitch - st->bv_from_pitch) * e;
}
```

- [ ] **Step 2: Call it in the frame loop after `camera_update`**

Find the camera update call in `main()` at line ~14678:

```c
            camera_update(&state.camera, &in, (float)dt);
```

Immediately after that line (still inside the same block), add:

```c
            board_view_update(&state, (float)dt);   /* overrides camera_update */
```

- [ ] **Step 3: Build both backends**

Run: `./build.sh && ./build.sh metal`
Expected: both build clean.

- [ ] **Step 4: Manual smoke (optional but recommended)**

Run `./solarium-metal` (or `./solarium`), select a whiteboard, press Enter: the camera should now GLIDE to frame the board. Esc should glide back. The cursor will still be locked and movement still active (Tasks 5–6 fix that) — you are only verifying the camera motion + framing here.

- [ ] **Step 5: Commit**

```bash
git add main.c
git commit -m "Board view 4/6: glide tween + frame-loop hook + vanish guard

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: cursor unlock + movement/look freeze in `read_input`

**Files:**
- Modify: `main.c` — `read_input` (cursor toggle near line ~9277; a `bv_active` local after the modal early-return ~9354; movement zeroing ~9362; keyboard look guard ~9394; mouse-look condition ~9424).

Context: the inventory cursor toggle at lines ~9270–9277 is the exact pattern to mirror. The modal early-return gate is at ~9300–9354 (board view is NOT added there — it must fall through so the 3D interactions run). `bv_active` covers the outbound glide too (movement stays frozen until `bv_t` settles).

- [ ] **Step 1: Add the cursor unlock edge**

Right after the inventory cursor toggle block (after `st->inv_was_open = st->inv_open;` at line ~9277), add:

```c
    /* board view frees the cursor for pointing at cards (mirrors inventory);
       first-person look re-locks on exit. */
    if (st->board_view && !st->board_view_was) {
        glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        st->mouse_skip = 2;
    } else if (!st->board_view && st->board_view_was &&
               !st->inv_open && !st->editor.active) {
        glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        st->mouse_skip = 2;
    }
    st->board_view_was = (sol_bool)(st->board_view != 0);
```

- [ ] **Step 2: Declare `bv_active` with the other `read_input` locals and set it**

C89 wants declarations at the top of the function block, so add `bv_active` to the existing local declaration rather than introducing a mid-function block. Find the top of `read_input` (~line 9261):

```c
static void read_input(GLFWwindow *w, CameraInput *in, double dt, AppState *st) {
    float    look = (float)dt * LOOK_SPEED;
    sol_bool tab_now, dragging, fp;
    double   mx, my;

    fp = (st->camera.mode != CAMERA_ORBIT);
```

Add `bv_active` to the `sol_bool` line and assign it right after `fp`:

```c
static void read_input(GLFWwindow *w, CameraInput *in, double dt, AppState *st) {
    float    look = (float)dt * LOOK_SPEED;
    sol_bool tab_now, dragging, fp, bv_active;
    double   mx, my;

    fp = (st->camera.mode != CAMERA_ORBIT);
    /* board view (and its outbound glide) freezes walking and look — the camera
       is pinned to the framed pose while you work the surface with the cursor. */
    bv_active = (sol_bool)(st->board_view != 0 || st->bv_t < 1.0f);
```

- [ ] **Step 3: Zero movement when `bv_active`**

Find the movement poll (lines ~9357–9362):

```c
    in->forward = glfwGetKey(w, GLFW_KEY_W) == GLFW_PRESS;
    in->back    = glfwGetKey(w, GLFW_KEY_S) == GLFW_PRESS;
    in->left    = glfwGetKey(w, GLFW_KEY_A) == GLFW_PRESS;
    in->right   = glfwGetKey(w, GLFW_KEY_D) == GLFW_PRESS;
    in->up      = glfwGetKey(w, GLFW_KEY_SPACE)        == GLFW_PRESS;
    in->down    = glfwGetKey(w, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS;
```

Immediately after those six lines add:

```c
    if (bv_active) {
        in->forward = in->back = in->left = in->right = SOL_FALSE;
        in->up = in->down = SOL_FALSE;
    }
```

- [ ] **Step 4: Guard keyboard look**

Find the arrow-key look block (the `else` branch at lines ~9393–9398):

```c
    } else {
        if (glfwGetKey(w, GLFW_KEY_RIGHT) == GLFW_PRESS) in->look_dx += look;
        if (glfwGetKey(w, GLFW_KEY_LEFT)  == GLFW_PRESS) in->look_dx -= look;
        if (glfwGetKey(w, GLFW_KEY_UP)    == GLFW_PRESS) in->look_dy += look;
        if (glfwGetKey(w, GLFW_KEY_DOWN)  == GLFW_PRESS) in->look_dy -= look;
    }
```

Wrap the four arrow lines in a `!bv_active` guard:

```c
    } else if (!bv_active) {
        if (glfwGetKey(w, GLFW_KEY_RIGHT) == GLFW_PRESS) in->look_dx += look;
        if (glfwGetKey(w, GLFW_KEY_LEFT)  == GLFW_PRESS) in->look_dx -= look;
        if (glfwGetKey(w, GLFW_KEY_UP)    == GLFW_PRESS) in->look_dy += look;
        if (glfwGetKey(w, GLFW_KEY_DOWN)  == GLFW_PRESS) in->look_dy -= look;
    }
```

- [ ] **Step 5: Guard mouse-look**

Find the mouse-look apply condition at line ~9424:

```c
    } else if ((fp || (dragging && st->drag_handle == 0)) && !st->editor.active) {
```

Change it to also exclude board view:

```c
    } else if ((fp || (dragging && st->drag_handle == 0)) &&
               !st->editor.active && !bv_active) {
```

(No closing brace to manage — `bv_active` is a function-scope local from Step 2.)

- [ ] **Step 6: Build both backends**

Run: `./build.sh && ./build.sh metal`
Expected: both build clean (no `-Werror` unused-variable errors — `bv_active` is read in Steps 3–5).

- [ ] **Step 7: Commit**

```bash
git add main.c
git commit -m "Board view 5/6: unlock cursor + freeze movement/look while framed

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 6: cursor-driven picking — `pick_ray` + FP press cursor-NDC + board-click deselect

**Files:**
- Modify: `main.c` — `pick_ray` (~line 3016); the FP press branch in `read_input` (~line 9487).

Context: `pick_ray` only reads the real cursor in `CAMERA_ORBIT` today. The FP press branch calls `do_pick(st, w, 0.0f, 0.0f)` (screen-center). In board view the camera stays `CAMERA_WALK` so `fp` is true → without changes, picking would target the crosshair, not the cursor. `do_pick(st, w, ndc_x, ndc_y)` sets `st->selected_handle` from a pick at the given NDC. The board itself is pickable, so clicking the board behind the cards must be treated as "deselect".

- [ ] **Step 1: Extend `pick_ray` to read the cursor in board view**

Find in `pick_ray` (~line 3016):

```c
    if (st->camera.mode == CAMERA_ORBIT && ww > 0 && wh > 0) {
        glfwGetCursorPos(w, &mx, &my);
        nx = 2.0f * (float)mx / (float)ww - 1.0f;
        ny = 1.0f - 2.0f * (float)my / (float)wh;
    }
```

Change the condition to also fire in board view:

```c
    if ((st->camera.mode == CAMERA_ORBIT || st->board_view != 0) && ww > 0 && wh > 0) {
        glfwGetCursorPos(w, &mx, &my);
        nx = 2.0f * (float)mx / (float)ww - 1.0f;
        ny = 1.0f - 2.0f * (float)my / (float)wh;
    }
```

This makes the drag/slide/resize handlers (which use `pick_ray`) follow the cursor in board view.

- [ ] **Step 2: Make the FP press select at the cursor + deselect on board-click**

Find the FP press branch (~line 9487):

```c
            } else if (fp) {
                do_pick(st, w, 0.0f, 0.0f);             /* select on press, as before */
                if (try_connect(st, st->selected_handle)) {
```

Replace just the `} else if (fp) {` line and the `do_pick(...)` line with:

```c
            } else if (fp) {
                float pnx = 0.0f, pny = 0.0f;           /* crosshair by default */
                if (st->board_view != 0) {              /* board view: pick at the cursor */
                    int bww, bwh;
                    glfwGetWindowSize(w, &bww, &bwh);
                    if (bww > 0 && bwh > 0) {
                        pnx = 2.0f * (float)mx / (float)bww - 1.0f;
                        pny = 1.0f - 2.0f * (float)my / (float)bwh;
                    }
                }
                do_pick(st, w, pnx, pny);               /* select on press */
                if (st->board_view != 0 && st->selected_handle == st->board_view)
                    st->selected_handle = 0;            /* clicked the board itself = deselect */
                if (try_connect(st, st->selected_handle)) {
```

Leave the rest of the branch (`resize_corner_pick`, `picture_move_pick`, `drag_begin`) unchanged — they read `pick_ray`, now cursor-aware.

- [ ] **Step 3: Build both backends**

Run: `./build.sh && ./build.sh metal`
Expected: both build clean.

- [ ] **Step 4: Commit**

```bash
git add main.c
git commit -m "Board view 6/6: cursor-driven picking (pick_ray + FP select + deselect)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Final verification (after all tasks)

- [ ] **Full gauntlet:** `./build.sh && ./build.sh metal && ./build.sh camtest && ./camera_test`
  Expected: GL + Metal build clean, `camera_test: OK`.
- [ ] **Human live-verify (GUI — the implementer cannot do this; hand back to Fran):**
  1. Select a wall-mounted whiteboard, press Enter → camera glides to frame the whole board with a margin; cursor unlocks. Esc → glides back to the prior standing pose.
  2. Repeat for a **landscape** board and a **free-standing** board (framing fits width-or-height correctly in each).
  3. In board view: click a card to select; drag a card → it slides on the board; drag a note's corner → resize; Enter on a selected note → edit text; Delete → removes it.
  4. Press **N** in board view → a new note spawns onto the board under the cursor.
  5. Click empty board space → deselects (does not exit). Movement keys and mouse-look do nothing while framed.
  6. Confirm no first-person look-jump when entering/leaving (the `mouse_skip = 2` guard).

---

## Self-Review notes (done by plan author)

- **Spec coverage:** framing (T1), state (T2), entry/exit + Enter/Esc (T3), glide + vanish guard (T4), cursor unlock + movement/look freeze incl. outbound glide (T5), cursor picking + create-note works via cursor-aware `pick_ray`/`N` + board-click deselect (T6). `N`-create needs no new code — the existing `N` path pins to the board under `pick_ray`, now cursor-aware.
- **Type consistency:** `CameraPose {pos,yaw,pitch}` and `camera_frame_pose(center,normal,half_w,half_h,fov,aspect,margin)` are used identically in T1 (def) and T3 (call). `bv_*` field names match between T2 (decl), T3/T4 (use). `board_view`/`board_view_was`/`bv_t`/`bv_dir` consistent throughout.
- **No new shader** → no MSL twin; Metal is still built every task to catch nothing-in-particular, by discipline.
