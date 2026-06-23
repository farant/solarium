# Note Cards Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Note cards lose the "note" label, gain per-note `+`/`-` text sizing, and become corner-drag resizable with auto-growing height.

**Architecture:** All in `main.c`. A new `note_autosize()` is the single authority for a note's height (line-count → height, top-anchored, registry-shared-mesh rebuild). The render reads a per-note `meta["text_size"]`; `+`/`-` in `read_input` adjust it; the whiteboard corner-resize machinery is generalized to free-standing notes via a `KIND_NOTE` branch in the resize-drag block. No new shader → no MSL twin.

**Tech Stack:** C89, GLFW key polling, the RHI/asset/scene/wtext layers. Build gauntlet: `./build.sh c89check && ./build.sh debug && ./build.sh metal`.

---

## Context the implementer needs

- **GUI feature, human-verified.** No unit-test surface beyond `board_resize_corner` (already tested, reused unchanged). Each task's gate is **the build gauntlet passes**; the full manual verification runs at the end of Task 4. Do **not** fabricate `*_test` targets.
- **Strict C89:** declarations at the top of each block, `/* */` comments only, `snprintf` not `sprintf`. The build runs `c89check` first — keep it green.
- **Card facts:** a card is `mesh_ref "card"` (params `w`/`h`/`t`, defaults `0.35/0.5/0.03`), bottom-origin (local `(0,0,0)` = bottom-centre, `(0,h,0)` = top-centre), front face +Z. Card render loop starts at `main.c:10793`; `lh = font_line_height(uf)` where `uf = state->ui_font` (`main.c:10669-10670`).
- **Registry-shared mesh rebuild (★):** the `"card"` mesh is keyed by its params, and `scene_resolve_meshes` only builds objects whose `mesh.index_count == 0`. To resize live: `mesh_asset_key(o, key)` (from the OLD params) → `scene_mesh_params_set(...)` → `asset_release(&g_mesh_assets, key)` → `memset(&o->mesh, 0, sizeof o->mesh)` → `scene_resolve_meshes`. Never `mesh_destroy` a shared shape.
- **`text_wrap(font, utf8, px2m, max_width, out, cap)`** (`text.c:126`) returns the line count and writes the wrapped string. `wtext_block(..., wrap_w, ...)` re-wraps every frame, so changing `w` reflows for free.
- **Editing mirrors keystrokes into `meta["text"]`** in `on_char` (`main.c:11629`) and the `on_key` backspace/enter branches (`main.c:11695`,`:11699`) — hooking `note_autosize` there makes the card grow live.
- **`read_input` early-returns** at `main.c:7398` when `edit_handle != 0 || palette.open`, so any key block added there is automatically silent while typing or in the palette.
- **Existing resize:** press gate `main.c:7544` (`board_is_mounted && resize_corner_pick`), drag block `main.c:7625` (wall-anchored, keyed off `resize_room`), handle render `main.c:10964`. `resize_corner_pick` (`main.c:6842`) and `board_world_corners` (`main.c:6828`) already read the resized object's own `w`/`h`/yaw, so they work for a `"card"` unchanged.
- **`board_resize_corner`** (`descend.h:48`): `void board_resize_corner(vec3 anchor, vec3 dragged, vec3 u, float min_size, float aspect, float *out_w, float *out_h, vec3 *out_origin);` — `aspect 0` = free.

---

### Task 1: Remove the "note" label + read per-note text size in the render

**Files:**
- Modify: `main.c` — add helper `note_text_size` after `room_interior_height` (`main.c:6899`); card render block (`main.c:10852`–`10893`).

- [ ] **Step 1: Add the `note_text_size` helper**

Insert immediately after `room_interior_height`'s closing `}` (currently `main.c:6898`):

```c
/* a note's body text size in metres-per-line; absent meta => the default,
   clamped to the editable range. */
static float note_text_size(Scene *s, sol_u32 h) {
    const char *v = scene_meta_get(s, h, "text_size");
    float ts = v ? (float)atof(v) : 0.028f;
    if (ts < 0.015f) ts = 0.015f;
    if (ts > 0.060f) ts = 0.060f;
    return ts;
}
```

