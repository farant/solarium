# Place Windows — Implementation Plan (Phase 1: Core)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Place rectangular windows in flat room walls — a command drops a framed, see-through window on the wall ahead; it's selectable, draggable, resizable, deletable, takes a colored glass pane (default: open hole).

**Architecture:** Approach A — a window is a normal `SceneObject` (`mesh_ref="window"`, `KIND_PLAIN`) parented to its room; it stores its size in `mesh_params` and its wall in `meta["wall"]`. The room's wall-rebuild *reads* the room's child windows and cuts a rectangular hole per window (the doorway "build around the gap" machinery, extended with a sill panel). Persistence is free (windows are normal objects). The frame is opaque dark_wood; the optional glass pane is a child object drawn in the existing sorted transparent pass.

**Tech Stack:** C89, OpenGL + Metal dual backend, the project's mesh registry + scene/meta system, GLFW input. Reuses `emit_doored_wall`, `editor_room_rect`, `descend_wall_mount`, `scene_world_to_local`, `draw_glass`, the registry-rebuild law, and `delete_board_card`.

---

## Testing approach (read first)

This is mesh + GUI integration in a 660 KB `main.c` engine. **Each task's automated gate is the three-target build gauntlet.** The one cleanly pure-logic piece — the wall sill panel — gets a real headless assertion in the existing `routetest` target (which already links `mesh.c` and is about `RoomOpening`). Everything interactive is human-verified after merge (subagents can't GUI-test).

**The gauntlet (run after every task):**
```bash
./build.sh c89check   # Expected: "c89check: PASS — all sources are C89-pedantic clean"
./build.sh            # Expected: "built ./solarium (debug)"
./build.sh metal      # Expected: "built ./solarium-metal (stage a: links clean, zero GL; runs from stage b)"
```

**C89 reminders (the gauntlet enforces them):** declare locals at the top of their block; `/* block comments */` only; cast like the project (`(sol_bool)`, `(float)`); no decl-after-statement.

**Model note:** Task 5 (interaction reuse + the wall-rebuild coupling) is the integration-heavy task — use a capable model and review it carefully. Tasks 1–4, 6, 7 are mechanical.

---

## Task 1: The sill panel — `RoomOpening.sill` + `emit_doored_wall`

A door reaches the floor (3-way split: left pier, right pier, header). A window is a hole in the middle of the wall and needs a 4th panel — a **sill** below it. Add a `sill` field to `RoomOpening` (0 = door, unchanged; >0 = window) and emit the sill box.

**Files:**
- Modify: `mesh.h` (RoomOpening struct, ~line 62), `mesh.c` (`emit_doored_wall`, lines 270-312), `route.c` (`route_room_openings_in`, ~line 330).
- Test: `route_test.c` (extends the existing `routetest` target).

- [ ] **Step 1: Add the `sill` field**

In `mesh.h`, the `RoomOpening` struct (line 62):
```c
typedef struct {
    int     wall;
    sol_f32 center;
    sol_f32 width;
    sol_f32 height;    /* top of the opening (lintel) above the floor */
    sol_f32 sill;      /* bottom of the opening above the floor; 0 = door (reaches floor) */
} RoomOpening;
```

- [ ] **Step 2: Emit the sill panel in `emit_doored_wall`**

In `mesh.c`, `emit_doored_wall` (lines 270-312). Add a `sl[]` array alongside `lo/hi/oy`, populate + sort it, and emit a sill box. Full replacement of the function body:

