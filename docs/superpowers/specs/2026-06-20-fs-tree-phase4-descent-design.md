# FS-Tree Phase 4 — Descent — Design

**Date:** 2026-06-20
**Status:** Approved (brainstorm complete; next = implementation plan)
**Arc:** Phase 4 of the spatial-filesystem tree (carry ✓ → home ✓ → roots ✓ → walkways ✓ → **descent** → rescan). Master spec: `2026-06-18-spatial-filesystem-tree-design.md` (locked decisions 1–8). The top-down editor (item 3 of the wider arc) is already merged.

## Goal

Walk *down* the directory tree: inside a mirror room, carry a **folder tablet** to a wall, **plant** it to open a **door**, an **empty preview** sub-room floats out beyond it on a walkway, and **walking into the preview finalizes** it — the room fills with that folder's contents and its subfolders become tablets. Repeat down the tree. The scene grows only where you've walked.

## Why now

Phase 3 made it cheap: routing (`route.c`) derives every walkway + door from the `connects` graph each load, and the recent perf pass made that derivation fast. So "descend" is just **add a room + a walkway** — doors and bridges derive as they already do. Carry (`E`) and the lightweight room builder (`create_root_from_path`) already exist to build on.

## Decisions (from this brainstorm; master decisions 1–8 still hold)

