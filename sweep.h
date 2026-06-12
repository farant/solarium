/* sweep.h — the profile lathe (34th TU; extracted from the gothic kit,
   P7 item 1). One small 2D section extruded along a planar path: miter
   joints (rings stretched 1/cos(half-turn) so the silhouette holds
   through a bend), auto-creasing past 30 degrees, closed loops, earclip
   end caps — and TAPER: a per-station scale with the cone-corrected
   normals trees need. Pure CPU, strict C89, headless-testable.

   A forest should not include a cathedral: gothic.c and flora are both
   clients. `gothic_sweep` remains the kit's untapered idiom — a one-line
   wrapper, so no call site ever moved and the old name stays currency. */
#ifndef SWEEP_H
#define SWEEP_H

#include "mesh.h"        /* MeshBuilder */
#include "sol_types.h"   /* vec3 */

#define SWEEP_MAX_PROF 16

/* the two-cap rule's ANGULAR cap (P6 item 1, ruled): 22.5 degrees per
   segment — rescues small circles the linear cap alone would polygonize.
   The LINEAR cap is the caller's policy (gothic passes GOTHIC_MAX_SEG). */
#define SWEEP_MAX_ANG (22.5f * 0.01745329252f)

/* A profile point, in section space: o = outward, u = up in the
   section's frame. Creased points get hard normals (a fillet arris);
   uncreased points share an averaged normal (a roll reads round).
   Profiles are OPEN polylines wound CCW in (o,u) so face normals point
   out of the material; CLOSED profiles repeat their first point
   (creased at the seam). */
typedef struct { float o, u; unsigned char crease; } ProfilePt;

/* Segment count for an arc of arc_len meters turning arc_angle radians:
   n = max(ceil(len / max_seg), ceil(angle / SWEEP_MAX_ANG)), at least 1;
   ceil carries a dust epsilon (an arc that is EXACTLY k segments must
   not flip to k+1 on the last bit of a float division). */
int sweep_segments(float arc_len, float arc_angle, float max_seg);

/* The sweep. scale multiplies the profile everywhere; `scales` (may be
   NULL) multiplies it AGAIN per path station — the taper. The contract:
   - scales == NULL (or any constant array) reproduces the untapered
     sweep BYTE-IDENTICALLY: the zero-taper path runs the exact same
     arithmetic (the equivalence law, held by the suite).
   - Under taper, normals lean by the cone correction
     n ~ m - sc'*(V.m)*t — sc' read from the INCOMING segment (exact
     for linear tapers; one-sided where the taper rate changes).
   - Closed loops (first == last point) may taper, but the caller owes
     scales[0] == scales[path_n-1] or the seam ring takes station 0's.
   Limits as ever: 2 <= prof_n <= SWEEP_MAX_PROF, path_n >= 2, planar
   path, no doubled-back joints — violations emit nothing. UVs world-
   scale: u = profile arclength (at the station's scale), v = path
   arclength. cap0/cap1 emit flat earclip end caps. */
void sweep_extrude(MeshBuilder *b, const ProfilePt *prof, int prof_n,
                   const vec3 *path, int path_n, vec3 plane_n,
                   float scale, const float *scales, int cap0, int cap1);

#endif /* SWEEP_H */
