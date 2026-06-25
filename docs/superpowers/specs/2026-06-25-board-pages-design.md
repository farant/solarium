# Board Pages — Design Spec

**Date:** 2026-06-25
**Status:** Approved (brainstorm), pending implementation plan
**Author:** Fran + Claude

## Goal

Turn each whiteboard from a single surface into a **navigable notebook of pages** —
a graph of sub-boards connected by **folder** links, browsed like folders in a file
manager. Each page holds its own cards/notes/pictures; you navigate between pages by
double-clicking folder objects (or arrow-cycling), and folders auto-create backlinks so
the graph stays traversable in both directions.

## Vocabulary

- **Page** — one navigable surface of a board, identified by a unique **slug/path**
  (e.g. `/`, `/chapter-1-notes`). The root page is `/`.
- **Folder** — a book-model object pinned to a page that *links to a target page*. Its
  label is the target's path. It is décor + a navigation handle; you never "read" it.
- **Active page** — the page a board is currently showing.

## Decisions (from brainstorming)

- **Flat namespace, graph (not a tree).** Each path is a unique page key within the
  board. Any page can link to any page; backlinks form cycles. The leading `/` is style,
  not nesting. (Matches the examples + the backlink / orphan / re-link behavior.)
- **Delete removes only the clicked folder.** The auto-created backlink on the other
  page is left intact and still works (its target page still exists). A target page is
  "orphaned" only once *all* folders pointing to it are gone — it still survives (its
  tagged objects keep their page) and stays reachable by arrow-cycling.
- **Approach A — tags + filter, mirroring workspaces.** Pages are emergent from
  `meta` tags; visibility is one O(1) gate folded into `scene_object_active`. No page
  container objects, no schema change. (Approaches B "explicit page-anchor objects" and
  C "off-scene page store" were rejected as heavier than the feature needs / a rewrite.)
- **`active_page` persists** on the board (a board "remembers" its page across reloads).
- **No new shader** — folders reuse `draw_mesh` PBR; titles/labels reuse `wtext`; the
  drop indicator reuses the selection-highlight path. No MSL twin.

## Non-Goals

- No hierarchical path math / breadcrumbs / "navigate up" (flat graph only).
- No per-page metadata or custom page ordering **yet** (see Extension Point). Ordering
  is a deterministic sort for now.
- Folders are not containers — they never parent cards. A card belongs to a page only
  via its own `meta["page"]` tag.
- No new card kinds beyond the folder object; notes/pictures/aliases are unchanged.

## Background (current state)

- A board is a `SceneObject` with `mesh_ref="board"` (`object_is_board`, main.c:4125).
  Cards/notes/pictures pin to it with `parent = board_handle` and board-local coords
  (`board_under_ray` main.c:4135, `board_pin_pos` main.c:4166).
- **No board-specific render loop.** Every pinned object is a flat scene object drawn by
  the main loop, gated by three checks: `mesh.index_count != 0`, `scene_object_active`
  (workspace + stowed filter), and `vis[]` (frustum) — main.c:13335.
- `scene_object_active` (workspace.c:35) already chains `scene_object_stowed` and the
  `active_ws` workspace filter via a parent-walk (`workspace_of`, workspace.c:8). This is
  the seam pages extend.
- Board view (`board_view` = focused board handle, main.c:2815) glides the camera to
  frame a board and unlocks the cursor; movement is frozen. The press handler already
  does double-click detection (`BOARD_DBL_S`/`BOARD_DBL_PX`, main.c:4299) dispatching
  note-edit / spawn-note (main.c:9827).
- `spawn_note` (main.c:9511) places a `KIND_NOTE` `"card"` at the cursor via
  `board_under_ray`. `note_edit_begin` (main.c:14716) opens a note for typing.
- Meta API: `scene_meta_set` / `scene_meta_get` (scene.h:125), round-tripping through
  STML on save/load. Codex randomization precedent: `cmd_mint_codex` (main.c:7469) bakes
  random book params + a random leather material via an LCG.
- Delete dispatch (main.c:10809): Backspace/Delete branches per kind/mesh_ref, calling
  `scene_remove` + releasing the keyed mesh.
- Bookshelf label precedent (main.c:14037): `wtext_block` floats `meta["label"]` above an
  object facing the camera — the model for the page title + folder labels.

## Architecture (all in `main.c`, plus one mesh in `mesh.c`)

### 1. Page tags & the page gate

**Tags (existing meta → STML, nothing new to persist):**
- Board-child note/picture/folder: `meta["page"]` = its page slug. **Absent ⇒ `/`.**
- Board: `meta["active_page"]` = the shown page. **Absent ⇒ `/`.**

**The gate** — a new helper:
```c
/* A board-child is "paged out" (hidden) when its page != the board's active page.
   O(1): page-bearing objects pin directly to the board (parent == board). */
static sol_bool scene_object_paged_out(Scene *s, sol_u32 handle);
```
Body: get the object; if `parent == 0` return false; get the parent; if the parent's
`mesh_ref != "board"` return false; compare `meta["page"]` (default `/`) of the object to
`meta["active_page"]` (default `/`) of the board; return true (hidden) if they differ.

