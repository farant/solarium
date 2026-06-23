# Writable Books — Design

**Date:** 2026-06-22
**Status:** Approved

## Goal

Turn the randomly-generated books (codices) into real notebooks:

- **Part A — shelve them:** carry a generated book and file it onto a bookshelf
  (the same carry-file gesture that files cards today).
- **Part B — write in them:** open a generated book and type into discrete,
  growable pages — append-only, one page per side, flip to edit other pages,
  flip past the end to add a page. Saved in the scene.

Scope is the **generated codices only**. File/alias cards and the Skyrim book stay
read-only — the existing reader is unchanged for them.

## Background — what exists today

- **A codex is a group:** `cmd_mint_codex` (`V`) adds a mesh-less anchor
  (`meta["name"]="codex"`) with two child meshes, `book_cover` + `book_block`,
  standing upright. It has **no bound text**.
- **`codex_cover_child(s, root)`** returns the cover child (`!= 0`) for a codex,
  `0` otherwise — the clean **editable predicate**.
- **The reader is read-only** (`reader_open` `main.c:5653`): it sets
  `reader_source = root`, raises an open-book mesh, and `reader_load_content` reads
  `o->content` (a *file path*) via `fs_read_file`, wrapping it into one `reader_text`
  blob that is paginated into spreads. `reader_open` already computes
  `cover = codex_cover_child(root)`; a codex carries no file, so it opens "empty"
  today.
- **Page rendering** (`reader_draw_page` / `_bent`, `main.c:5910`) draws lines
  `[page*L, page*L+L)` of `reader_text` onto a page plane, where
  `L = reader_lines_per_page`. The leaf-turn animation turns whole spreads
  (`reader_spread`, `reader_turning`).
- **Note editing** (`on_char` `main.c:11617`, `on_key` editing branch
  `main.c:11684`, `edit_buf`/`edit_handle`) is append-only: type appends, Backspace
  removes one codepoint, Enter inserts `\n`, each keystroke mirrors into a meta
  field. `read_input` early-returns (suppressing movement) when `edit_handle != 0 ||
  palette.open` (`main.c:7398`).
- **Filing** accepts only carried `"card"` objects (the `furniture_surface_aim`
  loop in `carry_update`); the drop re-parents to the shelf at a `shelf_free_slot`.
- **`carry_target`** (`main.c:6628`) resolves a codex (its `KIND_PLAIN` cover/block
  child) to the group-root anchor — but **refuses an anchor with `parent != 0`**, so
  a *shelved* codex can't be picked back up.

## Part A — Shelve generated books

Generalize the carry-file path from `"card"` to also accept a **codex group**
(an anchor whose `codex_cover_child != 0`).

1. **`carry_target`** — when the group-root anchor is a codex, return it even if it
   is parented to furniture (the same allowance `KIND_*` cards already get). This
   lets a shelved book be carried again; the existing pickup `on_furn` branch
   (`main.c:6769`) already detaches it from the shelf on pickup.
2. **`carry_update` furniture preview** — the loop that previews filing
   (currently gated on the carried object being a `"card"`) also runs when the
   carried object is a codex group. On a shelf hit it previews the book at the
   slot; on a table hit, lying on the surface.
3. **Drop (`cmd_carry_toggle` file branch)** — re-parent the codex anchor to the
   furniture at the resting transform (the `o->parent = furniture` precedent).
4. **Orientation** — on a shelf the book stands **spine-out**: a yaw that turns the
   spine to face the room, upright. (The codex's free-standing rotation already
   stands it; filing replaces it with the shelf orientation, restored to the
   pre-file rotation on pickup via `carry_prev_rot`.)
5. **Slot sizing** — books are thicker than a card spine; the shelf slot advances
   by the book's own width so books don't overlap. `shelf_free_slot` /
   `furniture_shelf_slot` gain awareness of the filed object's footprint (books
   take a wider slot than cards). If a clean width-aware slot is more than a small
   change, v1 may place books at card slots and widen in live-verify — flagged, not
   silently shipped.

No new mesh, no shader — the furniture-filing pattern generalized to codex groups.

## Part B — Write in generated books

### Storage

Each page is a text string. The book stores them on the **codex anchor's meta**:
`meta["pagecount"]` = N, and `meta["page0"]`…`meta["page{N-1}"]` each one page's
text. Per-page keys reuse the note-text round-trip (proven for multiline values) —
no separator escaping. A codex with no `pagecount` starts as **one blank page**.
Persisted in `scene.stml`; nothing external.

### Reader state (new)

The reader gains, alongside the existing read-only fields:

- `char **reader_pages; int reader_page_count;` — the page strings (source of truth
  while open).
- `int reader_page;` — the **current** page (the one with the caret).
- `sol_bool reader_editable;` — set in `reader_open` from `cover != 0`.

Editing is active when `reader_editable && reader_state == READER_OPEN`
(a `reader_is_editing(st)` helper).

### Reuse the existing render via a rebuilt blob

