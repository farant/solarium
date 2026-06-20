# Top-Down Editor — Design

**Date:** 2026-06-19
**Status:** Approved (brainstorm complete; next = implementation plan)
**Arc:** Item 3 of the spatial-filesystem direction (carry → fs-tree → **top-down RTS editor**).

## Goal

A bird's-eye editor mode for the spatial filesystem tree: look down on the floating
rooms from an angled orthographic vantage and **drag rooms to reposition, resize their
footprint, and re-wire the walkway connections between them** — with walkways and
doorways re-threading live as you work.

## Why now

Phase 3 ("walkways take 2") made this cheap. Walkway and room-shell geometry are now
fully *derived* from the routes each load (the §1.4 one-author law: `route.c` is the
sole author, `connections_rebuild` + `collide.c` are the two readers). Nothing geometric
is stored, so moving a room and watching the paths re-thread costs nothing extra — the
editor is essentially "change the authored room transform/params/graph, everything
re-derives."

## Decisions (from the brainstorm)

1. **Camera = angled orthographic** (axonometric). Tilted ~50°, parallel projection, so
   you still read height/stairs but scale is consistent — a room doesn't change size as
   it moves, which keeps dragging and resizing predictable. (Rejected: angled
   perspective — far rooms foreshorten; pure top-down flat — hides the vertical
   dimension.)
2. **Fixed camera orientation** for v1 (one canonical angle). Keeps handle hit-testing in
   stable screen-space; rooms rarely occlude (ring-spread). Rotation is a clean follow-on.
3. **v1 scope = drag + resize + re-wire connections.** Raising/lowering rooms in Y (which
   turns walkways into stairs) is the deferred follow-on.
4. **Direct manipulation** ("grab what you want"): one left-drag, behavior depends on what
   is under the cursor — body = move, outline/corner handles = resize, the connection node
   above a room = connect. (Rejected: tool modes; modifier-key chords.)

## Architecture — mode, camera, controls

It is a **new camera mode over the same scene**, not a separate world. Same objects, a
different way to look at and touch them.

- **Mode:** add `CAMERA_RTS` to the `CameraMode` enum (alongside `WALK`/`FLY`/`ORBIT`,
  `camera.h:17`). An editor-active concept lives in the new editor module (below).
- **Entry/exit:** a palette command **"Top-down editor"** (plus a hotkey if a free key
  exists — the letter keyspace is crowded, so palette-only is acceptable) toggles it.
  Entering lifts the camera to a high angled vantage centered on the room cloud; exiting
  restores first-person at the prior standing position. The mouse **cursor is visible**
  (`GLFW_CURSOR_NORMAL`) in the editor — everything is point-and-drag.
- **Projection:** `camera_proj` (`camera.c:132`) gets an **orthographic branch** gated on
  `CAMERA_RTS`. This is pure CPU — the projection is a uniform, so **no shader/Metal-twin
  work**. Pitch fixed ~50°. The orthographic *extent* is what zoom changes.
- **Controls:**
  - **Pan** — WASD (reuse the existing movement inputs, remapped to slide the view across
    the XZ plane) and drag on empty background.
  - **Zoom** — scroll widens/narrows the ortho extent (not a dolly; the angle never
    changes).
  - **Pick/drag** — left button via the existing BVH pick (`pick_at`, `main.c:3313`), with
    the room-shell pick-transparency (`pick_skip_land`, `main.c:2947`) flipped *on* for
    this mode so rooms become the primary target.

## Manipulation mechanics

Three grab-zones per room:

```
        (•)  <- connection node: hovers above room center
     o-------o
     | /src  |   outline + corners = RESIZE handles
     | body  |   interior = MOVE
     o-------o
```

### ① Move

Grab the body, slide on the XZ plane at the room's current height (no vertical change in
v1). The room parent's `pos` updates live each frame (parent-0, so world == local).
**Walkways and doorways re-thread live during the drag** — the RTS payoff —
by calling `connections_rebuild` as the room moves. If per-frame rebuilds of a large graph
ever bite performance, the fallback is to re-thread on release; build live first.

### ② Resize

Grab an edge or corner handle on the footprint outline. The grabbed wall moves; the
**opposite wall stays put** (standard editor feel). Because room shells are center-origin,
"opposite wall stays" means: adjust the shell's `mesh_params[0]/[1]` (w/d) **and** shift
the room parent's `pos` by half the delta along that axis. This is a deliberate user edit,
not an auto-move, so it is consistent with the SACRED-position law (§1.2). Apply a
min-size clamp so a room can't collapse. Each change rebuilds that room's shell mesh and
re-derives the routes (doors re-place against the new walls). Corner handles move two
walls at once.

