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
 * Pixel formats encode bytes per pixel in lower 4 bits.
 *
 * The remaining bits specify pixel encoding variation,
 * where preferred format has all other bits set to zero.
 */

#define RGB_FMT_INVALID  0x00
#define RGB_FMT_R5G6B5   0x02
#define RGB_FMT_R8G8B8   0x03
#define RGB_FMT_A8R8G8B8 0x04 //!< Little-endian: BGRA, Big-endian: ARGB (Recommended)
#define RGB_FMT_A8B8G8R8 0x14 //!< Little-endian: RGBA, Big-endian: ABGR

//! Pixel RGB format
typedef uint8_t rgb_fmt_t;

//! Get bytes per pixel for a format
static inline size_t rgb_format_bytes(rgb_fmt_t format)
{
    return format & 0x0F;
}

//! Get bits per pixel (bpp) for a format
static inline size_t rgb_format_bpp(rgb_fmt_t format)
{
    return rgb_format_bytes(format) << 3;
}

//! Get pixel format from bpp
static inline rgb_fmt_t rgb_format_from_bpp(size_t bpp)
{
    return (bpp >> 3) & 0x0F;
}

/*
 * Framebuffer API
 */

//! Framebuffer context description
typedef struct {
    void*     buffer; //!< Buffer in process memory
    uint32_t  width;  //!< Width  (In pixels)
    uint32_t  height; //!< Height (In pixels)
    uint32_t  stride; //!< Line alignment. Set to 0 if unsure.
    rgb_fmt_t format; //!< Pixel format enum
} fb_ctx_t;

//! Get frame buffer
static inline void* framebuffer_buffer(const fb_ctx_t* fb)
{
    return fb ? fb->buffer : NULL;
}

//! Get framebuffer pixel format
static inline rgb_fmt_t framebuffer_format(const fb_ctx_t* fb)
{
    return fb ? fb->format : RGB_FMT_INVALID;
}

//! Calculate effective framebuffer stride
static inline size_t framebuffer_stride(const fb_ctx_t* fb)
{
    if (fb && fb->stride) {
        return fb->stride;
    } else if (fb) {
        return fb->width * rgb_format_bytes(fb->format);
    }
    return 0;
}

//! Calculate framebuffer region size
static inline size_t framebuffer_size(const fb_ctx_t* fb)
{
    if (fb) {
        return framebuffer_stride(fb) * fb->height;
    }
    return 0;
}

/*
 * Simple-framebuffer device
 */

//! \brief   Attach framebuffer context to the machine.
//! \warning The buffer is not freed automatically.
PUBLIC rvvm_mmio_dev_t* framebuffer_init(rvvm_machine_t* machine, rvvm_addr_t addr, const fb_ctx_t* fb);

PUBLIC rvvm_mmio_dev_t* framebuffer_init_auto(rvvm_machine_t* machine, const fb_ctx_t* fb);

#endif
