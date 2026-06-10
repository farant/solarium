/* sol_math.c — column-major math layer. Definitions for sol_math.h.
   Pure CPU math: no GL, no allocation. */

#include "sol_math.h"

#include <math.h>

float sol_radians(float deg) { return deg * (SOL_PI / 180.0f); }

/* The cubic Hermite ease 3t^2 - 2t^3 on a clamped [0,1]: zero velocity at
   both ends — the animation ease (item 9 book rise / page turn; the same
   family as the room-ambient glide). */
float sol_smoothstep(float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

/* ---- vec3 ---- */
vec3 vec3_make(float x, float y, float z) {
    vec3 r;
    r.x = x; r.y = y; r.z = z;
    return r;
}

vec3 vec3_add(vec3 a, vec3 b) {
    vec3 r;
    r.x = a.x + b.x; r.y = a.y + b.y; r.z = a.z + b.z;
    return r;
}

vec3 vec3_sub(vec3 a, vec3 b) {
    vec3 r;
    r.x = a.x - b.x; r.y = a.y - b.y; r.z = a.z - b.z;
    return r;
}

vec3 vec3_scale(vec3 v, float s) {
    vec3 r;
    r.x = v.x * s; r.y = v.y * s; r.z = v.z * s;
    return r;
}

float vec3_dot(vec3 a, vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

vec3 vec3_cross(vec3 a, vec3 b) {
    vec3 r;
    r.x = a.y * b.z - a.z * b.y;
    r.y = a.z * b.x - a.x * b.z;
    r.z = a.x * b.y - a.y * b.x;
    return r;
}

vec3 vec3_normalize(vec3 v) {
    float len = sqrtf(vec3_dot(v, v));
    vec3 r;
    if (len == 0.0f) return v;
    r.x = v.x / len; r.y = v.y / len; r.z = v.z / len;
    return r;
}

/* ---- mat4 basics ---- */
mat4 mat4_identity(void) {
    mat4 r = {0};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

/* C = A * B (column-major). Applied to a vector, B happens first, then A. */
mat4 mat4_mul(mat4 a, mat4 b) {
    mat4 r;
    int col, row, k;
    for (col = 0; col < 4; col++) {
        for (row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (k = 0; k < 4; k++) {
                sum += a.m[k * 4 + row] * b.m[col * 4 + k];
            }
            r.m[col * 4 + row] = sum;
        }
    }
    return r;
}

/* ---- mat4 model transforms ---- */
mat4 mat4_translate(vec3 t) {
    mat4 r = mat4_identity();
    r.m[12] = t.x;  /* translation in the last column (column-major) */
    r.m[13] = t.y;
    r.m[14] = t.z;
    return r;
}

mat4 mat4_scale(vec3 s) {
    mat4 r = mat4_identity();
    r.m[0]  = s.x;       /* scale on the diagonal */
    r.m[5]  = s.y;
    r.m[10] = s.z;
    return r;
}

/* ---- mat4 camera + projection (standard recipes) ---- */
mat4 mat4_look_at(vec3 eye, vec3 center, vec3 up) {
    vec3 f = vec3_normalize(vec3_sub(center, eye)); /* forward */
    vec3 s = vec3_normalize(vec3_cross(f, up));     /* right   */
    vec3 u = vec3_cross(s, f);                      /* true up */
    mat4 r = mat4_identity();
    r.m[0] = s.x;  r.m[4] = s.y;  r.m[8]  = s.z;
    r.m[1] = u.x;  r.m[5] = u.y;  r.m[9]  = u.z;
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
    r.m[12] = -vec3_dot(s, eye);
    r.m[13] = -vec3_dot(u, eye);
    r.m[14] =  vec3_dot(f, eye);
    return r;
}

/* fovy in radians. Maps depth to NDC z in [-1, 1]. */
mat4 mat4_perspective(float fovy, float aspect, float near, float far) {
    float f = 1.0f / tanf(fovy * 0.5f);
    mat4 r = {0};
    r.m[0]  = f / aspect;                      /* aspect correction lives HERE */
    r.m[5]  = f;
    r.m[10] = (far + near) / (near - far);
    r.m[11] = -1.0f;                           /* feeds -z into w -> perspective */
    r.m[14] = (2.0f * far * near) / (near - far);
    return r;
}

/* ---- quaternions (unit quaternion = a rotation) ---- */
quat quat_identity(void) {
    quat q;
    q.x = 0.0f; q.y = 0.0f; q.z = 0.0f; q.w = 1.0f;   /* no rotation */
    return q;
}

/* rotation of `angle` radians about `axis`: (axis*sin(t/2), cos(t/2)) */
quat quat_from_axis_angle(vec3 axis, float angle) {
    vec3  a = vec3_normalize(axis);
    float h = angle * 0.5f;                 /* the half-angle is the quaternion trick */
    float s = sinf(h);
    quat  q;
    q.x = a.x * s;
    q.y = a.y * s;
    q.z = a.z * s;
    q.w = cosf(h);
    return q;
}

/* Hamilton product. quat_mul(a, b) applied to a vector = apply b, then a. */
quat quat_mul(quat a, quat b) {
    quat q;
    q.w = a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z;
    q.x = a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y;
    q.y = a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x;
    q.z = a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w;
    return q;
}

/* Spherical linear interpolation between unit quaternions: the constant-
   angular-velocity walk along the great-circle arc from a to b (item 9:
   the book's lift-and-face). Lerping components cuts a CHORD through the
   unit sphere instead — speeds up mid-arc and denormalizes. q and -q are
   the same rotation, so a negative dot flips b to take the SHORT way
   around; at tiny angles sin(theta) -> 0, so fall back to normalized lerp
   (chord and arc agree to first order there). */
quat quat_slerp(quat a, quat b, float t) {
    float d = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
    float theta, s, sa, sb;
    quat  r;
    if (d < 0.0f) {                     /* shortest path: -b is the same rotation */
        d = -d;
        b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w;
    }
    if (d > 0.9995f) {                  /* nearly parallel: nlerp is stable here */
        r.x = a.x + (b.x - a.x) * t;
        r.y = a.y + (b.y - a.y) * t;
        r.z = a.z + (b.z - a.z) * t;
        r.w = a.w + (b.w - a.w) * t;
        return quat_normalize(r);
    }
    theta = acosf(d);
    s     = sinf(theta);
    sa    = sinf((1.0f - t) * theta) / s;
    sb    = sinf(t * theta) / s;
    r.x = a.x * sa + b.x * sb;
    r.y = a.y * sa + b.y * sb;
    r.z = a.z * sa + b.z * sb;
    r.w = a.w * sa + b.w * sb;
    return r;
}

/* Conjugate = same axis, opposite angle. For a UNIT quaternion this is the
   inverse rotation (the general inverse divides by |q|^2, which is 1). */
quat quat_conjugate(quat q) {
    quat r;
    r.x = -q.x; r.y = -q.y; r.z = -q.z; r.w = q.w;
    return r;
}

/* Rotate a vector by a unit quaternion: the sandwich product q v q*, expanded
   to two cross products (cheaper than building the 4x4):
       t = 2 (q.xyz x v);  v' = v + q.w t + q.xyz x t */
vec3 quat_rotate(quat q, vec3 v) {
    vec3 qv = vec3_make(q.x, q.y, q.z);
    vec3 t  = vec3_scale(vec3_cross(qv, v), 2.0f);
    return vec3_add(v, vec3_add(vec3_scale(t, q.w), vec3_cross(qv, t)));
}

quat quat_normalize(quat q) {
    float len = sqrtf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    quat  r;
    if (len == 0.0f) return quat_identity();
    r.x = q.x / len; r.y = q.y / len; r.z = q.z / len; r.w = q.w / len;
    return r;
}

/* unit quaternion -> column-major rotation matrix */
mat4 quat_to_mat4(quat q) {
    float xx = q.x*q.x, yy = q.y*q.y, zz = q.z*q.z;
    float xy = q.x*q.y, xz = q.x*q.z, yz = q.y*q.z;
    float wx = q.w*q.x, wy = q.w*q.y, wz = q.w*q.z;
    mat4 r = mat4_identity();
    r.m[0] = 1.0f - 2.0f*(yy+zz);  r.m[4] = 2.0f*(xy-wz);         r.m[8]  = 2.0f*(xz+wy);
    r.m[1] = 2.0f*(xy+wz);         r.m[5] = 1.0f - 2.0f*(xx+zz);  r.m[9]  = 2.0f*(yz-wx);
    r.m[2] = 2.0f*(xz-wy);         r.m[6] = 2.0f*(yz+wx);         r.m[10] = 1.0f - 2.0f*(xx+yy);
    return r;
}

/* compose a TRS transform into a model matrix: M = T * R * S
   (applied to a vertex, right-to-left: scale, then rotate, then translate) */
mat4 mat4_from_trs(vec3 pos, quat rot, vec3 scale) {
    mat4 t = mat4_translate(pos);
    mat4 r = quat_to_mat4(rot);
    mat4 s = mat4_scale(scale);
    return mat4_mul(t, mat4_mul(r, s));
}

/* Single-node TRS applied to a POINT, and its exact inverse. The inverse of a
   composed function runs the inverse steps in REVERSE order:
       to_world: p' = T + R(S p)        to_local: p = S^-1 (R^-1 (p' - T))
   When R is identity and S is 1 these collapse to add/subtract — the
   "unrotated parent" shortcut the drag code used until item 8. */
vec3 trs_point_to_world(vec3 p, vec3 t, quat r, vec3 s) {
    p.x *= s.x; p.y *= s.y; p.z *= s.z;
    p = quat_rotate(r, p);
    return vec3_add(p, t);
}

/* Precondition: nonzero scale on every axis (a zero-scale node has collapsed
   a dimension; no inverse exists and the division yields inf, visibly). */
vec3 trs_point_to_local(vec3 p, vec3 t, quat r, vec3 s) {
    p = quat_rotate(quat_conjugate(r), vec3_sub(p, t));
    p.x /= s.x; p.y /= s.y; p.z /= s.z;
    return p;
}

/* Affine transform of a point (implicit w=1): includes the translation column.
   No perspective divide — model/world matrices keep w=1. */
vec3 mat4_mul_point(mat4 m, vec3 p) {
    vec3 r;
    r.x = m.m[0]*p.x + m.m[4]*p.y + m.m[8]*p.z  + m.m[12];
    r.y = m.m[1]*p.x + m.m[5]*p.y + m.m[9]*p.z  + m.m[13];
    r.z = m.m[2]*p.x + m.m[6]*p.y + m.m[10]*p.z + m.m[14];
    return r;
}

/* Ray vs the infinite plane through `point` with unit `normal`: solve
   dot(N, O + tD - P0) = 0 for t. SOL_FALSE when the ray is (near) parallel
   to the plane or the hit is behind the origin (t < 0). NOTE the danger
   zone is NEAR-parallel, not parallel: a grazing ray turns pixel-sized
   cursor moves into hits racing toward the horizon — callers placing
   objects should ALSO clamp t to a sane distance. */
sol_bool ray_vs_plane(Ray ray, vec3 point, vec3 normal, float *t_out) {
    float denom = vec3_dot(normal, ray.dir);
    float t;
    if (denom > -1e-4f && denom < 1e-4f) return SOL_FALSE;
    t = vec3_dot(normal, vec3_sub(point, ray.origin)) / denom;
    if (t < 0.0f) return SOL_FALSE;
    *t_out = t;
    return SOL_TRUE;
}

/* Project a world point through a view-projection matrix to NDC: the full
   4-component multiply (w out), then the PERSPECTIVE DIVIDE — the one place
   w earns its keep (distant points have larger w, so they shrink toward the
   center). Returns SOL_FALSE when clip w <= 0 (point at/behind the camera
   plane): dividing by negative w flips both axes, projecting a plausible but
   WRONG screen position — the classic mirrored-label-behind-you bug. Cull
   BEFORE the divide. The inverse direction of camera_ray. */
sol_bool mat4_project_point(mat4 m, vec3 p, vec3 *ndc_out) {
    float cx, cy, cz, cw;
    cx = m.m[0]*p.x + m.m[4]*p.y + m.m[8]*p.z  + m.m[12];
    cy = m.m[1]*p.x + m.m[5]*p.y + m.m[9]*p.z  + m.m[13];
    cz = m.m[2]*p.x + m.m[6]*p.y + m.m[10]*p.z + m.m[14];
    cw = m.m[3]*p.x + m.m[7]*p.y + m.m[11]*p.z + m.m[15];
    if (cw <= 0.0f) return SOL_FALSE;
    ndc_out->x = cx / cw;
    ndc_out->y = cy / cw;
    ndc_out->z = cz / cw;
    return SOL_TRUE;
}

/* Transform a direction by the upper 3x3 (no translation). For normals this is
   exact under rotation + uniform scale; under non-uniform scale it's an
   approximation (the strictly-correct form is the inverse-transpose). */
vec3 mat4_mul_dir(mat4 m, vec3 d) {
    vec3 r;
    r.x = m.m[0]*d.x + m.m[4]*d.y + m.m[8]*d.z;
    r.y = m.m[1]*d.x + m.m[5]*d.y + m.m[9]*d.z;
    r.z = m.m[2]*d.x + m.m[6]*d.y + m.m[10]*d.z;
    return r;
}

/* The normal matrix: the inverse-transpose of a model matrix's upper-left 3x3.
   For a 3x3 with columns (a,b,c), the inverse-transpose columns are
   (b x c, c x a, a x b) / det — so normals stay perpendicular under non-uniform
   scale, where the naive mat3(model) skews them. Computed via cross products,
   no general matrix inverse needed. */
mat3 mat3_normal_matrix(mat4 model) {
    vec3  a, b, c, n0, n1, n2;
    float det, inv;
    mat3  r;

    a = vec3_make(model.m[0], model.m[1], model.m[2]);   /* upper-left 3x3 columns */
    b = vec3_make(model.m[4], model.m[5], model.m[6]);
    c = vec3_make(model.m[8], model.m[9], model.m[10]);

    n0 = vec3_cross(b, c);
    n1 = vec3_cross(c, a);
    n2 = vec3_cross(a, b);

    det = vec3_dot(a, n0);                               /* = det of the 3x3 */
    inv = (det != 0.0f) ? 1.0f / det : 0.0f;

    r.m[0] = n0.x*inv; r.m[1] = n0.y*inv; r.m[2] = n0.z*inv;   /* col 0 */
    r.m[3] = n1.x*inv; r.m[4] = n1.y*inv; r.m[5] = n1.z*inv;   /* col 1 */
    r.m[6] = n2.x*inv; r.m[7] = n2.y*inv; r.m[8] = n2.z*inv;   /* col 2 */
    return r;
}

/* Transform an AABB: take the AABB of its 8 transformed corners. For a rotated
   box this is the (slightly loose) enclosing axis-aligned box — fine as a pick
   broad phase, and exact for axis-aligned boxes. */
Aabb aabb_transform(mat4 m, Aabb box) {
    float cx[2], cy[2], cz[2];
    Aabb  out;
    int   i;

    cx[0]=box.min.x; cx[1]=box.max.x;
    cy[0]=box.min.y; cy[1]=box.max.y;
    cz[0]=box.min.z; cz[1]=box.max.z;
    out.min = vec3_make(0.0f, 0.0f, 0.0f);   /* set on i==0 below */
    out.max = out.min;

    for (i = 0; i < 8; i++) {
        vec3 w = mat4_mul_point(m, vec3_make(cx[i & 1], cy[(i>>1) & 1], cz[(i>>2) & 1]));
        if (i == 0) {
            out.min = w;
            out.max = w;
        } else {
            if (w.x < out.min.x) out.min.x = w.x;  if (w.x > out.max.x) out.max.x = w.x;
            if (w.y < out.min.y) out.min.y = w.y;  if (w.y > out.max.y) out.max.y = w.y;
            if (w.z < out.min.z) out.min.z = w.z;  if (w.z > out.max.z) out.max.z = w.z;
        }
    }
    return out;
}

/* Ray vs AABB by the slab method: intersect the ray with each axis-aligned slab
   and keep the running overlap [tmin, tmax]. A hit needs tmin <= tmax with the
   box not entirely behind the origin. tmin starts at 0 so an origin inside the
   box reports t = 0. The vec3s are copied into float[3] so we can loop the axes
   without punning a struct as an array. */
sol_bool ray_vs_aabb(Ray ray, Aabb box, float *t_out) {
    float o[3], d[3], lo[3], hi[3];
    float tmin = 0.0f;
    float tmax = 1e30f;
    int   i;

    o[0]=ray.origin.x; o[1]=ray.origin.y; o[2]=ray.origin.z;
    d[0]=ray.dir.x;    d[1]=ray.dir.y;    d[2]=ray.dir.z;
    lo[0]=box.min.x;   lo[1]=box.min.y;   lo[2]=box.min.z;
    hi[0]=box.max.x;   hi[1]=box.max.y;   hi[2]=box.max.z;

    for (i = 0; i < 3; i++) {
        if (fabsf(d[i]) < 1e-8f) {                 /* ray parallel to this slab */
            if (o[i] < lo[i] || o[i] > hi[i]) return SOL_FALSE;   /* outside it -> miss */
        } else {
            float inv = 1.0f / d[i];
            float t1  = (lo[i] - o[i]) * inv;
            float t2  = (hi[i] - o[i]) * inv;
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }   /* enter <= exit */
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return SOL_FALSE;     /* slabs don't overlap (incl. behind) */
        }
    }
    if (t_out) *t_out = tmin;
    return SOL_TRUE;
}
