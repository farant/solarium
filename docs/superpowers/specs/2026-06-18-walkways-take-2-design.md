# Walkways Take 2 — Design

**Date:** 2026-06-18
**Status:** Approved (brainstorm complete)
**Branch:** `feature/fs-tree-phase3` (builds on the unmerged take-1 commits; the whole of Phase 3 merges together once this lands)

## Goal

Replace the fs-tree Phase 3 walkway system so paths route as **single-bend, right-angle L-shapes** that exit and enter rooms **perpendicular through real doorways**, and fix the disappearing-walkway bug — all driven from a single shared source of truth so paths and doorways can never disagree.

## Context — what take-1 got wrong

The unmerged Phase 3 (commits 9513b33, a93aa5d, 46da8fe) built straight diagonal walkways between room *edges*. Live-verify (2026-06-18) found three problems:

1. **No doorways** — a path butts a solid wall instead of entering through an opening.
2. **Disappearing walkway** — when a 2nd root is created, the 1st root's walkway vanishes.
3. **Corner-to-corner diagonals** — paths cut diagonally between room corners instead of exiting/entering walls at right angles.

The root cause of #1/#3 is that walkway geometry and room-wall geometry were computed independently. This redesign unifies them.

## Approved decisions (from brainstorm)

- **Path shape:** single-bend L (one 90° corner; straight/0-bend when rooms line up on an axis).
- **Doorway style:** thick walls with reveal (jambs + lintel underside + threshold) — reuses the existing built-around-the-gap geometry.
- **Architecture:** one `connections_rebuild(st)` pass (not two cooperating functions).
- **Corner landing:** yes, a small flat platform at the bend.
- **Door size:** standard ~2.1 m tall, ~1.4 m wide.
- **Stairs:** rise distributed as a steady climb across the L's total run.
- **Bug:** folded into this rewrite.

## Architecture: the shared **route** (one author, two readers)

The §1.4 "one author, many readers" law applied to connections. A **route** is computed once per `connects` edge from the two rooms' world positions and extents, and fully determines the connection:

```
Route {
    sol_u32 room_a, room_b;        /* the two connected room (parent) handles  */
    int     wall_a, wall_b;        /* which wall each room opens (N/E/S/W enum) */
    float   off_a,  off_b;         /* door center offset along that wall span   */
    vec3    door_a, door_b;        /* world positions of the two door centers   */
    vec3    corner;                /* the single L bend (== a leg meeting point) */
    float   door_w, door_h;        /* opening size (≈1.4 wide, 2.1 tall)         */
    /* rise/legs are derived from door_a, corner, door_b at emit time           */
}
```

Two readers consume routes; because they read the *same* route, the door and the path always line up:

- **Walkway reader** — emits the L ribbon mesh (leg A → landing at corner → leg B) + its collider.
- **Doorway reader** — each room collects the routes that touch it, groups them per wall, and rebuilds its shell with exactly those openings + collider.

### `connections_rebuild(st)` — the single pass

Replaces today's `walkways_rebuild` (main.c:4088) and absorbs room-shell opening. Ordering:

1. **Collect edges.** Scan all `walkway` objects; each carries two `connects` rels → its two room handles. (Walkway objects remain the persisted record of an edge; their geometry stays derived.)
2. **Compute routes.** For each edge, run the routing rules (below) to fill a `Route`. Door-offset spreading needs per-wall grouping, so this is done with knowledge of sibling edges on the same wall (one global routing pass, not isolated per-edge).
3. **Emit walkways.** For each route, rebuild the owning walkway object's mesh (L ribbon + landing) and params.
4. **Emit rooms.** For each room, gather the routes that name it, turn them into a per-wall opening list, and rebuild the room shell mesh with openings.
5. Caller then runs `collide_rebuild` and `scene_save` as today.

Nothing geometric is stored — routes are recomputed every load (`scene_resolve_meshes` makes the default closed shells + default walkway meshes, then `connections_rebuild` re-derives). **Old saved scenes migrate for free**, and the future top-down editor's live path-reshaping is free.

## Routing rules (single-bend L)

Both rooms are axis-aligned squares (not rotated; local axes = world axes). For an edge between room A (center `a`, half-extent `ea`) and B (center `b`, half-extent `eb`), let `dx = b.x-a.x`, `dz = b.z-a.z`:

- **Aligned** (`|dx|` or `|dz|` ≈ 0): straight path; doors on the two facing walls; 0 bends.
- **Diagonal** (both significant): exit along the axis of **greater separation**; one 90° corner; enter the partner perpendicular on the other axis.
  - If `|dx| ≥ |dz|`: A opens its **E/W** wall (sign of `dx`), B opens its **N/S** wall (sign of `-dz`); corner near `(b.x ± offset, a.z)`.
  - Else: A opens **N/S**, B opens **E/W**; mirror of the above.
