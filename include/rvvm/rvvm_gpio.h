/*
<rvvm/rvvm_gpio.h> - General-Purpose IO API
Copyright (C) 2020-2025  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _RVVM_GPIO_API_H
#define _RVVM_GPIO_API_H

#include <rvvm/rvvm.h>

RVVM_EXTERN_C_BEGIN

/**
 * @defgroup rvvm_gpio_api GPIO API
 * @addtogroup rvvm_gpio_api
 * @{
 *
 * GPIO controller and GPIO devices hold a handle to rvvm_gpio_dev_t,
 * and may set up GPIO callbacks on reading/writing their pins.
 *
 * They may also bi-directionally read/write pins instead of callbacks,
 * which may be more suitable for certain API usecases.
 *
 * GPIO handles can be muxed to a subset of pins of the GPIO group.
 *
 * Supplying a mask to rvvm_gpio_set_pins() allows updating specific pins.
 *
 * The rvvm_gpio_dev_t context is reference-counted.
 * Dropping a GPIO handle unreferences all muxed handles.
 */

/**
 * GPIO device handle
 */
typedef struct rvvm_gpio_dev rvvm_gpio_dev_t;

/**
 * GPIO callbacks
 *
 * May be set up by both GPIO controller / GPIO outer connection sides
 */
typedef struct {
    void     (*free)(rvvm_gpio_dev_t* gpio);
    uint32_t (*get_pins)(rvvm_gpio_dev_t* gpio, uint32_t off);
    void     (*set_pins)(rvvm_gpio_dev_t* gpio, uint32_t off, uint32_t pins);
} rvvm_gpio_cb_t;

/**
 * Create a new GPIO device context, return it's handle
 */
RVVM_PUBLIC rvvm_gpio_dev_t* rvvm_gpio_init(void);

/**
 * Attach GPIO handle to a GPIO group, muxing the GPIO bus
 */
RVVM_PUBLIC bool rvvm_gpio_attach(rvvm_gpio_dev_t* mux, rvvm_gpio_dev_t* gpio, //
                                  uint32_t pin_off, uint32_t pin_cnt);

/**
 * Increment GPIO handle reference count
 */
RVVM_PUBLIC void rvvm_gpio_inc_ref(rvvm_gpio_dev_t* gpio);

/**
 * Unreference GPIO handle, returns true if it was the last handle
 */
RVVM_PUBLIC bool rvvm_gpio_dec_ref(rvvm_gpio_dev_t* gpio);

/**
 * Set private GPIO device data
 */
RVVM_PUBLIC void rvvm_gpio_set_data(rvvm_gpio_dev_t* gpio, void* data);

/**
 * Get private GPIO device data
 */
RVVM_PUBLIC void* rvvm_gpio_get_data(rvvm_gpio_dev_t* gpio);

/**
 * Register GPIO device callbacks
 */
RVVM_PUBLIC void rvvm_gpio_register_cb(rvvm_gpio_dev_t* gpio, const rvvm_gpio_cb_t* cb);

/**
 * Set GPIO pins outbound from this device (With mask)
 */
RVVM_PUBLIC void rvvm_gpio_set_pins(rvvm_gpio_dev_t* gpio, uint32_t off, uint32_t pins, uint32_t mask);

/**
 * Get GPIO pins inbound to this device
 */
RVVM_PUBLIC uint32_t rvvm_gpio_get_pins(rvvm_gpio_dev_t* gpio, uint32_t off);

/**
 * Set a single GPIO pin value
 */
static inline void rvvm_gpio_set_pin(rvvm_gpio_dev_t* gpio, uint32_t pin, bool val)
{
    uint32_t mask = (1U << (pin & 0x1FU));
    rvvm_gpio_set_pins(gpio, pin >> 5, val ? mask : 0, mask);
}

/**
 * Get a single GPIO pin value
 */
static inline bool rvvm_gpio_get_pin(rvvm_gpio_dev_t* gpio, uint32_t pin)
{
    uint32_t pins = rvvm_gpio_get_pins(gpio, pin >> 5);
    return pins & (1U << (pin & 0x1FU));
}

/** @}*/

RVVM_EXTERN_C_END

#endif
