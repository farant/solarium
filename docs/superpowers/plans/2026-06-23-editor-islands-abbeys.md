# Editor: Drag / Connect / Resize Islands & Abbeys — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make terrain islands first-class editor footprints — draggable, walkway-connectable (to rooms and each other), and resizable — with abbeys (island + plot-linked church) dragging as a unit and not resizing.

**Architecture:** Generalize the editor's and router's room-only footprint reads to also read an island's footprint from its own `mesh_params`. Drag and connect then fall out of the existing machinery; resize gets a terrain branch plus a hero-dressing re-ground; the abbey rides its hill via the `plot` link. The terrain mesh is a registry asset, so a resized island re-tessellates through the registry-shared rebuild on commit.

**Tech Stack:** C89 (Dependable-C), OpenGL + Metal via the RHI seam. Build gauntlet: `./build.sh c89check && ./build.sh debug && ./build.sh metal && ./build.sh editortest && ./build.sh routetest`.

**Spec:** `docs/superpowers/specs/2026-06-23-editor-islands-abbeys-design.md`.

**Project laws (every task):** strict C89 — declarations at the top of each block, `/* */` only, no `//`, no mixed decl/statement, no VLAs, no C99 `fabsf` (use `fabs((double)x)`). `editor.c`/`route.c` stay pure (scene + math, no GL). Never `git add` `NOTES.stml`/`paper-picture.png`. Commits end with the `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>` line. Branch `editor-islands` is checked out.

**Verified facts (no need to re-confirm):** `mesh_ref_param("terrain", p, n, "w")`/`"d"` read params[0]/[1] (mesh.c:1038 — terrain params are `{w,d,sub,amp,seed}`). `terrain_height(const float *params, int count, float lx, float lz)` is in mesh.c (mesh.h:116), linked by editortest, callable from pure `editor.c`. `scene_add` assigns `o->nid` immediately (scene.c:64). The walkway door *cutout* is gated to `room_type` home/mirror (main.c:4198/4277), so terrain endpoints are doorless automatically. `scene_mesh_params_set` only sets params (scene.c) — it does NOT rebuild the mesh.

---

## Task 1: Router learns terrain footprints

Teach `route.c` to read an island's footprint and to collect islands as routable endpoints. This alone makes a walkway *route* to an island (the geometry/UI comes in Task 4).

**Files:**
- Modify: `route.c` (`room_half`, `collect_rooms`)
- Test: `route_test.c`

- [ ] **Step 1: Write the failing test**

In `route_test.c`, add an island helper after `add_walkway` (near line 38):

```c
/* add a terrain island (single object: mesh_ref "terrain", own w/d params). */
static sol_u32 add_island(Scene *s, float x, float y, float z, float w, float d) {
    Mesh    empty;
    sol_u32 h;
    float   p[5];
    memset(&empty, 0, sizeof empty);
    h = scene_add(s, 0, empty, vec3_make(x, y, z), quat_identity(), vec3_make(1,1,1));
    scene_mesh_ref_set(s, h, "terrain");
    scene_meta_set(s, h, "room_type", "terrain");
    p[0] = w; p[1] = d; p[2] = 56.0f; p[3] = 2.0f; p[4] = 1.0f;
    scene_mesh_params_set(s, h, p, 5);
    return h;
}
```

Then add a test block inside `main()` just before `if (fails == 0)`:

