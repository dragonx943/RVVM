/*
rtc-ds7142.c - Dallas DS1742 Real-time Clock
Copyright (C) 2023  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "rtc-ds1742.h"
#include "fdtlib.h"
#include "mem_ops.h"
#include "rvtimer.h"
#include "spinlock.h"
#include "utils.h"

PUSH_OPTIMIZATION_SIZE

#define DS1742_REG_CTL_CENT 0x0 // Control, Century
#define DS1742_REG_SECONDS  0x1 // Seconds [0, 59]
#define DS1742_REG_MINUTES  0x2 // Minutes [0, 59]
#define DS1742_REG_HOURS    0x3 // Hours [0, 23]
#define DS1742_REG_DAY      0x4 // Day of week [1, 7]
#define DS1742_REG_DATE     0x5 // Day of month [1, 31]
#define DS1742_REG_MONTH    0x6 // Month [1, 12]
#define DS1742_REG_YEAR     0x7 // Year [0, 99]

#define DS1742_MMIO_SIZE    0x8

#define DS1742_DAY_BATT     0x80 // Battery OK
#define DS1742_CTL_READ     0x40 // Lock registers for read
#define DS1742_CTL_MASK     0xC0 // Mask of control registers

typedef struct {
    spinlock_t lock;
    uint8_t    ctl;
    uint8_t    regs[DS1742_MMIO_SIZE];
} ds1742_dev_t;

static inline uint8_t bcd_conv_u8(uint8_t val)
{
    return (val % 10) | ((val / 10) << 4);
}

static inline uint64_t div_time_units(uint64_t* units, uint32_t div)
{
    uint64_t rem = *units % div;
    *units       = *units / div;
    return rem;
}

void rtc_ds1742_update_regs(ds1742_dev_t* rtc)
{
    uint64_t tmp  = rvtimer_unixtime();
    uint32_t sec  = div_time_units(&tmp, 60);          // Seconds [0, 59]
    uint32_t min  = div_time_units(&tmp, 60);          // Minutes [0, 59]
    uint32_t hour = div_time_units(&tmp, 24);          // Hours [0, 59]
    uint64_t days = tmp + 719468;                      // Days since 01.03.0000
    uint32_t wday = (days + 3) % 7;                    // Day of week [0, 6]
    uint32_t era  = days / 146097;                     // Era since 01.03.0000 (400 year unit)
    uint32_t doe  = days - (era * 146097);             // Day of era [0, 146096]
    uint32_t coe  = doe / 36524;                       // Century of era [0, 3]
    uint32_t lde  = (doe / 1460) - coe;                // Leap days of era
    uint32_t doel = doe - lde;                         // Day of era (Leap-compensated)
    uint32_t year = ((doel + 59) / 365) + (era * 400); // Year
    uint32_t doy  = doel % 365;                        // Day of year [0, 365] since 01.03.XXXX
    uint32_t mmf  = ((5 * doy) + 2) / 153;             // Month [0, 11] [Mar, Feb]
    uint32_t mday = doy - (((153 * mmf) + 2) / 5) + 1; // Day of month [1, 31]
    uint32_t mon  = mmf + (mmf < 10 ? 3 : -9);         // Month [1, 12] [Jan, Dec]

    rtc->regs[DS1742_REG_CTL_CENT] = bcd_conv_u8(year / 100);
    rtc->regs[DS1742_REG_SECONDS]  = bcd_conv_u8(sec);
    rtc->regs[DS1742_REG_MINUTES]  = bcd_conv_u8(min);
    rtc->regs[DS1742_REG_HOURS]    = bcd_conv_u8(hour);
    rtc->regs[DS1742_REG_DATE]     = bcd_conv_u8(mday);
    rtc->regs[DS1742_REG_DAY]      = bcd_conv_u8(wday);
    rtc->regs[DS1742_REG_MONTH]    = bcd_conv_u8(mon);
    rtc->regs[DS1742_REG_YEAR]     = bcd_conv_u8(year % 100);
}

static bool rtc_ds1742_mmio_read(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    ds1742_dev_t* rtc = dev->data;
    UNUSED(size);

    scoped_spin_lock (&rtc->lock) {
        uint8_t reg = rtc->regs[offset];
        switch (offset) {
            case DS1742_REG_CTL_CENT:
                reg |= rtc->ctl;
                break;
            case DS1742_REG_DAY:
                reg |= DS1742_DAY_BATT;
                break;
        }
        write_uint8(data, reg);
    }

    return true;
}

static bool rtc_ds1742_mmio_write(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    ds1742_dev_t* rtc = dev->data;
    UNUSED(size);

    if (offset == DS1742_REG_CTL_CENT) {
        uint8_t ctl = read_uint8(data) & DS1742_CTL_MASK;
        scoped_spin_lock (&rtc->lock) {
            if (!(rtc->ctl & DS1742_CTL_READ) && (ctl & DS1742_CTL_READ)) {
                rtc_ds1742_update_regs(rtc);
            }
            rtc->ctl = ctl;
        }
    }

    return true;
}

static rvvm_mmio_type_t rtc_ds1742_dev_type = {
    .name = "rtc_ds1742",
};

PUBLIC rvvm_mmio_dev_t* rtc_ds1742_init(rvvm_machine_t* machine, rvvm_addr_t base_addr)
{
    ds1742_dev_t*   rtc        = safe_new_obj(ds1742_dev_t);
    rvvm_mmio_dev_t rtc_ds1742 = {
        .addr        = base_addr,
        .size        = DS1742_MMIO_SIZE,
        .data        = rtc,
        .type        = &rtc_ds1742_dev_type,
        .read        = rtc_ds1742_mmio_read,
        .write       = rtc_ds1742_mmio_write,
        .min_op_size = 1,
        .max_op_size = 1,
    };
    rtc_ds1742_update_regs(rtc);
    rvvm_mmio_dev_t* mmio = rvvm_attach_mmio(machine, &rtc_ds1742);
    if (mmio == NULL) {
        return mmio;
    }
#ifdef USE_FDT
    struct fdt_node* rtc_fdt = fdt_node_create_reg("rtc", base_addr);
    fdt_node_add_prop_reg(rtc_fdt, "reg", base_addr, DS1742_MMIO_SIZE);
    fdt_node_add_prop_str(rtc_fdt, "compatible", "maxim,ds1742");
    fdt_node_add_child(rvvm_get_fdt_soc(machine), rtc_fdt);
#endif
    return mmio;
}

PUBLIC rvvm_mmio_dev_t* rtc_ds1742_init_auto(rvvm_machine_t* machine)
{
    rvvm_addr_t addr = rvvm_mmio_zone_auto(machine, RTC_DS1742_DEFAULT_MMIO, DS1742_MMIO_SIZE);
    return rtc_ds1742_init(machine, addr);
}

POP_OPTIMIZATION_SIZE
