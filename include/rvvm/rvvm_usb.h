/*
<rvvm/rvvm_usb.h> - RVVM USB API
Copyright (C) 2020-2026 LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _RVVM_USB_API_H
#define _RVVM_USB_API_H

#include <rvvm/rvvm_base.h>

RVVM_EXTERN_C_BEGIN

/**
 * @defgroup rvvm_usb_dev_api USB Device API
 * @addtogroup rvvm_usb_dev_api
 * @{
 */

/*
 * Endpoint types (bmAttributes bits 1:0)
 */
#define RVVM_USB_EP_CONTROL   0x00 /**< Control     */
#define RVVM_USB_EP_ISOCH     0x01 /**< Isochronous */
#define RVVM_USB_EP_BULK      0x02 /**< Bulk        */
#define RVVM_USB_EP_INTERRUPT 0x03 /**< Interrupt   */

/*
 * Transfer return codes
 *
 * Non-negative values indicate success (Number of bytes transferred)
 */
#define RVVM_USB_XFER_ASYNC   (-1) /**< Async transfer initiated                  */
#define RVVM_USB_XFER_BUSY    (-2) /**< Device busy                               */
#define RVVM_USB_XFER_STALL   (-3) /**< Endpoint stalled or request not supported */
#define RVVM_USB_XFER_OVRUN   (-4) /**< Data overrun                              */
#define RVVM_USB_XFER_ERROR   (-5) /**< Device or transport error                 */

/**
 * Auto-allocated USB port
 */
#define RVVM_USB_PORT_ANY     ((uint32_t)(-1))

/**
 * USB endpoint description
 *
 * Not copied, must be valid during device lifetime or static
 */
typedef struct {
    uint8_t  addr; /**< Endpoint address */
    uint8_t  type; /**< Endpoint type, one of RVVM_USB_EP_* */
    uint16_t size; /**< Maximum packet size */
} rvvm_usb_ep_desc_t;

/**
 * USB device product description
 *
 * Not copied, must be valid during device lifetime or static
 */
typedef struct {
    /**
     * Vendor ID from USB ID database
     */
    uint16_t vendor_id;

    /**
     * Product ID from USB ID database
     */
    uint16_t product_id;

    /**
     * USB device class
     */
    uint8_t class_code;

    /**
     * USB device subclass
     */
    uint8_t subclass;

    /**
     * USB device protocol
     */
    uint8_t protocol;

    /**
     * Manufacturer identifier string
     */
    const char* manufacturer;

    /**
     * Product identifier string
     */
    const char* product;

    /**
     * Serial number string
     */
    const char* serial;

    /**
     * List of endpoint descriptions
     */
    const rvvm_usb_ep_desc_t* ep_list;

    /**
     * Endpoint descriptions count
     */
    size_t ep_size;

} rvvm_usb_dev_prod_t;

/**
 * USB device callbacks
 *
 * Not copied, must be valid during device lifetime or static
 * Callbacks are optional (Nullable)
 */