```c
    /* a walkway routes between a room and a terrain island (island endpoint
       resolves to its own footprint; the route is valid). */
    {
        Scene s; Route r; sol_u32 home, isle, wk;
        scene_init(&s);
        home = add_room(&s,   0.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        isle = add_island(&s, 30.0f, 12.0f, 0.0f, 20.0f, 16.0f);  /* hw=10, hd=8 */
        wk   = add_walkway(&s, home, isle);
        CHECK(route_for_walkway(&s, wk, &r));
        CHECK(r.valid);
        /* the island endpoint sits ~30 east; its west edge is at x=30-10=20,
           well clear of the room's east edge (x=4): a real span, not degenerate */
        CHECK(r.room_lo == home || r.room_hi == home);
        CHECK(r.room_lo == isle || r.room_hi == isle);
        scene_free(&s);
    }
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `./build.sh routetest && ./route_test`
Expected: FAIL — the island is filtered out of `collect_rooms`, so `route_for_walkway` returns 0 / the route is invalid.

- [ ] **Step 3: Add the terrain branch to `room_half`**

In `route.c`, replace `room_half` (route.c:14-26) with:

```c
/* a routable endpoint's half-extents. A room: its "room" shell child's w/d. An
   island (mesh_ref "terrain"): its OWN w/d params. */
static void room_half(Scene *s, sol_u32 room, float *hw, float *hd) {
    SceneObject *ro = scene_get(s, room);
    sol_u32 i;
    *hw = 4.0f; *hd = 4.0f;
    if (ro && ro->mesh_ref && strcmp(ro->mesh_ref, "terrain") == 0) {
        *hw = 0.5f * mesh_ref_param("terrain", ro->mesh_params, ro->mesh_param_count, "w");
        *hd = 0.5f * mesh_ref_param("terrain", ro->mesh_params, ro->mesh_param_count, "d");
        return;
    }
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        if (o->parent == room && o->mesh_ref && strcmp(o->mesh_ref, "room") == 0) {
            *hw = 0.5f * mesh_ref_param("room", o->mesh_params, o->mesh_param_count, "w");
            *hd = 0.5f * mesh_ref_param("room", o->mesh_params, o->mesh_param_count, "d");
            return;
        }
    }
}
```

- [ ] **Step 4: Include terrain in `collect_rooms`**

In `route.c`, change the filter in `collect_rooms` (route.c:41) from:

```c
        if (strcmp(rt, "home") != 0 && strcmp(rt, "mirror") != 0) continue;
```

to:

```c
        if (strcmp(rt, "home")    != 0 && strcmp(rt, "mirror") != 0 &&
            strcmp(rt, "terrain") != 0 && strcmp(rt, "land")   != 0) continue;
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `./build.sh routetest && ./route_test`
Expected: `route_test: OK`, no sanitizer output. (Re-run gives the same — pure.)

- [ ] **Step 6: Verify C89 + both backends**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: `c89check: PASS`, both build.

- [ ] **Step 7: Commit**

```bash
git add route.c route_test.c
git commit -m "$(cat <<'EOF'
editor: router routes walkways to terrain islands

room_half reads an island's own w/d params (a terrain object is its own
footprint); collect_rooms includes room_type terrain/land. The door cutout is
already gated to home/mirror, so island endpoints are doorless. Unit-tested.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Editor footprint, pick, resizable predicate, abbey group-move

Make islands pickable and movable, exclude churches from picking, add the `editor_resizable` predicate, and move plot-linked churches with their hill.

**Files:**
- Modify: `editor.h` (declare `editor_resizable`)
- Modify: `editor.c` (`editor_room_rect`, `editor_room_under`, `editor_apply_move`, new `editor_resizable`)
- Test: `editor_test.c`

- [ ] **Step 1: Write the failing tests**

In `editor_test.c`, add island + church helpers after `add_room` (near line 27):

```c
/* a terrain island: a single object that IS its own footprint. */
static sol_u32 add_island(Scene *s, float x, float y, float z, float w, float d) {
    Mesh    empty;
    sol_u32 h;
    float   p[5];
    memset(&empty, 0, sizeof empty);
    h = scene_add(s, 0, empty, vec3_make(x, y, z), quat_identity(), vec3_make(1,1,1));
    scene_mesh_ref_set(s, h, "terrain");
    scene_meta_set(s, h, "room_type", "terrain");
    p[0] = w; p[1] = d; p[2] = 56.0f; p[3] = 2.0f; p[4] = 1.0f;
    scene_mesh_params_set(s, h, p, 5);
    return h;
}

