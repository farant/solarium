# Entity Browser — Design Spec

**Date:** 2026-06-29
**Status:** Approved (design), ready for implementation plan
**Author:** brainstormed with Fran

## Goal

A new HUD tool — a ranger/joshuto-style **Miller-columns browser** — that catalogs the
world's entities by type and lets you act on them. Top level = a list of entity TYPES;
drill in to that type's instances; a preview + a per-entity command list on the right.
v1 ships two types (**Pictures**, **Places**) and one command (**Place**). The whole thing
is built on a **type-provider registry** so new types/commands (and eventually STML-defined
types) plug in without touching the shell.

## Locked decisions (the brainstorm)

1. **HUD like the command palette** — a modal overlay that owns input while open.
2. **Miller columns** (ranger/joshuto): `[ Types | Entities | Preview + commands ]`, `h/j/k/l`
   flythrough, type-to-filter, Esc closes.
3. **Picking an entity = preview + a command list** (NOT one fixed action). v1 command = **Place**;
   Go-to and others are deferred but the command list is per-type and extensible.
4. **v1 types = Pictures + Places.** Disk (filesystem→room) and Rooms are deferred.
5. **Places = a saved-locations catalog** (name + lat/lon/zoom/basemap), seeded with a few
   cities; Place = spawn a fresh map board there. (Index-of-placed-maps rejected; that's a
   Go-to concern.) **The catalog is the future source of truth for map PINS too.**
6. **Provider-registry architecture** — a type is a plug-in `{ enumerate, preview, commands, run }`;
   this is the seam STML feeds later.

## Why this is tractable

It reuses three existing systems wholesale: the **palette** modal-input pattern (`palette.c`),
the **inventory bag's** live-thumbnail rig (forward-PBR + post, current-entity-only — `inventory.c`),
and **`fuzzy.c`** for filtering. Place reuses the **carry / `spawn_image_picture` / `spawn_map_board`**
paths. **No new shader → no MSL twin.**

## Architecture / components

### 1. `browser.c` / `.h` (+ `browsertest`) — NEW pure module, strict C89, headless

The Miller-columns **navigation state machine**, pure (no GL, no scene) in the `caret.c`/`route.c`
tradition. Operates on abstract lists of strings + counts:
- Three columns (Types, Entities, Commands) with a selected index each and a *focused* column.
- `h/←` (back/ascend focus), `l/→/Enter` (descend focus / on Commands = "activate"), `j/k/↑/↓` (move).
- A per-column **filter string** + fuzzy-ranked visible order (delegates ranking to `fuzzy.c`).
- Emits an intent on activate: `(type_index, entity_index, command_index)`.
- Headless-tested: navigation transitions, filter narrowing, focus clamping, empty columns.

### 2. The `TypeProvider` registry — `main.c` (scene/GL glue)

A static array (like `g_commands[]`), each entry:
```
typedef struct {
    const char *name;                                   /* "Pictures", "Places" */
    int   (*enumerate)(AppState*, EntityRef *out, int cap);   /* fill instance refs */
    void  (*preview)(AppState*, EntityRef, <preview target>); /* render a thumbnail */
    int   (*commands)(AppState*, EntityRef, const char **out, int cap); /* command labels */
    void  (*run)(AppState*, EntityRef, int command_index);    /* execute */
} TypeProvider;
```
`EntityRef` is a small tagged handle (e.g. a path for Pictures, a catalog index for Places). The
shell never knows type specifics — it calls these hooks. Adding a type = adding a row. **This is
the STML seam:** an STML-defined type later supplies the same four behaviors from data.

### 3. Pictures provider

- **enumerate** = scan `library/` (pasted `*.png`) + collect distinct image paths from scene
  objects where `reader_is_image_path(o->content)` + known assets (e.g. `paper-picture.png`);
  dedupe by path. Each ref = a file path (+ a flag if currently placed).
- **preview** = the **actual decoded image** drawn in the preview pane (via the cached
  `load_texture` → a flat textured quad), so you see the real picture before placing it. Cached
  by path, so re-highlighting an image is free; first highlight decodes+uploads (cursor moves are
  user-paced, not per-frame).
- **commands** v1 = `Place` → start carrying a fresh picture card for that image (reuse the carry /
  `spawn_image_picture` path, like taking an image stamp from the bag).

### 4. Places provider

