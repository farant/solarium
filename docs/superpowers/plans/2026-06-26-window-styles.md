# Window Styles Implementation Plan (Phase 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give windows five shape styles (plain / arched / pointed / circular / french) cycled with `←/→`, where the shape lives entirely in the window's frame + glass mesh and the wall hole stays a plain rectangle.

**Architecture:** A new `style` geometry param on the `"window"` / `"window_glass"` registry rows drives style-aware `make_window` / `make_window_glass`. The opaque dark_wood frame fills the `w×h` rectangle *minus* the glass aperture (spandrels), spanning the wall depth, so the wall hole + room rebuild + collision are untouched (no CSG). `←/→` mirrors the existing `↑/↓` color handler.

**Tech Stack:** C89, OpenGL + Metal dual backend, the project's mesh registry + the registry-rebuild law. Reuses `make_window`/`make_window_glass`, the `↑/↓`/`window_glass_resize` templates, `window_on_wall`, the arrow-look guard.

---

## Testing approach (read first)

Mesh + GUI in a C89 engine. **Each task's automated gate is the three-target build gauntlet.** The pure-logic pieces — shaped meshes have more geometry than plain — get headless assertions in the existing `routetest` target (it links `mesh.c`). GUI shape correctness (smoothness, no see-through corners, normals/lighting) is human-verified after merge.

**The gauntlet (run after every task):**
```bash
./build.sh c89check   # "c89check: PASS — all sources are C89-pedantic clean"
./build.sh            # "built ./solarium (debug)"
./build.sh metal      # "built ./solarium-metal (stage a: links clean, zero GL; runs from stage b)"
```
**C89 reminders:** locals at top of block; `/* */` only; no decl-after-statement; no signed/unsigned mismatch (strict targets are `-std=c89 -pedantic-errors -Werror -Wextra`).

**Backward-compat fact (verified):** `mesh_ref_build` (mesh.c:1366) merges an object's params over the registry defaults into a full-length `full[MESH_REF_MAX_PARAMS]` array before calling the emit fn — so an existing 4-param window reads `p[4]` as the schema default (we set it `0` = plain). No migration code needed; old windows render plain until cycled.

**Model note:** Tasks 2 and 3 (the no-CSG shape tessellation) are the geometry-heavy work — use a capable model and review carefully. Tasks 1 and 4 are mechanical.

---

## Task 1: The `style` param — plumbing only (everything stays plain)

Thread a `style` param end-to-end without changing any geometry yet (style is accepted but ignored → still plain). This isolates the param-count migration from the shape work.

**Files:** `mesh.h`, `mesh.c`, `main.c`.

- [ ] **Step 1: Update the mesh-gen signatures (mesh.h)**

mesh.h:41-42 become:
```c
void    make_window(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 t, sol_f32 fw, sol_f32 style);
void    make_window_glass(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 style);
```

- [ ] **Step 2: Accept-but-ignore `style` in the mesh-gens (mesh.c)**

In `make_window` add the param and a `(void)style;` for now (keep the existing plain body). Same for `make_window_glass`:
```c
void make_window(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 t, sol_f32 fw, sol_f32 style) {
    sol_f32 hw = w * 0.5f, hh = h * 0.5f, ht = t * 0.5f + WINDOW_PROUD;
    (void)style;   /* Task 2 dispatches on style; plain for now */
    if (fw < 0.01f) fw = 0.01f;
    /* ...existing 4 stiles/rails + sill ledge, unchanged... */
}
void make_window_glass(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 style) {
    sol_f32 hw = w * 0.5f, hh = h * 0.5f;
    sol_u32 v0, v1, v2, v3;
    (void)style;   /* Task 3 dispatches on style; rectangle for now */
    /* ...existing quad, unchanged... */
}
```

- [ ] **Step 3: Update the emit wrappers + registry rows (mesh.c:1033-1034, 1222-1223)**
```c
static void emit_window(MeshBuilder *b, const float *p) { make_window(b, p[0], p[1], p[2], p[3], p[4]); }
static void emit_window_glass(MeshBuilder *b, const float *p) { make_window_glass(b, p[0], p[1], p[2]); }
```
```c
    { "window", 5, { "w", "h", "t", "fw", "style" }, { 1.2f, 1.4f, 0.20f, 0.08f, 0.0f }, emit_window },
    { "window_glass", 3, { "w", "h", "style" }, { 1.2f, 1.4f, 0.0f }, emit_window_glass },
```
(Match the brace-initializer style of the surrounding rows; the name/default arrays are fixed `MESH_REF_MAX_PARAMS`-wide, partial init is valid C89.)

