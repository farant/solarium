# World Map Boards Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Place a world map on a wall/table as a board, centered on any lat/lon at a chosen zoom, sourced entirely from a bundled public-domain equirectangular basemap — zero runtime network.

**Architecture:** A new pure `mapmath` module (equirect lon/lat↔UV + crop-window math, headless-tested) drives a custom-UV quad (`make_map_quad`). A map is a `KIND_PLAIN` scene object with `mesh_ref="map"` whose lat/lon/zoom/basemap live in `meta` (auto-persisted). A dedicated pass in `scene_resolve_meshes` builds each map's cropped quad from its meta and binds the shared basemap texture via the existing `load_texture` path. Reuses the lit-albedo "picture" render path — **no new shader, no MSL twin.**

**Tech Stack:** C89 engine (`mapmath.c` pure C89; `mapmath_test.c` may be c11), GLFW/OpenGL + Metal RHI (untouched here), stb_image (existing), the existing scene/mesh/material/palette/meta subsystems.

**Spec:** `docs/superpowers/specs/2026-06-29-world-map-boards-design.md`

**Key facts established by exploration (do not re-derive):**
- Test harness: global `int fails;` + `CHECK(cond,msg)` macro; `main()` calls test fns, prints summary, `return fails ? 1 : 0`. See `caret_test.c`.
- `mb_push_vertex(b, px,py,pz, nx,ny,nz, u,v)` and `mb_push_triangle(b,a,b,c)` (mesh.h:24-26). `make_picture` (mesh.c:371) is the bottom-origin quad template.
- `scene_meta_set/get(s, handle, key, value)` (scene.h:133-134). **Meta auto-persists** via scene_io (no special handling).
- `load_texture(path)` (main.c:12575) → shared sRGB `RhiTexture`, `.id==0` on failure.
- `scene_resolve_meshes` (main.c:13018) skips objects whose `mesh.index_count != 0`; special-cases `"arrow"` with `continue`. The picture-albedo loop is at its tail (main.c:13081).
- Command row: `{ name, hint, key, run, can_run, was_down }` (command.h:13). `g_commands[]` at main.c:10514. `palette_prompt(&st->palette, label, cb)` collects a typed line, fires `cb(st, string)` on Enter (palette.h:39).
- `mint_tag_ws(st, h)` (main.c:8205) tags `meta["workspace"]`. `carry_place_point(st)` returns a drop point in front of the player (used in `place_confirm`, main.c:9960).
- `*.jpg` is **already gitignored** — the basemap images need no `.gitignore` change.

**C89 gotchas baked into the tasks:**
- `g_commands[]` (main.c:10514) references `cmd_place_map`, but `spawn_map_board` calls `scene_resolve_meshes`/`load_texture` (defined ~main.c:12575/13018, *after* the array). → **forward-declare `cmd_place_map` before the array; define the map functions in the region after `scene_resolve_meshes`.**
- `sprintf` is deprecated under `-Werror` (no C89 `snprintf`). → wrap `sprintf` calls in the existing `#pragma clang diagnostic ignored "-Wdeprecated-declarations"` guard, like the HUD does.

---

## File Structure

- **Create** `mapmath.h` / `mapmath.c` — pure equirect math (no GL/IO/engine deps).
- **Create** `mapmath_test.c` — headless test.
- **Modify** `build.sh` — add `mapmathtest` target; add `mapmath.c` to the 4 source lists.
- **Modify** `mesh.h` / `mesh.c` — add `make_map_quad`.
- **Modify** `main.c` — `basemap_path`, the map-resolve pass in `scene_resolve_meshes`, `spawn_map_board`, `place_map_from_coords`, `cmd_place_map`, the `g_commands[]` row + forward decl.
- **Create** `docs/world-basemaps.md` — the one-time data-acquisition note (source URLs + downsample command).

---

## Task 1: `mapmath` pure module + headless test

**Files:**
- Create: `mapmath.h`, `mapmath.c`, `mapmath_test.c`

- [ ] **Step 1: Write `mapmath.h`**

