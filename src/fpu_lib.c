/*
fpu_lib.c - Floating-point handling library
Copyright (C) 2025  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "fpu_lib.h"

#if !defined(USE_SOFT_FPU_FENV)
#if CHECK_INCLUDE(fenv.h, 1)
#include <fenv.h>
#endif

// Speed up FENV handling on x86_64; Prevent SIGILL on old x86 without SSE
#if defined(GNU_EXTS) && defined(__i386__) && !defined(__SSE__) && !defined(__FXSR__)
#define FENV_8087_IMPL 1
#elif defined(GNU_EXTS) && defined(__x86_64__) && defined(__SSE2__) && defined(__SSE2_MATH__)
#define FENV_SSE2_IMPL 1
#endif
#endif

// Thread-local raised exceptions flags for fenv access acceleration
// TODO: There are claims that TLS is broken on Win <Vista, this should be checked
// under MinGW / LLVM-MinGW / MSVC
#if defined(THREAD_LOCAL) && !defined(_WIN32)
#define FENV_TLS_IMPL 1
#endif

#if defined(FENV_TLS_IMPL)
static THREAD_LOCAL uint32_t tls_fpu_exceptions = 0;
#endif

#ifndef FE_ALL_EXCEPT
#define FE_ALL_EXCEPT 0
#endif
#ifndef FE_INEXACT
#define FE_INEXACT 0
#endif
#ifndef FE_UNDERFLOW
#define FE_UNDERFLOW 0
#endif
#ifndef FE_OVERFLOW
#define FE_OVERFLOW 0
#endif
#ifndef FE_DIVBYZERO
#define FE_DIVBYZERO 0
#endif
#ifndef FE_INVALID
#define FE_INVALID 0
#endif
#ifndef FE_TONEAREST
#define FE_TONEAREST 0
#endif
#ifndef FE_TOWARDZERO
#define FE_TOWARDZERO 0
#endif
#ifndef FE_DOWNWARD
#define FE_DOWNWARD 0
#endif
#ifndef FE_UPWARD
#define FE_UPWARD 0
#endif

static uint32_t fpu_get_exceptions_internal(void)
{
    uint32_t ret = 0;
#if defined(FENV_8087_IMPL) || defined(FENV_SSE2_IMPL)
#if defined(FENV_SSE2_IMPL)
    uint32_t stsw = 0;
    __asm__ volatile("stmxcsr %0" : "=m"(*&stsw) : : "memory");
#else
    uint16_t stsw = 0;
    __asm__ volatile("fnstsw %0" : "=a"(stsw) : : "memory");
#endif
    if (unlikely(stsw & 0x3D)) {
        if (stsw & 0x01) {
            ret |= FPU_ENV_FLAG_NV;
        }
        if (stsw & 0x04) {
            ret |= FPU_ENV_FLAG_DZ;
        }
        if (stsw & 0x08) {
            ret |= FPU_ENV_FLAG_OF;
        }
        if (stsw & 0x10) {
            ret |= FPU_ENV_FLAG_UF;
        }
        if (stsw & 0x20) {
            ret |= FPU_ENV_FLAG_NX;
        }
    }
#elif !defined(USE_SOFT_FPU_FENV)
    uint32_t fenv = fetestexcept(FE_ALL_EXCEPT);
    if (unlikely(fenv)) {
        if (fenv & FE_INEXACT) {
            ret |= FPU_ENV_FLAG_NX;
        }
        if (fenv & FE_UNDERFLOW) {
            ret |= FPU_ENV_FLAG_UF;
        }
        if (fenv & FE_OVERFLOW) {
            ret |= FPU_ENV_FLAG_OF;
        }
        if (fenv & FE_DIVBYZERO) {
            ret |= FPU_ENV_FLAG_DZ;
        }
        if (fenv & FE_INVALID) {
            ret |= FPU_ENV_FLAG_NV;
        }
    }
#endif
    return ret;
}

static void fpu_set_exceptions_internal(uint32_t exceptions)
{
#if defined(FENV_8087_IMPL) || defined(FENV_SSE2_IMPL)
    uint32_t stsw = 0;
#if defined(FENV_SSE2_IMPL)
    __asm__ volatile("stmxcsr %0" : "=m"(*&stsw) : : "memory");
#else
    uint16_t fenv_8087[32] = ZERO_INIT;
    __asm__ volatile("fnstenv %0" : "=m"(*&fenv_8087) : : "memory");
    stsw = fenv_8087[2];
#endif
    stsw &= ~0x3DU;
    if (unlikely(exceptions)) {
        if (exceptions & FPU_ENV_FLAG_NX) {
            stsw |= 0x20;
        }
        if (exceptions & FPU_ENV_FLAG_UF) {
            stsw |= 0x10;
        }
        if (exceptions & FPU_ENV_FLAG_OF) {
            stsw |= 0x08;
        }
        if (exceptions & FPU_ENV_FLAG_DZ) {
            stsw |= 0x04;
        }
        if (exceptions & FPU_ENV_FLAG_NV) {
            stsw |= 0x01;
        }
    }
#if defined(FENV_SSE2_IMPL)
    __asm__ volatile("ldmxcsr %0" : : "m"(*&stsw) : "memory");
#else
    fenv_8087[2] = stsw;
    __asm__ volatile("fldenv %0" : : "m"(*&fenv_8087) : "memory");
#endif
#elif !defined(USE_SOFT_FPU_FENV)
    uint32_t fenv = 0;
    feclearexcept(FE_ALL_EXCEPT);
    if (unlikely(exceptions)) {
        if (exceptions & FPU_ENV_FLAG_NX) {
            fenv |= FE_INEXACT;
        }
        if (exceptions & FPU_ENV_FLAG_UF) {
            fenv |= FE_UNDERFLOW;
        }
        if (exceptions & FPU_ENV_FLAG_OF) {
            fenv |= FE_OVERFLOW;
        }
        if (exceptions & FPU_ENV_FLAG_DZ) {
            fenv |= FE_DIVBYZERO;
        }
        if (exceptions & FPU_ENV_FLAG_NV) {
            fenv |= FE_INVALID;
        }
        if (fenv) {
            feraiseexcept(fenv);
        }
    }
#endif
    UNUSED(exceptions);
}

uint32_t fpu_get_exceptions(void)
{
#if defined(FENV_TLS_IMPL)
    return fpu_get_exceptions_internal() | tls_fpu_exceptions;
#else
    return fpu_get_exceptions_internal();
#endif
}

void fpu_set_exceptions(uint32_t exceptions)
{
#if defined(FENV_TLS_IMPL)
    tls_fpu_exceptions = exceptions;
    if (fpu_get_exceptions_internal() & ~exceptions) {
        fpu_set_exceptions_internal(0);
    }
#else
    fpu_set_exceptions_internal(exceptions);
#endif
}

void fpu_raise_exceptions(uint32_t exceptions)
{
    exceptions &= FPU_ENV_FLAGS_ALL;
#if defined(FENV_TLS_IMPL)
    tls_fpu_exceptions |= exceptions;
#else
    if (exceptions) {
        uint32_t tmp = fpu_get_exceptions_internal();
        if (~tmp & exceptions) {
            fpu_set_exceptions_internal(tmp | exceptions);
        }
    }
#endif
}

void fpu_clear_exceptions(uint32_t exceptions)
{
    exceptions &= FPU_ENV_FLAGS_ALL;
#if defined(FENV_TLS_IMPL)
    tls_fpu_exceptions &= ~exceptions;
    if (fpu_get_exceptions_internal() & exceptions) {
        fpu_set_exceptions_internal(0);
    }
#else
    if (exceptions == FPU_ENV_FLAGS_ALL) {
        fpu_set_exceptions(0);
    } else {
        uint32_t tmp = fpu_get_exceptions_internal();
        if (tmp & exceptions) {
            fpu_set_exceptions_internal(tmp & ~exceptions);
        }
    }
#endif
}

uint32_t fpu_get_rounding_mode(void)
{
#if !defined(USE_SOFT_FPU_FENV)
    uint32_t frm = fegetround();
    switch (frm) {
        case FE_DOWNWARD:
            return FPU_ENV_ROUND_DN;
        case FE_UPWARD:
            return FPU_ENV_ROUND_UP;
        case FE_TOWARDZERO:
            return FPU_ENV_ROUND_TZ;
    }
#endif
    return FPU_ENV_ROUND_NE;
}

void fpu_set_rounding_mode(uint32_t mode)
{
    UNUSED(mode);
#if !defined(USE_SOFT_FPU_FENV)
    switch (mode) {
        case FPU_ENV_ROUND_DN:
            fesetround(FE_DOWNWARD);
            break;
        case FPU_ENV_ROUND_UP:
            fesetround(FE_UPWARD);
            break;
        case FPU_ENV_ROUND_TZ:
            fesetround(FE_TOWARDZERO);
            break;
        default:
            fesetround(FE_TONEAREST);
            break;
    }
#endif
}

slow_path void fpu_raise_invalid(void)
{
    fpu_raise_exceptions(FPU_ENV_FLAG_NV);
}

slow_path void fpu_raise_inexact(void)
{
    fpu_raise_exceptions(FPU_ENV_FLAG_NX);
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
