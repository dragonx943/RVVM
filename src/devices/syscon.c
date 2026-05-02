/*
syscon.c - Poweroff/reset syscon device
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <rvvm/rvvm_board.h>
#include <rvvm/rvvm_fdt.h>
#include <rvvm/rvvm_region.h>

#include <rvvm/rvvm.h>

#include "mem_ops.h"

PUSH_OPTIMIZATION_SIZE

#define SYSCON_POWEROFF 0x5555
#define SYSCON_RESET    0x7777

static void syscon_mmio_write(rvvm_reg_dev_t* dev, const void* data, size_t size, size_t off)
{
    rvvm_machine_t* machine = rvvm_region_machine(dev);
    uint16_t        val     = read_uint16_le(data);
    UNUSED(size);

    if (!off && val == SYSCON_POWEROFF) {
        rvvm_reset_machine(machine, false);
    } else if (!off && val == SYSCON_RESET) {
        rvvm_reset_machine(machine, true);
    }
}

static const rvvm_reg_type_t syscon_type = {
    .name     = "syscon",
    .write    = syscon_mmio_write,
    .min_size = 2,
    .max_size = 2,
};

RVVM_PUBLIC rvvm_reg_dev_t* rvvm_syscon_init(rvvm_machine_t* machine, rvvm_addr_t addr)
{
    rvvm_reg_desc_t desc = {
        .addr = addr,
        .size = 0x1000,
        .type = &syscon_type,
    };

    rvvm_reg_dev_t*  dev = rvvm_region_init_auto(machine, &desc);
    rvvm_fdt_node_t* soc = rvvm_get_fdt_soc(machine);

    if (dev && soc) {
        rvvm_fdt_node_t* test = rvvm_fdt_init_reg("test", desc.addr);
        rvvm_fdt_prop_set_reg(test, "reg", desc.addr, desc.size);
        rvvm_fdt_prop_set_str_list(test, "compatible", "sifive,test1\0sifive,test0\0syscon");
        rvvm_fdt_attach(soc, test);

        rvvm_fdt_node_t* poweroff = rvvm_fdt_init("poweroff");
        rvvm_fdt_prop_set_str(poweroff, "compatible", "syscon-poweroff");
        rvvm_fdt_prop_set_u32(poweroff, "value", SYSCON_POWEROFF);
        rvvm_fdt_prop_set_u32(poweroff, "offset", 0);
        rvvm_fdt_prop_set_u32(poweroff, "regmap", rvvm_fdt_phandle(test));
        rvvm_fdt_attach(rvvm_get_fdt_root(machine), poweroff);

        rvvm_fdt_node_t* reboot = rvvm_fdt_init("reboot");
        rvvm_fdt_prop_set_str(reboot, "compatible", "syscon-reboot");
        rvvm_fdt_prop_set_u32(reboot, "value", SYSCON_RESET);
        rvvm_fdt_prop_set_u32(reboot, "offset", 0);
        rvvm_fdt_prop_set_u32(reboot, "regmap", rvvm_fdt_phandle(test));
        rvvm_fdt_attach(rvvm_get_fdt_root(machine), reboot);
    }
    return dev;
}

POP_OPTIMIZATION_SIZE