```c
#ifndef MAPMATH_H
#define MAPMATH_H
/* mapmath.h — equirectangular (plate carree) world-map math. Pure: no GL, no
   IO, no engine deps. Maps lon/lat <-> UV into a single equirect world image
   and computes the UV crop window a map board shows. See the world-map-boards
   design spec. */

/* lon in [-180,180], lat in [-90,90]; u,v in [0,1]. v=0 is the SOUTH edge
   (lat -90), v=1 the NORTH edge (+90): this matches image_load's vertical flip
   so north ends up at the top of the board. Inputs are clamped. */
void mapmath_lonlat_to_uv(double lon, double lat, double *u, double *v);
void mapmath_uv_to_lonlat(double u, double v, double *lon, double *lat);

/* The UV rectangle a board centered at (lon,lat) shows at integer zoom z
   (0 = whole world) for a board of the given aspect (= width/height). The shown
   width fraction is du = 1/2^z of the image; dv = 2*du/aspect keeps the geography
   undistorted (the image is 2:1, W=2H). The window is clamped into [0,1]; if it
   would cross an edge the center is shifted to keep it fully inside (no
   antimeridian/pole wrap in v1). Always yields u0<u1, v0<v1. */
void mapmath_window(double lon, double lat, int z, double aspect,
                    double *u0, double *v0, double *u1, double *v1);

#endif /* MAPMATH_H */
```

- [ ] **Step 2: Write the failing test `mapmath_test.c`**

```c
/* mapmath_test.c — pure-logic test for mapmath.c (equirect map math). GL-free,
   ASan/UBSan via `build.sh mapmathtest`. */
#include "mapmath.h"
#include <stdio.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)

static int near(double a, double b) { double d = a - b; if (d < 0) d = -d; return d < 1e-9; }

static void test_lonlat_corners(void) {
    double u, v;
    mapmath_lonlat_to_uv(-180.0, -90.0, &u, &v);
    CHECK(near(u, 0.0) && near(v, 0.0), "SW corner -> (0,0)");
    mapmath_lonlat_to_uv(180.0, 90.0, &u, &v);
    CHECK(near(u, 1.0) && near(v, 1.0), "NE corner -> (1,1)");
    mapmath_lonlat_to_uv(0.0, 0.0, &u, &v);
    CHECK(near(u, 0.5) && near(v, 0.5), "origin -> (0.5,0.5)");
}

static void test_roundtrip(void) {
    double u, v, lon, lat;
    mapmath_lonlat_to_uv(2.35, 48.85, &u, &v);       /* Paris */
    mapmath_uv_to_lonlat(u, v, &lon, &lat);
    CHECK(near(lon, 2.35) && near(lat, 48.85), "roundtrip Paris");
}

static void test_clamp_inputs(void) {
    double u, v;
    mapmath_lonlat_to_uv(999.0, -999.0, &u, &v);
    CHECK(near(u, 1.0) && near(v, 0.0), "out-of-range clamps to edges");
}

static void test_window_center(void) {
    double u0, v0, u1, v1;
    /* z=4, aspect 2, centered at origin: du = 1/16, dv = du. */
    mapmath_window(0.0, 0.0, 4, 2.0, &u0, &v0, &u1, &v1);
    CHECK(near(u1 - u0, 1.0 / 16.0), "z4 width = 1/16");
    CHECK(near(v1 - v0, 1.0 / 16.0), "z4 height = 1/16 at aspect 2");
    CHECK(near((u0 + u1) * 0.5, 0.5) && near((v0 + v1) * 0.5, 0.5), "centered at origin");
}

static void test_window_aspect(void) {
    double u0, v0, u1, v1;
    mapmath_window(0.0, 0.0, 4, 1.0, &u0, &v0, &u1, &v1);  /* square board */
    CHECK(near(u1 - u0, 1.0 / 16.0), "aspect1 width unchanged");
    CHECK(near(v1 - v0, 2.0 / 16.0), "aspect1 height = 2*du");
}

static void test_window_z0_full(void) {
    double u0, v0, u1, v1;
    mapmath_window(0.0, 0.0, 0, 2.0, &u0, &v0, &u1, &v1);
    CHECK(near(u0, 0.0) && near(u1, 1.0) && near(v0, 0.0) && near(v1, 1.0), "z0 = whole world");
}

static void test_window_edge_shift(void) {
    double u0, v0, u1, v1;
    /* near the north pole / antimeridian, the window must stay inside [0,1]
       at full size (center shifted, not cropped). */
    mapmath_window(179.0, 89.0, 4, 2.0, &u0, &v0, &u1, &v1);
    CHECK(near(u1 - u0, 1.0 / 16.0) && near(v1 - v0, 1.0 / 16.0), "edge window keeps size");
    CHECK(u0 >= 0.0 && u1 <= 1.0 && v0 >= 0.0 && v1 <= 1.0, "edge window inside [0,1]");
}

int main(void) {
    test_lonlat_corners();
    test_roundtrip();
    test_clamp_inputs();
    test_window_center();
    test_window_aspect();
    test_window_z0_full();
    test_window_edge_shift();
    if (fails == 0) printf("mapmath_test: all passed\n");
    return fails ? 1 : 0;
}
```

