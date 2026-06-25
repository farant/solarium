# Image Pictures on Boards Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An image landing on a whiteboard (by click-drag OR E-carry) becomes a resizable, draggable "picture" widget — the same as dropping an image onto a wall.

**Architecture:** Factor the picture-spawn into a shared helper; make the click-drag drop spawn a picture for images (the carry drop already does); then add the missing picture-on-board resize (a predicate extending three resize gates + a board-local, aspect-locked resize branch reusing `board_resize_corner` and the registry-shared-mesh rebuild dance). All in `main.c`.

**Tech Stack:** C89 (strict: `-Wall -Wextra -Werror -pedantic`), OpenGL + Metal dual backend. **No new shader** — pictures use the existing lit-albedo `draw_mesh` path → no MSL twin. Spec: `docs/superpowers/specs/2026-06-25-image-pictures-on-boards-design.md`.

**Conventions the implementer MUST follow:**
- Strict C89: declarations at the TOP of each block; no C99 math (`(float)sin((double)x)`).
- Commit on the current branch, message body ending with:
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`
- Never `git add` `NOTES.stml` or `paper-picture.png`. Stage only `main.c`.
- **Build gauntlet after EVERY task — ALL THREE:** `./build.sh c89check` (expect `c89check: PASS — all sources are C89-pedantic clean`), `./build.sh` (expect `built ./solarium (debug)`), `./build.sh metal` (expect `built ./solarium-metal ...`). The lenient `./build.sh` does NOT catch C89 decl-after-statement — `c89check` does, so do not skip it.
- Find anchors by the quoted CODE TEXT (line numbers are approximate).
- **Use-after-realloc rule:** `scene_add` may move the objects array, so a `SceneObject *` is invalid across it. Set fields on an existing object BEFORE any `scene_add`, and re-fetch via `scene_get(handle)` after.

---

## File Touch List

- `main.c` only:
  - `spawn_image_picture` helper (before `cmd_carry_toggle`) + refactor the carry release to use it. (Task 1)
  - drag-release-onto-board: image file → `spawn_image_picture` instead of an alias card. (Task 2)
  - `picture_on_board` predicate + extend the 3 resize gates + the board-local resize branch. (Task 3)

---

### Task 1: `spawn_image_picture` helper + carry-release refactor (behavior-preserving)

**Files:**
- Modify: `main.c` — add the helper just before `cmd_carry_toggle` (~line 8345); refactor the carry-release picture build (~lines 8366–8396) to call it.

Context: `image_load(path, &Image)` / `image_fit_box(src_w, src_h, field_w, field_h, &out_w, &out_h)` / `image_free(&Image)` (image.h), `scene_add` / `scene_mesh_ref_set` / `scene_content_set` / `scene_mesh_params_set` / `scene_resolve_meshes` / `apply_kind_materials` all exist and are used by the current carry release. `Image` has `.w` / `.h` (ints).

- [ ] **Step 1: Add the helper**

In `main.c`, immediately BEFORE `static void cmd_carry_toggle(AppState *st) {` (~line 8345), insert:

```c
/* Spawn a "picture" of `content` (an image path) parented to `parent` at local
   `pos`/`rot`, sized to the image's aspect (fit into a 1.6 x 1.2 box). Returns
   the new handle. Shared by the carry-drop and click-drag-drop paths so both
   build an identical picture. */
static sol_u32 spawn_image_picture(AppState *st, sol_u32 parent,
                                   vec3 pos, quat rot, const char *content) {
    Mesh    empty;
    vec3    one = vec3_make(1.0f, 1.0f, 1.0f);
    float   pw  = 1.2f, ph = 0.9f, pp[3];
    Image   img;
    sol_u32 a;
    memset(&empty, 0, sizeof empty);
    if (content && image_load(content, &img)) {     /* size the frame to the image's aspect */
        image_fit_box(img.w, img.h, 1.6f, 1.2f, &pw, &ph);
        image_free(&img);
    }
    a = scene_add(&st->scene, parent, empty, pos, rot, one);
    scene_mesh_ref_set(&st->scene, a, "picture");
    if (content) scene_content_set(&st->scene, a, content);
    pp[0] = pw; pp[1] = ph; pp[2] = 0.03f;
    scene_mesh_params_set(&st->scene, a, pp, 3);
    scene_resolve_meshes(&st->scene);               /* builds the mesh + loads the albedo */
    apply_kind_materials(&st->scene);               /* skips KIND_PLAIN -> keeps the image */
    return a;
}
```

- [ ] **Step 2: Refactor the carry release to use it**

In `cmd_carry_toggle`, find the picture-creation block (it begins `} else if (st->picture_aim && st->picture_target != 0 &&` and ends with `printf("hung a picture\n");`):

```c
            } else if (st->picture_aim && st->picture_target != 0 &&
                       scene_get(&st->scene, st->picture_target) != 0) {
                const char *path = o->content;       /* heap ptr survives scene_add */
                Mesh        empty;
                vec3        one    = vec3_make(1.0f, 1.0f, 1.0f);
                vec3        plocal = st->picture_local;
                float       pw = 1.2f, ph = 0.9f;
                float       defh = mesh_ref_param("picture", (const float *)0, 0, "h");
                sol_u32     a;
                Image       img;
                memset(&empty, 0, sizeof empty);
                if (path && image_load(path, &img)) { /* size the frame to the image's aspect */
                    image_fit_box(img.w, img.h, 1.6f, 1.2f, &pw, &ph);
                    image_free(&img);
                }
                plocal.y += (defh - ph) * 0.5f;       /* keep the previewed CENTER as h changes */
                o->parent = st->carry_prev_parent;    /* the image card RETURNS to where it was */
                o->pos    = st->carry_origin;          /*   (e.g. back onto its shelf) */
                o->rot    = st->carry_prev_rot;
                a = scene_add(&st->scene, st->picture_target, empty,
                              plocal, st->picture_rot, one);
                scene_mesh_ref_set(&st->scene, a, "picture");
                if (path) scene_content_set(&st->scene, a, path);
                {
                    float pp[3]; pp[0] = pw; pp[1] = ph; pp[2] = 0.03f;
                    scene_mesh_params_set(&st->scene, a, pp, 3);
                }
                scene_resolve_meshes(&st->scene);     /* builds mesh + loads albedo */
                apply_kind_materials(&st->scene);     /* skips KIND_PLAIN -> image kept */
                scene_save(&st->scene, "scene.stml");
                printf("hung a picture\n");
            } else if (st->file_aim && st->file_target != 0 &&
```

Replace the body of that `else if` (everything from `const char *path` through `printf("hung a picture\n");`) with the helper-based version (keep the `} else if (st->picture_aim ...) {` line and the following `} else if (st->file_aim ...` line unchanged):

```c
            } else if (st->picture_aim && st->picture_target != 0 &&
                       scene_get(&st->scene, st->picture_target) != 0) {
                const char *path   = o->content;      /* heap ptr survives scene_add */
                float       defh   = mesh_ref_param("picture", (const float *)0, 0, "h");
                vec3        plocal = st->picture_local;
                sol_u32     a;
                o->parent = st->carry_prev_parent;    /* the image card RETURNS to where it was */
                o->pos    = st->carry_origin;          /*   (e.g. back onto its shelf) */
                o->rot    = st->carry_prev_rot;
                a = spawn_image_picture(st, st->picture_target, plocal, st->picture_rot, path);
                {   /* keep the previewed CENTRE as the height changes from the
                       default to the image's aspect (the preview pinned at defh) */
                    SceneObject *ao = scene_get(&st->scene, a);
                    if (ao) {
                        float ph = mesh_ref_param("picture", ao->mesh_params,
                                                  ao->mesh_param_count, "h");
                        ao->pos.y += (defh - ph) * 0.5f;
                    }
                }
                scene_save(&st->scene, "scene.stml");
                printf("hung a picture\n");
            } else if (st->file_aim && st->file_target != 0 &&
```

(Note: `o->parent`/`o->pos`/`o->rot` are set BEFORE `spawn_image_picture` — which calls `scene_add` — so there's no use-after-realloc; `o` is not used afterward in this block.)

- [ ] **Step 3: Build gauntlet**

Run: `./build.sh c89check && ./build.sh && ./build.sh metal`
Expected: `c89check: PASS`, `built ./solarium (debug)`, `built ./solarium-metal ...`. (Pure refactor — carry-onto-wall and carry-onto-board still produce a picture exactly as before.)

- [ ] **Step 4: Commit**

```bash
git add main.c
git commit -m "Image pictures on boards 1/3: spawn_image_picture helper + carry refactor

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Click-drag an image file onto a board → a picture (not an alias card)

**Files:**
- Modify: `main.c` — the drag-release-onto-board branch in `read_input` (~lines 9993–10032).

Context: `reader_is_image_path(const char *path)` returns true for png/jpg/etc. `board_pin_pos(&st->scene, board, handle, blocal, ox, oy)` → board-local pos. The existing branch snaps the file record home then pins a `KIND_ALIAS` `"card"`. We keep that for non-images and spawn a picture for images. `cpath` is `o->content`, a heap string that survives `scene_add` (the existing code relies on this).

- [ ] **Step 1: Replace the drag-release-onto-board branch**

Find this block (inside `read_input`, ~line 9993):

```c
                if (o && (o->kind == KIND_FILE || o->kind == KIND_FOLDER) && o->content) {
                    /* a mirror's record never leaves its room (§1.3: membership
                       follows disk) — dropping it on a board snaps the record
                       home and pins an ALIAS at the drop point instead */
                    vec3    blocal;
                    sol_u32 board = board_under_ray(st, pick_ray(st, w), &blocal);
                    if (board != 0) {
                        const char *cpath = o->content;     /* heap string: the
                                                               pointer survives
                                                               the scene_add */
                        char        lbuf[16];
                        const char *nm = object_label(&st->scene, st->drag_handle, lbuf);
                        Mesh        empty;
                        vec3        one = vec3_make(1.0f, 1.0f, 1.0f);
                        float       ch  = mesh_ref_param("card", (const float *)0, 0, "h");
                        sol_u32     a;
                        o->parent = st->drag_prev_parent;   /* snap home */
                        o->pos    = st->drag_prev_pos;
                        o->rot    = st->drag_prev_rot;
                        memset(&empty, 0, sizeof empty);
                        a = scene_add(&st->scene, board, empty,
                                      vec3_make(0.0f, 0.0f, 0.0f), quat_identity(), one);
                        scene_kind_set(&st->scene, a, KIND_ALIAS);
                        scene_content_set(&st->scene, a, cpath);
                        scene_meta_set(&st->scene, a, "name", nm);
                        scene_mesh_ref_set(&st->scene, a, "card");
                        {
                            SceneObject *ao = scene_get(&st->scene, a);
                            if (ao) ao->pos = board_pin_pos(&st->scene, board, a,
                                                            blocal, 0.0f, -0.5f * ch);
                        }
                        scene_resolve_meshes(&st->scene);
                        apply_kind_materials(&st->scene);
                        st->selected_handle = a;
                        printf("pinned alias '%s' to the board — the record stays home\n", nm);
                        o = scene_get(&st->scene, st->drag_handle);  /* re-fetch:
                                                               scene_add may move
                                                               the objects array */
                    }
                }
```

Replace it with (image → picture; else → the existing alias path):

```c
                if (o && (o->kind == KIND_FILE || o->kind == KIND_FOLDER) && o->content) {
                    /* a mirror's record never leaves its room (§1.3) — dropping it
                       on a board snaps the record home. An IMAGE drops a resizable
                       PICTURE; any other file pins a filename ALIAS card. */
                    vec3    blocal;
                    sol_u32 board = board_under_ray(st, pick_ray(st, w), &blocal);
                    if (board != 0) {
                        const char *cpath = o->content;     /* heap str survives scene_add */
                        o->parent = st->drag_prev_parent;   /* snap the record home */
                        o->pos    = st->drag_prev_pos;
                        o->rot    = st->drag_prev_rot;
                        if (reader_is_image_path(cpath)) {
                            sol_u32      a = spawn_image_picture(st, board,
                                              vec3_make(0.0f, 0.0f, 0.0f),
                                              quat_identity(), cpath);
                            SceneObject *ao = scene_get(&st->scene, a);
                            if (ao) {
                                float ph = mesh_ref_param("picture", ao->mesh_params,
                                                          ao->mesh_param_count, "h");
                                ao->pos = board_pin_pos(&st->scene, board, a,
                                                        blocal, 0.0f, -0.5f * ph);
                            }
                            st->selected_handle = a;
                            printf("dropped an image picture on the board — the record stays home\n");
                        } else {
                            char        lbuf[16];
                            const char *nm = object_label(&st->scene, st->drag_handle, lbuf);
                            Mesh        empty;
                            vec3        one = vec3_make(1.0f, 1.0f, 1.0f);
                            float       ch  = mesh_ref_param("card", (const float *)0, 0, "h");
                            sol_u32     a;
                            memset(&empty, 0, sizeof empty);
                            a = scene_add(&st->scene, board, empty,
                                          vec3_make(0.0f, 0.0f, 0.0f), quat_identity(), one);
                            scene_kind_set(&st->scene, a, KIND_ALIAS);
                            scene_content_set(&st->scene, a, cpath);
                            scene_meta_set(&st->scene, a, "name", nm);
                            scene_mesh_ref_set(&st->scene, a, "card");
                            {
                                SceneObject *ao = scene_get(&st->scene, a);
                                if (ao) ao->pos = board_pin_pos(&st->scene, board, a,
                                                                blocal, 0.0f, -0.5f * ch);
                            }
                            scene_resolve_meshes(&st->scene);
                            apply_kind_materials(&st->scene);
                            st->selected_handle = a;
                            printf("pinned alias '%s' to the board — the record stays home\n", nm);
                        }
                        o = scene_get(&st->scene, st->drag_handle);  /* re-fetch after scene_add */
                    }
                }
```

- [ ] **Step 2: Build gauntlet**

Run: `./build.sh c89check && ./build.sh && ./build.sh metal`
Expected: all pass. (Behavior: click-drag an image file card onto a board now drops a picture, slidable; it is NOT corner-resizable yet — that's Task 3. A non-image file still pins an alias card.)

- [ ] **Step 3: Commit**

```bash
git add main.c
git commit -m "Image pictures on boards 2/3: drag image file onto a board -> picture

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: Picture-on-board resize (predicate + 3 gates + board-local resize branch)

**Files:**
- Modify: `main.c` — add `picture_on_board` (after `board_is_mounted`, ~line 8460); extend the 3 gates (~9721, ~10067, ~13930); add the board-local resize branch in the resize handler (~line 9844).

Context: `board_is_mounted` returns true only for WALL-mounted pictures/boards (parent has `room_type`); a picture parented to a board returns false, so it currently can't resize. `object_is_board(Scene*, handle)` exists. `resize_corner_pick` already works for any picture (it uses `board_world_corners`) and sets `st->resize_room = o->parent` (the board). `board_resize_corner(anchor, dragged, u, min, aspect, &nw, &nh, &origin)` + the registry-shared-mesh rebuild dance (`mesh_asset_key`/`scene_mesh_params_set`/`asset_release`/`memset(&o->mesh,0,…)`/`scene_resolve_meshes`) are used verbatim by the wall-picture resize. `st->resize_grab` is the no-jump-grab offset.

- [ ] **Step 1: Add the `picture_on_board` predicate**

In `main.c`, find `board_is_mounted` (`static sol_bool board_is_mounted(Scene *s, sol_u32 h) {` ~line 8460) and its closing `}`. Immediately AFTER that function, add:

```c
/* a "picture" parented to a whiteboard (not a wall) — resizable in board-local
   space, the third resizable kind alongside wall-mounts and free notes. */
static sol_bool picture_on_board(Scene *s, sol_u32 h) {
    SceneObject *o = scene_get(s, h);
    return (sol_bool)(o && o->mesh_ref && strcmp(o->mesh_ref, "picture") == 0 &&
                      object_is_board(s, o->parent));
}
```

- [ ] **Step 2: Extend the three resize gates**

(a) The resize-press corner pick (~line 9721):

```c
                } else if (st->selected_handle != 0 &&
                           (board_is_mounted(&st->scene, st->selected_handle) ||
                            note_resizable(&st->scene, st->selected_handle)) &&
                           resize_corner_pick(st, w)) {
```
→
```c
                } else if (st->selected_handle != 0 &&
                           (board_is_mounted(&st->scene, st->selected_handle) ||
                            note_resizable(&st->scene, st->selected_handle) ||
                            picture_on_board(&st->scene, st->selected_handle)) &&
                           resize_corner_pick(st, w)) {
```

(b) The hover gate (~line 10067):

```c
    } else if (st->selected_handle != 0 &&
               (board_is_mounted(&st->scene, st->selected_handle) ||
                note_resizable(&st->scene, st->selected_handle))) {
```
→
```c
    } else if (st->selected_handle != 0 &&
               (board_is_mounted(&st->scene, st->selected_handle) ||
                note_resizable(&st->scene, st->selected_handle) ||
                picture_on_board(&st->scene, st->selected_handle))) {
```

(c) The corner-handle render gate (~line 13930):

```c
        if (state->selected_handle != 0 &&
            (board_is_mounted(&state->scene, state->selected_handle) ||
             note_resizable(&state->scene, state->selected_handle))) {
```
→
```c
        if (state->selected_handle != 0 &&
            (board_is_mounted(&state->scene, state->selected_handle) ||
             note_resizable(&state->scene, state->selected_handle) ||
             picture_on_board(&state->scene, state->selected_handle))) {
```

- [ ] **Step 3: Add the board-local resize branch**

In the resize handler, find the room-vanished check and the wall `else` that follows it (~line 9841):

```c
            } else if (st->resize_room == 0 ||
                       scene_get(&st->scene, st->resize_room) == 0) {
                st->resize_board = 0;                   /* board/room vanished */
            } else {
                Ray      ray = pick_ray(st, w);
                RoomRect r   = editor_room_rect(&st->scene, st->resize_room);
```

Insert a new `else if` branch BETWEEN the room-vanished check and the wall `else` (so it becomes: room-vanished → board-picture [NEW] → wall):

```c
            } else if (st->resize_room == 0 ||
                       scene_get(&st->scene, st->resize_room) == 0) {
                st->resize_board = 0;                   /* board/room vanished */
            } else if (object_is_board(&st->scene, st->resize_room)) {
                /* a picture on a whiteboard: resize on the board face,
                   aspect-locked, board-local, clamped to the board. */
                Ray   ray = pick_ray(st, w);
                float yaw = board_yaw(&st->scene, st->resize_board);
                vec3  n   = vec3_make((float)sin((double)yaw), 0.0f, (float)cos((double)yaw));
                float pt  = mesh_ref_param("picture", o->mesh_params, o->mesh_param_count, "t");
                float tt;
                if (ray_vs_plane(ray, st->resize_anchor, n, &tt) && tt > 0.0f) {
                    vec3         hit = vec3_add(vec3_add(ray.origin, vec3_scale(ray.dir, tt)),
                                                st->resize_grab);   /* no jump on grab */
                    SceneObject *par = scene_get(&st->scene, st->resize_room);
                    float cw  = mesh_ref_param("picture", o->mesh_params, o->mesh_param_count, "w");
                    float ch  = mesh_ref_param("picture", o->mesh_params, o->mesh_param_count, "h");
                    float bw  = par ? mesh_ref_param("board", par->mesh_params, par->mesh_param_count, "w") : 1.8f;
                    float bh  = par ? mesh_ref_param("board", par->mesh_params, par->mesh_param_count, "h") : 1.2f;
                    float aspect = (ch > 0.0f) ? cw / ch : 0.0f;
                    vec3  origin;
                    float nw, nh, p3[3];
                    char  oldkey[160];
                    sol_bool keyed;
                    board_resize_corner(st->resize_anchor, hit, st->resize_u,
                                        0.3f, aspect, &nw, &nh, &origin);
                    if (nw > bw) { nw = bw; if (aspect > 0.0f) nh = nw / aspect; }  /* fit the board */
                    if (nh > bh) { nh = bh; if (aspect > 0.0f) nw = nh * aspect; }
                    p3[0] = nw; p3[1] = nh; p3[2] = pt;
                    keyed = mesh_asset_key(o, oldkey);   /* registry-shared rebuild (P4 item 4) */
                    scene_mesh_params_set(&st->scene, st->resize_board, p3, 3);
                    if (keyed) asset_release(&g_mesh_assets, oldkey);
                    o = scene_get(&st->scene, st->resize_board);
                    if (o) {
                        vec3  lp;
                        float lx = bw * 0.5f - nw * 0.5f;
                        memset(&o->mesh, 0, sizeof o->mesh);     /* drop the borrow */
                        lp = scene_world_to_local(&st->scene, o->parent, origin);
                        if (lx < 0.0f) lx = 0.0f;                /* clamp to the board face */
                        if (lp.x >  lx) lp.x =  lx;
                        if (lp.x < -lx) lp.x = -lx;
                        if (lp.y < 0.0f)        lp.y = 0.0f;
                        if (lp.y > bh - nh)     lp.y = bh - nh;
                        o->pos = lp;
                    }
                    scene_resolve_meshes(&st->scene);
                }
            } else {
                Ray      ray = pick_ray(st, w);
                RoomRect r   = editor_room_rect(&st->scene, st->resize_room);
```

- [ ] **Step 4: Build gauntlet**

Run: `./build.sh c89check && ./build.sh && ./build.sh metal`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add main.c
git commit -m "Image pictures on boards 3/3: board-local picture resize

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Final verification (after all tasks)

- [ ] **Full gauntlet:** `./build.sh c89check && ./build.sh && ./build.sh metal` — all pass.
- [ ] **Human live-verify (GUI — the implementer cannot do this; hand back to Fran):**
  1. **Click-drag** an image file card onto a board → a resizable image picture appears; the file tablet returns to its shelf.
  2. **Carry (E)** an image onto a board → same result.
  3. Select the board picture → **corner handles** show; **corner-drag** resizes it aspect-locked and it stays on the board; **body-drag** still slides it.
  4. **Save/reload** (or revisit the workspace) → the board picture persists; the file record is still home in its room.
  5. A **non-image** file card dragged onto a board still pins as a filename alias card (unchanged).
  6. Wall pictures + furniture filing still behave as before.

---

## Self-Review notes (plan author)

- **Spec coverage:** Part 1 — `spawn_image_picture` + carry refactor (T1), drag image→picture (T2). Part 2 — `picture_on_board` + 3 gates + board-local resize branch (T3). Testing (gauntlet incl. c89check + live-verify) covered.
- **Type consistency:** `spawn_image_picture(AppState*, sol_u32 parent, vec3 pos, quat rot, const char *content) -> sol_u32` defined in T1, called identically in T1 (carry) and T2 (drag). `picture_on_board(Scene*, sol_u32)` defined in T3, used in all 3 gates. `board_resize_corner`/`board_pin_pos`/`object_is_board`/`mesh_ref_param` signatures match their existing uses.
- **Build-clean intermediates:** T1 is behavior-preserving; T2 leaves board pictures slidable-but-not-resizable (sensible); T3 completes resize. Each builds.
- **No new shader** → no MSL twin. `c89check` is in every task's gauntlet (the board-view-regression lesson).
