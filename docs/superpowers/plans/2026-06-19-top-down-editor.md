# Top-Down Editor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Every subagent (implementers AND reviewers): READ-ONLY git only** — `git diff`/`log`/`show`, NEVER `checkout`/`switch`/`reset`/`stash`/`branch`/`commit` outside your one task commit. **Never stage `NOTES.stml` or `paper-picture.png`** (the user's files, always modified). Every commit message ends with the line `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. Strict C89: declarations at the top of each block, `/* */` comments only.

**Goal:** A bird's-eye editor camera mode that lets you drag floating rooms to reposition them, resize their footprint, and re-wire the walkway connections between them — with walkways and doorways re-threading live.

**Architecture:** A new `CAMERA_RTS` orthographic camera mode (fixed angled vantage) over the same scene. A new headless module `editor.c`/`editor.h` owns the interaction state machine and all pure geometry (hit-testing, resize math, connect validation), unit-tested in `editor_test.c`. `main.c` wires it in: a toggle command, camera enter/exit, routing the mouse to the editor when active, and driving `connections_rebuild`/`collide_rebuild`/`scene_save` from the editor's `dirty`/`commit` flags. The §1.4 one-author law is preserved — the editor only changes authored room transforms/params and the `connects` graph; geometry re-derives.

**Tech Stack:** C89 (strict), GLFW, the existing RHI seam (GL + Metal), the `ui.h` immediate-mode 2D overlay, the `route.c` routing author.

**Spec:** `docs/superpowers/specs/2026-06-19-top-down-editor-design.md`

---

## Background the implementer needs

- **Build gauntlet** (run after every task): `./build.sh c89check && ./build.sh debug && ./build.sh metal`. All three must pass. `c89check` is `-std=c89 -pedantic-errors -Werror`.
- **Headless tests** are standalone binaries built by `./build.sh <target>`; run them and check the final `... : OK` line. They link only the pure-CPU sources (no GL).
- **Live-verify is the human's job** — you cannot drive the GLFW window. Your bar is: the gauntlet passes and the touched `*_test` suites print OK. Hand off a live-verify checklist at the end of integration tasks.
- **Rooms** are a parent-0 empty object carrying `meta["room_type"]` ("home" or "mirror"), plus a child with `mesh_ref="room"` whose `mesh_params` are `[w, d, h, wn, we, ws, ww, ceil]` (width=X, depth=Z, height=Y, then 4 wall flags + ceiling flag). Because the anchor is parent-0, its `pos` IS its world position.
- **Walkways** are parent-0 objects with `mesh_ref="walkway"` and two `connects` relations to the two room anchors they join. Their geometry is derived by `connections_rebuild` from `route_all`.
- `mesh_ref_param(ref, params, count, name)` (mesh.h:157) reads a named param with registry defaults.
- `connections_rebuild(AppState*)` (main.c:4091) re-derives every walkway + room-shell mesh from the routes. `collide_rebuild(&st->colliders, &st->scene)` re-derives colliders. Both already exist and are called after `create_root_from_path` (main.c:6615-6616).

---

## Task 1: Camera — `CAMERA_RTS` orthographic mode

Add an angled-orthographic camera mode with pan/zoom and an ortho pick-ray. Pure math + state, headless-tested by `camtest`.

**Files:**
- Modify: `camera.h` (enum, struct field, one decl)
- Modify: `camera.c` (init, `camera_update`, `camera_proj`, `camera_ray`, new `camera_enter_rts`)
- Test: `camera_test.c` (append RTS cases)

- [ ] **Step 1: Add the mode, the extent field, and the enter decl in `camera.h`**

In `camera.h`, change the `CameraMode` enum (currently ends at `CAMERA_ORBIT`, line 17-22) to add `CAMERA_RTS`:

```c
typedef enum {
    CAMERA_WALK = 0,   /* first-person: movement locked to the ground plane;
                          height settles to CAMERA_EYE_HEIGHT while moving */
    CAMERA_FLY,        /* first-person: full-3D movement + vertical */
    CAMERA_ORBIT,      /* orbit a target: drag to rotate, scroll to dolly */
    CAMERA_RTS         /* top-down editor: angled orthographic, pan + zoom,
                          fixed orientation (the spatial-tree editor view) */
} CameraMode;
```

In the `Camera` struct (line 24-36), add one field after `ground_y`:

```c
    float      ortho_h;      /* CAMERA_RTS: half-height of the ortho view box,
                                world units; zoom changes this */
```

After the `camera_enter_*` declarations (line 56-58), add:

```c
void camera_enter_rts(Camera *c, vec3 center, float radius);  /* angled top-down, framing a disc of `radius` about `center` */
```

- [ ] **Step 2: Initialize `ortho_h` and add RTS constants in `camera.c`**

In `camera.c`, after the existing `#define CAMERA_SETTLE_RATE ...` block (line 8-11), add:

```c
#define CAMERA_RTS_PITCH (-50.0f)   /* degrees: looking down at the rooms */
#define CAMERA_RTS_YAW   (-90.0f)   /* degrees: forward toward -Z (north up) */
#define CAMERA_RTS_BACK   80.0f     /* how far back along -forward to sit */
#define CAMERA_RTS_PAN    1.5f      /* pan units/sec = this * ortho_h (even feel at any zoom) */
#define CAMERA_RTS_ZOOM   0.1f      /* fraction of ortho_h per scroll notch */
#define CAMERA_RTS_MIN_H  5.0f
#define CAMERA_RTS_MAX_H  200.0f
```

In `camera_init` (line 15-25), after `c->ground_y = 0.0f;` add:

```c
    c->ortho_h    = 20.0f;       /* a sane default until camera_enter_rts frames the scene */
```

- [ ] **Step 3: Branch `camera_update` for RTS (pan + zoom, no look)**

In `camera.c`, at the very TOP of `camera_update` (line 37, before the `look applies in every mode` block at line 41-46), insert the RTS branch so the fixed orientation never receives look deltas:

```c
void camera_update(Camera *c, const CameraInput *in, float dt) {
    vec3  fwd, right, move;
    float limit = sol_radians(89.0f);

    if (c->mode == CAMERA_RTS) {          /* top-down editor: pan + zoom, fixed angle */
        vec3 pfwd, pright, pmove;
        c->ortho_h -= in->zoom * CAMERA_RTS_ZOOM * c->ortho_h;   /* scroll up = zoom in */
        if (c->ortho_h < CAMERA_RTS_MIN_H) c->ortho_h = CAMERA_RTS_MIN_H;
        if (c->ortho_h > CAMERA_RTS_MAX_H) c->ortho_h = CAMERA_RTS_MAX_H;
        pfwd   = camera_forward(c); pfwd.y = 0.0f; pfwd = vec3_normalize(pfwd);
        pright = vec3_normalize(vec3_cross(pfwd, WORLD_UP));
        pmove  = vec3_make(0.0f, 0.0f, 0.0f);
        if (in->forward) pmove = vec3_add(pmove, pfwd);
        if (in->back)    pmove = vec3_sub(pmove, pfwd);
        if (in->right)   pmove = vec3_add(pmove, pright);
        if (in->left)    pmove = vec3_sub(pmove, pright);
        if (vec3_dot(pmove, pmove) > 0.0f) {
            pmove  = vec3_normalize(pmove);
            c->pos = vec3_add(c->pos, vec3_scale(pmove, CAMERA_RTS_PAN * c->ortho_h * dt));
        }
        return;
    }

    /* look applies in every mode (FP mouse/keys, or orbit drag); clamp pitch so
       look_at never degenerates */
    c->yaw   += in->look_dx;
    /* ... rest unchanged ... */
```

