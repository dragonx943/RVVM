/*
framebuffer.h - Simple Framebuffer
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_FRAMEBUFFER_H
#define RVVM_FRAMEBUFFER_H

#include <rvvm/rvvm_fb.h>

/**
 * Attach fbdev to the machine as simple-framebuffer device
 */
RVVM_PUBLIC rvvm_mmio_dev_t* rvvm_simplefb_init(rvvm_machine_t* machine, rvvm_addr_t addr, rvvm_fbdev_t* fbdev);

RVVM_PUBLIC rvvm_mmio_dev_t* rvvm_simplefb_init_auto(rvvm_machine_t* machine, rvvm_fbdev_t* fbdev);

#endif
