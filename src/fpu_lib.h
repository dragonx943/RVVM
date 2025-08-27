/*
fpu_lib.h - Floating-point handling library
Copyright (C) 2024  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef LEKKIT_FPU_LIB_H
#define LEKKIT_FPU_LIB_H

#include "fpu_types.h"
#include "mem_ops.h"

/*
 * NOTE: Floating-point is actually subtly (or seriously) broken on many compilers and platforms
 *
 * Signaling NaNs are silently converted to quit NaNs on 8087 FPU (Legacy x86 FPU before SSE2):
 * - The conversion happens upon storing a 8087 register into memory, together with other transformations
 * - This is possibly the most convoluted issue to fix, and a major reason this library exists at all
 * Solution: On i386, fpu_fp32_t/fpu_fp64_t typedefs are basically ingegers, and only upwrapped
 *   to a floating-point representation during an actual FP operation, thus preventing messing with
 *   the floating-point representation. sNaN checks are implemented using bit manipulations anyways.
 * The workaround is enabled on any non-SSE2 i386, and allows IEEE 754 conformance.
 *
 * Signaling comparisons (<, <=) may be miscompiled:
 * - GCC <8.1 is generating ucomiss instead of comiss on x86 SSE2
 * - Same for Clang <12.0, or without #pragma STDC FENV_ACCESS ON / -frounding-math
 * - Why the fuck is Clang not IEEE 754 compliant by default???
 * - GCC/Clang generate quiet (unordered) compares on ARM32, PowerPC
 * Solution: Manually raise FE_INVALID if neither a < b nor b <= a
 *
 * Quiet comparisons (__builtin_isless) may be miscompiled or inefficient:
 * - GCC/Clang targeting RISC-V generate signaling comparisons
 * Solution: Manually check fpu_is_nan() and then perform <, <= check
 *
 * Converting floats with 64-bit integers may be broken in various ways:
 * - ARM32 fails various fp->i64/u64 fcvt tests
 * - TCC fails to set FE_INEXACT on fp->i64/u64 - probably uses a software implementation
 * - Chibicc fails various i64/u64->fp conversions
 * Solution: Check whether fp fits into i32/u32 instead, with fallback to i64/u64
 *
 * Various libm calls (sqrt, fma, etc) may ignore FE_INEXACT/FE_INVALID semantics, etc:
 * - Windows libm seems to not set FE_INVALID on sqrt(-1), even on MinGW
 * Solution: Raise FE_INVALID manually for invalid operations.
 *
 * WASM, mips/mips64, m68k, Windows CE (ARM32) platforms lack FENV completely:
 * - Basically what the title says. None of those have a working <fenv.h> implementation.
 * - There is no way to implement fesetround() anyways, at least as far as I'm aware
 * Solution: Thread-local exception flags, which are raised manually based on various checks.
 *
 * NOTE: RISC-V THead CPUs also lack FPU exceptions, but I'm hesitant to enable such workaround
 * on RISC-V in general as this case is a single non-conforming hardware implementation.
 *
 * This list is not even considering various historical compiler versions, and MSVC. To be continued?...
 *
 * Because of this, and because most workarounds have little performance impact, consider limited
 * list of compilers/platforms a "Known Nice Platform" (C) and apply needed workarounds otherwise.
 */

#if defined(__wasm__) || defined(__mips__) || defined(__mips) || defined(__mips64__) || defined(__mips64) /**/         \
    || defined(__m68k__) || (defined(_WIN32) && defined(_M_ARM)) || !CHECK_INCLUDE(fenv.h, 1)
// Enable FENV emulation
#undef USE_SOFT_FPU_FENV
#define USE_SOFT_FPU_FENV 1
#endif

// Fix various floating-point misoptimizations (Duh)
#if CLANG_CHECK_VER(12, 0)
#pragma float_control(precise)
#pragma STDC FENV_ACCESS ON
#elif defined(_MSC_VER)
#pragma fenv_access(on)
#endif

// GCC 8.1+ and Clang 12.0+ generate mostly correct FP code, at least for cases checked here
#undef FPU_LIB_KNOWN_SANE_COMPILER
#if !defined(USE_FPU_WORKAROUNDS) && (GCC_CHECK_VER(8, 1) || CLANG_CHECK_VER(12, 0))
#define FPU_LIB_KNOWN_SANE_COMPILER 1
#endif

// Conversion long->fp is correct on modern GCC/Clang
#undef FPU_LIB_CORRECT_CONVERSION_LONG_FP
#if defined(FPU_LIB_KNOWN_SANE_COMPILER)
#define FPU_LIB_CORRECT_CONVERSION_LONG_FP 1
#endif

// Conversion fp->long is correct on modern GCC/Clang targeting anything 64-bit, and i386
#undef FPU_LIB_CORRECT_CONVERSION_FP_LONG
#if defined(FPU_LIB_KNOWN_SANE_COMPILER) && (defined(__i386__) || defined(HOST_64BIT))
#define FPU_LIB_CORRECT_CONVERSION_FP_LONG 1
#endif

// Compare via <, >=  is signaling on NaN on modern GCC/Clang targeting i386, x86_64, arm64, riscv
#undef FPU_LIB_CONFORMING_SIGNALING_COMPARE
#if !defined(USE_SOFT_FPU_FENV) && defined(FPU_LIB_KNOWN_SANE_COMPILER) /**/                                           \
    && (defined(__i386__) || defined(__x86_64__) || defined(__aarch64__) || defined(__riscv))
#define FPU_LIB_CONFORMING_SIGNALING_COMPARE 1
#endif

// Compare via __builtin_isless() is quiet on modern GCC/Clang targeting i386, x86_64, arm, arm64, ppc, ppc64
#undef FPU_LIB_CONFORMING_BUILTIN_QUIET_COMPARE
#if !defined(USE_SOFT_FPU_FENV) && defined(FPU_LIB_KNOWN_SANE_COMPILER)    /**/                                        \
    && (defined(__i386__) || defined(__x86_64__)                           /**/                                        \
        || defined(__arm__) || defined(__aarch64__)                        /**/                                        \
        || defined(__powerpc__) || defined(__powerpc64__))                 /**/                                        \
    && GNU_BUILTIN(__builtin_isless) && GNU_BUILTIN(__builtin_islessequal) /**/                                        \
    && GNU_BUILTIN(__builtin_isgreater) && GNU_BUILTIN(__builtin_isgreaterequal)
#define FPU_LIB_CONFORMING_BUILTIN_QUIET_COMPARE 1
#endif

// Min/Max via __builtin_fmin() conforms to IEEE 754-2008 on riscv
#undef FPU_LIB_CONFORMING_BUILTIN_IEEE754_2008_FMINMAX
#if defined(FPU_LIB_KNOWN_SANE_COMPILER) && defined(__riscv_f) && defined(__riscv_d) /**/                              \
    && GNU_BUILTIN(__builtin_fminf) && GNU_BUILTIN(__builtin_fmin)                   /**/                              \
    && GNU_BUILTIN(__builtin_fmaxf) && GNU_BUILTIN(__builtin_fmax)
#define FPU_LIB_CONFORMING_BUILTIN_IEEE754_2008_FMINMAX 1
#endif

