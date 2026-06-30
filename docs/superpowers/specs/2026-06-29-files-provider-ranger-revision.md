# Files Provider — Ranger Interaction (Revision)

**Date:** 2026-06-29
**Status:** Approved (live-verify feedback on the original Files provider), ready for plan
**Supersedes** the interaction model in `2026-06-29-files-provider-design.md` **for the Files provider only.** Pictures/Places keep the original 3-column Types→Entities→Commands behavior.

## Why

The original Files provider used the shared 3-column model: to walk into a folder you selected it, went `→` to a Commands column, and picked "Open." During live-verify that felt clunky — a filesystem wants `l` to *just enter*. The fix turns the Files column into a ranger-style two-pane: actions + contents in the middle, a live preview on the right.

## Decisions (from the design dialogue)

- **Files-only.** Pictures/Places are untouched; the browser shell branches on the Files provider.
- **Actions act on the *current folder*** (the directory you're viewing), not the highlighted child. To Mount/Carry a specific folder you walk into it first.
- **Right column becomes a preview** of the highlighted row (folder → its contents as text; image → the image; else blank). Display-only, not focusable.
- **`Shift+H` returns to the Types column;** plain `h`/`←` always walks up a directory.

## The interaction (Files provider)

Middle column (the Entities column) = **`Mount as Root` · `Carry` · ─divider─ · `../` · folders · files.** One selection runs through all rows.

| Key | action row (Mount/Carry) | `../` | folder | file |
|---|---|---|---|---|
| `l`/`→`/`Enter` | run on the **current** folder, close | go up | **drill in** (re-list) | **Carry**, close |
| `j`/`k`/`↑`/`↓` | move selection across actions + contents | | | |
| `h`/`←` | **up a directory** (re-list); no-op at `/` | | | |
| `Shift+H` | focus the **Types** column | | | |
| `/` | filter the **contents** (action rows stay pinned) | | | |
| `Esc`/`;` | close | | | |

Right column = preview of the highlighted row's target: a folder (incl. an action row → the current folder, or `../` → the parent) lists its contents as text; an image file shows the image; anything else is blank.

## Implementation approach (shell branches on the Files provider; reuses all guts)

Everything from the original build stays: `diskpath`, `fs_home_dir`/`fs_is_dir`/`fs_scan_dir`, `create_root_from_path`, `disk_carry`, `disk_enumerate` (still emits `../` + folders + files — **contents only**). The action rows are *virtual* (injected by the shell), so `disk_enumerate` barely changes.

1. **`provider_is_files(ti)`** helper — `strcmp(g_providers[ti].name, "Files") == 0`. The shell uses it to branch; no `TypeProvider` struct change.
2. **Virtual action rows via the count.** `browser_items`/`browser_ent_order` stay **contents-only**. In Files mode the shell treats column 1 as `FILES_ACTIONS (=2)` virtual rows followed by the `browser_ent_n` content rows: `browser_columns_counts` returns `browser_ent_n + 2` for col 1. `sel[1] < 2` → an action row; `sel[1] >= 2` → content row `browser_ent_order[sel[1]-2]`.
3. **Nav interception** in `browser_handle_key`, only when `provider_is_files(ti) && focus==1 && !filtering`:
   - `RIGHT`/`ENTER` → **activate** `sel[1]`: 0 = `create_root_from_path(cwd)` (close); 1 = `disk_carry(cwd, SOL_TRUE)` (close); else the content ref → if `../` or `fs_is_dir(ref)` drill (set `cwd=ref`, `browser_items_type=-1`, `sel[1]=0`, clear filter[1], refresh, **stay**), else `disk_carry(ref, SOL_FALSE)` (close).
   - `LEFT` → **up a directory**: if `!diskpath_is_root(cwd)` set `cwd=diskpath_parent(cwd)`, invalidate, `sel[1]=0`, refresh, stay; else no-op.
   - `UP`/`DOWN`/`FILTER`/`CANCEL` fall through to generic `browser_key`. When `focus==0` (Types), all keys are generic (so `l`/`→` still enters the column).
4. **`Shift+H`** handled in `on_key` (needs the GLFW `SHIFT` mod): when the browser is open, not filtering, Files mode, and `focus==1`, `GLFW_KEY_H` with `GLFW_MOD_SHIFT` → `browser.focus = 0` + `browser_refresh`. Must be checked before the lowercase `h`→`BROWSER_KEY_LEFT` mapping; suppress the `H` reaching `on_char`.
5. **Rendering** (`browser_draw_overlay`), Files mode:
   - Column 1: draw the 2 action rows (`Mount as Root`, `Carry`), a divider line, then the content rows (the existing scroll-window over `browser_ent_order`). The selection highlight maps `sel[1]<2` → action row, else content row.
   - Column 2: replace the command-list/image block with the **preview pane** — if the highlighted target is a folder, draw its contents as text lines; if an image, draw the texture (the existing `browser_preview_tex` path); else blank. No focus outline (not focusable).
6. **Folder-contents preview cache.** New `AppState` fields hold a small text listing (e.g. `char browser_dir_preview[ROWS][BROWSER_NAME_CAP]; int browser_dir_preview_n; char browser_dir_preview_ref[BROWSER_REF_CAP];`). Built frame-top (next to `browser_ensure_preview`) when the highlighted *folder* target changes: resolve the target (cwd for an action row, parent for `../`, the ref for a folder), `fs_scan_dir` it, copy up to ROWS names. The image preview keeps using `browser_ensure_preview`/`disk_preview` (folders already return `{.id=0}` there, so no image for folders).
7. **`disk_commands` for Files** returns 0 (the col-2 command list is unused in Files mode); `browser_refresh` still calls it harmlessly.

## Defaults chosen (flagged for confirmation in review)
- `h`/`←` at the filesystem root `/` is a no-op (Shift+H is the way back to Types).
- An action row highlighted → preview shows the **current folder's** contents.
- Non-image files → no preview. Dotfiles stay hidden.
- Drilling/ascending resets `sel[1]=0` (top of the new listing).
- Filtering (`/`) narrows the **contents** via the existing `browser_rank`; the 2 action rows are always present (they're virtual, outside the ranked list).

## Out of scope (still deferred)
File-info preview for non-images; folder preview recursion/icons; remembering selection position when ascending (lands at top); applying the ranger model to Pictures/Places.

## Testing
No new pure module (this is shell wiring). `browsertest` must stay green (the pure `browser.c` nav state is unchanged — all Files nav is shell-side). `diskpathtest` stays green. Full gauntlet (build/c89check/asan/metal) each task. **No new shader → no MSL twin.** Fran live-verifies the interaction (drill, up, Shift+H, mount, carry, folder/image preview).
