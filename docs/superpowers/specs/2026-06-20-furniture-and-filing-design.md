# Furniture & Filing — Design

**Date:** 2026-06-20
**Status:** Approved (brainstorm complete; ready for implementation plan)

## Goal

Let the user **place labeled furniture** — bookshelves and tables — by previewing a
translucent ghost they aim, rotate, and drop; then **file file-tablets onto that
furniture** by carrying them. A bookshelf is a labeled, auto-arranging collection
(tablets shelve as upright spines); a table holds tablets laid flat, hand-placed
with rotation. Furniture is a true container: its tablets ride along when it moves.

## Architecture (one sentence)

A transient **place mode** previews catalog meshes through the existing alpha
`glass_pipeline` and `scene_add`s the chosen one on confirm; **filing** extends the
carry mechanic (the `descend_wall_aim` aim-snap-attach pattern) to re-parent a
carried tablet onto the furniture under your aim — exactly as the whiteboard
re-parents dragged cards today.

## Tech Stack

Existing engine: C89 (`build.sh c89check`), the `mesh.c` procedural registry, the
command palette (`command.h`/`palette.c`, incl. `palette_prompt`), the carry system
(`carry_update`/`cmd_carry_toggle`/`carry_target`), the whiteboard re-parent
precedent (`o->parent = board` + board-local pos, main.c:7141), `wtext` SDF labels,
the `glass_pipeline` + `uOpacity` alpha path (P9 item 2), and workspace tagging
(`Scene.active_ws`, from Portals). **No new shader** (the ghost reuses the alpha
PBR path) → **no MSL twin**.

---

## 1. Concepts & glossary

- **Furniture** — a placed scene object with a registry mesh (`"table"` /
  `"bookshelf"`), parent-0, tagged with the active workspace. A container: filed
  tablets are its children.
- **Catalog** — the ordered list of placeable furniture kinds (v1: `table`,
  `bookshelf`). Extensible — adding a kind extends the list, not the commands.
- **Ghost** — the catalog item's mesh drawn translucent (via `glass_pipeline` at low
  `uOpacity`) at the place cursor; not a scene object until confirmed.
- **Place mode** — a transient input-owning UI mode (like the palette) for aiming,
  rotating, cycling, and dropping the ghost.
- **Filing** — carrying a file-tablet and placing it onto furniture, which
  re-parents the tablet into the furniture.
- **Tablet** — a file/alias/note card (`mesh_ref="card"`, a non-`KIND_PLAIN` kind),
  the thing you file.

## 2. Place mode

A `"Place furniture"` palette command (`g_commands[]`, palette-only, key 0) enters
place mode. While active, place mode **owns input** (like `palette.open`): an
`AppState` flag (e.g. `place.active`) gates `read_input` so movement/hotkeys are
suspended and the keys below drive the ghost.

- **Position** — the ghost sits at the ground point under your aim
  (`carry_place_point`'s camera-forward ray vs. the ground plane + `mint_ground`
  snap). Look around to move it.
- **Cycle** — `[` / `]` step the catalog (table ↔ bookshelf); the ghost mesh
  swaps live.
- **Rotate** — `,` / `.` (and/or `←`/`→`) spin the ghost's yaw in fixed steps.
- **Confirm** — `Enter` (or a click) `scene_add`s the solid furniture at the
  ghost's transform, **tagged `meta["workspace"] = active_ws`** (Portals law: new
  top-level content carries the active workspace). Then `scene_resolve_meshes` +
  `apply_kind_materials` + `scene_save`.
- **Cancel** — `Esc` exits with nothing placed.
- **Bookshelf label** — after a bookshelf is confirmed, `palette_prompt` asks for
  its label (the `create_root_from_path` → `cmd_new_root` pattern); the typed string
  is stored as `meta["label"]`.

**Ghost rendering:** in the render pass, if `place.active`, draw the catalog mesh
once through `glass_pipeline` (alpha-blend, depth-write-off) with `uOpacity ≈ 0.4`
at the ghost transform. The catalog meshes are realized once (registry build) and
reused; no per-frame mesh rebuild. No new shader.

## 3. The furniture meshes + labels