// Copying sign via __builtin_copysign() is observably more optimal on x86_64, arm64, riscv
#undef FPU_LIB_OPTIMAL_BUILTIN_COPYSIGN
#if !defined(USE_SOFT_FPU_WRAP) && defined(FPU_LIB_KNOWN_SANE_COMPILER)  /**/                                          \
    && (defined(__x86_64__) || defined(__aarch64__) || defined(__riscv)) /**/                                          \
    && GNU_BUILTIN(__builtin_copysign) && GNU_BUILTIN(__builtin_copysignf)
#define FPU_LIB_OPTIMAL_BUILTIN_COPYSIGN 1
#endif

/*
 * Hide <math.h> definitions from generic code if possible - it should not be used
 */
#if GNU_BUILTIN(__builtin_sqrt) && GNU_BUILTIN(__builtin_sqrtf)
#define fpu_sqrt_internal(d)  __builtin_sqrt(d)
#define fpu_sqrtf_internal(f) __builtin_sqrtf(f)
#else
#include <math.h>
#define fpu_sqrt_internal(d)  sqrt(d)
#define fpu_sqrtf_internal(f) sqrtf(f)
#endif

/*
 * FPU Environment handling
 *
 * NOTE: Those definitions match RISC-V bitfields :D
 */

// FPU Exception flags
#define FPU_LIB_FLAG_NX         0x01 // Inexact result
#define FPU_LIB_FLAG_UF         0x02 // Underflow
#define FPU_LIB_FLAG_OF         0x04 // Overflow
#define FPU_LIB_FLAG_DZ         0x08 // Division by zero
#define FPU_LIB_FLAG_NV         0x10 // Invalid operation

#define FPU_ENV_FLAGS_ALL       0x1F

// FPU Rounding modes
#define FPU_LIB_ROUND_NE        0x00 // Round to nearest
#define FPU_LIB_ROUND_TZ        0x01 // Round to zero
#define FPU_LIB_ROUND_DN        0x02 // Round down (Towards negative infinity)
#define FPU_LIB_ROUND_UP        0x03 // Round up (Towards positive infinity)
#define FPU_LIB_ROUND_MM        0x04 // Round to nearest, ties to Max Magnitude

// FPU Classification
#define FPU_CLASS_NEG_INF       0x00 // Negative infinity
#define FPU_CLASS_NEG_NORMAL    0x01 // Negative normal
#define FPU_CLASS_NEG_SUBNORMAL 0x02 // Negative subnormal
#define FPU_CLASS_NEG_ZERO      0x03 // Negative zero
#define FPU_CLASS_POS_ZERO      0x04 // Positive zero
#define FPU_CLASS_POS_SUBNORMAL 0x05 // Positive subnormal
#define FPU_CLASS_POS_NORMAL    0x06 // Positive normal
#define FPU_CLASS_POS_INF       0x07 // Positive infinity
#define FPU_CLASS_NAN_SIG       0x08 // Signaling NaN
#define FPU_CLASS_NAN_QUIET     0x09 // Quitet NaN

// Exception handling
uint32_t fpu_get_exceptions(void);
void     fpu_set_exceptions(uint32_t new_exceptions);
void     fpu_raise_exceptions(uint32_t set_exceptions);
void     fpu_clear_exceptions(uint32_t clr_exceptions);

// Rounding mode handling
uint32_t fpu_get_rounding_mode(void);
void     fpu_set_rounding_mode(uint32_t mode);

// Floating-point Classification
slow_path uint32_t fpu_fclass32(fpu_f32_t f);
slow_path uint32_t fpu_fclass64(fpu_f64_t d);

// Internal rounding helpers (Only to be coverted to an integer afterwards)
slow_path fpu_f32_t fpu_round_f32_internal(fpu_f32_t f, uint32_t mode);
slow_path fpu_f64_t fpu_round_f64_internal(fpu_f64_t d, uint32_t mode);

// Unintrusive soft exception handling for inlined paths
slow_path void fpu_raise_inexact(void);
slow_path void fpu_raise_invalid(void);
slow_path void fpu_raise_ovrflow(void);
slow_path void fpu_raise_divzero(void);

/*
 * Floating-point bitcasts
 */

static forceinline uint32_t fpu_bit_f32_to_u32(fpu_f32_t f)
{
#if defined(USE_SOFT_FPU_WRAP) && defined(USE_SOFT_FPU_ENCAP)
    return f.scalar;
#elif defined(USE_SOFT_FPU_WRAP)
    return f;
#else
    uint32_t u;
    memcpy(&u, &f, sizeof(f));
    return u;
#endif
}

static forceinline fpu_f32_t fpu_bit_u32_to_f32(uint32_t u)
{
#if defined(USE_SOFT_FPU_WRAP) && defined(USE_SOFT_FPU_ENCAP)
    fpu_f32_t ret = {.scalar = u};
    return ret;
#elif defined(USE_SOFT_FPU_WRAP)
    return u;
#else
    fpu_f32_t f;
    memcpy(&f, &u, sizeof(f));
    return f;
#endif
}

static forceinline uint64_t fpu_bit_f64_to_u64(fpu_f64_t d)
{
#if defined(USE_SOFT_FPU_WRAP) && defined(USE_SOFT_FPU_ENCAP)
    return d.scalar;
#elif defined(USE_SOFT_FPU_WRAP)
    return d;
#else
    uint64_t u;
    memcpy(&u, &d, sizeof(d));
    return u;
#endif
}

static forceinline fpu_f64_t fpu_bit_u64_to_f64(uint64_t u)
{
#if defined(USE_SOFT_FPU_WRAP) && defined(USE_SOFT_FPU_ENCAP)
    fpu_f64_t ret = {.scalar = u};
    return ret;
#elif defined(USE_SOFT_FPU_WRAP)
    return u;
#else
    fpu_f64_t d;
    memcpy(&d, &u, sizeof(d));
    return d;
#endif
}

/*
 * Software wrapping of scalar floating-point
 */

static forceinline actual_float_t fpu_raw_f32(fpu_f32_t f)
{
#if defined(USE_SOFT_FPU_WRAP)
    actual_float_t ret;
    memcpy(&ret, &f, sizeof(f));
    return ret;
#elif defined(USE_SOFT_FPU_ENCAP)
    return f.scalar;
#else
    return f;
#endif
}

static forceinline actual_double_t fpu_raw_f64(fpu_f64_t d)
{
#if defined(USE_SOFT_FPU_WRAP)
    actual_double_t ret;
    memcpy(&ret, &d, sizeof(d));
    return ret;
#elif defined(USE_SOFT_FPU_ENCAP)
    return d.scalar;
#else
    return d;
#endif
}

static forceinline fpu_f32_t fpu_wrap_f32(actual_float_t f)
{
#if defined(USE_SOFT_FPU_WRAP)
    fpu_f32_t ret;
    memcpy(&ret, &f, sizeof(f));
    return ret;
#elif defined(USE_SOFT_FPU_ENCAP)
    fpu_f32_t ret = {.scalar = f};
    return ret;
#else
    return f;
#endif
}