- [ ] **Step 3: Add a temporary build line and run to verify it FAILS to link** (no `mapmath.c` yet)

Run: `clang -std=c11 -g -fsanitize=address,undefined mapmath.c mapmath_test.c -o /tmp/mm 2>&1 | head`
Expected: FAIL — `mapmath.c` does not exist / undefined symbols. (Confirms the test references real symbols.)

- [ ] **Step 4: Write `mapmath.c`**

```c
#include "mapmath.h"

static double clampd(double x, double lo, double hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

void mapmath_lonlat_to_uv(double lon, double lat, double *u, double *v) {
    lon = clampd(lon, -180.0, 180.0);
    lat = clampd(lat,  -90.0,  90.0);
    *u = (lon + 180.0) / 360.0;
    *v = (lat +  90.0) / 180.0;
}

void mapmath_uv_to_lonlat(double u, double v, double *lon, double *lat) {
    u = clampd(u, 0.0, 1.0);
    v = clampd(v, 0.0, 1.0);
    *lon = u * 360.0 - 180.0;
    *lat = v * 180.0 -  90.0;
}

void mapmath_window(double lon, double lat, int z, double aspect,
                    double *u0, double *v0, double *u1, double *v1) {
    double du, dv, cu, cv, hu, hv;
    if (z < 0) z = 0;
    if (aspect <= 0.0) aspect = 2.0;
    du = 1.0 / (double)(1 << z);   /* fraction of image WIDTH shown */
    dv = 2.0 * du / aspect;        /* undistorted: image is 2:1 (W = 2H) */
    if (du > 1.0) du = 1.0;
    if (dv > 1.0) dv = 1.0;
    mapmath_lonlat_to_uv(lon, lat, &cu, &cv);
    hu = du * 0.5;
    hv = dv * 0.5;
    cu = clampd(cu, hu, 1.0 - hu); /* shift center so the window fits inside */
    cv = clampd(cv, hv, 1.0 - hv);
    *u0 = cu - hu; *u1 = cu + hu;
    *v0 = cv - hv; *v1 = cv + hv;
}
```

- [ ] **Step 5: Build & run the test (verify PASS)**

Run: `clang -std=c11 -g -fsanitize=address,undefined mapmath.c mapmath_test.c -o /tmp/mm && /tmp/mm`
Expected: `mapmath_test: all passed`, exit 0, no sanitizer output.

- [ ] **Step 6: Verify `mapmath.c` is C89-clean**

Run: `clang -std=c89 -pedantic-errors -Werror -Wall -Wextra -fsyntax-only mapmath.c`
Expected: no output (clean).

- [ ] **Step 7: Commit**

