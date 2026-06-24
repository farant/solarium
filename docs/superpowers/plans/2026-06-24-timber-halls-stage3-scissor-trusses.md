# Timber Halls — Stage 3: Scissor Trusses Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Add `dark_wood` scissor trusses — bents spaced along each room's ridge axis (rafters + crossing scissor chords + king post) — and suppress the flat ceiling so they're visible.

**Architecture:** Extend `room_frame_build`'s existing wood-mesh build (which already does the corner columns) with scissor-truss bents, reusing the `frame_beam` primitive. A `bent_pt` helper maps a bent's (along-ridge, across-span, height) coordinates to world for either ridge orientation. The flat ceiling is suppressed when the timber frame is active (moved up from Stage 4 — without it the trusses are hidden above the ceiling). No new shader; the wood mesh is already drawn.

**Tech Stack:** C89; the existing `frame_beam` + MeshBuilder + `sol_radians`/`tan`.

**Branch:** `timber-halls-stage3` (create at start; ff-merge to `main` at the end).

**C89 reminders:** decls at top of block; `/* */`; `(float)tan((double)x)` (no `tanf`); c89check is `-Wall -Wextra -Werror`. **Never commit** `NOTES.stml`/`paper-picture.png`. Commit trailer: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

**No unit test:** render-only — gauntlet + human live-verify.

**The frame plan (the geometry):** ridge runs along the LONGER of `w`/`d` (`rax = w >= d`); span = the shorter; half-span `sh`; `dy = sh·tan(35°)` (eave→ridge rise); `ridge_y = h + dy` (peak); `bents = round(ridge_len / 2.5)` (≥2), spaced evenly along the ridge axis. A bent (cross-section): two **rafters** eave→apex; two **scissor lower chords** eave→a point `f=0.6` up the OPPOSITE rafter (they cross at the centreline at `cross_y = h + dy·f/(2−f)`); a **king post** crossing→apex. Room is origin-at-floor-center; `hw=w/2, hd=d/2`; eaves sit at the wall-tops (y=h, span=±sh).

---

### Task 0: Branch

- [ ] **Step 1:** `git checkout -b timber-halls-stage3`

---

### Task 1: Scissor trusses + ceiling suppression

**Files:** Modify `main.c`.

- [ ] **Step 1: Add the truss constants**

After the `#define FRAME_COL_T ... / WOOD_TILE_M ...` lines (grep `#define FRAME_COL_T`, ~main.c:4181), add:
```c
#define FRAME_BEAM_T       0.14f     /* truss beam cross-section (m) */
#define FRAME_PITCH_DEG    35.0f     /* roof / truss pitch (degrees) */
#define FRAME_BENT_SPACING 2.5f      /* meters between scissor-truss bents */
#define FRAME_SCISSOR_FRAC 0.6f      /* lower chord meets the opposite rafter this far up */
```

- [ ] **Step 2: Add the `bent_pt` helper**

