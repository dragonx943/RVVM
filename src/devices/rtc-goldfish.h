/*
rtc-goldfish.h - Goldfish Real-time Clock
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_RTC_GOLDFISH_H
#define RVVM_RTC_GOLDFISH_H

#include <rvvm/rvvm_base.h>

/*
 * TODO: Remove this in favor of <rvvm/rvvm_board.h>
 */

RVVM_PUBLIC rvvm_reg_dev_t* rvvm_rtc_goldfish_init(rvvm_machine_t* machine, /**/
                                                   rvvm_addr_t     addr,    /**/
                                                   rvvm_irq_dev_t* irq_dev, /**/
                                                   rvvm_irq_t      irq);

static inline rvvm_reg_dev_t* rtc_goldfish_init_auto(rvvm_machine_t* machine)
{
    return rvvm_rtc_goldfish_init(machine, 0x00101000UL, NULL, 0);
}

#endif
