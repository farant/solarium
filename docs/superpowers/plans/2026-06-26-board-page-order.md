# Board Page Order & Persistence — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Board pages enumerate in creation order and persist when empty, by giving the board an ordered page list in its own `meta["pages"]`.

**Architecture:** `board_pages` (the one chokepoint) reads an ordered `meta["pages"]` list (with an emergent natural-sorted fallback); a load-time migration seeds existing boards; page creation appends. The new pure logic (a natural-order comparator + parse/contains/serialize helpers) lives in `boardpage.c` and is unit-tested; `main.c` rewires `board_pages` and the creation points.

**Tech Stack:** C89, the scene/meta system, `boardpage.c` (scene-free, libc-only, unit-tested via `boardpagetest`).

---

## Testing approach (read first)

The pure list logic lives in `boardpage.c` and is **headlessly unit-tested** (`boardpagetest`) — Task 1 is real TDD. The `main.c` rewiring (board_pages read, migration, appends) is GUI/scene integration, gated by the three-target build gauntlet + human live-verify.

**Gauntlet (after every task):**
```bash
./build.sh c89check   # "c89check: PASS — all sources are C89-pedantic clean"
./build.sh            # "built ./solarium (debug)"
./build.sh metal      # "built ./solarium-metal (stage a: links clean, zero GL; runs from stage b)"
```
**Pure-logic test:** `./build.sh boardpagetest && ./boardpage_test` (expect `boardpage_test: OK` / its CHECK lines).

**C89 reminders:** locals at top of block; `/* */` only; no decl-after-statement; no signed/unsigned mismatch.

**Verified facts:** `boardpage.c` has `static int page_cmp(a,b)` ("/"-first then `strcmp`), `static void page_add_unique(out,*count,cap,s)` (dedupe-append), `boardpage_collect(raw,n,active,out,cap)` (dedupe + `"/"`/active + sort-by-`page_cmp`), `boardpage_slugify`. `PAGE_SLUG_CAP 96`, `BOARD_PAGE_MAX 64`. `boardpage_test.c` uses a `CHECK(cond, "msg")` macro. `add_folder(st, board, page, link, local)` makes a folder on `page` linking to `link`; `create_folder_from_name` calls it with `(board, src_buf, target, …)` — `target` is the new page. `scene_meta_get/set` are by handle (realloc-safe).

---

## Task 1: `boardpage.c` pure helpers (natural order + parse/contains/serialize)

**Files:** `boardpage.h`, `boardpage.c`, `boardpage_test.c`.

- [ ] **Step 1: Declare the new helpers (boardpage.h)**

After the existing declarations:
```c
/* Build a board's ordered page list from its stored space-delimited slug list
   (creation order) + the active page: "/" first, then each token in order,
   then `active` if not already present. Dedupes; does NOT sort. Returns count. */
int  boardpage_list(const char *stored, const char *active,
                    char out[][PAGE_SLUG_CAP], int cap);

/* 1 if `slug` is a whole space-delimited token of `list`, else 0 (so "/page-1"
   is NOT contained in "/page-10"). */
int  boardpage_contains(const char *list, const char *slug);

/* Space-join the non-"/" entries of list[0..n) into out[cap] (NUL-terminated,
   truncate-safe). "/" is implicit and skipped. */
void boardpage_serialize(const char list[][PAGE_SLUG_CAP], int n,
                         char *out, int cap);
```

- [ ] **Step 2: Write failing tests (boardpage_test.c)**

