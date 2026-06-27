#include "route.h"
#include "scene.h"
#include <assert.h>
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

/* add a terrain island (single object: mesh_ref "terrain", own w/d params). */
static sol_u32 add_island(Scene *s, float x, float y, float z, float w, float d) {
    Mesh    empty;
    sol_u32 h;
    float   p[5];
    memset(&empty, 0, sizeof empty);
    h = scene_add(s, 0, empty, vec3_make(x, y, z), quat_identity(), vec3_make(1,1,1));
    scene_mesh_ref_set(s, h, "terrain");
    scene_meta_set(s, h, "room_type", "terrain");
    p[0] = w; p[1] = d; p[2] = 56.0f; p[3] = 2.0f; p[4] = 1.0f;
    scene_mesh_params_set(s, h, p, 5);
    return h;
}

static void test_window_sill_panel(void) {
    MeshBuilder mb;
    RoomOpening door, win;
    sol_u32 door_idx, win_idx;

    door.wall = ROOM_WALL_N; door.center = 0.0f; door.width = 1.4f;
    door.height = 2.1f; door.sill = 0.0f;
    mb_init(&mb);
    make_room_doored(&mb, 6.0f, 4.0f, 3.0f, 0.20f, 1, 1, 1, 1, 0, &door, 1);
    door_idx = mb.index_count;
    mb_free(&mb);

    win = door; win.sill = 0.9f;
    mb_init(&mb);
    make_room_doored(&mb, 6.0f, 4.0f, 3.0f, 0.20f, 1, 1, 1, 1, 0, &win, 1);
    win_idx = mb.index_count;
    mb_free(&mb);

    assert(win_idx > door_idx);   /* the sill box adds faces */
    printf("  window sill panel: door=%u win=%u OK\n",
           (unsigned)door_idx, (unsigned)win_idx);
}

static void test_window_fill_styles(void) {
    MeshBuilder mb; sol_u32 plain_fill, circ_fill, frame_plain, frame_arch;
    mb_init(&mb); make_window_fill(&mb, 1.2f, 1.4f, 0.20f, 0.08f, 0.0f); plain_fill = mb.index_count; mb_free(&mb);
    mb_init(&mb); make_window_fill(&mb, 1.2f, 1.4f, 0.20f, 0.08f, 3.0f); circ_fill  = mb.index_count; mb_free(&mb);
    mb_init(&mb); make_window(&mb, 1.2f, 1.4f, 0.20f, 0.08f, 0.0f); frame_plain = mb.index_count; mb_free(&mb);
    mb_init(&mb); make_window(&mb, 1.2f, 1.4f, 0.20f, 0.08f, 1.0f); frame_arch  = mb.index_count; mb_free(&mb);
    assert(plain_fill == 0);           /* plain/french carry no fill */
    assert(circ_fill > 0);             /* circular fill has geometry */
    assert(frame_arch == frame_plain); /* the casing no longer carries the fill */
    printf("  window fill split: plain_fill=%u circ_fill=%u frame=%u OK\n",
           (unsigned)plain_fill, (unsigned)circ_fill, (unsigned)frame_plain);
}

static void test_window_glass_styles(void) {
    MeshBuilder mb; sol_u32 rect, disc, arch, pnt;
    mb_init(&mb); make_window_glass(&mb, 1.2f, 1.4f, 0.0f); rect = mb.index_count; mb_free(&mb);
    mb_init(&mb); make_window_glass(&mb, 1.2f, 1.4f, 3.0f); disc = mb.index_count; mb_free(&mb);
    mb_init(&mb); make_window_glass(&mb, 1.2f, 1.4f, 1.0f); arch = mb.index_count; mb_free(&mb);
    mb_init(&mb); make_window_glass(&mb, 1.2f, 1.4f, 2.0f); pnt  = mb.index_count; mb_free(&mb);
    assert(disc > rect);   /* a disc fan has many tris vs a 2-tri quad */
    assert(arch > rect); assert(pnt > rect);
    printf("  window glass styles: rect=%u disc=%u arch=%u pnt=%u OK\n",
           (unsigned)rect, (unsigned)disc, (unsigned)arch, (unsigned)pnt);
}

