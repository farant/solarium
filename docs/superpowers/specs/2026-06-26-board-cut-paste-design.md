# Cut & Paste Board Cards — Design Spec

**Date:** 2026-06-26
**Status:** Approved (brainstorm), pending implementation plan
**Author:** Fran + Claude

## Goal

In board view, **cut** the selected card(s) with `Cmd+X` and **paste** them onto another
page (or another board entirely) with `Cmd+V` — the Finder file-cut/paste model. A cut does
**not** remove the card; it marks it. Pasting **moves** the card(s) to the board's currently
active page, preserving their board-local layout. Works on a single card or a multi-select
group, across pages of one board and across different boards.

## Decisions (from brainstorming)

- **`Cmd+V` is contextual (Finder-like).** If the cut buffer is non-empty, `Cmd+V` pastes the
  cut card(s); otherwise it falls through to the existing clipboard-image paste
  (`cmd_paste_image`). One key, like Finder pasting "whatever's on the clipboard."
- **Paste keeps original layout.** Each card keeps its board-local position; a group keeps
  its relative arrangement. Paste is a *move to this page*, not a re-place.
- **Cut cards are dimmed.** Cut cards render translucent (the existing `draw_glass`
  `GLASS_OPACITY` alpha path, no new shader) until pasted or cleared, so the pending cut is
  visible.
- **Cross-board is in scope.** Cut on one board, navigate to a different board (or sub-page),
  paste. The cut buffer is **global** and survives leaving board view.
- **Palette rows for discoverability.** Add palette-only "Cut selected" and "Paste cards"
  commands alongside the `Cmd+X` / `Cmd+V` shortcuts.
- **Folder backlink-rewrite is deferred (known limitation).** Moving a folder re-tags where
  it *sits*; its idempotent backlink still points at the old page, so the back-arrow can go
  stale — treated as a known minor limitation like the existing "stacked backlinks" one, not
  fixed in v1.

## Non-Goals

