/*
riscv-aplic.c - RISC-V Advanced Platform-Level Interrupt Controller
Copyright (C) 2024  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "riscv-aplic.h"
#include "riscv_hart.h"
#include "bit_ops.h"
#include "mem_ops.h"
#include "atomics.h"

#define APLIC_REGION_SIZE 0x4000

// APLIC registers & register groups
#define APLIC_REG_DOMAINCFG      0x0000 // Domain configuration
#define APLIC_REG_SOURCECFG_1    0x0004 // Source configurations (1 - 1023)
#define APLIC_REG_SOURCECFG_1023 0x0FFC // Source configurations (1 - 1023)
#define APLIC_REG_MMSIADDRCFG    0x1BC0 // Machine MSI address configuration
#define APLIC_REG_MMSIADDRCFGH   0x1BC4 // Machine MSI address configuration (high)
#define APLIC_REG_SMSIADDRCFG    0x1BC8 // Supervisor MSI address configuration
#define APLIC_REG_SMSIADDRCFGH   0x1BCC // Supervisor MSI address configuration (high)
#define APLIC_REG_SETIP_0        0x1C00 // Set interrupt-pending bits (0 - 31)
#define APLIC_REG_SETIP_31       0x1C7C // Set interrupt-pending bits (0 - 31)
#define APLIC_REG_SETIPNUM       0x1CDC // Set interrupt-pending bit by number
#define APLIC_REG_IN_CLRIP_0     0x1D00 // Rectified inputs, clear interrupt-pending bits (0 - 31)
#define APLIC_REG_IN_CLRIP_31    0x1D7C // Rectified inputs, clear interrupt-pending bits (0 - 31)
#define APLIC_REG_CLRIPNUM       0x1DDC // Clear interrupt-pending bit by number
#define APLIC_REG_SETIE_0        0x1E00 // Set interrupt-enabled bits (0 - 31)
#define APLIC_REG_SETIE_31       0x1E7C // Set interrupt-enabled bits (0 - 31)
#define APLIC_REG_SETIENUM       0x1EDC // Set interrupt-enabled bit by number
#define APLIC_REG_CLRIE_0        0x1F00 // Clear interrupt-enabled bits (0 - 31)
#define APLIC_REG_CLRIE_31       0x1F7C // Clear interrupt-enabled bits (0 - 31)
#define APLIC_REG_CLRIENUM       0x1FDC // Clear interrupt-enabled bit by number
#define APLIC_REG_SETIPNUM_LE    0x2000 // Set interrupt-pending bit by number (Little-endian)
#define APLIC_REG_SETIPNUM_BE    0x2004 // Set interrupt-pending bit by number (Big-endian)
#define APLIC_REG_GENMSI         0x3000 // Generate MSI
#define APLIC_REG_TARGET_1       0x3004 // Interrupt targets (1 - 1023)
#define APLIC_REG_TARGET_1023    0x3FFC // Interrupt targets (1 - 1023)

// APLIC register values
#define APLIC_DOMAINCFG_BE 0x1        // Big-endian mode
#define APLIC_DOMAINCFG_DM 0x4        // MSI delivery mode
#define APLIC_DOMAINCFG_IE 0x100      // Interrupts enabled
#define APLIC_DOMAINCFG    0x80000004 // Default hardwired domaincfg

#define APLIC_SOURCECFG_DELEGATE  0x400 // Delegate to child domain
#define APLIC_SOURCECFG_INACTIVE  0x0   // Inactive source
#define APLIC_SOURCECFG_DETACHED  0x1   // Detached source
#define APLIC_SOURCECFG_EDGE_RISE 0x4   // Active, edge-triggered on rise
#define APLIC_SOURCECFG_EDGE_FALL 0x5   // Active, edge-triggered on fall
#define APLIC_SOURCECFG_LVL_HIGH  0x6   // Active, level-triggered when high
#define APLIC_SOURCECFG_LVL_LOW   0x7   // Active, level-triggered when low
#define APLIC_SOURCECFG_MASK      0x7   // Valid source config mask

#define APLIC_MSIADDRCFGH_L 0x80000000  // Locked

// Limit on APLIC interrupt identities, maximum 1024
#define APLIC_SRC_LIMIT 64

// Number of APLIC source bitset registers
#define APLIC_SRC_REGS (APLIC_SRC_LIMIT >> 5)

typedef struct {
    rvvm_machine_t* machine;
    rvvm_intc_t intc;
    uint32_t phandle;
    uint32_t domaincfg[2];

    // Bitset of interrupts delegated to S-mode
    uint32_t deleg[APLIC_SRC_REGS];

    // Bitset of raised external interrupts
    uint32_t raised[APLIC_SRC_REGS];

    // Bitset of inverted external interrupts
    uint32_t invert[APLIC_SRC_REGS];

    // Bitset of pending interrupts
    uint32_t pending[APLIC_SRC_REGS];

    // Bitset of enabled interrupts
    uint32_t enabled[APLIC_SRC_REGS];

    // Interrupt source configuration
    uint32_t source[APLIC_SRC_LIMIT];

    // Interrupt target configuration
    uint32_t target[APLIC_SRC_LIMIT];
} aplic_ctx_t;

typedef struct {
    aplic_ctx_t* aplic;

    // This is used to invert delegation
    uint32_t deleg_invert;

    bool root_domain;
} aplic_domain_t;

/*
 * APLIC core handling
 */

