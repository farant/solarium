# Portals & Workspaces — Design

**Date:** 2026-06-20
**Status:** Approved (brainstorm complete; ready for implementation plan)

## Goal

Let the spatial filesystem hold more than one **world**. A *workspace* is a named,
self-contained archipelago of rooms. A *portal* is a freestanding gate you walk
through to travel from one workspace to another. Gates come in linked pairs, so
there is never a dead end.

## Architecture (one sentence)

All workspaces live in the **single `scene.stml` file** as tagged partitions;
exactly one workspace is *active* (rendered, collidable, pickable, routed) at a
time, and walking through a portal swaps the active partition in memory and sets
the player down at the paired return gate — no file is loaded or swapped.

## Tech Stack

Existing engine: C89, scene graph (`scene.c`/`scene_io.c`), `mesh.c` procedural
registry, the command palette (`command.h`/`palette.c`), `collide.c`, the BVH
pick/cull (`main.c`), `route.c`/`connections_rebuild`, `editor.c`. No new
external dependencies. **No new shader** (the gate reuses existing materials), so
**no MSL twin** to keep in sync.

---

## 1. Concepts & glossary

- **Workspace** — a named partition of the one scene: its own rooms, walkways,
  portals, props. Identified by a string name (e.g. `"home"`, `"photos"`).
- **WorkspaceAnchor** — a parent-0 empty object with `meta["workspace_name"]`,
  one per workspace. It is the *wrapper*: the thing that gives a workspace
  identity and a display name, and the object the "Portal to…" command
  enumerates. The `"home"` anchor is created on load if it does not exist.
- **Active workspace** — the one currently live. Held in
  `AppState.active_workspace` (a fixed char buffer; default `"home"`).
- **Portal / gate** — a placed object (`KIND_PORTAL`) you walk through to travel.
  Gates are created and persist in pairs.

## 2. Data model

### 2a. Workspace membership (the tag)

Every **top-level** content object (room anchors, walkways, portals, terrain,
and any other parent-0 object) carries `meta["workspace"] = "<name>"`. Children
(cards in a room, props parented to a room/anchor) do **not** need their own tag
— they inherit membership through their parent chain.

**Absent tag ⇒ `"home"`.** This is the migration rule: the entire scene that
exists today has no `workspace` tag, so every existing object belongs to
`"home"` with zero migration. New workspaces stamp their top-level objects.

`workspace_of(const Scene *s, sol_u32 handle)` resolves an object's workspace:
walk the parent chain to the first object bearing `meta["workspace"]`; if none is
found before parent 0, return `"home"`. The walk is ≤3 hops in practice
(card → room anchor → parent 0), so it is cheap enough to call per object in hot
loops.

### 2b. Portals (`KIND_PORTAL`)

Add a kind, keeping the enum value aligned with its `KIND_NAMES` index so
serialization round-trips:

- `scene.h`: append `KIND_PORTAL` after `KIND_TOMBSTONE` (value 6).
- `scene_io.c`: append `"portal"` to `KIND_NAMES` (index 6).

A gate is a parent-0 object with `mesh_ref = "portal"` and link meta:

| meta key            | meaning                                                        |
|---------------------|----------------------------------------------------------------|
| `portal_id`         | this gate's stable id, `"<workspace>-<n>"` (see 2c)            |
| `target_ws`         | the **workspace name** this gate leads to                      |
| `target_portal_id`  | the `portal_id` of the return gate in `target_ws`              |
| `name`              | display label (the destination workspace name)                 |
| `workspace`         | the workspace this gate *belongs to* (the side you enter from) |

The link rides **string ids, never runtime handles** — handles are per-load and
reassigned, but ids are stable across save/load.

### 2c. Portal id scheme

When creating a pair linking workspace `A` (current) to workspace `B`:

- `id_A = "A-" + (count of portals already tagged workspace A, + 1)`
- `id_B = "B-" + (count of portals already tagged workspace B, + 1)`

Because workspace names are unique and ids are namespaced by name, ids are unique
across the whole file. The two gates cross-reference:
`A-gate.target_portal_id = id_B`, `B-gate.target_portal_id = id_A`,
`A-gate.target_ws = "B"`, `B-gate.target_ws = "A"`.

## 3. The active-workspace filter (the heart of the feature)

Only the active workspace is live. Every place that **enumerates the scene**
skips objects whose `workspace_of(...)` is not the active workspace:

- **Render** — the per-frame draw enumeration over `st->scene.count` in `main.c`
  (the draw-submission loop near `main.c:3071`/`main.c:9665` and the frustum-cull
  feed) gains the active guard.
- **Collision** — `collide_rebuild` (`collide.c:309`) skips non-active objects so
  you never bump invisible walls from another world.