(Keep the existing body from line 43 onward exactly as-is.)

- [ ] **Step 4: Ortho branch in `camera_proj`**

Replace `camera_proj` (line 132-134) with:

```c
mat4 camera_proj(const Camera *c, float aspect) {
    if (c->mode == CAMERA_RTS) {
        float hh = c->ortho_h, hw = c->ortho_h * aspect;
        return mat4_ortho(-hw, hw, -hh, hh, 0.1f, 1000.0f);
    }
    return mat4_perspective(c->fov, aspect, 0.1f, 100.0f);
}
```

- [ ] **Step 5: Ortho branch in `camera_ray` (parallel rays)**

Replace `camera_ray` (line 140-151) with:

```c
Ray camera_ray(const Camera *c, float ndc_x, float ndc_y, float aspect) {
    vec3  fwd   = camera_forward(c);
    vec3  right = vec3_normalize(vec3_cross(fwd, WORLD_UP));
    vec3  up    = vec3_cross(right, fwd);
    Ray   r;
    if (c->mode == CAMERA_RTS) {          /* parallel projection: dir constant, origin slides */
        float hh = c->ortho_h, hw = c->ortho_h * aspect;
        r.origin = vec3_add(c->pos,
                     vec3_add(vec3_scale(right, ndc_x * hw),
                              vec3_scale(up,    ndc_y * hh)));
        r.dir = fwd;                      /* already unit length */
        return r;
    }
    {
        float th = tanf(c->fov * 0.5f);
        r.origin = c->pos;
        r.dir = vec3_normalize(vec3_add(vec3_add(fwd,
                    vec3_scale(right, ndc_x * th * aspect)),
                    vec3_scale(up,    ndc_y * th)));
    }
    return r;
}
```

- [ ] **Step 6: Add `camera_enter_rts`**

In `camera.c`, after `camera_enter_fp` (line 106-108), add:

```c
/* Enter the top-down editor view: a fixed angled-orthographic vantage that
   frames a disc of `radius` about `center`. Sits BACK along -forward so the
   whole disc is in front, within the ortho far plane. */
void camera_enter_rts(Camera *c, vec3 center, float radius) {
    c->mode    = CAMERA_RTS;
    c->yaw     = sol_radians(CAMERA_RTS_YAW);
    c->pitch   = sol_radians(CAMERA_RTS_PITCH);
    c->ortho_h = (radius > CAMERA_RTS_MIN_H) ? radius : CAMERA_RTS_MIN_H;
    c->pos     = vec3_sub(center, vec3_scale(camera_forward(c), CAMERA_RTS_BACK));
}
```

- [ ] **Step 7: Append RTS tests to `camera_test.c`**

In `camera_test.c`, just before the final `printf("camera_test: OK\n");` (line 201), insert:

```c
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
```

- [ ] **Step 8: Build and test**

Run: `./build.sh camtest && ./camera_test`
Expected: ends with `camera_test: OK` (and the new `rts ...` lines print).

Run the gauntlet: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: `c89check: PASS`, both builds report `built ...`.

- [ ] **Step 9: Commit**

```bash
git add camera.h camera.c camera_test.c
git commit -m "$(printf 'editor: CAMERA_RTS angled-ortho mode (proj, ray, pan/zoom)\n\nNew camera mode for the top-down editor: orthographic projection\nbranch, parallel pick-ray, fixed-orientation pan+zoom, and\ncamera_enter_rts framing. Headless camtest covers enter/pan/zoom/\nparallel-ray.\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 2: Editor module scaffold + pure geometry

Create the `editor` module with the `Editor` struct, the zone/action enums, and the four pure functions (`editor_room_rect`, `editor_classify`, `editor_resize_axis`, `editor_can_connect`). Wire a new `editortest` build target and add `editor.c` to every build. Unit-test the geometry.

**Files:**
- Create: `editor.h`
- Create: `editor.c`
- Create: `editor_test.c`
- Modify: `build.sh` (new `editortest` target; add `editor.c` to `c89check`, `metal`, `asan`, and the default debug build)

- [ ] **Step 1: Write `editor.h`**

Create `editor.h`:

```c
/* editor.h — the top-down spatial-tree editor (item 3 of the fs-tree arc).
   A headless interaction core: pure footprint geometry + resize math + connect
   validation (unit-tested), the per-frame interaction state machine driven by
   ortho-camera rays, and the Editor state struct that lives in AppState. No
   GLFW, no GL, no AppState — main.c owns the glue (cursor, overlay, rebuild). */
#ifndef SOL_EDITOR_H
#define SOL_EDITOR_H

#include "sol_base.h"
#include "sol_types.h"
#include "scene.h"
#include "camera.h"

#define EDITOR_GRAB_BAND 0.6f   /* world-unit band around a wall = resize handle */
#define EDITOR_MIN_SIZE  3.0f   /* a room footprint side can't shrink below this */
#define EDITOR_PORT_LIFT 2.6f   /* connection node hovers this far above the floor */
#define EDITOR_PORT_NDC  0.05f  /* port grab radius, in NDC half-units */

/* A room footprint on its floor plane, world space. cx/cz = center,
   hw/hd = half width (X) / half depth (Z), floor_y = world Y of the floor. */
typedef struct { float cx, cz, hw, hd, floor_y; } RoomRect;

/* Which part of a footprint a ground point falls on (axis-named: P=+, N=-). */
typedef enum {
    EDIT_ZONE_NONE = 0,
    EDIT_ZONE_BODY,
    EDIT_ZONE_EDGE_XP, EDIT_ZONE_EDGE_XN,
    EDIT_ZONE_EDGE_ZP, EDIT_ZONE_EDGE_ZN,
    EDIT_ZONE_CORNER_XPZP, EDIT_ZONE_CORNER_XPZN,
    EDIT_ZONE_CORNER_XNZP, EDIT_ZONE_CORNER_XNZN
} EditZone;

/* What the editor is doing between press and release. */
typedef enum { EDIT_IDLE = 0, EDIT_MOVE, EDIT_RESIZE, EDIT_CONNECT } EditAction;

/* The editor's whole state (lives in AppState). Zero = inactive, idle. */
typedef struct {
    sol_bool   active;
    sol_bool   was_active;     /* main.c edge-detects enter/exit on this */
    Camera     saved_cam;      /* first-person camera, restored on exit */
    EditAction action;
    sol_u32    room;           /* room parent handle under manipulation */
    EditZone   zone;           /* RESIZE: which handle */
    sol_u32    selected_wk;    /* walkway selected for delete; 0 = none */
    sol_u32    connect_from;   /* CONNECT: source room */
    vec3       grab_off;       /* MOVE: room center - grab ground point (XZ) */
    vec3       cursor_world;   /* latest ground point (rubber-band end) */
    sol_bool   dirty;          /* geometry changed -> re-thread this frame */
    sol_bool   commit;         /* interaction ended -> save */
} Editor;

/* ---- pure geometry (headless, unit-tested) ---- */
RoomRect editor_room_rect(Scene *s, sol_u32 room);
EditZone editor_classify(RoomRect r, float gx, float gz, float band);
void     editor_resize_axis(float center, float half, int sign,
                            float face_world, float min_size,
                            float *new_center, float *new_half);
sol_bool editor_can_connect(Scene *s, sol_u32 a, sol_u32 b);