- [ ] **Step 4: `cmd_place_window` writes a 5-element param array (main.c)**

In `cmd_place_window`, the `float p[4];` + `p[0..3]=...` + `scene_mesh_params_set(s, h, p, 4);` block becomes:
```c
        float p[5];
        ...
        p[0] = WINDOW_DEF_W;
        p[1] = WINDOW_DEF_H;
        p[2] = ROUTE_WALL_T;
        p[3] = WINDOW_FRAME_W;
        p[4] = 0.0f;                     /* style: plain */
        ...
        scene_mesh_params_set(s, h, p, 5);
```

- [ ] **Step 5: The window resize path carries `style` (main.c ~10984-11000)**

In the window resize branch, the `float ... p4[4];` becomes `p4[5]`, read the current style BEFORE rewriting, and write 5:
```c
                    float nw, nh, ow, oh, p4[5], style;
                    char  oldkey[160];
                    sol_bool keyed;
                    style = mesh_ref_param("window", o->mesh_params, o->mesh_param_count, "style");
                    ...
                    p4[0] = ow; p4[1] = oh; p4[2] = wt; p4[3] = fw; p4[4] = style;
                    keyed = mesh_asset_key(o, oldkey);
                    scene_mesh_params_set(&st->scene, st->resize_board, p4, 5);
```
(`o` is the window SceneObject already in scope here; read `style` from it before `scene_mesh_params_set`.)

- [ ] **Step 6: `window_glass_resize` carries `style` (main.c)**

`window_glass_resize` rewrites the glass child with `{w,h}` (count 2) → `{w,h,style}` (count 3), reading style from the window:
```c
    float        gp[3];
    ...
    gp[0] = mesh_ref_param("window", wo->mesh_params, wo->mesh_param_count, "w");
    gp[1] = mesh_ref_param("window", wo->mesh_params, wo->mesh_param_count, "h");
    gp[2] = mesh_ref_param("window", wo->mesh_params, wo->mesh_param_count, "style");
    keyed = mesh_asset_key(co, oldkey);
    scene_mesh_params_set(s, child, gp, 3);
```

- [ ] **Step 7: Gauntlet** — all three pass. No visual change (style ignored = plain). Place/resize/color a window: identical to before.

- [ ] **Step 8: Commit**
```bash
git add mesh.h mesh.c main.c
git commit -m "Window styles: thread the style param end-to-end (still plain)"
```

---

## Task 2: The shaped FRAME — `make_window` style dispatch

The opaque dark_wood frame for a shaped style = the existing outer rectangular ring (kept for all styles) **plus** an inner fill = the `w×h` opening rectangle *minus* the glass aperture, built without CSG as a strip between the aperture outline and its radial projection onto the rectangle, spanning the wall depth. French adds cross bars instead.

**Files:** `mesh.c` (helpers + `make_window` dispatch), `route_test.c` (assertion).

- [ ] **Step 1: Constants + shared aperture-outline helper**

