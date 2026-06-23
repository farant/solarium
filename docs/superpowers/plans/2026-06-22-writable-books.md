# Writable Books Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Generated codices become notebooks you can write in (discrete growable pages, append-only) and shelve like cards.

**Architecture:** All in `main.c`. Part B (writing) holds an editable page array on the reader and **packs it into the reader's existing wrapped blob** (each page padded to the fixed lines-per-page), so the page-draw + leaf-turn code is reused untouched; editing reuses the note-input model. Part A (shelving) generalizes the carry-file path from `"card"` to codex groups. No new mesh, **no new shader → no MSL twin**.

**Tech Stack:** C89, GLFW, the reader rig + carry/file + scene/wtext layers. Build gauntlet: `./build.sh c89check && ./build.sh debug && ./build.sh metal`.

---

## Context the implementer needs

- **GUI + world-render feature → human live-verify.** Each task's gate is the build gauntlet; full in-engine verification runs at the end of each Part. Don't fabricate `*_test` targets.
- **C89:** declarations at the top of each block, `/* */` only, `snprintf`/`strncpy`, no VLAs.
- **A codex is a group:** mesh-less anchor (`meta["name"]="codex"`) + `book_cover` + `book_block` children. `codex_cover_child(s, root) != 0` ⟺ it's a codex (the editable predicate). `cmd_mint_codex` (`V`) creates one.
- **Reader facts:** `reader_open` (`main.c:5653`) sets `reader_source = root`, computes `cover = codex_cover_child(root)`, builds the open-book mesh, then calls `reader_load_content(o->content)`. Field metrics (`reader_open`/`reader_load_content`): `wb = bp[0]-bp[4]`, `zh = bp[1]*0.5-bp[4]`, `mg = wb*0.06`, `xf = wb*BOOK_GUTTER_FRAC`, `field_w = wb-xf-2*mg`, `field_h = 2*zh-2*mg`, `px2m = (bp[1]*0.022)/font_line_height`, `L = field_h/(font_line_height*px2m)` (≥1). `bp = st->reader_params`.
- **Page render** (`reader_draw_page` `main.c:5910`) draws `reader_text` lines `[page*L, page*L+L)`; `reader_line_off[i]` = byte offset of line i; `reader_line_count` total; `reader_spread` = current pair. The leaf-turn animation (`reader_turning`/`reader_turn_old`/`reader_turn_t`) turns whole spreads.
- **`reader_free_text`** frees `reader_text`+`reader_line_off` **and resets `reader_spread = 0`** — do NOT call it from the per-edit pack (it would snap the view to page 0); free those two fields inline instead.
- **Note input model:** `on_char` (`main.c:11617`) `utf8_encode(cp, enc)` → append; `on_key` editing branch (`main.c:11684`) Backspace walks back over UTF-8 continuation bytes, Enter inserts `\n`. `read_input` early-returns (suppressing movement/look, with note click-blur) when `edit_handle != 0 || palette.open` (`main.c:7398`).
- **Read-only reader arrow-flip** lives in `read_input` (`main.c:7504`) and is gated on `reader_state==OPEN && reader_text`; it stays for read-only books. Editing suppresses that block (early-return) and flips via `on_key` instead.
- **Carry/file:** `carry_target` (`main.c:6628`) resolves to `group_root` but rejects a parented anchor. `carry_update`'s furniture-preview loop and `cmd_carry_toggle`'s `file_aim` drop are gated on the carried object being a `"card"`. `furniture_surface_aim` only succeeds when the ray hits the shelf/table; `shelf_free_slot`/`furniture_shelf_slot` give the slot.

---

# PART B — Write in generated books

### Task 1: Editable page array + render the codex's pages

**Files:** Modify `main.c` — AppState reader fields; helpers near the reader (`reader_load_content` is `main.c:5547`, define the new helpers just above `reader_open` at `main.c:5653`); `reader_open` content branch.

- [ ] **Step 1: Add reader state fields**

After the reader field `int reader_spread;` (`main.c:2735`) add:

```c
    char      **reader_pages;      /* editable codex: page text array */
    int         reader_page_count;
    int         reader_page;       /* current page (the caret's page) */
    sol_bool    reader_editable;   /* a codex opened as a writable notebook */
```

