/* mirror.h — the mirror-room reconciler (P3 item 6). A MIRROR room reflects
   a real directory under the §1.3 split: MEMBERSHIP FOLLOWS DISK (files
   appear and vanish with the folder), ARRANGEMENT FOLLOWS THE USER (§1.2: a
   placed card is never auto-moved). All the trust lives in these rules being
   boringly predictable. Strict C89, above every seam (filesystem access goes
   through platform_fs.h). */
#ifndef MIRROR_H
#define MIRROR_H

#include "scene.h"

/* Reconcile a mirror room against its source directory, both directions
   (matched by content PATH — the v1 identity key; a rename reads as a
   tombstone + a new arrival):
   - a disk entry with no card -> an UNPLACED card in the tray (never auto-
     placed into the arrangement);
   - a FILE/FOLDER card whose path vanished -> a TOMBSTONE, keeping its
     place and metadata (your note survives the file's deletion; dismissal
     is manual);
   - a TOMBSTONE whose path returned -> resurrected in place.
   Placed cards are otherwise never touched. Returns the number of changes,
   or -1 if the directory won't open. Run scene_resolve_meshes afterwards:
   new cards arrive as empties. */
int room_mirror_scan(Scene *s, sol_u32 room, const char *dirpath);

#endif /* MIRROR_H */
