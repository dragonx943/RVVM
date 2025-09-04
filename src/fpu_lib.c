/*
fpu_lib.c - Floating-point handling library
Copyright (C) 2025  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "fpu_lib.h"

PUSH_OPTIMIZATION_SIZE

#if !defined(USE_SOFT_FPU_FENV)

#include <fenv.h>

// Check presence of FE_ALL_EXCEPT and at least one exception bit flag
#if defined(FE_ALL_EXCEPT)                                                                                             \
    && (defined(FE_INEXACT) || defined(FE_UNDERFLOW) /**/                                                              \
        || defined(FE_OVERFLOW) || defined(FE_DIVBYZERO) || defined(FE_INVALID))
#define FENV_EXCEPTIONS_IMPL 1
#endif

// Check presence and at least one rounding mode other than FE_TONEAREST
#if defined(FE_DOWNWARD) || defined(FE_UPWARD) || defined(FE_TOWARDZERO)
#define FENV_ROUNDING_IMPL 1
#endif

#if !defined(USE_FPU_WORKAROUNDS) && defined(GNU_EXTS) && defined(__i386__) /**/                                       \
    && !defined(__SSE__) && !defined(__SSE_MATH__) && !defined(__FXSR__)
// Prevent SIGILL on old x86 without SSE
#define FENV_8087_IMPL 1
#elif !defined(USE_FPU_WORKAROUNDS) && defined(GNU_EXTS) && defined(__x86_64__) /**/                                   \
    && defined(__SSE2__) && defined(__SSE2_MATH__)
// Speed up FENV handling on x86_64 with SSE2
#define FENV_SSE2_IMPL 1
#endif

#endif

#if defined(THREAD_LOCAL)

static THREAD_LOCAL uint32_t fpu_exceptions = 0;
static THREAD_LOCAL uint32_t fpu_round_mode = FPU_LIB_ROUND_NE;

#define FENV_TLS_IMPL 1

#elif !defined(FENV_EXCEPTIONS_IMPL) && !defined(FENV_8087_IMPL) && !defined(FENV_SSE2_IMPL)

// Non thread-safe fpu environment emulation, will work fine for single-core guests
static uint32_t fpu_exceptions = 0;
static uint32_t fpu_round_mode = 0;

#define FENV_TLS_IMPL 1

#endif

#if defined(FENV_8087_IMPL) || defined(FENV_SSE2_IMPL)

static inline uint32_t fpu_x86_sw_to_exceptions(uint32_t sw)
{
    uint32_t ret = 0;
    if (sw & 0x01U) {
        ret |= FPU_LIB_FLAG_NV;
    }
    if (sw & 0x04U) {
        ret |= FPU_LIB_FLAG_DZ;
    }
    if (sw & 0x08U) {
        ret |= FPU_LIB_FLAG_OF;
    }
    if (sw & 0x10U) {
        ret |= FPU_LIB_FLAG_UF;
    }
    if (sw & 0x20U) {
        ret |= FPU_LIB_FLAG_NX;
    }
    return ret;
}

static inline uint32_t fpu_exceptions_to_x86_sw(uint32_t exceptions)
{
    uint32_t ret = 0;
    if (exceptions & FPU_LIB_FLAG_NV) {
        ret |= 0x01U;
    }
    if (exceptions & FPU_LIB_FLAG_DZ) {
        ret |= 0x04U;
    }
    if (exceptions & FPU_LIB_FLAG_OF) {
        ret |= 0x08U;
    }
    if (exceptions & FPU_LIB_FLAG_UF) {
        ret |= 0x10U;
    }
    if (exceptions & FPU_LIB_FLAG_NX) {
        ret |= 0x20U;
    }
    return ret;
}

#elif defined(FENV_EXCEPTIONS_IMPL)