- No backlink rewriting when a folder is moved (flagged limitation above).
- No copy/duplicate (`Cmd+C`) — cut/move only. Copy can come later.
- No cross-workspace UX beyond what re-parenting already implies (the card inherits the
  target board's workspace via the parent chain).
- No undo of a paste (the move persists immediately via `scene_save`, like every other board
  edit).
- Applies only in board view; no world/editor cut/paste.

## Background (current state)

- **Board cards** are board-children: `KIND_NOTE` (notes), `KIND_PLAIN` `mesh_ref="picture"`
  (pictures), `KIND_PLAIN` `mesh_ref="folderbook"` (folders). Each pins proud of the board
  face; its `pos` is a world position derived from a board-local offset via
  `board_pin_pos(scene, board, card, local, ox, oy)` (main.c:4222). `board_under_ray`
  (main.c:4159) returns the board-local hit; `board_local_frac(st, board, fx, fy)`
  (main.c:7804) maps a fractional face coord to a board-local point.
- **Pages** (board-pages): a card carries `meta["page"]` (absent ⇒ `/`); the board carries
  `meta["active_page"]` (absent ⇒ `/`). The **page gate** in `scene_object_active`
  (workspace.c) hides any board-child whose page ≠ the board's active page.
  `board_card_tag_page(st, handle)` (main.c:7680) tags a board-child with its board's active
  page. **LAW: anything pinned to a board in board view must inherit the board's active_page
  or it leaks to root.**
- **Multi-select** (multiselect): `sel[MULTISEL_CAP]` / `sel_count` on `AppState`
  (main.c:2914), invariant `sel_count==0 ⇒ selected_handle==0`, `==1 ⇒ ==sel[0]`.
  `sel_clear(st)` (main.c:7657) empties it. Cleared in `board_view_exit` (main.c:9120).
- **Existing `Cmd+V`** (main.c:11539-11544): edge-detected via `st->paste_was_down`,
  `board_view != 0`-gated, calls `cmd_paste_image(st, w)` (main.c:9968).
- **`draw_glass`** (main.c:13354) renders a mesh through the alpha pipeline at
  `GLASS_OPACITY 0.6` (main.c:13350) — used today for `church_glass` and the place-mode
  ghost. No new shader / MSL twin.
- **`mint_tag_ws(st, h)`** (main.c:7918) tags an object with the active workspace. **LAW
  (board-image-pictures):** a detach to `parent=0` in a named workspace orphans an item to
  "home" (hidden) — but cut/paste always re-parents to a board (never 0), so workspace is
  inherited via the parent chain; no orphan.

## Architecture

### 1. Cut-buffer state — `AppState` (main.c)

New fields beside the multi-select set:

```c
sol_u32  cut[MULTISEL_CAP];   /* the cut card handles; parallel to sel[] */
int      cut_count;           /* 0 = nothing cut */
sol_bool cut_was_down;        /* edge-detect for Cmd+X */
```

- **Global lifetime.** Unlike `sel[]`, the cut buffer is **NOT** cleared in
  `board_view_exit` — that is what lets you cut on board A, leave board view, enter board B,
  and paste. Initialize `cut_count = 0` in the AppState init block (near
  `paste_was_down = SOL_FALSE`, main.c:12199).

### 2. `Cmd+X` — cut the selection (main.c, near the `Cmd+V` block ~11539)

```
paste-style edge-detect: (LEFT_SUPER||RIGHT_SUPER) && KEY_X, gated board_view != 0
on the rising edge:
    if sel_count == 0: no-op (nothing to cut)
    else: copy sel[0..sel_count) into cut[], set cut_count = sel_count
          printf("cut %d card(s)\n", cut_count)
track st->cut_was_down
```

The cut is selection-based and replaces any previous cut. The cards are not touched.

### 3. `Cmd+V` — contextual paste (modify the existing handler, main.c:11542)

```
on the rising edge, board_view != 0:
    if st->cut_count > 0: cmd_paste_cut(st)
    else:                 cmd_paste_image(st, w)   /* unchanged existing path */
```

`cmd_paste_cut(AppState *st)`:
```
board = st->board_view                      (the board you're viewing)
page  = active_page of board (obj_meta, default "/")
for each h in cut[0..cut_count):
    card_move_to_page(st, h, board, page)
cut_count = 0                               (consume the cut; cards un-dim)
sel_clear(st)                               (leave nothing selected after a paste — predictable)
scene_save(&st->scene, "scene.stml")
printf("pasted %d card(s) to %s\n", n, page)
```

### 4. The move helper — `card_move_to_page(st, handle, board, page)` (main.c)

The single reusable mutation, uniform across note/picture/folder:

```
o = scene_get(handle); if !o return
if o->parent == board:                      /* same board, another page */
    scene_meta_set(scene, handle, "page", page)   /* layout preserved for free */
else:                                        /* cross-board move */
    derive the card's board-local offset on its OLD board (inverse of board_pin_pos,
        via board_under_ray geometry / the stored pin) so layout carries over
    o->parent = board
    re-pin: o->pos = board_pin_pos(scene, board, handle, old_local, ox, oy)
            (match the proud-of-face offset the card type uses today)
    scene_meta_set(scene, handle, "page", page)
    workspace: re-tag to the target board's workspace if different
        (mint_tag_ws-style; parent-walk means it inherits, but set explicitly to be safe)
```

- **NOTE on aliasing:** copy any `scene_meta_get` result to a local buffer before a
  `scene_meta_set`/`scene_add` that could realloc (the board-pages meta-pointer trap).
- **NOTE on handles:** never deref a `SceneObject*` across a `scene_add`/`scene_meta_set` that
  may realloc — re-`scene_get` after mutating (the spatial-filesystem use-after-realloc law).

### 5. Dim the cut cards — the board-card draw (main.c)

In the main opaque object draw loop, when an object's handle is in the cut set, render it via
`draw_glass(state, o->mesh, model, view, proj, eye, o->material)` (the translucent alpha
path) instead of the normal opaque `draw_mesh`. A tiny helper:

```c
static sol_bool handle_is_cut(const AppState *st, sol_u32 h) {
    int i; for (i = 0; i < st->cut_count; i++) if (st->cut[i] == h) return SOL_TRUE;
    return SOL_FALSE;
}
```

`cut_count` is tiny, so the per-object linear scan is negligible. Reuses the existing alpha
pipeline → no new shader, no MSL twin.

### 6. Clearing the cut buffer

The cut clears when:
- **Paste** consumes it (§3).
- **`Esc`** in board view: add `st->cut_count = 0;` to the existing board-view Esc handling.
- **A new `Cmd+X`** replaces it (§2 overwrites).
- **A cut card is deleted:** in the board-card delete path (`delete_board_card` / the group
  delete), drop any deleted handle from `cut[]` (compact the array, decrement `cut_count`) so
  the dim state and the paste set never reference a dead handle.

### 7. Palette rows — `g_commands[]` (main.c:9759)

Two palette-only rows (no key; `Cmd+X`/`Cmd+V` are the real shortcuts), with `can_run`
guards:
```c
{ "Cut selected cards", NULL, 0, cmd_cut_selection, can_cut_selection, SOL_FALSE },
{ "Paste cards",        NULL, 0, cmd_paste_cards,    can_paste_cards,   SOL_FALSE },
```
- `can_cut_selection` = board_view != 0 && sel_count > 0.
- `can_paste_cards` = board_view != 0 && cut_count > 0.
- `cmd_cut_selection` wraps the §2 copy; `cmd_paste_cards` wraps `cmd_paste_cut`.

## Data Flow

```
Cmd+X (board view, selection non-empty)
  -> cut[] = sel[], cut_count = sel_count        (cards stay, now dimmed)

navigate to target page (arrow / double-click folder) or another board

Cmd+V (board view)
  -> cut_count > 0 ?  cmd_paste_cut :  cmd_paste_image
       cmd_paste_cut: for each cut handle -> card_move_to_page(board_view, active_page)
                      -> same board: retag meta["page"]
                      -> cross board: re-parent + re-pin + retag page + workspace
                      -> cut_count = 0 (un-dim), scene_save
reload: cards load under their new parent/page from scene.stml
```

## File Touch List

- **`main.c`** (only file): `cut[]`/`cut_count`/`cut_was_down` AppState fields + init;
  `Cmd+X` handler; `Cmd+V` contextual branch; `card_move_to_page`; `cmd_paste_cut`;
  `handle_is_cut` + the dim branch in the board-card draw; Esc clear; delete-path cut
  compaction; the two `g_commands[]` rows + their `cmd_*`/`can_*` wrappers.
- No new module, no new shader, no build.sh change.

## Testing

- **Build gauntlet (all three):** `./build.sh c89check`, `./build.sh`, `./build.sh metal`.
- **Pure-logic:** thin. The only cleanly unit-testable piece is the cut-array compaction
  (remove a handle, preserve order) — a small assertion in an existing headless test or the
  multiselect test suffices. The move itself is scene mutation + GLFW input + render, covered
  by live-verify.
- **Human live-verify (both backends):**
  - Cut a single note (`Cmd+X`) → it dims; navigate to another page; `Cmd+V` → it moves
    there at the same spot; original page no longer shows it; reload persists.
  - Multi-select 3 cards (note + picture + folder), `Cmd+X` → all dim; paste on another page
    → all move, relative layout preserved.
  - Cross-board: cut on board A, leave board view, open board B, `Cmd+V` → cards appear on
    board B's active page (re-parented), correct workspace.
  - `Cmd+V` with an **empty** cut buffer but an image on the system clipboard → still pastes
    the image (contextual fall-through intact).
  - `Esc` after a cut → cards un-dim, a subsequent `Cmd+V` pastes the clipboard image (cut
    cleared).
  - Delete a cut card → no crash, paste set no longer references it.

## Risks

- **Cross-board re-pin math.** Preserving board-local layout across boards requires deriving
  the card's old board-local offset and re-pinning on the new board. Get the proud-of-face
  offset right per card type (the picture-slide vs note pin), or pasted cards z-fight / sit
  off the face. Mitigate by reusing `board_pin_pos` with the card's existing local offset.
- **Use-after-realloc.** `card_move_to_page` does `scene_meta_set` (can realloc); never hold a
  `SceneObject*` across it — re-`scene_get`. (Spatial-filesystem law.)
- **Stale handles in `cut[]`.** A deleted cut card must be dropped from `cut[]` (§6) or the dim
  scan / paste touches a dead handle.
- **Folder backlink staleness** (accepted v1 limitation): a moved folder's backlink still
  points at the old page.
- **No new shader** (reuses the `draw_glass` alpha path) ⇒ no MSL twin.