- **Pick / cull** — the BVH build (`main.c:2995`) only inserts active objects, so
  clicks and frustum culling see only this world.
- **Connections / routing** — `connections_rebuild` (`main.c:4100`) and `route.c`
  only consider rooms in the active workspace (otherwise a hidden room would draw
  a walkway/doorway).
- **Editor** — the RTS top-down view (`editor.c:184` room enumeration) shows only
  active-workspace rooms.

**Mechanism:** `AppState.active_workspace` holds the name. The per-frame loops
read it from `AppState`. The rebuild-based readers (`collide_rebuild`,
`connections_rebuild`, the BVH build) are given the active workspace name (by
parameter or by reading it from the scene) so they filter at build time; they are
already re-run on every switch. The exact passing mechanism (new parameter vs. a
scene-level field) is an implementation detail to settle in the plan — the
requirement is that **no scene reader may forget the filter**, since a reader that
does will leak ghosts from another world. This is the single highest-risk area
and gets explicit test and review attention.

`world_refilter` is not a separate pass: membership is derived on demand via
`workspace_of`. Switching simply changes `active_workspace` and re-runs the
existing rebuilds.

## 4. Creating workspaces & gates (command palette)

Two rows added to `g_commands[]` (`main.c:6816`), both palette-only
(`key = 0`), both using the existing text-entry prompt
(`palette_prompt`, mirrored on `create_root_from_path` at `main.c:6766`):

### 4a. "New workspace…"

1. Prompt for a name; reject empty / duplicate names (a name already owned by a
   WorkspaceAnchor).
2. Create a `WorkspaceAnchor` (`meta["workspace_name"] = name`) at parent 0.
3. Build a fresh **home room** for the new workspace, tagged
   `meta["workspace"] = name` — factor the home-room construction out of
   `populate_home_scene` (`main.c:8228`) into a reusable
   `add_home_room(scene, workspace_name, pos)`. Its position may overlap the real
   home in world space; it is hidden while inactive, so overlap is harmless.
4. `scene_add` the **return gate** inside the new workspace (tagged to `name`),
   positioned near its home room, with link meta pointing back to the current
   workspace.
5. `scene_add` the **outbound gate** in the **current** workspace (tagged to the
   current workspace), spawned in front of the player (the spawn-in-front pattern
   already used for props — `carry_place_point` / the `B` whiteboard at
   `main.c:5945`), with link meta pointing at `name` and `id_B`.
6. Resolve meshes, apply materials, `connections_rebuild`, `collide_rebuild`,
   save `scene.stml`. (You stay in the current workspace; the new world waits
   behind its gate.)

### 4b. "Portal to…"

1. Prompt for / fuzzy-complete an **existing** workspace name (enumerate the
   WorkspaceAnchors). Reject the current workspace and unknown names.
2. Append a **return gate** into the target workspace (tagged to it) and an
   **outbound gate** in the current workspace (steps 4–6 above), linked as a pair.

Because both gates live in the same file, there is **no cross-file write** — both
are `scene_add`ed into the live scene and saved with the next `scene_save`.

## 5. Travel

### 5a. Trigger

Per frame, after movement, test the player capsule against the **mouth** of each
active-workspace gate (a small trigger volume inside the arch opening — *not* the
frame, so brushing a post does not fire). This rides alongside the existing
per-frame proximity work (e.g. `current_room` detection around `main.c:2928`).

**Travel is blocked while carrying** (`st->carried != 0`): put the item down
first. This avoids cross-workspace object ownership entirely in v1.

### 5b. The switch

On a confirmed trigger by gate `G` (whose `target_ws = B`,
`target_portal_id = id_R`):

1. `scene_save(&st->scene, "scene.stml")` — persist the world you are leaving.
2. `strcpy(st->active_workspace, B)`.
3. Re-run the rebuild tail (factor the shared portion of `load_palace`
   (`main.c:8190`) into `world_rebuild(st)`: `connections_rebuild`,
   `collide_rebuild`, `meadow_rebuild`, `forest_rebuild`, `apply_kind_materials`
   — **but not** `scene_load`/`scene_reimport_glbs`, since nothing was loaded from
   disk). Pick/cull need no explicit step here: the BVH is rebuilt on its own
   cadence (`main.c:2995`) and, once its build is filtered (§3), it reflects the
   new active workspace automatically.
