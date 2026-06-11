/*
rvtimer.h - Timers, sleep functions
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef LEKKIT_RVTIMER_H
#define LEKKIT_RVTIMER_H

#include "compiler.h"
#include "rvvm_types.h"

typedef struct {
    // Those fields are internal use only
    uint64_t begin;
    uint64_t freq;
} rvtimer_t;

typedef struct {
    // Those fields are internal use only
    uint64_t   timecmp;
    rvtimer_t* timer;
} rvtimecmp_t;

// Convert between frequencies without overflow
static inline uint64_t rvtimer_convert_freq(uint64_t clk, uint64_t src_freq, uint64_t dst_freq)
{
#if defined(INT128_SUPPORT)
    uint128_t mul128 = (uint128_t)clk * (uint128_t)dst_freq;
    if (likely(!(mul128 >> 64))) {
        // Fast path for non overflow-case (Just mul + div on x86_64 with the overflow check)
        return ((uint64_t)mul128) / src_freq;
    }
#else
    if (likely(!(clk >> 32) && !(src_freq >> 32) && !(dst_freq >> 32))) {
        // Fast path for non overflow-case
        uint64_t tmp = clk * dst_freq;
        if (!(tmp >> 32)) {
            // Prefer 32-bit division
            return ((uint32_t)tmp) / ((uint32_t)src_freq);
        } else {
            return tmp / ((uint32_t)src_freq);
        }
    }
#endif
    // Slow path
    uint64_t clk_div = clk / src_freq;
    uint64_t clk_rem = clk - (clk_div * src_freq);
    return (clk_div * dst_freq) + (clk_rem * dst_freq / src_freq);
}

// Get monotonic clocksource with the specified frequency
uint64_t rvtimer_clocksource(uint64_t freq);

// Get UNIX timestamp (Seconds since 1 Jan 1970)
uint64_t rvtimer_unixtime(void);

/*
 * Timer
 */

// Initialize the timer and the clocksource
void rvtimer_init(rvtimer_t* timer, uint64_t freq);

// Get timer frequency
uint64_t rvtimer_freq(const rvtimer_t* timer);

// Get current timer value
uint64_t rvtimer_get(const rvtimer_t* timer);

// Rebase the clocksource by time field
void rvtimer_rebase(rvtimer_t* timer, uint64_t time);

/*
 * Timer comparators
 */

// Init timer comparator
void rvtimecmp_init(rvtimecmp_t* cmp, rvtimer_t* timer);

// Set comparator timestamp
void rvtimecmp_set(rvtimecmp_t* cmp, uint64_t timecmp);

// Get comparator timestamp
uint64_t rvtimecmp_get(const rvtimecmp_t* cmp);

// Check if we have a pending timer interrupt. Updates on it's own
bool rvtimecmp_pending(const rvtimecmp_t* cmp);

// Get delay until the timer interrupt (In timer frequency)
uint64_t rvtimecmp_delay(const rvtimecmp_t* cmp);

// Get delay until the timer interrupt (In nanoseconds)
uint64_t rvtimecmp_delay_ns(const rvtimecmp_t* cmp);

/*
 * Sleep
 */

// Set expected sleep latency (Internal use)
void sleep_low_latency(bool enable);

// Sleep precisely with nanosecond delay
void sleep_ns(uint64_t ns);

// Sleep with millisecond delay
void sleep_ms(uint32_t ms);

#endif
