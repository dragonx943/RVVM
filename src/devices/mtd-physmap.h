/*
mtd-physmap.h - Memory Technology Device Mapping
Copyright (C) 2023  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_MTD_PHYSMAP_H
#define RVVM_MTD_PHYSMAP_H

#include <rvvm/rvvm_blk.h>

/*
 * The main purpose of this device is to allow guests to flash
 * different firmware into the board memory chip
 */

#define MTD_PHYSMAP_DEFAULT_MMIO 0x04000000

RVVM_PUBLIC rvvm_mmio_dev_t* mtd_physmap_init_blk(rvvm_machine_t* machine, rvvm_addr_t addr, rvvm_blk_dev_t* blk);

RVVM_PUBLIC rvvm_mmio_dev_t* mtd_physmap_init(rvvm_machine_t* machine, rvvm_addr_t addr, const char* image, bool rw);
RVVM_PUBLIC rvvm_mmio_dev_t* mtd_physmap_init_auto(rvvm_machine_t* machine, const char* image, bool rw);

#endif