static void aplic_gen_msi(aplic_ctx_t* aplic, bool smode, uint32_t target)
{
    size_t hartid = target >> 18;
    if (hartid < vector_size(aplic->machine->harts)) {
        rvvm_hart_t* hart = vector_at(aplic->machine->harts, hartid);
        riscv_send_aia_irq(hart, smode, bit_cut(target, 0, 10));
    }
}

static uint32_t aplic_rectified_bits(aplic_ctx_t* aplic, size_t reg)
{
    if (likely(reg < APLIC_SRC_REGS)) {
        uint32_t raised = atomic_load_uint32_relax(&aplic->raised[reg]);
        uint32_t invert = atomic_load_uint32_relax(&aplic->invert[reg]);
        return (raised ^ invert);
    }
    return 0;
}

static bool aplic_rectified_src(aplic_ctx_t* aplic, size_t src)
{
    uint32_t mask = (1U << (src & 0x1F));
    return !!(aplic_rectified_bits(aplic, src >> 5) & mask);
}

static bool aplic_detached_src(aplic_ctx_t* aplic, size_t src)
{
    if (likely(src < APLIC_SRC_LIMIT)) {
        return atomic_load_uint32_relax(&aplic->source[src]) <= APLIC_SOURCECFG_DETACHED;
    }
    return true;
}

// Send interrupt through APLIC to MSI, or raise a pending bit
static void aplic_notify_interrupt(aplic_ctx_t* aplic, size_t src)
{
    if (likely(src < APLIC_SRC_LIMIT)) {
        size_t reg = src >> 5;
        uint32_t mask = (1U << (src & 0x1F));
        bool smode = !!(atomic_load_uint32_relax(&aplic->deleg[reg]) & mask);
        uint32_t enabled = atomic_load_uint32_relax(&aplic->enabled[reg]);
        uint32_t domaincfg = atomic_load_uint32_relax(&aplic->domaincfg[smode]);
        if (likely((enabled & mask) && (domaincfg & APLIC_DOMAINCFG_IE))) {
            uint32_t target = atomic_load_uint32_relax(&aplic->target[src]);
            aplic_gen_msi(aplic, smode, target);
        } else {
            atomic_or_uint32(&aplic->pending[reg], mask);
        }
    }
}

// Send interrupt edge through APLIC if it's not detached
static void aplic_edge_interrupt(aplic_ctx_t* aplic, size_t src)
{
    if (!aplic_detached_src(aplic, src)) {
        aplic_notify_interrupt(aplic, src);
    }
}

// React to interrupt pin state update
static void aplic_update_interrupt(aplic_ctx_t* aplic, size_t src)
{
    if (aplic_rectified_src(aplic, src)) {
        aplic_edge_interrupt(aplic, src);
    }
}

/*
 * APLIC domain handling
 */

static uint32_t aplic_valid_bits(aplic_domain_t* domain, size_t reg)
{
    if (likely(reg < APLIC_SRC_REGS)) {
        aplic_ctx_t* aplic = domain->aplic;
        return atomic_load_uint32_relax(&aplic->deleg[reg]) ^ domain->deleg_invert;
    }
    return 0;
}

static bool aplic_valid_src(aplic_domain_t* domain, size_t src)
{
    uint32_t mask = (1U << (src & 0x1F));
    return !!(aplic_valid_bits(domain, src >> 5) & mask);
}

