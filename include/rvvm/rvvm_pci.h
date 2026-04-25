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
#define RVVM_PCI_DMA_RD   0x01 /**< Read via DMA mapping       */
#define RVVM_PCI_DMA_WR   0x02 /**< Write via DMA mapping      */
#define RVVM_PCI_DMA_RW   0x03 /**< Read/Write via DMA mapping */
#define RVVM_PCI_DMA_PART 0x04 /**< Allow partial DMA mapping  */

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
 * Attach PCI function to machine at specific bus address
 *
 * The machine owns PCI devices and their handles
 *
 * If this call fails, device cleanup is invoked, same as when
 * machine is freed or device is hot-removed
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
RVVM_PUBLIC rvvm_pci_func_t* rvvm_pci_func_init(rvvm_machine_t*             machine, /**/
                                                const rvvm_pci_func_desc_t* desc,    /**/
                                                rvvm_pci_addr_t             addr);

/**
 * Remove PCI function from machine
 *
 * This should only be used for device hot-removal, device cleanup is automatic
 *
 * \param func PCI function handle
 * \note       Must not be called after rvvm_machine_free() on owning machine,
 *             nor from device's own callbacks or internal threads
 *
 * This function is thread-safe
 */
RVVM_PUBLIC void rvvm_pci_func_remove(rvvm_pci_func_t* func);

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
static inline void rvvm_pci_send_irq(rvvm_pci_func_t* func, uint32_t vec)
{
    rvvm_pci_set_irq(func, vec, true);
    rvvm_pci_set_irq(func, vec, false);
}

/**
 * Perform direct memory access to the PCI host
 *
 * If RVVM_PCI_DMA_PART is set, returned mapping may be smaller than requested, which will
 * be reflected in *size, otherwise this function may fall back to IOMMU bounce buffer
 *
 * The RVVM_PCI_DMA_RD / RVVM_PCI_DMA_WR specify cache invalidation policy,
 * as well as optimize redundant IOMMU bounce buffer copies
 *
 * For write-only mappings, it is expected the caller fully fills the mapping with data
 *
 * The region which backs the DMA mapping will become locked from removal,
 * and the DMA core internally maintains reference counting on DMA mappings
 *
 * \param func PCI function handle which performs DMA access
 * \param addr Physical memory address
 * \param size Memory region size, returns actual obtained size
 * \param attr DMA operation attributes (read/write/partial)
 * \return     Pointer to DMA memory or NULL
 * \note       DMA access must be ended via rvvm_pci_end_dma()
 *
 * This function is thread-safe
 */
RVVM_PUBLIC void* rvvm_pci_get_dma_ex(rvvm_pci_func_t* func, rvvm_addr_t addr, size_t* size, uint32_t attr);

/**
 * Perform direct memory access to the PCI host (Read/Write, possibly partial)
 *
 * \param func PCI function handle which performs DMA access
 * \param addr Physical memory address
 * \param size Memory region size, returns actual obtained size
 * \return     Pointer to DMA memory or NULL
 * \note       DMA access must be ended via rvvm_pci_end_dma()
 *
 * This function is thread-safe
 */
static inline void* rvvm_pci_get_dma_part(rvvm_pci_func_t* func, rvvm_addr_t addr, size_t* size)
{
    return rvvm_pci_get_dma_ex(func, addr, size, RVVM_PCI_DMA_RW | RVVM_PCI_DMA_PART);
}

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
    return rvvm_pci_get_dma_ex(func, addr, &size, RVVM_PCI_DMA_RW);
}

/**
 * End direct memory access started by rvvm_pci_get_dma()
 *
 * Must be called for every successful rvvm_pci_get_dma()
 * after you're no longer using the obtained mapping
 *
 * \param func PCI function handle which performs DMA access
 * \param ptr  Pointer to DMA memory obtained via rvvm_pci_get_dma()
 *
 * This function is thread-safe
 */
RVVM_PUBLIC void rvvm_pci_end_dma(rvvm_pci_func_t* func, void* ptr);

/** @}*/

RVVM_EXTERN_C_END

#endif