1. **Planting = aim at a wall.** While carrying a folder tablet, a new wall-aim ray (camera-forward vs the 4 interior wall planes of the room you're standing in) snaps the tablet flat to the wall you're looking at, at your aim point. `E` plants it there. (Rejected: drop-on-floor-near-a-wall; a no-planting "Descend" command — both lose the embodied ritual.) This sidesteps the engine's general no-ray-vs-wall limit by only testing the current room's 4 walls, whose geometry we already know.
2. **Empty preview, fills on walk-in.** Planting creates a real-but-empty `room_type="preview"` room (shell + name, no card scan), visually distinct. Walking into its footprint finalizes it: flip to `"mirror"`, scan its contents, save. (Rejected: fully-derived-never-stored preview [heaviest]; materialize-fully-on-plant [drops the ritual].) The preview being *stored* (lightweight) is the one deliberate, pragmatic deviation from the master spec's "preview rooms derived, never stored," chosen for simplicity.

## Architecture — the descent flow

You're inside a mirror room; its folder entries are cards (`kind=KIND_FOLDER`, `content=`full path) from `room_mirror_scan`.

1. **Carry** — select a folder card, press `E` → existing carry pose (no carry changes).
2. **Aim at a wall** — `descend_wall_aim` casts the camera-forward ray against the 4 interior wall planes (at ±hw on X, ±hd on Z, between floor and ceiling height) of the room the player stands in; picks the nearest hit whose point lies within the wall span and below door height; returns `{wall, offset-along-wall, hit}`. While carrying a folder, the tablet snaps flat to that wall at the (clamped-to-span) offset, with a ghost door preview.
3. **Plant** (`E`) → and the scene saves:
   - The folder card is marked `planted="1"`; its tray card **stops drawing** — the derived door + its name label become its visible form (no separate marker object).
   - A **preview room** (`room_type="preview"`, `source_path`=folder path, `name`=basename) is created **directly outward** from the planted wall (a standard gap beyond it), **nudged in Y only** until clear of existing rooms (the locked 1-D collision rule). Because the preview sits straight out from the plant, the router's cost-minimizing door lands ~at your aim point — so we don't need to pin the door to a stored offset.
   - A **walkway** (parent-0, two `connects` edges: parent room ↔ preview room) is created → `connections_rebuild` opens the door in the wall (near the plant, since the room sits just outside) and builds the bridge; the door carries the folder's name via the existing doorway labels.
4. **Preview is empty** — shell + name only, a visually distinct material (dimmer/ghostly). No card scan.
5. **Walk in** — a per-frame test (`descend_room_at` on the camera position) detects entering a `preview` room → `descend_finalize`: flip `room_type` `preview→mirror`, `room_mirror_scan` to populate cards (subfolders become tablets), normalize material, save. Idempotent: a `mirror` room is never re-scanned.
6. **Repeat** — plant one of its folder tablets to descend further.

The Phase-3 routing pipeline is **untouched**: planting only adds a room + a walkway for the router to read.

## Data model & persistence

**Authored truth (stored):**
- Each room: `room_type` ("home" / "mirror" / **new "preview"**), `source_path`, `name`, transform.
- Folder cards: `kind`, `content`, and — once planted — a `planted="1"` meta (the card stops drawing; the door represents it). No wall/offset stored — the door derives from the walkway like any other.
- Walkways: parent-0, two `connects` edges.

**Derived every load (unchanged):** room shells, walkway/stair geometry, door openings — `route.c` + `connections_rebuild`.

- The **planted folder card persists** (with `content`=path), so the existing rescan check ("a card with this `content` already exists → don't duplicate") keeps a planted folder from reappearing as a tray card — no special-casing.
- **Preview rooms are stored** lightweight (shell + name, no cards) — the deliberate simplicity tradeoff.
- **Finalize** = flip meta + `room_mirror_scan` + normalize material + save; idempotent (guard on `room_type == "preview"`).

## Code structure

- **New TU `descend.c` / `descend.h`** — descent logic, out of main.c's bulk, headless-testable where it counts:
  - `int descend_wall_aim(RoomRect r, Ray ray, float door_h, int *wall, float *offset)` — pure geometry (ray vs the room's 4 wall planes); returns 1 + fills wall/offset on a valid hit, else 0. Unit-tested.
  - `sol_u32 descend_room_at(Scene *s, vec3 p)` — the room (home/mirror/preview) whose footprint contains `p` (XZ in footprint, Y near floor); 0 if none. For "current room" + walk-in. Unit-tested.
  - `sol_u32 descend_plant(Scene *s, sol_u32 parent_room, sol_u32 folder_card, int wall, float offset)` — builds the preview room (outward from the wall, Y-nudged) + the walkway + marks the card planted; returns the new preview room handle (0 on refusal, e.g. already planted / not a folder). Scene mutation, headless-testable (asserts room+walkway+meta+edges).
  - `sol_bool descend_finalize(Scene *s, sol_u32 preview_room)` — flip + scan + idempotence guard; returns whether it promoted. Unit-tested (flips, populates, second call is a no-op).
  - `descend_test.c` (ASan, C89, like `route_test`/`editor_test`) + a `descendtest` build target.
- **`main.c`** wires it in: the carry path gains the wall-aim snap (while carrying a `KIND_FOLDER` card) + a plant branch on `E`; a per-frame walk-in check calls `descend_finalize`. Reuses `carry`, `connections_rebuild`, `collide_rebuild`, `scene_save`, `room_mirror_scan` (the room builder logic from `create_root_from_path` is factored so the preview/sub-room reuses it).

## Scope boundaries (v1 non-goals)

- **Door-seed-tablet unification** (master decision 6): "New root…" stays immediate (today's `create_root_from_path`). Only **folder tablets** get carry→plant→descend in v1. (Forward-note: unify "New root…" to mint a plantable tablet later.)
- **Tree-structural rescan reconciliation** (a folder deleted on disk → tear down its door + sub-room) is **Phase 5**. Per-room rescan keeps working; Phase 4 doesn't tear down planted structure on rescan yet.
- **Floating doorframes** (door with no wall) — already a master-spec non-goal.

## Technical notes / risks

- **The wall-aim ray** is the one new bit of math. It needs the "current room" (from `descend_room_at` on the player) and that room's world wall planes (center ± half-extents at the room's Y). Ray-vs-plane (already in `sol_math`) for each of the 4 walls; nearest forward hit within the wall's run-span and below `door_h`.
- **Preview placement outward from the wall:** the preview room's center = the planted door's world point + the wall's outward normal × (door-gap + half the preview's depth). Then `root_spot_occupied`-style Y-nudge until clear. The router then opens the parent door near the plant (the room is straight out) and the preview's own door facing back.
- **Walk-in finalize timing:** finalize mutates the scene (adds cards) + rebuilds connections — do it once on entry (edge-triggered: was-outside → now-inside a `preview`), not every frame inside.
- **Build gauntlet** stays `c89check && debug && metal`, plus the new `descendtest` and the existing `routetest`/`coltest`. Strict C89. No new shaders expected (preview material reuses the PBR path; the ghost look is a material tweak, not a new pipeline).

## Forward notes (accommodated, not built)

- Door-seed-tablet unification for "New root…".
- Phase 5: tree rescan reconciliation (add/remove doors + sub-rooms as disk changes).
- Un-plant / collapse a descended branch back to a tray card.
- Preview look polish (translucency / animated materialize on finalize).
