/*
<rvvm/rvvm_fb.h> - Framebuffer, RGB format handling
Copyright (C) 2020-2025  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _RVVM_FRAMEBUFFER_API_H
#define _RVVM_FRAMEBUFFER_API_H

#include <rvvm/rvvm.h>

RVVM_EXTERN_C_BEGIN

/**
 * @defgroup rvvm_rgb_api Pixel format API
 * @addtogroup rvvm_rgb_api
 * @{
 *
 * Pixel formats encode bytes per pixel in lower 4 bits.
 *
 * The remaining bits specify encoding variation,
 * where preferred format has all other bits set to zero.
 *
 * NOTE: Those formats must be implemented in an endian-agnostic way.
 * For example, RGB888 memory layout is {0xBB, 0xGG, 0xRR} aka BGR24
 */

/*
 * Special pixel format flags
 */
#define RVVM_RGB_SWAP_R_B    0x10 /**< Swapped red/blue channels                       */
#define RVVM_RGB_BIG_ENDIAN  0x20 /**< Byte-swapped pixel data                         */
#define RVVM_RGB_ALTER_BITS  0x40 /**< Altered bits per channel (10-bit channels, etc) */
#define RVVM_RGB_ALPHA_MASK  0x80 /**< Alpha channel present                           */

/*
 * Common pixel formats
 */
#define RVVM_RGB_INVALID     0x00 /**< Invalid pixel format                                 */
#define RVVM_RGB_RGB565      0x02 /**< Also known as r5g6b5, 16bpp                          */
#define RVVM_RGB_RGB888      0x03 /**< Also known as r8g8b8 / BGR24, 24bpp                  */
#define RVVM_RGB_XRGB8888    0x04 /**< Also known as a8r8g8b8 / BGRA32, 32bpp. Most common. */

/*
 * Swapped red/blue channel pixel formats, used for web canvases for example
 */
#define RVVM_RGB_BGR888      0x13 /**< Also known as b8g8r8 / RGB24, 24bpp    */
#define RVVM_RGB_XBGR8888    0x14 /**< Also known as a8b8g8r8 / RGBA32, 32bpp */

/*
 * Byte-swapped pixel formats (As if naively stored by a big-endian machine)
 */
#define RVVM_RGB_BGRX8888    0x24 /**< Byte-swapped XRGB8888 */
#define RVVM_RGB_RGBX8888    0x34 /**< Byte-swapped XBGR8888 */

/*
 * Altered bits per channel (Deep color, rgb555, etc)
 */
#define RVVM_RGB_XRGB1555    0x42 /**< Also known as r5g5b5, 15bpp                */
#define RVVM_RGB_XRGB2101010 0x44 /**< Deep color XRGB with 10-bit color channels */
#define RVVM_RGB_XBGR2101010 0x54 /**< Deep color XBGR with 10-bit color channels */

/**
 * RGB Pixel format
 */
typedef uint32_t rvvm_rgb_t;

/**
 * Get bytes per pixel for a pixel format
 */
static inline size_t rvvm_rgb_bytes(rvvm_rgb_t format)
{
    return format & 0x0F;
}

/**
 * Get bits per pixel (bpp) for a format
 */
static inline uint32_t rvvm_rgb_bpp(rvvm_rgb_t format)
{
    return rvvm_rgb_bytes(format) << 3;
}

/**
 * Get preferred pixel format from bpp
 */
static inline rvvm_rgb_t rvvm_rgb_from_bpp(uint32_t bpp)
{
    return (bpp >> 3) & 0x0F;
}

/** @}*/

/**
 * @defgroup rvvm_fb_api Framebuffer context API
 * @addtogroup rvvm_fb_api
 * @{
 */

/**
 * Framebuffer scanout context description
 */
typedef struct {
    void*      buffer; /**< Pixel data pointer                                */
    uint32_t   width;  /**< Width  (In pixels)                                */
    uint32_t   height; /**< Height (In pixels)                                */
    uint32_t   stride; /**< Byte width of horizontal line, set to 0 if unsure */
    rvvm_rgb_t format; /**< Pixel format enum                                 */
} rvvm_fb_t;

