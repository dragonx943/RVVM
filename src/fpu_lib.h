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
 * Signaling comparisons (<, <=) are miscompiled:
 * - On GCC <8.1, generating ucomiss instead of comiss
 * - On Clang <10.0 or without -frounding-math. Why the fuck is it not IEEE 754 compliant by default???
 * - Various issues on arm32, powerpc, even with a modern compiler...
 * Solution: Manually raise FE_INVALID if neither a < b nor b <= a
 *
 * Converting floats to 64-bit integers is broken in various ways:
 * - On Clang arm32, any fp<->i64/u64 tests are failing - have not tested GCC
 * - On TCC, it seems that it fails to set FE_INEXACT, probably uses a software fcvt implementation
 * Solution: Check whether fp fits into i32/u32 instead, with fallback to i64/u64,
 *   which is also more optimal for 32-bit architectures.
 *
 * Various libm calls (sqrt, fma, etc) may ignore FE_INEXACT/FE_INVALID semantics, etc:
 * - Windows libm seems to not set FE_INVALID on sqrt(-1), even on MinGW
 * Solution: Raise FE_INVALID manually for invalid operations.
 *
 * Emscripten, Windows CE (AR32), MIPS/MIPS64 platforms lack FENV completely:
 * - Basically what the title says. None of those have a working <fenv.h> implementation.
 * - There is no way to implement fesetround() anyways, at least as far as I'm aware
 * Solution: Thread-local exception flags, which are raised manually based on various checks.
 * NOTE: RISC-V THead CPUs also fall in this category, but I'm hesitant to enable such workaround
 * on RISC-V in general as this case is a single non-conforming hardware implementation.
 *
 * This list is not even considering various historical compiler versions, and MSVC. To be continued?...
 *
 * Because of this, and because most workarounds have little performance impact, consider limited
 * list of compilers/platforms a "Known Nice Platform" (C) and apply needed workarounds otherwise.
 */

#if defined(__EMSCRIPTEN__) || defined(__mips__) || defined(__mips) /**/                                               \
    || (defined(_WIN32) && defined(__arm__)) || defined(_M_ARM)
// Enable FENV emulation
#undef USE_SOFT_FPU_FENV
#define USE_SOFT_FPU_FENV 1
#elif CLANG_CHECK_VER(12, 0)
// Fix rounding misoptimizations even when -frounding-math is not present (I hope so dammit)
#pragma float_control(precise)
#pragma STDC FENV_ACCESS ON
#elif defined(_MSC_VER)
#pragma fenv_access(on)
#endif

#undef FPU_LIB_KNOWN_NICE_PLATFORM
#if !defined(USE_SOFT_FPU_FENV) && (GCC_CHECK_VER(8, 1) || CLANG_CHECK_VER(10, 0)) /**/                                                               \
    && (defined(__x86_64__) || defined(__aarch64__) || defined(__riscv))
#define FPU_LIB_KNOWN_NICE_PLATFORM 1
#endif

/*
 * Hide <math.h> definitions from generic code if possible - it should not be used
 */
#if GNU_BUILTIN(__builtin_sqrt) && GNU_BUILTIN(__builtin_copysign) && GNU_BUILTIN(__builtin_fmod)       /**/           \
    && GNU_BUILTIN(__builtin_sqrtf) && GNU_BUILTIN(__builtin_copysignf) && GNU_BUILTIN(__builtin_fmodf) /**/           \
    && GNU_BUILTIN(__builtin_isfinite) && !defined(USE_MATH_LIB)
#define fpu_isfinite_internal(f)     __builtin_isfinite(f)
#define fpu_sqrt_internal(d)         __builtin_sqrt(d)
#define fpu_sqrtf_internal(f)        __builtin_sqrtf(f)
#define fpu_copysign_internal(a, b)  __builtin_copysign(a, b)
#define fpu_copysignf_internal(a, b) __builtin_copysignf(a, b)
#define fpu_fmod_internal(a, b)      __builtin_fmod(a, b)
#define fpu_fmodf_internal(a, b)     __builtin_fmodf(a, b)
#else
#include <math.h>
#define fpu_isfinite_internal(f)     isfinite(f)
#define fpu_sqrt_internal(d)         sqrt(d)
#define fpu_sqrtf_internal(f)        sqrtf(f)
#define fpu_copysign_internal(a, b)  copysign(a, b)
#define fpu_copysignf_internal(a, b) copysignf(a, b)
#define fpu_fmod_internal(a, b)      fmod(a, b)
#define fpu_fmodf_internal(a, b)     fmodf(a, b)
#endif

