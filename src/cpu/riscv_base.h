/*
riscv_base.h - RISC-V Base Integer ISA interpreter
Copyright (C) 2024  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_RISCV_BASE_H
#define RVVM_RISCV_BASE_H

#include "riscv_atomics.h"
#include "riscv_common.h"
#include "riscv_priv.h"

#if defined(USE_FPU)
#include "riscv_fpu.h"
#endif

/*
 * Base 5-bit opcodes in insn[6:2]
 */
#define RISCV_OPC_LOAD     0x00
#define RISCV_OPC_LOAD_FP  0x04
#define RISCV_OPC_MISC_MEM 0x0C
#define RISCV_OPC_OP_IMM   0x10
#define RISCV_OPC_AUIPC    0x14
#define RISCV_OPC_OP_IMM32 0x18
#define RISCV_OPC_STORE    0x20
#define RISCV_OPC_STORE_FP 0x24
#define RISCV_OPC_AMO      0x2C
#define RISCV_OPC_OP       0x30
#define RISCV_OPC_LUI      0x34
#define RISCV_OPC_OP32     0x38
#define RISCV_OPC_FMADD    0x40
#define RISCV_OPC_FMSUB    0x44
#define RISCV_OPC_FNMSUB   0x48
#define RISCV_OPC_FNMADD   0x4C
#define RISCV_OPC_OP_FP    0x50
#define RISCV_OPC_BRANCH   0x60
#define RISCV_OPC_JALR     0x64
#define RISCV_OPC_JAL      0x6C
#define RISCV_OPC_SYSTEM   0x70

#if defined(RISCV64)
#define bit_clz(val)        bit_clz64(val)
#define bit_ctz(val)        bit_ctz64(val)
#define bit_popcnt(val)     bit_popcnt64(val)
#define bit_rotl(val, bits) bit_rotl64(val, bits)
#define bit_rotr(val, bits) bit_rotr64(val, bits)
#define bit_clmul(a, b)     bit_clmul64(a, b)
#define bit_clmulh(a, b)    bit_clmulh64(a, b)
#define bit_clmulr(a, b)    bit_clmulr64(a, b)
#else
#define bit_clz(val)        bit_clz32(val)
#define bit_ctz(val)        bit_ctz32(val)
#define bit_popcnt(val)     bit_popcnt32(val)
#define bit_rotl(val, bits) bit_rotl32(val, bits)
#define bit_rotr(val, bits) bit_rotr32(val, bits)
#define bit_clmul(a, b)     bit_clmul32(a, b)
#define bit_clmulh(a, b)    bit_clmulh32(a, b)
#define bit_clmulr(a, b)    bit_clmulr32(a, b)
#endif

static forceinline bitcnt_t decode_i_shamt(const uint32_t insn)
{
#if defined(RISCV64)
    return bit_cut(insn, 20, 6);
#else
    return bit_cut(insn, 20, 5);
#endif
}

static forceinline bitcnt_t decode_i_shift_funct7(const uint32_t insn)
{
#if defined(RISCV64)
    return (insn >> 26) << 1;
#else
    return insn >> 25;
#endif
}

static forceinline sxlen_t decode_i_branch_off(const uint32_t insn)
{
    const uint32_t imm = (bit_cut(insn, 31, 1) << 12) //
                       | (bit_cut(insn, 7, 1) << 11)  //
                       | (bit_cut(insn, 25, 6) << 5)  //
                       | (bit_cut(insn, 8, 4) << 1);
    return sign_extend(imm, 13);
}

static forceinline sxlen_t decode_i_jal_off(const uint32_t insn)
{
    const uint32_t imm = (bit_cut(insn, 31, 1) << 20) //
                       | (bit_cut(insn, 12, 8) << 12) //
                       | (bit_cut(insn, 20, 1) << 11) //
                       | (bit_cut(insn, 21, 10) << 1);
    return sign_extend(imm, 21);
}

