# Whiteboard Resize (Corner Handles) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Select a wall-mounted whiteboard → four corner handles appear; grab a corner and crosshair-drag to resize it in place on the wall (opposite corner anchored), live and persisted.

**Architecture:** Two new headless geometry functions (`board_corners`, `board_resize_corner`) in `descend.c`, plus `main.c` wiring: a `board_is_mounted` predicate, resize state, a handle render, and a corner hit-test + resize-drag woven into the existing first-person LMB press/drag/release path (the same crosshair model the carry-move uses).

**Tech Stack:** C89 (strict), `descend.c` board/wall geometry, the `main.c` drag system, `draw_mesh` for the handles. No new shader.

**Spec:** `docs/superpowers/specs/2026-06-22-whiteboard-resize-design.md`

---

## Conventions for every task

- **Strict C89:** declarations at the top of each block, `/* */` comments, no `//`, no mixed declarations/VLAs, `snprintf`/`strncpy`.
- **Gauntlet (stay green):** `./build.sh c89check && ./build.sh debug && ./build.sh metal`.
- Commit after each task; stage only the named files. **Never** `git add` `NOTES.stml`, `paper-picture.png`, `scene*.stml`.
- Trailer: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
- The `main.c` interaction is GUI — Fran live-verifies after Tasks 2 and 3. Geometry (Task 1) is unit-tested.

## File map

- `descend.h` / `descend.c` — `board_corners`, `board_resize_corner`.
- `descend_test.c` — their unit cases.
- `main.c` — `board_is_mounted` / `board_world_corners` / `room_interior_height` / `resize_corner_pick` helpers; `AppState` resize fields; handle render; press/drag/release wiring.

---

### Task 1: resize geometry + unit tests

**Files:** Modify `descend.h`, `descend.c`, `descend_test.c`.

- [ ] **Step 1: Write the failing tests** — in `descend_test.c`, add before the final `if (fails == 0) printf("descend_test: OK\n");`:

```c
    /* corner geometry + resize math for a wall-mounted board */
    {
        vec3 cor[4], u = vec3_make(1.0f, 0.0f, 0.0f), p = vec3_make(0.0f, 0.0f, 0.0f);
        float w, h; vec3 o;
        board_corners(p, 2.0f, 1.5f, u, cor);
        CHECK(fabs((double)(cor[0].x + 1.0f)) < 1e-4 && fabs((double)cor[0].y) < 1e-4);       /* BL */
        CHECK(fabs((double)(cor[1].x - 1.0f)) < 1e-4);                                         /* BR */
        CHECK(fabs((double)(cor[2].x - 1.0f)) < 1e-4 && fabs((double)(cor[2].y - 1.5f)) < 1e-4);/* TR */
        CHECK(fabs((double)(cor[3].x + 1.0f)) < 1e-4 && fabs((double)(cor[3].y - 1.5f)) < 1e-4);/* TL */
        /* anchor BL (-1,0,0), drag TR out to (2,2,0): w=3, h=2, origin = bottom-center */
        board_resize_corner(cor[0], vec3_make(2.0f, 2.0f, 0.0f), u, 0.3f, &w, &h, &o);
        CHECK(fabs((double)(w - 3.0f)) < 1e-4);
        CHECK(fabs((double)(h - 2.0f)) < 1e-4);
        CHECK(fabs((double)(o.x - 0.5f)) < 1e-4);   /* mid of -1..2 */
        CHECK(fabs((double)o.y) < 1e-4);            /* bottom (drag was above anchor) */
        /* anchor TR (1,1.5,0), drag down-left to (-2,0,0): origin bottom = lower y */
        board_resize_corner(vec3_make(1.0f, 1.5f, 0.0f), vec3_make(-2.0f, 0.0f, 0.0f),
                            u, 0.3f, &w, &h, &o);
        CHECK(fabs((double)(w - 3.0f)) < 1e-4);
        CHECK(fabs((double)(h - 1.5f)) < 1e-4);
        CHECK(fabs((double)(o.x + 0.5f)) < 1e-4);   /* mid of 1..-2 */
        CHECK(fabs((double)o.y) < 1e-4);            /* bottom = 1.5 - 1.5 */
        /* tiny drag floors at min_size */
        board_resize_corner(vec3_make(0.0f,0.0f,0.0f), vec3_make(0.1f,0.1f,0.0f),
                            u, 0.3f, &w, &h, &o);
        CHECK(fabs((double)(w - 0.3f)) < 1e-4 && fabs((double)(h - 0.3f)) < 1e-4);
    }
```