```c
static void emit_doored_wall(MeshBuilder *b, int runx, sol_f32 f0, sol_f32 f1,
                             sol_f32 s0, sol_f32 s1, sol_f32 h,
                             const RoomOpening *ops, int n_ops, int wall_id) {
    sol_f32 lo[ROOM_MAX_OPENINGS_PER_WALL];
    sol_f32 hi[ROOM_MAX_OPENINGS_PER_WALL];
    sol_f32 oy[ROOM_MAX_OPENINGS_PER_WALL];
    sol_f32 sl[ROOM_MAX_OPENINGS_PER_WALL];   /* sill (bottom) per opening; 0 = door */
    int     k = 0, i, j;
    sol_f32 cur;
    for (i = 0; i < n_ops; i++) {
        sol_f32 c, hwid;
        if (ops[i].wall != wall_id) continue;
        if (k >= ROOM_MAX_OPENINGS_PER_WALL) break;
        c = ops[i].center; hwid = ops[i].width * 0.5f;
        lo[k] = c - hwid; hi[k] = c + hwid; oy[k] = ops[i].height; sl[k] = ops[i].sill;
        k++;
    }
    for (i = 1; i < k; i++) {                 /* insertion sort by lo */
        sol_f32 pivot_lo = lo[i], pivot_hi = hi[i], pivot_oy = oy[i], pivot_sl = sl[i];
        j = i - 1;
        while (j >= 0 && lo[j] > pivot_lo) {
            lo[j + 1] = lo[j]; hi[j + 1] = hi[j]; oy[j + 1] = oy[j]; sl[j + 1] = sl[j]; j--;
        }
        lo[j + 1] = pivot_lo; hi[j + 1] = pivot_hi; oy[j + 1] = pivot_oy; sl[j + 1] = pivot_sl;
    }
    cur = s0;
    for (i = 0; i <= k; i++) {
        sol_f32 gL = (i < k) ? lo[i] : s1;
        sol_f32 gR = (i < k) ? hi[i] : s1;
        if (gL < s0) gL = s0;
        if (gR > s1) gR = s1;
        if (gL > cur) {                        /* solid pier [cur, gL] */
            if (runx) aabb_box(b, cur, gL, 0.0f, h, f0, f1);
            else      aabb_box(b, f0, f1, 0.0f, h, cur, gL);
        }
        if (i < k) {
            if (sl[i] > 0.0f) {                /* sill below the gap (windows) */
                if (runx) aabb_box(b, gL, gR, 0.0f, sl[i], f0, f1);
                else      aabb_box(b, f0, f1, 0.0f, sl[i], gL, gR);
            }
            if (oy[i] < h) {                   /* header above the gap */
                if (runx) aabb_box(b, gL, gR, oy[i], h, f0, f1);
                else      aabb_box(b, f0, f1, oy[i], h, gL, gR);
            }
            cur = gR;
        }
    }
}
```

- [ ] **Step 3: Zero the sill for route-derived doors**

In `route.c`, `route_room_openings_in` (~line 330), wherever it fills an `out[i]` opening's `.wall/.center/.width/.height`, add `out[i].sill = 0.0f;` (doors reach the floor). Find each `out[...]` assignment block and set `.sill = 0.0f` so the new field is always initialized (don't rely on stack garbage).

- [ ] **Step 4: Write the failing headless test**

In `route_test.c`, add a test that builds a room shell with a window opening and asserts it has *more* geometry than a door-only opening (the sill panel). Add near the other tests, and call it from `main`:

```c
static void test_window_sill_panel(void) {
    MeshBuilder mb;
    RoomOpening door, win;
    int door_idx, win_idx;

    /* a door: full-height opening, sill 0 */
    door.wall = ROOM_WALL_N; door.center = 0.0f; door.width = 1.4f;
    door.height = 2.1f; door.sill = 0.0f;
    mb_init(&mb);
    make_room_doored(&mb, 6.0f, 4.0f, 3.0f, 0.20f, 1, 1, 1, 1, 0, &door, 1);
    door_idx = mb.index_count;
    mb_free(&mb);

    /* a window: same wall/center, but a sill > 0 -> an extra sill panel */
    win = door; win.sill = 0.9f; win.height = 2.3f;
    mb_init(&mb);
    make_room_doored(&mb, 6.0f, 4.0f, 3.0f, 0.20f, 1, 1, 1, 1, 0, &win, 1);
    win_idx = mb.index_count;
    mb_free(&mb);

    assert(win_idx > door_idx);   /* the sill box adds faces */
    printf("  window sill panel: door=%d win=%d OK\n", door_idx, win_idx);
}
```
Call `test_window_sill_panel();` from `route_test.c`'s `main` alongside the existing test calls. (`make_room_doored`, `ROOM_WALL_N`, `MeshBuilder`, `mb_init/mb_free` are all declared in `mesh.h`, already included by `route_test.c`'s link set; `assert` needs `#include <assert.h>` if not present, `printf` needs `<stdio.h>`.)

- [ ] **Step 5: Run the test — expect it to FAIL first, then PASS**

```bash
./build.sh routetest && ./route_test
```
Before Step 2 it would not even compile (`sill` undefined); after Steps 1-2 it should print `window sill panel: door=… win=… OK` and the binary exits 0.

- [ ] **Step 6: Build gauntlet**

Run the three gauntlet commands. Expected: all pass (the new field defaults to 0 everywhere doors are built, so existing rooms are unchanged).

- [ ] **Step 7: Commit**
```bash
git add mesh.h mesh.c route.c route_test.c
git commit -m "Windows: RoomOpening.sill + emit_doored_wall sill panel"
```

---

## Task 2: The window meshes — `make_window` + `make_window_glass`

**Files:**
- Modify: `mesh.h` (declarations), `mesh.c` (the two mesh-gens + emit wrappers + REGISTRY rows).

- [ ] **Step 1: Declare the mesh-gens in `mesh.h`**

Near `make_picture`'s declaration in `mesh.h`, add:
```c
void make_window(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 t, sol_f32 fw);
void make_window_glass(MeshBuilder *b, sol_f32 w, sol_f32 h);
```