Add above `make_window` (mesh.c):
```c
#define WINDOW_ARC_SEG  20    /* segments per semicircle / pointed arc */
#define WINDOW_CIRC_SEG 32    /* segments around a full oculus */
#define WINDOW_OUTLINE_MAX 80 /* max outline points (x,y pairs) */

/* The GLASS-edge outline (the aperture the glass fills), inset by fw inside the
   w/2 x h/2 opening, as a CLOSED CCW loop of (x,y) points. Used by BOTH the
   frame inner-fill (Task 2) and the glass fan (Task 3). Returns the point count
   (0 for plain/french — those use the rectangle directly). xy holds 2*cap floats. */
static int window_outline(int style, float hw, float hh, float fw,
                          float *xy, int cap) {
    int n = 0, i;
    float aw = hw - fw, ah = hh - fw;   /* inset half-extents */
    if (aw < 0.05f) aw = 0.05f;
    if (ah < 0.05f) ah = 0.05f;
    if (style == 3) {                    /* circular: inscribed disc */
        float r = (aw < ah) ? aw : ah;
        for (i = 0; i < WINDOW_CIRC_SEG && n < cap; i++) {
            float a = 6.2831853f * (float)i / (float)WINDOW_CIRC_SEG;
            xy[2*n] = r * cosf(a);
            xy[2*n+1] = r * sinf(a);
            n++;
        }
        return n;
    }
    if (style == 1 || style == 2) {      /* arched / pointed: rect bottom + curved top */
        float r  = (aw < ah) ? aw : ah;          /* arch radius (clamped to fit) */
        float ys = ah - r;                        /* springline (semicircle case) */
        if (style == 2) {                         /* pointed: equilateral-ish apex */
            float apex = ah;                       /* apex at the inset top */
            ys = apex - aw * 1.2f;                 /* springline below apex */
            if (ys < -ah) ys = -ah;
        } else {
            if (ys < -ah) { ys = -ah; r = ah - ys; }
        }
        /* CCW: bottom-left -> bottom-right -> up right side to springline ->
           arc over the top -> down left side back. Sample so the glass fan and
           the fill share identical points. */
        xy[2*n]= -aw; xy[2*n+1]= -ah; n++;        /* bottom-left  */
        xy[2*n]=  aw; xy[2*n+1]= -ah; n++;        /* bottom-right */
        xy[2*n]=  aw; xy[2*n+1]=  ys; n++;        /* right springline */
        if (style == 1) {                          /* semicircle, right->left over top */
            for (i = 1; i < WINDOW_ARC_SEG && n < cap; i++) {
                float a = 3.14159265f * (float)i / (float)WINDOW_ARC_SEG; /* 0..pi */
                xy[2*n]   = r * cosf(a);   /* +r -> -r */
                xy[2*n+1] = ys + r * sinf(a);
                n++;
            }
        } else {                                   /* pointed: right springline -> apex -> left */
            for (i = 1; i < WINDOW_ARC_SEG && n < cap; i++) {
                float u = (float)i / (float)WINDOW_ARC_SEG;
                xy[2*n]   = aw * (1.0f - u);              /* right edge -> center */
                xy[2*n+1] = ys + (ah - ys) * u;          /* up to apex */
                n++;
            }
            for (i = 0; i < WINDOW_ARC_SEG && n < cap; i++) {
                float u = (float)i / (float)WINDOW_ARC_SEG;
                xy[2*n]   = -aw * u;                      /* center -> left edge */
                xy[2*n+1] = ah - (ah - ys) * u;          /* apex down to left springline */
                n++;
            }
        }
        xy[2*n]= -aw; xy[2*n+1]=  ys; n++;        /* left springline (closes the loop) */
        return n;
    }
    (void)cap;
    return 0;   /* plain / french: rectangle, no special outline */
}
```
(mesh.c already `#include <math.h>` and uses `cosf`/`sinf`/`sqrtf` (the float variants — use those, not the double `cos`/`sin`, to match convention and avoid `-Wextra` double-promotion). The pointed construction above is a simple straight-sided "lancet"; if it reads poorly in live-verify, switch to a true two-centered arc — flagged.)

- [ ] **Step 2: The inner-fill helper (rectangle minus aperture, as a solid)**

