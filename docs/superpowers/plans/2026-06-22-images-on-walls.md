# Images on Walls & Whiteboards — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Carry an image file card, aim at a room wall or whiteboard, and drop it — a picture (a flat quad showing the image) appears there; the file card stays. Wall pictures are corner-resizable.

**Architecture:** A new `"picture"` mesh (a flat quad reusing `emit_card`), `KIND_PLAIN`, `content` = image path, albedo from `load_texture(content)` resolved in `scene_resolve_meshes`. `carry_update` previews a wall mount (`descend_wall_mount`) or whiteboard pin (`board_under_ray`/`board_pin_pos`) for image cards; `cmd_carry_toggle` spawns the picture and restores the card. The resize gate generalizes from `"board"` to `"board"`/`"picture"`.

**Tech Stack:** C89 (strict). Reuses `load_texture`, `descend_wall_mount`, `board_pin_pos`, `board_resize_corner`, `draw_mesh`. No new shader.

**Spec:** `docs/superpowers/specs/2026-06-22-images-on-walls-design.md`

---

## Conventions for every task

- **Strict C89:** declarations at the top of each block, `/* */` comments, no `//`, no mixed declarations/VLAs, `snprintf`/`strncpy`.
- **Gauntlet (stay green):** `./build.sh c89check && ./build.sh debug && ./build.sh metal`.
- Commit after each task; stage only named files. **Never** `git add` `NOTES.stml`, `paper-picture.png`, `scene*.stml`.
- Trailer: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
- This is GUI-heavy (carry/render). Fran live-verifies after Task 3. No new pure function (reuses already-tested geometry).

## File map

- `mesh.c` — the `"picture"` registry row.
- `main.c` — the picture-albedo resolve pass; the resize-gate generalization; the image-card carry branch + the picture-spawn drop + the `AppState` picture fields.

---

### Task 1: the `"picture"` object (mesh + albedo)

**Files:** Modify `mesh.c`, `main.c`.

- [ ] **Step 1: Register the `"picture"` mesh** — in `mesh.c`, immediately after the `"card"` registry row (`{ "card", 3, ... emit_card },`):

```c
    { "picture", 3, { "w", "h", "t" }, { 1.2f, 0.9f, 0.03f }, emit_card },
```

(Reuses `emit_card` → `make_card`: bottom-origin `y` in `[0,h]`, UVs 0..1 on the ±Z faces — the geometry the resize math assumes.)

- [ ] **Step 2: Resolve the picture albedo from content** — in `main.c`, in `scene_resolve_meshes`, immediately before the function's final closing brace (after the texgen loop ends, `o->material.ao_tex = ts.orm;` then its `    }`):

```c
    /* picture albedo (images on walls): a "picture" object shows its image
       file through the lit-albedo path; decode it once via the texture registry
       (sRGB, hot-reload, shared). KIND_PLAIN, so apply_kind_materials never
       clobbers this. */
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        if (o->mesh_ref && strcmp(o->mesh_ref, "picture") == 0 &&
            o->content && o->content[0] && !o->material.albedo_tex.id)
            o->material.albedo_tex = load_texture(o->content);
    }
```