```bash
git add mapmath.h mapmath.c mapmath_test.c
git commit -m "$(printf 'map: equirect lon/lat<->UV + crop-window math (pure module + test)\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 2: Wire `mapmath` into build.sh

**Files:**
- Modify: `build.sh`

- [ ] **Step 1: Add the `mapmathtest` target** (place it next to the other `*test` blocks, e.g. after the `carettest` block ~line 88)

```bash
# mapmathtest: the equirect map-math pure module (scene-free C89). libc only.
if [ "$MODE" = "mapmathtest" ]; then
    set -x
    clang -std=c89 -pedantic-errors -Werror -g -fsanitize=address,undefined \
        mapmath.c mapmath_test.c \
        -o mapmath_test
    echo "built ./mapmath_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi
```

- [ ] **Step 2: Add `mapmath.c` to all four engine source lists**

In each of the four lists (c89check ~line 16, Metal ~line 434, ASan ~line 456, default ~line 476), insert `mapmath.c ` immediately **after** `material.c ` (the literal substring `material.c scene_io.c` becomes `material.c mapmath.c scene_io.c`). Apply to all four occurrences. (Note: the Metal list orders it `... material.c scene_io.c nid.c stml.c ...` — match each list's exact surrounding text; the common anchor `material.c ` works in all four.)

- [ ] **Step 3: Verify the test target builds & passes**

Run: `./build.sh mapmathtest && ./mapmath_test`
Expected: `mapmath_test: all passed`.

- [ ] **Step 4: Verify c89check still passes** (it now lints `mapmath.c`)

Run: `./build.sh c89check`
Expected: `c89check: PASS — all sources are C89-pedantic clean`.

- [ ] **Step 5: Commit**

```bash
git add build.sh
git commit -m "$(printf 'build: add mapmathtest target + mapmath.c to engine sources\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 3: `make_map_quad` — custom-UV quad

**Files:**
- Modify: `mesh.h` (declaration), `mesh.c` (definition, next to `make_picture` ~line 371)

- [ ] **Step 1: Declare in `mesh.h`** (next to the `make_picture` declaration ~mesh.h:40)

```c
/* like make_picture but with an explicit UV crop window (for map boards that
   sample a sub-rectangle of a shared equirectangular basemap). Bottom-origin
   +Z quad, w x h meters, UVs (u0,v0)=bottom-left .. (u1,v1)=top-right. */
void    make_map_quad(MeshBuilder *b, sol_f32 w, sol_f32 h,
                      sol_f32 u0, sol_f32 v0, sol_f32 u1, sol_f32 v1);
```

- [ ] **Step 2: Define in `mesh.c`** (immediately after `make_picture`)

```c
void make_map_quad(MeshBuilder *b, sol_f32 w, sol_f32 h,
                   sol_f32 u0, sol_f32 v0, sol_f32 u1, sol_f32 v1) {
    sol_f32 hw = w * 0.5f;
    sol_u32 a, c, d, e;
    a = mb_push_vertex(b, -hw, 0.0f, 0.0f,  0.0f, 0.0f, 1.0f,  u0, v0);  /* BL */
    c = mb_push_vertex(b,  hw, 0.0f, 0.0f,  0.0f, 0.0f, 1.0f,  u1, v0);  /* BR */
    d = mb_push_vertex(b,  hw, h,    0.0f,  0.0f, 0.0f, 1.0f,  u1, v1);  /* TR */
    e = mb_push_vertex(b, -hw, h,    0.0f,  0.0f, 0.0f, 1.0f,  u0, v1);  /* TL */
    mb_push_triangle(b, a, c, d);
    mb_push_triangle(b, a, d, e);
}
```

Note: this is **not** registered in the mesh-ref registry — map geometry is built directly from per-object meta (Task 4), like `"arrow"` is special-cased.

- [ ] **Step 3: Build-verify (compiles in the engine + the C89 check)**

Run: `./build.sh c89check`
Expected: PASS. (mesh.c is in the c89check list and the furniture_test link, so a signature/decl mismatch surfaces here.)

- [ ] **Step 4: Commit**

