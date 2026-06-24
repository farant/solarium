# Timber Halls — Stage 2: Corner Columns Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Generalize the wall-overlay cache into a per-room `RoomFrame`, then add four `dark_wood` corner columns per room (the first timber piece).

**Architecture:** Refactor the existing `g_wall_cache` (`{handle, mesh}`) into `RoomFrame` (`{handle, wall, wood}`) built in `connections_rebuild`/`_focus`; the wall planks carry through unchanged. Then add a `frame_beam` oriented-box primitive, a `g_dark_wood` material (via the existing `load_pbr_material`), and four vertical column beams per room, drawn from the cache in the opaque pass. No new shader.

**Tech Stack:** C89; the existing MeshBuilder + `load_pbr_material` + the room-frame cache.

**Branch:** `timber-halls-stage2` (create at start; ff-merge to `main` at the end).

**C89 reminders:** decls at top of block; `/* */`; `fabs((double)x)`, `sqrt((double)x)`; c89check is `-Wall -Wextra -Werror` (no unused statics). **Never commit** `NOTES.stml`/`paper-picture.png`. Commit trailer: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

**No unit test:** render-only — gauntlet + human live-verify per task. The room height is `mesh_params[2]` (now 4.5 after stage 1). Room walls: N at z=−hd, S at z=+hd, E at x=+hw, W at x=−hw; `hw=w/2, hd=d/2`. The engine never back-face culls; `mesh_from_builder` auto-computes tangents.

---

### Task 0: Branch

- [ ] **Step 1:** `git checkout -b timber-halls-stage2`

---

### Task 1: Refactor the wall cache into `RoomFrame` (no behavior change)

**Files:** Modify `main.c`, `.gitignore`. The wall planks must look identical after this task — it only renames the cache and gives it room for the wood mesh.

- [ ] **Step 1: Replace the cache declaration**

Replace (main.c:4184-4185):
```c
static struct { sol_u32 handle; Mesh mesh; } g_wall_cache[WALL_CACHE_MAX];
static int g_wall_cache_n = 0;
```
with:
```c
typedef struct { sol_u32 handle; Mesh wall, wood; } RoomFrame;
static RoomFrame g_room_frame[WALL_CACHE_MAX];
static int       g_room_frame_n = 0;
```

- [ ] **Step 2: Replace `wall_cache_flush` with `room_frame_flush`**

Replace the whole `wall_cache_flush` function (main.c:4187-4192) with:
```c
static void room_frame_flush(void) {
    int i;
    for (i = 0; i < g_room_frame_n; i++) {
        mesh_destroy(&g_room_frame[i].wall);
        mesh_destroy(&g_room_frame[i].wood);
    }
    g_room_frame_n = 0;
}
```

- [ ] **Step 3: Replace `wall_overlay_store` with `room_frame_build`**

Replace the whole `wall_overlay_store` function (main.c:4261-4299) with (builds the wall mesh exactly as before; `wood` stays empty until Task 2; the cache now stores both):
```c
static void room_frame_build(SceneObject *shell, const RoomOpening *ops, int no) {
    MeshBuilder mb;
    Mesh        wall, wood;
    float       w, d, h, hw, hd;
    int         i;
    if (g_wall_mat.albedo_tex.id == 0) return;     /* (Task 2 widens this guard) */
    w  = mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "w");
    d  = mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "d");
    h  = mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "h");
    hw = w * 0.5f; hd = d * 0.5f;
    memset(&wall, 0, sizeof wall);
    memset(&wood, 0, sizeof wood);
    mb_init(&mb);
    if (mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "wn") > 0.5f)
        wall_panels(&mb, 1, -hd + WALL_EPS,  1.0f, -hw, hw, h, ops, no, ROOM_WALL_N);
    if (mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "ws") > 0.5f)
        wall_panels(&mb, 1,  hd - WALL_EPS, -1.0f, -hw, hw, h, ops, no, ROOM_WALL_S);
    if (mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "we") > 0.5f)
        wall_panels(&mb, 0,  hw - WALL_EPS, -1.0f, -hd, hd, h, ops, no, ROOM_WALL_E);
    if (mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "ww") > 0.5f)
        wall_panels(&mb, 0, -hw + WALL_EPS,  1.0f, -hd, hd, h, ops, no, ROOM_WALL_W);
    if (mb.index_count > 0) wall = mesh_from_builder(&mb);
    mb_free(&mb);
    for (i = 0; i < g_room_frame_n; i++)
        if (g_room_frame[i].handle == shell->handle) {
            mesh_destroy(&g_room_frame[i].wall);
            mesh_destroy(&g_room_frame[i].wood);
            g_room_frame[i].wall = wall;
            g_room_frame[i].wood = wood;
            return;
        }
    if (g_room_frame_n >= WALL_CACHE_MAX) {
        static int warned = 0;
        if (!warned) { printf("room frame: cache full (%d rooms)\n",
                              WALL_CACHE_MAX); warned = 1; }
        mesh_destroy(&wall); mesh_destroy(&wood);
        return;
    }
    g_room_frame[g_room_frame_n].handle = shell->handle;
    g_room_frame[g_room_frame_n].wall   = wall;
    g_room_frame[g_room_frame_n].wood   = wood;
    g_room_frame_n++;
}
```