Add (mesh.c). It emits the opaque dark_wood between the aperture outline and the rectangle, with a front (+ht) cap, a back (-ht) cap, and the inner "tunnel" wall — so it's solid from both faces and at the glass edge. Radial projection puts each outline point on the rectangle boundary:
```c
/* radial projection of (px,py) onto the rect [-hw,hw]x[-hh,hh] boundary */
static void rect_project(float px, float py, float hw, float hh, float *qx, float *qy) {
    float ax = (px < 0 ? -px : px) / hw, ay = (py < 0 ? -py : py) / hh;
    float m  = (ax > ay) ? ax : ay;
    if (m < 1e-4f) m = 1e-4f;
    *qx = px / m; *qy = py / m;
}
/* opaque fill = strip between the aperture outline and its rect projection,
   front (+ht) + back (-ht) caps + inner tunnel wall. CCW outline assumed. */
static void window_fill(MeshBuilder *b, const float *xy, int n,
                        float hw, float hh, float ht) {
    int i;
    for (i = 0; i < n; i++) {
        int   j = (i + 1) % n;
        float ix = xy[2*i], iy = xy[2*i+1], jx = xy[2*j], jy = xy[2*j+1];
        float oix, oiy, ojx, ojy;
        sol_u32 a, c, d, e2;
        rect_project(ix, iy, hw, hh, &oix, &oiy);
        rect_project(jx, jy, hw, hh, &ojx, &ojy);
        /* front cap (+z, normal +z): inner_i, outer_i, outer_j, inner_j */
        a  = mb_push_vertex(b, ix,  iy,  ht, 0,0,1, 0,0);
        c  = mb_push_vertex(b, oix, oiy, ht, 0,0,1, 0,0);
        d  = mb_push_vertex(b, ojx, ojy, ht, 0,0,1, 0,0);
        e2 = mb_push_vertex(b, jx,  jy,  ht, 0,0,1, 0,0);
        mb_push_triangle(b, a, c, d); mb_push_triangle(b, a, d, e2);
        /* back cap (-z, normal -z): reverse winding */
        a  = mb_push_vertex(b, ix,  iy,  -ht, 0,0,-1, 0,0);
        c  = mb_push_vertex(b, oix, oiy, -ht, 0,0,-1, 0,0);
        d  = mb_push_vertex(b, ojx, ojy, -ht, 0,0,-1, 0,0);
        e2 = mb_push_vertex(b, jx,  jy,  -ht, 0,0,-1, 0,0);
        mb_push_triangle(b, a, d, c); mb_push_triangle(b, a, e2, d);
        /* inner tunnel wall (normal points toward the glass = inward = -outward) */
        {
            float nx = -ix, ny = -iy, nl = sqrtf(nx*nx+ny*ny);
            if (nl < 1e-4f) nl = 1e-4f; nx /= nl; ny /= nl;
            a  = mb_push_vertex(b, ix, iy,  ht, nx,ny,0, 0,0);
            c  = mb_push_vertex(b, ix, iy, -ht, nx,ny,0, 0,0);
            d  = mb_push_vertex(b, jx, jy, -ht, nx,ny,0, 0,0);
            e2 = mb_push_vertex(b, jx, jy,  ht, nx,ny,0, 0,0);
            mb_push_triangle(b, a, c, d); mb_push_triangle(b, a, d, e2);
        }
    }
}
```
**IMPORTANT (the no-cull lighting law):** the engine never backface-culls, so each flat face must wind so its normal faces the side it's seen from. Verify front cap faces +z, back cap −z, tunnel inward; if a face renders dark in live-verify, flip its winding. The plan's winding above is the intended one — confirm by build + eyeball.

- [ ] **Step 3: `make_window` dispatches on style**

Replace the `(void)style;` body so the outer ring + sill ledge stay for all styles, and the inner content varies:
```c
void make_window(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 t, sol_f32 fw, sol_f32 style) {
    sol_f32 hw = w * 0.5f, hh = h * 0.5f, ht = t * 0.5f + WINDOW_PROUD;
    int   s = (int)(style + 0.5f);
    float xy[2 * WINDOW_OUTLINE_MAX];
    int   n;
    if (fw < 0.01f) fw = 0.01f;
    /* outer rectangular casing (all styles), proud trim + sill ledge — UNCHANGED */
    aabb_box(b, -hw - fw, -hw,      -hh - fw, hh + fw, -ht, ht);
    aabb_box(b,  hw,       hw + fw, -hh - fw, hh + fw, -ht, ht);
    aabb_box(b, -hw,       hw,       hh,      hh + fw, -ht, ht);
    aabb_box(b, -hw,       hw,      -hh - fw, -hh,     -ht, ht);
    aabb_box(b, -hw - fw,  hw + fw, -hh - fw - 0.03f, -hh - fw, -ht, ht + 0.06f);
    if (s == 4) {                          /* french: cross mullion over the rect glass */
        float bw = fw;
        aabb_box(b, -bw * 0.5f, bw * 0.5f, -hh, hh, -ht, ht);   /* vertical bar   */
        aabb_box(b, -hw, hw, -bw * 0.5f, bw * 0.5f, -ht, ht);   /* horizontal bar */
        return;
    }
    if (s == 1 || s == 2 || s == 3) {      /* shaped: opaque fill = rect minus aperture */
        n = window_outline(s, hw, hh, fw, xy, WINDOW_OUTLINE_MAX);
        if (n >= 3) window_fill(b, xy, n, hw, hh, ht);
    }
    /* s == 0 plain: glass fills the rectangle, no inner fill */
}
```