static forceinline void riscv_emulate_i_opc_load(rvvm_hart_t* vm, const uint32_t insn)
{
    const uint32_t funct3 = bit_cut(insn, 12, 3);
    const regid_t  rds    = bit_cut(insn, 7, 5);
    const regid_t  rs1    = bit_cut(insn, 15, 5);
    const sxlen_t  offset = sign_extend(bit_cut(insn, 20, 12), 12);
    const xlen_t   addr   = riscv_read_reg(vm, rs1) + offset;
    switch (funct3) {
        case 0x00: // lb
            rvjit_trace_lb(rds, rs1, offset, 4);
            riscv_load_s8(vm, addr, rds);
            return;
        case 0x01: // lh
            rvjit_trace_lh(rds, rs1, offset, 4);
            riscv_load_s16(vm, addr, rds);
            return;
        case 0x02: // lw
            rvjit_trace_lw(rds, rs1, offset, 4);
            riscv_load_s32(vm, addr, rds);
            return;
#if defined(RISCV64)
        case 0x03: // ld
            rvjit_trace_ld(rds, rs1, offset, 4);
            riscv_load_u64(vm, addr, rds);
            return;
#endif
        case 0x04: // lbu
            rvjit_trace_lbu(rds, rs1, offset, 4);
            riscv_load_u8(vm, addr, rds);
            return;
        case 0x05: // lhu
            rvjit_trace_lhu(rds, rs1, offset, 4);
            riscv_load_u16(vm, addr, rds);
            return;
#if defined(RISCV64)
        case 0x06: // lwu
            rvjit_trace_lwu(rds, rs1, offset, 4);
            riscv_load_u32(vm, addr, rds);
            return;
#endif
    }
    riscv_illegal_insn(vm, insn);
}

static forceinline void riscv_emulate_i_opc_imm(rvvm_hart_t* vm, const uint32_t insn)
{
    const uint32_t funct3 = bit_cut(insn, 12, 3);
    const regid_t  rds    = bit_cut(insn, 7, 5);
    const regid_t  rs1    = bit_cut(insn, 15, 5);
    const xlen_t   imm    = sign_extend(bit_cut(insn, 20, 12), 12);
    const xlen_t   src    = riscv_read_reg(vm, rs1);
    switch (funct3) {
        case 0x00: // addi
            rvjit_trace_addi(rds, rs1, imm, 4);
            riscv_write_reg(vm, rds, src + imm);
            return;
        case 0x01: {
            const bitcnt_t shamt = decode_i_shamt(insn);
            switch (decode_i_shift_funct7(insn)) {
                case 0x00: // slli
                    rvjit_trace_slli(rds, rs1, shamt, 4);
                    riscv_write_reg(vm, rds, src << shamt);
                    return;
                case 0x14: // bseti (Zbs)
                    rvjit_trace_bseti(rds, rs1, shamt, 4);
                    riscv_write_reg(vm, rds, src | (((xlen_t)1U) << shamt));
                    return;
                case 0x24: // bclri (Zbs)
                    rvjit_trace_bclri(rds, rs1, shamt, 4);
                    riscv_write_reg(vm, rds, src & ~(((xlen_t)1U) << shamt));
                    return;
                case 0x34: // binvi (Zbs)
                    rvjit_trace_binvi(rds, rs1, shamt, 4);
                    riscv_write_reg(vm, rds, src ^ (((xlen_t)1U) << shamt));
                    return;
                case 0x30:
                    switch (shamt) {
                        case 0x00: // clz (Zbb)
                            // TODO: JIT
                            riscv_write_reg(vm, rds, bit_clz(src));
                            return;
                        case 0x01: // ctz (Zbb)
                            // TODO: JIT
                            riscv_write_reg(vm, rds, bit_ctz(src));
                            return;
                        case 0x02: // cpop (Zbb)
                            // TODO: JIT
                            riscv_write_reg(vm, rds, bit_popcnt(src));
                            return;
                        case 0x04: // sext.b (Zbb)
                            rvjit_trace_sext_b(rds, rs1, 4);
                            riscv_write_reg(vm, rds, (int8_t)src);
                            return;
                        case 0x05: // sext.h (Zbb)
                            rvjit_trace_sext_h(rds, rs1, 4);
                            riscv_write_reg(vm, rds, (int16_t)src);
                            return;
                    }
                    break;
            }
            break;
        }
        case 0x02: // slti
            rvjit_trace_slti(rds, rs1, imm, 4);
            riscv_write_reg(vm, rds, (((sxlen_t)src) < ((sxlen_t)imm)) ? 1 : 0);
            return;
        case 0x03: // sltiu
            rvjit_trace_sltiu(rds, rs1, imm, 4);
            riscv_write_reg(vm, rds, (src < imm) ? 1 : 0);
            return;
        case 0x04: // xori
            rvjit_trace_xori(rds, rs1, imm, 4);
            riscv_write_reg(vm, rds, src ^ imm);
            return;
        case 0x05: {
            const bitcnt_t shamt = decode_i_shamt(insn);
            switch (decode_i_shift_funct7(insn)) {
                case 0x00: // srli
                    rvjit_trace_srli(rds, rs1, shamt, 4);
                    riscv_write_reg(vm, rds, src >> shamt);
                    return;
                case 0x20: // srai
                    rvjit_trace_srai(rds, rs1, shamt, 4);
                    riscv_write_reg(vm, rds, ((sxlen_t)src) >> shamt);
                    return;
                case 0x14:
                    if (likely(shamt == 0x07)) { // orc.b (Zbb)
                        // TODO: JIT
                        riscv_write_reg(vm, rds, bit_orc_b(src));
                        return;
                    }
                    break;
                case 0x24: // bexti (Zbs)
                    rvjit_trace_bexti(rds, rs1, shamt, 4);
                    riscv_write_reg(vm, rds, (src >> shamt) & 1);
                    return;
                case 0x34:
#if defined(RISCV64)
                    if (likely(shamt == 0x38)) { // rev8 (Zbb), RV64 encoding
                        // TODO: JIT
                        riscv_write_reg(vm, rds, byteswap_uint64(src));
                        return;
                    }
#else
                    if (likely(shamt == 0x18)) { // rev8 (Zbb), RV32 encoding
                        // TODO: JIT
                        riscv_write_reg(vm, rds, byteswap_uint32(src));
                        return;
                    }
#endif
                    break;
                case 0x30: // rori (Zbb)
                    rvjit_trace_rori(rds, rs1, shamt, 4);
                    riscv_write_reg(vm, rds, bit_rotr(src, shamt));
                    return;
            }
            break;
        }
        case 0x06: // ori
            rvjit_trace_ori(rds, rs1, imm, 4);
            riscv_write_reg(vm, rds, src | imm);
            return;
        case 0x07: // andi
            rvjit_trace_andi(rds, rs1, imm, 4);
            riscv_write_reg(vm, rds, src & imm);
            return;
    }
    riscv_illegal_insn(vm, insn);
}

