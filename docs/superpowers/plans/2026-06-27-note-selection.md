# Text Selection for Notes — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add text selection to board notes — click-drag span, double-click word, triple-click whole note, Shift+arrows to extend, a visible highlight, and selection-aware editing (type-over, delete).

**Architecture:** `edit_sel_anchor` + `edit_cursor` (the moving end) define a span `[min,max)`. Pure `caret.c` gains `caret_word_at` (word/space/other runs) and `caret_sel_spans` (per-line highlight rects), both unit-tested. A shared `click_seq` counter unifies double/triple-click across the board-view and editing input paths. The highlight draws as translucent quads behind the ink; the §1.6 `text_shape`/`wtext_block` seam is untouched.

**Tech Stack:** C89 core (`caret.c`, `main.c`); C11 test (`caret_test.c` via `build.sh carettest`). Spec: `docs/superpowers/specs/2026-06-27-note-selection-design.md`.

**House rules:** strict C89 for `caret.c`/`main.c` (declarations at block top, no `//`, no mid-block decls; `-std=c89 -pedantic-errors -Werror -Wextra`). NEVER `git add NOTES.stml`/`paper-picture.png`. Commit bodies end with `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. Feature branch off `main`.

**Verified anchors:** AppState edit fields at main.c:2850-2852 (`edit_cursor`/`edit_goal_x`/`caret_mesh`). `on_char` 16659; the edit-key block `if (st->edit_handle != 0) {` 16775 (Esc/Backspace/Delete/Enter/Left/Right/Up/Down); `note_edit_begin` 16604 (seeds `edit_cursor`); `caret_click_place` 16557; the note render `KIND_NOTE`/caret block 15660-15697; the board-view multi-click press handler 11004-11046 (`is_dbl` via `last_press_t`/`BOARD_DBL_S`/`BOARD_DBL_PX`); the editing modal gate 10817 (`caret_click_place`/blur). `draw_glass(state, mesh, model, view, proj, eye, mat)` 14292 (alpha via `GLASS_OPACITY`). `on_key(window, key, scancode, action, mods)` has `mods`.

---

## Task 1: Pure `caret_word_at` + `caret_sel_spans` (caret.c) + tests

**Files:** Modify `caret.h`, `caret.c`, `caret_test.c`.

- [ ] **Step 1: Add declarations to `caret.h`** (before `#endif`, after the existing slot-search decls):

```c
/* the maximal run of one class (word [A-Za-z0-9_ or >=0x80] / space / other) around
   byte `off` in `src`; off clamped to [0,len]; at end-of-text takes the run ending there. */
void caret_word_at(const char *src, int off, int *start, int *end);

typedef struct { int line; float x0, x1; } CaretSpan;   /* a selection rect on one visual line */

/* per-visual-line highlight rects for the selection [lo,hi). First line lo.x->line end,
   middle lines 0->line end, last line 0->hi.x (single line: lo.x->hi.x). 0 spans if lo>=hi.
   Returns the span count (<= cap). */
int caret_sel_spans(const CaretField *cf, int lo, int hi, CaretSpan *out, int cap);
```

- [ ] **Step 2: Write failing tests in `caret_test.c`** (add these functions + call them from `main` after `test_field_trailing_space();`):

```c
static void test_word_at(void) {
    int s = 0, e = 0;
    caret_word_at("the quick fox", 5, &s, &e);     /* inside "quick" (4..9) */
    CHECK(s == 4 && e == 9, "word_at: mid word -> quick");
    caret_word_at("the quick fox", 0, &s, &e);
    CHECK(s == 0 && e == 3, "word_at: start -> the");
    caret_word_at("the quick fox", 13, &s, &e);    /* off == len -> last word */
    CHECK(s == 10 && e == 13, "word_at: end -> fox");
    caret_word_at("the quick fox", 3, &s, &e);     /* on the space */
    CHECK(s == 3 && e == 4, "word_at: on a space -> the space run");
    caret_word_at("a..b", 1, &s, &e);              /* on punctuation run ".." */
    CHECK(s == 1 && e == 3, "word_at: punctuation run");
    caret_word_at("\xC3\xA9z", 0, &s, &e);         /* multibyte word "éz" */
    CHECK(s == 0 && e == 3, "word_at: multibyte stays whole");
    caret_word_at("", 0, &s, &e);
    CHECK(s == 0 && e == 0, "word_at: empty");
}

static void test_sel_spans(void) {
    CaretField cf;
    CaretSpan  sp[8];
    int n;
    /* "ab cd ef" wrapped to two lines "ab cd\nef" (simulate). slots per line built by
       caret_field_build with unit advances + space_adv 1. */
    int   map[CARET_MAX_SLOTS];
    float adv[CARET_MAX_SLOTS];
    int   wlen = caret_reconcile("ab cd\nef", "ab cd\nef", map, CARET_MAX_SLOTS);
    {   /* unit advance per lead byte, 0 for '\n' */
        int i = 0;
        while (i < wlen) { int k = caret_cplen((unsigned char)"ab cd\nef"[i]);
            adv[i] = ("ab cd\nef"[i] == '\n') ? 0.0f : 1.0f;
            for (++i; k > 1 && i < wlen; k--, i++) adv[i] = 0.0f; }
    }
    caret_field_build("ab cd\nef", "ab cd\nef", map, adv, wlen, 1.0f, 1.0f, &cf);
    n = caret_sel_spans(&cf, 0, 2, sp, 8);         /* "ab" on line 0 */
    CHECK(n == 1 && sp[0].line == 0 && sp[0].x0 == 0.0f && sp[0].x1 == 2.0f, "sel_spans: single line");
    n = caret_sel_spans(&cf, 1, 7, sp, 8);         /* "b cd\nef"-ish span across 2 lines */
    CHECK(n == 2, "sel_spans: two lines -> two spans");
    CHECK(sp[0].line == 0 && sp[0].x0 == 1.0f, "sel_spans: line0 starts at lo.x");
    CHECK(sp[1].line == 1 && sp[1].x0 == 0.0f, "sel_spans: line1 starts at 0");
    n = caret_sel_spans(&cf, 3, 3, sp, 8);         /* empty */
    CHECK(n == 0, "sel_spans: empty range -> 0");
}
```
And in `main`: add `test_word_at();` and `test_sel_spans();` after `test_field_trailing_space();`.

- [ ] **Step 3: Verify the tests FAIL** (link error / asserts): `./build.sh carettest` — undefined `caret_word_at`/`caret_sel_spans` (or stub them returning nothing to see asserts fail).

- [ ] **Step 4: Implement in `caret.c`** (append after `caret_slot_nearest_x`):

```c
static int caret_class(unsigned char c) {
    if (c == ' ' || c == '\t' || c == '\n') return 0;          /* space */
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c >= 0x80u) return 1;  /* word */
    return 2;                                                  /* other (punct) */
}

void caret_word_at(const char *src, int off, int *start, int *end) {
    int len = 0, cls, s, e;
    while (src[len] != '\0') len++;
    if (off < 0) off = 0;
    if (off > len) off = len;
    if (off >= len) {                       /* end of text: take the run ending here */
        if (len == 0) { *start = 0; *end = 0; return; }
        cls = caret_class((unsigned char)src[len - 1]);
        s = len;
        while (s > 0 && caret_class((unsigned char)src[s - 1]) == cls) s--;
        *start = s; *end = len; return;
    }
    cls = caret_class((unsigned char)src[off]);
    s = off; while (s > 0   && caret_class((unsigned char)src[s - 1]) == cls) s--;
    e = off; while (e < len && caret_class((unsigned char)src[e])     == cls) e++;
    *start = s; *end = e;
}

int caret_sel_spans(const CaretField *cf, int lo, int hi, CaretSpan *out, int cap) {
    int n = 0, slo, shi, llo, lhi, li;
    if (lo >= hi) return 0;
    slo = caret_slot_for_offset(cf, lo);
    shi = caret_slot_for_offset(cf, hi);
    if (slo < 0 || shi < 0) return 0;
    llo = caret_line_of_slot(cf, slo);
    lhi = caret_line_of_slot(cf, shi);
    for (li = llo; li <= lhi && n < cap; li++) {
        int   last = cf->lines[li].slot0 + cf->lines[li].nslots - 1;
        float x0   = (li == llo) ? cf->slots[slo].x : 0.0f;
        float x1   = (li == lhi) ? cf->slots[shi].x : cf->slots[last].x;
        out[n].line = li; out[n].x0 = x0; out[n].x1 = x1;
        n++;
    }
    return n;
}
```

- [ ] **Step 5: Verify PASS + gauntlet:** `./build.sh carettest && ./caret_test` → `caret_test: all passed`; `./build.sh c89check` → PASS.

- [ ] **Step 6: Commit**

```bash
git add caret.h caret.c caret_test.c
git commit -m "$(cat <<'EOF'
Note selection: pure caret_word_at + caret_sel_spans

caret_word_at: the maximal word/space/other run around an offset
(multibyte = word). caret_sel_spans: per-visual-line highlight rects for
a [lo,hi) selection. Both pure + unit-tested; used by the mouse/keyboard
selection and the highlight render that follow.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Keyboard selection + highlight + selection-aware editing

**Files:** Modify `main.c`.

This makes selection fully usable + visible via the keyboard (Shift+arrows select; the highlight shows; type-over/delete work). Mouse comes in Task 3.

- [ ] **Step 1: AppState field + seed it**

At main.c:2851 (after `float edit_goal_x;`), add:
```c
    int         edit_sel_anchor;  /* selection's fixed end; span = [min,max) with edit_cursor */
```
In `note_edit_begin` (main.c:16604), after `st->edit_cursor = st->edit_len;`, add:
```c
    st->edit_sel_anchor = st->edit_cursor;   /* no selection on open */
```

- [ ] **Step 2: Selection helpers** — place just above `caret_build` (grep `static int caret_build`), so on_char/edit-key can call them:
```c
static int edit_sel_lo(const AppState *st) {
    return st->edit_cursor < st->edit_sel_anchor ? st->edit_cursor : st->edit_sel_anchor;
}
static int edit_sel_hi(const AppState *st) {
    return st->edit_cursor > st->edit_sel_anchor ? st->edit_cursor : st->edit_sel_anchor;
}
static int edit_has_sel(const AppState *st) {
    return st->edit_cursor != st->edit_sel_anchor;
}
```

- [ ] **Step 3: `selection_delete`** — place right after `caret_refresh_goal` (grep it). Forward-declare it near the `caret_build` forward decl if any caller precedes its definition (on_char at 16659 precedes ~16540 — define before on_char; placing it after `caret_refresh_goal` [~16540] is before on_char, so no forward decl needed):
```c
/* remove the current selection [lo,hi) and collapse; mirror to meta + autosize.
   No-op when there's no selection. */
static void selection_delete(AppState *st) {
    int lo, hi;
    if (!edit_has_sel(st)) return;
    lo = edit_sel_lo(st);
    hi = edit_sel_hi(st);
    memmove(st->edit_buf + lo, st->edit_buf + hi, (size_t)(st->edit_len - hi));
    st->edit_len -= (hi - lo);
    st->edit_cursor = st->edit_sel_anchor = lo;
    st->edit_buf[st->edit_len] = '\0';
    scene_meta_set(&st->scene, st->edit_handle, "text", st->edit_buf);
    note_autosize(st, st->edit_handle);
    caret_refresh_goal(st);
}
```

- [ ] **Step 4: on_char type-over** — in `on_char` (main.c:16670-16682), after `if (st->edit_handle == 0) return;` and `n = utf8_encode(...)` + the cap guard, the insert must act at the collapsed point. Replace the body from `n = utf8_encode(cp, enc);` through the end with:
```c
    n = utf8_encode(cp, enc);
    if (n <= 0) return;
    selection_delete(st);                                   /* type-over a selection */
    if (st->edit_len + n >= EDIT_BUF_CAP) return;
    memmove(st->edit_buf + st->edit_cursor + n,
            st->edit_buf + st->edit_cursor,
            (size_t)(st->edit_len - st->edit_cursor));
    memcpy(st->edit_buf + st->edit_cursor, enc, (size_t)n);
    st->edit_len    += n;
    st->edit_cursor += n;
    st->edit_sel_anchor = st->edit_cursor;
    st->edit_buf[st->edit_len] = '\0';
    scene_meta_set(&st->scene, st->edit_handle, "text", st->edit_buf);
    note_autosize(st, st->edit_handle);
    caret_refresh_goal(st);
```

- [ ] **Step 5: Backspace/Delete/Enter selection-aware** — in the edit-key block (main.c:16779-16810):
  - **Backspace** branch: change the guard so a selection deletes whole. Replace `} else if (key == GLFW_KEY_BACKSPACE && st->edit_cursor > 0) {` ... through its body with:
```c
        } else if (key == GLFW_KEY_BACKSPACE && (edit_has_sel(st) || st->edit_cursor > 0)) {
            if (edit_has_sel(st)) { selection_delete(st); }
            else {
                int e = st->edit_cursor, s = e - 1;
                while (s > 0 && ((unsigned char)st->edit_buf[s] & 0xC0u) == 0x80u) s--;
                memmove(st->edit_buf + s, st->edit_buf + e, (size_t)(st->edit_len - e));
                st->edit_len   -= (e - s);
                st->edit_cursor = st->edit_sel_anchor = s;
                st->edit_buf[st->edit_len] = '\0';
                scene_meta_set(&st->scene, st->edit_handle, "text", st->edit_buf);
                note_autosize(st, st->edit_handle);
                caret_refresh_goal(st);
            }
        } else if (key == GLFW_KEY_DELETE && (edit_has_sel(st) || st->edit_cursor < st->edit_len)) {
            if (edit_has_sel(st)) { selection_delete(st); }
            else {
                int s = st->edit_cursor, e = s + 1;
                while (e < st->edit_len && ((unsigned char)st->edit_buf[e] & 0xC0u) == 0x80u) e++;
                memmove(st->edit_buf + s, st->edit_buf + e, (size_t)(st->edit_len - e));
                st->edit_len -= (e - s);
                st->edit_sel_anchor = st->edit_cursor;
                st->edit_buf[st->edit_len] = '\0';
                scene_meta_set(&st->scene, st->edit_handle, "text", st->edit_buf);
                note_autosize(st, st->edit_handle);
                caret_refresh_goal(st);
            }
        } else if (key == GLFW_KEY_ENTER && st->edit_len + 1 < EDIT_BUF_CAP) {
            selection_delete(st);                            /* type-over with a newline */
            memmove(st->edit_buf + st->edit_cursor + 1,
                    st->edit_buf + st->edit_cursor,
                    (size_t)(st->edit_len - st->edit_cursor));
            st->edit_buf[st->edit_cursor] = '\n';
            st->edit_len++;
            st->edit_cursor++;
            st->edit_sel_anchor = st->edit_cursor;
            st->edit_buf[st->edit_len] = '\0';
            scene_meta_set(&st->scene, st->edit_handle, "text", st->edit_buf);
            note_autosize(st, st->edit_handle);
            caret_refresh_goal(st);
```
  (This replaces the existing Backspace, Delete, and Enter `else if` branches in place.)

- [ ] **Step 6: Shift+arrows + plain-arrow collapse** — replace the Left, Right, and Up/Down branches (main.c:16811-end-of-Up/Down) with these (a `shift` local read from `mods`):
```c
        } else if (key == GLFW_KEY_LEFT) {
            int shift = (mods & GLFW_MOD_SHIFT) != 0;
            if (!shift && edit_has_sel(st)) {
                st->edit_cursor = st->edit_sel_anchor = edit_sel_lo(st);   /* collapse to start */
            } else if (st->edit_cursor > 0) {
                st->edit_cursor--;
                while (st->edit_cursor > 0 &&
                       ((unsigned char)st->edit_buf[st->edit_cursor] & 0xC0u) == 0x80u)
                    st->edit_cursor--;
                if (!shift) st->edit_sel_anchor = st->edit_cursor;
            }
            caret_refresh_goal(st);
        } else if (key == GLFW_KEY_RIGHT) {
            int shift = (mods & GLFW_MOD_SHIFT) != 0;
            if (!shift && edit_has_sel(st)) {
                st->edit_cursor = st->edit_sel_anchor = edit_sel_hi(st);   /* collapse to end */
            } else if (st->edit_cursor < st->edit_len) {
                st->edit_cursor++;
                while (st->edit_cursor < st->edit_len &&
                       ((unsigned char)st->edit_buf[st->edit_cursor] & 0xC0u) == 0x80u)
                    st->edit_cursor++;
                if (!shift) st->edit_sel_anchor = st->edit_cursor;
            }
            caret_refresh_goal(st);
        } else if (key == GLFW_KEY_UP || key == GLFW_KEY_DOWN) {
            int shift = (mods & GLFW_MOD_SHIFT) != 0;
            if (!shift && edit_has_sel(st)) {
                st->edit_cursor = st->edit_sel_anchor =
                    (key == GLFW_KEY_UP) ? edit_sel_lo(st) : edit_sel_hi(st);
            } else {
                SceneObject *o = scene_get(&st->scene, st->edit_handle);
                if (o && st->ui_font) {
                    float      lh     = font_line_height(st->ui_font);
                    float      cw     = mesh_ref_param("card", o->mesh_params, o->mesh_param_count, "w");
                    float      usable = cw - 3.0f * 0.025f;
                    float      bpx2m  = note_text_size(&st->scene, st->edit_handle) / lh;
                    CaretField cf;
                    int        slot, line, tgt;
                    caret_build(st->ui_font, st->edit_buf, bpx2m, usable, &cf);
                    slot = caret_slot_for_offset(&cf, st->edit_cursor);
                    line = (slot >= 0) ? caret_line_of_slot(&cf, slot) : 0;
                    if (key == GLFW_KEY_UP)   line = (line > 0) ? line - 1 : -1;
                    else                      line = (line + 1 < cf.line_count) ? line + 1 : -1;
                    if (line < 0) st->edit_cursor = (key == GLFW_KEY_UP) ? 0 : st->edit_len;
                    else {
                        tgt = caret_slot_nearest_x(&cf, line, st->edit_goal_x);
                        if (tgt >= 0) st->edit_cursor = cf.slots[tgt].src;
                    }
                    if (!shift) st->edit_sel_anchor = st->edit_cursor;
                }
            }
        }