- **store** = a saved-locations **catalog** on a global hidden anchor in `scene.stml` (the bag's
  hidden-anchor precedent), each place = `{ name, lat, lon, zoom, basemap }`. Seeded with ~8–10
  notable cities on first run so the type is populated.
- **enumerate** = the catalog entries.
- **preview** = a 3D map-board thumbnail at that location (reuse the map mesh + basemap via the
  inventory thumbnail rig).
- **commands** v1 = `Place` → `spawn_map_board(lat,lon,zoom,basemap)`.

### 5. The HUD (main.c) — Miller-columns rendering + input

- Opens on **`;`** (provisional — the natural neighbor to `:`; confirm). Modal: owns input like the
  palette (suppress world hotkeys while open), Esc closes.
- Renders three columns via the existing UI/text immediate-mode draw. The **preview pane is LIVE —
  it tracks the highlighted entity** (ranger's preview-follows-cursor): for **Pictures** it draws the
  actual decoded image (flat textured quad); for **Places** it renders a 3D map-board thumbnail
  (inventory rig, current-entity-only to bound render targets). The **command list** sits under the
  preview.
- Activate a command (`Enter` on the Commands column) → close HUD + call `provider->run`.

## Constraints (engine laws)

- **Strict C89** engine `.c` (`browser.c` pure C89; `browser_test.c` may be c11). RHI seam inviolable.
- **No new shader / no MSL twin** (reuses the thumbnail + UI/text + lit-albedo paths).
- **Modal input ownership** (the palette/inventory/board-view discipline): while open, every discrete
  world hotkey is suppressed; all transient state clears on close.
- Pure-vs-glue split (the `caret.c` law): the column/navigation/filter logic is headless-testable;
  scene/GL coupling stays in the providers + the main.c HUD.
- Workspace + persistence: the Places catalog persists in `scene.stml` (global anchor meta);
  Place-spawned objects are workspace-tagged via `mint_tag_ws`.

## Out of scope / Future Directions

- **Disk / filesystem provider** — browse real OS directories (via `platform_fs`) and mount a folder
  as a room/root (ties into `descend`/`create_root`). The original motivation; its own next slice
  (real traversal + file previews + the mount action).
- **Rooms provider** + the **Go-to** command (camera-fly to an entity already in the world).
- **More commands** per type (Delete, Rename, Save-to-Places, Go-to…).
- **STML-defined entity types with attributes** — the registry's far end; a type's four behaviors
  come from STML data instead of C.
- **Map PINS** — a pin pass reads the **same Places catalog** and renders any place whose lat/lon
  falls within a placed map's window (the inverse UV math already exists). Designed-for now (the
  catalog is the shared source of truth), built later.
- "Save this location" command (capture the current/aimed map into the catalog).

## Testing

- `browsertest` (headless): column navigation, focus transitions, fuzzy-filter narrowing, clamping,
  empty/one-item columns, the activate intent.
- Build gauntlet (gl + metal + asan + c89check + the `*_test` suite).
- **Human live-verify** (subagents can't GUI-test): open with `;`, drill Types→Pictures→entity, see a
  preview + Place; Place a picture (carry to hang); drill Places→city→Place (map spawns); fuzzy-filter;
  Esc closes cleanly with no leaked state; persistence of the Places catalog across relaunch.

## Risks

- **Preview render-target budget** — the inventory rig already bounds this (current-entity-only);
  keep the browser to one live thumbnail at a time (the focused entity).
- **Pictures enumeration churn** — scanning `library/` + object content each open is fine (open is
  not per-frame); dedupe carefully so a placed image and its library file don't double-list.
- **Input-ownership regressions** — the recurring trap (palette/board-view): a modal that falls
  through the gate must suppress every discrete hotkey and clear all transient state on close.
- **Naming** — `browser` module name is provisional (avoid the web connotation; could be `catalog`/
  `entreg`). Cosmetic.

## Decomposition for the plan

A single cohesive first slice, built in order:
1. `browser` pure module + `browsertest` (Miller-columns navigation/filter state).
2. The HUD shell in main.c (open key, three-column render, input ownership, Esc/close) driving a
   stub provider.
3. The `TypeProvider` registry + the preview/command wiring (reuse the inventory thumbnail rig).
4. Pictures provider (enumerate + preview + Place).
5. Places provider (seeded catalog on a global anchor + persistence + preview + Place).

Disk, Rooms, Go-to, STML types, and pins each get their own later spec.