- [ ] **Step 2: Verify it fails to build** — Run: `./build.sh descendtest` → error `implicit declaration of function 'board_corners'`.

- [ ] **Step 3: Declare both in `descend.h`** — after the `descend_wall_mount` declaration:

```c
/* The 4 world corners of a wall-mounted board: bottom-center origin `p`, width
   `w`, height `h`, horizontal wall axis `u` (unit; vertical is world-up). Order:
   0=bottom-left, 1=bottom-right, 2=top-right, 3=top-left. */
void board_corners(vec3 p, float w, float h, vec3 u, vec3 out[4]);

/* Resize a board by dragging one corner: `anchor` = the fixed opposite corner,
   `dragged` = the grabbed corner's new point (on the wall plane), `u` = the wall
   horizontal axis. Returns new w/h (floored at min_size) + the bottom-center
   origin. */
void board_resize_corner(vec3 anchor, vec3 dragged, vec3 u, float min_size,
                         float *out_w, float *out_h, vec3 *out_origin);
```

- [ ] **Step 4: Define both in `descend.c`** — after `descend_wall_mount`:

```c
void board_corners(vec3 p, float w, float h, vec3 u, vec3 out[4]) {
    vec3 half = vec3_scale(u, w * 0.5f);
    vec3 up   = vec3_make(0.0f, h, 0.0f);
    out[0] = vec3_sub(p, half);                          /* bottom-left  */
    out[1] = vec3_add(p, half);                          /* bottom-right */
    out[2] = vec3_add(vec3_add(p, half), up);            /* top-right    */
    out[3] = vec3_add(vec3_sub(p, half), up);            /* top-left     */
}

void board_resize_corner(vec3 anchor, vec3 dragged, vec3 u, float min_size,
                         float *out_w, float *out_h, vec3 *out_origin) {
    vec3  d  = vec3_sub(dragged, anchor);
    float du = vec3_dot(d, u);
    float dv = dragged.y - anchor.y;
    float su = (du < 0.0f) ? -1.0f : 1.0f;
    float sv = (dv < 0.0f) ? -1.0f : 1.0f;
    float w  = (du < 0.0f) ? -du : du;
    float h  = (dv < 0.0f) ? -dv : dv;
    vec3  p;
    if (w < min_size) w = min_size;
    if (h < min_size) h = min_size;
    p   = vec3_add(anchor, vec3_scale(u, su * w * 0.5f));
    p.y = (sv >= 0.0f) ? anchor.y : anchor.y - h;
    *out_w = w; *out_h = h; *out_origin = p;
}
```

- [ ] **Step 5: Verify pass** — Run: `./build.sh descendtest && ./descend_test` → `descend_test: OK`.

- [ ] **Step 6: Gauntlet** — `./build.sh c89check && ./build.sh debug && ./build.sh metal` → all green.

- [ ] **Step 7: Commit**

```bash
git add descend.h descend.c descend_test.c
git commit -m "$(printf 'descend: board_corners + board_resize_corner geometry\n\nThe pure pieces of whiteboard resize: the 4 world corners of a wall-mounted\nboard, and a corner-drag resize (anchor opposite corner, min-size floor) ->\nnew w/h + bottom-center origin. Unit-tested.\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

### Task 2: resize state + handle render (handles appear; no interaction yet)

**Files:** Modify `main.c`.

- [ ] **Step 1: Add `AppState` resize fields** — after the `drag_board_oy;` line:

```c
    float       drag_board_oy;
    sol_u32     resize_board;      /* wall-board being corner-resized; 0 = none */
    vec3        resize_anchor;     /* the fixed (opposite) corner, world        */
    vec3        resize_u;          /* the wall's horizontal in-plane axis       */
    sol_u32     resize_room;       /* the board's parent room (wall clamp)      */
    Mesh        resize_handle_mesh;/* small corner quad; built once on first use */
```

- [ ] **Step 2: Add the board helpers** — in `main.c` just before `static void carry_update(AppState *st)`:

```c
/* a board mounted on a wall = mesh "board" whose parent carries room_type. */
static sol_bool board_is_mounted(Scene *s, sol_u32 h) {
    SceneObject *o = scene_get(s, h);
    SceneObject *par;
    if (!o || !o->mesh_ref || strcmp(o->mesh_ref, "board") != 0) return SOL_FALSE;
    if (o->parent == 0) return SOL_FALSE;
    par = scene_get(s, o->parent);
    return (sol_bool)(par != 0 && scene_meta_get(s, par->handle, "room_type") != 0);
}