- [ ] **Step 2: Add the page helpers**

Immediately above `static void reader_open(` (`main.c:5653`), add:

```c
static char *reader_strdup(const char *s) {
    size_t n = strlen(s ? s : "");
    char  *d = (char *)malloc(n + 1);
    if (d) { memcpy(d, s ? s : "", n); d[n] = '\0'; }
    return d;
}

static void reader_free_pages(AppState *st) {
    int i;
    if (st->reader_pages) {
        for (i = 0; i < st->reader_page_count; i++) free(st->reader_pages[i]);
        free(st->reader_pages);
    }
    st->reader_pages      = (char **)0;
    st->reader_page_count = 0;
    st->reader_page       = 0;
}

/* load page0..page{N-1} from the codex anchor's meta; a fresh book = 1 blank page */
static void reader_load_pages(AppState *st, sol_u32 root) {
    const char *pc = scene_meta_get(&st->scene, root, "pagecount");
    int n = pc ? atoi(pc) : 0, i;
    char key[16];
    reader_free_pages(st);
    if (n < 1) n = 1;
    if (n > 4096) n = 4096;
    st->reader_pages = (char **)malloc(sizeof(char *) * (size_t)n);
    if (!st->reader_pages) return;
    for (i = 0; i < n; i++) {
        const char *t;
        snprintf(key, sizeof key, "page%d", i);
        t = scene_meta_get(&st->scene, root, key);
        st->reader_pages[i] = reader_strdup(t ? t : "");
    }
    st->reader_page_count = n;
    st->reader_page       = 0;
}

static float reader_field_w(const AppState *st) {
    const float *bp = st->reader_params;
    float wb = bp[0] - bp[4];
    return wb - wb * BOOK_GUTTER_FRAC - 2.0f * (wb * 0.06f);
}

/* Rebuild reader_text from the page array so the EXISTING page/leaf render works:
   each page is wrapped to field_w, then capped + padded to exactly L lines and
   concatenated. The current page shows a trailing caret while open. Frees the old
   reader_text/line_off INLINE (not reader_free_text — that resets reader_spread). */
static void reader_pack_pages(AppState *st) {
    const float *bp = st->reader_params;
    float zh = bp[1] * 0.5f - bp[4];
    float field_h, field_w = reader_field_w(st);
    int   L, pg, c, line, total_lines;
    long  cap, used;
    char *out;
    int  *offs;
    if (!st->ui_font || st->reader_page_count <= 0) return;
    st->reader_font = st->ui_font;
    st->reader_px2m = (bp[1] * 0.022f) / font_line_height(st->reader_font);
    field_h = 2.0f * zh - 2.0f * (bp[0] - bp[4]) * 0.06f;
    L = (int)(field_h / (font_line_height(st->reader_font) * st->reader_px2m));
    if (L < 1) L = 1;
    st->reader_lines_per_page = L;
    total_lines = st->reader_page_count * L;
    cap  = (long)total_lines * 512L + 256L;
    out  = (char *)malloc((size_t)cap);
    offs = (int *)malloc(sizeof(int) * (size_t)(total_lines + 1));
    if (!out || !offs) { free(out); free(offs); return; }
    used = 0; line = 0;
    for (pg = 0; pg < st->reader_page_count; pg++) {
        char        wbuf[8192];
        char        caretp[8192];
        const char *src = st->reader_pages[pg] ? st->reader_pages[pg] : "";
        const char *p;
        int         nlines;
        if (pg == st->reader_page && st->reader_editable &&
            st->reader_state != READER_RETURNING) {
            size_t sl = strlen(src);
            if (sl > sizeof(caretp) - 2) sl = sizeof(caretp) - 2;
            memcpy(caretp, src, sl); caretp[sl] = '_'; caretp[sl + 1] = '\0';
            src = caretp;
        }
        nlines = text_wrap(st->reader_font, src, st->reader_px2m, field_w,
                           wbuf, (int)sizeof wbuf);
        if (nlines < 1) { wbuf[0] = '\0'; }
        p = wbuf;
        for (c = 0; c < L; c++) {
            offs[line++] = (int)used;
            if (c < nlines) {
                while (*p && *p != '\n' && used < cap - 2) out[used++] = *p++;
                if (*p == '\n') p++;
            }
            out[used++] = '\n';
        }
    }
    out[used] = '\0';
    offs[line] = (int)used + 1;        /* sentinel */
    free(st->reader_text);             /* inline free: keep reader_spread */
    free(st->reader_line_off);
    st->reader_text       = out;
    st->reader_text_len   = used;
    st->reader_line_off   = offs;
    st->reader_line_count = total_lines;
}
```

