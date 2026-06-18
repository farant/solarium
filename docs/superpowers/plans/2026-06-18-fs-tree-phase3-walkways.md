# Spatial Filesystem Tree — Phase 3: Walkways, Stairs & Ring Placement

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Floating rooms are connected by generated **walkways** (flat ribbons, or **stairs** where heights differ) you can walk across, and new roots are placed in a **ring around home**, nudged up in Y to avoid overlaps.

**Architecture:** A walkway is a connector **object** (`mesh_ref="walkway"`, two `connects` rels — modeled on `arrows_rebuild`). A new `"walkway"` mesh emitter builds a **stepped ribbon** (one flat box when level; N step-boxes when climbing). `walkways_rebuild` recomputes each walkway's transform + params (`len`, `dy`) from its two rooms each load/edit; render reads the params via the registry, collision via `collide_rebuild` — they agree by construction. Root placement moves from an east line to a ring around home with a Y-nudge collision avoider (the spec's "Y-only collision avoidance").

**Tech Stack:** Strict C89; the mesh registry + `face_*` box primitives; `collide_rebuild`; the `arrows_rebuild`/`ornament_sync` derive-each-load pattern; sol_math (`atan2f`, `quat_from_axis_angle`, `vec3_sub`).

**Spec:** `docs/superpowers/specs/2026-06-18-spatial-filesystem-tree-design.md` (Phase 3). Descent/tablets are Phase 4; the top-down editor is Phase 5. The `connects` graph this reads was authored in Phase 2.

**Conventions:** strict C89 (`-std=c89 -pedantic-errors -Werror`): declarations at top of block, `/* */` only, no mixed decl/code, ASCII-only. Commit on a branch; end messages with the `Co-Authored-By: Claude Opus 4.8 (1M context)` line. Never stage `NOTES.stml` or `paper-picture.png`.

**Testing note:** mesh/scene/collision interaction → verification is the build gauntlet + live-verify (deferred to the human). The pure mesh emitter could get a headless test but, consistent with the other mesh emitters (`make_room`/`make_path` have none), we rely on live-verify.

**Key facts (verified):**
- `arrows_rebuild` (main.c:4028) is the precedent: iterate objects with a given `mesh_ref`, read their two `connects` rel targets, `mesh_destroy(&o->mesh)`, build geometry directly, `o->mesh = mesh_from_builder(&mb)`. Called at load (main.c:7805, before `collide_rebuild` at 7806) and on-edit.
- Box-face helpers (static in mesh.c): `face_y(b, x0,x1, y, z0,z1, sign)` (quad in XZ at height y), `face_z(b, x0,x1, y0,y1, z, sign)`, `face_x(b, x, y0,y1, z0,z1, sign)`. `make_path` (mesh.c:309) builds a flat slab with them (deck at y=0, body in [-t,0]).
- `make_room`/`make_path` are declared in mesh.h; the registry table is in mesh.c (e.g. `{ "path", 3, {"len","w","t"}, {6,1.5,0.15}, emit_path }`).
- `collide_rebuild` handles `"path"` (collide.c:565) by `emit_local_box(cs, m, handle, cx, cz, hx, hz, y0, y1)` — a single box. It is called AFTER the rebuilds so it sees current params. `ref_p(o,"name")` reads a param with registry defaults.
- A room's footprint `w`/`d` lives on its SHELL child (the `mesh_ref="room"` child of the anchor), read via `mesh_ref_param`. `object_world_pos(Scene*, handle)` (main.c:2812) gives a room's world center.
- Phase 2's `create_root_from_path` (main.c, before `g_commands`) currently does `scene_rel_add(home, "connects", root)` and places roots at `home + (14 + 14*mirror_count, 0, 0)`.

---

## Task 1: The `walkway` mesh + collision

A stepped-ribbon mesh emitter and its collider. All in `mesh.c`, `mesh.h`, `collide.c`. Verification: `./build.sh c89check && ./build.sh debug && ./build.sh metal`.

- [ ] **Step 1: Declare `make_walkway` in mesh.h (after `make_path`, mesh.h:48)**
```c
/* A connector that climbs: a ribbon of N step-boxes from y=0 up to y=dy over
   `len` along X (width `w` along Z, slabs `t` thick). dy~0 => one flat box (a
   walkway); larger dy => stairs (each step's rise stays climbable). */
void    make_walkway(MeshBuilder *b, sol_f32 len, sol_f32 w, sol_f32 t, sol_f32 dy);
```