static forceinline fpu_f64_t fpu_wrap_f64(actual_double_t d)
{
#if defined(USE_SOFT_FPU_WRAP)
    fpu_f64_t ret;
    memcpy(&ret, &d, sizeof(d));
    return ret;
#elif defined(USE_SOFT_FPU_ENCAP)
    fpu_f64_t ret = {.scalar = d};
    return ret;
#else
    return d;
#endif
}

/*
 * Fast software floating-point helpers
 */

#define FPU_LIB_FP32_SIGNEDFP_MASK 0x80000000U
#define FPU_LIB_FP32_NOSIGNED_MASK 0x7FFFFFFFU
#define FPU_LIB_FP32_EXPONENT_MASK 0x7F800000U
#define FPU_LIB_FP32_MANTISSA_MASK 0x007FFFFFU
#define FPU_LIB_FP32_QUIETNAN_MASK 0x00400000U

#define FPU_LIB_FP32_CANONICAL_NAN 0x7FC00000U
#define FPU_LIB_FP32_POSITIVE_INF  0x7F800000U
#define FPU_LIB_FP32_NEGATIVE_INF  0xFF800000U

#define FPU_LIB_FP64_SIGNEDFP_MASK 0x8000000000000000ULL
#define FPU_LIB_FP64_NOSIGNED_MASK 0x7FFFFFFFFFFFFFFFULL
#define FPU_LIB_FP64_EXPONENT_MASK 0x7FF0000000000000ULL
#define FPU_LIB_FP64_MANTISSA_MASK 0x000FFFFFFFFFFFFFULL
#define FPU_LIB_FP64_QUIETNAN_MASK 0x0008000000000000ULL

#define FPU_LIB_FP64_CANONICAL_NAN 0x7FF8000000000000ULL
#define FPU_LIB_FP64_POSITIVE_INF  0x7FF0000000000000ULL
#define FPU_LIB_FP64_NEGATIVE_INF  0xFFF0000000000000ULL

// Checks for finite non-NaN, non-Inf input, never sets exceptions
static forceinline bool fpu_is_finite32(fpu_f32_t f)
{
    return (fpu_bit_f32_to_u32(f) & FPU_LIB_FP32_NOSIGNED_MASK) < FPU_LIB_FP32_EXPONENT_MASK;
}

static forceinline bool fpu_is_finite64(fpu_f64_t d)
{
    return (fpu_bit_f64_to_u64(d) & FPU_LIB_FP64_NOSIGNED_MASK) < FPU_LIB_FP64_EXPONENT_MASK;
}

// Checks for positive, possibly infinite non-NaN input, never sets exceptions
static forceinline bool fpu_is_positive32(fpu_f32_t f)
{
    return fpu_bit_f32_to_u32(f) <= FPU_LIB_FP32_POSITIVE_INF;
}

static forceinline bool fpu_is_positive64(fpu_f64_t d)
{
    return fpu_bit_f64_to_u64(d) <= FPU_LIB_FP64_POSITIVE_INF;
}

// Checks for negative, possibly infinite non-NaN input, never sets exceptions
static forceinline bool fpu_is_negative32(fpu_f32_t f)
{
    int32_t i = (int32_t)fpu_bit_f32_to_u32(f);
    return i <= (int32_t)FPU_LIB_FP32_NEGATIVE_INF;
}

static forceinline bool fpu_is_negative64(fpu_f64_t d)
{
    int64_t i = (int64_t)fpu_bit_f64_to_u64(d);
    return i <= (int64_t)FPU_LIB_FP64_NEGATIVE_INF;
}

// Checks for NaN input, never sets exceptions
static forceinline bool fpu_is_nan32_soft(fpu_f32_t f)
{
    return (fpu_bit_f32_to_u32(f) & FPU_LIB_FP32_NOSIGNED_MASK) > FPU_LIB_FP32_POSITIVE_INF;
}

static forceinline bool fpu_is_nan64_soft(fpu_f64_t d)
{
    return (fpu_bit_f64_to_u64(d) & FPU_LIB_FP64_NOSIGNED_MASK) > FPU_LIB_FP64_POSITIVE_INF;
}

// Checks for sNaN input, never sets exceptions
static forceinline bool fpu_is_snan32_soft(fpu_f32_t f)
{
    uint32_t u = fpu_bit_f32_to_u32(f) & FPU_LIB_FP32_NOSIGNED_MASK;
    return u > FPU_LIB_FP32_POSITIVE_INF && u < FPU_LIB_FP32_CANONICAL_NAN;
}

static forceinline bool fpu_is_snan64_soft(fpu_f64_t d)
{
    uint64_t u = fpu_bit_f64_to_u64(d) & FPU_LIB_FP64_NOSIGNED_MASK;
    return u > FPU_LIB_FP64_POSITIVE_INF && u < FPU_LIB_FP64_CANONICAL_NAN;
}

// Returns normalized exponent value
static forceinline int32_t fpu_exponent32(fpu_f32_t f)
{
    return ((int32_t)((fpu_bit_f32_to_u32(f) >> 23) & 0xFF)) - 0x7F;
}

static forceinline int32_t fpu_exponent64(fpu_f64_t f)
{
    return ((int32_t)((fpu_bit_f64_to_u64(f) >> 52) & 0x7FF)) - 0x3FF;
}

// Returns true on non-zero fractional part
static forceinline bool fpu_is_fractional32(fpu_f32_t f)
{
    uint32_t u = fpu_bit_f32_to_u32(f);
    if (u << 1) {
        int32_t e = fpu_exponent32(f);
        if (e < 0) {
            return true;
        } else if (e < 23) {
            return !!(u & (FPU_LIB_FP32_MANTISSA_MASK >> e));
        }
    }
    return false;
}

static forceinline bool fpu_is_fractional64(fpu_f64_t d)
{
    uint64_t u = fpu_bit_f64_to_u64(d);
    if (u << 1) {
        int32_t e = fpu_exponent64(d);
        if (e < 0) {
            return true;
        } else if (e < 52) {
            return !!(u & (FPU_LIB_FP64_MANTISSA_MASK >> e));
        }
    }
    return false;
}

/*
 * Software floating-point exception handling helpers
 *
 * NOTE: This doesn't set underflow flag, as if denormals are zero, but this is fine
 */

static forceinline void fpu_soft_fenv_copium32(fpu_f32_t f)
{
    UNUSED(f);
#if defined(USE_SOFT_FPU_FENV)
    if (unlikely(fpu_is_fractional32(f))) {
        fpu_raise_inexact();
    }
#endif
}

static forceinline void fpu_soft_fenv_copium64(fpu_f64_t d)
{
    UNUSED(d);
#if defined(USE_SOFT_FPU_FENV)
    if (unlikely(fpu_is_fractional64(d))) {
        fpu_raise_inexact();
    }
#endif
}

static forceinline void fpu_soft_fenv_alu_copium32(fpu_f32_t res, fpu_f32_t a, fpu_f32_t b, bool is_div)
{
    UNUSED(is_div);
    UNUSED(res);
    UNUSED(a);
    UNUSED(b);
#if defined(USE_SOFT_FPU_FENV)
    if (unlikely(!fpu_is_finite32(res))) {
        if (fpu_raw_f32(res) != fpu_raw_f32(res)) {
            fpu_raise_invalid();
        } else if (is_div) {
            fpu_raise_divzero();
        } else {
            fpu_raise_ovrflow();
        }
    } else if (unlikely((is_div || fpu_is_fractional32(a)) && fpu_is_fractional32(b))) {
        fpu_raise_inexact();
    }
#endif
}