- [ ] **Step 2: Implement the meshes in `mesh.c`**

After `make_picture` (mesh.c:357), add. The frame is **center-origin** (opening centered at local 0; the window's world transform sits at the hole center), a dark_wood ring around the opening spanning the wall thickness, with a small interior sill ledge:
```c
/* A window assembly's FRAME (Place Windows, Phase 1): a rectangular dark_wood
   ring around a centered opening [-w/2,w/2] x [-h/2,h/2], spanning the wall
   thickness in z [-t/2,t/2], border width fw, plus a modest interior sill
   ledge. Center-origin so the object's transform sits at the hole center.
   The glass pane (make_window_glass) is a separate object. */
void make_window(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 t, sol_f32 fw) {
    sol_f32 hw = w * 0.5f, hh = h * 0.5f, ht = t * 0.5f;
    if (fw < 0.01f) fw = 0.01f;
    aabb_box(b, -hw - fw, -hw,      -hh - fw, hh + fw, -ht, ht);  /* left stile  */
    aabb_box(b,  hw,       hw + fw, -hh - fw, hh + fw, -ht, ht);  /* right stile */
    aabb_box(b, -hw,       hw,       hh,      hh + fw, -ht, ht);  /* top rail    */
    aabb_box(b, -hw,       hw,      -hh - fw, -hh,     -ht, ht);  /* bottom rail */
    aabb_box(b, -hw - fw,  hw + fw, -hh - fw - 0.03f, -hh - fw, -ht, ht + 0.06f); /* sill ledge */
}

/* A window's GLASS pane: a centered double-readable quad at z=0, drawn on the
   translucent glass pipeline. base_color (material) tints it. */
void make_window_glass(MeshBuilder *b, sol_f32 w, sol_f32 h) {
    sol_f32 hw = w * 0.5f, hh = h * 0.5f;
    sol_u32 v0, v1, v2, v3;
    v0 = mb_push_vertex(b, -hw, -hh, 0.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f);
    v1 = mb_push_vertex(b,  hw, -hh, 0.0f,  0.0f, 0.0f, 1.0f,  1.0f, 0.0f);
    v2 = mb_push_vertex(b,  hw,  hh, 0.0f,  0.0f, 0.0f, 1.0f,  1.0f, 1.0f);
    v3 = mb_push_vertex(b, -hw,  hh, 0.0f,  0.0f, 0.0f, 1.0f,  0.0f, 1.0f);
    mb_push_triangle(b, v0, v1, v2);
    mb_push_triangle(b, v0, v2, v3);
}
```

- [ ] **Step 3: Add emit wrappers + REGISTRY rows**

In `mesh.c`, near `emit_picture` (mesh.c:997):
```c
static void emit_window(MeshBuilder *b, const float *p) { make_window(b, p[0], p[1], p[2], p[3]); }
static void emit_window_glass(MeshBuilder *b, const float *p) { make_window_glass(b, p[0], p[1]); }
```
In the `REGISTRY[]` table (after the `"picture"` row, mesh.c:1184):
```c
    { "window", 4, { "w", "h", "t", "fw" }, { 1.2f, 1.4f, 0.20f, 0.08f }, emit_window },
    { "window_glass", 2, { "w", "h" }, { 1.2f, 1.4f }, emit_window_glass },
```

- [ ] **Step 4: Build gauntlet** — all three pass.

- [ ] **Step 5: Commit**
```bash
git add mesh.h mesh.c
git commit -m "Windows: make_window frame + make_window_glass pane mesh-gens + registry"
```

---

## Task 3: The wall reads its windows — opening gather + per-room rebuild

The room wall-rebuild must include the room's child windows as openings, and a single window change must rebuild just that room.

**Files:**
- Modify: `main.c` — add `window_wall`, `room_append_windows`, `room_rebuild_one` helpers; wire `room_append_windows` into both rebuild blocks (main.c:4984-5008 and 5086-5110); bump the `ops[16]` arrays.

- [ ] **Step 1: Add the helpers**

Place these together **before the first room-rebuild block** (i.e. before main.c:4980; a good spot is just before the function that contains it). `ROOM_WALL_N/E/S/W` are 0/1/2/3 (from mesh.h). `editor_room_rect`, `route_room_openings`, `scene_get`, `mesh_ref_param`, `scene_object_active` are all existing:

```c
#define ROOM_OPENINGS_CAP 32   /* doors + windows per room, across all walls */

/* a window's wall index, from meta["wall"] ("0".."3"); default N. */
static int window_wall(Scene *s, sol_u32 h) {
    const char *m = scene_meta_get(s, h, "wall");
    int v = m ? atoi(m) : 0;
    if (v < 0 || v > 3) v = 0;
    return v;
}

/* append every active child window of `room` to ops[] as a RoomOpening.
   A window is center-origin and parented to the room, so its room-local pos
   gives the along-wall center (x for N/S, z for E/W) and the opening bottom
   (pos.y - h/2). height stores the lintel = sill + h. */
static void room_append_windows(Scene *s, sol_u32 room, RoomOpening *ops,
                                int *no, int max) {
    int i;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        float w, h, sill;
        int   wall;
        if (o->parent != room || !o->mesh_ref ||
            strcmp(o->mesh_ref, "window") != 0) continue;
        if (!scene_object_active(s, o->handle)) continue;
        if (*no >= max) break;
        w    = mesh_ref_param("window", o->mesh_params, o->mesh_param_count, "w");
        h    = mesh_ref_param("window", o->mesh_params, o->mesh_param_count, "h");
        wall = window_wall(s, o->handle);
        sill = o->pos.y - h * 0.5f;
        if (sill < 0.0f) sill = 0.0f;
        ops[*no].wall   = wall;
        ops[*no].center = (wall == ROOM_WALL_N || wall == ROOM_WALL_S) ? o->pos.x : o->pos.z;
        ops[*no].width  = w;
        ops[*no].height = sill + h;
        ops[*no].sill   = sill;
        (*no)++;
    }
}
```

- [ ] **Step 2: Wire windows into both full-rebuild blocks**

In **both** rebuild blocks (main.c:4986 and 5088), change the array declaration `RoomOpening ops[16];` to:
```c
            RoomOpening  ops[ROOM_OPENINGS_CAP];
```
and immediately after each `no = route_room_openings_in(routes, n, s, room->handle, ops, 16);` line (4995 and 5097), add:
```c
            room_append_windows(s, room->handle, ops, &no, ROOM_OPENINGS_CAP);
```
Also change the `, ops, 16)` argument in those `route_room_openings_in` calls to `, ops, ROOM_OPENINGS_CAP)`.

- [ ] **Step 3: Add `room_rebuild_one` for a single window change**

After `room_append_windows`, add a helper that rebuilds exactly one room's shell + frame (used on window place/drag-release/resize-release/delete). It mirrors the inner body of the full-rebuild block but for one room, using the self-contained `route_room_openings` (no `routes` array needed):
```c
/* Rebuild one room's shell mesh + timber frame, re-reading its doors + windows.
   Called when a single window changes (place / move-release / resize-release /
   delete) — cheaper than a full connections_rebuild, and not per-frame. */
static void room_rebuild_one(AppState *st, sol_u32 room) {
    Scene *s = &st->scene;
    int    i;
    for (i = 0; i < s->count; i++) {
        SceneObject *shell = &s->objects[i];
        RoomOpening  ops[ROOM_OPENINGS_CAP];
        int          no;
        float        w, d, h;
        MeshBuilder  mb;
        if (shell->parent != room || !shell->mesh_ref ||
            strcmp(shell->mesh_ref, "room") != 0) continue;
        w = mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "w");
        d = mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "d");
        h = mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "h");
        no = route_room_openings(s, room, ops, ROOM_OPENINGS_CAP);
        room_append_windows(s, room, ops, &no, ROOM_OPENINGS_CAP);
        mesh_destroy(&shell->mesh);
        mb_init(&mb);
        make_room_doored(&mb, w, d, h, ROUTE_WALL_T,
                         mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "wn") > 0.5f,
                         mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "we") > 0.5f,
                         mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "ws") > 0.5f,
                         mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "ww") > 0.5f,
                         (g_dark_wood.albedo_tex.id == 0 &&
                          mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "ceil") > 0.5f),
                         ops, no);
        if (mb.index_count > 0) shell->mesh = mesh_from_builder(&mb);
        mb_free(&mb);
        room_frame_build(shell, ops, no);
        break;   /* one shell per room */
    }
}
```
(`route_room_openings(Scene*, room, out, max)` is the self-contained door query at route.c:356. `atoi` needs `<stdlib.h>` — already included by main.c.)

- [ ] **Step 4: Build gauntlet** — all three pass. (No windows exist yet, so rooms are unchanged.)

- [ ] **Step 5: Commit**
```bash
git add main.c
git commit -m "Windows: room wall reads child windows; per-room rebuild helper"
```

---

## Task 4: Place a window — `cmd_place_window`

**Files:**
- Modify: `main.c` — `cmd_place_window` + the palette row + default constants.

- [ ] **Step 1: Default constants**

Near the top of the window helpers (before `cmd_place_window`):
```c
#define WINDOW_DEF_W    1.2f   /* default opening width  */
#define WINDOW_DEF_H    1.4f   /* default opening height */
#define WINDOW_DEF_SILL 0.9f   /* default sill above the floor */
#define WINDOW_FRAME_W  0.08f  /* dark_wood border width */
```

- [ ] **Step 2: The place command**

Add `cmd_place_window`. It mirrors the carry wall-mount aim at main.c:9428-9445 but creates a window (center-origin, no proud offset → pass `t = 0`), iterating active rooms to find the nearest wall hit:
```c
static void cmd_place_window(AppState *st) {
    Scene  *s = &st->scene;
    Ray     ray;
    sol_u32 best_room = 0, h;
    int     best_wall = 0;
    vec3    best_center;
    float   best_t = 1e30f;
    int     i;
    float   hw = WINDOW_DEF_W * 0.5f + WINDOW_FRAME_W;
    float   hh = WINDOW_DEF_H * 0.5f + WINDOW_FRAME_W;

    ray.origin = st->camera.pos;            /* the camera-forward aim ray (as the wall-mount aim builds it) */
    ray.dir    = camera_forward(&st->camera);

    for (i = 0; i < s->count; i++) {
        SceneObject *room = &s->objects[i];
        const char  *rt;
        RoomRect     r;
        float        ceil_y, tt;
        int          wall;
        vec3         center;
        rt = scene_meta_get(s, room->handle, "room_type");
        if (!rt || (strcmp(rt, "home") != 0 && strcmp(rt, "mirror") != 0)) continue;
        if (!scene_object_active(s, room->handle)) continue;
        r      = editor_room_rect(s, room->handle);
        ceil_y = r.floor_y + room_interior_height(s, room->handle);
        if (!descend_wall_mount(r, ray, ceil_y, hw, hh, 0.0f, &wall, &center)) continue;
        tt = vec3_len(vec3_sub(center, ray.origin));
        if (tt < best_t) { best_t = tt; best_room = room->handle; best_wall = wall; best_center = center; }
    }
    if (best_room == 0) { printf("no wall in front to place a window\n"); return; }

    {
        static const float wyaw[4] = { 0.0f, -90.0f, 180.0f, 90.0f };
        vec3  lp  = scene_world_to_local(s, best_room, best_center);
        quat  rot = quat_from_axis_angle(vec3_make(0.0f,1.0f,0.0f), sol_radians(wyaw[best_wall]));
        vec3  one = vec3_make(1.0f, 1.0f, 1.0f);
        Mesh  empty;
        float p[4]; char wbuf[8];
        memset(&empty, 0, sizeof empty);
        p[0] = WINDOW_DEF_W; p[1] = WINDOW_DEF_H; p[2] = ROUTE_WALL_T; p[3] = WINDOW_FRAME_W;
        h = scene_add(s, best_room, empty, lp, rot, one);    /* parent + empty mesh + scale */
        scene_kind_set(s, h, KIND_PLAIN);
        scene_mesh_ref_set(s, h, "window");
        scene_mesh_params_set(s, h, p, 4);
        snprintf(wbuf, sizeof wbuf, "%d", best_wall);
        scene_meta_set(s, h, "wall", wbuf);
        scene_meta_set(s, h, "glass", "none");
        mint_tag_ws(st, h);
        scene_resolve_meshes(s);     /* build the "window" frame mesh */
        room_rebuild_one(st, best_room);
        st->selected_handle = h;
        scene_save(s, "scene.stml");
        printf("placed window on wall %d\n", best_wall);
    }
}
static sol_bool can_place_window(AppState *st) {
    return (sol_bool)(st->board_view == 0 && st->reader_state == READER_IDLE);
}
```
**Verified signatures (match the source exactly):** `scene_add(Scene*, parent, Mesh, pos, rot, scale)` (scene.h:104 — takes an empty Mesh + parent + scale, NOT a kind/mesh_ref), then `scene_kind_set(s,h,KIND_PLAIN)` (scene.h:135), `scene_mesh_ref_set(s,h,"window")` (used at main.c:7834), `scene_mesh_params_set(s,h,p,n)` (scene.h:130), `scene_resolve_meshes`, then **re-fetch with `scene_get` since `scene_add` may realloc** (main.c:7839 precedent). The camera-forward ray is `ray.origin = st->camera.pos; ray.dir = camera_forward(&st->camera);` (the exact construction at main.c:9409-9410). `editor_room_rect`, `room_interior_height`, `scene_world_to_local`, `vec3_len`, `vec3_sub`, `sol_radians` are all real (verified). This is the same creation idiom as the folderbook spawn at main.c:7830-7840 — copy that pattern.

- [ ] **Step 3: Register the palette command**

In `g_commands[]` (main.c:9759 region), after the "Spawn whiteboard" / window-feature neighbors, add:
```c
    { "Place window",                NULL, 0,          cmd_place_window,      can_place_window,      SOL_FALSE },
```

- [ ] **Step 4: Build gauntlet** — all three pass.

- [ ] **Step 5: Manual smoke (controller may skip — GUI):** placement is visual; the build is the gate. Defer the visual check to live-verify.

- [ ] **Step 6: Commit**
```bash
git add main.c
git commit -m "Windows: Place window command (drop a framed open hole on the wall ahead)"
```

---

## Task 5: Interaction — select / drag / resize / delete + rebuild-on-change

A window reuses the wall-mounted-picture interaction; the one new thing is re-running `room_rebuild_one` when a window's geometry changes. **This task requires reading the existing picture select/move/resize/delete paths and extending their `mesh_ref`-recognition to also accept `"window"`.** Use a capable model; review carefully.

**Files:**
- Modify: `main.c` — the selection/pick, `move_board` drag, `resize_board` corner-resize, and delete paths; the LMB-release handler.

- [ ] **Step 1: Recognize `"window"` everywhere `"picture"` is a draggable/resizable wall object**

Search main.c for the wall-picture interaction predicates and add the window case. Concretely, grep:
```bash
grep -n '"picture"' main.c
```
For each site that is part of **select / move_board / resize_corner / wall-slide** recognition (NOT the image-loading or board-page-tagging sites), broaden `strcmp(o->mesh_ref, "picture") == 0` to also accept `"window"`. A window is a wall object exactly like a picture for these gestures (bottom of the frame is the grab plane, free-aspect resize). Leave content/image-specific sites (texture load, `spawn_image_picture`, board-page tagging) untouched.

- [ ] **Step 2: Free-aspect resize for windows**

Where the resize path calls `board_resize_corner(anchor, dragged, u, min, aspect, ...)`, pass **`aspect = 0.0f`** (free aspect) when the resized object is a window, and a sensible `min` (e.g. `0.3f`). When a window resize updates its `mesh_params`, it must update **both** `w` and `h` (params index 0 and 1) via the registry-rebuild law already used for picture/note resize (capture old key → `scene_mesh_params_set` → `asset_release(old key)` → `memset(&o->mesh,0,...)` → `scene_resolve_meshes`). The window's pos also shifts (corner resize moves the origin): set `o->pos` from the resize `out_origin` exactly as the picture path does — but note the window is **center-origin**, so the resize origin handling must keep the hole centered (the implementer reconciles this with `board_resize_corner`, which assumes bottom-origin; the simplest correct approach: treat the window like a board of size `w+2*fw × h+2*fw` and, after computing the new bottom-origin board, convert back to the window's center by `pos.y = bottom + (h/2 + fw)`).

