/* mirror.c — the mirror-room reconciler (P3 item 6). See mirror.h for the
   truth rules. Identity is the content PATH (v1): simple, serializable,
   predictable — its known limit is that a rename on disk reads as a
   removal plus an arrival, which the rules surface honestly rather than
   guess about. */

#include "mirror.h"
#include "platform_fs.h"

#include <string.h>

/* The tray: where new arrivals wait, by the room's south side — rows of
   slots in the ROOM'S LOCAL space (cards are children of the room anchor;
   assumes the anchor is unrotated, true of every room built so far). Auto-
   layout applies ONLY here: the moment a card is dragged it is placed, and
   placement is the user's forever. */
#define TRAY_COLS   8
#define TRAY_X0   (-1.6f)
#define TRAY_DX     0.45f
#define TRAY_Z0     2.2f
#define TRAY_DZ   (-0.42f)

static vec3 tray_slot(int i) {
    int  col = i % TRAY_COLS, row = i / TRAY_COLS;
    vec3 p;
    p.x = TRAY_X0 + (float)col * TRAY_DX;
    p.y = 0.0f;
    p.z = TRAY_Z0 + (float)row * TRAY_DZ;
    return p;
}

int room_mirror_scan(Scene *s, sol_u32 room, const char *dirpath) {
    FsListing l;
    Mesh      empty = {0};
    quat      qid;
    int       i, added = 0, tray_n = 0;
    sol_u32   j;

    if (!fs_scan_dir(dirpath, &l)) return -1;
    qid.x = 0.0f; qid.y = 0.0f; qid.z = 0.0f; qid.w = 1.0f;

    /* continue the tray after any cards still waiting from earlier scans */
    for (j = 0; j < s->count; j++) {
        const SceneObject *o = &s->objects[j];
        const char *u;
        if (o->parent != room) continue;
        u = scene_meta_get(s, o->handle, "unplaced");
        if (u && strcmp(u, "1") == 0) tray_n++;
    }

    for (i = 0; i < l.count; i++) {
        char     full[1024];
        sol_u32  h;
        int      found = 0;
        vec3     pos, one;

        if (strlen(dirpath) + strlen(l.entries[i].name) + 2 > sizeof full)
            continue;                                /* a path we can't hold */
        strcpy(full, dirpath);
        strcat(full, "/");
        strcat(full, l.entries[i].name);

        for (j = 0; j < s->count; j++) {             /* match by path identity */
            const SceneObject *o = &s->objects[j];
            if (o->parent == room && o->content && strcmp(o->content, full) == 0) {
                found = 1;
                break;
            }
        }
        if (found) continue;

        pos = tray_slot(tray_n++);
        one.x = 1.0f; one.y = 1.0f; one.z = 1.0f;
        h = scene_add(s, room, empty, pos, qid, one);
        scene_kind_set(s, h, l.entries[i].is_dir ? KIND_FOLDER : KIND_FILE);
        scene_content_set(s, h, full);
        scene_meta_set(s, h, "name", l.entries[i].name);
        scene_meta_set(s, h, "unplaced", "1");
        scene_mesh_ref_set(s, h, "card");
        if (l.entries[i].is_dir) {                   /* folders: a heavier card */
            float p[3];
            p[0] = 0.42f; p[1] = 0.55f; p[2] = 0.08f;
            scene_mesh_params_set(s, h, p, 3);
        }
        added++;
    }

    fs_listing_free(&l);
    return added;
}
