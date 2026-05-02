/*
rtc-ds7142.h - Dallas DS1742 Real-time Clock
Copyright (C) 2023  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_RTC_DS1742_H
#define RVVM_RTC_DS1742_H

/*
 * TODO: Remove this in favor of <rvvm/rvvm_board.h>
 */

#include <rvvm/rvvm_base.h>

RVVM_PUBLIC rvvm_reg_dev_t* rvvm_rtc_ds1742_init(rvvm_machine_t* machine, rvvm_addr_t addr);

static inline rvvm_reg_dev_t* rtc_ds1742_init_auto(rvvm_machine_t* machine)
{
    return rvvm_rtc_ds1742_init(machine, 0x00101000UL);
}

#endif