/* yaw of a mounted board (its facing). */
static float board_yaw(Scene *s, sol_u32 h) {
    quat q;
    if (!scene_get(s, h)) return 0.0f;
    q = scene_world_rotation(s, h);   /* takes a handle, not a SceneObject* */
    return 2.0f * (float)atan2((double)q.y, (double)q.w);
}

/* world corners (out[4]) + the wall horizontal axis (*out_u) of a mounted board. */
static void board_world_corners(Scene *s, sol_u32 h, vec3 out[4], vec3 *out_u) {
    SceneObject *o   = scene_get(s, h);
    vec3  p   = object_world_pos(s, h);
    float w   = o ? mesh_ref_param("board", o->mesh_params, o->mesh_param_count, "w") : 1.8f;
    float ht  = o ? mesh_ref_param("board", o->mesh_params, o->mesh_param_count, "h") : 1.2f;
    float yaw = board_yaw(s, h);
    vec3  u   = vec3_make((float)cos((double)yaw), 0.0f, -(float)sin((double)yaw));
    board_corners(p, w, ht, u, out);
    if (out_u) *out_u = u;
}

/* room interior height from its "room" shell (default 3.0 if none). */
static float room_interior_height(Scene *s, sol_u32 room) {
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        SceneObject *c = &s->objects[i];
        if (c->parent == room && c->mesh_ref && strcmp(c->mesh_ref, "room") == 0)
            return mesh_ref_param("room", c->mesh_params, c->mesh_param_count, "h");
    }
    return 3.0f;
}
```

- [ ] **Step 3: Render the handles** — in `render()`, inside the `if (state->ui_font)` block, immediately after the bookshelf-labels block closes (the `        }` that ends it) and before the `    }` that closes `if (state->ui_font)`:

```c
        /* resize handles (whiteboard resize): a selected wall-mounted board
           shows a small glowing quad at each of its 4 corners. */
        if (state->selected_handle != 0 &&
            board_is_mounted(&state->scene, state->selected_handle)) {
            vec3     cor[4], u, n;
            float    yaw = board_yaw(&state->scene, state->selected_handle);
            Material hm  = material_default();
            int      hk;
            if (state->resize_handle_mesh.index_count == 0) {
                MeshBuilder mb;
                mb_init(&mb);
                make_page(&mb, 0.12f, 0.12f);
                state->resize_handle_mesh = mesh_from_builder(&mb);
                mb_free(&mb);
            }
            board_world_corners(&state->scene, state->selected_handle, cor, &u);
            n  = vec3_make((float)sin((double)yaw), 0.0f, (float)cos((double)yaw));
            hm.base_color = vec3_make(1.0f, 0.85f, 0.2f);
            hm.emissive   = vec3_make(1.2f, 0.9f, 0.2f);   /* glow: reads on any wall */
            for (hk = 0; hk < 4; hk++) {
                mat4 m = mat4_mul(
                    mat4_translate(vec3_add(cor[hk], vec3_scale(n, 0.01f))),
                    quat_to_mat4(quat_from_axis_angle(vec3_make(0.0f,1.0f,0.0f), yaw)));
                draw_mesh(state, state->resize_handle_mesh, m, view, proj, eye, 0.0f, hm);
            }
        }
```

- [ ] **Step 4: Gauntlet** — `./build.sh c89check && ./build.sh debug && ./build.sh metal` → all green.

- [ ] **Step 5: Commit**

```bash
git add main.c
git commit -m "$(printf 'resize: show corner handles on a selected wall-mounted board\n\nboard_is_mounted / board_world_corners / room_interior_height helpers + the\nresize AppState fields, and a render block that draws a small glowing quad at\neach of the 4 corners when a mounted board is selected. No interaction yet.\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

- [ ] **Step 6: (optional) Fran smoke-check** — mount a board, select it, confirm four amber handles sit at the corners; deselect → they vanish; an off-wall board shows none.

---

### Task 3: corner pick + resize drag + release

**Files:** Modify `main.c`.

- [ ] **Step 1: Add `resize_corner_pick`** — in `main.c` right after `board_world_corners` (Task 2 Step 2):