/*
 * FPU Environment handling
 *
 * NOTE: Those definitions match RISC-V bitfields :D
 */

// FPU Exception flags
#define FPU_ENV_FLAG_NX         0x01 // Inexact result
#define FPU_ENV_FLAG_UF         0x02 // Underflow
#define FPU_ENV_FLAG_OF         0x04 // Overflow
#define FPU_ENV_FLAG_DZ         0x08 // Division by zero
#define FPU_ENV_FLAG_NV         0x10 // Invalid operation

#define FPU_ENV_FLAGS_ALL       0x1F

// FPU Rounding modes
#define FPU_ENV_ROUND_NE        0x00 // Round to nearest
#define FPU_ENV_ROUND_TZ        0x01 // Round to zero
#define FPU_ENV_ROUND_DN        0x02 // Round down (Towards negative infinity)
#define FPU_ENV_ROUND_UP        0x03 // Round up (Towards positive infinity)
#define FPU_ENV_ROUND_MM        0x04 // Round to nearest, ties to Max Magnitude

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
void     fpu_set_exceptions(uint32_t exceptions);
void     fpu_raise_exceptions(uint32_t exceptions);
void     fpu_clear_exceptions(uint32_t exceptions);

// Rounding mode handling
uint32_t fpu_get_rounding_mode(void);
void     fpu_set_rounding_mode(uint32_t mode);

// Unintrusive soft exception handling for inlined paths
slow_path void fpu_raise_inexact(void);
slow_path void fpu_raise_invalid(void);

// FP Classification
slow_path uint32_t fpu_fclass32(fpu_f32_t f);
slow_path uint32_t fpu_fclass64(fpu_f64_t d);

/*
 * FP Bitcasts
 */

static forceinline uint32_t fpu_bit_f32_to_u32(fpu_f32_t f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof(f));
    return u;
}

static forceinline fpu_f32_t fpu_bit_u32_to_f32(uint32_t u)
{
    fpu_f32_t f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

static forceinline uint64_t fpu_bit_f64_to_u64(fpu_f64_t d)
{
    uint64_t u;
    memcpy(&u, &d, sizeof(d));
    return u;
}

static forceinline fpu_f64_t fpu_bit_u64_to_f64(uint64_t u)
{
    fpu_f64_t d;
    memcpy(&d, &u, sizeof(d));
    return d;
}

/*
 * Soft FP wrap helpers
 */

static forceinline actual_float_t fpu_raw_f32(fpu_f32_t f)
{
#if defined(USE_SOFT_FPU_WRAP)
    actual_float_t ret;
    memcpy(&ret, &f, sizeof(f));
    return ret;
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
#else
    return d;
#endif
}

static forceinline bool fpu_soft_fenv_invalid32(fpu_f32_t f)
{
#if defined(USE_SOFT_FPU_FENV)
    return !fpu_isfinite_internal(fpu_raw_f32(f));
#else
    UNUSED(f);
    return false;
#endif
}

static forceinline bool fpu_soft_fenv_invalid64(fpu_f64_t d)
{
#if defined(USE_SOFT_FPU_FENV)
    return !fpu_isfinite_internal(fpu_raw_f64(d));
#else
    UNUSED(d);
    return false;
#endif
}

static forceinline bool fpu_soft_fenv_inexact32(fpu_f32_t f)
{
#if defined(USE_SOFT_FPU_FENV)
    return fpu_fmodf_internal(fpu_raw_f32(f), 1.f) != 0.f;
#else
    UNUSED(f);
    return false;
#endif
}

static forceinline bool fpu_soft_fenv_inexact64(fpu_f64_t d)
{
#if defined(USE_SOFT_FPU_FENV)
    return fpu_fmod_internal(fpu_raw_f64(d), 1.0) != 0.0;
#else
    UNUSED(d);
    return false;
#endif
}

static forceinline void fpu_soft_fenv_copium32(fpu_f32_t f)
{
    UNUSED(f);
#if defined(USE_SOFT_FPU_FENV)
    if (fpu_soft_fenv_invalid32(f)) {
        fpu_raise_invalid();
    } else if (fpu_soft_fenv_inexact32(f)) {
        fpu_raise_inexact();
    }
#endif
}

static forceinline void fpu_soft_fenv_copium64(fpu_f64_t d)
{
    UNUSED(d);
#if defined(USE_SOFT_FPU_FENV)
    if (fpu_soft_fenv_invalid64(d)) {
        fpu_raise_invalid();
    } else if (fpu_soft_fenv_inexact64(d)) {
        fpu_raise_inexact();
    }
#endif
}

static forceinline void fpu_soft_fenv_alu_copium32(fpu_f32_t a, fpu_f32_t b, bool div)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(div);
#if defined(USE_SOFT_FPU_FENV)
    if (fpu_soft_fenv_invalid32(a) || fpu_soft_fenv_invalid32(b)) {
        fpu_raise_invalid();
    } else if ((div || fpu_soft_fenv_inexact32(a)) && fpu_soft_fenv_inexact32(b)) {
        fpu_raise_inexact();
    }
#endif
}

