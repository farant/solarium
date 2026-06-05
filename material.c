#include "material.h"

/* Fields set directly (not via vec3_make) so this TU stays free of the math
   layer — it only needs the types. */
Material material_default(void) {
    Material m;
    m.albedo_tex.id = 0;
    m.mr_tex.id     = 0;
    m.ao_tex.id     = 0;
    m.base_color.x  = 1.0f;
    m.base_color.y  = 1.0f;
    m.base_color.z  = 1.0f;
    m.metallic      = 0.0f;     /* dielectric */
    m.roughness     = 0.6f;     /* mid — most of our procedural surfaces */
    m.ao_strength   = 1.0f;
    return m;
}