(`load_texture` is defined above `scene_resolve_meshes`, so it's in scope. `i` is already declared in the function.)

- [ ] **Step 3: Gauntlet** — `./build.sh c89check && ./build.sh debug && ./build.sh metal` → all green.

- [ ] **Step 4: Commit**

```bash
git add mesh.c main.c
git commit -m "$(printf 'picture: a "picture" mesh + albedo-from-content resolve\n\nA flat quad (reuses emit_card geometry) whose albedo loads from its content\nimage path via load_texture in scene_resolve_meshes. KIND_PLAIN so the image\nsurvives apply_kind_materials. Nothing creates one yet (Task 3).\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

### Task 2: generalize the resize gate to `"picture"`

**Files:** Modify `main.c`.

- [ ] **Step 1: `board_is_mounted` accepts pictures** — replace:

```c
    if (!o || !o->mesh_ref || strcmp(o->mesh_ref, "board") != 0) return SOL_FALSE;
```

with:

```c
    if (!o || !o->mesh_ref ||
        (strcmp(o->mesh_ref, "board") != 0 && strcmp(o->mesh_ref, "picture") != 0))
        return SOL_FALSE;
```

- [ ] **Step 2: `board_world_corners` reads the object's own ref** — replace the two `mesh_ref_param("board", ...)` lines in `board_world_corners`:

```c
    float w   = o ? mesh_ref_param("board", o->mesh_params, o->mesh_param_count, "w") : 1.8f;
    float ht  = o ? mesh_ref_param("board", o->mesh_params, o->mesh_param_count, "h") : 1.2f;
```

with (use the object's actual mesh ref — both `"board"` and `"picture"` share the `w,h,t` schema):

```c
    const char *mr = (o && o->mesh_ref) ? o->mesh_ref : "board";
    float w   = o ? mesh_ref_param(mr, o->mesh_params, o->mesh_param_count, "w") : 1.8f;
    float ht  = o ? mesh_ref_param(mr, o->mesh_params, o->mesh_param_count, "h") : 1.2f;
```

- [ ] **Step 3: the resize-drag branch reads the object's own ref** — in the `if (lmb && st->resize_board != 0)` branch, replace:

```c
                float    bt  = mesh_ref_param("board", o->mesh_params, o->mesh_param_count, "t");
```

with:

```c
                float    bt  = mesh_ref_param(o->mesh_ref ? o->mesh_ref : "board",
                                              o->mesh_params, o->mesh_param_count, "t");
```

- [ ] **Step 4: Gauntlet** — green. (No picture objects exist yet, so no behavior change; `descend_test` still `OK`.)

- [ ] **Step 5: Commit**

```bash
git add main.c
git commit -m "$(printf 'resize: generalize the corner-resize gate from board to picture\n\nboard_is_mounted accepts mesh_ref \"board\" OR \"picture\"; board_world_corners\nand the resize-drag branch read w/h/t from the object own mesh_ref (both share\nthe w,h,t schema, so the registry-release rebuild path is identical).\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

### Task 3: carry an image card, plant a picture (card stays)

**Files:** Modify `main.c`.

- [ ] **Step 1: Add `AppState` picture fields** — after the `Mesh resize_handle_mesh;` line:

```c
    Mesh        resize_handle_mesh;/* small corner quad; built once on first use */
    sol_u32     picture_aim;       /* image card aimed at a wall/board this frame */
    sol_u32     picture_target;    /* the room (wall) or board to parent the picture */
    vec3        picture_local;     /* the picture's local pos under the target */
    quat        picture_rot;       /* the picture's local rotation */
```

- [ ] **Step 2: Add the image-card branch in `carry_update`** — after the folder branch closes (`return; } } }`) and before `st->file_aim = SOL_FALSE;`, insert:

```c
    st->picture_aim = 0;
    if (o->mesh_ref && strcmp(o->mesh_ref, "card") == 0 && o->kind != KIND_FOLDER &&
        o->content && reader_is_image_path(o->content)) {
        float   pw   = mesh_ref_param("picture", (const float *)0, 0, "w");
        float   ph   = mesh_ref_param("picture", (const float *)0, 0, "h");
        float   pt   = mesh_ref_param("picture", (const float *)0, 0, "t");
        sol_u32 room = descend_room_at(&st->scene, st->camera.pos);
        Ray     ray;
        vec3    bloc;
        sol_u32 board;
        ray.origin = st->camera.pos;
        ray.dir    = camera_forward(&st->camera);
        if (room != 0) {                               /* aim at a WALL -> mount */
            RoomRect r = editor_room_rect(&st->scene, room);
            int   wall;
            vec3  center;
            float ceil_y = r.floor_y + room_interior_height(&st->scene, room);
            if (descend_wall_mount(r, ray, ceil_y, pw * 0.5f, ph * 0.5f, pt,
                                   &wall, &center)) {
                static const float wyaw[4] = { 0.0f, -90.0f, 180.0f, 90.0f };
                vec3 P = vec3_make(center.x, center.y - ph * 0.5f, center.z);
                st->picture_aim    = 1;
                st->picture_target = room;
                st->picture_rot    = quat_from_axis_angle(vec3_make(0.0f,1.0f,0.0f),
                                                          sol_radians(wyaw[wall]));
                st->picture_local  = scene_world_to_local(&st->scene, room, P);
                o->pos = scene_world_to_local(&st->scene, o->parent, P);
                o->rot = st->picture_rot;
                return;
            }
        }
        board = board_under_ray(st, ray, &bloc);       /* else aim at a WHITEBOARD */
        if (board != 0) {
            vec3 lp = board_pin_pos(&st->scene, board, st->carried, bloc, 0.0f, -0.5f * ph);
            mat4 bm = scene_world_matrix(&st->scene, scene_get(&st->scene, board));
            vec3 wp = mat4_mul_point(bm, lp);
            st->picture_aim    = 1;
            st->picture_target = board;
            st->picture_rot    = quat_identity();
            st->picture_local  = lp;
            o->pos = scene_world_to_local(&st->scene, o->parent, wp);
            o->rot = scene_world_rotation(&st->scene, board);
            return;
        }
    }
    st->file_aim = SOL_FALSE;
```

(An image card not aimed at a wall/whiteboard falls through to the existing furniture-filing branch unchanged.)

- [ ] **Step 3: Spawn the picture on drop** — in `cmd_carry_toggle`, insert a branch between the folder branch and the `file_aim` branch (i.e., change `} else if (st->file_aim && ...` to add the picture branch before it):

```c
            } else if (st->picture_aim && st->picture_target != 0 &&
                       scene_get(&st->scene, st->picture_target) != 0) {
                const char *path = o->content;       /* heap ptr survives scene_add */
                Mesh        empty;
                vec3        one = vec3_make(1.0f, 1.0f, 1.0f);
                sol_u32     a;
                memset(&empty, 0, sizeof empty);
                o->pos = st->carry_origin;            /* the image card STAYS */
                a = scene_add(&st->scene, st->picture_target, empty,
                              st->picture_local, st->picture_rot, one);
                scene_mesh_ref_set(&st->scene, a, "picture");
                if (path) scene_content_set(&st->scene, a, path);
                scene_resolve_meshes(&st->scene);     /* builds mesh + loads albedo */
                apply_kind_materials(&st->scene);     /* skips KIND_PLAIN -> image kept */
                scene_save(&st->scene, "scene.stml");
                printf("hung a picture\n");
            } else if (st->file_aim && st->file_target != 0 &&
```

(`scene_add` defaults to `KIND_PLAIN` — boards are created the same way — so no `scene_kind_set` is needed. The picture inherits its workspace from the parent room/board via the parent chain. Capture `path` and restore the card BEFORE `scene_add`, which reallocs the objects array — never deref `o` after.)

- [ ] **Step 4: Reset `picture_aim` on toggle** — find the trio at the end of `cmd_carry_toggle`:

```c
        st->carried   = 0;
        st->plant_aim = SOL_FALSE;
        st->file_aim  = SOL_FALSE;
```

and add:

```c
        st->carried     = 0;
        st->plant_aim   = SOL_FALSE;
        st->file_aim    = SOL_FALSE;
        st->picture_aim = SOL_FALSE;
```

- [ ] **Step 5: Gauntlet** — `./build.sh c89check && ./build.sh debug && ./build.sh metal` → all green.

- [ ] **Step 6: Regression** — `./build.sh descendtest && ./descend_test` → `descend_test: OK`.

- [ ] **Step 7: Commit**

```bash
git add main.c
git commit -m "$(printf 'picture: carry an image card, plant it on a wall or whiteboard\n\ncarry_update previews an image card as a wall mount (descend_wall_mount) or a\nwhiteboard pin (board_under_ray/board_pin_pos); cmd_carry_toggle spawns a\n"picture" object there and restores the card to its spot (the folder-plant\npattern). Wall pictures are corner-resizable via the generalized gate.\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

- [ ] **Step 8: Hand to Fran for live verify** — carry an image card (a png/jpg in a mirror room), aim at a wall → a picture mounts flush showing the image, the card stays on the floor; select it and corner-resize; reload keeps the picture (with image) and the card. Aim at a whiteboard → the image pins to the board face showing the image. Non-image cards and furniture-filing still behave as before. Both `./solarium` and `./solarium-metal`.

---

## Self-review (against the spec)

- **`"picture"` mesh (flat quad, bottom-origin) + albedo from content via `load_texture`, KIND_PLAIN** → Task 1. ✓
- **Resize gate generalized `"board"`→`"board"`/`"picture"`** → Task 2. ✓
- **Carry image card → wall mount (`descend_wall_mount`) or whiteboard pin (`board_pin_pos`); card stays; picture spawned** → Task 3 Steps 2–3. ✓
- **Persistence (mesh "picture" + content saved; texture re-resolves on load)** → Task 1 Step 2 (resolve runs every load). ✓
- **No `SceneObject*` across `scene_add`** → Task 3 Step 3 captures `path` + restores the card before the add. ✓
- **Live verify both backends** → Task 3 Step 8. ✓

**Type/name consistency:** `picture_aim`/`picture_target`/`picture_local`/`picture_rot` defined (Task 3 Step 1), used (Steps 2–4). `mesh_ref "picture"` registered (Task 1) and used in resolve (Task 1), resize gate (Task 2), spawn (Task 3). `reader_is_image_path`, `descend_room_at`, `editor_room_rect`, `room_interior_height`, `descend_wall_mount`, `board_under_ray`, `board_pin_pos`, `scene_world_matrix`, `mat4_mul_point`, `scene_world_rotation`, `quat_identity`, `quat_from_axis_angle`, `sol_radians`, `scene_world_to_local`, `scene_add`, `scene_mesh_ref_set`, `scene_content_set`, `scene_resolve_meshes`, `apply_kind_materials`, `load_texture` all exist as used. No placeholders.