- [ ] **Step 2: Skip the name label for notes**

In the render loop, wrap the front+back label block in a non-note guard. Replace:

```c
            /* the label: the card's name across the top, shrunk to fit */
            nm = object_label(&state->scene, o->handle, lbuf);
            px2m = 0.038f / lh;                             /* ~3.8cm line */
            text_measure(uf, nm, 1.0f, &name_w, (float *)0);
            if (name_w * px2m > usable && name_w > 0.0f)
                px2m = usable / name_w;                     /* shrink, don't clip */
            wtext_block(uf, vp, face, nm,
                        -cw * 0.5f + margin, ch - margin, px2m, 0.0f,
                        ink_r, ink_g, ink_b);
            {   /* the same label on the BACK face, so the tablet names itself
                   from both sides (rotate 180 about Y so it reads, not mirrors;
                   the engine never culls back faces, so the back is visible) */
                mat4 back = mat4_mul(
                    mat4_mul(scene_world_matrix(&state->scene, o),
                             quat_to_mat4(quat_from_axis_angle(
                                 vec3_make(0.0f, 1.0f, 0.0f), sol_radians(180.0f)))),
                    mat4_translate(vec3_make(0.0f, 0.0f, ct * 0.5f + 0.0008f)));
                wtext_block(uf, vp, back, nm,
                            -cw * 0.5f + margin, ch - margin, px2m, 0.0f,
                            ink_r, ink_g, ink_b);
            }
```

with the same code wrapped in `if (o->kind != KIND_NOTE) { ... }`:

```c
            /* the label: the card's name across the top, shrunk to fit. Notes
               carry their content in the body, not a name — skip their label. */
            if (o->kind != KIND_NOTE) {
                nm = object_label(&state->scene, o->handle, lbuf);
                px2m = 0.038f / lh;                             /* ~3.8cm line */
                text_measure(uf, nm, 1.0f, &name_w, (float *)0);
                if (name_w * px2m > usable && name_w > 0.0f)
                    px2m = usable / name_w;                     /* shrink, don't clip */
                wtext_block(uf, vp, face, nm,
                            -cw * 0.5f + margin, ch - margin, px2m, 0.0f,
                            ink_r, ink_g, ink_b);
                {   /* the same label on the BACK face, so the tablet names itself
                       from both sides (rotate 180 about Y so it reads, not mirrors;
                       the engine never culls back faces, so the back is visible) */
                    mat4 back = mat4_mul(
                        mat4_mul(scene_world_matrix(&state->scene, o),
                                 quat_to_mat4(quat_from_axis_angle(
                                     vec3_make(0.0f, 1.0f, 0.0f), sol_radians(180.0f)))),
                        mat4_translate(vec3_make(0.0f, 0.0f, ct * 0.5f + 0.0008f)));
                    wtext_block(uf, vp, back, nm,
                                -cw * 0.5f + margin, ch - margin, px2m, 0.0f,
                                ink_r, ink_g, ink_b);
                }
            }
```

- [ ] **Step 3: Body text starts at the top, at the per-note size**

In the note body block, replace:

```c
                if (txt && txt[0]) {
                    float bpx2m = 0.028f / lh;              /* ~2.8cm body lines */
                    wtext_block(uf, vp, face, txt,
                                -cw * 0.5f + margin, ch - margin - 0.055f,
                                bpx2m, usable, ink_r, ink_g, ink_b);
                }
```

with (label is gone, so start at `ch - margin`; size from meta):

```c
                if (txt && txt[0]) {
                    float bpx2m = note_text_size(&state->scene, o->handle) / lh;
                    wtext_block(uf, vp, face, txt,
                                -cw * 0.5f + margin, ch - margin,
                                bpx2m, usable, ink_r, ink_g, ink_b);
                }
```

- [ ] **Step 4: Build gauntlet**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: all three PASS. (Visual: a note now shows no "note" label and its body sits at the top; size still looks like before because the stored default equals the old `0.028`.)

- [ ] **Step 5: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
note-cards: drop the "note" label, read per-note text size in render

