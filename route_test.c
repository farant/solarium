#include "route.h"
#include "scene.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* add a fs-tree room (parent empty + "room" shell child) at pos, size w x d. */
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

static sol_u32 add_walkway(Scene *s, sol_u32 a, sol_u32 b) {
    Mesh    empty;
    sol_u32 w;
    memset(&empty, 0, sizeof empty);
    w = scene_add(s, 0, empty, vec3_make(0.0f, 0.0f, 0.0f), quat_identity(),
                  vec3_make(1.0f, 1.0f, 1.0f));
    scene_mesh_ref_set(s, w, "walkway");
    scene_rel_add(s, w, "connects", a);
    scene_rel_add(s, w, "connects", b);
    return w;
}

int main(void) {
    /* due-east: straight, A opens E, B opens W, 0 bends */
    {
        Scene s; Route r; sol_u32 home, east, wk;
        scene_init(&s);
        home = add_room(&s, 0.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        east = add_room(&s, 20.0f, 12.0f, 0.0f, 10.0f, 10.0f);
        wk   = add_walkway(&s, home, east);
        CHECK(route_for_walkway(&s, wk, &r));
        CHECK(r.valid);
        CHECK(r.straight);
        CHECK((r.wall_lo == ROOM_WALL_E && r.wall_hi == ROOM_WALL_W) ||
              (r.wall_lo == ROOM_WALL_W && r.wall_hi == ROOM_WALL_E));
        scene_free(&s);
    }
    /* diagonal up-right (dx>dz): A opens E, B opens N, single bend */
    {
        Scene s; Route r; sol_u32 a, b, wk;
        scene_init(&s);
        a  = add_room(&s, 0.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        b  = add_room(&s, 24.0f, 12.0f, 10.0f, 10.0f, 10.0f);
        wk = add_walkway(&s, a, b);
        CHECK(route_for_walkway(&s, wk, &r));
        CHECK(r.valid);
        CHECK(!r.straight);
        CHECK(fabs((double)(r.corner.z - r.door_lo.z)) < 1e-3);
        CHECK(fabs((double)(r.corner.x - r.door_hi.x)) < 1e-3);
        scene_free(&s);
    }
    /* two roots both east of home => two doors spread on home's E wall */
    {
        Scene s; RoomOpening op[16]; sol_u32 home, e1, e2; int n, i, eN = 0;
        scene_init(&s);
        home = add_room(&s, 0.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        e1   = add_room(&s, 20.0f, 12.0f, 6.0f, 10.0f, 10.0f);
        e2   = add_room(&s, 20.0f, 12.0f, -6.0f, 10.0f, 10.0f);
        add_walkway(&s, home, e1);
        add_walkway(&s, home, e2);
        n = route_room_openings(&s, home, op, 16);
        for (i = 0; i < n; i++) if (op[i].wall == ROOM_WALL_E) eN++;
        CHECK(eN == 2);
        if (eN == 2) {
            float c0 = 1e30f, c1 = -1e30f;
            for (i = 0; i < n; i++) if (op[i].wall == ROOM_WALL_E) {
                if (op[i].center < c0) c0 = op[i].center;
                if (op[i].center > c1) c1 = op[i].center;
            }
            CHECK(c1 - c0 > 0.5f);
        }
        scene_free(&s);
    }
    /* obstacle-aware: a NE root must not route its L corner THROUGH the room
       sitting due-east of home (it should exit on the other axis instead) */
    {
        Scene s; Route r; sol_u32 home, east, ne, wk_ne;
        scene_init(&s);
        home = add_room(&s, 0.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        east = add_room(&s, 16.0f, 12.0f, 0.0f, 10.0f, 10.0f);   /* due east */
        ne   = add_room(&s, 12.0f, 12.0f, 12.0f, 10.0f, 10.0f);  /* up-and-right */
        add_walkway(&s, home, east);
        wk_ne = add_walkway(&s, home, ne);
        CHECK(route_for_walkway(&s, wk_ne, &r));
        CHECK(r.valid);
        /* the corner must NOT land inside the east room (center 16,0, half 5) */
        CHECK(!(r.corner.x > 11.0f && r.corner.x < 21.0f &&
                r.corner.z > -5.0f && r.corner.z < 5.0f));
        scene_free(&s);
    }
    if (fails == 0) printf("route_test: OK\n");
    return fails ? 1 : 0;
}