/**
 * Get framebuffer pixel data pointer
 */
static inline void* rvvm_fb_buffer(const rvvm_fb_t* fb)
{
    return fb ? fb->buffer : NULL;
}

/**
 * Get framebuffer width
 */
static inline uint32_t rvvm_fb_width(const rvvm_fb_t* fb)
{
    return fb ? fb->width : 0;
}

/**
 * Get framebuffer height
 */
static inline uint32_t rvvm_fb_height(const rvvm_fb_t* fb)
{
    return fb ? fb->height : 0;
}

/**
 * Get framebuffer pixel format
 */
static inline rvvm_rgb_t rvvm_fb_format(const rvvm_fb_t* fb)
{
    return fb ? fb->format : RVVM_RGB_INVALID;
}

/**
 * Get framebuffer bytes per pixel
 */
static inline size_t rvvm_fb_rgb_bytes(const rvvm_fb_t* fb)
{
    return rvvm_rgb_bytes(rvvm_fb_format(fb));
}

/**
 * Get framebuffer bits per pixel
 */
static inline uint32_t rvvm_fb_rgb_bpp(const rvvm_fb_t* fb)
{
    return rvvm_rgb_bpp(rvvm_fb_format(fb));
}

/**
 * Calculate effective framebuffer stride
 */
static inline size_t rvvm_fb_stride(const rvvm_fb_t* fb)
{
    size_t stride = rvvm_fb_width(fb) * rvvm_fb_rgb_bytes(fb);
    return (fb && fb->stride > stride) ? fb->stride : stride;
}

/**
 * Calculate framebuffer memory region size
 */
static inline size_t rvvm_fb_size(const rvvm_fb_t* fb)
{
    return rvvm_fb_stride(fb) * rvvm_fb_height(fb);
}

/**
 * Check whether resolution of two framebuffers matches
 */
static inline bool rvvm_fb_same_res(const rvvm_fb_t* fb_a, const rvvm_fb_t* fb_b)
{
    return rvvm_fb_width(fb_a) == rvvm_fb_width(fb_b) && rvvm_fb_height(fb_a) == rvvm_fb_height(fb_b);
}

/**
 * Check whether memory layout of two framebuffers matches
 */
static inline bool rvvm_fb_same_layout(const rvvm_fb_t* fb_a, const rvvm_fb_t* fb_b)
{
    return rvvm_fb_same_res(fb_a, fb_b) && rvvm_fb_stride(fb_a) == rvvm_fb_stride(fb_b) /**/
        && fb_a->format == fb_b->format;
}

/**
 * Check whether two framebuffers are the same scanout
 */
static inline bool rvvm_fb_same_scanout(const rvvm_fb_t* fb_a, const rvvm_fb_t* fb_b)
{
    return rvvm_fb_same_layout(fb_a, fb_b) && fb_a->buffer == fb_b->buffer;
}

/** @}*/

/**
 * @defgroup rvvm_fbdev_api Framebuffer device API
 * @addtogroup rvvm_fbdev_api
 * @{
 *
 * Video device implementation responsibilities:
 * - Owns a reference to rvvm_fbdev_t* fbdev
 * - Unrefs fbdev handle upon removal
 * - Obtains video RAM via rvvm_fbdev_get_vram()
 * - Sets video mode (Scanout) via rvvm_fbdev_set_scanout(), which must reside in VRAM
 * - Periodically updates the display via rvvm_fbdev_update()
 * - Optionally receives flip completion on flip_done() callback
 * - Optionally marks framebuffer contents as dirty via rvvm_fbdev_dirty()
 *
 * Display implementation responsibilities:
 * - Creates VRAM memory region if needed (Otherwise will be backed by anonymous VMA)
 * - Obtains current video mode (Scanout) via rvvm_fbdev_get_scanout()
 * Then either:
 * - Registers draw/poll callbacks, perform rendering/input polling from them
 * - Registers free() callback and fully deinitialize upon it
 * - Ether draws synchronousy or reports rvvm_fbdev_flip_done()
 * Or:
 * - Increments fbdev refcount via rvvm_fbdev_inc_ref()
 * - Runs from any other thread, polling/drawing from a separate event loop
 * - Optionally checks rvvm_fbdev_is_dirty() to optimize idle redraw
 * - Reports rvvm_fbdev_flip_done()
 */

