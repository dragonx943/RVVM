/*
riscv-aplic.h - RISC-V Advanced Platform-Level Interrupt Controller
Copyright (C) 2024  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_APLIC_H
#define RVVM_APLIC_H

#include <rvvm/rvvm_base.h>

/*
 * TODO: Remove this in favor of <rvvm/rvvm_board.h>
 */

RVVM_PUBLIC rvvm_irq_dev_t* rvvm_riscv_aplic_init(rvvm_machine_t* machine, rvvm_addr_t maddr, rvvm_addr_t saddr);

static inline rvvm_irq_dev_t* riscv_aplic_init_auto(rvvm_machine_t* machine)
{
    return rvvm_riscv_aplic_init(machine, 0x0C000000UL, 0x0D000000UL);
}

#endif
