/* texgen.h — synthesized PBR material maps (the texture side-quest).
   The third verse of "never sourced": meshes are params-are-identity
   refs, sounds render from knobs — textures follow. A material is a
   (kind, knobs) pair and the pixels are a deterministic consequence:
   same inputs, memcmp-identical maps. Pure CPU; no RHI contact — the
   app uploads (albedo as sRGB, normal/ORM as linear data). */
#ifndef TEXGEN_H
#define TEXGEN_H

#include "sol_base.h"

#define TEXGEN_SIZE   512               /* each map is SIZE x SIZE RGBA8 */
#define TEXGEN_PARAMS 10                /* one shared knob vector */

/* kinds are PRESETS over one generator (the synth lesson: the schema is
   the instrument, a type just sets the dials) — plaster is literally
   stone with course=0: no masonry grid, only trowel and weather. BARK
   (P7 item 3) is the first ANISOTROPIC kind: ridge noise stretched
   along v, so the grain runs with the lathe's world-scale UVs — up the
   trunk, along every branch, trunk-to-twig, for free. APPEND-ONLY. */
enum { TEXGEN_STONE = 0, TEXGEN_PLASTER, TEXGEN_BARK, TEXGEN_KIND_COUNT };

int         texgen_kind(const char *name);    /* "stone"/"plaster" -> kind; -1 unknown */
const char *texgen_kind_name(int kind);       /* NULL if out of range */

/* The introspectable knob schema (names shared across kinds, defaults
   per kind). Returns TEXGEN_PARAMS, or -1 for an unknown kind. */
int texgen_schema(int kind, const char *const **names,
                  const float **defaults);

/* Render the three maps. params is a PREFIX over the kind's defaults
   (absent knobs keep their preset values — the mesh-ref rule). Each
   output buffer holds TEXGEN_SIZE*TEXGEN_SIZE*4 bytes:
     albedo — sRGB-encoded color (upload as RHI_TEX_SRGB8)
     normal — tangent-space, 0.5-biased (linear)
     orm    — R=occlusion G=roughness B=metallic (linear)
   SOL_FALSE only on allocation failure or a bad kind. */
sol_bool texgen_render(int kind, const float *params, int count,
                       unsigned char *albedo, unsigned char *normal,
                       unsigned char *orm);

#endif /* TEXGEN_H */
