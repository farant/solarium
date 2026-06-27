# Gable Windows Implementation Plan (Phase 3)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let windows sit in the gable triangle and span the wall→gable seam — the gable mesh learns to cut a rectangular notch for any window whose top rises above the wall.

**Architecture:** A window's opening splits at the wall top: the wall shell skips openings entirely above the wall (and naturally caps spanning ones); the gable's solid `gable_tri` is replaced by a triangle-minus-rectangle builder that cuts the part above. The window's own frame/oak-fill/glass meshes make the shape (rectangular notch + circular meshes = a rose window). No window-mesh changes.

**Tech Stack:** C89, OpenGL + Metal dual backend, the room-frame mesh builders. Reuses `bent_pt`, `gable_tri`, `room_frame_build`, `room_rebuild_one`, the gable-aware move/resize clamps.

---

## Testing approach (read first)

Each task's automated gate is the three-target build gauntlet. The wall-shell fix is headlessly testable (the gable notch lives in `room_frame_build` in main.c and isn't headlessly linkable, so it's gauntlet + human live-verify).

**Gauntlet (after every task):**
```bash
./build.sh c89check   # "c89check: PASS — all sources are C89-pedantic clean"
./build.sh            # "built ./solarium (debug)"
./build.sh metal      # "built ./solarium-metal (stage a: links clean, zero GL; runs from stage b)"
```
**C89 reminders:** locals at top of block; `/* */` only; no decl-after-statement; no signed/unsigned mismatch (-std=c89 -pedantic-errors -Werror -Wextra); use float trig `cosf/sinf/sqrtf`.

**Model note:** Task 2 (the no-CSG triangle-minus-rectangle tessellation) is the geometry-heavy task — capable model + careful review. Tasks 1 and 3 are small.

**Key verified facts:**
- `bent_pt(rax, along, span, y)` = `rax ? (along, y, span) : (span, y, along)` (main.c). For the gable: `bent_pt(rax, ge, s, y)` maps the gable's `(s,y)` plane to 3D on the inner face; `+ off` → outer face.
- The gable `(s,y)` plane: base `[-sh, sh]` at `y = h` (wall top), apex `(0, ridge_y)`. `halfw(y) = sh*(ridge_y - y)/(ridge_y - h)`.
- `gable_tri` inner-face UVs are position-based: `u = s/WALL_TILE_M`, `v = y/WALL_TILE_M` (`WALL_TILE_M 3.0`).
- The gable end → wall index: `rax ? (gi ? E : W) : (gi ? S : N)` (gi=1 ⇒ `ge=+along_h`). `ROOM_WALL_N/E/S/W` = 0/1/2/3.
- A window opening's `center` is room-local along the wall run = the gable `s` directly; `height` = lintel = `pos.y + h/2`; `sill` = `pos.y − h/2`.

---

## Task 1: Wall shell skips above-wall openings

**Files:** `mesh.c` (`emit_doored_wall` gather loop), `route_test.c` (test).

- [ ] **Step 1: Write the failing test (route_test.c)**

Add and call from `main`. A window entirely above the wall top must leave the wall identical to no opening; a spanning one must differ from a fully-below one:
```c
static void test_window_above_wall(void) {
    MeshBuilder mb;
    RoomOpening below, above, span;
    sol_u32 none_idx, below_idx, above_idx, span_idx;

    /* no opening */
    mb_init(&mb); make_room_doored(&mb, 6.0f, 4.0f, 3.0f, 0.20f, 1,1,1,1, 0, (RoomOpening*)0, 0);
    none_idx = mb.index_count; mb_free(&mb);

    below.wall = ROOM_WALL_N; below.center = 0.0f; below.width = 1.2f;
    below.height = 2.3f; below.sill = 0.9f;                 /* fully below h=3.0 */
    mb_init(&mb); make_room_doored(&mb, 6.0f, 4.0f, 3.0f, 0.20f, 1,1,1,1, 0, &below, 1);
    below_idx = mb.index_count; mb_free(&mb);

    above = below; above.sill = 3.4f; above.height = 4.4f;  /* entirely above h -> skipped */
    mb_init(&mb); make_room_doored(&mb, 6.0f, 4.0f, 3.0f, 0.20f, 1,1,1,1, 0, &above, 1);
    above_idx = mb.index_count; mb_free(&mb);

    span = below; span.sill = 2.4f; span.height = 4.0f;     /* spans the wall top */
    mb_init(&mb); make_room_doored(&mb, 6.0f, 4.0f, 3.0f, 0.20f, 1,1,1,1, 0, &span, 1);
    span_idx = mb.index_count; mb_free(&mb);

    assert(above_idx == none_idx);   /* above-wall window leaves the wall solid */
    assert(below_idx != none_idx);   /* a real window cuts the wall */
    assert(span_idx  != below_idx);  /* spanning differs (reaches the wall top, no header) */
    printf("  window above wall: none=%u below=%u above=%u span=%u OK\n",
           (unsigned)none_idx, (unsigned)below_idx, (unsigned)above_idx, (unsigned)span_idx);
}
```

