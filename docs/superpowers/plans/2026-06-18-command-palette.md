# Command Palette Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a keyboard-driven, fuzzy-searchable command palette (opened with `:`) over a new shared command registry that both the keyboard handler and the palette dispatch through.

**Architecture:** A pure subsequence fuzzy matcher (`fuzzy.c`) + a bounded palette state-machine/overlay (`palette.c`) that treats `AppState` opaquely + a `Command` registry table in `main.c` that replaces ~25 inline polled key-blocks with one dispatch loop. The palette owns the keyboard while open and never touches mouse capture. No new GPU shader, so no Metal MSL twin.

**Tech Stack:** Strict C89 ("Dependable C"), GLFW input, the engine's immediate-mode UI (`ui.h`/`text.h`), the existing SDF fonts. Headless tests build under clang ASan/UBSan via `build.sh`.

**Spec:** `docs/superpowers/specs/2026-06-18-command-palette-design.md`

**Conventions (verified against the codebase):**
- `sol_bool`/`SOL_TRUE`/`SOL_FALSE` live in `sol_base.h`.
- Headers use `#ifndef X_H` guards and `#include "sol_base.h"`.
- Test harnesses are hand-rolled `main()` returning 0/1, printing `ok:`/`FAIL:` and a final `name_test: OK` (see `nid_test.c`).
- `build.sh` test modes: `clang -std=c11 -g -O1 -fsanitize=address,undefined -Wall -Wextra <module>.c <module>_test.c -o <module>_test`.
- The c89check gate (`build.sh c89check`) compiles engine `.c` files with `-std=c89 -pedantic-errors -Werror -Wall -Wextra` (no linking). Every new engine `.c` must be added to its list.
- Commits go on `main` and end with the `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>` line. **Never** stage `NOTES.stml` or `paper-picture.png`.

---

## Task 1: The fuzzy matcher (`fuzzy.c`) — TDD

A pure, case-insensitive subsequence matcher with scoring. No engine state. v1 uses **greedy leftmost** matching (optimal for short command names; full DP alignment deferred).

**Files:**
- Create: `fuzzy.h`
- Create: `fuzzy_test.c`
- Create: `fuzzy.c`
- Modify: `build.sh` (add `fuzzytest` mode; add `fuzzy.c` to the c89check list)

- [ ] **Step 1: Create the header `fuzzy.h`**

```c
/* fuzzy.h — case-insensitive subsequence fuzzy matcher for the command palette.
   Pure: depends on the C library only, no engine state. Greedy leftmost match
   (good enough for short command names). See the palette design spec. */
#ifndef SOL_FUZZY_H
#define SOL_FUZZY_H

#include "sol_base.h"

/* Returns SOL_TRUE if every character of `query` appears in `cand` in order
   (case-insensitive). An empty query matches anything with score 0.
   On a match, *out_score (if non-NULL) gets a relevance score (higher = better),
   and out_pos[] (if non-NULL, capacity max_pos) gets the byte index in `cand` of
   each matched query character, in order. NULL query or cand => no match. */
sol_bool fuzzy_match(const char *query, const char *cand,
                     int *out_score, int *out_pos, int max_pos);

#endif /* SOL_FUZZY_H */
```

- [ ] **Step 2: Add the `fuzzytest` build mode to `build.sh`**

Insert this block immediately after the `nidtest` block (after its closing `fi`, around `build.sh:42`, before the `# Build + run the headless scene serialization test` comment):

```sh
# Build + run the standalone fuzzy-matcher test under the sanitizers.
if [ "$MODE" = "fuzzytest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        fuzzy.c fuzzy_test.c \
        -o fuzzy_test
    echo "built ./fuzzy_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi
```

- [ ] **Step 3: Create the failing test `fuzzy_test.c`**