static forceinline void riscv_emulate_i_auipc(rvvm_hart_t* vm, const uint32_t insn)
{
    const regid_t rds = bit_cut(insn, 7, 5);
    const xlen_t  imm = sign_extend(insn & 0xFFFFF000, 32);
    const xlen_t  pc  = riscv_read_reg(vm, RISCV_REG_PC);

    rvjit_trace_auipc(rds, imm, 4);
    riscv_write_reg(vm, rds, pc + imm);
}

#if defined(RISCV64)

static forceinline void riscv_emulate_i_opc_imm32(rvvm_hart_t* vm, const uint32_t insn)
{
    const uint32_t funct3 = bit_cut(insn, 12, 3);
    const regid_t  rds    = bit_cut(insn, 7, 5);
    const regid_t  rs1    = bit_cut(insn, 15, 5);
    const uint32_t src    = riscv_read_reg(vm, rs1);
    switch (funct3) {
        case 0x00: { // addiw
            const uint32_t imm = sign_extend(bit_cut(insn, 20, 12), 12);
            rvjit_trace_addiw(rds, rs1, imm, 4);
            vm->registers[rds] = (int32_t)(src + imm);
            return;
        }
        case 0x01:
            switch (insn >> 25) {
                case 0x00: { // slliw
                    const bitcnt_t shamt = bit_cut(insn, 20, 5);
                    rvjit_trace_slliw(rds, rs1, shamt, 4);
                    riscv_write_reg(vm, rds, (int32_t)(src << shamt));
                    return;
                }
                case 0x04:
                case 0x05: { // slli.uw (Zba)
                    const bitcnt_t shamt = bit_cut(insn, 20, 6);
                    rvjit_trace_slli_uw(rds, rs1, shamt, 4);
                    riscv_write_reg(vm, rds, ((xlen_t)src) << shamt);
                    return;
                }
                case 0x30:
                    switch (bit_cut(insn, 20, 5)) {
                        case 0x00: // clzw (Zbb)
                            // TODO: JIT
                            riscv_write_reg(vm, rds, bit_clz32(src));
                            return;
                        case 0x01: // ctzw (Zbb)
                            // TODO: JIT
                            riscv_write_reg(vm, rds, bit_ctz32(src));
                            return;
                        case 0x02: // cpopw (Zbb)
                            // TODO: JIT
                            riscv_write_reg(vm, rds, bit_popcnt32(src));
                            return;
                    }
                    break;
            }
            break;
        case 0x05: {
            const bitcnt_t shamt = bit_cut(insn, 20, 5);
            switch (insn >> 25) {
                case 0x00: // srli
                    rvjit_trace_srliw(rds, rs1, shamt, 4);
                    riscv_write_reg(vm, rds, (int32_t)(src >> shamt));
                    return;
                case 0x20: // srai
                    rvjit_trace_sraiw(rds, rs1, shamt, 4);
                    riscv_write_reg(vm, rds, ((int32_t)src) >> shamt);
                    return;
                case 0x30: // roriw (Zbb)
                    rvjit_trace_roriw(rds, rs1, shamt, 4);
                    riscv_write_reg(vm, rds, (int32_t)bit_rotr32(src, shamt));
                    return;
            }
            break;
        }
    }
    riscv_illegal_insn(vm, insn);
}