- [ ] **Step 4: Headless test — shaped frame has more geometry than plain**

In `route_test.c`, add and call from `main`:
```c
static void test_window_frame_styles(void) {
    MeshBuilder mb; sol_u32 plain, arch, circ;
    mb_init(&mb); make_window(&mb, 1.2f, 1.4f, 0.20f, 0.08f, 0.0f); plain = mb.index_count; mb_free(&mb);
    mb_init(&mb); make_window(&mb, 1.2f, 1.4f, 0.20f, 0.08f, 1.0f); arch  = mb.index_count; mb_free(&mb);
    mb_init(&mb); make_window(&mb, 1.2f, 1.4f, 0.20f, 0.08f, 3.0f); circ  = mb.index_count; mb_free(&mb);
    assert(arch > plain);   /* arched spandrel fill adds geometry */
    assert(circ > plain);   /* circular fill adds geometry */
    printf("  window frame styles: plain=%u arch=%u circ=%u OK\n",
           (unsigned)plain, (unsigned)arch, (unsigned)circ);
}
```
Run: `./build.sh routetest && ./route_test` — expect the line + exit 0.

- [ ] **Step 5: Gauntlet** — all three pass (the existing plain windows are unchanged; new styles add geometry).

- [ ] **Step 6: Commit**
```bash
git add mesh.c route_test.c
git commit -m "Window styles: shaped opaque frame (arched/pointed/circular fill + french bars)"
```

---

## Task 3: The shaped GLASS — `make_window_glass` style dispatch

**Files:** `mesh.c`, `route_test.c`.

- [ ] **Step 1: A glass-fan helper + dispatch**