static forceinline void fpu_soft_fenv_alu_copium64(fpu_f64_t res, fpu_f64_t a, fpu_f64_t b, bool is_div)
{
    UNUSED(is_div);
    UNUSED(res);
    UNUSED(a);
    UNUSED(b);
#if defined(USE_SOFT_FPU_FENV)
    if (unlikely(!fpu_is_finite64(res))) {
        if (fpu_raw_f64(res) != fpu_raw_f64(res)) {
            fpu_raise_invalid();
        } else if (is_div) {
            fpu_raise_divzero();
        } else {
            fpu_raise_ovrflow();
        }
    } else if (unlikely((is_div || fpu_is_fractional64(a)) && fpu_is_fractional64(b))) {
        fpu_raise_inexact();
    }
#endif
}

/*
 * NaN-boxing, explicit endian load/store intrinsics
 */

static forceinline fpu_f64_t fpu_nanbox_f32(fpu_f32_t f)
{
    return fpu_bit_u64_to_f64(fpu_bit_f32_to_u32(f) | 0xFFFFFFFF00000000ULL);
}

static forceinline fpu_f32_t fpu_unpack_f32_from_f64(fpu_f64_t d)
{
    return fpu_bit_u32_to_f32(fpu_bit_f64_to_u64(d));
    ;
}

static forceinline fpu_f32_t fpu_nan_unbox_f32(fpu_f64_t d)
{
    uint64_t u = fpu_bit_f64_to_u64(d);
    if (likely(((int32_t)(u >> 32)) == -1)) {
        return fpu_bit_u32_to_f32(u);
    }
    return fpu_bit_u32_to_f32(FPU_LIB_FP32_CANONICAL_NAN);
}

TSAN_SUPPRESS static forceinline fpu_f32_t fpu_load_f32_le(const void* addr)
{
#if defined(HOST_LITTLE_ENDIAN)
    return *(const safe_aliasing fpu_f32_t*)addr;
#else
    return fpu_bit_u32_to_f32(read_uint32_le(addr));
#endif
}

TSAN_SUPPRESS static forceinline void fpu_store_f32_le(void* addr, fpu_f32_t f)
{
#if defined(HOST_LITTLE_ENDIAN)
    *(safe_aliasing fpu_f32_t*)addr = f;
#else
    write_uint32_le(addr, fpu_bit_f32_to_u32(f));
#endif
}

TSAN_SUPPRESS static forceinline fpu_f64_t fpu_load_f64_le(const void* addr)
{
#if defined(HOST_LITTLE_ENDIAN)
    return *(const safe_aliasing fpu_f64_t*)addr;
#else
    return fpu_bit_u64_to_f64(read_uint64_le(addr));
#endif
}

TSAN_SUPPRESS static forceinline void fpu_store_f64_le(void* addr, fpu_f64_t d)
{
#if defined(HOST_LITTLE_ENDIAN)
    *(safe_aliasing fpu_f64_t*)addr = d;
#else
    write_uint64_le(addr, fpu_bit_f64_to_u64(d));
#endif
}

/*
 * NaN, sNaN checking
 */

static forceinline bool fpu_is_equal32_quiet(fpu_f32_t a, fpu_f32_t b)
{
#if defined(USE_SOFT_FPU_FENV)
    if (fpu_raw_f32(a) == fpu_raw_f32(b)) {
        return true;
    }
    if (fpu_is_snan32_soft(a) || fpu_is_snan32_soft(b)) {
        fpu_raise_invalid();
    }
    return false;
#else
    return fpu_raw_f32(a) == fpu_raw_f32(b);
#endif
}

static forceinline bool fpu_is_equal64_quiet(fpu_f64_t a, fpu_f64_t b)
{
#if defined(USE_SOFT_FPU_FENV)
    if (fpu_raw_f64(a) == fpu_raw_f64(b)) {
        return true;
    }
    if (fpu_is_snan64_soft(a) || fpu_is_snan64_soft(b)) {
        fpu_raise_invalid();
    }
    return false;
#else
    return fpu_raw_f64(a) == fpu_raw_f64(b);
#endif
}

// Those functions raise FE_INVALID on sNaN, and that is commonly needed
static forceinline bool fpu_is_nan32(fpu_f32_t f)
{
    return !fpu_is_equal32_quiet(f, f);
}

static forceinline bool fpu_is_nan64(fpu_f64_t d)
{
    return !fpu_is_equal64_quiet(d, d);
}

/*
 * Floating-point Arithmetic
 */

static forceinline fpu_f32_t fpu_add32(fpu_f32_t a, fpu_f32_t b)
{
    fpu_f32_t ret = fpu_wrap_f32(fpu_raw_f32(a) + fpu_raw_f32(b));
    fpu_soft_fenv_alu_copium32(ret, a, b, false);
    return ret;
}

static forceinline fpu_f64_t fpu_add64(fpu_f64_t a, fpu_f64_t b)
{
    fpu_f64_t ret = fpu_wrap_f64(fpu_raw_f64(a) + fpu_raw_f64(b));
    fpu_soft_fenv_alu_copium64(ret, a, b, false);
    return ret;
}

static forceinline fpu_f32_t fpu_sub32(fpu_f32_t a, fpu_f32_t b)
{
    fpu_f32_t ret = fpu_wrap_f32(fpu_raw_f32(a) - fpu_raw_f32(b));
    fpu_soft_fenv_alu_copium32(ret, a, b, false);
    return ret;
}

static forceinline fpu_f64_t fpu_sub64(fpu_f64_t a, fpu_f64_t b)
{
    fpu_f64_t ret = fpu_wrap_f64(fpu_raw_f64(a) - fpu_raw_f64(b));
    fpu_soft_fenv_alu_copium64(ret, a, b, false);
    return ret;
}

static forceinline fpu_f32_t fpu_mul32(fpu_f32_t a, fpu_f32_t b)
{
    fpu_f32_t ret = fpu_wrap_f32(fpu_raw_f32(a) * fpu_raw_f32(b));
    fpu_soft_fenv_alu_copium32(ret, a, b, false);
    return ret;
}

static forceinline fpu_f64_t fpu_mul64(fpu_f64_t a, fpu_f64_t b)
{
    fpu_f64_t ret = fpu_wrap_f64(fpu_raw_f64(a) * fpu_raw_f64(b));
    fpu_soft_fenv_alu_copium64(ret, a, b, false);
    return ret;
}

static forceinline fpu_f32_t fpu_div32(fpu_f32_t a, fpu_f32_t b)
{
    fpu_f32_t ret = fpu_wrap_f32(fpu_raw_f32(a) / fpu_raw_f32(b));
    fpu_soft_fenv_alu_copium32(ret, a, b, true);
    return ret;
}