/* a church anchor plot-linked to an island (an abbey's building). */
static sol_u32 add_church_on(Scene *s, sol_u32 island, float x, float y, float z) {
    Mesh    empty;
    sol_u32 h;
    SceneObject *io = scene_get(s, island);
    memset(&empty, 0, sizeof empty);
    h = scene_add(s, 0, empty, vec3_make(x, y, z), quat_identity(), vec3_make(1,1,1));
    scene_meta_set(s, h, "room_type", "church");
    if (io && io->nid) scene_meta_set(s, h, "plot", io->nid);
    return h;
}
```

Add a test block in `main()` before `if (fails == 0)`:

```c
    /* an island IS its own footprint (rect from its own w/d params). */
    {
        Scene s; sol_u32 isle; RoomRect r;
        scene_init(&s);
        isle = add_island(&s, 2.0f, 5.0f, -3.0f, 30.0f, 20.0f);
        r = editor_room_rect(&s, isle);
        CHECK(fabs((double)(r.cx - 2.0f)) < 1e-4);
        CHECK(fabs((double)(r.cz + 3.0f)) < 1e-4);
        CHECK(fabs((double)(r.floor_y - 5.0f)) < 1e-4);
        CHECK(fabs((double)(r.hw - 15.0f)) < 1e-4);
        CHECK(fabs((double)(r.hd - 10.0f)) < 1e-4);
        scene_free(&s);
    }
    /* resizable: a plain island yes; an island with a church (abbey) no; a room yes. */
    {
        Scene s; sol_u32 plain, abbey_hill, room;
        scene_init(&s);
        plain      = add_island(&s,  0.0f, 0.0f,   0.0f, 20.0f, 20.0f);
        abbey_hill = add_island(&s, 80.0f, 0.0f,   0.0f, 30.0f, 30.0f);
        add_church_on(&s, abbey_hill, 80.0f, 2.0f, 0.0f);
        room       = add_room(&s,   -80.0f, 0.0f,  0.0f, 8.0f, 8.0f);
        CHECK(editor_resizable(&s, plain)      == SOL_TRUE);
        CHECK(editor_resizable(&s, abbey_hill) == SOL_FALSE);
        CHECK(editor_resizable(&s, room)       == SOL_TRUE);
        scene_free(&s);
    }
    /* abbey group-move: dragging the hill shifts its church by the same delta. */
    {
        Scene s; sol_u32 hill, church; SceneObject *co; vec3 c0;
        scene_init(&s);
        hill   = add_island(&s, 0.0f, 0.0f, 0.0f, 30.0f, 30.0f);
        church = add_church_on(&s, hill, 1.0f, 2.0f, -2.0f);
        co = scene_get(&s, church); c0 = co->pos;
        editor_apply_move(&s, hill, 10.0f, 4.0f);   /* hill 0,0 -> 10,4 (delta 10,4) */
        co = scene_get(&s, church);
        CHECK(fabs((double)(co->pos.x - (c0.x + 10.0f))) < 1e-3);
        CHECK(fabs((double)(co->pos.z - (c0.z + 4.0f)))  < 1e-3);
        CHECK(fabs((double)(co->pos.y - c0.y)) < 1e-3);   /* y unchanged */
        scene_free(&s);
    }
