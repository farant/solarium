/* scene_io_test.c — headless exercise for scene_save. Builds a small scene of
   empties (zero Mesh, so no GL needed) carrying nids, mesh refs, and the
   overbuilt slots, saves it to STML, then reads the file back and prints it.
   Built by `build.sh iotest` under ASan/UBSan.

   We construct vec3/quat by field rather than via sol_math.h: that header is
   all-static, and pulling it into a TU that uses only a couple of helpers would
   trip -Wunused-function. scene.h (via sol_types.h) gives us the plain types. */

#include "scene.h"
#include "mirror.h"  /* room_mirror_scan (item 6) — headless against a temp dir */
#include "nid.h"     /* NID_LEN — for stack buffers holding a copied nid */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>   /* mkdir — this test file is C11/POSIX, not c89check'd */

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
    empty.bounds.min = v3(0.0f, 0.0f, 0.0f);
    empty.bounds.max = v3(0.0f, 0.0f, 0.0f);

    scene_init(&scene);

    floor_h  = scene_add(&scene, 0,        empty, v3(0.0f, 0.0f, 0.0f), q_identity(), v3(1.0f, 1.0f, 1.0f));
    anchor_h = scene_add(&scene, 0,        empty, v3(0.0f, 0.0f, 0.0f), q_identity(), v3(1.0f, 1.0f, 1.0f));
    box_h    = scene_add(&scene, anchor_h, empty, v3(1.5f, 1.0f, 0.0f), q_identity(), v3(1.0f, 1.0f, 1.0f));

    scene_mesh_ref_set(&scene, floor_h, "grid");
    scene_mesh_ref_set(&scene, box_h,   "box");

    /* parametric ref (item 5): the anchor doubles as a room shell */
    {
        float room_p[3];
        room_p[0] = 7.0f; room_p[1] = 5.0f; room_p[2] = 3.0f;
        scene_mesh_ref_set(&scene, anchor_h, "room");
        scene_mesh_params_set(&scene, anchor_h, room_p, 3);
    }

    /* kind (item 6): the box plays a FILE card for the round-trip */
    scene_kind_set(&scene, box_h, KIND_FILE);

    scene_meta_set(&scene, box_h, "title",  "Test Box");
    scene_meta_set(&scene, box_h, "author", "Solarium");
    scene_rel_add(&scene, box_h, "orbits", anchor_h);
    scene_content_set(&scene, box_h, "notes/box.txt");

    /* the TODO3 item-1 acceptance set: values with quotes, '<', '&', edge
       whitespace, and newlines must all survive save->load->save untouched */
    scene_meta_set(&scene, box_h, "quotes",    "she said \"hi\" and 'hey'");
    scene_meta_set(&scene, box_h, "angles",    "if a < b && b > c");
    scene_meta_set(&scene, box_h, "entity",    "see &some name here; later");
    scene_meta_set(&scene, box_h, "spaced",    "  edges matter  ");
    scene_meta_set(&scene, box_h, "multiline", "line one\n  indented two\nline three");
    scene_meta_set(&scene, box_h, "say\"what", "a key needing quote-selection");

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

        /* and the loaded VALUES are the originals, byte for byte — file
           identity alone could mask a writer+reader that agree on mangling */
        {
            static const char *want[][2] = {
                { "quotes",    "she said \"hi\" and 'hey'"          },
                { "angles",    "if a < b && b > c"                  },
                { "entity",    "see &some name here; later"         },
                { "spaced",    "  edges matter  "                   },
                { "multiline", "line one\n  indented two\nline three" },
                { "say\"what", "a key needing quote-selection"      },
            };
            sol_u32     bbox_h = scene_handle_for_nid(&b, a_box_nid);
            size_t      k;
            for (k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
                const char *got = scene_meta_get(&b, bbox_h, want[k][0]);
                if (!got || strcmp(got, want[k][1]) != 0) {
                    printf("FAIL: meta [%s] loaded as [%s]\n",
                           want[k][0], got ? got : "(missing)");
                    scene_free(&b); scene_free(&scene);
                    return 1;
                }
            }
            printf("special-character meta values load back exactly: ok\n");
        }

        /* parametric mesh-ref (item 5): the room's (w,d,h) survive the trip */
        {
            sol_u32      ba_h = scene_handle_for_nid(&b, a_anchor_nid);
            SceneObject *ba   = scene_get(&b, ba_h);
            if (!ba || !ba->mesh_ref || strcmp(ba->mesh_ref, "room") != 0 ||
                ba->mesh_param_count != 3 ||
                ba->mesh_params[0] != 7.0f || ba->mesh_params[1] != 5.0f ||
                ba->mesh_params[2] != 3.0f) {
                printf("FAIL: parametric room ref did not round-trip\n");
                scene_free(&b); scene_free(&scene);
                return 1;
            }
            printf("parametric mesh-ref (room w/d/h) round-trips: ok\n");
        }

        /* kind (item 6): semantic type survives the trip; plain stays absent */
        {
            sol_u32      bb_h = scene_handle_for_nid(&b, a_box_nid);
            sol_u32      bf_h = scene_handle_for_nid(&b, a_floor_nid);
            SceneObject *bb   = scene_get(&b, bb_h);
            SceneObject *bf   = scene_get(&b, bf_h);
            if (!bb || bb->kind != KIND_FILE || !bf || bf->kind != KIND_PLAIN) {
                printf("FAIL: object kind did not round-trip\n");
                scene_free(&b); scene_free(&scene);
                return 1;
            }
            printf("object kind round-trips (file kept, plain absent): ok\n");
        }

        /* the emitters themselves are headless now (mesh.c is pure CPU since
           the item-5 split): sanity-check the new architectural pieces */
        {
            MeshBuilder mb;
            float       wall_p[6];
            mb_init(&mb);
            if (!mesh_ref_build("room", (const float *)0, 0, &mb) ||
                mb.vertex_count != 24 || mb.index_count != 36) {
                printf("FAIL: room shell should be 6 quads (24v/36i), got %uv/%ui\n",
                       (unsigned)mb.vertex_count, (unsigned)mb.index_count);
                mb_free(&mb); scene_free(&b); scene_free(&scene);
                return 1;
            }
            mb_free(&mb);
            /* a doorway flush with the floor: left panel (6 exposed faces) +
               right panel (6) + header (4) + the threshold (1: the opening's
               floor across the wall thickness) = 17 quads = 68 verts; a
               full-height opening at the left edge degenerates to the right
               panel + threshold = 7 quads = 28 verts */
            mb_init(&mb);
            wall_p[0] = 4.0f; wall_p[1] = 3.0f;
            wall_p[2] = 1.5f; wall_p[3] = 1.0f; wall_p[4] = 2.2f;
            wall_p[5] = 0.15f;
            if (!mesh_ref_build("wall", wall_p, 6, &mb) || mb.vertex_count != 68) {
                printf("FAIL: doorway wall should be 17 exposed faces (68v), got %u\n",
                       (unsigned)mb.vertex_count);
                mb_free(&mb); scene_free(&b); scene_free(&scene);
                return 1;
            }
            mb_free(&mb);
            mb_init(&mb);
            wall_p[2] = 0.0f; wall_p[4] = 3.0f;   /* full-height opening at the left edge */
            mesh_ref_build("wall", wall_p, 6, &mb);
            if (mb.vertex_count != 28) {
                printf("FAIL: degenerate pieces must be skipped (want 28v, got %u)\n",
                       (unsigned)mb.vertex_count);
                mb_free(&mb); scene_free(&b); scene_free(&scene);
                return 1;
            }
            mb_free(&mb);
            /* item 7: presence flags — a bare platform is just the floor */
            mb_init(&mb);
            {
                float folly_p[8];
                folly_p[0] = 5.0f; folly_p[1] = 5.0f; folly_p[2] = 3.0f;
                folly_p[3] = 0.0f; folly_p[4] = 0.0f; folly_p[5] = 0.0f;
                folly_p[6] = 0.0f; folly_p[7] = 0.0f;
                mesh_ref_build("room", folly_p, 8, &mb);
            }
            if (mb.vertex_count != 4 || mb.index_count != 6) {
                printf("FAIL: floor-only room should be 1 quad (4v/6i)\n");
                mb_free(&mb); scene_free(&b); scene_free(&scene);
                return 1;
            }
            mb_free(&mb);
            mb_init(&mb);
            if (!mesh_ref_build("path", (const float *)0, 0, &mb) || mb.vertex_count != 24) {
                printf("FAIL: path slab should be 6 faces (24v)\n");
                mb_free(&mb); scene_free(&b); scene_free(&scene);
                return 1;
            }
            mb_free(&mb);
            printf("room + wall + path emitters (flags, degenerate skip): ok\n");
        }

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

    /* mirror scan (item 6): reflect a real directory — every entry becomes a
       FILE card with the path as identity; a rescan adds NOTHING (idempotent:
       membership follows disk, and disk didn't change) */
    {
        Scene   m;
        sol_u32 room, card;
        int     n1, n2;
        FILE   *f1;

        mkdir("mirror_test_dir", 0755);                    /* EEXIST is fine */
        f1 = fopen("mirror_test_dir/alpha.txt", "w");
        if (f1) { fputs("a\n", f1); fclose(f1); }
        f1 = fopen("mirror_test_dir/beta.txt", "w");
        if (f1) { fputs("b\n", f1); fclose(f1); }

        scene_init(&m);
        room = scene_add(&m, 0, empty, v3(0.0f, 0.0f, 0.0f), q_identity(), v3(1.0f, 1.0f, 1.0f));
        n1 = room_mirror_scan(&m, room, "mirror_test_dir");
        n2 = room_mirror_scan(&m, room, "mirror_test_dir");
        if (n1 != 2 || n2 != 0) {
            printf("FAIL: mirror scan added %d then %d (want 2 then 0)\n", n1, n2);
            scene_free(&m);
            return 1;
        }
        card = 0;
        {
            sol_u32 i;
            for (i = 0; i < m.count; i++) {
                if (m.objects[i].content &&
                    strcmp(m.objects[i].content, "mirror_test_dir/alpha.txt") == 0) {
                    card = m.objects[i].handle;
                }
            }
        }
        {
            SceneObject *co = scene_get(&m, card);
            const char  *u  = scene_meta_get(&m, card, "unplaced");
            if (!co || co->kind != KIND_FILE || co->parent != room ||
                !u || strcmp(u, "1") != 0 ||
                !co->mesh_ref || strcmp(co->mesh_ref, "card") != 0) {
                printf("FAIL: scanned card is not a tray'd FILE card\n");
                scene_free(&m);
                return 1;
            }
        }
        printf("mirror scan (2 cards, idempotent rescan, tray'd FILE kind): ok\n");

        /* reconciliation (6c): delete alpha on disk -> its card TOMBSTONES,
           keeping its place and the user's note; recreate it -> RESURRECTED
           in place. beta is untouched throughout. */
        {
            SceneObject *co;
            vec3         kept_pos;
            int          n;

            scene_meta_set(&m, card, "note", "the user's precious thought");
            co = scene_get(&m, card);
            kept_pos = co->pos;

            remove("mirror_test_dir/alpha.txt");
            n  = room_mirror_scan(&m, room, "mirror_test_dir");
            co = scene_get(&m, card);
            if (n != 1 || !co || co->kind != KIND_TOMBSTONE ||
                !scene_meta_get(&m, card, "note") ||
                strcmp(scene_meta_get(&m, card, "note"),
                       "the user's precious thought") != 0 ||
                co->pos.x != kept_pos.x || co->pos.z != kept_pos.z) {
                printf("FAIL: vanished file should tombstone in place, note intact\n");
                scene_free(&m);
                return 1;
            }

            f1 = fopen("mirror_test_dir/alpha.txt", "w");
            if (f1) { fputs("a\n", f1); fclose(f1); }
            n  = room_mirror_scan(&m, room, "mirror_test_dir");
            co = scene_get(&m, card);
            if (n != 1 || !co || co->kind != KIND_FILE ||
                co->pos.x != kept_pos.x || co->pos.z != kept_pos.z) {
                printf("FAIL: returned file should resurrect in place\n");
                scene_free(&m);
                return 1;
            }
            printf("tombstone + surviving note + resurrection in place: ok\n");
        }

        /* workspaces (6d): one file aliased into TWO workspaces — both hold
           a live reference to the single real path (no 2D equivalent);
           re-aliasing dedups; a vanished target FLAGS stale, never removes */
        {
            sol_u32 ws1, ws2, a1, a2, a3;
            ws1 = scene_add(&m, 0, empty, v3(10.0f, 0.0f, 0.0f), q_identity(), v3(1.0f, 1.0f, 1.0f));
            ws2 = scene_add(&m, 0, empty, v3(20.0f, 0.0f, 0.0f), q_identity(), v3(1.0f, 1.0f, 1.0f));
            a1 = workspace_add_alias(&m, ws1, "mirror_test_dir/beta.txt", "beta.txt");
            a2 = workspace_add_alias(&m, ws2, "mirror_test_dir/beta.txt", "beta.txt");
            a3 = workspace_add_alias(&m, ws1, "mirror_test_dir/beta.txt", "beta.txt");
            if (!a1 || !a2 || a1 == a2 || a3 != a1 ||
                scene_get(&m, a1)->kind != KIND_ALIAS ||
                strcmp(scene_get(&m, a2)->content, "mirror_test_dir/beta.txt") != 0) {
                printf("FAIL: alias into two workspaces / dedup\n");
                scene_free(&m);
                return 1;
            }
            remove("mirror_test_dir/beta.txt");
            if (workspace_validate_aliases(&m) != 2 ||
                !scene_meta_get(&m, a1, "stale") ||
                strcmp(scene_meta_get(&m, a1, "stale"), "1") != 0 ||
                scene_get(&m, a2)->kind != KIND_ALIAS) {
                printf("FAIL: vanished target should flag BOTH aliases stale\n");
                scene_free(&m);
                return 1;
            }
            f1 = fopen("mirror_test_dir/beta.txt", "w");
            if (f1) { fputs("b\n", f1); fclose(f1); }
            if (workspace_validate_aliases(&m) != 2 ||
                strcmp(scene_meta_get(&m, a1, "stale"), "1") == 0) {
                printf("FAIL: returned target should clear the stale flags\n");
                scene_free(&m);
                return 1;
            }
            printf("alias x2 workspaces, dedup, stale flag + clear: ok\n");
        }
        scene_free(&m);
    }

    /* the raw blind spot: a multiline value containing the literal terminator
       "</" is unrepresentable — save must refuse loudly and leave no file */
    {
        const char *path4 = "scene_io_test_4.stml";
        FILE       *probe;
        scene_meta_set(&scene, box_h, "poison", "line one</meta>\nline two");
        if (scene_save(&scene, path4)) {
            printf("FAIL: save accepted an unrepresentable raw value\n");
            scene_free(&scene);
            return 1;
        }
        probe = fopen(path4, "rb");
        if (probe) {
            printf("FAIL: refused save left a truncated file behind\n");
            fclose(probe);
            scene_free(&scene);
            return 1;
        }
        printf("unrepresentable value (multiline with \"</\") refused cleanly: ok\n");
    }

    scene_free(&scene);
    printf("scene_io_test: OK\n");
    return 0;
}
