#ifndef SOL_MATH_H
#define SOL_MATH_H

#include <math.h>

#define SOL_PI 3.14159265358979323846f
static inline float sol_radians(float deg) { return deg * (SOL_PI / 180.0f); }

/* ---- types ---- */
typedef struct { float x, y, z; }    vec3;
typedef struct { float x, y, z, w; } vec4;

/* mat4: 16 floats, COLUMN-MAJOR — element (row, col) lives at m[col*4 + row],
   which is exactly how OpenGL wants it, so upload is transpose=GL_FALSE. */
typedef struct { float m[16]; } mat4;

/* ---- vec3 ---- */
static inline vec3 vec3_make(float x, float y, float z) { return (vec3){x, y, z}; }

static inline vec3 vec3_sub(vec3 a, vec3 b) {
    return (vec3){a.x - b.x, a.y - b.y, a.z - b.z};
}

static inline float vec3_dot(vec3 a, vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline vec3 vec3_cross(vec3 a, vec3 b) {
    return (vec3){
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

static inline vec3 vec3_normalize(vec3 v) {
    float len = sqrtf(vec3_dot(v, v));
    if (len == 0.0f) return v;
    return (vec3){v.x / len, v.y / len, v.z / len};
}

/* ---- mat4 basics ---- */
static inline mat4 mat4_identity(void) {
    mat4 r = {0};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

/* C = A * B (column-major). Applied to a vector, B happens first, then A. */
static inline mat4 mat4_mul(mat4 a, mat4 b) {
    mat4 r;
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += a.m[k * 4 + row] * b.m[col * 4 + k];
            }
            r.m[col * 4 + row] = sum;
        }
    }
    return r;
}

/* ---- mat4 model transforms ---- */
static inline mat4 mat4_translate(vec3 t) {
    mat4 r = mat4_identity();
    r.m[12] = t.x;  /* translation sits in the last COLUMN (column-major) */
    r.m[13] = t.y;
    r.m[14] = t.z;
    return r;
}

static inline mat4 mat4_rotate_x(float a) {
    float c = cosf(a), s = sinf(a);
    mat4 r = mat4_identity();
    r.m[5] = c;  r.m[9]  = -s;
    r.m[6] = s;  r.m[10] =  c;
    return r;
}

static inline mat4 mat4_rotate_y(float a) {
    float c = cosf(a), s = sinf(a);
    mat4 r = mat4_identity();
    r.m[0] = c;  r.m[8]  =  s;
    r.m[2] = -s; r.m[10] =  c;
    return r;
}

/* ---- mat4 camera + projection (standard recipes) ---- */
static inline mat4 mat4_look_at(vec3 eye, vec3 center, vec3 up) {
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
static inline mat4 mat4_perspective(float fovy, float aspect, float near, float far) {
    float f = 1.0f / tanf(fovy * 0.5f);
    mat4 r = {0};
    r.m[0]  = f / aspect;                      /* aspect correction lives HERE */
    r.m[5]  = f;
    r.m[10] = (far + near) / (near - far);
    r.m[11] = -1.0f;                           /* feeds -z into w -> perspective */
    r.m[14] = (2.0f * far * near) / (near - far);
    return r;
}

#endif /* SOL_MATH_H */
