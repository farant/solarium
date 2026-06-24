# Timber Halls — Stage 4: Pitched Roof + Gable Ends Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Close the hall — a pitched `distressed_painted_planks` roof on the trusses, plus wall-plank gable triangles at the ends — the timber-frame finale.

**Architecture:** Extend `RoomFrame` with `roof` + `gable` meshes; hoist the frame-plan computation (ridge axis, span, pitch→ridge height) to one place in `room_frame_build` so the trusses, roof, and gables all read it (the spec's "one author"). The roof = two sloped quads (via the existing `frame_quad`); the gables = two triangles (a new `gable_tri`) reusing the wall material. The flat ceiling was already suppressed in Stage 3. No new shader.

**Tech Stack:** C89; the existing `frame_quad`/`bent_pt`/`load_pbr_material` + MeshBuilder.

**Branch:** `timber-halls-stage4` (create at start; ff-merge to `main` at the end).

**C89 reminders:** decls at top of block; `/* */`; `(float)tan/sqrt((double)x)`; c89check is `-Wall -Wextra -Werror` (no unused statics). **Never commit** `NOTES.stml`/`paper-picture.png`. Commit trailer: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

**No unit test:** render-only — gauntlet + human live-verify per task. `distressed_painted_planks/` is already gitignored (added in Stage 2). `RoomFrame` is `typedef struct { sol_u32 handle; Mesh wall, wood; } RoomFrame;` (~main.c:4191); `room_frame_flush` destroys wall+wood; `room_frame_build` builds wall + wood (columns + trusses) and stores them; the draw block (grep `room_frame_get`) draws `rf->wall` (g_wall_mat) + `rf->wood` (g_dark_wood). The trusses' wood block currently computes its own `rax/rlen/along_h/sh/dy/ridge_y` locally — Task 1 hoists those.

---

### Task 0: Branch

- [ ] **Step 1:** `git checkout -b timber-halls-stage4`

---

### Task 1: Hoist the frame plan + the pitched roof

**Files:** Modify `main.c`.

- [ ] **Step 1: Extend `RoomFrame`**

Replace `typedef struct { sol_u32 handle; Mesh wall, wood; } RoomFrame;` (~main.c:4191) with:
```c
typedef struct { sol_u32 handle; Mesh wall, wood, roof, gable; } RoomFrame;
```

- [ ] **Step 2: `room_frame_flush` destroys all four meshes**

In `room_frame_flush`, after the two existing `mesh_destroy(&g_room_frame[i].wall); mesh_destroy(&g_room_frame[i].wood);` lines, add:
```c
        mesh_destroy(&g_room_frame[i].roof);
        mesh_destroy(&g_room_frame[i].gable);
```

- [ ] **Step 3: Roof constant + material**

After the `#define FRAME_SCISSOR_FRAC ...` line (grep it), add:
```c
#define ROOF_TILE_M  2.0f        /* meters per roof texture-repeat */
```
After `static Material g_dark_wood;` (grep it), add:
```c
static Material g_roof_mat;       /* pitched roof; albedo_tex.id == 0 => no roof */
```
After `static void dark_wood_mat_init(void) { ... }` (grep it), add:
```c
static void roof_mat_init(void) {
    g_roof_mat = load_pbr_material(
        "distressed_painted_planks/distressed_painted_planks_diff_1k.png",
        "distressed_painted_planks/distressed_painted_planks_nor_gl_1k.png",
        "distressed_painted_planks/distressed_painted_planks_arm_1k.png");
}
```
After the `dark_wood_mat_init();` startup call (grep it), add:
```c
    roof_mat_init();    /* timber halls: pitched roof */
```

- [ ] **Step 4: Hoist the frame plan in `room_frame_build` + extend its meshes/guard/cache**

In `room_frame_build`:

(a) decls — change `Mesh wall, wood;` to `Mesh wall, wood, roof, gable;` and add the plan vars to the declaration block:
```c
    int   rax;
    float rlen, along_h, sh, dy, ridge_y;
```

(b) widen the early-return guard to include the roof:
```c
    if (g_wall_mat.albedo_tex.id == 0 && g_dark_wood.albedo_tex.id == 0) return;
```
→
```c
    if (g_wall_mat.albedo_tex.id == 0 && g_dark_wood.albedo_tex.id == 0 &&
        g_roof_mat.albedo_tex.id == 0) return;
```

(c) after `hw = w * 0.5f; hd = d * 0.5f;`, add the `memset`s for the two new meshes and the hoisted plan:
```c
    memset(&roof,  0, sizeof roof);
    memset(&gable, 0, sizeof gable);
    rax     = (w >= d) ? 1 : 0;     /* 1 = ridge along X (span = Z) */
    rlen    = rax ? w : d;          /* ridge length */
    along_h = rax ? hw : hd;        /* half the ridge length */
    sh      = rax ? hd : hw;        /* half-span (eave -> centre) */
    dy      = sh * (float)tan((double)sol_radians(FRAME_PITCH_DEG));
    ridge_y = h + dy;               /* ridge peak above the floor */
```
(The existing `memset(&wall,...)` / `memset(&wood,...)` stay.)

(d) refactor the truss block to use the hoisted vars. In the `/* scissor trusses ... */ { ... }` block, DELETE its local plan declarations:
```c
            int   rax     = (w >= d) ? 1 : 0;      /* 1 = ridge along X (span = Z) */
            float rlen    = rax ? w : d;           /* ridge length */
            float along_h = rax ? hw : hd;         /* half the ridge length */
            float sh      = rax ? hd : hw;         /* half-span (eave -> centre) */
            float dy      = sh * (float)tan((double)sol_radians(FRAME_PITCH_DEG));
            float ridge_y = h + dy;                /* ridge peak above the floor */
```
keeping the rest (`float f = FRAME_SCISSOR_FRAC; float cross_y = ...; int bents = ...; int bi; if (bents < 2)...; for ...`). The block now reads `rax/rlen/along_h/sh/dy/ridge_y` from the hoisted vars. (The opening `{` can stay as a scope for `f/cross_y/bents/bi`, or be removed — leave it; it just needs its now-shorter decl list at the top.)

(e) AFTER the wood block's closing `}` (the `if (g_dark_wood...) { ... }`), and BEFORE the cache-store `for` loop, add the roof build:
```c
    if (g_roof_mat.albedo_tex.id != 0) {
        float slope_l = (float)sqrt((double)(sh * sh + dy * dy));
        float uL = rlen / ROOF_TILE_M, uS = slope_l / ROOF_TILE_M;
        int   side;
        mb_init(&mb);
        for (side = 0; side < 2; side++) {
            float sg  = side ? -1.0f : 1.0f;
            vec3  en  = bent_pt(rax, -along_h, sg * sh, h);        /* eave near */
            vec3  ef  = bent_pt(rax,  along_h, sg * sh, h);        /* eave far  */
            vec3  rdf = bent_pt(rax,  along_h, 0.0f,    ridge_y);  /* ridge far */
            vec3  rdn = bent_pt(rax, -along_h, 0.0f,    ridge_y);  /* ridge near*/
            vec3  nrm = vec3_normalize(bent_pt(rax, 0.0f, sg * dy, sh));
            frame_quad(&mb, en, ef, rdf, rdn, nrm, 0.0f, 0.0f, uL, uS);
        }
        if (mb.index_count > 0) roof = mesh_from_builder(&mb);
        mb_free(&mb);
    }
```

(f) the cache store/replace/overflow — handle `roof` + `gable` alongside `wall`/`wood`:
- in the replace-by-handle branch, after the two `mesh_destroy(&g_room_frame[i].wall/wood)` and the two assignments, add `mesh_destroy(&g_room_frame[i].roof); mesh_destroy(&g_room_frame[i].gable);` and `g_room_frame[i].roof = roof; g_room_frame[i].gable = gable;`
- in the overflow branch, add `mesh_destroy(&roof); mesh_destroy(&gable);` next to `mesh_destroy(&wall); mesh_destroy(&wood);`
- in the append branch, add `g_room_frame[g_room_frame_n].roof = roof; g_room_frame[g_room_frame_n].gable = gable;` (before the `g_room_frame_n++;`)

- [ ] **Step 5: Draw the roof**

In the draw block (grep `room_frame_get`), after the `rf->wood` draw line, add:
```c
            if (g_roof_mat.albedo_tex.id != 0 && rf->roof.index_count > 0)
                draw_mesh(state, rf->roof, rm, view, proj, eye, 0.0f, g_roof_mat);
```

- [ ] **Step 6: Build + commit**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal` — all PASS. (No new function; `frame_quad`/`bent_pt` reused; the hoisted plan vars are used by the trusses + roof. If a build fails, fix minimally or report BLOCKED.)
```bash
git add main.c
git commit -m "$(cat <<'EOF'
Timber halls stage 4a: pitched plank roof

Hoist the frame plan (ridge axis/span/pitch->ridge height) to one place in
room_frame_build so trusses + roof + gables share it; extend RoomFrame with
roof/gable meshes; build the two sloped distressed_painted_planks roof planes
(via frame_quad) into rf->roof and draw them. No new shader.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Gable ends

**Files:** Modify `main.c`.

- [ ] **Step 1: Add the `gable_tri` helper**

Immediately BEFORE `static void room_frame_build(` (grep it), add:
```c
/* one triangle a,b,c with per-vertex normal n and explicit UVs (for the gable
   ends, whose UVs are position-based to line up with the wall planks below). */
static void gable_tri(MeshBuilder *mb, vec3 a, vec3 b, vec3 c, vec3 n,
                      float ua, float va, float ub, float vb, float uc, float vc) {
    sol_u32 ia, ib, ic;
    ia = mb_push_vertex(mb, a.x, a.y, a.z, n.x, n.y, n.z, ua, va);
    ib = mb_push_vertex(mb, b.x, b.y, b.z, n.x, n.y, n.z, ub, vb);
    ic = mb_push_vertex(mb, c.x, c.y, c.z, n.x, n.y, n.z, uc, vc);
    mb_push_triangle(mb, ia, ib, ic);
}
```

- [ ] **Step 2: Build the gable triangles**

In `room_frame_build`, AFTER the roof block (from Task 1) and BEFORE the cache-store `for` loop, add:
```c
    if (g_roof_mat.albedo_tex.id != 0 && g_wall_mat.albedo_tex.id != 0) {
        int gi;
        mb_init(&mb);
        for (gi = 0; gi < 2; gi++) {
            float ge  = gi ? along_h : -along_h;       /* the two ridge ends */
            float gn  = gi ? 1.0f : -1.0f;             /* outward along the ridge axis */
            vec3  eL  = bent_pt(rax, ge, -sh,  h);
            vec3  eR  = bent_pt(rax, ge,  sh,  h);
            vec3  ap  = bent_pt(rax, ge, 0.0f, ridge_y);
            vec3  nrm = vec3_normalize(bent_pt(rax, gn, 0.0f, 0.0f));
            gable_tri(&mb, eL, eR, ap, nrm,
                      -sh / WALL_TILE_M, h / WALL_TILE_M,
                       sh / WALL_TILE_M, h / WALL_TILE_M,
                      0.0f,              ridge_y / WALL_TILE_M);
        }
        if (mb.index_count > 0) gable = mesh_from_builder(&mb);
        mb_free(&mb);
    }
```

- [ ] **Step 3: Draw the gables**

In the draw block, after the `rf->roof` draw line (Task 1 Step 5), add (gables reuse the wall plank material):
```c
            if (g_wall_mat.albedo_tex.id != 0 && rf->gable.index_count > 0)
                draw_mesh(state, rf->gable, rm, view, proj, eye, 0.0f, g_wall_mat);
```

- [ ] **Step 4: Build + commit**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal` — all PASS. (`gable_tri`←the gable build; called. If a build fails, fix minimally or report BLOCKED.)
```bash
git add main.c
git commit -m "$(cat <<'EOF'
Timber halls stage 4b: gable ends

Two wall-plank gable triangles (via gable_tri, position-based UVs to align with
the walls below) close each room's roof ends, into rf->gable, drawn with the
wall material. The timber-frame hall is complete.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Gauntlet, live-verify, finish

- [ ] **Step 1: Full gauntlet** — `./build.sh c89check && ./build.sh debug && ./build.sh metal` (all PASS).

- [ ] **Step 2: Human live-verify (Fran)** — on `./solarium` and `./solarium-metal`: each room now has a **pitched plank roof** on the trusses (two sloped `distressed_painted_planks` planes meeting at the ridge) and **closed gable ends** (the triangular ends above the short walls, in the wall plank texture) — the hall reads complete, floor → walls → columns → trusses → roof. No light leaks through the ends; the ridge runs along the longer dimension; a non-square and a resized room both look right. Tune `ROOF_TILE_M` / `FRAME_PITCH_DEG` if needed.

- [ ] **Step 3: Finish** — superpowers:finishing-a-development-branch; ff-merge `timber-halls-stage4` to `main`. Do NOT stage `NOTES.stml`/`paper-picture.png`. (This completes the timber-frame halls — consider noting it in memory.)

---

## Plan self-review

**Spec coverage (Stage 4):** `RoomFrame` gains roof+gable ✓ (T1 S1); `g_roof_mat` via `load_pbr_material` ✓ (T1 S3); the frame plan hoisted to one author shared by trusses/roof/gable ✓ (T1 S4); two sloped roof planes (flush eaves, tiling UVs) drawn with `g_roof_mat` ✓ (T1 S4-5); gable triangles in the wall material, position-based UVs ✓ (T2); the flat ceiling already suppressed (Stage 3) ✓; cache flush/store handle the new meshes ✓ (T1 S2,S4f); no shader/MSL ✓; gauntlet + live-verify ✓ (T3).

**Placeholder scan:** none — full code + exact anchors.

**Type consistency:** `RoomFrame{wall,wood,roof,gable}`, `g_roof_mat`, `roof_mat_init`, `ROOF_TILE_M`, `gable_tri`, and the hoisted `rax/rlen/along_h/sh/dy/ridge_y` are used identically across T1/T2. The roof normal `bent_pt(rax,0,sg·dy,sh)` and gable normal `bent_pt(rax,gn,0,0)` reuse `bent_pt`'s pure axis-permutation (valid for directions, no offset). The roof/gable both read the hoisted plan, so they agree with the trusses by construction. `gable_tri` is introduced with its caller (no unused-function). The truss refactor (T1 S4d) only removes now-duplicate locals; the truss geometry is unchanged.