- [ ] **Step 3: Rebuild the room on release / delete**

Find the LMB-release handler where `st->move_board` and `st->resize_board` are cleared back to 0 (grep `st->move_board = 0` / `st->resize_board = 0`). When the released object's `mesh_ref` is `"window"`, after clearing the drag/resize state call:
```c
            room_rebuild_one(st, scene_get(&st->scene, released_handle)->parent);
            scene_save(&st->scene, "scene.stml");
```
(capture `released_handle` before clearing the state var; guard the `scene_get` for NULL). During the drag itself the frame object moves live (cheap); the **wall hole** updates only here, on release — per the editor perf lesson (no heavy rebuild per drag frame).

For **delete**: `delete_board_card` already removes any selected handle. Add a wrapper at the window delete site (the Delete/Backspace handler that calls `delete_board_card`): before deleting a `"window"`, capture its `parent`; after deletion call `room_rebuild_one(st, parent)` so the hole closes. Also delete the window's glass child if present (see Task 7) — gather the window's children and `delete_board_card` them first.

- [ ] **Step 4: Build gauntlet** — all three pass.

- [ ] **Step 5: Commit**
```bash
git add main.c
git commit -m "Windows: select/drag/resize/delete reuse + rebuild room on change"
```

---

## Task 6: Glass render pass — admit `window_glass`, raise the cap