#endif

static forceinline void riscv_emulate_i_opc_store(rvvm_hart_t* vm, const uint32_t insn)
{
    const uint32_t funct3 = bit_cut(insn, 12, 3);
    const regid_t  rs1    = bit_cut(insn, 15, 5);
    const regid_t  rs2    = bit_cut(insn, 20, 5);
    const sxlen_t  offset = sign_extend(bit_cut(insn, 7, 5) | (bit_cut(insn, 25, 7) << 5), 12);
    const xlen_t   addr   = riscv_read_reg(vm, rs1) + offset;
    switch (funct3) {
        case 0x00: // sb
            rvjit_trace_sb(rs2, rs1, offset, 4);
            riscv_store_u8(vm, addr, rs2);
            return;
        case 0x01: // sh
            rvjit_trace_sh(rs2, rs1, offset, 4);
            riscv_store_u16(vm, addr, rs2);
            return;
        case 0x02: // sw
            rvjit_trace_sw(rs2, rs1, offset, 4);
            riscv_store_u32(vm, addr, rs2);
            return;
#if defined(RISCV64)
        case 0x03: // sd
            rvjit_trace_sd(rs2, rs1, offset, 4);
            riscv_store_u64(vm, addr, rs2);
            return;
#endif
    }
    riscv_illegal_insn(vm, insn);
}