- [ ] **Step 2: Define `make_walkway` in mesh.c (right after `make_path`, ~mesh.c:317)**
```c
#define WALKWAY_STEP_RISE 0.18f   /* per-step climb; under the collide step-up */

void make_walkway(MeshBuilder *b, sol_f32 len, sol_f32 w, sol_f32 t, sol_f32 dy) {
    sol_f32 hl = len * 0.5f, hw = w * 0.5f;
    sol_f32 ady = (dy < 0.0f) ? -dy : dy;
    sol_f32 tread, rise;
    int     n, i;
    n = (ady < 0.02f) ? 1 : (int)(ady / WALKWAY_STEP_RISE) + 1;
    tread = len / (sol_f32)n;
    rise  = dy  / (sol_f32)n;
    for (i = 0; i < n; i++) {                       /* each step is a full box */
        sol_f32 x0 = -hl + (sol_f32)i * tread;
        sol_f32 x1 = x0 + tread;
        sol_f32 yd = (sol_f32)(i + 1) * rise;       /* this step's deck height */
        face_y(b, x0, x1, yd,  -hw, hw,  1);        /* deck (top) */
        face_y(b, x0, x1, -t,  -hw, hw, -1);        /* underside */
        face_z(b, x0, x1, -t, yd,  hw,  1);         /* +z side */
        face_z(b, x0, x1, -t, yd, -hw, -1);         /* -z side */
        face_x(b, x1, -t, yd, -hw, hw,  1);         /* +x riser/end */
        face_x(b, x0, -t, yd, -hw, hw, -1);         /* -x riser/end */
    }
}
```

- [ ] **Step 3: Add the registry entry + emitter in mesh.c (with the other entries, near `emit_path`)**

Add the emitter wrapper next to `emit_path`:
```c
static void emit_walkway(MeshBuilder *b, const float *p) {
    make_walkway(b, p[0], p[1], p[2], p[3]);
}
```
Add a row to the registry table (next to the `"path"` row):
```c
    { "walkway", 4, { "len", "w", "t", "dy" }, { 4.0f, 1.6f, 0.15f, 0.0f }, emit_walkway },
```

- [ ] **Step 4: Collide the `walkway` in collide.c (after the `"path"` case, collide.c:571)**
```c
        } else if (strcmp(o->mesh_ref, "walkway") == 0) {
            /* mirror make_walkway: one box per step, deck top at (i+1)*rise.
               The step-up treaty makes each rise climbable. */
            float len = ref_p(o, "len"), w = ref_p(o, "w");
            float t   = ref_p(o, "t"),   dy = ref_p(o, "dy");
            float hl  = len * 0.5f, hw = w * 0.5f;
            float ady = (dy < 0.0f) ? -dy : dy, tread, rise;
            int   n, i;
            mat4  m = scene_world_matrix(s, o);
            n     = (ady < 0.02f) ? 1 : (int)(ady / 0.18f) + 1;
            tread = len / (float)n;
            rise  = dy  / (float)n;
            for (i = 0; i < n; i++) {
                float cx = -hl + ((float)i + 0.5f) * tread;
                emit_local_box(cs, m, o->handle, cx, 0.0f,
                               tread * 0.5f, hw, -t, (float)(i + 1) * rise);
            }
```
(Match the existing `else if` chain's brace style — this slots in as another `else if` branch.)

- [ ] **Step 5: Build gauntlet** — `./build.sh c89check && ./build.sh debug && ./build.sh metal`, all pass. (The mesh is unused until Task 2; this confirms it compiles and the registry/collision accept it.)

- [ ] **Step 6: Commit**
```bash
git add mesh.h mesh.c collide.c
git commit -m "feat: fs-tree phase 3a — walkway stepped-ribbon mesh + collider" -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: `walkways_rebuild` + author walkway objects

Walkways become connector objects; a rebuild recomputes their transform/params from their two rooms; wire it into the load + create paths. All in `main.c`.

- [ ] **Step 1: Add helpers + `walkways_rebuild`, near `arrows_rebuild` (main.c:4028)**
```c
/* a room's half-extent (max of its shell's w/d, halved) — for edge points */
static float room_half_extent(Scene *s, sol_u32 anchor) {
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        if (o->parent == anchor && o->mesh_ref &&
            strcmp(o->mesh_ref, "room") == 0) {
            float w = mesh_ref_param("room", o->mesh_params, o->mesh_param_count, "w");
            float d = mesh_ref_param("room", o->mesh_params, o->mesh_param_count, "d");
            return (w > d ? w : d) * 0.5f;
        }
    }
    return 4.0f;   /* fallback */
}

/* Derive each walkway's transform + params from the two rooms it connects.
   Models arrows_rebuild: walkway objects carry two `connects` rels; geometry is
   rebuilt here. Anchored at the LOWER room's edge, climbing dy to the higher. */