typedef struct {
    /**
     * Product description
     *
     * Must be valid if control callback is NULL
     */
    const rvvm_usb_dev_prod_t* prod;

    /**
     * Control transfer request on EP0, including standard USB requests
     *
     * If this is NULL, product description is used instead
     *
     * For IN requests:  Return bytes written
     * For OUT requests: Return bytes consumed
     * No data stage:    Return 0 for success
     *
     * \param dev   USB device handle
     * \param setup USB setup packet
     * \param data  Data buffer or NULL for no data stage
     * \param size  Data buffer size (IN) or data length (OUT)
     * \return Bytes transferred (>= 0) or RVVM_USB_XFER_* code
     */
    int32_t (*control)(rvvm_usb_dev_t* dev, const void* setup, void* data, size_t size);

    /**
     * Data transfer request on endpoint (bulk, interrupt, isochronous)
     *
     * For IN endpoint:  Return bytes written
     * For OUT endpoint: Return bytes consumed
     *
     * For asynchronous transfers, device returns RVVM_USB_XFER_ASYNC,
     * and later on calls rvvm_usb_dev_xfer_done() to report completion
     *
     * \param dev  USB device handle
     * \param ep   Endpoint address
     * \param data Data buffer
     * \param size Buffer size (IN) or data length (OUT)
     * \return Bytes transferred (>= 0) or RVVM_USB_XFER_* code
     */
    int32_t (*xfer)(rvvm_usb_dev_t* dev, uint8_t ep, void* data, size_t size);

    /**
     * Cancel pending async transfer on endpoint
     *
     * Called when the controller needs to abort a pending async transfer
     * (guest cancelled a TRB, device being detached, etc)
     *
     * After this returns, no completion for this endpoint should be reported
     *
     * \param dev USB device handle
     * \param ep  Endpoint address (0 for control EP0)
     */
    void (*cancel)(rvvm_usb_dev_t* dev, uint8_t ep);

    /**
     * Handle device reset
     *
     * \param dev USB device handle
     */
    void (*reset)(rvvm_usb_dev_t* dev);

    /**
     * Suspend/resume, serialize snapshot state
     *
     * \param dev    USB device handle
     * \param snap   Snapshot state handle or NULL
     * \param resume Resuming
     */
    void (*suspend)(rvvm_usb_dev_t* dev, rvvm_snapshot_t* snap, bool resume);

    /**
     * Cleanup private data and resources
     *
     * Called on cleanup, device removal or attach failure
     *
     * \param dev USB device handle
     */
    void (*free)(rvvm_usb_dev_t* dev);

} rvvm_usb_dev_type_t;

/**
 * Attach USB device to specific port number
 *
 * On failure, device cleanup is invoked and NULL returned
 * Port 0 is reserved from root hub
 *
 * \param bus  USB bus handle
 * \param type USB device type (Callbacks, VID/PID)
 * \param data USB device private data
 * \param port Desired port number or RVVM_USB_PORT_ANY
 * \return USB device handle or NULL
 */
RVVM_PUBLIC rvvm_usb_dev_t* rvvm_usb_dev_init_at(rvvm_usb_bus_t*            bus,  /**/
                                                 const rvvm_usb_dev_type_t* type, /**/
                                                 void*                      data, /**/
                                                 uint32_t                   port);

/**
 * Attach USB device to any usable port
 *
 * On failure, device cleanup is invoked and NULL returned
 *
 * \param bus  USB bus handle
 * \param type USB device type (Callbacks, VID/PID)
 * \param data USB device private data
 * \return USB device handle or NULL
 */
RVVM_PUBLIC rvvm_usb_dev_t* rvvm_usb_dev_init(rvvm_usb_bus_t*            bus,  /**/
                                              const rvvm_usb_dev_type_t* type, /**/
                                              void*                      data)
{
    return rvvm_usb_dev_init_at(bus, type, data, RVVM_USB_PORT_ANY);
}

/**
 * Free USB device (Hotplug supported)
 *
 * Cancels pending and in-progress transfers
 * Removes device from the bus and invokes cleanup as usual
 *
 * \param dev Device handle
 */
RVVM_PUBLIC void rvvm_usb_dev_free(rvvm_usb_dev_t* dev);

/**
 * Get USB device private data
 *
 * \param dev USB device handle
 * \return Private data
 */
RVVM_PUBLIC void* rvvm_usb_dev_get_data(rvvm_usb_dev_t* dev);

/**
 * Complete an async USB transfer
 *
 * \param dev Device handle
 * \param ep  Endpoint address
 * \param ret Bytes transferred (>= 0) or RVVM_USB_XFER_* code
 */
RVVM_PUBLIC void rvvm_usb_dev_xfer_done(rvvm_usb_dev_t* dev, uint8_t ep, int32_t ret);

/** @}*/

/**
 * @defgroup rvvm_usb_bus_api USB Bus (Host controller) API
 * @addtogroup rvvm_usb_bus_api
 * @{
 */

/**
 * USB controller callbacks
 *
 * Not copied, must be valid during device lifetime or static
 * Callbacks are optional (Nullable)
 */
