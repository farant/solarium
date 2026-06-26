# Cut & Paste Board Cards — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finder-style cut (`Cmd+X`) and paste (`Cmd+V`) for board cards — cut marks the selection (cards stay, render dimmed); paste moves them to the current board's active page, including across boards.

**Architecture:** A global cut-buffer (`cut[]`/`cut_count`) on `AppState`, parallel to the multi-select `sel[]`. `Cmd+X` copies the selection into it; `Cmd+V` is contextual — if the cut buffer is non-empty it moves the cut cards via a single `card_move_to_page` helper (retag `meta["page"]` same-board, re-parent + re-pin cross-board), else it falls through to the existing clipboard-image paste. Cut cards render through the existing `draw_glass` alpha path (no new shader / MSL twin). All changes live in `main.c`.

**Tech Stack:** C89, OpenGL + Metal dual backend, GLFW input, the project's scene/meta system. Reuses `board_pin_pos`, `mint_tag_ws`, `scene_meta_set/get`, `sel_clear`, and the already-tested `msel_remove`.

---

## Testing approach (read first)

This is GUI/input/render integration in a 660 KB `main.c` engine. Per the project's established
pattern, **each task's automated gate is the three-target build gauntlet**, and the GUI behavior
is human-verified after merge (subagents cannot GUI-test). The one piece of pure logic — dropping
a deleted handle from the cut array — **reuses `msel_remove`, which already has unit tests in
`multiselect_test.c`**, so no new test is written for it (DRY/YAGNI).

**The gauntlet (run after every task):**
```bash
./build.sh c89check   # Expected: "c89check: PASS — all sources are C89-pedantic clean"
./build.sh            # Expected: "built ./solarium (debug)"
./build.sh metal      # Expected: "built ./solarium-metal (stage a: links clean, zero GL; runs from stage b)"
```

**C89 reminders for every task (the gauntlet enforces these — write them right the first time):**
- Declare all locals at the top of their block, before any statement.
- `/* block comments */` only — no `//`.
- Cast where the project casts (`(sol_bool)`, `(size_t)`); no implicit narrowing warnings.

---

## Task 1: Cut-buffer state on AppState

**Files:**
- Modify: `main.c` — the `AppState` struct (the multi-select fields are at ~main.c:2914) and the AppState init block (near `st->paste_was_down = SOL_FALSE;` at ~main.c:12199).

- [ ] **Step 1: Add the cut-buffer fields to AppState**

Find the multi-select block in the `AppState` struct (around main.c:2914):
```c
    sol_u32     sel[MULTISEL_CAP];   /* multi-select set; <=1 mirrors selected_handle */
    int         sel_count;
```
Immediately after `int sel_count;`, add:
```c
    sol_u32     cut[MULTISEL_CAP];   /* cut buffer (Cmd+X): handles marked to MOVE on paste */
    int         cut_count;           /* 0 = nothing cut; GLOBAL — survives board_view_exit */
    sol_bool    cut_was_down;        /* edge-detect for Cmd+X */
```

- [ ] **Step 2: Initialize the cut buffer**

Find the init line `st->paste_was_down = SOL_FALSE;` (around main.c:12199) and add right after it:
```c
    st->cut_count    = 0;
    st->cut_was_down = SOL_FALSE;
```
(The `cut[]` array contents are irrelevant while `cut_count == 0`; no need to zero it.)

- [ ] **Step 3: Build gauntlet**

Run the three gauntlet commands above. Expected: all three pass. (No behavior change yet — this only adds fields.)

- [ ] **Step 4: Commit**

```bash
git add main.c
git commit -m "Cut/paste: AppState cut-buffer fields + init"
```

---

## Task 2: The cut/paste helper block (move + commands + predicates)

