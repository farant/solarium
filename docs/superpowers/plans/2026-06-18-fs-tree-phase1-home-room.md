# Spatial Filesystem Tree — Phase 1: Floating Home Room Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A fresh `scene.stml` starts as a single floating "home" room, and the player spawns standing inside it.

**Architecture:** Replace the old P3 demo-scene builder with a `populate_home_scene` that builds one open-topped parametric room (the existing `"room"` mesh-ref) anchored high in the sky, with the demo handles nulled. Spawn the camera in whatever room is tagged `room_type="home"` (falling back to today's spawn if none — so loading an old scene still works).

**Tech Stack:** Strict C89, the existing parametric `make_room`/mesh-registry + scene-builder helpers (`scene_add`, `scene_meta_set`, `scene_mesh_ref_set`, `scene_mesh_params_set`, `scene_material_set`, `scene_resolve_meshes`).

**Spec:** `docs/superpowers/specs/2026-06-18-spatial-filesystem-tree-design.md` (Phase 1 of the phasing). This is the foundation phase — small on purpose; it de-risks the fresh-scene-default + spawn-in-home changes that every later phase builds on.

**Conventions:** strict C89 (the c89check gate compiles main.c with `-std=c89 -pedantic-errors -Werror`): declarations at top of block, `/* */` only, no mixed decl/code. Commit on a branch, end messages with the `Co-Authored-By: Claude Opus 4.8 (1M context)` line. Never stage `NOTES.stml` or `paper-picture.png`.

**Testing note:** this is a scene-construction + spawn change with no cleanly-isolatable pure unit, so — consistent with the engine's practice — verification is the **build gauntlet + live-verify** (deferred to the human, who also archives their scene). No new headless test.

**Key facts (verified):**
- `populate_default_scene` is main.c:7791–8002; it builds the demo (floor/box/page + hall/cell/folly rooms), calls `scene_resolve_meshes` at 7933, sets `floor_handle` at 7935.
- It's called once, in the `else` of `if (load_palace(state))` (main.c:8469). Replacing/renaming it avoids a `-Wunused-function` error.
- The demo handles `box_handle`/`page_handle`/`anchor_handle` are referenced at main.c:3948 (page click), 6615 (drag), 8558–8571 (`adopt_legacy_motion`) — all guarded against an invalid handle EXCEPT the init-tail print at 8523–8525 (`scene_get(box_handle)->rel_count`), which must be guarded.
- `room` mesh params are `[w, d, h, wall_n, wall_e, wall_s, wall_w, ceil]` (1.0 = present). Origin = floor center, interior at world-Y of the anchor.
- Spawn is a hardcoded `camera_init` at main.c:8478.

---

## Task 1: Home scene + spawn-in-home

All changes in `main.c`. A subagent CANNOT interactively test the GUI; verification is `./build.sh c89check && ./build.sh debug && ./build.sh metal` (all pass). Do not run `./solarium` interactively.

**Files:**
- Modify: `main.c` (rename+rewrite `populate_default_scene` → `populate_home_scene`; update its call site; guard the init-tail box print; replace the spawn)

- [ ] **Step 1: Replace `populate_default_scene` (main.c:7791–8002) with `populate_home_scene`**

Delete the entire body of `populate_default_scene` (lines 7791–8002, the function from `static void populate_default_scene(AppState *state) {` through its closing `}` at 8002) and replace it with:

```c
#define HOME_FLOOR_Y 12.0f   /* the home room floats this high in the sky */

/* Build a FRESH scene: one floating "home" room you spawn into — the hub the
   filesystem-tree roots will hang off (later phases). Replaces the old P3 demo
   palace, which is preserved in git history. */
static void populate_home_scene(AppState *state) {
    Mesh     empty = {0};
    sol_u32  home, shell;
    float    home_p[8];
    Material stone = material_default();

    scene_init(&state->scene);

    /* the home scene has no demo box/page/anchor — null the handles so the
       guarded references elsewhere all no-op */
    state->box_handle    = 0;
    state->page_handle   = 0;
    state->anchor_handle = 0;
    state->floor_handle  = 0;

    /* an open-topped 8x8 parametric room (floor + 4 walls, no ceiling) anchored
       high up, so it reads as a platform floating in the sky */
    home_p[0] = 8.0f;  home_p[1] = 8.0f;  home_p[2] = 3.0f;
    home_p[3] = 1.0f;  home_p[4] = 1.0f;  home_p[5] = 1.0f;  home_p[6] = 1.0f;
    home_p[7] = 0.0f;

    home = scene_add(&state->scene, 0, empty,
              vec3_make(0.0f, HOME_FLOOR_Y, 0.0f), quat_identity(),
              vec3_make(1.0f, 1.0f, 1.0f));
    scene_meta_set(&state->scene, home, "room_type", "home");
    scene_meta_set(&state->scene, home, "name", "home");

    shell = scene_add(&state->scene, home, empty,
              vec3_make(0.0f, 0.0f, 0.0f), quat_identity(),
              vec3_make(1.0f, 1.0f, 1.0f));
    scene_mesh_ref_set(&state->scene, shell, "room");
    scene_mesh_params_set(&state->scene, shell, home_p, 8);

    stone.base_color = vec3_make(0.58f, 0.55f, 0.50f);
    stone.roughness  = 0.92f;
    scene_material_set(&state->scene, shell, stone);

    scene_resolve_meshes(&state->scene);
}
```

- [ ] **Step 2: Update the call site (main.c:8469)**

Change:
```c
        populate_default_scene(state);
```
to:
```c
        populate_home_scene(state);
```
(The surrounding `collide_rebuild` / `meadow_rebuild` / `forest_rebuild` / `adopt_legacy_motion` calls stay — they no-op cleanly with no terrain and null demo handles.)

- [ ] **Step 3: Guard the init-tail box print (main.c:8522–8525)**

The print dereferences `scene_get(box_handle)->rel_count`, which crashes when there's no box (the home scene, or any loaded scene without "the box"). Wrap it:

Change:
```c
    printf("box meta: title=\"%s\", author=\"%s\"; %u relations\n",
           scene_meta_get(&state->scene, state->box_handle, "title"),
           scene_meta_get(&state->scene, state->box_handle, "author"),
           (unsigned)scene_get(&state->scene, state->box_handle)->rel_count);
```
to:
```c
    if (state->box_handle) {
        printf("box meta: title=\"%s\", author=\"%s\"; %u relations\n",
               scene_meta_get(&state->scene, state->box_handle, "title"),
               scene_meta_get(&state->scene, state->box_handle, "author"),
               (unsigned)scene_get(&state->scene, state->box_handle)->rel_count);
    }
```

- [ ] **Step 4: Spawn in the home room (replace main.c:8478–8479)**

Change the hardcoded spawn:
```c
    camera_init(&state->camera, vec3_make(0.0f, CAMERA_EYE_HEIGHT, 5.0f),
                sol_radians(-90.0f), sol_radians(-10.0f));
```
to a find-the-home-room spawn with the old position as fallback:
```c
    {
        vec3 spawn = vec3_make(0.0f, CAMERA_EYE_HEIGHT, 5.0f);   /* fallback */
        int  i;
        for (i = 0; i < (int)state->scene.count; i++) {
            const char *rt = scene_meta_get(&state->scene,
                                 state->scene.objects[i].handle, "room_type");
            if (rt && strcmp(rt, "home") == 0) {
                vec3 c = object_world_pos(&state->scene,
                             state->scene.objects[i].handle);
                spawn = vec3_make(c.x, c.y + CAMERA_EYE_HEIGHT, c.z + 2.0f);
                break;
            }
        }
        camera_init(&state->camera, spawn, sol_radians(-90.0f), sol_radians(-10.0f));
    }
```
(If `state->scene.count` is a `sol_u32`, the `(int)` cast keeps `-Wsign-compare` quiet; if it's already `int`, the cast is harmless. `object_world_pos`, `scene_meta_get`, `strcmp` are all already used in this file.)

- [ ] **Step 5: Build the full gauntlet**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: `c89check: PASS`, `built ./solarium (debug)`, `built ./solarium-metal ...` — all succeed.

If c89check flags an unused function, confirm `populate_default_scene` was fully replaced (not left alongside the new one). If it flags a sign-compare, match the loop index type to `scene.count`.

- [ ] **Step 6: (Interactive verify — DEFERRED TO HUMAN, do not attempt.)**

- [ ] **Step 7: Commit**

```bash
git add main.c
git commit -m "feat: fs-tree phase 1 — floating home room + spawn-in-home" -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Final verification (human, live)

This is also the moment to **start your fresh scene** (the thing you originally wanted):

1. **Archive your current scene** so the fresh default builds: rename `scene.stml` → e.g. `scene-archive-2026-06-18.stml` (keeps all your accumulated content; nothing is lost — `scene.stml` is gitignored and yours).
2. Run `./solarium`.
   - [ ] You spawn **standing inside a room floating in the sky** (open to the skybox above), not the old demo palace.
   - [ ] You can walk around the floor; the 4 walls keep you from walking off the edge.
   - [ ] No crash on startup (the box-print guard holds with no demo box).
3. Run `./solarium-metal` → same floating home room (no MSL twin added, so parity is expected).
4. To get your old world back at any time: quit, rename your archive back to `scene.stml`. (Loading an old scene still spawns at the fallback position — the home-room finder simply doesn't match, so nothing breaks.)

## Self-review (writing-plans)

- **Spec coverage (Phase 1 slice):** fresh-scene-is-home-room (Step 1 + 2), floating room shell (Step 1, `"room"` mesh elevated to `HOME_FLOOR_Y`, open top), spawn-in-home (Step 4), archive-the-old-scene (live-verify step 1). Walkways/roots/descent/rescan are later phases, explicitly out of scope.
- **Placeholders:** none — all code concrete; every helper (`scene_init`, `scene_add`, `scene_meta_set`, `scene_mesh_ref_set`, `scene_mesh_params_set`, `scene_material_set`, `scene_resolve_meshes`, `material_default`, `vec3_make`, `quat_identity`, `object_world_pos`, `scene_meta_get`, `strcmp`) is already used in main.c.
- **Consistency:** `populate_home_scene` is the only name introduced and is referenced exactly once (the call site); `room_type="home"` is written in Step 1 and read in Step 4; `HOME_FLOOR_Y` defined and used in Step 1.
- **Safety:** the only unguarded demo-handle deref (the box print) is guarded in Step 3; all other demo-handle references were verified to already no-op on a null handle.
