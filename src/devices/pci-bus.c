/*
pci-bus.c - Peripheral Component Interconnect Bus
Copyright (C) 2021  LekKit <github.com/LekKit>
                    cerg2010cerg2010 <github.com/cerg2010cerg2010>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "pci-bus.h"
#include "bit_ops.h"
#include "fdtlib.h"
#include "mem_ops.h"
#include "spinlock.h"
#include "utils.h"
#include "vector.h"

/*
 * PCI Configuration Space Registers
 */

#define PCI_REG_DEV_VEN_ID           0x00 // Device, Vendor ID
#define PCI_REG_STATUS_CMD           0x04 // Status, Command
#define PCI_REG_CLASS_REV            0x08 // Class, Subclass, Prog IF, revision
#define PCI_REG_INFO                 0x0C // BIST, Header type, Latency, Cache line size
#define PCI_REG_BAR0                 0x10 // BAR0 Base Address
#define PCI_REG_BAR1                 0x14 // BAR1 Base Address (Or upper 32 bits of BAR0)
#define PCI_REG_BAR2                 0x18 // BAR2 Base Address (Or upper 32 bits of BAR1)
#define PCI_REG_BAR3                 0x1C // BAR3 Base Address (Or upper 32 bits of BAR2)
#define PCI_REG_BAR4                 0x20 // BAR4 Base Address (Or upper 32 bits of BAR3)
#define PCI_REG_BAR5                 0x24 // BAR5 Base Address (Or upper 32 bits of BAR4)
#define PCI_REG_SSID_SVID            0x2C // Subsystem ID, Subsystem Vendor ID
#define PCI_REG_EXPANSION_ROM        0x30 // Expansion ROM Base Address
#define PCI_REG_CAP_PTR              0x34 // Capabilities List Pointer
#define PCI_REG_IRQ_PIN_LINE         0x3C // Interrupt PIN, Interrupt Line

// Command bits
#define PCI_CMD_IO_SPACE             0x0001 // Accessible through IO ports
#define PCI_CMD_MEM_SPACE            0x0002 // Accessible through MMIO
#define PCI_CMD_BUS_MASTER           0x0004 // May use DMA
#define PCI_CMD_INTX_DISABLE         0x0400 // INTx Interrupt Disabled

#define PCI_CMD_DEFAULT              (PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER)
#define PCI_CMD_MASK                 (PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER | PCI_CMD_INTX_DISABLE)

// Status bits
#define PCI_STATUS_INTX              0x08 // INTx Interrupt Status
#define PCI_STATUS_CAP               0x10 // Capabilities List Present

// Header type bits
#define PCI_HEADER_PCI_PCI           0x01 // PCI-PCI Bridge
#define PCI_HEADER_MULTIFUNC         0x80 // Multi-function device

// BAR bits
#define PCI_BAR_IO_SPACE             0x01
#define PCI_BAR_64_BIT               0x04
#define PCI_BAR_PREFETCH             0x08

// Expansion ROM bits
#define PCI_EXPANSION_ROM_ENABLED    0x01

// PCI capabilities offset
#define PCI_CAP_LIST_OFF             0x40 // Capabilities List Offset

/*
 * PCI Capability Registers
 */

#define PCI_REG_ENDPOINT             0x40 // Endpoint capability

#define PCI_REG_PMCSR                0x84 // Power Management Control/Status

#define PCI_REG_MSI                  0x90 // MSI capability
#define PCI_REG_MSI_AL               0x94 // MSI address low
#define PCI_REG_MSI_AH               0x98 // MSI address high
#define PCI_REG_MSI_DATA             0x9C // MSI data
#define PCI_REG_MSI_MASK             0xA0 // MSI mask vector
#define PCI_REG_MSI_PEND             0xA4 // MSI pending vector

#define PCI_REG_MSIX                 0xB0 // MSI-X capability
#define PCI_REG_MSIX_TBL             0xB4 // MSI-X table offset/BAR
#define PCI_REG_MSIX_PBO             0xB8 // MSI-X pending bits offset/BAR

// PCI Express capability port type
#define PCIE_CAP_PORT_ENDPOINT       0x00
#define PCIE_CAP_ROOT_PORT           0x04
#define PCIE_CAP_UPSTREAM_SWITCH     0x05
#define PCIE_CAP_DOWNSTREAM_SWITCH   0x06
#define PCIE_CAP_INTEGRATED_ENDPOINT 0x09

// Power management
#define PCI_PMCSR_PS                 0x03

// MSI-X interrupts
#define PCI_MSIX_MAX_IRQS            32
#define PCI_MSIX_TBL_SIZE            (((PCI_MSIX_MAX_IRQS + 1) >> 1) << 3)
#define PCI_MSIX_PBA_SIZE            ((PCI_MSIX_MAX_IRQS + 0x1F) >> 5)
#define PCI_MSIX_BAR_SIZE            (PCI_MSIX_TBL_SIZE + PCI_MSIX_PBA_SIZE)

#define PCI_MSIX_ENABLED             0x80000000
#define PCI_MSIX_MASKED              0x40000000
#define PCI_MSIX_VALID               0xC0000000

// MSI interrupts
#define PCI_MSI_ENABLED              0x00010000 // MSI Enabled
#define PCI_MSI_DATA                 0x0000FFFF // MSI Data (16-bit)
#define PCI_MSI_BIT                  0x00000001 // MSI Pending/Masked bit

static const uint32_t pci_express_caps_ro[] = {
    // [40] PCI Express (v2) Endpoint, IntMsgNum 0
    0x01028010,
    0x00008002, // DevCap: MaxPayload 512 bytes, RBE+
    0x00002050, // DevCtl: RlxdOrd+
    0x01800D02, // LnkCap: Speed 5GT/s, Width x16
    0x01020003, // LnkSta: Speed 5GT/s, Width x16
    0x00020060,
    0x00200028,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000002,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,

    // [80] Power Management version 3
    0x00039001,
    0x00000008, // NoSoftRst+
    0x00000000,
    0x00000000,

    // [90] MSI: Enable- Count=1/1 Maskable+ 64bit+
    0x0180B005,
    0x00000000, // Message Address Low
    0x00000000, // Message Address High
    0x00000000, // Message Data
    0x00000000, // Mask
    0x00000000, // Pending
    0x00000000,
    0x00000000,

    // [B0] MSI-X: Enable- Count=X Masked-
    0x00000011 | ((PCI_MSIX_MAX_IRQS - 1) << 16),
};

