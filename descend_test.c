#include "descend.h"
#include "scene.h"
#include "mesh.h"       /* ROOM_WALL_* */
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

/* a folder card parented to `room`, content = path. */
static sol_u32 add_folder_card(Scene *s, sol_u32 room, const char *path) {
    Mesh    empty;
    sol_u32 c;
    memset(&empty, 0, sizeof empty);
    c = scene_add(s, room, empty, vec3_make(0.0f, 0.0f, 0.0f), quat_identity(),
                  vec3_make(1.0f, 1.0f, 1.0f));
    scene_kind_set(s, c, KIND_FOLDER);
    scene_content_set(s, c, path);
    scene_mesh_ref_set(s, c, "card");
    return c;
}

int main(void) {
    /* wall-aim: a horizontal ray from room center toward +X hits the E wall at
       offset 0; aiming straight up hits no wall */
    {
        RoomRect r;
        Ray      ray;
        int      wall;
        float    off;
        r.cx = 0.0f; r.cz = 0.0f; r.hw = 5.0f; r.hd = 5.0f; r.floor_y = 0.0f;
        ray.origin = vec3_make(0.0f, 1.0f, 0.0f);
        ray.dir    = vec3_make(1.0f, 0.0f, 0.0f);
        CHECK(descend_wall_aim(r, ray, 2.1f, &wall, &off) == 1);
        CHECK(wall == ROOM_WALL_E);
        CHECK(fabs((double)off) < 1e-3);
        ray.dir = vec3_make(0.0f, 1.0f, 0.0f);       /* up the wall plane: parallel, no hit */
        CHECK(descend_wall_aim(r, ray, 2.1f, &wall, &off) == 0);
    }

    /* wall-aim corner clamp: a ray that hits the N wall past the corner margin
       (lim = hw - ROUTE_DOOR_W/2 - 0.4 = 5 - 0.7 - 0.4 = 3.9) clamps the offset */
    {
        RoomRect r;
        Ray      ray;
        int      wall;
        float    off;
        r.cx = 0.0f; r.cz = 0.0f; r.hw = 5.0f; r.hd = 5.0f; r.floor_y = 0.0f;
        ray.origin = vec3_make(0.0f, 1.0f, 0.0f);
        ray.dir    = vec3_make(0.9f, 0.0f, -1.0f);   /* hits N (z=-5) at x=4.5, nearer than E */
        CHECK(descend_wall_aim(r, ray, 2.1f, &wall, &off) == 1);
        CHECK(wall == ROOM_WALL_N);
        CHECK(fabs((double)(off - 3.9f)) < 1e-3);    /* clamped off the corner */
    }

    /* door-point on the E wall at offset +2 is at (cx+hw, floor, cz+2) */
    {
        RoomRect r;
        vec3     p;
        r.cx = 1.0f; r.cz = 1.0f; r.hw = 5.0f; r.hd = 5.0f; r.floor_y = 12.0f;
        p = descend_door_point(r, ROOM_WALL_E, 2.0f);
        CHECK(fabs((double)(p.x - 6.0f)) < 1e-4);    /* cx + hw */
        CHECK(fabs((double)(p.z - 3.0f)) < 1e-4);    /* cz + offset */
        CHECK(fabs((double)(p.y - 12.0f)) < 1e-4);
    }

    /* room-at: a point inside a room's footprint resolves to it; far away = 0 */
    {
        Scene s; sol_u32 a;
        scene_init(&s);
        a = add_room(&s, 0.0f, 12.0f, 0.0f, 10.0f, 10.0f);
        CHECK(descend_room_at(&s, vec3_make(0.0f, 12.5f, 0.0f)) == a);
        CHECK(descend_room_at(&s, vec3_make(50.0f, 12.5f, 0.0f)) == 0);
        CHECK(descend_room_at(&s, vec3_make(0.0f, 40.0f, 0.0f)) == 0);   /* wrong Y */
        scene_free(&s);
    }

    /* plant: opening a folder — a real "mirror" sub-room + a walkway appear,
       the card is marked opened, and a second plant is refused */
    {
        Scene s; sol_u32 home, fld, pv, i, wk = 0;
        scene_init(&s);
        home = add_room(&s, 0.0f, 12.0f, 0.0f, 10.0f, 10.0f);
        fld  = add_folder_card(&s, home, "/tmp/sub");
        pv   = descend_plant(&s, home, fld, ROOM_WALL_E, 0.0f);
        CHECK(pv != 0);
        CHECK(strcmp(scene_meta_get(&s, pv, "room_type"), "mirror") == 0);
        CHECK(strcmp(scene_meta_get(&s, pv, "source_path"), "/tmp/sub") == 0);
        CHECK(strcmp(scene_meta_get(&s, pv, "name"), "sub") == 0);
        CHECK(scene_meta_get(&s, fld, "planted") != NULL);          /* opened: no duplicate */
        for (i = 0; i < s.count; i++) {                            /* find the walkway */
            SceneObject *o = &s.objects[i];
            if (o->mesh_ref && strcmp(o->mesh_ref, "walkway") == 0) { wk = o->handle; break; }
        }
        CHECK(wk != 0);
        {
            SceneObject *wo = scene_get(&s, wk);
            sol_u32 a = 0, b = 0, j;
            for (j = 0; j < wo->rel_count; j++)
                if (strcmp(wo->relations[j].type, "connects") == 0) {
                    if (a == 0) a = wo->relations[j].target; else b = wo->relations[j].target;
                }
            CHECK((a == home && b == pv) || (a == pv && b == home));
        }
        CHECK(descend_plant(&s, home, fld, ROOM_WALL_E, 0.0f) == 0);  /* already planted */
        scene_free(&s);
    }

    /* plant refuses a non-folder card and a folder with no content */
    {
        Scene s; sol_u32 home, plain, nofolder; Mesh empty;
        scene_init(&s);
        home = add_room(&s, 0.0f, 12.0f, 0.0f, 10.0f, 10.0f);
        memset(&empty, 0, sizeof empty);
        plain = scene_add(&s, home, empty, vec3_make(0.0f,0.0f,0.0f),
                          quat_identity(), vec3_make(1.0f,1.0f,1.0f));   /* KIND_PLAIN default */
        CHECK(descend_plant(&s, home, plain, ROOM_WALL_E, 0.0f) == 0);   /* not a folder */
        nofolder = scene_add(&s, home, empty, vec3_make(0.0f,0.0f,0.0f),
                             quat_identity(), vec3_make(1.0f,1.0f,1.0f));
        scene_kind_set(&s, nofolder, KIND_FOLDER);                       /* folder but no content */
        CHECK(descend_plant(&s, home, nofolder, ROOM_WALL_E, 0.0f) == 0);
        scene_free(&s);
    }

    /* a sub-room + walkway descended from a workspace-tagged room inherit that workspace */
    {
        Scene s; sol_u32 home, fld, pv, i, wk = 0;
        scene_init(&s);
        home = add_room(&s, 0.0f, 12.0f, 0.0f, 10.0f, 10.0f);
        scene_meta_set(&s, home, "workspace", "photos");     /* parent is in 'photos' */
        fld  = add_folder_card(&s, home, "/tmp/sub");
        pv   = descend_plant(&s, home, fld, ROOM_WALL_E, 0.0f);
        CHECK(pv != 0);
        CHECK(strcmp(scene_meta_get(&s, pv, "workspace"), "photos") == 0);
        for (i = 0; i < s.count; i++) {
            SceneObject *o = &s.objects[i];
            if (o->mesh_ref && strcmp(o->mesh_ref, "walkway") == 0) { wk = o->handle; break; }
        }
        CHECK(wk != 0);
        CHECK(strcmp(scene_meta_get(&s, wk, "workspace"), "photos") == 0);
        scene_free(&s);
    }

    /* wall-mount: a flat board (1.8 x 1.2 x 0.05) flush on the aimed wall */
    {
        Scene s; RoomRect r; Ray ray; int wall; vec3 c; int ok;
        sol_u32 room;
        float wh = 0.9f, hh = 0.6f, t = 0.05f, ceil_y;
        scene_init(&s);
        room   = add_room(&s, 0.0f, 0.0f, 0.0f, 8.0f, 8.0f);  /* 8x8, floor y=0, h=3 */
        r      = editor_room_rect(&s, room);
        ceil_y = r.floor_y + 3.0f;
        /* centered horizontal aim at the NORTH wall (z = cz - hd = -4) */
        ray.origin = vec3_make(0.0f, 1.5f, 0.0f);
        ray.dir    = vec3_make(0.0f, 0.0f, -1.0f);
        ok = descend_wall_mount(r, ray, ceil_y, wh, hh, t, &wall, &c);
        CHECK(ok);
        CHECK(wall == ROOM_WALL_N);
        CHECK(fabs((double)(c.z - (-4.0f + t * 0.5f))) < 1e-4);  /* flush + pushed out */
        CHECK(fabs((double)c.x) < 1e-4);                          /* centered along wall */
        CHECK(fabs((double)(c.y - 1.5f)) < 1e-4);                 /* at the aim height */
        /* aim HIGH on the north wall: center clamps to ceil_y - hh */
        ray.dir = vec3_make(0.0f, 1.3f, -4.0f);   /* hits (0, 2.8, -4) */
        ok = descend_wall_mount(r, ray, ceil_y, wh, hh, t, &wall, &c);
        CHECK(ok);
        CHECK(fabs((double)(c.y - (ceil_y - hh))) < 1e-4);
        /* aim into the +x CORNER of the north wall: clamps to hw - wh */
        ray.dir = vec3_make(3.5f, 0.0f, -4.0f);   /* hits (3.5, 1.5, -4) */
        ok = descend_wall_mount(r, ray, ceil_y, wh, hh, t, &wall, &c);
        CHECK(ok);
        CHECK(fabs((double)(c.x - (4.0f - wh))) < 1e-4);
        /* a board WIDER than the wall is refused */
        ray.dir = vec3_make(0.0f, 0.0f, -1.0f);
        ok = descend_wall_mount(r, ray, ceil_y, 5.0f, hh, t, &wall, &c);
        CHECK(!ok);
        scene_free(&s);
    }

    /* corner geometry + resize math for a wall-mounted board */
    {
        vec3 cor[4], u = vec3_make(1.0f, 0.0f, 0.0f), p = vec3_make(0.0f, 0.0f, 0.0f);
        float w, h; vec3 o;
        board_corners(p, 2.0f, 1.5f, u, cor);
        CHECK(fabs((double)(cor[0].x + 1.0f)) < 1e-4 && fabs((double)cor[0].y) < 1e-4);        /* BL */
        CHECK(fabs((double)(cor[1].x - 1.0f)) < 1e-4);                                          /* BR */
        CHECK(fabs((double)(cor[2].x - 1.0f)) < 1e-4 && fabs((double)(cor[2].y - 1.5f)) < 1e-4);/* TR */
        CHECK(fabs((double)(cor[3].x + 1.0f)) < 1e-4 && fabs((double)(cor[3].y - 1.5f)) < 1e-4);/* TL */
        /* anchor BL (-1,0,0), drag TR out to (2,2,0): w=3, h=2, origin = bottom-center */
        board_resize_corner(cor[0], vec3_make(2.0f, 2.0f, 0.0f), u, 0.3f, 0.0f, &w, &h, &o);
        CHECK(fabs((double)(w - 3.0f)) < 1e-4);
        CHECK(fabs((double)(h - 2.0f)) < 1e-4);
        CHECK(fabs((double)(o.x - 0.5f)) < 1e-4);   /* mid of -1..2 */
        CHECK(fabs((double)o.y) < 1e-4);            /* bottom (drag was above anchor) */
        /* anchor TR (1,1.5,0), drag down-left to (-2,0,0): origin bottom = lower y */
        board_resize_corner(vec3_make(1.0f, 1.5f, 0.0f), vec3_make(-2.0f, 0.0f, 0.0f),
                            u, 0.3f, 0.0f, &w, &h, &o);
        CHECK(fabs((double)(w - 3.0f)) < 1e-4);
        CHECK(fabs((double)(h - 1.5f)) < 1e-4);
        CHECK(fabs((double)(o.x + 0.5f)) < 1e-4);   /* mid of 1..-2 */
        CHECK(fabs((double)o.y) < 1e-4);            /* bottom = 1.5 - 1.5 */
        /* tiny drag floors at min_size */
        board_resize_corner(vec3_make(0.0f,0.0f,0.0f), vec3_make(0.1f,0.1f,0.0f),
                            u, 0.3f, 0.0f, &w, &h, &o);
        CHECK(fabs((double)(w - 0.3f)) < 1e-4 && fabs((double)(h - 0.3f)) < 1e-4);
        /* aspect 2:1 LOCK — the wider drag drives, h derives so w == 2*h */
        board_resize_corner(vec3_make(0.0f,0.0f,0.0f), vec3_make(2.0f,0.5f,0.0f),
                            u, 0.3f, 2.0f, &w, &h, &o);
        CHECK(fabs((double)(w - 2.0f)) < 1e-4 && fabs((double)(h - 1.0f)) < 1e-4);
    }

    if (fails == 0) printf("descend_test: OK\n");
    return fails ? 1 : 0;
}