```c
/* On a press: if the crosshair ray passes near a corner handle of the selected
   mounted board, begin a resize with the OPPOSITE corner anchored. 1 on a grab. */
static int resize_corner_pick(AppState *st, GLFWwindow *w) {
    Ray   ray = pick_ray(st, w);
    vec3  cor[4], u;
    int   i, best = -1;
    float bestd = 0.18f;                       /* grab radius (m) */
    SceneObject *o = scene_get(&st->scene, st->selected_handle);
    ray.dir = vec3_normalize(ray.dir);
    board_world_corners(&st->scene, st->selected_handle, cor, &u);
    for (i = 0; i < 4; i++) {
        vec3  rel   = vec3_sub(cor[i], ray.origin);
        float along = vec3_dot(rel, ray.dir);
        vec3  perp;
        float d;
        if (along <= 0.0f) continue;           /* behind the camera */
        perp = vec3_sub(rel, vec3_scale(ray.dir, along));
        d    = (float)sqrt((double)vec3_dot(perp, perp));
        if (d < bestd) { bestd = d; best = i; }
    }
    if (best < 0) return 0;
    st->resize_board  = st->selected_handle;
    st->resize_anchor = cor[(best + 2) % 4];   /* the opposite corner */
    st->resize_u      = u;
    st->resize_room   = o ? o->parent : 0;
    return 1;
}
```

- [ ] **Step 2: Hook the press** — in the first-person press branch, replace:

```c
            } else if (fp) {
                do_pick(st, w, 0.0f, 0.0f);             /* select on press, as before */
                if (try_connect(st, st->selected_handle)) {
                    /* the press completed a connection — no drag */
                } else if (st->selected_handle != 0 && st->selected_handle != st->page_handle)
                    drag_begin(st, w, st->selected_handle);
            } else {
```

with (a corner grab pre-empts the carry-move):

```c
            } else if (fp) {
                do_pick(st, w, 0.0f, 0.0f);             /* select on press, as before */
                if (try_connect(st, st->selected_handle)) {
                    /* the press completed a connection — no drag */
                } else if (st->selected_handle != 0 &&
                           board_is_mounted(&st->scene, st->selected_handle) &&
                           resize_corner_pick(st, w)) {
                    /* grabbed a corner handle — resize, not carry */
                } else if (st->selected_handle != 0 && st->selected_handle != st->page_handle)
                    drag_begin(st, w, st->selected_handle);
            } else {
```

- [ ] **Step 3: Add the resize-drag branch** — immediately before `if (lmb && st->drag_handle != 0) {              /* ---- carrying ---- */`:

```c
        if (lmb && st->resize_board != 0) {             /* ---- resizing ---- */
            SceneObject *o = scene_get(&st->scene, st->resize_board);
            if (!o || st->resize_room == 0 ||
                scene_get(&st->scene, st->resize_room) == 0) {
                st->resize_board = 0;                   /* board/room vanished */
            } else {
                Ray      ray = pick_ray(st, w);
                RoomRect r   = editor_room_rect(&st->scene, st->resize_room);
                float    yaw = board_yaw(&st->scene, st->resize_board);
                vec3     n   = vec3_make((float)sin((double)yaw), 0.0f, (float)cos((double)yaw));
                float    bt  = mesh_ref_param("board", o->mesh_params, o->mesh_param_count, "t");
                float    tt;
                if (ray_vs_plane(ray, st->resize_anchor, n, &tt) && tt > 0.0f) {
                    vec3  hit   = vec3_add(ray.origin, vec3_scale(ray.dir, tt));
                    float perpH = (n.z * n.z > 0.25f) ? r.hd : r.hw;
                    float runH  = (n.z * n.z > 0.25f) ? r.hw : r.hd;
                    float ceil_y= r.floor_y + room_interior_height(&st->scene, st->resize_room);
                    vec3  wallc = vec3_sub(vec3_make(r.cx, hit.y, r.cz), vec3_scale(n, perpH));
                    float du    = vec3_dot(vec3_sub(hit, wallc), st->resize_u);
                    float hy    = hit.y;
                    vec3  dragged, origin;
                    float nw, nh, p3[3];
                    if (du >  runH) du =  runH;          /* clamp along the wall */
                    if (du < -runH) du = -runH;
                    if (hy < r.floor_y) hy = r.floor_y;  /* clamp floor..ceil */
                    if (hy > ceil_y)    hy = ceil_y;
                    dragged   = vec3_add(vec3_make(wallc.x, hy, wallc.z),
                                         vec3_scale(st->resize_u, du));
                    board_resize_corner(st->resize_anchor, dragged, st->resize_u, 0.3f,
                                        &nw, &nh, &origin);
                    p3[0] = nw; p3[1] = nh; p3[2] = bt;
                    scene_mesh_params_set(&st->scene, st->resize_board, p3, 3);
                    o = scene_get(&st->scene, st->resize_board);
                    if (o) o->pos = scene_world_to_local(&st->scene, o->parent, origin);
                    scene_resolve_meshes(&st->scene);
                }
            }
        }
        if (lmb && st->drag_handle != 0) {              /* ---- carrying ---- */
```