Two new procedural registry rows in `mesh.c` (synthesized-never-sourced):

- **`"table"`** — a top slab + legs, built from `aabb_box` (the room/walkway box
  idiom). Params: `w`, `d`, `h` (and slack). The top's upper face is the filing
  surface.
- **`"bookshelf"`** — two side panels + a back + `N` evenly-spaced horizontal
  shelves, `aabb_box` boxes. Params include `w`, `h`, `d`, `shelves`. Each shelf
  span is a row of spine slots.

The bookshelf's `meta["label"]` renders as `wtext` SDF text across its top rail,
facing outward (the doorway/room-label machinery — `wtext_block`). Both meshes are
realized via the standard registry path and get a wood-ish material via
`apply_kind_materials` or a default (furniture is `KIND_PLAIN` props; see §5 note).

## 4. Filing tablets (carry, extended)

Carry a tablet (`E`, via `carry_target` — already returns cards regardless of
parent). Filing mirrors the **descent** wall-aim path in `carry_update`:

- **Furniture-aim:** each frame while carrying a tablet, test the carried object's
  kind is a card and cast the camera-forward ray at nearby furniture. If it hits a
  **table** top, snap the tablet **flat** (pitch −90°, "on its back") to the aim
  point on the table surface, applying the current carry **yaw** (the `,`/`.`
  rotate keys, shared with place mode — the rotation control requested). If it hits
  a **bookshelf**, snap the tablet to the **next free shelf slot** as an upright
  spine. Set a `file_aim` flag + the target furniture handle (the `plant_aim` /
  `plant_room` pattern).
- **Attach on place:** in `cmd_carry_toggle`, if `file_aim` is set, **re-parent**
  the tablet to the furniture (`tablet->parent = furniture; tablet->pos =
  <surface-local point>`) — exactly the whiteboard's `o->parent = board` +
  `board_pin_pos` move (main.c:7141). Else fall back to the existing ground place.
- **Detach on pickup:** carrying a filed tablet (`E`) re-parents it back to world
  (parent 0, world position) — the `drag_ground_parent` idea — so it leaves the
  collection cleanly.

This is purely an extension of the existing carry path; the whiteboard's
click-drag attach is untouched.

## 5. The container / data model

Filing **re-parents** (the board precedent), so furniture is a real container:

- **Table:** children are positioned in table-local space, lying flat, at the
  hand-placed point + the carry yaw. Free arrangement (no auto-layout).
- **Bookshelf:** children **auto-arrange** as upright spines. A pure
  `shelf_slot(i, params)` function maps the i-th filed tablet to a local transform
  (column within a shelf, shelf row top-down, fill left-to-right) — the room-tray
  `tray_slot` idiom at shelf scale. On filing, the new tablet takes the next free
  index (count of existing children). The bookshelf's `meta["label"]` names the
  collection.
- **Workspace inheritance:** because membership walks the parent chain
  (`workspace_of`), a tablet re-parented onto workspace-tagged furniture inherits
  the furniture's workspace automatically — no separate tag, and filing in a
  non-home workspace just works.
- **Kind:** furniture objects are `KIND_PLAIN` props (like the whiteboard/codex) —
  they animate nothing and carry their identity in `mesh_ref` + meta. (No new
  `ObjectKind` needed; the catalog is keyed by `mesh_ref`.)

## 6. New module + the headless geometry

A new headless module **`furniture.c` / `furniture.h` + `furniture_test`** (the
`descend.c` mold — pure geometry, no GL, no AppState), owning:

- `furniture_surface_aim(...)` — camera ray vs a table top / bookshelf bounds →
  whether it's hit and the local attach point (the `descend_wall_aim` analogue).
- `furniture_table_point(...)` — the flat-on-table local placement point + the
  lying-flat orientation.
- `furniture_shelf_slot(i, params)` — the i-th spine's local transform on a
  bookshelf (the auto-arrange layout).

`main.c` owns the glue (place-mode input, ghost draw, the carry hooks, the label
prompt), GL stays in the renderer, exactly as `descend.c`/`editor.c` keep main.c as
the only integrator.

## 7. Persistence