```bash
git add mesh.h mesh.c
git commit -m "$(printf 'mesh: make_map_quad — quad with an explicit UV crop window\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 4: Map-resolve pass — build geometry + bind basemap from meta

**Files:**
- Modify: `main.c` (in `scene_resolve_meshes`, ~13018–13086)

- [ ] **Step 1: Add map constants + `basemap_path`** (file-scope, above `scene_resolve_meshes`; place after `load_texture` so no forward decls are needed)

```c
/* world-map boards: bundled public-domain equirectangular basemaps (gitignored
   sourced-binary exception). A map board is a quad cropped to a lon/lat window. */
#define MAP_BOARD_W 1.6f
#define MAP_BOARD_H 0.8f          /* aspect 2:1, matching the equirect source */
#define MAP_ZMAX    6

static const char *basemap_path(const char *style) {
    if (style && strcmp(style, "satellite") == 0) return "basemap_satellite.jpg";
    return "basemap_relief.jpg";  /* default: Natural Earth relief */
}
```

- [ ] **Step 2: Skip `"map"` in the generic mesh-ref resolve loop** (next to the existing `"arrow"` skip near the top of `scene_resolve_meshes`)

Find:
```c
        if (!o->mesh_ref || o->mesh.index_count != 0) continue;
        if (strcmp(o->mesh_ref, "arrow") == 0) continue;
```
Change to:
```c
        if (!o->mesh_ref || o->mesh.index_count != 0) continue;
        if (strcmp(o->mesh_ref, "arrow") == 0) continue;
        if (strcmp(o->mesh_ref, "map") == 0) continue;   /* built from meta below */
```

- [ ] **Step 3: Add the map-build loop** at the **tail** of `scene_resolve_meshes`, immediately after the picture-albedo loop (main.c:13081-13086)

```c
    /* map boards: build the lon/lat-cropped quad from meta and bind the bundled
       equirectangular basemap. Geometry is per-object (unique UVs) so it is NOT
       routed through the shared mesh asset store; the basemap texture IS shared
       via load_texture. Missing basemap file -> a flat placeholder tint. */
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        const char *slat, *slon, *szoom, *style;
        double lon, lat, u0, v0, u1, v1;
        int z;
        MeshBuilder mb;
        if (!o->mesh_ref || strcmp(o->mesh_ref, "map") != 0) continue;
        if (o->mesh.index_count != 0) continue;
        slat  = scene_meta_get(s, o->handle, "lat");
        slon  = scene_meta_get(s, o->handle, "lon");
        szoom = scene_meta_get(s, o->handle, "zoom");
        style = scene_meta_get(s, o->handle, "basemap");
        lat = slat  ? atof(slat)  : 0.0;
        lon = slon  ? atof(slon)  : 0.0;
        z   = szoom ? atoi(szoom) : 0;
        mapmath_window(lon, lat, z, (double)(MAP_BOARD_W / MAP_BOARD_H),
                       &u0, &v0, &u1, &v1);
        mb_init(&mb);
        make_map_quad(&mb, MAP_BOARD_W, MAP_BOARD_H,
                      (sol_f32)u0, (sol_f32)v0, (sol_f32)u1, (sol_f32)v1);
        o->mesh = mesh_from_builder(&mb);
        mb_free(&mb);
        o->material = material_default();
        o->material.albedo_tex = load_texture(basemap_path(style));
        if (!o->material.albedo_tex.id)
            o->material.base_color = vec3_make(0.32f, 0.34f, 0.38f); /* "no basemap" */
    }
```

- [ ] **Step 4: Add `#include "mapmath.h"`** to main.c's includes (with the other engine headers near the top).

- [ ] **Step 5: Build the engine (default + metal + c89check)**

Run: `./build.sh c89check && ./build.sh && ./build.sh metal`
Expected: all succeed. (No map objects exist yet, so nothing renders — this verifies it compiles and links on both backends.)

- [ ] **Step 6: Commit**

