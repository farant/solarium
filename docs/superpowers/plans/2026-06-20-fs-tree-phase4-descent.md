# FS-Tree Phase 4 — Descent — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Every subagent (implementers AND reviewers): READ-ONLY git only** — `git diff`/`log`/`show`, NEVER `checkout`/`switch`/`reset`/`stash`/`branch`/`commit` outside your one task commit. **Never stage `NOTES.stml` or `paper-picture.png`** (the user's files, always modified). Every commit message ends with the line `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. Strict C89: declarations at the top of each block, `/* */` comments only.

**Goal:** Descend the directory tree by carrying a folder tablet, aiming it at a wall to plant a door, and walking into the empty preview sub-room that floats out — which finalizes it (fills with the folder's contents; its subfolders become tablets).

**Architecture:** A new headless `descend.c`/`descend.h` module owns the descent logic: a wall-aim ray (camera ray vs the current room's 4 walls), "which room is this point in", planting (build a `room_type="preview"` room + a walkway + mark the folder card `planted`), and finalize (flip `preview→mirror`, normalize material, return the `source_path` for the caller to scan). It reuses the editor's `RoomRect`/`editor_room_rect` and stays free of GL and the filesystem (main.c runs `room_mirror_scan`). main.c wires it into carry: aiming a carried folder snaps it to the wall, `E` plants, and a per-frame walk-in check finalizes. Phase-3 routing is untouched — planting just adds a room + a walkway.

**Tech Stack:** C89 (strict), the existing carry / routing / mirror-scan systems, `editor.c` (RoomRect).

**Spec:** `docs/superpowers/specs/2026-06-20-fs-tree-phase4-descent-design.md`

---

## Background the implementer needs

- **Build gauntlet** (after every task): `./build.sh c89check && ./build.sh debug && ./build.sh metal`. All three must pass. `c89check` is `-std=c89 -pedantic-errors -Werror -Wall -Wextra`.
- **Headless tests** are standalone ASan binaries built by `./build.sh <target>`; run and check the final `... : OK` line.
- **Live-verify is the human's job** for Tasks 3–4 (GUI). Your bar there is the gauntlet + the headless suites passing.
- **Rooms** are parent-0 empty anchors carrying `meta["room_type"]` ("home"/"mirror"/**new "preview"**), `meta["source_path"]`, `meta["name"]`, plus a child with `mesh_ref="room"` and `mesh_params=[w,d,h,wn,we,ws,ww,ceil]`. The anchor's `pos` IS its world position.
- **`RoomRect`** (editor.h:21): `{ float cx, cz, hw, hd, floor_y; }`. **`editor_room_rect(Scene*, room)`** (editor.h:53) returns it (works for any parent-0 room, including preview).
- **`ROOM_WALL_N/E/S/W` = 0/1/2/3** (mesh.h). **`ROUTE_DOOR_W` 1.4 / `ROUTE_DOOR_H` 2.1** (route.h).
- **Folder cards**: `kind=KIND_FOLDER`, `content`=the folder's full path, `mesh_ref="card"`, parented to their room (created by `room_mirror_scan`, mirror.c).
- **`room_mirror_scan(Scene *s, sol_u32 room, const char *dirpath)`** (mirror.h:24) → int count; fills a room with file/folder cards (idempotent: skips cards whose `content` already exists).
- **`ray_vs_plane(Ray, vec3 point, vec3 normal, float *t)`** (sol_math.h) → FALSE if parallel/behind. **`Ray { vec3 origin, dir; }`** (sol_types.h).

---

## Task 1: `descend` module — wall-aim, room-at, door-point (pure geometry) + build wiring

**Files:** Create `descend.h`, `descend.c`, `descend_test.c`; Modify `build.sh`.

- [ ] **Step 1: Write `descend.h`**

```c
/* descend.h — fs-tree Phase 4 "descent": carry a folder tablet, aim it at a
   wall to plant a door, walk into the empty preview sub-room to finalize it.
   Headless: scene + geometry only (reuses editor's RoomRect), NO GL and NO
   filesystem — main.c runs room_mirror_scan after finalize returns a path. */
#ifndef SOL_DESCEND_H
#define SOL_DESCEND_H

#include "sol_base.h"
#include "sol_types.h"
#include "scene.h"
#include "editor.h"     /* RoomRect, editor_room_rect */

/* ---- pure geometry (headless, unit-tested) ---- */

/* The room (home/mirror/preview) whose footprint contains p (XZ inside, Y near
   its floor); 0 if none. For "which room am I in" + walk-in detection. */
sol_u32 descend_room_at(Scene *s, vec3 p);

/* Cast `ray` against the 4 interior wall planes of room rect `r`; on the nearest
   forward hit within a wall's run-span (kept off the corners) and around door
   height, return 1 and fill *wall (ROOM_WALL_*) + *offset (along-wall, from
   center). Else return 0. */
int  descend_wall_aim(RoomRect r, Ray ray, float door_h, int *wall, float *offset);

/* World point on wall `wall` at `offset` along it (y = the room's floor). */
vec3 descend_door_point(RoomRect r, int wall, float offset);

/* ---- scene ops (headless, unit-tested; built in Task 2) ---- */

/* Plant `folder_card` as a door on `parent_room`'s `wall` at `offset`: build a
   "preview" room outward from the wall (Y-nudged clear), a walkway joining them,
   and mark the card planted. Returns the new preview room handle, 0 if refused
   (not a folder / already planted / no content). */
sol_u32 descend_plant(Scene *s, sol_u32 parent_room, sol_u32 folder_card,
                      int wall, float offset);

/* Promote a preview room to a real mirror room: flip room_type preview->mirror
   and normalize its material. Returns the source_path to scan (the CALLER runs
   room_mirror_scan + rebuild + save), or NULL if it wasn't a preview
   (idempotent). */
const char *descend_finalize(Scene *s, sol_u32 preview_room);

#endif /* SOL_DESCEND_H */
```

- [ ] **Step 2: Write `descend.c` with ONLY the pure geometry (scene ops come in Task 2)**

```c
/* descend.c — see descend.h. Headless: no GL, no filesystem. */

#include "descend.h"
#include "mesh.h"       /* ROOM_WALL_* */
#include "route.h"      /* ROUTE_DOOR_W / ROUTE_DOOR_H */
#include "sol_math.h"

#include <string.h>     /* strcmp, strrchr, memset */

/* a point's room: home/mirror/preview anchor whose footprint contains it */
sol_u32 descend_room_at(Scene *s, vec3 p) {
    sol_u32 i, best = 0;
    float   bestd = 1e30f;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        const char  *rt;
        RoomRect     r;
        float        dy, dx, dz, dd;
        if (o->mesh_ref) continue;                 /* anchors are empties */
        rt = scene_meta_get(s, o->handle, "room_type");
        if (!rt) continue;
        if (strcmp(rt, "home") != 0 && strcmp(rt, "mirror") != 0 &&
            strcmp(rt, "preview") != 0) continue;
        r = editor_room_rect(s, o->handle);
        if (p.x < r.cx - r.hw || p.x > r.cx + r.hw) continue;
        if (p.z < r.cz - r.hd || p.z > r.cz + r.hd) continue;
        dy = p.y - r.floor_y;
        if (dy < -0.5f || dy > 5.0f) continue;     /* near this floor (room h ~3 + headroom) */
        dx = p.x - r.cx; dz = p.z - r.cz; dd = dx * dx + dz * dz;
        if (dd < bestd) { bestd = dd; best = o->handle; }
    }
    return best;
}

vec3 descend_door_point(RoomRect r, int wall, float offset) {
    if (wall == ROOM_WALL_N) return vec3_make(r.cx + offset, r.floor_y, r.cz - r.hd);
    if (wall == ROOM_WALL_S) return vec3_make(r.cx + offset, r.floor_y, r.cz + r.hd);
    if (wall == ROOM_WALL_E) return vec3_make(r.cx + r.hw, r.floor_y, r.cz + offset);
    return vec3_make(r.cx - r.hw, r.floor_y, r.cz + offset);   /* W */
}

int descend_wall_aim(RoomRect r, Ray ray, float door_h, int *wall, float *offset) {
    /* the 4 interior walls in ROOM_WALL_* order (N,E,S,W): a point on the plane,
       the inward normal, the run-axis half-span, and whether the run axis is X */
    struct { vec3 pt, n; float half; int runx; } w[4];
    int   bestw = -1, k;
    float bestt = 1e30f, besto = 0.0f;
    w[0].pt = vec3_make(r.cx, r.floor_y, r.cz - r.hd); w[0].n = vec3_make(0.0f,0.0f, 1.0f); w[0].half = r.hw; w[0].runx = 1; /* N */
    w[1].pt = vec3_make(r.cx + r.hw, r.floor_y, r.cz); w[1].n = vec3_make(-1.0f,0.0f,0.0f); w[1].half = r.hd; w[1].runx = 0; /* E */
    w[2].pt = vec3_make(r.cx, r.floor_y, r.cz + r.hd); w[2].n = vec3_make(0.0f,0.0f,-1.0f); w[2].half = r.hw; w[2].runx = 1; /* S */
    w[3].pt = vec3_make(r.cx - r.hw, r.floor_y, r.cz); w[3].n = vec3_make( 1.0f,0.0f,0.0f); w[3].half = r.hd; w[3].runx = 0; /* W */
    for (k = 0; k < 4; k++) {
        float t, run, lim;
        vec3  hit;
        if (!ray_vs_plane(ray, w[k].pt, w[k].n, &t)) continue;
        if (t <= 0.05f || t >= bestt) continue;
        hit = vec3_add(ray.origin, vec3_scale(ray.dir, t));
        if (hit.y < r.floor_y - 0.1f || hit.y > r.floor_y + door_h + 1.0f) continue;
        run = w[k].runx ? (hit.x - r.cx) : (hit.z - r.cz);
        lim = w[k].half - ROUTE_DOOR_W * 0.5f - 0.4f;   /* keep the door off the corners */
        if (lim < 0.0f) continue;
        if (run < -lim) run = -lim;
        if (run >  lim) run =  lim;
        bestt = t; bestw = k; besto = run;
    }
    if (bestw < 0) return 0;
    *wall = bestw;       /* 0/1/2/3 == ROOM_WALL_N/E/S/W */
    *offset = besto;
    return 1;
}
```

- [ ] **Step 3: Write `descend_test.c`**

```c
#include "descend.h"
#include "scene.h"
#include "mesh.h"       /* ROOM_WALL_* */
#include "sol_math.h"
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

/* a folder card parented to `room`, content = path. */
static sol_u32 add_folder_card(Scene *s, sol_u32 room, const char *path) {
    Mesh    empty;
    sol_u32 c;
    memset(&empty, 0, sizeof empty);
    c = scene_add(s, room, empty, vec3_make(0.0f, 0.0f, 0.0f), quat_identity(),
                  vec3_make(1.0f, 1.0f, 1.0f));
    scene_kind_set(s, c, KIND_FOLDER);
    scene_content_set(s, c, path);
    scene_mesh_ref_set(s, c, "card");
    return c;
}

int main(void) {
    /* wall-aim: a horizontal ray from room center toward +X hits the E wall at
       offset 0; aiming straight up hits no wall */
    {
        RoomRect r;
        Ray      ray;
        int      wall;
        float    off;
        r.cx = 0.0f; r.cz = 0.0f; r.hw = 5.0f; r.hd = 5.0f; r.floor_y = 0.0f;
        ray.origin = vec3_make(0.0f, 1.0f, 0.0f);
        ray.dir    = vec3_make(1.0f, 0.0f, 0.0f);
        CHECK(descend_wall_aim(r, ray, 2.1f, &wall, &off) == 1);
        CHECK(wall == ROOM_WALL_E);
        CHECK(fabs((double)off) < 1e-3);
        ray.dir = vec3_make(0.0f, 1.0f, 0.0f);       /* up the wall plane: parallel, no hit */
        CHECK(descend_wall_aim(r, ray, 2.1f, &wall, &off) == 0);
    }

    /* door-point on the E wall at offset +2 is at (cx+hw, floor, cz+2) */
    {
        RoomRect r;
        vec3     p;
        r.cx = 1.0f; r.cz = 1.0f; r.hw = 5.0f; r.hd = 5.0f; r.floor_y = 12.0f;
        p = descend_door_point(r, ROOM_WALL_E, 2.0f);
        CHECK(fabs((double)(p.x - 6.0f)) < 1e-4);    /* cx + hw */
        CHECK(fabs((double)(p.z - 3.0f)) < 1e-4);    /* cz + offset */
        CHECK(fabs((double)(p.y - 12.0f)) < 1e-4);
    }

    /* room-at: a point inside a room's footprint resolves to it; far away = 0 */
    {
        Scene s; sol_u32 a;
        scene_init(&s);
        a = add_room(&s, 0.0f, 12.0f, 0.0f, 10.0f, 10.0f);
        CHECK(descend_room_at(&s, vec3_make(0.0f, 12.5f, 0.0f)) == a);
        CHECK(descend_room_at(&s, vec3_make(50.0f, 12.5f, 0.0f)) == 0);
        CHECK(descend_room_at(&s, vec3_make(0.0f, 40.0f, 0.0f)) == 0);   /* wrong Y */
        scene_free(&s);
    }

    (void)add_folder_card;   /* used in Task 2's tests */
    if (fails == 0) printf("descend_test: OK\n");
    return fails ? 1 : 0;
}
```

- [ ] **Step 4: Add the `descendtest` target + `descend.c` to the builds in `build.sh`**

After the `editortest` block, add:

```sh
# descendtest: fs-tree Phase 4 geometry + scene ops (no GL, no fs). Links the
# scene spine + editor.c (RoomRect) + camera.c.
if [ "$MODE" = "descendtest" ]; then
    set -x
    clang -std=c89 -pedantic-errors -Werror -g -fsanitize=address,undefined \
        descend.c descend_test.c editor.c scene.c material.c mesh.c flora.c rock.c gothic.c sweep.c nid.c sol_math.c camera.c \
        -o descend_test
    echo "built ./descend_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi
```

Append `descend.c` right after `editor.c` in the four full-build source lists (`c89check`, `metal`, `asan`, default/release).

- [ ] **Step 5: Build and test**

Run: `./build.sh descendtest && ./descend_test` → ends with `descend_test: OK`.
Run the gauntlet: `./build.sh c89check && ./build.sh debug && ./build.sh metal` → all pass. (`descend.c`'s scene-op functions are declared but not defined yet; nothing references them, so the debug/metal links succeed.)

- [ ] **Step 6: Commit**

```bash
git add descend.h descend.c descend_test.c build.sh
git commit -m "$(printf 'descent: module scaffold + wall-aim/room-at geometry\n\nNew headless descend.c/.h for fs-tree Phase 4: descend_room_at\n(point -> containing room), descend_wall_aim (camera ray vs the 4\nwalls -> wall+offset), descend_door_point. New descendtest target;\ndescend.c added to every build. Scene ops (plant/finalize) declared,\nbuilt next task.\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 2: `descend_plant` + `descend_finalize` (scene ops)

**Files:** Modify `descend.c` (append), `descend_test.c` (append).

- [ ] **Step 1: Append the scene ops to `descend.c`**

Add `#include "material.h"` to descend.c's includes (for `Material`/`material_default`), then append:

```c
#define DESCEND_GAP 4.0f   /* clear gap between the parent wall and the preview room */

/* does a 2*half footprint at center c overlap an existing room close in Y?
   (mirrors main.c's root_spot_occupied but scene-level + includes previews) */
static sol_bool descend_spot_occupied(Scene *s, vec3 c, float half) {
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        const char  *rt;
        RoomRect     r;
        float        e;
        if (o->mesh_ref) continue;
        rt = scene_meta_get(s, o->handle, "room_type");
        if (!rt) continue;
        if (strcmp(rt, "home") != 0 && strcmp(rt, "mirror") != 0 &&
            strcmp(rt, "preview") != 0) continue;
        r = editor_room_rect(s, o->handle);
        e = (r.hw > r.hd) ? r.hw : r.hd;
        if ((c.y > r.floor_y ? c.y - r.floor_y : r.floor_y - c.y) >= 3.5f) continue;
        if (c.x + half < r.cx - e || c.x - half > r.cx + e) continue;
        if (c.z + half < r.cz - e || c.z - half > r.cz + e) continue;
        return SOL_TRUE;
    }
    return SOL_FALSE;
}

sol_u32 descend_plant(Scene *s, sol_u32 parent_room, sol_u32 folder_card,
                      int wall, float offset) {
    SceneObject *card = scene_get(s, folder_card);
    Mesh         empty;
    RoomRect     pr;
    vec3         door, outn, center;
    sol_u32      room, shell, wk;
    float        p[8];
    const char  *path, *name, *slash;
    Material     ghost = material_default();
    int          guard;
    if (!card || card->kind != KIND_FOLDER) return 0;
    if (scene_meta_get(s, folder_card, "planted")) return 0;   /* already a door */
    if (!card->content) return 0;

    pr   = editor_room_rect(s, parent_room);
    door = descend_door_point(pr, wall, offset);
    if      (wall == ROOM_WALL_N) outn = vec3_make(0.0f, 0.0f, -1.0f);
    else if (wall == ROOM_WALL_S) outn = vec3_make(0.0f, 0.0f,  1.0f);
    else if (wall == ROOM_WALL_E) outn = vec3_make(1.0f, 0.0f,  0.0f);
    else                          outn = vec3_make(-1.0f, 0.0f, 0.0f);   /* W */
    center   = vec3_add(door, vec3_scale(outn, DESCEND_GAP + 5.0f));     /* gap + half preview depth */
    center.y = pr.floor_y;
    guard = 0;
    while (descend_spot_occupied(s, center, 5.5f) && guard < 20) {
        center.y += 5.0f; guard++;                 /* 1-D Y nudge until clear */
    }

    memset(&empty, 0, sizeof empty);
    room  = scene_add(s, 0, empty, center, quat_identity(), vec3_make(1.0f,1.0f,1.0f));
    path  = card->content;
    slash = strrchr(path, '/');
    name  = (slash && slash[1]) ? slash + 1 : path;
    scene_meta_set(s, room, "room_type",   "preview");
    scene_meta_set(s, room, "source_path", path);
    scene_meta_set(s, room, "name",        name);

    shell = scene_add(s, room, empty, vec3_make(0.0f,0.0f,0.0f), quat_identity(),
                      vec3_make(1.0f,1.0f,1.0f));
    scene_mesh_ref_set(s, shell, "room");
    p[0]=10.0f; p[1]=10.0f; p[2]=3.0f; p[3]=1.0f; p[4]=1.0f; p[5]=1.0f; p[6]=1.0f; p[7]=0.0f;
    scene_mesh_params_set(s, shell, p, 8);
    ghost.base_color = vec3_make(0.35f, 0.42f, 0.55f);   /* dim, bluish — a ghost */
    ghost.roughness  = 0.95f;
    scene_material_set(s, shell, ghost);

    wk = scene_add(s, 0, empty, vec3_make(0.0f,0.0f,0.0f), quat_identity(),
                   vec3_make(1.0f,1.0f,1.0f));
    scene_mesh_ref_set(s, wk, "walkway");
    scene_rel_add(s, wk, "connects", parent_room);
    scene_rel_add(s, wk, "connects", room);

    scene_meta_set(s, folder_card, "planted", "1");   /* the card is a door now */
    return room;
}

const char *descend_finalize(Scene *s, sol_u32 preview_room) {
    const char *rt = scene_meta_get(s, preview_room, "room_type");
    sol_u32     i;
    if (!rt || strcmp(rt, "preview") != 0) return NULL;
    scene_meta_set(s, preview_room, "room_type", "mirror");
    for (i = 0; i < s->count; i++) {       /* normalize the shell material */
        SceneObject *o = &s->objects[i];
        if (o->parent == preview_room && o->mesh_ref &&
            strcmp(o->mesh_ref, "room") == 0) {
            Material stone = material_default();
            stone.base_color = vec3_make(0.55f, 0.53f, 0.50f);
            stone.roughness  = 0.92f;
            scene_material_set(s, o->handle, stone);
            break;
        }
    }
    return scene_meta_get(s, preview_room, "source_path");
}
```

- [ ] **Step 2: Append op tests to `descend_test.c`**

Before the final `if (fails == 0) ...`, replace the `(void)add_folder_card;` line with:

```c
    /* plant: a folder card becomes a door — a preview room + a walkway appear,
       the card is marked planted, and a second plant is refused */
    {
        Scene s; sol_u32 home, fld, pv, i, wk = 0;
        scene_init(&s);
        home = add_room(&s, 0.0f, 12.0f, 0.0f, 10.0f, 10.0f);
        fld  = add_folder_card(&s, home, "/tmp/sub");
        pv   = descend_plant(&s, home, fld, ROOM_WALL_E, 0.0f);
        CHECK(pv != 0);
        CHECK(strcmp(scene_meta_get(&s, pv, "room_type"), "preview") == 0);
        CHECK(strcmp(scene_meta_get(&s, pv, "source_path"), "/tmp/sub") == 0);
        CHECK(strcmp(scene_meta_get(&s, pv, "name"), "sub") == 0);
        CHECK(scene_meta_get(&s, fld, "planted") != NULL);          /* card is a door */
        for (i = 0; i < s.count; i++) {                            /* find the walkway */
            SceneObject *o = &s.objects[i];
            if (o->mesh_ref && strcmp(o->mesh_ref, "walkway") == 0) { wk = o->handle; break; }
        }
        CHECK(wk != 0);
        {
            SceneObject *wo = scene_get(&s, wk);
            sol_u32 a = 0, b = 0, j;
            for (j = 0; j < wo->rel_count; j++)
                if (strcmp(wo->relations[j].type, "connects") == 0) {
                    if (a == 0) a = wo->relations[j].target; else b = wo->relations[j].target;
                }
            CHECK((a == home && b == pv) || (a == pv && b == home));
        }
        CHECK(descend_plant(&s, home, fld, ROOM_WALL_E, 0.0f) == 0);  /* already planted */
        scene_free(&s);
    }

    /* finalize: flip preview->mirror, return the path to scan, idempotent */
    {
        Scene s; sol_u32 home, fld, pv; const char *sp;
        scene_init(&s);
        home = add_room(&s, 0.0f, 12.0f, 0.0f, 10.0f, 10.0f);
        fld  = add_folder_card(&s, home, "/tmp/sub");
        pv   = descend_plant(&s, home, fld, ROOM_WALL_E, 0.0f);
        sp   = descend_finalize(&s, pv);
        CHECK(sp != NULL && strcmp(sp, "/tmp/sub") == 0);
        CHECK(strcmp(scene_meta_get(&s, pv, "room_type"), "mirror") == 0);   /* flipped */
        CHECK(descend_finalize(&s, pv) == NULL);                             /* idempotent */
        scene_free(&s);
    }
```

- [ ] **Step 3: Build and test**

Run: `./build.sh descendtest && ./descend_test` → `descend_test: OK`.
Gauntlet: `./build.sh c89check && ./build.sh debug && ./build.sh metal` → all pass.

- [ ] **Step 4: Commit**

```bash
git add descend.c descend_test.c
git commit -m "$(printf 'descent: scene ops — plant (preview room + walkway) + finalize\n\ndescend_plant builds a preview room outward from the planted wall\n(Y-nudged clear), a walkway joining parent<->preview, and marks the\nfolder card planted (refuses non-folders / already-planted).\ndescend_finalize flips preview->mirror, normalizes the material, and\nreturns the source_path for the caller to scan (idempotent). Headless\ntests cover both.\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 3: Wire planting into main.c (aim-snap during carry, plant on E, hide planted cards)

**Files:** Modify `main.c`. Verified by the build gauntlet + human live-verify.

- [ ] **Step 1: `#include "descend.h"` and add carry-plant state to `AppState`**

Add `#include "descend.h"` near the other module includes in main.c (with `editor.h`).

In the `AppState` struct, near the carry fields (`sol_u32 carried;` ~main.c:2646), add:

```c
    sol_u32     plant_room;     /* descent: room you're aiming a folder tablet in; 0 = none */
    int         plant_wall;     /* descent: ROOM_WALL_* the carried folder is aimed at */
    float       plant_off;      /* descent: offset along that wall */
    sol_bool    plant_aim;      /* descent: a valid wall-aim this frame */
```

`AppState state = {0};` zero-inits them.

- [ ] **Step 2: Wall-snap a carried folder in `carry_update`**

Replace `carry_update` (main.c:6589-6599) with:

```c
/* Per-frame: float the carried object in front of the camera. Called right after
   update() (so it runs before components_update reads poses). A carried FOLDER
   tablet instead snaps flat to the wall you're aiming at (descent planting). */
static void carry_update(AppState *st) {
    SceneObject *o;
    vec3         fwd, hold;
    if (st->carried == 0) return;
    o = scene_get(&st->scene, st->carried);
    if (!o) { st->carried = 0; return; }                   /* it vanished */
    st->plant_aim = SOL_FALSE;
    if (o->kind == KIND_FOLDER) {
        sol_u32 room = descend_room_at(&st->scene, st->camera.pos);
        if (room != 0) {
            RoomRect r = editor_room_rect(&st->scene, room);
            Ray   ray;
            int   wall;
            float off;
            ray.origin = st->camera.pos;
            ray.dir    = camera_forward(&st->camera);
            if (descend_wall_aim(r, ray, ROUTE_DOOR_H, &wall, &off)) {
                vec3 wpt = descend_door_point(r, wall, off);
                wpt.y += 1.0f;                              /* hover at ~door height */
                o->pos = scene_world_to_local(&st->scene, o->parent, wpt);
                st->plant_room = room; st->plant_wall = wall;
                st->plant_off = off;  st->plant_aim = SOL_TRUE;
                return;
            }
        }
    }
    fwd     = camera_forward(&st->camera);
    hold    = vec3_add(st->camera.pos, vec3_scale(fwd, CARRY_HOLD_DIST));
    hold.y -= CARRY_HOLD_DROP;
    o->pos  = scene_world_to_local(&st->scene, o->parent, hold);
}
```

(`ROUTE_DOOR_H` is from route.h, already included by main.c via the routing code.)

- [ ] **Step 3: Plant on `E` in `cmd_carry_toggle`**

Replace the put-down branch of `cmd_carry_toggle` (main.c:6572-6585). The new body:

```c
/* E: put down what you're carrying, else pick up the selected movable object.
   A carried FOLDER aimed at a wall PLANTS as a door (descent). */
static void cmd_carry_toggle(AppState *st) {
    if (st->carried != 0) {
        SceneObject *o = scene_get(&st->scene, st->carried);
        if (o) {
            if (o->kind == KIND_FOLDER && st->plant_aim &&
                descend_plant(&st->scene, st->plant_room, st->carried,
                              st->plant_wall, st->plant_off) != 0) {
                scene_resolve_meshes(&st->scene);
                apply_kind_materials(&st->scene);
                connections_rebuild(st);
                collide_rebuild(&st->colliders, &st->scene);
                scene_save(&st->scene, "scene.stml");
                printf("planted '%s' as a door\n",
                       o->content ? o->content : "?");
            } else {
                vec3 w = carry_place_point(st);
                o->pos = scene_world_to_local(&st->scene, o->parent, w);
                scene_save(&st->scene, "scene.stml");
            }
        }
        st->carried   = 0;
        st->plant_aim = SOL_FALSE;
    } else {
        sol_u32 t = carry_target(st, st->selected_handle);
        if (t != 0) st->carried = t;
    }
}
```

- [ ] **Step 4: Hide planted folder cards in the draw loop**

In `render`, the visible-object loop (main.c ~9572, the one with `const SceneObject *o = &state->scene.objects[i];` and the `pond`/`church_glass` skips), add right after the `if (o->mesh.index_count == 0) continue;` line:

```c
        if (scene_meta_get(&state->scene, o->handle, "planted"))
            continue;                             /* a planted folder is a door now */
```

- [ ] **Step 5: Build (gauntlet) — verified live**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal` → all pass.
Run: `./build.sh descendtest && ./descend_test` → `descend_test: OK`.

- [ ] **Step 6: Commit**

```bash
git add main.c
git commit -m "$(printf 'descent: wire planting into carry (aim-snap + plant on E)\n\nA carried FOLDER tablet snaps flat to the wall you aim at\n(descend_room_at + descend_wall_aim in carry_update); E plants it as\na door (descend_plant + connections_rebuild + collide_rebuild +\nsave); the planted card stops drawing (the derived door represents\nit). Non-folder carry + ground placement unchanged.\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

**Live-verify checklist (hand to the human):**
1. Run `./solarium`, walk into a mirror room with subfolders (or make a root with `:` → New root). Select a folder card, press `E` to carry it.
2. Aim at a wall — the tablet snaps flat to the wall at your aim point.
3. Press `E` — the card disappears, a door opens in the wall, and an empty (dim/ghostly) preview room floats out beyond it on a walkway, labeled with the folder's name.
4. Reload (`L`) — the door + preview persist; the planted folder doesn't reappear as a tray card.

---

## Task 4: Wire walk-in finalize into the main loop

**Files:** Modify `main.c`. Verified by the gauntlet + human live-verify.

- [ ] **Step 1: Add the `last_room` edge field to `AppState`**

Near `plant_aim` (from Task 3), add:

```c
    sol_u32     last_room;      /* descent: room you were in last frame (walk-in edge) */
```

- [ ] **Step 2: Finalize a preview when you walk into it**

In the main loop, right after the `carry_update(&state);` call (main.c:10930), add:

```c
        {   /* descent: walking into a preview room finalizes it (edge-triggered) */
            sol_u32 room = descend_room_at(&state.scene, state.camera.pos);
            if (room != 0 && room != state.last_room) {
                const char *rt = scene_meta_get(&state.scene, room, "room_type");
                if (rt && strcmp(rt, "preview") == 0) {
                    const char *sp = descend_finalize(&state.scene, room);
                    if (sp) {
                        room_mirror_scan(&state.scene, room, sp);   /* fill with cards */
                        scene_resolve_meshes(&state.scene);
                        apply_kind_materials(&state.scene);
                        connections_rebuild(&state);
                        collide_rebuild(&state.colliders, &state.scene);
                        scene_save(&state.scene, "scene.stml");
                        printf("finalized '%s'\n", sp);
                    }
                }
            }
            state.last_room = room;
        }
```

(`room_mirror_scan` is from mirror.h, already included by main.c via `create_root_from_path`.)

- [ ] **Step 3: Build (gauntlet) — verified live**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal` → all pass.
Run the headless suites touched indirectly: `./build.sh descendtest && ./descend_test && ./build.sh routetest && ./route_test` → both `... : OK`.

- [ ] **Step 4: Commit**

```bash
git add main.c
git commit -m "$(printf 'descent: finalize a preview room on walk-in\n\nPer-frame edge-detected entry (descend_room_at on the camera) into a\nroom_type=preview triggers descend_finalize (flip preview->mirror) +\nroom_mirror_scan (its files/folders become cards/tablets) + rebuild +\nsave. Idempotent: a mirror room is never re-scanned. Completes the\nplant -> preview -> walk-in -> finalize descent loop.\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

**Live-verify checklist (hand to the human):**
1. After planting a folder (Task 3), walk across the bridge into the empty preview room.
2. On entry it **fills**: the folder's files/subfolders appear as cards, the room's material normalizes from ghostly to stone, and it saves.
3. Its subfolders are now **tablets** — carry one, aim at a wall, plant, and descend again. Repeat down the tree.
4. Reload (`L`) — finalized rooms persist with their contents; previews you haven't entered stay empty.
5. Walking back out and in again does **not** re-scan or duplicate (idempotent).

---

## Final review

After all four tasks: dispatch a final code reviewer over the whole diff, then use **superpowers:finishing-a-development-branch** to fast-forward merge to `main` — only after the human confirms the live-verify checklists pass. Update memory (`spatial-filesystem-direction.md`, `MEMORY.md`).

## Self-review notes (for the controller)

- **Spec coverage:** aim-at-wall planting (Task 1 wall-aim + Task 3 carry-snap) · plant → preview room + walkway (Task 2 `descend_plant`) · empty preview, ghost material (Task 2) · walk-in finalize + scan + subfolders-become-tablets (Task 4) · planted card stops drawing (Task 3 draw skip) · preview stored lightweight (Task 2) · routing untouched (planting only adds room+walkway) · Y-nudge 1-D collision (Task 2 `descend_spot_occupied`). Deferred per spec: door-seed unification (New root unchanged), tree rescan (Phase 5), floating doorframes.
- **Type consistency:** `descend_room_at`/`descend_wall_aim`/`descend_door_point`/`descend_plant`/`descend_finalize` signatures match across header, defs, tests, and main.c call sites. `RoomRect`/`editor_room_rect` reused from editor.h. `ROOM_WALL_*` ordering (N/E/S/W = 0/1/2/3) consistent between `descend_wall_aim`'s `w[]` array and `descend_door_point`.
- **Known caveat (acceptable v1):** the carried folder snaps to the wall but isn't *rotated* flat (position only) — a forward-note polish. The preview "ghost" is a dim base color, not true translucency (avoids the alpha pipeline).