struct rvvm_pci_function {
    pci_bus_t*       bus;
    rvvm_mmio_dev_t* bar[PCI_FUNC_BARS];
    rvvm_mmio_dev_t* expansion_rom;
    pci_bus_addr_t   addr;

    // Atomic variables
    uint32_t command;
    uint32_t status;
    uint32_t irq_line;

    // Bridge IO/MEM assignment
    uint32_t bridge_io;
    uint32_t bridge_mem;

    // Power management state
    uint32_t pm_csr;

    // MSI state
    uint32_t msi_control;
    uint32_t msi_addr_low;
    uint32_t msi_addr_high;
    uint32_t msi_data;
    uint32_t msi_mask;
    uint32_t msi_pending;

    // MSI-X state (Sits in a dedicated BAR)
    uint32_t msix_control;
    uint32_t msix_bar;
    uint32_t msix[PCI_MSIX_BAR_SIZE];

    // RO attributes
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t class_code;
    uint8_t  prog_if;
    uint8_t  rev;
    uint8_t  irq_pin;
};

struct rvvm_pci_device {
    pci_bus_t*     bus;
    pci_func_t*    func[PCI_DEV_FUNCS];
    pci_bus_addr_t addr;
};

struct rvvm_pci_bus {
    rvvm_machine_t* machine;
    rvvm_intc_t*    intc;

    vector_t(pci_dev_t*) dev;
    spinlock_t           lock;

    rvvm_irq_t irqs[PCI_BUS_IRQS];

    rvvm_addr_t io_addr;
    size_t      io_len;
    rvvm_addr_t mem_addr;
    size_t      mem_len;
};

// Get INTx IRQ pin routing id for a function
static inline rvvm_irq_t pci_bus_intx_irq(pci_bus_t* bus, uint32_t dev_id, uint32_t irq_pin)
{
    return bus->irqs[(dev_id + irq_pin + 3) & 3];
}

// Get INTx IRQ for a function
static inline rvvm_irq_t pci_func_intx_irq(pci_func_t* func)
{
    return pci_bus_intx_irq(func->bus, func->addr >> 3, func->irq_pin);
}

// Send/Raise INTx IRQ
static void pci_func_send_intx_irq(pci_func_t* func, bool raise)
{
    if (likely(!(atomic_load_uint32_relax(&func->command) & PCI_CMD_INTX_DISABLE))) {
        if (likely(func->irq_pin)) {
            atomic_store_uint32_relax(&func->status, PCI_STATUS_INTX);
            if (raise) {
                rvvm_raise_irq(func->bus->intc, pci_func_intx_irq(func));
            } else {
                rvvm_send_irq(func->bus->intc, pci_func_intx_irq(func));
            }
        }
    }
}

// Lower INTx IRQ
static void pci_func_lower_intx_irq(pci_func_t* func)
{
    atomic_store_uint32_relax(&func->status, 0);
    rvvm_lower_irq(func->bus->intc, pci_func_intx_irq(func));
}

// Attempts to send an IRQ via MSI, returns true if enabled
static bool pci_func_send_msi_irq(pci_func_t* func)
{
    if (atomic_load_uint32_relax(&func->msi_control) & PCI_MSI_ENABLED) {
        // MSI enabled
        uint32_t command = atomic_load_uint32_relax(&func->command);
        if (likely(command & PCI_CMD_BUS_MASTER)) {
            // Bus mastering enabled
            uint32_t    data = atomic_load_uint32_relax(&func->msi_data);
            rvvm_addr_t addr = atomic_load_uint32_relax(&func->msi_addr_low)
                             | ((uint64_t)atomic_load_uint32_relax(&func->msi_addr_high) << 32);

            if (likely(!(atomic_load_uint32_relax(&func->msi_mask)))) {
                // Perform an MSI write
                rvvm_send_msi_irq(func->bus->machine, addr, data);
            } else {
                // Vector is masked, set pending bit
                atomic_store_uint32_relax(&func->msi_pending, PCI_MSI_BIT);
            }
        }
        return true;
    }
    return false;
}

// Lower MSI IRQ
static bool pci_func_lower_msi_irq(pci_func_t* func)
{
    if (atomic_load_uint32_relax(&func->msi_control) & PCI_MSI_ENABLED) {
        // MSI enabled
        atomic_store_uint32_relax(&func->msi_pending, 0);
        return true;
    }
    return false;
}

// Attempts to send an IRQ via MSI-X, returns true if enabled
static bool pci_func_send_msix_irq(pci_func_t* func, uint32_t msi_id)
{
    uint32_t msix_control = atomic_load_uint32_relax(&func->msix_control);
    if (msix_control & PCI_MSIX_ENABLED) {
        // MSI-X enabled
        uint32_t command = atomic_load_uint32_relax(&func->command);
        if (likely((command & PCI_CMD_BUS_MASTER) && (msi_id < PCI_MSIX_MAX_IRQS))) {
            // Bus mastering enabled, valid MSI-X vector
            uint32_t    mask = atomic_load_uint32_relax(&func->msix[(msi_id << 2) + 3]);
            uint32_t    data = atomic_load_uint32_relax(&func->msix[(msi_id << 2) + 2]);
            rvvm_addr_t addr = atomic_load_uint32_relax(&func->msix[(msi_id << 2)])
                             | ((uint64_t)atomic_load_uint32_relax(&func->msix[(msi_id << 2) + 1]) << 32);

            if (likely(!(msix_control & PCI_MSIX_MASKED) && !(mask & 1))) {
                // Perform an MSI write
                rvvm_send_msi_irq(func->bus->machine, addr, data);
            } else {
                // Vector is masked, set pending bit
                uint32_t reg = msi_id >> 5;
                uint32_t bit = 1U << (msi_id & 0x1F);
                atomic_or_uint32(&func->msix[PCI_MSIX_TBL_SIZE + reg], bit);
            }
        }
        return true;
    }
    return false;
}

