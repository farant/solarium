#ifndef GOTHIC_H
#define GOTHIC_H

#include "mesh.h"        /* MeshBuilder */
#include "sol_types.h"   /* vec3 */

/* gothic.h — the Gothic kit (P6). Pure CPU geometry vocabulary with
   collide.c's citizenship: strict C89, headless-testable, no rhi/scene
   includes. mesh.c's registry rows compose these; main.c places them
   (TODO6 §1.7 — the kit is a vocabulary, main.c composes).

   Item 1: the profile sweep. One operation underlies the entire molded
   vocabulary — ribs, archivolts, mullions, string courses, hood-molds,
   shafts, bases are a small 2D section extruded along a planar path. */

/* Tessellation — two caps, take the finer (TODO6 item 1, ruled): the
   linear cap governs big rib arcs (voussoir-scale faceting, cheap); the
   angular cap rescues small circles (a 0.3 m tracery foil under the
   linear cap alone would be a 4-gon). */
#define GOTHIC_MAX_SEG 0.25f                     /* meters per chord    */
#define GOTHIC_MAX_ANG (22.5f * 0.01745329252f)  /* radians per segment */

/* Segment count for an arc of arc_len meters turning arc_angle radians:
   n = max(ceil(len / MAX_SEG), ceil(angle / MAX_ANG)), at least 1. */
int gothic_arc_segments(float arc_len, float arc_angle);

/* A profile point, in section space: o = outward (away from the wall or
   core; profiles project into +o — flip via the sweep's plane/path), u =
   up in the section's frame. Creased points get hard normals (a fillet
   arris); uncreased points share an averaged normal (a roll reads round).
   Profiles are OPEN polylines walked bottom -> out -> top, wound CCW in
   (o,u) so face normals point out of the material; CLOSED profiles repeat
   their first point (creased at the seam). */
typedef struct { float o, u; unsigned char crease; } ProfilePt;

/* The molding table (§Item 1) — APPEND-ONLY: profile ids are scene-file
   currency once the kit's params reference them. The glossary is the
   namespace (§1.6). */
enum {
    PROF_RIB = 0,    /* chamfered vault rib: roll between fillets        */
    PROF_MULLION,    /* the standard double-chamfer window bar (closed)  */
    PROF_STRING,     /* string course: cavetto over roll, weathered top  */
    PROF_BASE,       /* attic base: torus, scotia, torus (plinth apart)  */
    PROF_HOOD,       /* hood-mold drip over an arch                      */
    PROF_SHAFT_OCT,  /* octagonal shaft section (closed)                 */
    PROF_COUNT
};

/* The table entry: points + count (borrowed pointer, static lifetime).
   NULL for an unknown id. Profiles are sized in meters at canonical
   scale; a nave rib and a window mullion are the same profile at
   different scales — historically true. */
const ProfilePt *gothic_profile(int prof_id, int *out_n);

/* Sweep `prof` along the PLANAR polyline `path` (plane normal `plane_n`,
   unit not required). The binormal is the plane normal, constant along
   the sweep — exact framing, no parallel transport, no torsion (the v1
   planarity law). At interior joints rings lie in the MITER plane (the
   in-plane axis stretched by 1/cos(half-turn)) so segment faces stay
   planar continuations; joints turning more than ~30 degrees auto-crease
   (two rings, per-segment hard normals — a sharp arris, not a smear).
   Section axes in world: o maps to cross(plane_n, path_dir), u maps to
   plane_n. scale multiplies the profile; cap0/cap1 emit flat fan end
   caps (abutting ends per §1.2 skip theirs). UVs are world-scale: u =
   profile arclength, v = path arclength, in meters.
   Limits: 2 <= prof_n <= GOTHIC_SWEEP_MAX_PROF, path_n >= 2, no
   doubled-back joints (turn ~180 degrees) — violations emit nothing. */
#define GOTHIC_SWEEP_MAX_PROF 16
void gothic_sweep(MeshBuilder *b, const ProfilePt *prof, int prof_n,
                  const vec3 *path, int path_n, vec3 plane_n,
                  float scale, int cap0, int cap1);

/* ---- item 2: the arch family ----
   The two-arc pointed construction: an opening of span s springs at
   y=0 from (+-s/2, 0); each half is a circular arc centered ON the
   springing line at -+c, radius r = c + s/2, crown at sqrt(r^2 - c^2).
   One shape float, ACUTENESS a = 2c/s, spans the whole historical
   family: 0 = semicircular, 1 = equilateral, >1 = lancet, between =
   drop arch. */

/* head height above the springing at offset x from center (0 outside) */
float gothic_arch_y(float s, float a, float x);