static forceinline fpu_f64_t fpu_div64(fpu_f64_t a, fpu_f64_t b)
{
    fpu_f64_t ret = fpu_wrap_f64(fpu_raw_f64(a) / fpu_raw_f64(b));
    fpu_soft_fenv_alu_copium64(ret, a, b, true);
    return ret;
}

static forceinline fpu_f32_t fpu_fma32(fpu_f32_t a, fpu_f32_t b, fpu_f32_t c)
{
    return fpu_add32(fpu_mul32(a, b), c);
}

static forceinline fpu_f64_t fpu_fma64(fpu_f64_t a, fpu_f64_t b, fpu_f64_t c)
{
    return fpu_add64(fpu_mul64(a, b), c);
}

static forceinline fpu_f32_t fpu_sqrt32(fpu_f32_t f)
{
    fpu_f32_t ret = fpu_wrap_f32(fpu_sqrtf_internal(fpu_raw_f32(f)));
    if (likely(fpu_is_positive32(f))) {
        fpu_soft_fenv_copium32(ret);
        return ret;
    }
    // Windows libm doesn't raise FE_INVALID on sqrt(<0)
    fpu_raise_invalid();
    return ret;
}

static forceinline fpu_f64_t fpu_sqrt64(fpu_f64_t d)
{
    fpu_f64_t ret = fpu_wrap_f64(fpu_sqrt_internal(fpu_raw_f64(d)));
    if (likely(fpu_is_positive64(d))) {
        fpu_soft_fenv_copium64(ret);
        return ret;
    }
    // Windows libm doesn't raise FE_INVALID on sqrt(<0)
    fpu_raise_invalid();
    return ret;
}

/*
 * Floating-point sign operations
 */

static forceinline bool fpu_signbit32(fpu_f32_t f)
{
    return !!(fpu_bit_f32_to_u32(f) >> 31);
}

static forceinline bool fpu_signbit64(fpu_f64_t d)
{
    return !!(fpu_bit_f64_to_u64(d) >> 63);
}

static forceinline fpu_f32_t fpu_neg32(fpu_f32_t f)
{
#if defined(USE_SOFT_FPU_WRAP)
    return fpu_bit_u32_to_f32(fpu_bit_f32_to_u32(f) ^ FPU_LIB_FP32_SIGNEDFP_MASK);
#else
    return fpu_wrap_f32(-fpu_raw_f32(f));
#endif
}

static forceinline fpu_f64_t fpu_neg64(fpu_f64_t d)
{
#if defined(USE_SOFT_FPU_WRAP)
    return fpu_bit_u64_to_f64(fpu_bit_f64_to_u64(d) ^ FPU_LIB_FP64_SIGNEDFP_MASK);
#else
    return fpu_wrap_f64(-fpu_raw_f64(d));
#endif
}

static forceinline fpu_f32_t fpu_fsgnj32(fpu_f32_t a, fpu_f32_t b)
{
#if defined(FPU_LIB_OPTIMAL_BUILTIN_COPYSIGN)
    return fpu_wrap_f32(__builtin_copysignf(fpu_raw_f32(a), fpu_raw_f32(b)));
#else
    uint32_t ua = fpu_bit_f32_to_u32(a);
    uint32_t ub = fpu_bit_f32_to_u32(b);
    return fpu_bit_u32_to_f32(ua ^ ((ua ^ ub) & FPU_LIB_FP32_SIGNEDFP_MASK));
#endif
}

static forceinline fpu_f64_t fpu_fsgnj64(fpu_f64_t a, fpu_f64_t b)
{
#if defined(FPU_LIB_OPTIMAL_BUILTIN_COPYSIGN)
    return fpu_wrap_f64(__builtin_copysign(fpu_raw_f64(a), fpu_raw_f64(b)));
#else
    uint64_t ua = fpu_bit_f64_to_u64(a);
    uint64_t ub = fpu_bit_f64_to_u64(b);
    return fpu_bit_u64_to_f64(ua ^ ((ua ^ ub) & FPU_LIB_FP64_SIGNEDFP_MASK));
#endif
}

static forceinline fpu_f32_t fpu_fsgnjn32(fpu_f32_t a, fpu_f32_t b)
{
#if defined(FPU_LIB_OPTIMAL_BUILTIN_COPYSIGN)
    return fpu_wrap_f32(__builtin_copysignf(fpu_raw_f32(a), -fpu_raw_f32(b)));
#else
    uint32_t ua = fpu_bit_f32_to_u32(a);
    uint32_t ub = fpu_bit_f32_to_u32(b);
    return fpu_bit_u32_to_f32(ua ^ (~(ua ^ ub) & FPU_LIB_FP32_SIGNEDFP_MASK));
#endif
}

static forceinline fpu_f64_t fpu_fsgnjn64(fpu_f64_t a, fpu_f64_t b)
{
#if defined(FPU_LIB_OPTIMAL_BUILTIN_COPYSIGN)
    return fpu_wrap_f64(__builtin_copysign(fpu_raw_f64(a), -fpu_raw_f64(b)));
#else
    uint64_t ua = fpu_bit_f64_to_u64(a);
    uint64_t ub = fpu_bit_f64_to_u64(b);
    return fpu_bit_u64_to_f64(ua ^ (~(ua ^ ub) & FPU_LIB_FP64_SIGNEDFP_MASK));
#endif
}

static forceinline fpu_f32_t fpu_fsgnjx32(fpu_f32_t a, fpu_f32_t b)
{
    uint32_t ua = fpu_bit_f32_to_u32(a);
    uint32_t ub = fpu_bit_f32_to_u32(b);
    return fpu_bit_u32_to_f32(ua ^ (ub & FPU_LIB_FP32_SIGNEDFP_MASK));
}

static forceinline fpu_f64_t fpu_fsgnjx64(fpu_f64_t a, fpu_f64_t b)
{
    uint64_t ua = fpu_bit_f64_to_u64(a);
    uint64_t ub = fpu_bit_f64_to_u64(b);
    return fpu_bit_u64_to_f64(ua ^ (ub & FPU_LIB_FP64_SIGNEDFP_MASK));
}

/*
 * Floating-point width conversions
 */

static forceinline fpu_f64_t fpu_fcvt_f32_to_f64(fpu_f32_t f)
{
    return fpu_wrap_f64((actual_double_t)fpu_raw_f32(f));
}

static forceinline fpu_f32_t fpu_fcvt_f64_to_f32(fpu_f64_t d)
{
    return fpu_wrap_f32((actual_double_t)fpu_raw_f64(d));
}

/*
 * Integer to floating-point conversions
 */

static forceinline fpu_f32_t fpu_fcvt_u32_to_f32(uint32_t u)
{
    return fpu_wrap_f32((actual_float_t)u);
}

static forceinline fpu_f64_t fpu_fcvt_u32_to_f64(uint32_t u)
{
    return fpu_wrap_f64((actual_double_t)u);
}

static forceinline fpu_f32_t fpu_fcvt_i32_to_f32(int32_t i)
{
    return fpu_wrap_f32((actual_float_t)i);
}

static forceinline fpu_f64_t fpu_fcvt_i32_to_f64(int32_t i)
{
    return fpu_wrap_f64((actual_double_t)i);
}