Replace `test_collect`'s alphabetical assertions with natural order, and add tests for the new helpers. Add a `test_order_and_list` called from `main`:
```c
static void test_order_and_list(void) {
    const char *raw[3];
    char out[BOARD_PAGE_MAX][PAGE_SLUG_CAP];
    char ser[BOARD_PAGE_MAX * 8];
    int  n;

    /* natural order: /page-2 before /page-10, "/" first */
    raw[0] = "/page-10"; raw[1] = "/page-2"; raw[2] = "/page-1";
    n = boardpage_collect(raw, 3, "/", out, BOARD_PAGE_MAX);
    CHECK(n == 4, "natural: 4 pages");
    CHECK(strcmp(out[0], "/") == 0,        "natural: '/' first");
    CHECK(strcmp(out[1], "/page-1") == 0,  "natural: page-1");
    CHECK(strcmp(out[2], "/page-2") == 0,  "natural: page-2 before page-10");
    CHECK(strcmp(out[3], "/page-10") == 0, "natural: page-10 last");

    /* boardpage_list preserves stored ORDER (no sort) + "/" first */
    n = boardpage_list("/page-3 /page-1 /page-2", "/page-1", out, BOARD_PAGE_MAX);
    CHECK(n == 4, "list: 4 pages");
    CHECK(strcmp(out[0], "/") == 0,       "list: '/' first");
    CHECK(strcmp(out[1], "/page-3") == 0, "list: order preserved (3)");
    CHECK(strcmp(out[2], "/page-1") == 0, "list: order preserved (1)");
    CHECK(strcmp(out[3], "/page-2") == 0, "list: order preserved (2)");

    /* boardpage_list appends a missing active */
    n = boardpage_list("/page-1", "/page-9", out, BOARD_PAGE_MAX);
    CHECK(n == 3 && strcmp(out[2], "/page-9") == 0, "list: missing active appended");

    /* contains = whole-token match */
    CHECK(boardpage_contains("/page-1 /page-10", "/page-1") == 1, "contains: token present");
    CHECK(boardpage_contains("/page-10", "/page-1") == 0,         "contains: not a substring");
    CHECK(boardpage_contains("", "/page-1") == 0,                 "contains: empty list");

    /* serialize skips "/" and round-trips with list */
    n = boardpage_list("/page-3 /page-1", "/", out, BOARD_PAGE_MAX);   /* out = / /page-3 /page-1 */
    boardpage_serialize(out, n, ser, (int)sizeof ser);
    CHECK(strcmp(ser, "/page-3 /page-1") == 0, "serialize: '/' skipped, order kept");
}
```
Add `test_order_and_list();` to `main`. (`CHECK` is the existing macro.)

- [ ] **Step 3: Run — FAIL** (`./build.sh boardpagetest && ./boardpage_test`): the helpers don't exist + the old `test_collect` alphabetical asserts now contradict natural order (you'll also fix `test_collect`'s asserts to natural — for its `/alpha`,`/beta` inputs natural == alphabetical, so those stay; only add the new `/page-N` test).

- [ ] **Step 4: Change `page_cmp` to natural order (boardpage.c)**

Replace `page_cmp`:
```c
static int page_cmp(const char *a, const char *b) {
    int ra = (strcmp(a, "/") == 0), rb = (strcmp(b, "/") == 0);
    if (ra != rb) return ra ? -1 : 1;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= '0' && ca <= '9' && cb >= '0' && cb <= '9') {
            const char *ea = a, *eb = b;   /* compare two digit runs numerically */
            int la, lb;
            while (*ea >= '0' && *ea <= '9') ea++;
            while (*eb >= '0' && *eb <= '9') eb++;
            while (a < ea - 1 && *a == '0') a++;   /* strip leading zeros */
            while (b < eb - 1 && *b == '0') b++;
            la = (int)(ea - a); lb = (int)(eb - b);
            if (la != lb) return la < lb ? -1 : 1;        /* fewer digits = smaller */
            while (a < ea) { if (*a != *b) return *a < *b ? -1 : 1; a++; b++; }
        } else {
            if (ca != cb) return ca < cb ? -1 : 1;
            a++; b++;
        }
    }
    if (*a) return 1;
    if (*b) return -1;
    return 0;
}
```

- [ ] **Step 5: Implement the three helpers (boardpage.c)**

```c
int boardpage_list(const char *stored, const char *active,
                   char out[][PAGE_SLUG_CAP], int cap) {
    int         count = 0;
    const char *p;
    page_add_unique(out, &count, cap, "/");
    if (stored) {
        p = stored;
        while (*p) {
            const char *start;
            int len;
            while (*p == ' ') p++;
            start = p;
            while (*p && *p != ' ') p++;
            len = (int)(p - start);
            if (len > 0 && len < PAGE_SLUG_CAP) {
                char tok[PAGE_SLUG_CAP];
                memcpy(tok, start, (size_t)len); tok[len] = '\0';
                page_add_unique(out, &count, cap, tok);
            }
        }
    }
    page_add_unique(out, &count, cap, (active && active[0]) ? active : "/");
    return count;
}

int boardpage_contains(const char *list, const char *slug) {
    const char *p;
    int slen;
    if (!list || !slug || !slug[0]) return 0;
    slen = (int)strlen(slug);
    p = list;
    while (*p) {
        const char *start;
        int len;
        while (*p == ' ') p++;
        start = p;
        while (*p && *p != ' ') p++;
        len = (int)(p - start);
        if (len == slen && strncmp(start, slug, (size_t)slen) == 0) return 1;
    }
    return 0;
}

void boardpage_serialize(const char list[][PAGE_SLUG_CAP], int n,
                         char *out, int cap) {
    int i, oi = 0;
    if (cap <= 0) return;
    out[0] = '\0';
    for (i = 0; i < n; i++) {
        int len;
        if (strcmp(list[i], "/") == 0) continue;
        len = (int)strlen(list[i]);
        if (oi > 0 && oi + 1 < cap) out[oi++] = ' ';
        if (oi + len < cap) { memcpy(out + oi, list[i], (size_t)len); oi += len; }
        else break;
    }
    out[oi] = '\0';
}
```
(`memcpy`/`strncpy`/`strlen` need `<string.h>` — already included.)

