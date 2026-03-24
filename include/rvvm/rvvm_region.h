/*
<rvvm/rvvm_region.h> - RVVM Region-based devices
Copyright (C) 2020-2026 LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _RVVM_REGION_API_H
#define _RVVM_REGION_API_H

#include <rvvm/rvvm_base.h>

RVVM_EXTERN_C_BEGIN

/**
 * @defgroup rvvm_reg_dev RVVM Region-based devices
 * @addtogroup rvvm_reg_dev
 * @{
 */

/*
 * Region spaces
 */
#define RVVM_REGION_MEM 0x00 /**< Memory and MMIO, default  */
#define RVVM_REGION_ROM 0x01 /**< Read-only memory and MMIO */
#define RVVM_REGION_PIO 0x02 /**< Port IO (x86)             */

/**
 * Region-based device callbacks
 *
 * Must be valid during device lifetime, or static
 * Callbacks are optional (Nullable)
 */
typedef struct {
    /**
     * Device identifier string, used in logger, snapshot and registry
     */
    const char* name;

    /**
     * Region read callback
     *
     * \param dev  Region device handle
     * \param data Pointer to data to be read
     * \param size Access size
     * \param off  Offset within region (Always aligned to size)
     */
    void (*read)(rvvm_reg_dev_t* dev, void* data, size_t size, size_t off);

    /**
     * Region write callback
     *
     * \param dev  Region device handle
     * \param data Pointer to written data
     * \param size Access size
     * \param off  Offset within region (Always aligned to size)
     */
    void (*write)(rvvm_reg_dev_t* dev, const void* data, size_t size, size_t off);

    /**
     * Periodical poll callback
     *
     * \param dev Region device handle
     */
    void (*poll)(rvvm_reg_dev_t* dev);

    /**
     * Reset callback
     *
     * \param dev  Region device handle
     */
    void (*reset)(rvvm_reg_dev_t* dev);

    /**
     * Suspend/resume, serialize snapshot
     *
     * \param dev    Region device handle
     * \param snap   Snapshot state if non-null
     * \param resume Deserialize, resume device processing
     */
    void (*suspend)(rvvm_reg_dev_t* dev, rvvm_snapshot_t* snap, bool resume);

    /**
     * Cleanup (free) callback, should drop device private data / mappings
     *
     * Called on machine cleanup, failure to attach device, or
     * explicit rvvm_region_free() / rvvm_region_free_desc() calls
     *
     * \param dev Region device handle
     */
    void (*free)(rvvm_reg_dev_t* dev);

    /**
     * Minimum operation size allowed
     */
    uint32_t min_size;

    /**
     * Maximum operation size allowed
     */
    uint32_t max_size;

} rvvm_region_type_t;

/**
 * Region device instance description
 *
 * Disposable, passed to rvvm_reg_dev_init()
 */
typedef struct {
    /**
     * Region address in memory / port space
     */
    rvvm_addr_t addr;

    /**
     * Region size
     */
    size_t size;

    /**
     * Private data, managed by device
     */
    void* data;

    /**
     * Directly mapped memory region, managed by device
     */
    void* mmap;

    /**
     * Region type and callbacks
     */
    const rvvm_region_type_t* type;

    /**
     * Region space, defaults to memory and MMIO
     */
    uint32_t space;

} rvvm_region_desc_t;

/**
 * Attach region device to machine
 *
 * If the requested address is busy, nearest usable address is picked
 * If attach fails or machine is NULL, device is freed and NULL returned
 *
 * \param  machine Machine handle
 * \param  desc    Region description, copied internally
 * \return Region device handle, or NULL on failure
 */
RVVM_PUBLIC rvvm_reg_dev_t* rvvm_region_init(rvvm_machine_t* machine, const rvvm_region_desc_t* desc);

/**
 * Free region device handle
 *
 * Removes the device from owning machine and invokes cleanup as usual
 *
 * \param dev Region device handle
 */
RVVM_PUBLIC void rvvm_region_free(rvvm_reg_dev_t* dev);

/**
 * Invoke region device cleanup callback via description
 *
 * Calls device free callback as if device attach failed
 * This may be used to properly clean up multi-region devices on attach error
 *
 * \param desc Region description, copied internally
 */
static inline void rvvm_region_free_desc(const rvvm_region_desc_t* desc)
{
    rvvm_region_init(NULL, desc);
}

/**
 * Get region device private data
 *
 * \param dev Region device handle
 * \return Region device private data
 */
RVVM_PUBLIC void* rvvm_region_get_data(rvvm_reg_dev_t* dev);

/**
 * Get region device owning machine
 *
 * \param dev Region device handle
 * \return Machine handle or NULL
 */
RVVM_PUBLIC rvvm_machine_t* rvvm_region_get_machine(rvvm_reg_dev_t* dev);

/**
 * Get region description from handle
 *
 * \param dev Region device handle
 * \return Region description or NULL
 */
RVVM_PUBLIC const rvvm_region_desc_t* rvvm_region_get_desc(rvvm_reg_dev_t* dev);

/**
 * Update region device description
 *
 * This may be used to relocate or resize, update region private data, etc
 * Region updates are carried atomically with respect to running vCPUs
 *
 * \param dev  Region device handle
 * \param desc New region device description
 */
RVVM_PUBLIC void rvvm_region_update_desc(rvvm_reg_dev_t* dev, const rvvm_region_desc_t* desc);

/**
 * Relocate register device to other address
 *
 * This may be used e.g. for PCI BARs
 *
 * \param dev  Region device handle
 * \param addr New region address
 */
static inline void rvvm_region_relocate(rvvm_reg_dev_t* dev, rvvm_addr_t addr)
{
    const rvvm_region_desc_t* orig = rvvm_region_get_desc(dev);
    if (orig) {
        rvvm_region_desc_t desc = *orig;
        /* Replace description address */
        desc.addr = addr;
        rvvm_region_update_desc(dev, &desc);
    }
}

/** @}*/

RVVM_EXTERN_C_END

#endif