/**
 * Framebuffer device (2D GPU) handle
 */
typedef struct rvvm_fbdev rvvm_fbdev_t;

/**
 * Framebuffer device callbacks
 */
typedef struct {
    /**
     * Report completed scanout flip
     */
    void (*flip_done)(rvvm_fbdev_t* fbdev);
} rvvm_fb_dev_cb_t;

/**
 * Display implementation callbacks
 */
typedef struct {
    /**
     * Redraw the scanout onto display
     */
    void (*draw)(rvvm_fbdev_t* fbdev);

    /**
     * Optional periodic display input poll
     */
    void (*poll)(rvvm_fbdev_t* fbdev);

    /**
     * Deinitialize & free display private data
     */
    void (*free)(rvvm_fbdev_t* fbdev);
} rvvm_display_cb_t;

/**
 * Create a new fbdev context, return it's handle
 */
RVVM_PUBLIC rvvm_fbdev_t* rvvm_fbdev_init(void);

/**
 * Increment fbdev handle reference count
 */
RVVM_PUBLIC void rvvm_fbdev_inc_ref(rvvm_fbdev_t* fbdev);

/**
 * Unreference fbdev handle, returns true if freed
 */
RVVM_PUBLIC bool rvvm_fbdev_dec_ref(rvvm_fbdev_t* fbdev);

/**
 * Register fbdev callbacks
 */
RVVM_PUBLIC void rvvm_fbdev_register_dev(rvvm_fbdev_t* fbdev, const rvvm_fb_dev_cb_t* cb);

/**
 * Set private fbdev data
 */
RVVM_PUBLIC void rvvm_fbdev_set_dev_data(rvvm_fbdev_t* fbdev, void* data);

/**
 * Get private fbdev data
 */
RVVM_PUBLIC void* rvvm_fbdev_get_dev_data(rvvm_fbdev_t* fbdev);

/**
 * Register display implementation
 */
RVVM_PUBLIC void rvvm_fbdev_register_display(rvvm_fbdev_t* fbdev, const rvvm_display_cb_t* cb);

/**
 * Set private display data
 */
RVVM_PUBLIC void rvvm_fbdev_set_display_data(rvvm_fbdev_t* fbdev, void* data);

/**
 * Get private display data
 */
RVVM_PUBLIC void* rvvm_fbdev_get_display_data(rvvm_fbdev_t* fbdev);

/**
 * Set fbdev video RAM buffer & size. Must be done before passing fbdev to the GPU device.
 */
RVVM_PUBLIC bool rvvm_fbdev_set_vram(rvvm_fbdev_t* fbdev, void* vram, size_t size);

/**
 * Get fbdev video RAM buffer & size
 */
RVVM_PUBLIC void* rvvm_fbdev_get_vram(rvvm_fbdev_t* fbdev, size_t* size);

/**
 * Set fbdev video mode (scanout) atomically, must reside in video RAM
 */
RVVM_PUBLIC bool rvvm_fbdev_set_scanout(rvvm_fbdev_t* fbdev, const rvvm_fb_t* fb);

/**
 * Get current fbdev video mode atomically
 */
RVVM_PUBLIC bool rvvm_fbdev_get_scanout(rvvm_fbdev_t* fbdev, rvvm_fb_t* fb);

/**
 * Mark framebuffer as dirty from device side
 */
RVVM_PUBLIC void rvvm_fbdev_dirty(rvvm_fbdev_t* fbdev);

/**
 * Check whether framebuffer is dirty
 */
RVVM_PUBLIC bool rvvm_fbdev_is_dirty(rvvm_fbdev_t* fbdev);

/**
 * Perform display update from device side
 */
RVVM_PUBLIC void rvvm_fbdev_update(rvvm_fbdev_t* fbdev);

/**
 * Acknowledge a scanout flip from display side
 */
RVVM_PUBLIC void rvvm_fbdev_flip_done(rvvm_fbdev_t* fbdev);

/** @}*/

RVVM_EXTERN_C_END

#endif