4. Find the gate `R` in the now-active workspace with `portal_id == id_R`. Set the
   camera just in front of `R` on its entry side, yaw facing into world `B`
   (mirror the startup spawn at `main.c:8760` — there it offsets `+2.0` from the
   home room; here it offsets from the gate along the gate's facing). Fallback if
   `R` is missing (data drift): spawn at `B`'s home room, or its WorkspaceAnchor.

### 5c. Arrival debounce

You arrive standing in `R`'s trigger volume; without a guard you would bounce
straight back. Record the arrival gate handle in `AppState`; suppress its trigger
until the player has left its volume once, then clear the guard.

## 6. The gate mesh & rendering

- Synthesized in the `mesh.c` registry as `mesh_ref = "portal"` — per the
  synthesized-never-sourced law — a rectangular/arched doorframe plus a glowing
  inner quad. Keep it simple in v1; it can grow prettier later.
- The shimmer is a **static emissive** material (or the existing P9 stained-glass
  alpha pipeline) → **no new shader, no MSL twin**.
- `apply_kind_materials` gives `KIND_PORTAL` its distinct emissive look.
- Collision: the gate is a **trigger only** in v1 — the frame is non-solid, so
  walking through is frictionless. (Frame colliders are a possible later refinement.)
- `KIND_PORTAL` is excluded from carry/drag targeting (you cannot pick up a gate)
  and from the mirror/alias card logic.

## 7. Migration & backward compatibility

- Launch is unchanged: load `scene.stml`, `active_workspace = "home"`. Absent
  `workspace` tags ⇒ everything is `"home"`. Existing scenes need no conversion.
- A `"home"` WorkspaceAnchor is created on load if none exists (so "Portal to…"
  can always enumerate home).
- An older build loading a portal-bearing scene degrades `KIND_PORTAL` to
  `KIND_PLAIN` (the documented `kind_from_name` fallback) — non-fatal.

## 8. Testing

### Headless (`portal_test.c`, in the style of `descend_test.c`)

- **Pair creation** writes correct link meta on both gates (ids cross-reference,
  `target_ws` correct on each side, both tagged to their own workspace).
- **Id scheme** is unique and namespaced (`"A-1"`, `"B-1"`, second pair `"A-2"`…).
- **`workspace_of`** resolves: tagged top-level object → its name; child of a
  tagged room → the room's name; untagged parent-0 object → `"home"`.
- **Arrival lookup** finds the gate by `portal_id` and the spawn point lands in
  front of it on the correct side, facing in.
- **Membership filter** predicate returns the right active/inactive verdict for a
  two-workspace fixture (the unit behind the render/collide/pick guards).

### Human-verified (GUI — subagents cannot drive the window)

- "New workspace…" creates a gate; walking through lands you in an empty home
  room; the return gate is there; walking back returns you to the origin gate.
- The previous world is fully gone while away (no ghost rooms/walls/cards), and
  fully restored on return.
- Travel is refused while carrying.
- Both OpenGL and Metal backends (no MSL twin, but verify the gate renders on both).

## 9. Out of scope (YAGNI for v1)

- Animated / shader-driven shimmer (static emissive first).
- Renaming or deleting workspaces; moving a portal between workspaces.
- Portals that land at a *specific room or spot* inside a world (v1 lands only at
  the paired return gate).
- Carrying objects through a portal (blocked in v1).
- A "map of all worlds" overview.
- Per-workspace environment (sky/sun/IBL stay global; only objects partition).

## 10. File-by-file change map

| File | Change |
|------|--------|
| `scene.h` | `KIND_PORTAL` appended to the `ObjectKind` enum |
| `scene_io.c` | `"portal"` appended to `KIND_NAMES` (index aligned) |
| `mesh.c` / `mesh.h` | `"portal"` procedural mesh in the registry (arch + shimmer quad) |
| `platform_fs.*` | *(none — single-file model needs no new fs helper)* |
| `workspace.c` / `workspace.h` (new) | `workspace_of`, `add_home_room`, portal-pair creation, id scheme, the membership predicate — headless, no GL |
| `main.c` | `AppState.active_workspace` + arrival-debounce fields; the active filter at render/cull/BVH; the two `g_commands[]` rows + their prompt callbacks; per-frame gate trigger; `world_rebuild` factored from `load_palace`; arrival spawn |
| `collide.c` | active-workspace filter in `collide_rebuild` |
| `route.c` / `connections_rebuild` | active-workspace filter on room enumeration |
| `editor.c` | active-workspace filter on room enumeration |
| `workspace_test.c` (new) | the headless suite in §8 |
| `apply_kind_materials` (main.c) | `KIND_PORTAL` emissive material |
| `.gitignore` | add `/workspace_test` |

## 11. Implementation details to settle in the plan

- How the active workspace name reaches the `Scene*`-only readers
  (`collide_rebuild`, `connections_rebuild`, BVH build): new parameter vs. a
  scene-level field. Recommendation: thread it as a parameter or a small
  filter-predicate so `collide.c`/`route.c` stay free of app state.
- Exact `world_rebuild` factoring out of `load_palace` (which steps are shared vs.
  load-only).
- The gate trigger-volume dimensions and the arrival stand-off distance.
- Where to store the arrival-debounce state on `AppState`.
