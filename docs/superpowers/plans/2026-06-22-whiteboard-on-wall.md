# Whiteboard on the Wall — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Carry a whiteboard (`E`), aim at a room wall, and place it flush + upright where you aim (clamped to the wall).

**Architecture:** A new headless `descend_wall_mount` (the height-unconstrained sibling of `descend_wall_aim`) returns the clamped, flush mount center for the nearest aimed-at wall. `carry_update` gains a `"board"` branch that computes the mount and sets the existing `file_*` fields, so the existing `cmd_carry_toggle` drop re-parents the board onto the room. Pickup detaches a mounted board.

**Tech Stack:** C89 (strict), `descend.c` wall-ray geometry (`ray_vs_plane`, `RoomRect`), the carry system in `main.c`. No new shader / MSL twin.

**Spec:** `docs/superpowers/specs/2026-06-22-whiteboard-on-wall-design.md`

---

## Conventions for every task

- **Strict C89:** declarations at the top of each block, `/* */` comments only, no `//`, no mixed declarations/VLAs, `snprintf`/`strncpy` not `sprintf`/`strcpy`.
- **Build gauntlet (must stay green):** `./build.sh c89check && ./build.sh debug && ./build.sh metal`.
- Commit after each task; stage only the files the task names. **Never** `git add` `NOTES.stml`, `paper-picture.png`, or `scene*.stml`.
- Commit trailer: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
- The `main.c` carry wiring is GUI — Fran live-verifies after Task 2. The geometry (Task 1) is headless and unit-tested.

## File map

- `descend.h` / `descend.c` — `descend_wall_mount` (the mount geometry).
- `descend_test.c` — its unit cases (the `descendtest` target already exists).
- `main.c` — `carry_update` board branch; `cmd_carry_toggle` pickup-detach extension.

---

### Task 1: `descend_wall_mount` geometry + unit tests

**Files:**
- Modify: `descend.h` (declaration), `descend.c` (definition)
- Modify: `descend_test.c` (cases)

- [ ] **Step 1: Write the failing tests** — in `descend_test.c`, add this block inside `main()` right before the final `if (fails == 0) printf("descend_test: OK\n");`:

```c
    /* wall-mount: a flat board (1.8 x 1.2 x 0.05) flush on the aimed wall */
    {
        Scene s; RoomRect r; Ray ray; int wall; vec3 c; int ok;
        sol_u32 room;
        float wh = 0.9f, hh = 0.6f, t = 0.05f, ceil_y;
        scene_init(&s);
        room   = add_room(&s, 0.0f, 0.0f, 0.0f, 8.0f, 8.0f);  /* 8x8, floor y=0, h=3 */
        r      = editor_room_rect(&s, room);
        ceil_y = r.floor_y + 3.0f;
        /* centered horizontal aim at the NORTH wall (z = cz - hd = -4) */
        ray.origin = vec3_make(0.0f, 1.5f, 0.0f);
        ray.dir    = vec3_make(0.0f, 0.0f, -1.0f);
        ok = descend_wall_mount(r, ray, ceil_y, wh, hh, t, &wall, &c);
        CHECK(ok);
        CHECK(wall == ROOM_WALL_N);
        CHECK(fabs((double)(c.z - (-4.0f + t * 0.5f))) < 1e-4);  /* flush + pushed out */
        CHECK(fabs((double)c.x) < 1e-4);                          /* centered along wall */
        CHECK(fabs((double)(c.y - 1.5f)) < 1e-4);                 /* at the aim height */
        /* aim HIGH on the north wall: center clamps to ceil_y - hh */
        ray.dir = vec3_make(0.0f, 1.3f, -4.0f);   /* hits (0, 2.8, -4) */
        ok = descend_wall_mount(r, ray, ceil_y, wh, hh, t, &wall, &c);
        CHECK(ok);
        CHECK(fabs((double)(c.y - (ceil_y - hh))) < 1e-4);
        /* aim into the +x CORNER of the north wall: clamps to hw - wh */
        ray.dir = vec3_make(3.5f, 0.0f, -4.0f);   /* hits (3.5, 1.5, -4) */
        ok = descend_wall_mount(r, ray, ceil_y, wh, hh, t, &wall, &c);
        CHECK(ok);
        CHECK(fabs((double)(c.x - (4.0f - wh))) < 1e-4);
        /* a board WIDER than the wall is refused */
        ray.dir = vec3_make(0.0f, 0.0f, -1.0f);
        ok = descend_wall_mount(r, ray, ceil_y, 5.0f, hh, t, &wall, &c);
        CHECK(!ok);
        scene_free(&s);
    }
```

