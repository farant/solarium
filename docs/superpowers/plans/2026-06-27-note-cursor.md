# Moveable Insertion Caret for Notes — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give board notes a moveable insertion caret — a byte offset into the note's text that you move with arrow keys (and, in board view, by clicking), with typing/Backspace/Delete/Enter acting AT the caret instead of at the end.

**Architecture:** A pure, GL-free `caret.c` does the risky layout math (source↔wrapped reconciliation + slot/line assembly + slot search) and is unit-tested. A `static caret_build` in `main.c` is the only font-bound glue (calls the unchanged `text_wrap`, gathers per-char advances, hands them to the pure assembler). `text_shape`/`wtext_block` (the §1.6 render seam) are untouched; the caret rides alongside the note text as a thin quad.

**Tech Stack:** C89 core (`caret.c`, `main.c`); C11 test (`caret_test.c` via `build.sh carettest`, ASan/UBSan, GL-free). Spec: `docs/superpowers/specs/2026-06-27-note-cursor-design.md`.

**House rules:** strict C89 for `caret.c`/`main.c` (declarations at block top, no `//`, no mid-block decls; `-std=c89 -pedantic-errors -Werror -Wextra`). NEVER `git add NOTES.stml` or `paper-picture.png`. Commit bodies end with `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. Work on a feature branch off `main`.

**Verified anchors (current code):**
- `on_char` (main.c:16513) appends `enc` at `edit_len`.
- The edit-mode key block `if (st->edit_handle != 0) {` (main.c:16624) handles Esc/Backspace/Enter then `return;` — arrows are free.
- `note_edit_begin` (main.c:16460) seeds `edit_buf`/`edit_len`/`edit_handle`.
- The note-body render appends a `_` (main.c:15654) and draws via `wtext_block(uf, vp, face, txt, -cw*0.5+2*margin, ch-2*margin, bpx2m, usable, …)` (main.c:15660); `bpx2m = note_text_size(scene,h)/lh`, `lh = font_line_height(uf)`.
- The blur path: `if (st->edit_handle != 0) { … if (lmb && !lmb_was_down) note_edit_end(st); }` (main.c:16624… and the modal-gate copy at main.c:16808).
- AppState edit fields at main.c:2846-2848; `resize_handle_mesh` (cached-on-first-use quad) at main.c:2801 / 15800.
- `make_page(b,w,h)` = XY quad centered at origin, +Z (mesh.c:121). `make_card`: x∈[−w/2,w/2], y∈[0,h] (mesh.c:357).
- Font API: `font_glyph`, `font_ascent`, `font_line_height`, `font_kern`, `FontGlyph.advance` (font.h). `utf8_encode` (main.c:16484). `text_wrap(f,utf8,scale,max_width,out,cap)` (text.h:48). `pick_ray(st,w)` (main.c:3149), `ray_vs_plane`, `mat4_mul_point` (sol_math.h:67).

---

## Task 1: `caret.c` / `caret.h` (pure) + `caret_test`

**Files:**
- Create: `caret.h`, `caret.c`, `caret_test.c`
- Modify: `build.sh` (add `carettest` target; add `caret.c` to the `c89check` `-fsyntax-only` source list)

- [ ] **Step 1: Write `caret.h`**

```c
#ifndef CARET_H
#define CARET_H

/* Pure (GL-free) caret layout for note text editing. caret_build (font-bound)
   lives in main.c and feeds these the wrapped string + per-char advances. */

#define CARET_MAX_SLOTS 2304   /* >= EDIT_BUF_CAP (2048) + per-line leading slots */
#define CARET_MAX_LINES 256

typedef struct { int src; float x; } CaretSlot;        /* caret position: source byte + note-local x */
typedef struct { int slot0, nslots, line; } CaretLine; /* slots[slot0 .. slot0+nslots) */
typedef struct {
    CaretSlot slots[CARET_MAX_SLOTS];
    int       slot_count;
    CaretLine lines[CARET_MAX_LINES];
    int       line_count;
    float     line_h;                  /* note-local line height */
} CaretField;

/* byte length of the UTF-8 codepoint whose lead byte is `lead` (1..4; 1 on a
   stray continuation byte, so a walk always makes progress). */
int caret_cplen(unsigned char lead);

/* source<->wrapped reconciliation. text_wrap only INSERTS '\n' (replacing a run
   of break-spaces) and passes source '\n' through, so a byte-lockstep walk
   recovers each wrapped BYTE's source offset. out_src[i] = source byte offset of
   wrapped[i]. Returns the wrapped byte length (entries written), capped at cap. */
int caret_reconcile(const char *src, const char *wrapped, int *out_src, int cap);

/* Assemble a CaretField. wrapped + out_src (map) from the two calls above; adv[i]
   = the x to add at wrapped byte i (a codepoint's advance at its lead byte, 0 at
   continuation bytes and '\n'); wlen = the wrapped byte length; line_h in metres.
   A leading slot opens each line at x=0; a trailing slot follows each codepoint.
   Pure. Returns the line count. */
int caret_field_build(const char *src, const char *wrapped, const int *map,
                      const float *adv, int wlen, float line_h, CaretField *out);

int caret_slot_for_offset(const CaretField *cf, int cursor);            /* slot with .src==cursor (nearest fallback) */
int caret_line_of_slot(const CaretField *cf, int slot);                 /* slot -> visual line */
int caret_slot_nearest_x(const CaretField *cf, int line, float goal_x); /* nearest .x on `line`; -1 if line OOB */

#endif
```

- [ ] **Step 2: Write the failing test `caret_test.c`**

```c
/* caret_test.c — pure-logic test for caret.c (the note-caret layout math).
   GL-free, ASan/UBSan via `build.sh carettest`. Feeds synthetic advances so no
   Font is needed. */

#include "caret.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

/* unit advance per byte: 1.0 at each lead byte, 0.0 at continuation bytes/'\n'. */
static void unit_adv(const char *wrapped, int wlen, float *adv) {
    int i = 0;
    while (i < wlen) {
        int n = caret_cplen((unsigned char)wrapped[i]);
        adv[i] = (wrapped[i] == '\n') ? 0.0f : 1.0f;
        for (++i; n > 1 && i < wlen; n--, i++) adv[i] = 0.0f;
    }
}

static int build(const char *src, const char *wrapped, CaretField *cf) {
    int   map[CARET_MAX_SLOTS];
    float adv[CARET_MAX_SLOTS];
    int   wlen = caret_reconcile(src, wrapped, map, CARET_MAX_SLOTS);
    unit_adv(wrapped, wlen, adv);
    return caret_field_build(src, wrapped, map, adv, wlen, 1.0f, cf);
}

static void test_cplen(void) {
    CHECK(caret_cplen((unsigned char)'a') == 1, "cplen ascii=1");
    CHECK(caret_cplen(0xC3u) == 2, "cplen 2-byte lead");
    CHECK(caret_cplen(0xE2u) == 3, "cplen 3-byte lead");
    CHECK(caret_cplen(0x80u) == 1, "cplen stray continuation -> 1 (progress)");
}

static void test_reconcile_plain(void) {
    int map[16], n = caret_reconcile("hello", "hello", map, 16), i;
    CHECK(n == 5, "reconcile plain: len");
    for (i = 0; i < 5; i++) CHECK(map[i] == i, "reconcile plain: identity offsets");
}

static void test_reconcile_softwrap(void) {
    /* "ab cd" wrapped to "ab\ncd": the space (src 2) collapses into the '\n' */
    int map[16], n = caret_reconcile("ab cd", "ab\ncd", map, 16);
    CHECK(n == 5, "reconcile softwrap: len");
    CHECK(map[0] == 0 && map[1] == 1, "reconcile softwrap: ab -> 0,1");
    CHECK(map[2] == 2, "reconcile softwrap: '\\n' maps to the collapsed space (2)");
    CHECK(map[3] == 3 && map[4] == 4, "reconcile softwrap: cd -> 3,4 (post-space)");
}

static void test_reconcile_multispace(void) {
    /* "ab   cd" -> "ab\ncd": three spaces (2,3,4) collapse to one '\n' */
    int map[16], n = caret_reconcile("ab   cd", "ab\ncd", map, 16);
    CHECK(n == 5, "reconcile multispace: len");
    CHECK(map[2] == 2, "reconcile multispace: '\\n' at first collapsed space");
    CHECK(map[3] == 5 && map[4] == 6, "reconcile multispace: cd -> 5,6 (past 3 spaces)");
}

static void test_reconcile_hard_nl(void) {
    int map[16], n = caret_reconcile("a\nb", "a\nb", map, 16);
    CHECK(n == 3 && map[0] == 0 && map[1] == 1 && map[2] == 2, "reconcile hard '\\n' passes through");
}

static void test_field_plain(void) {
    CaretField cf;
    int lines = build("abc", "abc", &cf);
    CHECK(lines == 1, "field plain: one line");
    /* slots: leading(0) + after a,b,c => 4 slots at x 0,1,2,3, src 0,1,2,3 */
    CHECK(cf.slot_count == 4, "field plain: 4 slots");
    CHECK(cf.slots[0].src == 0 && cf.slots[0].x == 0.0f, "field plain: leading slot");
    CHECK(cf.slots[3].src == 3 && cf.slots[3].x == 3.0f, "field plain: end slot at x=3,src=3");
    CHECK(caret_slot_for_offset(&cf, 2) == 2, "field plain: offset 2 -> slot 2");
    CHECK(caret_line_of_slot(&cf, 3) == 0, "field plain: slot 3 on line 0");
    CHECK(caret_slot_nearest_x(&cf, 0, 1.4f) == 1, "field plain: x~1.4 -> slot 1");
    CHECK(caret_slot_nearest_x(&cf, 0, 1.6f) == 2, "field plain: x~1.6 -> slot 2");
}

static void test_field_softwrap(void) {
    CaretField cf;
    int lines = build("ab cd", "ab\ncd", &cf);
    CHECK(lines == 2, "field softwrap: two lines");
    CHECK(cf.lines[1].line == 1, "field softwrap: second line index");
    /* line 1 leading slot src = first char after the collapsed space = 3 */
    {
        int s0 = cf.lines[1].slot0;
        CHECK(cf.slots[s0].src == 3 && cf.slots[s0].x == 0.0f, "field softwrap: line1 leading slot src=3 x=0");
    }
    CHECK(caret_line_of_slot(&cf, cf.lines[1].slot0) == 1, "field softwrap: leading slot of line1 -> line 1");
    /* cursor 4 (between c and d) resolves on line 1 */
    {
        int s = caret_slot_for_offset(&cf, 4);
        CHECK(caret_line_of_slot(&cf, s) == 1, "field softwrap: offset 4 on line 1");
    }
}

static void test_field_empty(void) {
    CaretField cf;
    int lines = build("", "", &cf);
    CHECK(lines == 1 && cf.slot_count == 1, "field empty: one line, one slot");
    CHECK(cf.slots[0].src == 0 && cf.slots[0].x == 0.0f, "field empty: slot at origin");
}

static void test_field_multibyte(void) {
    /* "é" = 0xC3 0xA9 (2 bytes), then 'x'. src "éx". One line; slots: leading(0),
       after é (x=1, src=2), after x (x=2, src=3). */
    CaretField cf;
    int lines = build("\xC3\xA9x", "\xC3\xA9x", &cf);
    CHECK(lines == 1, "field multibyte: one line");
    CHECK(cf.slot_count == 3, "field multibyte: 3 slots (not 4 — é is one codepoint)");
    CHECK(cf.slots[1].src == 2 && cf.slots[1].x == 1.0f, "field multibyte: after é src=2 x=1");
    CHECK(cf.slots[2].src == 3 && cf.slots[2].x == 2.0f, "field multibyte: after x src=3 x=2");
}

int main(void) {
    test_cplen();
    test_reconcile_plain();
    test_reconcile_softwrap();
    test_reconcile_multispace();
    test_reconcile_hard_nl();
    test_field_plain();
    test_field_softwrap();
    test_field_empty();
    test_field_multibyte();
    if (fails == 0) printf("caret_test: all passed\n");
    return fails ? 1 : 0;
}
```

- [ ] **Step 3: Add the `carettest` target + caret.c to c89check in `build.sh`**

After the `iotest`/`scenetest` block (find the first `if [ "$MODE" = "scenetest" ]; then … fi`), insert:

```sh

# Build + run the pure note-caret layout test under the sanitizers. caret.c is
# GL-free (libc only), so this links just it + the test.
if [ "$MODE" = "carettest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        caret.c caret_test.c \
        -o caret_test
    echo "built ./caret_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi
```

In the `c89check` block (the `-fsyntax-only … main.c rhi_gl.c …` source list, build.sh ~line 16), add `caret.c` to the list (e.g. right after `boardpage.c`).

- [ ] **Step 4: Verify the test FAILS (no implementation yet)**

Run: `./build.sh carettest`
Expected: a LINK error (undefined `caret_cplen`/`caret_reconcile`/…) — `caret.c` is empty/missing the functions. (Write a stub `caret.c` with empty bodies first if you prefer a compile-then-assert-fail; either way the goal of this step is "not yet green".)

- [ ] **Step 5: Implement `caret.c`**

```c
/* caret.c — see caret.h. Pure note-caret layout: no font, no GL, no scene. */

#include "caret.h"

int caret_cplen(unsigned char lead) {
    if (lead < 0x80u) return 1;
    if ((lead & 0xE0u) == 0xC0u) return 2;
    if ((lead & 0xF0u) == 0xE0u) return 3;
    if ((lead & 0xF8u) == 0xF0u) return 4;
    return 1;                                  /* stray continuation: make progress */
}

int caret_reconcile(const char *src, const char *wrapped, int *out_src, int cap) {
    int si = 0, wi = 0;
    while (wrapped[wi] != '\0' && wi < cap) {
        if (wrapped[wi] == src[si]) {
            out_src[wi] = si;
            si++; wi++;
        } else {
            out_src[wi] = si;                  /* an inserted soft-break '\n' */
            if (wrapped[wi] == '\n')
                while (src[si] == ' ') si++;   /* consume the collapsed break-spaces */
            else if (src[si] != '\0')
                si++;                           /* defensive: stay in lockstep */
            wi++;
        }
    }
    return wi;
}

int caret_field_build(const char *src, const char *wrapped, const int *map,
                      const float *adv, int wlen, float line_h, CaretField *out) {
    int   wi, line, srclen = 0;
    float pen = 0.0f;
    while (src[srclen] != '\0') srclen++;
    out->slot_count = 0;
    out->line_count = 0;
    out->line_h     = line_h;
    /* open line 0 with a leading slot at x=0 */
    out->lines[0].slot0  = 0;
    out->lines[0].nslots = 0;
    out->lines[0].line   = 0;
    out->line_count      = 1;
    line = 0;
    out->slots[0].src = (wlen > 0) ? map[0] : srclen;
    out->slots[0].x   = 0.0f;
    out->slot_count   = 1;
    out->lines[0].nslots = 1;
    wi = 0;
    while (wi < wlen) {
        if (wrapped[wi] == '\n') {             /* close line; open the next */
            int next = (wi + 1 < wlen) ? map[wi + 1] : srclen;
            wi++;
            if (out->line_count >= CARET_MAX_LINES) break;
            line = out->line_count;
            out->lines[line].slot0  = out->slot_count;
            out->lines[line].nslots = 0;
            out->lines[line].line   = line;
            out->line_count++;
            pen = 0.0f;
            if (out->slot_count < CARET_MAX_SLOTS) {
                out->slots[out->slot_count].src = next;
                out->slots[out->slot_count].x   = 0.0f;
                out->slot_count++;
                out->lines[line].nslots++;
            }
            continue;
        }
        {   /* one codepoint: advance the pen, emit a trailing slot */
            int n = caret_cplen((unsigned char)wrapped[wi]);
            int after;
            pen += adv[wi];
            after = (wi + n < wlen) ? map[wi + n] : srclen;
            if (out->slot_count < CARET_MAX_SLOTS) {
                out->slots[out->slot_count].src = after;
                out->slots[out->slot_count].x   = pen;
                out->slot_count++;
                out->lines[line].nslots++;
            }
            wi += n;
        }
    }
    return out->line_count;
}

int caret_slot_for_offset(const CaretField *cf, int cursor) {
    int i, best = -1, bd = 0, d;
    for (i = 0; i < cf->slot_count; i++)
        if (cf->slots[i].src == cursor) return i;
    for (i = 0; i < cf->slot_count; i++) {     /* nearest as a fallback */
        d = cf->slots[i].src - cursor; if (d < 0) d = -d;
        if (best < 0 || d < bd) { best = i; bd = d; }
    }
    return best;
}

int caret_line_of_slot(const CaretField *cf, int slot) {
    int i;
    for (i = 0; i < cf->line_count; i++)
        if (slot >= cf->lines[i].slot0 &&
            slot <  cf->lines[i].slot0 + cf->lines[i].nslots) return i;
    return cf->line_count > 0 ? cf->line_count - 1 : 0;
}

int caret_slot_nearest_x(const CaretField *cf, int line, float goal_x) {
    int   i, s0, s1, best = -1;
    float bd = 0.0f, d;
    if (line < 0 || line >= cf->line_count) return -1;
    s0 = cf->lines[line].slot0;
    s1 = s0 + cf->lines[line].nslots;
    for (i = s0; i < s1; i++) {
        d = cf->slots[i].x - goal_x; if (d < 0.0f) d = -d;
        if (best < 0 || d < bd) { best = i; bd = d; }
    }
    return best;
}
```

- [ ] **Step 6: Verify the test PASSES + gauntlet**

Run: `./build.sh carettest && ./caret_test` → `caret_test: all passed` (no sanitizer output).
Run: `./build.sh c89check` → `c89check: PASS …` (now syntax-checks caret.c too).

- [ ] **Step 7: Commit**

```bash
git add caret.h caret.c caret_test.c build.sh
git commit -m "$(cat <<'EOF'
Note caret: pure layout core (caret.c) + carettest

caret.c (GL-free): caret_reconcile (source<->wrapped offset mapping
through text_wrap's '\n'-insertion + space-collapse), caret_field_build
(slot/line assembly from precomputed advances), and slot/line search.
Unit-tested with synthetic advances (plain / soft-wrap / multi-space /
hard newline / multibyte / empty). caret_build (font glue) follows in
main.c. No render-path change.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: `caret_build` glue + caret state + render the caret quad (caret at end; no behavior change yet)

**Files:**
- Modify: `main.c` (include caret.h; `static caret_build`; `edit_cursor`/`edit_goal_x` fields + a `caret_mesh`; `note_edit_begin` seeds; the note-body render draws a caret quad instead of `_`; `on_char`/Backspace/Enter keep `edit_cursor = edit_len`)
- Modify: `build.sh` (add `caret.c` to the GL link line AND the Metal link line)

This task makes the caret VISIBLE (a thin quad at the insertion point) and proves `caret_build`'s x-accumulation aligns with the rendered text. The caret stays at the END (editing still typewriter), so there is no behavioral change — only the `_` becomes a quad.

- [ ] **Step 1: Link caret.c into the real builds**

In `build.sh`, add `caret.c` to BOTH the GL link line(s) (the `clang … main.c rhi_gl.c mesh.c … boardpage.c multiselect.c widget.c app_synth.c …`) and the Metal link line (`… boardpage.c multiselect.c widget.c app_synth.c \`), next to `boardpage.c`, mirroring how other modules appear in all link lists.

- [ ] **Step 2: Include caret.h + add AppState fields**

Near the other `#include` lines at the top of `main.c`, add `#include "caret.h"`.

At `main.c:2848` (after `int edit_len;`), add:
```c
    int         edit_cursor;      /* byte offset into edit_buf: the caret */
    float       edit_goal_x;      /* remembered note-local x for Up/Down */
    Mesh        caret_mesh;       /* unit caret quad; built once on first use */
```

- [ ] **Step 3: Add `static caret_build` (font glue)**

Place this near the note-edit helpers (e.g. just above `note_edit_begin`, main.c:16459). It needs `text_wrap`, the font API, and `caret_cplen`/`caret_reconcile`/`caret_field_build` (all already declared via caret.h / text.h / font.h, included in main.c):

```c
/* Build the note's caret field: wrap exactly as the renderer does, recover
   source offsets, gather per-char advances from the font, assemble (pure). */
static int caret_build(const Font *f, const char *src, float px2m, float wrap_w,
                       CaretField *out) {
    char  wrapped[CARET_MAX_SLOTS];
    int   map[CARET_MAX_SLOTS];
    float adv[CARET_MAX_SLOTS];
    int   wlen, wi, prevg = 0;
    text_wrap(f, src, px2m, wrap_w, wrapped, (int)sizeof wrapped);
    wlen = caret_reconcile(src, wrapped, map, CARET_MAX_SLOTS);
    for (wi = 0; wi < wlen; ) {                 /* advance per codepoint at its lead byte */
        unsigned long    cp = 0;
        int              n  = caret_cplen((unsigned char)wrapped[wi]);
        int              gi, k;
        const FontGlyph *g;
        for (k = 0; k < n && wi + k < wlen; k++) adv[wi + k] = 0.0f;   /* zero whole codepoint */
        if (wrapped[wi] == '\n') { prevg = 0; wi += n; continue; }
        cp = (unsigned long)(unsigned char)wrapped[wi];               /* ascii fast path; */
        if (n > 1) {                                                   /* decode multibyte */
            cp = (unsigned long)((unsigned char)wrapped[wi] & (0x7Fu >> n));
            for (k = 1; k < n && wi + k < wlen; k++)
                cp = (cp << 6) | ((unsigned long)(unsigned char)wrapped[wi + k] & 0x3Fu);
        }
        gi = font_glyph_index(f, cp);
        g  = gi ? font_glyph(f, gi) : (const FontGlyph *)0;
        if (g) {
            float a = g->advance;
            if (prevg) a += font_kern(f, prevg, gi);
            adv[wi] = a * px2m;
            prevg = gi;
        } else {
            prevg = 0;                                                 /* glyph-less: 0 advance */
        }
        wi += n;
    }
    return caret_field_build(src, wrapped, map, adv,
                             wlen, font_line_height(f) * px2m, out);
}
```
(If `font_glyph_index` isn't the exact name, use whatever `text_shape` calls to map a codepoint → glyph index; grep `text.c` — it uses `font_glyph_index`.)

- [ ] **Step 4: Seed the caret in `note_edit_begin`**

In `note_edit_begin` (main.c:16460), after `st->edit_handle = handle;`, add:
```c
    st->edit_cursor = st->edit_len;    /* caret at the end, matching today's feel */
    st->edit_goal_x = 0.0f;            /* refreshed on the first horizontal move/draw */
```

- [ ] **Step 5: Keep `edit_cursor` pinned to the end in the existing edit ops (no behavior change this task)**

In `on_char` (main.c:16527-16529 region), after `st->edit_buf[st->edit_len] = '\0';`, add `st->edit_cursor = st->edit_len;`.
In the Backspace branch (after `st->edit_buf[st->edit_len] = '\0';`, main.c:16634) add `st->edit_cursor = st->edit_len;`.
In the Enter branch (after `st->edit_buf[st->edit_len] = '\0';`, main.c:16639) add `st->edit_cursor = st->edit_len;`.
(These lines are REPLACED in Task 3 with real cursor logic; here they keep the caret at the end so this task is a pure render change.)

- [ ] **Step 6: Draw the caret quad instead of the `_`**

Replace the note-body render block (main.c:15647-15664) with:
```c
            if (o->kind == KIND_NOTE) {
                const char *txt = scene_meta_get(&state->scene, o->handle, "text");
                if (txt && txt[0]) {
                    float bpx2m = note_text_size(&state->scene, o->handle) / lh;
                    wtext_block(uf, vp, face, txt,
                                -cw * 0.5f + 2.0f * margin, ch - 2.0f * margin,
                                bpx2m, usable, ink_r, ink_g, ink_b);
                }
                if (state->edit_handle == o->handle) {     /* the moveable caret */
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
                    if (state->caret_mesh.index_count == 0) {
                        MeshBuilder mb;
                        mb_init(&mb);
                        make_page(&mb, 1.0f, 1.0f);        /* unit quad, scaled per draw */
                        state->caret_mesh = mesh_from_builder(&mb);
                        mb_free(&mb);
                    }
                    if (slot >= 0) {
                        float cx = x0 + cf.slots[slot].x;
                        float cw_caret = 0.006f;           /* caret thickness (m) */
                        float ctop = y0 - (float)linei * cf.line_h;
                        vec3  cpos = vec3_make(cx + cw_caret * 0.5f,
                                               ctop - cf.line_h * 0.5f, 0.0006f);
                        mat4  cmodel = mat4_mul(face,
                                   mat4_from_trs(cpos, quat_identity(),
                                                 vec3_make(cw_caret, cf.line_h, 1.0f)));
                        cm.base_color = vec3_make(0.10f, 0.09f, 0.08f);
                        cm.emissive   = vec3_make(0.10f, 0.09f, 0.08f);   /* reads on any ink */
                        draw_mesh(state, state->caret_mesh, cmodel, view, proj, eye, 0.0f, cm);
                    }
                }
            }
```
(Drop the `char ebuf[EDIT_BUF_CAP + 2];` and the `_`-append entirely.)

- [ ] **Step 7: Free `caret_mesh` on shutdown**

Find where `resize_handle_mesh` is destroyed (grep `mesh_destroy(&state.resize_handle_mesh)` / the shutdown frees near `font_destroy(state.ui_font)` at main.c ~16936) and add `mesh_destroy(&state.caret_mesh);` beside it. If `resize_handle_mesh` is never explicitly freed, add `mesh_destroy(&state.caret_mesh);` next to `font_destroy(state.mono_font);` in the shutdown block.

- [ ] **Step 8: Gauntlet + live-verify**

```bash
./build.sh c89check && ./build.sh && ./build.sh metal && ./build.sh carettest && ./caret_test
```
All green. Then human live-verify (report back): edit a note (board view + first-person) — a thin caret bar appears at the END of the text, vertically on the right line, aligned with where the next character would go (the `_` is gone). Typing/Backspace/Enter behave exactly as before, caret tracking the end. Confirm the caret sits ON the text baseline area, not offset — if it's horizontally off, `caret_build`'s advance gathering doesn't match the render; tune before proceeding.

- [ ] **Step 9: Commit**

```bash
git add main.c build.sh
git commit -m "$(cat <<'EOF'
Note caret: caret_build glue + render the caret as a quad

Add the font-bound caret_build (text_wrap + advance gather + the pure
caret_field_build) and draw the insertion caret as a thin quad at
edit_cursor, replacing the trailing '_'. edit_cursor stays pinned to
the end here, so editing is unchanged — this is the render + alignment
step. caret.c linked into the GL + Metal builds.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Edit at the caret + Left/Right movement

**Files:**
- Modify: `main.c` (`on_char` insert-at-cursor; the edit-key block: Backspace-before / Delete-after / Enter-split / Left / Right; a small `caret_refresh_goal` helper)

- [ ] **Step 1: Add a goal-x refresh helper**

Place near `caret_build` (main.c). It rebuilds the field and records the caret's current x as the goal column:
```c
/* recompute edit_goal_x from the caret's current slot (after a horizontal move/edit). */
static void caret_refresh_goal(AppState *st) {
    SceneObject *o = scene_get(&st->scene, st->edit_handle);
    CaretField   cf;
    float        bpx2m, usable, cw, lh;
    int          slot;
    const char  *txt;
    if (!o || !st->ui_font) { st->edit_goal_x = 0.0f; return; }
    lh    = font_line_height(st->ui_font);
    cw    = mesh_ref_param("card", o->mesh_params, o->mesh_param_count, "w");
    usable = cw - 3.0f * 0.025f;                 /* matches the render's margin math */
    bpx2m  = note_text_size(&st->scene, st->edit_handle) / lh;
    txt    = st->edit_buf;
    caret_build(st->ui_font, txt, bpx2m, usable, &cf);
    slot = caret_slot_for_offset(&cf, st->edit_cursor);
    st->edit_goal_x = (slot >= 0) ? cf.slots[slot].x : 0.0f;
}
```
(Confirm the field name `st->ui_font` and `mesh_ref_param`/the margin constant `0.025f` against the render block; reuse the exact `usable`/`bpx2m` formulas from main.c:15659-15662 so the goal x matches what's drawn.)

- [ ] **Step 2: `on_char` inserts at the caret**

Replace the tail of `on_char` (main.c:16526-16531) — from `if (n <= 0 || st->edit_len + n >= EDIT_BUF_CAP) return;` through `note_autosize(st, st->edit_handle);` — with:
```c
    if (n <= 0 || st->edit_len + n >= EDIT_BUF_CAP) return;
    memmove(st->edit_buf + st->edit_cursor + n,
            st->edit_buf + st->edit_cursor,
            (size_t)(st->edit_len - st->edit_cursor));
    memcpy(st->edit_buf + st->edit_cursor, enc, (size_t)n);
    st->edit_len    += n;
    st->edit_cursor += n;
    st->edit_buf[st->edit_len] = '\0';
    scene_meta_set(&st->scene, st->edit_handle, "text", st->edit_buf);
    note_autosize(st, st->edit_handle);
    caret_refresh_goal(st);
```

- [ ] **Step 3: Backspace-before / Delete-after / Enter-split / Left / Right in the edit-key block**

Replace the Backspace and Enter branches (main.c:16628-16642) and add Delete/Left/Right, so the block reads:
```c
        } else if (key == GLFW_KEY_BACKSPACE && st->edit_cursor > 0) {
            int e = st->edit_cursor, s = e - 1;   /* delete the codepoint before the caret */
            while (s > 0 && ((unsigned char)st->edit_buf[s] & 0xC0u) == 0x80u) s--;
            memmove(st->edit_buf + s, st->edit_buf + e,
                    (size_t)(st->edit_len - e));
            st->edit_len   -= (e - s);
            st->edit_cursor = s;
            st->edit_buf[st->edit_len] = '\0';
            scene_meta_set(&st->scene, st->edit_handle, "text", st->edit_buf);
            note_autosize(st, st->edit_handle);
            caret_refresh_goal(st);
        } else if (key == GLFW_KEY_DELETE && st->edit_cursor < st->edit_len) {
            int s = st->edit_cursor, e = s + 1;   /* delete the codepoint after the caret */
            while (e < st->edit_len && ((unsigned char)st->edit_buf[e] & 0xC0u) == 0x80u) e++;
            memmove(st->edit_buf + s, st->edit_buf + e,
                    (size_t)(st->edit_len - e));
            st->edit_len -= (e - s);
            st->edit_buf[st->edit_len] = '\0';
            scene_meta_set(&st->scene, st->edit_handle, "text", st->edit_buf);
            note_autosize(st, st->edit_handle);
            caret_refresh_goal(st);
        } else if (key == GLFW_KEY_ENTER && st->edit_len + 1 < EDIT_BUF_CAP) {
            memmove(st->edit_buf + st->edit_cursor + 1,
                    st->edit_buf + st->edit_cursor,
                    (size_t)(st->edit_len - st->edit_cursor));
            st->edit_buf[st->edit_cursor] = '\n';
            st->edit_len++;
            st->edit_cursor++;
            st->edit_buf[st->edit_len] = '\0';
            scene_meta_set(&st->scene, st->edit_handle, "text", st->edit_buf);
            note_autosize(st, st->edit_handle);
            caret_refresh_goal(st);
        } else if (key == GLFW_KEY_LEFT && st->edit_cursor > 0) {
            st->edit_cursor--;
            while (st->edit_cursor > 0 &&
                   ((unsigned char)st->edit_buf[st->edit_cursor] & 0xC0u) == 0x80u)
                st->edit_cursor--;
            caret_refresh_goal(st);
        } else if (key == GLFW_KEY_RIGHT && st->edit_cursor < st->edit_len) {
            st->edit_cursor++;
            while (st->edit_cursor < st->edit_len &&
                   ((unsigned char)st->edit_buf[st->edit_cursor] & 0xC0u) == 0x80u)
                st->edit_cursor++;
            caret_refresh_goal(st);
        }
```
Also DELETE the three `st->edit_cursor = st->edit_len;` lines added in Task 2 Step 5 (in `on_char`, Backspace, Enter) — they're superseded here. (`on_char`'s is replaced by Step 2; the Backspace/Enter ones are gone with this rewrite.)

- [ ] **Step 4: Gauntlet + live-verify**

```bash
./build.sh c89check && ./build.sh && ./build.sh metal && ./build.sh carettest && ./caret_test
```
Live-verify (report back): place the caret implicitly by typing — actually edit a note, then with Left/Right step through it; type in the MIDDLE → inserts at the caret; Backspace deletes the char before; Delete the char after; Enter splits the line at the caret; the caret bar tracks each operation; Left across a multi-byte char (if you can enter one) moves one codepoint. First-person and board view both.

- [ ] **Step 5: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
Note caret: insert/delete at the caret + Left/Right

on_char inserts at edit_cursor; Backspace deletes the codepoint before,
Delete the one after, Enter splits at the caret; Left/Right step by
codepoint. Each refreshes the Up/Down goal column. Editing is no longer
end-only.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Up/Down movement + board-view click-to-position

**Files:**
- Modify: `main.c` (Up/Down in the edit-key block; the click-to-position branch in the blur path)

- [ ] **Step 1: Up/Down in the edit-key block**

Add to the edit-key `if/else` chain (after the Right branch from Task 3):
```c
        } else if (key == GLFW_KEY_UP || key == GLFW_KEY_DOWN) {
            SceneObject *o = scene_get(&st->scene, st->edit_handle);
            if (o && st->ui_font) {
                float      lh    = font_line_height(st->ui_font);
                float      cw    = mesh_ref_param("card", o->mesh_params, o->mesh_param_count, "w");
                float      usable = cw - 3.0f * 0.025f;
                float      bpx2m  = note_text_size(&st->scene, st->edit_handle) / lh;
                CaretField cf;
                int        slot, line, tgt;
                caret_build(st->ui_font, st->edit_buf, bpx2m, usable, &cf);
                slot = caret_slot_for_offset(&cf, st->edit_cursor);
                line = (slot >= 0) ? caret_line_of_slot(&cf, slot) : 0;
                if (key == GLFW_KEY_UP)   line = (line > 0) ? line - 1 : -1;
                else                      line = (line + 1 < cf.line_count) ? line + 1 : -1;
                if (line < 0) {
                    st->edit_cursor = (key == GLFW_KEY_UP) ? 0 : st->edit_len;  /* off the top/bottom */
                } else {
                    tgt = caret_slot_nearest_x(&cf, line, st->edit_goal_x);     /* keep the column */
                    if (tgt >= 0) st->edit_cursor = cf.slots[tgt].src;
                }
                /* NOTE: do NOT refresh edit_goal_x here — preserving it lets the
                   column survive passing through a short line. */
            }
        }
```

- [ ] **Step 2: Click-to-position in board view (blur path)**

In the modal-gate blur branch (main.c:16808-16811 — `if (st->edit_handle != 0) { … if (lmb && !st->lmb_was_down) note_edit_end(st); … }`), replace the `note_edit_end` call with a hit-test:
```c
        if (st->edit_handle != 0) {     /* board-view click places the caret; else blur */
            sol_bool lmb = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            if (lmb && !st->lmb_was_down) {
                if (!caret_click_place(st, w)) note_edit_end(st);
            }
            st->lmb_was_down = lmb;
        }
```
And add the helper near `caret_build` (returns 1 if it placed the caret, 0 if the click missed the edited note so the caller should blur):
```c
/* In board view, if the click ray hits the note being edited, set edit_cursor
   to the nearest caret slot and return 1; otherwise (miss, or not board view)
   return 0 so the caller blurs. */
static int caret_click_place(AppState *st, GLFWwindow *w) {
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
    lx = vec3_dot(d, rx) / rr2;                 /* note-local x (pre-scale metres) */
    ly = vec3_dot(d, ry) / ru2;                 /* note-local y, 0..ch */
    if (lx < -cw * 0.5f || lx > cw * 0.5f || ly < 0.0f || ly > ch) return 0;  /* off the card */
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
    if (slot >= 0) {
        st->edit_cursor = cf.slots[slot].src;
        st->edit_goal_x = lx - x0;
    }
    return 1;
}
```
(Confirm the helper names: `scene_world_matrix`, `mat4_mul_point`, `ray_vs_plane`, `vec3_sub/cross/dot/normalize/scale/add`, `pick_ray`, `mesh_ref_param`, `note_text_size`, `st->ui_font`. The margin constant `0.025f` and the `x0`/`y0`/`usable` formulas MUST equal the render block's, main.c:15659-15662.)

- [ ] **Step 3: Gauntlet + live-verify**

```bash
./build.sh c89check && ./build.sh && ./build.sh metal && ./build.sh carettest && ./caret_test
```
Live-verify (report back):
- Multi-line note (Enter a few lines): Up/Down move the caret between visual lines; the column is preserved when passing through a short line; Up on line 0 jumps to start, Down on the last line to end.
- In BOARD VIEW, click inside the note → the caret lands at the clicked character; click off the note → editing ends (blur) as before.
- In first-person, clicking still blurs (no caret placement); arrows still move the caret.
- Regression: note autosize/resize, double-click-to-edit, Esc-to-end all still work.

- [ ] **Step 4: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
Note caret: Up/Down by visual line + board-view click-to-place

Up/Down move the caret between wrapped lines, preserving the goal column
(off the top/bottom -> start/end). In board view, a click on the edited
note ray-hits its surface and sets the caret to the nearest slot; a click
elsewhere blurs as before. First-person is arrows-only. Completes v1
(notes); the reader-book caret is a deliberate follow-up.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-Review notes (for the implementer)

- **Spec coverage:** Task 1 = the pure layout + test (spec §2, caret.c). Task 2 = caret_build glue + caret render (spec §1 state, §2 caret_build, §5 render). Task 3 = insert/delete-at-caret + Left/Right (spec §3, §4 horizontal). Task 4 = Up/Down + board-view click (spec §4 vertical, §6). Non-goals respected: no reader caret, no Home/End/word-jump, no first-person click, `text_shape`/`wtext_block` untouched, solid caret, forward-Delete included.
- **Type/name consistency:** `edit_cursor` / `edit_goal_x` / `caret_mesh` (AppState); `caret_build` / `caret_refresh_goal` / `caret_click_place` (main.c statics); `CaretField`/`CaretSlot`/`CaretLine`, `caret_cplen`/`caret_reconcile`/`caret_field_build`/`caret_slot_for_offset`/`caret_line_of_slot`/`caret_slot_nearest_x` (caret.h). The `usable`/`bpx2m`/`x0`/`y0`/`0.025f margin` formulas MUST stay identical to the render block in every consumer (caret render, caret_refresh_goal, Up/Down, caret_click_place) — a divergence misplaces the caret. Consider hoisting them, but matching literals is acceptable for v1.
- **C89:** all new locals at block tops; no `//`; UTF-8 byte tests use `& 0xC0u`/`& 0x80u`. `caret_test.c` is C11 (its own target).
- **Build order:** caret.c enters the c89check list in Task 1 and the GL/Metal link lines in Task 2 (when main.c first references caret_build); keep that order so each task's gauntlet is consistent.
- **Verify-before-trust:** confirm `font_glyph_index` (grep text.c) and `st->ui_font` exist with those names before relying on them; the plan notes both.
- **Human live-verify is load-bearing** (the font-bound `caret_build` x-accumulation isn't unit-tested): after Task 2, the caret must sit exactly where the next glyph would; after Task 4, clicks must land on the right character. Report caret-vs-text alignment explicitly.