```

- [ ] **Step 2: Run to verify it fails**

Run: `./build.sh editortest && ./editor_test`
Expected: FAIL — `editor_resizable` is undefined (link error), and `editor_room_rect`/`editor_apply_move` don't yet handle terrain.

- [ ] **Step 3: Declare `editor_resizable`**

In `editor.h`, after the `editor_can_connect` declaration (line ~58), add:

```c
sol_bool editor_resizable(Scene *s, sol_u32 room);   /* room or church-less island */
```

- [ ] **Step 4: Terrain branch in `editor_room_rect`**

In `editor.c`, replace `editor_room_rect` (editor.c:12-30) with:

```c
RoomRect editor_room_rect(Scene *s, sol_u32 room) {
    RoomRect     r;
    SceneObject *ro = scene_get(s, room);
    sol_u32      i;
    float        w = 10.0f, d = 10.0f;
    r.cx = r.cz = r.floor_y = 0.0f; r.hw = r.hd = 5.0f;
    if (!ro) return r;
    if (ro->mesh_ref && strcmp(ro->mesh_ref, "terrain") == 0) {  /* island = own footprint */
        w = mesh_ref_param("terrain", ro->mesh_params, ro->mesh_param_count, "w");
        d = mesh_ref_param("terrain", ro->mesh_params, ro->mesh_param_count, "d");
    } else {
        for (i = 0; i < s->count; i++) {
            SceneObject *o = &s->objects[i];
            if (o->parent == room && o->mesh_ref && strcmp(o->mesh_ref, "room") == 0) {
                w = mesh_ref_param("room", o->mesh_params, o->mesh_param_count, "w");
                d = mesh_ref_param("room", o->mesh_params, o->mesh_param_count, "d");
                break;
            }
        }
    }
    r.cx = ro->pos.x; r.cz = ro->pos.z; r.floor_y = ro->pos.y;
    r.hw = 0.5f * w;  r.hd = 0.5f * d;
    return r;
}
```

- [ ] **Step 5: Add `editor_resizable` and the abbey group-move**

In `editor.c`, add `editor_resizable` just after `editor_room_rect` (and before `editor_classify`):

```c
/* Resizable footprints: a room (has a "room" shell child) and a terrain island
   that carries NO church (an abbey hill is movable + connectable but not
   resizable — its church stone is baked from church_plan). */
sol_bool editor_resizable(Scene *s, sol_u32 room) {
    SceneObject *o = scene_get(s, room);
    sol_u32      i;
    if (!o) return SOL_FALSE;
    if (o->mesh_ref && strcmp(o->mesh_ref, "terrain") == 0) {
        if (o->nid) {
            for (i = 0; i < s->count; i++) {
                const char *rt = scene_meta_get(s, s->objects[i].handle, "room_type");
                const char *pl = scene_meta_get(s, s->objects[i].handle, "plot");
                if (rt && strcmp(rt, "church") == 0 && pl && strcmp(pl, o->nid) == 0)
                    return SOL_FALSE;     /* an abbey: not resizable */
            }
        }
        return SOL_TRUE;
    }
    for (i = 0; i < s->count; i++)        /* a room: has a "room" shell child */
        if (s->objects[i].parent == room && s->objects[i].mesh_ref &&
            strcmp(s->objects[i].mesh_ref, "room") == 0) return SOL_TRUE;
    return SOL_FALSE;
}
```

Then replace `editor_apply_move` (editor.c:118-123) with:

```c
/* Move a room/island: write its anchor's world XZ. If the moved object is a
   terrain island, every church plot-linked to it rides along by the same delta
   (the abbey moves as a unit) — scene structure unchanged. */
void editor_apply_move(Scene *s, sol_u32 room, float cx, float cz) {
    SceneObject *o = scene_get(s, room);
    float        dx, dz;
    if (!o) return;
    dx = cx - o->pos.x;
    dz = cz - o->pos.z;
    o->pos.x = cx;
    o->pos.z = cz;
    if (o->mesh_ref && strcmp(o->mesh_ref, "terrain") == 0 && o->nid) {
        sol_u32 i;
        for (i = 0; i < s->count; i++) {
            SceneObject *c  = &s->objects[i];
            const char  *rt = scene_meta_get(s, c->handle, "room_type");
            const char  *pl = scene_meta_get(s, c->handle, "plot");
            if (rt && strcmp(rt, "church") == 0 && pl && o->nid && strcmp(pl, o->nid) == 0) {
                c->pos.x += dx;
                c->pos.z += dz;
            }
        }
    }
}
```

(Note: re-reading `o`/`c` by index inside the loop is safe — no `scene_add` happens here, so pointers stay valid. `o` is captured once before the loop and not used after the loop body mutates siblings.)

- [ ] **Step 6: Make islands pickable, exclude churches**

In `editor.c`, in `editor_room_under` (editor.c:208), change:

```c
        if (o->mesh_ref) continue;                      /* anchors are empties */
        if (!scene_meta_get(s, o->handle, "room_type")) continue;