```c
/* fuzzy_test.c — exercises the command-palette fuzzy matcher: subsequence
   correctness, case-insensitivity, ranking (boundary/contiguous beat scattered),
   empty-query-matches-all, no-match, and position reporting. Built by
   `build.sh fuzzytest` with ASan/UBSan. */
#include "fuzzy.h"

#include <stdio.h>

static int failures = 0;

static void check(int cond, const char *msg) {
    if (cond) {
        printf("ok: %s\n", msg);
    } else {
        printf("FAIL: %s\n", msg);
        failures++;
    }
}

int main(void) {
    int score_a, score_b, pos[8];

    check(fuzzy_match("bl", "Toggle bloom", NULL, NULL, 0) == SOL_TRUE,
          "\"bl\" matches \"Toggle bloom\"");

    check(fuzzy_match("BLOOM", "Toggle bloom", NULL, NULL, 0) == SOL_TRUE,
          "\"BLOOM\" matches case-insensitively");

    check(fuzzy_match("mb", "Toggle bloom", NULL, NULL, 0) == SOL_FALSE,
          "\"mb\" does not match (wrong order)");

    check(fuzzy_match("xyz", "Toggle bloom", NULL, NULL, 0) == SOL_FALSE,
          "\"xyz\" does not match");

    check(fuzzy_match("", "anything", &score_a, NULL, 0) == SOL_TRUE && score_a == 0,
          "empty query matches with score 0");

    /* word-boundary hit outranks a mid-word hit */
    fuzzy_match("m", "Toggle mist", &score_a, NULL, 0);  /* 'm' after a space */
    fuzzy_match("m", "Storm",       &score_b, NULL, 0);  /* 'm' mid-word */
    check(score_a > score_b, "word-boundary \"m\" outranks mid-word \"m\"");

    /* contiguous run outranks a scattered match */
    fuzzy_match("bc", "aabcd", &score_a, NULL, 0);  /* b,c adjacent */
    fuzzy_match("bc", "abxcd", &score_b, NULL, 0);  /* b,c with a gap */
    check(score_a > score_b, "contiguous \"bc\" outranks scattered \"bc\"");

    {
        sol_bool m = fuzzy_match("bl", "Toggle bloom", NULL, pos, 8);
        check(m == SOL_TRUE && pos[0] == 7 && pos[1] == 8,
              "positions of \"bl\" in \"Toggle bloom\" are 7,8");
    }

    if (failures == 0) {
        printf("fuzzy_test: OK\n");
        return 0;
    }
    printf("fuzzy_test: %d FAILURE(S)\n", failures);
    return 1;
}
```

- [ ] **Step 4: Create a stub `fuzzy.c` so the test compiles and fails**

```c
/* fuzzy.c — stub; real implementation in the next step. */
#include "fuzzy.h"

sol_bool fuzzy_match(const char *query, const char *cand,
                     int *out_score, int *out_pos, int max_pos) {
    (void)query; (void)cand; (void)out_pos; (void)max_pos;
    if (out_score) *out_score = 0;
    return SOL_FALSE;
}
```

- [ ] **Step 5: Run the test and confirm it FAILS**

Run: `./build.sh fuzzytest && ./fuzzy_test`
Expected: several `FAIL:` lines and a final `fuzzy_test: N FAILURE(S)`, exit code 1.

- [ ] **Step 6: Replace `fuzzy.c` with the real implementation**

```c
/* fuzzy.c — case-insensitive subsequence fuzzy matcher for the command palette.
   Pure: depends on the C library only. Greedy leftmost match; scoring rewards
   start-of-string and word-boundary hits and contiguous runs, penalises gaps. */
#include "fuzzy.h"

#include <ctype.h>

#define FUZZY_BONUS_FIRST     12   /* match at the very first char of cand */
#define FUZZY_BONUS_BOUNDARY   8   /* match right after a separator (word start) */
#define FUZZY_BONUS_CONTIG     5   /* match directly follows the previous match */
#define FUZZY_PENALTY_GAP      1   /* per skipped char between matches */

static int fuzzy_is_sep(int c) {
    return c == ' ' || c == '-' || c == '_' || c == '/' || c == '.';
}

static int fuzzy_lower(int c) {
    return tolower((unsigned char)c);
}

sol_bool fuzzy_match(const char *query, const char *cand,
                     int *out_score, int *out_pos, int max_pos) {
    int qi, ci, score, npos, prev;

    if (query == NULL || cand == NULL) {
        if (out_score) *out_score = 0;
        return SOL_FALSE;
    }

    qi = 0; ci = 0; score = 0; npos = 0; prev = -1;

    while (query[qi] != '\0') {
        while (cand[ci] != '\0' && fuzzy_lower(cand[ci]) != fuzzy_lower(query[qi]))
            ci++;
        if (cand[ci] == '\0') {                 /* ran out: not a subsequence */
            if (out_score) *out_score = 0;
            return SOL_FALSE;
        }
        if (ci == 0)
            score += FUZZY_BONUS_FIRST;
        else if (fuzzy_is_sep((unsigned char)cand[ci - 1]))
            score += FUZZY_BONUS_BOUNDARY;
        if (prev >= 0) {
            if (ci == prev + 1) score += FUZZY_BONUS_CONTIG;
            else                score -= FUZZY_PENALTY_GAP * (ci - prev - 1);
        }
        if (out_pos != NULL && npos < max_pos)
            out_pos[npos] = ci;
        npos++;
        prev = ci;
        ci++;
        qi++;
    }

    if (out_score != NULL) *out_score = score;
    return SOL_TRUE;
}
```

