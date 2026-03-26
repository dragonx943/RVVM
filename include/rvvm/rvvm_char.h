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
 * Serial controller and serial device both hold a handle to rvvm_char_dev_t,
 * and may set up callbacks on reading/writing serial data by the other side
 *
 * The rvvm_char_dev_t context contains a FIFO, similar to a pipe buffer, and
 * is internally synchronized - callbacks are never called from multiple threads
 *
 * Both ends may always read/write serial data via FIFO if the other end does
 * not immediately consume all the data via callback
 *
 * The callbacks may be optionally set to NULL to purely use the FIFO
 *
 * The write callback may return zero, and used as a notification callback
 *
 * Periodic polling mode may be enabled, which will coalesce read/write
 * callbacks, and only transfer data upon polling or when FIFO is filled up,
 * which optimizes operation of slow character devices (Like host terminals)
 *
 * Both ends should be explicitly paired, and receive a close callback when
 * the other side frees its character device handle
 *
 * The close callback should be handled if the device is considered to be owned
 * by the other side (Serial controller, etc) to free its private data / handle
 */

/*
 * Serial data availability flags for rvvm_char_poll()
 */
#define RVVM_CHAR_RX            0x01 /**< Available data to read   */
#define RVVM_CHAR_TX            0x02 /**< Available space to write */

/*
 * Auxiliary codes for rvvm_char_aux()
 */
#define RVVM_CHAR_AUX_PAIRED    0x0000 /**< Paired with other character device  */
#define RVVM_CHAR_AUX_UNPAIRED  0x0001 /**< Unpaired                            */

/**
 * Character device callbacks
 *
 * Must be valid during chardev lifetime, or static
 * Callbacks are optional (Nullable)
 */
typedef struct {
    /**
     * Read callback
     */
    size_t (*read)(rvvm_char_dev_t* chardev, void* buffer, size_t size);

    /**
     * Write callback
     */
    size_t (*write)(rvvm_char_dev_t* chardev, const void* buffer, size_t size);

    /**
     * Auxiliary callback for special devices, like I2C
     */
    int32_t (*aux)(rvvm_char_dev_t* chardev, uint32_t aux, void* ptr);

    /**
     * Suspend/resume, serialize to snapshot if non-null
     */
    void (*suspend)(rvvm_char_dev_t* chardev, rvvm_snapshot_t* snap, bool resume);

    /**
     * Close callback
     *
     * Should be handled if this chardev is owned by other side,
     * and may be ignored if current chardev is owned elsewhere
     */
    void (*close)(rvvm_char_dev_t* chardev);

} rvvm_char_cb_t;

/**
 * Create a new character device
 *
 * \param cb   Character device callbacks
 * \param data Private character device data to obtain via rvvm_char_get_data()
 * \return Character device handle
 */
RVVM_PUBLIC rvvm_char_dev_t* rvvm_char_init(const rvvm_char_cb_t* cb, void* data);

/**
 * Free character device handle
 *
 * Calls close callback on both sides, unpairing character devices beforehand
 *
 * May be safely called recursively from close callback, it is simply ignored
 *
 * \param chardev Character device handle
 */
RVVM_PUBLIC void rvvm_char_free(rvvm_char_dev_t* chardev);

/**
 * Pair character devices, providing a pipe-like mechanism
 *
 * The previous pair established on both devices is torn down
 */
RVVM_PUBLIC void rvvm_char_pair(rvvm_char_dev_t* a, rvvm_char_dev_t* b);

/**
 * Unpair character devices back
 *
 * \param chardev Character device handle
 */
static inline void rvvm_char_unpair(rvvm_char_dev_t* chardev)
{
    rvvm_char_pair(chardev, NULL);
}

/**
 * Get private character device data
 *
 * \param chardev Character device handle
 * \return Own private data passed to rvvm_char_init()
 */
RVVM_PUBLIC void* rvvm_char_get_data(rvvm_char_dev_t* chardev);

/**
 * Get line status, optional periodic polling
 *
 * \param chardev Character device handle
 * \param polling Perform periodic polling, if desired, disables event-driven model
 * \return Combination of RVVM_CHAR_RX/RVVM_CHAR_TX bits based on data availability
 */
RVVM_PUBLIC uint32_t rvvm_char_poll(rvvm_char_dev_t* chardev, bool polling);

/**
 * Read serial data
 *
 * \param chardev Character device handle
 * \param data    Serial data buffer
 * \param size    Buffer capacity
 * \return Amount of bytes actually read
 */
RVVM_PUBLIC size_t rvvm_char_read(rvvm_char_dev_t* chardev, void* data, size_t size);

/**
 * Write serial data
 *
 * \param chardev Character device handle
 * \param data    Serial data buffer
 * \param size    Serial data length
 * \return Amount of bytes actually written
 */
RVVM_PUBLIC size_t rvvm_char_write(rvvm_char_dev_t* chardev, const void* data, size_t size);

/**
 * Auxiliary operation on other side, similar to ioctl()
 *
 * \param chardev Character device handle
 * \param aux     Auxiliary operation code
 * \param ptr     Auxiliary data for operation
 * \return Auxiliary return value from aux callback
 */
RVVM_PUBLIC int32_t rvvm_char_aux(rvvm_char_dev_t* chardev, uint32_t aux, void* ptr);

/**
 * Request device suspend/resume on other side
 *
 * \param chardev Character device handle
 * \param snap    Snapshot state to serialize into if non-null
 */
RVVM_PUBLIC void rvvm_char_suspend(rvvm_char_dev_t* chardev, rvvm_snapshot_t* snap, bool resume);

/** @}*/

RVVM_EXTERN_C_END

#endif