static bool aplic_ungated_src(aplic_domain_t* domain, size_t src)
{
    if (likely(aplic_valid_src(domain, src))) {
        return aplic_rectified_src(domain->aplic, src) || aplic_detached_src(domain->aplic, src);
    }
    return false;
}

static uint32_t aplic_read_ip(aplic_domain_t* domain, size_t reg)
{
    if (likely(reg < APLIC_SRC_REGS)) {
        aplic_ctx_t* aplic = domain->aplic;
        return atomic_load_uint32_relax(&aplic->pending[reg]) & aplic_valid_bits(domain, reg);
    }
    return 0;
}

static uint32_t aplic_read_in(aplic_domain_t* domain, size_t reg)
{
    return aplic_rectified_bits(domain->aplic, reg) & aplic_valid_bits(domain, reg);
}

static uint32_t aplic_read_ie(aplic_domain_t* domain, size_t reg)
{
    if (likely(reg < APLIC_SRC_REGS)) {
        aplic_ctx_t* aplic = domain->aplic;
        return atomic_load_uint32_relax(&aplic->enabled[reg]) & aplic_valid_bits(domain, reg);
    }
    return 0;
}

static void aplic_setipnum(aplic_domain_t* domain, size_t src)
{
    if (aplic_ungated_src(domain, src)) {
        aplic_notify_interrupt(domain->aplic, src);
    }
}

static void aplic_setip(aplic_domain_t* domain, size_t reg, uint32_t bits)
{
    if (likely(reg < APLIC_SRC_REGS)) {
        uint32_t set = bits & aplic_valid_bits(domain, reg);
        if (set) {
            // Re-notify rectified interrupts
            for (size_t i = 0; i < 31; ++i) {
                if (set & (1U << i)) {
                    aplic_setipnum(domain, (reg << 5) + i);
                }
            }
        }
    }
}

static void aplic_clrip(aplic_domain_t* domain, size_t reg, uint32_t bits)
{
    if (likely(reg < APLIC_SRC_REGS)) {
        uint32_t clr = bits & aplic_valid_bits(domain, reg);
        if (clr) {
            aplic_ctx_t* aplic = domain->aplic;
            atomic_and_uint32(&aplic->pending[reg], ~clr);
        }
    }
}

static void aplic_clripnum(aplic_domain_t* domain, size_t src)
{
    size_t reg = src >> 5;
    uint32_t mask = (1U << (src & 0x1F));
    aplic_clrip(domain, reg, mask);
}

static void aplic_setie(aplic_domain_t* domain, size_t reg, uint32_t bits)
{
    if (likely(reg < APLIC_SRC_REGS)) {
        aplic_ctx_t* aplic = domain->aplic;
        uint32_t set = bits & aplic_valid_bits(domain, reg);
        set &= ~atomic_or_uint32(&aplic->enabled[reg], set);
        if (set) {
            uint32_t deliver = atomic_and_uint32(&aplic->pending[reg], ~set);
            if (deliver) {
                // Notify about re-enabled pending interruts
                for (size_t i = 0; i < 31; ++i) {
                    if (deliver & (1U << i)) {
                        aplic_notify_interrupt(aplic, (reg << 5) + i);
                    }
                }
            }
        }
    }
}

static void aplic_setienum(aplic_domain_t* domain, size_t src)
{
    size_t reg = src >> 5;
    uint32_t mask = (1U << (src & 0x1F));
    aplic_setie(domain, reg, mask);
}

static void aplic_clrie(aplic_domain_t* domain, size_t reg, uint32_t bits)
{
    if (likely(reg < APLIC_SRC_REGS)) {
        uint32_t clr = bits & aplic_valid_bits(domain, reg);
        if (clr) {
            aplic_ctx_t* aplic = domain->aplic;
            atomic_and_uint32(&aplic->enabled[reg], ~clr);
        }
    }
}

static void aplic_clrienum(aplic_domain_t* domain, size_t src)
{
    size_t reg = src >> 5;
    uint32_t mask = (1U << (src & 0x1F));
    aplic_clrie(domain, reg, mask);
}

