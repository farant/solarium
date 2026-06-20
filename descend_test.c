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

    (void)add_folder_card;   /* used in Task 2's tests */
    if (fails == 0) printf("descend_test: OK\n");
    return fails ? 1 : 0;
}
