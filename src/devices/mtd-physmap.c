/*
mtd-physmap.c - Memory Technology Device Mapping
Copyright (C) 2023  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "mtd-physmap.h"

#include "fdtlib.h"
#include "mem_ops.h"
#include "utils.h"

PUSH_OPTIMIZATION_SIZE

typedef struct {
    rvvm_blk_dev_t* blk;

    uint8_t  buffer[4096];
    uint32_t dirty;
} mtd_flash_t;

static void mtd_flash_remove(rvvm_mmio_dev_t* dev)
{
    mtd_flash_t* mtd = dev->data;
    if (atomic_load_uint32_relax(&mtd->dirty)) {
        rvvm_blk_write_head(mtd->blk, mtd->buffer, sizeof(mtd->buffer));
    }
    rvvm_blk_close(mtd->blk);
    free(mtd);
}

static void mtd_flash_reset(rvvm_mmio_dev_t* dev)
{
    mtd_flash_t* mtd  = dev->data;
    rvvm_addr_t  addr = rvvm_get_opt(dev->machine, RVVM_OPT_RESET_PC);
    rvvm_addr_t  size = rvvm_blk_get_size(mtd->blk);

    void* ptr = rvvm_get_dma_ptr(dev->machine, addr, size);
    if (ptr) {
        rvvm_blk_read(mtd->blk, ptr, size, 0);
    }
}

static const rvvm_mmio_type_t mtd_flash_type = {
    .name   = "mtd-physmap",
    .remove = mtd_flash_remove,
    .reset  = mtd_flash_reset,
};

static bool mtd_flash_mmio_read(rvvm_mmio_dev_t* dev, void* data, size_t off, uint8_t size)
{
    mtd_flash_t* mtd = dev->data;
    if ((off >> 12) != (rvvm_blk_tell_head(mtd->blk) >> 12)) {
        rvvm_blk_seek_head(mtd->blk, (off >> 12) << 12, RVVM_BLK_SEEK_SET);
        rvvm_blk_read_head(mtd->blk, mtd->buffer, sizeof(mtd->buffer));
    }
    memcpy(data, mtd->buffer + (off & 0xFFF), size);
    return true;
}

static bool mtd_flash_mmio_write(rvvm_mmio_dev_t* dev, void* data, size_t off, uint8_t size)
{
    mtd_flash_t* mtd = dev->data;
    if ((off >> 12) != (rvvm_blk_tell_head(mtd->blk) >> 12)) {
        rvvm_blk_write_head(mtd->blk, mtd->buffer, sizeof(mtd->buffer));
        rvvm_blk_seek_head(mtd->blk, (off >> 12) << 12, RVVM_BLK_SEEK_SET);
    }
    memcpy(mtd->buffer + (off & 0xFFF), data, size);
    atomic_store_uint32_relax(&mtd->dirty, true);
    return true;
}

PUBLIC rvvm_mmio_dev_t* mtd_physmap_init_blk(rvvm_machine_t* machine, rvvm_addr_t addr, rvvm_blk_dev_t* blk)
{
    mtd_flash_t* mtd = safe_new_obj(mtd_flash_t);

    mtd->blk = blk;

    rvvm_blk_read_head(mtd->blk, mtd->buffer, sizeof(mtd->buffer));

    rvvm_mmio_dev_t mtd_mmio = {
        .addr        = addr,
        .size        = rvvm_blk_get_size(mtd->blk),
        .min_op_size = 1,
        .max_op_size = 8,
        .read        = mtd_flash_mmio_read,
        .write       = mtd_flash_mmio_write,
        .data        = mtd,
        .type        = &mtd_flash_type,
    };
    rvvm_mmio_dev_t* mmio = rvvm_attach_mmio(machine, &mtd_mmio);
    if (mmio == NULL) {
        return mmio;
    }
#if defined(USE_FDT)
    struct fdt_node* mtd_fdt = fdt_node_create_reg("flash", mtd_mmio.addr);
    fdt_node_add_prop_reg(mtd_fdt, "reg", mtd_mmio.addr, mtd_mmio.size);
    fdt_node_add_prop_str(mtd_fdt, "compatible", "mtd-ram");
    fdt_node_add_prop_u32(mtd_fdt, "bank-width", 0x1);
    fdt_node_add_prop_u32(mtd_fdt, "#address-cells", 1);
    fdt_node_add_prop_u32(mtd_fdt, "#size-cells", 1);
    {
        struct fdt_node* partition0 = fdt_node_create("partition@0");
        uint32_t         reg[2]     = {0, mtd_mmio.size};
        fdt_node_add_prop_cells(partition0, "reg", reg, 2);
        fdt_node_add_prop_str(partition0, "label", "firmware");
        fdt_node_add_child(mtd_fdt, partition0);
    }
    fdt_node_add_child(rvvm_get_fdt_soc(machine), mtd_fdt);
#endif
    return mmio;
}

rvvm_mmio_dev_t* mtd_physmap_init(rvvm_machine_t* machine, rvvm_addr_t addr, const char* image, bool rw)
{
    rvvm_blk_dev_t* blk = rvvm_blk_open(image, NULL, rw ? RVVM_BLK_RW : RVVM_BLK_READ);
    if (blk) {
        return mtd_physmap_init_blk(machine, addr, blk);
    }
    return NULL;
}

rvvm_mmio_dev_t* mtd_physmap_init_auto(rvvm_machine_t* machine, const char* image, bool rw)
{
    return mtd_physmap_init(machine, MTD_PHYSMAP_DEFAULT_MMIO, image, rw);
}

POP_OPTIMIZATION_SIZE