Add the fan helper (a center fan of the aperture outline at z=0, normal +z) and dispatch `make_window_glass`:
```c
static void window_glass_fan(MeshBuilder *b, const float *xy, int n) {
    sol_u32 c, prev, cur;
    int i;
    c = mb_push_vertex(b, 0.0f, 0.0f, 0.0f, 0,0,1, 0.5f,0.5f);   /* center */
    prev = mb_push_vertex(b, xy[0], xy[1], 0.0f, 0,0,1, 0,0);
    for (i = 1; i <= n; i++) {
        int j = i % n;
        cur = mb_push_vertex(b, xy[2*j], xy[2*j+1], 0.0f, 0,0,1, 0,0);
        mb_push_triangle(b, c, prev, cur);
        prev = cur;
    }
}
void make_window_glass(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 style) {
    sol_f32 hw = w * 0.5f, hh = h * 0.5f, fw = WINDOW_FRAME_W;
    int   s = (int)(style + 0.5f);
    float xy[2 * WINDOW_OUTLINE_MAX];
    int   n;
    if (s == 1 || s == 2 || s == 3) {
        n = window_outline(s, hw, hh, fw, xy, WINDOW_OUTLINE_MAX);
        if (n >= 3) { window_glass_fan(b, xy, n); return; }
    }
    {   /* plain / french: the existing rectangle quad (inset slightly by fw so it
           sits inside the frame; keep the current full-rect if you prefer) */
        sol_u32 v0, v1, v2, v3;
        v0 = mb_push_vertex(b, -hw, -hh, 0.0f, 0,0,1, 0,0);
        v1 = mb_push_vertex(b,  hw, -hh, 0.0f, 0,0,1, 1,0);
        v2 = mb_push_vertex(b,  hw,  hh, 0.0f, 0,0,1, 1,1);
        v3 = mb_push_vertex(b, -hw,  hh, 0.0f, 0,0,1, 0,1);
        mb_push_triangle(b, v0, v1, v2); mb_push_triangle(b, v0, v2, v3);
    }
}
```
NOTE: `make_window_glass` does NOT receive `t`/`fw` as params (its registry row is `{w,h,style}`), so it uses the `WINDOW_FRAME_W` constant for the inset — this MUST equal the `fw` the frame uses (placement default 0.08) so the glass edge meets the muntin. (If a window's `fw` ever diverges from the default, the glass inset would mismatch — acceptable since `fw` is constant in this phase; note it.)

- [ ] **Step 2: Headless test — circular glass fan has more tris than the plain quad**

In `route_test.c`, add + call:
```c
static void test_window_glass_styles(void) {
    MeshBuilder mb; sol_u32 rect, disc;
    mb_init(&mb); make_window_glass(&mb, 1.2f, 1.4f, 0.0f); rect = mb.index_count; mb_free(&mb);
    mb_init(&mb); make_window_glass(&mb, 1.2f, 1.4f, 3.0f); disc = mb.index_count; mb_free(&mb);
    assert(disc > rect);   /* a disc fan has many triangles vs a 2-tri quad */
    printf("  window glass styles: rect=%u disc=%u OK\n", (unsigned)rect, (unsigned)disc);
}
```
Run: `./build.sh routetest && ./route_test`.

- [ ] **Step 3: Gauntlet** — all three pass.

- [ ] **Step 4: Commit**
```bash
git add mesh.c route_test.c
git commit -m "Window styles: shaped glass pane (arch/pointed/disc fans)"
```

---

## Task 4: `←/→` cycles style

**Files:** `main.c`.

- [ ] **Step 1: AppState field + init**

Add `sol_bool win_style_was;` beside `win_color_was` in `AppState`, and init it `SOL_FALSE` where `win_color_was` is initialized.

- [ ] **Step 2: Guard `←/→` off camera-look when a window is selected**

In the arrow-look block (main.c ~10566-10569), the existing `win_look_free` gates `↑/↓`. Add the same gate to the `←/→` → `look_dx` lines:
```c
        if (win_look_free && glfwGetKey(w, GLFW_KEY_RIGHT) == GLFW_PRESS) in->look_dx += look;
        if (win_look_free && glfwGetKey(w, GLFW_KEY_LEFT)  == GLFW_PRESS) in->look_dx -= look;
        if (win_look_free && glfwGetKey(w, GLFW_KEY_UP)    == GLFW_PRESS) in->look_dy += look;
        if (win_look_free && glfwGetKey(w, GLFW_KEY_DOWN)  == GLFW_PRESS) in->look_dy -= look;
```
(Only add `win_look_free &&` to the two `look_dx` lines; the `look_dy` lines already have it.)

- [ ] **Step 3: `window_set_style` helper**

Add near `window_set_glass` (main.c). It rewrites `style` on BOTH the window (5 params) and its glass child (3 params) via the registry-rebuild law, re-fetching after each realloc:
```c
static const char *WINDOW_STYLE_NAME[5] = { "plain", "arched", "pointed", "circular", "french" };

static void window_set_style(AppState *st, sol_u32 win, int style) {
    Scene       *s = &st->scene;
    sol_u32      child = window_glass_child(s, win);
    SceneObject *wo = scene_get(s, win);
    float        wp[5], gp[3];
    char         wkey[160], gkey[160];
    sol_bool     wkeyed, gkeyed;
    if (!wo) return;
    wp[0] = mesh_ref_param("window", wo->mesh_params, wo->mesh_param_count, "w");
    wp[1] = mesh_ref_param("window", wo->mesh_params, wo->mesh_param_count, "h");
    wp[2] = mesh_ref_param("window", wo->mesh_params, wo->mesh_param_count, "t");
    wp[3] = mesh_ref_param("window", wo->mesh_params, wo->mesh_param_count, "fw");
    wp[4] = (float)style;
    wkeyed = mesh_asset_key(wo, wkey);
    scene_mesh_params_set(s, win, wp, 5);
    if (wkeyed) asset_release(&g_mesh_assets, wkey);
    wo = scene_get(s, win);
    if (wo) memset(&wo->mesh, 0, sizeof wo->mesh);
    if (child) {
        SceneObject *co = scene_get(s, child);
        gp[0] = wp[0]; gp[1] = wp[1]; gp[2] = (float)style;
        gkeyed = co ? mesh_asset_key(co, gkey) : SOL_FALSE;
        scene_mesh_params_set(s, child, gp, 3);
        if (gkeyed) asset_release(&g_mesh_assets, gkey);
        co = scene_get(s, child);
        if (co) memset(&co->mesh, 0, sizeof co->mesh);
    }
    scene_resolve_meshes(s);
}
```

- [ ] **Step 4: The `←/→` cycle handler**

Add beside the `↑/↓` color handler (main.c ~12096), mirroring it:
```c
    /* Left/Right: cycle the selected window's shape style. */
    if (st->selected_handle != 0 && st->board_view == 0) {
        if (window_on_wall(&st->scene, st->selected_handle)) {
            sol_bool left  = (sol_bool)(glfwGetKey(w, GLFW_KEY_LEFT)  == GLFW_PRESS);
            sol_bool right = (sol_bool)(glfwGetKey(w, GLFW_KEY_RIGHT) == GLFW_PRESS);
            sol_bool now   = (sol_bool)(left || right);
            if (now && !st->win_style_was) {
                SceneObject *so = scene_get(&st->scene, st->selected_handle);
                int idx = so ? (int)(mesh_ref_param("window", so->mesh_params, so->mesh_param_count, "style") + 0.5f) : 0;
                idx = (idx + (right ? 1 : 5 - 1)) % 5;
                window_set_style(st, st->selected_handle, idx);
                scene_save(&st->scene, "scene.stml");
                printf("window style: %s\n", WINDOW_STYLE_NAME[idx]);
            }
            st->win_style_was = now;
        } else {
            st->win_style_was = SOL_FALSE;
        }
    } else {
        st->win_style_was = SOL_FALSE;
    }
```
**NOTE:** this block and the existing `↑/↓` color block both gate on `selected_handle != 0 && board_view == 0 && window_on_wall`. They are independent (one reads `LEFT/RIGHT`, the other `UP/DOWN`); placing this one right after the color block is fine. Do NOT merge them — keep two edge-detect bools.

- [ ] **Step 5: Gauntlet** — all three pass.

- [ ] **Step 6: Commit**
```bash
git add main.c
git commit -m "Window styles: Left/Right cycle the selected window's shape"
```

---

## Final verification (controller, after all tasks)

- [ ] **Full gauntlet:** `./build.sh c89check && ./build.sh && ./build.sh metal` — all pass.
- [ ] **routetest:** `./build.sh routetest && ./route_test` — the frame + glass style assertions pass.
- [ ] **Final holistic review** over the whole diff (especially the tessellation winding/normals and the param-migration seams), then hand to the human.

## Human live-verify checklist (post-merge, both backends)

- Place a window → `←/→` cycles plain → arched → pointed → circular → french → plain; each shows the shaped frame; no see-through corners; arcs read smooth.
- Color a shaped window (`↑/↓`) → the shaped pane tints.
- Resize a shaped window → the shape follows (aperture re-tessellates from the new w/h).
- Reload (`L`) → the style persists; existing pre-Phase-2 windows still render plain.
- Eyeball lighting on the arcs/spandrels (no dark/inside-out faces → winding is right).

## Notes / known limitations (Phase 2)

- The wall opening stays rectangular behind the spandrels (a sliver may show at grazing side angles) — the accepted no-CSG trade.
- The pointed-arch outline is a simple straight-sided lancet; can be upgraded to a true two-centered arc if it reads poorly.
- `make_window_glass` uses the `WINDOW_FRAME_W` constant for the muntin inset (its row has no `fw` param); fine while `fw` is constant.
- No new shader ⇒ no MSL twin.

---

## Self-review notes (author)

- **Spec coverage:** style param + dispatch → Task 1; shaped frame (spandrels/fill, french) → Task 2; shaped glass → Task 3; `←/→` cycle + guard + `window_set_style` → Task 4; resize/color compose + migration → Task 1 (resize/glass_resize carry style) + Task 4. Unchanged wall/room/collision → nothing touches them (verified: only mesh-gen + the handler change). All covered.
- **Type/symbol consistency:** `make_window(...,style)` / `make_window_glass(...,style)`, `window_outline`/`window_fill`/`rect_project`/`window_glass_fan`, `window_set_style`, `WINDOW_STYLE_NAME`, `win_style_was`, `WINDOW_ARC_SEG`/`WINDOW_CIRC_SEG`/`WINDOW_OUTLINE_MAX`, style ints 0-4 used consistently. Registry rows `{w,h,t,fw,style}` / `{w,h,style}` match the emit wrappers' `p[4]`/`p[2]`.
- **Migration risk pinned:** every param-write site (place=5, resize=5, glass_resize=3, window_set_style=5/3) writes the full array carrying style — no path drops it back to 4/2.