static forceinline void fpu_soft_fenv_alu_copium64(fpu_f64_t a, fpu_f64_t b, bool div)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(div);
#if defined(USE_SOFT_FPU_FENV)
    if (fpu_soft_fenv_invalid64(a) || fpu_soft_fenv_invalid64(b)) {
        fpu_raise_invalid();
    } else if ((div || fpu_soft_fenv_inexact64(a)) && fpu_soft_fenv_inexact64(b)) {
        fpu_raise_inexact();
    }
#endif
}

/*
 * FP NaN-boxing, explicit endian load/store intrinsics
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
    if (likely(!(((uint32_t)(u >> 32)) + 1))) {
        return fpu_bit_u32_to_f32(u);
    }
    return fpu_bit_u32_to_f32(0x7FC00000U);
}

static forceinline fpu_f32_t fpu_load_f32_le(const void* addr)
{
#if defined(HOST_LITTLE_ENDIAN)
    return *(const safe_aliasing fpu_f32_t*)addr;
#else
    return fpu_bit_u32_to_f32(read_uint32_le(addr));
#endif
}

static forceinline void fpu_store_f32_le(void* addr, fpu_f32_t f)
{
#if defined(HOST_LITTLE_ENDIAN)
    *(safe_aliasing fpu_f32_t*)addr = f;
#else
    write_uint32_le(addr, fpu_bit_f32_to_u32(f));
#endif
}

static forceinline fpu_f64_t fpu_load_f64_le(const void* addr)
{
#if defined(HOST_LITTLE_ENDIAN)
    return *(const safe_aliasing fpu_f64_t*)addr;
#else
    return fpu_bit_u64_to_f64(read_uint64_le(addr));
#endif
}

static forceinline void fpu_store_f64_le(void* addr, fpu_f64_t d)
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

// Those functions won't raise FE_INVALID on sNaN
static slow_path bool fpu_is_nan32_quiet(fpu_f32_t f)
{
    uint32_t u = fpu_bit_f32_to_u32(f);
    return ((((uint16_t)(u >> 16) & 0x7FFFU)) + !!(uint16_t)u) > 0x7F80U;
}

static slow_path bool fpu_is_nan64_quiet(fpu_f64_t d)
{
    uint64_t u = fpu_bit_f64_to_u64(d);
    return ((((uint32_t)(u >> 32)) & 0x7FFFFFFFU) + !!(uint32_t)u) > 0x7FF00000U;
}

static forceinline bool fpu_is_snan32(fpu_f32_t f)
{
    if (fpu_bit_f32_to_u32(f) & 0x00400000U) {
        return false;
    }
    return fpu_is_nan32_quiet(f);
}

static forceinline bool fpu_is_snan64(fpu_f64_t d)
{
    if (fpu_bit_f64_to_u64(d) & 0x0008000000000000ULL) {
        return false;
    }
    return fpu_is_nan64_quiet(d);
}

// Quiet == comparisons, raise FE_INVALID on sNaN
static forceinline bool fpu_is_equal32_quiet(fpu_f32_t a, fpu_f32_t b)
{
    if (fpu_raw_f32(a) == fpu_raw_f32(b)) {
        return true;
    }
#if defined(USE_SOFT_FPU_FENV)
    if (fpu_is_snan32(a) || fpu_is_snan32(b)) {
        fpu_raise_invalid();
    }
#endif
    return false;
}

static forceinline bool fpu_is_equal64_quiet(fpu_f64_t a, fpu_f64_t b)
{
    if (fpu_raw_f64(a) == fpu_raw_f64(b)) {
        return true;
    }
#if defined(USE_SOFT_FPU_FENV)
    if (fpu_is_snan64(a) || fpu_is_snan64(b)) {
        fpu_raise_invalid();
    }
#endif
    return false;
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
 * FP Conversion
 */

