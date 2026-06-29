# Entity Browser Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A ranger-style Miller-columns HUD that catalogs world entities by type (Pictures, Places) via a pluggable type-provider registry, with a live preview and a per-entity command list (v1 command: Place).

**Architecture:** A pure `browser` module owns the three-column navigation/filter state (headless-tested). A modal HUD in `main.c` — modeled on the palette + inventory bag — renders the columns, owns input via the same three early-return gates the palette uses, and drives a `TypeProvider` registry (`{ enumerate, commands, preview, run }`). Reuses the inventory thumbnail rig + `fuzzy.c` + the carry/`spawn_image_picture`/`spawn_map_board` paths. **No new shader → no MSL twin.**

**Tech Stack:** C89 engine (`browser.c` pure; `browser_test.c` c11), GLFW/GL + Metal RHI (untouched), the existing palette/inventory/ui/text/fuzzy/platform_fs systems.

**Spec:** `docs/superpowers/specs/2026-06-29-entity-browser-design.md`

**Key facts established by exploration (do not re-derive):**
- **Input ownership = three early-return gates:** top of `on_key` (palette gate at main.c:17230, inventory at 17246 — add browser after), top of `on_char` (main.c:17137), and the movement-suppression `if` in `read_input` (main.c:10896). Each is `if (modal_open) { ...; return; }`.
- **Open trigger pattern:** palette opens on Shift+Semicolon (`:`) at main.c:17287, gated on `edit_handle==0 && reader_state==READER_IDLE && board_view==0`. Plain `;` (`GLFW_KEY_SEMICOLON` without Shift) is FREE → the browser's open key.
- **Inventory thumbnail rig** (main.c:14931–15055): `inventory_thumb_render(st,item,dst)` frames a mesh 3/4 and tonemaps via `post_pipeline` with neutral inputs; `inventory_thumb_get` caches; `inventory_ensure_thumbs` builds current-page-only at frame top (main.c:15199). `draw_mesh(state,mesh,model,view,proj,eye,highlight,mat)` at main.c:14591. Scratch `inv_thumb_hdr`, neutral `inv_white_tex`/`inv_black_tex`. RT caps: `rhi_gl.c:26`=16, `rhi_metal.m:83`=48.
- **fuzzy:** `sol_bool fuzzy_match(const char *q, const char *cand, int *score, int *pos, int maxpos)` (fuzzy.h:14). `palette_rank` (palette.c:48) = match-filter + stable insertion sort by descending score.
- **Draw primitives:** `ui_quad`, `ui_quad_outline`, `ui_line`, `ui_textured_quad(RhiTexture,x,y,w,h)` (ui.h); `ui_text(font,utf8,x,y,scale,r,g,b,a)` (text.h:40), `text_measure[_cached]` (text.h:46/53), `font_ascent`/`font_line_height` (font.h:44). Overlays draw inside `ui_begin`/`ui_end` (main.c:16672/16916), alongside `palette_draw`/`inventory_draw_overlay` at main.c:16912. DPI scale `us = fb_h/1080.0f`.
- **Directory listing EXISTS:** `fs_scan_dir(path,&FsListing)` + `fs_listing_free` (platform_fs.h:26); `FsEntry{char*name; sol_bool is_dir; long size;}`. Precedent: `room_mirror_scan` (mirror.c:53). `reader_is_image_path(path)` (main.c:6781) accepts png/jpg/jpeg.
- **Pasted images:** `library/<nid>.png` via `library_write` (main.c:10773). Image objects: `o->content` + `reader_is_image_path`; `is_image_card(o)` (main.c:9044).
- **Global-anchor persistence:** `inventory_anchor_find`/`inventory_anchor` (main.c:8956/8971) — mesh-less root anchor tagged `meta["inventory"]`, found-or-created, children persist in scene.stml. Mirror for Places with tag `meta["places"]`, child records carrying `name/lat/lon/zoom/basemap` meta.
- **Place actions:** `spawn_image_picture(st,parent,pos,rot,content)` (main.c:9090); carry via `st->carried` + `cmd_carry_toggle` (main.c:9118, key E); `inventory_take` (main.c:9055) is the "take a stamp onto the cursor" precedent. `spawn_map_board(st,lat,lon,z,style)` (main.c:13238) stores lat/lon/zoom/basemap meta with identical encodings.
- `scene_meta_get/set` (scene.h:133-134), `scene_add`, `carry_place_point`, `mint_tag_ws`, `scene_save(&scene,"scene.stml")`.

