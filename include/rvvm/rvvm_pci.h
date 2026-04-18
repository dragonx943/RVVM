/*
rvvm_pci.h - RVVM PCI Bus API
Copyright (C) 2020-2026 LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _RVVM_PCI_API_H
#define _RVVM_PCI_API_H

#include <rvvm/rvvm_region.h>

RVVM_EXTERN_C_BEGIN

/**
 * @defgroup rvvm_pci_api PCI Bus API
 * @addtogroup rvvm_pci_api
 * @{
 */

/*
 * PCI INTx pins
 */
#define RVVM_PCI_PIN_NONE 0x00
#define RVVM_PCI_PIN_INTA 0x01
#define RVVM_PCI_PIN_INTB 0x02
#define RVVM_PCI_PIN_INTC 0x03
#define RVVM_PCI_PIN_INTD 0x04

/*
 * PCI DMA attributes
 */
#define RVVM_PCI_DMA_RD   0x01
#define RVVM_PCI_DMA_WR   0x02
#define RVVM_PCI_DMA_RW   0x03

/**
 * Auto-allocated bus address
 */
#define RVVM_PCI_ADDR_ANY 0xFFFFFFFFUL

/**
 * Auto-allocated hot-pluggable bus address
 */
#define RVVM_PCI_ADDR_HOT 0xFFFFFFFEUL

/**
 * PCI function description
 */
typedef struct {
    /**
     * Vendor ID from PCI database
     */
    uint16_t vendor_id;

    /**
     * Device ID from PCI database
     */
    uint16_t device_id;

    /**
     * Subsystem vendor ID (May be zero)
     */
    uint16_t subsys_ven;

    /**
     * Subsystem device ID (May be zero)
     */
    uint16_t subsys_dev;

    /**
     * Class code
     */
    uint8_t class_code;

    /**
     * Subclass
     */
    uint8_t subclass;

    /**
     * Programming interface
     */
    uint8_t prog_if;

    /**
     * Revision
     */
    uint8_t rev;

    /**
     * INTx interrupt pin
     */
    uint8_t pin;

    /**
     * BAR region descriptions (Nullable)
     */
    const rvvm_reg_desc_t* bar[6];

    /**
     * Expansion ROM region (Nullable)
     */
    const rvvm_reg_desc_t* rom;

    /**
     * Additional legacy PCI capabilities (Nullable)
     * This capability starts at 0xC0 in legacy PCI configuration space
     */
    const rvvm_reg_desc_t* cap;

    /**
     * Additional PCI Express capabilities (Nullable)
     * This capability starts at 0x100 in PCI Express configuration space
     */
    const rvvm_reg_desc_t* ecap;

} rvvm_pci_func_desc_t;

/**
 * Attach ECAM PCIe host controller (ACPI/FDT based)
 *
 * \param machine  Machine handle
 * \param domain   PCI domain
 * \param addr     Base address of ECAM space
 * \param irq_dev  Wired interrupt controller handle
 * \param irqs     Vector of 4 wired IRQs for legacy INTx interrupts
 * \param io_addr  Start of PCI IO Port space
 * \param io_size  Length of PCI IO Port space
 * \param mem_addr Start of PCI MMIO Space
 * \param mem_size Length of PCI MMIO Space
 * \return         Attach success
 */
RVVM_PUBLIC bool rvvm_pci_bus_init_ecam(rvvm_machine_t*   machine,   /**/
                                        uint32_t          domain,    /**/
                                        rvvm_addr_t       addr,      /**/
                                        rvvm_irq_dev_t*   irq_dev,   /**/
                                        const rvvm_irq_t* irqs,      /**/
                                        rvvm_addr_t       io_addr,   /**/
                                        rvvm_addr_t       io_size,   /**/
                                        rvvm_addr_t       mem_addr,  /**/
                                        rvvm_addr_t       mem_size); /**/

/**
 * Attach legacy x86 PCI controller via IO ports
 *
 * \param machine Machine handle
 * \param port    Base port of PCI controller, usually 0xCF8
 * \param irq_dev Wired interrupt controller handle
 * \param irqs    Vector of 4 wired IRQs, usually 11, 10, 9, 5
 * \return        Attach success
 */
RVVM_PUBLIC bool rvvm_pci_bus_init_legacy(rvvm_machine_t*   machine, /**/
                                          rvvm_addr_t       port,    /**/
                                          rvvm_irq_dev_t*   irq_dev, /**/
                                          const rvvm_irq_t* irqs);

/**
 * Attach PCI function to machine at specific bus address
 *
 * If attach fails, device is freed and NULL returned
 *
 * \param machine Machine handle
 * \param desc    PCI function description, fully copied internally
 * \param addr    PCI bus address or RVVM_PCI_ADDR_ANY
 * \return        PCI function handle or NULL
 *
 * Multi-function devices may be constructed manually via this
 *
 * This function is thread-safe
 */
RVVM_PUBLIC rvvm_pci_func_t* rvvm_pci_func_init_at(rvvm_machine_t*             machine, /**/
                                                   const rvvm_pci_func_desc_t* desc,    /**/
                                                   rvvm_pci_addr_t             addr);