// Lower MSI-X IRQ
static bool pci_func_lower_msix_irq(pci_func_t* func, uint32_t msi_id)
{
    if (atomic_load_uint32_relax(&func->msix_control) & PCI_MSIX_ENABLED) {
        // MSI-X enabled
        if (likely(msi_id < PCI_MSIX_MAX_IRQS)) {
            uint32_t reg = PCI_MSIX_TBL_SIZE + (msi_id >> 5);
            if (atomic_load_uint32_relax(&func->msix[reg]) & bit_set32(msi_id)) {
                atomic_and_uint32(&func->msix[reg], ~bit_set32(msi_id));
            }
        }
        return true;
    }
    return false;
}

// Re-send pending MSI IRQs on MSI vector mask update
static void pci_func_update_msi_mask(pci_func_t* func)
{
    uint32_t mask = atomic_load_uint32_relax(&func->msi_mask);
    if (atomic_and_uint32(&func->msi_pending, mask)) {
        pci_func_send_msi_irq(func);
    }
}

// Re-send pending MSI-X IRQs on MSI-X vector mask or function mask update
static void pci_func_update_msix_mask(pci_func_t* func)
{
    if (atomic_load_uint32_relax(&func->msix_control) == PCI_MSIX_ENABLED) {
        for (size_t reg = 0; reg < PCI_MSIX_PBA_SIZE; ++reg) {
            uint32_t pending = atomic_load_uint32_relax(&func->msix[PCI_MSIX_TBL_SIZE + reg]);
            if (pending) {
                pending = atomic_swap_uint32(&func->msix[PCI_MSIX_TBL_SIZE + reg], 0);
                for (size_t bit = 0; bit < 32; ++bit) {
                    if (bit_check(pending, bit)) {
                        pci_func_send_msix_irq(func, (reg << 5) | bit);
                    }
                }
            }
        }
    }
}

static bool pci_msix_bar_read(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    pci_func_t* func = dev->data;
    size_t      reg  = offset >> 2;
    uint32_t    val  = atomic_load_uint32_relax(&func->msix[reg]);
    UNUSED(size);
    write_uint32_le(data, val);
    return true;
}

static bool pci_msix_bar_write(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    pci_func_t* func = dev->data;
    size_t      reg  = offset >> 2;
    uint32_t    val  = read_uint32_le(data);
    UNUSED(size);
    if (reg < PCI_MSIX_TBL_SIZE && (reg & 3) == 3 && !(val & 1)) {
        if (atomic_swap_uint32(&func->msix[reg], val) & 1) {
            // MSI-X vector unmasked
            pci_func_update_msix_mask(func);
        }
    } else {
        atomic_store_uint32_relax(&func->msix[reg], val);
    }
    return true;
}

static void msix_bar_remove(rvvm_mmio_dev_t* dev)
{
    UNUSED(dev);
}

static const rvvm_mmio_type_t pci_msix_dev_type = {
    .name   = "msix_bar",
    .remove = msix_bar_remove,
};

// Free a PCI function description that failed to attach
static void pci_free_func_desc(const pci_func_desc_t* desc)
{
    if (desc) {
        for (size_t bar_id = 0; bar_id < PCI_FUNC_BARS; ++bar_id) {
            rvvm_cleanup_mmio_desc(&desc->bar[bar_id]);
        }
    }
}

// Free a PCI device description that failed to attach
static void pci_free_dev_desc(const pci_dev_desc_t* desc)
{
    if (desc) {
        for (size_t func_id = 0; func_id < PCI_DEV_FUNCS; ++func_id) {
            pci_free_func_desc(desc->func[func_id]);
        }
    }
}

// Linearize PCI bus addesses into internal vector
static inline size_t pci_bus_addr_to_id_internal(pci_bus_addr_t bus_addr)
{
    return (bus_addr < 0x100) ? (bus_addr >> 3) : ((bus_addr >> 8) + PCI_BUS_DEVS);
}

static inline bool pci_bus_addr_valid(pci_bus_addr_t bus_addr)
{
    return (bus_addr < 0x100) || !(bus_addr & 0xF8);
}

// Assign a PCI device handle to a PCI slot (Must be called under locking)
static inline bool pci_set_bus_dev_internal(pci_bus_t* bus, pci_bus_addr_t bus_addr, pci_dev_t* dev)
{
    size_t dev_id = pci_bus_addr_to_id_internal(bus_addr);
    if (bus && (dev_id >= vector_size(bus->dev) || (!!vector_at(bus->dev, dev_id) != !!dev))) {
        vector_put(bus->dev, dev_id, dev);
        return true;
    }
    return false;
}

// Free a PCI function (Must be called under bus->lock), omit removing mmio on machine cleanup
static void pci_free_func_internal(pci_func_t* func, bool remove_mmio)
{
    if (func) {
        if (remove_mmio) {
            // Omit double-removing MMIO on final machine cleanup stage
            for (size_t bar_id = 0; bar_id < PCI_FUNC_BARS; ++bar_id) {
                rvvm_remove_mmio(func->bar[bar_id]);
            }
        }
        free(func);
    }
}

// Free a PCI device (Must be called under bus->lock), omit removing mmio on machine cleanup
static void pci_free_dev_internal(pci_dev_t* dev, bool remove_mmio)
{
    if (dev) {
        for (size_t func_id = 0; func_id < PCI_DEV_FUNCS; ++func_id) {
            pci_free_func_internal(dev->func[func_id], remove_mmio);
        }
        free(dev);
    }
}

