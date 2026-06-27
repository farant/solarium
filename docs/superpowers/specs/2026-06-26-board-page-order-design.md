# Board Page Order & Persistence — Design Spec

**Date:** 2026-06-26
**Status:** Approved (brainstorm), pending implementation plan
**Author:** Fran + Claude

## Goal

Board pages currently exist only as long as some note is tagged with them, and they enumerate in
alphabetical order (so `/page-10` sorts before `/page-2`) and vanish when navigated away from while
empty. Give the board an **ordered, persistent page list in its own metadata** so pages enumerate in
**creation order** and **persist when empty**. (Companion bug-fix already done on this branch: a new
arrow inherits the board's active page.)

## Decisions (from brainstorming)

- **`meta["pages"]` on the board = a space-delimited list of the non-root page slugs in creation
  order.** `"/"` is always the implicit first page (not stored). Slugs are `/[a-z0-9-]+` (no spaces),
  so a space splits cleanly.
- **`board_pages` reads that list** (parse → prepend `"/"`), replacing today's scrape-tags +
  alphabetical sort. It is the one chokepoint — `cycle_page`, navigation, and `board_card_tag_page`
  all flow through it, so they get ordering + persistence for free.
- **Migration (once, on load):** for an existing board with no `meta["pages"]`, seed it from today's
  emergent page collection, **natural-sorted** (digit runs compared numerically → `/page-2` before
  `/page-10`; custom-named folder pages still order sanely). Pre-existing *empty* non-active pages
  were already lost under the old model (not recoverable); everything currently visible is preserved,
  and pages created afterward persist.
- **Creation appends (real creation order):** `board_new_page` (Shift+→) appends the smallest-free
  `/page-N`; `add_folder` appends its link-target slug when that page isn't listed yet.
- **No page-delete** in v1 (pages persist once created — matches today, which has no delete).
- The natural-sort comparator + a parse/serialize pair are the only genuinely new pure logic;
  everything else is rewiring `board_pages`.

## Non-Goals

- No page deletion (a later add).
- No invisible per-page parent object — pages stay lightweight (a tag on the board, no new object kind).
- No change to `meta["page"]` (a card's page), `meta["active_page"]` (the board's current page),
  the page gate (`scene_object_active`), folders, or persistence schema beyond the new `meta["pages"]`.
- No recovery of already-lost empty pages from before this change.

## Background (current state — verified)

- **`board_pages(AppState*, board, out, cap)`** (main.c): scrapes each board-child's `meta["page"]`
  into `raw[]`, then `boardpage_collect(raw, n, active, out, cap)`.
- **`boardpage_collect`** (boardpage.c): dedupes, always includes `"/"` and `active`, **insertion-sorts
  by `page_cmp`** (which puts `"/"` first then ascending `strcmp` = alphabetical). `PAGE_SLUG_CAP 96`,
  `BOARD_PAGE_MAX 64`. Also `boardpage_slugify`. (`boardpage_test.c` covers these.)
- **`board_new_page`** (main.c): finds the smallest-free `/page-N` (checking `board_pages`), tags the
  selection onto it, sets `active_page`. A page minted with no cards vanishes once it's no longer
  active (the emergent-page symptom).
- **`cycle_page`** (main.c): steps `active_page` through `board_pages` (currently alphabetical).
- **`add_folder`** (main.c): the `d`-key makes a folder (`mesh_ref="folderbook"`, `meta["link"]=target
  page`) — a page link.
- **`board_card_tag_page`** (main.c): tags a board child with the board's `active_page`. (The arrow
  fix on this branch calls the same logic.)
- **`scene_meta_get/set`** are by handle (realloc-safe). The codex stores its pages as ordered meta
  the same way (the established pattern).

## Architecture

### 1. The ordered page list — `meta["pages"]`

A single board meta value: the non-root slugs in creation order, space-joined, e.g.
`"/page-1 /page-2 /ideas"`. `"/"` is implicit-first (never stored). Empty/absent ⇒ the board has only
the root page.

### 2. Pure helpers in `boardpage.c` (+ `boardpage_test.c`)

- **Change `page_cmp` to NATURAL order:** keep `"/"`-first, then compare by splitting each slug into
  alternating non-digit / digit runs and comparing run-by-run (digit runs numerically). This makes
  the migration seed (and the `boardpage_collect` fallback) order `/page-2` before `/page-10`. Update
  the `boardpage_test` assertions from alphabetical to natural order.
- **`int boardpage_parse(const char *list, char out[][PAGE_SLUG_CAP], int cap)`** — split a
  space-delimited list into `out[]` (skip empty tokens, dedupe), return the count. Pure, tested.
- **`void boardpage_serialize(const char *const *slugs, int n, char *out, int cap)`** — space-join
  `slugs` (skipping any `"/"`) into `out[cap]` (NUL-terminated, truncate-safe). Pure, tested.