static inline uint32_t fpu_fenv_to_exceptions(uint32_t fenv)
{
    uint32_t ret = 0;
#if defined(FE_INEXACT)
    if (fenv & FE_INEXACT) {
        ret |= FPU_LIB_FLAG_NX;
    }
#endif
#if defined(FE_UNDERFLOW)
    if (fenv & FE_UNDERFLOW) {
        ret |= FPU_LIB_FLAG_UF;
    }
#endif
#if defined(FE_OVERFLOW)
    if (fenv & FE_OVERFLOW) {
        ret |= FPU_LIB_FLAG_OF;
    }
#endif
#if defined(FE_DIVBYZERO)
    if (fenv & FE_DIVBYZERO) {
        ret |= FPU_LIB_FLAG_DZ;
    }
#endif
#if defined(FE_INVALID)
    if (fenv & FE_INVALID) {
        ret |= FPU_LIB_FLAG_NV;
    }
#endif
    return ret;
}

static inline uint32_t fpu_exceptions_to_fenv(uint32_t exceptions)
{
    uint32_t ret = 0;
#if defined(FE_INEXACT)
    if (exceptions & FPU_LIB_FLAG_NX) {
        ret |= FE_INEXACT;
    }
#endif
#if defined(FE_UNDERFLOW)
    if (exceptions & FPU_LIB_FLAG_UF) {
        ret |= FE_UNDERFLOW;
    }
#endif
#if defined(FE_OVERFLOW)
    if (exceptions & FPU_LIB_FLAG_OF) {
        ret |= FE_OVERFLOW;
    }
#endif
#if defined(FE_DIVBYZERO)
    if (exceptions & FPU_LIB_FLAG_DZ) {
        ret |= FE_DIVBYZERO;
    }
#endif
#if defined(FE_DIVBYZERO)
    if (exceptions & FPU_LIB_FLAG_NV) {
        ret |= FE_INVALID;
    }
#endif
    return ret;
}

#endif

static uint32_t fpu_pump_exceptions_internal(uint32_t set_exceptions, uint32_t clr_exceptions)
{
#if defined(FENV_SSE2_IMPL)
    uint32_t sw = 0, nsw = 0;
    __asm__ __volatile__("stmxcsr %0" : "=m"(*&sw) : : "memory");
    // Disable DAZ/FTZ, mask all exceptions just to be on the safe size
    nsw = (sw & ~0x8040U) | 0x1F80U;
    if (set_exceptions) {
        nsw |= fpu_exceptions_to_x86_sw(set_exceptions);
    }
    if (clr_exceptions) {
        nsw &= ~fpu_exceptions_to_x86_sw(clr_exceptions);
    }
    if (unlikely(nsw != sw)) {
        __asm__ __volatile__("ldmxcsr %0" : : "m"(*&nsw) : "memory");
    }
    return fpu_x86_sw_to_exceptions(sw);
#elif defined(FENV_8087_IMPL)
    uint16_t fenv_8087[32] = ZERO_INIT, sw = 0, nsw = 0;
    if (set_exceptions || clr_exceptions) {
        __asm__ __volatile__("fnstenv %0" : "=m"(*&fenv_8087) : : "memory");
        sw  = fenv_8087[2];
        nsw = sw;
        if (set_exceptions) {
            nsw |= fpu_exceptions_to_x86_sw(set_exceptions);
        }
        if (clr_exceptions) {
            nsw &= ~fpu_exceptions_to_x86_sw(clr_exceptions);
        }
        if (unlikely(nsw != sw)) {
            fenv_8087[2] = nsw;
            __asm__ __volatile__("fldenv %0" : : "m"(*&fenv_8087) : "memory");
        }
    } else {
        // Simply read status word
        __asm__ __volatile__("fnstsw %0" : "=a"(sw) : : "memory");
    }
    return fpu_x86_sw_to_exceptions(sw);
#elif defined(FENV_EXCEPTIONS_IMPL)
    uint32_t ret    = fpu_fenv_to_exceptions(fetestexcept(FE_ALL_EXCEPT));
    set_exceptions &= ~ret;
    clr_exceptions &= ret;
    if (set_exceptions) {
        feraiseexcept(fpu_exceptions_to_fenv(set_exceptions));
    }
    if (clr_exceptions) {
        feclearexcept(fpu_exceptions_to_fenv(clr_exceptions));
    }
    return ret;
#else
    UNUSED(set_exceptions);
    UNUSED(clr_exceptions);
    return 0;
#endif
}