// Assign MMIO address for a BAR
static rvvm_addr_t pci_assign_mmio_addr(pci_bus_t* bus, size_t bar_size)
{
    rvvm_addr_t size = bit_next_pow2(EVAL_MAX(bar_size, 0x1000));
    if (size >= 0x10000000U) {
        // Assign to prefetchable MMIO64 range
        return rvvm_mmio_zone_auto(bus->machine, 0x4000000000ULL, size);
    } else {
        return rvvm_mmio_zone_auto(bus->machine, 0x40000000U, size);
    }
}

// Attach a preconfigured BAR to the PCI host
static rvvm_mmio_dev_t* pci_attach_bar(pci_bus_t* bus, rvvm_mmio_dev_t bar)
{
    bar.addr = pci_assign_mmio_addr(bus, bar.size);
    return rvvm_attach_mmio(bus->machine, &bar);
}

static pci_func_t* pci_attach_func_internal(pci_bus_t* bus, const pci_func_desc_t* desc, pci_bus_addr_t bus_addr)
{
    pci_func_t* func = safe_new_obj(pci_func_t);
    func->bus        = bus;

    func->vendor_id  = desc->vendor_id;
    func->device_id  = desc->device_id;
    func->class_code = desc->class_code;
    func->prog_if    = desc->prog_if;
    func->rev        = desc->rev;
    func->irq_pin    = desc->irq_pin;

    func->addr = bus_addr;

    func->command  = PCI_CMD_DEFAULT;
    func->irq_line = pci_func_intx_irq(func);

    for (size_t bar_id = 0; bar_id < PCI_FUNC_BARS; ++bar_id) {
        if (desc->bar[bar_id].size) {
            func->bar[bar_id] = pci_attach_bar(bus, desc->bar[bar_id]);
            if (func->bar[bar_id] == NULL) {
                // Failed to attach function BAR
                free(func);
                return NULL;
            }
        } else if (!rvvm_has_arg("no_msix") && func->irq_pin && bar_id >= 4 && !func->msix_bar) {
            // Try to attach MSI-X BAR (Unless it's a host bridge)
            rvvm_mmio_dev_t msix_bar = {
                .size  = sizeof(func->msix),
                .data  = func,
                .type  = &pci_msix_dev_type,
                .read  = pci_msix_bar_read,
                .write = pci_msix_bar_write,
            };
            func->bar[bar_id] = pci_attach_bar(bus, msix_bar);
            if (func->bar[bar_id]) {
                // Successfully attached MSI-X BAR
                func->msix_bar = bar_id;
            }
        }
    }

    if (desc->expansion_rom.size) {
        func->expansion_rom = pci_attach_bar(bus, desc->expansion_rom);
        if (func->expansion_rom == NULL) {
            // Failed to attach function expansion ROM
            free(func);
            return NULL;
        }
    }

    return func;
}

// Check whether this BAR is an upper half for a previous 64-bit BAR
static inline bool pci_bar_is_upper_half(pci_func_t* func, size_t bar_id)
{
    return bar_id && !func->bar[bar_id] && func->bar[bar_id - 1];
}

// Check whether this BAR is eligible to be 64-bit wide
static inline bool pci_bar_is_64bit(pci_func_t* func, size_t bar_id)
{
    return bar_id + 1 < PCI_FUNC_BARS && func->bar[bar_id] && !func->bar[bar_id + 1];
}

// Get effective BAR (Returns same device bar for low/high parts)
static inline rvvm_mmio_dev_t* pci_effective_bar(pci_func_t* func, size_t bar_id)
{
    if (pci_bar_is_upper_half(func, bar_id)) {
        return func->bar[bar_id - 1];
    }
    return func->bar[bar_id];
}