- **Door offset** along the chosen wall = centered, **unless** multiple routes share the same `(room, wall)` → spread evenly along that wall span with a margin (this is what multi-opening walls are for). The corner is then derived from the two door world-positions so both legs stay perpendicular.

The "lower room is the anchor, climb `dy` to the higher" idea from take-1 is preserved via the rise being derived from `door_a.y`/`door_b.y`.

## Doorway geometry

Generalize the single-opening `make_wall_with_opening` (mesh.c:228, already battle-tested in the gothic kit) into a **room builder that accepts a list of openings per wall**:

```
make_room_doored(b, w, d, h, t, ceil, openings[], n_open)
  opening = { int wall; float center; float width; float height; }
```

Each wall with K openings emits **K+1 solid piers** + **a header per opening** + **a threshold per opening**, all built-around-the-gap (real-thickness boxes emitting only exposed faces — fronts/backs, outer ends, jambs/reveals, tops/bottoms). A wall with 0 openings is one solid thick box. Floor (and optional ceiling) as today. This is the multi-opening generalization of the existing single-door function; the same anti-coplanar discipline applies (no two quads at the same depth → no z-fighting).

Rooms become **derived each load** like walkways: `scene_resolve_meshes` builds the default closed thick shell from the `room` mesh_ref params; `connections_rebuild` then rebuilds the shell child with its doorways. The room object structure is unchanged (parent empty carrying `room_type`/`source_path`/`name` + a child shell with `mesh_ref="room"`); only the shell mesh gains openings.

**Walls gain thickness** (scene-wide visual change, accepted): `t` ≈ 0.2 m. Interior extent is preserved by keeping the inner face at the room's nominal half-extent (`room_half_extent` continues to report the interior so routing math is unaffected).

## Stairs + corner landing

- The L ribbon is the take-1 stepped ribbon (`make_walkway`, `WALKWAY_STEP_RISE` in mesh.h), now emitted as **two legs** meeting at a **small flat square landing** at the corner (avoids a mitered joint and gives the bend a readable platform).
- Total rise distributed as a steady climb across the combined run length; the step-up treaty (~0.2 m) keeps the result walkable.

## Collision

- **Room collider:** thick walls **with the door gaps left open** (pier boxes between/around openings, matching the mesh) + the floor box already added in take-1.
- **Walkway collider:** follows the two L legs + the landing, mirroring the emitted step math (the existing `walkway` collide case generalizes from one ribbon to two legs + landing).

## Affected units

- `mesh.c` / `mesh.h` — add `make_room_doored` (+ its `Opening` struct); generalize/retain `make_wall_with_opening`; L-ribbon emission helper for two legs + landing. Registry rows as needed.
- `main.c` — replace `walkways_rebuild` with `connections_rebuild` (routing pass + both readers); update `create_root_from_path` (main.c:6524) and `populate_home_scene` to call it; keep `object_is_walkway` exclusions.
- `collide.c` — generalize the `room` case to thick multi-opening walls; generalize the `walkway` case to two legs + landing.
- The `connects` graph, room-shell parent/child structure, ring placement (`create_root_from_path`), and palette "New root…" command are unchanged.

## Verification

- **Build gauntlet (subagent bar):** `./build.sh c89check && ./build.sh debug && ./build.sh metal` all pass. Strict C89 (declarations at block top), and the dual-backend rule — but note this feature is **CPU mesh/collision only, no new shaders**, so there is no MSL twin to keep in sync.
- **Human live-verify checklist (Fran):**
  1. Fresh scene → spawn in home.
  2. New root east of home → path is straight (0 bends), enters/exits perpendicular through visible doorways with reveals.
  3. New root up-and-to-the-side → single-bend L, both ends perpendicular through doorways.
  4. **Second root does NOT make the first root's walkway disappear** (the bug).
  5. Two roots that land on the same wall of home → two spread doorways, two paths, no overlap.
  6. Walk the paths: doorways are walkable, stairs/landing are walkable, no fall-through.
  7. Reload the scene → everything re-derives identically.

## Out of scope (YAGNI)

- Manhattan multi-segment routing / weaving around obstacle rooms (revisit when the world is crowded).
- Multi-bend Z "facing-wall" paths.
- Arched/decorated doorframes (the gothic-arch variant of the build-around-gap pattern).
- Editing door size/position by hand (the editor phase).
