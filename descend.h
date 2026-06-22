/* descend.h — fs-tree Phase 4 "descent": carry a folder tablet, aim it at a
   wall, drop it to OPEN the folder as a real sub-room (door + walkway + its
   contents). Headless: scene + geometry only (reuses editor's RoomRect), NO GL
   and NO filesystem — main.c populates the new room via room_mirror_scan. */
#ifndef SOL_DESCEND_H
#define SOL_DESCEND_H

#include "sol_base.h"
#include "sol_types.h"
#include "scene.h"
#include "editor.h"     /* RoomRect, editor_room_rect */

/* ---- pure geometry (headless, unit-tested) ---- */

/* The room (home/mirror) whose footprint contains p (XZ inside, Y near its
   floor); 0 if none. For "which room am I in" (the wall-aim while carrying). */
sol_u32 descend_room_at(Scene *s, vec3 p);

/* Cast `ray` against the 4 interior wall planes of room rect `r`; on the nearest
   forward hit within a wall's run-span (kept off the corners) and around door
   height, return 1 and fill *wall (ROOM_WALL_*) + *offset (along-wall, from
   center). Else return 0. */
int  descend_wall_aim(RoomRect r, Ray ray, float door_h, int *wall, float *offset);

/* World point on wall `wall` at `offset` along it (y = the room's floor). */
vec3 descend_door_point(RoomRect r, int wall, float offset);

/* MOUNT a flat board (half-width w_half, half-height h_half, thickness t) flush
   on the wall `ray` aims at. The height-UNCONSTRAINED sibling of descend_wall_aim:
   accepts any hit between floor and ceil_y, clamps the center so the board stays
   fully on the wall, and pushes it off the surface by t/2 (back flush). Returns 1
   with *out_wall (ROOM_WALL_*) and the world-space *out_center; 0 if no wall is
   hit or the board is bigger than the wall. */
int descend_wall_mount(RoomRect r, Ray ray, float ceil_y,
                       float w_half, float h_half, float t,
                       int *out_wall, vec3 *out_center);

/* The 4 world corners of a wall-mounted board: bottom-center origin `p`, width
   `w`, height `h`, horizontal wall axis `u` (unit; vertical is world-up). Order:
   0=bottom-left, 1=bottom-right, 2=top-right, 3=top-left. */
void board_corners(vec3 p, float w, float h, vec3 u, vec3 out[4]);

/* Resize a board by dragging one corner: `anchor` = the fixed opposite corner,
   `dragged` = the grabbed corner's new point (on the wall plane), `u` = the wall
   horizontal axis. Returns new w/h (floored at min_size) + the bottom-center
   origin. */
void board_resize_corner(vec3 anchor, vec3 dragged, vec3 u, float min_size,
                         float *out_w, float *out_h, vec3 *out_origin);

/* ---- scene ops (headless, unit-tested; built in Task 2) ---- */

/* Plant `folder_card`: build a real "mirror" sub-room for its folder outward
   from `parent_room`'s `wall` at `offset` (Y-nudged clear), a walkway joining
   them, and mark the card opened. Returns the new room handle, 0 if refused
   (not a folder / already opened / no content). The CALLER populates the room
   (room_mirror_scan) + rebuilds + saves — descend.c stays free of the fs. */
sol_u32 descend_plant(Scene *s, sol_u32 parent_room, sol_u32 folder_card,
                      int wall, float offset);

#endif /* SOL_DESCEND_H */