static void test_window_above_wall(void) {
    MeshBuilder mb;
    RoomOpening below, above, span;
    sol_u32 none_idx, below_idx, above_idx, span_idx;

    mb_init(&mb); make_room_doored(&mb, 6.0f, 4.0f, 3.0f, 0.20f, 1,1,1,1, 0, (RoomOpening*)0, 0);
    none_idx = mb.index_count; mb_free(&mb);

    below.wall = ROOM_WALL_N; below.center = 0.0f; below.width = 1.2f;
    below.height = 2.3f; below.sill = 0.9f;                 /* fully below h=3.0 */
    mb_init(&mb); make_room_doored(&mb, 6.0f, 4.0f, 3.0f, 0.20f, 1,1,1,1, 0, &below, 1);
    below_idx = mb.index_count; mb_free(&mb);

    above = below; above.sill = 3.4f; above.height = 4.4f;  /* entirely above h -> must be skipped */
    mb_init(&mb); make_room_doored(&mb, 6.0f, 4.0f, 3.0f, 0.20f, 1,1,1,1, 0, &above, 1);
    above_idx = mb.index_count; mb_free(&mb);

    span = below; span.sill = 2.4f; span.height = 4.0f;     /* spans the wall top */
    mb_init(&mb); make_room_doored(&mb, 6.0f, 4.0f, 3.0f, 0.20f, 1,1,1,1, 0, &span, 1);
    span_idx = mb.index_count; mb_free(&mb);

    assert(above_idx == none_idx);   /* above-wall window leaves the wall solid */
    assert(below_idx != none_idx);   /* a real window cuts the wall */
    assert(span_idx  != below_idx);  /* spanning differs (reaches the wall top, no header) */
    assert(span_idx != none_idx);  /* spanning window still cuts the wall */
    printf("  window above wall: none=%u below=%u above=%u span=%u OK\n",
           (unsigned)none_idx, (unsigned)below_idx, (unsigned)above_idx, (unsigned)span_idx);
}

