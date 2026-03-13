/*
rvvm_vmm.h - Hardware Virtualization (Virtual Machine Monitor)
Copyright (C) 2024  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _RVVM_VMM_API_H
#define _RVVM_VMM_API_H

#include <rvvm/rvvm.h>

RVVM_EXTERN_C_BEGIN

/**
 * @defgroup rvvm_vmm_api Hardware Virtualization (Virtual Machine Monitor)
 * @addtogroup rvvm_vmm_api
 * @{
 *
 * VMM interface is backed by a native host backend, such as:
 * - KVM /dev/kvm (Linux)
 * - Bhyve /dev/vmm (FreeBSD)
 * - VMM /dev/vmm (OpenBSD)
 * - NVMM /dev/misc/nvmm (NetBSD, DragonFly, possibly Haiku)
 * - HVF hv_vm_* (Apple)
 * - WHPX (Windows)
 *
 * The basic functionality is:
 * - Create VMM machine
 * - Map host process memory as VMM physical memory region
 * - Create vCPU attached to a VMM
 * - Get/Set vCPU registers
 * - Run vCPU from current thread
 * - Kick running vCPU to pause it from another thread
 * - Handle vCPU exits (MMIO/PMIO access, halt, signal)
 *
 * The various backends may support additional sets of features:
 * - Create hardware-virtualized interrupt controllers
 * - Send level-triggered / MSI IRQs to the VMM
 * - Send IRQ vector to the vCPU
 */

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IA32)
#define RVVM_VMM_X86
#elif defined(__aarch64__) || defined(_M_ARM64)
#define RVVM_VMM_ARM64
#elif defined(__riscv)
#define RVVM_VMM_RISCV
#elif defined(__mips64__)
#define RVVM_VMM_MIPS
#elif defined(__powerpc64__)
#define RVVM_VMM_PPC
#endif

/*
 * Exit reasons for rvvm_vmm_vcpu_run()
 */
#define RVVM_VMM_EXIT_KICKED     0x00 /**< Kicked out from vCPU execution */
#define RVVM_VMM_EXIT_PORT_R     0x01 /**< IO Port read */
#define RVVM_VMM_EXIT_PORT_W     0x02 /**< IO Port write */
#define RVVM_VMM_EXIT_MMIO_R     0x03 /**< MMIO read */
#define RVVM_VMM_EXIT_MMIO_W     0x04 /**< MMIO write */
#define RVVM_VMM_EXIT_RDMSR      0x05 /**< x86 MSR read */
#define RVVM_VMM_EXIT_WRMSR      0x06 /**< x86 MSR write */
#define RVVM_VMM_EXIT_VCALL      0x07 /**< Hypercall */
#define RVVM_VMM_EXIT_DEBUG      0x08 /**< Hit a breakpoint in debug mode */
#define RVVM_VMM_EXIT_FATAL      0x09 /**< Fatal error reported by backend */
#define RVVM_VMM_EXIT_TRAP       0x0A /**< Caught exception */
#define RVVM_VMM_EXIT_HALT       0x0B /**< Wait for interrupt */

/*
 * Device types for rvvm_vmm_dev_init()
 */
#define RVVM_VMM_DEV_X86_APIC    0x00 /**< PIC, IOAPIC, LAPIC */
#define RVVM_VMM_DEV_X86_PIT     0x01 /**< i8254 PIT, PC Speaker */
#define RVVM_VMM_DEV_ARM64_GICV2 0x02 /**< GICv2 */
#define RVVM_VMM_DEV_ARM64_GICV3 0x03 /**< GICv3 */

/**
 * Register indexes for rvvm_vmm_vcpu_get_reg() / rvvm_vmm_vcpu_set_reg()
 */

#if defined(RVVM_VMM_X86)

