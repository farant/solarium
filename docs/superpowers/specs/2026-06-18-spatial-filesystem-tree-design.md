# Spatial Filesystem Tree — Design Spec

**Date:** 2026-06-18
**Status:** Designed; **queued behind the Carry spec** (which it depends on). Review
when we start this cycle.
**Author:** Fran Arant (with Claude)
**Depends on:** [Carry](2026-06-18-carry-design.md) (door-tablet planting uses carry).

## Problem

Today's mirror is **flat and manual**: a "mirror" room points at one directory and
shows its immediate entries in a grid tray; folders are *inert cards*; there is no
descent into subdirectories and essentially no spatial generation. The goal is to
turn the directory **tree** into navigable **space** — independently floating rooms
connected by walkways, generated from the filesystem, that you walk through.

This is the **spine**. A later, separate spec covers the **top-down RTS editor**
(pan, drag-rooms, auto-reroute paths).

## The shape (what you experience)

- You spawn in a small persistent **home room** (hub/anchor).
- A **"New root…" palette command** mints a **door-seed tablet** for an arbitrary
  directory path. You **carry** it to a wall and **plant** it → a door whose room
  is that directory, walked-to from home.
- Inside any directory-room: its **files are cards**; its **subfolders are
  tablets** you can carry and plant on a wall. Planting a folder-tablet makes a
  **door**, and a **preview** of that subdirectory's room floats out beyond it on a
  walkway. **Walking into the preview finalizes** it — the room persists, its files
  populate, and *its* subfolders become tablets. Repeat down the tree.
- Rooms are **independent floating platforms**; **walkways** connect them, becoming
  **stairs** where heights differ. The scene grows only where you've walked.

## Goals

- Directory tree → navigable floating rooms, materialized on visit, persisted.
- One unified placement model (door-tablets: folder tablets + root door-seeds).
- Derived embodiment: tiny authored truth, all geometry regenerated each load.
- Rescan reconciliation extended from the flat tray to the tree (§1.3 covenant).

## Non-goals (this spec)

- **The top-down RTS editor** — its own later spec/cycle.
- **Resize / transform UI** — *accommodated* (transforms edit authored params) but
  not built.
- **Floating doorframes where there's no wall** — *accommodated* (a door is a
  standalone object; a wall is one backing) but not built; v1 plants doors on walls.
- **Inode-based rename identity** — renames stay remove+add (today's limit).

## Locked decisions (from brainstorm)

1. **Nested rooms, walk-through** — folders descend into rooms you physically walk
   between (not teleport-pockets).
2. **Independently floating rooms + walkways**, stairs when heights differ.
3. **Materialize-on-visit + persist**, via a **preview → finalize** promotion: a
   planted folder shows a lightweight derived **preview** room; walking into it
   promotes it to a persisted dir-room. Scene grows only where you've stepped.
4. **Derived embodiment (one author, many readers)** — authored truth = per-room
   `source_path` + transform + the `connects` graph + per-card arrangement.
   Room **shells and walkways are regenerated every load** as readers; nothing
   geometric is stored. (This is what makes the future editor's live path-reshaping
   free, and keeps `scene.stml` tiny.)
5. **Home hub + multiple roots.** Spawn in home; create roots by minting + planting
   a **door-seed tablet** for any path.
6. **Door-tablet unification.** Folder tablets (auto-present, one per subdirectory)
   and root door-seeds (minted by command for arbitrary paths) are the *same kind of
   object*; planting any of them on a wall makes a door.
7. **You place the door, the system places the room.** A door snaps to a **standard
   height** at the horizontal spot you plant it. The room beyond is auto-positioned
   outward; if it would collide with an existing room it is nudged **up/down in Y
   only** until clear, with the walkway becoming **stairs** — collapsing collision
   resolution from a 2D/3D packing problem to a 1-D slide along the free vertical
   axis.
8. **Rooms have walls** (the drop surface for tablets, same affordance as the
   whiteboard) plus the floating floor and a walkable interior.

## Design

### Section 1 — Data model (authored truth vs derived)

**Authored in `scene.stml`:**
- **Home room** — `room_type="home"`, a transform. Spawn anchor; no directory.
- **Dir-rooms** — one per *materialized* directory (roots and subdirectories are the
  same kind). Each carries `source_path` (absolute dir), a **transform** (position
  now; size/rotation reserved for the resize-later note), and a `connects` relation
  to its parent (→ home for a root, → parent dir-room for a subdirectory).
- **File cards** — children of a dir-room (`kind=file`, `content`=path, `name`, plus
  arrangement: placed position or `unplaced` tray slot). Unchanged from today.
- **Folder tablets / doors** — a `kind=folder` object that is a **door-tablet**:
  before planting it's a carryable tablet (a card); planted on a wall it's a door
  carrying a `connects` edge to its child dir-room once materialized. (Membership
  follows disk: a folder-tablet exists iff its subdirectory does.)
- **Door-seed tablets** — minted by the "New root…" command: a door-tablet for an
  arbitrary path, not auto-derived from a parent directory.

The authored **graph** is home ↔ roots and dir-room ↔ (door ↔ child dir-room), plus
each room's transform. That graph + transforms are everything the generators read.

**Derived every load (never stored):** room shells, walkways/stairs, preview rooms.

### Section 2 — Generators (the readers)

Run at load and on materialize/rescan, like `meadow_rebuild`/`forest_rebuild`.
Synthesized, never sourced.
- **Room-shell generator** — per room: a **floating platform + walls + walkable
  interior**, at the room's transform, sized to its content footprint (later a
  `size` param for resize). Walls are the tablet drop surface.