```

to:

```c
        if (o->mesh_ref && strcmp(o->mesh_ref, "terrain") != 0) continue;  /* rooms (empty) + islands */
        {   /* a church rides its hill: grabbing an abbey lands on the terrain */
            const char *rt = scene_meta_get(s, o->handle, "room_type");
            if (!rt || strcmp(rt, "church") == 0) continue;
        }
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `./build.sh editortest && ./editor_test`
Expected: `editor_test: OK`, no sanitizer output.

- [ ] **Step 8: Verify C89 + both backends**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: all pass.

- [ ] **Step 9: Commit**

```bash
git add editor.h editor.c editor_test.c
git commit -m "$(cat <<'EOF'
editor: islands as draggable footprints; abbey moves as a unit

editor_room_rect reads a terrain island's own w/d; editor_room_under picks
islands and excludes churches (they ride the hill); new editor_resizable
(room or church-less island); editor_apply_move shifts plot-linked churches
with their hill. Unit-tested (footprint, resizable, group-move).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Island resize + hero re-ground, resize gating

Add the terrain branch to `editor_apply_resize` (write the island's own params + re-ground its dressing) and gate resize in `editor_press` so non-resizable footprints only move.

**Files:**
- Modify: `editor.c` (`editor_apply_resize`, `editor_press`)
- Test: `editor_test.c`

- [ ] **Step 1: Write the failing test**

In `editor_test.c`, add a test block before `if (fails == 0)`:

```c
    /* resize a plain island: its own w/d params grow, and a hero child
       re-positions proportionally (and gets re-grounded to the new terrain). */
    {
        Scene s; sol_u32 isle, tree; Mesh empty; SceneObject *io, *to;
        float lx0;
        memset(&empty, 0, sizeof empty);
        scene_init(&s);
        isle = add_island(&s, 0.0f, 0.0f, 0.0f, 20.0f, 20.0f);   /* hw=10, hd=10 */
        /* a hero child at local (4,*,0) = +0.4 of half-width, like wild dressing */
        tree = scene_add(&s, isle, empty, vec3_make(4.0f, 0.0f, 0.0f),
                         quat_identity(), vec3_make(1,1,1));
        scene_mesh_ref_set(&s, tree, "oak");
        to = scene_get(&s, tree); lx0 = to->pos.x;
        /* drag the +X edge out to x=20: -X wall fixed at -10, so new hw=15, cx=5 */
        editor_apply_resize(&s, isle, EDIT_ZONE_EDGE_XP, 20.0f, 0.0f);
        io = scene_get(&s, isle);
        CHECK(fabs((double)(mesh_ref_param("terrain", io->mesh_params,
                            io->mesh_param_count, "w") - 30.0f)) < 1e-2);  /* w 20 -> 30 */
        to = scene_get(&s, tree);
        CHECK(fabs((double)(to->pos.x - lx0 * 1.5f)) < 1e-2);   /* local x scaled by 15/10 */
        scene_free(&s);
    }
