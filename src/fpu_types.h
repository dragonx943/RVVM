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

#include "compiler.h"

#if (defined(__i386__) && !defined(__SSE2_MATH__)) || defined(_M_IX86) || defined(__m68k__)

/*
 * i386 without SSE2 (8087 FPU) implicitly converts sNaN into qNaN, m68k has excess intermediate precision
 * FP type wrapping is needed for IEEE 754 conformance.
 */

#undef USE_SOFT_FPU_WRAP
#define USE_SOFT_FPU_WRAP 1

#endif

#if defined(USE_SOFT_FPU_WRAP) || GCC_CHECK_VER(10, 0) || CLANG_CHECK_VER(10, 0)

/*
 * Floating-point types encapsulation for compile-time type checking
 */
#undef USE_SOFT_FPU_ENCAP
#define USE_SOFT_FPU_ENCAP 1

#endif

/*
 * Native host floating-point types
 * NOTE: Only use if you know what you're doing
 */

typedef float  actual_float_t;
typedef double actual_double_t;

/*
 * Scalar floating-point storage typedefs
 */

typedef uint16_t fpu_scalar_f16_t;

#if defined(USE_SOFT_FPU_WRAP)

typedef uint32_t fpu_scalar_f32_t;
typedef uint64_t fpu_scalar_f64_t;

#else

typedef actual_float_t  fpu_scalar_f32_t;
typedef actual_double_t fpu_scalar_f64_t;

#endif

/*
 * Encapsulated floating-point typedefs
 */

#if defined(USE_SOFT_FPU_ENCAP)

typedef struct {
    fpu_scalar_f16_t scalar;
} fpu_f16_t;

typedef struct {
    fpu_scalar_f32_t scalar;
} fpu_f32_t;

typedef struct {
    fpu_scalar_f64_t scalar;
} fpu_f64_t;

#else

typedef fpu_scalar_f16_t fpu_f16_t;
typedef fpu_scalar_f32_t fpu_f32_t;
typedef fpu_scalar_f64_t fpu_f64_t;

#endif

BUILD_ASSERT(sizeof(fpu_f32_t) == sizeof(actual_float_t));
BUILD_ASSERT(sizeof(fpu_f64_t) == sizeof(actual_double_t));

#endif
