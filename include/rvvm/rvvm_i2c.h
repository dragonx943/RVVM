/*
<rvvm/rvvm_i2c.h> - I2C Bus API
Copyright (C) 2020-2026 LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _RVVM_I2C_API_H
#define _RVVM_I2C_API_H

#include <rvvm/rvvm_char.h>

RVVM_EXTERN_C_BEGIN

/**
 * @defgroup rvvm_i2c_api I2C Bus API
 * @addtogroup rvvm_i2c_api
 * @{
 */

/*
 * Auxiliary I2C commands
 */
#define RVVM_CHAR_AUX_I2C_START 0x1000 /**< Start I2C, points to bool write */
#define RVVM_CHAR_AUX_I2C_STOP  0x1001 /**< Stop I2C                        */

/**
 * Auto-allocated bus address
 */
#define RVVM_I2C_ADDR_ANY       0

/**
 * I2C controller implementation callbacks
 *
 * Must be valid during I2C bus lifetime, or static
 */
typedef struct {
    /**
     * Attach character device to I2C bus
     *
     * Internally, I2C controller should create it's own end of pipe
     * and pair it with external character device handle
     */
    uint16_t (*attach)(rvvm_i2c_bus_t* bus, rvvm_char_dev_t* dev, uint16_t addr);

    /**
     * Get I2C controller FDT node
     */
    rvvm_fdt_node_t* (*get_fdt)(rvvm_i2c_bus_t* bus);

} rvvm_i2c_bus_cb_t;

/**
 * Create I2C bus backed by I2C controller
 *
 * \param cb   I2C controller callbacks
 * \param data I2C controller private data
 * \return I2C bus handle
 */
RVVM_PUBLIC rvvm_i2c_bus_t* rvvm_i2c_bus_init(const rvvm_i2c_bus_cb_t* cb, void* data);

/**
 * Get I2C controller FDT node
 *
 * \param bus I2C bus handle
 * \return FDT node handle or NULL
 */
RVVM_PUBLIC rvvm_fdt_node_t* rvvm_i2c_bus_get_fdt(rvvm_i2c_bus_t* bus);

/**
 * Attach character device to I2C bus at specific address
 *
 * Character device receives read/write callbacks, and auxiliary callbacks which report I2C start/stop events
 *
 * \param bus  I2C bus handle
 * \param dev  Character device handle treated as I2C device
 * \param addr I2C address to attach at or RVVM_I2C_ADDR_ANY
 * \return I2C address at which device is attached, or zero on failure
 */
RVVM_PUBLIC uint16_t rvvm_i2c_bus_attach_at(rvvm_i2c_bus_t* bus, rvvm_char_dev_t* dev, uint16_t addr);

/**
 * Attach character device to I2C bus
 *
 * Character device receives read/write callbacks, and auxiliary callbacks which report I2C start/stop events
 *
 * \param bus  I2C bus handle
 * \param dev  Character device handle treated as I2C device
 * \return I2C address at which device is attached, or zero on failure
 */
static inline uint16_t rvvm_i2c_bus_attach(rvvm_i2c_bus_t* bus, rvvm_char_dev_t* dev)
{
    return rvvm_i2c_bus_attach_at(bus, dev, RVVM_I2C_ADDR_ANY);
}

/**
 * Report I2C start condition from controller side
 *
 * Repeated start is supported
 *
 * \param bus_dev I2C controller end of character device pipe
 * \param write   Whether the transfer is read or write
 */
static inline void rvvm_i2c_bus_start(rvvm_char_dev_t* bus_dev, bool write)
{
    rvvm_char_aux(bus_dev, RVVM_CHAR_AUX_I2C_START, &write);
}

/**
 * Report I2C stop condition from controller side
 *
 * \param bus_dev I2C controller end of character device pipe
 */
static inline void rvvm_i2c_bus_stop(rvvm_char_dev_t* bus_dev)
{
    rvvm_char_aux(bus_dev, RVVM_CHAR_AUX_I2C_STOP, NULL);
}

/**
 * Read I2C data from controller side
 *
 * \param bus_dev I2C controller end of character device pipe
 * \param data    Data buffer
 * \param size    Data size
 * \return Amount of bytes actually read
 */
static inline size_t rvvm_i2c_bus_read(rvvm_char_dev_t* bus_dev, void* data, size_t size)
{
    return rvvm_char_read(bus_dev, data, size);
}

/**
 * Write I2C data from controller side
 *
 * \param bus_dev I2C controller end of character device pipe
 * \param data    Data buffer
 * \param size    Data size
 * \return Amount of bytes actually written
 */
static inline size_t rvvm_i2c_bus_write(rvvm_char_dev_t* bus_dev, const void* data, size_t size)
{
    return rvvm_char_write(bus_dev, data, size);
}

/** @}*/

RVVM_EXTERN_C_END

#endif