// Float to float
static forceinline fpu_f64_t fpu_fcvt_f32_to_f64(fpu_f32_t f)
{
    return fpu_wrap_f64((actual_double_t)fpu_raw_f32(f));
}

static forceinline fpu_f32_t fpu_fcvt_f64_to_f32(fpu_f64_t d)
{
    return fpu_wrap_f32((actual_double_t)fpu_raw_f64(d));
}

// Integer to float
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
#if !defined(FPU_LIB_KNOWN_NICE_PLATFORM)
    if (u <= 4294967295) {
        return fpu_wrap_f32((actual_float_t)(uint32_t)u);
    }
#endif
    return fpu_wrap_f32((actual_float_t)u);
}

static forceinline fpu_f64_t fpu_fcvt_u64_to_f64(uint64_t u)
{
#if !defined(FPU_LIB_KNOWN_NICE_PLATFORM)
    if (u <= 4294967295) {
        return fpu_wrap_f64((actual_double_t)(uint32_t)u);
    }
#endif
    return fpu_wrap_f64((actual_double_t)u);
}

static forceinline fpu_f32_t fpu_fcvt_i64_to_f32(int64_t i)
{
#if !defined(FPU_LIB_KNOWN_NICE_PLATFORM)
    if (i >= -2147483648 && i <= 2147483647) {
        return fpu_wrap_f32((actual_float_t)(int32_t)i);
    }
#endif
    return fpu_wrap_f32((actual_float_t)i);
}

static forceinline fpu_f64_t fpu_fcvt_i64_to_f64(int64_t i)
{
#if !defined(FPU_LIB_KNOWN_NICE_PLATFORM)
    if (i >= -2147483648 && i <= 2147483647) {
        return fpu_wrap_f64((actual_double_t)(int32_t)i);
    }
#endif
    return fpu_wrap_f64((actual_double_t)i);
}

// Checking whether valid float to integer conversion is possible
static forceinline bool fpu_f32_fits_u32(fpu_f32_t f)
{
    actual_float_t tmp = fpu_raw_f32(f);
    return tmp > -1.f && tmp < 4294967296.f;
}

static forceinline bool fpu_f64_fits_u32(fpu_f64_t d)
{
    actual_double_t tmp = fpu_raw_f64(d);
    return tmp > -1.0 && tmp < 4294967296.0;
}

static forceinline bool fpu_f32_fits_i32(fpu_f32_t f)
{
    actual_float_t tmp = fpu_raw_f32(f);
    return tmp > -2147483649.f && tmp < 2147483648.f;
}

static forceinline bool fpu_f64_fits_i32(fpu_f64_t d)
{
    actual_double_t tmp = fpu_raw_f64(d);
    return tmp > -2147483649.0 && tmp < 2147483648.0;
}