**v1 interaction decision (ranger-faithful):** a `filtering` mode flag distinguishes two modes. **Nav mode** (default): `h/j/k/l` AND arrows navigate (h/←,l/→ change focused column; k/↑,j/↓ move row), `Enter` descends Types→Entities→Commands / activates on the Commands column, `/` enters filter mode for the focused column, `Esc` closes. **Filter mode**: printable keys append to the focused column's filter (fuzzy-narrowed), `Backspace` trims, ↑/↓ still move the selection, `Enter` commits the filter + returns to nav, `Esc` cancels the filter + returns to nav. (So letter keys are nav keys in nav mode and filter text in filter mode — the shell routes them by `browser.filtering`.)

**C89 gotchas:** declarations at top of block; no C99/C11 in engine `.c` (`browser.c`); `browser_test.c` may be c11; commit messages end with the `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>` line; `git add` only the named files (never NOTES.stml / paper-picture.png / library/ / scratch).

---

## File Structure
- **Create** `browser.h` / `browser.c` — pure Miller-columns nav/filter state + rank (depends only on `fuzzy.h`).
- **Create** `browser_test.c` — headless test.
- **Modify** `build.sh` — `browsertest` target + `browser.c` into the 4 source lists.
- **Modify** `main.c` — `AppState` fields, the 3 input gates + open key, the column render, the `TypeProvider` registry, the preview wiring, and the two providers.

---

## Task 1: `browser` pure module + headless test

**Files:** Create `browser.h`, `browser.c`, `browser_test.c`.

- [ ] **Step 1: Write `browser.h`**
```c
#ifndef BROWSER_H
#define BROWSER_H
/* browser.h — pure Miller-columns navigation state for the entity browser HUD.
   No GL, no scene, no engine deps (only fuzzy.h, also pure). The shell feeds
   per-column row counts; this owns focus, per-column selection, and per-column
   filter strings, plus a fuzzy rank helper. */
#include "sol_base.h"

#define BROWSER_COLS       3      /* 0 = Types, 1 = Entities, 2 = Commands */
#define BROWSER_FILTER_CAP 64

typedef enum {
    BROWSER_KEY_NONE = 0,
    BROWSER_KEY_LEFT, BROWSER_KEY_RIGHT,   /* change focused column */
    BROWSER_KEY_UP,   BROWSER_KEY_DOWN,    /* move selection in focused column */
    BROWSER_KEY_ENTER,                     /* nav: descend/activate; filter: commit + leave */
    BROWSER_KEY_BACKSPACE,                 /* filter: trim */
    BROWSER_KEY_FILTER,                    /* nav: '/' -> enter filter mode */
    BROWSER_KEY_CANCEL                     /* nav: close; filter: cancel filter + leave */
} BrowserKey;

typedef enum { BROWSER_NONE = 0, BROWSER_ACTIVATE, BROWSER_CLOSE } BrowserAction;

typedef struct {
    int      focus;                                   /* 0..BROWSER_COLS-1 */
    int      sel[BROWSER_COLS];                        /* selected row per column */
    char     filter[BROWSER_COLS][BROWSER_FILTER_CAP];
    int      flen[BROWSER_COLS];
    sol_bool filtering;                               /* true = typing edits the focused filter */
} Browser;

void          browser_reset(Browser *b);
/* clamp every selection into [0, counts[col]-1] (0 when empty). Call after the
   visible row counts change (filter edit, type/entity switch). */
void          browser_clamp(Browser *b, const int counts[BROWSER_COLS]);
/* one nav/edit key; counts = current visible row counts per column. */
BrowserAction browser_key(Browser *b, BrowserKey k, const int counts[BROWSER_COLS]);
/* append one printable char to the focused column's filter (and reset its sel). */
void          browser_char(Browser *b, char c);
/* fuzzy-rank `names` (n of them) against `filter`: write winning indices into
   out[<=cap], return match count. Mirrors palette_rank (stable, score-desc). */
int           browser_rank(const char *filter, const char *const *names, int n,
                           int *out, int cap);
#endif /* BROWSER_H */
```