The page array is the source of truth; the reader's blob is **derived** from it so
*all* existing rendering and the leaf animation work unchanged:

`reader_pack_pages(st)` — for each page, wrap its text to `field_w`, **cap at `L`
lines** (`L = reader_lines_per_page`), and **pad to exactly `L` lines** with blanks;
concatenate into `reader_text` + rebuild `reader_line_off`. Because every page
occupies exactly `L` lines, reader page index `k` == discrete page `k`, and
`reader_spread = reader_page / 2` selects the spread that shows pages `2s` (left)
and `2s+1` (right). The caret (`'_'`) is appended to the **current** page's text
before packing (the note-caret trick). `reader_pack_pages` runs on open and after
every edit.

The page **capacity** is `L` lines: when wrapping the current page already fills
`L` lines, further `on_char` input is ignored (you flip or add a page).

### Editing input (append-only, keyboard-captured)

- `read_input` early-return guard (`main.c:7398`) also fires when
  `reader_is_editing(st)` — movement is suppressed (`w` types, not walks), exactly
  like note editing.
- `on_char`: when editing, append the codepoint to `reader_pages[reader_page]`
  (unless the page is at capacity), then `reader_pack_pages`.
- `on_key` editing branch: **Backspace** removes one codepoint from the current
  page; **Enter** appends `\n`; **Esc** closes (saves); **◄/►** flip (below). All
  re-pack.

### Navigation & growth (single-page flip)

`reader_flip(st, dir)` (extracted so both read-only and editing can call it):

- Editing: `reader_page += dir`, clamped at `0`. If `reader_page` would exceed the
  last page, **append a new blank page** (`reader_page_count++`) — that is "add a
  page." Recompute `reader_spread = reader_page / 2`; if the spread changed, trigger
  the **leaf-turn animation** (the existing `reader_turning` path); otherwise the
  caret just moves to the other page of the same spread (no leaf turn). Re-pack.
- Read-only: unchanged — `reader_flip` turns whole spreads as today.

### Save / close

`reader_close` (and Esc): write the pages back —
`scene_meta_set("pagecount", N)` and `meta["page{i}"] = reader_pages[i]` on
`reader_source`, then `scene_save`. Free `reader_pages`. (Trailing blank pages are
kept — the book keeps the pages you added.)

### Read-only path unchanged

File/alias cards and any non-codex book keep `reader_load_content` from a file and
the existing rendering verbatim. `reader_editable` is false for them, so none of
the editing branches engage.

## Architecture notes

- The page array → packed-blob indirection means **zero changes to the page
  draw / leaf-turn code** — the riskiest visual machinery is reused as-is.
- This turns the read-only reader rig into an **interactive** one (keyboard
  capture, live text on the page) — the foundation the eventual TODO5 app-engine
  ("apps live in books") would build on. Noted, not built here.
- All in `main.c` (the reader + carry/file + input live there). No new mesh ref,
  **no new shader → no MSL twin**.

## Files

- Modify: `main.c` — reader state + `reader_open` (set `reader_editable`, init
  pages), new `reader_pack_pages` / `reader_flip` / `reader_is_editing`, page
  draw fed by the packed blob, `reader_close` save, `on_char`/`on_key` editing
  branches, `read_input` guard; `carry_target` + `carry_update` furniture loop +
  `cmd_carry_toggle` drop for codex groups; shelf-slot footprint.
- Possibly: a tiny pure helper for the page-pack/wrap-and-pad math, unit-tested
  if cleanly separable from the font/scene (otherwise verified in-engine).

## Testing

GUI + world-render feature → **human live-verify** (subagents can't GUI-test),
consistent with every prior reader/card feature. Build gauntlet
(`./build.sh c89check && ./build.sh debug && ./build.sh metal`) must pass on both
backends. If the page-pack line math is extracted to a pure function, give it a
small `*_test`; otherwise no new test target (the reader has none today).

Manual verification:
1. Mint a book (`V`), select it, `Enter` → it opens and accepts typing; text wraps
   and a caret tails it.
2. Fill a page → typing stops at the page's last line; `►` flips to the next page
   (leaf turn on a new spread); type there; `◄` returns and the earlier text is
   intact.
3. `►` past the last page adds a fresh blank page.
4. `Esc` closes and saves; reload the scene, reopen the book → all pages and text
   are there.
5. Carry the book (`E`), aim at a bookshelf → it previews at a slot; drop → it
   files spine-out; pick it back up (`E`) → it detaches; reload → still filed.
6. Regression: a file/alias card and the Skyrim book still open **read-only**
   (no caret, no typing), arrows still flip, walking still works while reading them.

## Constraints (project laws)

- Strict C89; build gauntlet passes on both backends.
- No new shader → no MSL twin.
- Never stage/commit `NOTES.stml` or `paper-picture.png`.
- Feature branch in-place → ff-merge to main; commits end with the
  `Co-Authored-By: Claude Opus 4.8 (1M context)` line.
