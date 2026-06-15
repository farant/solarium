/* rock.h — the island's own stone (P7 item 6, the 36th TU). Boulders and
   scree: an fBm-displaced subdivided octahedron, flat-shaded (rocks are
   faceted, not smooth). Pure CPU, strict C89, its own noise twin. Not
   flora — stone earns its own home. */
#ifndef ROCK_H
#define ROCK_H

#include "sol_types.h"   /* vec3 */
#include "mesh.h"        /* MeshBuilder */

/* a boulder: an octahedron subdivided to a roundish ball, then every
   vertex displaced along its direction by fBm — lumps and hollows.
   size = nominal radius (m); seed picks the noise (same seed, same rock,
   forever); flat 0..1 squashes it vertically AND flattens its crown into
   a standable table-rock (a flat boulder is a viewpoint). World-scale
   planar UVs feed the course-free stone texture. ~128 tris. */
void rock_boulder(MeshBuilder *b, float size, unsigned seed, float flat);

/* the unit pebble (FIELD scree): a small displaced octahedron at radius
   ~1, the scatter scales it. Fewer subdivisions — scree is distant. */
void rock_pebble_unit(MeshBuilder *b);

/* the collider's box (one author, two readers — the §1.2 law): the
   boulder's plan half-extent and standable top, derived from the SAME
   size/flat the mesh is built from. */
void rock_boulder_dims(float size, float flat, float *half, float *top);

#endif /* ROCK_H */