```bash
git add main.c
git commit -m "$(printf 'map: scene_resolve_meshes builds map boards from meta + binds basemap\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 5: "Place map" command — prompt, spawn, tag, persist

**Files:**
- Modify: `main.c` (forward decl before `g_commands[]` ~10514; definitions after `scene_resolve_meshes` ~13018; one registry row)

- [ ] **Step 1: Forward-declare `cmd_place_map` before `g_commands[]`** (with the other forward decls / just above the array at ~main.c:10513)

```c
static void cmd_place_map(AppState *st);
```

- [ ] **Step 2: Add the registry row** to `g_commands[]` (main.c:10514, alongside `cmd_place_furniture`)

```c
    { "Place map",                   NULL, 0,          cmd_place_map,         NULL,                  SOL_FALSE },
```

- [ ] **Step 3: Define the spawn + prompt + command** in the region **after** `scene_resolve_meshes` (so `scene_resolve_meshes`, `load_texture`, `basemap_path`, `carry_place_point` are all already defined — no forward decls needed)

```c
/* spawn a map board ~in front of the player (like a dropped card). lat/lon/zoom
   + basemap go to meta (auto-persisted); geometry+texture are built by the
   map-resolve pass. */
static sol_u32 spawn_map_board(AppState *st, double lat, double lon, int z,
                               const char *style) {
    Mesh    empty;
    vec3    pos = carry_place_point(st);
    quat    rot = carry_place_rot(st);   /* same facing a dropped card uses; see note */
    sol_u32 h;
    char    buf[32];
    memset(&empty, 0, sizeof empty);
    h = scene_add(&st->scene, 0, empty, pos, rot, vec3_make(1.0f, 1.0f, 1.0f));
    scene_mesh_ref_set(&st->scene, h, "map");
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    sprintf(buf, "%.6f", lat);  scene_meta_set(&st->scene, h, "lat",  buf);
    sprintf(buf, "%.6f", lon);  scene_meta_set(&st->scene, h, "lon",  buf);
    sprintf(buf, "%d",    z);   scene_meta_set(&st->scene, h, "zoom", buf);
#pragma clang diagnostic pop
    scene_meta_set(&st->scene, h, "basemap", style && style[0] ? style : "relief");
    mint_tag_ws(st, h);
    scene_resolve_meshes(&st->scene);
    return h;
}

static void place_map_from_coords(AppState *st, const char *s) {
    double lat = 0.0, lon = 0.0;
    int    z = 0, n;
    char   style[16];
    if (!s || !s[0]) return;
    style[0] = '\0';
    n = sscanf(s, "%lf , %lf , %d , %15s", &lat, &lon, &z, style);
    if (n < 3) { printf("map: enter 'lat,lon,zoom' e.g. 48.85,2.35,5\n"); return; }
    if (lat < -90.0)  lat = -90.0;  if (lat > 90.0)  lat = 90.0;
    if (lon < -180.0) lon = -180.0; if (lon > 180.0) lon = 180.0;
    if (z < 0) z = 0; if (z > MAP_ZMAX) z = MAP_ZMAX;
    spawn_map_board(st, lat, lon, z, style);
    scene_save(&st->scene, "scene.stml");
    printf("placed map @ %.4f,%.4f z%d\n", lat, lon, z);
}

static void cmd_place_map(AppState *st) {
    palette_prompt(&st->palette, "map (lat,lon,zoom)", place_map_from_coords);
}
```

**Implementer note on `carry_place_rot`:** if no such helper exists, orient the map exactly like the existing image-card drop does — inspect the caller of `spawn_image_picture` (main.c:~9075) for how it builds the drop `rot`, and use the same expression (a yaw quat facing the player). A wrong facing is a cosmetic live-verify fix, not a blocker; if unsure, use the card-drop rotation verbatim.

- [ ] **Step 4: Build (default + metal + c89check)**

Run: `./build.sh c89check && ./build.sh && ./build.sh metal`
Expected: all succeed.

- [ ] **Step 5: Commit**

```bash
git add main.c
git commit -m "$(printf 'map: Place map palette command (prompt lat/lon/zoom, spawn, persist)\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 6: Data-acquisition note (bundled basemaps)