static bool aplic_mmio_read(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    aplic_domain_t* domain = dev->data;
    aplic_ctx_t* aplic = domain->aplic;
    uint32_t val = 0;
    UNUSED(size);

    switch (offset) {
        case APLIC_REG_DOMAINCFG:
            // Hardwired little-endian & MSI delivery mode
            val = atomic_load_uint32_relax(&aplic->domaincfg[!domain->root_domain]) | APLIC_DOMAINCFG;
            break;
        case APLIC_REG_MMSIADDRCFGH:
        case APLIC_REG_SMSIADDRCFGH:
            // Hardwired MSI addresses
            val = APLIC_MSIADDRCFGH_L;
            break;
        default:
            if (offset >= APLIC_REG_SETIP_0 && offset <= APLIC_REG_SETIP_31) {
                size_t reg = ((offset - APLIC_REG_SETIP_0) >> 2);
                val = aplic_read_ip(domain, reg);
            } else if (offset >= APLIC_REG_IN_CLRIP_0 && offset <= APLIC_REG_IN_CLRIP_31) {
                size_t reg = ((offset - APLIC_REG_IN_CLRIP_0) >> 2);
                val = aplic_read_in(domain, reg);
            } else if (offset >= APLIC_REG_SETIE_0 && offset <= APLIC_REG_SETIE_31) {
                size_t reg = ((offset - APLIC_REG_SETIE_0) >> 2);
                val = aplic_read_ie(domain, reg);
            } else if (offset >= APLIC_REG_SOURCECFG_1 && offset <= APLIC_REG_SOURCECFG_1023) {
                size_t reg = ((offset - APLIC_REG_SOURCECFG_1) >> 2) + 1;
                if (aplic_valid_src(domain, reg)) {
                    // Source configuration from our domain
                    val = atomic_load_uint32_relax(&aplic->source[reg]);
                } else if (domain->root_domain) {
                    // Source configuration delegated and we-re an M-mode domain
                    val = APLIC_SOURCECFG_DELEGATE;
                }
            } else if (offset >= APLIC_REG_TARGET_1 && offset <= APLIC_REG_TARGET_1023) {
                size_t reg = ((offset - APLIC_REG_TARGET_1) >> 2) + 1;
                if (aplic_valid_src(domain, reg)) {
                    // Target configuration from our domain
                    val = atomic_load_uint32_relax(&aplic->target[reg]);
                }
            }
            break;
    }

    write_uint32_le(data, val);
    return true;
}

static bool aplic_mmio_write(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    aplic_domain_t* domain = dev->data;
    aplic_ctx_t* aplic = domain->aplic;
    uint32_t val = read_uint32_le(data);
    UNUSED(size);

    switch (offset) {
        case APLIC_REG_DOMAINCFG:
            atomic_store_uint32_relax(&aplic->domaincfg[!domain->root_domain], val & APLIC_DOMAINCFG_IE);
            break;
        case APLIC_REG_SETIPNUM:
        case APLIC_REG_SETIPNUM_LE:
            aplic_setipnum(domain, val);
            break;
        case APLIC_REG_CLRIPNUM:
            aplic_clripnum(domain, val);
            break;
        case APLIC_REG_SETIENUM:
            aplic_setienum(domain, val);
            break;
        case APLIC_REG_CLRIENUM:
            aplic_clrienum(domain, val);
            break;
        case APLIC_REG_SETIPNUM_BE:
            aplic_setipnum(domain, read_uint32_be_m(data));
            break;
        case APLIC_REG_GENMSI:
            aplic_gen_msi(aplic, !domain->root_domain, val);
            break;
        default:
            if (offset >= APLIC_REG_SETIP_0 && offset <= APLIC_REG_SETIP_31) {
                size_t reg = ((offset - APLIC_REG_SETIP_0) >> 2);
                aplic_setip(domain, reg, val);
            } else if (offset >= APLIC_REG_IN_CLRIP_0 && offset <= APLIC_REG_IN_CLRIP_31) {
                size_t reg = ((offset - APLIC_REG_IN_CLRIP_0) >> 2);
                aplic_clrip(domain, reg, val);
            } else if (offset >= APLIC_REG_SETIE_0 && offset <= APLIC_REG_SETIE_31) {
                size_t reg = ((offset - APLIC_REG_SETIE_0) >> 2);
                aplic_setie(domain, reg, val);
            } else if (offset >= APLIC_REG_CLRIE_0 && offset <= APLIC_REG_CLRIE_31) {
                size_t reg = ((offset - APLIC_REG_CLRIE_0) >> 2);
                aplic_clrie(domain, reg, val);
            } else if (offset >= APLIC_REG_SOURCECFG_1 && offset <= APLIC_REG_SOURCECFG_1023) {
                size_t reg = ((offset - APLIC_REG_SOURCECFG_1) >> 2) + 1;
                if (domain->root_domain) {
                    // M-mode source configuration write
                    uint32_t mask = (1U << (reg & 0x1F));
                    if (val & APLIC_SOURCECFG_DELEGATE) {
                        // Enable delegation
                        atomic_or_uint32(&aplic->deleg[reg >> 5], mask);
                    } else {
                        // Disable delegation
                        atomic_and_uint32(&aplic->deleg[reg >> 5], ~mask);
                    }
                }
                if (aplic_valid_src(domain, reg)) {
                    // Source configuration for our domain
                    uint32_t cfg = val & APLIC_SOURCECFG_MASK;
                    uint32_t mask = (1U << (reg & 0x1F));
                    atomic_store_uint32_relax(&aplic->source[reg], cfg);
                    if (cfg == APLIC_SOURCECFG_LVL_LOW || cfg == APLIC_SOURCECFG_EDGE_FALL) {
                        // Enable input inversion
                        atomic_or_uint32(&aplic->invert[reg >> 5], mask);
                    } else {
                        // Disable input inversion
                        atomic_and_uint32(&aplic->invert[reg >> 5], ~mask);
                    }
                }
            } else if (offset >= APLIC_REG_TARGET_1 && offset <= APLIC_REG_TARGET_1023) {
                size_t reg = ((offset - APLIC_REG_TARGET_1) >> 2) + 1;
                if (aplic_valid_src(domain, reg)) {
                    // Target configuration for our domain
                    atomic_store_uint32_relax(&aplic->target[reg], val);
                }
            }
    }

    return true;
}

