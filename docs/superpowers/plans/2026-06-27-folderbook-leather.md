# Folder Books: Blue Leather Cover + White Pages — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Board folder books (`mesh_ref="folderbook"`) get a blue-leather cover (the `red_leather` material, tinted blue) + a white page block.

**Architecture:** Load `red_leather` as `g_book_leather` (no albedo → blue `base_color`). `make_folderbook` keeps only the cover; the white page block is a render-time `make_box` unit cube scaled per-folder in the draw loop (so visibility/picking are free). `folderbook_materialize` re-applies the leather textures on every load/rebuild (they aren't serialized) while keeping each folder's blue shade. `red_leather/` stays local + gitignored.

**Tech Stack:** C89 (`mesh.c`, `main.c`), `load_texture_linear`/PBR material path. Spec: `docs/superpowers/specs/2026-06-27-folderbook-leather-design.md`.

**House rules:** strict C89 (declarations at block top, no `//`, no mid-block decls; `-std=c89 -pedantic-errors -Werror -Wextra`). NEVER `git add NOTES.stml`/`paper-picture.png`. Commit body ends with `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. Feature branch off `main`.

**Verified anchors:** `make_folderbook` (mesh.c:1123) — the leaf-block box is the first `box_minmax` (mesh.c:1130-1131). `make_box(b,w,h,d)` is public + centered (mesh.h:36). `g_floor_mat`/`load_pbr_material`/`load_texture_linear`/`oak_mat_init` (main.c ~12607-12689); the `*_mat_init()` startup calls (~main.c:13883, after `oak_mat_init();`). `apply_kind_materials(Scene*)` (main.c:13055) skips `KIND_PLAIN`; its forward decl is main.c:5478. `world_rebuild` and `load_palace` each call `apply_kind_materials(&st->scene)` (the load-derive chokepoints). `add_folder` (main.c:8084) creates the folder + sets a blue material + `scene_resolve_meshes` (8107-8116). AppState `caret_mesh` (main.c:2855); its shutdown free `mesh_destroy(&state.caret_mesh)` (~17345). The draw loop draws `o->mesh` then `state->draws_done++` (main.c:15095-15097); `model`/`hl`/`view`/`proj`/`eye` are in scope there. Material is `{albedo_tex, mr_tex, ao_tex, normal_tex, base_color, emissive, metallic, roughness, ao_strength, normal_scale}`.

---

## Task 1: Blue-leather cover + white pages for folder books

**Files:** `mesh.c`, `main.c`, `.gitignore`.

- [ ] **Step 1: `.gitignore` — keep the material local**

Append `/red_leather/` to `.gitignore` (next to the other sourced-material lines `/dark_wood/`, `/oak_veneer/`, etc.).

- [ ] **Step 2: `make_folderbook` — cover only (mesh.c)**

In `make_folderbook` (mesh.c:1123), DELETE the leaf-block box and its comment (mesh.c:1130-1131):
```c
    /* the leaf block: flush at the spine (left), inset elsewhere */
    box_minmax(b, -hw + board, hw - lip, inset, h - inset, board, d - board);
```
Leave the rest (cover boards, spine slab, bands) unchanged. (`board`/`inset`/`lip` locals stay — the bands still use them.)

- [ ] **Step 3: `g_book_leather` material (main.c)**

Add the declaration next to `g_floor_mat` (grep `static Material g_floor_mat;`):
```c
static Material g_book_leather;   /* folder-book cover: red_leather grain, blue-tinted; id 0 => flat */
```
Add the init after `oak_mat_init()` (grep `static void oak_mat_init`):
```c
static void book_leather_mat_init(void) {
    Material m = material_default();
    m.normal_tex = load_texture_linear("red_leather/leather_red_02_nor_gl_1k.png");
    if (m.normal_tex.id == 0) { g_book_leather = m; return; }   /* missing -> flat fallback */
    m.mr_tex = load_texture_linear("red_leather/leather_red_02_arm_1k.png");  /* ARM: R=AO,G=rough,B=metal */
    if (m.mr_tex.id != 0) {
        m.ao_tex      = m.mr_tex;
        m.metallic    = 1.0f;
        m.roughness   = 1.0f;
        m.ao_strength = 1.0f;
    }
    m.normal_scale = 1.0f;
    /* base_color stays white — folderbook_materialize sets the per-folder blue */
    g_book_leather = m;
}
```
Call it at startup, right after `oak_mat_init();` (grep `oak_mat_init();`):
```c
    oak_mat_init();     /* oak-veneer file/folder tablets */
    book_leather_mat_init();   /* folder-book covers (red_leather, blue-tinted) */
```

- [ ] **Step 4: `folderbook_materialize` derive (main.c)**

Add a forward declaration next to the `apply_kind_materials` forward decl (grep `static void apply_kind_materials(Scene \*s);`):
```c
static void folderbook_materialize(Scene *s);   /* re-apply the leather cover (textures aren't serialized) */
```
Add the definition right after `apply_kind_materials` (grep its definition `static void apply_kind_materials(Scene *s) {` and place this after its closing `}`):
```c
/* Re-apply g_book_leather to every folder book's cover, keeping each folder's own
   blue base_color. Needed on load/rebuild because texture handles aren't serialized. */
static void folderbook_materialize(Scene *s) {
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        vec3 keep;
        if (!o->mesh_ref || strcmp(o->mesh_ref, "folderbook") != 0) continue;
        keep = o->material.base_color;                       /* the per-folder blue shade */
        o->material.albedo_tex   = g_book_leather.albedo_tex;   /* id 0 (no albedo map) */
        o->material.normal_tex   = g_book_leather.normal_tex;
        o->material.mr_tex       = g_book_leather.mr_tex;
        o->material.ao_tex       = g_book_leather.ao_tex;
        o->material.metallic     = g_book_leather.metallic;
        o->material.roughness    = g_book_leather.roughness;
        o->material.normal_scale = g_book_leather.normal_scale;
        o->material.ao_strength  = g_book_leather.ao_strength;
        o->material.base_color   = keep;                     /* keep the blue */
    }
}
```

- [ ] **Step 5: Call `folderbook_materialize` at the derive chokepoints (main.c)**

In `world_rebuild` (grep `static void world_rebuild`) and the `load_palace` tail (grep `static sol_bool load_palace`), add a call right after their `apply_kind_materials(&st->scene);` line:
```c
    apply_kind_materials(&st->scene);
    folderbook_materialize(&st->scene);   /* re-apply the leather covers */
