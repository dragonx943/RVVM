/*
framebuffer.h - Framebuffer context, RGB format handling
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_FRAMEBUFFER_H
#define RVVM_FRAMEBUFFER_H

#include <rvvm/rvvm_fb.h>

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
RVVM_PUBLIC rvvm_mmio_dev_t* framebuffer_init(rvvm_machine_t* machine, rvvm_addr_t addr, const fb_ctx_t* fb);

RVVM_PUBLIC rvvm_mmio_dev_t* framebuffer_init_auto(rvvm_machine_t* machine, const fb_ctx_t* fb);

#endif
