/*
fpu_lib.c - Floating-point handling library
Copyright (C) 2025  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "fpu_lib.h"

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

// Speed up FENV handling on x86_64; Prevent SIGILL on old x86 without SSE
#if defined(GNU_EXTS) && defined(__i386__) && !defined(__SSE__) && !defined(__FXSR__)
#define FENV_8087_IMPL 1
#elif defined(GNU_EXTS) && defined(__x86_64__) && defined(__SSE2__) && defined(__SSE2_MATH__)
#define FENV_SSE2_IMPL 1
#endif

#endif

#if defined(THREAD_LOCAL)
static THREAD_LOCAL uint32_t fpu_exceptions = 0;
#else
// Non thread-safe fpu exceptions shared across threads, will work fine for single-core guests
static uint32_t fpu_exceptions = 0;
#endif

static uint32_t fpu_get_exceptions_internal(void)
{
    uint32_t ret = 0;
#if defined(FENV_8087_IMPL) || defined(FENV_SSE2_IMPL)
#if defined(FENV_SSE2_IMPL)
    uint32_t sw = 0;
    __asm__ volatile("stmxcsr %0" : "=m"(*&sw) : : "memory");
#else
    uint16_t sw = 0;
    __asm__ volatile("fnstsw %0" : "=a"(sw) : : "memory");
#endif
    if (unlikely(sw & 0x3D)) {
        if (sw & 0x01) {
            ret |= FPU_LIB_FLAG_NV;
        }
        if (sw & 0x04) {
            ret |= FPU_LIB_FLAG_DZ;
        }
        if (sw & 0x08) {
            ret |= FPU_LIB_FLAG_OF;
        }
        if (sw & 0x10) {
            ret |= FPU_LIB_FLAG_UF;
        }
        if (sw & 0x20) {
            ret |= FPU_LIB_FLAG_NX;
        }
    }
#elif defined(FENV_EXCEPTIONS_IMPL)
    uint32_t fenv = fetestexcept(FE_ALL_EXCEPT);
    if (unlikely(fenv)) {
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
    }
#endif
    return ret;
}


static void fpu_set_exceptions_internal(uint32_t exceptions)
{
#if defined(FENV_8087_IMPL) || defined(FENV_SSE2_IMPL)
    uint32_t sw = 0, nsw = 0;
#if defined(FENV_SSE2_IMPL)
    __asm__ volatile("stmxcsr %0" : "=m"(*&sw) : : "memory");
#else
    uint16_t fenv_8087[32] = ZERO_INIT;
    __asm__ volatile("fnstenv %0" : "=m"(*&fenv_8087) : : "memory");
    sw = fenv_8087[2];
#endif
    nsw = sw & ~0x0000003DU;
    if (unlikely(exceptions)) {
        if (exceptions & FPU_LIB_FLAG_NV) {
            nsw |= 0x01;
        }
        if (exceptions & FPU_LIB_FLAG_DZ) {
            nsw |= 0x04;
        }
        if (exceptions & FPU_LIB_FLAG_OF) {
            nsw |= 0x08;
        }
        if (exceptions & FPU_LIB_FLAG_UF) {
            nsw |= 0x10;
        }
        if (exceptions & FPU_LIB_FLAG_NX) {
            nsw |= 0x20;
        }
    }
    if (nsw != sw) {
#if defined(FENV_SSE2_IMPL)
        __asm__ volatile("ldmxcsr %0" : : "m"(*&nsw) : "memory");
#else
        fenv_8087[2] = nsw;
        __asm__ volatile("fldenv %0" : : "m"(*&fenv_8087) : "memory");
#endif
    }
#elif defined(FENV_EXCEPTIONS_IMPL)
    uint32_t fenv = 0;
    feclearexcept(FE_ALL_EXCEPT);
    if (unlikely(exceptions)) {
#if defined(FE_INEXACT)
        if (exceptions & FPU_LIB_FLAG_NX) {
            fenv |= FE_INEXACT;
        }
#endif
#if defined(FE_UNDERFLOW)
        if (exceptions & FPU_LIB_FLAG_UF) {
            fenv |= FE_UNDERFLOW;
        }
#endif
#if defined(FE_OVERFLOW)
        if (exceptions & FPU_LIB_FLAG_OF) {
            fenv |= FE_OVERFLOW;
        }
#endif
#if defined(FE_DIVBYZERO)
        if (exceptions & FPU_LIB_FLAG_DZ) {
            fenv |= FE_DIVBYZERO;
        }
#endif
#if defined(FE_DIVBYZERO)
        if (exceptions & FPU_LIB_FLAG_NV) {
            fenv |= FE_INVALID;
        }
#endif
        if (fenv) {
            feraiseexcept(fenv);
        }
    }
#endif
    UNUSED(exceptions);
}

uint32_t fpu_get_exceptions(void)
{
    return fpu_get_exceptions_internal() | fpu_exceptions;
}

void fpu_set_exceptions(uint32_t exceptions)
{
    fpu_exceptions = exceptions;
    if (fpu_get_exceptions_internal() & ~exceptions) {
        fpu_set_exceptions_internal(0);
    }
}

void fpu_raise_exceptions(uint32_t exceptions)
{
    fpu_exceptions |= (exceptions & FPU_ENV_FLAGS_ALL);
}

void fpu_clear_exceptions(uint32_t exceptions)
{
    exceptions &= FPU_ENV_FLAGS_ALL;
    fpu_exceptions &= ~exceptions;
    if (fpu_get_exceptions_internal() & exceptions) {
        fpu_set_exceptions_internal(0);
    }
}

uint32_t fpu_get_rounding_mode(void)
{
#if defined(FENV_8087_IMPL)
    uint16_t cw = 0;
    __asm__ volatile("fnstcw %0" : "=m"(*&cw) : : "memory");
    if ((cw & 0x0C00U) == 0x0C00U) {
        return FPU_LIB_ROUND_TZ;
    } else if (cw & 0x0400U) {
        return FPU_LIB_ROUND_DN;
    } else if (cw & 0x0800U) {
        return FPU_LIB_ROUND_UP;
    }
#elif defined(FENV_ROUNDING_IMPL)
    uint32_t frm = fegetround();
    switch (frm) {
#if defined(FE_DOWNWARD)
        case FE_DOWNWARD:
            return FPU_LIB_ROUND_DN;
#endif
#if defined(FE_UPWARD)
        case FE_UPWARD:
            return FPU_LIB_ROUND_UP;
#endif
#if defined(FE_TOWARDZERO)
        case FE_TOWARDZERO:
            return FPU_LIB_ROUND_TZ;
#endif
    }
#endif
    return FPU_LIB_ROUND_NE;
}

void fpu_set_rounding_mode(uint32_t mode)
{
#if defined(FENV_8087_IMPL)
    uint16_t cw = 0, ncw = 0;
    __asm__ volatile("fnstcw %0" : "=m"(*&cw) : : "memory");
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
        __asm__ volatile("fldcw %0" : : "m"(*&ncw) : "memory");
    }
#elif defined(FENV_ROUNDING_IMPL)
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
#endif
    UNUSED(mode);
}

#define FPU_FP32_EXPONENT_MASK 0x7F800000U
#define FPU_FP32_MANTISSA_MASK 0x007FFFFFU
#define FPU_FP32_QUIETNAN_MASK 0x00400000U
#define FPU_FP32_SIGNEDFP_MASK 0x80000000U

#define FPU_FP64_EXPONENT_MASK 0x7FF0000000000000ULL
#define FPU_FP64_MANTISSA_MASK 0x000FFFFFFFFFFFFFULL
#define FPU_FP64_QUIETNAN_MASK 0x0008000000000000ULL
#define FPU_FP64_SIGNEDFP_MASK 0x8000000000000000ULL

slow_path uint32_t fpu_fclass32(fpu_f32_t f)
{
    uint32_t u    = fpu_bit_f32_to_u32(f);
    uint32_t exp  = u & FPU_FP32_EXPONENT_MASK;
    uint32_t mant = u & FPU_FP32_MANTISSA_MASK;
    if (exp == FPU_FP32_EXPONENT_MASK) {
        if (mant) {
            return (u & FPU_FP32_QUIETNAN_MASK) ? FPU_CLASS_NAN_QUIET : FPU_CLASS_NAN_SIG;
        } else {
            return (u & FPU_FP32_SIGNEDFP_MASK) ? FPU_CLASS_NEG_INF : FPU_CLASS_POS_INF;
        }
    } else if (!exp && mant) {
        return (u & FPU_FP32_SIGNEDFP_MASK) ? FPU_CLASS_NEG_SUBNORMAL : FPU_CLASS_POS_SUBNORMAL;
    } else if (u << 1) {
        return (u & FPU_FP32_SIGNEDFP_MASK) ? FPU_CLASS_NEG_NORMAL : FPU_CLASS_POS_NORMAL;
    }
    return (u & FPU_FP32_SIGNEDFP_MASK) ? FPU_CLASS_NEG_ZERO : FPU_CLASS_POS_ZERO;
}

slow_path uint32_t fpu_fclass64(fpu_f64_t d)
{
    uint64_t u    = fpu_bit_f64_to_u64(d);
    uint64_t exp  = u & FPU_FP64_EXPONENT_MASK;
    uint64_t mant = u & FPU_FP64_MANTISSA_MASK;
    if (exp == FPU_FP64_EXPONENT_MASK) {
        // Infinity or NaN
        if (mant) {
            // NaN
            return (u & FPU_FP64_QUIETNAN_MASK) ? FPU_CLASS_NAN_QUIET : FPU_CLASS_NAN_SIG;
        } else {
            return (u & FPU_FP64_SIGNEDFP_MASK) ? FPU_CLASS_NEG_INF : FPU_CLASS_POS_INF;
        }
    } else if (!exp && mant) {
        return (u & FPU_FP64_SIGNEDFP_MASK) ? FPU_CLASS_NEG_SUBNORMAL : FPU_CLASS_POS_SUBNORMAL;
    } else if (u << 1) {
        return (u & FPU_FP64_SIGNEDFP_MASK) ? FPU_CLASS_NEG_NORMAL : FPU_CLASS_POS_NORMAL;
    }
    return (u & FPU_FP64_SIGNEDFP_MASK) ? FPU_CLASS_NEG_ZERO : FPU_CLASS_POS_ZERO;
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
