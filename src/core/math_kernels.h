/* The operations of badger/math.h that run on many floats at once, and have a
 * SIMD form. The build compiles one file that defines the tb_math_kernel_
 * functions, for the processor it targets: math_sse2.c on x64, math_neon.c on
 * ARM, and math_kernel_portable.c anywhere else. The portable forms are always
 * built, and every form adds and multiplies in the same order, so they agree
 * bit for bit; the tests hold them to that. */
#ifndef TB_MATH_KERNELS_H
#define TB_MATH_KERNELS_H

#include "badger/math.h"

#include <stddef.h>

/* What the build chose: "sse2", "neon" or "portable". */
extern const char tb_math_kernel_name[];

/* m * vectors[i] into results[i]; results may be vectors. */
void tb_math_kernel_transform_many(const tb_mat4 *m, const tb_vec4 *vectors, tb_vec4 *results,
                                   size_t count);

/* m * (points[i], 1), without the w component, into results[i]; results may
 * be points. */
void tb_math_kernel_transform_points(const tb_mat4 *m, const tb_vec3 *points, tb_vec3 *results,
                                     size_t count);

/* The same, in portable C. */
void tb_math_portable_transform_many(const tb_mat4 *m, const tb_vec4 *vectors, tb_vec4 *results,
                                     size_t count);
void tb_math_portable_transform_points(const tb_mat4 *m, const tb_vec3 *points, tb_vec3 *results,
                                       size_t count);

#endif