static uint32_t fpu_pump_rounding_mode_internal(uint32_t mode)
{
#if defined(FENV_SSE2_IMPL)
    uint32_t cw = 0, ncw = 0;
    __asm__ __volatile__("stmxcsr %0" : "=m"(*&cw) : : "memory");
    // Disable DAZ/FTZ, mask all exceptions just to be on the safe size
    ncw = (cw & ~0x8040U) | 0x1F80U;
    if (mode <= FPU_LIB_ROUND_MM) {
        ncw &= ~0x6000U;
        switch (mode) {
            case FPU_LIB_ROUND_DN:
                ncw |= 0x2000U;
                break;
            case FPU_LIB_ROUND_UP:
                ncw |= 0x4000U;
                break;
            case FPU_LIB_ROUND_TZ:
                ncw |= 0x6000U;
                break;
        }
        if (ncw != cw) {
            __asm__ __volatile__("ldmxcsr %0" : : "m"(*&ncw) : "memory");
        }
    }
    switch (cw & 0x6000U) {
        case 0x2000:
            return FPU_LIB_ROUND_DN;
        case 0x4000U:
            return FPU_LIB_ROUND_UP;
        case 0x6000U:
            return FPU_LIB_ROUND_TZ;
    }
    return FPU_LIB_ROUND_NE;
#elif defined(FENV_8087_IMPL)
    uint16_t cw = 0, ncw = 0;
    __asm__ __volatile__("fnstcw %0" : "=m"(*&cw) : : "memory");
    if (mode <= FPU_LIB_ROUND_MM) {
        ncw = cw & ~0x0C00U;
        switch (mode) {
            case FPU_LIB_ROUND_DN:
                ncw |= 0x0400U;
                break;
            case FPU_LIB_ROUND_UP:
                ncw |= 0x0800U;
                break;
            case FPU_LIB_ROUND_TZ:
                ncw |= 0x0C00U;
                break;
        }
        if (ncw != cw) {
            __asm__ __volatile__("fldcw %0" : : "m"(*&ncw) : "memory");
        }
    }
    switch (cw & 0x0C00U) {
        case 0x0400U:
            return FPU_LIB_ROUND_DN;
        case 0x0800U:
            return FPU_LIB_ROUND_UP;
        case 0x0C00U:
            return FPU_LIB_ROUND_TZ;
    }
    return FPU_LIB_ROUND_NE;
#elif defined(FENV_ROUNDING_IMPL)
    uint32_t ret = FPU_LIB_ROUND_NE;
    switch (fegetround()) {
#if defined(FE_DOWNWARD)
        case FE_DOWNWARD:
            ret = FPU_LIB_ROUND_DN;
            break;
#endif
#if defined(FE_UPWARD)
        case FE_UPWARD:
            ret = FPU_LIB_ROUND_UP;
            break;
#endif
#if defined(FE_TOWARDZERO)
        case FE_TOWARDZERO:
            ret = FPU_LIB_ROUND_TZ;
            break;
#endif
    }
    if (mode <= FPU_LIB_ROUND_MM && mode != ret) {
        switch (mode) {
#if defined(FE_DOWNWARD)
            case FPU_LIB_ROUND_DN:
                fesetround(FE_DOWNWARD);
                break;
#endif
#if defined(FE_UPWARD)
            case FPU_LIB_ROUND_UP:
                fesetround(FE_UPWARD);
                break;
#endif
#if defined(FE_TOWARDZERO)
            case FPU_LIB_ROUND_TZ:
                fesetround(FE_TOWARDZERO);
                break;
#endif
#if defined(FE_TONEAREST)
            default:
                fesetround(FE_TONEAREST);
                break;
#endif
        }
    }
    return ret;
#else
    UNUSED(mode);
    return FPU_LIB_ROUND_NE;
#endif
}

uint32_t fpu_get_exceptions(void)
{
#if defined(FENV_TLS_IMPL)
    fpu_exceptions |= fpu_pump_exceptions_internal(0, FPU_LIB_FLAGS_ALL);
    return fpu_exceptions;
#else
    return fpu_pump_exceptions_internal(0, 0);
#endif
}

void fpu_set_exceptions(uint32_t new_exceptions)
{
    new_exceptions &= FPU_LIB_FLAGS_ALL;
#if defined(FENV_TLS_IMPL)
    fpu_pump_exceptions_internal(0, FPU_LIB_FLAGS_ALL);
    fpu_exceptions = new_exceptions;
#else
    fpu_pump_exceptions_internal(new_exceptions, ~new_exceptions);
#endif
}

