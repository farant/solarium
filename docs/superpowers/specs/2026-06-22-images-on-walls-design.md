# Images on Walls & Whiteboards — Design

**Goal:** Carry an image file card (png/jpg/jpeg), aim at a room wall or a
whiteboard, and drop it — a **picture** (a flat quad showing the actual image)
appears there, while the file card stays in place (like planting a folder as a
door). Wall pictures are corner-resizable; whiteboard-pinned pictures display
only, this pass.

**Context:** Every piece this needs already exists. `reader_is_image_path`
detects images; `load_texture` decodes + caches them (sRGB, hot-reload);
`descend_wall_mount` mounts flush on a wall; `board_under_ray`/`board_pin_pos`
pin to a whiteboard; `board_resize_corner` + the corner handles resize a mounted
flat quad; the descend folder-plant already keeps the carried card in place. This
pass adds a `"picture"` object and the carry/plant flow that creates it. **No new
shader.**

---

## Architecture

- **The `"picture"` object** — a new mesh ref (a flat quad, geometry identical to
  the board/card so resize math applies), `KIND_PLAIN`, with `content` = the
  image's file path. Its `material.albedo_tex` resolves from that path. `KIND_PLAIN`
  matters: `apply_kind_materials` skips it (line 8560), so the image albedo is
  never clobbered, and the mirror-scan (which keys on FILE/FOLDER/ALIAS) never
  duplicates or tombstones it.
- **Texture resolve** — a small pass: for any `"picture"` object with `content`
  and no albedo yet, `material.albedo_tex = load_texture(content)`. Runs at
  scene-load (in `scene_resolve_meshes`, after the mesh half) and at creation, so
  the image persists across reloads via the texture registry.
- **Placement** — `carry_update` gains an image-card branch; `cmd_carry_toggle`
  spawns the picture on drop and restores the card.
- **Resize** — the existing gate (`board_is_mounted`, handle render, mount-resize
  branch) is generalized from `"board"` to `"board"` OR `"picture"`.

## The `"picture"` mesh

`mesh.c` registry row: `{ "picture", 3, { "w","h","t" }, { 1.2f, 0.9f, 0.03f }, emit_card }`
— reuses `emit_card` (`make_card`, bottom-origin: `y` in `[0,h]`, UVs 0..1 on the
±Z faces), the same geometry the resize math assumes. Default 1.2 × 0.9 (a
landscape frame); resizable afterward.

## Placement flow (carry an image card, plant it)

When the carried object is a card whose `content` is an image
(`reader_is_image_path`), `carry_update` previews:

- aimed at a room **wall** (`descend_room_at` + `descend_wall_mount`, board half-
  extents from the default picture size): a flush picture preview, target = the
  room;
- else aimed at a **whiteboard** (`board_under_ray`): the image pinned to the
  board face at the hit point, target = the board.

On drop, `cmd_carry_toggle`:
1. **spawns the picture** — `scene_add` a `KIND_PLAIN` object, `mesh_ref
   "picture"`, `content` = the card's image path, parented to the room (wall, with
   the mount pose/yaw from `descend_wall_mount`) or the board (whiteboard, with the
   pin pose from `board_pin_pos`); then `scene_resolve_meshes` (builds the mesh +
   loads the albedo) + `apply_kind_materials`;
2. **restores the file card** to its pre-carry spot (`carry_origin`) — the marker
   stays, exactly like the folder-plant;
3. `scene_save`.

Capture the image path (a heap string) before `scene_add`, which can realloc the
objects array (the established alias-pin discipline).

## Render

A `"picture"` object draws through the normal opaque mesh loop with its material;
`albedo_tex` = the image and the quad's UVs map it across the face — the same lit-
albedo path `paper-picture.png` and the reader image use. No special case.

## Resize

Wall pictures are corner-resizable by generalizing the gate: `board_is_mounted`
accepts `mesh_ref` `"board"` OR `"picture"`; `board_world_corners` and the mount-
resize branch read `w/h/t` from the object's own `mesh_ref` (both schemas are
`w,h,t`, so the registry-release rebuild path is identical). A whiteboard-pinned
picture's parent is the board (not a room), so `board_is_mounted` is false →
no handles, display only. (Resizing those would need the resize plane to be the
board face — a clean follow-up.)

## Persistence

The picture saves to `scene.stml` as a normal object (mesh `"picture"`, `content`,
parent, size). On load, the texture-resolve pass re-decodes `content` via
`load_texture`. If the image file is gone, `load_texture` fails → the picture
shows its flat base color (no crash); we leave it (the record card surfaces the
disk truth, as always).

## Error handling

- Non-image card / no wall / no whiteboard under the aim → no picture preview; the
  card carries/places normally.
- `load_texture` failure → flat-colored picture, no crash.
- Spawn captures `content` before `scene_add`; never deref a `SceneObject*` across
  `scene_add`.

## Testing

- The placement geometry reuses the already-unit-tested `descend_wall_mount` and
  `board_pin_pos`; the resize reuses the unit-tested `board_resize_corner`. No new
  pure function needs its own test.
- **Live verify (Fran):** carry an image card, aim at a wall → a picture mounts
  flush showing the image, the card stays on the floor; corner-resize it; reload
  keeps both the picture (with image) and the card. Aim at a whiteboard → the image
  pins to the board face showing the image. Both `./solarium` and `./solarium-metal`.

## Scope / non-goals (YAGNI)

In: PNG/JPG/JPEG, the two targets (room wall, whiteboard face), wall pictures
corner-resizable, the file card stays. Out: whiteboard-picture resize (follow-up),
re-mounting a placed picture to a different wall by carry, cropping/rotation,
non-image content, video/animated formats.

## Files touched

- `mesh.c` — the `"picture"` registry row.
- `main.c` — picture texture-resolve pass; the image-card branch in `carry_update`;
  the picture spawn + card-restore in `cmd_carry_toggle`; generalize the resize
  gate (`board_is_mounted`, `board_world_corners`, the mount-resize branch, the
  handle render) from `"board"` to `"board"`/`"picture"`.
