#ifndef SOL_TYPES_H
#define SOL_TYPES_H

/* Math data types, separated from the operations in sol_math.h so that
   modules needing only the types (e.g. scene.h) don't pull in the static
   math functions and trip -Wunused-function. */

typedef struct { float x, y, z; }    vec3;
typedef struct { float x, y, z, w; } vec4;

/* mat4: 16 floats, COLUMN-MAJOR — element (row, col) at m[col*4 + row],
   which is how OpenGL wants it, so upload is transpose=GL_FALSE. */
typedef struct { float m[16]; } mat4;

typedef struct { float x, y, z, w; } quat;   /* unit quaternion = a rotation */

typedef struct { vec3 origin, dir; } Ray;    /* dir kept unit in practice */
typedef struct { vec3 min, max;    } Aabb;   /* axis-aligned bounding box */

#endif /* SOL_TYPES_H */
