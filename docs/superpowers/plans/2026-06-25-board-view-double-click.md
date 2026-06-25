# Board-View Double-Click Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** In board view, double-click the empty board to create a note at the cursor and immediately edit it, and double-click a note to edit it.

**Architecture:** Factor the `N`-key note-spawn into a `spawn_note` helper; add lightweight double-click detection (press time + position) to the board-view press handler as a new first dispatch arm — double-click a note → edit; double-click the empty board → create + edit; double-click a non-note card → nothing. All in `main.c`.

**Tech Stack:** C89 (strict: `-Wall -Wextra -Werror -pedantic`), GLFW input, OpenGL + Metal. **No new shader** → no MSL twin. Spec: `docs/superpowers/specs/2026-06-25-board-view-double-click-design.md`.

**Conventions the implementer MUST follow:**
- Strict C89: declarations at the TOP of each block.
- Commit on the current branch, message body ending with:
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`
- Never `git add` `NOTES.stml` or `paper-picture.png`. Stage only `main.c`.
- **Build gauntlet after EVERY task — ALL THREE:** `./build.sh c89check` (expect `c89check: PASS — all sources are C89-pedantic clean`), `./build.sh` (expect `built ./solarium (debug)`), `./build.sh metal` (expect `built ./solarium-metal ...`).
- Find anchors by quoted CODE TEXT (line numbers approximate).

---

## File Touch List

- `main.c` only:
  - `spawn_note` helper (before `read_input`) + the `N`-key one-liner. (Task 1)
  - `AppState` fields + 2 constants; forward-decl `note_edit_begin`; the double-click branch in the board-view press handler. (Task 2)

---

### Task 1: Factor `spawn_note` out of the `N` key (behavior-preserving)

**Files:**
- Modify: `main.c` — add `spawn_note` immediately before `static void read_input(...)` (~line 9505); replace the `N`-key block body (~line 10344) with a call.

Context: `board_under_ray`, `pick_ray`, `board_pin_pos`, `mint_ground`, `camera_forward`, `NOTE_CARD_W/H/T`, and the `scene_*` helpers are all defined/declared before `read_input`, so the helper compiles there. `atan2f` is used verbatim from the current `N` code (it's already in main.c and passes c89check).

- [ ] **Step 1: Add the `spawn_note` helper**

In `main.c`, immediately BEFORE the line `static void read_input(GLFWwindow *w, CameraInput *in, double dt, AppState *st) {` (~line 9505), insert:

```c
/* Spawn a KIND_NOTE "card": pinned to the board under the cursor, else on the
   floor ahead. Tags the active workspace, selects it, saves. Returns the handle.
   Shared by the N key and the board-view double-click. */
static sol_u32 spawn_note(AppState *st, GLFWwindow *w) {
    Mesh    empty;
    vec3    one = vec3_make(1.0f, 1.0f, 1.0f);
    vec3    blocal;
    sol_u32 board, h;
    memset(&empty, 0, sizeof empty);
    blocal = vec3_make(0.0f, 0.0f, 0.0f);
    board  = board_under_ray(st, pick_ray(st, w), &blocal);
    if (board != 0) {
        h = scene_add(&st->scene, board, empty,
                      vec3_make(0.0f, 0.0f, 0.0f), quat_identity(), one);
    } else {
        vec3  f = camera_forward(&st->camera);
        vec3  pos;
        float yaw;
        f.y = 0.0f;
        if (vec3_dot(f, f) < 1e-6f) f = vec3_make(0.0f, 0.0f, -1.0f);
        f   = vec3_normalize(f);
        pos = vec3_add(st->camera.pos, vec3_scale(f, 1.8f));
        pos.y = mint_ground(st, pos);  /* bottom-origin card on YOUR ground */
        yaw = atan2f(-f.x, -f.z);       /* facing you */
        h = scene_add(&st->scene, 0, empty, pos,
                      quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), yaw), one);
    }
    scene_kind_set(&st->scene, h, KIND_NOTE);
    scene_meta_set(&st->scene, h, "name", "note");
    scene_meta_set(&st->scene, h, "workspace",
                   st->scene.active_ws[0] ? st->scene.active_ws : "home");
    scene_meta_set(&st->scene, h, "text", "press Enter to edit me");
    scene_mesh_ref_set(&st->scene, h, "card");
    {   /* notes get a roomy landscape card + a matching min height */
        float np3[3];
        char  mhb[16];
        np3[0] = NOTE_CARD_W; np3[1] = NOTE_CARD_H; np3[2] = NOTE_CARD_T;
        scene_mesh_params_set(&st->scene, h, np3, 3);
        snprintf(mhb, sizeof mhb, "%.4f", (double)NOTE_CARD_H);
        scene_meta_set(&st->scene, h, "min_h", mhb);
    }
    if (board != 0) {
        SceneObject *no = scene_get(&st->scene, h);
        if (no) no->pos = board_pin_pos(&st->scene, board, h,
                                        blocal, 0.0f, -0.5f * NOTE_CARD_H);
    }
    scene_resolve_meshes(&st->scene);
    apply_kind_materials(&st->scene);
    st->selected_handle = h;
    scene_save(&st->scene, "scene.stml");
    printf("note #%u spawned%s\n", (unsigned)h, board ? " on the board" : "");
    return h;
}
```

- [ ] **Step 2: Replace the `N`-key block body with the call**

Find the `N`-key block (~line 10344):

```c
        sol_bool n_now = glfwGetKey(w, GLFW_KEY_N) == GLFW_PRESS;
        if (n_now && !st->n_was_down) {
            Mesh    empty;
            vec3    one = vec3_make(1.0f, 1.0f, 1.0f);
            vec3    blocal;
            sol_u32 board, h;
            memset(&empty, 0, sizeof empty);
            blocal = vec3_make(0.0f, 0.0f, 0.0f);
            board  = board_under_ray(st, pick_ray(st, w), &blocal);
```

…through its end:

```c
            scene_resolve_meshes(&st->scene);
            apply_kind_materials(&st->scene);
            st->selected_handle = h;
            scene_save(&st->scene, "scene.stml");
            printf("note #%u spawned%s\n", (unsigned)h,
                   board ? " on the board" : "");
        }
        st->n_was_down = n_now;
