/*
vmm_kvm.h - RVVM KVM Backend
Copyright (C) 2024  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_VMM_KVM_H
#define RVVM_VMM_KVM_H

#include <rvvm/rvvm_vmm.h>

#include "atomics.h"
#include "mem_ops.h"
#include "utils.h"
#include "vma_ops.h"

#include <pthread.h>
#include <signal.h>

#include <fcntl.h>
#include <linux/kvm.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define RVVM_KVM_GPR_DIRTY 0x01
#define RVVM_KVM_FPU_DIRTY 0x02
#define RVVM_KVM_SRS_DIRTY 0x04
#define RVVM_KVM_DBG_DIRTY 0x08
#define RVVM_KVM_MSR_DIRTY 0x10

struct rvvm_vmm_machine_internal {
    // Memory ranges
    struct kvm_userspace_memory_region ranges[32];

    // KVM machine descriptor
    int fd;
};

struct rvvm_vmm_vcpu_internal {
    // Shared memory with kernel space
    struct kvm_run* run;

    // Register buffers
    struct kvm_regs*  regs;
    struct kvm_fpu*   fpu;
    struct kvm_sregs* sregs;

    struct kvm_guest_debug_arch* debug;

#if defined(RVVM_VMM_X86)
    struct kvm_msrs* msr;
#endif

    // Bitmask of dirty register groups
    uint32_t dirty;

    // vCPU running thread
    pthread_t thread;
    uint32_t  registered;

    // KVM vCPU descriptor
    int fd;
};

static int rvvm_kvm_systemfd = 0;
static int rvvm_kvm_run_size = 0;

static void kvm_init_once(void)
{
    rvvm_kvm_systemfd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (rvvm_kvm_systemfd > 0) {
        int kvm_ver = ioctl(rvvm_kvm_systemfd, KVM_GET_API_VERSION, 0);
        if (kvm_ver != KVM_API_VERSION) {
            rvvm_warn("Invalid KVM API version %#x, expected %#x", kvm_ver, (int)KVM_API_VERSION);
            close(rvvm_kvm_systemfd);
            rvvm_kvm_systemfd = 0;
        }
        rvvm_kvm_run_size = ioctl(rvvm_kvm_systemfd, KVM_GET_VCPU_MMAP_SIZE, 0);
        if (rvvm_kvm_run_size <= 0) {
            rvvm_error("Invalid KVM vCPU struct kvm_run mmap size!");
            close(rvvm_kvm_systemfd);
            rvvm_kvm_systemfd = 0;
            rvvm_kvm_run_size = 0;
        }
    } else {
        rvvm_info("Failed to open /dev/kvm");
    }
}

static bool kvm_init(void)
{
    DO_ONCE(kvm_init_once());
    return rvvm_kvm_systemfd > 0;
}

rvvm_vmm_mach_t* rvvm_vmm_init(void)
{
    if (kvm_init()) {
        int vmm_fd = ioctl(rvvm_kvm_systemfd, KVM_CREATE_VM, 0);
        if (vmm_fd > 0) {
            rvvm_vmm_mach_t* vmm = safe_new_obj(rvvm_vmm_mach_t);
#if defined(RVVM_VMM_X86) && defined(USE_KVM_LEGACY)
            // Workaround for old Intel hosts without unrestricted_guest
            uint64_t ident_map = 0xFFFFC000UL;
            ioctl(vmm_fd, KVM_SET_TSS_ADDR, 0xFFFFD000UL);
            ioctl(vmm_fd, KVM_SET_IDENTITY_MAP_ADDR, &ident_map);
#endif
            vmm->fd = vmm_fd;
            return vmm;
        }
    }

    return NULL;
}

void rvvm_vmm_free(rvvm_vmm_mach_t* vmm)
{
    if (vmm) {
        if (vmm->fd > 0) {
            close(vmm->fd);
        }
        free(vmm);
    }
}

void* rvvm_vmm_map_phys(rvvm_vmm_mach_t* vmm, uint64_t phys, size_t size)
{
    if (vmm) {
        void* vma = vma_alloc(NULL, size, VMA_RDWR);
        if (vma && rvvm_vmm_attach_phys(vmm, phys, vma, size)) {
            return vma;
        }
        vma_free(vma, size);
    }
    return NULL;
}

void rvvm_vmm_unmap_phys(rvvm_vmm_mach_t* vmm, void* data, size_t size)
{
    UNUSED(vmm);
    vma_free(data, size);
}

bool rvvm_vmm_attach_phys(rvvm_vmm_mach_t* vmm, uint64_t phys, void* data, size_t size)
{
    if (vmm && size) {
        // Search for overlapping regions
        for (size_t i = 0; i < STATIC_ARRAY_SIZE(vmm->ranges); ++i) {
            uint64_t rg_phys = vmm->ranges[i].guest_phys_addr;
            uint64_t rg_size = vmm->ranges[i].memory_size;
            if (rg_phys < (phys + size) && phys < (rg_phys + rg_size)) {
                if (!data && rg_phys == phys && rg_size == size) {
                    // Detach memory region
                    vmm->ranges[i].slot            = i;
                    vmm->ranges[i].guest_phys_addr = 0;
                    vmm->ranges[i].memory_size     = 0;
                    vmm->ranges[i].userspace_addr  = 0;
                    ioctl(vmm->fd, KVM_SET_USER_MEMORY_REGION, &vmm->ranges[i]);
                    return true;
                }
                return false;
            }
        }
        // Try to attach new memory region
        for (size_t i = 0; i < STATIC_ARRAY_SIZE(vmm->ranges); ++i) {
            if (data && !vmm->ranges[i].memory_size) {
                // Found empty slot to attach
                vmm->ranges[i].slot            = i;
                vmm->ranges[i].guest_phys_addr = phys;
                vmm->ranges[i].memory_size     = size;
                vmm->ranges[i].userspace_addr  = (size_t)data;
                if (!ioctl(vmm->fd, KVM_SET_USER_MEMORY_REGION, &vmm->ranges[i])) {
                    return true;
                } else {
                    // Failed to attach region
                    vmm->ranges[i].memory_size = 0;
                    return false;
                }
            }
        }
    }
    return false;
}

void rvvm_vmm_detach_phys(rvvm_vmm_mach_t* vmm, uint64_t phys, size_t size)
{
    rvvm_vmm_attach_phys(vmm, phys, NULL, size);
}

bool rvvm_vmm_dev_init(rvvm_vmm_mach_t* vmm, uint32_t type)
{
    if (vmm) {
#if defined(RVVM_VMM_X86)
        switch (type) {
            case RVVM_VMM_DEV_X86_APIC:
                return !ioctl(vmm->fd, KVM_CREATE_IRQCHIP);
            case RVVM_VMM_DEV_X86_PIT: {
                struct kvm_pit_config pit_cfg = {.flags = 1};
                return !ioctl(vmm->fd, KVM_CREATE_PIT2, &pit_cfg);
            }
        }
#elif defined(RVVM_VMM_ARM64)
        switch (type) {
            case RVVM_VMM_DEV_ARM64_GICV2:
                return !ioctl(vmm->fd, KVM_CREATE_IRQCHIP);
            case RVVM_VMM_DEV_X86_PIT: {
                struct kvm_create_device dev_cfg = {.type = KVM_DEV_TYPE_ARM_VGIC_V3};
                return !ioctl(vmm->fd, KVM_CREATE_DEVICE, &dev_cfg);
            }
        }
#endif
    }
    return false;
}

void rvvm_vmm_irq_level(rvvm_vmm_mach_t* vmm, uint32_t irq, bool set)
{
    if (vmm) {
        struct kvm_irq_level irq_lvl = {
            .irq   = irq,
            .level = set,
        };
        ioctl(vmm->fd, KVM_IRQ_LINE, &irq_lvl);
    }
}

void rvvm_vmm_irq_msi(rvvm_vmm_mach_t* vmm, uint64_t addr, uint32_t data)
{
    if (vmm) {
        struct kvm_msi msi = {
            .address_lo = (uint32_t)addr,
            .address_hi = addr >> 32,
            .data       = data,
        };
        ioctl(vmm->fd, KVM_SIGNAL_MSI, &msi);
    }
}

rvvm_vmm_vcpu_t* rvvm_vmm_vcpu_init(rvvm_vmm_mach_t* vmm, uint32_t cpu_id)
{
    if (vmm) {
        rvvm_vmm_vcpu_t* vcpu = safe_new_obj(rvvm_vmm_vcpu_t);
        // Create new vCPU FD
        vcpu->fd = ioctl(vmm->fd, KVM_CREATE_VCPU, cpu_id);
        if (vcpu->fd < 0) {
            rvvm_error("Failed to create KVM vCPU!");
            rvvm_vmm_vcpu_free(vcpu);
            return NULL;
        }
        vcpu->run = mmap(NULL, rvvm_kvm_run_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu->fd, 0);
        if (vcpu->run == MAP_FAILED) {
            vcpu->run = NULL;
        }
        if (!vcpu->run) {
            rvvm_error("Failed to mmap KVM vCPU struct kvm_run!");
            vcpu->run = NULL;
            rvvm_vmm_vcpu_free(vcpu);
            return NULL;
        }
#if defined(RVVM_VMM_X86)
        // Passthrough host cpuid
        uint32_t* cpuid = safe_new_arr(uint32_t, 2 + (256 * 40));
        cpuid[0]        = 256;
        ioctl(rvvm_kvm_systemfd, KVM_GET_SUPPORTED_CPUID, cpuid);
        ioctl(vcpu->fd, KVM_SET_CPUID, cpuid);
        free(cpuid);
#endif
        return vcpu;
    }
    return NULL;
}

void rvvm_vmm_vcpu_free(rvvm_vmm_vcpu_t* vcpu)
{
    if (vcpu) {
        if (vcpu->run) {
            munmap(vcpu->run, rvvm_kvm_run_size);
        }
        if (vcpu->fd > 0) {
            close(vcpu->fd);
        }
        free(vcpu);
    }
}

static void kvm_sigcont_handler(int sig)
{
    UNUSED(sig);
}

uint32_t rvvm_vmm_vcpu_run(rvvm_vmm_vcpu_t* vcpu)
{
    if (vcpu) {
        // Write back dirty vCPU state
        if (vcpu->regs) {
            if (vcpu->dirty & RVVM_KVM_GPR_DIRTY) {
                ioctl(vcpu->fd, KVM_SET_REGS, vcpu->regs);
            }
            free(vcpu->regs);
            vcpu->regs = NULL;
        }
        if (vcpu->fpu) {
            if (vcpu->dirty & RVVM_KVM_FPU_DIRTY) {
                ioctl(vcpu->fd, KVM_SET_FPU, vcpu->fpu);
            }
            free(vcpu->fpu);
            vcpu->fpu = NULL;
        }
        if (vcpu->sregs) {
            if (vcpu->dirty & RVVM_KVM_SRS_DIRTY) {
                ioctl(vcpu->fd, KVM_SET_SREGS, vcpu->sregs);
            }
            free(vcpu->sregs);
            vcpu->sregs = NULL;
        }
#if defined(RVVM_VMM_X86)
        if (vcpu->debug) {
            if (vcpu->dirty & RVVM_KVM_DBG_DIRTY) {
                ioctl(vcpu->fd, KVM_SET_DEBUGREGS, vcpu->debug);
            }
            free(vcpu->debug);
            vcpu->debug = NULL;
        }
        if (vcpu->msr) {
            if (vcpu->dirty & RVVM_KVM_MSR_DIRTY) {
                ioctl(vcpu->fd, KVM_SET_MSRS, vcpu->msr);
            }
            free(vcpu->msr);
            vcpu->msr = NULL;
        }
#endif
        vcpu->dirty = 0;
        // Check we're not interrupted
        if (!atomic_load_uint8_relax(&vcpu->run->immediate_exit)) {
            bool kvm_exit = !ioctl(vcpu->fd, KVM_RUN, 0);
            if (kvm_exit) {
                switch (vcpu->run->exit_reason) {
                    case KVM_EXIT_EXCEPTION:
                        return RVVM_VMM_EXIT_TRAP;
                    case KVM_EXIT_IO:
                        if (vcpu->run->io.direction) {
                            return RVVM_VMM_EXIT_PORT_W;
                        }
                        return RVVM_VMM_EXIT_PORT_R;
                    case KVM_EXIT_HYPERCALL:
                        return RVVM_VMM_EXIT_VCALL;
                    case KVM_EXIT_DEBUG:
                        return RVVM_VMM_EXIT_DEBUG;
                    case KVM_EXIT_HLT:
                        return RVVM_VMM_EXIT_HALT;
                    case KVM_EXIT_MMIO:
                        if (vcpu->run->mmio.is_write) {
                            return RVVM_VMM_EXIT_MMIO_W;
                        }
                        return RVVM_VMM_EXIT_MMIO_R;
                }
            }
        }
        atomic_store_uint8_relax(&vcpu->run->immediate_exit, 0);
        return RVVM_VMM_EXIT_KICKED;
    }
    return RVVM_VMM_EXIT_FATAL;
}

void rvvm_vmm_vcpu_register(rvvm_vmm_vcpu_t* vcpu)
{
    if (vcpu) {
        DO_ONCE_SCOPED {
            struct sigaction sa_old = {0};
            struct sigaction sa_new = {
                .sa_handler = kvm_sigcont_handler,
            };
            sigaction(SIGCONT, &sa_new, &sa_old);
        }
        atomic_store_uint8(&vcpu->run->immediate_exit, 1);
        atomic_store_uint32(&vcpu->registered, 0);
        vcpu->thread = pthread_self();
        atomic_store_uint32(&vcpu->registered, 1);
        atomic_store_uint8(&vcpu->run->immediate_exit, 0);
    }
}

void rvvm_vmm_vcpu_kick(rvvm_vmm_vcpu_t* vcpu)
{
    if (vcpu && atomic_load_uint32(&vcpu->registered)) {
        if (atomic_cas_uint8(&vcpu->run->immediate_exit, 0, 1)) {
            pthread_kill(vcpu->thread, SIGCONT);
        }
    }
}

#if defined(RVVM_VMM_X86)

static void kvm_prepare_gpr(rvvm_vmm_vcpu_t* vcpu, bool dirty)
{
    if (!vcpu->regs) {
        vcpu->regs = safe_new_obj(struct kvm_regs);
        ioctl(vcpu->fd, KVM_GET_REGS, vcpu->regs);
    }
    if (dirty) {
        vcpu->dirty |= RVVM_KVM_GPR_DIRTY;
    }
}

static void kvm_prepare_fpu(rvvm_vmm_vcpu_t* vcpu, bool dirty)
{
    if (!vcpu->fpu) {
        vcpu->fpu = safe_new_obj(struct kvm_fpu);
        ioctl(vcpu->fd, KVM_GET_FPU, vcpu->fpu);
    }
    if (dirty) {
        vcpu->dirty |= RVVM_KVM_FPU_DIRTY;
    }
}

static void kvm_prepare_sregs(rvvm_vmm_vcpu_t* vcpu, bool dirty)
{
    if (!vcpu->sregs) {
        vcpu->sregs = safe_new_obj(struct kvm_sregs);
        ioctl(vcpu->fd, KVM_GET_SREGS, vcpu->sregs);
    }
    if (dirty) {
        vcpu->dirty |= RVVM_KVM_SRS_DIRTY;
    }
}

static void kvm_prepare_debug(rvvm_vmm_vcpu_t* vcpu, bool dirty)
{
    if (!vcpu->debug) {
        vcpu->debug = safe_new_obj(struct kvm_guest_debug_arch);
        ioctl(vcpu->fd, KVM_GET_DEBUGREGS, vcpu->debug);
    }
    if (dirty) {
        vcpu->dirty |= RVVM_KVM_DBG_DIRTY;
    }
}

static void kvm_prepare_msr(rvvm_vmm_vcpu_t* vcpu, bool dirty)
{
    if (!vcpu->msr) {
        // Skip EFER as it's part of kvm_sregs
        size_t bmsr = RVVM_VMM_REG_X86_MSR_STAR;
        size_t nmsr = RVVM_VMM_REG_X86_MSR_SIZE - 1;
        // Allocate MSR list
        vcpu->msr = safe_calloc(sizeof(struct kvm_msrs) + (sizeof(struct kvm_msr_entry) * nmsr), 1);
        // Map MSRs
        vcpu->msr->nmsrs                                             = nmsr;
        vcpu->msr->entries[RVVM_VMM_REG_X86_MSR_STAR - bmsr].index   = 0xC0000081UL;
        vcpu->msr->entries[RVVM_VMM_REG_X86_MSR_LSTAR - bmsr].index  = 0xC0000082UL;
        vcpu->msr->entries[RVVM_VMM_REG_X86_MSR_CSTAR - bmsr].index  = 0xC0000083UL;
        vcpu->msr->entries[RVVM_VMM_REG_X86_MSR_SFMASK - bmsr].index = 0xC0000084UL;
        vcpu->msr->entries[RVVM_VMM_REG_X86_MSR_SWAPGS - bmsr].index = 0xC0000102UL;
        vcpu->msr->entries[RVVM_VMM_REG_X86_MSR_SYSCS - bmsr].index  = 0x00000174UL;
        vcpu->msr->entries[RVVM_VMM_REG_X86_MSR_SYSESP - bmsr].index = 0x00000175UL;
        vcpu->msr->entries[RVVM_VMM_REG_X86_MSR_SYSEIP - bmsr].index = 0x00000176UL;
        vcpu->msr->entries[RVVM_VMM_REG_X86_MSR_PAT - bmsr].index    = 0x00000277UL;
        vcpu->msr->entries[RVVM_VMM_REG_X86_MSR_TSC - bmsr].index    = 0x00000010UL;
        ioctl(vcpu->fd, KVM_GET_MSRS, vcpu->msr);
    }
    if (dirty) {
        vcpu->dirty |= RVVM_KVM_MSR_DIRTY;
    }
}

static void* kvm_access_x86_gpr(rvvm_vmm_vcpu_t* vcpu, size_t reg_id)
{
    switch (reg_id) {
        case RVVM_VMM_REG_X86_GPR_RAX:
            return &vcpu->regs->rax;
        case RVVM_VMM_REG_X86_GPR_RCX:
            return &vcpu->regs->rcx;
        case RVVM_VMM_REG_X86_GPR_RDX:
            return &vcpu->regs->rdx;
        case RVVM_VMM_REG_X86_GPR_RBX:
            return &vcpu->regs->rbx;
        case RVVM_VMM_REG_X86_GPR_RSP:
            return &vcpu->regs->rsp;
        case RVVM_VMM_REG_X86_GPR_RBP:
            return &vcpu->regs->rbp;
        case RVVM_VMM_REG_X86_GPR_RSI:
            return &vcpu->regs->rsi;
        case RVVM_VMM_REG_X86_GPR_RDI:
            return &vcpu->regs->rdi;
        case RVVM_VMM_REG_X86_GPR_R8:
            return &vcpu->regs->r8;
        case RVVM_VMM_REG_X86_GPR_R9:
            return &vcpu->regs->r9;
        case RVVM_VMM_REG_X86_GPR_R10:
            return &vcpu->regs->r10;
        case RVVM_VMM_REG_X86_GPR_R11:
            return &vcpu->regs->r11;
        case RVVM_VMM_REG_X86_GPR_R12:
            return &vcpu->regs->r12;
        case RVVM_VMM_REG_X86_GPR_R13:
            return &vcpu->regs->r13;
        case RVVM_VMM_REG_X86_GPR_R14:
            return &vcpu->regs->r14;
        case RVVM_VMM_REG_X86_GPR_R15:
            return &vcpu->regs->r15;
        case RVVM_VMM_REG_X86_GPR_RIP:
            return &vcpu->regs->rip;
        case RVVM_VMM_REG_X86_GPR_RFL:
            return &vcpu->regs->rflags;
    }
    return NULL;
}

static struct kvm_segment* kvm_access_x86_segment(rvvm_vmm_vcpu_t* vcpu, size_t reg_id)
{
    switch (reg_id & ~1) {
        case RVVM_VMM_REG_X86_SEG_ES:
            return &vcpu->sregs->es;
        case RVVM_VMM_REG_X86_SEG_CS:
            return &vcpu->sregs->cs;
        case RVVM_VMM_REG_X86_SEG_SS:
            return &vcpu->sregs->ss;
        case RVVM_VMM_REG_X86_SEG_DS:
            return &vcpu->sregs->ds;
        case RVVM_VMM_REG_X86_SEG_FS:
            return &vcpu->sregs->fs;
        case RVVM_VMM_REG_X86_SEG_GS:
            return &vcpu->sregs->gs;
        case RVVM_VMM_REG_X86_SEG_LDT:
            return &vcpu->sregs->ldt;
        case RVVM_VMM_REG_X86_SEG_TR:
            return &vcpu->sregs->tr;
    }
    return NULL;
}

static struct kvm_dtable* kvm_access_x86_dtable(rvvm_vmm_vcpu_t* vcpu, size_t reg_id)
{
    switch (reg_id & ~1) {
        case RVVM_VMM_REG_X86_SEG_GDT:
            return &vcpu->sregs->gdt;
        case RVVM_VMM_REG_X86_SEG_IDT:
            return &vcpu->sregs->idt;
    }
    return NULL;
}

static void* kvm_access_x86_cr(rvvm_vmm_vcpu_t* vcpu, size_t reg_id)
{
    switch (reg_id) {
        case RVVM_VMM_REG_X86_CR_CR0:
            return &vcpu->sregs->cr0;
        case RVVM_VMM_REG_X86_CR_CR2:
            return &vcpu->sregs->cr2;
        case RVVM_VMM_REG_X86_CR_CR3:
            return &vcpu->sregs->cr3;
        case RVVM_VMM_REG_X86_CR_CR4:
            return &vcpu->sregs->cr4;
        case RVVM_VMM_REG_X86_CR_CR8:
            return &vcpu->sregs->cr8;
    }
    return NULL;
}

static void* kvm_access_x86_dr(rvvm_vmm_vcpu_t* vcpu, size_t reg_id)
{
    if (reg_id - RVVM_VMM_REG_X86_DR_DR0 < 4) {
        return &vcpu->debug->debugreg[reg_id - RVVM_VMM_REG_X86_DR_DR0];
    } else if (reg_id - RVVM_VMM_REG_X86_DR_DR6 < 2) {
        return &vcpu->debug->debugreg[reg_id - RVVM_VMM_REG_X86_DR_DR6 + 6];
    }
    return NULL;
}

#endif

uint64_t rvvm_vmm_vcpu_get_reg(rvvm_vmm_vcpu_t* vcpu, uint32_t reg_id)
{
    if (vcpu) {
        UNUSED(reg_id);
#if defined(RVVM_VMM_X86)
        if (reg_id - RVVM_VMM_REG_X86_GPR_BASE < RVVM_VMM_REG_X86_GPR_SIZE) {
            // Reag general-purpose register
            kvm_prepare_gpr(vcpu, false);
            uint64_t* gpr = (uint64_t*)kvm_access_x86_gpr(vcpu, reg_id);
            if (gpr) {
                return *gpr;
            }
        } else if (reg_id - RVVM_VMM_REG_X86_FPU_BASE < RVVM_VMM_REG_X86_FPU_SIZE) {
            // Reag 8087 FPU register
            kvm_prepare_fpu(vcpu, false);
            if (reg_id == RVVM_VMM_REG_X86_FPU_FCSW) {
                return vcpu->fpu->fcw                             //
                     | (((uint64_t)vcpu->fpu->fsw) << 16)         //
                     | (((uint64_t)vcpu->fpu->last_opcode) << 32) //
                     | (((uint64_t)vcpu->fpu->ftwx) << 48);
            } else if (reg_id == RVVM_VMM_REG_X86_FPU_LSIP) {
                return vcpu->fpu->last_ip;
            } else if (reg_id == RVVM_VMM_REG_X86_FPU_LSDP) {
                return vcpu->fpu->last_dp;
            } else {
                size_t fp = (reg_id - RVVM_VMM_REG_X86_FPU_BASE) >> 1;
                return read_uint64_le(&vcpu->fpu->fpr[fp][(reg_id & 1) << 3]);
            }
        } else if (reg_id == RVVM_VMM_REG_X86_XMM_MXCSR) {
            // Reag MXCSR register
            kvm_prepare_fpu(vcpu, false);
            return vcpu->fpu->mxcsr;
        } else if (reg_id - RVVM_VMM_REG_X86_XMM_BASE < RVVM_VMM_REG_X86_XMM_SIZE) {
            // Reag XMM register
            kvm_prepare_fpu(vcpu, false);
            size_t xmm = (reg_id - RVVM_VMM_REG_X86_XMM_BASE) >> 1;
            return read_uint64_le(&vcpu->fpu->xmm[xmm][(reg_id & 1) << 3]);
        } else if (reg_id - RVVM_VMM_REG_X86_SEG_BASE < RVVM_VMM_REG_X86_SEG_SIZE) {
            // Reag segment or descriptor table
            kvm_prepare_sregs(vcpu, false);
            struct kvm_segment* seg = kvm_access_x86_segment(vcpu, reg_id);
            struct kvm_dtable*  tbl = kvm_access_x86_dtable(vcpu, reg_id);
            if (seg && !(reg_id & 1)) {
                return ((uint16_t)seg->limit)                          // Limit (low)
                     | (((uint64_t)(seg->base & 0x00FFFFFFUL)) << 16)  // Base (low)
                     | (((uint64_t)(seg->type & 0x0F)) << 40)          // Type
                     | (((uint64_t)(seg->s & 0x01)) << 44)             // System
                     | (((uint64_t)(seg->dpl & 0x03)) << 45)           // DPL
                     | (((uint64_t)(seg->present & 0x01)) << 47)       // Present
                     | (((uint64_t)((seg->limit >> 16) & 0x0F)) << 48) // Limit (high)
                     | (((uint64_t)(seg->avl & 0x01)) << 52)           // Available for software
                     | (((uint64_t)(seg->l & 0x01)) << 53)             // Long
                     | (((uint64_t)(seg->db & 0x01)) << 54)            // D/B (Size)
                     | (((uint64_t)(seg->g & 0x01)) << 55)             // Granularity
                     | (((uint64_t)(uint8_t)(seg->base >> 24)) << 56); // Base (high)
            } else if (seg && (reg_id & 1)) {
                return seg->selector              // Selector
                     | ((seg->base >> 32) << 16); // Base (high)
            } else if (tbl && !(reg_id & 1)) {
                return ((uint16_t)tbl->limit)                          // Limit (low)
                     | (((uint64_t)(tbl->base & 0x00FFFFFFUL)) << 16)  // Base (low)
                     | (((uint64_t)((tbl->limit >> 16) & 0x0F)) << 48) // Limit (high)
                     | (((uint64_t)(uint8_t)(tbl->base >> 24)) << 56); // Base (high)
            } else if (tbl && (reg_id & 1)) {
                return (tbl->base >> 32) << 16;
            }
        } else if (reg_id - RVVM_VMM_REG_X86_CR_BASE < RVVM_VMM_REG_X86_CR_SIZE) {
            kvm_prepare_sregs(vcpu, false);
            uint64_t* cr = (uint64_t*)kvm_access_x86_cr(vcpu, reg_id);
            if (cr) {
                return *cr;
            }
        } else if (reg_id - RVVM_VMM_REG_X86_DR_BASE < RVVM_VMM_REG_X86_DR_SIZE) {
            kvm_prepare_debug(vcpu, false);
            uint64_t* dr = (uint64_t*)kvm_access_x86_dr(vcpu, reg_id);
            if (dr) {
                return *dr;
            }
        } else if (reg_id == RVVM_VMM_REG_X86_MSR_EFER) {
            kvm_prepare_sregs(vcpu, false);
            return vcpu->sregs->efer;
        } else if (reg_id == RVVM_VMM_REG_X86_MSR_APICB) {
            kvm_prepare_sregs(vcpu, false);
            return vcpu->sregs->apic_base;
        } else if (reg_id - RVVM_VMM_REG_X86_MSR_BASE < RVVM_VMM_REG_X86_MSR_SIZE) {
            kvm_prepare_msr(vcpu, false);
            return vcpu->msr->entries[reg_id - RVVM_VMM_REG_X86_MSR_STAR].data;
        }
#endif
    }
    return 0;
}

void rvvm_vmm_vcpu_set_reg(rvvm_vmm_vcpu_t* vcpu, uint32_t reg_id, uint64_t val)
{
    if (vcpu) {
        UNUSED(reg_id && val);
#if defined(RVVM_VMM_X86)
        if (reg_id - RVVM_VMM_REG_X86_GPR_BASE < RVVM_VMM_REG_X86_GPR_SIZE) {
            // Write general-purpose register
            kvm_prepare_gpr(vcpu, true);
            uint64_t* gpr = (uint64_t*)kvm_access_x86_gpr(vcpu, reg_id);
            if (gpr) {
                *gpr = val;
            }
        } else if (reg_id - RVVM_VMM_REG_X86_FPU_BASE < RVVM_VMM_REG_X86_FPU_SIZE) {
            // Write 8087 FPU register
            kvm_prepare_fpu(vcpu, true);
            if (reg_id == RVVM_VMM_REG_X86_FPU_FCSW) {
                vcpu->fpu->fcw         = (uint16_t)(val);
                vcpu->fpu->fsw         = (uint16_t)(val >> 16);
                vcpu->fpu->last_opcode = (uint16_t)(val >> 32);
                vcpu->fpu->ftwx        = (uint8_t)(val >> 48);
            } else if (reg_id == RVVM_VMM_REG_X86_FPU_LSIP) {
                vcpu->fpu->last_ip = val;
            } else if (reg_id == RVVM_VMM_REG_X86_FPU_LSDP) {
                vcpu->fpu->last_dp = val;
            } else {
                size_t fp = (reg_id - RVVM_VMM_REG_X86_FPU_BASE) >> 1;
                write_uint64_le(&vcpu->fpu->fpr[fp][(reg_id & 1) << 3], val);
            }
        } else if (reg_id == RVVM_VMM_REG_X86_XMM_MXCSR) {
            // Write MXCSR register
            kvm_prepare_fpu(vcpu, true);
            vcpu->fpu->mxcsr = val;
        } else if (reg_id - RVVM_VMM_REG_X86_XMM_BASE < RVVM_VMM_REG_X86_XMM_SIZE) {
            // Write XMM register
            kvm_prepare_fpu(vcpu, true);
            size_t xmm = (reg_id - RVVM_VMM_REG_X86_XMM_BASE) >> 1;
            write_uint64_le(&vcpu->fpu->xmm[xmm][(reg_id & 1) << 3], val);
        } else if (reg_id - RVVM_VMM_REG_X86_SEG_BASE < RVVM_VMM_REG_X86_SEG_SIZE) {
            // Write segment or descriptor table
            kvm_prepare_sregs(vcpu, true);
            struct kvm_segment* seg = kvm_access_x86_segment(vcpu, reg_id);
            struct kvm_dtable*  tbl = kvm_access_x86_dtable(vcpu, reg_id);
            if (seg && !(reg_id & 1)) {
                seg->limit = ((uint16_t)val) | ((val >> 32) & 0x000F0000UL); // Limit
                seg->base  = ((seg->base >> 32) << 32)                       // Base
                          | ((val >> 16) & 0x00FFFFFFUL) | ((val >> 32) & 0xFF000000UL);
                seg->type    = (val >> 40) & 0x0F; // Type
                seg->s       = (val >> 44) & 0x01; // System
                seg->dpl     = (val >> 45) & 0x03; // DPL
                seg->present = (val >> 47) & 0x01; // Present
                seg->avl     = (val >> 52) & 0x01; // Available for software
                seg->l       = (val >> 53) & 0x01; // Long
                seg->db      = (val >> 54) & 0x01; // D/B (Size)
                seg->g       = (val >> 55) & 0x01; // Granularity
            } else if (seg && (reg_id & 1)) {
                seg->selector = (uint16_t)val;                               // Selector
                seg->base     = ((uint32_t)seg->base) | ((val >> 16) << 32); // Base (high)
            } else if (tbl && !(reg_id & 1)) {
                tbl->limit = ((uint16_t)val) | ((val >> 32) & 0x000F0000UL); // Limit
                tbl->base  = ((tbl->base >> 32) << 32)                       // Base
                          | ((val >> 16) & 0x00FFFFFFUL) | ((val >> 32) & 0xFF000000UL);
            } else if (tbl && (reg_id & 1)) {
                tbl->base = ((uint32_t)tbl->base) | ((val >> 16) << 32); // Base (high)
            }
        } else if (reg_id - RVVM_VMM_REG_X86_CR_BASE < RVVM_VMM_REG_X86_CR_SIZE) {
            kvm_prepare_sregs(vcpu, true);
            uint64_t* cr = (uint64_t*)kvm_access_x86_cr(vcpu, reg_id);
            if (cr) {
                *cr = val;
            }
        } else if (reg_id - RVVM_VMM_REG_X86_DR_BASE < RVVM_VMM_REG_X86_DR_SIZE) {
            kvm_prepare_debug(vcpu, true);
            uint64_t* dr = (uint64_t*)kvm_access_x86_dr(vcpu, reg_id);
            if (dr) {
                *dr = val;
            }
        } else if (reg_id == RVVM_VMM_REG_X86_MSR_EFER) {
            kvm_prepare_sregs(vcpu, true);
            vcpu->sregs->efer = val;
        } else if (reg_id == RVVM_VMM_REG_X86_MSR_APICB) {
            kvm_prepare_sregs(vcpu, true);
            vcpu->sregs->apic_base = val;
        } else if (reg_id - RVVM_VMM_REG_X86_MSR_BASE < RVVM_VMM_REG_X86_MSR_SIZE) {
            kvm_prepare_msr(vcpu, true);
            vcpu->msr->entries[reg_id - RVVM_VMM_REG_X86_MSR_STAR].data = val;
        }
#endif
    }
}

uint64_t rvvm_vmm_vcpu_op_addr(rvvm_vmm_vcpu_t* vcpu)
{
    if (vcpu) {
        if (vcpu->run->exit_reason == KVM_EXIT_MMIO) {
            return vcpu->run->mmio.phys_addr;
        } else if (vcpu->run->exit_reason == KVM_EXIT_IO) {
            return vcpu->run->io.port;
#if defined(RVVM_VMM_X86)
        } else if (vcpu->run->exit_reason == KVM_EXIT_X86_RDMSR || vcpu->run->exit_reason == KVM_EXIT_X86_WRMSR) {
            return vcpu->run->msr.index;
#endif
        }
    }
    return 0;
}

uint32_t rvvm_vmm_vcpu_op_size(rvvm_vmm_vcpu_t* vcpu)
{
    if (vcpu) {
        if (vcpu->run->exit_reason == KVM_EXIT_MMIO) {
            return vcpu->run->mmio.len;
        } else if (vcpu->run->exit_reason == KVM_EXIT_IO) {
            return vcpu->run->io.size;
#if defined(RVVM_VMM_X86)
        } else if (vcpu->run->exit_reason == KVM_EXIT_X86_RDMSR || vcpu->run->exit_reason == KVM_EXIT_X86_WRMSR) {
            return 8;
#endif
        }
    }
    return 0;
}

void* rvvm_vmm_vcpu_op_data(rvvm_vmm_vcpu_t* vcpu)
{
    if (vcpu) {
        if (vcpu->run->exit_reason == KVM_EXIT_MMIO) {
            return vcpu->run->mmio.data;
        } else if (vcpu->run->exit_reason == KVM_EXIT_IO) {
            return ((uint8_t*)vcpu->run) + vcpu->run->io.data_offset;
#if defined(RVVM_VMM_X86)
        } else if (vcpu->run->exit_reason == KVM_EXIT_X86_RDMSR || vcpu->run->exit_reason == KVM_EXIT_X86_WRMSR) {
            return &vcpu->run->msr.data;
#endif
        }
    }
    return NULL;
}

bool rvvm_vmm_vcpu_interrupt(rvvm_vmm_vcpu_t* vcpu, uint32_t vec, bool set)
{
    if (vcpu) {
        // TODO: Improve handling of level-triggered CPU interrupts
        struct kvm_interrupt kvm_irq = {.irq = vec};
        return !set || !ioctl(vcpu->fd, KVM_INTERRUPT, &kvm_irq);
    }
    return false;
}

bool rvvm_vmm_vcpu_debug_mode(rvvm_vmm_vcpu_t* vcpu, bool enable)
{
    if (vcpu) {
        struct kvm_guest_debug kvm_dbg = {.control = enable ? 0x00030001UL : 0};
        return !ioctl(vcpu->fd, KVM_SET_GUEST_DEBUG, &kvm_dbg);
    }
    return false;
}

#endif