static forceinline bool fpu_f32_fits_u64(fpu_f32_t f)
{
    actual_float_t tmp = fpu_raw_f32(f);
    return tmp > -1.f && tmp < 18446744073709551616.f;
}

static forceinline bool fpu_f64_fits_u64(fpu_f64_t d)
{
    actual_double_t tmp = fpu_raw_f64(d);
    return tmp > -1.0 && tmp < 18446744073709551616.0;
}

static forceinline bool fpu_f32_fits_i64(fpu_f32_t f)
{
    actual_float_t tmp = fpu_raw_f32(f);
    return tmp > -9223372036854775809.f && tmp < 9223372036854775808.f;
}

static forceinline bool fpu_f64_fits_i64(fpu_f64_t d)
{
    actual_double_t tmp = fpu_raw_f64(d);
    return tmp > -9223372036854775809.0 && tmp < 9223372036854775808.0;
}

// Float to integer
static forceinline uint32_t fpu_fcvt_f32_to_u32(fpu_f32_t f)
{
    fpu_soft_fenv_copium32(f);
    return (uint32_t)fpu_raw_f32(f);
}

static forceinline uint32_t fpu_fcvt_f64_to_u32(fpu_f64_t d)
{
    fpu_soft_fenv_copium64(d);
    return (uint32_t)fpu_raw_f64(d);
}

static forceinline int32_t fpu_fcvt_f32_to_i32(fpu_f32_t f)
{
    fpu_soft_fenv_copium32(f);
    return (int32_t)fpu_raw_f32(f);
}

static forceinline int32_t fpu_fcvt_f64_to_i32(fpu_f64_t d)
{
    fpu_soft_fenv_copium64(d);
    return (int32_t)fpu_raw_f64(d);
}

static forceinline uint64_t fpu_fcvt_f32_to_u64(fpu_f32_t f)
{
    fpu_soft_fenv_copium32(f);
#if !defined(FPU_LIB_KNOWN_NICE_PLATFORM)
    if (likely(fpu_f32_fits_u32(f))) {
        return (uint32_t)fpu_raw_f32(f);
    }
#endif
    return (uint64_t)fpu_raw_f32(f);
}

static forceinline uint64_t fpu_fcvt_f64_to_u64(fpu_f64_t d)
{
    fpu_soft_fenv_copium64(d);
#if !defined(FPU_LIB_KNOWN_NICE_PLATFORM)
    if (likely(fpu_f64_fits_u32(d))) {
        return (uint32_t)fpu_raw_f64(d);
    }
#endif
    return (uint64_t)fpu_raw_f64(d);
}

static forceinline int64_t fpu_fcvt_f32_to_i64(fpu_f32_t f)
{
    fpu_soft_fenv_copium32(f);
#if !defined(FPU_LIB_KNOWN_NICE_PLATFORM)
    if (likely(fpu_f32_fits_i32(f))) {
        return (int32_t)fpu_raw_f32(f);
    }
#endif
    return (int64_t)fpu_raw_f32(f);
}

static forceinline int64_t fpu_fcvt_f64_to_i64(fpu_f64_t d)
{
    fpu_soft_fenv_copium64(d);
#if !defined(FPU_LIB_KNOWN_NICE_PLATFORM)
    if (likely(fpu_f64_fits_i32(d))) {
        return (int32_t)fpu_raw_f64(d);
    }
#endif
    return (int64_t)fpu_raw_f64(d);
}

/*
 * FP Arithmetic
 */

static forceinline fpu_f32_t fpu_add32(fpu_f32_t a, fpu_f32_t b)
{
    fpu_soft_fenv_alu_copium32(a, b, false);
    return fpu_wrap_f32(fpu_raw_f32(a) + fpu_raw_f32(b));
}

static forceinline fpu_f64_t fpu_add64(fpu_f64_t a, fpu_f64_t b)
{
    fpu_soft_fenv_alu_copium64(a, b, false);
    return fpu_wrap_f64(fpu_raw_f64(a) + fpu_raw_f64(b));
}

