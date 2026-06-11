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

#endif /* GOTHIC_H */