typedef struct {
    /**
     * Update USB port presence status
     *
     * Controller should generate a port status change event
     * If device is no longer present, all transfers should be aborted
     *
     * \param bus  USB bus handle
     * \param port Port number
     * \param pres Device presence
     */
    void (*port_update)(rvvm_usb_bus_t* bus, uint32_t port, bool pres);

    /**
     * Async USB transfer completed
     *
     * Called from the thread that invoked rvvm_usb_xfer_done()
     * Controller should post a transfer completion event
     *
     * \param bus USB bus handle
     * \param dev USB device handle
     * \param ep  Endpoint address
     * \param ret Bytes transferred (>= 0) or RVVM_USB_XFER_* code
     */
    void (*xfer_done)(rvvm_usb_bus_t* bus, rvvm_usb_dev_t* dev, uint8_t ep, int32_t ret);

} rvvm_usb_bus_cb_t;

/**
 * Create USB bus
 *
 * \param cb   USB controller callbacks
 * \param data USB controller private data
 * \return USB bus handle
 */
RVVM_PUBLIC rvvm_usb_bus_t* rvvm_usb_bus_init(const rvvm_usb_bus_cb_t* cb, void* data);

/**
 * Free USB bus
 *
 * Frees all attached devices, should be called on controller cleanup
 *
 * \param bus USB bus handle
 */
RVVM_PUBLIC void rvvm_usb_bus_free(rvvm_usb_bus_t* bus);

/**
 * Get USB controller private data
 *
 * \param bus USB bus handle
 * \return Private data
 */
RVVM_PUBLIC void* rvvm_usb_bus_get_data(rvvm_usb_bus_t* bus);

/**
 * Get USB device handle by port number
 *
 * \param bus  USB bus handle
 * \param port Port number
 * \return USB device handle or NULL
 */
RVVM_PUBLIC rvvm_usb_dev_t* rvvm_usb_bus_port_dev(rvvm_usb_bus_t* bus, uint32_t port);

/**
 * Request a control transfer from device EP0
 *
 * \param dev   USB device handle
 * \param setup USB setup packet
 * \param data  Data buffer or NULL for no data stage
 * \param size  Buffer size (IN) or data length (OUT)
 * \return Bytes transferred (>= 0) or RVVM_USB_XFER_* code
 */
RVVM_PUBLIC int32_t rvvm_usb_dev_control(rvvm_usb_dev_t* dev,   /**/
                                         const void*     setup, /**/
                                         void*           data,  /**/
                                         size_t          size);

/**
 * Request USB endpoint data transfer
 *
 * \param dev  Device handle
 * \param ep   Endpoint address
 * \param data Data buffer
 * \param size Buffer size (IN) or data length (OUT)
 * \return Bytes transferred (>= 0) or RVVM_USB_XFER_* code
 */
RVVM_PUBLIC int32_t rvvm_usb_dev_xfer(rvvm_usb_dev_t* dev,  /**/
                                      uint8_t         ep,   /**/
                                      void*           data, /**/
                                      size_t          size);

/**
 * Cancel pending async USB transfer on an endpoint
 *
 * After this call returns, the controller will not receive a completion
 *
 * \param dev Device handle
 * \param ep  Endpoint address
 */
RVVM_PUBLIC void rvvm_usb_dev_cancel(rvvm_usb_dev_t* dev, uint8_t ep);

/**
 * Reset USB device
 *
 * \param dev Device handle
 */
RVVM_PUBLIC void rvvm_usb_dev_reset(rvvm_usb_dev_t* dev);

/**
 * Request USB device suspend/resume
 *
 * \param dev    USB device handle
 * \param snap   Snapshot state to serialize into if non-null
 * \param resume Deserialize and resume processing
 */
RVVM_PUBLIC void rvvm_usb_dev_suspend(rvvm_usb_dev_t* dev, rvvm_snapshot_t* snap, bool resume);

/** @}*/

RVVM_EXTERN_C_END

#endif
