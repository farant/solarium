# Delete Pictures — Design

**Date:** 2026-06-22
**Status:** Approved

## Goal

Let the user remove a placed image (a "picture") by selecting it in first-person
and pressing **Delete** (or Backspace). Pictures live in two places — mounted on a
wall, or pinned to a whiteboard — and both are covered by one removal path.

Scope is **images only**. Whiteboards themselves remain undeletable in this pass
(deferred: a whiteboard can hold pinned children, which raises a cascade question
this feature deliberately avoids). Pictures are leaf objects with no children, so
removal is clean.

## Background — what already exists

- **Selection.** In first-person, aiming the crosshair and tapping calls
  `do_pick(st, w, 0, 0)` (`main.c:3940`), which sets `st->selected_handle` and
  plays the selection blip + prints a console line. Tapping a picture selects it
  (a click-drag instead slides it via `picture_move_pick`; a pure tap leaves it
  selected).
- **An existing delete handler.** The Backspace block at `main.c:8383` already
  removes a *selected* tombstone or arrow:
  `mesh_asset_key(o, key)` → `asset_release(&g_mesh_assets, key)` →
  `scene_remove` → `scene_save`. This block sits behind the same mode-guards that
  keep delete keys from colliding with note-text editing and the RTS editor's own
  Delete (which removes walkways).
- **Picture identity.** A picture is `KIND_PLAIN` with `mesh_ref == "picture"`.
  Its parent is a room (wall-mounted) or a board (pinned). Pictures are the only
  objects created with that ref, so `mesh_ref == "picture"` is a sufficient and
  exact guard.
- **Asset model.**
  - Picture **meshes** are keyed by `mesh_asset_key` (ref + `w`/`h`/`t` params),
    so two identical pictures share one GPU buffer; `asset_release` drops the
    refcount and frees only when the last user goes away.
  - Picture **textures** come from `load_texture(content)` (`main.c:8423`), which
    is a path-cached, refcounted, hot-reload-watched asset in `g_tex_assets`.
    Textures are session-lived in this codebase; deletion leaves them resident.

## Chosen approach

Extend the existing delete handler (Approach A). Add a third branch to the
`main.c:8383` block for `mesh_ref == "picture"`, and broaden the block's trigger
from Backspace-only to **Delete OR Backspace** (matching the editor's existing
delete trigger and the user's "delete key" phrasing). Reusing this block means the
picture delete automatically inherits its mode-guards — it cannot fire while
typing note text or while the RTS editor owns the Delete key.

Rejected:
- **B — a `:` palette command.** A second code path for the same act, and a menu
  can't aim a crosshair; doesn't match "select + press delete."
- **C — factor a `delete_selected()` helper.** Premature with only three cases;
  revisit if delete grows.

## Behavior

1. **Select** (unchanged): aim the crosshair at a picture and tap → it becomes
   `st->selected_handle`.
2. **Delete** — with a picture selected, pressing Delete or Backspace:
   1. `mesh_asset_key(o, key)` → `asset_release(&g_mesh_assets, key)` — drop the
      mesh buffer refcount (shared identical pictures survive).
   2. Clear transient references to the handle: set `st->selected_handle = 0`, and
      if `st->resize_board == handle` set it to 0, and if `st->move_board == handle`
      set it to 0 (so a stale drag/resize can't dereference a removed object).
   3. `scene_remove(&st->scene, handle)`.
   4. `scene_save(&st->scene, "scene.stml")`; print a confirmation line.
3. **Non-destructive**: only the wall/board display copy is removed. The image's
   file card already returned to its shelf when the picture was planted; the shared
   texture stays resident; the file on disk is untouched. No arrows attach to a
   picture, so no `arrows_rebuild`. No material/mesh re-resolve is needed.

## Edge cases

- **No selection / non-picture selection**: the new branch is skipped; existing
  tombstone/arrow branches keep their current behavior (now also reachable via
  Delete, which is a harmless consistency gain).
- **Mid-resize / mid-slide**: clearing `resize_board`/`move_board` in step 2.2
  prevents a dangling handle. (In practice you must release the drag before the key
  registers, but the guard is cheap and correct.)
- **Text editing / editor mode**: handled for free by the existing block's
  guards — the same protection tombstone/arrow delete already relies on.

## Testing

This is a UI/input change in `main.c`, exercised through GLFW key state and the
live scene — it is verified by **human live-verify**, consistent with every prior
carry/place/file feature (subagents can't GUI-test). The build gauntlet
(`./build.sh c89check && ./build.sh debug && ./build.sh metal`) must pass. No new
pure-geometry unit surface is introduced (no new `descend.c`/`mesh.c` math), so no
new `*_test` target.

Manual verification steps:
1. Place an image on a wall; tap it to select; press Delete → it disappears, scene
   saves, the file card is still on its shelf, reload shows it gone.
2. Repeat for an image pinned to a whiteboard.
3. Confirm Backspace also deletes a selected picture.
4. Confirm a selected tombstone/arrow still deletes (no regression), via both keys.
5. Confirm typing in a note's text and the RTS editor's walkway-delete are
   unaffected (no key collision).

## Constraints (project laws)

- Strict C89; build gauntlet must pass on both backends.
- No new shader → no MSL twin.
- Never stage/commit `NOTES.stml` or `paper-picture.png`.
- Feature branch in-place → ff-merge to main; commits end with the
  `Co-Authored-By: Claude Opus 4.8 (1M context)` line.
