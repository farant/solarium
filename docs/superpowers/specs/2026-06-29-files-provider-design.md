# Files Provider (Disk / Filesystem Browser) — Design

**Date:** 2026-06-29
**Status:** Approved (brainstorm), ready for implementation plan
**Depends on:** the entity browser (`browser.c/.h`, the `TypeProvider` registry in `main.c`), the mount machinery (`create_root_from_path`, `descend_plant`, `room_mirror_scan`), the carry mechanic (`inventory_take`), and the POSIX fs seam (`platform_fs.c`).

---

## Goal

Add a third provider — **"Files"** — to the entity browser (`;`) that lets the user walk the **real, live filesystem** starting at their home directory and, once they find a folder or file they want, either **mount a folder as a room** or **carry any file/folder into their grasp** as a tablet. This is the long-deferred "Disk/filesystem provider" — the original motivation for the entity browser.

## Background — the two seams this plugs into

The feature is almost entirely **reuse**. Two existing seams do the heavy lifting:

1. **The entity browser is a pluggable provider registry.** `TypeProvider { name, enumerate, commands, run, preview }`, registered in `g_providers[]` (cap 8). The browser is a fixed **3-column Miller layout: Types → Entities → Commands** (NOT a ranger Parent/Current/Preview tree). A new provider is one initializer row plus four callbacks; the shared shell (`browser_refresh`, `browser_handle_key`, `browser_ensure_preview`, `browser_draw_overlay`) is untouched except for one contained tweak (§4).

2. **Turning a real path into world content already exists.** `create_root_from_path(st, path)` builds a `room_type="mirror"` room ringed near the active workspace's home room, tags it `meta["source_path"]=path`, scans the directory into file/folder cards via `room_mirror_scan`, connects a walkway, and saves — the full job, today reachable only by *typing* a path into the `:` palette ("New root…"). And `descend_plant` opens a **carried** `KIND_FOLDER` card (dropped on a wall) into a sub-room. So mounting is downstream of browsing; the provider just feeds a picked path into machinery that already works.

**Key reframing:** deep filesystem exploration is the **3D world's** job (mount a folder → walk in → descend sub-folders by carrying folder-cards to walls). The Files provider is therefore a **picker** — its purpose is to *find an entry folder/file on the live disk*, not to be a full recursive file manager in the HUD.

## Decisions (settled during brainstorming)

| Question | Decision |
|---|---|
| Navigation model | **Live drill-down** through the real filesystem (not a fixed catalog of mounted folders). |
| Drive keys | **All via the Commands column** — no overload of the shared shell's `h/l/←/→`. Navigation is select-item → `Open`. |
| Mount target(s) | **Mount as root** (`create_root_from_path`) **and Carry** (a tablet into your grasp, then the existing carry+descend flow). No wall-aim-from-HUD wiring. |
| Show files? | **Yes** — folders and files, folders first; files are **Carry**-able; image files get a real-image preview. |
| Start dir | **Remember last, start `~/`** — session-persistent cwd, init to home, reset on app restart. |

## Architecture & module layout

Follows the engine's law: *split pure logic from GL/scene glue and headless-test the risky part* (the `caret.c`/`route.c` precedent).

### New: `diskpath.c` / `diskpath.h` + `diskpathtest` (pure C89, no GL/scene/POSIX)

The only genuinely tricky logic is path string math, so it lives in its own headless-tested unit:

```c
/* diskpath.h */
/* Parent directory of `path` into out[cap]. "/a/b/c" -> "/a/b"; "/a" -> "/";
   "/" -> "/" (no parent). Trailing slashes ignored. Always NUL-terminates. */
void        diskpath_parent(const char *path, char *out, int cap);
/* Join dir + name with exactly one '/'. "/a" + "b" -> "/a/b"; "/" + "b" -> "/b";
   "/a/" + "b" -> "/a/b". Always NUL-terminates; clamps to cap. */
void        diskpath_join(const char *dir, const char *name, char *out, int cap);
/* Pointer to the last path segment (basename). "/a/b" -> "b"; "/" -> "". Borrowed. */
const char *diskpath_basename(const char *path);
/* SOL_TRUE if path is the filesystem root "/" (so enumerate omits "../"). */
sol_bool    diskpath_is_root(const char *path);
```

