/* mirror.h — the mirror-room reconciler (P3 item 6). A MIRROR room reflects
   a real directory under the §1.3 split: MEMBERSHIP FOLLOWS DISK (files
   appear and vanish with the folder), ARRANGEMENT FOLLOWS THE USER (§1.2: a
   placed card is never auto-moved). All the trust lives in these rules being
   boringly predictable. Strict C89, above every seam (filesystem access goes
   through platform_fs.h). */
#ifndef MIRROR_H
#define MIRROR_H

#include "scene.h"

/* Reconcile a mirror room against its source directory: every disk entry
   not yet present (matched by content PATH — the v1 identity key; a rename
   reads as remove+add) becomes an UNPLACED card in the tray rows by the
   room's south side, kind FILE or FOLDER. Existing cards are never touched.
   (6c adds the other direction: tombstones for vanished files.)
   Returns the number of cards added, or -1 if the directory won't open.
   Run scene_resolve_meshes afterwards: new cards arrive as empties. */
int room_mirror_scan(Scene *s, sol_u32 room, const char *dirpath);

#endif /* MIRROR_H */