static void walkways_rebuild(AppState *st) {
    Scene  *s = &st->scene;
    sol_u32 i, j;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        sol_u32      ha = 0, hb = 0, lo, hi;
        vec3         pa, pb, plo, phi, dir, mid;
        float        ea, eb, dx, dz, lenxz, dy, yaw, wp[4];
        MeshBuilder  mb;
        if (!o->mesh_ref || strcmp(o->mesh_ref, "walkway") != 0) continue;
        for (j = 0; j < o->rel_count; j++) {
            if (strcmp(o->relations[j].type, "connects") != 0) continue;
            if (ha == 0)      ha = o->relations[j].target;
            else if (hb == 0) hb = o->relations[j].target;
        }
        if (ha == 0 || hb == 0 || !scene_get(s, ha) || !scene_get(s, hb)) {
            mesh_destroy(&o->mesh);                 /* dangling -> invisible */
            continue;
        }
        pa = object_world_pos(s, ha);
        pb = object_world_pos(s, hb);
        lo = (pa.y <= pb.y) ? ha : hb;              /* anchor at the lower room */
        hi = (lo == ha) ? hb : ha;
        plo = object_world_pos(s, lo);
        phi = object_world_pos(s, hi);
        ea = room_half_extent(s, lo);
        eb = room_half_extent(s, hi);
        dx = phi.x - plo.x; dz = phi.z - plo.z;
        lenxz = (float)sqrt((double)(dx * dx + dz * dz));
        if (lenxz < 0.001f) { mesh_destroy(&o->mesh); continue; }
        dir = vec3_make(dx / lenxz, 0.0f, dz / lenxz);
        plo = vec3_add(plo, vec3_scale(dir, ea));   /* step off the lower edge */
        phi = vec3_sub(phi, vec3_scale(dir, eb));    /* arrive at the higher edge */
        dx = phi.x - plo.x; dz = phi.z - plo.z;
        lenxz = (float)sqrt((double)(dx * dx + dz * dz));
        dy = phi.y - plo.y;
        if (lenxz < 0.05f) { mesh_destroy(&o->mesh); continue; } /* edges meet */
        yaw = (float)atan2((double)(-dz), (double)dx); /* face from lo toward hi */
        mid = vec3_make((plo.x + phi.x) * 0.5f, plo.y, (plo.z + phi.z) * 0.5f);
        o->pos = mid;                                /* walkways parent = world */
        o->rot = quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), yaw);
        wp[0] = lenxz; wp[1] = 1.6f; wp[2] = 0.15f; wp[3] = dy;
        scene_mesh_params_set(s, o->handle, wp, 4);
        mesh_destroy(&o->mesh);
        mb_init(&mb);
        make_walkway(&mb, wp[0], wp[1], wp[2], wp[3]);
        if (mb.index_count > 0) o->mesh = mesh_from_builder(&mb);
        mb_free(&mb);
    }
}
```
(If `o->pos`/`o->rot` aren't the right field names, match how `arrows_rebuild`/the scene write an object's transform — read `arrows_rebuild` + `scene.h`. `vec3_scale`, `vec3_add`, `vec3_sub`, `quat_from_axis_angle` are in sol_math; `mb_init`/`mb_free`/`mesh_from_builder`/`mesh_destroy` are used by `arrows_rebuild`.)

- [ ] **Step 2: Author a walkway object in `create_root_from_path` (replace the `connects` rel)**

Phase 2 added `if (home != 0) scene_rel_add(&st->scene, home, "connects", root);`. Replace that line with a walkway connector object carrying both edges:
```c
    if (home != 0) {
        Mesh    empty2 = {0};
        sol_u32 wk = scene_add(&st->scene, 0, empty2, vec3_make(0.0f, 0.0f, 0.0f),
                               quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
        scene_mesh_ref_set(&st->scene, wk, "walkway");
        scene_rel_add(&st->scene, wk, "connects", home);
        scene_rel_add(&st->scene, wk, "connects", root);
    }
```
(Declare `empty2` at the top of that block — C89.)

- [ ] **Step 3: Run `walkways_rebuild` where geometry is derived**

In `create_root_from_path`, after the existing `scene_resolve_meshes(&st->scene);` and BEFORE `collide_rebuild(...)`, add:
```c
    walkways_rebuild(st);
```
And in the LOAD path: find where `arrows_rebuild(st);` is called at load (main.c:7805, just before `collide_rebuild`) and add `walkways_rebuild(st);` immediately after it (so collide sees the walkways' params). Also add `walkways_rebuild(st);` right after `arrows_rebuild(st)` in the populate/else path if one exists there; otherwise the create path covers fresh roots.

- [ ] **Step 4: Build gauntlet** — `./build.sh c89check && ./build.sh debug && ./build.sh metal`, all pass.

- [ ] **Step 5: (Interactive verify — DEFERRED TO HUMAN.)**

- [ ] **Step 6: Commit**
```bash
git add main.c
git commit -m "feat: fs-tree phase 3b — walkways_rebuild connects home to roots" -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Ring placement + Y-nudge

Replace the east-line root placement with a ring around home, lifting in Y to avoid overlaps. All in `main.c` (`create_root_from_path`).

- [ ] **Step 1: Add an XZ-occupancy test near `create_root_from_path`**
```c
/* Does a w x w footprint centered at `c` overlap an existing room close in Y?
   (XZ AABB overlap AND within one room-height in Y => a real collision.) */
static sol_bool root_spot_occupied(AppState *st, vec3 c, float half) {
    sol_u32 i;
    for (i = 0; i < st->scene.count; i++) {
        SceneObject *o = &st->scene.objects[i];
        const char  *rt = scene_meta_get(&st->scene, o->handle, "room_type");
        float        e; vec3 p;
        if (!rt) continue;
        if (strcmp(rt, "home") != 0 && strcmp(rt, "mirror") != 0) continue;
        e = room_half_extent(&st->scene, o->handle);
        p = object_world_pos(&st->scene, o->handle);
        if ((c.y > p.y ? c.y - p.y : p.y - c.y) >= 3.5f) continue;   /* clear in Y */
        if (c.x + half < p.x - e || c.x - half > p.x + e) continue;  /* clear in X */
        if (c.z + half < p.z - e || c.z - half > p.z + e) continue;  /* clear in Z */
        return SOL_TRUE;                                             /* overlaps */
    }
    return SOL_FALSE;
}
```

- [ ] **Step 2: Replace the line placement in `create_root_from_path`**

Phase 2 computed:
```c
    pos = vec3_add(home_pos,
                   vec3_make(14.0f + 14.0f * (float)mirror_count, 0.0f, 0.0f));
```
Replace it with a ring slot + Y-nudge:
```c
    {
        int   slot = mirror_count % 8;             /* 8 slots per ring */
        int   turn = mirror_count / 8;             /* outer rings as it fills */
        float ang  = (float)slot * (6.2831853f / 8.0f);
        float r    = 16.0f + (float)turn * 12.0f;
        int   guard = 0;
        pos = vec3_make(home_pos.x + r * (float)cos((double)ang),
                        home_pos.y,
                        home_pos.z + r * (float)sin((double)ang));
        while (root_spot_occupied(st, pos, 5.0f) && guard < 20) {
            pos.y += 5.0f;                          /* go vertical to clear */
            guard++;
        }
    }
```
(`mirror_count`/`home_pos` are already computed above this point in the function. `cos`/`sin` from `<math.h>` — already included. Declare the block's locals at its top — C89.)

- [ ] **Step 3: Build gauntlet** — all three pass.

- [ ] **Step 4: (Interactive verify — DEFERRED TO HUMAN.)**

- [ ] **Step 5: Commit**
```bash
git add main.c
git commit -m "feat: fs-tree phase 3c — ring placement + Y-nudge for roots" -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Final verification (human, live)

Run `./solarium` (spawns in home):
- [ ] `:new root` a directory → a root room appears, **connected to home by a walkway** you can see.
- [ ] Walk across the walkway from home into the root room (no falling; the walkway is solid).
- [ ] Create several roots → they spread **around home in a ring** (not a line); each gets its own walkway.
- [ ] If two would overlap (crowd the ring), the later one **floats higher** and its walkway becomes **stairs** you can climb.
- [ ] Quit/relaunch → walkways regenerate correctly from the saved rooms + `connects` edges (the geometry isn't stored — it re-derives).
- [ ] `./solarium-metal` → same (no new shader; walkway uses existing materials/pipeline).

## Self-review (writing-plans)

- **Spec coverage (Phase 3 slice):** walkways from the `connects` graph (Task 2 `walkways_rebuild`), stairs for height differences (Task 1 stepped ribbon + collision), derived-not-stored geometry (rebuilt each load like `arrows_rebuild`), ring placement around home + Y-collision-nudge (Task 3). Descent/tablets (Phase 4) + the editor (Phase 5) out of scope.
- **Placeholders:** none — concrete code throughout; the few "match the existing field/brace style" notes point at named precedents (`arrows_rebuild`, the `"path"` cases) the implementer reads.
- **Consistency:** the `"walkway"` params `[len, w, t, dy]` are written identically in the emitter (Task 1), the collider (Task 1), and `walkways_rebuild` (Task 2); `room_half_extent` defined in Task 2 is reused by Task 3's `root_spot_occupied`; the walkway object created in Task 2 is what `walkways_rebuild` consumes.
- **Order risk:** `walkways_rebuild` MUST run before `collide_rebuild` (so collision reads fresh params) — Task 2 Step 3 places it accordingly at both the load and create sites.