static forceinline void riscv_emulate_i_opc_op(rvvm_hart_t* vm, const uint32_t insn)
{
    const uint32_t funct = insn & 0xFE007000;
    const regid_t  rds   = bit_cut(insn, 7, 5);
    const regid_t  rs1   = bit_cut(insn, 15, 5);
    const regid_t  rs2   = bit_cut(insn, 20, 5);
    const xlen_t   reg1  = riscv_read_reg(vm, rs1);
    const xlen_t   reg2  = riscv_read_reg(vm, rs2);

    switch (funct) {
        case 0x00000000: // add
            rvjit_trace_add(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, reg1 + reg2);
            return;
        case 0x40000000: // sub
            rvjit_trace_sub(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, reg1 - reg2);
            return;
        case 0x00001000: // sll
            rvjit_trace_sll(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, reg1 << (reg2 & bit_mask(SHAMT_BITS)));
            return;
        case 0x00002000: // slt
            rvjit_trace_slt(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, (((sxlen_t)reg1) < ((sxlen_t)reg2)) ? 1 : 0);
            return;
        case 0x00003000: // sltu
            rvjit_trace_sltu(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, (reg1 < reg2) ? 1 : 0);
            return;
        case 0x00004000: // xor
            rvjit_trace_xor(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, reg1 ^ reg2);
            return;
        case 0x00005000: // srl
            rvjit_trace_srl(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, reg1 >> (reg2 & bit_mask(SHAMT_BITS)));
            return;
        case 0x40005000: // sra
            rvjit_trace_sra(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, ((sxlen_t)reg1) >> (reg2 & bit_mask(SHAMT_BITS)));
            return;
        case 0x00006000: // or
            rvjit_trace_or(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, reg1 | reg2);
            return;
        case 0x00007000: // and
            rvjit_trace_and(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, reg1 & reg2);
            return;
        /*
         * Multiplication / Division
         */
        case 0x02000000: // mul
            rvjit_trace_mul(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, reg1 * reg2);
            return;
        case 0x02001000: // mulh
            rvjit_trace_mulh(rds, rs1, rs2, 4);
#if defined(RISCV64)
            riscv_write_reg(vm, rds, mulh_uint64(reg1, reg2));
#else
            riscv_write_reg(vm, rds, ((int64_t)(sxlen_t)reg1 * (int64_t)(sxlen_t)reg2) >> 32);
#endif
            return;
        case 0x02002000: // mulhsu
            rvjit_trace_mulhsu(rds, rs1, rs2, 4);
#if defined(RISCV64)
            riscv_write_reg(vm, rds, mulhsu_uint64(reg1, reg2));
#else
            riscv_write_reg(vm, rds, ((int64_t)(sxlen_t)reg1 * (uint64_t)reg2) >> 32);
#endif
            return;
        case 0x02003000: // mulhu
            rvjit_trace_mulhu(rds, rs1, rs2, 4);
#if defined(RISCV64)
            riscv_write_reg(vm, rds, mulhu_uint64(reg1, reg2));
#else
            riscv_write_reg(vm, rds, ((uint64_t)reg1 * (uint64_t)reg2) >> 32);
#endif
            return;
        case 0x02004000: { // div
            sxlen_t result = -1;
            rvjit_trace_div(rds, rs1, rs2, 4);
            if ((sxlen_t)reg1 == DIV_OVERFLOW_RS1 && (sxlen_t)reg2 == -1) {
                // overflow
                result = DIV_OVERFLOW_RS1;
            } else if (reg2 != 0) {
                // division by zero check (we already setup result var for error)
                result = (sxlen_t)reg1 / (sxlen_t)reg2;
            }
            riscv_write_reg(vm, rds, result);
            return;
        }
        case 0x02005000: { // divu
            xlen_t result = (sxlen_t)-1;
            rvjit_trace_divu(rds, rs1, rs2, 4);
            // division by zero check (we already setup result var for error)
            if (reg2 != 0) {
                result = reg1 / reg2;
            }
            riscv_write_reg(vm, rds, result);
            return;
        }
        case 0x02006000: { // rem
            sxlen_t result = reg1;
            rvjit_trace_rem(rds, rs1, rs2, 4);
            // overflow
            if ((sxlen_t)reg1 == DIV_OVERFLOW_RS1 && (sxlen_t)reg2 == -1) {
                result = 0;
                // division by zero check (we already setup result var for error)
            } else if (reg2 != 0) {
                result = (sxlen_t)reg1 % (sxlen_t)reg2;
            }
            riscv_write_reg(vm, rds, result);
            return;
        }
        case 0x02007000: { // remu
            xlen_t result = reg1;
            rvjit_trace_remu(rds, rs1, rs2, 4);
            // division by zero check (we already setup result var for error)
            if (reg2 != 0) {
                result = reg1 % reg2;
            }
            riscv_write_reg(vm, rds, result);
            return;
        }
        /*
         * Zbc: Carry-less multiplication
         */
        case 0x0A001000: // clmul (Zbc)
            riscv_write_reg(vm, rds, bit_clmul(reg1, reg2));
            return;
        case 0x0A002000: // clmulr (Zbc)
            riscv_write_reg(vm, rds, bit_clmulr(reg1, reg2));
            return;
        case 0x0A003000: // clmulh (Zbc)
            riscv_write_reg(vm, rds, bit_clmulh(reg1, reg2));
            return;
        /*
         * Zbb: Basic bit-manipulation
         */
        case 0x0A004000: // min (Zbb)
            rvjit_trace_min(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, EVAL_MIN((sxlen_t)reg1, (sxlen_t)reg2));
            return;
        case 0x0A005000: // minu (Zbb)
            rvjit_trace_minu(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, EVAL_MIN(reg1, reg2));
            return;
        case 0x0A006000: // max (Zbb)
            rvjit_trace_max(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, EVAL_MAX((sxlen_t)reg1, (sxlen_t)reg2));
            return;
        case 0x0A007000: // maxu (Zbb)
            rvjit_trace_maxu(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, EVAL_MAX(reg1, reg2));
            return;
#if defined(RISCV32)
        case 0x08004000: // zext.h (Zbb), RV32 encoding
            if (likely(!rs2)) {
                rvjit_trace_andi(rds, rs1, 0xFFFF, 4);
                riscv_write_reg(vm, rds, (uint16_t)reg1);
                return;
            }
            return;
#endif
        case 0x40004000: // xnor (Zbb)
            rvjit_trace_xnor(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, reg1 ^ ~reg2);
            return;
        case 0x40006000: // orn (Zbb)
            rvjit_trace_orn(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, reg1 | ~reg2);
            return;
        case 0x40007000: // andn (Zbb)
            rvjit_trace_andn(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, reg1 & ~reg2);
            return;
        case 0x60001000: // rol (Zbb)
            rvjit_trace_rol(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, bit_rotl(reg1, reg2 & bit_mask(SHAMT_BITS)));
            return;
        case 0x60005000: // ror (Zbb)
            rvjit_trace_ror(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, bit_rotr(reg1, reg2 & bit_mask(SHAMT_BITS)));
            return;
        /*
         * Zicond: Integer conditionals
         */
        case 0x0E005000: // czero.eqz (Zicond)
            // TODO: JIT
            riscv_write_reg(vm, rds, reg2 ? reg1 : 0);
            return;
        case 0x0E007000: // czero.nez (Zicond)
            // TODO: JIT
            riscv_write_reg(vm, rds, reg2 ? 0 : reg1);
            return;
        /*
         * Zba: Address generation
         */
        case 0x20002000: // sh1add (Zba)
            rvjit_trace_shadd(rds, rs1, rs2, 1, 4);
            riscv_write_reg(vm, rds, reg2 + (reg1 << 1));
            return;
        case 0x20004000: // sh2add (Zba)
            rvjit_trace_shadd(rds, rs1, rs2, 2, 4);
            riscv_write_reg(vm, rds, reg2 + (reg1 << 2));
            return;
        case 0x20006000: // sh3add (Zba)
            rvjit_trace_shadd(rds, rs1, rs2, 3, 4);
            riscv_write_reg(vm, rds, reg2 + (reg1 << 3));
            return;
        /*
         * Zbs: Bitset manipulation
         */
        case 0x28001000: // bset (Zbs)
            rvjit_trace_bset(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, reg1 | (((xlen_t)1U) << (reg2 & bit_mask(SHAMT_BITS))));
            return;
        case 0x48005000: // bext (Zbs)
            rvjit_trace_bext(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, (reg1 >> (reg2 & bit_mask(SHAMT_BITS))) & 1);
            return;
        case 0x68001000: // binv (Zbs)
            rvjit_trace_binv(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, reg1 ^ (((xlen_t)1U) << (reg2 & bit_mask(SHAMT_BITS))));
            return;
    }

    riscv_illegal_insn(vm, insn);
}

