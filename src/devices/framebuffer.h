/*
framebuffer.h - Framebuffer context, RGB format handling
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_FRAMEBUFFER_H
#define RVVM_FRAMEBUFFER_H

#include "rvvmlib.h"

/*
 * Pixel format API
 *
 * Pixel formats encode bytes per pixel in lower 4 bits.
 *
 * The remaining bits specify encoding variation,
 * where preferred format has all other bits set to zero.
 *
 * NOTE: Those formats must be implemented in an endian-agnostic way.
 * For example, RGB888 memory layout is {0xBB, 0xGG, 0xRR} aka BGR24
 */

// Special pixel format flags
#define RVVM_RGB_SWAP_R_B    0x10 //!< Swapped red/blue channels
#define RVVM_RGB_BIG_ENDIAN  0x20 //!< Byte-swapped pixel data
#define RVVM_RGB_ALTER_BITS  0x40 //!< Altered bits per channel (10-bit channels, etc)
#define RVVM_RGB_ALPHA_MASK  0x80 //!< Alpha channel present

// Common pixel formats
#define RVVM_RGB_INVALID     0x00 //!< Invalid pixel format
#define RVVM_RGB_RGB565      0x02 //!< Also known as r5g6b5, 16bpp
#define RVVM_RGB_RGB888      0x03 //!< Also known as r8g8b8 / BGR24, 24bpp
#define RVVM_RGB_XRGB8888    0x04 //!< Also known as a8r8g8b8 / BGRA32, 32bpp. Most common.

// Swapped red/blue channel pixel formats, used for web canvases for example
#define RVVM_RGB_BGR888      0x13 //!< Also known as b8g8r8 / RGB24, 24bpp
#define RVVM_RGB_XBGR8888    0x14 //!< Also known as a8b8g8r8 / RGBA32, 32bpp

// Byte-swapped pixel formats (As if naively stored by a big-endian machine)
#define RVVM_RGB_BGRX8888    0x24 //!< Byte-swapped XRGB8888
#define RVVM_RGB_RGBX8888    0x34 //!< Byte-swapped XBGR8888

// Altered bits per channel (Deep color, rgb555, etc)
#define RVVM_RGB_XRGB1555    0x42 //!< Also known as r5g5b5, 15bpp
#define RVVM_RGB_XRGB2101010 0x44 //!< Deep color XRGB with 10-bit color channels
#define RVVM_RGB_XBGR2101010 0x54 //!< Deep color XBGR with 10-bit color channels

//! Pixel RGB format
typedef uint32_t rvvm_rgb_t;

//! Get bytes per pixel for a format
static inline size_t rvvm_rgb_bytes(rvvm_rgb_t format)
{
    return format & 0x0F;
}

//! Get bits per pixel (bpp) for a format
static inline size_t rvvm_rgb_bpp(rvvm_rgb_t format)
{
    return rvvm_rgb_bytes(format) << 3;
}

//! Get preferred pixel format from bpp
static inline rvvm_rgb_t rvvm_rgb_from_bpp(size_t bpp)
{
    return (bpp >> 3) & 0x0F;
}

/*
 * Framebuffer API
 */

//! Framebuffer context description
typedef struct {
    void*      buffer; //!< Buffer in process memory
    uint32_t   width;  //!< Width  (In pixels)
    uint32_t   height; //!< Height (In pixels)
    uint32_t   stride; //!< Byte width of horizontal line, set to 0 if unsure
    rvvm_rgb_t format; //!< Pixel format enum
} rvvm_fb_t;

//! Get frame buffer
static inline void* rvvm_fb_buffer(const rvvm_fb_t* fb)
{
    return fb ? fb->buffer : NULL;
}

//! Get framebuffer width
static inline uint32_t rvvm_fb_width(const rvvm_fb_t* fb)
{
    return fb ? fb->width : 0;
}

//! Get framebuffer height
static inline uint32_t rvvm_fb_height(const rvvm_fb_t* fb)
{
    return fb ? fb->height : 0;
}

//! Get framebuffer pixel format
static inline rvvm_rgb_t rvvm_fb_format(const rvvm_fb_t* fb)
{
    return fb ? fb->format : RVVM_RGB_INVALID;
}

//! Get framebuffer bytes per pixel
static inline rvvm_rgb_t rvvm_fb_rgb_bytes(const rvvm_fb_t* fb)
{
    return rvvm_rgb_bytes(rvvm_fb_format(fb));
}

//! Get framebuffer bits per pixel
static inline rvvm_rgb_t rvvm_fb_rgb_bpp(const rvvm_fb_t* fb)
{
    return rvvm_rgb_bpp(rvvm_fb_format(fb));
}

//! Calculate effective framebuffer stride
static inline size_t rvvm_fb_stride(const rvvm_fb_t* fb)
{
    if (fb && fb->stride) {
        return fb->stride;
    } else if (fb) {
        return fb->width * rvvm_rgb_bytes(fb->format);
    }
    return 0;
}

//! Calculate framebuffer memory region size
static inline size_t rvvm_fb_size(const rvvm_fb_t* fb)
{
    return rvvm_fb_stride(fb) * rvvm_fb_height(fb);
}

//! Check whether resolution of two framebuffers matches
static inline bool rvvm_fb_same_res(const rvvm_fb_t* fb_a, const rvvm_fb_t* fb_b)
{
    return rvvm_fb_width(fb_a) == rvvm_fb_width(fb_b) && rvvm_fb_height(fb_a) == rvvm_fb_height(fb_b);
}

//! Check whether memory layout of two framebuffers matches
static inline bool rvvm_fb_same_layout(const rvvm_fb_t* fb_a, const rvvm_fb_t* fb_b)
{
    return rvvm_fb_same_res(fb_a, fb_b) && fb_a->format == fb_b->format //
        && rvvm_fb_stride(fb_a) == rvvm_fb_stride(fb_b);
}

/*
 * Deprecated definitions without RVVM prefix
 */

#define RGB_FMT_INVALID  RVVM_RGB_INVALID
#define RGB_FMT_R5G6B5   RVVM_RGB_RGB565
#define RGB_FMT_R8G8B8   RVVM_RGB_RGB888
#define RGB_FMT_A8R8G8B8 RVVM_RGB_XRGB8888
#define RGB_FMT_A8B8G8R8 RVVM_RGB_XBGR8888

typedef rvvm_rgb_t rgb_fmt_t;

#define rgb_format_bytes    rvvm_rgb_bytes
#define rgb_format_bpp      rvvm_rgb_bpp
#define rgb_format_from_bpp rvvm_rgb_from_bpp

typedef rvvm_fb_t fb_ctx_t;

#define framebuffer_buffer      rvvm_fb_buffer
#define framebuffer_format      rvvm_fb_format
#define framebuffer_stride      rvvm_fb_stride
#define framebuffer_size        rvvm_fb_size
#define framebuffer_same_res    rvvm_fb_same_res
#define framebuffer_same_layout rvvm_fb_same_layout

/*
 * Simple-framebuffer device
 */

//! \brief   Attach framebuffer context to the machine.
//! \warning The buffer is not freed automatically.
PUBLIC rvvm_mmio_dev_t* framebuffer_init(rvvm_machine_t* machine, rvvm_addr_t addr, const fb_ctx_t* fb);

PUBLIC rvvm_mmio_dev_t* framebuffer_init_auto(rvvm_machine_t* machine, const fb_ctx_t* fb);

#endif