**Files:**
- Modify: `main.c` — the opaque draw loop skip (main.c:14152), the sorted glass pass (main.c:14986-15026).

- [ ] **Step 1: Skip `window_glass` in the opaque loop**

At main.c:14152, the opaque loop already skips `church_glass`:
```c
        if (o->mesh_ref && strcmp(o->mesh_ref, "church_glass") == 0)
            continue;                             /* P9 item 2: the GLASS sub-pass draws them */
```
Add right after it:
```c
        if (o->mesh_ref && strcmp(o->mesh_ref, "window_glass") == 0)
            continue;                             /* Windows: drawn in the glass sub-pass */
```

- [ ] **Step 2: Admit `window_glass` into the sorted glass pass + raise the cap**

In the glass pass (main.c:14990), change the cap and the filter. Replace `sol_u32 gidx[16];` / `float gdist[16];` / `gn < 16` with a constant, and broaden the `mesh_ref` test:
```c
#define GLASS_DRAW_MAX 64   /* church glass + window panes, sorted together */
```
(put the `#define` just above the block, or near `GLASS_OPACITY`). Then:
```c
        sol_u32 gidx[GLASS_DRAW_MAX];
        float   gdist[GLASS_DRAW_MAX];
        int     gn = 0, ga, gb;
        for (i = 0; i < state->scene.count && gn < GLASS_DRAW_MAX; i++) {
```
and the filter line (was `church_glass` only):
```c
            if (!o->mesh_ref ||
                (strcmp(o->mesh_ref, "church_glass") != 0 &&
                 strcmp(o->mesh_ref, "window_glass") != 0)) continue;
```
The rest of the block (distance, sort, `draw_glass`) is unchanged and handles window panes the same as church glass (`material.base_color` tints, `draw_glass` applies `GLASS_OPACITY`).