/* ---- scene ops (headless, unit-tested) ---- */
sol_u32  editor_connect(Scene *s, sol_u32 a, sol_u32 b);   /* new walkway, 0 if invalid */
void     editor_disconnect(Scene *s, sol_u32 walkway);
void     editor_apply_move(Scene *s, sol_u32 room, float cx, float cz);
void     editor_apply_resize(Scene *s, sol_u32 room, EditZone zone,
                             float gx, float gz);

/* ---- cursor-driven interaction (built in Task 4; verified live) ---- */
void     editor_press(Editor *e, Scene *s, const Camera *c,
                      float ndc_x, float ndc_y, float aspect);
void     editor_drag(Editor *e, Scene *s, const Camera *c,
                     float ndc_x, float ndc_y, float aspect);
void     editor_release(Editor *e, Scene *s, const Camera *c,
                        float ndc_x, float ndc_y, float aspect);
void     editor_delete_selected(Editor *e, Scene *s);

#endif /* SOL_EDITOR_H */
```

- [ ] **Step 2: Write `editor.c` with ONLY the pure geometry (the rest comes in Tasks 3-4)**

Create `editor.c`:

```c
/* editor.c — see editor.h. Pure CPU; no GLFW, no GL, no AppState. */

#include "editor.h"
#include "mesh.h"       /* mesh_ref_param */
#include "sol_math.h"

#include <string.h>     /* strcmp, memset */

/* The room footprint: center + floor from the parent-0 anchor's pos, w/d from
   the "room" shell child's mesh_params. */
RoomRect editor_room_rect(Scene *s, sol_u32 room) {
    RoomRect     r;
    SceneObject *ro = scene_get(s, room);
    sol_u32      i;
    float        w = 10.0f, d = 10.0f;
    r.cx = r.cz = r.floor_y = 0.0f; r.hw = r.hd = 5.0f;
    if (!ro) return r;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        if (o->parent == room && o->mesh_ref && strcmp(o->mesh_ref, "room") == 0) {
            w = mesh_ref_param("room", o->mesh_params, o->mesh_param_count, "w");
            d = mesh_ref_param("room", o->mesh_params, o->mesh_param_count, "d");
            break;
        }
    }
    r.cx = ro->pos.x; r.cz = ro->pos.z; r.floor_y = ro->pos.y;
    r.hw = 0.5f * w;  r.hd = 0.5f * d;
    return r;
}

/* Classify a ground point against a footprint, with a grab band (world units)
   straddling each wall. Outside (footprint + band) -> NONE. */
EditZone editor_classify(RoomRect r, float gx, float gz, float band) {
    float dx = gx - r.cx, dz = gz - r.cz;
    int   xp, xn, zp, zn;
    if (dx >  r.hw + band || dx < -r.hw - band) return EDIT_ZONE_NONE;
    if (dz >  r.hd + band || dz < -r.hd - band) return EDIT_ZONE_NONE;
    xp = (dx >  r.hw - band);
    xn = (dx < -r.hw + band);
    zp = (dz >  r.hd - band);
    zn = (dz < -r.hd + band);
    if (xp && zp) return EDIT_ZONE_CORNER_XPZP;
    if (xp && zn) return EDIT_ZONE_CORNER_XPZN;
    if (xn && zp) return EDIT_ZONE_CORNER_XNZP;
    if (xn && zn) return EDIT_ZONE_CORNER_XNZN;
    if (xp) return EDIT_ZONE_EDGE_XP;
    if (xn) return EDIT_ZONE_EDGE_XN;
    if (zp) return EDIT_ZONE_EDGE_ZP;
    if (zn) return EDIT_ZONE_EDGE_ZN;
    return EDIT_ZONE_BODY;
}

/* Resize one axis with the OPPOSITE wall held fixed. sign = which face moves
   (+1 = the +axis wall, -1 = the -axis wall). face_world = cursor coord on
   that axis. Clamps so the box keeps at least min_size. */
void editor_resize_axis(float center, float half, int sign, float face_world,
                        float min_size, float *new_center, float *new_half) {
    float opp  = center - (float)sign * half;   /* the wall that stays put */
    float face = face_world;
    if (sign > 0) { if (face < opp + min_size) face = opp + min_size; }
    else          { if (face > opp - min_size) face = opp - min_size; }
    *new_half   = 0.5f * (face > opp ? face - opp : opp - face);
    *new_center = 0.5f * (opp + face);
}

/* Can we add a walkway between rooms a and b? Both must be rooms, distinct, and
   not already joined by a walkway. */
