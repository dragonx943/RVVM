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
#define RVVM_PCI_PIN_INTA 0x01
#define RVVM_PCI_PIN_INTB 0x02
#define RVVM_PCI_PIN_INTC 0x03
#define RVVM_PCI_PIN_INTD 0x04

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
     * BAR region descriptions
     */
    const rvvm_region_desc_t* bar[6];

    /**
     * Expansion ROM region description
     */
    const rvvm_region_desc_t* rom;

    /**
     * Configuration space region description
     */
    const rvvm_region_desc_t* cfg;

} rvvm_pci_func_desc_t;

/**
 *  Attach ECAM PCIe host controller (ACPI/FDT based)
 *
 * \param machine  Machine handle
 * \param domain   PCI domain
 * \param addr     Base address of ECAM space
 * \param intc     Wired interrupt controller handle
 * \param irqs     Vector of 4 wire IRQs for legacy INTx interrupts
 * \param io_addr  Start of PCI IO Port space
 * \param io_size  Length of PCI IO Port space
 * \param mem_addr Start of PCI MMIO Space
 * \param mem_size Length of PCI MMIO Space
 * \return Attach success
 */
RVVM_PUBLIC bool rvvm_pci_bus_init_ecam(rvvm_machine_t*   machine,   /**/
                                        uint32_t          domain,    /**/
                                        rvvm_addr_t       addr,      /**/
                                        rvvm_intc_t*      intc,      /**/
                                        const rvvm_irq_t* irqs,      /**/
                                        rvvm_addr_t       io_addr,   /**/
                                        rvvm_addr_t       io_size,   /**/
                                        rvvm_addr_t       mem_addr,  /**/
                                        rvvm_addr_t       mem_size); /**/

/**
 * Attach legacy x86 PCI controller at IO port 0xCF8
 *
 * \param machine Machine handle
 * \return Attach success
 */
RVVM_PUBLIC bool rvvm_pci_bus_init_legacy(rvvm_machine_t* machine);

/**
 * Attach PCI function to machine at specific bus address
 *
 * If attach fails, device is freed and NULL returned
 * Multi-function devices may be constructed by manually plugging their separate functions
 *
 * \param machine Machine handle
 * \param desc    PCI function description, copied internally
 * \param addr    PCI bus address or -1 for automatic address
 * \return PCI function handle or NULL
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
 * \param desc    PCI function description, copied internally
 * \return PCI function handle or NULL
 */
static inline rvvm_pci_func_t* rvvm_pci_func_init(rvvm_machine_t* machine, const rvvm_pci_func_desc_t* desc)
{
    return rvvm_pci_func_init_at(machine, desc, -1);
}

/**
 * Free PCI function handle
 *
 * Removes the device from owning machine and invokes cleanup as usual
 *
 * \param func PCI function handle
 */
RVVM_PUBLIC void rvvm_pci_func_free(rvvm_pci_func_t* func);

/**
 * Get PCI function handle from bus address
 *
 * \param machine Machine handle
 * \param addr    PCI bus address
 * \return PCI function handle or NULL
 */
RVVM_PUBLIC rvvm_pci_func_t* rvvm_pci_func_from_addr(rvvm_machine_t* machine, rvvm_pci_addr_t addr);

/**
 * Get bus address of a PCI function
 *
 * \param func PCI function handle
 * \return PCI function bus address
 */
RVVM_PUBLIC rvvm_pci_addr_t rvvm_pci_func_get_addr(rvvm_pci_func_t* func);

/**
 * Set interrupt level of a PCI function
 *
 * \param func    PCI function handle which set the IRQ
 * \param msi_vec MSI/MSI-X IRQ Vector (Ignored when INTx enabled)
 */
RVVM_PUBLIC void rvvm_pci_set_irq(rvvm_pci_func_t* func, uint32_t msi_vec, bool level);

/**
 * Raise interrupt vector of a PCI function
 *
 * \param func    PCI function handle which raised the IRQ
 * \param msi_vec MSI/MSI-X IRQ Vector (Ignored when INTx enabled)
 */
static inline void rvvm_pci_raise_irq(rvvm_pci_func_t* func, uint32_t msi_vec)
{
    rvvm_pci_set_irq(func, msi_vec, true);
}

/**
 * Lower interrupt vector of a PCI function
 *
 * \param func    PCI function handle which lowered the IRQ
 * \param msi_vec MSI/MSI-X IRQ Vector (Ignored when INTx enabled)
 */
static inline void rvvm_pci_lower_irq(rvvm_pci_func_t* func, uint32_t msi_vec)
{
    rvvm_pci_set_irq(func, msi_vec, false);
}

/**
 * Perform direct memory access to the PCI host
 *
 * DMA access must be ended via rvvm_pci_end_dma() for IOMMU and RAM hotplug support
 *
 * \param func PCI function handle which performs DMA access
 * \param addr Physical memory address
 * \param size Memory region size
 * \param rw   Writable region
 * \return Pointer to DMA memory or NULL
 */
RVVM_PUBLIC void* rvvm_pci_get_dma(rvvm_pci_func_t* func, rvvm_addr_t addr, size_t size, bool rw);

/**
 * End direct memory access to the PCI host
 *
 * \param func PCI function handle which performs DMA access
 * \param ptr  Pointer to DMA memory obtained via rvvm_pci_get_dma()
 */
RVVM_PUBLIC void rvvm_pci_end_dma(rvvm_pci_func_t* func, void* ptr);


/** @}*/

RVVM_EXTERN_C_END

#endif