```

- [ ] **Step 2: Run to verify it fails**

Run: `./build.sh editortest && ./editor_test`
Expected: FAIL — `editor_apply_resize` finds no "room" child for the island, so its params don't change (w stays 20) and the child doesn't move.

- [ ] **Step 3: Add the terrain branch to `editor_apply_resize`**

In `editor.c`, in `editor_apply_resize`, the body currently (after computing `ncx/ncz/nhw/nhd` and writing `ro->pos.x/z`) scans for a "room" child and then rescales wall children. Insert a terrain branch right after `ro->pos.x = ncx; ro->pos.z = ncz;` and BEFORE the existing `for (i = 0; ...)` "room" shell loop, and `return` from it (terrain islands don't have room shells or wall-mounted boards):

```c
    ro->pos.x = ncx;
    ro->pos.z = ncz;
    if (ro->mesh_ref && strcmp(ro->mesh_ref, "terrain") == 0) {
        /* write the island's OWN w/d; re-ground its hero dressing: scale each
           child's local x/z by the per-axis ratio, re-snap y to the new terrain. */
        float rx = (r.hw > 1e-4f) ? nhw / r.hw : 1.0f;
        float rz = (r.hd > 1e-4f) ? nhd / r.hd : 1.0f;
        float p[MESH_REF_MAX_PARAMS];
        int   k, np = ro->mesh_param_count;
        if (np < 2) np = 2;
        if (np > MESH_REF_MAX_PARAMS) np = MESH_REF_MAX_PARAMS;
        for (k = 0; k < np; k++)
            p[k] = (k < ro->mesh_param_count) ? ro->mesh_params[k] : 0.0f;
        p[0] = 2.0f * nhw;
        p[1] = 2.0f * nhd;
        scene_mesh_params_set(s, room, p, np);
        for (i = 0; i < s->count; i++) {
            SceneObject *c = &s->objects[i];
            if (c->parent != room) continue;
            c->pos.x *= rx;
            c->pos.z *= rz;
            c->pos.y  = terrain_height(p, np, c->pos.x, c->pos.z);
        }
        return;
    }
```

(The existing room-shell loop and the wall-board rescale below this stay unchanged for rooms.)

Add the include for `terrain_height` at the top of `editor.c` — it already includes `mesh.h` (for `mesh_ref_param`), and `mesh.h:116` declares `terrain_height`, so no new include is needed. `MESH_REF_MAX_PARAMS` comes from `mesh.h`/`scene.h` (already in scope via the existing includes; if the compiler can't find it, add `#include "scene.h"` — it is already included transitively through `editor.h`).

- [ ] **Step 4: Gate resize in `editor_press`**

In `editor.c`, in `editor_press` (editor.c ~256-262), the tail currently is:

```c
        if (z == EDIT_ZONE_BODY) {
            e->action   = EDIT_MOVE;
            e->grab_off = vec3_make(r.cx - gp.x, 0.0f, r.cz - gp.z);
        } else {
            e->action = EDIT_RESIZE;
            e->zone   = z;
        }
```

Replace with (a non-resizable footprint always moves, even from an edge/corner):

```c
        if (z == EDIT_ZONE_BODY || !editor_resizable(s, room)) {
            e->action   = EDIT_MOVE;
            e->grab_off = vec3_make(r.cx - gp.x, 0.0f, r.cz - gp.z);
        } else {
            e->action = EDIT_RESIZE;
            e->zone   = z;
        }
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `./build.sh editortest && ./editor_test`
Expected: `editor_test: OK`.

- [ ] **Step 6: Verify C89 + both backends**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add editor.c editor_test.c
git commit -m "$(cat <<'EOF'
editor: resize terrain islands (re-ground dressing); gate non-resizables

editor_apply_resize writes a terrain island's own w/d and re-grounds each
child (scale local x/z by the size ratio, re-snap y to terrain_height of the
new params). editor_press treats edges/corners of a non-resizable footprint
(an abbey hill) as a move. Unit-tested.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Wire it into main.c (overlay, terrain mesh rebuild)

The pure pieces work; now the editor overlay must draw islands and gate resize handles, and a resized island must actually re-tessellate (the terrain mesh is a registry asset).

**Files:**
- Modify: `main.c` (`editor_draw_overlay`; the editor press/commit handling; AppState fields)

- [ ] **Step 1: AppState fields for the terrain-resize rebuild**

In the `AppState` struct, near the editor-related fields (search for `editor_del_was`), add:

```c
    char     editor_resize_key[160];  /* a resized island's mesh registry key, captured at press */
    sol_bool editor_resize_keyed;     /* a terrain resize is in flight (rebuild its mesh on commit) */