- [ ] **Step 2: Write the failing test `browser_test.c`** (CHECK-macro harness like `caret_test.c`)
```c
/* browser_test.c — pure-logic test for browser.c. GL-free; build.sh browsertest. */
#include "browser.h"
#include <string.h>
#include <stdio.h>

static int fails = 0;
#define CHECK(c,m) do { if(!(c)){ printf("FAIL %s:%d: %s\n",__FILE__,__LINE__,m); fails++; } } while(0)

static void test_focus_clamps(void) {
    Browser b; int counts[3]; counts[0]=2; counts[1]=3; counts[2]=1;
    browser_reset(&b);
    CHECK(b.focus==0, "starts on Types");
    browser_key(&b, BROWSER_KEY_LEFT, counts);   CHECK(b.focus==0, "left clamps at 0");
    browser_key(&b, BROWSER_KEY_RIGHT, counts);  CHECK(b.focus==1, "right -> Entities");
    browser_key(&b, BROWSER_KEY_RIGHT, counts);  CHECK(b.focus==2, "right -> Commands");
    browser_key(&b, BROWSER_KEY_RIGHT, counts);  CHECK(b.focus==2, "right clamps at last col");
}

static void test_select_moves(void) {
    Browser b; int counts[3]; counts[0]=2; counts[1]=3; counts[2]=1;
    browser_reset(&b);
    browser_key(&b, BROWSER_KEY_RIGHT, counts);  /* Entities, count 3 */
    browser_key(&b, BROWSER_KEY_DOWN, counts);   CHECK(b.sel[1]==1, "down");
    browser_key(&b, BROWSER_KEY_DOWN, counts);   CHECK(b.sel[1]==2, "down");
    browser_key(&b, BROWSER_KEY_DOWN, counts);   CHECK(b.sel[1]==2, "down clamps at count-1");
    browser_key(&b, BROWSER_KEY_UP, counts);
    browser_key(&b, BROWSER_KEY_UP, counts);
    browser_key(&b, BROWSER_KEY_UP, counts);     CHECK(b.sel[1]==0, "up clamps at 0");
}

static void test_empty_column(void) {
    Browser b; int counts[3]; counts[0]=2; counts[1]=0; counts[2]=0;
    browser_reset(&b);
    browser_key(&b, BROWSER_KEY_RIGHT, counts);
    browser_key(&b, BROWSER_KEY_DOWN, counts);   CHECK(b.sel[1]==0, "down on empty col stays 0");
}

static void test_enter_descends_then_activates(void) {
    Browser b; int counts[3]; counts[0]=2; counts[1]=3; counts[2]=1;
    BrowserAction a;
    browser_reset(&b);
    a = browser_key(&b, BROWSER_KEY_ENTER, counts); CHECK(a==BROWSER_NONE && b.focus==1, "enter Types->Entities");
    a = browser_key(&b, BROWSER_KEY_ENTER, counts); CHECK(a==BROWSER_NONE && b.focus==2, "enter Entities->Commands");
    a = browser_key(&b, BROWSER_KEY_ENTER, counts); CHECK(a==BROWSER_ACTIVATE, "enter on Commands activates");
}

static void test_cancel(void) {
    Browser b; int counts[3]; counts[0]=1; counts[1]=1; counts[2]=1;
    browser_reset(&b);
    CHECK(browser_key(&b, BROWSER_KEY_CANCEL, counts)==BROWSER_CLOSE, "esc closes");
}

static void test_filter_mode(void) {
    Browser b; int counts[3]; counts[0]=2; counts[1]=3; counts[2]=1;
    browser_reset(&b);
    browser_key(&b, BROWSER_KEY_RIGHT, counts);    /* focus Entities */
    browser_char(&b, 'a');
    CHECK(b.flen[1]==0, "letters don't filter in nav mode");
    browser_key(&b, BROWSER_KEY_FILTER, counts);   /* '/' enters filter mode */
    CHECK(b.filtering, "slash enters filter mode");
    browser_char(&b, 'a'); browser_char(&b, 'b');
    CHECK(b.flen[1]==2 && strcmp(b.filter[1],"ab")==0, "chars append while filtering");
    browser_key(&b, BROWSER_KEY_BACKSPACE, counts);
    CHECK(b.flen[1]==1 && strcmp(b.filter[1],"a")==0, "backspace trims");
    CHECK(browser_key(&b, BROWSER_KEY_ENTER, counts)==BROWSER_NONE && !b.filtering
          && strcmp(b.filter[1],"a")==0, "enter commits filter + leaves nav");
    browser_key(&b, BROWSER_KEY_FILTER, counts);
    browser_char(&b, 'x');
    CHECK(browser_key(&b, BROWSER_KEY_CANCEL, counts)==BROWSER_NONE && !b.filtering
          && b.flen[1]==0, "esc cancels filter (does NOT close) + clears");
}

static void test_clamp_after_shrink(void) {
    Browser b; int counts[3]; counts[0]=2; counts[1]=5; counts[2]=1;
    browser_reset(&b);
    browser_key(&b, BROWSER_KEY_RIGHT, counts);
    b.sel[1] = 4;
    counts[1] = 2;                 /* filter shrank the list */
    browser_clamp(&b, counts);
    CHECK(b.sel[1]==1, "selection clamps after shrink");
}

static void test_rank(void) {
    const char *names[3]; int out[3], n;
    names[0]="sunset"; names[1]="map-clip"; names[2]="paper";
    n = browser_rank("", names, 3, out, 3);          CHECK(n==3, "empty filter = all");
    n = browser_rank("map", names, 3, out, 3);       CHECK(n==1 && out[0]==1, "filter narrows + indexes");
    n = browser_rank("zzz", names, 3, out, 3);       CHECK(n==0, "no match");
}

int main(void) {
    test_focus_clamps(); test_select_moves(); test_empty_column();
    test_enter_descends_then_activates(); test_cancel();
    test_filter_mode(); test_clamp_after_shrink(); test_rank();
    if (fails==0) printf("browser_test: all passed\n");
    return fails ? 1 : 0;
}
```