#define RVVM_VMM_REG_X86_GPR_BASE   0x00000000UL
#define RVVM_VMM_REG_X86_GPR_SIZE   0x00000012UL
#define RVVM_VMM_REG_X86_GPR_RAX    0x00000000UL
#define RVVM_VMM_REG_X86_GPR_RCX    0x00000001UL
#define RVVM_VMM_REG_X86_GPR_RDX    0x00000002UL
#define RVVM_VMM_REG_X86_GPR_RBX    0x00000003UL
#define RVVM_VMM_REG_X86_GPR_RSP    0x00000004UL
#define RVVM_VMM_REG_X86_GPR_RBP    0x00000005UL
#define RVVM_VMM_REG_X86_GPR_RSI    0x00000006UL
#define RVVM_VMM_REG_X86_GPR_RDI    0x00000007UL
#define RVVM_VMM_REG_X86_GPR_R8     0x00000008UL
#define RVVM_VMM_REG_X86_GPR_R9     0x00000009UL
#define RVVM_VMM_REG_X86_GPR_R10    0x0000000AUL
#define RVVM_VMM_REG_X86_GPR_R11    0x0000000BUL
#define RVVM_VMM_REG_X86_GPR_R12    0x0000000CUL
#define RVVM_VMM_REG_X86_GPR_R13    0x0000000DUL
#define RVVM_VMM_REG_X86_GPR_R14    0x0000000EUL
#define RVVM_VMM_REG_X86_GPR_R15    0x0000000FUL
/* Instruction pointer */
#define RVVM_VMM_REG_X86_GPR_RIP    0x00000010UL
/* Flags */
#define RVVM_VMM_REG_X86_GPR_RFL    0x00000011UL

#define RVVM_VMM_REG_X86_FPU_BASE   0x10000000UL
#define RVVM_VMM_REG_X86_FPU_SIZE   0x00000014UL
/* FPU Control [0:15], Status [16:31], Last Opcode [32:47], Tag Word [48:63] (XSAVE format) */
#define RVVM_VMM_REG_X86_FPU_FCSW   0x10000011UL
/* Last FPU Instruction Pointer */
#define RVVM_VMM_REG_X86_FPU_LSIP   0x10000012UL
/* Last FPU Data Pointer */
#define RVVM_VMM_REG_X86_FPU_LSDP   0x10000013UL

#define RVVM_VMM_REG_X86_XMM_BASE   0x20000000UL
#define RVVM_VMM_REG_X86_XMM_SIZE   0x00000020UL
#define RVVM_VMM_REG_X86_XMM_MXCSR  0x20000080UL

#define RVVM_VMM_REG_X86_SEG_BASE   0x50000000UL
#define RVVM_VMM_REG_X86_SEG_SIZE   0x00000014UL
/* Limit [0:15][48:51], Base[16:39][56:63], Type[40:43], S[44], DPL[45:46], P[47], AVL[52], L[53], DB[54], G[55] */
#define RVVM_VMM_REG_X86_SEG_ES     0x50000000UL
/* Selector[0:15], High Base [16:47] */
#define RVVM_VMM_REG_X86_SEG_ES_H   0x50000001UL
#define RVVM_VMM_REG_X86_SEG_CS     0x50000002UL
#define RVVM_VMM_REG_X86_SEG_CS_H   0x50000003UL
#define RVVM_VMM_REG_X86_SEG_SS     0x50000004UL
#define RVVM_VMM_REG_X86_SEG_SS_H   0x50000005UL
#define RVVM_VMM_REG_X86_SEG_DS     0x50000006UL
#define RVVM_VMM_REG_X86_SEG_DS_H   0x50000007UL
#define RVVM_VMM_REG_X86_SEG_FS     0x50000008UL
#define RVVM_VMM_REG_X86_SEG_FS_H   0x50000009UL
#define RVVM_VMM_REG_X86_SEG_GS     0x5000000AUL
#define RVVM_VMM_REG_X86_SEG_GS_H   0x5000000BUL
#define RVVM_VMM_REG_X86_SEG_GDT    0x5000000CUL
#define RVVM_VMM_REG_X86_SEG_GDT_H  0x5000000DUL
#define RVVM_VMM_REG_X86_SEG_IDT    0x5000000EUL
#define RVVM_VMM_REG_X86_SEG_IDT_H  0x5000000FUL
#define RVVM_VMM_REG_X86_SEG_LDT    0x50000010UL
#define RVVM_VMM_REG_X86_SEG_LDT_H  0x50000011UL
#define RVVM_VMM_REG_X86_SEG_TR     0x50000012UL
#define RVVM_VMM_REG_X86_SEG_TR_H   0x50000013UL

#define RVVM_VMM_REG_X86_CR_BASE    0x60000000UL
#define RVVM_VMM_REG_X86_CR_SIZE    0x00000006UL
#define RVVM_VMM_REG_X86_CR_CR0     0x60000000UL
#define RVVM_VMM_REG_X86_CR_CR2     0x60000001UL
#define RVVM_VMM_REG_X86_CR_CR3     0x60000002UL
#define RVVM_VMM_REG_X86_CR_CR4     0x60000003UL
#define RVVM_VMM_REG_X86_CR_CR8     0x60000004UL
#define RVVM_VMM_REG_X86_CR_XCR0    0x60000005UL