static forceinline fpu_f32_t fpu_sub32(fpu_f32_t a, fpu_f32_t b)
{
    fpu_soft_fenv_alu_copium32(a, b, false);
    return fpu_wrap_f32(fpu_raw_f32(a) - fpu_raw_f32(b));
}

static forceinline fpu_f64_t fpu_sub64(fpu_f64_t a, fpu_f64_t b)
{
    fpu_soft_fenv_alu_copium64(a, b, false);
    return fpu_wrap_f64(fpu_raw_f64(a) - fpu_raw_f64(b));
}

static forceinline fpu_f32_t fpu_mul32(fpu_f32_t a, fpu_f32_t b)
{
    fpu_soft_fenv_alu_copium32(a, b, false);
    return fpu_wrap_f32(fpu_raw_f32(a) * fpu_raw_f32(b));
}

static forceinline fpu_f64_t fpu_mul64(fpu_f64_t a, fpu_f64_t b)
{
    fpu_soft_fenv_alu_copium64(a, b, false);
    return fpu_wrap_f64(fpu_raw_f64(a) * fpu_raw_f64(b));
}

static forceinline fpu_f32_t fpu_div32(fpu_f32_t a, fpu_f32_t b)
{
    fpu_soft_fenv_alu_copium32(a, b, true);
    return fpu_wrap_f32(fpu_raw_f32(a) / fpu_raw_f32(b));
}

static forceinline fpu_f64_t fpu_div64(fpu_f64_t a, fpu_f64_t b)
{
    fpu_soft_fenv_alu_copium64(a, b, true);
    return fpu_wrap_f64(fpu_raw_f64(a) / fpu_raw_f64(b));
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
    fpu_soft_fenv_copium32(f);
    if (likely(fpu_raw_f32(f) >= 0.f)) {
        fpu_soft_fenv_copium32(ret);
        return ret;
    }
    // Windows libm doesn't raise FE_INVALID on sqrt(-1)
    fpu_raise_invalid();
    return ret;
}

static forceinline fpu_f64_t fpu_sqrt64(fpu_f64_t d)
{
    fpu_f64_t ret = fpu_wrap_f64(fpu_sqrt_internal(fpu_raw_f64(d)));
    fpu_soft_fenv_copium64(d);
    if (likely(fpu_raw_f64(d) >= 0.0)) {
        fpu_soft_fenv_copium64(ret);
        return ret;
    }
    // Windows libm doesn't raise FE_INVALID on sqrt(-1)
    fpu_raise_invalid();
    return ret;
}