static forceinline fpu_f32_t fpu_fcvt_u64_to_f32(uint64_t u)
{
#if !defined(FPU_LIB_CORRECT_CONVERSION_LONG_FP)
    if (likely(!(u >> 32))) {
        return fpu_wrap_f32((actual_float_t)(uint32_t)u);
    } else if (unlikely(u >= 0x8000000000000000ULL)) {
        // Emulate conversion beyond i64 in software
        return fpu_bit_u32_to_f32(0x5E800000U + ((((uint32_t)(u >> 39)) + 1) >> 1));
    }
#endif
    return fpu_wrap_f32((actual_float_t)u);
}

static forceinline fpu_f64_t fpu_fcvt_u64_to_f64(uint64_t u)
{
#if !defined(FPU_LIB_CORRECT_CONVERSION_LONG_FP)
    if (likely(!(u >> 32))) {
        return fpu_wrap_f64((actual_double_t)(uint32_t)u);
    } else if (unlikely(u >= 0x8000000000000000ULL)) {
        // Emulate conversion beyond i64 in software
        return fpu_bit_u64_to_f64(0x43D0000000000000ULL + (((u >> 10) + 1) >> 1));
    }
#endif
    return fpu_wrap_f64((actual_double_t)u);
}

static forceinline fpu_f32_t fpu_fcvt_i64_to_f32(int64_t i)
{
#if !defined(FPU_LIB_CORRECT_CONVERSION_LONG_FP)
    if (likely(((int64_t)(int32_t)i) == i)) {
        return fpu_wrap_f32((actual_float_t)(int32_t)i);
    }
#endif
    return fpu_wrap_f32((actual_float_t)i);
}

static forceinline fpu_f64_t fpu_fcvt_i64_to_f64(int64_t i)
{
#if !defined(FPU_LIB_CORRECT_CONVERSION_LONG_FP)
    if (likely(((int64_t)(int32_t)i) == i)) {
        return fpu_wrap_f64((actual_double_t)(int32_t)i);
    }
#endif
    return fpu_wrap_f64((actual_double_t)i);
}

/*
 * Check whether floating-point value fits an integer type, never raises exceptions
 */

static forceinline bool fpu_f32_fits_u32(fpu_f32_t f)
{
    uint32_t u = fpu_bit_f32_to_u32(f);
    return likely(u < 0x4F800000U) || (u & FPU_LIB_FP32_NOSIGNED_MASK) < 0x3F800000U;
}

static forceinline bool fpu_f64_fits_u32(fpu_f64_t d)
{
    uint64_t u = fpu_bit_f64_to_u64(d);
    return likely(u < 0x41F0000000000000ULL) || (u & FPU_LIB_FP64_NOSIGNED_MASK) < 0x3FF0000000000000ULL;
}

static forceinline bool fpu_f32_fits_i32(fpu_f32_t f)
{
    uint32_t u = fpu_bit_f32_to_u32(f);
    return (u & FPU_LIB_FP32_NOSIGNED_MASK) < 0x4F000000U;
}

static forceinline bool fpu_f64_fits_i32(fpu_f64_t d)
{
    uint64_t u = fpu_bit_f64_to_u64(d);
    return (u & FPU_LIB_FP64_NOSIGNED_MASK) < 0x41E0000000000000ULL;
}

static forceinline bool fpu_f32_fits_u64(fpu_f32_t f)
{
    uint32_t u = fpu_bit_f32_to_u32(f);
    return likely(u < 0x5F800000U) || (u & FPU_LIB_FP32_NOSIGNED_MASK) < 0x3F800000U;
}

static forceinline bool fpu_f64_fits_u64(fpu_f64_t d)
{
    uint64_t u = fpu_bit_f64_to_u64(d);
    return likely(u < 0x43F0000000000000ULL) || (u & FPU_LIB_FP64_NOSIGNED_MASK) < 0x3FF0000000000000ULL;
}

static forceinline bool fpu_f32_fits_i64(fpu_f32_t f)
{
    uint32_t u = fpu_bit_f32_to_u32(f);
    return (u & FPU_LIB_FP32_NOSIGNED_MASK) < 0x5F000000U;
}

static forceinline bool fpu_f64_fits_i64(fpu_f64_t d)
{
    uint64_t u = fpu_bit_f64_to_u64(d);
    return (u & FPU_LIB_FP64_NOSIGNED_MASK) < 0x43E0000000000000ULL;
}

/*
 * Floating-point to integer conversions
 *
 * - Converting a value outside output range raises invalid flag & saturates the output
 * - NaN is treated like positive infinity
 */

// Float to integer
static forceinline uint32_t fpu_fcvt_f32_to_u32(fpu_f32_t f)
{
    if (likely(fpu_f32_fits_u32(f))) {
        fpu_soft_fenv_copium32(f);
        return (uint32_t)fpu_raw_f32(f);
    }
    fpu_raise_invalid();
    if (fpu_is_negative32(f)) {
        return 0;
    }
    return -1;
}

static forceinline uint32_t fpu_fcvt_f64_to_u32(fpu_f64_t d)
{
    if (likely(fpu_f64_fits_u32(d))) {
        fpu_soft_fenv_copium64(d);
        return (uint32_t)fpu_raw_f64(d);
    }
    fpu_raise_invalid();
    if (fpu_is_negative64(d)) {
        return 0;
    }
    return -1;
}

static forceinline int32_t fpu_fcvt_f32_to_i32(fpu_f32_t f)
{
    if (likely(fpu_f32_fits_i32(f))) {
        fpu_soft_fenv_copium32(f);
        return (int32_t)fpu_raw_f32(f);
    }
    fpu_raise_invalid();
    if (fpu_is_negative32(f)) {
        return 0x80000000U;
    }
    return 0x7FFFFFFFU;
}

static forceinline int32_t fpu_fcvt_f64_to_i32(fpu_f64_t d)
{
    if (likely(fpu_f64_fits_i32(d))) {
        fpu_soft_fenv_copium64(d);
        return (int32_t)fpu_raw_f64(d);
    }
    fpu_raise_invalid();
    if (fpu_is_negative64(d)) {
        return 0x80000000U;
    }
    return 0x7FFFFFFFU;
}

static forceinline uint64_t fpu_fcvt_f32_to_u64(fpu_f32_t f)
{
    if (likely(fpu_f32_fits_u64(f))) {
        fpu_soft_fenv_copium32(f);
#if !defined(FPU_LIB_CORRECT_CONVERSION_FP_LONG)
        if (likely(fpu_f32_fits_u32(f))) {
            return (uint32_t)fpu_raw_f32(f);
        } else if (likely(fpu_f32_fits_i64(f))) {
            return (int64_t)fpu_raw_f32(f);
        }
#endif
        return (uint64_t)fpu_raw_f32(f);
    }
    fpu_raise_invalid();
    if (fpu_is_negative32(f)) {
        return 0;
    }
    return -1;
}

