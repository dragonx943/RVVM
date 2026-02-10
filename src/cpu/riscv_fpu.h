/*
riscv_fpu.h - RISC-V Floating-Point ISA interpreter template
Copyright (C) 2024  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_RISCV_FPU_H
#define RVVM_RISCV_FPU_H

#include "fpu_lib.h"      // IWYU pragma: keep
#include "riscv_common.h" // IWYU pragma: keep

/*
 * Floating-point funct7 opcodes in insn[31:25]
 */
#define RISCV_FADD_S      0x00
#define RISCV_FSUB_S      0x04
#define RISCV_FMUL_S      0x08
#define RISCV_FDIV_S      0x0C
#define RISCV_FSQRT_S     0x2C // rs2 field is zero
#define RISCV_FSGNJ_S     0x10 // rm field encodes funct3
#define RISCV_FCLAMP_S    0x14 // rm field encodes funct3, fmin/fmax
#define RISCV_FCVT_W_S    0x60 // rs2 field encodes conversion type
#define RISCV_FMVCLS_S    0x70 // rs2 field is zero, rm encodes fmv.x.w or fclass
#define RISCV_FCMP_S      0x50 // rm field encodes funct3
#define RISCV_FCVT_S_W    0x68 // rs2 field encodes conversion type
#define RISCV_FMV_W_X     0x78 // rs2, rm fields are zero

#define RISCV_FADD_D      0x01
#define RISCV_FSUB_D      0x05
#define RISCV_FMUL_D      0x09
#define RISCV_FDIV_D      0x0D
#define RISCV_FSQRT_D     0x2D // rs2 field is zero
#define RISCV_FSGNJ_D     0x11 // rm field encodes funct3
#define RISCV_FCLAMP_D    0x15 // rm field encodes funct3, fmin/fmax
#define RISCV_FCVT_S_D    0x20 // rs2 is 1
#define RISCV_FCVT_D_S    0x21 // rs2 is 0
#define RISCV_FCVT_W_D    0x61 // rs2 field encodes conversion type
#define RISCV_FMVCLS_D    0x71 // rs2 field is zero, rm encodes fmv.x.w or fclass
#define RISCV_FCMP_D      0x51 // rm field encodes funct3
#define RISCV_FCVT_D_W    0x69 // rs2 field encodes conversion type
#define RISCV_FMV_D_X     0x79 // rs2, rm fields are zero

/*
 * Floating-point fclass results
 */
#define FCL_NEG_INF       0x00
#define FCL_NEG_NORMAL    0x01
#define FCL_NEG_SUBNORMAL 0x02
#define FCL_NEG_ZERO      0x03
#define FCL_POS_ZERO      0x04
#define FCL_POS_SUBNORMAL 0x05
#define FCL_POS_NORMAL    0x06
#define FCL_POS_INF       0x07
#define FCL_NAN_SIG       0x08
#define FCL_NAN_QUIET     0x09

#if defined(USE_FPU)

// Rounding mode values of 5 & 6 are illegal
static forceinline bool riscv_fpu_rm_is_valid(uint32_t rm)
{
    rm = 6 - rm;
    return rm > 1;
}

// Bit-precise register reads
static forceinline fpu_f32_t riscv_view_s(rvvm_hart_t* vm, size_t reg)
{
    return fpu_unpack_f32_from_f64(vm->fpu_registers[reg]);
}

// Normalized register reads
static forceinline fpu_f32_t riscv_read_s(rvvm_hart_t* vm, size_t reg)
{
    return fpu_nan_unbox_f32(vm->fpu_registers[reg]);
}

// For bit-precise float register writes
static forceinline void riscv_emit_s(rvvm_hart_t* vm, size_t reg, fpu_f32_t val)
{
    vm->fpu_registers[reg] = fpu_nanbox_f32(val);
    riscv_fpu_set_dirty(vm);
}