- **Walkway generator** — per `connects` edge: a constant-width ribbon between the
  door anchor and the child room's entry, reading both endpoints' heights. Level →
  flat/ramp; large Δy → **stairs** (count/rise derived from Δy and run). This is the
  reader that makes "move/resize a room → path reflows" free.
- **Auto-placement defaults** — new file cards arrive in the tray (today's grid);
  door positions are authored by where you plant them (no auto-layout solver).

### Section 3 — Materialization & navigation

- **Tablet → door → preview → finalize:** a folder-tablet (card) is carried/dragged
  onto a wall → becomes a **door** at standard height where you planted it, and a
  **lightweight derived preview** room floats out (auto-placed outward, Y-nudged to
  avoid collisions, stairs bridging). **Walking into the preview** promotes it to a
  persisted dir-room (transform seeded from the preview position so it doesn't jump;
  files populate; `connects` edge written; its subfolders become tablets).
- **Back-nav** is free: walkways are bidirectional; root rooms walk back to home.
- **Rescan reconciliation (§1.3 extended):** for each *persisted* dir-room, walk it
  against its directory — new files → tray cards, deleted → tombstones (placement +
  notes preserved), new subfolders → tablets, deleted subfolders → tombstone the
  door / flag any persisted child stale; **never delete** — your arrangement and
  notes survive. `R` triggers it.

### Section 4 — Home hub + roots

- Fresh scene spawns you in the **home room**.
- **"New root…" palette command** prompts for a directory path (typed into the
  palette's text field), mints a **door-seed tablet** for it. You carry + plant it
  on a wall like any tablet → a root dir-room walked-to from home. Multiple roots
  supported (reuses today's multi-mirror idea). Existing workspaces / aliases /
  gather (`G`) stay as they are.

### Section 5 — Scope, fresh scene, files, testing

- **Fresh scene:** archive the current `scene.stml` under a new name; the new
  `scene.stml` starts with just the home room. (The engine already creates a default
  on missing/first-run; here the "default" becomes the home hub.)
- **Code:** evolve `mirror.c`'s `room_mirror_scan` for tree-aware reconciliation;
  add generator(s) for shells + walkways/stairs (likely a new module, e.g.
  `roomgen.c`, reading the room graph — the one-author pattern); the door-tablet /
  preview / finalize logic; the "New root" command + door-seed tablet; spawn-in-home.
- **Testing:** headless unit tests for the *pure* pieces (walkway/stairs geometry
  from endpoints; rescan reconciliation against a synthetic directory) following the
  `*_test` pattern; navigation/materialization live-verified. Watch for **new
  shaders** (floating-platform / walkway materials) — if any are added they need GLSL
  + MSL twins (grep struct AND body); reuse existing materials where possible to stay
  tax-free.

## Forward-looking (accommodated, not built)

- **Resize/transform:** edits the authored room params; geometry re-derives.
- **Floating doorframes:** a door is a standalone object; the frame generator seats
  it in a wall when there's a backing, freestanding when there isn't.
- **Top-down RTS editor:** pan/drag-rooms/auto-reroute — the next spec after this.
