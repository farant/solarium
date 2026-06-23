# Editor: Drag / Connect / Resize Islands & Abbeys — Design

**Date:** 2026-06-23
**Status:** Approved

## Goal

Extend the top-down RTS editor so terrain **islands** and **abbeys** can be
dragged, connected with walkways (to rooms and to each other), and — for plain
islands — resized. The whole feature reduces to one idea: **a terrain island
becomes a first-class editor footprint, read the same way a room is.** An
"abbey" is not a special editor concept; it is a terrain island that has a
church `plot`-linked to it.

## Background — what exists today

- **Editor rooms** (`editor.c`, `route.c`, `main.c`): a room is an *empty*
  anchor (no `mesh_ref`) carrying `meta["room_type"]` ("home"/"mirror"), plus a
  `mesh_ref="room"` shell child holding the footprint in its `mesh_params[0]`/`[1]`
  (w/d). `editor_room_under` (editor.c:198) picks any active empty with a
  `room_type`; `editor_room_rect` (editor.c:12) derives the `RoomRect`
  (cx,cz,floor_y,hw,hd) by scanning for the "room" child; `editor_apply_move`
  (editor.c:118) writes the anchor pos; `editor_apply_resize` (editor.c:128)
  rewrites the "room" child's w/d params (and, as of the latest fix, repositions
  wall-mounted boards/pictures). `editor_connect` (editor.c:91) mints a
  `mesh_ref="walkway"` empty with two "connects" relations; `editor_can_connect`
  (editor.c:69) just needs both to carry `room_type`.
- **Routing** (`route.c`): `collect_rooms` (route.c:34) keeps only
  `room_type` in {"home","mirror"}; `room_half` (route.c:15) reads w/d from the
  "room" child (defaults 4×4 if absent); `routes_pass1` solves the walkway path
  and is type-agnostic downstream. `connections_rebuild` (main.c:4162) calls
  `route_all`, builds each walkway mesh via `make_walkway_L` between two door
  points, and re-tessellates room shells with door cutouts
  (`route_room_openings_in`).
- **Islands** (`cmd_mint_island`, main.c:6426): a *single* object —
  `mesh_ref="terrain"`, `meta["room_type"]="terrain"`, `mesh_params =
  [w, d, sub=56, relief, seed]`, `pos` = center (xz) + ground (y). Its wild
  dressing (a hero tree, erratics, an optional pond) are CHILDREN in the
  island's local frame. The grass meadow + forest are FIELD data, re-derived
  from the island each frame (not scene objects).