All new functions go in **one block placed immediately after `mint_tag_ws` ends** (around main.c:7922), because every dependency it needs (`board_pin_pos` at 4222, `scene_meta_set/get`, `sel_clear` at 7657, `mint_tag_ws` at 7918, `cmd_paste_image` is NOT needed here) is defined by that point, and the block must precede both the `g_commands[]` array (main.c:9759) and the draw loop (main.c:14125) that reference these symbols. `PAGE_SLUG_CAP` comes from `boardpage.h` (already included).

**Files:**
- Modify: `main.c` — insert the block after `mint_tag_ws` (~main.c:7922).

- [ ] **Step 1: Insert the helper block**

Immediately after the closing `}` of `mint_tag_ws` (the function ending around main.c:7922), insert:

```c
/* ---- cut & paste board cards (Cmd+X / Cmd+V) ---------------------------
   A cut MARKS a set of board-card handles without removing them; paste MOVES
   them onto the board you're viewing, on its active page. The cut buffer is
   GLOBAL (not cleared on board_view_exit) so a paste can cross boards. */

static sol_bool handle_is_cut(const AppState *st, sol_u32 h) {
    int i;
    for (i = 0; i < st->cut_count; i++)
        if (st->cut[i] == h) return SOL_TRUE;
    return SOL_FALSE;
}

/* Move one board card onto (board, page), preserving its board-local layout.
   Same board: just retag the page (position is already board-local). Cross
   board: re-parent, re-pin onto the new face (keep local x/y, recompute the
   pin z from the new board's thickness), retag the page, and re-tag workspace
   so it joins the world you're viewing. NOTE: scene_meta_set may realloc the
   object array — set o->parent/o->pos FIRST and never deref o afterwards. */
static void card_move_to_page(AppState *st, sol_u32 handle,
                              sol_u32 board, const char *page) {
    SceneObject *o = scene_get(&st->scene, handle);
    if (!o || board == 0) return;
    if (o->parent != board) {                /* cross-board: re-parent + re-pin + re-world */
        vec3 local = vec3_make(o->pos.x, o->pos.y, 0.0f);
        o->parent = board;
        o->pos    = board_pin_pos(&st->scene, board, handle, local, 0.0f, 0.0f);
        scene_meta_set(&st->scene, handle, "page", page);   /* may realloc; o now stale */
        mint_tag_ws(st, handle);                            /* inherit the target board's world */
        return;
    }
    scene_meta_set(&st->scene, handle, "page", page);       /* same board: just retag the page */
}

/* Cmd+V when a cut is pending: move every cut card to the viewed board's
   active page, consume the cut (cards un-dim), deselect, and persist. */
static void cmd_paste_cut(AppState *st) {
    sol_u32     board = st->board_view;
    char        page[PAGE_SLUG_CAP];
    const char *ap;
    int         i, n;
    if (board == 0 || st->cut_count == 0) return;
    ap = scene_meta_get(&st->scene, board, "active_page");
    /* copy the slug out before any scene_meta_set realloc invalidates the meta ptr */
    snprintf(page, sizeof page, "%s", (ap && ap[0]) ? ap : "/");
    n = st->cut_count;
    for (i = 0; i < n; i++)
        card_move_to_page(st, st->cut[i], board, page);
    st->cut_count = 0;                        /* consume the cut: cards un-dim */
    sel_clear(st);                            /* nothing selected after a paste */
    scene_save(&st->scene, "scene.stml");
    printf("pasted %d card(s) to %s\n", n, page);
}

/* Cmd+X: copy the selection into the cut buffer (cards stay, render dimmed).
   With an EMPTY selection it clears the cut — the explicit "cancel" gesture. */
static void cmd_cut_selection(AppState *st) {
    int i;
    if (st->board_view == 0) return;
    if (st->sel_count == 0) {                 /* cut nothing = cancel the pending cut */
        st->cut_count = 0;
        printf("cut cleared\n");
        return;
    }
    for (i = 0; i < st->sel_count; i++) st->cut[i] = st->sel[i];
    st->cut_count = st->sel_count;
    printf("cut %d card(s)\n", st->cut_count);
}

/* palette wrappers + availability predicates */
static void cmd_paste_cards(AppState *st) { cmd_paste_cut(st); }

static sol_bool can_cut_selection(AppState *st) {
    return (sol_bool)(st->board_view != 0 && st->sel_count > 0);
}
static sol_bool can_paste_cards(AppState *st) {
    return (sol_bool)(st->board_view != 0 && st->cut_count > 0);
}
```