/*
 * FP Sign operations
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
    return fpu_bit_u32_to_f32(fpu_bit_f32_to_u32(f) ^ 0x80000000U);
#else
    return fpu_wrap_f32(-fpu_raw_f32(f));
#endif
}

static forceinline fpu_f64_t fpu_neg64(fpu_f64_t d)
{
#if defined(USE_SOFT_FPU_WRAP)
    return fpu_bit_u64_to_f64(fpu_bit_f64_to_u64(d) ^ 0x8000000000000000ULL);
#else
    return fpu_wrap_f64(-fpu_raw_f64(d));
#endif
}

static forceinline fpu_f32_t fpu_fsgnj32(fpu_f32_t a, fpu_f32_t b)
{
#if defined(FPU_LIB_KNOWN_NICE_PLATFORM) && !defined(USE_SOFT_FPU_WRAP)
    return fpu_wrap_f32(fpu_copysignf_internal(fpu_raw_f32(a), fpu_raw_f32(b)));
#else
    uint32_t ia, ib;
    memcpy(&ia, &a, sizeof(a));
    memcpy(&ib, &b, sizeof(b));
    ia ^= ((ia ^ ib) & 0x80000000U);
    memcpy(&a, &ia, sizeof(a));
    return a;
#endif
}

static forceinline fpu_f64_t fpu_fsgnj64(fpu_f64_t a, fpu_f64_t b)
{
#if defined(FPU_LIB_KNOWN_NICE_PLATFORM) && !defined(USE_SOFT_FPU_WRAP)
    return fpu_wrap_f64(fpu_copysign_internal(fpu_raw_f64(a), fpu_raw_f64(b)));
#else
    uint64_t ua, ub;
    memcpy(&ua, &a, sizeof(a));
    memcpy(&ub, &b, sizeof(b));
    ua ^= ((ua ^ ub) & 0x8000000000000000ULL);
    memcpy(&a, &ua, sizeof(a));
    return a;
#endif
}

static forceinline fpu_f32_t fpu_fsgnjn32(fpu_f32_t a, fpu_f32_t b)
{
#if defined(FPU_LIB_KNOWN_NICE_PLATFORM) && !defined(USE_SOFT_FPU_WRAP)
    return fpu_wrap_f32(fpu_copysignf_internal(fpu_raw_f32(a), -fpu_raw_f32(b)));
#else
    uint32_t ia, ib;
    memcpy(&ia, &a, sizeof(a));
    memcpy(&ib, &b, sizeof(b));
    ia ^= (~(ia ^ ib) & 0x80000000U);
    memcpy(&a, &ia, sizeof(a));
    return a;
#endif
}

static forceinline fpu_f64_t fpu_fsgnjn64(fpu_f64_t a, fpu_f64_t b)
{
#if defined(FPU_LIB_KNOWN_NICE_PLATFORM) && !defined(USE_SOFT_FPU_WRAP)
    return fpu_wrap_f64(fpu_copysign_internal(fpu_raw_f64(a), -fpu_raw_f64(b)));
#else
    uint64_t ua, ub;
    memcpy(&ua, &a, sizeof(a));
    memcpy(&ub, &b, sizeof(b));
    ua ^= (~(ua ^ ub) & 0x8000000000000000ULL);
    memcpy(&a, &ua, sizeof(a));
    return a;
#endif
}

static forceinline fpu_f32_t fpu_fsgnjx32(fpu_f32_t a, fpu_f32_t b)
{
    uint32_t ia, ib;
    memcpy(&ia, &a, sizeof(a));
    memcpy(&ib, &b, sizeof(b));
    ia ^= (ib & 0x80000000U);
    memcpy(&a, &ia, sizeof(a));
    return a;
}

static forceinline fpu_f64_t fpu_fsgnjx64(fpu_f64_t a, fpu_f64_t b)
{
    uint64_t ia, ib;
    memcpy(&ia, &a, sizeof(a));
    memcpy(&ib, &b, sizeof(b));
    ia ^= (ib & 0x8000000000000000ULL);
    memcpy(&a, &ia, sizeof(a));
    return a;
}

/*
 * IEEE 754 Signaling Comparisons
 *
 * - If only one operand is a NaN, the result is false, the invalid operation exception flag is set
 * - Negative zero -0.0 is not distinguished from 0.0
 */

// Fast versions, possibly broken in case you pass an sNaN
static forceinline bool fpu_is_flt32_fast(fpu_f32_t a, fpu_f32_t b)
{
    return fpu_raw_f32(a) < fpu_raw_f32(b);
}

static forceinline bool fpu_is_flt64_fast(fpu_f64_t a, fpu_f64_t b)
{
    return fpu_raw_f64(a) < fpu_raw_f64(b);
}

static forceinline bool fpu_is_fle32_fast(fpu_f32_t a, fpu_f32_t b)
{
    return fpu_raw_f32(a) <= fpu_raw_f32(b);
}

static forceinline bool fpu_is_fle64_fast(fpu_f64_t a, fpu_f64_t b)
{
    return fpu_raw_f64(a) <= fpu_raw_f64(b);
}

static forceinline bool fpu_is_flt32_sig(fpu_f32_t a, fpu_f32_t b)
{
#if defined(FPU_LIB_KNOWN_NICE_PLATFORM)
    return fpu_is_flt32_fast(a, b);
#else
    if (fpu_is_flt32_fast(a, b)) {
        return true;
    } else if (!fpu_is_fle32_fast(b, a)) {
        fpu_raise_invalid();
    }
    return false;
#endif
}