KIND_NOTE cards no longer draw a name label (front or back); the body starts
at the top. Body size now comes from meta["text_size"] via note_text_size()
(default 0.028 = the old fixed size, so today's notes render unchanged).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: `note_autosize()` — the height authority + live-grow hooks

**Files:**
- Modify: `main.c` — add `note_autosize` after `note_text_size`; hook it in `on_char` (`main.c:11629`) and the `on_key` backspace/enter branches (`main.c:11695`,`:11699`).

- [ ] **Step 1: Add `note_autosize`**

Insert immediately after the `note_text_size` helper from Task 1:

```c
/* THE authority for a note's height + vertical position: fit the card to its
   wrapped text at the current width and size, top-anchored (the top edge stays
   put, the card grows downward), but never shorter than meta["min_h"]. Rebuilds
   the registry-shared "card" mesh only when the height actually changes; width
   and horizontal position are left untouched. */
static void note_autosize(AppState *st, sol_u32 h) {
    SceneObject *o = scene_get(&st->scene, h);
    const char  *txt, *mv;
    char         wbuf[4096];
    float        cw, h0, ct, ts, lh, px2m, usable, content_h, min_h, new_h;
    int          lines;
    if (!o || o->kind != KIND_NOTE || !st->ui_font) return;
    if (!o->mesh_ref || strcmp(o->mesh_ref, "card") != 0) return;
    cw = mesh_ref_param("card", o->mesh_params, o->mesh_param_count, "w");
    h0 = mesh_ref_param("card", o->mesh_params, o->mesh_param_count, "h");
    ct = mesh_ref_param("card", o->mesh_params, o->mesh_param_count, "t");
    ts = note_text_size(&st->scene, h);
    lh = font_line_height(st->ui_font);
    px2m = (lh > 0.0f) ? ts / lh : ts;
    usable = cw - 2.0f * 0.025f;
    txt = scene_meta_get(&st->scene, h, "text");
    lines = (txt && txt[0] && usable > 0.0f)
          ? text_wrap(st->ui_font, txt, px2m, usable, wbuf, (int)sizeof wbuf)
          : 1;
    if (lines < 1) lines = 1;
    content_h = (float)lines * ts + 2.0f * 0.025f;          /* top+bottom margins */
    mv = scene_meta_get(&st->scene, h, "min_h");
    min_h = mv ? (float)atof(mv) : 0.5f;                    /* default card height */
    new_h = (content_h > min_h) ? content_h : min_h;
    if (new_h < 0.05f) new_h = 0.05f;
    if (new_h > h0 - 0.001f && new_h < h0 + 0.001f) return; /* unchanged: no rebuild */
    {   /* top-anchored rebuild: capture the world top-centre, then keep it fixed */
        mat4 M       = scene_world_matrix(&st->scene, o);
        vec3 top_w   = mat4_mul_point(M, vec3_make(0.0f, h0, 0.0f));
        char oldkey[160];
        sol_bool keyed = mesh_asset_key(o, oldkey);
        float p3[3];
        p3[0] = cw; p3[1] = new_h; p3[2] = ct;
        scene_mesh_params_set(&st->scene, h, p3, 3);
        if (keyed) asset_release(&g_mesh_assets, oldkey);
        o = scene_get(&st->scene, h);
        if (o) {
            vec3 nb = vec3_make(top_w.x, top_w.y - new_h, top_w.z);
            memset(&o->mesh, 0, sizeof o->mesh);
            o->pos = scene_world_to_local(&st->scene, o->parent, nb);
        }
        scene_resolve_meshes(&st->scene);
    }
}
```

- [ ] **Step 2: Grow live while typing — `on_char`**

In `on_char`, after the line that mirrors the buffer into the meta (`main.c:11629`):

```c
    scene_meta_set(&st->scene, st->edit_handle, "text", st->edit_buf);
```

add:

```c
    note_autosize(st, st->edit_handle);
```

- [ ] **Step 3: Grow on backspace and newline — `on_key`**

In the `on_key` editing branch, after **each** of the two `scene_meta_set(... "text" ...)` calls (the backspace branch at `main.c:11695` and the enter branch at `main.c:11699`), add `note_autosize(st, st->edit_handle);`. Result:

```c
        } else if (key == GLFW_KEY_BACKSPACE && st->edit_len > 0) {
            st->edit_len--;
            while (st->edit_len > 0 &&
                   ((unsigned char)st->edit_buf[st->edit_len] & 0xC0u) == 0x80u)
                st->edit_len--;
            st->edit_buf[st->edit_len] = '\0';
            scene_meta_set(&st->scene, st->edit_handle, "text", st->edit_buf);
            note_autosize(st, st->edit_handle);
        } else if (key == GLFW_KEY_ENTER && st->edit_len + 1 < EDIT_BUF_CAP) {
            st->edit_buf[st->edit_len++] = '\n';
            st->edit_buf[st->edit_len]   = '\0';
            scene_meta_set(&st->scene, st->edit_handle, "text", st->edit_buf);
            note_autosize(st, st->edit_handle);
        }
```

- [ ] **Step 4: Build gauntlet**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: all three PASS. (`note_autosize` is now referenced by `on_char`/`on_key`, so no `-Wunused-function`.)

- [ ] **Step 5: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
note-cards: note_autosize() height authority; grow live while typing

note_autosize fits a note's card to its wrapped text (top-anchored, >= min_h),
rebuilding the registry-shared card mesh only when the height changes. Hooked
into on_char and the on_key backspace/enter branches so a note grows as you type.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: `+`/`-` adjust the selected note's text size

**Files:**
- Modify: `main.c` — add `sol_bool textsize_was_down;` to `AppState` near `bs_was_down` (`main.c:2548`); add a key block in `read_input` after the Backspace block (`main.c:8406`).

- [ ] **Step 1: Add the edge-tracker field**

After the `AppState` field `sol_bool bs_was_down;` (`main.c:2548`):

```c
    sol_bool    textsize_was_down; /* edge-detect +/- note text sizing */
```

- [ ] **Step 2: Add the `+`/`-` handling**

In `read_input`, immediately after the Backspace block's closing `}` (the block that ends with `st->bs_was_down = bs_now;`, `main.c:8406`), insert:

```c
    /* +/- resize the SELECTED note's body text. read_input has already returned
       above if a note is being edited or the palette is open, so these keys are
       free here. =/+ grows, -/_ shrinks; numpad +/- too. */
    {
        sol_bool plus_now  = (sol_bool)(glfwGetKey(w, GLFW_KEY_EQUAL) == GLFW_PRESS ||
                                        glfwGetKey(w, GLFW_KEY_KP_ADD) == GLFW_PRESS);
        sol_bool minus_now = (sol_bool)(glfwGetKey(w, GLFW_KEY_MINUS) == GLFW_PRESS ||
                                        glfwGetKey(w, GLFW_KEY_KP_SUBTRACT) == GLFW_PRESS);
        sol_bool ts_now    = (sol_bool)(plus_now || minus_now);
        if (ts_now && !st->textsize_was_down && st->selected_handle != 0) {
            SceneObject *o = scene_get(&st->scene, st->selected_handle);
            if (o && o->kind == KIND_NOTE) {
                float ts = note_text_size(&st->scene, st->selected_handle);
                char  tb[32];
                ts += plus_now ? 0.004f : -0.004f;
                if (ts < 0.015f) ts = 0.015f;
                if (ts > 0.060f) ts = 0.060f;
                snprintf(tb, sizeof tb, "%.4f", (double)ts);
                scene_meta_set(&st->scene, st->selected_handle, "text_size", tb);
                note_autosize(st, st->selected_handle);
                scene_save(&st->scene, "scene.stml");
            }
        }
        st->textsize_was_down = ts_now;
    }
```

- [ ] **Step 3: Build gauntlet**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: all three PASS.

- [ ] **Step 4: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
note-cards: +/- adjust the selected note's text size

While a note is selected (and not being edited), =/+ and -/_ (and numpad +/-)
step meta["text_size"] by 0.004 within [0.015, 0.060], re-fit via note_autosize,
and save. Guarded for free by read_input's edit/palette early-return.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Corner-drag resize for free-standing notes

**Files:**
- Modify: `main.c` — add `note_resizable` after `note_autosize`; widen the press gate (`main.c:7544`) and handle-render gate (`main.c:10964`); add a `KIND_NOTE` branch to the resize-drag block (`main.c:7625`).

- [ ] **Step 1: Add the `note_resizable` predicate**

Insert immediately after `note_autosize` (Task 2):

```c
/* a note card can be corner-resized (free-standing or pinned, unlike the
   wall-only board path). */
static sol_bool note_resizable(Scene *s, sol_u32 h) {
    SceneObject *o = scene_get(s, h);
    return (sol_bool)(o && o->kind == KIND_NOTE &&
                      o->mesh_ref && strcmp(o->mesh_ref, "card") == 0);
}
```

- [ ] **Step 2: Widen the press gate to notes**

In the press handler, replace:

```c
                } else if (st->selected_handle != 0 &&
                           board_is_mounted(&st->scene, st->selected_handle) &&
                           resize_corner_pick(st, w)) {
```

with:

```c
                } else if (st->selected_handle != 0 &&
                           (board_is_mounted(&st->scene, st->selected_handle) ||
                            note_resizable(&st->scene, st->selected_handle)) &&
                           resize_corner_pick(st, w)) {
```

- [ ] **Step 3: Widen the handle-render gate to notes**

In the resize-handle render, replace:

```c
        if (state->selected_handle != 0 &&
            board_is_mounted(&state->scene, state->selected_handle)) {
```

with:

```c
        if (state->selected_handle != 0 &&
            (board_is_mounted(&state->scene, state->selected_handle) ||
             note_resizable(&state->scene, state->selected_handle))) {
```

- [ ] **Step 4: Add the note branch to the resize-drag block**

The resize-drag block currently opens (`main.c:7625`):

```c
        if (lmb && st->resize_board != 0) {             /* ---- resizing ---- */
            SceneObject *o = scene_get(&st->scene, st->resize_board);
            if (!o || st->resize_room == 0 ||
                scene_get(&st->scene, st->resize_room) == 0) {
                st->resize_board = 0;                   /* board/room vanished */
            } else {
```

Replace those lines with a kind branch (a free note has no room, so the note path must come **before** the wall guard):

```c
        if (lmb && st->resize_board != 0) {             /* ---- resizing ---- */
            SceneObject *o = scene_get(&st->scene, st->resize_board);
            if (!o) {
                st->resize_board = 0;                   /* object vanished */
            } else if (o->kind == KIND_NOTE) {
                /* free-standing note: drag the card's OWN front-face plane (no
                   wall). Horizontal drag sets the width (the wrap boundary);
                   vertical drag sets a MIN height; note_autosize then enforces
                   height >= wrapped content and keeps the top edge fixed. */
                Ray   ray = pick_ray(st, w);
                float yaw = board_yaw(&st->scene, st->resize_board);
                vec3  n   = vec3_make((float)sin((double)yaw), 0.0f,
                                      (float)cos((double)yaw));
                float tt;
                ray.dir = vec3_normalize(ray.dir);
                if (ray_vs_plane(ray, st->resize_anchor, n, &tt) && tt > 0.0f) {
                    vec3  hit = vec3_add(ray.origin, vec3_scale(ray.dir, tt));
                    vec3  origin, cur_bottom, nb;
                    float nw, nh, p3[3];
                    float ct  = mesh_ref_param("card", o->mesh_params,
                                               o->mesh_param_count, "t");
                    char  oldkey[160], mhbuf[32];
                    sol_bool keyed;
                    board_resize_corner(st->resize_anchor, hit, st->resize_u,
                                        0.15f, 0.0f, &nw, &nh, &origin);
                    snprintf(mhbuf, sizeof mhbuf, "%.4f", (double)nh);
                    scene_meta_set(&st->scene, st->resize_board, "min_h", mhbuf);
                    /* width: release/rebuild the shared card mesh, keep height +
                       vertical; set horizontal centre from the drag's origin */
                    cur_bottom = object_world_pos(&st->scene, st->resize_board);
                    keyed = mesh_asset_key(o, oldkey);
                    p3[0] = nw;
                    p3[1] = mesh_ref_param("card", o->mesh_params,
                                           o->mesh_param_count, "h");
                    p3[2] = ct;
                    scene_mesh_params_set(&st->scene, st->resize_board, p3, 3);
                    if (keyed) asset_release(&g_mesh_assets, oldkey);
                    o = scene_get(&st->scene, st->resize_board);
                    if (o) {
                        memset(&o->mesh, 0, sizeof o->mesh);
                        nb = vec3_make(origin.x, cur_bottom.y, origin.z);
                        o->pos = scene_world_to_local(&st->scene, o->parent, nb);
                    }
                    scene_resolve_meshes(&st->scene);
                    note_autosize(st, st->resize_board);  /* height + top-anchor */
                }
            } else if (st->resize_room == 0 ||
                       scene_get(&st->scene, st->resize_room) == 0) {
                st->resize_board = 0;                   /* board/room vanished */
            } else {
```

The existing wall-resize body and its closing braces stay exactly as they are — this only changes the opening `if`/`else if` ladder so the wall path becomes the final `else`.

- [ ] **Step 5: Confirm the resize-release saves**

The resize release path that runs on mouse-up (`main.c:7754`, `if (st->resize_board != 0) { ...; st->resize_board = 0; }`) calls `scene_save`. Read it and confirm a note resize is saved on release (it acts on `resize_board` regardless of kind). No code change expected; if it does not save, add `scene_save(&st->scene, "scene.stml");` there.

- [ ] **Step 6: Build gauntlet**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: all three PASS.

- [ ] **Step 7: Human live-verify (hand to Fran)**

Subagents can't GUI-test. Hand Fran these steps; wait for confirmation before the final commit:
1. `N` spawns a note; `Enter`, type a paragraph → the card grows downward as text wraps; the top edge stays put.
2. Select the note (tap), not editing → press `+`/`-` → body text grows/shrinks, the card re-fits; reload → size persists.
3. Selected note shows four corner handles → aim a corner, hold, look to drag **horizontally** → width changes and text reflows; drag **down** → extra blank height; release → saved; reload → size persists.
4. No "note" label on the front or back of any note.
5. Regression: file/alias/tombstone cards still show name labels and are not resizable; a wall-mounted whiteboard/picture still corner-resizes; typing in a note does not trigger `+`/`-` sizing or the resize handles.

- [ ] **Step 8: Commit**

Only after Fran confirms.

```bash
git add main.c
git commit -m "$(cat <<'EOF'
note-cards: corner-drag resize for free-standing notes

Generalizes the whiteboard corner-resize to notes: note_resizable() widens the
press + handle-render gates, and a KIND_NOTE branch in the resize-drag block
drags the card's own front-face plane (no wall) — horizontal sets width (the
wrap boundary), vertical sets a min height, and note_autosize fits the height
top-anchored. meta["min_h"] persists.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-review

- **Spec coverage:**
  - Feature 1 (remove "note" label) → Task 1 Steps 2–3.
  - Feature 2 (per-note text size, render) → Task 1 Steps 1, 3; (input `+`/`-`) → Task 3.
  - Feature 3 (`note_autosize`, auto-grow, top-anchor, live typing) → Task 2; (resize handles + drag) → Task 4; (drag = width, height auto) → Task 4 Step 4 + `note_autosize`.
  - Constraints (C89, gauntlet both backends, no shader, no NOTES.stml/paper-picture.png, Co-Authored-By) → every task's gauntlet + commit steps.
- **Placeholder scan:** none — every code step shows complete code; the one "read & confirm" step (Task 4 Step 5) has an explicit fallback.
- **Type/name consistency:** `note_text_size(Scene*, sol_u32)→float`, `note_autosize(AppState*, sol_u32)→void`, `note_resizable(Scene*, sol_u32)→sol_bool`, `meta["text_size"]`/`meta["min_h"]`, field `textsize_was_down` — all used consistently across tasks. `board_resize_corner(anchor,dragged,u,min,aspect,&w,&h,&origin)` matches `descend.h:48`. The registry-rebuild sequence matches the established board pattern (`mesh_asset_key`→`scene_mesh_params_set`→`asset_release`→`memset`→`scene_resolve_meshes`).
- **Ordering:** helpers are introduced with their first callers (`note_text_size`→Task 1 render, `note_autosize`→Task 2 hooks, `note_resizable`→Task 4 gates), so no `-Wunused-function` at any task boundary.
