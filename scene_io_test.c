/* scene_io_test.c — headless exercise for scene_save. Builds a small scene of
   empties (zero Mesh, so no GL needed) carrying nids, mesh refs, and the
   overbuilt slots, saves it to STML, then reads the file back and prints it.
   Built by `build.sh iotest` under ASan/UBSan.

   We construct vec3/quat by field rather than via sol_math.h: that header is
   all-static, and pulling it into a TU that uses only a couple of helpers would
   trip -Wunused-function. scene.h (via sol_types.h) gives us the plain types. */

#include "scene.h"
#include "nid.h"     /* NID_LEN — for stack buffers holding a copied nid */

#include <stdio.h>
#include <string.h>

static vec3 v3(float x, float y, float z) {
    vec3 v; v.x = x; v.y = y; v.z = z; return v;
}

static quat q_identity(void) {
    quat q; q.x = 0.0f; q.y = 0.0f; q.z = 0.0f; q.w = 1.0f; return q;
}

static void dump_file(const char *path) {
    FILE *f = fopen(path, "r");
    int   c;
    if (!f) { printf("(could not reopen %s)\n", path); return; }
    while ((c = fgetc(f)) != EOF) putchar(c);
    fclose(f);
}

/* Byte-for-byte file comparison. */
static int files_equal(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb");
    FILE *fb = fopen(b, "rb");
    int   ca, cb, same = 1;
    if (!fa || !fb) { if (fa) fclose(fa); if (fb) fclose(fb); return 0; }
    do {
        ca = fgetc(fa);
        cb = fgetc(fb);
        if (ca != cb) { same = 0; break; }
    } while (ca != EOF);
    fclose(fa);
    fclose(fb);
    return same;
}