- [ ] **Step 2: Run the tests to verify they FAIL to build** (function undeclared)

Run: `./build.sh descendtest`
Expected: error — `call to undeclared function 'descend_wall_mount'`.

- [ ] **Step 3: Declare `descend_wall_mount`** — in `descend.h`, after the `descend_door_point` declaration:

```c
/* MOUNT a flat board (half-width w_half, half-height h_half, thickness t) flush
   on the wall `ray` aims at. The height-UNCONSTRAINED sibling of descend_wall_aim:
   accepts any hit between floor and ceil_y, clamps the center so the board stays
   fully on the wall, and pushes it off the surface by t/2 (back flush). Returns 1
   with *out_wall (ROOM_WALL_*) and the world-space *out_center; 0 if no wall is
   hit or the board is bigger than the wall. */
int descend_wall_mount(RoomRect r, Ray ray, float ceil_y,
                       float w_half, float h_half, float t,
                       int *out_wall, vec3 *out_center);
```

- [ ] **Step 4: Implement `descend_wall_mount`** — in `descend.c`, immediately after the `descend_wall_aim` function (it shares that function's wall table):

```c
int descend_wall_mount(RoomRect r, Ray ray, float ceil_y,
                       float w_half, float h_half, float t,
                       int *out_wall, vec3 *out_center) {
    struct { vec3 pt, n; float half; int runx; } w[4];
    int   bestw = -1, k;
    float bestt = 1e30f;
    vec3  besthit;
    besthit.x = 0.0f; besthit.y = 0.0f; besthit.z = 0.0f;
    w[0].pt = vec3_make(r.cx, r.floor_y, r.cz - r.hd); w[0].n = vec3_make(0.0f,0.0f, 1.0f); w[0].half = r.hw; w[0].runx = 1; /* N */
    w[1].pt = vec3_make(r.cx + r.hw, r.floor_y, r.cz); w[1].n = vec3_make(-1.0f,0.0f,0.0f); w[1].half = r.hd; w[1].runx = 0; /* E */
    w[2].pt = vec3_make(r.cx, r.floor_y, r.cz + r.hd); w[2].n = vec3_make(0.0f,0.0f,-1.0f); w[2].half = r.hw; w[2].runx = 1; /* S */
    w[3].pt = vec3_make(r.cx - r.hw, r.floor_y, r.cz); w[3].n = vec3_make( 1.0f,0.0f,0.0f); w[3].half = r.hd; w[3].runx = 0; /* W */
    for (k = 0; k < 4; k++) {
        float t0;
        vec3  hit;
        if (!ray_vs_plane(ray, w[k].pt, w[k].n, &t0)) continue;
        if (t0 <= 0.05f || t0 >= bestt) continue;
        hit = vec3_add(ray.origin, vec3_scale(ray.dir, t0));
        if (hit.y < r.floor_y - 0.1f || hit.y > ceil_y + 0.1f) continue;
        bestt = t0; bestw = k; besthit = hit;
    }
    if (bestw < 0) return 0;
    {
        float lim = w[bestw].half - w_half;          /* along-wall room for the board */
        float ylo = r.floor_y + h_half, yhi = ceil_y - h_half;
        float run, cy;
        vec3  c;
        if (lim < 0.0f || yhi < ylo) return 0;       /* board bigger than the wall */
        run = w[bestw].runx ? (besthit.x - r.cx) : (besthit.z - r.cz);
        if (run < -lim) run = -lim;
        if (run >  lim) run =  lim;
        cy = besthit.y;
        if (cy < ylo) cy = ylo;
        if (cy > yhi) cy = yhi;
        if (w[bestw].runx) { c.x = r.cx + run;        c.z = w[bestw].pt.z; }
        else               { c.z = r.cz + run;        c.x = w[bestw].pt.x; }
        c.x += w[bestw].n.x * (t * 0.5f);            /* push out so the back is flush */
        c.z += w[bestw].n.z * (t * 0.5f);
        c.y  = cy;
        *out_wall   = bestw;
        *out_center = c;
        return 1;
    }
}
```

- [ ] **Step 5: Run the tests to verify they PASS**

Run: `./build.sh descendtest && ./descend_test`
Expected: `descend_test: OK` (no sanitizer output).

- [ ] **Step 6: Gauntlet** (descend.c is in the c89check set)

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: `c89check: PASS`, both binaries build.

- [ ] **Step 7: Commit**

```bash
git add descend.h descend.c descend_test.c
git commit -m "$(printf 'descend: descend_wall_mount — flush wall mount for a carried board\n\nHeight-unconstrained sibling of descend_wall_aim: ray vs the 4 walls,\nnearest hit clamped so the board stays fully on the wall, pushed out by\nt/2 so the back is flush. Unit-tested (center/clamp/oversize-refusal).\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

### Task 2: carry the board onto the wall (carry_update branch + pickup detach)

**Files:**
- Modify: `main.c` — `carry_update` (board branch, after the card branch); `cmd_carry_toggle` (pickup detach).

After this task the feature works end to end. The drop branch in `cmd_carry_toggle` (`file_aim` → re-parent onto `file_target`) is reused unchanged.

- [ ] **Step 1: Add the board branch to `carry_update`** — find the end of the card-filing branch and the start of the hold fallthrough:

```c
            return;
        }
    }
    fwd     = camera_forward(&st->camera);