- [ ] **Step 3: Build gauntlet** — all three pass.

- [ ] **Step 4: Commit**
```bash
git add main.c
git commit -m "Windows: render window_glass in the sorted transparent pass; raise cap to 64"
```

---

## Task 7: Color cycling — Up/Down presets + the glass-pane child

**Files:**
- Modify: `main.c` — a color preset table, the glass-child management helper, the Up/Down handler.

- [ ] **Step 1: Preset table + child management helper**

Add near the window helpers:
```c
static const struct { const char *name; float r, g, b; } WINDOW_GLASS[] = {
    { "none",  0.00f, 0.00f, 0.00f },   /* open hole — no pane */
    { "clear", 0.85f, 0.92f, 1.00f },
    { "blue",  0.25f, 0.45f, 0.85f },
    { "green", 0.30f, 0.70f, 0.45f },
    { "amber", 0.95f, 0.70f, 0.25f },
    { "red",   0.85f, 0.25f, 0.30f }
};
#define WINDOW_GLASS_N ((int)(sizeof WINDOW_GLASS / sizeof WINDOW_GLASS[0]))

/* find a window's existing glass-pane child (mesh_ref "window_glass"), else 0. */
static sol_u32 window_glass_child(Scene *s, sol_u32 win) {
    int i;
    for (i = 0; i < s->count; i++)
        if (s->objects[i].parent == win && s->objects[i].mesh_ref &&
            strcmp(s->objects[i].mesh_ref, "window_glass") == 0)
            return s->objects[i].handle;
    return 0;
}

/* set a window's glass color preset by name: "none" removes the pane; any other
   adds/retints a window_glass child sized to the opening. */
static void window_set_glass(AppState *st, sol_u32 win, const char *name) {
    Scene *s = &st->scene;
    sol_u32 child = window_glass_child(s, win);
    SceneObject *wo = scene_get(s, win);
    int  pi;
    if (!wo) return;
    scene_meta_set(s, win, "glass", name);
    if (strcmp(name, "none") == 0) {
        if (child) delete_board_card(st, child);
        return;
    }
    pi = 0;
    { int i; for (i = 0; i < WINDOW_GLASS_N; i++)
        if (strcmp(WINDOW_GLASS[i].name, name) == 0) { pi = i; break; } }
    if (child == 0) {
        float w  = mesh_ref_param("window", wo->mesh_params, wo->mesh_param_count, "w");
        float h  = mesh_ref_param("window", wo->mesh_params, wo->mesh_param_count, "h");
        float gp[2]; vec3 one = vec3_make(1.0f,1.0f,1.0f); Mesh empty;
        gp[0] = w; gp[1] = h;
        memset(&empty, 0, sizeof empty);
        child = scene_add(s, win, empty, vec3_make(0.0f,0.0f,0.0f), quat_identity(), one);
        scene_kind_set(s, child, KIND_PLAIN);
        scene_mesh_ref_set(s, child, "window_glass");
        scene_mesh_params_set(s, child, gp, 2);
        scene_resolve_meshes(s);
        wo = scene_get(s, win);   /* re-fetch: scene_add may have realloced */
        if (!wo) return;
    }
    {
        SceneObject *co = scene_get(s, child);
        if (co) {
            co->material = material_default();
            co->material.base_color = vec3_make(WINDOW_GLASS[pi].r, WINDOW_GLASS[pi].g, WINDOW_GLASS[pi].b);
        }
    }
}
```
(Re-`scene_get` after `scene_add`/`scene_resolve_meshes` — they can realloc — exactly as done here; never hold a `SceneObject*` across them. `material_default`, `quat_identity`, `vec3_make` are existing.)