// Canonizes the written result
static forceinline void riscv_write_s(rvvm_hart_t* vm, size_t reg, fpu_f32_t val)
{
    if (likely(!fpu_is_nan32(val))) {
        riscv_emit_s(vm, reg, val);
    } else {
        riscv_emit_s(vm, reg, fpu_bit_u32_to_f32(FPU_LIB_FP32_CANONICAL_NAN));
    }
}

static forceinline fpu_f64_t riscv_view_d(rvvm_hart_t* vm, size_t reg)
{
    return vm->fpu_registers[reg];
}

static forceinline fpu_f64_t riscv_read_d(rvvm_hart_t* vm, size_t reg)
{
    return vm->fpu_registers[reg];
}

static forceinline void riscv_emit_d(rvvm_hart_t* vm, size_t reg, fpu_f64_t val)
{
    vm->fpu_registers[reg] = val;
    riscv_fpu_set_dirty(vm);
}

static forceinline void riscv_write_d(rvvm_hart_t* vm, size_t reg, fpu_f64_t val)
{
    if (likely(!fpu_is_nan64(val))) {
        riscv_emit_d(vm, reg, val);
    } else {
        riscv_emit_d(vm, reg, fpu_bit_u64_to_f64(FPU_LIB_FP64_CANONICAL_NAN));
    }
}

slow_path void riscv_emulate_f_opc_op(rvvm_hart_t* vm, const uint32_t insn);

#if defined(RISCV32) || defined(RISCV64)

static forceinline void riscv_emulate_f_opc_load(rvvm_hart_t* vm, const uint32_t insn)
{
    const size_t  rds  = bit_ext_u32(insn, 7, 5);
    const size_t  rs1  = bit_ext_u32(insn, 15, 5);
    const sxlen_t off  = bit_ext_i32(insn, 20, 12);
    const xlen_t  addr = riscv_read_reg(vm, rs1) + off;

    if (likely(riscv_fpu_is_enabled(vm))) {
        switch (bit_ext_u32(insn, 12, 3)) {
            case 0x2: // flw
                riscv_load_float(vm, addr, rds);
                return;
            case 0x3: // fld
                riscv_load_double(vm, addr, rds);
                return;
        }
    }

    riscv_illegal_insn(vm, insn);
}

static forceinline void riscv_emulate_f_opc_store(rvvm_hart_t* vm, const uint32_t insn)
{
    const size_t  rs1  = bit_ext_u32(insn, 15, 5);
    const size_t  rs2  = bit_ext_u32(insn, 20, 5);
    const sxlen_t off  = (int32_t)((((uint32_t)(((int32_t)insn) >> 25)) << 5) | bit_ext_u32(insn, 7, 5));
    const xlen_t  addr = riscv_read_reg(vm, rs1) + off;

    if (likely(riscv_fpu_is_enabled(vm))) {
        switch (bit_ext_u32(insn, 12, 3)) {
            case 0x2: // fsw
                riscv_store_float(vm, addr, rs2);
                return;
            case 0x3: // fsd
                riscv_store_double(vm, addr, rs2);
                return;
        }
    }

    riscv_illegal_insn(vm, insn);
}

static forceinline void riscv_emulate_f_fmadd(rvvm_hart_t* vm, const uint32_t insn)
{
    const size_t   rds = bit_ext_u32(insn, 7, 5);
    const uint32_t rm  = bit_ext_u32(insn, 12, 3);
    const size_t   rs1 = bit_ext_u32(insn, 15, 5);
    const size_t   rs2 = bit_ext_u32(insn, 20, 5);
    const size_t   rs3 = insn >> 27;

    if (likely(riscv_fpu_is_enabled(vm) && riscv_fpu_rm_is_valid(rm))) {
        switch (bit_ext_u32(insn, 25, 2)) {
            case 0x0: // fmadd.s
                riscv_emit_s(vm, rds,
                             fpu_fma32(riscv_view_s(vm, rs1), //
                                       riscv_view_s(vm, rs2), //
                                       riscv_view_s(vm, rs3)));
                return;
            case 0x1: // fmadd.d
                riscv_emit_d(vm, rds,
                             fpu_fma64(riscv_view_d(vm, rs1), //
                                       riscv_view_d(vm, rs2), //
                                       riscv_view_d(vm, rs3)));
                return;
        }
    }

    riscv_illegal_insn(vm, insn);
}