```

Insert the board branch between the card branch's close (`    }`) and `    fwd     = camera_forward(...)`:

```c
            return;
        }
    }
    if (o->mesh_ref && strcmp(o->mesh_ref, "board") == 0) {
        sol_u32 room = descend_room_at(&st->scene, st->camera.pos);
        if (room != 0) {
            RoomRect r = editor_room_rect(&st->scene, room);
            Ray     ray;
            int     wall;
            vec3    center;
            float   bw, bh, bt, rh;
            sol_u32 ci;
            bw = mesh_ref_param("board", o->mesh_params, o->mesh_param_count, "w");
            bh = mesh_ref_param("board", o->mesh_params, o->mesh_param_count, "h");
            bt = mesh_ref_param("board", o->mesh_params, o->mesh_param_count, "t");
            rh = 3.0f;                                  /* room interior height (default) */
            for (ci = 0; ci < st->scene.count; ci++) {
                SceneObject *c = &st->scene.objects[ci];
                if (c->parent == room && c->mesh_ref &&
                    strcmp(c->mesh_ref, "room") == 0) {
                    rh = mesh_ref_param("room", c->mesh_params, c->mesh_param_count, "h");
                    break;
                }
            }
            ray.origin = st->camera.pos;
            ray.dir    = camera_forward(&st->camera);
            if (descend_wall_mount(r, ray, r.floor_y + rh, bw * 0.5f, bh * 0.5f, bt,
                                   &wall, &center)) {
                static const float wall_yaw[4] = { 0.0f, -90.0f, 180.0f, 90.0f };
                vec3 P = vec3_make(center.x, center.y - bh * 0.5f, center.z);
                st->file_aim    = SOL_TRUE;
                st->file_target = room;
                st->file_rot    = quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f),
                                                       sol_radians(wall_yaw[wall]));
                st->file_local  = scene_world_to_local(&st->scene, room, P);
                o->pos = scene_world_to_local(&st->scene, o->parent, P);
                o->rot = st->file_rot;
                return;
            }
        }
    }
    fwd     = camera_forward(&st->camera);
