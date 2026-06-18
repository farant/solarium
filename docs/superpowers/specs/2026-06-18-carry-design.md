# Carry — Design Spec

**Date:** 2026-06-18
**Status:** Approved (ready for implementation plan)
**Author:** Fran Arant (with Claude)

## Problem

Repositioning objects in a captured first-person view is awkward with drag. The
spatial-filesystem-tree work (next spec) needs a natural way to move objects and
especially to *plant door-tablets on walls*. A small, general **carry** primitive —
pick up the selected object, walk with it, set it down where you aim — solves both,
and is useful on its own for any object.

## Goals

- Pick up the **selected** object into a carried pose that tracks the camera.
- Place it onto the surface **under your aim** (floor or wall); the placed position
  is saved (§1.2 — a placed object's position is sacred).
- Works on **any** movable object — not mirror-specific.
- Reclaim the `E` key (currently the rare "mint dust" hotkey) as the carry toggle,
  demonstrating the command-palette payoff: rare commands shed their hotkeys.

## Non-goals

- **Door behavior.** Carry only knows "place this object on the surface I'm aiming
  at." Turning a door-tablet-on-a-wall into a door is the *spine* spec's job. Carry
  stays generic.
- Multi-object carry, physics/throwing, inventory.
- The top-down editor (separate later spec).

## Locked decisions (from brainstorm)

1. **`E` toggles carry** — press to pick up the selected object; press again to
   place it under your aim. One key, no double-click detection. `E` is reclaimed
   from `cmd_mint_dust` (which becomes palette-only via `:dust`).
2. **Place onto the aimed surface** — a ray from the crosshair finds the floor/wall
   you're facing; the object lands there.
3. **Carry stays generic** — no door/mirror knowledge.
4. **Coexists** with the existing drag-to-place (kept for orbit / the future
   top-down editor). Selection is unchanged.

## Design

### State
A single field on `AppState`: `sol_u32 carried` (the handle of the object currently
carried; `0` = none). Carrying one object at a time.

### Reclaiming `E`
`cmd_mint_dust`'s registry row gets its `key` set to `0` (palette-only) and its hint
cleared. `E` is then free. "Mint dust emitter" stays fully reachable as `:dust` in
the palette — only its hotkey is dropped. `R` (rescan mirrors) keeps its hotkey (it
gets *more* useful with mirror trees).

### Pick up / place — a registry command `cmd_carry_toggle` bound to `E`
Carry-toggle is a discrete edge action, so it lives in the command registry (and is
thus also palette-listed as "Carry / place selected"). ("Movable" = the placeable
objects the existing drag-to-place already moves — cards, tablets, props — not rooms,
terrain, or structural meshes.)
- **If not carrying** and a movable object is selected → set `carried = selected`.
- **If carrying** → place: cast a ray from the camera/crosshair (reuse `pick_ray`)
  against the world (reuse the collision/BVH raycast), set the carried object's
  position to the hit point (and orient to the surface normal — e.g. a tablet lies
  flat against a wall), clear `carried`, and save the scene. If no surface is hit
  within a max range, drop it at a fixed distance in front of the camera at floor
  level.
- A `can_carry_toggle` precondition: runnable when something is carried OR a movable
  object is selected.

### Carried pose (the per-frame follow)
A small update step in the main loop (alongside the other per-frame updates): when
`carried != 0`, each frame set that object's position to a fixed offset in front of
the camera (a comfortable hold distance, slightly below eye line), tracking camera
position + facing. The carried object is excluded from its own placement raycast and
from the pick that would re-select it. No physics — it simply follows.

### Persistence
Placing writes the object's new transform and saves `scene.stml` (the existing
per-mutation save path). Carrying is transient state (not serialized). If the app
exits mid-carry, the object keeps its last serialized position (no loss).

## Components / files

- **`main.c`** — the `carried` field on `AppState`; `cmd_carry_toggle` +
  `can_carry_toggle` + its `g_commands` row (key `GLFW_KEY_E`); the per-frame
  carried-follow update; the `cmd_mint_dust` key→0 reclaim. Reuses existing
  `pick_ray` + collision raycast for placement.

No new translation unit (carry is small and coupled to camera/scene/pick, like the
other `cmd_*`). **No new GPU shader** — carry only repositions an existing object, so
there is no MSL twin (tax-free on the dual-backend front).

## Testing

- **Live-verified** (consistent with how interactions/overlays are checked):
  select an object, press `E` → it lifts and follows as you walk/look; aim at the
  floor, press `E` → it lands there; aim at a wall, press `E` → it lands flat on the
  wall; `:dust` still mints dust from the palette; `R` still rescans; the carried
  object isn't double-selected or self-colliding.
- **Build gauntlet:** `c89check` + GL build + Metal build (all C changes).

## Build changes

None beyond the C edits (no new TU, no new `build.sh` source, no new shader).

## Future work (enabled, not built)

- The spine's "door-tablet on a wall → door" behavior consumes carry's
  place-on-surface result.
- Throw/toss, multi-carry, or a held-object highlight — out of scope.