```
In `add_folder` (grep `static sol_u32 add_folder`), after the existing `scene_resolve_meshes(&st->scene);` (and before `o = scene_get(...)` re-fetch / it's fine after the material set), add at the END of the function — after the `if (o) { … }` block that sets `m.base_color`/`o->material`, before `return h;`:
```c
        o->pos = board_pin_pos(&st->scene, board, h, blocal,
                               0.0f, -0.5f * p[1]);  /* center on the point */
    }
    folderbook_materialize(&st->scene);   /* apply the leather to the new folder */
    return h;
```
(`add_folder` still sets its blue `base_color` as today — that's the tint `folderbook_materialize` preserves.)

- [ ] **Step 6: The white page block — render-derived (main.c)**

Add the cached mesh to AppState, after `Mesh caret_mesh;` (grep `Mesh        caret_mesh;`):
```c
    Mesh        folderbook_leaves_mesh; /* unit box for the folder-book white page block; built once */
```
Free it at shutdown, next to `mesh_destroy(&state.caret_mesh);` (grep it):
```c
    mesh_destroy(&state.caret_mesh);
    mesh_destroy(&state.folderbook_leaves_mesh);
```
In the draw loop, right after the per-object draw and BEFORE `state->draws_done++;` (grep `state->draws_done++;` — the FIRST one, in the main object loop at ~15097), add:
```c
        if (o->mesh_ref && strcmp(o->mesh_ref, "folderbook") == 0) {
            float fw = mesh_ref_param("folderbook", o->mesh_params, o->mesh_param_count, "w");
            float fh = mesh_ref_param("folderbook", o->mesh_params, o->mesh_param_count, "h");
            float fd = mesh_ref_param("folderbook", o->mesh_params, o->mesh_param_count, "d");
            float board = fd * 0.18f, inset = fh * 0.05f, lip = fw * 0.03f;
            float lw, lh2, ld;
            if (board < 1e-4f) board = 1e-4f;          /* match make_folderbook */
            lw  = fw - lip - board;
            lh2 = fh - 2.0f * inset;
            ld  = fd - 2.0f * board;
            if (lw > 0.0f && lh2 > 0.0f && ld > 0.0f) {
                Material pm = material_default();        /* white pages */
                vec3     c  = vec3_make((board - lip) * 0.5f, fh * 0.5f, fd * 0.5f);
                mat4     lm = mat4_mul(model, mat4_from_trs(c, quat_identity(),
                                       vec3_make(lw, lh2, ld)));
                if (state->folderbook_leaves_mesh.index_count == 0) {
                    MeshBuilder mb;
                    mb_init(&mb);
                    make_box(&mb, 1.0f, 1.0f, 1.0f);     /* centered unit cube */
                    state->folderbook_leaves_mesh = mesh_from_builder(&mb);
                    mb_free(&mb);
                }
                draw_mesh(state, state->folderbook_leaves_mesh, lm, view, proj, eye, hl, pm);
            }
        }
        state->draws_done++;
