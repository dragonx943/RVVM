/*
<rvvm/rvvm_blk.h> - Block device IO
Copyright (C) 2020-2025  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _RVVM_BLKDEV_API_H
#define _RVVM_BLKDEV_API_H

#include <rvvm/rvvm.h>

RVVM_EXTERN_C_BEGIN

/**
 * @defgroup rvvm_blk_api Block device API
 * @addtogroup rvvm_blk_api
 * @{
 */

/*
 * Options for rvvm_blk_open()
 */

#define RVVM_BLK_READ   0x01 /**< Permit reading the block device image      */
#define RVVM_BLK_WRITE  0x02 /**< Permit writing the block device image      */
#define RVVM_BLK_RW     0x03 /**< Open block device image in read/write mode */
#define RVVM_BLK_CREAT  0x04 /**< Create image if it doesn't exist           */
#define RVVM_BLK_DIRECT 0x20 /**< Direct DMA IO with the underlying storage  */
#define RVVM_BLK_SYNC   0x40 /**< Disable writeback buffering                */

/**
 * Block device image handle
 */
typedef struct rvvm_blk_dev rvvm_blk_dev_t;

/**
 * Open a block device image
 *
 * \param name Image name (File path, etc)
 * \param fmt  Specific format identifier, or NULL to probe a matching one
 * \param opts Open options (Read/Write/Create), zero implies RVVM_BLK_RW
 * \return     Block device image handle, or NULL on failure
 */
RVVM_PUBLIC rvvm_blk_dev_t* rvvm_blk_open(const char* name, const char* fmt, uint32_t opts);

/**
 * Close a block device image handle
 */
RVVM_PUBLIC void rvvm_blk_close(rvvm_blk_dev_t* dev);

/**
 * Get block device size in bytes
 */
RVVM_PUBLIC uint64_t rvvm_blk_get_size(rvvm_blk_dev_t* dev);

/**
 * Get block device format identifier string
 */
RVVM_PUBLIC const char* rvvm_blk_get_format(rvvm_blk_dev_t* dev);

/**
 * Resize block device (If supported)
 */
RVVM_PUBLIC bool rvvm_blk_set_size(rvvm_blk_dev_t* dev, uint64_t size);

/**
 * Read data at specified offset, returns how much was actually read
 */
RVVM_PUBLIC size_t rvvm_blk_read(rvvm_blk_dev_t* dev, void* dst, size_t size, uint64_t off);

/**
 * Write data at specified offset, returns how much was actually written
 */
RVVM_PUBLIC size_t rvvm_blk_write(rvvm_blk_dev_t* dev, const void* src, size_t size, uint64_t off);

/**
 * Seek drive head
 */
RVVM_PUBLIC bool rvvm_blk_seek_head(rvvm_blk_dev_t* dev, uint64_t off);

/**
 * Tell drive head position
 */
RVVM_PUBLIC uint64_t rvvm_blk_tell_head(rvvm_blk_dev_t* dev);

/**
 * Write data at drive head, returns how much was actually written
 */
RVVM_PUBLIC size_t rvvm_blk_write_head(rvvm_blk_dev_t* dev, const void* src, size_t size);

/**
 * Read data at drive head, returns how much was actually read
 */
RVVM_PUBLIC size_t rvvm_blk_read_head(rvvm_blk_dev_t* dev, void* dst, size_t size);

/**
 * Write data at drive head, returns how much was actually written
 */
RVVM_PUBLIC size_t rvvm_blk_write_head(rvvm_blk_dev_t* dev, const void* src, size_t size);

/**
 * Synchronize / Barrier buffered writes to stable storage
 */
RVVM_PUBLIC bool rvvm_blk_sync(rvvm_blk_dev_t* dev);

/**
 * Trim (Deallocate) unused data (If supported)
 */
RVVM_PUBLIC bool rvvm_blk_trim(rvvm_blk_dev_t* dev, uint64_t off, uint64_t size);

/** @}*/

/**
 * @defgroup rvvm_blk_format_api Block device image format API
 * @addtogroup rvvm_blk_format_api
 * @{
 */

/**
 * Block device format handlers
 */
typedef struct {
    /**
     * Block device format identifier
     */
    const char* name;

    /**
     * Probe block device format
     */
    bool (*probe)(rvvm_blk_dev_t* dev, const char* name, uint32_t opts);

    /**
     * Free private block device data
     */
    void (*close)(rvvm_blk_dev_t* dev);

    /**
     * Get block device size in bytes
     */
    uint64_t (*get_size)(rvvm_blk_dev_t* dev);

    /**
     * Resize block device (If supported)
     */
    bool (*set_size)(rvvm_blk_dev_t* dev, uint64_t size);

    /**
     * Read data at specified offset, returns how much was actually read
     */
    size_t (*read)(rvvm_blk_dev_t* dev, void* dst, size_t size, uint64_t off);

    /**
     * Write data at specified offset, returns how much was actually written
     */
    size_t (*write)(rvvm_blk_dev_t* dev, const void* src, size_t size, uint64_t off);

    /**
     * Synchronize / Barrier buffered writes to stable storage
     */
    bool (*sync)(rvvm_blk_dev_t* dev);

    /**
     * Trim (Deallocate) unused data (If supported)
     */
    bool (*trim)(rvvm_blk_dev_t* dev, uint64_t off, uint64_t size);
} rvvm_blk_cb_t;

/**
 * Register block device image format
 *
 * \param cb Block device handlers
 *
 * The structure must be available until rvvm_unregister_blk_format() is called.
 * It is preferable to simply statically allocate it, like this:
 *
 * static const rvvm_blk_cb_t blk_raw_cb = {
 *     .open = blk_raw_open,
 *     ...
 * };
 */
RVVM_PUBLIC bool rvvm_register_blk_format(const rvvm_blk_cb_t* cb);

/**
 * Unregister block device image format
 *
 * \param cb Block device handlers previously registered by rvvm_register_blk_format()
 */
RVVM_PUBLIC void rvvm_unregister_blk_format(const rvvm_blk_cb_t* cb);

/**
 * Set private block device image format data (On probe)
 */
RVVM_PUBLIC void rvvm_blk_set_format_data(rvvm_blk_dev_t* dev, void* data);

/**
 * Get private block device image format data
 */
RVVM_PUBLIC void* rvvm_blk_get_format_data(rvvm_blk_dev_t* dev);

/** @}*/

RVVM_EXTERN_C_END

#endif
