/*
framebuffer.c - Simple Framebuffer
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "framebuffer.h"
#include "compiler.h"
#include "fdtlib.h"

PUSH_OPTIMIZATION_SIZE

static bool simplefb_write(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    rvvm_fbdev_t* fbdev = dev->data;
    rvvm_fbdev_dirty(fbdev);
    UNUSED(data);
    UNUSED(offset);
    UNUSED(size);
    return true;
}

static void simplefb_remove(rvvm_mmio_dev_t* dev)
{
    rvvm_fbdev_t* fbdev = dev->data;
    rvvm_fbdev_dec_ref(fbdev);
}

static void simplefb_update(rvvm_mmio_dev_t* dev)
{
    rvvm_fbdev_t* fbdev = dev->data;
    rvvm_fbdev_update(fbdev);
}

static rvvm_mmio_type_t simplefb_dev_type = {
    .name   = "simple-framebuffer",
    .remove = simplefb_remove,
    .update = simplefb_update,
};

PUBLIC rvvm_mmio_dev_t* rvvm_simplefb_init(rvvm_machine_t* machine, rvvm_addr_t addr, rvvm_fbdev_t* fbdev)
{
    if (!fbdev) {
        return NULL;
    }

    rvvm_fb_t fb = ZERO_INIT;
    rvvm_fbdev_get_scanout(fbdev, &fb);

    // Map the framebuffer into physical memory
    rvvm_mmio_dev_t simplefb_mmio = {
        .addr    = addr,
        .size    = rvvm_fb_size(&fb),
        .data    = fbdev,
        .mapping = rvvm_fb_buffer(&fb),
        .type    = &simplefb_dev_type,
        .write   = simplefb_write,
    };

    rvvm_mmio_dev_t* mmio = rvvm_attach_mmio(machine, &simplefb_mmio);
    if (mmio == NULL) {
        return mmio;
    }

#if defined(USE_FDT)
    struct fdt_node* fb_fdt = fdt_node_create_reg("framebuffer", simplefb_mmio.addr);
    fdt_node_add_prop_reg(fb_fdt, "reg", simplefb_mmio.addr, simplefb_mmio.size);
    fdt_node_add_prop_str(fb_fdt, "compatible", "simple-framebuffer");
    switch (rvvm_fb_format(&fb)) {
        case RVVM_RGB_RGB565:
            fdt_node_add_prop_str(fb_fdt, "format", "r5g6b5");
            break;
        case RVVM_RGB_RGB888:
            fdt_node_add_prop_str(fb_fdt, "format", "r8g8b8");
            break;
        case RVVM_RGB_BGR888:
            fdt_node_add_prop_str(fb_fdt, "format", "b8g8r8");
            break;
        case RVVM_RGB_XRGB8888:
            fdt_node_add_prop_str(fb_fdt, "format", "a8r8g8b8");
            break;
        case RVVM_RGB_XBGR8888:
            fdt_node_add_prop_str(fb_fdt, "format", "a8b8g8r8");
            break;
    }
    fdt_node_add_prop_u32(fb_fdt, "width", rvvm_fb_width(&fb));
    fdt_node_add_prop_u32(fb_fdt, "height", rvvm_fb_height(&fb));
    fdt_node_add_prop_u32(fb_fdt, "stride", rvvm_fb_stride(&fb));

    fdt_node_add_child(rvvm_get_fdt_soc(machine), fb_fdt);
#endif

    return mmio;
}

RVVM_PUBLIC rvvm_mmio_dev_t* rvvm_simplefb_init_auto(rvvm_machine_t* machine, rvvm_fbdev_t* fbdev)
{
    rvvm_fb_t fb = ZERO_INIT;
    rvvm_fbdev_get_scanout(fbdev, &fb);
    rvvm_addr_t      addr = rvvm_mmio_zone_auto(machine, 0x18000000, rvvm_fb_size(&fb));
    rvvm_mmio_dev_t* mmio = rvvm_simplefb_init(machine, addr, fbdev);
    if (mmio) {
        rvvm_append_cmdline(machine, "console=tty0");
    }
    return mmio;
}

POP_OPTIMIZATION_SIZE