- [ ] **Step 2: Run — it FAILS** (`./build.sh routetest && ./route_test`): without the skip, `above_idx != none_idx` (the above-wall window still cuts a gap + a too-tall sill box).

- [ ] **Step 3: Add the skip (mesh.c `emit_doored_wall` gather loop)**

In the `for (i = 0; i < n_ops; i++)` gather loop, right after `if (ops[i].wall != wall_id) continue;`, add:
```c
        if (ops[i].sill >= h) continue;   /* entirely above the wall: it's a gable window — wall stays solid */
```

- [ ] **Step 4: Run — it PASSES.** `./build.sh routetest && ./route_test` prints the line, exit 0.

- [ ] **Step 5: Gauntlet** — all three pass (existing windows/doors unchanged: their sill < h).

- [ ] **Step 6: Commit**
```bash
git add mesh.c route_test.c
git commit -m "Gable windows: wall shell skips openings entirely above the wall top"
```

---

## Task 2: The gable cuts the notch (triangle-minus-rectangle)

**Files:** `main.c` — three static helpers before `room_frame_build`, and the `gi`-loop change inside it.

- [ ] **Step 1: Add the geometry helpers (main.c, before `room_frame_build`)**

```c
/* push one gable-plane vertex: (s,y) -> 3D via bent_pt + offv, position UVs. */
static sol_u32 g_push(MeshBuilder *mb, int rax, float ge, vec3 offv, vec3 n, float s, float y) {
    vec3 p = vec3_add(bent_pt(rax, ge, s, y), offv);
    return mb_push_vertex(mb, p.x, p.y, p.z, n.x, n.y, n.z, s / WALL_TILE_M, y / WALL_TILE_M);
}

/* one gable end FACE (inner: offv=0,n=nin / outer: offv=off,n=nout) as the
   triangle (base [-sh,sh] at hwall, apex (0,ridge_y)) MINUS the notch
   [s0,s1]x[yb,yt]. Four regions: bottom band, left strip, right strip, top piece. */
static void gable_face_notched(MeshBuilder *mb, int rax, float ge, vec3 offv, vec3 n,
                               float sh, float hwall, float ridge_y,
                               float s0, float s1, float yb, float yt) {
    float spanH = ridge_y - hwall;
    float hwb   = sh * (ridge_y - yb) / spanH;   /* halfwidth at yb */
    float hwt   = sh * (ridge_y - yt) / spanH;   /* halfwidth at yt */
    sol_u32 a, b, c, d;
    if (yb > hwall + 1e-4f) {                     /* bottom band: full slab hwall..yb */
        a = g_push(mb,rax,ge,offv,n, -sh,  hwall);
        b = g_push(mb,rax,ge,offv,n,  sh,  hwall);
        c = g_push(mb,rax,ge,offv,n,  hwb, yb);
        d = g_push(mb,rax,ge,offv,n, -hwb, yb);
        mb_push_triangle(mb,a,b,c); mb_push_triangle(mb,a,c,d);
    }
    a = g_push(mb,rax,ge,offv,n, -hwb, yb);       /* left strip yb..yt */
    b = g_push(mb,rax,ge,offv,n,  s0,  yb);
    c = g_push(mb,rax,ge,offv,n,  s0,  yt);
    d = g_push(mb,rax,ge,offv,n, -hwt, yt);
    mb_push_triangle(mb,a,b,c); mb_push_triangle(mb,a,c,d);
    a = g_push(mb,rax,ge,offv,n,  s1,  yb);       /* right strip yb..yt */
    b = g_push(mb,rax,ge,offv,n,  hwb, yb);
    c = g_push(mb,rax,ge,offv,n,  hwt, yt);
    d = g_push(mb,rax,ge,offv,n,  s1,  yt);
    mb_push_triangle(mb,a,b,c); mb_push_triangle(mb,a,c,d);
    if (yt < ridge_y - 1e-4f) {                   /* top piece yt..apex (triangle) */
        a = g_push(mb,rax,ge,offv,n, -hwt, yt);
        b = g_push(mb,rax,ge,offv,n,  hwt, yt);
        c = g_push(mb,rax,ge,offv,n,  0.0f, ridge_y);
        mb_push_triangle(mb,a,b,c);
    }
}

/* the notch's inner hole walls (solid, gable material), each spanning inner->outer
   (offv 0->off) so you don't see through the slab thickness. Normals point into
   the hole. `has_bottom` adds the bottom wall (interior/gable-only notch). */
static void gable_notch_reveal(MeshBuilder *mb, int rax, float ge, vec3 off,
                               float s0, float s1, float yb, float yt, int has_bottom) {
    vec3 su = rax ? vec3_make(0.0f,0.0f,1.0f) : vec3_make(1.0f,0.0f,0.0f);  /* +s direction */
    /* a wall quad between two (s,y) points, inner(0)->outer(off), normal n */
    /* left wall s=s0 (faces +s into hole) */
    { vec3 n = su; vec3 ia=bent_pt(rax,ge,s0,yb), ib=bent_pt(rax,ge,s0,yt);
      vec3 oa=vec3_add(ia,off), ob=vec3_add(ib,off); sol_u32 v0,v1,v2,v3;
      v0=mb_push_vertex(mb,ia.x,ia.y,ia.z,n.x,n.y,n.z,0,0);
      v1=mb_push_vertex(mb,ib.x,ib.y,ib.z,n.x,n.y,n.z,1,0);
      v2=mb_push_vertex(mb,ob.x,ob.y,ob.z,n.x,n.y,n.z,1,1);
      v3=mb_push_vertex(mb,oa.x,oa.y,oa.z,n.x,n.y,n.z,0,1);
      mb_push_triangle(mb,v0,v1,v2); mb_push_triangle(mb,v0,v2,v3); }
    /* right wall s=s1 (faces -s) */
    { vec3 n = vec3_scale(su,-1.0f); vec3 ia=bent_pt(rax,ge,s1,yt), ib=bent_pt(rax,ge,s1,yb);
      vec3 oa=vec3_add(ia,off), ob=vec3_add(ib,off); sol_u32 v0,v1,v2,v3;
      v0=mb_push_vertex(mb,ia.x,ia.y,ia.z,n.x,n.y,n.z,0,0);
      v1=mb_push_vertex(mb,ib.x,ib.y,ib.z,n.x,n.y,n.z,1,0);
      v2=mb_push_vertex(mb,ob.x,ob.y,ob.z,n.x,n.y,n.z,1,1);
      v3=mb_push_vertex(mb,oa.x,oa.y,oa.z,n.x,n.y,n.z,0,1);
      mb_push_triangle(mb,v0,v1,v2); mb_push_triangle(mb,v0,v2,v3); }
    /* top wall y=yt (faces -y, down into hole) */
    { vec3 n = vec3_make(0.0f,-1.0f,0.0f); vec3 ia=bent_pt(rax,ge,s0,yt), ib=bent_pt(rax,ge,s1,yt);
      vec3 oa=vec3_add(ia,off), ob=vec3_add(ib,off); sol_u32 v0,v1,v2,v3;
      v0=mb_push_vertex(mb,ia.x,ia.y,ia.z,n.x,n.y,n.z,0,0);
      v1=mb_push_vertex(mb,ib.x,ib.y,ib.z,n.x,n.y,n.z,1,0);
      v2=mb_push_vertex(mb,ob.x,ob.y,ob.z,n.x,n.y,n.z,1,1);
      v3=mb_push_vertex(mb,oa.x,oa.y,oa.z,n.x,n.y,n.z,0,1);
      mb_push_triangle(mb,v0,v1,v2); mb_push_triangle(mb,v0,v2,v3); }
    if (has_bottom) {   /* bottom wall y=yb (faces +y) */
      vec3 n = vec3_make(0.0f,1.0f,0.0f); vec3 ia=bent_pt(rax,ge,s1,yb), ib=bent_pt(rax,ge,s0,yb);
      vec3 oa=vec3_add(ia,off), ob=vec3_add(ib,off); sol_u32 v0,v1,v2,v3;
      v0=mb_push_vertex(mb,ia.x,ia.y,ia.z,n.x,n.y,n.z,0,0);
      v1=mb_push_vertex(mb,ib.x,ib.y,ib.z,n.x,n.y,n.z,1,0);
      v2=mb_push_vertex(mb,ob.x,ob.y,ob.z,n.x,n.y,n.z,1,1);
      v3=mb_push_vertex(mb,oa.x,oa.y,oa.z,n.x,n.y,n.z,0,1);
      mb_push_triangle(mb,v0,v1,v2); mb_push_triangle(mb,v0,v2,v3); }
}
```
**Lighting note:** the engine lights from the SUPPLIED vertex normal (no `gl_FrontFacing`, culling off both backends — confirmed in Phase 2), so the supplied `n` is what matters; the inner face passes `nin`, the outer `nout`, the reveal walls their into-hole normals. Winding is cosmetic here but kept consistent.