- **Abbeys** (the `Z` handler, main.c ~8718): a terrain island
  (`room_type="land"`) PLUS a separate parent-0 church anchor (an empty,
  `meta["room_type"]="church"`, `meta["plot"]` = the island's `nid`) whose
  children are the gothic stone meshes (church_stone/glass/roof/floor/decals),
  each carrying `mesh_params=[w,d,seed,style,ruin]` derived from `church_plan`.
- **The gaps:** islands have a `mesh_ref`, so `editor_room_under` skips them
  (not pickable); `editor_room_rect`/`editor_apply_resize`/`room_half` all assume
  the "room" child; `collect_rooms` filters out "terrain"/"land"/"church".

## The footprint abstraction (the core)

A single helper resolves any editable object's footprint, replacing the inline
"room"-child scan in `editor_room_rect` and `route.c`'s `room_half`:

```
footprint(object) ->
  if it has a mesh_ref=="room" child:        room  (w/d from the child params)   [today]
  else if object mesh_ref=="terrain":        island (w/d from the object's own params[0]/[1])
  -- a church (room_type=="church") is NOT a footprint; it rides its island.
```

- **Editable footprints = rooms + terrain islands.** A terrain island's
  `RoomRect` = `{cx=pos.x, cz=pos.z, floor_y=pos.y, hw=params[0]/2, hd=params[1]/2}`.
- **Resizable** = rooms, and terrain islands that have **no** church plot-linked
  to them. An island that carries an abbey is movable + connectable but shows no
  resize handles (abbey resize is out of scope — see below).

## Drag

- **Pick:** `editor_room_under` skips objects with a `mesh_ref`; relax it to also
  admit `mesh_ref=="terrain"`, and **exclude** `room_type=="church"` (a church's
  footprint sits inside its hill, so clicking an abbey lands on the hill, which
  drags the unit). The body/edge/corner classification (`editor_classify`) is
  footprint-relative and already generic.
- **Move:** `editor_apply_move` writes the anchor/object `pos` (already generic).
  **Abbey-as-unit:** when the moved object is a terrain island, also shift every
  object whose `meta["plot"]` equals the island's `nid` by the same world delta —
  the church (and any plot-linked dressing) rides its hill. Pure scene logic →
  lives in `editor.c`. Scene structure is unchanged (no re-parenting).

## Resize

- **Terrain branch in `editor_apply_resize`:** write the new w/d to the island's
  own `mesh_params[0]/[1]` (via `scene_mesh_params_set`, releasing/rebuilding the
  shared mesh through the registry path the engine already uses for param edits),
  not a "room" child.
- **Re-ground the hero dressing:** terrain height is a function of the size
  params, so a resize re-shapes the ground. For each direct child of the island,
  scale its local `x`/`z` by the per-axis ratio (`nhw/hw`, `nhd/hd`) and re-snap
  its local `y` to `terrain_height(new params, lx, lz)`, keeping the tree/rocks/
  pond on the land. (`terrain_height`'s location decides whether the re-ground
  runs in `editor.c` or via a `main.c` hook — resolved in the plan.)
- **Handles:** the overlay draws resize handles only for resizable footprints
  (room, church-less island); an abbey hill shows move + connect affordances but
  no resize corners.

## Connect (paths / walkways)

- **`collect_rooms`** includes terrain islands (`room_type` "terrain"/"land")
  alongside "home"/"mirror".
- **`room_half`** gets the terrain branch (read w/d from the island's own params).
- **Doorless endpoints:** a room connects through a door cut in a wall; an island
  has no walls, so a walkway to an island **terminates at the point on the
  island's footprint edge nearest the other endpoint** — a path meeting the land,
  with **no door cutout**. The routing solver picks that edge point; the existing
  L-shaped `make_walkway_L` still draws between the two endpoints.
  `connections_rebuild` skips the shell re-tessellation (door cutting) for terrain
  endpoints (there is no "room" shell to cut).
- This yields room↔island, island↔island, and room↔abbey (via its hill).
  `editor_can_connect` already accepts any `room_type`.

## Scope notes (YAGNI)

- **Abbeys are not resizable** — the church stone is baked from `church_plan` at
  fixed proportions; re-deriving it on drag is a separate, larger effort.
- **Churches are not independent footprints** — they ride their island for drag,
  and are not walkway endpoints (you connect to the abbey's hill/land).
- **First-person drag is unchanged** — islands/abbeys remain immovable in
  first-person (§1.2 / architecture-is-sacred); this is editor-only "god mode".
- **No new shader** — walkways, terrain, and the overlay all reuse existing
  pipelines.

## Files

- **`editor.c` / `editor.h`** — the footprint lookup (terrain branch in
  `editor_room_rect`), terrain pick in `editor_room_under`, abbey group-move in
  `editor_apply_move`, terrain resize + hero re-ground in `editor_apply_resize`,
  a `resizable` predicate. Pure → unit-tested in `editor_test`.
- **`route.c`** — `room_half` terrain branch, `collect_rooms` includes terrain,
  the doorless edge endpoint. Pure → unit-tested in `route_test`.
- **`main.c`** — `connections_rebuild` walkway-to-island (skip the door cutout
  for terrain endpoints); `editor_draw_overlay` draws island footprints and gates
  resize handles by the `resizable` predicate; the re-ground hook if
  `terrain_height` is not linkable into `editor.c`.

## Testing

- **Pure math** (`editor.c`, `route.c`) → unit tests:
  - `editor_test`: footprint of a terrain island (rect from own params); a
    terrain pick; resize a church-less island (params update + a hero child
    re-positions); abbey group-move (dragging the hill shifts a plot-linked
    church by the same delta); `resizable` false for an island with a church.
  - `route_test`: a walkway routes between a room and a terrain island (the
    island endpoint resolves to a footprint-edge point; the route is valid).
- **Visual / interactive** → human live-verify, build gauntlet green:
  `./build.sh c89check && ./build.sh debug && ./build.sh metal &&
   ./build.sh editortest && ./build.sh routetest`. Manual: drag an island; drag
  an abbey (church follows); resize a plain island (dressing stays grounded);
  connect room↔island, room↔abbey, island↔island; reload → layout persists.

## Constraints (project laws)

- Strict C89; the gauntlet (incl. `editortest` + `routetest`) green on both
  backends. `editor.c`/`route.c` stay pure (no GL); scene/GL glue in `main.c`.
- No new shader → no MSL twin.
- Never stage/commit `NOTES.stml` or `paper-picture.png`.
- Feature branch in-place → ff-merge to main; commits end with the
  `Co-Authored-By: Claude Opus 4.8 (1M context)` line.