```

- [ ] **Step 2: Capture the island's mesh key when a terrain resize begins**

In `main.c`, in the editor input block, the press branch (main.c ~8172) currently is:

```c
                } else {
                    st->selected_handle = st->editor.room;       /* highlight the room */
                }
```

Replace with:

```c
                } else {
                    st->selected_handle = st->editor.room;       /* highlight the room */
                    /* a terrain RESIZE rebuilds the registry mesh on commit;
                       capture the OLD key now, before drag rewrites the params. */
                    st->editor_resize_keyed = SOL_FALSE;
                    if (st->editor.action == EDIT_RESIZE) {
                        SceneObject *ro = scene_get(&st->scene, st->editor.room);
                        if (ro && ro->mesh_ref && strcmp(ro->mesh_ref, "terrain") == 0)
                            st->editor_resize_keyed = mesh_asset_key(ro, st->editor_resize_key);
                    }
                }
```

- [ ] **Step 3: Rebuild the island's mesh on commit**

In `main.c`, the editor commit block (main.c ~8581) currently is:

```c
    if (st->editor.commit) {
        connections_rebuild(st);
        collide_rebuild(&st->colliders, &st->scene);
        scene_save(&st->scene, "scene.stml");
        st->editor.commit = SOL_FALSE;
        st->editor.dirty  = SOL_FALSE;
    } else if (st->editor.dirty) {
```

Replace the `if (st->editor.commit) {` body's first lines so the terrain mesh rebuilds before the rest:

```c
    if (st->editor.commit) {
        if (st->editor_resize_keyed) {           /* a terrain island was resized: re-tessellate */
            SceneObject *ro = scene_get(&st->scene, st->editor.room);
            asset_release(&g_mesh_assets, st->editor_resize_key);
            if (ro) memset(&ro->mesh, 0, sizeof ro->mesh);   /* drop the borrow; resolve rebuilds */
            scene_resolve_meshes(&st->scene);
            st->editor_resize_keyed = SOL_FALSE;
        }
        connections_rebuild(st);
        collide_rebuild(&st->colliders, &st->scene);
        scene_save(&st->scene, "scene.stml");
        st->editor.commit = SOL_FALSE;
        st->editor.dirty  = SOL_FALSE;
    } else if (st->editor.dirty) {
```

(`mesh_asset_key`, `asset_release`, `g_mesh_assets`, `scene_resolve_meshes` are all existing main.c symbols used by the board-resize path at main.c:7450-7461 — match those call shapes.)

- [ ] **Step 4: Draw island footprints + gate resize handles in the overlay**

In `main.c`, in `editor_draw_overlay` (main.c ~11192), change the skip line:

```c
        if (st->scene.objects[i].mesh_ref) continue;
        if (!scene_meta_get(&st->scene, h, "room_type")) continue;
        if (!scene_object_active(&st->scene, h)) continue;   /* only the active world's rooms */
```

to (admit terrain islands; exclude churches — they ride the hill):

```c
        if (st->scene.objects[i].mesh_ref &&
            strcmp(st->scene.objects[i].mesh_ref, "terrain") != 0) continue;
        {
            const char *rt = scene_meta_get(&st->scene, h, "room_type");
            if (!rt || strcmp(rt, "church") == 0) continue;
        }
        if (!scene_object_active(&st->scene, h)) continue;   /* only the active world */
```

Then find the "corner resize handles" loop in the same function:

```c
        /* corner resize handles */
        for (k = 0; k < 4; k++)
            ui_quad(px[k] - hs, py[k] - hs, 2.0f * hs, 2.0f * hs,
                    0.92f, 0.92f, 0.96f, 0.95f);
```

and gate it on `editor_resizable`:

```c
        /* corner resize handles — only for resizable footprints (rooms,
           church-less islands); an abbey hill shows move + connect, no resize */
        if (editor_resizable(&st->scene, h))
            for (k = 0; k < 4; k++)
                ui_quad(px[k] - hs, py[k] - hs, 2.0f * hs, 2.0f * hs,
                        0.92f, 0.92f, 0.96f, 0.95f);
```

- [ ] **Step 5: Build the gauntlet**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal && ./build.sh editortest && ./build.sh routetest`
Expected: `c89check: PASS`, both binaries build, both tests OK. (No new shader — confirm `metal` links clean.)

- [ ] **Step 6: Manual live-verify (human)**

Run `./solarium` (and `./solarium-metal`). In the top-down editor:
1. **Drag an island** — its footprint outline shows; dragging moves it; the grass/forest follow.
2. **Drag an abbey** — grabbing it moves the hill *and* its church together; no resize handles on the abbey.
3. **Resize a plain island** — drag a corner/edge → the terrain re-tessellates on release, the hero tree/rocks/pond stay on the (re-shaped) land (not buried/floating).
4. **Connect** — drag from a room's port to an island (and island→island, room→abbey hill) → a walkway appears meeting the island's edge (no door cut into the island).
5. **Reload (`L`)** — the moved/resized/connected layout persists.
6. **Workspaces** — islands/abbeys only show + manipulate in their own workspace (the overlay filter already added).

- [ ] **Step 7: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
editor: draw island footprints, gate resize handles, re-tessellate on resize

editor_draw_overlay admits terrain islands and excludes churches; resize
handles only show for resizable footprints. A resized island's mesh (a
registry asset) re-tessellates on commit via the registry-shared rebuild
(old key captured at press, asset_release + clear borrow + resolve).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Finish the development branch

- [ ] **Step 1: Full gauntlet**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal && ./build.sh editortest && ./build.sh routetest && ./editor_test && ./route_test`
Expected: `c89check: PASS`, both binaries build, both tests print OK.

- [ ] **Step 2: Confirm the tree**

Run: `git status --short`
Expected: clean except `NOTES.stml`/`paper-picture.png` and gitignored artifacts.

- [ ] **Step 3: Finish the branch**

Announce: "I'm using the finishing-a-development-branch skill to complete this work." Follow `superpowers:finishing-a-development-branch` — verify tests, present options, and on the chosen option ff-merge `editor-islands` into `main` and delete the branch.

---

## Self-review notes (for the implementer)

- **Pure files stay pure:** `editor.c`/`route.c` add no GL and no AppState. The terrain mesh *rebuild* (registry) is the only piece that must live in `main.c` (Task 4), because it touches `g_mesh_assets`.
- **Old-key timing:** the registry rebuild needs the island's mesh key from *before* the resize. Capture it at press (Task 4 Step 2), not at commit — the drag rewrites params every frame, so by commit the live key no longer matches the borrowed mesh.
- **Never deref across `scene_add`:** none of these edits call `scene_add` mid-loop, so index re-reads are safe. (`editor_connect`/`editor_apply_move` don't add while iterating siblings.)
- **Terrain endpoints are doorless for free:** the door cutout is gated to home/mirror shells (main.c:4198/4277) — no change needed; islands just get a walkway meeting their edge.
- **Known v1 limitation:** a walkway meets an island at the island's *base* Y (its anchor pos.y); the walkable surface at the very edge may sit a little above that if the edge has relief. Cosmetic; tune later if visible.