- [ ] **Step 2: Build gauntlet**

Run the three gauntlet commands. Expected: all pass. These functions are defined but not yet
referenced by the registry/handlers/draw — a clean compile confirms signatures and the C89
local-declaration discipline. (If the compiler warns "defined but not used" it is non-fatal here,
but the project builds warning-clean; Tasks 3–5 wire every function, so re-run the gauntlet at the
end of Task 4 if an unused-function warning appears mid-way. It will not, because `c89check` uses
`-Wall` without `-Werror` on these; proceed.)

- [ ] **Step 3: Commit**

```bash
git add main.c
git commit -m "Cut/paste: move helper, paste/cut commands, predicates"
```

---

## Task 3: Dim the cut cards in the draw loop

**Files:**
- Modify: `main.c` — the main scene-object draw call at main.c:14125 (`draw_mesh(state, o->mesh, model, view, proj, eye, hl, dm);`).

- [ ] **Step 1: Route cut cards through the alpha path**

Replace the single draw call at main.c:14125:
```c
            draw_mesh(state, o->mesh, model, view, proj, eye, hl, dm);
```
with:
```c
            if (handle_is_cut(state, o->handle))
                draw_glass(state, o->mesh, model, view, proj, eye, dm);  /* cut: dimmed (Cmd+X) */
            else
                draw_mesh(state, o->mesh, model, view, proj, eye, hl, dm);
```
(`draw_glass` is declared at main.c:13354, before this loop. It renders the same mesh through
the translucent `GLASS_OPACITY` pipeline — no `hl` parameter, which is fine: a cut card shows its
dim state instead of a selection highlight. `state` is the local name in this function — match it
exactly; do not rename to `st` here.)

- [ ] **Step 2: Build gauntlet**

Run the three gauntlet commands. Expected: all pass.

- [ ] **Step 3: Commit**

```bash
git add main.c
git commit -m "Cut/paste: render cut cards dimmed via the draw_glass alpha path"
```

---

## Task 4: Register the palette rows

**Files:**
- Modify: `main.c` — the `g_commands[]` array (main.c:9759-9792).

- [ ] **Step 1: Add the two palette rows**

In `g_commands[]`, find the "Spawn whiteboard" row:
```c
    { "Spawn whiteboard",            "B", GLFW_KEY_B, cmd_mint_whiteboard,   NULL,                  SOL_FALSE },
```
Insert immediately after it:
```c
    { "Cut selected cards",          NULL, 0,          cmd_cut_selection,     can_cut_selection,     SOL_FALSE },
    { "Paste cards",                 NULL, 0,          cmd_paste_cards,       can_paste_cards,       SOL_FALSE },
```
(Both are palette-only — `Cmd+X`/`Cmd+V` are the real shortcuts, added in Task 5. The `can_run`
predicates grey them out unless applicable. The trailing-comma/brace structure of the array is
unchanged — these are interior rows.)

- [ ] **Step 2: Build gauntlet**

Run the three gauntlet commands. Expected: all pass.

- [ ] **Step 3: Manual smoke (optional, controller may skip)**

The palette is GUI; defer to the human live-verify. Build success is the gate here.

- [ ] **Step 4: Commit**

```bash
git add main.c
git commit -m "Cut/paste: palette rows (Cut selected cards / Paste cards)"
```

---

## Task 5: Keyboard shortcuts — Cmd+X cut, contextual Cmd+V