static forceinline void riscv_emulate_i_lui(rvvm_hart_t* vm, const uint32_t insn)
{
    const regid_t rds = bit_cut(insn, 7, 5);
    const xlen_t  imm = sign_extend(insn & 0xFFFFF000, 32);

    rvjit_trace_li(rds, imm, 4);
    riscv_write_reg(vm, rds, imm);
}

#if defined(RISCV64)

static forceinline void riscv_emulate_i_opc_op32(rvvm_hart_t* vm, const uint32_t insn)
{
    const uint32_t funct = insn & 0xFE007000;
    const regid_t  rds   = bit_cut(insn, 7, 5);
    const regid_t  rs1   = bit_cut(insn, 15, 5);
    const regid_t  rs2   = bit_cut(insn, 20, 5);
    const uint32_t reg1  = riscv_read_reg(vm, rs1);
    const uint32_t reg2  = riscv_read_reg(vm, rs2);

    switch (funct) {
        case 0x00000000: // addw
            rvjit_trace_addw(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, (int32_t)(reg1 + reg2));
            return;
        case 0x40000000: // subw
            rvjit_trace_subw(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, (int32_t)(reg1 - reg2));
            return;
        case 0x00001000: // sllw
            rvjit_trace_sllw(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, (int32_t)(reg1 << (reg2 & 0x1F)));
            return;
        case 0x00005000: // srlw
            rvjit_trace_srlw(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, (int32_t)(reg1 >> (reg2 & 0x1F)));
            return;
        case 0x40005000: // sraw
            rvjit_trace_sraw(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, (int32_t)(((int32_t)reg1) >> (reg2 & 0x1F)));
            return;
        /*
         * Multiplication / Division
         */
        case 0x02000000: // mulw
            rvjit_trace_mulw(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, (int32_t)(reg1 * reg2));
            return;
        case 0x02004000: { // divw
            int32_t result = -1;
            rvjit_trace_divw(rds, rs1, rs2, 4);
            // overflow
            if ((int32_t)reg1 == ((int32_t)0x80000000U) && (int32_t)reg2 == -1) {
                result = ((int32_t)0x80000000U);
                // division by zero check (we already setup result var for error)
            } else if (reg2 != 0) {
                result = (int32_t)reg1 / (int32_t)reg2;
            }
            riscv_write_reg(vm, rds, result);
            return;
        }
        case 0x02005000: { // divuw
            uint32_t result = -1;
            rvjit_trace_divuw(rds, rs1, rs2, 4);
            // overflow
            if (reg2 != 0) {
                result = reg1 / reg2;
            }
            riscv_write_reg(vm, rds, (int32_t)result);
            return;
        }
        case 0x02006000: { // remw
            int32_t result = reg1;
            rvjit_trace_remw(rds, rs1, rs2, 4);
            // overflow
            if ((int32_t)reg1 == ((int32_t)0x80000000U) && (int32_t)reg2 == -1) {
                result = 0;
                // division by zero check (we already setup result var for error)
            } else if (reg2 != 0) {
                result = (int32_t)reg1 % (int32_t)reg2;
            }
            riscv_write_reg(vm, rds, result);
            return;
        }
        case 0x02007000: { // remuw
            uint32_t result = reg1;
            rvjit_trace_remuw(rds, rs1, rs2, 4);
            // division by zero check (we already setup result var for error)
            if (reg2 != 0) {
                result = reg1 % reg2;
            }
            riscv_write_reg(vm, rds, (int32_t)result);
            return;
        }
        /*
         * Zbb: Basic bit-manipulation
         */
        case 0x08004000: // zext.h (Zbb), RV64 encoding
            if (likely(!rs2)) {
                rvjit_trace_andi(rds, rs1, 0xFFFF, 4);
                riscv_write_reg(vm, rds, (uint16_t)reg1);
                return;
            }
            break;
        case 0x60001000: // rolw (Zbb)
            rvjit_trace_rolw(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, (int32_t)bit_rotl32(reg1, reg2 & bit_mask(SHAMT_BITS)));
            return;
        case 0x60005000: // rorw (Zbb)
            rvjit_trace_rorw(rds, rs1, rs2, 4);
            riscv_write_reg(vm, rds, (int32_t)bit_rotr32(reg1, reg2 & bit_mask(SHAMT_BITS)));
            return;
        /*
         * Zba: Address generation
         */
        case 0x08000000: // add.uw (Zba)
            rvjit_trace_shadd_uw(rds, rs1, rs2, 0, 4);
            riscv_write_reg(vm, rds, riscv_read_reg(vm, rs2) + ((xlen_t)reg1));
            return;
        case 0x20002000: // sh1add.uw (Zba)
            rvjit_trace_shadd_uw(rds, rs1, rs2, 1, 4);
            riscv_write_reg(vm, rds, riscv_read_reg(vm, rs2) + (((xlen_t)reg1) << 1));
            return;
        case 0x20004000: // sh2add.uw (Zba)
            rvjit_trace_shadd_uw(rds, rs1, rs2, 2, 4);
            riscv_write_reg(vm, rds, riscv_read_reg(vm, rs2) + (((xlen_t)reg1) << 2));
            return;
        case 0x20006000: // sh3add.uw (Zba)
            rvjit_trace_shadd_uw(rds, rs1, rs2, 3, 4);
            riscv_write_reg(vm, rds, riscv_read_reg(vm, rs2) + (((xlen_t)reg1) << 3));
            return;
    }

    riscv_illegal_insn(vm, insn);
}

