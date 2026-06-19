#ifndef SOL_ROUTE_H
#define SOL_ROUTE_H

#include "scene.h"
#include "sol_math.h"
#include "mesh.h"   /* RoomOpening, ROOM_WALL_* */

#define ROUTE_DOOR_W 1.4f
#define ROUTE_DOOR_H 2.1f
#define ROUTE_WALL_T 0.20f
#define ROUTE_DECK_W (ROUTE_DOOR_W - 0.2f)   /* walkway deck a touch NARROWER than the door, so it passes through the opening cleanly */
#define ROUTE_DECK_T 0.15f                    /* walkway deck thickness */
#define ROUTE_MAX    256
#define ROUTE_STRAIGHT_EPS 0.5f   /* exit-axis offset under which a route is "straight" */

/* One connection's fully-resolved geometry. World coords. The lower-Y room is
   the anchor: door_lo is the mesh origin of the walkway; the path bends at
   `corner` and ends at door_hi. `straight` => no bend (corner == door_hi). */
typedef struct {
    sol_u32 walkway;
    sol_u32 room_lo, room_hi;     /* parent (room_type) handles */
    int     wall_lo, wall_hi;     /* ROOM_WALL_* opened on each room */
    vec3    door_lo, corner, door_hi;
    int     straight;
    int     valid;                /* 0 = dangling/degenerate; skip */
} Route;

/* Compute every walkway's route. Returns the count written to out (<= max).
   Door centers are spread when multiple routes share a (room,wall). */
int  route_all(Scene *s, Route *out, int max);

/* The route for one walkway handle (recomputes route_all internally). Returns
   1 and fills *out if found+valid, else 0. */
int  route_for_walkway(Scene *s, sol_u32 walkway, Route *out);

/* The door openings on one room (parent handle), in ROOM-LOCAL coords, for
   make_room_doored / the collider. Returns the count written (<= max). */
int  route_room_openings(Scene *s, sol_u32 room, RoomOpening *out, int max);

#endif