### 3. `board_pages` reads the list (main.c)

```
stored = meta["pages"]; active = meta["active_page"]
if stored && stored[0]:
    n = boardpage_parse(stored, tmp, BOARD_PAGE_MAX)
    out[0] = "/"; copy tmp (deduped, "/"-excluded) after it
    ensure `active` is in out (append if missing) — defensive
    return count
else:
    # un-migrated / page-less board: fall back to the emergent collection,
    # now natural-sorted via the page_cmp change — robust either way
    scrape raw page tags (today's loop) -> boardpage_collect(raw, n, active, out, cap)
```
`board_pages` stays **read-only** (no writes), so it's safe to call freely.

### 4. Migration pass on load — `boards_migrate_pages(AppState*)`

For each `"board"` object lacking `meta["pages"]`, compute its emergent page list via `board_pages`
(natural-sorted), and if it has any non-root page, `boardpage_serialize` those into `meta["pages"]`.
This makes the list (and thus empty-page persistence) real for existing boards. **Call it in BOTH
`world_rebuild` AND `load_palace`** (the load-derive law — `load_palace` does not call `world_rebuild`),
before/at the existing derive tail; the next `scene_save` persists it. Boards with only `"/"` are left
untouched (no clutter).

### 5. Creation appends (main.c)

- **`board_new_page`:** find the smallest-free `/page-N` against `board_pages`; **append** the slug to
  `meta["pages"]` (read current list via `boardpage_parse`, push the new slug, `boardpage_serialize`
  back); set `active_page`; keep the move-selection behavior. (If `meta["pages"]` was absent, seed it
  from the current `board_pages` list first, then append — so a first page-creation on an un-migrated
  board still establishes the ordered list.)
- **`add_folder`:** when the folder links a page not already in `meta["pages"]`, append the target
  slug the same way.

### 6. Unchanged

`cycle_page`, the page gate (`scene_object_active`), `board_card_tag_page`, navigation (double-click a
folder), and arrow tagging all flow through `board_pages` / `active_page` — they inherit ordering +
persistence with no change.

## Data Flow

```
load -> boards_migrate_pages: each board w/o meta["pages"] -> seed (natural-sorted emergent) -> meta["pages"]
board_pages -> parse meta["pages"] (or fallback to emergent natural-sorted) -> "/" + ordered list
Shift+Right -> board_new_page -> append /page-N to meta["pages"] -> set active_page -> save
d (folder to new page) -> add_folder -> append target slug to meta["pages"]
cycle / navigate -> active_page steps through board_pages (creation order); empty pages stay listed
```

## File Touch List

- **`boardpage.c` / `boardpage.h`**: `page_cmp` → natural order; `boardpage_parse`;
  `boardpage_serialize`. **`boardpage_test.c`**: natural-order assertions + parse/serialize tests.
- **`main.c`**: `board_pages` reads `meta["pages"]` (with the emergent fallback); `boards_migrate_pages`
  + its calls in `world_rebuild` and `load_palace`; `board_new_page` and `add_folder` append to the
  list.

## Testing

- **Build gauntlet (all three):** `./build.sh c89check`, `./build.sh`, `./build.sh metal`.
- **Pure-logic (`boardpagetest`, scene-free):** natural sort orders `/page-2` before `/page-10` and
  `"/"` first; `boardpage_parse` round-trips a space list (dedupe, skip empties); `boardpage_serialize`
  joins + skips `"/"`; a parse∘serialize round-trip is stable. Update the existing alphabetical
  assertions.
- **Human live-verify:** create several pages (Shift+→) → they cycle in creation order (`/page-1, …,
  /page-10`, not lexical); make an empty page, navigate away and back → it's still there; a folder to a
  new page lists it; reload → order + empty pages persist; an existing (pre-feature) board reorders
  `/page-N` numerically on first load and keeps its pages.

## Risks

- **Migration timing:** `board_pages` must work whether or not migration ran (the emergent fallback
  guarantees it); the migration pass only *persists* the list. Confirm `boards_migrate_pages` runs on
  both load paths.
- **`meta["pages"]` length:** `BOARD_PAGE_MAX 64` × ~`PAGE_SLUG_CAP` is a large worst-case value; real
  boards have a handful of pages. `boardpage_serialize` must truncate-safely at `cap`; cap the value at
  `BOARD_PAGE_MAX` slugs.
- **Active page not in the list:** `board_pages` defensively appends `active_page` if parsing didn't
  include it, so navigation never strands the user on an unlisted page.
- **Natural-sort correctness:** the digit-run comparator must handle leading zeros, equal prefixes, and
  the `"/"`-first rule — covered by `boardpagetest`.
- No new shader / no persistence-schema change beyond the new meta value.