Immediately BEFORE `static void frame_beam(` (grep it), add:
```c
/* map a bent's (along-ridge, across-span, height) to a world point. ridge_along_x:
   1 = ridge runs X so span is Z; 0 = ridge runs Z so span is X. */
static vec3 bent_pt(int ridge_along_x, float along, float span, float y) {
    return ridge_along_x ? vec3_make(along, y, span)
                         : vec3_make(span, y, along);
}
```
(It must precede the truss build in Step 3, which calls it; it's also used there in the same task, so no unused-function.)

- [ ] **Step 3: Build the trusses in the wood block**

In `room_frame_build`, the `if (g_dark_wood.albedo_tex.id != 0) { ... }` block builds four corner columns into `mb`, then `if (mb.index_count > 0) wood = mesh_from_builder(&mb);`. Insert the truss build AFTER the four column `frame_beam(...)` lines and BEFORE the `if (mb.index_count > 0) wood = ...` line:
```c
        /* scissor trusses: bents spaced along the ridge axis (the longer dim) */
        {
            int   rax     = (w >= d) ? 1 : 0;      /* 1 = ridge along X (span = Z) */
            float rlen    = rax ? w : d;           /* ridge length */
            float along_h = rax ? hw : hd;         /* half the ridge length */
            float sh      = rax ? hd : hw;         /* half-span (eave -> centre) */
            float dy      = sh * (float)tan((double)sol_radians(FRAME_PITCH_DEG));
            float ridge_y = h + dy;                /* ridge peak above the floor */
            float f       = FRAME_SCISSOR_FRAC;
            float cross_y = h + dy * f / (2.0f - f);  /* where the lower chords cross */
            int   bents   = (int)(rlen / FRAME_BENT_SPACING + 0.5f);
            int   bi;
            if (bents < 2) bents = 2;
            for (bi = 0; bi < bents; bi++) {
                float al    = -along_h + rlen * ((float)bi + 0.5f) / (float)bents;
                vec3  eaveL = bent_pt(rax, al, -sh,  h);
                vec3  eaveR = bent_pt(rax, al,  sh,  h);
                vec3  apex  = bent_pt(rax, al, 0.0f, ridge_y);
                frame_beam(&mb, eaveL, apex, FRAME_BEAM_T);                                            /* rafter L */
                frame_beam(&mb, eaveR, apex, FRAME_BEAM_T);                                            /* rafter R */
                frame_beam(&mb, eaveL, bent_pt(rax, al,  sh * (1.0f - f), h + dy * f), FRAME_BEAM_T);  /* scissor L */
                frame_beam(&mb, eaveR, bent_pt(rax, al, -sh * (1.0f - f), h + dy * f), FRAME_BEAM_T);  /* scissor R */
                frame_beam(&mb, bent_pt(rax, al, 0.0f, cross_y), apex, FRAME_BEAM_T);                  /* king post */
            }
        }
```

- [ ] **Step 4: Suppress the flat ceiling when the frame is active**

There are TWO `make_room_doored(...)` calls (in `connections_rebuild` ~main.c:4424 and `connections_rebuild_focus` ~4506). Each passes a `ceil` argument:
```c
                             mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "ceil") > 0.5f,
```
Replace BOTH occurrences with (no flat ceiling once the timber frame is on — the trusses, and later the roof, take its place):
```c
                             (g_dark_wood.albedo_tex.id == 0 &&
                              mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "ceil") > 0.5f),
```
(`g_dark_wood` is a file-scope static declared before `connections_rebuild`, so it's in scope. If the `dark_wood` folder is missing, `g_dark_wood.albedo_tex.id == 0` → the flat ceiling stays — graceful.)

- [ ] **Step 5: Build both backends**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal` — all PASS. (`bent_pt`←the truss build; `frame_beam` reused; all called. No shader → Metal risk is compile-only.) If a build fails, fix minimally or report BLOCKED.

- [ ] **Step 6: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
Timber halls stage 3: scissor trusses

dark_wood scissor-truss bents along each room's ridge axis (rafters + crossing
scissor lower chords + king post, via frame_beam + a bent_pt orientation
helper), into the RoomFrame wood mesh. The flat ceiling is suppressed when the
frame is active so the trusses are visible (the roof closes the top in stage 4).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Gauntlet, live-verify, finish

- [ ] **Step 1: Full gauntlet** — `./build.sh c89check && ./build.sh debug && ./build.sh metal` (all PASS).

- [ ] **Step 2: Human live-verify (Fran)** — on `./solarium` and `./solarium-metal`: each room shows scissor trusses overhead — rafters rising from the wall-tops to a ridge, the two lower chords crossing at the centre with a short king post above, spaced down the room's length (the longer dimension). The flat ceiling is gone (you look up into the timber, open to the sky — the roof comes in stage 4). The columns + wall planks + sandstone floor still look right. Check a non-square room (the ridge should run along its longer side) and a resized room (trusses rebuild). Tune `FRAME_PITCH_DEG` / `FRAME_BENT_SPACING` / `FRAME_BEAM_T` / `FRAME_SCISSOR_FRAC` if needed.

- [ ] **Step 3: Finish** — superpowers:finishing-a-development-branch; ff-merge `timber-halls-stage3` to `main`. Do NOT stage `NOTES.stml`/`paper-picture.png`.

---

## Plan self-review

**Spec coverage (Stage 3 + the ceiling re-order):** scissor trusses — bents along the ridge axis, rafters + crossing scissor chords + king post, `dark_wood` via `frame_beam` ✓ (Task 1 Steps 2-3); the frame plan (ridge=longer dim, pitch→ridge_y, bent count/spacing, scissor crossing) ✓; flat-ceiling suppression moved from Stage 4 to here so trusses are visible ✓ (Step 4, a justified deviation noted in the commit); no draw change (wood mesh already drawn) ✓; no shader/MSL ✓; gauntlet + live-verify ✓ (Task 2).

**Placeholder scan:** none — full code + exact anchors.

**Type consistency:** `bent_pt(int, float, float, float) -> vec3`, `frame_beam`, the `FRAME_BEAM_T`/`FRAME_PITCH_DEG`/`FRAME_BENT_SPACING`/`FRAME_SCISSOR_FRAC` constants, and `rax`/`sh`/`dy`/`ridge_y`/`cross_y` are used consistently. The scissor `cross_y = h + dy·f/(2−f)` matches where the two lower chords (eave→`sh·(1−f)` up the opposite rafter) intersect the centreline. `bent_pt` is introduced with its caller (the truss build) so no unused-function. The ceiling suppression keys on the same `g_dark_wood` the wood build does, so trusses-present ⇔ ceiling-gone are consistent.
