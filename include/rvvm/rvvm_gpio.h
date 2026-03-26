/*
<rvvm/rvvm_gpio.h> - RVVM GPIO API
Copyright (C) 2020-2026  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _RVVM_GPIO_API_H
#define _RVVM_GPIO_API_H

#include <rvvm/rvvm_base.h>

RVVM_EXTERN_C_BEGIN

/**
 * @defgroup rvvm_gpio_api GPIO API
 * @addtogroup rvvm_gpio_api
 * @{
 *
 * GPIO controller and GPIO devices hold a handle to rvvm_gpio_dev_t,
 * and may set up GPIO callbacks on reading/writing their pins
 *
 * Each end should be explicitly muxed, multiple GPIO devices may be
 * muxed to a subset of pins of other GPIO handle, and receive a close
 * callback when the other end frees its GPIO handle
 *
 * Each end may always read/write pins instead of callbacks, which may be
 * more suitable for certain API usecases, pin data is stored internally
 *
 * Callbacks are internally synchronized between multiple threads
 *
 * Reading / writing GPIO pins is done in bulk via u32 bit mask, which
 * is called a GPIO pin group, and indexed in read / write operations
 *
 * Supplying a mask to rvvm_gpio_set_pins() allows updating specific pins
 *
 * The close callback should be handled if the device is considered to be owned
 * by the other side (GPIO controller, etc) to free its private data / handle
 */

/**
 * GPIO callbacks
 *
 * Must be valid during GPIO device lifetime, or static
 * Callbacks are optional (Nullable)
 */
typedef struct {
    /**
     * Read GPIO pins group
     */
    uint32_t (*read)(rvvm_gpio_dev_t* gpio, size_t idx);

    /**
     * Write GPIO pins group
     */
    void (*write)(rvvm_gpio_dev_t* gpio, size_t idx, uint32_t pins);

    /**
     * Close callback
     *
     * Should be handled if this device is owned by other side,
     * and may be ignored if current device is owned elsewhere
     */
    void (*close)(rvvm_gpio_dev_t* gpio);

} rvvm_gpio_cb_t;

/**
 * Create a new GPIO device handle
 *
 * \param cb   GPIO device callbacks
 * \param data GPIO device private data
 */
RVVM_PUBLIC rvvm_gpio_dev_t* rvvm_gpio_init(const rvvm_gpio_cb_t* cb, void* data);

/**
 * Free GPIO device handle
 *
 * Calls close callback on both sides, unpairing devices beforehand
 *
 * May be safely called recursively from close callback, it is simply ignored
 *
 * \param gpio GPIO device handle
 */
RVVM_PUBLIC void rvvm_gpio_free(rvvm_gpio_dev_t* gpio);

/**
 * Attach GPIO device to a parent, muxing the GPIO bus
 *
 * Previous connection established at child handle is torn down
 * If parent is NULL, child device is unconnected from its parent
 * Overlapping pins in multiple child devices is disallowed
 *
 * \param parent Parent GPIO device handle
 * \param child  Child GPIO device handle
 * \param offset Parent pins offset
 * \param count  Parent pins count
 */
RVVM_PUBLIC void rvvm_gpio_attach(rvvm_gpio_dev_t* parent, /**/
                                  rvvm_gpio_dev_t* child,  /**/
                                  size_t           offset, /**/
                                  size_t           count);

/**
 * Detach GPIO device from a parent
 *
 * \param gpio GPIO device handle
 */
static inline void rvvm_gpio_detach(rvvm_gpio_dev_t* gpio)
{
    rvvm_gpio_attach(NULL, gpio, 0, 0);
}

/**
 * Get private GPIO device data
 *
 * \param gpio GPIO device handle
 * \return GPIO device private data
 */
RVVM_PUBLIC void* rvvm_gpio_get_data(rvvm_gpio_dev_t* gpio);

/**
 * Set GPIO pins outbound from this device
 *
 * \param gpio GPIO device handle
 * \param idx  Pin group index
 * \param pins Pin group bits
 * \param mask Pin group mask
 */
RVVM_PUBLIC void rvvm_gpio_set_pins(rvvm_gpio_dev_t* gpio, size_t idx, uint32_t pins, uint32_t mask);

/**
 * Get GPIO pins inbound to this device
 *
 * \param gpio GPIO device handle
 * \param idx  Pin group index
 * \return Pin group bits
 */
RVVM_PUBLIC uint32_t rvvm_gpio_get_pins(rvvm_gpio_dev_t* gpio, size_t idx);

/**
 * Set a single GPIO pin value
 *
 * \param gpio GPIO device handle
 * \param pin  Pin index
 * \param val  Pin value
 */
static inline void rvvm_gpio_set_pin(rvvm_gpio_dev_t* gpio, size_t pin, bool val)
{
    uint32_t mask = (1U << (pin & 0x1F));
    rvvm_gpio_set_pins(gpio, pin >> 5, val ? mask : 0, mask);
}

/**
 * Get a single GPIO pin value
 *
 * \param gpio GPIO device handle
 * \param pin  Pin index
 * \return Pin value
 */
static inline bool rvvm_gpio_get_pin(rvvm_gpio_dev_t* gpio, size_t pin)
{
    uint32_t pins = rvvm_gpio_get_pins(gpio, pin >> 5);
    return pins & (1U << (pin & 0x1F));
}

/** @}*/

RVVM_EXTERN_C_END

#endif