/* the LEVEL-CROWN solve — the formula Gothic exists for: the acuteness
   that lifts span s to crown height H (valid H >= s/2; flatter clamps
   to 0, the semicircle). Item 5's vaults lean on this: arches of
   different spans meeting at one crown. */
float gothic_arch_acuteness_for(float s, float crown_h);

/* The arch polyline in the XY plane (z = 0), springing (-s/2,0) to
   (+s/2,0), subdivided per half by the two-cap rule against max_seg
   (pass GOTHIC_MAX_SEG normally). Returns the point count — always ODD,
   apex EXACTLY at (0, crown) (the crown must be a vertex or every rib
   boss floats), bit-symmetric about x=0, x monotone. 0 if max_n is too
   small or params are degenerate. */
#define GOTHIC_ARCH_MAX_PTS 129
int gothic_arch_path(vec3 *out, int max_n, float s, float a, float max_seg);

/* make_wall_with_opening's sibling with a pointed head (TODO6 §1.4 —
   around-the-gap, now with curves): jamb panels left/right, jamb
   reveals up to the springing, the head as VERTICAL STRIPS off the arch
   polyline (front/back from curve to wall top, intrados across the real
   thickness, creased per strip — voussoir faceting, deliberately not
   smooth), threshold at y=0. Same conventions and degenerate handling
   as the flat emitter: x in [-w/2,w/2], y in [0,h], thickness centered,
   opening ox from the LEFT edge, ow wide, springing at spring_h.
   A crown above the wall top emits nothing. */
void gothic_wall_arched(MeshBuilder *b, float w, float h, float t,
                        float ox, float ow, float spring_h, float a);

/* The same wall with N RECESSED ORDERS stepping through the thickness
   (the mechanism item 7's portal dresses): the centered opening repeats
   at spans stepped +2*step per order toward the FRONT (+z) face —
   widest outside, funneling in. Step faces connect each order's
   reveals to the next; archivolts != 0 dresses every order's arris
   with a PROF_RIB sweep (roll toward the opening). */
#define GOTHIC_MAX_ORDERS 4
void gothic_wall_portal(MeshBuilder *b, float w, float h, float t,
                        float ow, float spring_h, float a,
                        int orders, float step, int archivolts);

/* ---- item 3: the plan function — the constitutional centerpiece ----
   church_plan() is to the church what terrain_height is to the island
   (§1.2): a pure, deterministic, allocation-free expansion of the ref
   params into the building's structural truth. The stone emitter, the
   glass emitter, the collider derivation, the ruin pass — every one a
   READER of the same expansion. There is no stored plan, no cached
   layout, no second description. */

/* The lane hash (§1.3): every architectural decision draws from a NAMED
   LANE — never a sequential stream, so adding a decision can never
   reshuffle an existing church. Per-bay decisions key on (i, j). */
float gothic_hash01(unsigned seed, int lane, int i, int j);

/* APPEND-ONLY: reordering lanes is a save-breaking change, treated like
   reordering the STML schema. Add at the tail, forever. */
enum {
    LANE_STYLE = 0,   /* hall vs basilica                            */
    LANE_MODULE,      /* bay length jitter about nave_w/2            */
    LANE_NAVE_W,      /* nave width within the style range           */
    LANE_APSE,        /* polygonal vs flat east end                  */
    LANE_TOWER,       /* west tower bay                              */
    LANE_ELEV,        /* the elevation formula's scalars (j = which) */
    LANE_TOWER_H,     /* item 4: tower height                        */
    LANE_PITCH,       /* item 7: roof pitch                          */
    LANE_TRACERY,     /* item 6: lights divisor jitter               */
    LANE_RUIN,        /* item 8: the decay field                     */
    LANE_RUIN_DIR,    /* item 8: which end collapses first           */
    LANE_SPIRE,       /* item 7: broach vs parapet-and-needle        */
    LANE_COUNT
};

enum { CHURCH_CHAPEL = 0, CHURCH_HALL = 1, CHURCH_BASILICA = 2 };

#define PLAN_MAX_BAYS 16

/* Scalars ONLY — every ELEMENT is answered by the queries below,
   computed on demand (terrain_height's citizenship). The plan frame:
   east = local +X, the nave runs the plot's LONGER dimension, plot
   centered on the origin; `swapped` says the source plot was deeper
   than wide (the spawner's yaw concern, the readers just need to map
   x<->z). memset-zeroed before filling, so struct memcmp IS the §1.8
   determinism test. */
