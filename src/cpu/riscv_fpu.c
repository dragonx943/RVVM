/*
riscv_fpu.h - RISC-V Floating-Point ISA interpreter template
Copyright (C) 2024  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#if defined(USE_FPU)

#include "riscv_fpu.h"

slow_path void riscv_emulate_f_opc_op(rvvm_hart_t* vm, const uint32_t insn)
{
    const size_t   rds = bit_ext_u32(insn, 7, 5);
    const uint32_t rm  = bit_ext_u32(insn, 12, 3);
    const size_t   rs1 = bit_ext_u32(insn, 15, 5);
    const size_t   rs2 = bit_ext_u32(insn, 20, 5);

    if (likely(riscv_fpu_is_enabled(vm) && riscv_fpu_rm_is_valid(rm))) {
        switch (insn >> 25) {
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
                if (likely(!rs2)) {
                    riscv_emit_s(vm, rds, fpu_sqrt32(riscv_view_s(vm, rs1)));
                    return;
                }
                break;
            case RISCV_FSQRT_D:
                if (likely(!rs2)) {
                    riscv_emit_d(vm, rds, fpu_sqrt64(riscv_view_d(vm, rs1)));
                    return;
                }
                break;
            case RISCV_FSGNJ_S:
                switch (rm) {
                    case 0x00: // fsgnj.s
                        riscv_emit_s(vm, rds, fpu_fsgnj32(riscv_read_s(vm, rs1), riscv_read_s(vm, rs2)));
                        return;
                    case 0x01: // fsgnjn.s
                        riscv_emit_s(vm, rds, fpu_fsgnjn32(riscv_view_s(vm, rs1), riscv_view_s(vm, rs2)));
                        return;
                    case 0x02: // fsgnjx.s
                        riscv_emit_s(vm, rds, fpu_fsgnjx32(riscv_view_s(vm, rs1), riscv_view_s(vm, rs2)));
                        return;
                }
                break;
            case RISCV_FSGNJ_D:
                switch (rm) {
                    case 0x00: // fsgnj.d
                        riscv_emit_d(vm, rds, fpu_fsgnj64(riscv_read_d(vm, rs1), riscv_read_d(vm, rs2)));
                        return;
                    case 0x01: // fsgnjn.d
                        riscv_emit_d(vm, rds, fpu_fsgnjn64(riscv_view_d(vm, rs1), riscv_view_d(vm, rs2)));
                        return;
                    case 0x02: // fsgnjx.d
                        riscv_emit_d(vm, rds, fpu_fsgnjx64(riscv_view_d(vm, rs1), riscv_view_d(vm, rs2)));
                        return;
                }
                break;
            case RISCV_FCLAMP_S:
                switch (rm) {
                    case 0x00: // fmin.s
                        riscv_emit_s(vm, rds, fpu_min32(riscv_view_s(vm, rs1), riscv_view_s(vm, rs2)));
                        return;
                    case 0x01: // fmax.s
                        riscv_emit_s(vm, rds, fpu_max32(riscv_view_s(vm, rs1), riscv_view_s(vm, rs2)));
                        return;
                }
                break;
            case RISCV_FCLAMP_D:
                switch (rm) {
                    case 0x00: // fmin.d
                        riscv_emit_d(vm, rds, fpu_min64(riscv_view_d(vm, rs1), riscv_view_d(vm, rs2)));
                        return;
                    case 0x01: // fmax.d
                        riscv_emit_d(vm, rds, fpu_max64(riscv_view_d(vm, rs1), riscv_view_d(vm, rs2)));
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
                if (likely(!rs2)) {
                    riscv_write_d(vm, rds, fpu_fcvt_f32_to_f64(riscv_view_s(vm, rs1)));
                    return;
                }
                break;
            case RISCV_FCVT_W_S:
                switch (rs2) {
                    case 0x00: // fcvt.w.s
                        riscv_write_reg(vm, rds, (int32_t)fpu_round_f32_to_i32(riscv_view_s(vm, rs1), rm));
                        return;
                    case 0x01: // fcvt.wu.s
                        riscv_write_reg(vm, rds, (int32_t)fpu_round_f32_to_u32(riscv_view_s(vm, rs1), rm));
                        return;
                    case 0x02: // fcvt.l.s
                        if (likely(vm->rv64)) {
                            riscv_write_reg(vm, rds, (int64_t)fpu_round_f32_to_i64(riscv_view_s(vm, rs1), rm));
                            return;
                        }
                        break;
                    case 0x03: // fcvt.lu.s
                        if (likely(vm->rv64)) {
                            riscv_write_reg(vm, rds, (int64_t)fpu_round_f32_to_u64(riscv_view_s(vm, rs1), rm));
                            return;
                        }
                        break;
                }
                break;
            case RISCV_FCVT_W_D:
                switch (rs2) {
                    case 0x00: // fcvt.w.d
                        riscv_write_reg(vm, rds, (int32_t)fpu_round_f64_to_i32(riscv_view_d(vm, rs1), rm));
                        return;
                    case 0x01: // fcvt.wu.d
                        riscv_write_reg(vm, rds, (int32_t)fpu_round_f64_to_u32(riscv_view_d(vm, rs1), rm));
                        return;
                    case 0x02: // fcvt.l.d
                        if (likely(vm->rv64)) {
                            riscv_write_reg(vm, rds, (int64_t)fpu_round_f64_to_i64(riscv_view_d(vm, rs1), rm));
                            return;
                        }
                        break;
                    case 0x03: // fcvt.lu.d
                        if (likely(vm->rv64)) {
                            riscv_write_reg(vm, rds, (int64_t)fpu_round_f64_to_u64(riscv_view_d(vm, rs1), rm));
                            return;
                        }
                        break;
                }
                break;
            case RISCV_FMVCLS_S:
                if (likely(!rs2)) {
                    switch (rm) {
                        case 0x00: // fmv.x.w
                            riscv_write_reg(vm, rds, (int32_t)fpu_bit_f32_to_u32(riscv_view_s(vm, rs1)));
                            return;
                        case 0x01: // fclass.s
                            riscv_write_reg(vm, rds, 1U << fpu_fclass32(riscv_view_s(vm, rs1)));
                            return;
                    }
                }
                break;
            case RISCV_FMVCLS_D:
                if (likely(!rs2)) {
                    switch (rm) {
                        case 0x00: // fmv.x.d
                            if (likely(vm->rv64)) {
                                riscv_write_reg(vm, rds, (int64_t)fpu_bit_f64_to_u64(riscv_view_d(vm, rs1)));
                                return;
                            }
                            break;
                        case 0x01: // fclass.d
                            riscv_write_reg(vm, rds, 1U << fpu_fclass64(riscv_view_d(vm, rs1)));
                            return;
                    }
                }
                break;
            case RISCV_FCMP_S:
                switch (rm) {
                    case 0x00: // fle.s
                        riscv_write_reg(vm, rds, fpu_is_fle32_sig(riscv_view_s(vm, rs1), riscv_view_s(vm, rs2)));
                        return;
                    case 0x01: // flt.s
                        riscv_write_reg(vm, rds, fpu_is_flt32_sig(riscv_view_s(vm, rs1), riscv_view_s(vm, rs2)));
                        return;
                    case 0x02: // feq.s
                        riscv_write_reg(vm, rds, fpu_is_equal32_quiet(riscv_read_s(vm, rs1), riscv_view_s(vm, rs2)));
                        return;
                }
                break;
            case RISCV_FCMP_D:
                switch (rm) {
                    case 0x00: // fle.d
                        riscv_write_reg(vm, rds, fpu_is_fle64_sig(riscv_view_d(vm, rs1), riscv_view_d(vm, rs2)));
                        return;
                    case 0x01: // flt.d
                        riscv_write_reg(vm, rds, fpu_is_flt64_sig(riscv_view_d(vm, rs1), riscv_view_d(vm, rs2)));
                        return;
                    case 0x02: // feq.d
                        riscv_write_reg(vm, rds, fpu_is_equal64_quiet(riscv_read_d(vm, rs1), riscv_view_d(vm, rs2)));
                        return;
                }
                break;
            case RISCV_FCVT_S_W:
                switch (rs2) {
                    case 0x00: // fcvt.s.w
                        riscv_emit_s(vm, rds, fpu_fcvt_i32_to_f32(riscv_read_reg(vm, rs1)));
                        return;
                    case 0x01: // fcvt.s.wu
                        riscv_emit_s(vm, rds, fpu_fcvt_u32_to_f32(riscv_read_reg(vm, rs1)));
                        return;
                    case 0x02: // fcvt.s.l
                        if (likely(vm->rv64)) {
                            riscv_emit_s(vm, rds, fpu_fcvt_i64_to_f32(riscv_read_reg(vm, rs1)));
                            return;
                        }
                        break;
                    case 0x03: // fcvt.s.lu
                        if (likely(vm->rv64)) {
                            riscv_emit_s(vm, rds, fpu_fcvt_u64_to_f32(riscv_read_reg(vm, rs1)));
                            return;
                        }
                        break;
                }
                break;
            case RISCV_FCVT_D_W:
                switch (rs2) {
                    case 0x00: // fcvt.d.w
                        riscv_emit_d(vm, rds, fpu_fcvt_i32_to_f64(riscv_read_reg(vm, rs1)));
                        return;
                    case 0x01: // fcvt.d.wu
                        riscv_emit_d(vm, rds, fpu_fcvt_u32_to_f64(riscv_read_reg(vm, rs1)));
                        return;
                    case 0x02: // fcvt.d.l
                        if (likely(vm->rv64)) {
                            riscv_emit_d(vm, rds, fpu_fcvt_i64_to_f64(riscv_read_reg(vm, rs1)));
                            return;
                        }
                        break;
                    case 0x03: // fcvt.d.lu
                        if (likely(vm->rv64)) {
                            riscv_emit_d(vm, rds, fpu_fcvt_u64_to_f64(riscv_read_reg(vm, rs1)));
                            return;
                        }
                        break;
                }
                break;
            case RISCV_FMV_W_X:
                if (likely(!rs2 && !rm)) {
                    riscv_emit_s(vm, rds, fpu_bit_u32_to_f32(riscv_read_reg(vm, rs1)));
                    return;
                }
                break;
            case RISCV_FMV_D_X:
                if (likely(vm->rv64 && !rs2 && !rm)) {
                    riscv_emit_d(vm, rds, fpu_bit_u64_to_f64(riscv_read_reg(vm, rs1)));
                    return;
                }
                break;
        }
    }

    riscv_illegal_insn(vm, insn);
}

#endif