- [ ] **Step 3: Branch `reader_open` for an editable codex**

In `reader_open`, replace the single content call (`main.c:5719`):

```c
    reader_load_content(st, o->content);       /* the card's file; a codex
                                                  carries none (yet) */
```

with:

```c
    if (cover != 0) {                          /* a codex: an editable notebook */
        st->reader_editable = SOL_TRUE;
        reader_load_pages(st, root);
        reader_pack_pages(st);
    } else {
        st->reader_editable = SOL_FALSE;
        reader_load_content(st, o->content);   /* a file/alias card: read-only */
    }
    st->reader_page   = 0;
    st->reader_spread = 0;
```

- [ ] **Step 4: Build + verify it renders**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: all PASS. In-engine: mint a book (`V`), select + `Enter` → it rises and shows blank cream pages (no crash); arrows do nothing useful yet (editing not wired). Esc closes (no save yet). (`reader_pages` leaks on close until Task 4 — acceptable mid-plan.)

- [ ] **Step 5: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
writable-books: editable page array, packed into the reader blob

A codex opens with reader_editable set and its pages (meta page0..N, >=1 blank)
loaded into a page array, packed into reader_text padded to L lines/page so the
existing page+leaf render is reused. File/alias cards still read-only.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Type into the current page (append-only)

**Files:** Modify `main.c` — `reader_is_editing` + append/backspace/newline helpers (near the Task 1 helpers); `on_char` (`main.c:11617`); `on_key` (before the note branch `main.c:11684`); `read_input` guard (`main.c:7398`).

- [ ] **Step 1: Add the edit helpers**

Below `reader_pack_pages` (Task 1), add:

```c
static sol_bool reader_is_editing(AppState *st) {
    return (sol_bool)(st->reader_editable && st->reader_state == READER_OPEN);
}

/* would the current page wrap to more than L lines? (capacity guard) */
static int reader_page_full(AppState *st, const char *text) {
    char wbuf[8192];
    int  lines = text_wrap(st->reader_font, text, st->reader_px2m,
                           reader_field_w(st), wbuf, (int)sizeof wbuf);
    return lines > st->reader_lines_per_page;
}

static void reader_page_append(AppState *st, const char *enc, int n) {
    char  *pg, *grown;
    size_t len;
    if (!st->reader_pages || st->reader_page >= st->reader_page_count) return;
    pg  = st->reader_pages[st->reader_page];
    len = pg ? strlen(pg) : 0;
    grown = (char *)malloc(len + (size_t)n + 1);
    if (!grown) return;
    if (pg) memcpy(grown, pg, len);
    memcpy(grown + len, enc, (size_t)n);
    grown[len + n] = '\0';
    if (reader_page_full(st, grown)) { free(grown); return; }  /* page is full */
    free(pg);
    st->reader_pages[st->reader_page] = grown;
    reader_pack_pages(st);
}

static void reader_page_backspace(AppState *st) {
    char  *pg;
    size_t len;
    if (!st->reader_pages || st->reader_page >= st->reader_page_count) return;
    pg  = st->reader_pages[st->reader_page];
    len = pg ? strlen(pg) : 0;
    if (len == 0) return;
    len--;
    while (len > 0 && ((unsigned char)pg[len] & 0xC0u) == 0x80u) len--;
    pg[len] = '\0';
    reader_pack_pages(st);
}

static void reader_page_newline(AppState *st) {
    char nl = '\n';
    reader_page_append(st, &nl, 1);            /* respects capacity */
}
```

Note: these reference `reader_state`/`READER_OPEN` and the reader fields, all defined above their use.