- [ ] **Step 2: Up/Down cycles the preset when a window is selected**

Find the arrow-key handler (the board-page `←/→` cycle is near main.c:11630; `↑/↓` are free outside the palette). Add an edge-detected handler: when `st->selected_handle` is a window and not in palette/board view, `↑` advances and `↓` retreats the preset index, then calls `window_set_glass` and (since the hole geometry is unchanged) just `scene_save`:
```c
    /* Up/Down: cycle the selected window's glass color preset. */
    if (st->selected_handle != 0 && st->board_view == 0) {
        SceneObject *so = scene_get(&st->scene, st->selected_handle);
        if (so && so->mesh_ref && strcmp(so->mesh_ref, "window") == 0) {
            sol_bool up   = glfwGetKey(w, GLFW_KEY_UP)   == GLFW_PRESS;
            sol_bool down = glfwGetKey(w, GLFW_KEY_DOWN) == GLFW_PRESS;
            sol_bool now  = (sol_bool)(up || down);
            if (now && !st->win_color_was) {
                const char *cur = scene_meta_get(&st->scene, st->selected_handle, "glass");
                int idx = 0, i;
                for (i = 0; i < WINDOW_GLASS_N; i++)
                    if (cur && strcmp(WINDOW_GLASS[i].name, cur) == 0) { idx = i; break; }
                idx = (idx + (up ? 1 : WINDOW_GLASS_N - 1)) % WINDOW_GLASS_N;
                window_set_glass(st, st->selected_handle, WINDOW_GLASS[idx].name);
                scene_save(&st->scene, "scene.stml");
                printf("window glass: %s\n", WINDOW_GLASS[idx].name);
            }
            st->win_color_was = now;
        } else {
            st->win_color_was = SOL_FALSE;
        }
    } else {
        st->win_color_was = SOL_FALSE;
    }
```
Add `sol_bool win_color_was;` to `AppState` (near `cut_was_down`) and init it `SOL_FALSE` in the AppState init block.