- [ ] **Step 4: Hook the release** — in the release block, make the resize finish first. Replace:

```c
        if (!lmb && st->lmb_was_down) {                 /* ---- release ---- */
            if (st->drag_handle != 0 && st->drag_moved) {
```

with:

```c
        if (!lmb && st->lmb_was_down) {                 /* ---- release ---- */
            if (st->resize_board != 0) {                /* finished a resize */
                scene_save(&st->scene, "scene.stml");
                st->resize_board = 0;
            } else if (st->drag_handle != 0 && st->drag_moved) {
```

- [ ] **Step 5: Gauntlet** — `./build.sh c89check && ./build.sh debug && ./build.sh metal` → all green.

- [ ] **Step 6: Regression** — `./build.sh descendtest && ./descend_test` → `descend_test: OK`.

- [ ] **Step 7: Commit**

```bash
git add main.c
git commit -m "$(printf 'resize: grab a corner handle and drag to resize a wall board\n\nA press near a corner handle of a selected mounted board begins a resize\n(opposite corner anchored), pre-empting the carry-move. The drag projects the\ncrosshair onto the wall plane, clamps within the wall, and rebuilds the board\nlive via board_resize_corner; release saves.\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

- [ ] **Step 8: Hand to Fran for live verify** — mount a board, select it, grab a corner and look around: it grows/shrinks live with the opposite corner pinned, clamped to the wall; tiny drags floor at 0.3 m; release persists; reload keeps the size; carry-move (grabbing the board body, not a corner) still works; off-wall boards show no handles. Both `./solarium` and `./solarium-metal`.

---

## Self-review (against the spec)

- **`board_resize_corner` (anchor opposite, min-size floor, bottom-center origin)** → Task 1. ✓
- **Wall axes from yaw (`n`, `u`); 4 corners** → `board_corners` (Task 1) + `board_world_corners`/`board_yaw` (Task 2). ✓
- **`board_is_mounted` predicate** → Task 2 Step 2. ✓
- **Handles render when a mounted board is selected** → Task 2 Step 3. ✓
- **Press grabs a corner (priority over carry); drag projects on wall plane + clamps + rebuilds; release saves** → Task 3 Steps 2/3/4. ✓
- **AppState resize state** → Task 2 Step 1. ✓
- **Min size + wall clamp (run span + floor/ceil)** → Task 3 Step 3. ✓
- **Live rebuild + persistence** → Task 3 Step 3 (`scene_resolve_meshes`) + Step 4 (`scene_save`). ✓
- **Unit test + live verify** → Task 1 tests + Task 3 Step 8. ✓

**Type/name consistency:** `board_corners(vec3,float,float,vec3,vec3[4])` and `board_resize_corner(vec3,vec3,vec3,float,float*,float*,vec3*)` declared (Task 1 Step 3), defined (Step 4), called identically in tests and in `main.c` (Tasks 2/3). `resize_board`/`resize_anchor`/`resize_u`/`resize_room`/`resize_handle_mesh` defined in Task 2 Step 1, used in Tasks 2/3. `board_is_mounted`/`board_yaw`/`board_world_corners`/`room_interior_height`/`resize_corner_pick` defined before use. `pick_ray`, `do_pick`, `drag_begin`, `editor_room_rect`, `ray_vs_plane`, `scene_mesh_params_set`, `scene_resolve_meshes`, `scene_world_to_local`, `object_world_pos`, `scene_world_rotation`, `make_page`, `material_default`, `draw_mesh`, `vec3_normalize` all exist as used. No placeholders.