static bool pci_bus_read(rvvm_mmio_dev_t* ecam, void* data, size_t offset, uint8_t size)
{
    pci_bus_t*     bus      = ecam->data;
    pci_bus_addr_t bus_addr = offset >> 12;
    size_t         reg      = offset & 0xFFC;
    uint32_t       val      = 0;
    UNUSED(size);

    pci_func_t* func = pci_get_bus_func(bus, bus_addr);
    if (!func) {
        // Nonexistent devices have all 0xFFFFFFFF in their conf space
        write_uint32_le(data, 0xFFFFFFFF);
        return true;
    }

    if (bus_addr && reg >= PCI_CAP_LIST_OFF) {
        // Handle RO PCI capabilities
        size_t cap_id = (reg - PCI_CAP_LIST_OFF) >> 2;
        if (cap_id < STATIC_ARRAY_SIZE(pci_express_caps_ro)) {
            val = pci_express_caps_ro[cap_id];
        }
    }

    switch (reg) {
        case PCI_REG_DEV_VEN_ID:
            val = (func->vendor_id | (uint32_t)func->device_id << 16);
            break;
        case PCI_REG_STATUS_CMD: {
            val = atomic_load_uint32_relax(&func->command) | (atomic_load_uint32_relax(&func->status) << 16);
            if (bus_addr) {
                // Host bridge doesn't have capabilities
                val |= PCI_STATUS_CAP << 16;
            }
            break;
        }
        case PCI_REG_CLASS_REV:
            val = func->rev | (((uint32_t)func->prog_if) << 8) | (((uint32_t)func->class_code) << 16);
            break;
        case PCI_REG_INFO: {
            // Advertise 64-byte cache lines
            val            = 16;
            pci_dev_t* dev = pci_get_bus_device(bus, bus_addr);
            for (size_t func_id = 0; func_id < PCI_DEV_FUNCS; ++func_id) {
                if (pci_get_device_func(dev, func_id) && (func_id != (bus_addr & 0x07))) {
                    // This is a multi-function device
                    val |= (PCI_HEADER_MULTIFUNC << 16);
                    break;
                }
            }
            if (func->class_code == 0x0604) {
                // This is a PCI-PCI bridge (PCI Express Root Port)
                val |= (PCI_HEADER_PCI_PCI << 16);
            }
            break;
        }
        case PCI_REG_IRQ_PIN_LINE:
            val = (atomic_load_uint32_relax(&func->irq_line) | ((uint32_t)func->irq_pin) << 8);
            break;
        case PCI_REG_BAR0:
        case PCI_REG_BAR1:
        case PCI_REG_BAR2:
        case PCI_REG_BAR3:
        case PCI_REG_BAR4:
        case PCI_REG_BAR5: {
            size_t bar_id = (reg - PCI_REG_BAR0) >> 2;
            // Only BAR0 & BAR1 exist for a PCI-PCI bridge
            if (func->class_code != 0x0604 || bar_id < 0x02) {
                rvvm_mmio_dev_t* bar = pci_effective_bar(func, bar_id);
                if (bar) {
                    if (pci_bar_is_upper_half(func, bar_id)) {
                        // This is an upper half of a 64-bit BAR
                        val = bar->addr >> 32;
                    } else {
                        val = bar->addr;
                        if (pci_bar_is_64bit(func, bar_id)) {
                            // This BAR supports 64-bit addressing
                            val |= PCI_BAR_64_BIT;
                        }
                        if (bar->size >= 0x10000000U) {
                            // This is a prefetchable BAR (GPU VRAM, etc)
                            val |= PCI_BAR_PREFETCH;
                        }
                    }
                }
            } else {
                // PCIe Root Ports within 00:XX.x cover every other bus in the system
                uint8_t secondary_bus = (bus_addr >> 3) + (bus_addr & 0x7);
                switch (bar_id) {
                    case 0x02:
                        val = (secondary_bus << 16) | (secondary_bus << 8);
                        break;
                    case 0x03:
                        val = atomic_load_uint32_relax(&func->bridge_io);
                        break;
                    case 0x04:
                        val = atomic_load_uint32_relax(&func->bridge_mem);
                        break;
                }
            }
            break;
        }
        case PCI_REG_SSID_SVID:
            val = 0x510010DC;
            break;
        case PCI_REG_EXPANSION_ROM:
            if (func->expansion_rom) {
                val = func->expansion_rom->addr | PCI_EXPANSION_ROM_ENABLED;
            }
            break;
        case PCI_REG_CAP_PTR:
            if (bus_addr) {
                val = PCI_CAP_LIST_OFF;
            }
            break;

        // PCI capabilities
        case PCI_REG_ENDPOINT:
            if (func->class_code == 0x0604) {
                // This is a PCI-PCI bridge (PCI Express Root Port)
                val |= (PCIE_CAP_ROOT_PORT << 20);
            } else if (!(bus_addr >> 8)) {
                // This is an integrated endpoint on bus 00
                val |= (PCIE_CAP_INTEGRATED_ENDPOINT << 20);
            }
            break;

        case PCI_REG_PMCSR:
            val |= atomic_load_uint32_relax(&func->pm_csr);
            break;

        case PCI_REG_MSI:
            val |= atomic_load_uint32_relax(&func->msi_control);
            if (!func->msix_bar) {
                // Hide MSI-X capability if MSI-X BAR is missing
                val &= ~(0xFF00U);
            }
            break;
        case PCI_REG_MSI_AL:
            val = atomic_load_uint32_relax(&func->msi_addr_low);
            break;
        case PCI_REG_MSI_AH:
            val = atomic_load_uint32_relax(&func->msi_addr_high);
            break;
        case PCI_REG_MSI_DATA:
            val = atomic_load_uint32_relax(&func->msi_data);
            break;
        case PCI_REG_MSI_MASK:
            val = atomic_load_uint32_relax(&func->msi_mask);
            break;
        case PCI_REG_MSI_PEND:
            val = atomic_load_uint32_relax(&func->msi_pending);
            break;

        case PCI_REG_MSIX:
            val |= atomic_load_uint32_relax(&func->msix_control);
            break;
        case PCI_REG_MSIX_TBL:
            val = func->msix_bar;
            break;
        case PCI_REG_MSIX_PBO:
            val = func->msix_bar | PCI_MSIX_TBL_SIZE;
            break;
    }

    write_uint32_le(data, val);

    return true;
}

