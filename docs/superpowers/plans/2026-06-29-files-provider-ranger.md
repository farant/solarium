# Files Provider — Ranger Interaction: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Reshape the Files provider's entity-browser interaction into a ranger two-pane: middle column = `Mount as Root` · `Carry` · divider · `../`/folders/files (with `l` drilling, `h` ascending, `Shift+H` → Types); right column = a live preview of the highlighted item (folder contents as text, or the image). Files-only; Pictures/Places unchanged.

**Architecture:** Shell-side branches on `provider_is_files(ti)`. The action rows are *virtual* — `browser_items` stays contents-only, and the shell presents column 1 as `FILES_ACTIONS (=2)` rows + the `browser_ent_n` content rows (`sel[1] < 2` → action, else content `sel[1]-2`). Nav is intercepted in `browser_handle_key`/`on_key`; rendering and the preview are branched in `browser_draw_overlay`. Reuses `diskpath`, `fs_*`, `create_root_from_path`, `disk_carry`, `disk_enumerate` unchanged. No new shader, no `browser.c` (pure) change.

**Tech Stack:** C89 strict (`-std=c89 -pedantic-errors -Werror -Wall -Wextra`), OpenGL + Metal, GLFW input. All edits in `main.c`.

**Conventions (every task):** strict C89 (decls at top of block, no `//`); stage ONLY `main.c`; NEVER `git add` NOTES.stml / paper-picture.png; commit body ends with `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`; don't run `./solarium*`. Gauntlet per task: `./build.sh`, `./build.sh c89check`, `./build.sh asan`, `./build.sh metal`, `./build.sh browsertest && ./browser_test`, `./build.sh diskpathtest && ./diskpath_test`.

Branch: `files-provider` (tip `6f495d3`). The original Files provider is built; this revises its interaction.

---

## Reference: current code shape (read before editing)

- `provider block` (main.c ~15394–15510): `disk_carry`, `disk_enumerate`, `disk_commands`, `disk_run`, `disk_preview`, then `g_providers[]` + `G_PROVIDER_COUNT`.
- `browser_refresh` (~15530): ranks columns, lazily enumerates, builds `browser_cmds`, clamps. Sets `counts[1] = browser_ent_n` today.
- `browser_columns_counts` (~15560): writes `counts[0..2]` from `browser_type_n/ent_n/cmd_n`.
- `browser_open_now` (~15553), `browser_handle_key` (~15562): nav dispatch; `BROWSER_ACTIVATE` branch runs `g_providers[ti].run` honoring stay/close.
- `browser_ensure_preview` (~15585): frame-top; resolves the focused ref from `browser_ent_order[sel[1]]`, calls provider `preview`.
- `browser_draw_overlay` (~15600+): draws the dim backdrop + 3 columns; the `col==2` block draws `browser_preview_tex` (if id) above the command-list rows; each column has a centered scroll window + scrollbar.
- `on_key` browser block (~17870): maps GLFW keys → `BrowserKey`; nav-only keys (`hjkl`, Left/Right, `/`) gated on `!filtering`. Signature provides `int mods` (has `GLFW_MOD_SHIFT`).
- `on_char` browser hook (~17744): in nav mode it returns (swallows the char).

---

## Task R-A: Files ranger interaction + column-1 rendering

**Files:** Modify `main.c`.

Makes column 1 behave and look ranger: virtual action rows, `l`/Enter activation, `h` ascend, `Shift+H` → Types, and the action-rows+divider+contents rendering. After this task the Files column is fully navigable; column 2 is left as-is (handled in R-B).

- [ ] **Step 1: `provider_is_files` helper + `FILES_ACTIONS`**

Immediately after the `g_providers[]` / `G_PROVIDER_COUNT` block, add:
```c
#define FILES_ACTIONS 2   /* virtual rows atop the Files Entities column: Mount as Root, Carry */

/* The Files provider uses the ranger interaction (drill with l, ascend with h,
   a preview pane). Detected by name so no TypeProvider struct change is needed. */
static sol_bool provider_is_files(int ti) {
    return (ti >= 0 && ti < G_PROVIDER_COUNT &&
            strcmp(g_providers[ti].name, "Files") == 0) ? SOL_TRUE : SOL_FALSE;
}
```

