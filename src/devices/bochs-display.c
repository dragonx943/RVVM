/*
bochs-display.c - Bochs Display
Copyright (C) 2025  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "bochs-display.h"

#include "mem_ops.h"
#include "utils.h"
#include "vma_ops.h"

PUSH_OPTIMIZATION_SIZE

typedef struct {
    rvvm_fbdev_t* fbdev;

    uint32_t version;
    uint32_t enable;
    uint32_t xres;
    uint32_t yres;
    uint32_t xvirt;
    uint32_t yvirt;
    uint32_t xoff;
    uint32_t yoff;
} bochs_display_t;

// Bochs display registers
#define BOCHS_DISPI_ID          0x0500
#define BOCHS_DISPI_XRES        0x0502
#define BOCHS_DISPI_YRES        0x0504
#define BOCHS_DISPI_BPP         0x0506
#define BOCHS_DISPI_ENABLE      0x0508
#define BOCHS_DISPI_BANK        0x050A
#define BOCHS_DISPI_VIRT_WIDTH  0x050C // Scanout stride
#define BOCHS_DISPI_VIRT_HEIGHT 0x050E // Not useful
#define BOCHS_DISPI_X_OFFSET    0x0510
#define BOCHS_DISPI_Y_OFFSET    0x0512
#define BOCHS_DISPI_VRAM        0x0514

// Bochs VBE verions
#define BOCHS_VER_ID0           0xB0C0 // Bochs VBE version 0
#define BOCHS_VER_ID5           0xB0C5 // Bochs VBE version 5

// Enable register bits
#define BOCHS_ENABLE            0x01
#define BOCHS_CAPS              0x02
#define BOCHS_8BIT              0x20
#define BOCHS_LFB               0x40
#define BOCHS_NOCLR             0x80
#define BOCHS_ENABLE_MASK       0xE3

static void bochs_display_update_mode(bochs_display_t* disp, bool upd_res)
{
    rvvm_fb_t fb   = ZERO_INIT;
    uint8_t*  vram = rvvm_fbdev_get_vram(disp->fbdev, NULL);
    uint32_t  off  = atomic_load_uint32_relax(&disp->xoff);
    rvvm_fbdev_get_scanout(disp->fbdev, &fb);
    if (upd_res) {
        fb.width  = atomic_load_uint32_relax(&disp->xres);
        fb.height = atomic_load_uint32_relax(&disp->yres);
    }
    fb.stride = EVAL_MAX(atomic_load_uint32_relax(&disp->xvirt), fb.width * 4);
    fb.format = RVVM_RGB_XRGB8888;
    fb.buffer = vram + off + (atomic_load_uint32_relax(&disp->yoff) * fb.stride);
    rvvm_fbdev_set_scanout(disp->fbdev, &fb);
}

static bool bochs_display_read(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    bochs_display_t* disp = dev->data;
    uint16_t         val  = 0;
    UNUSED(size);

    switch (offset) {
        case BOCHS_DISPI_ID:
            val = atomic_load_uint32_relax(&disp->version);
            if (val < BOCHS_VER_ID0 || val > BOCHS_VER_ID5) {
                val = BOCHS_VER_ID5;
            }
            break;
        case BOCHS_DISPI_XRES:
            if (atomic_load_uint32_relax(&disp->enable) & BOCHS_CAPS) {
                val = 2560;
            } else {
                val = atomic_load_uint32_relax(&disp->xres);
            }
            break;
        case BOCHS_DISPI_YRES:
            if (atomic_load_uint32_relax(&disp->enable) & BOCHS_CAPS) {
                val = 1440;
            } else {
                val = atomic_load_uint32_relax(&disp->yres);
            }
            break;
        case BOCHS_DISPI_BPP:
            val = 32;
            break;
        case BOCHS_DISPI_ENABLE:
            val = atomic_load_uint32_relax(&disp->enable);
            break;
        case BOCHS_DISPI_VIRT_WIDTH:
            val = atomic_load_uint32_relax(&disp->xvirt);
            break;
        case BOCHS_DISPI_VIRT_HEIGHT:
            val = atomic_load_uint32_relax(&disp->yvirt);
            break;
        case BOCHS_DISPI_X_OFFSET:
            val = atomic_load_uint32_relax(&disp->xoff);
            break;
        case BOCHS_DISPI_Y_OFFSET:
            val = atomic_load_uint32_relax(&disp->yoff);
            break;
        case BOCHS_DISPI_VRAM: {
            size_t vram_size = RVVM_BOCHS_DISPLAY_VRAM;
            rvvm_fbdev_get_vram(disp->fbdev, &vram_size);
            val = vram_size >> 16;
            break;
        }
    }

    write_uint16_le(data, val);
    return true;
}

static bool bochs_display_write(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    bochs_display_t* disp = dev->data;
    uint16_t         val  = read_uint16_le(data);
    UNUSED(size);

    switch (offset) {
        case BOCHS_DISPI_ID:
            atomic_store_uint32_relax(&disp->version, val);
            break;
        case BOCHS_DISPI_XRES:
            if (!(atomic_load_uint32_relax(&disp->enable) & BOCHS_ENABLE_MASK)) {
                atomic_store_uint32_relax(&disp->xres, val);
            }
            break;
        case BOCHS_DISPI_YRES:
            if (!(atomic_load_uint32_relax(&disp->enable) & BOCHS_ENABLE_MASK)) {
                atomic_store_uint32_relax(&disp->yres, val);
            }
            break;
        case BOCHS_DISPI_VIRT_WIDTH:
            atomic_store_uint32_relax(&disp->xvirt, val);
            bochs_display_update_mode(disp, false);
            break;
        case BOCHS_DISPI_VIRT_HEIGHT:
            // NOTE: This has no effect
            atomic_store_uint32_relax(&disp->yvirt, val);
            break;
        case BOCHS_DISPI_X_OFFSET:
            atomic_store_uint32_relax(&disp->xoff, val);
            bochs_display_update_mode(disp, false);
            break;
        case BOCHS_DISPI_Y_OFFSET:
            atomic_store_uint32_relax(&disp->yoff, val);
            bochs_display_update_mode(disp, false);
            break;
        case BOCHS_DISPI_ENABLE:
            atomic_store_uint32_relax(&disp->enable, val & BOCHS_ENABLE_MASK);
            if (val & BOCHS_ENABLE) {
                if (!(val & BOCHS_NOCLR)) {
                    // Clear VRAM
                    size_t vsiz = 0;
                    void*  vram = rvvm_fbdev_get_vram(disp->fbdev, &vsiz);
                    vma_clean(vram, vsiz, false);
                }
                // Fully update video mode
                bochs_display_update_mode(disp, true);
            }
            break;
    }

    return true;
}

static bool bochs_vram_write(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    bochs_display_t* disp = dev->data;
    rvvm_fbdev_dirty(disp->fbdev);
    UNUSED(data);
    UNUSED(offset);
    UNUSED(size);
    return true;
}

static void bochs_display_remove(rvvm_mmio_dev_t* dev)
{
    bochs_display_t* disp = dev->data;
    if (rvvm_fbdev_dec_ref(disp->fbdev)) {
        safe_free(disp);
    }
}

static void bochs_display_update(rvvm_mmio_dev_t* dev)
{
    bochs_display_t* disp = dev->data;
    rvvm_fbdev_update(disp->fbdev);
}

static const rvvm_mmio_type_t bochs_vram_type = {
    .name   = "bochs_vram",
    .remove = bochs_display_remove,
};

static const rvvm_mmio_type_t bochs_display_type = {
    .name   = "bochs_display",
    .remove = bochs_display_remove,
    .update = bochs_display_update,
};

pci_dev_t* rvvm_bochs_display_init(pci_bus_t* pci_bus, rvvm_fbdev_t* fbdev)
{
    bochs_display_t* disp = safe_new_obj(bochs_display_t);

    size_t vram_size = RVVM_BOCHS_DISPLAY_VRAM;
    void*  vram      = rvvm_fbdev_get_vram(fbdev, &vram_size);

    // fbdev is released twice
    rvvm_fbdev_inc_ref(fbdev);
    disp->fbdev = fbdev;

    pci_func_desc_t bochs_desc = {
        .vendor_id  = 0x1234, // Not in PCI ID database yet, should be Bochs
        .device_id  = 0x1111, // Not in PCI ID database yet, should be Bochs-Display
        .class_code = 0x0380, // Display controller
        .bar[0] = {
            .size    = vram_size,
            .data    = disp,
            .mapping = vram,
            .type    = &bochs_vram_type,
            .write   = bochs_vram_write,
        },
        .bar[2] = {
            .size        = 0x1000,
            .data        = disp,
            .type        = &bochs_display_type,
            .read        = bochs_display_read,
            .write       = bochs_display_write,
            .min_op_size = 2,
            .max_op_size = 2,
        },
    };

    pci_dev_t* pci_dev = pci_attach_func(pci_bus, &bochs_desc);
    return pci_dev;
}

pci_dev_t* rvvm_bochs_display_init_auto(rvvm_machine_t* machine, rvvm_fbdev_t* fbdev)
{
    return rvvm_bochs_display_init(rvvm_get_pci_bus(machine), fbdev);
}

POP_OPTIMIZATION_SIZE
