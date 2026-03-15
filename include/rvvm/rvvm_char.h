/*
<rvvm/rvvm_char.h> - RVVM Character Device (Serial) API
Copyright (C) 2020-2026  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _RVVM_CHAR_API_H
#define _RVVM_CHAR_API_H

#include <rvvm/rvvm_base.h>

RVVM_EXTERN_C_BEGIN

/**
 * @defgroup rvvm_char_api Character Device API
 * @addtogroup rvvm_char_api
 * @{
 *
 * Serial controller and character device both hold a handle to rvvm_char_dev_t,
 * and may set up callbacks on reading/writing serial data by another side
 *
 * Both may also bi-directionally read/write serial data instead of callbacks,
 * which may be more suitable for certain API usecases
 *
 * The rvvm_char_dev_t context contains an RX/TX FIFO, akin to a pipe buffer
 *
 * The rvvm_char_dev_t context is reference-counted
 */

/*
 * Serial data availability flags for rvvm_char_poll()
 */
#define RVVM_CHAR_RX            0x01 /** Serial data receive available  */
#define RVVM_CHAR_TX            0x02 /** Serial data transmit available */

/*
 * Auxillary codes for rvvm_char_aux()
 */
#define RVVM_CHAR_AUX_I2C_START 0x00 /** Start I2C transmission */
#define RVVM_CHAR_AUX_I2C_STOP  0x01 /** Stop I2C transmission */

/**
 * Character device callbacks
 *
 * Must be valid during chardev lifetime, or static
 */
typedef struct {
    /**
     * Free callback (All references are dropped)
     */
    void (*free)(rvvm_char_dev_t* chardev);

    /**
     * Read callback (Other side wants to pull data out)
     */
    size_t (*read)(rvvm_char_dev_t* chardev, void* buffer, size_t size);

    /**
     * Write callback (Other side wants to push data in)
     */
    size_t (*write)(rvvm_char_dev_t* chardev, const void* buffer, size_t size);

    /**
     * Auxillary callback for special devices, like I2C
     */
    void* (*aux)(rvvm_char_dev_t* chardev, uint32_t aux, void* ptr);

    /**
     * Suspend/resume, serialize to snapshot if non-null
     */
    void (*suspend)(rvvm_char_dev_t* chardev, rvvm_snapshot_t* snap, bool resume);

} rvvm_char_cb_t;

/**
 * Create a new character device
 *
 * \param cb   Character device callbacks or NULL
 * \param data Private character device data to obtain via rvvm_char_get_data()
 * \return New character device handle
 */
RVVM_PUBLIC rvvm_char_dev_t* rvvm_char_init(const rvvm_char_cb_t* cb, void* data);

/**
 * Pair character devices into a pipe
 *
 * Paired devices share their reference count, so they are
 * both freed whenever both sides drop their char references
 */
RVVM_PUBLIC void rvvm_char_pair(rvvm_char_dev_t* a, rvvm_char_dev_t* b);

/**
 * Unpair character devices back
 *
 * This may be done on serial controller deinit, which doesn't expect a
 * free callback to ever be called, and only goes down with the machine
 *
 * Example:
 * if (!rvvm_char_drop_ref(chardev)) {
 *     rvvm_char_unpair(chardev);
 * }
 */
static inline void rvvm_char_unpair(rvvm_char_dev_t* chardev)
{
    rvvm_char_pair(chardev, NULL);
}

/**
 * Modify / Get character device reference count
 *
 * \param  ref Increment refcount (May be zero or negative)
 * \return Resulting refcount
 */
RVVM_PUBLIC uint32_t rvvm_char_refcount(rvvm_char_dev_t* chardev, int32_t ref);

/**
 * Drop character device reference
 * \return True if the character device/pair was freed
 */
static inline bool rvvm_char_drop_ref(rvvm_char_dev_t* chardev)
{
    return !rvvm_char_refcount(chardev, -1);
}

/**
 * Get private character device data
 */
RVVM_PUBLIC void* rvvm_char_get_data(rvvm_char_dev_t* chardev);

/**
 * Poll serial data availability on character device side
 */
RVVM_PUBLIC uint32_t rvvm_char_poll(rvvm_char_dev_t* chardev);

/**
 * Read serial data inbound to character device
 */
RVVM_PUBLIC size_t rvvm_char_read(rvvm_char_dev_t* chardev, void* buffer, size_t size);

/**
 * Write serial data outbound from character device
 */
RVVM_PUBLIC size_t rvvm_char_write(rvvm_char_dev_t* chardev, const void* buffer, size_t size);

/**
 * Auxillary operation on other side, similar to ioctl()
 */
RVVM_PUBLIC void* rvvm_char_aux(rvvm_char_dev_t* chardev, uint32_t aux, void* ptr);

/**
 * Request device suspend/resume
 * \param snap Snapshot state to serialize into if non-null
 */
RVVM_PUBLIC void rvvm_char_suspend(rvvm_char_dev_t* chardev, rvvm_snapshot_t* snap, bool resume);

/** @}*/

RVVM_EXTERN_C_END

#endif
