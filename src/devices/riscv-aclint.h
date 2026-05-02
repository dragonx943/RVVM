/*
riscv-aclint.h - RISC-V Advanced Core Local Interruptor
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_ACLINT_H
#define RVVM_ACLINT_H

#include <rvvm/rvvm_base.h>

/*
 * TODO: Remove this in favor of <rvvm/rvvm_board.h>
 */

RVVM_PUBLIC bool rvvm_riscv_clint_init(rvvm_machine_t* machine, rvvm_addr_t addr);

static inline bool riscv_clint_init_auto(rvvm_machine_t* machine)
{
    return rvvm_riscv_clint_init(machine, 0x2000000UL);
}

#endif