- [ ] **Step 2: Dispatch in the `gi` loop (`room_frame_build`, main.c ~4706-4731)**

Replace the two existing `gable_tri(&mb, eL, eR, ap, nin, ...)` and `gable_tri(&mb, eRo, eLo, apo, nout, ...)` calls (keep everything before — `ge/gout/off/nin/nout/eL/eR/ap/eLo/eRo/apo` — and the rake-cap block after) with:
```c
            {
                int   gwall = rax ? (gi ? ROOM_WALL_E : ROOM_WALL_W)
                                  : (gi ? ROOM_WALL_S : ROOM_WALL_N);
                int   oi, found = -1;
                float s0 = 0.0f, s1 = 0.0f, yb = 0.0f, yt = 0.0f;
                for (oi = 0; oi < no; oi++)
                    if (ops[oi].wall == gwall && ops[oi].height > h + 1e-3f) { found = oi; break; }
                if (found >= 0) {
                    float cen = ops[found].center, hwid = ops[found].width * 0.5f;
                    float hwt;
                    s0 = cen - hwid; s1 = cen + hwid;
                    yb = ops[found].sill;   if (yb < h)       yb = h;
                    yt = ops[found].height; if (yt > ridge_y) yt = ridge_y;
                    hwt = sh * (ridge_y - yt) / (ridge_y - h);   /* fit width to the triangle at yt */
                    if (s0 < -hwt) s0 = -hwt;
                    if (s1 >  hwt) s1 =  hwt;
                    if (s1 - s0 < 0.05f || yt - yb < 0.05f) found = -1;   /* degenerate -> solid */
                }
                if (found >= 0) {
                    gable_face_notched(&mb, rax, ge, vec3_make(0.0f,0.0f,0.0f), nin,
                                       sh, h, ridge_y, s0, s1, yb, yt);
                    gable_face_notched(&mb, rax, ge, off, nout,
                                       sh, h, ridge_y, s0, s1, yb, yt);
                    gable_notch_reveal(&mb, rax, ge, off, s0, s1, yb, yt, yb > h + 1e-3f);
                } else {
                    gable_tri(&mb, eL, eR, ap, nin,            /* the existing solid pair */
                              -sh / WALL_TILE_M, h / WALL_TILE_M,
                               sh / WALL_TILE_M, h / WALL_TILE_M,
                              0.0f,              ridge_y / WALL_TILE_M);
                    gable_tri(&mb, eRo, eLo, apo, nout,
                               sh / WALL_TILE_M, h / WALL_TILE_M,
                              -sh / WALL_TILE_M, h / WALL_TILE_M,
                              0.0f,              ridge_y / WALL_TILE_M);
                }
            }
```
(Copy the exact UV args of the existing `gable_tri` calls into the `else` branch — they must match what's there now. `ROOM_WALL_*`, `rax`, `gi`, `ge`, `off`, `nin`, `nout`, `sh`, `h`, `ridge_y`, `ops`, `no` are all in scope.)

- [ ] **Step 3: Gauntlet** — all three pass. (No window in a gable ⇒ the `else` branch = the current solid gable, unchanged.)

- [ ] **Step 4: Commit**
```bash
git add main.c
git commit -m "Gable windows: cut a rectangular notch in the gable (triangle-minus-rectangle)"
```

---

## Task 3: Verify the window reaches the gable; re-cut on change

**Files:** `main.c` — read-only verification + a conditional clamp tweak.

- [ ] **Step 1: Verify the move + resize clamps reach the gable**

Read the window **drag** path (`move_board` → `wall_clamp_run_cy`) and the window **resize** path (uses `wall_gable_geom` → `topcap = is_gable ? apex_y : wall_top`). Confirm BOTH let a window's vertical extent reach above the wall top toward the apex on a ridge-end wall, and narrow the along-wall span inside the triangle. (From Phase 1 these are already gable-aware.)
- If BOTH already reach the apex: no code change — the gable re-cuts because the window-change paths already call `room_rebuild_one` (which calls `room_frame_build`). Note this in the report.
- If a clamp caps at the wall top: raise its `topcap` to the gable apex via `wall_gable_geom(r, ih, normal_x).apex_y` (mirror whichever path already does it). Show the exact diff.

- [ ] **Step 2: Confirm the gable re-cuts on window change**

Verify `room_rebuild_one` calls `room_frame_build` (it does — the per-room rebuild builds the frame), so dragging/resizing a window into the gable and releasing re-cuts the gable notch. No change expected; confirm by reading `room_rebuild_one`.

- [ ] **Step 3: Gauntlet** — all three pass (whether or not a clamp tweak was needed).

- [ ] **Step 4: Commit** (only if a clamp tweak was made; otherwise note "verification only, no change")
```bash
git add main.c
git commit -m "Gable windows: raise the drag/resize clamp to the gable apex on ridge-end walls"
```

---

## Final verification (controller, after all tasks)

- [ ] **Full gauntlet:** `./build.sh c89check && ./build.sh && ./build.sh metal` — all pass.
- [ ] **routetest:** `./build.sh routetest && ./route_test` — the above-wall test passes.
- [ ] **Final holistic review** over the whole diff (the tessellation winding/normals, the gable→wall mapping, the seam, no gaps), then hand to the human.

## Human live-verify checklist (post-merge, both backends)

- On a ridge-end (gable) wall, place a window and drag/corner-resize it UP so it crosses the wall top → the gable opens a matching notch; the window reads continuous across the seam; no see-through gap, no solid gable behind the upper part.
- Drag a small window fully into the gable peak → a clean notch in the triangle (gable-only); the wall below stays solid.
- Cycle it to circular/arched → a rose window in the gable (the oak fill + glass shape it; the notch is the rectangle behind).
- Reload (`L`) → persists + re-cuts. Delete → the gable closes back to a solid triangle.
- An eave (long) wall window is unaffected.

## Notes / known limitations (Phase 3)

- One window per gable end (the builder cuts the first matching opening; more is a future add).
- The wall↔gable seam at `h` is a butt joint (a hairline could show) — tunable later.
- The gable notch reveal is solid gable material (no dark_wood casing).
- Gable windows only appear when the room is textured (the gable mesh builds only when `g_roof_mat` + `g_wall_mat` are loaded — the normal palace).
- No new shader ⇒ no MSL twin.

---

## Self-review notes (author)

- **Spec coverage:** §1 wall-shell fix → Task 1 (the skip); §2 gable notch → Task 2 (`gable_face_notched` + `gable_notch_reveal` + the `gi` dispatch); §3 placement/drag/resize → Task 3 (verify + conditional tweak); §4 window meshes unchanged → nothing touches them; the per-room re-cut → Task 3 Step 2 (existing `room_rebuild_one`). All covered.
- **Symbol consistency:** `g_push`, `gable_face_notched`, `gable_notch_reveal`, `halfw = sh*(ridge_y-y)/(ridge_y-h)`, the `(s,y)` plane, `gwall` mapping, `bent_pt(rax,ge,s,y)`, the UV scale `/WALL_TILE_M` all used consistently. `ROOM_WALL_N/E/S/W` = 0/1/2/3. Existing symbols (`bent_pt`, `gable_tri`, `room_frame_build`, `room_rebuild_one`, `ROUTE_WALL_T`, `ridge_y`, `sh`, `off`, `nin`, `nout`) verified in source.
- **Lighting law:** the supplied normal drives lighting (Phase 2 finding) — inner=nin, outer=nout, reveals into-hole; winding cosmetic but consistent.