void fpu_raise_exceptions(uint32_t set_exceptions)
{
    set_exceptions &= FPU_LIB_FLAGS_ALL;
#if defined(FENV_TLS_IMPL)
    fpu_exceptions |= set_exceptions;
#else
    fpu_pump_exceptions_internal(set_exceptions, 0);
#endif
}

void fpu_clear_exceptions(uint32_t clr_exceptions)
{
    clr_exceptions &= FPU_LIB_FLAGS_ALL;
#if defined(FENV_TLS_IMPL)
    fpu_exceptions |= fpu_pump_exceptions_internal(0, FPU_LIB_FLAGS_ALL);
    fpu_exceptions &= ~clr_exceptions;
#else
    fpu_pump_exceptions_internal(0, clr_exceptions);
#endif
}

uint32_t fpu_get_rounding_mode(void)
{
#if defined(FENV_TLS_IMPL)
    return fpu_round_mode;
#else
    return fpu_pump_rounding_mode_internal(-1);
#endif
}

void fpu_set_rounding_mode(uint32_t mode)
{
    if (mode > FPU_LIB_ROUND_MM) {
        mode = FPU_LIB_ROUND_NE;
    }
#if defined(FENV_TLS_IMPL)
    if (fpu_round_mode != mode) {
        fpu_round_mode = mode;
        fpu_pump_rounding_mode_internal(mode);
    }
#else
    fpu_pump_rounding_mode_internal(mode);
#endif
}

slow_path void fpu_raise_invalid(void)
{
    fpu_raise_exceptions(FPU_LIB_FLAG_NV);
}

slow_path void fpu_raise_inexact(void)
{
    fpu_raise_exceptions(FPU_LIB_FLAG_NX);
}

slow_path void fpu_raise_ovrflow(void)
{
    fpu_raise_exceptions(FPU_LIB_FLAG_OF);
}

slow_path void fpu_raise_divzero(void)
{
    fpu_raise_exceptions(FPU_LIB_FLAG_DZ);
}

slow_path uint32_t fpu_fclass32(fpu_f32_t f)
{
    uint32_t s = fpu_bit_f32_to_u32(f);
    uint32_t u = s << 1;
    uint32_t ret;
    if (!u) {
        ret = FPU_CLASS_POS_ZERO;
    } else {
        uint32_t exp = u >> 24;
        if (!exp) {
            ret = FPU_CLASS_POS_SUBNORMAL;
        } else if (exp < 0xFFU) {
            ret = FPU_CLASS_POS_NORMAL;
        } else if (!(s << 9)) {
            ret = FPU_CLASS_POS_INF;
        } else {
            return (s & FPU_LIB_FP32_QUIETNAN_MASK) ? FPU_CLASS_NAN_QUIET : FPU_CLASS_NAN_SIG;
        }
    }
    if (s >> 31) {
        ret ^= FPU_CLASS_POS_INF;
    }
    return ret;
}

slow_path uint32_t fpu_fclass64(fpu_f64_t d)
{
    uint64_t s = fpu_bit_f64_to_u64(d);
    uint64_t u = s << 1;
    uint32_t ret;
    if (!u) {
        ret = FPU_CLASS_POS_ZERO;
    } else {
        uint32_t exp = u >> 53;
        if (!exp) {
            ret = FPU_CLASS_POS_SUBNORMAL;
        } else if (exp < 0x7FFU) {
            ret = FPU_CLASS_POS_NORMAL;
        } else if (!(s << 12)) {
            ret = FPU_CLASS_POS_INF;
        } else {
            return (s & FPU_LIB_FP64_QUIETNAN_MASK) ? FPU_CLASS_NAN_QUIET : FPU_CLASS_NAN_SIG;
        }
    }
    if (s >> 63) {
        ret ^= FPU_CLASS_POS_INF;
    }
    return ret;
}