static bool pci_bus_write(rvvm_mmio_dev_t* ecam, void* data, size_t offset, uint8_t size)
{
    pci_bus_t*     bus      = ecam->data;
    pci_bus_addr_t bus_addr = offset >> 12;
    size_t         reg      = offset & 0xFFC;
    uint32_t       val      = read_uint32_le(data);
    UNUSED(size);

    pci_func_t* func = pci_get_bus_func(bus, bus_addr);
    if (!func) {
        // No such device
        return true;
    }

    switch (reg) {
        case PCI_REG_STATUS_CMD: {
            uint32_t prev = atomic_swap_uint32(&func->command, val & PCI_CMD_MASK);
            if (!(prev & PCI_CMD_INTX_DISABLE) && (val & PCI_CMD_INTX_DISABLE)) {
                // Clear INTx interrupt whenever INTx is disabled
                pci_func_lower_intx_irq(func);
            }
            break;
        }
        case PCI_REG_BAR0:
        case PCI_REG_BAR1:
        case PCI_REG_BAR2:
        case PCI_REG_BAR3:
        case PCI_REG_BAR4:
        case PCI_REG_BAR5: {
            size_t bar_id = (reg - PCI_REG_BAR0) >> 2;
            // Only BAR0 & BAR1 exist for a PCI-PCI bridge
            if (func->class_code != 0x0604 || bar_id < 0x02) {
                rvvm_mmio_dev_t* bar = pci_effective_bar(func, bar_id);
                if (bar) {
                    rvvm_addr_t bar_addr = bar->addr;
                    rvvm_addr_t bar_size = bit_next_pow2(bar->size);
                    if (pci_bar_is_upper_half(func, bar_id)) {
                        // This is an upper half of a 64-bit BAR
                        bar_addr = bit_replace(bar_addr, 32, 32, val);
                    } else {
                        // Mask lower option bits and align to page
                        bar_addr = bit_replace(bar_addr, 0, 32, val & ~0xFFFU);
                    }

                    // Align address to BAR size (Must be power of 2)
                    bar->addr = bar_addr & ~(bar_size - 1);
                    atomic_fence();
                }
            } else {
                switch (bar_id) {
                    case 0x03:
                        atomic_store_uint32_relax(&func->bridge_io, val);
                        break;
                    case 0x04:
                        atomic_store_uint32_relax(&func->bridge_mem, val);
                        break;
                }
            }
            break;
        }
        case PCI_REG_EXPANSION_ROM:
            if (func->expansion_rom) {
                rvvm_addr_t rom_addr      = val & ~0xFFFU;
                rvvm_addr_t rom_size      = bit_next_pow2(func->expansion_rom->size);
                func->expansion_rom->addr = rom_addr & ~(rom_size - 1);
                atomic_fence();
            }
            break;
        case PCI_REG_IRQ_PIN_LINE:
            atomic_store_uint32_relax(&func->irq_line, (uint8_t)val);
            break;

        case PCI_REG_PMCSR:
            atomic_store_uint32_relax(&func->pm_csr, val & PCI_PMCSR_PS);
            break;

        // PCI Capabilities
        case PCI_REG_MSI:
            atomic_store_uint32_relax(&func->msi_control, val & PCI_MSI_ENABLED);
            break;
        case PCI_REG_MSI_AL:
            atomic_store_uint32_relax(&func->msi_addr_low, val);
            break;
        case PCI_REG_MSI_AH:
            atomic_store_uint32_relax(&func->msi_addr_high, val);
            break;
        case PCI_REG_MSI_DATA:
            atomic_store_uint32_relax(&func->msi_data, val & PCI_MSI_DATA);
            break;
        case PCI_REG_MSI_MASK:
            atomic_store_uint32_relax(&func->msi_mask, val & PCI_MSI_BIT);
            pci_func_update_msi_mask(func);
            break;

        case PCI_REG_MSIX: {
            uint32_t prev = atomic_swap_uint32(&func->msix_control, val & PCI_MSIX_VALID);
            if ((prev & PCI_MSIX_MASKED) && !(val & PCI_MSIX_MASKED)) {
                pci_func_update_msix_mask(func);
            }
            break;
        }
    }

    return true;
}

static void pci_bus_remove(rvvm_mmio_dev_t* ecam)
{
    pci_bus_t* bus = ecam->data;
    vector_foreach (bus->dev, i) {
        pci_free_dev_internal(vector_at(bus->dev, i), false);
    }
    vector_free(bus->dev);
    free(bus);
}

static const rvvm_mmio_type_t pci_bus_type = {
    .name   = "pci_bus",
    .remove = pci_bus_remove,
};

