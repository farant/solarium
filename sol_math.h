#ifndef SOL_MATH_H
#define SOL_MATH_H

#include "sol_base.h"    /* sol_bool */
#include "sol_types.h"   /* vec3, vec4, mat4, quat, Ray, Aabb */

/* Column-major math layer. These are external functions defined in sol_math.c,
   so any module can include this header and use only what it needs — no
   -Wunused-function from header-static definitions. */

#define SOL_PI 3.14159265358979323846f

float sol_radians(float deg);

/* ---- vec3 ---- */
vec3  vec3_make(float x, float y, float z);
vec3  vec3_add(vec3 a, vec3 b);
vec3  vec3_sub(vec3 a, vec3 b);
vec3  vec3_scale(vec3 v, float s);
float vec3_dot(vec3 a, vec3 b);
vec3  vec3_cross(vec3 a, vec3 b);
vec3  vec3_normalize(vec3 v);

/* ---- mat4 basics ---- */
mat4  mat4_identity(void);
mat4  mat4_mul(mat4 a, mat4 b);   /* C = A * B; applied to a vector, B happens first */

/* ---- mat3 (normals; item 8) ---- */
mat3  mat3_normal_matrix(mat4 model);   /* inverse-transpose of model's upper-left 3x3 */

/* ---- mat4 model transforms ---- */
mat4  mat4_translate(vec3 t);
mat4  mat4_scale(vec3 s);

/* ---- mat4 camera + projection ---- */
mat4  mat4_look_at(vec3 eye, vec3 center, vec3 up);
mat4  mat4_perspective(float fovy, float aspect, float near, float far);  /* fovy in radians */

/* ---- quaternions (unit quaternion = a rotation) ---- */
quat  quat_identity(void);
quat  quat_from_axis_angle(vec3 axis, float angle);
quat  quat_mul(quat a, quat b);   /* Hamilton product; quat_mul(a,b) = apply b, then a */
quat  quat_normalize(quat q);
mat4  quat_to_mat4(quat q);

/* compose a TRS transform: M = T * R * S (vertex order: scale, rotate, translate) */
mat4  mat4_from_trs(vec3 pos, quat rot, vec3 scale);

/* ---- ray casting (item 4 picking) ---- */
vec3     mat4_mul_point(mat4 m, vec3 p);                 /* affine point transform (w=1) */
vec3     mat4_mul_dir(mat4 m, vec3 d);                   /* direction transform (no translation) */
Aabb     aabb_transform(mat4 m, Aabb box);               /* AABB of the transformed corners */
sol_bool ray_vs_aabb(Ray ray, Aabb box, float *t_out);  /* slab test; *t_out = entry distance */

#endif /* SOL_MATH_H */