static forceinline void riscv_emulate_f_fmsub(rvvm_hart_t* vm, const uint32_t insn)
{
    const size_t   rds = bit_ext_u32(insn, 7, 5);
    const uint32_t rm  = bit_ext_u32(insn, 12, 3);
    const size_t   rs1 = bit_ext_u32(insn, 15, 5);
    const size_t   rs2 = bit_ext_u32(insn, 20, 5);
    const size_t   rs3 = insn >> 27;

    if (likely(riscv_fpu_is_enabled(vm) && riscv_fpu_rm_is_valid(rm))) {
        switch (bit_ext_u32(insn, 25, 2)) {
            case 0x0: // fmsub.s
                riscv_emit_s(vm, rds,
                             fpu_fma32(riscv_view_s(vm, rs1), //
                                       riscv_view_s(vm, rs2), //
                                       fpu_neg32(riscv_view_s(vm, rs3))));
                return;
            case 0x1: // fmsub.d
                riscv_emit_d(vm, rds,
                             fpu_fma64(riscv_view_d(vm, rs1), //
                                       riscv_view_d(vm, rs2), //
                                       fpu_neg64(riscv_view_d(vm, rs3))));
                return;
        }
    }

    riscv_illegal_insn(vm, insn);
}

static forceinline void riscv_emulate_f_fnmsub(rvvm_hart_t* vm, const uint32_t insn)
{
    const size_t   rds = bit_ext_u32(insn, 7, 5);
    const uint32_t rm  = bit_ext_u32(insn, 12, 3);
    const size_t   rs1 = bit_ext_u32(insn, 15, 5);
    const size_t   rs2 = bit_ext_u32(insn, 20, 5);
    const size_t   rs3 = insn >> 27;

    if (likely(riscv_fpu_is_enabled(vm) && riscv_fpu_rm_is_valid(rm))) {
        switch (bit_ext_u32(insn, 25, 2)) {
            case 0x0: // fnmsub.s
                riscv_emit_s(vm, rds,
                             fpu_fma32(fpu_neg32(riscv_view_s(vm, rs1)), //
                                       riscv_view_s(vm, rs2),            //
                                       riscv_view_s(vm, rs3)));
                return;
            case 0x1: // fnmsub.d
                riscv_emit_d(vm, rds,
                             fpu_fma64(fpu_neg64(riscv_view_d(vm, rs1)), //
                                       riscv_view_d(vm, rs2),            //
                                       riscv_view_d(vm, rs3)));
                return;
        }
    }

    riscv_illegal_insn(vm, insn);
}

static forceinline void riscv_emulate_f_fnmadd(rvvm_hart_t* vm, const uint32_t insn)
{
    const size_t   rds = bit_ext_u32(insn, 7, 5);
    const uint32_t rm  = bit_ext_u32(insn, 12, 3);
    const size_t   rs1 = bit_ext_u32(insn, 15, 5);
    const size_t   rs2 = bit_ext_u32(insn, 20, 5);
    const size_t   rs3 = insn >> 27;

    if (likely(riscv_fpu_is_enabled(vm) && riscv_fpu_rm_is_valid(rm))) {
        switch (bit_ext_u32(insn, 25, 2)) {
            case 0x0: // fnmadd.s
                riscv_emit_s(vm, rds,
                             fpu_neg32(fpu_fma32(riscv_view_s(vm, rs1), //
                                                 riscv_view_s(vm, rs2), //
                                                 riscv_view_s(vm, rs3))));
                return;
            case 0x1: // fnmadd.d
                riscv_emit_d(vm, rds,
                             fpu_neg64(fpu_fma64(riscv_view_d(vm, rs1), //
                                                 riscv_view_d(vm, rs2), //
                                                 riscv_view_d(vm, rs3))));
                return;
        }
    }

    riscv_illegal_insn(vm, insn);
}

#endif

#endif

#endif