#endif

static forceinline void riscv_emulate_i_opc_branch(rvvm_hart_t* vm, const uint32_t insn)
{
    const uint32_t funct3 = bit_cut(insn, 12, 3);
    const regid_t  rs1    = bit_cut(insn, 15, 5);
    const regid_t  rs2    = bit_cut(insn, 20, 5);
    const sxlen_t  offset = decode_i_branch_off(insn);
    switch (funct3) {
        case 0x00: // beq
            if (riscv_read_reg(vm, rs1) == riscv_read_reg(vm, rs2)) {
                const xlen_t pc = riscv_read_reg(vm, RISCV_REG_PC);
                rvjit_trace_beq(rs1, rs2, offset, 4, 4);
                riscv_write_reg(vm, RISCV_REG_PC, pc + offset - 4);
            } else {
                rvjit_trace_bne(rs1, rs2, 4, offset, 4);
            }
            return;
        case 0x01: // bne
            if (riscv_read_reg(vm, rs1) != riscv_read_reg(vm, rs2)) {
                const xlen_t pc = riscv_read_reg(vm, RISCV_REG_PC);
                rvjit_trace_bne(rs1, rs2, offset, 4, 4);
                riscv_write_reg(vm, RISCV_REG_PC, pc + offset - 4);
            } else {
                rvjit_trace_beq(rs1, rs2, 4, offset, 4);
            }
            return;
        case 0x04: // blt
            if (riscv_read_reg_s(vm, rs1) < riscv_read_reg_s(vm, rs2)) {
                const xlen_t pc = riscv_read_reg(vm, RISCV_REG_PC);
                rvjit_trace_blt(rs1, rs2, offset, 4, 4);
                riscv_write_reg(vm, RISCV_REG_PC, pc + offset - 4);
            } else {
                rvjit_trace_bge(rs1, rs2, 4, offset, 4);
            }
            return;
        case 0x05: // bge
            if (riscv_read_reg_s(vm, rs1) >= riscv_read_reg_s(vm, rs2)) {
                const xlen_t pc = riscv_read_reg(vm, RISCV_REG_PC);
                rvjit_trace_bge(rs1, rs2, offset, 4, 4);
                riscv_write_reg(vm, RISCV_REG_PC, pc + offset - 4);
            } else {
                rvjit_trace_blt(rs1, rs2, 4, offset, 4);
            }
            return;
        case 0x06: // bltu
            if (riscv_read_reg(vm, rs1) < riscv_read_reg(vm, rs2)) {
                const xlen_t pc = riscv_read_reg(vm, RISCV_REG_PC);
                rvjit_trace_bltu(rs1, rs2, offset, 4, 4);
                riscv_write_reg(vm, RISCV_REG_PC, pc + offset - 4);
            } else {
                rvjit_trace_bgeu(rs1, rs2, 4, offset, 4);
            }
            return;
        case 0x07: // bgeu
            if (riscv_read_reg(vm, rs1) >= riscv_read_reg(vm, rs2)) {
                const xlen_t pc = riscv_read_reg(vm, RISCV_REG_PC);
                rvjit_trace_bgeu(rs1, rs2, offset, 4, 4);
                riscv_write_reg(vm, RISCV_REG_PC, pc + offset - 4);
            } else {
                rvjit_trace_bltu(rs1, rs2, 4, offset, 4);
            }
            return;
    }
    riscv_illegal_insn(vm, insn);
}