- [ ] **Step 2: count the virtual action rows (both count sites)**

In `browser_columns_counts`, make col 1 include the action rows for Files:
```c
static void browser_columns_counts(AppState *st, int counts[3]) {
    int ti = (st->browser_type_n > 0) ? st->browser_type_order[st->browser.sel[0]] : -1;
    counts[0] = st->browser_type_n;
    counts[1] = st->browser_ent_n + (provider_is_files(ti) ? FILES_ACTIONS : 0);
    counts[2] = st->browser_cmd_n;
}
```
In `browser_refresh`, the local `counts[1]` used for the final `browser_clamp` must match. Find where it sets `counts[1] = st->browser_ent_n;` (just before `browser_clamp`) and change it to:
```c
    counts[1] = st->browser_ent_n + (provider_is_files(ti) ? FILES_ACTIONS : 0);
```
(`ti` is already computed in `browser_refresh`.)

- [ ] **Step 3: Files nav interception in `browser_handle_key`**

At the very top of `browser_handle_key` (before the existing `browser_columns_counts`/`browser_key` call), insert the Files branch. It handles RIGHT/ENTER (activate) and LEFT (ascend) when focused on column 1 in nav mode; everything else falls through to the existing generic path:
```c
    int ti = (st->browser_type_n > 0) ? st->browser_type_order[st->browser.sel[0]] : -1;
    if (provider_is_files(ti) && st->browser.focus == 1 && !st->browser.filtering &&
        (pk == BROWSER_KEY_RIGHT || pk == BROWSER_KEY_ENTER || pk == BROWSER_KEY_LEFT)) {
        if (pk == BROWSER_KEY_LEFT) {                         /* up a directory */
            if (!diskpath_is_root(st->browser_disk_cwd)) {
                char parent[1024];
                diskpath_parent(st->browser_disk_cwd, parent, (int)sizeof parent);
                strncpy(st->browser_disk_cwd, parent, sizeof st->browser_disk_cwd - 1);
                st->browser_disk_cwd[sizeof st->browser_disk_cwd - 1] = '\0';
                st->browser_items_type   = -1;
                st->browser.sel[1]       = 0;
                st->browser.filter[1][0] = '\0';
                st->browser.flen[1]      = 0;
                browser_refresh(st);
            }
            return;
        }
        if (st->browser.sel[1] == 0) {                        /* Mount as Root (current folder) */
            create_root_from_path(st, st->browser_disk_cwd);
            st->browser_open = SOL_FALSE;
            return;
        }
        if (st->browser.sel[1] == 1) {                        /* Carry (current folder) */
            disk_carry(st, st->browser_disk_cwd, SOL_TRUE);
            st->browser_open = SOL_FALSE;
            return;
        }
        {                                                     /* a content row */
            int idx = st->browser.sel[1] - FILES_ACTIONS;
            if (idx >= 0 && idx < st->browser_ent_n) {
                const char *ref = st->browser_items[st->browser_ent_order[idx]].ref;
                if (fs_is_dir(ref)) {                          /* ../ or folder: drill in */
                    strncpy(st->browser_disk_cwd, ref, sizeof st->browser_disk_cwd - 1);
                    st->browser_disk_cwd[sizeof st->browser_disk_cwd - 1] = '\0';
                    st->browser_items_type   = -1;
                    st->browser.sel[1]       = 0;
                    st->browser.filter[1][0] = '\0';
                    st->browser.flen[1]      = 0;
                    browser_refresh(st);
                } else {                                       /* file: Carry */
                    disk_carry(st, ref, SOL_FALSE);
                    st->browser_open = SOL_FALSE;
                }
            }
            return;
        }
    }
```
Declarations (`int ti;`, and the inner `char parent[1024];`/`int idx;`) must be at the top of their blocks (C89). Note `disk_carry`/`create_root_from_path`/`diskpath_*`/`fs_is_dir` are all defined earlier in the file and in scope here.