Folded into `scene_object_active` (workspace.c) right after the stowed check:
```c
if (scene_object_stowed(s, handle)) return SOL_FALSE;
if (scene_object_paged_out(s, handle)) return SOL_FALSE;   /* NEW */
/* ...existing workspace filter... */
```
`scene_object_paged_out` lives wherever `scene_object_active` can call it (workspace.c,
or a small forward-declared helper). It needs only `scene_get` + `scene_meta_get`, both
already used in workspace.c.

Because the board itself has a non-board parent (room/wall/0), it is never paged out.
Legacy boards (no tags): object page `/` == board active `/` ⇒ everything visible.

**Page-set derivation** (for arrow-cycle + link-to-existing), a helper that scans a
board's direct children once:
```c
/* Collect the board's distinct page slugs into out[] (sorted: "/" first, then
   alphabetical), always including "/" and the board's current active_page. */
static int board_pages(Scene *s, sol_u32 board, char out[][PAGE_SLUG_CAP], int cap);
```
Boards have a handful of children, so the scan is cheap and called only on demand
(folder create, arrow press), never per-frame.

### 2. The folder object & its mesh

- A folder = `KIND_PLAIN`, `mesh_ref="folderbook"`, `parent=board`, `meta["page"]` = page
  it sits on, `meta["link"]` = target page slug, baked random `mesh_params`.
- `object_is_folder(s,h)` predicate = `mesh_ref == "folderbook"`.
- **New mesh `make_folderbook(MeshBuilder*, params)` in mesh.c:** a single closed-book
  mesh seen cover-out, protruding a few cm from the board face (front face parallel to
  the board, like a thick card), reusing the book cover/block/spine geometry. `params`
  carry the randomized proportions (w/h ratio, thickness, spine raised-bands). Registered
  in the same mesh-ref dispatch as the other meshes.
- **Creation randomization** (a `mint_folderbook`-style helper in main.c, modeled on
  `cmd_mint_codex`): bake random book params into `mesh_params` and a random leather
  material color into the object's `Material`, so folders look varied.
- **Folder label:** `wtext` of `meta["link"]` (the target path) floating just above the
  book, facing the camera — same call shape as the bookshelf label (main.c:14037),
  gated by `scene_object_active` so paged-out folders' labels stay hidden too.

### 3. Create a folder — `d` in board view

- `d` (edge-detected; movement is frozen in board view so the WASD `D` binding is inert
  here) opens the existing text-input modal, prompt shown with a leading `/`.
- **Slugify** the submitted name: lowercase; trim; collapse whitespace runs to a single
  `-`; ensure exactly one leading `/`. Empty result ⇒ cancel. `target == active_page`
  (self-link) ⇒ ignore.
- **Resolve target** against `board_pages`: existing ⇒ *link to existing*; new ⇒ new page.
- **Forward folder** on the current page: `scene_add(parent=board)`, `KIND_PLAIN`,
  `mesh_ref="folderbook"`, `meta["page"]=active_page`, `meta["link"]=target`, random
  params/material; positioned at the cursor (`board_under_ray`, else board center) via
  `board_pin_pos`. Select it.
- **Backlink folder** on the target page — **idempotent**: only if no existing folder on
  the target page already has `link == active_page`. Same construction with
  `meta["page"]=target`, `meta["link"]=active_page`, placed top-left of the board.
- Save the scene (`scene_save`).

**Use-after-realloc discipline:** `scene_add` may move the objects array — set fields via
handle-based setters / re-fetch with `scene_get` after each add; never hold a
`SceneObject*` across an add (project law).

### 4. Navigate

- **Double-click a folder** (extends the board-view double-click dispatch, main.c:9827):
  if the selected object `object_is_folder`, set
  `scene_meta_set(board_view, "active_page", folder->link)` and save. The page gate swaps
  what renders; the camera keeps framing the same board. Existing arms (note→edit,
  empty→spawn-note) unchanged; a non-folder non-note card double-click still does nothing.
- **Arrow-cycle** (board view, `selected_handle == 0`): `←` / `→` (edge-detected) step the
  board's `active_page` through `board_pages(board_view, …)` — wrap-around; save. The list
  always contains `/` and the current page, so `/` is always reachable.

### 5. The board title

Render `active_page` as `wtext` above the board (bookshelf-label pattern), shown when:
`board_view == this_board` **or** `active_page != "/"`. So you always see where you are in
board view, a board turned to a sub-page is labeled out in the world, and plain `/` boards
stay clean.

### 6. File a card into a folder (drag) — Milestone 2

- During a board-view card drag, each frame find the folder under the cursor
  (`board_under_ray` → nearest `object_is_folder` child within a pick radius); record it
  in `st->drop_target_handle` (0 = none).
- **Drop indicator:** render `drop_target_handle` with a highlight tint (reuse the
  selection-highlight render path) — the "color-changing drop indicator."
