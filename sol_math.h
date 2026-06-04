#ifndef SOL_MATH_H
#define SOL_MATH_H

#include <math.h>

#include "sol_types.h"   /* vec3, vec4, mat4, quat */

#define SOL_PI 3.14159265358979323846f
static float sol_radians(float deg) { return deg * (SOL_PI / 180.0f); }

/* ---- vec3 ---- */
static vec3 vec3_make(float x, float y, float z) {
    vec3 r;
    r.x = x; r.y = y; r.z = z;
    return r;
}

static vec3 vec3_sub(vec3 a, vec3 b) {
    vec3 r;
    r.x = a.x - b.x; r.y = a.y - b.y; r.z = a.z - b.z;
    return r;
}

static float vec3_dot(vec3 a, vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static vec3 vec3_cross(vec3 a, vec3 b) {
    vec3 r;
    r.x = a.y * b.z - a.z * b.y;
    r.y = a.z * b.x - a.x * b.z;
    r.z = a.x * b.y - a.y * b.x;
    return r;
}

static vec3 vec3_normalize(vec3 v) {
    float len = sqrtf(vec3_dot(v, v));
    vec3 r;
    if (len == 0.0f) return v;
    r.x = v.x / len; r.y = v.y / len; r.z = v.z / len;
    return r;
}

/* ---- mat4 basics ---- */
static mat4 mat4_identity(void) {
    mat4 r = {0};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

/* C = A * B (column-major). Applied to a vector, B happens first, then A. */
static mat4 mat4_mul(mat4 a, mat4 b) {
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
static mat4 mat4_translate(vec3 t) {
    mat4 r = mat4_identity();
    r.m[12] = t.x;  /* translation in the last column (column-major) */
    r.m[13] = t.y;
    r.m[14] = t.z;
    return r;
}

static mat4 mat4_scale(vec3 s) {
    mat4 r = mat4_identity();
    r.m[0]  = s.x;       /* scale on the diagonal */
    r.m[5]  = s.y;
    r.m[10] = s.z;
    return r;
}

/* ---- mat4 camera + projection (standard recipes) ---- */
static mat4 mat4_look_at(vec3 eye, vec3 center, vec3 up) {
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
static mat4 mat4_perspective(float fovy, float aspect, float near, float far) {
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
static quat quat_identity(void) {
    quat q;
    q.x = 0.0f; q.y = 0.0f; q.z = 0.0f; q.w = 1.0f;   /* no rotation */
    return q;
}

/* rotation of `angle` radians about `axis`: (axis*sin(t/2), cos(t/2)) */
static quat quat_from_axis_angle(vec3 axis, float angle) {
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
static quat quat_mul(quat a, quat b) {
    quat q;
    q.w = a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z;
    q.x = a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y;
    q.y = a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x;
    q.z = a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w;
    return q;
}

static quat quat_normalize(quat q) {
    float len = sqrtf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    quat  r;
    if (len == 0.0f) return quat_identity();
    r.x = q.x / len; r.y = q.y / len; r.z = q.z / len; r.w = q.w / len;
    return r;
}

/* unit quaternion -> column-major rotation matrix */
static mat4 quat_to_mat4(quat q) {
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
static mat4 mat4_from_trs(vec3 pos, quat rot, vec3 scale) {
    mat4 t = mat4_translate(pos);
    mat4 r = quat_to_mat4(rot);
    mat4 s = mat4_scale(scale);
    return mat4_mul(t, mat4_mul(r, s));
}

#endif /* SOL_MATH_H */