### ③ Connect / disconnect

- **Connect:** grab the connection node above a room, rubber-band a line to the cursor,
  release over another room → create a walkway object (parent-0, two `connects` edges via
  `scene_rel_add`) and thread it with `connections_rebuild`. Guards: **no self-connect, no
  duplicate pair**.
- **Disconnect:** click a walkway to select it (walkways are already pickable — not in
  `pick_skip_land`), press **Delete** (and a "Disconnect selected" palette command) →
  remove the walkway object and re-thread.
- **Persistence:** walkways and their `connects` edges are real scene objects, so re-wiring
  saves and loads normally.

### Persistence

Every drop / resize / connect **autosaves**, reusing the existing save-on-release path
(`scene_save` + `collide_rebuild` + `connections_rebuild`) — exactly like prop-dragging
does today (`main.c:6880`).

### Selection

Reuse the existing golden-tint highlight (`uHighlight`, FS at `main.c:628` / MSL twin
`main.c:386`) for the picked room or walkway.

## Scope boundaries (non-goals for v1)

- Edits **rooms + walkways only.** Cards/props *inside* rooms remain first-person drag.
- Edits the spatial **walkway graph**, not the filesystem tree — a room's `source_path` is
  untouched. (Re-homing a room in the tree via re-wiring is a forward-note.)
- **No vertical raise/lower** (the deferred follow-on that turns walkways into stairs).
- **No grid-snap** (free drag; snap is a forward-note).
- To step inside a room, toggle back to first-person (teleport-into-room is a forward-note).

## File structure

- **`camera.h` / `camera.c`** — add `CAMERA_RTS`, the orthographic projection branch in
  `camera_proj`, and an **ortho pick-ray variant** (today's `camera_ray`, `camera.c:140`,
  is perspective-only: eye-through-NDC; ortho needs parallel rays whose origin varies
  across the view rect).
- **`editor.c` / `editor.h`** (new TU) — owns the editor: the active flag, the
  move/resize/connect interaction state machine, handle + port hit-testing, the
  rubber-band connect, and the overlay draw (resize handles, port nodes, rubber-band
  line). Keeps this out of main.c's bulk. The editor computes *intent*; it signals scene
  changes back to main.c rather than reaching into the RHI/scene-rebuild orchestration
  itself.
- **`main.c`** — wires it in: the toggle command row in `g_commands[]` (`main.c:6633`),
  calling the editor's per-frame update + overlay draw, flipping pick policy + cursor mode
  on entry, and performing `connections_rebuild` + `collide_rebuild` + `scene_save` when
  the editor signals a change (this orchestration already lives in main.c).
- **`editor_test.c`** (new, ASan, following `route_test`/`collide_test`) — unit-tests the
  pure geometry: handle hit-testing, the resize math (center-shift correctness against the
  fixed-opposite-wall rule), and connect validation (self / duplicate-pair guards). Add a
  `build.sh` target (`editortest`) mirroring `routetest`.

## Technical notes / risks

- **Ortho pick ray** is the one genuinely new bit of camera math. For a parallel
  projection the ray direction is the camera forward for every pixel; the origin slides
  across the view rectangle by the NDC point scaled to the ortho extent. Dragging on the
  ground plane is still a ray-vs-plane intersect, just with that ortho ray.
- **Live re-thread cost.** `connections_rebuild` rebuilds GPU meshes for every walkway and
  room shell. Per-frame during a drag is the goal (small graphs); the on-release fallback
  is a one-line change if needed.
- **Resize center-shift** must keep the opposite wall fixed — verify with a unit test
  (grab +X edge by Δ: width += Δ, center.x += Δ/2, so the −X wall world-position is
  unchanged).
- **Build gauntlet** stays `./build.sh c89check && ./build.sh debug && ./build.sh metal`,
  plus the new `editortest` and the existing `routetest`/`coltest`. Strict C89:
  declarations at block top, no shader changes expected (projection + overlay reuse
  existing pipelines).

## Forward notes (accommodated, not built)

- **Camera rotation** (90° yaw snaps) to peek at occluded sides.
- **Raise/lower rooms in Y** → walkways become stairs (the next phase after this).
- **Grid-snap** for position and size.
- **Re-home in the tree** — a re-wire could re-parent a room's `source_path` relationship,
  not just the spatial walkway.
- **Teleport-into-room** — double-click a room in the editor to drop first-person inside it.
