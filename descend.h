/* descend.h — fs-tree Phase 4 "descent": carry a folder tablet, aim it at a
   wall to plant a door, walk into the empty preview sub-room to finalize it.
   Headless: scene + geometry only (reuses editor's RoomRect), NO GL and NO
   filesystem — main.c runs room_mirror_scan after finalize returns a path. */
#ifndef SOL_DESCEND_H
#define SOL_DESCEND_H

#include "sol_base.h"
#include "sol_types.h"
#include "scene.h"
#include "editor.h"     /* RoomRect, editor_room_rect */

/* ---- pure geometry (headless, unit-tested) ---- */

/* The room (home/mirror/preview) whose footprint contains p (XZ inside, Y near
   its floor); 0 if none. For "which room am I in" + walk-in detection. */
sol_u32 descend_room_at(Scene *s, vec3 p);

/* Cast `ray` against the 4 interior wall planes of room rect `r`; on the nearest
   forward hit within a wall's run-span (kept off the corners) and around door
   height, return 1 and fill *wall (ROOM_WALL_*) + *offset (along-wall, from
   center). Else return 0. */
int  descend_wall_aim(RoomRect r, Ray ray, float door_h, int *wall, float *offset);

/* World point on wall `wall` at `offset` along it (y = the room's floor). */
vec3 descend_door_point(RoomRect r, int wall, float offset);

/* ---- scene ops (headless, unit-tested; built in Task 2) ---- */

/* Plant `folder_card`: build a real "mirror" sub-room for its folder outward
   from `parent_room`'s `wall` at `offset` (Y-nudged clear), a walkway joining
   them, and mark the card opened. Returns the new room handle, 0 if refused
   (not a folder / already opened / no content). The CALLER populates the room
   (room_mirror_scan) + rebuilds + saves — descend.c stays free of the fs. */
sol_u32 descend_plant(Scene *s, sol_u32 parent_room, sol_u32 folder_card,
                      int wall, float offset);

#endif /* SOL_DESCEND_H */
