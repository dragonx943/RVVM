/*
<rvvm/rvvm_irq.h> - RVVM Wired Interrupts API
Copyright (C) 2020-2026 LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _RVVM_IRQ_API_H
#define _RVVM_IRQ_API_H

#include <rvvm/rvvm_base.h>

RVVM_EXTERN_C_BEGIN

/**
 * @defgroup rvvm_irq_api Wired interrupts API
 * @addtogroup rvvm_irq_api
 * @{
 */

/**
 * Interrupt controller callbacks
 *
 * Must be valid during controller lifetime, or static
 * Callbacks are optional (Nullable)
 */
typedef struct {
    /**
     * Set IRQ pin on interrupt controller
     *
     * Callable from any thread, interrupt controller should use
     * locking or atomics internally to prevent race conditions
     *
     * Interrupt pin state changes should be observable in consistent order,
     * and state transitions should be detected as interrupt edge
     */
    void (*set_irq)(rvvm_irq_dev_t* irq_dev, rvvm_irq_t irq, bool level);

    /**
     * Get FDT phandle of an interrupt controller
     */
    uint32_t (*fdt_phandle)(rvvm_irq_dev_t* irq_dev);

    /**
     * Get interrupts-extended FDT cells for an IRQ
     */
    size_t (*fdt_irq_cells)(rvvm_irq_dev_t* irq_dev, rvvm_irq_t irq, uint32_t* cells, size_t size);

} rvvm_irq_cb_t;

/**
 * Create a new interrupt controller
 *
 * \param cb   Interrupt controller callbacks
 * \param data Private interrupt controller data
 * \return Interrupt controller handle
 */
RVVM_PUBLIC rvvm_irq_dev_t* rvvm_irq_dev_init(const rvvm_irq_cb_t* cb, void* data);

/**
 * Free interrupt controller handle
 *
 * Should be called by interrupt controller implementation on cleanup
 *
 * The interrupt controller must be cleaned last in device hierarchy,
 * so that the devices which were using its interrupt pins are already freed
 *
 * \param irq_dev Interrupt controller handle
 */
RVVM_PUBLIC void rvvm_irq_dev_free(rvvm_irq_dev_t* irq_dev);

/**
 * Get FDT phandle of an interrupt controller
 *
 * \param irq_dev Interrupt controller handle
 * \return FDT phandle
 */
RVVM_PUBLIC uint32_t rvvm_irq_dev_fdt_phandle(rvvm_irq_dev_t* irq_dev);

/**
 * Allocate interrupt pin
 *
 * \param irq_dev Interrupt controller handle
 * \param suggest Suggest IRQ pin to allocate, or zero for automatic allocation
 * \return Allocated IRQ pin
 */
RVVM_PUBLIC rvvm_irq_t rvvm_irq_alloc(rvvm_irq_dev_t* irq_dev, rvvm_irq_t suggest);

/**
 * Deallocate interrupt pin
 *
 * Should be called when device no longer intends to use the interrupt pin
 *
 * \param irq_dev Interrupt controller handle
 * \param irq     Allocated IRQ pin
 */
RVVM_PUBLIC void rvvm_irq_dealloc(rvvm_irq_dev_t* irq_dev, rvvm_irq_t irq);

/**
 * Set interrupt pin level
 *
 * Interrupt pin state changes are observable in consistent order
 *
 * \param irq_dev Interrupt controller handle
 * \param irq     Interrupt pin
 * \param level   Interrupt level
 */
RVVM_PUBLIC void rvvm_irq_set(rvvm_irq_dev_t* irq_dev, rvvm_irq_t irq, bool level);

/**
 * Raise interrupt pin
 *
 * \param irq_dev Interrupt controller handle
 * \param irq     Interrupt pin
 */
static inline void rvvm_irq_raise(rvvm_irq_dev_t* irq_dev, rvvm_irq_t irq)
{
    rvvm_irq_set(irq_dev, irq, true);
}

/**
 * Lower interrupt pin
 *
 * \param irq_dev Interrupt controller handle
 * \param irq     Interrupt pin
 */
static inline void rvvm_irq_lower(rvvm_irq_dev_t* irq_dev, rvvm_irq_t irq)
{
    rvvm_irq_set(irq_dev, irq, false);
}

/**
 * Send an edge-triggered interrupt
 *
 * \param irq_dev Interrupt controller handle
 * \param irq     Interrupt pin
 */
static inline void rvvm_irq_send(rvvm_irq_dev_t* irq_dev, rvvm_irq_t irq)
{
    rvvm_irq_set(irq_dev, irq, true);
    rvvm_irq_set(irq_dev, irq, false);
}

/**
 * Add interrupt-parent & interrupts fields to FDT node
 *
 * \param node    FDT node to describe interrupt into
 * \param irq_dev Interrupt controller handle
 * \param irq     Interrupt pin
 */
RVVM_PUBLIC void rvvm_irq_fdt_describe(rvvm_fdt_node_t* node, rvvm_irq_dev_t* irq_dev, rvvm_irq_t irq);

/**
 * Get interrupts-extended FDT cells for an IRQ
 *
 * \param irq_dev Interrupt controller handle
 * \param irq     Interrupt pin
 * \param cells   FDT cells buffer
 * \param size    Buffer size (In cells)
 * \return FDT cells count
 */
RVVM_PUBLIC size_t rvvm_irq_fdt_cells(rvvm_irq_dev_t* irq_dev, rvvm_irq_t irq, uint32_t* cells, size_t size);

/** @}*/

RVVM_EXTERN_C_END

#endif