- [ ] **Step 3: Build gauntlet** — all three pass.

- [ ] **Step 4: Commit**
```bash
git add main.c
git commit -m "Windows: Up/Down cycle glass color presets (none -> tinted pane)"
```

---

## Final verification (controller, after all tasks)

- [ ] **Full gauntlet:** `./build.sh c89check && ./build.sh && ./build.sh metal` — all pass.
- [ ] **Run `routetest`:** `./build.sh routetest && ./route_test` — the sill-panel assertion passes.
- [ ] **Dispatch a final holistic code review** (spec compliance + quality) over the whole diff, then hand to the human for live-verify.

## Human live-verify checklist (post-merge, both backends)

- Face a room wall → palette "Place window" → a framed, see-through hole appears in the wall.
- Select it → drag along the wall (stays on the wall; the hole follows when you release) → corner-resize (free aspect; the hole follows on release).
- `↑/↓` cycles glass color (none → clear → blue → green → amber → red → none); a tinted pane appears/changes; "none" = open hole again.
- Delete → the hole closes, the wall is solid; the glass child is gone too.
- Reload (`L`) → windows persist and the holes re-cut; colored panes reload tinted.
- Place several windows and keep the church stained glass visible (the cap raise works).
- Walk into the wall under a window → still solid (collision unchanged).

## Notes / known limitations (Phase 1)

- Rectangular windows in **flat** walls only. Arched/circular/pointed/french **styles** (Phase 2)
  and **gable / wall-gable spanning** (Phase 3) are separate specs.
- Moving a window to within ~half a frame of a corner or a doorway may overlap a pier; the
  along-wall clamp keeps it on the wall but does not yet avoid doorways (acceptable v1).
- No real light transport (no colored shafts) — the deferred P9 capstone.
- No new shader ⇒ no MSL twin.

---

## Self-review notes (author)

- **Spec coverage:** spec §1 object model → Task 4 (creation) + Task 3 (the wall reads it);
  §2 wall hole/sill → Task 1; §3 frame+glass meshes → Task 2, glass child → Task 7; §4 placement
  → Task 4; §5 interaction + rebuild-on-release → Task 5; §6 rendering → Task 6; §7 persistence →
  free (normal objects; verified by the reload live-verify). All covered.
- **Symbol consistency:** `RoomOpening.sill`, `make_window(w,h,t,fw)`, `make_window_glass(w,h)`,
  `"window"` / `"window_glass"`, `window_wall`, `room_append_windows`, `room_rebuild_one`,
  `cmd_place_window`/`can_place_window`, `window_set_glass`/`window_glass_child`, `win_color_was`,
  `WINDOW_GLASS[]`, `ROOM_OPENINGS_CAP`, `GLASS_DRAW_MAX`, `WINDOW_DEF_*`/`WINDOW_FRAME_W` are
  used identically across tasks. Existing symbols (`emit_doored_wall`, `make_room_doored`,
  `route_room_openings`/`_in`, `room_frame_build`, `editor_room_rect`, `descend_wall_mount`,
  `scene_world_to_local`, `draw_glass`, `delete_board_card`, the registry-rebuild law) verified in
  source.
- **Known soft spots flagged for the implementer:** the camera-forward ray helper name in Task 4
  (use the existing aim-ray construction), and the center-origin vs board bottom-origin
  reconciliation in Task 5 resize. Both are called out inline.