**Files:**
- Modify: `main.c` — the existing `Cmd+V` block (main.c:11539-11546), and add a `Cmd+X` block beside it.

- [ ] **Step 1: Make Cmd+V contextual**

Find the existing Cmd+V block (main.c:11539-11546):
```c
    /* Cmd+V in board view: paste the clipboard image onto the board. */
    {
        sol_bool paste_now = (sol_bool)(
            (glfwGetKey(w, GLFW_KEY_LEFT_SUPER)  == GLFW_PRESS ||
             glfwGetKey(w, GLFW_KEY_RIGHT_SUPER) == GLFW_PRESS) &&
            glfwGetKey(w, GLFW_KEY_V) == GLFW_PRESS);
        if (paste_now && !st->paste_was_down && st->board_view != 0)
            cmd_paste_image(st, w);
        st->paste_was_down = paste_now;
    }
```
Replace the body's `if` line so paste is contextual:
```c
    /* Cmd+V in board view: paste cut cards if a cut is pending, else the
       clipboard image (Finder-style: paste whatever's on the clipboard). */
    {
        sol_bool paste_now = (sol_bool)(
            (glfwGetKey(w, GLFW_KEY_LEFT_SUPER)  == GLFW_PRESS ||
             glfwGetKey(w, GLFW_KEY_RIGHT_SUPER) == GLFW_PRESS) &&
            glfwGetKey(w, GLFW_KEY_V) == GLFW_PRESS);
        if (paste_now && !st->paste_was_down && st->board_view != 0) {
            if (st->cut_count > 0) cmd_paste_cut(st);
            else                   cmd_paste_image(st, w);
        }
        st->paste_was_down = paste_now;
    }
```

- [ ] **Step 2: Add the Cmd+X block**

Immediately after the closing `}` of the Cmd+V block, add:
```c
    /* Cmd+X in board view: cut the selection (cards stay, render dimmed). With
       nothing selected it cancels a pending cut. */
    {
        sol_bool cut_now = (sol_bool)(
            (glfwGetKey(w, GLFW_KEY_LEFT_SUPER)  == GLFW_PRESS ||
             glfwGetKey(w, GLFW_KEY_RIGHT_SUPER) == GLFW_PRESS) &&
            glfwGetKey(w, GLFW_KEY_X) == GLFW_PRESS);
        if (cut_now && !st->cut_was_down && st->board_view != 0)
            cmd_cut_selection(st);
        st->cut_was_down = cut_now;
    }
```
(These edge-detected polling blocks sit in the same per-frame input section as the existing Cmd+V
and `d`-key blocks; `st` and `w` are in scope there. `GLFW_KEY_X` is otherwise the ghost-toggle
key but registered hotkeys are disabled in board view, and this is a `SUPER`-modified combo, so
there is no conflict.)

- [ ] **Step 3: Build gauntlet**

Run the three gauntlet commands. Expected: all pass.

- [ ] **Step 4: Commit**

```bash
git add main.c
git commit -m "Cut/paste: Cmd+X cut + contextual Cmd+V (cut cards else image)"
```

---

## Task 6: Drop deleted cards from the cut buffer

A cut card can be deleted (Delete/Backspace) before it is pasted. Every board-card delete route
funnels through `delete_board_card` (main.c:9933) — the single-card path and the group-delete loop
at main.c:11438 both call it — so dropping the handle there covers all routes. Reuse the
already-tested `msel_remove`.

**Files:**
- Modify: `main.c` — `delete_board_card` (main.c:9933-9945).

- [ ] **Step 1: Compact the cut buffer on delete**

