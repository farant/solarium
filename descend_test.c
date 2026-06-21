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

    if (fails == 0) printf("descend_test: OK\n");
    return fails ? 1 : 0;
}