int main(void) {
    Scene   scene;
    Mesh    empty;
    sol_u32 floor_h, anchor_h, box_h;
    const char *path = "scene_io_test.stml";
    char    a_floor_nid[NID_LEN + 1], a_box_nid[NID_LEN + 1], a_anchor_nid[NID_LEN + 1];

    empty.vbuffer.id = 0;     /* a zero Mesh -> transform-only; no GL involved */
    empty.ibuffer.id = 0;
    empty.index_count = 0;

    scene_init(&scene);

    floor_h  = scene_add(&scene, 0,        empty, v3(0.0f, 0.0f, 0.0f), q_identity(), v3(1.0f, 1.0f, 1.0f));
    anchor_h = scene_add(&scene, 0,        empty, v3(0.0f, 0.0f, 0.0f), q_identity(), v3(1.0f, 1.0f, 1.0f));
    box_h    = scene_add(&scene, anchor_h, empty, v3(1.5f, 1.0f, 0.0f), q_identity(), v3(1.0f, 1.0f, 1.0f));

    scene_mesh_ref_set(&scene, floor_h, "grid");
    scene_mesh_ref_set(&scene, box_h,   "box");

    scene_meta_set(&scene, box_h, "title",  "Test Box");
    scene_meta_set(&scene, box_h, "author", "Solarium");
    scene_rel_add(&scene, box_h, "orbits", anchor_h);
    scene_content_set(&scene, box_h, "notes/box.txt");

    /* remember the minted ids up front; both the round-trip and keystone
       sections find the same objects by these later */
    strcpy(a_floor_nid,  scene_get(&scene, floor_h)->nid);
    strcpy(a_box_nid,    scene_get(&scene, box_h)->nid);
    strcpy(a_anchor_nid, scene_get(&scene, anchor_h)->nid);

    if (!scene_save(&scene, path)) {
        printf("FAIL: scene_save could not write %s\n", path);
        scene_free(&scene);
        return 1;
    }

    printf("--- saved %s ---\n", path);
    dump_file(path);
    printf("--- end ---\n");

    /* spot-check that the child's parent reference is the anchor's nid */
    {
        SceneObject *box    = scene_get(&scene, box_h);
        SceneObject *anchor = scene_get(&scene, anchor_h);
        if (!box || !anchor || !box->nid || !anchor->nid) {
            printf("FAIL: missing object or nid\n");
            scene_free(&scene);
            return 1;
        }
        printf("box.nid    = %s\n", box->nid);
        printf("anchor.nid = %s  (box's parent should reference this)\n", anchor->nid);
    }

    /* round-trip: load what we saved, re-save, and the two files must match. If
       load dropped, reordered, or mangled anything, the second file differs. */
    {
        const char *path2 = "scene_io_test_2.stml";
        Scene       b;

        if (!scene_load(&b, path)) {
            printf("FAIL: scene_load(%s)\n", path);
            scene_free(&scene);
            return 1;
        }
        if (!scene_save(&b, path2)) {
            printf("FAIL: re-save to %s\n", path2);
            scene_free(&b); scene_free(&scene);
            return 1;
        }
        if (!files_equal(path, path2)) {
            printf("FAIL: round-trip save->load->save is NOT byte-identical\n");
            scene_free(&b); scene_free(&scene);
            return 1;
        }
        printf("round-trip save->load->save is byte-identical: ok\n");

        /* identity + hierarchy survived: B's box (found by A's nid) must have a
           parent whose nid is A's anchor nid */
        {
            sol_u32      bbox_h = scene_handle_for_nid(&b, a_box_nid);
            SceneObject *bbox   = scene_get(&b, bbox_h);
            SceneObject *bpar   = bbox ? scene_get(&b, bbox->parent) : (SceneObject *)0;
            if (!bbox || !bpar || !bpar->nid || strcmp(bpar->nid, a_anchor_nid) != 0) {
                printf("FAIL: identity/hierarchy not preserved across load\n");
                scene_free(&b); scene_free(&scene);
                return 1;
            }
            printf("loaded box keeps its nid and its parent resolves to the anchor: ok\n");
        }
        scene_free(&b);
    }

    /* keystone: deleting an unrelated object reshuffles array positions, but the
       survivors keep their identity and their references still resolve. */
    printf("--- keystone: delete reshuffles position, identity persists ---\n");
    {
        SceneObject *box0   = scene_get(&scene, box_h);
        long         before = box0 - scene.objects;     /* box's current array index */
        SceneObject *box, *anchor;
        long         after;

        scene_remove(&scene, floor_h);                  /* delete an unreferenced root */

        if (scene.count != 2 || scene_get(&scene, floor_h) != (SceneObject *)0) {
            printf("FAIL: floor not cleanly removed (count=%u)\n", (unsigned)scene.count);
            scene_free(&scene);
            return 1;
        }

        box    = scene_get(&scene, box_h);              /* still found by original handle */
        anchor = scene_get(&scene, anchor_h);
        if (!box || !anchor) {
            printf("FAIL: a survivor lost its handle\n");
            scene_free(&scene);
            return 1;
        }
        after = box - scene.objects;

        if (strcmp(box->nid, a_box_nid) != 0 || strcmp(anchor->nid, a_anchor_nid) != 0) {
            printf("FAIL: a survivor's nid changed\n");
            scene_free(&scene);
            return 1;
        }
        if (scene_get(&scene, box->parent) != anchor) {
            printf("FAIL: box's parent reference broke\n");
            scene_free(&scene);
            return 1;
        }
        printf("deleted floor: box moved index %ld -> %ld, kept nid %s\n",
               before, after, box->nid);
        printf("box's parent still resolves to the anchor: ok\n");

        /* identity survives persistence after the delete, too */
        {
            const char  *path3 = "scene_io_test_3.stml";
            Scene        d;
            sol_u32      dbox_h;
            SceneObject *dbox, *dpar;

            if (!scene_save(&scene, path3) || !scene_load(&d, path3)) {
                printf("FAIL: save/reload after delete\n");
                scene_free(&scene);
                return 1;
            }
            dbox_h = scene_handle_for_nid(&d, a_box_nid);
            dbox   = scene_get(&d, dbox_h);
            dpar   = dbox ? scene_get(&d, dbox->parent) : (SceneObject *)0;
            if (d.count != 2 ||
                scene_handle_for_nid(&d, a_floor_nid) != 0 ||
                !dbox || !dpar || strcmp(dpar->nid, a_anchor_nid) != 0) {
                printf("FAIL: survivors/links not preserved across delete+reload\n");
                scene_free(&d);
                scene_free(&scene);
                return 1;
            }
            printf("after delete, save+reload keeps survivors and drops the floor: ok\n");
            scene_free(&d);
        }
    }

    scene_free(&scene);
    printf("scene_io_test: OK\n");
    return 0;
}