static void aplic_remove(rvvm_mmio_dev_t* dev)
{
    aplic_domain_t* domain = dev->data;
    if (domain->root_domain) {
        free(domain->aplic);
    }
    free(domain);
}

static rvvm_mmio_type_t aplic_dev_type = {
    .name = "riscv_aplic",
    .remove = aplic_remove,
};

/*
 * RVVM Interrupt API glue
 */

static bool aplic_send_irq(rvvm_intc_t* intc, rvvm_irq_t irq)
{
    if (irq > 0 && irq < APLIC_SRC_LIMIT) {
        aplic_edge_interrupt(intc->data, irq);
    }
    return false;
}

static bool aplic_raise_irq(rvvm_intc_t* intc, rvvm_irq_t irq)
{
    aplic_ctx_t* aplic = intc->data;
    if (irq > 0 && irq < APLIC_SRC_LIMIT) {
        uint32_t mask = (1U << (irq & 0x1F));
        if ((~atomic_or_uint32(&aplic->raised[irq >> 5], mask)) & mask) {
            aplic_update_interrupt(aplic, irq);
        }
        return true;
    }
    return false;
}

static bool aplic_lower_irq(rvvm_intc_t* intc, rvvm_irq_t irq)
{
    aplic_ctx_t* aplic = intc->data;
    if (irq > 0 && irq < APLIC_SRC_LIMIT) {
        uint32_t mask = (1U << (irq & 0x1F));
        if (atomic_and_uint32(&aplic->raised[irq >> 5], ~mask) & ~mask) {
            aplic_update_interrupt(aplic, irq);
        }
        return true;
    }
    return false;
}

static uint32_t aplic_fdt_phandle(rvvm_intc_t* intc)
{
    aplic_ctx_t* aplic = intc->data;
    return aplic->phandle;
}

static size_t aplic_fdt_irq_cells(rvvm_intc_t* intc, rvvm_irq_t irq, uint32_t* cells, size_t size)
{
    UNUSED(intc);
    if (cells && size >= 2) {
        cells[0] = irq;
        cells[1] = 0x4; // Level-triggered
        return 2;
    }
    return 0;
}

