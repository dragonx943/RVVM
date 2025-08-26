/*
framebuffer.c - Simple Framebuffer
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "framebuffer.h"
#include "fdtlib.h"
#include "utils.h"

PUSH_OPTIMIZATION_SIZE

static void fb_remove(rvvm_mmio_dev_t* dev)
{
    UNUSED(dev);
}

static rvvm_mmio_type_t fb_dev_type = {
    .name   = "framebuffer",
    .remove = fb_remove,
};

PUBLIC rvvm_mmio_dev_t* framebuffer_init(rvvm_machine_t* machine, rvvm_addr_t addr, const fb_ctx_t* fb)
{
    // Map the framebuffer into physical memory
    rvvm_mmio_dev_t fb_mmio = {
        .addr    = addr,
        .size    = framebuffer_size(fb),
        .mapping = fb->buffer,
        .type    = &fb_dev_type,
    };

    rvvm_mmio_dev_t* mmio = rvvm_attach_mmio(machine, &fb_mmio);
    if (mmio == NULL) {
        return mmio;
    }

#ifdef USE_FDT
    struct fdt_node* fb_fdt = fdt_node_create_reg("framebuffer", fb_mmio.addr);
    fdt_node_add_prop_reg(fb_fdt, "reg", fb_mmio.addr, fb_mmio.size);
    fdt_node_add_prop_str(fb_fdt, "compatible", "simple-framebuffer");
    switch (fb->format) {
        case RGB_FMT_R5G6B5:
            fdt_node_add_prop_str(fb_fdt, "format", "r5g6b5");
            break;
        case RGB_FMT_R8G8B8:
            fdt_node_add_prop_str(fb_fdt, "format", "r8g8b8");
            break;
        case RGB_FMT_A8R8G8B8:
            fdt_node_add_prop_str(fb_fdt, "format", "a8r8g8b8");
            break;
        case RGB_FMT_A8B8G8R8:
            fdt_node_add_prop_str(fb_fdt, "format", "a8b8g8r8");
            break;
        default:
            rvvm_warn("Unknown RGB format in framebuffer_init()!");
            break;
    }
    fdt_node_add_prop_u32(fb_fdt, "width", fb->width);
    fdt_node_add_prop_u32(fb_fdt, "height", fb->height);
    fdt_node_add_prop_u32(fb_fdt, "stride", framebuffer_stride(fb));

    fdt_node_add_child(rvvm_get_fdt_soc(machine), fb_fdt);
#endif

    return mmio;
}

PUBLIC rvvm_mmio_dev_t* framebuffer_init_auto(rvvm_machine_t* machine, const fb_ctx_t* fb)
{
    rvvm_addr_t      addr = rvvm_mmio_zone_auto(machine, 0x18000000, framebuffer_size(fb));
    rvvm_mmio_dev_t* mmio = framebuffer_init(machine, addr, fb);
    if (mmio) {
        rvvm_append_cmdline(machine, "console=tty0");
    }
    return mmio;
}

POP_OPTIMIZATION_SIZE