/**
 * Attach PCI function to machine at any usable bus address
 *
 * If attach fails, device is freed and NULL returned
 *
 * \param machine Machine handle
 * \param desc    PCI function description, fully copied internally
 * \return        PCI function handle or NULL
 *
 * This function is thread-safe
 */
static inline rvvm_pci_func_t* rvvm_pci_func_init(rvvm_machine_t* machine, const rvvm_pci_func_desc_t* desc)
{
    return rvvm_pci_func_init_at(machine, desc, RVVM_PCI_ADDR_ANY);
}

/**
 * Free PCI function handle
 *
 * Removes the device from owning machine and invokes cleanup as usual
 *
 * \param func PCI function handle
 * \note       Must not be called from device's own callbacks or internal threads
 *
 * This function is thread-safe
 */
RVVM_PUBLIC void rvvm_pci_func_free(rvvm_pci_func_t* func);

/**
 * Get PCI function handle from bus address
 *
 * \param machine Machine handle
 * \param addr    PCI bus address
 * \return        PCI function handle or NULL
 *
 * This function is thread-safe
 */
RVVM_PUBLIC rvvm_pci_func_t* rvvm_pci_func_from_addr(rvvm_machine_t* machine, rvvm_pci_addr_t addr);

/**
 * Get bus address of a PCI function
 *
 * \param func PCI function handle
 * \return     PCI function bus address
 *
 * This function is thread-safe
 */
RVVM_PUBLIC rvvm_pci_addr_t rvvm_pci_func_get_addr(rvvm_pci_func_t* func);

/**
 * Set interrupt level of a PCI function
 *
 * \param func PCI function handle which set the IRQ
 * \param vec  Interrupt vector index provided by the device
 * \param lvl  Interrupt line level
 *
 * The interrupt is delivered via INTx/MSI/MSI-X based on configuration space
 *
 * This function is thread-safe
 */
RVVM_PUBLIC void rvvm_pci_set_irq(rvvm_pci_func_t* func, uint32_t vec, bool lvl);

/**
 * Raise interrupt vector of a PCI function
 *
 * \param func PCI function handle which raised the IRQ
 * \param vec  Interrupt vector index provided by the device
 *
 * This function is thread-safe
 */
static inline void rvvm_pci_raise_irq(rvvm_pci_func_t* func, uint32_t vec)
{
    rvvm_pci_set_irq(func, vec, true);
}

/**
 * Lower interrupt vector of a PCI function
 *
 * \param func PCI function handle which lowered the IRQ
 * \param vec  Interrupt vector index provided by the device
 *
 * This function is thread-safe
 */
static inline void rvvm_pci_lower_irq(rvvm_pci_func_t* func, uint32_t vec)
{
    rvvm_pci_set_irq(func, vec, false);
}

/**
 * Send interrupt edge (pulse) from a PCI function
 *
 * \param func PCI function handle which sent the IRQ
 * \param vec  Interrupt vector index provided by the device
 *
 * Should be used for VFIO or other devices which can't report lowered interrupt
 *
 * This function is thread-safe
 */
static inline void rvvm_pci_pulse_irq(rvvm_pci_func_t* func, uint32_t vec)
{
    rvvm_pci_set_irq(func, vec, true);
    rvvm_pci_set_irq(func, vec, false);
}

/**
 * Perform direct memory access to the PCI host
 *
 * \param func PCI function handle which performs DMA access
 * \param addr Physical memory address
 * \param size Memory region size
 * \param attr DMA operation attributes (read/write/etc)
 * \return     Pointer to DMA memory or NULL
 * \note       DMA access must be ended via rvvm_pci_end_dma()
 *
 * This function is thread-safe
 */
RVVM_PUBLIC void* rvvm_pci_get_dma_ex(rvvm_pci_func_t* func, rvvm_addr_t addr, size_t size, uint32_t attr);

/**
 * Perform direct memory access to the PCI host (Read/Write)
 *
 * \param func PCI function handle which performs DMA access
 * \param addr Physical memory address
 * \param size Memory region size
 * \return     Pointer to DMA memory or NULL
 * \note       DMA access must be ended via rvvm_pci_end_dma()
 *
 * This function is thread-safe
 */
static inline void* rvvm_pci_get_dma(rvvm_pci_func_t* func, rvvm_addr_t addr, size_t size)
{
    return rvvm_pci_get_dma_ex(func, addr, size, RVVM_PCI_DMA_RW);
}

/**
 * End direct memory access started by rvvm_pci_get_dma()
 *
 * \param func PCI function handle which performs DMA access
 * \param ptr  Pointer to DMA memory obtained via rvvm_pci_get_dma()
 *
 * Required for proper RAM hotplug and IOMMU support
 *
 * This function is thread-safe
 */
RVVM_PUBLIC void rvvm_pci_end_dma(rvvm_pci_func_t* func, void* ptr);

/** @}*/

RVVM_EXTERN_C_END

#endif