- [ ] **Step 6: Run — PASS.** `./build.sh boardpagetest && ./boardpage_test` prints OK.

- [ ] **Step 7: Gauntlet** — all three (boardpage.c is in every link line; the new funcs are used in Task 2-3 but defining them now is fine — they're public, no unused-static warning).

- [ ] **Step 8: Commit**
```bash
git add boardpage.h boardpage.c boardpage_test.c
git commit -m "Board pages: natural page order + list/contains/serialize helpers"
```

---

## Task 2: `board_pages` reads the ordered list + load migration

**Files:** `main.c` (`board_pages`, a new `board_page_register`, a new `boards_migrate_pages` + its two callers).

- [ ] **Step 1: `board_pages` reads `meta["pages"]` with an emergent fallback**

Replace `board_pages`'s body so it prefers the stored ordered list:
```c
static int board_pages(AppState *st, sol_u32 board,
                       char out[][PAGE_SLUG_CAP], int cap) {
    const char *stored = scene_meta_get(&st->scene, board, "pages");
    const char *active = scene_meta_get(&st->scene, board, "active_page");
    const char *raw[BOARD_PAGE_MAX];
    int         n = 0;
    sol_u32     i;
    if (stored && stored[0])
        return boardpage_list(stored, active, out, cap);   /* ordered, creation order */
    /* un-migrated / page-less board: emergent collection, now natural-sorted */
    for (i = 0; i < st->scene.count && n < BOARD_PAGE_MAX; i++) {
        SceneObject *o = &st->scene.objects[i];
        const char  *pg;
        if (o->parent != board) continue;
        pg = scene_meta_get(&st->scene, o->handle, "page");
        if (pg) raw[n++] = pg;
    }
    return boardpage_collect(raw, n, active, out, cap);
}
```

- [ ] **Step 2: `board_page_register` — append a slug to `meta["pages"]`**

Add near `board_pages`:
```c
/* Append `slug` to the board's ordered "pages" meta if not already listed.
   "/" is implicit and never stored. */
static void board_page_register(AppState *st, sol_u32 board, const char *slug) {
    const char *cur;
    char        buf[BOARD_PAGE_MAX * PAGE_SLUG_CAP];
    if (!slug || !slug[0] || strcmp(slug, "/") == 0) return;
    cur = scene_meta_get(&st->scene, board, "pages");
    if (cur && boardpage_contains(cur, slug)) return;
    if (cur && cur[0]) snprintf(buf, sizeof buf, "%s %s", cur, slug);
    else               snprintf(buf, sizeof buf, "%s", slug);
    scene_meta_set(&st->scene, board, "pages", buf);
}
```

- [ ] **Step 3: `boards_migrate_pages` — seed existing boards on load**

Add (after `board_page_register`):
```c
/* Seed "pages" for any board that lacks it, from its emergent page list
   (natural-sorted). Makes empty-page persistence real for pre-feature boards.
   No save here — the next scene_save persists it; re-running is idempotent. */
static void boards_migrate_pages(AppState *st) {
    Scene *s = &st->scene;
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        char  pages[BOARD_PAGE_MAX][PAGE_SLUG_CAP];
        char  buf[BOARD_PAGE_MAX * PAGE_SLUG_CAP];
        int   np;
        if (!o->mesh_ref || strcmp(o->mesh_ref, "board") != 0) continue;
        if (scene_meta_get(s, o->handle, "pages")) continue;   /* already migrated */
        np = board_pages(st, o->handle, pages, BOARD_PAGE_MAX);  /* emergent, natural-sorted */
        if (np <= 1) continue;                                   /* only "/" -> nothing to store */
        boardpage_serialize(pages, np, buf, (int)sizeof buf);
        if (buf[0]) scene_meta_set(s, o->handle, "pages", buf);
    }
}
```

- [ ] **Step 4: Call `boards_migrate_pages` on BOTH load paths**

In `world_rebuild` AND in `load_palace` (the load-derive law — `load_palace` does not call `world_rebuild`), add a call to `boards_migrate_pages(st);` near the other derive steps (after the scene is resolved; it only sets meta, order vs the mesh resolve doesn't matter). Match where `windows_migrate_fills(st)` is called (the established precedent) and add `boards_migrate_pages(st);` beside it in both functions.

- [ ] **Step 5: Gauntlet** — all three pass.

- [ ] **Step 6: Manual smoke (controller may skip — GUI):** board nav is GUI; the build + the Task-1 unit tests are the gate here.

- [ ] **Step 7: Commit**
```bash
git add main.c
git commit -m "Board pages: board_pages reads ordered meta list; seed existing boards on load"
```

---

## Task 3: Creation appends to the page list

**Files:** `main.c` (`board_new_page`, `create_folder_from_name`).

- [ ] **Step 1: `board_new_page` registers the new page**

In `board_new_page`, after the smallest-free `/page-N` `slug` is chosen and BEFORE (or after) setting `active_page`, register it:
```c
    board_page_register(st, board, slug);                 /* persist the page in creation order */
    for (i = 0; i < st->sel_count; i++)
        scene_meta_set(&st->scene, st->sel[i], "page", slug);
    scene_meta_set(&st->scene, board, "active_page", slug);
    scene_save(&st->scene, "scene.stml");
```
(The existing `scene_save` persists `meta["pages"]`. The smallest-free search via `board_pages` already includes the stored list, so it won't reuse a listed-but-empty `/page-N`.)

- [ ] **Step 2: `create_folder_from_name` registers the target page**

In `create_folder_from_name`, after the slugified `target` page is known and the folders are created, register the target (the page the forward folder links to) so a folder-created page persists + orders:
```c
    board_page_register(st, board, target);
```
Place it before the function's `scene_save` (so the existing save persists it). Verify the local is named `target` and `board` is in scope (grep the function).

- [ ] **Step 3: Gauntlet** — all three pass.

- [ ] **Step 4: Commit**
```bash
git add main.c
git commit -m "Board pages: new pages (Shift+Right, folder link) append to the ordered list"
```

---

## Final verification (controller, after all tasks)

- [ ] **Full gauntlet:** `./build.sh c89check && ./build.sh && ./build.sh metal` — all pass.
- [ ] **boardpagetest:** `./build.sh boardpagetest && ./boardpage_test` — natural order + list/contains/serialize pass.
- [ ] **Final holistic review** over the whole diff (the migration seam on both load paths, the append/contains correctness, the read fallback), then hand to the human.

## Human live-verify checklist (post-merge)

- Make several pages (`Shift+→`) → they cycle in **creation order** (`/page-1, /page-2, … /page-10`, not lexical `/page-1, /page-10, /page-2`).
- Make an empty page, navigate away (arrow-cycle / a folder) and back → it's **still there**.
- A folder (`d`) to a new page → that page is listed and ordered.
- Reload (`L`) → order + empty pages persist.
- An existing (pre-feature) board → its `/page-N` pages **reorder numerically** on first load; nothing is lost; its cards stay on their pages.
- The arrow fix (already on branch): connect two cards on a non-root page → the arrow appears on **that** page.

## Notes / known limitations

- No page-delete (v1) — pages persist once created.
- Pre-existing *empty* pages from before this change were never recorded and can't be recovered.
- `meta["pages"]` caps at `BOARD_PAGE_MAX` slugs; `boardpage_serialize` truncates safely.
- No new shader / no persistence-schema change beyond the `meta["pages"]` value.

---

## Self-review notes (author)

- **Spec coverage:** §1 storage → Task 2/3 (`meta["pages"]` written by register/migrate); §2 helpers →
  Task 1; §3 `board_pages` read + fallback → Task 2 Step 1; §4 migration on both load paths → Task 2
  Steps 3-4; §5 creation appends → Task 3; §6 unchanged (cycle/gate/tag flow through board_pages). All
  covered.
- **Symbol consistency:** `boardpage_list`, `boardpage_contains`, `boardpage_serialize`,
  `board_page_register`, `boards_migrate_pages`, `page_cmp` (natural) used identically across tasks;
  `boardpage_collect`/`page_add_unique`/`board_pages` are existing. The migration calls `board_pages`
  (which falls back to emergent natural-sorted when `meta["pages"]` is absent) — consistent.
- **No-save-in-migration** is intentional (idempotent; next save persists) — flagged in the code comment.