// Returns phandle
static uint32_t aplic_attach_domain(rvvm_machine_t* machine, rvvm_addr_t addr, aplic_domain_t* domain)
{
    bool smode = !domain->deleg_invert;

    rvvm_mmio_dev_t aplic_mmio = {
        .addr = addr,
        .size = APLIC_REGION_SIZE,
        .data = domain,
        .min_op_size = 4,
        .max_op_size = 4,
        .read = aplic_mmio_read,
        .write = aplic_mmio_write,
        .type = &aplic_dev_type,
    };

    if (!rvvm_attach_msi_target(machine, &aplic_mmio)) {
        return 0;
    }

#ifdef USE_FDT
    struct fdt_node* imsic_fdt = fdt_node_find_reg_any(rvvm_get_fdt_soc(machine), smode ? "imsics_s" : "imsics_m");
    struct fdt_node* aplic_s = fdt_node_find_reg_any(rvvm_get_fdt_soc(machine), "aplic_s");
    if (!imsic_fdt) {
        rvvm_warn("Missing /soc/imsic node in FDT!");
    }
    if (!smode && !aplic_s) {
        rvvm_warn("Missing /soc/aplic_s node in FDT!");
    }

    struct fdt_node* aplic_fdt = fdt_node_create_reg(smode ? "aplic_s" : "aplic_m", aplic_mmio.addr);
    fdt_node_add_prop_reg(aplic_fdt, "reg", aplic_mmio.addr, aplic_mmio.size);
    fdt_node_add_prop_str(aplic_fdt, "compatible", "riscv,aplic");
    fdt_node_add_prop_u32(aplic_fdt, "msi-parent", fdt_node_get_phandle(imsic_fdt));
    fdt_node_add_prop(aplic_fdt, "interrupt-controller", NULL, 0);
    fdt_node_add_prop_u32(aplic_fdt, "#interrupt-cells", 2);
    fdt_node_add_prop_u32(aplic_fdt, "#address-cells", 0);
    fdt_node_add_prop_u32(aplic_fdt, "riscv,num-sources", APLIC_SRC_LIMIT - 1);

    if (!smode) {
        uint32_t children = fdt_node_get_phandle(aplic_s);
        uint32_t delegate[] = { children, 1, APLIC_SRC_LIMIT - 1, };
        fdt_node_add_prop_u32(aplic_fdt, "riscv,children", children);
        fdt_node_add_prop_cells(aplic_fdt, "riscv,delegate", delegate, STATIC_ARRAY_SIZE(delegate));
        fdt_node_add_prop_cells(aplic_fdt, "riscv,delegation", delegate, STATIC_ARRAY_SIZE(delegate));
    }

    fdt_node_add_child(rvvm_get_fdt_soc(machine), aplic_fdt);

    return fdt_node_get_phandle(aplic_fdt);
#else
    return 1;
#endif
}

PUBLIC rvvm_intc_t* riscv_aplic_init(rvvm_machine_t* machine, rvvm_addr_t m_addr, rvvm_addr_t s_addr)
{
    aplic_ctx_t* aplic = safe_new_obj(aplic_ctx_t);
    aplic_domain_t* m_domain = safe_new_obj(aplic_domain_t);
    aplic_domain_t* s_domain = safe_new_obj(aplic_domain_t);

    aplic->machine = machine;

    for (size_t i = 0; i < APLIC_SRC_REGS; ++i) {
        aplic->deleg[i] = -1;
    }

    m_domain->deleg_invert = -1;
    m_domain->root_domain = true;
    m_domain->aplic = aplic;
    s_domain->aplic = aplic;

    rvvm_intc_t aplic_intc = {
        .data = aplic,
        .send_irq = aplic_send_irq,
        .raise_irq = aplic_raise_irq,
        .lower_irq = aplic_lower_irq,
        .fdt_phandle = aplic_fdt_phandle,
        .fdt_irq_cells = aplic_fdt_irq_cells,
    };

    aplic->intc = aplic_intc;
    aplic->phandle = aplic_attach_domain(machine, s_addr, s_domain);
    if (!aplic->phandle || !aplic_attach_domain(machine, m_addr, m_domain)) {
        rvvm_error("Failed to attach RISC-V APLIC!");
        return NULL;
    }

    rvvm_set_intc(machine, &aplic->intc);
    return &aplic->intc;
}

PUBLIC rvvm_intc_t* riscv_aplic_init_auto(rvvm_machine_t* machine)
{
    rvvm_addr_t m_addr = rvvm_mmio_zone_auto(machine, APLIC_M_ADDR_DEFAULT, APLIC_REGION_SIZE);
    rvvm_addr_t s_addr = rvvm_mmio_zone_auto(machine, APLIC_S_ADDR_DEFAULT, APLIC_REGION_SIZE);
    return riscv_aplic_init(machine, m_addr, s_addr);
}