typedef struct {
    int      style;            /* CHURCH_CHAPEL / HALL / BASILICA          */
    int      nbays;            /* regular bays, west -> east               */
    int      aisles;           /* 1 = aisled (hall/basilica)               */
    int      apse_sides;       /* 0 = flat east wall, 5 = the 5/8 octagon  */
    int      tower;            /* 1 = west tower annex                     */
    int      swapped;          /* plot was deeper than wide                */
    float    plot_l, plot_w;   /* nave-frame plot dims (l = the long one)  */
    float    margin;           /* perimeter buttress reserve               */
    float    nave_w, aisle_w;  /* clear widths (ad quadratum: aw = nw/2)   */
    float    bay_l;            /* the module (the doubled-square bay)      */
    float    wall_t;
    float    tower_d, apse_d;  /* annex depths (0 when absent)             */
    float    west_x, east_x;   /* the BODY's west/east faces (tower+bays)  */
    float    porch;            /* west residue: item 7's steps             */
    float    plinth_h, sill_h;
    float    impost_h;         /* arcade springing / capital line          */
    float    arcade_h;         /* top of the arcade band                   */
    float    clerest_h0, clerest_h1;  /* clerestory band (basilica only)   */
    float    aisle_h, wall_h;  /* aisle / nave wall heads                  */
    float    parapet_h;        /* band above the wall head (item 4)        */
    float    acute;
    unsigned seed;
} ChurchPlan;

/* params follow the church ref schema (shared verbatim by all four
   sub-refs, §1.7): { w, d, seed, style, ruin, built, acute, reserved },
   defaults { 18, 30, 7, -1, 0, 1, 1, 0 }. A PREFIX is legal; the tail
   fills from defaults (the registry's merge rule). */
void church_plan(ChurchPlan *p, const float *params, int count);

/* pier stations: i = 0..nbays along the axis (fenceposts), j = row.
   Chapel has no arcade rows. Returns 1 + position in the plan frame. */
enum {
    PIER_ROW_S_WALL = 0, PIER_ROW_S_ARCADE,
    PIER_ROW_N_ARCADE,   PIER_ROW_N_WALL
};
int plan_pier(const ChurchPlan *p, int i, int j, float *out_x, float *out_z);

/* the apse polygon's 6 vertices (5 sides of an octagon), south to
   north; the mouth pair sits ON the east wall line at z = -+nave_w/2 */
int plan_apse_pier(const ChurchPlan *p, int k, float *out_x, float *out_z);

enum { GOTHIC_BAY_NONE = 0, GOTHIC_BAY_NAVE, GOTHIC_BAY_AISLE };
int plan_bay_kind(const ChurchPlan *p, int i, int lane);  /* lane 0/1/2 = S aisle/nave/N aisle */

/* openings by RULE, not by draw: aisle and clerestory bays get one
   window each (width 0.55 x the bay's clear span, sill at the string
   course); the west front gets the portal (i = 0) and the great window
   (i = 1). Windows FLATTEN TO FIT via the level-crown solve when the
   default acuteness would push the crown into the wall head — the plan
   guarantees the emitters' preconditions. kind NONE = nothing there. */
enum { GOTHIC_OPEN_NONE = 0, GOTHIC_OPEN_WINDOW, GOTHIC_OPEN_DOOR };
enum {
    WALL_AISLE_S = 0, WALL_AISLE_N,
    WALL_CLEREST_S,   WALL_CLEREST_N,
    WALL_WEST,
    WALL_EAST,        /* the flat east end's window (apse_sides == 0)  */
    WALL_APSE         /* one lancet per chevet side, i = 0..4          */
};
typedef struct {
    int   kind;
    float cx;             /* center ALONG the wall (x for S/N, z for west) */
    float w;              /* opening span                                  */
    float sill, spring;   /* heights; doors sill 0                         */
    float acute;          /* possibly flattened from the plan's            */
} GothicOpening;
void plan_opening(const ChurchPlan *p, int wall, int i, GothicOpening *out);

/* plan_supports (§1.5, the dependency edges) lands with its consumers:
   item 5 registers flyer/web edges, item 8 traverses them. The element
   id encoding is deferred until those classes exist — nothing reads it
   yet, and an unread encoding is a guess. */

/* ---- item 4: the stone shell — the plan's first full reader ----
   Everything load-bearing and opaque in ONE mesh: perimeter walls with
   their windows, open arcades, the clerestory band, the split west
   front (portal below, great window above), the flat east or the
   chevet, piers (octagonal shaft, attic base, capital), stepped
   weathered buttresses, the string course jogging around them,
   parapets, and the tower with paired belfry lights. ruin/built params
   are carried but idle until item 8. */
void church_stone(MeshBuilder *b, const float *params, int count);

/* the shared ref-schema defaults — mesh.c's registry rows must carry
   these same values (gothictest asserts the two tables agree) */
extern const float gothic_church_defaults[8];

#endif /* GOTHIC_H */
