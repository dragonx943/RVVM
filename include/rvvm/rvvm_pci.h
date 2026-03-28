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
#define RVVM_PCI_ADDR_ANY ((rvvm_pci_addr_t)(-1))

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
    const rvvm_region_desc_t* bar[6];

    /**
     * Expansion ROM region (Nullable)
     */
    const rvvm_region_desc_t* rom;

    /**
     * Additional configuration space capabilities (Nullable)
     *
     * Useful for VFIO and Virtio devices
     */
    const rvvm_region_desc_t* cap;

} rvvm_pci_func_desc_t;

/**
 *  Attach ECAM PCIe host controller (ACPI/FDT based)
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
 * \return Attach success
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
 * \return Attach success
 */
RVVM_PUBLIC bool rvvm_pci_bus_init_legacy(rvvm_machine_t*   machine, /**/
                                          rvvm_addr_t       port,    /**/
                                          rvvm_irq_dev_t*   irq_dev, /**/
                                          const rvvm_irq_t* irqs);

/**
 * Attach PCI function to machine at specific bus address
 *
 * If attach fails, device is freed and NULL returned
 * Multi-function devices may be constructed by manually plugging their separate functions
 *
 * \param machine Machine handle
 * \param desc    PCI function description, temporary
 * \param addr    PCI bus address or RVVM_PCI_ADDR_ANY
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
 * \param desc    PCI function description, temporary
 * \return PCI function handle or NULL
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
 * The interrupt is delivered via INTx/MSI/MSI-X based on guest configuration
 *
 * \param func PCI function handle which set the IRQ
 * \param vec  Interrupt vector index provided by the device
 */
RVVM_PUBLIC void rvvm_pci_set_irq(rvvm_pci_func_t* func, uint32_t vec, bool level);

/**
 * Raise interrupt vector of a PCI function
 *
 * The interrupt is delivered via INTx/MSI/MSI-X based on guest configuration
 *
 * \param func PCI function handle which raised the IRQ
 * \param vec  Interrupt vector index provided by the device
 */
static inline void rvvm_pci_raise_irq(rvvm_pci_func_t* func, uint32_t vec)
{
    rvvm_pci_set_irq(func, vec, true);
}

/**
 * Lower interrupt vector of a PCI function
 *
 * The interrupt is delivered via INTx/MSI/MSI-X based on guest configuration
 *
 * \param func PCI function handle which lowered the IRQ
 * \param vec  Interrupt vector index provided by the device
 */
static inline void rvvm_pci_lower_irq(rvvm_pci_func_t* func, uint32_t vec)
{
    rvvm_pci_set_irq(func, vec, false);
}

/**
 * Perform direct memory access to the PCI host
 *
 * DMA access must be ended via rvvm_pci_end_dma() for IOMMU and RAM hotplug support
 *
 * \param func PCI function handle which performs DMA access
 * \param addr Physical memory address
 * \param size Memory region size
 * \param attr DMA operation attributes (read/write/etc)
 * \return Pointer to DMA memory or NULL
 */
RVVM_PUBLIC void* rvvm_pci_get_dma_ex(rvvm_pci_func_t* func, rvvm_addr_t addr, size_t size, uint32_t attr);

/**
 * Perform direct memory access to the PCI host (Read/Write)
 *
 * DMA access must be ended via rvvm_pci_end_dma() for IOMMU and RAM hotplug support
 *
 * \param func PCI function handle which performs DMA access
 * \param addr Physical memory address
 * \param size Memory region size
 * \return Pointer to DMA memory or NULL
 */
static inline void* rvvm_pci_get_dma(rvvm_pci_func_t* func, rvvm_addr_t addr, size_t size)
{
    return rvvm_pci_get_dma_ex(func, addr, size, RVVM_PCI_DMA_RW);
}

/**
 * End direct memory access to the PCI host started by rvvm_pci_get_dma()
 *
 * \param func PCI function handle which performs DMA access
 * \param ptr  Pointer to DMA memory obtained via rvvm_pci_get_dma()
 */
RVVM_PUBLIC void rvvm_pci_end_dma(rvvm_pci_func_t* func, void* ptr);


/** @}*/

RVVM_EXTERN_C_END

#endif
