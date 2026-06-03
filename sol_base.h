/* sol_base.h — fundamental types for strict C89. Assumes 32-bit int,
   which holds on all target desktop platforms (Win/Linux/macOS). */
#ifndef SOL_BASE_H
#define SOL_BASE_H

typedef unsigned char  sol_u8;
typedef unsigned short sol_u16;
typedef unsigned int   sol_u32;
typedef int            sol_i32;
typedef float          sol_f32;
typedef double         sol_f64;

typedef int sol_bool;
#define SOL_TRUE  1
#define SOL_FALSE 0

#define SOL_INLINE /* empty: C89 has no inline */

#endif /* SOL_BASE_H */