Furniture objects and their re-parented tablet children serialize through the
normal scene path (`scene_io`); the bookshelf label is `meta["label"]`; child
tablet local transforms persist as ordinary object transforms. No new format.

## 8. Testing

### Headless (`furniture_test.c`, in the `descend_test` style)

- **Surface-aim:** a ray from a known camera pose at a table/bookshelf returns the
  expected hit + local attach point; a miss returns no-hit.
- **Table point:** the flat-placement local point sits on the table top, and the
  orientation lies the card on its back.
- **Shelf-slot layout:** `shelf_slot(0..n)` fills left-to-right then wraps to the
  next shelf down, within the shelf's span; spacing is the spine width.
- **Catalog:** cycling wraps (table → bookshelf → table) and the count is correct.

### Human-verified (GUI — subagents can't drive the window)

- Place mode: the ghost previews translucent, tracks aim, rotates, cycles
  table↔bookshelf, drops a solid object; Esc cancels; a placed bookshelf prompts
  for its label and the label renders.
- Filing: carry a tablet → it snaps flat on a table (with rotation) / slots as a
  spine on a bookshelf; on drop it attaches; moving the furniture carries its
  tablets; picking a tablet back up detaches it.
- Furniture placed inside a non-home workspace stays in that workspace; persists
  across save/reload.
- Both OpenGL and Metal backends render the ghost + furniture (no MSL twin, but
  confirm both).

## 9. Out of scope (YAGNI for v1)

- Re-labeling / deleting a bookshelf label after placement (label set once at
  placement).
- Collision colliders for furniture (props are non-colliding in v1, like the
  whiteboard; you can walk through — revisit if it bothers in live-verify).
- Resizing furniture after placement (fixed sizes per kind, with mint-style
  variation if desired).
- Drag-to-file with the mouse (carry-and-place only; the whiteboard drag stays for
  cards-on-boards).
- Snapping furniture to walls/floors/grid, or onto other furniture.
- Bookshelf paging when shelves overflow (cap at capacity; overflow behavior is a
  follow-up).

## 10. File-by-file change map

| File | Change |
|------|--------|
| `furniture.h` / `furniture.c` (NEW) | headless geometry: surface-aim, table point, shelf-slot layout, catalog list |
| `furniture_test.c` (NEW) | the headless suite (§8) |
| `mesh.c` | `"table"` + `"bookshelf"` procedural meshes + registry rows |
| `main.c` | `AppState` place-mode state (active, catalog index, ghost yaw); the `"Place furniture"` command + place-mode input in `read_input`; ghost draw in `render()` (glass_pipeline); the carry furniture-aim hook in `carry_update`; the attach/detach re-parent in `cmd_carry_toggle`; the bookshelf label prompt; workspace-tag on confirm |
| `build.sh` | `furnituretest` mode; `furniture.c` added to the main builds (c89check/metal/asan/debug) where main.c references it |
| `.gitignore` | `/furniture_test` |

## 11. Decisions settled (this brainstorm)

- **Interaction:** aim to position, keys to rotate (`,`/`.`) + confirm (`Enter`),
  `Esc` cancel; place mode owns input.
- **Selection:** one `"Place furniture"` command + `[`/`]` cycle the catalog.
- **Containment:** filing **re-parents** the tablet into the furniture (true
  container).
- **Bookshelf fill:** **auto-arranged** upright spines (a managed collection).
- **Label:** **prompted right after** placing a bookshelf (`palette_prompt`).

## 12. Implementation details to settle in the plan

- Exact place-mode keys (avoid global hotkey collisions — but place mode owns input
  while active, so most keys are free; confirm the cycle/rotate set).
- Whether furniture gets a distinct material (a `mesh_ref`-keyed wood look) vs. the
  default `KIND_PLAIN` material.
- The carried-tablet kind gate for furniture-aim (which `mesh_ref="card"` kinds
  file: FILE/ALIAS/NOTE — and whether FOLDER stays wall-aim only).
- Bookshelf shelf count / spine width constants and the table dimensions.
- Detach-on-pickup mechanics (restore world transform; the `carry_origin` analogue).