**Files:**
- Create: `docs/world-basemaps.md`

- [ ] **Step 1: Write the acquisition note** so the bundled pack is reproducible. (The engine never downloads these; they are a one-time manual prep. `*.jpg` is already gitignored.)

```markdown
# World basemaps (bundled, public domain)

The map-board feature reads two equirectangular world images from the working
directory. Both are **public domain** — embed freely, no attribution required.
They are gitignored (`*.jpg`); acquire them once with the steps below.

| File | Source | License |
|------|--------|---------|
| `basemap_relief.jpg`    | Natural Earth raster (e.g. "Natural Earth II with shaded relief", 1:10m) — naturalearthdata.com | Public domain |
| `basemap_satellite.jpg` | NASA Blue Marble Next Generation — visibleearth.nasa.gov | Public domain |

Downsample each to a common 8192×4096 equirectangular (≈134 MB VRAM each; loaded
lazily, so an unused style never loads). On macOS:

    sips -z 4096 8192 source.tif --out basemap_relief.jpg
    # or: magick source.tif -resize 8192x4096! basemap_relief.jpg

If a file is absent, its map boards render a flat placeholder tint (no crash).
```

- [ ] **Step 2: Commit**

```bash
git add docs/world-basemaps.md
git commit -m "$(printf 'docs: how to fetch the bundled public-domain world basemaps\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 7 (stretch — optional, sequence last): basemap toggle + re-center

A placed map's view is fixed at creation. This task makes a *selected* map's basemap toggle and its center/zoom editable by re-running the prompt, rebuilding geometry+texture in place.

**Files:** Modify `main.c`.

- [ ] **Step 1:** Add `cmd_map_retarget` (palette): if a map is selected (`st->selected_handle` is a `mesh_ref=="map"` object), `palette_prompt` for new `lat,lon,zoom[,style]`; on confirm update its meta, then rebuild **in place**:

```c
    /* free the old per-object mesh and clear it so the resolve pass rebuilds.
       maps own their mesh directly (not the shared store) -> mesh_destroy is
       correct here (unlike registry-shared shapes). */
    mesh_destroy(&o->mesh);
    memset(&o->mesh, 0, sizeof o->mesh);
    o->material.albedo_tex.id = 0;          /* re-resolve basemap */
    scene_resolve_meshes(&st->scene);
    scene_save(&st->scene, "scene.stml");
```

- [ ] **Step 2:** Register `{ "Retarget map", NULL, 0, cmd_map_retarget, NULL, SOL_FALSE }` (with a `can_run` that returns true only when a map is selected, if that pattern is used nearby).
- [ ] **Step 3:** Build (default + metal + c89check), commit.

**Note:** corner-drag *resize* of a map (re-deriving the window to stay undistorted) is deliberately out of v1 — it needs the "viewport re-derive" behavior and is a separate follow-up.

---

## Final verification (whole feature)

- [ ] `./build.sh c89check` → PASS
- [ ] `./build.sh mapmathtest && ./mapmath_test` → all passed
- [ ] `./build.sh` (gl) and `./build.sh metal` → both build
- [ ] `./build.sh asan` → builds (ASan/UBSan engine)
- [ ] **Human live-verify (subagents cannot GUI-test):** acquire the two basemaps (Task 6), launch, run `:` → "Place map" → e.g. `48.85,2.35,5`, confirm the right region shows right-side-up; reload and confirm the map persists with the same view; try `satellite` style; confirm a missing basemap shows the placeholder tint rather than crashing.

**Live-verify watch-items:** (1) **vertical orientation** — if the map is upside down, flip the `v` convention in `mapmath_lonlat_to_uv` (and the test expectations); the comment documents the assumption. (2) **facing** — the map should face the player when dropped (see the `carry_place_rot` note). (3) **mesh ownership** — maps build their own mesh; confirm deleting a map (existing delete handler) frees cleanly under ASan.