- [ ] **Step 4: Replace `wall_overlay_get` with `room_frame_get`**

Replace `wall_overlay_get` (main.c:4301-4306) with:
```c
static RoomFrame *room_frame_get(sol_u32 handle) {
    int i;
    for (i = 0; i < g_room_frame_n; i++)
        if (g_room_frame[i].handle == handle) return &g_room_frame[i];
    return (RoomFrame *)0;
}
```

- [ ] **Step 5: Update the flush call + the two build hooks**

- The flush call (grep `wall_cache_flush();`, ~main.c:4314): `wall_cache_flush();` → `room_frame_flush();`
- BOTH hook calls (grep `wall_overlay_store(shell, ops, no);` — there are two, in `connections_rebuild` ~4371 and `connections_rebuild_focus` ~4453): `wall_overlay_store(shell, ops, no);` → `room_frame_build(shell, ops, no);`

- [ ] **Step 6: Update the draw block to use `RoomFrame`**

Replace the wall-overlay draw block body (main.c:12088-12100, the `if (g_wall_mat.albedo_tex.id != 0) { ... }`). Replace these lines:
```c
    if (g_wall_mat.albedo_tex.id != 0) {
        sol_u32 wi;
        for (wi = 0; wi < state->scene.count; wi++) {
            SceneObject *o = &state->scene.objects[wi];
            Mesh        *wm;
            if (!o->mesh_ref || strcmp(o->mesh_ref, "room") != 0) continue;
            if (!scene_object_active(&state->scene, o->handle)) continue;
            if (vis && !vis[o->handle]) continue;
            wm = wall_overlay_get(o->handle);
            if (!wm || wm->index_count == 0) continue;
            draw_mesh(state, *wm, scene_world_matrix(&state->scene, o),
                      view, proj, eye, 0.0f, g_wall_mat);
        }
```
with (iterate rooms once, draw each frame mesh with its material; only the `wall` exists this task):
```c
    {
        sol_u32 wi;
        for (wi = 0; wi < state->scene.count; wi++) {
            SceneObject *o = &state->scene.objects[wi];
            RoomFrame   *rf;
            mat4         rm;
            if (!o->mesh_ref || strcmp(o->mesh_ref, "room") != 0) continue;
            if (!scene_object_active(&state->scene, o->handle)) continue;
            if (vis && !vis[o->handle]) continue;
            rf = room_frame_get(o->handle);
            if (!rf) continue;
            rm = scene_world_matrix(&state->scene, o);
            if (g_wall_mat.albedo_tex.id != 0 && rf->wall.index_count > 0)
                draw_mesh(state, rf->wall, rm, view, proj, eye, 0.0f, g_wall_mat);
        }
```
(Keep the original block's trailing `}` that closes the loop+block — you are replacing the inner lines; confirm the brace count by reading. The comment line above it can stay or be updated to "room frame overlay".)

- [ ] **Step 7: Gitignore both timber-material folders**

In `.gitignore`, after `/weathered_brown_planks/`, add:
```
/dark_wood/
/distressed_painted_planks/
```

- [ ] **Step 8: Build + verify no behavior change**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal` — all PASS. Run `git status --short dark_wood/ distressed_painted_planks/` → no output (gitignored). The walls must look **identical** (pure refactor). If a build fails, grep for any leftover `g_wall_cache`/`wall_overlay_`/`wall_cache_flush` reference and update it.

- [ ] **Step 9: Commit**

```bash
git add main.c .gitignore
git commit -m "$(cat <<'EOF'
Timber halls stage 2a: refactor wall cache into RoomFrame

g_wall_cache{handle,mesh} -> g_room_frame: RoomFrame{handle, wall, wood};
wall_cache_flush/wall_overlay_store/_get -> room_frame_flush/_build/_get. The
wall planks carry through unchanged (the wood mesh is empty until the columns).
Both timber-material folders gitignored.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: `dark_wood` material + `frame_beam` + the corner columns

**Files:** Modify `main.c`.

- [ ] **Step 1: Add the frame constants**

Find the `#define WALL_TILE_M ...` / `WALL_EPS` / `WALL_CACHE_MAX` block (early, ~main.c:4178) and immediately after it add:
```c
#define FRAME_COL_T  0.24f       /* corner column cross-section (m) */
#define WOOD_TILE_M  1.0f        /* meters per wood texture-repeat along a beam */
```

- [ ] **Step 2: Declare `g_dark_wood` (next to `g_wall_mat`)**

After `static Material g_wall_mat;` (~main.c:4182), add:
```c
static Material g_dark_wood;      /* timber frame; albedo_tex.id == 0 => no wood */
```

- [ ] **Step 3: Add `dark_wood_mat_init` + call it at startup**

Next to `wall_mat_init` (grep `static void wall_mat_init`, ~main.c:9772), add after it:
```c
static void dark_wood_mat_init(void) {
    g_dark_wood = load_pbr_material(
        "dark_wood/dark_wood_diff_1k.png",
        "dark_wood/dark_wood_nor_gl_1k.png",
        "dark_wood/dark_wood_arm_1k.png");
}
```
And after the `wall_mat_init();` startup call (grep `wall_mat_init();`, ~main.c:10928), add:
```c
    dark_wood_mat_init();   /* timber halls: corner columns + trusses */
```

- [ ] **Step 4: Add the `frame_quad` + `frame_beam` geometry helpers**

Immediately BEFORE `room_frame_build` (grep `static void room_frame_build`), add:
```c
/* a quad a->b->c->d (CCW from +n) with bilinear UVs: a=(u0,v0) ... c=(u1,v1). */
static void frame_quad(MeshBuilder *mb, vec3 a, vec3 b, vec3 c, vec3 d,
                       vec3 n, float u0, float v0, float u1, float v1) {
    sol_u32 ia, ib, ic, id;
    ia = mb_push_vertex(mb, a.x, a.y, a.z, n.x, n.y, n.z, u0, v0);
    ib = mb_push_vertex(mb, b.x, b.y, b.z, n.x, n.y, n.z, u1, v0);
    ic = mb_push_vertex(mb, c.x, c.y, c.z, n.x, n.y, n.z, u1, v1);
    id = mb_push_vertex(mb, d.x, d.y, d.z, n.x, n.y, n.z, u0, v1);
    mb_push_triangle(mb, ia, ib, ic);
    mb_push_triangle(mb, ia, ic, id);
}

/* a t x t square-section beam swept A->B, wood tiling along its length. */
static void frame_beam(MeshBuilder *mb, vec3 a, vec3 b, float t) {
    vec3  dir, side, vup, refv, s, v;
    vec3  a00, a01, a11, a10, b00, b01, b11, b10;
    float len, hr = t * 0.5f, uL, vT;
    dir = vec3_sub(b, a);
    len = (float)sqrt((double)vec3_dot(dir, dir));
    if (len < 1e-5f) return;
    dir  = vec3_scale(dir, 1.0f / len);
    refv = (fabs((double)dir.y) < 0.99) ? vec3_make(0.0f, 1.0f, 0.0f)
                                        : vec3_make(1.0f, 0.0f, 0.0f);
    side = vec3_normalize(vec3_cross(dir, refv));
    vup  = vec3_normalize(vec3_cross(side, dir));
    s = vec3_scale(side, hr); v = vec3_scale(vup, hr);
    a00 = vec3_sub(vec3_sub(a, s), v); a10 = vec3_sub(vec3_add(a, s), v);
    a11 = vec3_add(vec3_add(a, s), v); a01 = vec3_add(vec3_sub(a, s), v);
    b00 = vec3_sub(vec3_sub(b, s), v); b10 = vec3_sub(vec3_add(b, s), v);
    b11 = vec3_add(vec3_add(b, s), v); b01 = vec3_add(vec3_sub(b, s), v);
    uL = len / WOOD_TILE_M; vT = t / WOOD_TILE_M;
    frame_quad(mb, a10, b10, b11, a11, side,                       0.0f, 0.0f, uL, vT);  /* +side */
    frame_quad(mb, a01, a00, b00, b01, vec3_scale(side, -1.0f),    0.0f, 0.0f, uL, vT);  /* -side */
    frame_quad(mb, a11, b11, b01, a01, vup,                        0.0f, 0.0f, uL, vT);  /* +vup */
    frame_quad(mb, a00, a10, b10, b00, vec3_scale(vup,  -1.0f),    0.0f, 0.0f, uL, vT);  /* -vup */
    frame_quad(mb, b10, b00, b01, b11, dir,                        0.0f, 0.0f, vT, vT);  /* +end */
    frame_quad(mb, a00, a10, a11, a01, vec3_scale(dir,  -1.0f),    0.0f, 0.0f, vT, vT);  /* -end */
}
```

- [ ] **Step 5: Widen the guard + build the columns in `room_frame_build`**

In `room_frame_build` (from Task 1):
(a) widen the early-return guard so the wood can build even if planks are off:
```c
    if (g_wall_mat.albedo_tex.id == 0) return;
```
→
```c
    if (g_wall_mat.albedo_tex.id == 0 && g_dark_wood.albedo_tex.id == 0) return;
```
Also guard the wall-build block: wrap the `mb_init(&mb); ...four wall_panels...; if (mb.index_count>0) wall = ...; mb_free(&mb);` in `if (g_wall_mat.albedo_tex.id != 0) { ... }`.

(b) immediately BEFORE the cache-store loop (`for (i = 0; i < g_room_frame_n; i++)`), add the wood build:
```c
    if (g_dark_wood.albedo_tex.id != 0) {
        float ci = FRAME_COL_T * 0.5f + 0.02f;   /* inset just inside the walls */
        float cx = hw - ci, cz = hd - ci;
        mb_init(&mb);
        frame_beam(&mb, vec3_make(-cx, 0.0f, -cz), vec3_make(-cx, h, -cz), FRAME_COL_T);
        frame_beam(&mb, vec3_make( cx, 0.0f, -cz), vec3_make( cx, h, -cz), FRAME_COL_T);
        frame_beam(&mb, vec3_make( cx, 0.0f,  cz), vec3_make( cx, h,  cz), FRAME_COL_T);
        frame_beam(&mb, vec3_make(-cx, 0.0f,  cz), vec3_make(-cx, h,  cz), FRAME_COL_T);
        if (mb.index_count > 0) wood = mesh_from_builder(&mb);
        mb_free(&mb);
    }
```

- [ ] **Step 6: Draw the wood mesh**

In the draw block (from Task 1 Step 6), after the `if (g_wall_mat... rf->wall ...)` draw line, add:
```c
            if (g_dark_wood.albedo_tex.id != 0 && rf->wood.index_count > 0)
                draw_mesh(state, rf->wood, rm, view, proj, eye, 0.0f, g_dark_wood);
```

- [ ] **Step 7: Build both backends**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal` — all PASS. (`frame_quad`←`frame_beam`←the column build; all called. No shader → Metal risk is compile-only.) If a build fails, fix minimally or report BLOCKED.

- [ ] **Step 8: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
Timber halls stage 2b: dark-wood corner columns

frame_beam (a tiled oriented box, via frame_quad) + g_dark_wood material; four
column beams per room from floor to wall-top, into the RoomFrame's wood mesh,
drawn through the lit PBR pipeline. No new shader.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Gauntlet, live-verify, finish

- [ ] **Step 1: Full gauntlet** — `./build.sh c89check && ./build.sh debug && ./build.sh metal` (all PASS).

- [ ] **Step 2: Human live-verify (Fran)** — on `./solarium` and `./solarium-metal`: four `dark_wood` columns stand in each room's corners, floor to wall-top, just inside the walls; the wood texture tiles up them without obvious stretch; the wall planks still look right (refactor preserved them); the columns rebuild correctly when a room is resized in the editor. Tune `FRAME_COL_T` / `WOOD_TILE_M` if needed.

- [ ] **Step 3: Finish** — superpowers:finishing-a-development-branch; ff-merge `timber-halls-stage2` to `main`. Do NOT stage `NOTES.stml`/`paper-picture.png`.

---

## Plan self-review

**Spec coverage (Stage 2):** refactor the wall cache → `RoomFrame{wall,wood}` ✓ (Task 1); `g_dark_wood` via `load_pbr_material` ✓ (Task 2 Steps 2-3); `frame_beam` oriented box ✓ (Task 2 Step 4); four corner columns floor→wall-top into the wood mesh, drawn with `g_dark_wood` ✓ (Task 2 Steps 5-6); gitignore both folders ✓ (Task 1 Step 7); no shader/MSL ✓; gauntlet + live-verify ✓ (Task 3).

**Placeholder scan:** none — full code + exact anchors.

**Type consistency:** `RoomFrame{handle,wall,wood}`, `room_frame_flush/_build/_get`, `g_room_frame`/`g_room_frame_n`, `g_dark_wood`, `frame_quad`/`frame_beam`, `FRAME_COL_T`/`WOOD_TILE_M` are used identically across Tasks 1-2. The draw uses `room_frame_get` returning `RoomFrame*` (matches Task 1 Step 4). The unused-function order holds: `frame_quad`/`frame_beam` are introduced in Task 2 with their caller (the column build); the renamed `room_frame_*` keep their callers. The wall-build is wrapped so an enabled-wood / disabled-wall room still gets its columns.