```

(The board's geometric center is local `(0, h/2, 0)` because `make_card` is bottom-origin, so the object origin is `P = center - (0, h_half, 0)`. The yaw maps the board's +Z front to the wall's inward normal: N/E/S/W → 0/-90/180/90.)

- [ ] **Step 2: Extend the pickup detach in `cmd_carry_toggle`** — find:

```c
                    SceneObject *par = scene_get(&st->scene, co->parent);
                    if (par && par->mesh_ref &&
                        (furniture_is_table(par->mesh_ref) || furniture_is_shelf(par->mesh_ref))) {
                        vec3 wp = object_world_pos(&st->scene, t);   /* world pos before detach */
                        co->parent = 0;                               /* leave the furniture */
                        co->pos    = wp;
                    }
```

Replace with (detach a mounted board too — it is a child of the room):

```c
                    SceneObject *par = scene_get(&st->scene, co->parent);
                    sol_bool on_furn = (sol_bool)(par && par->mesh_ref &&
                        (furniture_is_table(par->mesh_ref) || furniture_is_shelf(par->mesh_ref)));
                    sol_bool mounted = (sol_bool)(co->parent != 0 && co->mesh_ref &&
                        strcmp(co->mesh_ref, "board") == 0);
                    if (on_furn || mounted) {
                        vec3 wp = object_world_pos(&st->scene, t);   /* world pos before detach */
                        co->parent = 0;                               /* leave the wall/furniture */
                        co->pos    = wp;
                    }
```

- [ ] **Step 3: Gauntlet**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: `c89check: PASS`, both binaries build.

- [ ] **Step 4: Re-run the geometry test (regression)**

Run: `./build.sh descendtest && ./descend_test`
Expected: `descend_test: OK`.

- [ ] **Step 5: Commit**

```bash
git add main.c
git commit -m "$(printf 'carry: mount a carried whiteboard on the wall you aim at\n\ncarry_update gains a board branch that uses descend_wall_mount to compute a\nflush, clamped mount and sets the existing file_* fields, so the drop branch\nre-parents the board onto the room. Pickup detaches a mounted board (the\nfurniture-detach pattern, extended to boards).\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

- [ ] **Step 6: Hand to Fran for live verify** — carry a whiteboard (`E` to pick up the selected board), aim high and low on a wall: it previews flat + upright, following the aim, clamped on the wall edges. `E` mounts it flush, facing the room; it persists across reload; re-picking with `E` lifts it back into the hand. Check both `./solarium` and `./solarium-metal` (no shader change, so Metal should match). Note any tuning (push-out distance, clamp feel) for a follow-up.

---

## Self-review (against the spec)

- **`descend_wall_mount` (height-unconstrained, clamped, pushed-out, refuses oversize)** (spec §"The geometry") → Task 1, with unit cases for center/high-clamp/corner-clamp/oversize. ✓
- **`carry_update` board branch: room + wall aim → `file_*` + preview** (spec §"carry_update") → Task 2 Step 1. ✓
- **Bottom-origin offset `P = center - (0,h/2,0)` + yaw table N/E/S/W = 0/-90/180/90** (spec) → Task 2 Step 1. ✓
- **Drop reused unchanged** (spec §"drop") → no edit; relies on the existing `file_aim` branch. ✓
- **Pickup detaches a mounted board** (spec §"pickup detach") → Task 2 Step 2. ✓
- **ceil_y from the room shell `"h"` (default 3)** (spec §"error handling") → Task 2 Step 1 inline scan. ✓
- **Headless unit test + live verify** (spec §"Testing") → Task 1 tests + Task 2 Step 6. ✓
- **Scope: boards only, carry flow, axis-aligned walls; resize deferred** (spec §"Scope") → no resize anywhere. ✓

**Type/name consistency:** `descend_wall_mount(RoomRect, Ray, float, float, float, float, int*, vec3*)` is declared (Task 1 Step 3), defined (Step 4), and called identically (Task 2 Step 1). `file_aim`/`file_target`/`file_local`/`file_rot`, `descend_room_at`, `editor_room_rect`, `mesh_ref_param`, `scene_world_to_local`, `quat_from_axis_angle`, `sol_radians`, `camera_forward`, `object_world_pos`, `furniture_is_table/_shelf`, `ROOM_WALL_N`, `ray_vs_plane`, `vec3_make/_add/_scale` all exist as used. No placeholders.
