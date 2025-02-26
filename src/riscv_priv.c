/*
riscv_priv.c - RISC-V Privileged
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "riscv_priv.h"
#include "riscv_csr.h"
#include "riscv_hart.h"
#include "riscv_mmu.h"
#include "riscv_cpu.h"
#include "bit_ops.h"
#include "atomics.h"

// Precise instruction values for SYSTEM opcode decoding
#define RISCV_PRIV_S_ECALL       0x73
#define RISCV_PRIV_S_EBREAK      0x100073
#define RISCV_PRIV_S_SRET        0x10200073
#define RISCV_PRIV_S_MRET        0x30200073
#define RISCV_PRIV_S_WFI         0x10500073

// Privileged FENCE instructions mask and decoding
#define RV_PRIV_S_FENCE_MASK  0xFE007FFF
#define RV_PRIV_S_SFENCE_VMA  0x12000073

slow_path void riscv_emulate_opc_system(rvvm_hart_t* vm, const uint32_t insn)
{
    switch (insn) {
        case RISCV_PRIV_S_ECALL:
            riscv_trap(vm, RISCV_TRAP_ECALL_UMODE + vm->priv_mode, 0);
            return;
        case RISCV_PRIV_S_EBREAK:
            riscv_breakpoint(vm);
            return;
        case RISCV_PRIV_S_SRET:
            // Executing sret should trap in S-mode when mstatus.TSR is enabled
            if (likely((vm->priv_mode >= RISCV_PRIV_SUPERVISOR
                    && !(vm->csr.status & CSR_STATUS_TSR))
                    || vm->priv_mode == RISCV_PRIV_MACHINE)) {
                uint8_t next_priv = bit_cut(vm->csr.status, 8, 1);
                // Set SPP to U
                vm->csr.status = bit_replace(vm->csr.status, 8, 1, RISCV_PRIV_USER);
                // Set SIE to SPIE
                vm->csr.status = bit_replace(vm->csr.status, 1, 1, bit_cut(vm->csr.status, 5, 1));
                // Set PC to csr.sepc, counteract PC increment done by interpreter
                vm->registers[RISCV_REG_PC] = vm->csr.epc[RISCV_PRIV_SUPERVISOR] - 4;
                // Set privilege mode to SPP
                riscv_switch_priv(vm, next_priv);
                riscv_hart_check_interrupts(vm);
                return;
            }
            break;
        case RISCV_PRIV_S_MRET:
            if (likely(vm->priv_mode == RISCV_PRIV_MACHINE)) {
                uint8_t next_priv = bit_cut(vm->csr.status, 11, 2);
                if (next_priv < RISCV_PRIV_MACHINE) {
                    // Clear MPRV when returning to less privileged mode
                    vm->csr.status &= ~CSR_STATUS_MPRV;
                }
                // Set MPP to U
                vm->csr.status = bit_replace(vm->csr.status, 11, 2, RISCV_PRIV_USER);
                // Set MIE to MPIE
                vm->csr.status = bit_replace(vm->csr.status, 3, 1, bit_cut(vm->csr.status, 7, 1));
                // Set PC to csr.mepc, counteract PC increment done by interpreter
                vm->registers[RISCV_REG_PC] = vm->csr.epc[RISCV_PRIV_MACHINE] - 4;
                // Set privilege mode to MPP
                riscv_switch_priv(vm, next_priv);
                riscv_hart_check_interrupts(vm);
                return;
            }
            break;
        case RISCV_PRIV_S_WFI:
            // Executing wfi should trap in S-mode when mstatus.TSR is enabled, and in U-mode
            if (likely((vm->priv_mode >= RISCV_PRIV_SUPERVISOR
                    && !(vm->csr.status & CSR_STATUS_TW))
                    || vm->priv_mode == RISCV_PRIV_MACHINE)) {
                // Resume execution for locally enabled interrupts pending at any privilege level
                if (!riscv_interrupts_pending(vm)) {
                    while (atomic_load_uint32_ex(&vm->running, ATOMIC_RELAXED)) {
                        // Stall the hart until an interrupt might need servicing
                        uint64_t delay = CONDVAR_INFINITE;
                        if (vm->csr.ie & (1U << RISCV_INTERRUPT_MTIMER)) {
                            delay = rvtimecmp_delay_ns(&vm->mtimecmp);
                        }
                        if (vm->csr.ie & (1U << RISCV_INTERRUPT_STIMER)) {
                            delay = EVAL_MIN(delay, rvtimecmp_delay_ns(&vm->stimecmp));
                        }
                        condvar_wait_ns(vm->wfi_cond, delay);

                        // Check timer expiration
                        riscv_hart_check_timer(vm);
                    }
                }
                return;
            }
            break;
    }

    const regid_t rds = bit_cut(insn, 7, 5);
    const uint32_t funct3 = bit_cut(insn, 12, 3);
    const regid_t rs1 = bit_cut(insn, 15, 5);
    const uint32_t csr = insn >> 20;

    switch (funct3) {
        case 0x0:
            switch (insn & RV_PRIV_S_FENCE_MASK) {
                case RV_PRIV_S_SFENCE_VMA:
                    // Allow sfence.vma only when in S-mode and TVM isn't enabled, or more privileged
                    if (likely((vm->priv_mode >= RISCV_PRIV_SUPERVISOR
                            && !(vm->csr.status & CSR_STATUS_TVM))
                            || vm->priv_mode == RISCV_PRIV_MACHINE)) {
                        if (rs1) {
                            riscv_tlb_flush_page(vm, vm->registers[rs1]);
                        } else {
                            riscv_tlb_flush(vm);
                        }
                        return;
                    }
                    break;
            }
            break;
        case 0x1: { // csrrw
            rvvm_uxlen_t val = vm->registers[rs1];
            if (riscv_csr_op(vm, csr, &val, CSR_SWAP)) {
                vm->registers[rds] = val;
                return;
            }
            break;
        }
        case 0x2: { // csrrs
            rvvm_uxlen_t val = vm->registers[rs1];
            if (riscv_csr_op(vm, csr, &val, CSR_SETBITS)) {
                vm->registers[rds] = val;
                return;
            }
            break;
        }
        case 0x3: { // csrrc
            rvvm_uxlen_t val = vm->registers[rs1];
            if (riscv_csr_op(vm, csr, &val, CSR_CLEARBITS)) {
                vm->registers[rds] = val;
                return;
            }
            break;
        }
        case 0x5: { // csrrwi
            rvvm_uxlen_t val = bit_cut(insn, 15, 5);
            if (riscv_csr_op(vm, csr, &val, CSR_SWAP)) {
                vm->registers[rds] = val;
                return;
            }
            break;
        }
        case 0x6: { // csrrsi
            rvvm_uxlen_t val = bit_cut(insn, 15, 5);
            if (riscv_csr_op(vm, csr, &val, CSR_SETBITS)) {
                vm->registers[rds] = val;
                return;
            }
            break;
        }
        case 0x7: { // csrrci
            rvvm_uxlen_t val = bit_cut(insn, 15, 5);
            if (riscv_csr_op(vm, csr, &val, CSR_CLEARBITS)) {
                vm->registers[rds] = val;
                return;
            }
            break;
        }
    }

    riscv_illegal_insn(vm, insn);
}

#define RISCV_INSN_PAUSE 0x100000F // Instruction value for pause hint

TSAN_SUPPRESS static forceinline void riscv_cbo_zero(rvvm_hart_t* vm, rvvm_addr_t vaddr)
{
    // ThreadSanitizer doesn't recognize fences between non-atomic memset and AMO
    void* ptr = riscv_rmw_translate(vm, vaddr & ~63ULL, NULL, 64);
    if (ptr) {
        memset(assume_aligned_ptr(ptr, 64), 0, 64);
    }
}

slow_path void riscv_emulate_opc_misc_mem(rvvm_hart_t* vm, const uint32_t insn)
{
    const uint32_t funct3 = bit_cut(insn, 12, 3);
    switch (funct3) {
        case 0x0: // fence
            if (unlikely(insn == RISCV_INSN_PAUSE)) {
                // Pause hint, yield the vCPU thread
                sleep_ms(0);
            } else if (unlikely((insn & 0x05000000) && (insn & 0x00A00000))) {
                // StoreLoad fence needed (SEQ_CST)
                atomic_fence_ex(ATOMIC_SEQ_CST);
            } else {
                // LoadLoad, LoadStore, StoreStore fence (ACQ_REL)
                atomic_fence_ex(ATOMIC_ACQ_REL);
            }
            return;
        case 0x1: // fence.i
#ifdef USE_JIT
            if (rvvm_get_opt(vm->machine, RVVM_OPT_JIT_HARVARD)) {
                riscv_jit_flush_cache(vm);
            } else {
                // This eliminates possible dangling dirty blocks in JTLB
                riscv_jit_tlb_flush(vm);
            }
#endif
            return;
        case 0x2: {
            const regid_t rds = bit_cut(insn, 7, 5);
            const regid_t rs1 = bit_cut(insn, 15, 5);
            switch (insn >> 20) {
                case 0x0: // cbo.inval
                    if (likely(!rds && riscv_csr_cbi_enabled(vm))) {
                        // Simply use a fence, all emulated devices are coherent
                        atomic_fence();
                        return;
                    }
                    break;
                case 0x1: // cbo.clean
                case 0x2: // cbo.flush
                    if (likely(!rds && riscv_csr_cbcf_enabled(vm))) {
                        // Simply use a fence, all emulated devices are coherent
                        atomic_fence();
                        return;
                    }
                    break;
                case 0x4: // cbo.zero
                    if (likely(!rds && riscv_csr_cbz_enabled(vm))) {
                        riscv_cbo_zero(vm, vm->registers[rs1]);
                        return;
                    }
                    break;
            }
            break;
        }
    }
    riscv_illegal_insn(vm, insn);
}