### New helpers in `platform_fs.c` / `platform_fs.h` (POSIX quarantine, excluded from c89check)

```c
const char *fs_home_dir(void);          /* getenv("HOME"); "/" fallback. Borrowed, stable. */
sol_bool    fs_is_dir(const char *path); /* stat + S_ISDIR; SOL_FALSE on failure */
```

`fs_is_dir` lets `commands`/`run` branch folder-vs-file off a bare path `ref` (a `BrowserItem` carries only `name`+`ref`, no type bit). `fs_scan_dir`/`fs_listing_free` are reused as-is (they already sort and hide dotfiles).

### New provider block in `main.c` (beside `pictures_*` / `places_*`)

`disk_enumerate` / `disk_commands` / `disk_run` / `disk_preview`, plus one row in `g_providers[]`:

```c
{ "Files", disk_enumerate, disk_commands, disk_run, disk_preview }
```

### New `AppState` field

```c
char browser_disk_cwd[1024];   /* current dir; session-persistent; NOT serialized */
```

Initialized to `fs_home_dir()` lazily (when empty). Persists across browser opens within a session; resets on app restart.

## Provider behavior (data flow)

### `disk_enumerate(st, out, cap)`
1. If `browser_disk_cwd[0]==0`, set it to `fs_home_dir()`.
2. Unless `diskpath_is_root(cwd)`, emit item 0 = **`../`** with `ref = diskpath_parent(cwd)`.
3. `fs_scan_dir(cwd, &l)`. If it fails (permissions/gone), stop here — the `../` alone lets the user back out.
4. Two passes over the listing: **folders first**, then files. For each, `name` = entry name (folders displayed bracketed, e.g. `[Projects/]` — cosmetic only), `ref = diskpath_join(cwd, entry.name)`.
5. Cap at `BROWSER_MAX_ITEMS` (2048); if the directory exceeds it, `log()` the truncation (no silent drop).
6. `fs_listing_free`; return count.

### `disk_commands(st, ref, out, cap)`
Branch on `fs_is_dir(ref)` only (a `BrowserItem` carries no type bit, and `commands` receives only `ref`):
- folder → `["Open", "Mount as root", "Carry"]`
- file → `["Carry"]`

The `../` entry is just the parent directory (`ref` = the parent path), so it lands in the folder branch and `Open` ascends — no special-casing needed. Offering Mount/Carry on the parent folder is a valid action, so the redundancy is harmless. (Order: `Open` first because traversal dominates. Labels are static strings, per the `const char **` contract.)

### `disk_run(st, ref, cmd)` → returns int (0 = close, 1 = stay; see §4)
- **Open** → copy `ref` into `browser_disk_cwd`, set `st->browser_items_type = -1` (invalidate the entities cache), reset `browser.sel[1]=0` and clear `browser.filter[1]`, **return 1** (stay open + re-focus Entities + refresh).
- **Mount as root** → `create_root_from_path(st, ref)` (verbatim reuse), **return 0**.
- **Carry** → spawn a tablet and put it in the grasp (§5), **return 0**.

### `disk_preview(st, ref, *out_aspect)`
- If `reader_is_image_path(ref)` → `load_texture(ref)` + `image_dims(ref, …)` for aspect (identical to `pictures_preview`).
- Else → `{.id = 0}` (no preview). Folder child-count / icon previews are deferred.

## The one shared-shell tweak — "Open stays open"

Today `browser_handle_key` does `run(...)` then unconditionally `browser_open = SOL_FALSE` on ACTIVATE. To let **Open** re-list without closing:

- Change the `TypeProvider.run` function-pointer type from `void (*run)(…)` to **`int (*run)(…)`** (0 = close, 1 = stay).
- `pictures_run` / `places_run` get a trivial `return 0;`.
- In `browser_handle_key`: `if (g_providers[ti].run(st, ref, cmd)) { st->browser.focus = 1; browser_refresh(st); } else { st->browser_open = SOL_FALSE; }`.

Cache invalidation rides `browser_items_type = -1`, which forces `browser_refresh` to re-enumerate even though the provider **index** didn't change (the existing refresh only re-enumerates on provider change — this is the subtlety that makes drill-down work). This is the entire change to shared code.

## Mount / Carry reuse (no new placement code)

- **Mount as root** is the same code path as the `:` "New root…" palette command, fed a picked path instead of a typed one. Result: a `room_type="mirror"` room near home, walkway-connected, `room_mirror_scan`'d into cards, `scene_save`'d.
- **Carry** follows `pictures_run`'s exact shape, respecting realloc discipline:
  1. Capture `anchor = inventory_anchor(st)` (may `scene_add` → realloc) **before** adding the card.
  2. `h = scene_add(...)` parented to `anchor`; `scene_kind_set(h, fs_is_dir(ref) ? KIND_FOLDER : KIND_FILE)`; `scene_content_set(h, ref)`; `scene_meta_set(h, "name", diskpath_basename(ref))`; `mesh_ref = "card"`; `mint_tag_ws(st, h)`.
  3. `scene_resolve_meshes` + `apply_kind_materials` (and `folderbook_materialize` on the load/rebuild path, as the other mints do) — producing the same folder/file card representation `room_mirror_scan` emits — then `inventory_take(st, h)` to put it on the cursor.

The card is born meeting `descend_plant`'s preconditions (`KIND_FOLDER` + non-empty `content`, no `planted` meta), so **dropping a carried folder on a wall opens it as a sub-room through the existing descend flow** — nothing new to wire. A carried file behaves like any other file card (readable, shelvable, fileable).

## Persistence & workspace

- `browser_disk_cwd` is session-only `AppState` — never written to `scene.stml`.
- Mounted rooms persist exactly as today: their `meta["source_path"]` re-scans on load via `rescan_mirrors`; the card list round-trips with placements/notes/tombstones.
- Carried/dropped cards inherit the active workspace via `mint_tag_ws`, so a folder mounted in workspace "A" does not leak into "home".

## Testing

- **`diskpathtest`** (new, headless): parent/join/basename/is_root incl. edges — root `/`, trailing slash, empty string, no separator, buffer clamp, multi-segment.
- **`browsertest`** (existing): must still pass — the shell tweak touches shared code, so this is the regression guard.
- **Live-verify by Fran** (subagents can't GUI-test): open `;` → Files; drill down and up (`Open` / `../`); remember-last across reopen; Mount as root → walk into the room; Carry a folder → drop on a wall → descends; Carry a file → read it / shelve it; image-file preview renders right-side-up and aspect-correct.
- **Full gauntlet** every task: `./build.sh` (GL), `./build.sh metal`, `./build.sh c89check`, `./build.sh asan`, `diskpathtest`, `browsertest`. **No new shader → no MSL twin.**

## Error handling / edge cases

- **Unreadable / permission-denied dir** → `fs_scan_dir` fails; enumerate still emits `../`, so the user can always back out.
- **Filesystem root `/`** → no `../`.
- **> 2048 entries** → truncated at `BROWSER_MAX_ITEMS`; `log()` the count dropped.
- **Long paths** → 1024-byte buffers; `diskpath_join`/`diskpath_parent` clamp to `cap`.
- **Realloc** → every scene mutation captures `ref`/anchor into locals before the first `scene_add`.

## Out of scope (deferred follow-ons)

- Showing dotfiles (a toggle).
- "Mount as room here" (sub-room of the room you're standing in) — Carry+descend covers the need for now.
- Folder previews (child count / thumbnail), volume/drive enumeration, rename/delete commands.
- Per-provider keybinding for faster drill-down (the ranger `l`/`h` variant we declined).