sol_bool editor_can_connect(Scene *s, sol_u32 a, sol_u32 b) {
    sol_u32 i;
    if (a == 0 || b == 0 || a == b) return SOL_FALSE;
    if (!scene_meta_get(s, a, "room_type")) return SOL_FALSE;
    if (!scene_meta_get(s, b, "room_type")) return SOL_FALSE;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        sol_u32      x = 0, y = 0, j;
        if (!o->mesh_ref || strcmp(o->mesh_ref, "walkway") != 0) continue;
        for (j = 0; j < o->rel_count; j++) {
            if (strcmp(o->relations[j].type, "connects") != 0) continue;
            if      (x == 0) x = o->relations[j].target;
            else if (y == 0) y = o->relations[j].target;
        }
        if ((x == a && y == b) || (x == b && y == a)) return SOL_FALSE;
    }
    return SOL_TRUE;
}
```

- [ ] **Step 3: Write `editor_test.c` (geometry cases)**

Create `editor_test.c`:

```c
#include "editor.h"
#include "scene.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* a fs-tree room: parent empty (room_type) + "room" shell child w x d. */
static sol_u32 add_room(Scene *s, float x, float y, float z, float w, float d) {
    Mesh    empty;
    sol_u32 parent, shell;
    float   p[8];
    memset(&empty, 0, sizeof empty);
    parent = scene_add(s, 0, empty, vec3_make(x, y, z), quat_identity(),
                       vec3_make(1.0f, 1.0f, 1.0f));
    scene_meta_set(s, parent, "room_type", "mirror");
    shell = scene_add(s, parent, empty, vec3_make(0.0f, 0.0f, 0.0f),
                      quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
    scene_mesh_ref_set(s, shell, "room");
    p[0] = w; p[1] = d; p[2] = 3.0f; p[3] = 1.0f; p[4] = 1.0f; p[5] = 1.0f;
    p[6] = 1.0f; p[7] = 0.0f;
    scene_mesh_params_set(s, shell, p, 8);
    return parent;
}

int main(void) {
    /* room_rect reads center+floor from the anchor and w/d from the shell */
    {
        Scene s; sol_u32 a; RoomRect r;
        scene_init(&s);
        a = add_room(&s, 2.0f, 12.0f, -3.0f, 8.0f, 6.0f);
        r = editor_room_rect(&s, a);
        CHECK(fabs((double)(r.cx - 2.0f)) < 1e-4);
        CHECK(fabs((double)(r.cz + 3.0f)) < 1e-4);
        CHECK(fabs((double)(r.floor_y - 12.0f)) < 1e-4);
        CHECK(fabs((double)(r.hw - 4.0f)) < 1e-4);
        CHECK(fabs((double)(r.hd - 3.0f)) < 1e-4);
        scene_free(&s);
    }

    /* classify: body, each edge, a corner, and outside */
    {
        RoomRect r; r.cx = 0.0f; r.cz = 0.0f; r.hw = 5.0f; r.hd = 5.0f; r.floor_y = 0.0f;
        CHECK(editor_classify(r,  0.0f,  0.0f, 0.6f) == EDIT_ZONE_BODY);
        CHECK(editor_classify(r,  5.0f,  0.0f, 0.6f) == EDIT_ZONE_EDGE_XP);
        CHECK(editor_classify(r, -5.0f,  0.0f, 0.6f) == EDIT_ZONE_EDGE_XN);
        CHECK(editor_classify(r,  0.0f,  5.0f, 0.6f) == EDIT_ZONE_EDGE_ZP);
        CHECK(editor_classify(r,  0.0f, -5.0f, 0.6f) == EDIT_ZONE_EDGE_ZN);
        CHECK(editor_classify(r,  5.0f,  5.0f, 0.6f) == EDIT_ZONE_CORNER_XPZP);
        CHECK(editor_classify(r,  9.0f,  0.0f, 0.6f) == EDIT_ZONE_NONE);
    }

    /* resize +X face: the -X wall stays at -5, width grows, center shifts to +2 */
    {
        float nc = 0.0f, nh = 0.0f;
        editor_resize_axis(0.0f, 5.0f, +1, 9.0f, 3.0f, &nc, &nh);
        CHECK(fabs((double)(nc - 2.0f)) < 1e-4);   /* center */
        CHECK(fabs((double)(nh - 7.0f)) < 1e-4);   /* half */
        CHECK(fabs((double)((nc - nh) + 5.0f)) < 1e-4);  /* -X wall fixed at -5 */
    }
    /* resize -X face: the +X wall stays at +5 */
    {
        float nc = 0.0f, nh = 0.0f;
        editor_resize_axis(0.0f, 5.0f, -1, -9.0f, 3.0f, &nc, &nh);
        CHECK(fabs((double)((nc + nh) - 5.0f)) < 1e-4);  /* +X wall fixed at +5 */
    }
    /* resize min-size clamp: dragging the +X face past the opposite wall */
    {
        float nc = 0.0f, nh = 0.0f;
        editor_resize_axis(0.0f, 5.0f, +1, -10.0f, 3.0f, &nc, &nh);
        CHECK(fabs((double)(2.0f * nh - 3.0f)) < 1e-4);  /* width clamped to min */
    }

    /* can_connect: self/duplicate/non-room guards */
    {
        Scene s; sol_u32 a, b, c; Mesh empty;
        scene_init(&s);
        a = add_room(&s, 0.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        b = add_room(&s, 20.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        memset(&empty, 0, sizeof empty);
        c = scene_add(&s, 0, empty, vec3_make(0,0,0), quat_identity(), vec3_make(1,1,1));  /* not a room */
        CHECK(editor_can_connect(&s, a, b) == SOL_TRUE);
        CHECK(editor_can_connect(&s, a, a) == SOL_FALSE);
        CHECK(editor_can_connect(&s, a, c) == SOL_FALSE);
        /* once joined, the pair is refused */
        {
            sol_u32 wk = scene_add(&s, 0, empty, vec3_make(0,0,0), quat_identity(), vec3_make(1,1,1));
            scene_mesh_ref_set(&s, wk, "walkway");
            scene_rel_add(&s, wk, "connects", a);
            scene_rel_add(&s, wk, "connects", b);
            CHECK(editor_can_connect(&s, a, b) == SOL_FALSE);
            CHECK(editor_can_connect(&s, b, a) == SOL_FALSE);
        }
        scene_free(&s);
    }

    if (fails == 0) printf("editor_test: OK\n");
    return fails ? 1 : 0;
}
```

- [ ] **Step 4: Add the `editortest` target and `editor.c` to the builds in `build.sh`**

In `build.sh`, after the `routetest` block (ends at line 113), add a new block:

```sh
# editortest: the top-down editor's geometry + scene ops (no GL). Links the
# scene spine + mesh.c (RoomOpening/params) + camera.c (ortho rays).
if [ "$MODE" = "editortest" ]; then
    set -x
    clang -std=c89 -pedantic-errors -Werror -g -fsanitize=address,undefined \
        editor.c editor_test.c scene.c material.c mesh.c flora.c rock.c gothic.c sweep.c nid.c sol_math.c camera.c \
        -o editor_test
    echo "built ./editor_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi
```

Add `editor.c` to the SOURCE LIST of the four full builds (append it right after `route.c` in each):
- `c89check` (line 16)
- `metal` (line 264)
- `asan` (line 280)
- the default/release build (line 295)

Example for the default build (line 295): the list `... fuzzy.c palette.c route.c` becomes `... fuzzy.c palette.c route.c editor.c`. Do the same in the other three.

- [ ] **Step 5: Build and test**

Run: `./build.sh editortest && ./editor_test`
Expected: ends with `editor_test: OK`.

Run the gauntlet: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: all pass. (`editor.c` now compiles into every build, though nothing calls the cursor functions yet — they're declared but defined in Tasks 3-4; ensure the file at least compiles. Because Tasks 3-4 add the remaining `editor.c` definitions, at THIS task `editor.c` defines only the pure functions. The DEBUG/METAL builds link `editor.c`; main.c does not yet reference any editor symbol, so undefined cursor functions are fine — they're simply absent, not referenced. Confirm the gauntlet passes.)

- [ ] **Step 6: Commit**

```bash
git add editor.h editor.c editor_test.c build.sh
git commit -m "$(printf 'editor: module scaffold + footprint geometry\n\nNew headless editor module: Editor state struct, zone/action enums,\nand the pure functions room_rect/classify/resize_axis/can_connect.\nNew editortest target; editor.c added to every build. Unit tests\ncover classification, the opposite-wall-fixed resize math + min\nclamp, and connect guards.\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 3: Editor scene ops (connect / disconnect / move / resize)

Add the scene-mutating ops to `editor.c` and test them headlessly: connecting adds a walkway with two `connects` edges, disconnecting removes it, moving writes the anchor pos, and resizing writes the shell params + center with the opposite wall fixed in WORLD space.

**Files:**
- Modify: `editor.c` (append the four ops)
- Modify: `editor_test.c` (append op tests)

- [ ] **Step 1: Append the scene ops to `editor.c`**

At the end of `editor.c`, add:

```c
/* Create a walkway joining rooms a and b (parent-0, two "connects" edges).
   Returns the new walkway handle, or 0 if the pair is invalid. */
sol_u32 editor_connect(Scene *s, sol_u32 a, sol_u32 b) {
    Mesh    empty;
    sol_u32 wk;
    if (!editor_can_connect(s, a, b)) return 0;
    memset(&empty, 0, sizeof empty);
    wk = scene_add(s, 0, empty, vec3_make(0.0f, 0.0f, 0.0f), quat_identity(),
                   vec3_make(1.0f, 1.0f, 1.0f));
    scene_mesh_ref_set(s, wk, "walkway");
    scene_rel_add(s, wk, "connects", a);
    scene_rel_add(s, wk, "connects", b);
    return wk;
}

/* Remove a walkway (its connects edges go with it). No-op if not a walkway. */
void editor_disconnect(Scene *s, sol_u32 walkway) {
    SceneObject *o = scene_get(s, walkway);
    if (o && o->mesh_ref && strcmp(o->mesh_ref, "walkway") == 0)
        scene_remove(s, walkway);
}

/* Move a room: write its anchor's world XZ (anchors are parent-0). */
void editor_apply_move(Scene *s, sol_u32 room, float cx, float cz) {
    SceneObject *o = scene_get(s, room);
    if (!o) return;
    o->pos.x = cx;
    o->pos.z = cz;
}

/* Resize a room by dragging zone's wall(s) to the ground point (gx,gz), keeping
   the opposite wall(s) fixed. Writes the new center to the anchor and the new
   w/d to the shell child's params (h + wall flags preserved). */
void editor_apply_resize(Scene *s, sol_u32 room, EditZone zone, float gx, float gz) {
    RoomRect     r  = editor_room_rect(s, room);
    SceneObject *ro = scene_get(s, room);
    sol_u32      i;
    float        ncx = r.cx, ncz = r.cz, nhw = r.hw, nhd = r.hd;
    int          tx = 0, tz = 0, sx = 0, sz = 0;
    if (!ro) return;
    switch (zone) {
        case EDIT_ZONE_EDGE_XP:     tx = 1; sx =  1; break;
        case EDIT_ZONE_EDGE_XN:     tx = 1; sx = -1; break;
        case EDIT_ZONE_EDGE_ZP:     tz = 1; sz =  1; break;
        case EDIT_ZONE_EDGE_ZN:     tz = 1; sz = -1; break;
        case EDIT_ZONE_CORNER_XPZP: tx = tz = 1; sx =  1; sz =  1; break;
        case EDIT_ZONE_CORNER_XPZN: tx = tz = 1; sx =  1; sz = -1; break;
        case EDIT_ZONE_CORNER_XNZP: tx = tz = 1; sx = -1; sz =  1; break;
        case EDIT_ZONE_CORNER_XNZN: tx = tz = 1; sx = -1; sz = -1; break;
        default: return;
    }
    if (tx) editor_resize_axis(r.cx, r.hw, sx, gx, EDITOR_MIN_SIZE, &ncx, &nhw);
    if (tz) editor_resize_axis(r.cz, r.hd, sz, gz, EDITOR_MIN_SIZE, &ncz, &nhd);
    ro->pos.x = ncx;
    ro->pos.z = ncz;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        if (o->parent == room && o->mesh_ref && strcmp(o->mesh_ref, "room") == 0) {
            float p[8];
            int   k;
            for (k = 0; k < 8; k++)
                p[k] = (k < o->mesh_param_count) ? o->mesh_params[k] : 0.0f;
            if (o->mesh_param_count < 8) {       /* defensive: shells are made with 8 */
                p[2] = 3.0f; p[3] = 1.0f; p[4] = 1.0f; p[5] = 1.0f; p[6] = 1.0f; p[7] = 0.0f;
            }
            p[0] = 2.0f * nhw;
            p[1] = 2.0f * nhd;
            scene_mesh_params_set(s, o->handle, p, 8);
            break;
        }
    }
}
```

- [ ] **Step 2: Append op tests to `editor_test.c`**

In `editor_test.c`, before the final `if (fails == 0) ...` line, add:

```c
    /* connect adds a walkway joining the pair; disconnect removes it */
    {
        Scene s; sol_u32 a, b, wk;
        scene_init(&s);
        a = add_room(&s, 0.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        b = add_room(&s, 20.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        wk = editor_connect(&s, a, b);
        CHECK(wk != 0);
        CHECK(editor_can_connect(&s, a, b) == SOL_FALSE);   /* now joined */
        CHECK(editor_connect(&s, a, b) == 0);               /* duplicate refused */
        editor_disconnect(&s, wk);
        CHECK(editor_can_connect(&s, a, b) == SOL_TRUE);    /* free again */
        scene_free(&s);
    }

    /* apply_move writes the anchor's world XZ */
    {
        Scene s; sol_u32 a; SceneObject *o;
        scene_init(&s);
        a = add_room(&s, 0.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        editor_apply_move(&s, a, 7.0f, -4.0f);
        o = scene_get(&s, a);
        CHECK(o != 0);
        CHECK(fabs((double)(o->pos.x - 7.0f)) < 1e-4);
        CHECK(fabs((double)(o->pos.z + 4.0f)) < 1e-4);
        CHECK(fabs((double)(o->pos.y - 12.0f)) < 1e-4);   /* height untouched */
        scene_free(&s);
    }

    /* apply_resize dragging the +X wall to x=9: width 14, center +2, -X wall
       stays at -5 in WORLD space */
    {
        Scene s; sol_u32 a; RoomRect r; SceneObject *ro;
        scene_init(&s);
        a = add_room(&s, 0.0f, 12.0f, 0.0f, 10.0f, 10.0f);
        editor_apply_resize(&s, a, EDIT_ZONE_EDGE_XP, 9.0f, 0.0f);
        r  = editor_room_rect(&s, a);
        ro = scene_get(&s, a);
        CHECK(fabs((double)(2.0f * r.hw - 14.0f)) < 1e-3);     /* width grew */
        CHECK(fabs((double)(ro->pos.x - 2.0f)) < 1e-3);        /* center shifted */
        CHECK(fabs((double)((r.cx - r.hw) + 5.0f)) < 1e-3);    /* -X wall fixed */
        CHECK(fabs((double)(2.0f * r.hd - 10.0f)) < 1e-3);     /* depth untouched */
        scene_free(&s);
    }
```

- [ ] **Step 3: Build and test**

Run: `./build.sh editortest && ./editor_test`
Expected: ends with `editor_test: OK`.

Run the gauntlet: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: all pass.

- [ ] **Step 4: Commit**

```bash
git add editor.c editor_test.c
git commit -m "$(printf 'editor: scene ops (connect/disconnect/move/resize)\n\nThe four scene-mutating ops, headless-tested: connect adds a\nwalkway with two connects edges (duplicate refused), disconnect\nremoves it, move writes the anchor XZ, resize writes shell w/d +\ncenter with the opposite wall fixed in world space.\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 4: Cursor state machine + main.c wiring

Build the cursor-driven interaction in `editor.c` and wire the editor into `main.c`: the `Editor` field, the `G` toggle (and a palette "Disconnect selected"), camera enter/exit with save/restore, routing the mouse to the editor when active, suppressing other hotkeys, and driving `connections_rebuild`/`collide_rebuild`/`scene_save` from the `dirty`/`commit` flags. **Verified live** (the gauntlet is the automated bar).

**Files:**
- Modify: `editor.c` (cursor functions + their static helpers)
- Modify: `main.c` (`#include`, `Editor` field, two commands, registry rows, `read_input` wiring, dispatch guard)

- [ ] **Step 1: Add the cursor state machine to `editor.c`**

At the end of `editor.c`, add the static helpers and the four cursor functions:

```c
/* world ground point where the cursor ray meets the plane y = floor_y */
static sol_bool editor_ground_at(const Camera *c, float nx, float ny, float aspect,
                                 float floor_y, vec3 *out) {
    Ray   r = camera_ray(c, nx, ny, aspect);
    float t;
    if (!ray_vs_plane(r, vec3_make(0.0f, floor_y, 0.0f),
                      vec3_make(0.0f, 1.0f, 0.0f), &t)) return SOL_FALSE;
    *out = vec3_add(r.origin, vec3_scale(r.dir, t));
    return SOL_TRUE;
}

/* nearest room (parent handle) the cursor falls within (body or grab band).
   Fills zone + ground point. 0 if none. */
static sol_u32 editor_room_under(Scene *s, const Camera *c, float nx, float ny,
                                 float aspect, EditZone *zone_out, vec3 *gp_out) {
    sol_u32 i, best = 0;
    float   bestd = 1e30f;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        RoomRect     r;
        vec3         gp;
        EditZone     z;
        float        dx, dz, dd;
        if (o->mesh_ref) continue;                      /* anchors are empties */
        if (!scene_meta_get(s, o->handle, "room_type")) continue;
        r = editor_room_rect(s, o->handle);
        if (!editor_ground_at(c, nx, ny, aspect, r.floor_y, &gp)) continue;
        z = editor_classify(r, gp.x, gp.z, EDITOR_GRAB_BAND);
        if (z == EDIT_ZONE_NONE) continue;
        dx = gp.x - r.cx; dz = gp.z - r.cz; dd = dx * dx + dz * dz;
        if (dd < bestd) {
            bestd = dd; best = o->handle;
            if (zone_out) *zone_out = z;
            if (gp_out)   *gp_out   = gp;
        }
    }
    return best;
}

/* is the cursor over a room's connection node (projected to screen)? */
static sol_bool editor_port_hit(const Camera *c, RoomRect r, float nx, float ny,
                                float aspect) {
    vec3 pw = vec3_make(r.cx, r.floor_y + EDITOR_PORT_LIFT, r.cz);
    mat4 vp = mat4_mul(camera_proj(c, aspect), camera_view(c));
    vec3 ndc;
    float ex, ey;
    if (!mat4_project_point(vp, pw, &ndc)) return SOL_FALSE;
    ex = ndc.x - nx; ey = ndc.y - ny;
    return (sol_bool)(ex * ex + ey * ey <= EDITOR_PORT_NDC * EDITOR_PORT_NDC);
}

void editor_press(Editor *e, Scene *s, const Camera *c,
                  float nx, float ny, float aspect) {
    EditZone z = EDIT_ZONE_NONE;
    vec3     gp;
    sol_u32  room;
    e->action = EDIT_IDLE;
    room = editor_room_under(s, c, nx, ny, aspect, &z, &gp);
    if (room == 0) return;                          /* main.c may select a walkway */
    {
        RoomRect r = editor_room_rect(s, room);
        if (editor_port_hit(c, r, nx, ny, aspect)) {
            e->action       = EDIT_CONNECT;
            e->connect_from = room;
            e->cursor_world = gp;
            e->selected_wk  = 0;
            return;
        }
        e->room        = room;
        e->selected_wk = 0;
        if (z == EDIT_ZONE_BODY) {
            e->action   = EDIT_MOVE;
            e->grab_off = vec3_make(r.cx - gp.x, 0.0f, r.cz - gp.z);
        } else {
            e->action = EDIT_RESIZE;
            e->zone   = z;
        }
    }
}

void editor_drag(Editor *e, Scene *s, const Camera *c,
                 float nx, float ny, float aspect) {
    vec3     gp;
    RoomRect r;
    if (e->action == EDIT_IDLE) return;
    if (e->action == EDIT_CONNECT) {
        r = editor_room_rect(s, e->connect_from);
        if (editor_ground_at(c, nx, ny, aspect, r.floor_y, &gp)) e->cursor_world = gp;
        return;
    }
    r = editor_room_rect(s, e->room);
    if (!editor_ground_at(c, nx, ny, aspect, r.floor_y, &gp)) return;
    if (e->action == EDIT_MOVE) {
        editor_apply_move(s, e->room, gp.x + e->grab_off.x, gp.z + e->grab_off.z);
        e->dirty = SOL_TRUE;
    } else if (e->action == EDIT_RESIZE) {
        editor_apply_resize(s, e->room, e->zone, gp.x, gp.z);
        e->dirty = SOL_TRUE;
    }
}

void editor_release(Editor *e, Scene *s, const Camera *c,
                    float nx, float ny, float aspect) {
    if (e->action == EDIT_CONNECT) {
        EditZone z;
        vec3     gp;
        sol_u32  target = editor_room_under(s, c, nx, ny, aspect, &z, &gp);
        if (target != 0 && editor_connect(s, e->connect_from, target) != 0) {
            e->dirty = SOL_TRUE; e->commit = SOL_TRUE;
        }
    } else if (e->action == EDIT_MOVE || e->action == EDIT_RESIZE) {
        e->commit = SOL_TRUE;
    }
    e->action       = EDIT_IDLE;
    e->connect_from = 0;
}

void editor_delete_selected(Editor *e, Scene *s) {
    if (e->selected_wk != 0) {
        editor_disconnect(s, e->selected_wk);
        e->selected_wk = 0;
        e->dirty = SOL_TRUE; e->commit = SOL_TRUE;
    }
}
```

These reference `ray_vs_plane`, `mat4_mul`, `mat4_project_point`, `camera_proj`, `camera_view` — all already included via `sol_math.h` and `camera.h`. No new includes.

- [ ] **Step 2: `#include "editor.h"` and add the `Editor` field in `main.c`**

Add `#include "editor.h"` alongside the other module includes near the top of `main.c` (with `camera.h`, `scene.h`, etc.).

In the `AppState` struct (`typedef struct AppState {` at main.c:2482), add a field near the camera (after line 2483's `int fb_width, fb_height;` is fine, or beside other subsystem state):

```c
    Editor      editor;
```

`AppState state = {0};` (main.c:10436) zero-inits it, so the editor starts inactive.

- [ ] **Step 3: Add the toggle + disconnect commands in `main.c`**

Just above the `g_commands[]` table (before main.c:6633), add the framing helper and the two command callbacks:

```c
/* frame all rooms for the editor's entry vantage: centroid + farthest-room
   radius (with margin). */
static void editor_frame_rooms(AppState *st, vec3 *center, float *radius) {
    sol_u32 i;
    int     n = 0;
    vec3    c = vec3_make(0.0f, 0.0f, 0.0f);
    float   maxd = 0.0f;
    for (i = 0; i < st->scene.count; i++) {
        sol_u32 h = st->scene.objects[i].handle;
        if (!scene_meta_get(&st->scene, h, "room_type")) continue;
        c = vec3_add(c, object_world_pos(&st->scene, h));
        n++;
    }
    if (n > 0) c = vec3_scale(c, 1.0f / (float)n);
    else       c = vec3_make(0.0f, HOME_FLOOR_Y, 0.0f);
    for (i = 0; i < st->scene.count; i++) {
        sol_u32 h = st->scene.objects[i].handle;
        vec3    p;
        float   dx, dz, dd;
        if (!scene_meta_get(&st->scene, h, "room_type")) continue;
        p  = object_world_pos(&st->scene, h);
        dx = p.x - c.x; dz = p.z - c.z;
        dd = (float)sqrt((double)(dx * dx + dz * dz));
        if (dd > maxd) maxd = dd;
    }
    *center = c;
    *radius = maxd + 14.0f;
}

/* G / palette: toggle the top-down editor. The camera enter/exit + cursor mode
   happen in read_input on the active edge (it has the GLFWwindow). */
static void cmd_toggle_editor(AppState *st) {
    st->editor.active = (sol_bool)!st->editor.active;
}

static sol_bool can_disconnect(AppState *st) {
    return (sol_bool)(st->editor.active && st->editor.selected_wk != 0);
}

/* palette: remove the selected walkway. */
static void cmd_disconnect(AppState *st) {
    editor_delete_selected(&st->editor, &st->scene);
}
```

`object_world_pos` (main.c:2813), `HOME_FLOOR_Y` (main.c:6517), and `sqrt` are all already available above this point. (`HOME_FLOOR_Y` is defined at 6517, before 6633 — in scope.)

Add two rows to `g_commands[]` (main.c:6633-6656). Put the toggle after the "New root..." row and add the disconnect as palette-only:

```c
    { "New root...",                 NULL, 0,          cmd_new_root,          NULL,                  SOL_FALSE },
    { "Top-down editor",             "G", GLFW_KEY_G, cmd_toggle_editor,     NULL,                  SOL_FALSE },
    { "Disconnect selected",         NULL, 0,          cmd_disconnect,        can_disconnect,        SOL_FALSE }
```

(Add a comma after the existing `cmd_new_root` row's `}` — it is currently the last row with no trailing comma.)

- [ ] **Step 4: Handle editor enter/exit + cursor mode in `read_input`**

In `read_input`, after the command-dispatch loop (the `for (ci = 0; ci < G_COMMAND_COUNT; ci++)` block ending around main.c:6969), add the enter/exit edge handling. `w` is the GLFWwindow in scope here:

```c
        /* editor enter/exit: framed RTS camera in, saved first-person camera out */
        if (st->editor.active && !st->editor.was_active) {
            vec3  center;
            float radius;
            st->editor.saved_cam = st->camera;
            editor_frame_rooms(st, &center, &radius);
            camera_enter_rts(&st->camera, center, radius);
            st->editor.action      = EDIT_IDLE;
            st->editor.selected_wk = 0;
            st->selected_handle    = 0;
            glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            st->mouse_skip = 2;
        } else if (!st->editor.active && st->editor.was_active) {
            st->camera = st->editor.saved_cam;
            glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            st->mouse_skip = 2;
        }
        st->editor.was_active = st->editor.active;

        /* per-frame: drive re-thread + save from the editor's flags */
        if (st->editor.dirty) {
            connections_rebuild(st);
            collide_rebuild(&st->colliders, &st->scene);
            st->editor.dirty = SOL_FALSE;
        }
        if (st->editor.commit) {
            scene_save(&st->scene, "scene.stml");
            st->editor.commit = SOL_FALSE;
        }
```

- [ ] **Step 5: Suppress non-toggle hotkeys while the editor is active**

In the command dispatch loop (main.c:6960-6968), guard so only the editor toggle fires by hotkey while editing (the palette still reaches everything). Change the loop body's fire condition: before `now = glfwGetKey(...)`, add a skip:

```c
        for (ci = 0; ci < G_COMMAND_COUNT; ci++) {
            Command *cmd = &g_commands[ci];
            sol_bool now;
            if (cmd->key == 0) continue;                       /* palette-only */
            if (st->editor.active && cmd->run != cmd_toggle_editor)
                { cmd->was_down = SOL_FALSE; continue; }       /* editor: only G by key */
            now = glfwGetKey(w, cmd->key) == GLFW_PRESS;
            if (now && !cmd->was_down && (cmd->can_run == NULL || cmd->can_run(st)))
                cmd->run(st);
            cmd->was_down = now;
        }
```

- [ ] **Step 6: Route the mouse to the editor when active**

In `read_input`, the FP/orbit mouse handling is the block of three `if`s starting at the press handler (main.c:6783) through the release handler (ending just before `st->lmb_was_down = lmb;` at main.c:6953). Wrap that whole block in an `else` of a new editor branch. Insert BEFORE the press `if` at 6783:

```c
        if (st->editor.active) {                       /* ---- editor mouse ---- */
            int    ww, wh;
            float  nx, ny, aspect;
            glfwGetWindowSize(w, &ww, &wh);
            aspect = (wh > 0) ? (float)ww / (float)wh : 1.0f;
            nx = 2.0f * (float)mx / (float)ww - 1.0f;
            ny = 1.0f - 2.0f * (float)my / (float)wh;
            if (lmb && !st->lmb_was_down) {
                editor_press(&st->editor, &st->scene, &st->camera, nx, ny, aspect);
                if (st->editor.action == EDIT_IDLE) {
                    float   t;
                    sol_u32 hit = pick_at(st, w, nx, ny, &t);   /* rooms are pick-skipped: this finds a walkway */
                    st->editor.selected_wk = object_is_walkway(&st->scene, hit) ? hit : 0;
                    st->selected_handle    = st->editor.selected_wk;
                } else {
                    st->selected_handle = st->editor.room;       /* highlight the room */
                }
            } else if (lmb && st->lmb_was_down) {
                editor_drag(&st->editor, &st->scene, &st->camera, nx, ny, aspect);
            } else if (!lmb && st->lmb_was_down) {
                editor_release(&st->editor, &st->scene, &st->camera, nx, ny, aspect);
            }
        } else {
```

Then close that `else` with a `}` immediately before the existing `st->lmb_was_down = lmb;` line (main.c:6953). (The existing three `if` blocks become the `else` body, unchanged.)

- [ ] **Step 7: Don't accumulate mouse-look while editing**

The mouse-look accumulation (main.c ~6755-6766: `float dx = (float)(mx - st->mouse_last_x); ... in->look_dx += dx * MOUSE_SENSITIVITY;`) must not run in the editor (fixed orientation, visible cursor). Guard its enclosing condition with `&& !st->editor.active`. Find the `if (...)` that wraps the `in->look_dx += ...` lines and append `&& !st->editor.active` to it. (CAMERA_RTS in `camera_update` already ignores look deltas; this just avoids spurious accumulation and keeps the cursor free.)

- [ ] **Step 8: Add the Delete-key disconnect**

In `read_input`, near the other discrete key handling, add an edge-detected Delete/Backspace that removes the selected walkway when editing. Add a field `sol_bool editor_del_was;` to `AppState` (next to other `_was_down` flags around main.c:2645), then in `read_input`:

```c
        {
            sol_bool del_now = (sol_bool)(glfwGetKey(w, GLFW_KEY_DELETE) == GLFW_PRESS ||
                                          glfwGetKey(w, GLFW_KEY_BACKSPACE) == GLFW_PRESS);
            if (st->editor.active && del_now && !st->editor_del_was)
                editor_delete_selected(&st->editor, &st->scene);
            st->editor_del_was = del_now;
        }
```

(The `dirty`/`commit` flags this sets are drained by the per-frame handler from Step 4.)

- [ ] **Step 9: Build (gauntlet) — this task is verified live**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: `c89check: PASS`; both builds report `built ...`.

Run the touched headless suites to confirm no regressions: `./build.sh editortest && ./editor_test && ./build.sh camtest && ./camera_test`
Expected: both end with `... : OK`.

- [ ] **Step 10: Commit**

```bash
git add main.c editor.c
git commit -m "$(printf 'editor: cursor state machine + main.c wiring\n\nG (and palette) toggles the top-down editor: enter saves the\nfirst-person camera and frames the rooms in RTS, exit restores it.\nMouse routes to editor press/drag/release when active (move/resize/\nconnect); Delete + palette disconnect a selected walkway; other\nhotkeys suppressed while editing. dirty/commit flags drive\nconnections_rebuild + collide_rebuild + scene_save.\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

**Live-verify checklist (hand to the human):**
1. Build and run `./solarium`. Press **G** — the view lifts to an angled top-down over the rooms; the cursor appears.
2. WASD pans; scroll zooms (angle stays fixed).
3. Drag a room body — it slides; the walkway re-threads live; on release it saves (console: `placed`-style or no error). Reload (**L**) — the room stays where you left it.
4. Drag a room edge/corner — it resizes; the opposite wall stays put; doorways/walkways re-derive.
5. Drag from a room's connection node onto another room — a new walkway appears between them.
6. Click a walkway, press **Delete** (or palette → "Disconnect selected") — it's removed.
7. Press **G** again — back to first-person where you were standing.
8. (Overlay handles/ports aren't drawn yet — Task 5 adds them; for now hit-testing works by feel near the walls/center.)

---

## Task 5: Editor overlay (handles, ports, rubber-band) + selection

Draw the editor's affordances in the UI pass: each room's footprint outline, corner resize handles, the connection-node port, the rubber-band line during a connect, and a tint on the room/walkway under manipulation. This is glue (projects world → screen and emits `ui_*` quads/lines), verified live.

**Files:**
- Modify: `main.c` (a `editor_draw_overlay` function + a call in the UI pass)

- [ ] **Step 1: Add `editor_draw_overlay` to `main.c`**

Add this function above `render` (before main.c:9186). It mirrors the existing world→screen pattern at main.c:10270-10271:

```c
/* Project a world point to framebuffer pixels (UI space, y-down). FALSE if
   behind the camera. */
static sol_bool editor_world_to_screen(AppState *st, float aspect, vec3 wp,
                                        float *sx, float *sy) {
    mat4 vp = mat4_mul(camera_proj(&st->camera, aspect), camera_view(&st->camera));
    vec3 ndc;
    if (!mat4_project_point(vp, wp, &ndc)) return SOL_FALSE;
    *sx = (ndc.x * 0.5f + 0.5f) * (float)st->fb_width;
    *sy = (0.5f - ndc.y * 0.5f) * (float)st->fb_height;
    return SOL_TRUE;
}

/* The editor's 2D affordances, drawn inside the open UI batch. */
static void editor_draw_overlay(AppState *st) {
    float   aspect = (st->fb_height > 0)
                   ? (float)st->fb_width / (float)st->fb_height : 1.0f;
    float   hs = 6.0f;     /* half handle size, pixels */
    sol_u32 i;
    if (!st->editor.active) return;
    for (i = 0; i < st->scene.count; i++) {
        sol_u32  h = st->scene.objects[i].handle;
        RoomRect r;
        vec3     cw[4], port;
        float    px[4], py[4], psx, psy;
        int      k, ok = 1;
        sol_bool active_room;
        if (st->scene.objects[i].mesh_ref) continue;
        if (!scene_meta_get(&st->scene, h, "room_type")) continue;
        r = editor_room_rect(&st->scene, h);
        cw[0] = vec3_make(r.cx - r.hw, r.floor_y, r.cz - r.hd);
        cw[1] = vec3_make(r.cx + r.hw, r.floor_y, r.cz - r.hd);
        cw[2] = vec3_make(r.cx + r.hw, r.floor_y, r.cz + r.hd);
        cw[3] = vec3_make(r.cx - r.hw, r.floor_y, r.cz + r.hd);
        for (k = 0; k < 4; k++)
            if (!editor_world_to_screen(st, aspect, cw[k], &px[k], &py[k])) ok = 0;
        if (!ok) continue;
        active_room = (sol_bool)(st->editor.action != EDIT_IDLE && st->editor.room == h);
        /* footprint outline (brighter on the active room) */
        for (k = 0; k < 4; k++) {
            int n = (k + 1) & 3;
            if (active_room)
                ui_line(px[k], py[k], px[n], py[n], 2.0f, 1.0f, 0.85f, 0.30f, 0.95f);
            else
                ui_line(px[k], py[k], px[n], py[n], 1.5f, 0.65f, 0.72f, 0.80f, 0.85f);
        }
        /* corner resize handles */
        for (k = 0; k < 4; k++)
            ui_quad(px[k] - hs, py[k] - hs, 2.0f * hs, 2.0f * hs,
                    0.92f, 0.92f, 0.96f, 0.95f);
        /* connection node (port) above the center */
        port = vec3_make(r.cx, r.floor_y + EDITOR_PORT_LIFT, r.cz);
        if (editor_world_to_screen(st, aspect, port, &psx, &psy))
            ui_quad(psx - 5.0f, psy - 5.0f, 10.0f, 10.0f, 0.45f, 0.85f, 1.0f, 0.95f);
    }
    /* rubber-band: from the source room's port to the live cursor ground point */
    if (st->editor.action == EDIT_CONNECT && st->editor.connect_from != 0) {
        RoomRect r = editor_room_rect(&st->scene, st->editor.connect_from);
        vec3     port = vec3_make(r.cx, r.floor_y + EDITOR_PORT_LIFT, r.cz);
        float    ax, ay, bx, by;
        if (editor_world_to_screen(st, aspect, port, &ax, &ay) &&
            editor_world_to_screen(st, aspect, st->editor.cursor_world, &bx, &by))
            ui_line(ax, ay, bx, by, 2.0f, 0.45f, 0.85f, 1.0f, 0.9f);
    }
}
```

`ui_line`/`ui_quad` are declared in `ui.h` (already included by main.c). `EDITOR_PORT_LIFT`, `RoomRect`, and `editor_room_rect` come from `editor.h`.

- [ ] **Step 2: Call it in the UI pass**

In `render`, inside the open UI batch — between `ui_begin(state->fb_width, state->fb_height);` (main.c:10073) and `ui_end();` (main.c:10289) — add the call just before `ui_end();`:

```c
    editor_draw_overlay(state);
    ui_end();
```

(The selected room/walkway also lights up in the 3D pass via the existing `uHighlight` path, since Task 4 sets `st->selected_handle` to the room or walkway — no change needed there.)

- [ ] **Step 3: Build (gauntlet) — verified live**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: all pass.

- [ ] **Step 4: Commit**

```bash
git add main.c
git commit -m "$(printf 'editor: overlay — handles, ports, rubber-band\n\nDraws the editor affordances in the UI pass: each room footprint\noutline (brighter on the active room), corner resize handles, the\nconnection-node port, and the rubber-band line during a connect.\nProjects world->screen and emits ui_line/ui_quad.\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

**Live-verify checklist (hand to the human):**
1. Press **G**. Each room now shows a thin footprint outline, four white corner handles, and a cyan connection-node dot above its center.
2. Grabbing a corner (resize) vs. the body (move) vs. the port (connect) lines up with the drawn affordances.
3. Starting a connect drags a cyan rubber-band from the source port to the cursor; dropping on another room wires them.
4. The room being moved/resized has a brighter gold outline; a clicked walkway shows the gold 3D highlight.
5. Full loop: rearrange several rooms, resize one, add and delete a connection, reload (**L**) — the layout persists exactly.

---

## Final review

After all five tasks: dispatch a final code reviewer over the whole diff (`git diff main...HEAD`), then use **superpowers:finishing-a-development-branch** to fast-forward merge to `main` — only after the human confirms the live-verify checklists pass. Update memory (`spatial-filesystem-direction.md`, `MEMORY.md`) per the build process.

## Self-review notes (for the controller)

- **Spec coverage:** angled-ortho camera (T1) · direct-manipulation move/resize/connect (T2-T4) · live re-thread + autosave (T4 flags) · disconnect via Delete + palette (T4) · overlay handles/ports/rubber-band (T5) · rooms+walkways only, walkway-graph not the FS tree, no Y / no grid-snap (scope respected — none of those are built). Fixed orientation (T1 constants). New TUs editor.c/.h + editor_test.c (T2). Ortho pick-ray (T1). All spec sections map to a task.
- **Type consistency:** `Editor`, `RoomRect`, `EditZone`, `EditAction` defined once in editor.h and used verbatim in editor.c/main.c. `editor_room_rect`/`editor_classify`/`editor_resize_axis`/`editor_can_connect`/`editor_connect`/`editor_disconnect`/`editor_apply_move`/`editor_apply_resize`/`editor_press`/`editor_drag`/`editor_release`/`editor_delete_selected` signatures match between header, definitions, tests, and call sites. `camera_enter_rts` signature consistent T1↔T4.
- **Known caveat (acceptable for v1):** the sun cascades / depth reconstruction hardcode perspective near/far (main.c:8713, 9877, 9952) "to match camera_proj"; in the ortho editor view shadows may be slightly misfit — cosmetic, not a blocker. Note it at merge.
```