static forceinline uint64_t fpu_fcvt_f64_to_u64(fpu_f64_t d)
{
    if (likely(fpu_f64_fits_u64(d))) {
        fpu_soft_fenv_copium64(d);
#if !defined(FPU_LIB_CORRECT_CONVERSION_FP_LONG)
        if (likely(fpu_f64_fits_u32(d))) {
            return (uint32_t)fpu_raw_f64(d);
        } else if (likely(fpu_f64_fits_i64(d))) {
            return (int64_t)fpu_raw_f64(d);
        }
#endif
        return (uint64_t)fpu_raw_f64(d);
    }
    fpu_raise_invalid();
    if (fpu_is_negative64(d)) {
        return 0;
    }
    return -1;
}

static forceinline int64_t fpu_fcvt_f32_to_i64(fpu_f32_t f)
{
    if (likely(fpu_f32_fits_i64(f))) {
        fpu_soft_fenv_copium32(f);
#if !defined(FPU_LIB_CORRECT_CONVERSION_FP_LONG)
        if (likely(fpu_f32_fits_i32(f))) {
            return (int32_t)fpu_raw_f32(f);
        }
#endif
        return (int64_t)fpu_raw_f32(f);
    }
    fpu_raise_invalid();
    if (fpu_is_negative32(f)) {
        return 0x8000000000000000ULL;
    }
    return 0x7FFFFFFFFFFFFFFFULL;
}

static forceinline int64_t fpu_fcvt_f64_to_i64(fpu_f64_t d)
{
    if (likely(fpu_f64_fits_i64(d))) {
        fpu_soft_fenv_copium64(d);
#if !defined(FPU_LIB_CORRECT_CONVERSION_FP_LONG)
        if (likely(fpu_f64_fits_i32(d))) {
            return (int32_t)fpu_raw_f64(d);
        }
#endif
        return (int64_t)fpu_raw_f64(d);
    }
    fpu_raise_invalid();
    if (fpu_is_negative64(d)) {
        return 0x8000000000000000ULL;
    }
    return 0x7FFFFFFFFFFFFFFFULL;
}

/*
 * Floating-point to integer rounding
 */

static forceinline uint32_t fpu_round_f32_to_u32(fpu_f32_t f, uint32_t mode)
{
    if (likely(mode == FPU_LIB_ROUND_TZ)) {
        return fpu_fcvt_f32_to_u32(f);
    }
    return fpu_fcvt_f32_to_u32(fpu_round_f32_internal(f, mode));
}

static forceinline uint32_t fpu_round_f64_to_u32(fpu_f64_t d, uint32_t mode)
{
    if (likely(mode == FPU_LIB_ROUND_TZ)) {
        return fpu_fcvt_f64_to_u32(d);
    }
    return fpu_fcvt_f64_to_u32(fpu_round_f64_internal(d, mode));
}

static forceinline int32_t fpu_round_f32_to_i32(fpu_f32_t f, uint32_t mode)
{
    if (likely(mode == FPU_LIB_ROUND_TZ)) {
        return fpu_fcvt_f32_to_i32(f);
    }
    return fpu_fcvt_f32_to_i32(fpu_round_f32_internal(f, mode));
}

static forceinline int32_t fpu_round_f64_to_i32(fpu_f64_t d, uint32_t mode)
{
    if (likely(mode == FPU_LIB_ROUND_TZ)) {
        return fpu_fcvt_f64_to_i32(d);
    }
    return fpu_fcvt_f64_to_i32(fpu_round_f64_internal(d, mode));
}

static forceinline uint64_t fpu_round_f32_to_u64(fpu_f32_t f, uint32_t mode)
{
    if (likely(mode == FPU_LIB_ROUND_TZ)) {
        return fpu_fcvt_f32_to_u64(f);
    }
    return fpu_fcvt_f32_to_u64(fpu_round_f32_internal(f, mode));
}

static forceinline uint64_t fpu_round_f64_to_u64(fpu_f64_t d, uint32_t mode)
{
    if (likely(mode == FPU_LIB_ROUND_TZ)) {
        return fpu_fcvt_f64_to_u64(d);
    }
    return fpu_fcvt_f64_to_u64(fpu_round_f64_internal(d, mode));
}

static forceinline int64_t fpu_round_f32_to_i64(fpu_f32_t f, uint32_t mode)
{
    if (likely(mode == FPU_LIB_ROUND_TZ)) {
        return fpu_fcvt_f32_to_i64(f);
    }
    return fpu_fcvt_f32_to_i64(fpu_round_f32_internal(f, mode));
}

static forceinline int64_t fpu_round_f64_to_i64(fpu_f64_t d, uint32_t mode)
{
    if (likely(mode == FPU_LIB_ROUND_TZ)) {
        return fpu_fcvt_f64_to_i64(d);
    }
    return fpu_fcvt_f64_to_i64(fpu_round_f64_internal(d, mode));
}

/*
 * IEEE 754 Signaling Comparisons
 *
 * - If only one operand is a NaN, the result is false, the invalid operation exception flag is set
 * - Negative zero -0.0 is not distinguished from 0.0
 */

// Comparison: Less than (<)
static forceinline bool fpu_is_flt32_sig(fpu_f32_t a, fpu_f32_t b)
{
#if defined(FPU_LIB_CONFORMING_SIGNALING_COMPARE)
    return fpu_raw_f32(a) < fpu_raw_f32(b);
#else
    if (fpu_raw_f32(a) < fpu_raw_f32(b)) {
        return true;
    } else if (!(fpu_raw_f32(a) >= fpu_raw_f32(b))) {
        fpu_raise_invalid();
    }
    return false;
#endif
}

static forceinline bool fpu_is_flt64_sig(fpu_f64_t a, fpu_f64_t b)
{
#if defined(FPU_LIB_CONFORMING_SIGNALING_COMPARE)
    return fpu_raw_f64(a) < fpu_raw_f64(b);
#else
    if (fpu_raw_f64(a) < fpu_raw_f64(b)) {
        return true;
    } else if (!(fpu_raw_f64(a) >= fpu_raw_f64(b))) {
        fpu_raise_invalid();
    }
    return false;
#endif
}

// Comparison: Less than or equal (<=)
static forceinline bool fpu_is_fle32_sig(fpu_f32_t a, fpu_f32_t b)
{
#if defined(FPU_LIB_CONFORMING_SIGNALING_COMPARE)
    return fpu_raw_f32(a) <= fpu_raw_f32(b);
#else
    if (fpu_raw_f32(a) <= fpu_raw_f32(b)) {
        return true;
    } else if (!(fpu_raw_f32(a) > fpu_raw_f32(b))) {
        fpu_raise_invalid();
    }
    return false;
#endif
}

static forceinline bool fpu_is_fle64_sig(fpu_f64_t a, fpu_f64_t b)
{
#if defined(FPU_LIB_CONFORMING_SIGNALING_COMPARE)
    return fpu_raw_f64(a) <= fpu_raw_f64(b);
#else
    if (fpu_raw_f64(a) <= fpu_raw_f64(b)) {
        return true;
    } else if (!(fpu_raw_f64(a) > fpu_raw_f64(b))) {
        fpu_raise_invalid();
    }
    return false;
#endif
}

/*
 * IEEE 754 Quiet Comparisons
 *
 * - If at least one operand is a NaN, the result is false
 * - If at least one operand is a signaling NaN, the invalid operation exception flag is set
 * - Infinity is handled as being larger than anything finite, -Inf < any non-Nan, etc
 * - Negative zero -0.0 is not distinguished from 0.0
 */