PUBLIC pci_bus_t* pci_bus_init(rvvm_machine_t* machine,                   //
                               rvvm_intc_t* intc, const rvvm_irq_t* irqs, //
                               rvvm_addr_t ecam_addr, size_t bus_count,   //
                               rvvm_addr_t io_addr, size_t io_len,        //
                               rvvm_addr_t mem_addr, size_t mem_len)
{
    pci_bus_t* bus = safe_new_obj(pci_bus_t);
    bus->machine   = machine;
    bus->intc      = intc;

    for (size_t i = 0; i < PCI_BUS_IRQS; ++i) {
        bus->irqs[i] = irqs[i];
    }

    bus->io_addr  = io_addr;
    bus->io_len   = io_len;
    bus->mem_addr = mem_addr;
    bus->mem_len  = mem_len;

    rvvm_mmio_dev_t pci_bus_mmio = {
        .addr        = ecam_addr,
        .size        = (bus_count << 20),
        .data        = bus,
        .type        = &pci_bus_type,
        .read        = pci_bus_read,
        .write       = pci_bus_write,
        .min_op_size = 4,
        .max_op_size = 4,
    };

    if (!rvvm_attach_mmio(machine, &pci_bus_mmio)) {
        // Failed to attach the PCI bus
        rvvm_error("Failed to attach PCI ECAM Root Complex!");
        return NULL;
    }

    // Host Bridge: SiFive, Inc. FU740-C000 RISC-V SoC PCI Express x8
    pci_func_desc_t bridge_desc = {
        .vendor_id  = 0xF15E,
        .class_code = 0x0600,
    };
    pci_attach_func(bus, &bridge_desc);

    if (rvvm_has_arg("pcie_ports")) {
        // Root Ports
        pci_func_desc_t root_port_desc = {
            .vendor_id  = 0x1556,
            .device_id  = 0xBE00,
            .class_code = 0x0604,
            .irq_pin    = PCI_IRQ_PIN_INTA,
        };
        for (pci_bus_addr_t bus_addr = 1; bus_addr < 4; ++bus_addr) {
            pci_attach_func_at(bus, &root_port_desc, bus_addr);
        }
    }

    rvvm_set_pci_bus(machine, bus);

#if defined(USE_FDT)
    struct fdt_node* imsic_fdt = fdt_node_find_reg_any(rvvm_get_fdt_soc(machine), "imsics_s");
    struct fdt_node* pci_fdt   = fdt_node_create_reg("pci", pci_bus_mmio.addr);
    fdt_node_add_prop_reg(pci_fdt, "reg", pci_bus_mmio.addr, pci_bus_mmio.size);
    fdt_node_add_prop_str(pci_fdt, "compatible", "pci-host-ecam-generic");
    fdt_node_add_prop_str(pci_fdt, "device_type", "pci");
    fdt_node_add_prop(pci_fdt, "dma-coherent", NULL, 0);

    if (imsic_fdt) {
        fdt_node_add_prop_u32(pci_fdt, "msi-parent", fdt_node_get_phandle(imsic_fdt));
    }

    uint32_t bus_range[] = {
        0,
        bus_count - 1,
    };
    fdt_node_add_prop_cells(pci_fdt, "bus-range", bus_range, STATIC_ARRAY_SIZE(bus_range));

    fdt_node_add_prop_u32(pci_fdt, "#address-cells", 3);
    fdt_node_add_prop_u32(pci_fdt, "#size-cells", 2);
    fdt_node_add_prop_u32(pci_fdt, "#interrupt-cells", 1);

#define FDT_ADDR(addr) (((uint64_t)(addr)) >> 32), ((uint32_t)(addr))

    // clang-format off
    uint32_t ranges[] = {
        // IO Range 0x03000000...0x0300FFFF -> 0x00000000
        0x01000000U, FDT_ADDR(0x00000000U),     FDT_ADDR(0x03000000U),     FDT_ADDR(0x00010000U),
        // Non-prefetchable MMIO32 Range 0x40000000...0x7FFFFFFF -> 0x40000000
        0x02000000U, FDT_ADDR(0x40000000U),     FDT_ADDR(0x40000000U),     FDT_ADDR(0x40000000U),
        // Prefetchable MMIO64 Range 0x4000000000...0x7FFFFFFFFF -> 0x4000000000
        0x43000000U, FDT_ADDR(0x4000000000ULL), FDT_ADDR(0x4000000000ULL), FDT_ADDR(0x4000000000ULL),
    };
    // clang-format on

    fdt_node_add_prop_cells(pci_fdt, "ranges", ranges, STATIC_ARRAY_SIZE(ranges));

    // Crossing-style IRQ routing for IRQ balancing
    // INTA of dev 2 routes the same way as INTB of dev 1, etc
    uint32_t           intc_handle = rvvm_fdt_intc_phandle(intc);
    vector_t(uint32_t) irq_map     = ZERO_INIT;

    for (uint32_t dev_id = 0; dev_id < PCI_BUS_IRQS; ++dev_id) {
        for (uint32_t irq_pin = 1; irq_pin <= PCI_BUS_IRQS; ++irq_pin) {
            rvvm_irq_t irq      = pci_bus_intx_irq(bus, dev_id, irq_pin);
            uint32_t   cells[8] = ZERO_INIT;
            size_t     count    = rvvm_fdt_irq_cells(intc, irq, cells, STATIC_ARRAY_SIZE(cells));

            // PCI address
            vector_push_back(irq_map, dev_id << 11);
            vector_push_back(irq_map, 0);
            vector_push_back(irq_map, 0);

            // PCI irq pin
            vector_push_back(irq_map, irq_pin);

            // Interrupt controller handle
            vector_push_back(irq_map, intc_handle);

            // Interrupt cells
            for (size_t cell = 0; cell < count; ++cell) {
                vector_push_back(irq_map, cells[cell]);
            }
        }
    }

    fdt_node_add_prop_cells(pci_fdt, "interrupt-map", vector_buffer(irq_map), vector_size(irq_map));
    vector_free(irq_map);

    uint32_t irq_mask[] = {0x1800, 0x0000, 0x0000, 0x0007};
    fdt_node_add_prop_cells(pci_fdt, "interrupt-map-mask", irq_mask, STATIC_ARRAY_SIZE(irq_mask));

    fdt_node_add_child(rvvm_get_fdt_soc(machine), pci_fdt);
#endif
    return bus;
}

PUBLIC pci_bus_t* pci_bus_init_auto(rvvm_machine_t* machine)
{
    rvvm_intc_t* intc               = rvvm_get_intc(machine);
    rvvm_irq_t   irqs[PCI_BUS_IRQS] = ZERO_INIT;
    size_t       bus_count          = 256;
    rvvm_addr_t  addr               = rvvm_mmio_zone_auto(machine, PCI_ECAM_ADDR_DEFAULT, bus_count << 20);
    for (size_t i = 0; i < PCI_BUS_IRQS; ++i) {
        irqs[i] = rvvm_alloc_irq(intc);
    }
    return pci_bus_init(machine, intc, irqs, addr, bus_count,     //
                        PCI_IO_ADDR_DEFAULT, PCI_IO_SIZE_DEFAULT, //
                        PCI_MEM_ADDR_DEFAULT, PCI_MEM_SIZE_DEFAULT);
}

PUBLIC pci_dev_t* pci_attach_func(pci_bus_t* bus, const pci_func_desc_t* desc)
{
    pci_dev_desc_t dev_desc = {
        .func[0] = desc,
    };
    return pci_attach_multifunc(bus, &dev_desc);
}

PUBLIC pci_dev_t* pci_attach_multifunc(pci_bus_t* bus, const pci_dev_desc_t* desc)
{
    for (pci_bus_addr_t bus_addr = 0x00; bus_addr < 0x100; bus_addr += PCI_DEV_FUNCS) {
        if (!pci_get_bus_device(bus, bus_addr)) {
            // Found a free integrated PCI slot
            return pci_attach_multifunc_at(bus, desc, bus_addr);
        }
    }
    if (rvvm_has_arg("pcie_ports")) {
        for (pci_bus_addr_t bus_addr = 0x100; bus_addr < 0x800; bus_addr += 0x100) {
            if (!pci_get_bus_device(bus, bus_addr)) {
                // Found a free PCI port
                return pci_attach_multifunc_at(bus, desc, bus_addr);
            }
        }
    }
    rvvm_error("Too many PCI devices attached!");
    pci_free_dev_desc(desc);
    return NULL;
}

