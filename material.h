/* material.h — a surface's PBR material: the textures + scalar factors that feed
   the Cook-Torrance shader (item 8). Texture handles are shared references (the
   app owns/destroys them), so a Material is trivially copyable by value. */
#ifndef MATERIAL_H
#define MATERIAL_H

#include "rhi.h"         /* RhiTexture */
#include "sol_types.h"   /* vec3 */

typedef struct {
    RhiTexture albedo_tex;   /* base-color map; id 0 = none (use base_color alone) */
    RhiTexture mr_tex;       /* metallic-roughness map (G=rough, B=metal); id 0 = none */
    RhiTexture ao_tex;       /* occlusion map (R channel); id 0 = none. often == mr_tex (ORM) */
    RhiTexture normal_tex;   /* tangent-space normal map (linear); id 0 = none */
    vec3       base_color;   /* baseColorFactor (linear); multiplies albedo */
    vec3       emissive;     /* emitted light, linear (P4 item 5): ADDED after
                                the lighting sum — seen by the camera, felt by
                                nobody (pair with a point light for the room).
                                May exceed 1.0: that's what bloom bites on. */
    float      metallic;     /* metallicFactor  [0,1] */
    float      roughness;    /* roughnessFactor [0,1] */
    float      ao_strength;  /* occlusionTexture.strength (default 1.0) */
    float      normal_scale; /* normalTexture.scale (default 1.0) — bump strength */
} Material;

/* A neutral dielectric (white, non-metal, mid roughness) — what every scene
   object starts as until something sets it. */
Material material_default(void);

#endif /* MATERIAL_H */
