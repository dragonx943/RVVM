/*
rvtimer.c - Timers, sleep functions
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

// Expose clock_gettime(), nanosleep()
#include "feature_test.h"

#include "atomics.h"
#include "rvtimer.h"

#include "dlib.h"     // IWYU pragma: keep
#include "spinlock.h" // IWYU pragma: keep
#include "utils.h"    // IWYU pragma: keep

// For nanosleep(), clock_gettime(), CLOCK_MONOTONIC, etc
#include <time.h>

#if defined(HOST_TARGET_WIN32)
// Use QueryPerformanceCounter(), QueryPerformanceFrequency(),
// QueryUnbiasedInterruptTime() probing, timer locking
#include <windows.h>

// Obtain a thread local waitable timer with a specified timeout
#if defined(USE_THREAD_EMU)
static inline HANDLE thread_local_waitable_timer(uint64_t ns)
{
    UNUSED(ns);
    return NULL;
}
#else
HANDLE thread_local_waitable_timer(uint64_t ns);
#endif

// Obtain imprecise monotonic time without time in suspend (NT 6.1+)
static BOOL (*__stdcall query_uit)(PULONGLONG) = NULL;

static spinlock_t qpc_lock = SPINLOCK_INIT;
static uint64_t   qpc_last = 0, qpc_freq = 0;
static uint64_t   qpc_prev = 0, uit_prev = 0, qpc_off = 0;
#endif

#if defined(HOST_TARGET_POSIX) && HOST_TARGET_POSIX >= 199506L
// Use nanosleep()
#define NANOSLEEP_IMPL 1
#endif

#if defined(HOST_TARGET_LINUX) && defined(THREAD_LOCAL) && CHECK_INCLUDE(sys/prctl.h, 1)
// For PR_SET_TIMERSLACK
#include <sys/prctl.h>
#if defined(PR_SET_TIMERSLACK)
static THREAD_LOCAL bool timerslack_lowlatency = 0;
#define LINUX_TIMERSLACK_IMPL 1
#endif
#endif

#if defined(HOST_TARGET_DARWIN) && CHECK_INCLUDE(mach/mach_time.h, 1)
// Use mach_absolute_time()
#include <mach/mach_time.h>
static uint64_t mach_clk_freq = 0;
#define MACH_CLOCKSOURCE_IMPL 1
#endif

#if defined(HOST_TARGET_POSIX)
#if defined(HOST_TARGET_SERENITY) && defined(CLOCK_MONOTONIC_COARSE)
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
#endif

#if defined(TIME_MONOTONIC)
#define C11_TIMESPEC_CLOCKSOURCE TIME_MONOTONIC
#elif defined(TIME_UTC)
#define C11_TIMESPEC_CLOCKSOURCE TIME_UTC
#endif

#if !defined(HOST_TARGET_WIN32) && !defined(MACH_CLOCKSOURCE_IMPL)       /**/                                          \
    && !defined(POSIX_CLOCKSOURCE) && !defined(C11_TIMESPEC_CLOCKSOURCE) /**/                                          \
    && CHECK_INCLUDE(sys/time.h, 1)
#include <sys/time.h> // For gettimeofday()
#define GETTIMEOFDAY_CLOCKSOURCE 1
#endif