#define RVVM_VMM_REG_X86_DR_BASE    0x70000000UL
#define RVVM_VMM_REG_X86_DR_SIZE    0x00000006UL
#define RVVM_VMM_REG_X86_DR_DR0     0x70000000UL
#define RVVM_VMM_REG_X86_DR_DR1     0x70000001UL
#define RVVM_VMM_REG_X86_DR_DR2     0x70000002UL
#define RVVM_VMM_REG_X86_DR_DR3     0x70000003UL
#define RVVM_VMM_REG_X86_DR_DR6     0x70000004UL
#define RVVM_VMM_REG_X86_DR_DR7     0x70000005UL

#define RVVM_VMM_REG_X86_MSR_BASE   0x80000000UL
#define RVVM_VMM_REG_X86_MSR_SIZE   0x0000000CUL
/* Extended features */
#define RVVM_VMM_REG_X86_MSR_EFER   0x80000000UL
/* APIC Base (Only for an in-kernel APIC) */
#define RVVM_VMM_REG_X86_MSR_APICB  0x80000001UL
/* Legacy mode SYSCALL target */
#define RVVM_VMM_REG_X86_MSR_STAR   0x80000002UL
/* Long mode SYSCALL target */
#define RVVM_VMM_REG_X86_MSR_LSTAR  0x80000003UL
/* Compat mode SYSCALL target */
#define RVVM_VMM_REG_X86_MSR_CSTAR  0x80000004UL
/* Syscall flags mask */
#define RVVM_VMM_REG_X86_MSR_SFMASK 0x80000005UL
/* SWAPGS GS shadow */
#define RVVM_VMM_REG_X86_MSR_SWAPGS 0x80000006UL
/* Ring 0 code segment */
#define RVVM_VMM_REG_X86_MSR_SYSCS  0x80000007UL
/* Ring 0 stack pointer */
#define RVVM_VMM_REG_X86_MSR_SYSESP 0x80000008UL
/* Ring 0 instruction pointer */
#define RVVM_VMM_REG_X86_MSR_SYSEIP 0x80000009UL
/* Page attribute table */
#define RVVM_VMM_REG_X86_MSR_PAT    0x8000000AUL
/* TSC offset */
#define RVVM_VMM_REG_X86_MSR_TSC    0x8000000BUL

#elif defined(RVVM_VMM_ARM64)

#define RVVM_VMM_REG_ARM64_GPR_BASE   0x00000000UL
#define RVVM_VMM_REG_ARM64_GPR_SIZE   0x00000020UL
#define RVVM_VMM_REG_ARM64_GPR_PC     0x00000080UL
#define RVVM_VMM_REG_ARM64_GPR_PSTATE 0x00000081UL

#define RVVM_VMM_REG_ARM64_VEC_BASE   0x10000000UL
#define RVVM_VMM_REG_ARM64_VEC_SIZE   0x00000040UL
#define RVVM_VMM_REG_ARM64_VEC_FPSR   0x10000080UL
#define RVVM_VMM_REG_ARM64_VEC_FPCR   0x10000081UL

#define RVVM_VMM_REG_ARM64_EL1_BASE   0x70000000UL
#define RVVM_VMM_REG_ARM64_EL1_SIZE   0x00000002UL
#define RVVM_VMM_REG_ARM64_EL1_SP     0x70000000UL
#define RVVM_VMM_REG_ARM64_EL1_ELR    0x70000001UL

#define RVVM_VMM_REG_ARM64_SPSR_BASE  0x80000000UL
#define RVVM_VMM_REG_ARM64_SPSR_SIZE  0x00000005UL
#define RVVM_VMM_REG_ARM64_SPSR_EL1   0x80000000UL
#define RVVM_VMM_REG_ARM64_SPSR_ABT   0x80000001UL
#define RVVM_VMM_REG_ARM64_SPSR_UND   0x80000002UL
#define RVVM_VMM_REG_ARM64_SPSR_IRQ   0x80000003UL
#define RVVM_VMM_REG_ARM64_SPSR_FIQ   0x80000004UL

#elif defined(RVVM_VMM_RISCV)

#define RVVM_VMM_REG_RISCV_GPR_BASE 0x00000000UL
#define RVVM_VMM_REG_RISCV_GPR_SIZE 0x00000020UL
#define RVVM_VMM_REG_RISCV_GPR_PC   0x00000080UL

#define RVVM_VMM_REG_RISCV_FPU_BASE 0x10000000UL
#define RVVM_VMM_REG_RISCV_FPU_SIZE 0x00000020UL

#define RVVM_VMM_REG_RISCV_RVV_BASE 0x20000000UL
#define RVVM_VMM_REG_RISCV_RVV_SIZE 0x00000080UL

