/*
fpu_types.h - Floating-point types
Copyright (C) 2025  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef LEKKIT_FPU_TYPES_H
#define LEKKIT_FPU_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if (defined(__i386__) && !defined(__SSE2_MATH__)) || defined(_M_IX86)
/*
 * i386 without SSE2 (8087 FPU) implicitly converts sNaN into qNaN.
 * FP type wrapping is needed for IEEE 754 conformance.
 */
#undef USE_SOFT_FPU_WRAP
#define USE_SOFT_FPU_WRAP 1
#endif

/*
 * NOTE: Only use if you know what you're doing
 */
typedef float  actual_float_t;
typedef double actual_double_t;

/*
 * Use fpu_f32_t instead of float
 * Use fpu_f64_t instead of double
 */

#if defined(USE_SOFT_FPU_WRAP)
typedef uint32_t fpu_f32_t;
typedef uint64_t fpu_f64_t;
#else
typedef actual_float_t  fpu_f32_t;
typedef actual_double_t fpu_f64_t;
#endif

#endif