uint64_t rvtimer_clocksource(uint64_t freq)
{
#if defined(HOST_TARGET_WIN32)
    // Read the latest cached timer value from userspace
    uint64_t qpc_val = atomic_load_uint64_relax(&qpc_last);

    if (likely(spin_try_lock(&qpc_lock))) {
        // Claimed the QPC lock, obtain new clock timestamp
        LARGE_INTEGER tmp = ZERO_INIT;
        if (unlikely(!qpc_freq)) {
            // Initialize the clock frequency once
            QueryPerformanceFrequency(&tmp);
            qpc_freq = tmp.QuadPart;
            if (!qpc_freq) {
                rvvm_fatal("QueryPerformanceFrequency() failed!");
            }
            // Initialize unbiased clock if present
            query_uit = dlib_get_symbol("kernel32.dll", "QueryUnbiasedInterruptTime");
        }
        if (likely(QueryPerformanceCounter(&tmp))) {
            uint64_t qpc_new = ((uint64_t)tmp.QuadPart) + qpc_off;
            if (likely(qpc_new >= qpc_val)) {
                if (likely(query_uit)) {
                    // Check against unbiased clock to compensate for suspend & forward jumps
                    uint64_t qpc_delta = qpc_new - qpc_prev;
                    if (unlikely(qpc_delta > qpc_freq)) {
                        ULONGLONG uit_new = 0;
                        if (query_uit(&uit_new)) {
                            uint64_t uit_delta = rvtimer_convert_freq(uit_new - uit_prev, 10000000ULL, qpc_freq);
                            if (qpc_delta > uit_delta + qpc_freq) {
                                uint64_t compensate = EVAL_MIN(qpc_delta - uit_delta, qpc_new - qpc_val);
                                // Apply compensation when skew >1s
                                qpc_off  -= compensate;
                                qpc_new  -= compensate;
                                qpc_prev  = qpc_new;
                                uit_prev  = uit_new;
                            }
                        } else {
                            rvvm_warn("QueryUnbiasedInterruptTime() failed!");
                            query_uit = NULL;
                        }
                    }
                }
                // Cache the new timer value
                qpc_val = qpc_new;
                atomic_store_uint64_relax(&qpc_last, qpc_val);
            } else {
                // Sometimes TSC drifts back on obscure hardware, Windows doesn't fix this up
                DO_ONCE(rvvm_warn("Unstable clocksource (Backward drift observed)"));
            }
        } else {
            rvvm_fatal("QueryPerformanceCounter() failed!");
        }
        spin_unlock(&qpc_lock);
    } else if (unlikely(!qpc_val)) {
        // Wait for thread which got to initialize QPC first
        spin_lock_busy_loop(&qpc_lock);
        spin_unlock(&qpc_lock);
        qpc_val = atomic_load_uint64_relax(&qpc_last);
    }

    return rvtimer_convert_freq(qpc_val, qpc_freq, freq);

#elif defined(MACH_CLOCKSOURCE_IMPL)
    // Use mach_absolute_time()
    DO_ONCE_SCOPED {
        // Calculate Mach timer frequency
        mach_timebase_info_data_t mach_clk_info = ZERO_INIT;
        mach_timebase_info(&mach_clk_info);
        if (mach_clk_info.numer && mach_clk_info.denom) {
            mach_clk_freq = (mach_clk_info.denom * 1000000000ULL) / mach_clk_info.numer;
        }
        if (!mach_clk_freq) {
            rvvm_fatal("mach_timebase_info() failed!");
        }
    };
    return rvtimer_convert_freq(mach_absolute_time(), mach_clk_freq, freq);

#elif defined(POSIX_CLOCKSOURCE)
    // Use POSIX clock_gettime()
    struct timespec ts = ZERO_INIT;
    clock_gettime(POSIX_CLOCKSOURCE, &ts);
    return (ts.tv_sec * freq) + (ts.tv_nsec * freq / 1000000000ULL);

#elif defined(C11_TIMESPEC_CLOCKSOURCE)
    // Use C11 timespec_get()
    struct timespec ts = ZERO_INIT;
    timespec_get(C11_TIMESPEC_CLOCKSOURCE, &ts);
    return (ts.tv_sec * freq) + (ts.tv_nsec * freq / 1000000000ULL);

#elif defined(GETTIMEOFDAY_CLOCKSOURCE)
    // Use gettimeofday()
    struct timeval tv = ZERO_INIT;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * freq) + (tv.tv_usec * freq / 1000000U);

#else
#pragma message("Falling back to time() clocksource")
    return rvtimer_unixtime() * freq;

#endif
}

uint64_t rvtimer_unixtime(void)
{
#if defined(HOST_TARGET_WINNT)
    SYSTEMTIME st = ZERO_INIT;
    FILETIME   ft = ZERO_INIT;
    GetSystemTime(&st);
    if (SystemTimeToFileTime(&st, &ft)) {
        uint64_t wintime = ((uint64_t)(uint32_t)ft.dwLowDateTime) | (((uint64_t)(uint32_t)ft.dwHighDateTime) << 32);
        return (wintime / 10000000ULL) - 11644473600ULL;
    }
    return 0;
#elif defined(HOST_TARGET_DOS) && !defined(HOST_32BIT)
    return 0;
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
#if defined(LINUX_TIMERSLACK_IMPL)
    if (timerslack_lowlatency != enable) {
        timerslack_lowlatency = enable;
        prctl(PR_SET_TIMERSLACK, enable ? 1L : 0L);
    }
#endif
    UNUSED(enable);
}

void sleep_ns(uint64_t ns)
{
    UNUSED(ns);
#if defined(HOST_TARGET_WIN32)
    HANDLE timer = thread_local_waitable_timer(ns);
    if (likely(timer)) {
        WaitForSingleObject(timer, INFINITE);
    } else {
        Sleep(EVAL_MAX(ns / 1000000ULL, 1));
    }
#elif defined(NANOSLEEP_IMPL)
    struct timespec ts = {
        .tv_sec  = ns / 1000000000ULL,
        .tv_nsec = ns % 1000000000ULL,
    };
    sleep_low_latency(true);
    while (nanosleep(&ts, &ts) < 0) {
    }
#endif
}

void sleep_ms(uint32_t ms)
{
    sleep_ns(ms * 1000000ULL);
}