PUBLIC pci_dev_t* pci_attach_func_at(pci_bus_t* bus, const pci_func_desc_t* desc, pci_bus_addr_t bus_addr)
{
    pci_dev_t* dev     = pci_get_bus_device(bus, bus_addr);
    size_t     func_id = bus_addr & 0x07;
    if (dev) {
        // Inject a function into an existing device
        pci_func_t* func = pci_attach_func_internal(bus, desc, bus_addr);
        if (!func) {
            // Failed to attach function
            pci_free_func_desc(desc);
            return NULL;
        }

        scoped_spin_lock (&bus->lock) {
            // Pause the vCPUs and attach the device
            bool was_running = rvvm_pause_machine(bus->machine);
            if (dev->func[func_id]) {
                // Another function already in the slot
                rvvm_error("PCI function %02x:%02x.%01x is busy!", //
                           (bus_addr >> 8) & 0xFF, (bus_addr >> 3) & 0x1F, (uint32_t)func_id);
                pci_free_func_internal(func, true);
                dev = NULL;
            } else {
                dev->func[func_id] = func;
            }
            if (was_running) {
                rvvm_start_machine(bus->machine);
            }
        }
        return dev;
    } else {
        // Create a new device
        pci_dev_desc_t dev_desc = ZERO_INIT;
        dev_desc.func[func_id]  = desc;
        return pci_attach_multifunc_at(bus, &dev_desc, bus_addr & ~0x7U);
    }
}

PUBLIC pci_dev_t* pci_attach_multifunc_at(pci_bus_t* bus, const pci_dev_desc_t* desc, pci_bus_addr_t bus_addr)
{
    if (bus) {
        pci_dev_t* dev = safe_new_obj(pci_dev_t);
        dev->bus       = bus;
        dev->addr      = bus_addr;

        for (size_t func_id = 0; func_id < PCI_DEV_FUNCS; ++func_id) {
            if (desc->func[func_id]) {
                pci_func_t* func   = pci_attach_func_internal(bus, desc->func[func_id], bus_addr + func_id);
                dev->func[func_id] = func;
                if (!func) {
                    // Failed to attach function
                    pci_free_dev_internal(dev, true);
                    return NULL;
                }
            }
        }

        scoped_spin_lock (&bus->lock) {
            // Pause the vCPUs and attach the device
            bool was_running = rvvm_pause_machine(bus->machine);
            if (!pci_set_bus_dev_internal(bus, bus_addr, dev)) {
                // Another device already in the slot
                rvvm_error("PCI slot %02x:%02x.0 is busy!", //
                           (bus_addr >> 8) & 0xFF, (bus_addr >> 3) & 0x1F);
                pci_free_dev_internal(dev, true);
                dev = NULL;
            }
            if (was_running) {
                rvvm_start_machine(bus->machine);
            }
        }

        return dev;
    }

    pci_free_dev_desc(desc);
    return NULL;
}

PUBLIC pci_dev_t* pci_get_bus_device(pci_bus_t* bus, pci_bus_addr_t bus_addr)
{
    size_t     dev_id = pci_bus_addr_to_id_internal(bus_addr);
    pci_dev_t* dev    = NULL;
    if (bus && pci_bus_addr_valid(bus_addr)) {
        scoped_spin_lock (&bus->lock) {
            if (dev_id < vector_size(bus->dev)) {
                dev = vector_at(bus->dev, dev_id);
            }
        }
    }
    return dev;
}

PUBLIC pci_func_t* pci_get_bus_func(pci_bus_t* bus, pci_bus_addr_t bus_addr)
{
    if (bus) {
        pci_dev_t* dev = pci_get_bus_device(bus, bus_addr);
        if (dev) {
            return pci_get_device_func(dev, bus_addr & 0x07);
        }
    }
    return NULL;
}

PUBLIC pci_bus_addr_t pci_get_device_bus_addr(pci_dev_t* dev)
{
    return dev ? dev->addr : 0;
}

PUBLIC pci_bus_addr_t pci_get_func_bus_addr(pci_func_t* func)
{
    return func ? func->addr : 0;
}

PUBLIC void pci_remove_device(pci_dev_t* dev)
{
    if (dev) {
        pci_bus_t* bus = dev->bus;
        scoped_spin_lock (&bus->lock) {
            // Pause the vCPUs and remove the device
            bool was_running = rvvm_pause_machine(bus->machine);
            pci_set_bus_dev_internal(bus, pci_get_device_bus_addr(dev), NULL);
            pci_free_dev_internal(dev, true);
            if (was_running) {
                rvvm_start_machine(bus->machine);
            }
        }
    }
}

PUBLIC pci_func_t* pci_get_device_func(pci_dev_t* dev, size_t func_id)
{
    if (dev && func_id < PCI_DEV_FUNCS) {
        return dev->func[func_id];
    }
    return NULL;
}

PUBLIC void pci_send_irq(pci_func_t* func, uint32_t msi_id)
{
    if (likely(func)) {
        // Try to send MSI/MSI-X IRQ
        if (!pci_func_send_msix_irq(func, msi_id) && !pci_func_send_msi_irq(func)) {
            // Send INTx IRQ edge - obscure, but needed for e.q. VFIO to work somehow
            pci_func_send_intx_irq(func, false);
        }
    }
}

PUBLIC void pci_raise_irq(pci_func_t* func, uint32_t msi_id)
{
    if (likely(func)) {
        // Try to send MSI/MSI-X IRQ
        if (!pci_func_send_msix_irq(func, msi_id) && !pci_func_send_msi_irq(func)) {
            // Raise a level-triggered INTx IRQ
            pci_func_send_intx_irq(func, true);
        }
    }
}

PUBLIC void pci_lower_irq(pci_func_t* func, uint32_t msi_id)
{
    if (likely(func)) {
        // Try to lower MSI/MSI-X IRQ
        if (!pci_func_lower_msix_irq(func, msi_id) && !pci_func_lower_msi_irq(func)) {
            // Lower a level-triggered INTx IRQ
            pci_func_lower_intx_irq(func);
        }
    }
}

PUBLIC void* pci_get_dma_ptr(pci_func_t* func, rvvm_addr_t addr, size_t size)
{
    if (likely(func && (atomic_load_uint32_relax(&func->command) & PCI_CMD_BUS_MASTER))) {
        // DMA requires bus mastering to be enabled
        return rvvm_get_dma_ptr(func->bus->machine, addr, size);
    }
    return NULL;
}