static void test_stacked_windows(void) {
    MeshBuilder mb;
    RoomOpening two[2], merged;
    sol_u32 stacked_idx, merged_idx, solid_idx;

    /* solid wall (no opening) baseline */
    mb_init(&mb); make_room_doored(&mb, 6.0f, 4.0f, 3.0f, 0.20f, 1,1,1,1, 0, (RoomOpening*)0, 0);
    solid_idx = mb.index_count; mb_free(&mb);

    /* two stacked windows, same x, A=[0.5,1.5] B=[2.0,2.8] */
    two[0].wall = ROOM_WALL_N; two[0].center = 0.0f; two[0].width = 1.2f; two[0].sill = 0.5f; two[0].height = 1.5f;
    two[1] = two[0]; two[1].sill = 2.0f; two[1].height = 2.8f;
    mb_init(&mb); make_room_doored(&mb, 6.0f, 4.0f, 3.0f, 0.20f, 1,1,1,1, 0, two, 2);
    stacked_idx = mb.index_count; mb_free(&mb);

    /* one merged opening covering both [0.5,2.8] */
    merged = two[0]; merged.sill = 0.5f; merged.height = 2.8f;
    mb_init(&mb); make_room_doored(&mb, 6.0f, 4.0f, 3.0f, 0.20f, 1,1,1,1, 0, &merged, 1);
    merged_idx = mb.index_count; mb_free(&mb);

    assert(stacked_idx != solid_idx);     /* both holes are cut (not solid) */
    assert(stacked_idx > merged_idx);     /* the wall BAND between the two windows exists */
    printf("  stacked windows: solid=%u merged=%u stacked=%u OK\n",
           (unsigned)solid_idx, (unsigned)merged_idx, (unsigned)stacked_idx);
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
    /* two roots both north of home (different x): home gets two non-overlapping,
       spread doors on its north wall — the door-search slides them apart */
    {
        Scene s; RoomOpening op[16]; sol_u32 home, n1, n2;
        int   n, i, nN = 0;
        float c0 = 1e30f, c1 = -1e30f;
        scene_init(&s);
        home = add_room(&s, 0.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        n1   = add_room(&s, -12.0f, 12.0f, -20.0f, 10.0f, 10.0f);
        n2   = add_room(&s,  12.0f, 12.0f, -20.0f, 10.0f, 10.0f);
        add_walkway(&s, home, n1);
        add_walkway(&s, home, n2);
        n = route_room_openings(&s, home, op, 16);
        CHECK(n == 2);
        for (i = 0; i < n; i++) if (op[i].wall == ROOM_WALL_N) {
            nN++;
            if (op[i].center < c0) c0 = op[i].center;
            if (op[i].center > c1) c1 = op[i].center;
        }
        CHECK(nN == 2);
        CHECK(c1 - c0 >= ROUTE_DOOR_W);   /* separated, not on top of each other */
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
    /* a room in a non-active workspace is not routed to */
    {
        Scene s; Route routes[ROUTE_MAX]; int i, n, touches = 0;
        sol_u32 home, other;
        scene_init(&s);
        home  = add_room(&s, 0.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        other = add_room(&s, 14.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        add_walkway(&s, home, other);                  /* would otherwise route */
        scene_meta_set(&s, other, "workspace", "hidden");
        strcpy(s.active_ws, "home");                   /* ...but other is hidden */
        n = route_all(&s, routes, ROUTE_MAX);
        for (i = 0; i < n; i++)
            if (routes[i].room_lo == other || routes[i].room_hi == other) touches = 1;
        CHECK(!touches);
        scene_free(&s);
    }
    /* an INACTIVE room OVERLAPPING the active home room must not DEFLECT a door
       (the solarium-overlaps-home case). The straight home->east leg grazes the
       overlapping ghost; only the workspace filter keeps it out of collect_rooms,
       so the door stays on the east wall. Filtered: straight. */
    {
        Scene s; Route r; sol_u32 home, east, ghost, wk;
        scene_init(&s);
        home  = add_room(&s, 0.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        east  = add_room(&s, 20.0f, 12.0f, 0.0f, 10.0f, 10.0f);
        ghost = add_room(&s, 0.0f, 12.0f, 0.0f, 8.0f, 8.0f);  /* overlaps home */
        scene_meta_set(&s, ghost, "workspace", "other");
        strcpy(s.active_ws, "home");                          /* ghost hidden */
        wk = add_walkway(&s, home, east);
        CHECK(route_for_walkway(&s, wk, &r));
        CHECK(r.valid);
        CHECK(r.straight);
        CHECK((r.wall_lo == ROOM_WALL_E && r.wall_hi == ROOM_WALL_W) ||
              (r.wall_lo == ROOM_WALL_W && r.wall_hi == ROOM_WALL_E));
        scene_free(&s);
    }
    /* the same scene UNFILTERED (active_ws ""): the ghost is now a bystander and
       DEFLECTS the door off the straight path. This is the load-time bug — the
       fix is to set active_ws before route/collide rebuild, not after. */
    {
        Scene s; Route r; sol_u32 home, east, ghost, wk;
        scene_init(&s);
        home  = add_room(&s, 0.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        east  = add_room(&s, 20.0f, 12.0f, 0.0f, 10.0f, 10.0f);
        ghost = add_room(&s, 0.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        scene_meta_set(&s, ghost, "workspace", "other");      /* but no filter set */
        wk = add_walkway(&s, home, east);
        CHECK(route_for_walkway(&s, wk, &r));
        CHECK(r.valid);
        CHECK(!r.straight);          /* deflected by the overlapping bystander */
        scene_free(&s);
    }
    /* a walkway routes between a room and a terrain island (island endpoint
       resolves to its own footprint; the route is valid). */
    {
        Scene s; Route r; sol_u32 home, isle, wk;
        scene_init(&s);
        home = add_room(&s,   0.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        isle = add_island(&s, 30.0f, 12.0f, 0.0f, 20.0f, 16.0f);  /* hw=10, hd=8 */
        wk   = add_walkway(&s, home, isle);
        CHECK(route_for_walkway(&s, wk, &r));
        CHECK(r.valid);
        CHECK(r.room_lo == home || r.room_hi == home);
        CHECK(r.room_lo == isle || r.room_hi == isle);
        scene_free(&s);
    }
    test_window_sill_panel();
    test_window_fill_styles();
    test_window_glass_styles();
    test_window_above_wall();
    test_stacked_windows();
    if (fails == 0) printf("route_test: OK\n");
    return fails ? 1 : 0;
}