In `delete_board_card`, find the existing handle-clearing lines, e.g.:
```c
    if (st->selected_handle    == h) st->selected_handle    = 0;
    scene_remove(&st->scene, h);
```
Insert a `msel_remove` call just before `scene_remove`:
```c
    if (st->selected_handle    == h) st->selected_handle    = 0;
    msel_remove(st->cut, &st->cut_count, h);   /* a cut card was deleted: drop it */
    scene_remove(&st->scene, h);
```
(`msel_remove(sol_u32 *set, int *len, sol_u32 h)` removes `h` while preserving order and is a
no-op if `h` is absent — declared in `multiselect.h`, already included. It is unit-tested in
`multiselect_test.c`, so no new test is needed.)

- [ ] **Step 2: Build gauntlet**

Run the three gauntlet commands. Expected: all pass.

- [ ] **Step 3: Run the multiselect test (confirms msel_remove still green)**

```bash
./build.sh multiselecttest && ./multiselect_test
```
Expected: the existing assertions pass (this is the suite that covers `msel_remove`). If the
`multiselecttest` target name differs, check `build.sh` for the exact target; the binary is
`./multiselect_test`.

- [ ] **Step 4: Commit**

```bash
git add main.c
git commit -m "Cut/paste: drop a deleted card from the cut buffer (msel_remove)"
```

---

## Final verification (controller, after all tasks)

- [ ] **Full gauntlet once more:**
```bash
./build.sh c89check && ./build.sh && ./build.sh metal
```
All three must pass.

- [ ] **Dispatch a final holistic code review** over the whole diff (spec compliance + code
  quality), then hand to the human for live-verify.

## Human live-verify checklist (post-merge, both backends)

- Cut a single note (`Cmd+X`) → it dims. Navigate to another page (arrow / double-click a folder).
  `Cmd+V` → the note appears there at the same spot; the origin page no longer shows it. Reload
  (`L`) → the move persisted.
- Multi-select a note + a picture + a folder, `Cmd+X` → all dim. Paste on another page → all move,
  relative layout preserved.
- Cross-board: cut on board A, `Esc` out (cards stay dimmed in the walkaround), open board B,
  `Cmd+V` → the cards appear on board B's active page, correct workspace.
- `Cmd+V` with an **empty** cut buffer but an image on the system clipboard → still pastes the
  image (contextual fall-through intact).
- Cancel a cut: deselect (click empty board) → `Cmd+X` → cards un-dim; a later `Cmd+V` pastes the
  clipboard image.
- Delete a cut card (Delete) → no crash; a subsequent paste does not reference it.

## Known v1 limitation (documented, not a bug)

Moving a **folder** re-tags the page it sits on but does not rewrite its idempotent backlink, so
the back-arrow can point at the old page — like the existing "stacked backlinks" limitation.
Backlink rewriting is deferred.

---

## Self-review notes (author)

- **Spec coverage:** §1 interaction → Tasks 4–5; §2 cut-buffer state → Task 1; §3 Cmd+X handler →
  Task 5 + `cmd_cut_selection` (Task 2); §3 contextual Cmd+V → Task 5; §4 `card_move_to_page` →
  Task 2; §5 dim draw → Task 3; §6 clearing (paste/new-cut/empty-cut-cancel/delete) → Tasks 2, 5,
  6; §7 palette rows → Task 4. All covered.
- **Symbol consistency:** `cut`/`cut_count`/`cut_was_down`, `handle_is_cut`, `card_move_to_page`,
  `cmd_paste_cut`, `cmd_cut_selection`, `cmd_paste_cards`, `can_cut_selection`, `can_paste_cards`
  used identically across tasks. `draw_glass`/`draw_mesh`/`board_pin_pos`/`mint_tag_ws`/`sel_clear`/
  `msel_remove`/`scene_meta_set`/`scene_meta_get`/`PAGE_SLUG_CAP` are all existing symbols verified
  in the source.
- **Ordering:** the Task 2 block is placed before `g_commands[]` (9759) and the draw loop (14125),
  so every reference resolves. `cmd_paste_image` (9968) and the input handlers (11539) are after
  the block, which is fine — they call into it.