#endif

/**
 * VMM context
 */
typedef struct rvvm_vmm_machine_internal rvvm_vmm_mach_t;

/**
 * vCPU context
 */
typedef struct rvvm_vmm_vcpu_internal rvvm_vmm_vcpu_t;

/**
 * Create VMM context
 */
rvvm_vmm_mach_t* rvvm_vmm_init(void);

/**
 * Free VMM context
 */
void rvvm_vmm_free(rvvm_vmm_mach_t* vmm);

/**
 * Create VMM physical memory region, map into process memory
 */
void* rvvm_vmm_map_phys(rvvm_vmm_mach_t* vmm, uint64_t phys, size_t size);

/**
 * Unmap VMM physical memory region from process memory
 */
void rvvm_vmm_unmap_phys(rvvm_vmm_mach_t* vmm, void* data, size_t size);

/**
 * Attach existing process memory as VMM physical memory region (For framebuffers, etc)
 * \note May fail or destroy contents of memory region (NVMM, bhyve quirks)
 */
bool rvvm_vmm_attach_phys(rvvm_vmm_mach_t* vmm, uint64_t phys, void* data, size_t size);

/**
 * Detach physical memory region from VMM
 */
void rvvm_vmm_detach_phys(rvvm_vmm_mach_t* vmm, uint64_t phys, size_t size);

/**
 * Initialize interrupt controllers on VMM backend side, if supported
 * \param type Specifies one of RVVM_VMM_DEV_ constants
 */
bool rvvm_vmm_dev_init(rvvm_vmm_mach_t* vmm, uint32_t type);

/**
 * Set level-triggered wired IRQ level for VMM
 * \note Only valid with a hardware-virtualized interrupt controller
 */
void rvvm_vmm_irq_level(rvvm_vmm_mach_t* vmm, uint32_t irq, bool set);

/**
 * Send MSI IRQ to VMM
 * \note Only valid with a hardware-virtualized interrupt controller
 */
void rvvm_vmm_irq_msi(rvvm_vmm_mach_t* vmm, uint64_t addr, uint32_t data);

/**
 * Create vCPU attached to a VMM
 */
rvvm_vmm_vcpu_t* rvvm_vmm_vcpu_init(rvvm_vmm_mach_t* vmm, uint32_t cpu_id);

/**
 * Free vCPU
 */
void rvvm_vmm_vcpu_free(rvvm_vmm_vcpu_t* vcpu);

/**
 * Run vCPU context. Returns one of RVVM_VMM_EXIT_ constants
 */
uint32_t rvvm_vmm_vcpu_run(rvvm_vmm_vcpu_t* vcpu);

/**
 * Register vCPU thread once for rvvm_vmm_vcpu_kick() to work
 */
void rvvm_vmm_vcpu_register(rvvm_vmm_vcpu_t* vcpu);

/**
 * Kick vCPU to return from rvvm_vmm_vcpu_run() as soon as possible
 */
void rvvm_vmm_vcpu_kick(rvvm_vmm_vcpu_t* vcpu);

/**
 * Get vCPU register value
 */
uint64_t rvvm_vmm_vcpu_get_reg(rvvm_vmm_vcpu_t* vcpu, uint32_t reg_id);

/**
 * Set vCPU register value. Pure state manipulation, do NOT expect any side effects to happen.
 */
void rvvm_vmm_vcpu_set_reg(rvvm_vmm_vcpu_t* vcpu, uint32_t reg_id, uint64_t val);

/**
 * Get MMIO / Port IO operation address
 */
uint64_t rvvm_vmm_vcpu_op_addr(rvvm_vmm_vcpu_t* vcpu);

/**
 * Get MMIO / Port IO operation size
 */
uint32_t rvvm_vmm_vcpu_op_size(rvvm_vmm_vcpu_t* vcpu);

/**
 * Access MMIO / Port IO operation data
 */
void* rvvm_vmm_vcpu_op_data(rvvm_vmm_vcpu_t* vcpu);

/**
 * Send vCPU an interrupt vector (As opposed to platform-level IRQ), if supported
 */
bool rvvm_vmm_vcpu_interrupt(rvvm_vmm_vcpu_t* vcpu, uint32_t vec, bool set);

/**
 * Enable debug mode, where any int3 or equialent causes RVVM_VMM_EXIT_DEBUG, if supported
 */
bool rvvm_vmm_vcpu_debug_mode(rvvm_vmm_vcpu_t* vcpu, bool enable);

/** @}*/

RVVM_EXTERN_C_END

#endif
