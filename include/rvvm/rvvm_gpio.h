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
 * GPIO Controller and GPIO User both hold a handle to rvvm_gpio_dev_t,
 * and may set up GPIO callbacks on reading/writing pins by another side.
 *
 * Both may also bi-directionally read/write pins instead of callbacks,
 * which may be more suitable for certain API usecases.
 *
 * Supplying a mask to rvvm_gpio_set_pins() allows updating specific pins.
 *
 * The rvvm_gpio_dev_t context is reference-counted.
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
 * Increment GPIO reference count
 */
RVVM_PUBLIC void rvvm_gpio_inc_ref(rvvm_gpio_dev_t* gpio);

/**
 * Unreference GPIO handle, returns true if it was the last handle
 */
RVVM_PUBLIC bool rvvm_gpio_dec_ref(rvvm_gpio_dev_t* gpio);

/**
 * Set private GPIO user data
 */
RVVM_PUBLIC void rvvm_gpio_set_data(rvvm_gpio_dev_t* gpio, void* data);

/**
 * Get private GPIO user data
 */
RVVM_PUBLIC void* rvvm_gpio_get_data(rvvm_gpio_dev_t* gpio);

/**
 * Register GPIO user callbacks
 */
RVVM_PUBLIC void rvvm_gpio_register_cb(rvvm_gpio_dev_t* gpio, const rvvm_gpio_cb_t* cb);

/**
 * Set GPIO pins inbound to the GPIO controller (With mask)
 */
RVVM_PUBLIC void rvvm_gpio_set_pins(rvvm_gpio_dev_t* gpio, uint32_t off, uint32_t pins, uint32_t mask);

/**
 * Get GPIO pins outbound from the GPIO controller
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

/**
 * @defgroup rvvm_gpio_ctl_api GPIO Controller API
 * @addtogroup rvvm_gpio_ctl_api
 * @{
 */

/**
 * Register GPIO controller callbacks
 */
RVVM_PUBLIC void rvvm_gpio_ctl_register_cb(rvvm_gpio_dev_t* gpio, const rvvm_gpio_cb_t* cb);

/**
 * Set private GPIO controller data
 */
RVVM_PUBLIC void rvvm_gpio_ctl_set_data(rvvm_gpio_dev_t* gpio, void* data);

/**
 * Get private GPIO controller data
 */
RVVM_PUBLIC void* rvvm_gpio_ctl_get_data(rvvm_gpio_dev_t* gpio);

/**
 * Set outbound GPIO controller pins, in case GPIO controller callbacks are not used
 */
RVVM_PUBLIC void rvvm_gpio_ctl_set_pins(rvvm_gpio_dev_t* gpio, uint32_t off, uint32_t pins);

/**
 * Get inbound GPIO controller pins, in case GPIO controller callbacks are not used
 */
RVVM_PUBLIC uint32_t rvvm_gpio_ctl_get_pins(rvvm_gpio_dev_t* gpio, uint32_t off);

/** @}*/

/**
 * @defgroup rvvm_gpio_mux_api GPIO Mux API
 * @addtogroup rvvm_gpio_mux_api
 * @{
 */

/**
 * Create a muxed GPIO splitter to be connected to the upstream GPIO controller
 */
RVVM_PUBLIC rvvm_gpio_dev_t* rvvm_gpio_mux_init(void);

/**
 * Create a muxed GPIO device connected to a GPIO mux
 */
RVVM_PUBLIC rvvm_gpio_dev_t* rvvm_gpio_mux_attach(rvvm_gpio_dev_t* gpio, uint32_t pin_off, uint32_t pin_cnt);

/** @}*/

RVVM_EXTERN_C_END

#endif