- [ ] **Step 4: fix `browser_ensure_preview` indexing for the offset**

`browser_ensure_preview` resolves the focused ref to build the image preview. In Files mode, `sel[1]` is offset by `FILES_ACTIONS` and rows `0..1` are actions (no image). Make it Files-aware. Find the ref resolution and replace with:
```c
    if (provider_is_files(ti)) {
        if (st->browser.sel[1] < FILES_ACTIONS) ref = (const char *)0;     /* action row: no image */
        else {
            int idx = st->browser.sel[1] - FILES_ACTIONS;
            ref = (st->browser_ent_n > 0 && idx < st->browser_ent_n)
                  ? st->browser_items[st->browser_ent_order[idx]].ref : (const char *)0;
        }
    } else {
        ref = (st->browser_ent_n > 0)
              ? st->browser_items[st->browser_ent_order[st->browser.sel[1]]].ref : (const char *)0;
    }
```
(`disk_preview` already returns `{.id=0}` for a folder ref, so a highlighted folder yields no image here — the folder's text preview is R-B.)

- [ ] **Step 5: `Shift+H` → Types column, in `on_key`**

In the `on_key` browser block's nav-only branch (`else if (!st->browser.filtering) { ... }`), BEFORE the `key == GLFW_KEY_H` → `BROWSER_KEY_LEFT` mapping, add a Shift+H case that jumps to the Types column for Files and consumes the key:
```c
                if (key == GLFW_KEY_H && (mods & GLFW_MOD_SHIFT)) {
                    int ti = (st->browser_type_n > 0)
                           ? st->browser_type_order[st->browser.sel[0]] : -1;
                    if (provider_is_files(ti) && st->browser.focus == 1) {
                        st->browser.focus = 0;
                        browser_refresh(st);
                    }
                    /* pk stays NONE: consume Shift+H, don't fall through to LEFT */
                } else if (key == GLFW_KEY_LEFT  || key == GLFW_KEY_H) pk = BROWSER_KEY_LEFT;
                else if (key == GLFW_KEY_RIGHT || key == GLFW_KEY_L) pk = BROWSER_KEY_RIGHT;
                else if (key == GLFW_KEY_K)     pk = BROWSER_KEY_UP;
                else if (key == GLFW_KEY_J)     pk = BROWSER_KEY_DOWN;
                else if (key == GLFW_KEY_SLASH) pk = BROWSER_KEY_FILTER;
```
(Keep the rest of the chain intact; only the leading `if`/`else if` for H changes. `int ti;` is declared at the top of that brace block.) `on_char` already swallows the capital `H` in nav mode, so no double-handling.

- [ ] **Step 6: render column 1 as actions + divider + contents (Files mode)**

In `browser_draw_overlay`, the per-column loop draws `counts[col]` rows from `*_order[r]`. For Files, column 1's first `FILES_ACTIONS` rows are the action labels and the rest are contents (`browser_ent_order[r - FILES_ACTIONS]`), with a divider line drawn between them. Implement a Files branch in the `col == 1` row-drawing so that:
- The label for row `r` is: `r == 0` → `"Mount as Root"`, `r == 1` → `"Carry"`, else `st->browser_items[st->browser_ent_order[r - FILES_ACTIONS]].name`.
- After drawing the action rows and before the first content row, draw a thin horizontal divider line (`ui_quad` 1px, dim, full column width) at that y.
- The selection highlight stays keyed on `r == st->browser.sel[col]` (unchanged — `sel[1]` already spans the virtual list).

Match the existing row loop's coordinates/colors. The simplest implementation: keep the existing scroll-window loop, but when `col == 1 && provider_is_files(ti)`, compute the label via the mapping above and, when transitioning from the last action row to the first content row within the visible window, draw the divider. (`ti` for the active type is `st->browser_type_order[st->browser.sel[0]]` when `browser_type_n>0`.) Read the existing `col==0/1/2` label selection and reuse its row geometry.

- [ ] **Step 7: gauntlet + commit**

Run the full gauntlet (all must pass; `browsertest`/`diskpathtest` are regression guards — the pure modules are untouched):
```
./build.sh && ./build.sh c89check && ./build.sh asan && ./build.sh metal \
  && ./build.sh browsertest && ./browser_test && ./build.sh diskpathtest && ./diskpath_test
```
Commit:
```bash
git add main.c
git commit -m "$(cat <<'EOF'
files-provider: ranger interaction (drill, ascend, action rows)

Files Entities column becomes ranger: 2 virtual action rows (Mount as
Root, Carry on the current folder) + divider + contents; l/Enter drills a
folder or carries a file, h ascends a directory, Shift+H returns to Types.
Files-only; browser.c (pure) untouched.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task R-B: the preview pane (column 2)

**Files:** Modify `main.c`.

Replaces the Files column-2 command list with a preview of the highlighted row: a folder's contents as text, an image, or blank.

- [ ] **Step 1: `disk_commands` returns 0 for Files**

`disk_commands` is now unused for Files (column 2 is a preview, not a command list). Make it return 0 so `browser_cmd_n == 0` for Files:
```c
static int disk_commands(AppState *st, const char *ref, const char **out, int cap) {
    (void)st; (void)ref; (void)out; (void)cap;
    return 0;   /* Files uses the ranger preview pane, not a command list */
}
```

- [ ] **Step 2: AppState fields for the folder-contents preview cache**

Add near the other `browser_*` fields in `struct AppState`:
```c
    char browser_dir_preview[24][BROWSER_NAME_CAP]; /* highlighted folder's contents (text preview) */
    int  browser_dir_preview_n;                     /* lines used; -1 when the target isn't a folder */
    char browser_dir_preview_ref[BROWSER_REF_CAP];  /* the target whose listing is cached */
```

- [ ] **Step 3: build the folder-contents cache frame-top**

Add a `browser_ensure_dir_preview(AppState *st)` (next to `browser_ensure_preview`, called from the same frame-top site). It resolves the *target folder* for the highlighted Files row and caches up to 24 names:
```c
static void browser_ensure_dir_preview(AppState *st) {
    int         ti = (st->browser_type_n > 0) ? st->browser_type_order[st->browser.sel[0]] : -1;
    const char *target = (const char *)0;
    FsListing   l;
    int         i, n;
    if (!provider_is_files(ti)) { st->browser_dir_preview_n = -1; st->browser_dir_preview_ref[0] = '\0'; return; }
    if (st->browser.sel[1] < FILES_ACTIONS) {
        target = st->browser_disk_cwd;                 /* action row -> the current folder */
    } else {
        int idx = st->browser.sel[1] - FILES_ACTIONS;
        if (st->browser_ent_n > 0 && idx < st->browser_ent_n) {
            const char *ref = st->browser_items[st->browser_ent_order[idx]].ref;
            if (fs_is_dir(ref)) target = ref;          /* ../ or a folder */
        }
    }
    if (!target) { st->browser_dir_preview_n = -1; st->browser_dir_preview_ref[0] = '\0'; return; }
    if (strcmp(target, st->browser_dir_preview_ref) == 0) return;   /* unchanged */
    strncpy(st->browser_dir_preview_ref, target, BROWSER_REF_CAP - 1);
    st->browser_dir_preview_ref[BROWSER_REF_CAP - 1] = '\0';
    n = 0;
    if (fs_scan_dir(target, &l)) {
        for (i = 0; i < l.count && n < 24; i++) {
            strncpy(st->browser_dir_preview[n], l.entries[i].name, BROWSER_NAME_CAP - 1);
            st->browser_dir_preview[n][BROWSER_NAME_CAP - 1] = '\0';
            n++;
        }
        fs_listing_free(&l);
    }
    st->browser_dir_preview_n = n;
}
```
Call it right after `browser_ensure_preview(state)` at the frame-top site (find where `browser_ensure_preview` is invoked and add `browser_ensure_dir_preview(state);` beside it). Reset both caches in `browser_open_now` (`st->browser_dir_preview_n = -1; st->browser_dir_preview_ref[0] = '\0';`).

- [ ] **Step 4: draw the preview in column 2 (Files mode)**

In `browser_draw_overlay`'s `col == 2` handling, branch for Files: do NOT draw the command-list rows; instead draw the preview pane:
- If `st->browser_preview_tex.id` (an image is highlighted) → draw the existing letterboxed `ui_textured_quad_flip` image block (reuse the current image-preview code).
- Else if `st->browser_dir_preview_n > 0` → draw those names as text lines (`ui_text`, same row geometry/colors as the list rows, no selection highlight), within the column's scroll area; if `> 24` were truncated it's fine (cap).
- Else → leave blank (just the column background).
Do not draw the focus outline for column 2 in Files mode (it isn't focusable). Pictures/Places keep the existing `col==2` behavior unchanged.

- [ ] **Step 5: gauntlet + commit**

Run the full gauntlet (as in R-A). Commit:
```bash
git add main.c
git commit -m "$(cat <<'EOF'
files-provider: ranger preview pane (column 2)

The Files column 2 becomes a preview of the highlighted row: a folder's
contents as text, an image file as the image, else blank. disk_commands
returns 0 for Files. Pictures/Places column 2 unchanged.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task R-C: final holistic review + gauntlet + live-verify handoff

**Files:** none (verification).

- [ ] **Step 1:** Read R-A + R-B together. Verify the `sel[1]` offset (`FILES_ACTIONS`) is applied CONSISTENTLY at every site: `browser_columns_counts`, `browser_refresh` clamp, `browser_handle_key` activation, `browser_ensure_preview` image indexing, `browser_ensure_dir_preview`, and the column-1/column-2 rendering. A mismatch by 2 at any one site = wrong row acts/previews. Confirm Pictures/Places are byte-unchanged in behavior (the `provider_is_files` guards all default to the old path).
- [ ] **Step 2:** Full gauntlet from clean: `./build.sh && ./build.sh c89check && ./build.sh asan && ./build.sh metal && ./build.sh browsertest && ./browser_test && ./build.sh diskpathtest && ./diskpath_test`.
- [ ] **Step 3:** Hand off to Fran for GUI live-verify (both backends). Checklist: Files column shows `Mount as Root`/`Carry`/divider/`../`+folders+files; `l`/`j`/`k` move + `l` drills a folder without picking Open; `h` ascends; `Shift+H` returns to Types; `l`/Enter on `Mount as Root` mounts the current folder; on `Carry` carries it; on a file carries it; the right pane previews a highlighted folder's contents as text and an image file as the image; Pictures/Places still behave as before. After Fran confirms, use **superpowers:finishing-a-development-branch** to ff-merge `files-provider` to `main`.

---

## Self-Review

**Spec coverage:** action rows on current folder (R-A Steps 1-3,6) ✓ · `l` drills / Enter activates / file carries (R-A Step 3) ✓ · `h` ascends, no-op at root (R-A Step 3) ✓ · `Shift+H` → Types (R-A Step 5) ✓ · `/` filters contents, actions pinned (virtual rows are outside the ranked list, so filtering narrows `browser_ent_n` only — R-A Step 2 counts) ✓ · right pane = folder text / image / blank (R-B Steps 3-4) ✓ · Files-only, Pictures/Places unchanged (every change guarded by `provider_is_files`) ✓ · reuse diskpath/fs/mount/carry, no new shader (no provider-guts or shader edits) ✓.

**Placeholder scan:** Logic steps carry exact code. Rendering steps (R-A Step 6, R-B Step 4) give the precise label-mapping/branch rules and point at the existing row loop to match geometry — acceptable because the geometry is pre-existing and must be matched, not invented; the implementer reads `browser_draw_overlay` and extends it.

**Type consistency:** `FILES_ACTIONS` and `provider_is_files(int)` defined in R-A Step 1, used identically in every later site. `sel[1]-FILES_ACTIONS` indexing matches between activation, preview, dir-preview, and rendering. Cache fields (R-B Step 2) match their use in Steps 3-4.