static forceinline void riscv_emulate_i_jalr(rvvm_hart_t* vm, const uint32_t insn)
{
    const uint32_t funct3 = bit_cut(insn, 12, 3);
    if (likely(!funct3)) {
        const regid_t rds      = bit_cut(insn, 7, 5);
        const regid_t rs1      = bit_cut(insn, 15, 5);
        const sxlen_t offset   = sign_extend(bit_cut(insn, 20, 12), 12);
        const xlen_t  pc       = riscv_read_reg(vm, RISCV_REG_PC);
        const xlen_t  jmp_addr = riscv_read_reg(vm, rs1);
        rvjit_trace_jalr(rds, rs1, offset, 4);
        riscv_write_reg(vm, rds, pc + 4);
        riscv_write_reg(vm, RISCV_REG_PC, ((jmp_addr + offset) & (~(xlen_t)1)) - 4);
        return;
    }
    riscv_illegal_insn(vm, insn);
}

static forceinline void riscv_emulate_i_jal(rvvm_hart_t* vm, const uint32_t insn)
{
    const regid_t rds    = bit_cut(insn, 7, 5);
    const sxlen_t offset = decode_i_jal_off(insn);
    const xlen_t  pc     = riscv_read_reg(vm, RISCV_REG_PC);

    rvjit_trace_jal(rds, offset, 4);
    riscv_write_reg(vm, rds, pc + 4);
    riscv_write_reg(vm, RISCV_REG_PC, pc + offset - 4);
}

static forceinline void riscv_emulate_i(rvvm_hart_t* vm, const uint32_t insn)
{
    const uint32_t op = insn & 0x7C;
    switch (op) {
        case RISCV_OPC_LOAD:
            riscv_emulate_i_opc_load(vm, insn);
            return;
#if defined(USE_FPU)
        case RISCV_OPC_LOAD_FP:
            riscv_emulate_f_opc_load(vm, insn);
            return;
#endif
        case RISCV_OPC_MISC_MEM:
            riscv_emulate_opc_misc_mem(vm, insn);
            return;
        case RISCV_OPC_OP_IMM:
            riscv_emulate_i_opc_imm(vm, insn);
            return;
        case RISCV_OPC_AUIPC:
            riscv_emulate_i_auipc(vm, insn);
            return;
#if defined(RISCV64)
        case RISCV_OPC_OP_IMM32:
            riscv_emulate_i_opc_imm32(vm, insn);
            return;
#endif
        case RISCV_OPC_STORE:
            riscv_emulate_i_opc_store(vm, insn);
            return;
#if defined(USE_FPU)
        case RISCV_OPC_STORE_FP:
            riscv_emulate_f_opc_store(vm, insn);
            return;
#endif
        case RISCV_OPC_AMO:
            riscv_emulate_a_opc_amo(vm, insn);
            return;
        case RISCV_OPC_OP:
            riscv_emulate_i_opc_op(vm, insn);
            return;
        case RISCV_OPC_LUI:
            riscv_emulate_i_lui(vm, insn);
            return;
#if defined(RISCV64)
        case RISCV_OPC_OP32:
            riscv_emulate_i_opc_op32(vm, insn);
            return;
#endif
#if defined(USE_FPU)
        case RISCV_OPC_FMADD:
            riscv_emulate_f_fmadd(vm, insn);
            return;
        case RISCV_OPC_FMSUB:
            riscv_emulate_f_fmsub(vm, insn);
            return;
        case RISCV_OPC_FNMSUB:
            riscv_emulate_f_fnmsub(vm, insn);
            return;
        case RISCV_OPC_FNMADD:
            riscv_emulate_f_fnmadd(vm, insn);
            return;
        case RISCV_OPC_OP_FP:
            riscv_emulate_f_opc_op(vm, insn);
            return;
#endif
        case RISCV_OPC_BRANCH:
            riscv_emulate_i_opc_branch(vm, insn);
            return;
        case RISCV_OPC_JALR:
            riscv_emulate_i_jalr(vm, insn);
            return;
        case RISCV_OPC_JAL:
            riscv_emulate_i_jal(vm, insn);
            return;
        case RISCV_OPC_SYSTEM:
            riscv_emulate_opc_system(vm, insn);
            return;
    }
    riscv_illegal_insn(vm, insn);
}

#endif