```

Replace that entire block (the `sol_bool n_now ...` line through `st->n_was_down = n_now;`) with:

```c
        sol_bool n_now = glfwGetKey(w, GLFW_KEY_N) == GLFW_PRESS;
        if (n_now && !st->n_was_down)
            (void)spawn_note(st, w);
        st->n_was_down = n_now;
```

(The enclosing `{ ... }` braces around the `N` block stay; only the body between them changes.)

- [ ] **Step 3: Build gauntlet**

Run: `./build.sh c89check && ./build.sh && ./build.sh metal`
Expected: `c89check: PASS`, `built ./solarium (debug)`, `built ./solarium-metal ...`. (Pure refactor — `N` still spawns a note exactly as before.)

- [ ] **Step 4: Commit**

```bash
git add main.c
git commit -m "Board-view double-click 1/2: factor spawn_note out of the N key

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Double-click detection + the create/edit branch

**Files:**
- Modify: `main.c` — `AppState` fields (near `board_view` ~line 2815) + 2 constants; a forward-declaration of `note_edit_begin` (before `read_input`); the board-view press handler (the `} else if (fp) {` branch).

Context: `note_edit_begin(AppState*, sol_u32)` is defined at ~line 14684, AFTER `read_input`, so a forward declaration is required to call it from the press handler. `scene_meta_set` is in `scene.h`. `glfwGetTime()` returns seconds (double). `fabs` is C89 (`<math.h>`, already used in main.c). `mx`/`my` (cursor pos, double) are already in scope in the press handler.

- [ ] **Step 1: Add the constants**

Near the `BOARD_VIEW_*` constants (search for `#define BOARD_VIEW_GLIDE_S`), add:

```c
#define BOARD_DBL_S   0.35   /* max seconds between the two clicks of a double-click */
#define BOARD_DBL_PX  6.0    /* max cursor drift (px) between them */
```

- [ ] **Step 2: Add the AppState fields**

In `AppState`, find `sol_u32  board_view;` (~line 2815). After the board-view field group (anywhere in `AppState` is fine; placing it right after `sol_u32  board_view;` keeps it together), add:

```c
    double   last_press_t, last_press_x, last_press_y;  /* board-view double-click detect */
```

(`AppState state = {0}` zero-inits these — `last_press_t = 0.0` means the first press is never a double-click. No explicit init needed.)

- [ ] **Step 3: Forward-declare `note_edit_begin`**

Immediately BEFORE `static void read_input(GLFWwindow *w, CameraInput *in, double dt, AppState *st) {` (and after `spawn_note` from Task 1), add the prototype:

```c
static void note_edit_begin(AppState *st, sol_u32 handle);   /* defined later; called by the board-view double-click */
```

- [ ] **Step 4: Add the double-click branch in the press handler**

In `read_input`'s first-person press branch (`} else if (fp) {`), find the deselect line followed by the dispatch:

```c
                do_pick(st, w, pnx, pny);               /* select on press */
                if (st->board_view != 0 && st->selected_handle == st->board_view)
                    st->selected_handle = 0;            /* clicked the board itself = deselect */
                if (try_connect(st, st->selected_handle)) {
```

Replace that (keeping `do_pick` and the deselect, changing the `if (try_connect` opener) with:

```c
                do_pick(st, w, pnx, pny);               /* select on press */
                if (st->board_view != 0 && st->selected_handle == st->board_view)
                    st->selected_handle = 0;            /* clicked the board itself = deselect */
                if (st->board_view != 0) {              /* board-view double-click detect */
                    double now = glfwGetTime();
                    is_dbl = (sol_bool)(now - st->last_press_t < BOARD_DBL_S &&
                                        fabs(mx - st->last_press_x) < BOARD_DBL_PX &&
                                        fabs(my - st->last_press_y) < BOARD_DBL_PX);
                    st->last_press_t = now;
                    st->last_press_x = mx; st->last_press_y = my;
                    if (is_dbl) st->last_press_t = 0.0;   /* consume: a 3rd click isn't a 2nd double */
                }
                if (is_dbl) {                           /* edit a note, or create+edit on the empty board */
                    SceneObject *so = scene_get(&st->scene, st->selected_handle);
                    if (so && so->kind == KIND_NOTE) {
                        note_edit_begin(st, st->selected_handle);
                    } else if (st->selected_handle == 0) {
                        vec3 bl;
                        if (board_under_ray(st, pick_ray(st, w), &bl) != 0) {  /* over the board only */
                            sol_u32 nh = spawn_note(st, w);
                            scene_meta_set(&st->scene, nh, "text", "");        /* type into it empty */
                            note_edit_begin(st, nh);
                        }
                    }
                    /* else: double-click a non-note card -> nothing (the select stands) */
                } else if (try_connect(st, st->selected_handle)) {
```

Then add the `is_dbl` declaration to the top of the `} else if (fp) {` block. Find:

```c
            } else if (fp) {
                float pnx = 0.0f, pny = 0.0f;           /* crosshair by default */
```

Change to:

```c
            } else if (fp) {
                float pnx = 0.0f, pny = 0.0f;           /* crosshair by default */
                sol_bool is_dbl = SOL_FALSE;
```

(The rest of the dispatch — `} else if (… resize_corner_pick …)`, `} else if (… picture_move_pick)`, `} else if (… drag_begin)` — is unchanged; the new `if (is_dbl) { … } else if (try_connect …)` simply prepends one arm. The braces stay balanced: the `if (is_dbl) { … }` closes, then `else if (try_connect …) {` continues the existing chain.)

- [ ] **Step 5: Build gauntlet**

Run: `./build.sh c89check && ./build.sh && ./build.sh metal`
Expected: all pass.

- [ ] **Step 6: Commit**

```bash
git add main.c
git commit -m "Board-view double-click 2/2: create+edit / edit a note on double-click

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Final verification (after all tasks)

- [ ] **Full gauntlet:** `./build.sh c89check && ./build.sh && ./build.sh metal` — all pass.
- [ ] **Human live-verify (GUI — the implementer cannot do this; hand back to Fran):**
  1. In board view, **double-click the empty board** → a new note appears at the cursor and you're typing in it immediately (empty, no "press Enter to edit me" placeholder).
  2. **Double-click a note** → it opens for editing.
  3. **Single-click** still selects a card / deselects on the empty board; **drag** still slides/resizes; a double-click does NOT also start a drag.
  4. **Double-click an image/alias card** → nothing special (it's just selected).
  5. `N` still spawns a note; **outside board view**, double-click does nothing new.

---

## Self-Review notes (plan author)

- **Spec coverage:** `spawn_note` factor + `N` one-liner (T1); constants + fields + forward-decl + the double-click branch with note/empty/non-note cases + the over-board gate + the triple-click guard (T2). Testing (gauntlet incl. c89check + live-verify) covered.
- **Type consistency:** `spawn_note(AppState*, GLFWwindow*) -> sol_u32` defined in T1, called in T1 (`N`) and T2 (double-click). `note_edit_begin(AppState*, sol_u32)` forward-declared in T2, matches its definition. `last_press_t/x/y` (double) declared in T2, used in the same task. `is_dbl` (sol_bool) declared + used in the FP block.
- **Build-clean intermediates:** T1 is behavior-preserving; T2 adds the feature. Each builds (incl. c89check).
- **No new shader** → no MSL twin. `c89check` in every task's gauntlet.