- **Release over a folder:** instead of finalizing the move, re-tag the dragged card:
  `scene_meta_set(card, "page", folder->link)` and save. The card page-filters out of the
  current page (it's "filed"). Release **not** over a folder ⇒ ordinary move (today's
  behavior). Clear `drop_target_handle` on release.
- A folder is itself a card-like pinned object, but you cannot file a folder into a folder
  (only `KIND_NOTE` / picture cards are filable); filing a folder is a no-op move.

### 7. Delete a folder

New branch in the delete dispatch (main.c:10809), beside the `picture` branch: if the
selected object `object_is_folder`, release its keyed mesh (`mesh_asset_key` →
`asset_release`), `scene_remove` it, clear carry/resize/drop refs, `scene_save`. The
target page and its contents survive; the backlink on the other side is untouched.

## Data Flow

```
d (board view) -> name prompt -> slugify -> target
   target == active_page         -> ignore
   target exists in board_pages  -> link to existing (no new page)
   else                          -> new page
   -> forward folder on active_page (link=target) at cursor
   -> backlink folder on target (link=active_page), top-left, idempotent
   -> save

double-click folder -> board.active_page = folder.link -> page gate re-filters -> save
arrow L/R (nothing selected) -> active_page = board_pages[i +/- 1] -> save

drag card, release over folder -> card.page = folder.link (filed) -> save
delete selected folder -> scene_remove(folder)  (page + backlink survive) -> save

render/pick/cull: scene_object_active() -> + scene_object_paged_out() gate
```

## Extension Point (deliberately left open, not built)

Per-page metadata / custom ordering can later be added without making pages into
containers: a lightweight **non-visual page-marker object** tagged `meta["page"]` (with
`meta["order"]`, `meta["info"]`, …) or a page-order blob on the board. Notes keep their own
`meta["page"]` tag and never re-parent. `board_pages`'s deterministic sort would defer to
an explicit order when present.

## File Touch List

- `mesh.c` — new `make_folderbook` builder + its mesh-ref dispatch entry.
- `workspace.c` — fold `scene_object_paged_out` into `scene_object_active` (and define or
  forward-declare the helper where reachable).
- `main.c` — `scene_object_paged_out` + `board_pages` + `object_is_folder` helpers;
  `mint_folderbook` randomizer; the `d` create flow (prompt + slugify + link-resolve +
  forward/backlink); the double-click navigate arm; arrow-cycle; the page title render;
  the folder label render; the drag-to-file drop-target + indicator + re-tag (M2); the
  delete-folder branch; `AppState` field `drop_target_handle` and constant `PAGE_SLUG_CAP`.

## Milestones

- **M1 — Navigable notebook:** page tags + gate; `make_folderbook` mesh; `d`-create
  (prompt, slugify, link-existing, backlink); double-click navigate; path title + folder
  labels; arrow-cycle; delete-folder. Result: a fully navigable, persistent board notebook.
- **M2 — Drag-to-file:** per-frame drop-target detection, the color-changing indicator,
  and re-tag-on-release.

## Testing

- **Build gauntlet (all three, every task):** `./build.sh c89check` (strict
  `-std=c89 -pedantic-errors`), `./build.sh` (debug), `./build.sh metal`.
- **Pure-logic unit coverage** where a `*_test` harness fits: slugify rules
  (lowercase / space→`-` / single leading `/` / empty), `board_pages` sort & dedupe,
  the idempotent-backlink check.
- **Human live-verify (board view):**
  - `d` → name a folder → a randomized book appears at the cursor with its `/path` label;
    a backlink folder appears on the new page.
  - Double-click the folder → the board swaps to that page (root cards gone); the title
    shows the new path; the backlink folder is present and returns you.
  - Add notes on a sub-page → they belong to it (hidden when you leave, shown on return).
  - Arrow keys (nothing selected) cycle all pages; `/` always reachable.
  - Re-using an existing path links to it (no duplicate page, no duplicate backlink).
  - Delete a folder → it's gone, the target page + backlink survive (reach it by cycling).
  - **M2:** drag a note onto a folder → it highlights (drop indicator); release files the
    note onto the target page (it disappears from the current page).
  - Legacy boards still work (one `/` page); reload restores the active page.
  - Both backends; no launch-time MSL break (no new shader).

## Risks

- **`scene_object_active` is hot** (per-object, per-frame, + BVH). The page gate must stay
  O(1) — direct-parent board check only, no deep walk. Verify no idle-CPU regression.
- **Filter completeness:** folding into `scene_object_active` covers render/pick/cull for
  free, but confirm no reader bypasses it for board children (the workspace FILTER LAW).
- **Persistence round-trip:** pages are emergent from `meta`; confirm tags + `active_page`
  survive `scene_save`/load and that `room_mirror_scan` ignores `KIND_PLAIN` folders (as
  it does board pictures).
- **Drag-vs-file (M2):** the drop-target test must not interfere with ordinary card moves;
  live-verify the "drag a card near a folder but drop on empty board" feel.
- No new shader ⇒ no MSL-twin risk.
