/*
riscv_fpu.h - RISC-V Floating-Point ISA interpreter template
Copyright (C) 2024  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_RISCV_FPU_H
#define RVVM_RISCV_FPU_H

#include "fpu_lib.h"
#include "fpu_types.h"
#include "riscv_csr.h"

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

// FPU fclass instruction results
#define FCL_NEG_INF       0x0
#define FCL_NEG_NORMAL    0x1
#define FCL_NEG_SUBNORMAL 0x2
#define FCL_NEG_ZERO      0x3
#define FCL_POS_ZERO      0x4
#define FCL_POS_SUBNORMAL 0x5
#define FCL_POS_NORMAL    0x6
#define FCL_POS_INF       0x7
#define FCL_NAN_SIG       0x8
#define FCL_NAN_QUIET     0x9

// Bit-precise register reads
static forceinline fpu_f32_t riscv_view_s(rvvm_hart_t* vm, regid_t reg)
{
    return fpu_unpack_f32_from_f64(vm->fpu_registers[reg]);
}

// Normalized register reads
static forceinline fpu_f32_t riscv_read_s(rvvm_hart_t* vm, regid_t reg)
{
    return fpu_nan_unbox_f32(vm->fpu_registers[reg]);
}

// For bit-precise float register writes
static forceinline void riscv_emit_s(rvvm_hart_t* vm, regid_t reg, fpu_f32_t val)
{
    vm->fpu_registers[reg] = fpu_nanbox_f32(val);
    riscv_fpu_set_dirty(vm);
}

// Canonizes the written result
static forceinline void riscv_write_s(rvvm_hart_t* vm, regid_t reg, fpu_f32_t val)
{
    if (likely(!fpu_is_nan32(val))) {
        riscv_emit_s(vm, reg, val);
    } else {
        riscv_emit_s(vm, reg, fpu_bit_u32_to_f32(0x7FC00000U));
    }
}

static forceinline fpu_f64_t riscv_view_d(rvvm_hart_t* vm, regid_t reg)
{
    return vm->fpu_registers[reg];
}

static forceinline fpu_f64_t riscv_read_d(rvvm_hart_t* vm, regid_t reg)
{
    return vm->fpu_registers[reg];
}

static forceinline void riscv_emit_d(rvvm_hart_t* vm, regid_t reg, fpu_f64_t val)
{
    vm->fpu_registers[reg] = val;
    riscv_fpu_set_dirty(vm);
}

static forceinline void riscv_write_d(rvvm_hart_t* vm, regid_t reg, fpu_f64_t val)
{
    if (likely(!fpu_is_nan64(val))) {
        riscv_emit_d(vm, reg, val);
    } else {
        riscv_emit_d(vm, reg, fpu_bit_u64_to_f64(0x7FF8000000000000U));
    }
}

static forceinline void riscv_emulate_f_opc_load(rvvm_hart_t* vm, const uint32_t insn)
{
    const uint32_t funct3 = bit_cut(insn, 12, 3);
    const regid_t  rds    = bit_cut(insn, 7, 5);
    const regid_t  rs1    = bit_cut(insn, 15, 5);
    const sxlen_t  offset = sign_extend(bit_cut(insn, 20, 12), 12);
    const xlen_t   addr   = riscv_read_reg(vm, rs1) + offset;
    if (likely(riscv_fpu_is_enabled(vm))) {
        switch (funct3) {
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
    const uint32_t funct3 = bit_cut(insn, 12, 3);
    const regid_t  rs1    = bit_cut(insn, 15, 5);
    const regid_t  rs2    = bit_cut(insn, 20, 5);
    const sxlen_t  offset = sign_extend(bit_cut(insn, 7, 5) | (bit_cut(insn, 25, 7) << 5), 12);
    const xlen_t   addr   = riscv_read_reg(vm, rs1) + offset;
    if (likely(riscv_fpu_is_enabled(vm))) {
        switch (funct3) {
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

// Rounding mode values of 5 & 6 are illegal
static forceinline bool riscv_fpu_rm_is_valid(uint8_t rm)
{
    rm = 6 - rm;
    return rm > 1;
}

static forceinline void riscv_emulate_f_fmadd(rvvm_hart_t* vm, const uint32_t insn)
{
    const regid_t  rds    = bit_cut(insn, 7, 5);
    const uint8_t  rm     = bit_cut(insn, 12, 3);
    const regid_t  rs1    = bit_cut(insn, 15, 5);
    const regid_t  rs2    = bit_cut(insn, 20, 5);
    const uint32_t funct2 = bit_cut(insn, 25, 2);
    const regid_t  rs3    = insn >> 27;

    if (likely(riscv_fpu_is_enabled(vm) && riscv_fpu_rm_is_valid(rm))) {
        switch (funct2) {
            case 0x0: // fmadd.s
                riscv_emit_s(vm, rds, fpu_fma32(riscv_view_s(vm, rs1), riscv_view_s(vm, rs2), riscv_view_s(vm, rs3)));
                return;
            case 0x1: // fmadd.d
                riscv_emit_d(vm, rds, fpu_fma64(riscv_view_d(vm, rs1), riscv_view_d(vm, rs2), riscv_view_d(vm, rs3)));
                return;
        }
    }
    riscv_illegal_insn(vm, insn);
}

static forceinline void riscv_emulate_f_fmsub(rvvm_hart_t* vm, const uint32_t insn)
{
    const regid_t  rds    = bit_cut(insn, 7, 5);
    const uint8_t  rm     = bit_cut(insn, 12, 3);
    const regid_t  rs1    = bit_cut(insn, 15, 5);
    const regid_t  rs2    = bit_cut(insn, 20, 5);
    const uint32_t funct2 = bit_cut(insn, 25, 2);
    const regid_t  rs3    = insn >> 27;

    if (likely(riscv_fpu_is_enabled(vm) && riscv_fpu_rm_is_valid(rm))) {
        switch (funct2) {
            case 0x0: // fmsub.s
                riscv_emit_s(vm, rds, fpu_fma32(riscv_view_s(vm, rs1), riscv_view_s(vm, rs2), fpu_neg32(riscv_view_s(vm, rs3))));
                return;
            case 0x1: // fmsub.d
                riscv_emit_d(vm, rds, fpu_fma64(riscv_view_d(vm, rs1), riscv_view_d(vm, rs2), fpu_neg64(riscv_view_d(vm, rs3))));
                return;
        }
    }
    riscv_illegal_insn(vm, insn);
}

static forceinline void riscv_emulate_f_fnmsub(rvvm_hart_t* vm, const uint32_t insn)
{
    const regid_t  rds    = bit_cut(insn, 7, 5);
    const uint8_t  rm     = bit_cut(insn, 12, 3);
    const regid_t  rs1    = bit_cut(insn, 15, 5);
    const regid_t  rs2    = bit_cut(insn, 20, 5);
    const uint32_t funct2 = bit_cut(insn, 25, 2);
    const regid_t  rs3    = insn >> 27;

    if (likely(riscv_fpu_is_enabled(vm) && riscv_fpu_rm_is_valid(rm))) {
        switch (funct2) {
            case 0x0: // fnmsub.s
                riscv_emit_s(vm, rds, fpu_fma32(fpu_neg32(riscv_view_s(vm, rs1)), riscv_view_s(vm, rs2), riscv_view_s(vm, rs3)));
                return;
            case 0x1: // fnmsub.d
                riscv_emit_d(vm, rds, fpu_fma64(fpu_neg64(riscv_view_d(vm, rs1)), riscv_view_d(vm, rs2), riscv_view_d(vm, rs3)));
                return;
        }
    }
    riscv_illegal_insn(vm, insn);
}

static forceinline void riscv_emulate_f_fnmadd(rvvm_hart_t* vm, const uint32_t insn)
{
    const regid_t  rds    = bit_cut(insn, 7, 5);
    const uint8_t  rm     = bit_cut(insn, 12, 3);
    const regid_t  rs1    = bit_cut(insn, 15, 5);
    const regid_t  rs2    = bit_cut(insn, 20, 5);
    const uint32_t funct2 = bit_cut(insn, 25, 2);
    const regid_t  rs3    = insn >> 27;

    if (likely(riscv_fpu_is_enabled(vm) && riscv_fpu_rm_is_valid(rm))) {
        switch (funct2) {
            case 0x0: // fnmadd.s
                riscv_emit_s(vm, rds, fpu_neg32(fpu_fma32(riscv_view_s(vm, rs1), riscv_view_s(vm, rs2), riscv_view_s(vm, rs3))));
                return;
            case 0x1: // fnmadd.d
                riscv_emit_d(vm, rds, fpu_neg64(fpu_fma64(riscv_view_d(vm, rs1), riscv_view_d(vm, rs2), riscv_view_d(vm, rs3))));
                return;
        }
    }
    riscv_illegal_insn(vm, insn);
}

static forceinline void riscv_emulate_f_opc_op(rvvm_hart_t* vm, const uint32_t insn)
{
    const regid_t  rds    = bit_cut(insn, 7, 5);
    const uint8_t  rm     = bit_cut(insn, 12, 3);
    const regid_t  rs1    = bit_cut(insn, 15, 5);
    const regid_t  rs2    = bit_cut(insn, 20, 5);
    const uint32_t funct7 = insn >> 25;

    if (likely(riscv_fpu_is_enabled(vm) && riscv_fpu_rm_is_valid(rm))) {
        switch (funct7) {
            case RISCV_FADD_S:
                riscv_emit_s(vm, rds, fpu_add32(riscv_view_s(vm, rs1), riscv_view_s(vm, rs2)));
                return;
            case RISCV_FADD_D:
                riscv_emit_d(vm, rds, fpu_add64(riscv_view_d(vm, rs1), riscv_view_d(vm, rs2)));
                return;
            case RISCV_FSUB_S:
                riscv_write_s(vm, rds, fpu_sub32(riscv_view_s(vm, rs1), riscv_view_s(vm, rs2)));
                return;
            case RISCV_FSUB_D:
                riscv_write_d(vm, rds, fpu_sub64(riscv_view_d(vm, rs1), riscv_view_d(vm, rs2)));
                return;
            case RISCV_FMUL_S:
                riscv_emit_s(vm, rds, fpu_mul32(riscv_view_s(vm, rs1), riscv_view_s(vm, rs2)));
                return;
            case RISCV_FMUL_D:
                riscv_emit_d(vm, rds, fpu_mul64(riscv_view_d(vm, rs1), riscv_view_d(vm, rs2)));
                return;
            case RISCV_FDIV_S:
                riscv_emit_s(vm, rds, fpu_div32(riscv_view_s(vm, rs1), riscv_view_s(vm, rs2)));
                return;
            case RISCV_FDIV_D:
                riscv_emit_d(vm, rds, fpu_div64(riscv_view_d(vm, rs1), riscv_view_d(vm, rs2)));
                return;
            case RISCV_FSQRT_S:
                if (likely(rs2 == 0)) {
                    riscv_write_s(vm, rds, fpu_sqrt32(riscv_view_s(vm, rs1)));
                    return;
                }
                break;
            case RISCV_FSQRT_D:
                if (likely(rs2 == 0)) {
                    riscv_write_d(vm, rds, fpu_sqrt64(riscv_view_d(vm, rs1)));
                    return;
                }
                break;
            case RISCV_FSGNJ_S:
                switch (rm) {
                    case 0x0: // fsgnj.s
                        riscv_emit_s(vm, rds, fpu_fsgnj32(riscv_read_s(vm, rs1), riscv_read_s(vm, rs2)));
                        return;
                    case 0x1: // fsgnjn.s
                        riscv_emit_s(vm, rds, fpu_fsgnjn32(riscv_view_s(vm, rs1), riscv_view_s(vm, rs2)));
                        return;
                    case 0x2: // fsgnjx.s
                        riscv_emit_s(vm, rds, fpu_fsgnjx32(riscv_view_s(vm, rs1), riscv_view_s(vm, rs2)));
                        return;
                }
                break;
            case RISCV_FSGNJ_D:
                switch (rm) {
                    case 0x0: // fsgnj.d
                        riscv_emit_d(vm, rds, fpu_fsgnj64(riscv_read_d(vm, rs1), riscv_read_d(vm, rs2)));
                        return;
                    case 0x1: // fsgnjn.d
                        riscv_emit_d(vm, rds, fpu_fsgnjn64(riscv_view_d(vm, rs1), riscv_view_d(vm, rs2)));
                        return;
                    case 0x2: // fsgnjx.d
                        riscv_emit_d(vm, rds, fpu_fsgnjx64(riscv_view_d(vm, rs1), riscv_view_d(vm, rs2)));
                        return;
                }
                break;
            case RISCV_FCLAMP_S:
                switch (rm) {
                    case 0x0: // fmin.s
                        riscv_write_s(vm, rds, fpu_min32(riscv_view_s(vm, rs1), riscv_view_s(vm, rs2)));
                        return;
                    case 0x1: // fmax.s
                        riscv_write_s(vm, rds, fpu_max32(riscv_view_s(vm, rs1), riscv_view_s(vm, rs2)));
                        return;
                }
                break;
            case RISCV_FCLAMP_D:
                switch (rm) {
                    case 0x0: // fmin.d
                        riscv_write_d(vm, rds, fpu_min64(riscv_view_d(vm, rs1), riscv_view_d(vm, rs2)));
                        return;
                    case 0x1: // fmax.d
                        riscv_write_d(vm, rds, fpu_max64(riscv_view_d(vm, rs1), riscv_view_d(vm, rs2)));
                        return;
                }
                break;
            case RISCV_FCVT_S_D:
                if (likely(rs2 == 1)) {
                    riscv_write_s(vm, rds, fpu_fcvt_f64_to_f32(riscv_view_d(vm, rs1)));
                    return;
                }
                break;
            case RISCV_FCVT_D_S:
                if (likely(rs2 == 0)) {
                    riscv_write_d(vm, rds, fpu_fcvt_f32_to_f64(riscv_view_s(vm, rs1)));
                    return;
                }
                break;
            case RISCV_FCVT_W_S:
                switch (rs2) {
                    case 0x0: // fcvt.w.s
                        riscv_write_reg(vm, rds, (int32_t)fpu_fcvt_f32_to_i32(riscv_view_s(vm, rs1)));
                        return;
                    case 0x1: // fcvt.wu.s
                        riscv_write_reg(vm, rds, (int32_t)fpu_fcvt_f32_to_u32(riscv_view_s(vm, rs1)));
                        return;
#ifdef RV64
                    case 0x2: // fcvt.l.s
                        riscv_write_reg(vm, rds, (int64_t)fpu_fcvt_f32_to_i64(riscv_view_s(vm, rs1)));
                        return;
                    case 0x3: // fcvt.lu.s
                        riscv_write_reg(vm, rds, (int64_t)fpu_fcvt_f32_to_u64(riscv_view_s(vm, rs1)));
                        return;
#endif
                }
                break;
            case RISCV_FCVT_W_D:
                switch (rs2) {
                    case 0x0: // fcvt.w.d
                        riscv_write_reg(vm, rds, (int32_t)fpu_fcvt_f64_to_i32(riscv_view_d(vm, rs1)));
                        return;
                    case 0x1: // fcvt.wu.d
                        riscv_write_reg(vm, rds, (int32_t)fpu_fcvt_f64_to_u32(riscv_view_d(vm, rs1)));
                        return;
#ifdef RV64
                    case 0x2: // fcvt.l.d
                        riscv_write_reg(vm, rds, (int64_t)fpu_fcvt_f64_to_i64(riscv_view_d(vm, rs1)));
                        return;
                    case 0x3: // fcvt.lu.d
                        riscv_write_reg(vm, rds, (int64_t)fpu_fcvt_f64_to_u64(riscv_view_d(vm, rs1)));
                        return;
#endif
                }
                break;
            case RISCV_FMVCLS_S:
                if (likely(rs2 == 0)) {
                    switch (rm) {
                        case 0x0: // fmv.x.w
                            riscv_write_reg(vm, rds, (int32_t)fpu_bit_f32_to_u32(riscv_view_s(vm, rs1)));
                            return;
                        case 0x1: // fclass.s
                            riscv_write_reg(vm, rds, 1U << fpu_fclass32(riscv_view_s(vm, rs1)));
                            return;
                    }
                }
                break;
            case RISCV_FMVCLS_D:
                if (likely(rs2 == 0)) {
                    switch (rm) {
#ifdef RV64
                        case 0x0: // fmv.x.d
                            riscv_write_reg(vm, rds, (int64_t)fpu_bit_f64_to_u64(riscv_view_d(vm, rs1)));
                            return;
#endif
                        case 0x1: // fclass.d
                            riscv_write_reg(vm, rds, 1U << fpu_fclass64(riscv_view_d(vm, rs1)));
                            return;
                    }
                }
                break;
            case RISCV_FCMP_S:
                switch (rm) {
                    case 0x0: // fle.s
                        riscv_write_reg(vm, rds, fpu_is_fle32_sig(riscv_view_s(vm, rs1), riscv_view_s(vm, rs2)));
                        return;
                    case 0x1: // flt.s
                        riscv_write_reg(vm, rds, fpu_is_flt32_sig(riscv_view_s(vm, rs1), riscv_view_s(vm, rs2)));
                        return;
                    case 0x2: // feq.s
                        riscv_write_reg(vm, rds, fpu_is_equal32_quiet(riscv_read_s(vm, rs1), riscv_view_s(vm, rs2)));
                        return;
                }
                break;
            case RISCV_FCMP_D:
                switch (rm) {
                    case 0x0: // fle.d
                        riscv_write_reg(vm, rds, fpu_is_fle64_sig(riscv_view_d(vm, rs1), riscv_view_d(vm, rs2)));
                        return;
                    case 0x1: // flt.d
                        riscv_write_reg(vm, rds, fpu_is_flt64_sig(riscv_view_d(vm, rs1), riscv_view_d(vm, rs2)));
                        return;
                    case 0x2: // feq.d
                        riscv_write_reg(vm, rds, fpu_is_equal64_quiet(riscv_read_d(vm, rs1), riscv_view_d(vm, rs2)));
                        return;
                }
                break;
            case RISCV_FCVT_S_W:
                switch (rs2) {
                    case 0x0: // fcvt.s.w
                        riscv_emit_s(vm, rds, fpu_fcvt_i32_to_f32(riscv_read_reg(vm, rs1)));
                        return;
                    case 0x1: // fcvt.s.wu
                        riscv_emit_s(vm, rds, fpu_fcvt_u32_to_f32(riscv_read_reg(vm, rs1)));
                        return;
#ifdef RV64
                    case 0x2: // fcvt.s.l
                        riscv_emit_s(vm, rds, fpu_fcvt_i64_to_f32(riscv_read_reg(vm, rs1)));
                        return;
                    case 0x3: // fcvt.s.lu
                        riscv_emit_s(vm, rds, fpu_fcvt_u64_to_f32(riscv_read_reg(vm, rs1)));
                        return;
#endif
                }
                break;
            case RISCV_FCVT_D_W:
                switch (rs2) {
                    case 0x0: // fcvt.d.w
                        riscv_emit_d(vm, rds, fpu_fcvt_i32_to_f64(riscv_read_reg(vm, rs1)));
                        return;
                    case 0x1: // fcvt.d.wu
                        riscv_emit_d(vm, rds, fpu_fcvt_u32_to_f64(riscv_read_reg(vm, rs1)));
                        return;
#ifdef RV64
                    case 0x2: // fcvt.d.l
                        riscv_emit_d(vm, rds, fpu_fcvt_i64_to_f64(riscv_read_reg(vm, rs1)));
                        return;
                    case 0x3: // fcvt.d.lu
                        riscv_emit_d(vm, rds, fpu_fcvt_u64_to_f64(riscv_read_reg(vm, rs1)));
                        return;
#endif
                }
                break;
            case RISCV_FMV_W_X:
                if (likely(rs2 == 0 && rm == 0)) {
                    riscv_emit_s(vm, rds, fpu_bit_u32_to_f32(riscv_read_reg(vm, rs1)));
                    return;
                }
                break;
#ifdef RV64
            case RISCV_FMV_D_X:
                if (likely(rs2 == 0 && rm == 0)) {
                    riscv_emit_d(vm, rds, fpu_bit_u64_to_f64(riscv_read_reg(vm, rs1)));
                    return;
                }
                break;
#endif
        }
    }
    riscv_illegal_insn(vm, insn);
}

#endif
