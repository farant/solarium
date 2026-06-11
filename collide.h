/* collide.h — the collision layer (P4 item 1): the world's lateral push-back.
   A kinematic constraint filter between "desired move" and "actual move": the
   player is a vertical capsule, the world is a flat set of upright (possibly
   yaw-rotated) box colliders DERIVED from the same parametric refs the
   renderer draws (TODO4 §1.3 — the world's shape has one author). Because
   everything we collide with is upright, and a yaw rotation keeps a vertical
   capsule vertical in the box's local frame, every test reduces to a CIRCLE
   vs a RECTANGLE in the floor plan, gated by a vertical interval.

   Vertical motion is deliberately NOT resolved here: the ground seam
   (ground_under + the settle glide) keeps the up-down authority; this layer
   owns sideways, plus a fly-mode clamp at undersides and tops. Pure CPU math
   over a flat array — no GL, no scene types, headless-tested (coltest). */

#ifndef COLLIDE_H
#define COLLIDE_H

#include "sol_base.h"
#include "sol_types.h"

/* The treaty constant between the two motion authorities: a box whose top is
   within STEP_UP of your feet is GROUND (the settle glide climbs it — this
   layer ignores it); higher is a WALL (this layer resists it). main.c's
   ground_under must use the SAME constant — one constant, two consumers, or
   thresholds become curbs. */
#define COLLIDE_STEP_UP  0.6f

/* The player capsule: RADIUS is the body half-width, HEIGHT runs feet to
   crown (eye height 1.65 sits just below the crown). */
#define COLLIDE_RADIUS   0.30f
#define COLLIDE_HEIGHT   1.80f

/* One upright box collider in world space: a rectangle in the floor plan
   (center, yaw about +Y stored as cos/sin, half-extents along the box's own
   plan axes) swept over a vertical interval. `source` records the scene
   handle that contributed it (0 = hand-built / none) for debugging and
   exclusions. */
typedef struct ColliderBox {
    float   cx, cz;       /* plan center */
    float   cyaw, syaw;   /* cos(yaw), sin(yaw), precomputed at add */
    float   hx, hz;       /* plan half-extents in the box's local axes */
    float   y0, y1;       /* world vertical interval, bottom/top */
    sol_u32 source;
} ColliderBox;

typedef struct ColliderSet {
    ColliderBox *boxes;
    int          count;
    int          cap;
} ColliderSet;

void collide_set_init(ColliderSet *cs);
void collide_set_free(ColliderSet *cs);
void collide_set_clear(ColliderSet *cs);   /* count -> 0, capacity survives (the rebuild pattern) */
void collide_set_add(ColliderSet *cs, float cx, float cz, float yaw,
                     float hx, float hz, float y0, float y1, sol_u32 source);

/* Resolve a desired move laterally against the set. `feet` is the capsule's
   ground point (world); the return is the new feet position. move.y passes
   through untouched — the ground seam owns it. Internally the move is
   SUBSTEPPED (so one clamped-dt frame can never tunnel a thin wall) and per
   substep the deepest contact is pushed out along its normal up to a few
   iterations (corners need two): the push-out IS the slide — it removes only
   the into-wall component of the committed motion, so the tangential part
   survives and you glide. A zero move resolves nothing (motion is the only
   trigger — the settle-while-moving doctrine, again). */
vec3 collide_slide(const ColliderSet *cs, vec3 feet, vec3 move,
                   float radius, float height);

/* Fly mode's vertical clamp: returns how much of dy is actually permitted
   before the crown meets an underside (rising) or the feet meet a top
   (sinking). A box participates only when the capsule's plan circle overlaps
   its footprint; faces are only caught from OUTSIDE (crossing in), which is
   all fly motion can legally attempt. */
float collide_clamp_y(const ColliderSet *cs, vec3 feet, float dy,
                      float radius, float height);

#endif /* COLLIDE_H */
