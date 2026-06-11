#ifndef SOL_MATH_H
#define SOL_MATH_H

#include "sol_base.h"    /* sol_bool */
#include "sol_types.h"   /* vec3, vec4, mat4, quat, Ray, Aabb */

/* Column-major math layer. These are external functions defined in sol_math.c,
   so any module can include this header and use only what it needs — no
   -Wunused-function from header-static definitions. */

#define SOL_PI 3.14159265358979323846f

float sol_radians(float deg);
float sol_smoothstep(float t);    /* 3t^2-2t^3 on clamped [0,1] — animation ease */

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
quat  quat_slerp(quat a, quat b, float t);  /* great-circle arc, constant angular velocity; takes the short way */
quat  quat_conjugate(quat q);     /* the inverse rotation (for unit quaternions) */
quat  quat_normalize(quat q);
vec3  quat_rotate(quat q, vec3 v);   /* rotate v by unit quaternion q */
mat4  quat_to_mat4(quat q);
quat  quat_from_mat4(mat4 m);  /* the rotation in a mat4 whose upper 3x3 is
                                  ORTHONORMAL (normalize columns first to
                                  shed scale) — Shepperd's branching method.
                                  The item-6d deferral (matrix -> TRS
                                  decomposition) cashed for item 9's
                                  matrix-form joints. */

/* compose a TRS transform: M = T * R * S (vertex order: scale, rotate, translate) */
mat4  mat4_from_trs(vec3 pos, quat rot, vec3 scale);

/* one TRS node forward/inverse on a point (item 8: rotated-parent write-back) */
vec3  trs_point_to_world(vec3 p, vec3 t, quat r, vec3 s);  /* T + R(S p) */
vec3  trs_point_to_local(vec3 p, vec3 t, quat r, vec3 s);  /* S^-1(R^-1(p - T)); scale must be nonzero */

/* ---- ray casting (item 4 picking) ---- */
vec3     mat4_mul_point(mat4 m, vec3 p);                 /* affine point transform (w=1) */
vec3     mat4_mul_dir(mat4 m, vec3 d);                   /* direction transform (no translation) */
sol_bool mat4_project_point(mat4 m, vec3 p, vec3 *ndc);  /* full mul + perspective divide; FALSE if w<=0 (behind camera) */
Aabb     aabb_transform(mat4 m, Aabb box);               /* AABB of the transformed corners */
sol_bool ray_vs_aabb(Ray ray, Aabb box, float *t_out);  /* slab test; *t_out = entry distance */
sol_bool ray_vs_plane(Ray ray, vec3 point, vec3 normal, float *t_out);  /* FALSE if parallel or behind */
sol_bool ray_vs_triangle(Ray ray, vec3 v0, vec3 v1, vec3 v2, float *t_out); /* Moller-Trumbore; two-sided; t in units of |dir| */
Frustum  frustum_from_vp(mat4 vp);            /* Gribb-Hartmann: six planes from any view-projection */
sol_bool frustum_intersects_aabb(const Frustum *f, Aabb box); /* positive-vertex test; conservative */

#endif /* SOL_MATH_H */