- [ ] **Step 7: Run the test and confirm it PASSES**

Run: `./build.sh fuzzytest && ./fuzzy_test`
Expected: all `ok:` lines and a final `fuzzy_test: OK`, exit code 0.

- [ ] **Step 8: Add `fuzzy.c` to the c89check list and run it**

In `build.sh:18`, append ` fuzzy.c` to the end of the `-fsyntax-only ...` source list (it currently ends `... json.c glb.c`):

```
        -fsyntax-only $GLFW_CFLAGS main.c rhi_gl.c mesh.c flora.c rock.c gothic.c sweep.c texgen.c mesh_gpu.c ui.c text.c wtext.c scene.c mirror.c material.c scene_io.c stml.c nid.c sol_math.c camera.c collide.c bvh.c asset.c component.c particles.c synth.c wav.c mixer.c reverb.c skel.c json.c glb.c fuzzy.c
```

Run: `./build.sh c89check`
Expected: `c89check: PASS — all sources are C89-pedantic clean`.

- [ ] **Step 9: Commit**

```bash
git add fuzzy.h fuzzy.c fuzzy_test.c build.sh
git commit -m "feat: fuzzy subsequence matcher for the command palette" -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Command registry + dispatch loop + first command (bloom)

Introduce the `Command` registry and the single dispatch loop, migrating exactly one command (`K` = toggle bloom) as the worked example. Proves the mechanism end-to-end with the keyboard before the palette or bulk migration.

**Files:**
- Create: `command.h`
- Modify: `main.c` (tag `AppState`; include `command.h`; add `cmd_toggle_bloom` + `g_commands` + dispatch loop; remove the inline `K` block + its `k_was_down` field)

- [ ] **Step 1: Create `command.h`**

```c
/* command.h — the shared command registry: one descriptor per discrete,
   edge-triggered command, consumed by BOTH read_input (keyboard) and the
   command palette. Continuous input (movement, exposure) is NOT here. */
#ifndef SOL_COMMAND_H
#define SOL_COMMAND_H

#include "sol_base.h"

/* AppState is the engine god-struct (main.c). Referenced opaquely by tag so this
   header needs no engine internals and creates no duplicate typedef. */
struct AppState;

typedef struct {
    const char *name;     /* shown + fuzzy-matched, e.g. "Toggle bloom" */
    const char *hint;     /* key label for display, e.g. "K"; NULL = none */
    int         key;      /* GLFW key code for dispatch; 0 = palette-only */
    void      (*run)(struct AppState *);
    sol_bool  (*can_run)(struct AppState *);   /* NULL = always available */
    sol_bool    was_down; /* edge-detect state */
} Command;

