/*
framebuffer.h - Simple Framebuffer
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_FRAMEBUFFER_H
#define RVVM_FRAMEBUFFER_H

#include <rvvm/rvvm.h>

/*
 * TODO: Remove this in favor of <rvvm/rvvm_board.h>
 */

RVVM_PUBLIC rvvm_reg_dev_t* rvvm_simplefb_init(rvvm_machine_t* machine, rvvm_fbdev_t* fbdev, rvvm_addr_t addr);

static inline rvvm_reg_dev_t* rvvm_simplefb_init_auto(rvvm_machine_t* machine, rvvm_fbdev_t* fbdev)
{
    rvvm_reg_dev_t* ret = rvvm_simplefb_init(machine, fbdev, 0x18000000UL);
    if (ret) {
        rvvm_append_cmdline(machine, "console=tty0");
    }
    return ret;
}

#endif
