/*
<rvvm/rvvm_char.h> - Character Device (Serial) API
Copyright (C) 2020-2025  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _RVVM_CHARDEV_API_H
#define _RVVM_CHARDEV_API_H

#include <rvvm/rvvm.h>

RVVM_EXTERN_C_BEGIN

/**
 * @defgroup rvvm_char_api Character Device API
 * @addtogroup rvvm_char_api
 * @{
 *
 * Serial controller and character device both hold a handle to rvvm_char_dev_t,
 * and may set up callbacks on reading/writing serial data by another side.
 *
 * Both may also bi-directionally read/write serial data instead of callbacks,
 * which may be more suitable for certain API usecases.
 *
 * The rvvm_char_dev_t context contains an RX/TX FIFO, akin to a pipe buffer.
 *
 * The rvvm_char_dev_t context is reference-counted.
 */

/**
 * Serial data availability flags
 */
#define RVVM_CHAR_RX 0x01 /** Serial data receive available  */
#define RVVM_CHAR_TX 0x02 /** Serial data transmit available */

/**
 * Character device handle
 */
typedef struct rvvm_char_dev rvvm_char_dev_t;

/**
 * Character device callbacks
 */
typedef struct {
    void   (*free)(rvvm_char_dev_t* chardev);
    size_t (*read)(rvvm_char_dev_t* chardev, void* buffer, size_t size);
    size_t (*write)(rvvm_char_dev_t* chardev, const void* buffer, size_t size);
} rvvm_char_cb_t;

/**
 * Create a new character device pair (Akin to a pipe)
 */
RVVM_PUBLIC bool rvvm_char_pair_init(rvvm_char_dev_t** pair);

/**
 * Increment character device reference count
 */
RVVM_PUBLIC void rvvm_char_inc_ref(rvvm_char_dev_t* chardev);

/**
 * Unreference character device handle, returns true if it was the last handle
 */
RVVM_PUBLIC bool rvvm_char_dec_ref(rvvm_char_dev_t* chardev);

/**
 * Set character device internal FIFO size
 */
RVVM_PUBLIC void rvvm_char_set_fifo_size(rvvm_char_dev_t* chardev, size_t size);

/**
 * Set private character device data
 */
RVVM_PUBLIC void rvvm_char_set_data(rvvm_char_dev_t* chardev, void* data);

/**
 * Get private character device data
 */
RVVM_PUBLIC void* rvvm_char_get_data(rvvm_char_dev_t* chardev);

/**
 * Register character device callbacks
 */
RVVM_PUBLIC void rvvm_char_register_cb(rvvm_char_dev_t* chardev, const rvvm_char_cb_t* cb);

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

/** @}*/

RVVM_EXTERN_C_END

#endif