// Comparison: Less than (<)
static forceinline bool fpu_is_flt32_quiet(fpu_f32_t a, fpu_f32_t b)
{
#if defined(FPU_LIB_CONFORMING_BUILTIN_QUIET_COMPARE)
    return __builtin_isless(fpu_raw_f32(a), fpu_raw_f32(b));
#else
    if (likely(!fpu_is_nan32(a) && !fpu_is_nan32(b))) {
        return fpu_raw_f32(a) < fpu_raw_f32(b);
    }
    return false;
#endif
}

static forceinline bool fpu_is_flt64_quiet(fpu_f64_t a, fpu_f64_t b)
{
#if defined(FPU_LIB_CONFORMING_BUILTIN_QUIET_COMPARE)
    return __builtin_isless(fpu_raw_f64(a), fpu_raw_f64(b));
#else
    if (likely(!fpu_is_nan64(a) && !fpu_is_nan64(b))) {
        return fpu_raw_f64(a) < fpu_raw_f64(b);
    }
    return false;
#endif
}

// Comparison: Less than or equal (<=)
static forceinline bool fpu_is_fle32_quiet(fpu_f32_t a, fpu_f32_t b)
{
#if defined(FPU_LIB_CONFORMING_BUILTIN_QUIET_COMPARE)
    return __builtin_islessequal(fpu_raw_f32(a), fpu_raw_f32(b));
#else
    if (likely(!fpu_is_nan32(a) && !fpu_is_nan32(b))) {
        return fpu_raw_f32(a) <= fpu_raw_f32(b);
    }
    return false;
#endif
}

static forceinline bool fpu_is_fle64_quiet(fpu_f64_t a, fpu_f64_t b)
{
#if defined(FPU_LIB_CONFORMING_BUILTIN_QUIET_COMPARE)
    return __builtin_islessequal(fpu_raw_f64(a), fpu_raw_f64(b));
#else
    if (likely(!fpu_is_nan64(a) && !fpu_is_nan64(b))) {
        return fpu_raw_f64(a) <= fpu_raw_f64(b);
    }
    return false;
#endif
}

/*
 * IEEE 754-2008 minNum and maxNum
 *
 * - If one operand is a NaN, the result is the non-NaN operand
 * - If at least one operand is a signaling NaN, the invalid operation exception flag is set
 * - Negative zero -0.0 is considered smaller than 0.0
 */

static forceinline fpu_f32_t fpu_min32(fpu_f32_t a, fpu_f32_t b)
{
#if defined(FPU_LIB_CONFORMING_BUILTIN_IEEE754_2008_FMINMAX)
    // On RISC-V, fminf() actually behaves the way we need
    return fpu_wrap_f32(__builtin_fminf(fpu_raw_f32(a), fpu_raw_f32(b)));
#elif defined(FPU_LIB_CONFORMING_BUILTIN_QUIET_COMPARE)
    if (fpu_is_flt32_quiet(a, b)) {
        return a;
    } else if (likely(fpu_is_flt32_quiet(b, a))) {
        return b;
    } else if (fpu_bit_f32_to_u32(a) == FPU_LIB_FP32_SIGNEDFP_MASK) {
        return a;
    }
    return b;
#else
    if (unlikely(fpu_is_nan32(a))) {
        return b;
    } else if (unlikely(fpu_is_nan32(b))) {
        return a;
    } else if (fpu_raw_f32(a) < fpu_raw_f32(b)) {
        return a;
    } else if (fpu_raw_f32(b) < fpu_raw_f32(a)) {
        return b;
    }
    return fpu_signbit32(a) ? a : b;
#endif
}

static forceinline fpu_f64_t fpu_min64(fpu_f64_t a, fpu_f64_t b)
{
#if defined(FPU_LIB_CONFORMING_BUILTIN_IEEE754_2008_FMINMAX)
    return fpu_wrap_f64(__builtin_fmin(fpu_raw_f64(a), fpu_raw_f64(b)));
#elif defined(FPU_LIB_CONFORMING_BUILTIN_QUIET_COMPARE)
    // fpu_is_flt64_quiet() matches -Inf < non-Nan,
    if (fpu_is_flt64_quiet(a, b)) {
        return a;
    } else if (likely(fpu_is_flt64_quiet(b, a))) {
        return b;
    } else if (fpu_bit_f64_to_u64(a) == FPU_LIB_FP64_SIGNEDFP_MASK) {
        return a;
    }
    return b;
#else
    if (unlikely(fpu_is_nan64(a))) {
        return b;
    } else if (unlikely(fpu_is_nan64(b))) {
        return a;
    } else if (fpu_raw_f64(a) < fpu_raw_f64(b)) {
        return a;
    } else if (fpu_raw_f64(b) < fpu_raw_f64(a)) {
        return b;
    }
    return fpu_signbit64(a) ? a : b;
#endif
}

static forceinline fpu_f32_t fpu_max32(fpu_f32_t a, fpu_f32_t b)
{
#if defined(FPU_LIB_CONFORMING_BUILTIN_IEEE754_2008_FMINMAX)
    return fpu_wrap_f32(__builtin_fmaxf(fpu_raw_f32(a), fpu_raw_f32(b)));
#elif defined(FPU_LIB_CONFORMING_BUILTIN_QUIET_COMPARE)
    if (fpu_is_flt32_quiet(b, a)) {
        return a;
    } else if (likely(fpu_is_flt32_quiet(a, b))) {
        return b;
    } else if (!fpu_bit_f32_to_u32(a)) {
        return a;
    }
    return b;
#else
    if (unlikely(fpu_is_nan32(a))) {
        return b;
    } else if (unlikely(fpu_is_nan32(b))) {
        return a;
    } else if (fpu_raw_f32(a) > fpu_raw_f32(b)) {
        return a;
    } else if (fpu_raw_f32(b) > fpu_raw_f32(a)) {
        return b;
    }
    return fpu_signbit32(a) ? b : a;
#endif
}

static forceinline fpu_f64_t fpu_max64(fpu_f64_t a, fpu_f64_t b)
{
#if defined(FPU_LIB_CONFORMING_BUILTIN_IEEE754_2008_FMINMAX)
    return fpu_wrap_f64(__builtin_fmax(fpu_raw_f64(a), fpu_raw_f64(b)));
#elif defined(FPU_LIB_CONFORMING_BUILTIN_QUIET_COMPARE)
    if (fpu_is_flt64_quiet(b, a)) {
        return a;
    } else if (likely(fpu_is_flt64_quiet(a, b))) {
        return b;
    } else if (!fpu_bit_f64_to_u64(a)) {
        return a;
    }
    return b;
#else
    if (unlikely(fpu_is_nan64(a))) {
        return b;
    } else if (unlikely(fpu_is_nan64(b))) {
        return a;
    } else if (fpu_raw_f64(a) > fpu_raw_f64(b)) {
        return a;
    } else if (fpu_raw_f64(b) > fpu_raw_f64(a)) {
        return b;
    }
    return fpu_signbit64(a) ? b : a;
#endif
}

#endif
