/*
rvtimer.c - Timers, sleep functions
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

// Make POSIX 2008 features available in strict C standard mode
// For clock_gettime(), nanosleep(), sched_yield()
#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L

// Force 64-bit time_t
#undef _TIME_BITS
#define _TIME_BITS 64
#undef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64

#include "rvtimer.h"
#include "atomics.h"
#include "compiler.h"

// For nanosleep(), clock_gettime(), CLOCK_MONOTONIC, etc
#include <time.h>

#if defined(__linux__) && defined(THREAD_LOCAL) && CHECK_INCLUDE(sys/prctl.h, 1)
// For PR_SET_TIMERSLACK
#include <sys/prctl.h>

#if defined(PR_SET_TIMERSLACK)
static THREAD_LOCAL bool timerslack_lowlatency;

#define TIMERSLACK_IMPL 1
#endif
#endif

#if defined(__unix__) || defined(__APPLE__) || defined(__HAIKU__)
#define NANOSLEEP_IMPL 1
#else
#include "threading.h"
#endif

#if defined(NANOSLEEP_IMPL) && CHECK_INCLUDE(sched.h, 1)
// For sched_yield()
#include <sched.h>

#define SCHED_YIELD_IMPL 1
#endif

#if defined(_WIN32)
// Use QueryPerformanceCounter(), QueryPerformanceFrequency()
#include <windows.h>
// For QueryUnbiasedInterruptTime() probing, timer locking
#include "dlib.h"
#include "spinlock.h"
#include "utils.h"

static spinlock_t qpc_lock = SPINLOCK_INIT;
static uint64_t   qpc_last = 0, qpc_freq = 0, qpc_off = 0;
static uint64_t   qpc_last_checked = 0, uit_last_checked = 0;

static BOOL (*__stdcall query_uit)(PULONGLONG) = NULL;

HANDLE thread_local_waitable_timer(uint64_t ns);

#define WIN32_CLOCKSOURCE_IMPL 1

#elif defined(__APPLE__)
// Use mach_absolute_time()
#include <mach/mach_time.h>

#include "utils.h"

static uint64_t mach_clk_freq = 0;

#define MACH_CLOCKSOURCE_IMPL 1

#endif

#if defined(__serenity__) && defined(CLOCK_MONOTONIC_COARSE)
// Use CLOCK_MONOTONIC_COARSE on Serenity for performance reasons
#define POSIX_CLOCKSOURCE CLOCK_MONOTONIC_COARSE
#elif defined(CLOCK_UPTIME)
// Use CLOCK_UPTIME on OpenBSD, FreeBSD, etc to skip time in suspend
#define POSIX_CLOCKSOURCE CLOCK_UPTIME
#elif defined(CLOCK_MONOTONIC)
// Use CLOCK_MONOTONIC, on Linux it halts in suspend
#define POSIX_CLOCKSOURCE CLOCK_MONOTONIC
#elif defined(CLOCK_REALTIME)
#define POSIX_CLOCKSOURCE CLOCK_REALTIME
#endif

#if defined(TIME_MONOTONIC)
#define C11_TIMESPEC_CLOCKSOURCE TIME_MONOTONIC
#elif defined(TIME_UTC)
#define C11_TIMESPEC_CLOCKSOURCE TIME_UTC
#endif

uint64_t rvtimer_clocksource(uint64_t freq)
{
#if defined(WIN32_CLOCKSOURCE_IMPL)
    // Read the latest cached timer value from userspace
    uint64_t qpc_val = atomic_load_uint64_relax(&qpc_last);

    scoped_spin_try_lock (&qpc_lock) {
        // Claimed the QPC lock, obtain new clock timestamp
        LARGE_INTEGER qpc = {0};

        if (unlikely(!qpc_freq)) {
            // Initialize the clock frequency once
            LARGE_INTEGER tmp = {0};
            QueryPerformanceFrequency(&tmp);
            qpc_freq = tmp.QuadPart;
            if (!qpc_freq) {
                rvvm_fatal("QueryPerformanceFrequency() failed!");
            }
            // Initialize unbiased backup clock if present
            query_uit = dlib_get_symbol("kernel32.dll", "QueryUnbiasedInterruptTime");
        }

        if (unlikely(!QueryPerformanceCounter(&qpc))) {
            rvvm_fatal("QueryPerformanceCounter() failed!");
        }

        uint64_t qpc_new = ((uint64_t)qpc.QuadPart) + qpc_off;
        if (qpc_new < qpc_val) {
            // Sometimes TSC drifts back on obscure hardware, Windows doesn't fix this up
            DO_ONCE(rvvm_warn("Unstable clocksource (backward drift observed)"));
        } else {
            if (query_uit) {
                // Check unbiased backup clock to compensate for suspend & forward jumps
                uint64_t qpc_delta = qpc_new - qpc_last_checked;
                if (qpc_delta > qpc_freq) {
                    ULONGLONG uit = 0;
                    query_uit(&uit);
                    uint64_t uit_new   = uit;
                    uint64_t uit_delta = rvtimer_convert_freq(uit_new - uit_last_checked, 10000000ULL, qpc_freq);
                    if (qpc_delta > uit_delta + qpc_freq && qpc_last_checked) {
                        uint64_t compensate  = EVAL_MIN(qpc_delta - uit_delta, qpc_new - qpc_val);
                        qpc_off             -= compensate;
                        qpc_new             -= compensate;
                    }

                    qpc_last_checked = qpc_new;
                    uit_last_checked = uit_new;
                }
            }

            // Cache the new timer value
            qpc_val = qpc_new;
            atomic_store_uint64_relax(&qpc_last, qpc_val);
        }
    }

    return rvtimer_convert_freq(qpc_val, qpc_freq, freq);
#elif defined(MACH_CLOCKSOURCE_IMPL)
    // Use mach_absolute_time()
    DO_ONCE_SCOPED {
        // Calculate Mach timer frequency
        mach_timebase_info_data_t mach_clk_info = {0};
        mach_timebase_info(&mach_clk_info);
        if (!mach_clk_info.numer || !mach_clk_info.denom) {
            rvvm_fatal("mach_timebase_info() failed!");
        }
        mach_clk_freq = (mach_clk_info.denom * 1000000000ULL) / mach_clk_info.numer;
    };
    return rvtimer_convert_freq(mach_absolute_time(), mach_clk_freq, freq);
#elif defined(POSIX_CLOCKSOURCE)
    // Use POSIX 2008 clock_gettime()
    struct timespec ts = {0};
    clock_gettime(POSIX_CLOCKSOURCE, &ts);
    return (ts.tv_sec * freq) + (ts.tv_nsec * freq / 1000000000ULL);
#elif defined(C11_TIMESPEC_CLOCKSOURCE)
    // Use C11 timespec_get()
    struct timespec ts = {0};
    timespec_get(C11_TIMESPEC_CLOCKSOURCE, &ts);
    return (ts.tv_sec * freq) + (ts.tv_nsec * freq / 1000000000ULL);
#else
#warning Falling back to imprecise generic clocksource
    // Use wall clock with no sub-second precision
    return rvtimer_unixtime() * freq;
#endif
}

uint64_t rvtimer_unixtime(void)
{
#if defined(_WIN32) && !defined(UNDER_CE)
    FILETIME ft = {0};
    GetSystemTimeAsFileTime(&ft);
    uint64_t wintime = ((uint64_t)(uint32_t)ft.dwLowDateTime) | (((uint64_t)(uint32_t)ft.dwHighDateTime) << 32);
    return (wintime / 10000000ULL) - 11644473600ULL;
#else
    return time(NULL);
#endif
}

void rvtimer_init(rvtimer_t* timer, uint64_t freq)
{
    timer->freq = freq;
    rvtimer_rebase(timer, 0);
}

uint64_t rvtimer_freq(const rvtimer_t* timer)
{
    return timer->freq;
}

uint64_t rvtimer_get(const rvtimer_t* timer)
{
    return rvtimer_clocksource(timer->freq) - atomic_load_uint64_relax(&timer->begin);
}

void rvtimer_rebase(rvtimer_t* timer, uint64_t time)
{
    atomic_store_uint64_relax(&timer->begin, rvtimer_clocksource(timer->freq) - time);
}

void rvtimecmp_init(rvtimecmp_t* cmp, rvtimer_t* timer)
{
    cmp->timer = timer;
    rvtimecmp_set(cmp, -1);
}

void rvtimecmp_set(rvtimecmp_t* cmp, uint64_t timecmp)
{
    atomic_store_uint64_relax(&cmp->timecmp, timecmp);
}

uint64_t rvtimecmp_get(const rvtimecmp_t* cmp)
{
    return atomic_load_uint64_relax(&cmp->timecmp);
}

bool rvtimecmp_pending(const rvtimecmp_t* cmp)
{
    return rvtimer_get(cmp->timer) >= rvtimecmp_get(cmp);
}

uint64_t rvtimecmp_delay(const rvtimecmp_t* cmp)
{
    uint64_t timer   = rvtimer_get(cmp->timer);
    uint64_t timecmp = rvtimecmp_get(cmp);
    return (timer < timecmp) ? (timecmp - timer) : 0;
}

uint64_t rvtimecmp_delay_ns(const rvtimecmp_t* cmp)
{
    return rvtimer_convert_freq(rvtimecmp_delay(cmp), rvtimer_freq(cmp->timer), 1000000000ULL);
}

void sleep_low_latency(bool enable)
{
#if defined(TIMERSLACK_IMPL)
    if (timerslack_lowlatency != enable) {
        timerslack_lowlatency = enable;
        prctl(PR_SET_TIMERSLACK, enable ? 1L : 0L);
    }
#endif
    UNUSED(enable);
}

void sleep_ns(uint64_t ns)
{
#if defined(_WIN32)
    if (ns) {
        HANDLE timer = thread_local_waitable_timer(ns);
        if (timer) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    Sleep(ns ? EVAL_MAX(ns / 1000000ULL, 1) : 0);
#elif defined(NANOSLEEP_IMPL)
    if (ns) {
        struct timespec ts = {
            .tv_sec  = ns / 1000000000ULL,
            .tv_nsec = ns % 1000000000ULL,
        };
        sleep_low_latency(true);
        while (nanosleep(&ts, &ts) < 0) {
        }
        return;
    }
#else
    if (ns) {
        cond_var_t* cond = condvar_create();
        condvar_wait_ns(cond, ns);
        condvar_free(cond);
        return;
    }
#endif

#if defined(SCHED_YIELD_IMPL)
    if (!ns) {
        // Yield this thread time slice, as does Win32 Sleep(0)
        sched_yield();
    }
#endif
}

void sleep_ms(uint32_t ms)
{
    sleep_ns(ms * 1000000ULL);
}
