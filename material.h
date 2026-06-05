/* material.h — a surface's PBR material: the textures + scalar factors that feed
   the Cook-Torrance shader (item 8). Texture handles are shared references (the
   app owns/destroys them), so a Material is trivially copyable by value. */
#ifndef MATERIAL_H
#define MATERIAL_H

#include "rhi.h"         /* RhiTexture */
#include "sol_types.h"   /* vec3 */

typedef struct {
    RhiTexture albedo_tex;   /* base-color map; id 0 = none (use base_color alone) */
    vec3       base_color;   /* baseColorFactor (linear); multiplies albedo */
    float      metallic;     /* metallicFactor  [0,1] */
    float      roughness;    /* roughnessFactor [0,1] */
    /* 8b adds mr_tex (G=rough, B=metal); 8c ao_tex; 8d normal_tex + normal_scale */
} Material;

/* A neutral dielectric (white, non-metal, mid roughness) — what every scene
   object starts as until something sets it. */
Material material_default(void);

#endif /* MATERIAL_H */
