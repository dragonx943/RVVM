/*
riscv_common.h - RISC-V Interpreter helpers
Copyright (C) 2024  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_RISCV_COMMON_H
#define RVVM_RISCV_COMMON_H

#include "bit_ops.h"    // IWYU pragma: keep
#include "compiler.h"   // IWYU pragma: keep
#include "riscv_cpu.h"  // IWYU pragma: keep
#include "riscv_hart.h" // IWYU pragma: keep
#include "riscv_jit.h"  // IWYU pragma: keep
#include "riscv_mmu.h"  // IWYU pragma: keep

#if defined(RISCV32)

typedef uint32_t xlen_t;
typedef int32_t  sxlen_t;
typedef uint32_t xaddr_t;
#define SHAMT_BITS       5
#define DIV_OVERFLOW_RS1 ((sxlen_t)0x80000000U)

#else

typedef uint64_t xlen_t;
typedef int64_t  sxlen_t;
typedef uint64_t xaddr_t;
#define SHAMT_BITS       6
#define DIV_OVERFLOW_RS1 ((sxlen_t)0x8000000000000000ULL)

#endif

#if defined(USE_THREAD_EMU)

static uint32_t preempt_count = 0;

static forceinline void riscv_voluntary_preempt(rvvm_hart_t* vm)
{
    if (++preempt_count >= 100000) {
        preempt_count = 0;
        riscv_restart_dispatch(vm);
    }
}

#else

#define riscv_voluntary_preempt(...)                                                                                   \
    do {                                                                                                               \
    } while (0)

#endif

static forceinline xlen_t riscv_read_reg(rvvm_hart_t* vm, size_t reg)
{
    return vm->registers[reg];
}

static forceinline sxlen_t riscv_read_reg_s(rvvm_hart_t* vm, size_t reg)
{
    return vm->registers[reg];
}

static forceinline void riscv_write_reg(rvvm_hart_t* vm, size_t reg, sxlen_t data)
{
    vm->registers[reg] = data;
}

#endif
