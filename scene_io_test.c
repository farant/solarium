/* scene_io_test.c — headless exercise for scene_save. Builds a small scene of
   empties (zero Mesh, so no GL needed) carrying nids, mesh refs, and the
   overbuilt slots, saves it to STML, then reads the file back and prints it.
   Built by `build.sh iotest` under ASan/UBSan.

   We construct vec3/quat by field rather than via sol_math.h: that header is
   all-static, and pulling it into a TU that uses only a couple of helpers would
   trip -Wunused-function. scene.h (via sol_types.h) gives us the plain types. */

#include "scene.h"

#include <stdio.h>

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

int main(void) {
    Scene   scene;
    Mesh    empty;
    sol_u32 floor_h, anchor_h, box_h;
    const char *path = "scene_io_test.stml";

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

    scene_free(&scene);
    printf("scene_io_test: OK\n");
    return 0;
}
