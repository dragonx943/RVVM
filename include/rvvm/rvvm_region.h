/*
<rvvm/rvvm_region.h> - RVVM Region-based devices API
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
 * @defgroup rvvm_reg_dev Region-based devices API
 * @addtogroup rvvm_reg_dev
 * @{
 */

/*
 * Region attributes
 */
#define RVVM_REG_ATTR_FIX 0x01 /**< Region address is fixed           */
#define RVVM_REG_ATTR_ROM 0x02 /**< Region is read-only               */
#define RVVM_REG_ATTR_PIO 0x04 /**< Region accessed via Port IO (x86) */

/**
 * Region-based device callbacks
 *
 * Must be valid during device lifetime, or static
 *
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
     *
     * This function may be called from multiple threads
     */
    void (*read)(rvvm_reg_dev_t* dev, void* data, size_t size, size_t off);

    /**
     * Region write callback
     *
     * \param dev  Region device handle
     * \param data Pointer to written data
     * \param size Access size
     * \param off  Offset within region (Always aligned to size)
     *
     * This function may be called from multiple threads
     */
    void (*write)(rvvm_reg_dev_t* dev, const void* data, size_t size, size_t off);

    /**
     * Periodical poll callback
     *
     * \param dev Region device handle
     *
     * This function is exclusively called from a single thread
     */
    void (*poll)(rvvm_reg_dev_t* dev);

    /**
     * Reset callback
     *
     * \param dev  Region device handle
     *
     * This function is exclusively called from a single thread
     */
    void (*reset)(rvvm_reg_dev_t* dev);

    /**
     * Suspend/resume, serialize snapshot
     *
     * \param dev    Region device handle
     * \param snap   Snapshot state if non-null
     * \param resume Deserialize, resume device processing
     *
     * This function is exclusively called from a single thread
     */
    void (*suspend)(rvvm_reg_dev_t* dev, rvvm_snapshot_t* snap, bool resume);

    /**
     * Cleanup (free) callback, should drop device private data / mappings
     *
     * Called on machine cleanup, failure to attach device, or
     * explicit rvvm_region_free() / rvvm_region_free_desc() calls
     *
     * \param dev Region device handle
     *
     * This function is exclusively called from a single thread
     */
    void (*free)(rvvm_reg_dev_t* dev);

    /**
     * Minimum operation size allowed
     *
     * This effectively enforces minimum alignment as well,
     * smaller operations are implicitly promoted to larger ones
     */
    uint32_t min_size;

    /**
     * Maximum operation size allowed (Or zero for no limit)
     *
     * Larger operations are implicitly split into smaller ones
     */
    uint32_t max_size;

} rvvm_reg_type_t;

/**
 * Region device instance description
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
     *
     * If this is non-null, read/write callbacks are not invoked
     */
    void* mmap;

    /**
     * Region type and callbacks
     */
    const rvvm_reg_type_t* type;

    /**
     * Region attributes
     *
     * Defaults to read-write memory with address allocation
     */
    uint32_t attr;

} rvvm_reg_desc_t;

/**
 * Attach region device to machine
 *
 * If the requested address is busy and RVVM_REG_ATTR_FIX is not set,
 * nearest usable address is automatically picked for the region
 *
 * If this call fails, device cleanup is invoked, same as when
 * machine is freed or device is hot-removed
 *
 * \param  machine Machine handle (Nullable)
 * \param  desc    Region description, copied internally
 * \return         Region device handle or NULL
 *
 * This function is thread-safe
 */
RVVM_PUBLIC rvvm_reg_dev_t* rvvm_region_init(rvvm_machine_t* machine, const rvvm_reg_desc_t* desc);

/**
 * Free region device handle
 *
 * Removes the device from owning machine and invokes cleanup as usual
 *
 * \param dev Region device handle (Nullable)
 * \note      Must not be called from device's own callbacks or internal threads
 *
 * This function is thread-safe
 */
RVVM_PUBLIC void rvvm_region_free(rvvm_reg_dev_t* dev);

/**
 * Invoke region device cleanup via description
 *
 * This may be used to properly clean up multi-region devices on attach error
 *
 * Implementation is based on well-defined behavior that region init invokes cleanup
 *
 * \param desc Region description, copied internally
 */
static inline void rvvm_region_free_desc(const rvvm_reg_desc_t* desc)
{
    rvvm_region_init(NULL, desc);
}

/**
 * Get region device private data
 *
 * \param dev Region device handle (Nullable)
 * \return    Region device private data
 *
 * This function is thread-safe
 */
RVVM_PUBLIC void* rvvm_region_data(rvvm_reg_dev_t* dev);

/**
 * Get region device owning machine
 *
 * \param dev Region device handle (Nullable)
 * \return    Machine handle or NULL
 *
 * This function is thread-safe
 */
RVVM_PUBLIC rvvm_machine_t* rvvm_region_machine(rvvm_reg_dev_t* dev);

/**
 * Get region device description
 *
 * \param dev  Region device handle (Nullable)
 * \param desc Pointer to fill description
 * \return     Success
 *
 * This function is thread-safe
 */
RVVM_PUBLIC bool rvvm_region_get_desc(rvvm_reg_dev_t* dev, rvvm_reg_desc_t* desc);

/**
 * Update region device description
 *
 * This may be used to relocate or resize, update region private data, etc
 * Region updates are carried atomically with respect to running vCPUs
 *
 * \param dev  Region device handle (Nullable)
 * \param desc Pointer to new description
 * \return     Success
 *
 * This function is thread-safe
 */
RVVM_PUBLIC bool rvvm_region_set_desc(rvvm_reg_dev_t* dev, const rvvm_reg_desc_t* desc);

/**
 * Relocate register device to other address
 *
 * This may be used e.g. for PCI BARs
 *
 * \param dev  Region device handle (Nullable)
 * \param addr New region address
 * \return     Success
 *
 * This function is thread-safe
 */
static inline bool rvvm_region_relocate(rvvm_reg_dev_t* dev, rvvm_addr_t addr)
{
    rvvm_reg_desc_t desc;
    if (rvvm_region_get_desc(dev, &desc)) {
        desc.addr = addr;
        return rvvm_region_set_desc(dev, &desc);
    }
    return false;
}

/**
 * Attach region device to machine, obtain resulting description back
 *
 * If the requested address is busy and RVVM_REG_ATTR_FIX is not set,
 * nearest usable address is automatically picked for the region
 *
 * If attach fails or machine is NULL, device is freed and NULL returned
 *
 * \param  machine Machine handle (Nullable)
 * \param  desc    Region description, copied internally and updated with new data
 * \return         Region device handle or NULL
 *
 * This function is thread-safe
 */
static inline rvvm_reg_dev_t* rvvm_region_init_auto(rvvm_machine_t* machine, rvvm_reg_desc_t* desc)
{
    rvvm_reg_dev_t* dev = rvvm_region_init(machine, desc);
    if (dev) {
        rvvm_region_get_desc(dev, desc);
    }
    return dev;
}

/** @}*/

RVVM_EXTERN_C_END

#endif