#endif /* SOL_COMMAND_H */
```

- [ ] **Step 2: Tag the `AppState` struct in `main.c`**

At `main.c:2479`, change the struct that closes as `} AppState;` (main.c:2739) to carry a tag:

Change:
```c
typedef struct {
    int         fb_width, fb_height;
```
to:
```c
typedef struct AppState {
    int         fb_width, fb_height;
```

- [ ] **Step 3: Include `command.h` in `main.c`**

Add `#include "command.h"` with the other project `#include "..."` lines near the top of `main.c` (alongside `#include "ui.h"` etc.). It must appear *before* line 2479 so the `Command` type is available where `g_commands` is defined.

- [ ] **Step 4: Add `cmd_toggle_bloom`, the registry table, and the count macro**

Place this block in `main.c` immediately **before** the `read_input` function definition (so every helper a command calls is already defined). The body of `cmd_toggle_bloom` is lifted verbatim from the existing inline `K` block (main.c:6034–6035):

```c
/* ---- Command registry (palette spec) ----------------------------------------
   One row per discrete, edge-triggered command. read_input() polls each row's
   key and the palette dispatches the same run() — one author, two consumers. */

static void cmd_toggle_bloom(AppState *st) {
    st->bloom_on = !st->bloom_on;
    printf("bloom %s\n", st->bloom_on ? "on" : "off");
}

static Command g_commands[] = {
    { "Toggle bloom", "K", GLFW_KEY_K, cmd_toggle_bloom, NULL, SOL_FALSE }
};

#define G_COMMAND_COUNT ((int)(sizeof g_commands / sizeof g_commands[0]))
```

- [ ] **Step 5: Add the dispatch loop inside `read_input`**

Add this near the start of the discrete-command section of `read_input` (e.g. just before the existing `F` block at main.c:6014). It is guarded so a modal that owns the keyboard suppresses commands. (The `st->palette.open` term is added in Task 3; for now guard only on `edit_handle`.)

```c
    /* Registered discrete commands: poll each hotkey, edge-trigger, honour the
       precondition. The palette dispatches these same run()s. */
    if (st->edit_handle == 0) {
        int ci;
        for (ci = 0; ci < G_COMMAND_COUNT; ci++) {
            Command *cmd = &g_commands[ci];
            sol_bool now;
            if (cmd->key == 0) continue;
            now = glfwGetKey(w, cmd->key) == GLFW_PRESS;
            if (now && !cmd->was_down && (cmd->can_run == NULL || cmd->can_run(st)))
                cmd->run(st);
            cmd->was_down = now;
        }
    }
```

- [ ] **Step 6: Delete the inline `K` block and its field**

Delete the inline bloom block at main.c:6030–6038 (the `/* K toggles bloom ... */` block). Then remove the now-unused `k_was_down` field declaration from the `AppState` struct (grep `k_was_down`; after the block is gone it is referenced nowhere).

- [ ] **Step 7: Build the full gauntlet**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: `c89check: PASS`, `built ./solarium (debug)`, `built ./solarium-metal ...` — all succeed.

- [ ] **Step 8: Live-verify bloom still toggles via the keyboard**

Run: `./solarium`, press `K` a few times.
Expected: console prints `bloom off` / `bloom on` exactly as before; the glow visibly toggles. Press `K` confirms the registry path drives it (the inline block is gone).

- [ ] **Step 9: Commit**

```bash
git add command.h main.c
git commit -m "feat: shared command registry + dispatch loop (bloom migrated)" -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Palette state machine, rendering, and input routing

The full palette over the current 1-command registry: open with `:`, type to filter, arrows/Enter/Esc, the debug-panel-idiom overlay.

**Files:**
- Create: `palette.h`
- Create: `palette.c`
- Modify: `main.c` (add `Palette palette;` to `AppState`; include `palette.h`; route `on_key`/`on_char`; extend the read_input blackhole + dispatch guard; call `palette_draw`)
- Modify: `build.sh` (add `palette.c` to c89check; add `fuzzy.c palette.c` to the GL, Metal, and ASan link lists)

- [ ] **Step 1: Create `palette.h`**

```c
/* palette.h — keyboard-driven, fuzzy-searchable command palette over the shared
   command registry. Owns the keyboard while open; never touches mouse capture.
   AppState is opaque here (only passed through to command callbacks). GLFW key
   codes are translated to PaletteKey by main.c, so this unit needs no GLFW. */
#ifndef SOL_PALETTE_H
#define SOL_PALETTE_H

#include "sol_base.h"
#include "command.h"
#include "font.h"

#define PALETTE_QUERY_CAP 128

typedef struct {
    sol_bool open;
    char     query[PALETTE_QUERY_CAP];
    int      len;
    int      sel;       /* highlighted result row */
    sol_bool eat_char;  /* swallow the leading ':' that opened the palette */
} Palette;

typedef enum {
    PALETTE_KEY_NONE = 0,
    PALETTE_KEY_UP,
    PALETTE_KEY_DOWN,
    PALETTE_KEY_ENTER,
    PALETTE_KEY_BACKSPACE,
    PALETTE_KEY_CANCEL
} PaletteKey;

void     palette_open_now(Palette *p);
sol_bool palette_is_open(const Palette *p);
void     palette_input_char(Palette *p, unsigned int cp);
/* Returns SOL_TRUE if the palette consumed the key (always true while open). */
sol_bool palette_input_key(Palette *p, PaletteKey k, struct AppState *st,
                           const Command *cmds, int ncmds);
void     palette_draw(const Palette *p, struct AppState *st, Font *font,
                      const Command *cmds, int ncmds, int fb_w, int fb_h);

#endif /* SOL_PALETTE_H */
```

- [ ] **Step 2: Create `palette.c`**

```c
/* palette.c — the command palette overlay + state machine. Pure UI + the fuzzy
   matcher; AppState is opaque (only shuttled to command callbacks). */
#include "palette.h"

#include "fuzzy.h"
#include "ui.h"
#include "text.h"

#include <string.h>

#define PALETTE_MAX_COMMANDS 64
#define PALETTE_MAX_ROWS     12

void palette_open_now(Palette *p) {
    p->open     = SOL_TRUE;
    p->query[0] = '\0';
    p->len      = 0;
    p->sel      = 0;
    p->eat_char = SOL_TRUE;   /* the ':' that opened us arrives next as a char */
}

sol_bool palette_is_open(const Palette *p) {
    return p->open;
}

void palette_input_char(Palette *p, unsigned int cp) {
    if (!p->open) return;
    if (p->eat_char) { p->eat_char = SOL_FALSE; return; }
    if (cp < 0x20 || cp > 0x7e) return;             /* v1: printable ASCII only */
    if (p->len + 1 >= PALETTE_QUERY_CAP) return;
    p->query[p->len++] = (char)cp;
    p->query[p->len]   = '\0';
    p->sel = 0;
}

/* Build the filtered, score-sorted list of command indices (best first). Returns
   the total match count; writes up to `cap` indices into out[]. Stable on ties
   (registry order preserved). */
static int palette_rank(const Palette *p, const Command *cmds, int ncmds,
                        int *out, int cap) {
    int idx[PALETTE_MAX_COMMANDS];
    int score[PALETTE_MAX_COMMANDS];
    int n = 0, i, j;

    if (ncmds > PALETTE_MAX_COMMANDS) ncmds = PALETTE_MAX_COMMANDS;
    for (i = 0; i < ncmds; i++) {
        int sc;
        if (fuzzy_match(p->query, cmds[i].name, &sc, NULL, 0)) {
            idx[n] = i; score[n] = sc; n++;
        }
    }
    for (i = 1; i < n; i++) {                        /* stable insertion sort */
        int ti = idx[i], ts = score[i];
        j = i - 1;
        while (j >= 0 && score[j] < ts) {
            idx[j + 1] = idx[j]; score[j + 1] = score[j]; j--;
        }
        idx[j + 1] = ti; score[j + 1] = ts;
    }
    if (cap > n) cap = n;
    for (i = 0; i < cap; i++) out[i] = idx[i];
    return n;
}

sol_bool palette_input_key(Palette *p, PaletteKey k, struct AppState *st,
                           const Command *cmds, int ncmds) {
    int order[PALETTE_MAX_COMMANDS];
    int n;

    if (!p->open) return SOL_FALSE;

    if (k == PALETTE_KEY_CANCEL) { p->open = SOL_FALSE; return SOL_TRUE; }

    n = palette_rank(p, cmds, ncmds, order, PALETTE_MAX_COMMANDS);

    if (k == PALETTE_KEY_DOWN) { if (p->sel + 1 < n) p->sel++; return SOL_TRUE; }
    if (k == PALETTE_KEY_UP)   { if (p->sel > 0)     p->sel--; return SOL_TRUE; }
    if (k == PALETTE_KEY_BACKSPACE) {
        if (p->len > 0) { p->len--; p->query[p->len] = '\0'; p->sel = 0; }
        return SOL_TRUE;
    }
    if (k == PALETTE_KEY_ENTER) {
        p->open = SOL_FALSE;                         /* close first */
        if (n > 0 && p->sel < n) {
            const Command *cmd = &cmds[order[p->sel]];
            if (cmd->can_run == NULL || cmd->can_run(st))
                cmd->run(st);
        }
        return SOL_TRUE;
    }
    return SOL_TRUE;                                  /* swallow anything else */
}

void palette_draw(const Palette *p, struct AppState *st, Font *font,
                  const Command *cmds, int ncmds, int fb_w, int fb_h) {
    int   order[PALETTE_MAX_COMMANDS];
    int   n, shown, top, i;
    float us, pad, row_h, ts, box_w, box_h, box_x, box_y;

    if (!p->open || font == NULL) return;

    us    = (float)fb_h / 1080.0f;
    pad   = 14.0f * us;
    row_h = 26.0f * us;
    ts    = 0.45f * us;

    n     = palette_rank(p, cmds, ncmds, order, PALETTE_MAX_COMMANDS);
    shown = (n < PALETTE_MAX_ROWS) ? n : PALETTE_MAX_ROWS;

    box_w = (float)fb_w * 0.55f;
    box_h = pad * 2.0f + row_h * (float)(shown + 1);
    box_x = ((float)fb_w - box_w) * 0.5f;
    box_y = (float)fb_h * 0.18f;

    ui_quad(box_x, box_y, box_w, box_h, 0.05f, 0.07f, 0.10f, 0.92f);
    ui_quad_outline(box_x, box_y, box_w, box_h, 1.0f * us, 0.95f, 0.80f, 0.45f, 0.9f);

    {   /* query row: ":<typed>_" */
        char  line[PALETTE_QUERY_CAP + 4];
        float qy = box_y + pad + font_ascent(font) * ts;
        int   ql = p->len;
        line[0] = ':';
        memcpy(line + 1, p->query, (size_t)ql);
        line[1 + ql] = '_';
        line[2 + ql] = '\0';
        ui_text(font, line, box_x + pad, qy, ts, 0.95f, 0.92f, 0.80f, 1.0f);
    }

    top = 0;
    if (p->sel >= PALETTE_MAX_ROWS) top = p->sel - PALETTE_MAX_ROWS + 1;

    for (i = 0; i < shown; i++) {
        int          ri = top + i;
        const Command *cmd;
        float        ry, ty;
        sol_bool     enabled;
        if (ri >= n) break;
        cmd     = &cmds[order[ri]];
        enabled = (cmd->can_run == NULL) || cmd->can_run(st);
        ry      = box_y + pad + row_h * (float)(i + 1);
        ty      = ry + font_ascent(font) * ts;
        if (ri == p->sel)
            ui_quad(box_x + pad * 0.5f, ry, box_w - pad, row_h,
                    0.20f, 0.24f, 0.30f, 0.9f);
        if (enabled)
            ui_text(font, cmd->name, box_x + pad, ty, ts, 0.92f, 0.92f, 0.92f, 1.0f);
        else
            ui_text(font, cmd->name, box_x + pad, ty, ts, 0.50f, 0.50f, 0.50f, 1.0f);
        if (cmd->hint != NULL) {
            float hw, hh;
            text_measure(font, cmd->hint, ts, &hw, &hh);
            ui_text(font, cmd->hint, box_x + box_w - pad - hw, ty, ts,
                    0.70f, 0.62f, 0.40f, 1.0f);
        }
    }
}
```

- [ ] **Step 3: Add the `Palette` field to `AppState` and include the header**

Add `#include "palette.h"` near the other project includes in `main.c` (it includes `command.h` and `font.h`; safe alongside them). Then add a member to the `AppState` struct (e.g. next to `edit_handle`/`edit_buf`):

```c
    Palette palette;        /* command palette state (palette.h) */
```

- [ ] **Step 4: Route typed characters into the palette (`on_char`)**

In `on_char` (main.c:10198), add a palette branch **above** the `edit_handle` logic. The function becomes:

```c
static void on_char(GLFWwindow *w, unsigned int cp) {
    AppState *st = (AppState *)glfwGetWindowUserPointer(w);
    char      enc[4];
    int       n;
    if (!st) return;
    if (st->palette.open) { palette_input_char(&st->palette, cp); return; }
    if (st->edit_handle == 0) return;
    n = utf8_encode(cp, enc);
    if (n <= 0 || st->edit_len + n >= EDIT_BUF_CAP) return;
    memcpy(st->edit_buf + st->edit_len, enc, (size_t)n);
    st->edit_len += n;
    st->edit_buf[st->edit_len] = '\0';
    scene_meta_set(&st->scene, st->edit_handle, "text", st->edit_buf);
}
```

- [ ] **Step 5: Route the open key and navigation into the palette (`on_key`)**

In `on_key` (main.c:10214), after fetching `st`, add two blocks **before** the existing edit-handle handling: (a) while the palette is open, translate the key to a `PaletteKey` and hand it over; (b) otherwise, `:` (Shift+`;`) opens it when no other modal owns input.

```c
    if (!st) return;

    /* Command palette owns the keyboard while open. */
    if (st->palette.open) {
        if (action == GLFW_PRESS || action == GLFW_REPEAT) {
            PaletteKey pk = PALETTE_KEY_NONE;
            if      (key == GLFW_KEY_ESCAPE)    pk = PALETTE_KEY_CANCEL;
            else if (key == GLFW_KEY_UP)        pk = PALETTE_KEY_UP;
            else if (key == GLFW_KEY_DOWN)      pk = PALETTE_KEY_DOWN;
            else if (key == GLFW_KEY_ENTER ||
                     key == GLFW_KEY_KP_ENTER)  pk = PALETTE_KEY_ENTER;
            else if (key == GLFW_KEY_BACKSPACE) pk = PALETTE_KEY_BACKSPACE;
            if (pk != PALETTE_KEY_NONE)
                palette_input_key(&st->palette, pk, st, g_commands, G_COMMAND_COUNT);
        }
        return;
    }

    /* ':' (Shift+;) opens the palette when nothing else owns the keyboard. */
    if (action == GLFW_PRESS && key == GLFW_KEY_SEMICOLON && (mods & GLFW_MOD_SHIFT)
        && st->edit_handle == 0 && st->reader_state == READER_IDLE) {
        palette_open_now(&st->palette);
        return;
    }
```

(Keep the existing `edit_handle` key handling below these blocks unchanged.)

- [ ] **Step 6: Extend the read_input blackhole and the dispatch guard**

In `read_input`, the dispatch loop guard added in Task 2 (`if (st->edit_handle == 0)`) becomes:

```c
    if (st->edit_handle == 0 && !st->palette.open) {
```

Also extend the existing movement/look suppression (the note-editing blackhole near main.c:5730) so it also triggers while the palette is open — change its condition from `st->edit_handle != 0` (or equivalent) to `st->edit_handle != 0 || st->palette.open`.

- [ ] **Step 7: Draw the palette in the 2D UI pass**

In the `ui_begin`/`ui_end` block (main.c:9924–10177), add this call just **before** `ui_end();` (main.c:10177). Note the local `AppState` here is named `state`:

```c
    palette_draw(&state->palette, state, state->mono_font,
                 g_commands, G_COMMAND_COUNT, state->fb_width, state->fb_height);
```

- [ ] **Step 8: Add the new sources to the build lists**

In `build.sh`: append ` palette.c` to the c89check list (`build.sh:18`, after `fuzzy.c`). Append ` fuzzy.c palette.c` (before the trailing ` \`) to each of the three link lists: the Metal build (`build.sh:242`), the ASan build (`build.sh:258`), and the GL build (`build.sh:273`). All three currently end `... json.c glb.c \`; they become `... json.c glb.c fuzzy.c palette.c \`.

- [ ] **Step 9: Build the full gauntlet**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: `c89check: PASS`, GL and Metal builds succeed.

- [ ] **Step 10: Live-verify the palette vertical slice**

Run: `./solarium`. Press `:` — a centered box appears with `:_` and one row `Toggle bloom    K`. Type `bl` — the row stays/filters; backspace clears it. Press Enter — the palette closes and bloom toggles (console `bloom on`/`off`). Reopen, press Esc — it closes with no action. Confirm walking (WASD) and other hotkeys are dead while open, and that the leading `:` does not appear in the field. Repeat the open/run check on `./solarium-metal`.

- [ ] **Step 11: Commit**

```bash
git add palette.h palette.c main.c build.sh
git commit -m "feat: command palette overlay, routing, and rendering" -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Migration tasks (4–5): move the remaining v1 commands into the registry

These are **mechanical relocations of existing code**, not new logic. For each command, apply this exact procedure:

1. Find its inline block in `read_input` (search for its `GLFW_KEY_*`).
2. Write `static void cmd_<name>(AppState *st) { ... }` just before `read_input`, with the body being **only the action statements** inside the block's `if (..._now && !..._was_down ...)` — **not** the `glfwGetKey`/`*_was_down` polling lines. If the block's `if` has an extra condition beyond the edge check (e.g. `&& st->irradiance_cubemap.id`, `&& st->current_terrain`), make a `static sol_bool can_<name>(AppState *st) { return <that condition>; }` predicate.
3. Add a row to `g_commands[]`: `{ "<Name>", "<hint>", GLFW_KEY_<X>, cmd_<name>, can_<name>_or_NULL, SOL_FALSE }`.
4. Delete the inline block and remove its now-unused `*_was_down` field from `AppState`.
5. **Safety valve:** if a body references `w` (the GLFW window) and cannot be expressed as `cmd_<name>(AppState *st)`, leave that command inline and note it — it is not a clean discrete command for v1.

> **Note on line numbers:** the ranges below are *pre-migration*; each deletion shifts later lines. Always locate a block by its `GLFW_KEY_*`, not by line number.

### Task 4: Migrate the toggles + scene-ops batch

**Files:** Modify `main.c` only.

- [ ] **Step 1: Migrate each command below using the procedure above**

| Key | Name | hint | cmd name | Precondition (`can_run`) |
|---|---|---|---|---|
| `GLFW_KEY_F` | Toggle walk/fly | `F` | `cmd_toggle_fly` | camera not orbit: `st->camera.mode != CAMERA_ORBIT` |
| `GLFW_KEY_X` | Toggle ghost (no-clip) | `X` | `cmd_toggle_ghost` | none |
| `GLFW_KEY_M` | Toggle shadow-map inspector | `M` | `cmd_toggle_shadowmap` | none |
| `GLFW_KEY_GRAVE_ACCENT` | Toggle day/night | `` ` `` | `cmd_toggle_daynight` | none |
| `GLFW_KEY_9` | Cycle color grade | `9` | `cmd_cycle_grade` | none |
| `GLFW_KEY_I` | Toggle irradiance view | `I` | `cmd_toggle_irradiance` | `st->irradiance_cubemap.id` |
| `GLFW_KEY_P` | Cycle prefilter inspector | `P` | `cmd_cycle_prefilter` | check the block for a guard |
| `GLFW_KEY_T` | Cycle text inspector | `T` | `cmd_cycle_textinspect` | none |
| `GLFW_KEY_J` | Toggle floor-plan overlay | `J` | `cmd_toggle_floorplan` | none |
| `GLFW_KEY_R` | Rescan mirrors | `R` | `cmd_rescan_mirrors` | none |
| `GLFW_KEY_L` | Reload scene | `L` | `cmd_reload_scene` | none |

For `F`, the existing block also toggles only when not in orbit; encode that as the `can_run`. Worked example for `F`:

```c
static sol_bool can_toggle_fly(AppState *st) {
    return st->camera.mode != CAMERA_ORBIT;
}
static void cmd_toggle_fly(AppState *st) {
    st->camera.mode = (st->camera.mode == CAMERA_WALK) ? CAMERA_FLY : CAMERA_WALK;
}
```
Row: `{ "Toggle walk/fly", "F", GLFW_KEY_F, cmd_toggle_fly, can_toggle_fly, SOL_FALSE }`.

- [ ] **Step 2: Build the full gauntlet**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: all pass.

- [ ] **Step 3: Live-verify the batch**

Run: `./solarium`. Press each migrated key and confirm its old behavior is intact (fly toggles, ghost prints, day/night swaps, `9` cycles grade, etc.). Open the palette with `:` and confirm all 11 names now appear and run from there (e.g. type `grade`, Enter → grade cycles; type `irrad` with no irradiance map → row is dimmed and Enter no-ops).

- [ ] **Step 4: Commit**

```bash
git add main.c
git commit -m "feat: migrate toggles + scene-ops into the command registry" -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

### Task 5: Migrate the one-shot mints batch

**Files:** Modify `main.c` only. Same procedure.

- [ ] **Step 1: Migrate each command below**

| Key | Name | hint | cmd name | Precondition (`can_run`) |
|---|---|---|---|---|
| `GLFW_KEY_B` | Spawn whiteboard | `B` | `cmd_mint_whiteboard` | check block |
| `GLFW_KEY_N` | Spawn note card | `N` | `cmd_mint_note` | check block |
| `GLFW_KEY_V` | Mint codex (book) | `V` | `cmd_mint_codex` | check block |
| `GLFW_KEY_H` | Mint island | `H` | `cmd_mint_island` | check block |
| `GLFW_KEY_U` | Mint church | `U` | `cmd_mint_church` | `st->current_terrain` set |
| `GLFW_KEY_O` | Mint lantern | `O` | `cmd_mint_lantern` | check block |
| `GLFW_KEY_Q` | Mint pond | `Q` | `cmd_mint_pond` | check block |
| `GLFW_KEY_E` | Mint dust emitter | `E` | `cmd_mint_dust` | check block |
| `GLFW_KEY_Y` | Mint fox | `Y` | `cmd_mint_fox` | check block |

"check block" = inspect that command's inline `if` for any condition beyond the edge check; if present, lift it into a `can_run` predicate, else pass `NULL`. Apply the Step-5 safety valve if a body needs `w`.

- [ ] **Step 2: Build the full gauntlet**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: all pass.

- [ ] **Step 3: Live-verify the batch**

Run: `./solarium`. Mint an island (`H`), then with it current, a church (`U`); spawn a whiteboard, note, lantern, pond, dust, fox. Confirm each still appears as before by hotkey. Then drive each from the palette (`:church` Enter, etc.). With no current terrain, confirm `Mint church` shows dimmed and Enter no-ops. Repeat a couple on `./solarium-metal`.

- [ ] **Step 4: Commit**

```bash
git add main.c
git commit -m "feat: migrate one-shot mints into the command registry" -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Deferred (explicitly out of v1 scope)

These keep working by hotkey via their inline blocks; migrate them in a later pass: mint abbey (`Z`, ~350 lines), gather-workspace (`G`), connection-arm (`C`), contextual `Backspace`. They are *not* part of this plan; do not attempt them here.

---

## Final verification

- [ ] `./build.sh c89check` → PASS
- [ ] `./build.sh debug` → builds; `./build.sh metal` → builds
- [ ] `./build.sh fuzzytest && ./fuzzy_test` → `fuzzy_test: OK`
- [ ] `./solarium`: every v1 command runs both by its hotkey and from the palette; `:` opens, type/arrows/Enter/Esc all behave; disabled commands dim and no-op; no `:` leak; movement and hotkeys are inert while open.
- [ ] `./solarium-metal`: palette opens and runs a command (no MSL twin, so parity is expected).
- [ ] `git status` shows only intended files staged across the commits (never `NOTES.stml` / `paper-picture.png`).