- [ ] **Step 2: Route `on_char` to the page**

In `on_char`, before `if (st->edit_handle == 0) return;` (`main.c:11623`), add:

```c
    if (reader_is_editing(st)) {
        n = utf8_encode(cp, enc);
        if (n > 0) reader_page_append(st, enc, n);
        return;
    }
```

(`n` and `enc` are already declared at the top of `on_char`.)

- [ ] **Step 3: Add the `on_key` editing branch**

In `on_key`, immediately before `if (st->edit_handle != 0) {` (`main.c:11684`), add:

```c
    if (reader_is_editing(st)) {
        if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
        if (key == GLFW_KEY_ESCAPE) { if (action == GLFW_PRESS) reader_close(st); }
        else if (key == GLFW_KEY_BACKSPACE) reader_page_backspace(st);
        else if (key == GLFW_KEY_ENTER)     reader_page_newline(st);
        return;
    }
```

(Arrow flipping is added in Task 3. `reader_close` is declared earlier — it's `main.c:5728`, well above `on_key`.)

- [ ] **Step 4: Capture the keyboard while editing**

In `read_input`'s guard (`main.c:7398`), extend the condition and add a click-to-close, mirroring the note blur:

```c
    if (st->edit_handle != 0 || st->palette.open || reader_is_editing(st)) {
        in->forward = in->back = in->left = in->right = SOL_FALSE;
        in->up = in->down = SOL_FALSE;
        in->look_dx = 0.0f;
        in->look_dy = 0.0f;
        in->zoom    = 0.0f;
        st->scroll_accum = 0.0;
        glfwGetCursorPos(w, &mx, &my);
        if (st->edit_handle != 0) {     /* note blur is edit-only */
            sol_bool lmb = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            if (lmb && !st->lmb_was_down) note_edit_end(st);
            st->lmb_was_down = lmb;
        } else if (reader_is_editing(st)) {  /* click closes the book (saves) */
            sol_bool lmb = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            if (lmb && !st->lmb_was_down) reader_close(st);
            st->lmb_was_down = lmb;
        }
        st->mouse_last_x = mx;
        st->mouse_last_y = my;
        return;
    }
```

(Replace the existing guard block body — it currently has only the `edit_handle` arm. Keep the surrounding `if (st->edit_handle != 0 || st->palette.open) { ... }` structure; just widen the condition and add the `else if` arm.)

- [ ] **Step 5: Build + verify typing**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: PASS. In-engine: open a codex, type → letters appear on the page with a caret; `w` types (doesn't walk); Backspace deletes; Enter starts a line; fill the page → typing stops at the last line; click or Esc closes. (Pages still don't persist — Task 4. Flipping pages — Task 3.)

- [ ] **Step 6: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
writable-books: append-only typing into the current page

reader_is_editing gates a keyboard-capturing edit mode like notes: on_char
appends to the current page (blocked at L-line capacity), on_key handles
Backspace/Enter/Esc, read_input suppresses movement and closes on click.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Flip pages one at a time + add a page past the end

**Files:** Modify `main.c` — `reader_edit_flip` (near the edit helpers); the `on_key` editing branch (Task 2).

- [ ] **Step 1: Add `reader_edit_flip`**

Below `reader_page_newline` (Task 2), add:

```c
/* single-page navigation while editing: move the caret one page; flipping past
   the last page appends a blank page; play the leaf turn only when the SPREAD
   changes (so left<->right within a spread just moves the caret). */
static void reader_edit_flip(AppState *st, int dir) {
    int old_spread = st->reader_spread;
    if (st->reader_turning != 0) return;       /* one leaf in flight */
    if (dir > 0) {
        if (st->reader_page + 1 >= st->reader_page_count) {
            char **grown = (char **)realloc(st->reader_pages,
                sizeof(char *) * (size_t)(st->reader_page_count + 1));
            if (!grown) return;
            st->reader_pages = grown;
            st->reader_pages[st->reader_page_count] = reader_strdup("");
            st->reader_page_count++;
        }
        st->reader_page++;
    } else {
        if (st->reader_page == 0) return;
        st->reader_page--;
    }
    st->reader_spread = st->reader_page / 2;
    if (st->reader_spread != old_spread) {
        st->reader_turn_old = old_spread;
        st->reader_turning  = (st->reader_spread > old_spread) ? 1 : -1;
        st->reader_turn_t   = 0.0f;
        play_oneshot(g_snd_whoosh, g_snd_whoosh_frames, 0.30f, 0.0f);
    }
    reader_pack_pages(st);
}
```

- [ ] **Step 2: Wire arrows in the `on_key` editing branch**

In the Task 2 `on_key` editing branch, add the two arrow cases before the closing `return;`:

```c
    if (reader_is_editing(st)) {
        if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
        if (key == GLFW_KEY_ESCAPE) { if (action == GLFW_PRESS) reader_close(st); }
        else if (key == GLFW_KEY_BACKSPACE) reader_page_backspace(st);
        else if (key == GLFW_KEY_ENTER)     reader_page_newline(st);
        else if (key == GLFW_KEY_RIGHT)     reader_edit_flip(st, +1);
        else if (key == GLFW_KEY_LEFT)      reader_edit_flip(st, -1);
        return;
    }
```

- [ ] **Step 3: Build + verify navigation**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: PASS. In-engine: type on page 0, `►` → caret moves to the right page (no leaf turn within a spread), `►` again → leaf turns to the next spread; type there; `◄` returns and earlier text is intact; `►` past the last page adds a fresh blank page.

- [ ] **Step 4: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
writable-books: single-page flip + add-page past the end

reader_edit_flip moves the caret one page (leaf-turn only across a spread
boundary); flipping past the last page appends a blank page. Wired to the
left/right arrows in the on_key editing branch.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Save pages on close

**Files:** Modify `main.c` — `reader_save_pages` (near the helpers); `reader_close` (`main.c:5728`).

- [ ] **Step 1: Add `reader_save_pages`**

Below `reader_load_pages` (Task 1), add:

```c
static void reader_save_pages(AppState *st) {
    char key[16], cbuf[16];
    int  i;
    if (!st->reader_editable || !st->reader_pages || st->reader_source == 0) return;
    snprintf(cbuf, sizeof cbuf, "%d", st->reader_page_count);
    scene_meta_set(&st->scene, st->reader_source, "pagecount", cbuf);
    for (i = 0; i < st->reader_page_count; i++) {
        snprintf(key, sizeof key, "page%d", i);
        scene_meta_set(&st->scene, st->reader_source, key,
                       st->reader_pages[i] ? st->reader_pages[i] : "");
    }
}
```

- [ ] **Step 2: Save + free in `reader_close`**

In `reader_close` (`main.c:5728`), after the early `return` guard and before `st->reader_a_pos = ...`, add:

```c
    if (st->reader_editable) {
        reader_save_pages(st);
        scene_save(&st->scene, "scene.stml");
        reader_free_pages(st);
        st->reader_editable = SOL_FALSE;
    }
```

- [ ] **Step 3: Build + verify persistence**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: PASS. In-engine: write across several pages (add a couple), Esc, **reload the scene** (or re-run), reopen the book → all pages and text are present. Confirm a file/alias card and the Skyrim book still open **read-only** (no caret, arrows still flip whole spreads, walking still works while reading).

- [ ] **Step 4: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
writable-books: persist pages to the codex meta on close

reader_close writes pagecount + page0..N back to the codex anchor's meta and
saves the scene, then frees the page array. Pages round-trip across reload.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 5: Part B live-verify checkpoint (hand to Fran)**

Hand Fran the full Part B manual checklist (mint→type→fill→flip→add page→Esc→reload→reopen; plus the read-only regression). Wait for confirmation before starting Part A.

---

# PART A — Shelve generated books

### Task 5: Carry a shelved book again

**Files:** Modify `main.c` — `carry_target` (`main.c:6628`).

- [ ] **Step 1: Let a codex anchor carry even when parented**

In `carry_target`, inside the final block, replace:

```c
        o = scene_get(&st->scene, target);
        if (!o || o->parent != 0) return 0;
        return target;
```

with:

```c
        o = scene_get(&st->scene, target);
        if (!o) return 0;
        if (codex_cover_child(&st->scene, target) != 0) return target;  /* a book
                                          carries even when shelved (like cards) */
        if (o->parent != 0) return 0;
        return target;
```

- [ ] **Step 2: Build**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: PASS. (Behaviour unchanged until Task 6 lets a book be filed; this just stops refusing a parented codex.)

- [ ] **Step 3: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
writable-books: carry_target accepts a parented codex (shelved book)

A codex anchor is carryable even when parented to furniture, the same
allowance cards already have, so a shelved book can be picked back up.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: File a carried book onto a shelf

**Files:** Modify `main.c` — the furniture-preview loop in `carry_update` (the block gated on the carried object being a `"card"`, ~`main.c:6932` after the recent carry edits) and, if needed, the `cmd_carry_toggle` `file_aim` drop branch (`main.c:6733`).

- [ ] **Step 1: Read the current furniture-preview loop**

Open `carry_update` and locate the block that begins (text may have drifted):

```c
    if (o->mesh_ref && strcmp(o->mesh_ref, "card") == 0 && o->kind != KIND_FOLDER) {
        sol_u32 fi;
        for (fi = 0; fi < st->scene.count; fi++) {
            ...
            if (furniture_is_shelf(f->mesh_ref)) {
                int idx = shelf_free_slot(st, f->handle, st->carried, ...);
                st->file_local = furniture_shelf_slot(f->mesh_params, ..., idx);
                st->file_rot   = quat_from_axis_angle(vec3_make(0,1,0), sol_radians(90.0f));
            } else {
                st->file_local = furniture_table_point(...);
                st->file_rot   = quat_mul(... place_yaw ..., ... -90 about x ...);
            }
            ... preview pose ...
            return;
        }
    }
```

- [ ] **Step 2: Broaden the gate to codex groups**

Change the guard so a carried **codex** also enters the loop. Replace the `if (o->mesh_ref && strcmp(o->mesh_ref, "card") == 0 && o->kind != KIND_FOLDER) {` line with:

```c
    {
        sol_bool carry_is_card  = (sol_bool)(o->mesh_ref &&
            strcmp(o->mesh_ref, "card") == 0 && o->kind != KIND_FOLDER);
        sol_bool carry_is_codex = (sol_bool)(codex_cover_child(&st->scene, st->carried) != 0);
        if (carry_is_card || carry_is_codex) {
```

and add one extra closing `}` after the loop's existing closing `}` (the outer brace just introduced). Keep `sol_u32 fi;` as the first declaration inside.

- [ ] **Step 3: Orient a shelved/tabled book**

Inside the loop, where `st->file_rot`/`st->file_local` are set, branch for a codex. After the existing shelf/table `if/else`, wrap the rotation choice so a codex stands spine-out on a shelf (upright) or lies on a table. Replace the shelf/table block with:

```c
            if (carry_is_codex) {
                float bh = mesh_ref_param("book_cover", (const float *)0, 0, "h");
                /* upright, spine toward the room. STARTING orientation — verify
                   spine-out in-engine and adjust the yaw term (+/-90) if the
                   fore-edge faces out instead. */
                st->file_rot = quat_mul(
                    quat_from_axis_angle(vec3_make(0.0f,1.0f,0.0f), fyaw + sol_radians(90.0f)),
                    quat_from_axis_angle(vec3_make(1.0f,0.0f,0.0f), sol_radians(-90.0f)));
                if (furniture_is_shelf(f->mesh_ref)) {
                    int idx = shelf_free_slot(st, f->handle, st->carried,
                                              f->mesh_params, f->mesh_param_count);
                    st->file_local = furniture_shelf_slot(f->mesh_params,
                                              f->mesh_param_count, idx);
                    st->file_local.y += bh * 0.5f;   /* base rests on the shelf board */
                } else {
                    st->file_local = furniture_table_point(f->mesh_params,
                                              f->mesh_param_count, loc);
                }
            } else if (furniture_is_shelf(f->mesh_ref)) {
                int idx = shelf_free_slot(st, f->handle, st->carried,
                                          f->mesh_params, f->mesh_param_count);
                st->file_local = furniture_shelf_slot(f->mesh_params, f->mesh_param_count, idx);
                st->file_rot   = quat_from_axis_angle(vec3_make(0.0f,1.0f,0.0f), sol_radians(90.0f));
            } else {
                st->file_local = furniture_table_point(f->mesh_params, f->mesh_param_count, loc);
                st->file_rot   = quat_mul(quat_from_axis_angle(vec3_make(0.0f,1.0f,0.0f), st->place_yaw),
                                          quat_from_axis_angle(vec3_make(1.0f,0.0f,0.0f), sol_radians(-90.0f)));
            }
```

Use the loop's existing `fyaw` (furniture yaw) and `loc` (table hit). The `furniture_is_shelf`/`furniture_table_point`/`shelf_free_slot` names are exactly as used in the card path.

- [ ] **Step 4: Confirm the drop re-parents the codex**

Read the `cmd_carry_toggle` `file_aim` drop branch (`main.c:6733`): `o->parent = st->file_target; o->pos = st->file_local; o->rot = st->file_rot;`. This is kind-agnostic and works for the codex anchor (children follow). No change expected; if it special-cases `"card"`, generalize it to the carried object. Note what you find.

- [ ] **Step 5: Build + live-verify (hand to Fran)**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: PASS. Hand Fran:
1. Mint a book (`V`), carry it (`E`), aim at a bookshelf → it previews at a slot; drop → it files **standing spine-out** (report if the cover/fore-edge faces out instead — the `fyaw +/- 90` term is the knob), base resting on the shelf board.
2. Pick it back up (`E`) → it detaches and carries; its pre-file rotation restores.
3. Reload → still filed. **Known refinement:** books are wider than card spines, so slots may crowd — note overlap; width-aware slot spacing is the follow-up flagged in the spec (don't ship silent overlap — report it).
4. Open a shelved book (`Enter`) → still editable (Part B) and saves in place.

- [ ] **Step 6: Commit (after Fran confirms)**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
writable-books: file a carried codex onto a shelf

carry_update's furniture-preview loop and the drop accept a carried codex
group, standing it spine-out on a shelf (base on the board) or lying it on a
table. Slot reuses shelf_free_slot; width-aware spacing is a flagged follow-up.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-review

- **Spec coverage:**
  - Part A shelve (carry_target, furniture loop+drop, orientation, slot) → Tasks 5–6 (slot width-awareness flagged for live-verify, per the spec's own caveat).
  - Part B storage (`meta page0..N`+`pagecount`) → Tasks 1 (load) + 4 (save).
  - Editable predicate (`cover != 0` → `reader_editable`) → Task 1 Step 3.
  - Page array packed into the reuse blob (cap+pad to L, caret) → Task 1 Step 2 (`reader_pack_pages`).
  - Append-only input, keyboard capture → Task 2.
  - Single-page flip + add-page → Task 3.
  - Save on close, read-only unchanged → Task 4.
- **Placeholder scan:** none — every code step is complete. The one empirical value (the codex shelf yaw `fyaw ± 90`) ships a concrete starting orientation with an explicit in-engine tuning callout (orientation is tuned visually throughout this project), not a blank.
- **Type/name consistency:** `reader_pages`/`reader_page_count`/`reader_page`/`reader_editable`; `reader_strdup`/`reader_free_pages`/`reader_load_pages`/`reader_save_pages`/`reader_field_w`/`reader_pack_pages`/`reader_page_full`/`reader_page_append`/`reader_page_backspace`/`reader_page_newline`/`reader_edit_flip`/`reader_is_editing` — all defined before first use (helpers sit above `reader_open`; `reader_edit_flip`/append/backspace live with them, all above `read_input`/`on_char`/`on_key`). `codex_cover_child`, `furniture_is_shelf`, `furniture_shelf_slot`, `shelf_free_slot`, `furniture_table_point`, `utf8_encode`, `g_snd_whoosh`, `scene_meta_set/get`, `scene_save` all match existing signatures. `reader_pack_pages` frees `reader_text`/`reader_line_off` inline (NOT `reader_free_text`) to preserve `reader_spread`.
- **Ordering:** Part B before Part A (B is fully deterministic and the headline; A is independent). Each task builds and is verifiable on its own; live-verify checkpoints at the end of each Part.
