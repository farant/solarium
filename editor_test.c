#include "editor.h"
#include "scene.h"
#include "sol_math.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* a fs-tree room: parent empty (room_type) + "room" shell child w x d. */
static sol_u32 add_room(Scene *s, float x, float y, float z, float w, float d) {
    Mesh    empty;
    sol_u32 parent, shell;
    float   p[8];
    memset(&empty, 0, sizeof empty);
    parent = scene_add(s, 0, empty, vec3_make(x, y, z), quat_identity(),
                       vec3_make(1.0f, 1.0f, 1.0f));
    scene_meta_set(s, parent, "room_type", "mirror");
    shell = scene_add(s, parent, empty, vec3_make(0.0f, 0.0f, 0.0f),
                      quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
    scene_mesh_ref_set(s, shell, "room");
    p[0] = w; p[1] = d; p[2] = 3.0f; p[3] = 1.0f; p[4] = 1.0f; p[5] = 1.0f;
    p[6] = 1.0f; p[7] = 0.0f;
    scene_mesh_params_set(s, shell, p, 8);
    return parent;
}

int main(void) {
    /* room_rect reads center+floor from the anchor and w/d from the shell */
    {
        Scene s; sol_u32 a; RoomRect r;
        scene_init(&s);
        a = add_room(&s, 2.0f, 12.0f, -3.0f, 8.0f, 6.0f);
        r = editor_room_rect(&s, a);
        CHECK(fabs((double)(r.cx - 2.0f)) < 1e-4);
        CHECK(fabs((double)(r.cz + 3.0f)) < 1e-4);
        CHECK(fabs((double)(r.floor_y - 12.0f)) < 1e-4);
        CHECK(fabs((double)(r.hw - 4.0f)) < 1e-4);
        CHECK(fabs((double)(r.hd - 3.0f)) < 1e-4);
        scene_free(&s);
    }

    /* classify: body, each edge, a corner, and outside */
    {
        RoomRect r; r.cx = 0.0f; r.cz = 0.0f; r.hw = 5.0f; r.hd = 5.0f; r.floor_y = 0.0f;
        CHECK(editor_classify(r,  0.0f,  0.0f, 0.6f) == EDIT_ZONE_BODY);
        CHECK(editor_classify(r,  5.0f,  0.0f, 0.6f) == EDIT_ZONE_EDGE_XP);
        CHECK(editor_classify(r, -5.0f,  0.0f, 0.6f) == EDIT_ZONE_EDGE_XN);
        CHECK(editor_classify(r,  0.0f,  5.0f, 0.6f) == EDIT_ZONE_EDGE_ZP);
        CHECK(editor_classify(r,  0.0f, -5.0f, 0.6f) == EDIT_ZONE_EDGE_ZN);
        CHECK(editor_classify(r,  5.0f,  5.0f, 0.6f) == EDIT_ZONE_CORNER_XPZP);
        CHECK(editor_classify(r,  9.0f,  0.0f, 0.6f) == EDIT_ZONE_NONE);
        /* the grab band STRADDLES the wall: a point just OUTSIDE the footprint
           but within band still reads as that edge (the band is the whole point) */
        CHECK(editor_classify(r,  5.4f,  0.0f, 0.6f) == EDIT_ZONE_EDGE_XP);
        CHECK(editor_classify(r,  0.0f, -5.4f, 0.6f) == EDIT_ZONE_EDGE_ZN);
        /* the other three corners */
        CHECK(editor_classify(r,  5.0f, -5.0f, 0.6f) == EDIT_ZONE_CORNER_XPZN);
        CHECK(editor_classify(r, -5.0f,  5.0f, 0.6f) == EDIT_ZONE_CORNER_XNZP);
        CHECK(editor_classify(r, -5.0f, -5.0f, 0.6f) == EDIT_ZONE_CORNER_XNZN);
    }

    /* resize +X face: the -X wall stays at -5, width grows, center shifts to +2 */
    {
        float nc = 0.0f, nh = 0.0f;
        editor_resize_axis(0.0f, 5.0f, +1, 9.0f, 3.0f, &nc, &nh);
        CHECK(fabs((double)(nc - 2.0f)) < 1e-4);   /* center */
        CHECK(fabs((double)(nh - 7.0f)) < 1e-4);   /* half */
        CHECK(fabs((double)((nc - nh) + 5.0f)) < 1e-4);  /* -X wall fixed at -5 */
    }
    /* resize -X face: the +X wall stays at +5 */
    {
        float nc = 0.0f, nh = 0.0f;
        editor_resize_axis(0.0f, 5.0f, -1, -9.0f, 3.0f, &nc, &nh);
        CHECK(fabs((double)((nc + nh) - 5.0f)) < 1e-4);  /* +X wall fixed at +5 */
    }
    /* resize min-size clamp: dragging the +X face past the opposite wall */
    {
        float nc = 0.0f, nh = 0.0f;
        editor_resize_axis(0.0f, 5.0f, +1, -10.0f, 3.0f, &nc, &nh);
        CHECK(fabs((double)(2.0f * nh - 3.0f)) < 1e-4);  /* width clamped to min */
    }

    /* can_connect: self/duplicate/non-room guards */
    {
        Scene s; sol_u32 a, b, c; Mesh empty;
        scene_init(&s);
        a = add_room(&s, 0.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        b = add_room(&s, 20.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        memset(&empty, 0, sizeof empty);
        c = scene_add(&s, 0, empty, vec3_make(0,0,0), quat_identity(), vec3_make(1,1,1));  /* not a room */
        CHECK(editor_can_connect(&s, a, b) == SOL_TRUE);
        CHECK(editor_can_connect(&s, a, a) == SOL_FALSE);
        CHECK(editor_can_connect(&s, a, c) == SOL_FALSE);
        /* once joined, the pair is refused */
        {
            sol_u32 wk = scene_add(&s, 0, empty, vec3_make(0,0,0), quat_identity(), vec3_make(1,1,1));
            scene_mesh_ref_set(&s, wk, "walkway");
            scene_rel_add(&s, wk, "connects", a);
            scene_rel_add(&s, wk, "connects", b);
            CHECK(editor_can_connect(&s, a, b) == SOL_FALSE);
            CHECK(editor_can_connect(&s, b, a) == SOL_FALSE);
        }
        scene_free(&s);
    }

    /* connect adds a walkway joining the pair; disconnect removes it */
    {
        Scene s; sol_u32 a, b, wk;
        scene_init(&s);
        a = add_room(&s, 0.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        b = add_room(&s, 20.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        wk = editor_connect(&s, a, b);
        CHECK(wk != 0);
        CHECK(editor_can_connect(&s, a, b) == SOL_FALSE);   /* now joined */
        CHECK(editor_connect(&s, a, b) == 0);               /* duplicate refused */
        editor_disconnect(&s, wk);
        CHECK(editor_can_connect(&s, a, b) == SOL_TRUE);    /* free again */
        scene_free(&s);
    }

    /* apply_move writes the anchor's world XZ */
    {
        Scene s; sol_u32 a; SceneObject *o;
        scene_init(&s);
        a = add_room(&s, 0.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        editor_apply_move(&s, a, 7.0f, -4.0f);
        o = scene_get(&s, a);
        CHECK(o != 0);
        CHECK(fabs((double)(o->pos.x - 7.0f)) < 1e-4);
        CHECK(fabs((double)(o->pos.z + 4.0f)) < 1e-4);
        CHECK(fabs((double)(o->pos.y - 12.0f)) < 1e-4);   /* height untouched */
        scene_free(&s);
    }

    /* apply_resize dragging the +X wall to x=9: width 14, center +2, -X wall
       stays at -5 in WORLD space */
    {
        Scene s; sol_u32 a; RoomRect r; SceneObject *ro;
        scene_init(&s);
        a = add_room(&s, 0.0f, 12.0f, 0.0f, 10.0f, 10.0f);
        editor_apply_resize(&s, a, EDIT_ZONE_EDGE_XP, 9.0f, 0.0f);
        r  = editor_room_rect(&s, a);
        ro = scene_get(&s, a);
        CHECK(fabs((double)(2.0f * r.hw - 14.0f)) < 1e-3);     /* width grew */
        CHECK(fabs((double)(ro->pos.x - 2.0f)) < 1e-3);        /* center shifted */
        CHECK(fabs((double)((r.cx - r.hw) + 5.0f)) < 1e-3);    /* -X wall fixed */
        CHECK(fabs((double)(2.0f * r.hd - 10.0f)) < 1e-3);     /* depth untouched */
        scene_free(&s);
    }

    /* apply_resize corner CORNER_XPZN: +X and -Z walls move to the cursor; the
       -X and +Z walls stay fixed; h + wall flags preserved */
    {
        Scene s; sol_u32 a, i; RoomRect r;
        scene_init(&s);
        a = add_room(&s, 0.0f, 12.0f, 0.0f, 10.0f, 10.0f);
        editor_apply_resize(&s, a, EDIT_ZONE_CORNER_XPZN, 9.0f, -8.0f);
        r = editor_room_rect(&s, a);
        CHECK(fabs((double)(2.0f * r.hw - 14.0f)) < 1e-3);   /* X grew to 14 */
        CHECK(fabs((double)(2.0f * r.hd - 13.0f)) < 1e-3);   /* Z grew to 13 */
        CHECK(fabs((double)((r.cx - r.hw) + 5.0f)) < 1e-3);  /* -X wall fixed at -5 */
        CHECK(fabs((double)((r.cz + r.hd) - 5.0f)) < 1e-3);  /* +Z wall fixed at +5 */
        for (i = 0; i < s.count; i++) {
            SceneObject *o = &s.objects[i];
            if (o->parent == a && o->mesh_ref && strcmp(o->mesh_ref, "room") == 0) {
                CHECK(fabs((double)(o->mesh_params[2] - 3.0f)) < 1e-4);  /* h preserved */
                CHECK(fabs((double)(o->mesh_params[6] - 1.0f)) < 1e-4);  /* ww flag preserved */
                CHECK(fabs((double)(o->mesh_params[7] - 0.0f)) < 1e-4);  /* ceil preserved */
                break;
            }
        }
        scene_free(&s);
    }

    /* the editor refuses to connect a room in a non-active workspace */
    {
        Scene s; sol_u32 a, b;
        scene_init(&s);
        a = add_room(&s, 0.0f,  12.0f, 0.0f, 8.0f, 8.0f);
        b = add_room(&s, 14.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        CHECK(editor_can_connect(&s, a, b));         /* both visible: connectable */
        scene_meta_set(&s, b, "workspace", "hidden");
        strcpy(s.active_ws, "home");
        CHECK(!editor_can_connect(&s, a, b));        /* b hidden: refused */
        scene_free(&s);
    }

    if (fails == 0) printf("editor_test: OK\n");
    return fails ? 1 : 0;
}