```
(`model`/`hl`/`view`/`proj`/`eye` are in scope; the branch runs only when the folderbook was drawn this iteration → the pages follow its visibility + selection highlight. `make_box` is public, centered.)

- [ ] **Step 7: Build gauntlet**

```bash
./build.sh c89check && ./build.sh && ./build.sh metal && ./build.sh carettest && ./caret_test
```
All five must pass. Fix any C89/-Werror/brace issue and re-run until green. Do NOT run `./solarium` (no display).

- [ ] **Step 8: Commit**

```bash
git add mesh.c main.c .gitignore
git commit -m "$(cat <<'EOF'
Folder books: blue leather cover + white page block

The board folderbooks now wear the red_leather PBR material tinted blue
(no albedo map -> base_color carries the color) on the cover, with a
white page block. make_folderbook drops the leaf box; the page block is
a render-time make_box unit cube scaled per folder in the draw loop
(visibility + picking come free). folderbook_materialize re-applies the
leather on every load/rebuild (textures aren't serialized), keeping each
folder's blue shade. red_leather kept local (gitignored). No shader.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-Review notes (for the implementer)

- **Spec coverage:** Step 1 = gitignore; Step 2 = cover-only mesh; Step 3 = `g_book_leather` (no albedo, graceful); Steps 4-5 = `folderbook_materialize` + chokepoint calls (world_rebuild/load_palace/add_folder); Step 6 = the render-derived white box. All spec §1-5.
- **The leaf-block math MUST mirror `make_folderbook`:** `board=fd*0.18` (clamp ≥1e-4 BEFORE using it), `inset=fh*0.05`, `lip=fw*0.03`; box spans x∈[-fw/2+board, fw/2-lip], y∈[inset, fh-inset], z∈[board, fd-board] → `make_box(1,1,1)` scaled by `(fw-lip-board, fh-2*inset, fd-2*board)` translated to `((board-lip)/2, fh/2, fd/2)` reproduces it exactly (verified). Naming `lh2` avoids shadowing any outer `lh`.
- **Material persistence:** `apply_kind_materials` skips `KIND_PLAIN` so it never resets a folderbook; the only fresh-material moments are load (`load_palace`) and creation (`add_folder`) — `folderbook_materialize` covers both (+ `world_rebuild` for the load-derive law). It's idempotent and cheap.
- **Graceful fallback:** if `red_leather/` is absent, `g_book_leather` has no textures → folderbooks stay flat blue (today's look), no crash.
- **C89:** declarations at block top in `folderbook_materialize` (`sol_u32 i; … vec3 keep;` per the loop body) and the draw branch (`float fw,fh,fd,board,inset,lip,lw,lh2,ld;` at the block top, then statements). No `//`.
- **Human live-verify (after build, both backends):** a folder book shows a blue-leather cover (grain/sheen) + white pages; existing folders update on load and keep their individual blue shades; a folder hidden on another board page hides its pages (no orphan white box); clicking a folder still selects/navigates (pages aren't separately clickable); with `red_leather/` removed, folders fall back to flat blue. **Tuning:** if the leather grain reads too large/small (it rides the cover's meter UVs), report it — it's a UV-scale tweak.