```
(Up/Down still does NOT call `caret_refresh_goal` — the goal column is preserved. Left/Right do.)

- [ ] **Step 7: The highlight render** — restructure the `KIND_NOTE` block (main.c:15660-15697) so the highlight draws BEFORE the text. The block currently draws `wtext_block` (text) then the caret. Replace from `if (txt && txt[0]) {` (the wtext_block draw) and the following caret `if (state->edit_handle == o->handle)` block with:
```c
                if (state->edit_handle == o->handle) {     /* selection highlight (behind ink) */
                    float       bpx2m = note_text_size(&state->scene, o->handle) / lh;
                    float       x0 = -cw * 0.5f + 2.0f * margin;
                    float       y0 = ch - 2.0f * margin;
                    const char *src = txt ? txt : "";
                    CaretField  cf;
                    caret_build(uf, src, bpx2m, usable, &cf);
                    if (state->caret_mesh.index_count == 0) {
                        MeshBuilder mb;
                        mb_init(&mb);
                        make_page(&mb, 1.0f, 1.0f);
                        state->caret_mesh = mesh_from_builder(&mb);
                        mb_free(&mb);
                    }
                    if (state->edit_cursor != state->edit_sel_anchor) {
                        CaretSpan sp[CARET_MAX_LINES];
                        int lo = state->edit_cursor < state->edit_sel_anchor
                                     ? state->edit_cursor : state->edit_sel_anchor;
                        int hi = state->edit_cursor > state->edit_sel_anchor
                                     ? state->edit_cursor : state->edit_sel_anchor;
                        int ns = caret_sel_spans(&cf, lo, hi, sp, CARET_MAX_LINES), si;
                        Material hm = material_default();
                        hm.base_color = vec3_make(0.30f, 0.45f, 0.85f);   /* soft blue */
                        for (si = 0; si < ns; si++) {
                            float w  = sp[si].x1 - sp[si].x0;
                            vec3  hp;
                            mat4  hmodel;
                            if (w <= 0.0f) continue;
                            hp = vec3_make(x0 + sp[si].x0 + w * 0.5f,
                                           y0 - (float)sp[si].line * cf.line_h - cf.line_h * 0.5f,
                                           0.0004f);
                            hmodel = mat4_mul(face, mat4_from_trs(hp, quat_identity(),
                                              vec3_make(w, cf.line_h, 1.0f)));
                            draw_glass(state, state->caret_mesh, hmodel, view, proj, eye, hm);
                        }
                    }
                }
                if (txt && txt[0]) {
                    float bpx2m = note_text_size(&state->scene, o->handle) / lh;
                    wtext_block(uf, vp, face, txt,
                                -cw * 0.5f + 2.0f * margin, ch - 2.0f * margin,
                                bpx2m, usable, ink_r, ink_g, ink_b);
                }
                if (state->edit_handle == o->handle) {     /* the caret quad, on top */
                    float       bpx2m = note_text_size(&state->scene, o->handle) / lh;
                    float       x0 = -cw * 0.5f + 2.0f * margin;
                    float       y0 = ch - 2.0f * margin;
                    const char *src = txt ? txt : "";
                    CaretField  cf;
                    int         slot, linei;
                    Material    cm = material_default();
                    caret_build(uf, src, bpx2m, usable, &cf);
                    slot  = caret_slot_for_offset(&cf, state->edit_cursor);
                    linei = (slot >= 0) ? caret_line_of_slot(&cf, slot) : 0;
                    if (slot >= 0) {
                        float cw_caret = 0.006f;
                        float ctop = y0 - (float)linei * cf.line_h;
                        vec3  cpos = vec3_make(x0 + cf.slots[slot].x + cw_caret * 0.5f,
                                               ctop - cf.line_h * 0.5f, 0.0006f);
                        mat4  cmodel = mat4_mul(face,
                                   mat4_from_trs(cpos, quat_identity(),
                                                 vec3_make(cw_caret, cf.line_h, 1.0f)));
                        cm.base_color = vec3_make(0.10f, 0.09f, 0.08f);
                        cm.emissive   = vec3_make(0.10f, 0.09f, 0.08f);
                        draw_mesh(state, state->caret_mesh, cmodel, view, proj, eye, cm);
                    }
                }
```
(The original `if (txt && txt[0]) { wtext_block }` + the original caret block are REPLACED by the three blocks above: highlight, then text, then caret. The lazy `caret_mesh` build moves into the highlight block which now runs first; the caret block no longer builds it. Note `draw_mesh`'s signature here matches the existing call — keep it identical to the original caret draw.)

- [ ] **Step 8: Gauntlet + commit**

```bash
./build.sh c89check && ./build.sh && ./build.sh metal && ./build.sh carettest && ./caret_test
```
All green. (Human live-verify happens after Task 3.)

```bash
git add main.c
git commit -m "$(cat <<'EOF'
Note selection: keyboard select + highlight + selection editing

edit_sel_anchor defines a span with edit_cursor. Shift+arrows extend,
plain arrows collapse to an edge. on_char/Enter type-over a selection;
Backspace/Delete remove it (selection_delete). The note render draws a
translucent soft-blue highlight (caret_sel_spans) behind the ink, caret
on top. Mouse selection follows.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Mouse selection — drag, double/triple-click, Shift+click

**Files:** Modify `main.c`.

- [ ] **Step 1: Factor `caret_hit_offset` from `caret_click_place`**

Replace `caret_click_place` (main.c:16557-16601) with a hit helper + a thin place wrapper:
```c
/* the source byte offset under the cursor on the note being edited; returns 1 and
   writes *out on a hit, 0 if off the card / not board view. */
static int caret_hit_offset(AppState *st, GLFWwindow *w, int *out) {
    SceneObject *o;
    Ray   ray;
    mat4  face;
    vec3  origin, rx, ry, nrm, hit, d;
    float cw, ch, ct, t, rr2, ru2, lx, ly, lh, bpx2m, usable, x0, y0;
    CaretField cf;
    int   line, slot;
    if (st->board_view == 0) return 0;
    o = scene_get(&st->scene, st->edit_handle);
    if (!o || !st->ui_font) return 0;
    cw = mesh_ref_param("card", o->mesh_params, o->mesh_param_count, "w");
    ch = mesh_ref_param("card", o->mesh_params, o->mesh_param_count, "h");
    ct = mesh_ref_param("card", o->mesh_params, o->mesh_param_count, "t");
    face = mat4_mul(scene_world_matrix(&st->scene, o),
                    mat4_translate(vec3_make(0.0f, 0.0f, ct * 0.5f + 0.0008f)));
    origin = mat4_mul_point(face, vec3_make(0.0f, 0.0f, 0.0f));
    rx     = vec3_sub(mat4_mul_point(face, vec3_make(1.0f, 0.0f, 0.0f)), origin);
    ry     = vec3_sub(mat4_mul_point(face, vec3_make(0.0f, 1.0f, 0.0f)), origin);
    nrm    = vec3_normalize(vec3_cross(rx, ry));
    ray    = pick_ray(st, w);
    if (!ray_vs_plane(ray, origin, nrm, &t) || t <= 0.0f) return 0;
    hit = vec3_add(ray.origin, vec3_scale(ray.dir, t));
    d   = vec3_sub(hit, origin);
    rr2 = vec3_dot(rx, rx); ru2 = vec3_dot(ry, ry);
    if (rr2 < 1e-9f || ru2 < 1e-9f) return 0;
    lx = vec3_dot(d, rx) / rr2;
    ly = vec3_dot(d, ry) / ru2;
    if (lx < -cw * 0.5f || lx > cw * 0.5f || ly < 0.0f || ly > ch) return 0;
    lh     = font_line_height(st->ui_font);
    usable = cw - 3.0f * 0.025f;
    bpx2m  = note_text_size(&st->scene, st->edit_handle) / lh;
    x0     = -cw * 0.5f + 2.0f * 0.025f;
    y0     = ch - 2.0f * 0.025f;
    caret_build(st->ui_font, st->edit_buf, bpx2m, usable, &cf);
    line = (int)((y0 - ly) / cf.line_h);
    if (line < 0) line = 0;
    if (line >= cf.line_count) line = cf.line_count - 1;
    slot = caret_slot_nearest_x(&cf, line, lx - x0);
    if (slot < 0) return 0;
    *out = cf.slots[slot].src;
    st->edit_goal_x = lx - x0;
    return 1;
}

static int caret_click_place(AppState *st, GLFWwindow *w) {
    int off;
    if (!caret_hit_offset(st, w, &off)) return 0;
    st->edit_cursor = st->edit_sel_anchor = off;   /* place caret, clear selection */
    return 1;
}
```
(The forward decl `static int caret_click_place(AppState *st, GLFWwindow *w);` at main.c:10666 stays. `caret_hit_offset` is defined before `caret_click_place`, both before the modal gate's use — but the modal gate at 10817 calls them from `read_input` which precedes the definitions, so ALSO add a forward decl `static int caret_hit_offset(AppState *st, GLFWwindow *w, int *out);` next to the `caret_click_place` forward decl at 10666.)

- [ ] **Step 2: AppState `click_seq` + a drag flag + the bump helper**

At main.c:2852 (after the new `edit_sel_anchor`), add:
```c
    int         click_seq;        /* shared multi-click counter (1 single, 2 double, 3 triple) */
    sol_bool    edit_dragging;    /* a left-drag is selecting text in the edited note */
```
Add the bump helper near `caret_hit_offset`:
```c
/* advance the shared multi-click counter on a left-press; returns the count within
   the BOARD_DBL time/px window (1,2,3,...). Updates last_press_*. */
static int click_seq_bump(AppState *st, double mx, double my) {
    double now = glfwGetTime();
    if (now - st->last_press_t < BOARD_DBL_S &&
        fabs(mx - st->last_press_x) < BOARD_DBL_PX &&
        fabs(my - st->last_press_y) < BOARD_DBL_PX)
        st->click_seq += 1;
    else
        st->click_seq = 1;
    st->last_press_t = now;
    st->last_press_x = mx;
    st->last_press_y = my;
    return st->click_seq;
}
```
Forward-declare it next to `caret_hit_offset`'s forward decl (it's used in `read_input` before its definition): `static int click_seq_bump(AppState *st, double mx, double my);`.

- [ ] **Step 3: Editing modal gate — press/drag/multi-click + shift** — replace the editing branch in the modal gate (main.c:10817-10822):
```c
        if (st->edit_handle != 0) {     /* board-view click: place/select; drag extends; else blur */
            sol_bool lmb = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            if (lmb && !st->lmb_was_down) {            /* press */
                int seq = click_seq_bump(st, mx, my);
                int off;
                if (!caret_hit_offset(st, w, &off)) {
                    note_edit_end(st);                 /* off the note -> blur */
                } else if (glfwGetKey(w, GLFW_KEY_LEFT_SHIFT)  == GLFW_PRESS ||
                           glfwGetKey(w, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS) {
                    st->edit_cursor = off;             /* shift-click: extend, keep anchor */
                    st->edit_dragging = SOL_TRUE;
                } else if (seq == 2) {                 /* double: word */
                    int s, e;
                    caret_word_at(st->edit_buf, off, &s, &e);
                    st->edit_sel_anchor = s; st->edit_cursor = e;
                    st->edit_dragging = SOL_FALSE;
                } else if (seq >= 3) {                 /* triple: all */
                    st->edit_sel_anchor = 0; st->edit_cursor = st->edit_len;
                    st->edit_dragging = SOL_FALSE;
                } else {                               /* single: caret + arm drag */
                    st->edit_cursor = st->edit_sel_anchor = off;
                    st->edit_dragging = SOL_TRUE;
                }
            } else if (lmb && st->lmb_was_down && st->edit_dragging) {   /* drag extends */
                int off;
                if (caret_hit_offset(st, w, &off)) st->edit_cursor = off;
            } else if (!lmb) {
                st->edit_dragging = SOL_FALSE;         /* release */
            }
            st->lmb_was_down = lmb;
        } else if (reader_is_editing(st)) {
```
(Leave the `else if (reader_is_editing(st))` and the rest of the modal-gate chain unchanged.)

- [ ] **Step 4: Board-view double-click → shared counter + word-on-enter-edit** — in the board-view press handler (main.c:11018-11045), replace the `is_dbl` computation block (11019-11026) so it reads the shared counter, and make the note double-click select the clicked word:

Replace:
```c
                if (st->board_view != 0) {              /* board-view double-click detect */
                    double now = glfwGetTime();
                    is_dbl = (sol_bool)(now - st->last_press_t < BOARD_DBL_S &&
                                        fabs(mx - st->last_press_x) < BOARD_DBL_PX &&
                                        fabs(my - st->last_press_y) < BOARD_DBL_PX);
                    st->last_press_t = now;
                    st->last_press_x = mx; st->last_press_y = my;
                    if (is_dbl) st->last_press_t = 0.0;   /* consume: a 3rd click isn't a 2nd double */
                }
```
with:
```c
                if (st->board_view != 0)                /* shared multi-click counter */
                    is_dbl = (sol_bool)(click_seq_bump(st, mx, my) == 2);
```
And in the note double-click branch, replace `note_edit_begin(st, st->selected_handle);` (main.c:11037) with:
```c
                    } else if (so && so->kind == KIND_NOTE) {
                        int off;
                        note_edit_begin(st, st->selected_handle);
                        if (caret_hit_offset(st, w, &off)) {   /* select the clicked word */
                            int s, e;
                            caret_word_at(st->edit_buf, off, &s, &e);
                            st->edit_sel_anchor = s; st->edit_cursor = e;
                        }
```
(Keep the rest of the `is_dbl` branch — folder nav, create-note — unchanged.)

- [ ] **Step 5: Gauntlet + commit**

```bash
./build.sh c89check && ./build.sh && ./build.sh metal && ./build.sh carettest && ./caret_test
```
All green.

```bash
git add main.c
git commit -m "$(cat <<'EOF'
Note selection: drag + double/triple-click + Shift-click

Factor caret_hit_offset from caret_click_place. A shared click_seq
counter (within the BOARD_DBL window) drives double->word / triple->all
across the board-view and editing input paths, so triple-click works
from a cold note (card -> edit+word -> all). The editing modal gate
press places/selects, a drag extends the span, Shift-click extends.
Double-clicking a note to edit selects the clicked word.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-Review notes (for the implementer)

- **Spec coverage:** Task 1 = spec §4/§7 pure logic. Task 2 = §1 state, §5 Shift+arrows/collapse, §6 selection editing, §7 highlight. Task 3 = §2 click_seq, §3 hit/drag/press, §4 word-on-enter. Non-goals respected (no clipboard, no word-drag, reader untouched, text_shape seam untouched).
- **Type/name consistency:** fields `edit_sel_anchor` / `click_seq` / `edit_dragging`; helpers `edit_sel_lo`/`edit_sel_hi`/`edit_has_sel`/`selection_delete`/`caret_hit_offset`/`click_seq_bump`; pure `caret_word_at`/`caret_sel_spans`/`CaretSpan`. Used identically across tasks.
- **Forward decls (C89 define-before-use):** `caret_hit_offset` and `click_seq_bump` are called in `read_input` (~10817) far above their definitions (~16540s) — add forward decls beside the existing `caret_click_place` forward decl (10666). `selection_delete`/`edit_sel_*` are defined before `on_char` (16659), no forward decl needed.
- **In-place edit-key replacements (Task 2 Steps 5-6):** the Backspace/Delete/Enter and Left/Right/Up/Down `else if` branches are replaced *in place* inside `if (st->edit_handle != 0) { … }`. Keep the leading `if (key == GLFW_KEY_ESCAPE …)` branch and the trailing `return; /* everything else stays quiet */` + the block's closing brace (currently main.c:16845-16846) intact — only the branch bodies change. The build gauntlet catches any brace mismatch.
- **Cursor stays on codepoint boundaries:** word offsets come from `caret_word_at` (class runs over whole multibyte chars), click/drag from slot `.src`, arrows codepoint-walk — so `selection_delete`'s memmove never splits a codepoint.
- **Highlight order:** drawn BEFORE `wtext_block` (ink on top); `caret_build` is called up to 3× per edited note per frame (highlight, text-implicit, caret) — acceptable (one edited note; per-frame, not per-note).
- **Human live-verify (after Task 3, both backends):** drag-select a span (highlight follows); double-click→word, triple-click→whole note, triple from a cold note; Shift+arrows extend, plain arrow collapses, Shift+click extends; type-over / Backspace / Delete / Enter on a selection; highlight spans wrapped lines; blur clears selection; **regression:** caret move/place, double-click-to-edit, autosize, Esc, and board CARD cut/paste (Cmd+X/V) still act on cards.