slow_path fpu_f32_t fpu_round_f32_internal(fpu_f32_t f, uint32_t mode)
{
    uint32_t u = fpu_bit_f32_to_u32(f);
    uint32_t s = u & FPU_LIB_FP32_SIGNEDFP_MASK;
    if (unlikely(mode > FPU_LIB_ROUND_UP)) {
        mode = fpu_get_rounding_mode();
    }
    switch (mode) {
        case FPU_LIB_ROUND_NE:
        case FPU_LIB_ROUND_MM:
            return fpu_add32(f, fpu_bit_u32_to_f32(0x3F000000U | s));
        case FPU_LIB_ROUND_DN:
            if (s && fpu_is_fractional32(f)) {
                return fpu_sub32(f, fpu_bit_u32_to_f32(0x3F800000U));
            }
            break;
        case FPU_LIB_ROUND_UP:
            if (!s && fpu_is_fractional32(f)) {
                return fpu_add32(f, fpu_bit_u32_to_f32(0x3F800000U));
            }
            break;
    }
    return f;
}

slow_path fpu_f64_t fpu_round_f64_internal(fpu_f64_t d, uint32_t mode)
{
    uint64_t u = fpu_bit_f64_to_u64(d);
    uint64_t s = u & FPU_LIB_FP64_SIGNEDFP_MASK;
    if (unlikely(mode > FPU_LIB_ROUND_UP)) {
        mode = fpu_get_rounding_mode();
    }
    switch (mode) {
        case FPU_LIB_ROUND_NE:
        case FPU_LIB_ROUND_MM:
            return fpu_add64(d, fpu_bit_u64_to_f64(0x3FE0000000000000ULL | s));
        case FPU_LIB_ROUND_DN:
            if (s && fpu_is_fractional64(d)) {
                return fpu_sub64(d, fpu_bit_u64_to_f64(0x3FF0000000000000ULL));
            }
            break;
        case FPU_LIB_ROUND_UP:
            if (!s && fpu_is_fractional64(d)) {
                return fpu_add64(d, fpu_bit_u64_to_f64(0x3FF0000000000000ULL));
            }
            break;
    }
    return d;
}

#if defined(USE_SOFT_FPU_FENV)

slow_path void fpu_soft_fenv_check_add32(fpu_f32_t sum, fpu_f32_t a, fpu_f32_t b)
{
    if (unlikely(!fpu_is_finite32(sum))) {
        if (fpu_raw_f32(sum) != fpu_raw_f32(sum)) {
            fpu_raise_invalid();
        } else {
            fpu_raise_ovrflow();
        }
    } else if (unlikely(fpu_bit_f32_to_u32(fpu_add_error32(sum, a, b)))) {
        fpu_raise_inexact();
    }
}

slow_path void fpu_soft_fenv_check_add64(fpu_f64_t sum, fpu_f64_t a, fpu_f64_t b)
{
    if (unlikely(!fpu_is_finite64(sum))) {
        if (fpu_raw_f64(sum) != fpu_raw_f64(sum)) {
            fpu_raise_invalid();
        } else {
            fpu_raise_ovrflow();
        }
    } else if (unlikely(fpu_bit_f64_to_u64(fpu_add_error64(sum, a, b)))) {
        fpu_raise_inexact();
    }
}

slow_path void fpu_soft_fenv_check_mul32(fpu_f32_t mul, fpu_f32_t a, fpu_f32_t b, bool is_div)
{
    if (unlikely(!fpu_is_finite32(mul))) {
        if (fpu_raw_f32(mul) != fpu_raw_f32(mul)) {
            fpu_raise_invalid();
        } else if (is_div) {
            fpu_raise_divzero();
        } else {
            fpu_raise_ovrflow();
        }
    } else if (unlikely(fpu_bit_f32_to_u32(fpu_mul_error32(mul, a, b)))) {
        fpu_raise_inexact();
    }
}

slow_path void fpu_soft_fenv_check_mul64(fpu_f64_t sum, fpu_f64_t a, fpu_f64_t b, bool is_div)
{
    if (unlikely(!fpu_is_finite64(sum))) {
        if (fpu_raw_f64(sum) != fpu_raw_f64(sum)) {
            fpu_raise_invalid();
        } else if (is_div) {
            fpu_raise_divzero();
        } else {
            fpu_raise_ovrflow();
        }
    } else if (unlikely(fpu_bit_f64_to_u64(fpu_mul_error64(sum, a, b)))) {
        fpu_raise_inexact();
    }
}

#endif

POP_OPTIMIZATION_SIZE