- [ ] **Step 3: Verify it does NOT yet build (no browser.c).** Run:
`clang -std=c11 -g -fsanitize=address,undefined browser.c browser_test.c fuzzy.c -o /tmp/bt 2>&1 | head`
Expected: error (browser.c missing).

- [ ] **Step 4: Write `browser.c`**
```c
#include "browser.h"
#include "fuzzy.h"
#include <string.h>

void browser_reset(Browser *b) {
    int i;
    b->focus = 0; b->filtering = SOL_FALSE;
    for (i = 0; i < BROWSER_COLS; i++) { b->sel[i] = 0; b->filter[i][0] = '\0'; b->flen[i] = 0; }
}

void browser_clamp(Browser *b, const int counts[BROWSER_COLS]) {
    int i;
    for (i = 0; i < BROWSER_COLS; i++) {
        if (b->sel[i] < 0) b->sel[i] = 0;
        if (counts[i] <= 0) b->sel[i] = 0;
        else if (b->sel[i] > counts[i] - 1) b->sel[i] = counts[i] - 1;
    }
}

BrowserAction browser_key(Browser *b, BrowserKey k, const int counts[BROWSER_COLS]) {
    int f = b->focus;
    if (b->filtering) {                          /* filter mode */
        switch (k) {
        case BROWSER_KEY_CANCEL:                 /* cancel the filter, back to nav */
            b->filtering = SOL_FALSE;
            b->filter[f][0] = '\0'; b->flen[f] = 0; b->sel[f] = 0; break;
        case BROWSER_KEY_ENTER:                  /* commit the filter, back to nav */
            b->filtering = SOL_FALSE; break;
        case BROWSER_KEY_BACKSPACE:
            if (b->flen[f] > 0) { b->flen[f]--; b->filter[f][b->flen[f]] = '\0'; b->sel[f] = 0; }
            break;
        case BROWSER_KEY_UP:   if (b->sel[f] > 0) b->sel[f]--; break;
        case BROWSER_KEY_DOWN: if (counts[f] > 0 && b->sel[f] < counts[f] - 1) b->sel[f]++; break;
        default: break;                          /* LEFT/RIGHT/FILTER ignored while filtering */
        }
        browser_clamp(b, counts);
        return BROWSER_NONE;
    }
    switch (k) {                                 /* nav mode */
    case BROWSER_KEY_CANCEL: return BROWSER_CLOSE;
    case BROWSER_KEY_LEFT:   if (b->focus > 0) b->focus--; break;
    case BROWSER_KEY_RIGHT:  if (b->focus < BROWSER_COLS - 1) b->focus++; break;
    case BROWSER_KEY_UP:     if (b->sel[f] > 0) b->sel[f]--; break;
    case BROWSER_KEY_DOWN:   if (counts[f] > 0 && b->sel[f] < counts[f] - 1) b->sel[f]++; break;
    case BROWSER_KEY_ENTER:
        if (b->focus < BROWSER_COLS - 1) { b->focus++; break; }
        return BROWSER_ACTIVATE;                 /* Enter on Commands */
    case BROWSER_KEY_FILTER: b->filtering = SOL_TRUE; break;   /* '/' */
    default: break;
    }
    browser_clamp(b, counts);
    return BROWSER_NONE;
}

void browser_char(Browser *b, char c) {
    int f = b->focus;
    if (!b->filtering) return;                   /* letters are nav keys unless filtering */
    if (c < 0x20 || c > 0x7e) return;            /* printable ASCII only */
    if (b->flen[f] >= BROWSER_FILTER_CAP - 1) return;
    b->filter[f][b->flen[f]++] = c;
    b->filter[f][b->flen[f]] = '\0';
    b->sel[f] = 0;
}

int browser_rank(const char *filter, const char *const *names, int n, int *out, int cap) {
    int score[256];
    int i, j, cnt = 0;
    for (i = 0; i < n && cnt < cap && cnt < 256; i++) {
        int sc;
        if (fuzzy_match(filter, names[i], &sc, (int *)0, 0)) { out[cnt] = i; score[cnt] = sc; cnt++; }
    }
    for (i = 1; i < cnt; i++) {                  /* stable insertion sort, score desc */
        int ti = out[i], ts = score[i];
        j = i - 1;
        while (j >= 0 && score[j] < ts) { out[j+1] = out[j]; score[j+1] = score[j]; j--; }
        out[j+1] = ti; score[j+1] = ts;
    }
    return cnt;
}
```
*(Note: `out`/`score` capped at 256 candidates per column — far above any real type's list; document the cap.)*

- [ ] **Step 5: Build & run.** Run:
`clang -std=c11 -g -fsanitize=address,undefined browser.c browser_test.c fuzzy.c -o /tmp/bt && /tmp/bt`
Expected: `browser_test: all passed`, exit 0, no sanitizer output.

- [ ] **Step 6: C89-clean check.** Run:
`clang -std=c89 -pedantic-errors -Werror -Wall -Wextra -fsyntax-only browser.c`
Expected: no output.

- [ ] **Step 7: Commit** `browser.h browser.c browser_test.c` with the Co-Authored-By trailer.

---

## Task 2: Wire `browser` into build.sh

**Files:** Modify `build.sh`.

- [ ] **Step 1:** Add a `browsertest` target next to `carettest`/`mapmathtest`:
```bash
if [ "$MODE" = "browsertest" ]; then
    set -x
    clang -std=c89 -pedantic-errors -Werror -g -fsanitize=address,undefined \
        browser.c browser_test.c fuzzy.c \
        -o browser_test
    echo "built ./browser_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi
```
- [ ] **Step 2:** Insert `browser.c ` into the four engine source lists. Anchor on `fuzzy.c ` (present in all four): change `fuzzy.c palette.c` → `fuzzy.c browser.c palette.c` (replace_all). Verify with `grep -c 'fuzzy.c browser.c palette.c' build.sh` (expect 4) and that no bare `fuzzy.c palette.c` remains on a source-list line.
- [ ] **Step 3:** `./build.sh browsertest && ./browser_test` → all passed.
- [ ] **Step 4:** `./build.sh c89check` → PASS (now lints browser.c).
- [ ] **Step 5:** Commit `build.sh`.

---

## Task 3: HUD shell — struct, input gates, open key, stub columns

**Files:** Modify `main.c`. Goal: open with `;`, own input, render three columns from STUB data, Esc closes — before any provider exists.

- [ ] **Step 1:** Add to `AppState` (near `Palette palette;` ~main.c:2864): `#include "browser.h"` at the top with the other headers; then
```c
    sol_bool browser_open;
    Browser  browser;
```
- [ ] **Step 2: Open key.** Next to the palette open (main.c:17287), add (plain `;`, no Shift, same idle guards + not while palette/inv open):
```c
    if (action == GLFW_PRESS && key == GLFW_KEY_SEMICOLON && !(mods & GLFW_MOD_SHIFT)
        && st->edit_handle == 0 && st->reader_state == READER_IDLE && st->board_view == 0
        && !st->palette.open && !st->inv_open) {
        browser_reset(&st->browser);
        st->browser_open = SOL_TRUE;
        return;
    }
```
- [ ] **Step 3: Keyboard gate** at the top of `on_key`, AFTER the inventory gate (main.c:17258). It maps arrows/enter/backspace/esc to `BrowserKey`, calls a helper `browser_handle_key(st, pk)` (Step 5), and returns:
```c
    if (st->browser_open) {
        if (action == GLFW_PRESS || action == GLFW_REPEAT) {
            BrowserKey pk = BROWSER_KEY_NONE;
            if      (key == GLFW_KEY_ESCAPE)    pk = BROWSER_KEY_CANCEL;
            else if (key == GLFW_KEY_ENTER ||
                     key == GLFW_KEY_KP_ENTER)  pk = BROWSER_KEY_ENTER;
            else if (key == GLFW_KEY_BACKSPACE) pk = BROWSER_KEY_BACKSPACE;
            else if (key == GLFW_KEY_UP)        pk = BROWSER_KEY_UP;
            else if (key == GLFW_KEY_DOWN)      pk = BROWSER_KEY_DOWN;
            else if (!st->browser.filtering) {  /* nav-only keys; in filter mode letters reach on_char */
                if      (key == GLFW_KEY_LEFT  || key == GLFW_KEY_H) pk = BROWSER_KEY_LEFT;
                else if (key == GLFW_KEY_RIGHT || key == GLFW_KEY_L) pk = BROWSER_KEY_RIGHT;
                else if (key == GLFW_KEY_K)     pk = BROWSER_KEY_UP;
                else if (key == GLFW_KEY_J)     pk = BROWSER_KEY_DOWN;
                else if (key == GLFW_KEY_SLASH) pk = BROWSER_KEY_FILTER;
            }
            if (pk != BROWSER_KEY_NONE) browser_handle_key(st, pk);
        }
        return;
    }
```
- [ ] **Step 4: Char gate** at the top of `on_char` (after the palette gate, main.c:17137):
```c
    if (st->browser_open) { if (st->browser.filtering) browser_char(&st->browser, (char)cp); return; }
```
- [ ] **Step 5: Movement-suppression clause.** Add `|| st->browser_open` to the combined `if` at main.c:10896 (so world movement/look is frozen while open). The browser is keyboard-only (no cursor release needed — do NOT mirror the inventory cursor toggle).
- [ ] **Step 6: The counts/columns helpers + key handler.** Add (above `on_key`):
  - `static int browser_type_count(void);` returns 2 (stub: "Pictures","Places"). A `static const char *BROWSER_TYPES[2] = {"Pictures","Places"};`
  - `static void browser_columns_counts(AppState *st, int counts[3]);` — STUB v1: `counts[0]=2; counts[1]=3; counts[2]=1;` (real provider counts arrive in Task 4).
  - `static void browser_handle_key(AppState *st, BrowserKey pk)`:
```c
static void browser_handle_key(AppState *st, BrowserKey pk) {
    int counts[3];
    BrowserAction a;
    browser_columns_counts(st, counts);
    a = browser_key(&st->browser, pk, counts);
    if (a == BROWSER_CLOSE) { st->browser_open = SOL_FALSE; return; }
    if (a == BROWSER_ACTIVATE) {
        /* Task 4 wires provider->run here */
        st->browser_open = SOL_FALSE;
    }
}
```
- [ ] **Step 7: Render.** Add `static void browser_draw_overlay(AppState *st, int fb_w, int fb_h)` and call it inside the `ui_begin`/`ui_end` block next to `palette_draw`/`inventory_draw_overlay` (main.c:16912), guarded `if (state->browser_open)`. v1 draws three vertical panels (split `fb_w` into thirds with margins), each a `ui_quad` backdrop + `ui_quad_outline`, separated by `ui_line`; mirror the palette row loop (palette.c:171-193) per column: for each visible row draw the label via `ui_text`, and for the **focused** column draw a `ui_quad` highlight behind `sel[col]` (a dimmer highlight for the other columns' retained selection). Column 0 rows = `BROWSER_TYPES`; columns 1/2 = stub strings (`"entity 0/1/2"`, `"Place"`). Use `us = fb_h/1080.0f` scaling like palette.c:129. (Real content in Tasks 4–6.)
- [ ] **Step 8: Build** (`./build.sh c89check && ./build.sh && ./build.sh metal`). **Step 9: Commit** `main.c`. (Live-verify later: `;` opens a 3-column overlay, `hjkl`/arrows move/change column, Enter walks columns, `/` toggles filter mode, Esc closes, world input frozen while open.)

---

## Task 4: `TypeProvider` registry + real columns + preview + run

**Files:** Modify `main.c`.

- [ ] **Step 1: The provider interface + item type** (above the providers):
```c
#define BROWSER_NAME_CAP 64
#define BROWSER_REF_CAP  256
typedef struct { char name[BROWSER_NAME_CAP]; char ref[BROWSER_REF_CAP]; } BrowserItem;

typedef struct {
    const char *name;                                              /* "Pictures" */
    int        (*enumerate)(AppState *st, BrowserItem *out, int cap);
    int        (*commands)(AppState *st, const char *ref, const char **out, int cap);
    void       (*run)(AppState *st, const char *ref, int cmd);
    RhiTexture (*preview)(AppState *st, const char *ref);          /* texture to blit; id 0 = none */
} TypeProvider;

static TypeProvider g_providers[/* 2 after Tasks 5-6 */];
static int          g_provider_count;
```
- [ ] **Step 2: A per-open cache** on AppState so enumerate isn't called per-frame:
```c
    BrowserItem browser_items[256];   int browser_item_count;   int browser_items_type; /* which type cached */
    const char *browser_cmds[16];     int browser_cmd_count;
    int         browser_type_order[8], browser_ent_order[256], browser_cmd_order[16];
    int         browser_type_n, browser_ent_n, browser_cmd_n;   /* ranked visible counts */
    RhiTexture  browser_preview_tex;  char browser_preview_ref[BROWSER_REF_CAP]; /* prepared at frame top */
```
- [ ] **Step 3:** Replace the Task-3 stubs with real logic. A `static void browser_refresh(AppState *st)` (called after every key + on open) that: ranks the type names (g_providers[i].name) by `filter[0]` → `browser_type_order`/`browser_type_n`; resolves the selected type (the provider at `browser_type_order[sel[0]]`); enumerates its items into `browser_items` (only when the selected type changed — cache via `browser_items_type`); ranks item names by `filter[1]` → `browser_ent_order`/`browser_ent_n`; gets the selected item's commands → ranks by `filter[2]`. `browser_columns_counts` returns `{browser_type_n, browser_ent_n, browser_cmd_n}`.
- [ ] **Step 4:** `browser_handle_key`: call `browser_refresh` after `browser_key`; on `BROWSER_ACTIVATE`, resolve provider+ref+cmd from the ranked orders and call `provider->run(st, ref, cmd_index)`, then close.
- [ ] **Step 5: Preview — prepared at FRAME TOP, blitted in draw** (the inventory rig's discipline: never render a pass mid-UI-batch). Add `static void browser_ensure_preview(AppState *st)` and call `if (state->browser_open) browser_ensure_preview(state);` next to `inventory_ensure_thumbs` (main.c:15199). `browser_ensure_preview`: resolve the highlighted entity's `ref`; if it differs from `st->browser_preview_ref`, call the selected provider's `preview(st, ref)` (Pictures: `load_texture` — a cached upload; Places: render the map quad to a cached RT, Task 6) and store the returned `RhiTexture` in `st->browser_preview_tex` + the ref in `browser_preview_ref`. Then in `browser_draw_overlay`, the preview box just blits `st->browser_preview_tex` via `ui_textured_quad` (letterboxed) if `tex.id`, with the command list (column 2) beneath. **No GPU work in draw.** Keep ≤2 live RTs total (watch the 16/48 caps); free the Places RT when the ref changes.
- [ ] **Step 6:** Build (c89check + gl + metal), commit. (Providers added next; until then `g_provider_count=0` → empty columns; keep the array sized for 2.)

---

## Task 5: Pictures provider

**Files:** Modify `main.c`.

- [ ] **Step 1: enumerate** — `static int pictures_enumerate(AppState *st, BrowserItem *out, int cap)`:
  - `fs_scan_dir("library", &l)`; for each `l.entries[i]` where `!is_dir && reader_is_image_path(name)`, add an item: `ref = "library/<name>"`, `name = <name>` (basename). `fs_listing_free(&l)`.
  - Then scan `st->scene.objects`: for any `o->content && reader_is_image_path(o->content)`, add its `content` path if not already present (dedupe by ref string). (Optionally include `paper-picture.png` if present via `fs_exists`.)
  - Return count (≤ cap).
- [ ] **Step 2: preview** — `static RhiTexture pictures_preview(AppState *st, const char *ref)` = `load_texture(ref)` (cached sRGB; the actual image). (No RT needed.)
- [ ] **Step 3: commands** — returns one: `out[0] = "Place"; return 1;`.
- [ ] **Step 4: run** — `pictures_run(st, ref, cmd)`: for cmd 0 (Place), take a fresh image card onto the cursor for hanging. Mirror `inventory_take`'s image-card path (main.c:9055): create a `"card"` object (KIND_PLAIN) with `content = ref` at `carry_place_point(st)`, set `st->carried`, `st->carry_prev_parent` (a sensible home — e.g. the inventory anchor so it returns there after hanging, matching the reusable-stamp rule), `carry_prev_rot = identity`, workspace-tag it, and set the picture-aim state so E hangs it via `cmd_carry_toggle`'s picture branch. Close the browser (already closed by handle_key). *(Implementer: reuse the exact carried-card setup `inventory_take` performs for an image card; the difference is the source is a library path, not an existing bag item.)*
- [ ] **Step 5:** Register `{ "Pictures", pictures_enumerate, pictures_commands, pictures_run, pictures_preview }` as `g_providers[0]`; set `g_provider_count` to include it.
- [ ] **Step 6:** Build (c89check + gl + metal), commit. (Live-verify: `;` → Pictures → an image highlights → its real image previews on the right → Place → you carry a card → E hangs it.)

---

## Task 6: Places provider (seeded saved-locations catalog)

**Files:** Modify `main.c`.

- [ ] **Step 1: The catalog anchor** — mirror `inventory_anchor` (main.c:8956/8971): `static sol_u32 places_anchor_find(AppState*)` (scan for `meta["places"]`) and `static sol_u32 places_anchor(AppState*)` (find-or-create a mesh-less root anchor tagged `meta["places"]="1"`). On create, **seed** ~8–10 cities: for each `{name,lat,lon,zoom,basemap}`, `scene_add` a mesh-less child under the anchor and `scene_meta_set` the five keys (lat/lon as `"%.6f"`, zoom as `"%d"`, under the `-Wdeprecated-declarations` sprintf guard). Seed list e.g. London 51.51,-0.13,5; Paris 48.85,2.35,5; New York 40.71,-74.01,5; Tokyo 35.68,139.69,6; Cairo 30.04,31.24,5; Sydney -33.87,151.21,5; Rio -22.91,-43.17,5; Reykjavík 64.15,-21.94,5. (basemap "relief".)
- [ ] **Step 2: enumerate** — `places_enumerate`: ensure the anchor (creates+seeds first time), then for each child `o->parent == anchor`, emit an item: `name = meta["name"]`, `ref = "<handle>"` (the child handle as decimal, via the sprintf guard). Return count.
- [ ] **Step 3: commands** — `out[0]="Place"; return 1;`.
- [ ] **Step 4: run** — `places_run(st, ref, cmd)`: parse the handle from `ref`, read `lat/lon/zoom/basemap` meta off that record, call `spawn_map_board(st, lat, lon, zoom, basemap)`, then `scene_save(&st->scene, "scene.stml")`.
- [ ] **Step 5: preview** — render a small map-board thumbnail to a cached RT and return its texture. Build a temporary map quad via `make_map_quad` with UVs from `mapmath_window(lon,lat,zoom, MAP_BOARD_W/MAP_BOARD_H, ...)`, material `albedo_tex = load_texture(basemap_path(basemap))`; render head-on into an RT via the `inventory_thumb_render` path (a flat quad, framed front-on) and return `rhi_render_target_texture(rt)`. Driven by `browser_ensure_preview` (Task 4 Step 5), highlighted-entity-only (one RT), freed/rebuilt on highlight change. If the basemap is missing (id 0), return id 0 (the shell shows the name only).
- [ ] **Step 6:** Register `{ "Places", places_enumerate, places_commands, places_run, places_preview }` as `g_providers[1]`; bump `g_provider_count` to 2.
- [ ] **Step 7:** Build (c89check + gl + metal + asan), commit. (Live-verify: `;` → Places → a city highlights → a 3D map thumbnail previews → Place → a map board spawns at that location; the catalog persists across relaunch.)

---

## Final verification (whole feature)
- [ ] `./build.sh c89check` → PASS; `./build.sh browsertest && ./browser_test` → all passed.
- [ ] `./build.sh`, `./build.sh metal`, `./build.sh asan` → all build.
- [ ] **Human live-verify:** `;` opens the Miller-columns browser; `hjkl`/arrows + Enter walk Types→Entities→Commands; `/` enters filter mode (type to narrow, Enter commits / Esc cancels the filter); Esc in nav closes — world input frozen while open, no leaked state. Pictures: real-image preview + Place (carry → hang). Places: 3D map thumbnail preview + Place (spawn map) + catalog persists across relaunch.

**Live-verify watch-items:** input-ownership (no world hotkey leaks while open; all state resets on close); the preview RT budget (≤2 live targets; watch the 16/48 caps); enumerate isn't called per-frame (cache by selected type); Pictures dedupe (a placed image + its library file shouldn't double-list).

**Deferred (own later specs):** hjkl-as-nav, Disk/filesystem provider, Rooms + Go-to, more commands (Delete/Rename/Save-to-Places), STML-defined types, map pins fed by the Places catalog.