static forceinline bool fpu_is_flt64_sig(fpu_f64_t a, fpu_f64_t b)
{
#if defined(FPU_LIB_KNOWN_NICE_PLATFORM)
    return fpu_is_flt64_fast(a, b);
#else
    if (fpu_is_flt64_fast(a, b)) {
        return true;
    } else if (!fpu_is_fle64_fast(b, a)) {
        fpu_raise_invalid();
    }
    return false;
#endif
}

static forceinline bool fpu_is_fle32_sig(fpu_f32_t a, fpu_f32_t b)
{
#if defined(FPU_LIB_KNOWN_NICE_PLATFORM)
    return fpu_is_fle32_fast(a, b);
#else
    if (fpu_is_fle32_fast(a, b)) {
        return true;
    } else if (!fpu_is_flt32_fast(b, a)) {
        fpu_raise_invalid();
    }
    return false;
#endif
}

static forceinline bool fpu_is_fle64_sig(fpu_f64_t a, fpu_f64_t b)
{
#if defined(FPU_LIB_KNOWN_NICE_PLATFORM)
    return fpu_is_fle64_fast(a, b);
#else
    if (fpu_is_fle64_fast(a, b)) {
        return true;
    } else if (!fpu_is_flt64_fast(b, a)) {
        fpu_raise_invalid();
    }
    return false;
#endif
}

/*
 * IEEE 754-2008 minNum and maxNum
 *
 * - If only one operand is a NaN, the result is the non-NaN operand
 * - Signaling NaN inputs set the invalid operation exception flag, even when the result is not NaN
 * - Negative zero -0.0 is considered smaller than 0.0
 */

static forceinline fpu_f32_t fpu_min32(fpu_f32_t a, fpu_f32_t b)
{
#if defined(FPU_LIB_KNOWN_NICE_PLATFORM) && defined(__riscv_f)
    // On RISC-V, fminf() actually behaves the way we need
    return fminf(a, b);
#else
    if (unlikely(fpu_is_nan32(a))) {
        return b;
    } else if (unlikely(fpu_is_nan32(b))) {
        return a;
    } else if (fpu_is_flt32_fast(a, b)) {
        return a;
    } else if (fpu_is_flt32_fast(b, a)) {
        return b;
    } else {
        return fpu_signbit32(a) ? a : b;
    }
#endif
}

static forceinline fpu_f32_t fpu_max32(fpu_f32_t a, fpu_f32_t b)
{
#if defined(FPU_LIB_KNOWN_NICE_PLATFORM) && defined(__riscv_f)
    return fmaxf(a, b);
#else
    if (unlikely(fpu_is_nan32(a))) {
        return b;
    } else if (unlikely(fpu_is_nan32(b))) {
        return a;
    } else if (fpu_is_flt32_fast(b, a)) {
        return a;
    } else if (fpu_is_flt32_fast(a, b)) {
        return b;
    } else {
        return fpu_signbit32(a) ? b : a;
    }
#endif
}

static forceinline fpu_f64_t fpu_min64(fpu_f64_t a, fpu_f64_t b)
{
#if defined(FPU_LIB_KNOWN_NICE_PLATFORM) && defined(__riscv_d)
    return fmin(a, b);
#else
    if (unlikely(fpu_is_nan64(a))) {
        return b;
    } else if (unlikely(fpu_is_nan64(b))) {
        return a;
    } else if (fpu_is_flt64_fast(a, b)) {
        return a;
    } else if (fpu_is_flt64_fast(b, a)) {
        return b;
    } else {
        return fpu_signbit64(a) ? a : b;
    }
#endif
}

static forceinline fpu_f64_t fpu_max64(fpu_f64_t a, fpu_f64_t b)
{
#if defined(FPU_LIB_KNOWN_NICE_PLATFORM) && defined(__riscv_d)
    return fmax(a, b);
#else
    if (unlikely(fpu_is_nan64(a))) {
        return b;
    } else if (unlikely(fpu_is_nan64(b))) {
        return a;
    } else if (fpu_is_flt64_fast(b, a)) {
        return a;
    } else if (fpu_is_flt64_fast(a, b)) {
        return b;
    } else {
        return fpu_signbit64(a) ? b : a;
    }
#endif
}

#endif
