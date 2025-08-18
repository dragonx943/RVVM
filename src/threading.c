/*
threading.c - Threading, Futexes, Conditional variables, Threadpool
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

// Expose clock_gettime(), pthread_condattr_setclock(), pthread_cond_timedwait_relative_np(), syscall()
#include "feature_test.h"

#include "atomics.h"
#include "rvtimer.h"
#include "spinlock.h"
#include "threading.h"
#include "utils.h"
#include "vector.h"

SOURCE_OPTIMIZATION_SIZE

#if defined(_MSC_VER)
#include <intrin.h> // For _mm_pause()
#endif

#if defined(HOST_TARGET_WIN32)

// Use Win32 threads & events
#include <windows.h>

#if defined(HOST_TARGET_WINNT)

#include "dlib.h"

// High-resolution waitable timers via CreateWaitableTimerExW(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION), Win10 1803+
static HANDLE (*__stdcall create_waitable_timer_ex_w)(void*, LPCWSTR, DWORD, DWORD)                    = NULL;
static BOOL   (*__stdcall set_waitable_timer)(HANDLE, const LARGE_INTEGER*, LONG, void*, LPVOID, BOOL) = NULL;

// Fiber-local storage, Win Vista+
static DWORD (*__stdcall fls_alloc)(PFLS_CALLBACK_FUNCTION) = NULL;
static BOOL  (*__stdcall fls_free)(DWORD)                   = NULL;
static PVOID (*__stdcall fls_get_val)(DWORD)                = NULL;
static BOOL  (*__stdcall fls_set_val)(DWORD, PVOID)         = NULL;

// Scheduler resoluton handling via NtSetTimerResolution(), used when hires timers are missing
static int32_t (*__stdcall nt_set_timer_resolution)(ULONG, BOOLEAN, PULONG) = NULL;

static DWORD fls_waitable_timer = 0;

#define WIN32_WAITABLE_TIMER_IMPL 1

#endif

#else

// Use pthreads, pthread_cond
// Use futexes if present, even for condvar, as opposed to Windows where futex timeout precision is terrible
#include <pthread.h> // For pthread, pthread_cond, pthread_mutex
#include <time.h>    // For clock_gettime(), if CLOCK_REALTIME/CLOCK_MONOTONIC are defined

#if defined(HOST_TARGET_LINUX) && CHECK_INCLUDE(sys/syscall.h, 1)

// Linux futexes (Linux 2.6.22+)
#include <sys/syscall.h> // For __NR_futex, __NR_futex_time64
#include <unistd.h>      // For syscall()

#if !defined(__NR_futex) && defined(__NR_futex_time64)
#define __NR_futex __NR_futex_time64
#endif

#if defined(__NR_epoll_create) && CHECK_INCLUDE(linux/futex.h, 1)
#include <linux/futex.h> // For FUTEX_WAIT_PRIVATE, FUTEX_WAKE_PRIVATE
#endif

#if defined(__NR_futex) && defined(FUTEX_WAIT_PRIVATE) && defined(FUTEX_WAKE_PRIVATE)
#define LINUX_FUTEX_IMPL 1
#endif

#elif defined(HOST_TARGET_FREEBSD) && HOST_TARGET_FREEBSD >= 11 && CHECK_INCLUDE(sys/umtx.h, 1)

// FreeBSD futexes (_umtx_op(), FreeBSD 11.0+)
#include <sys/types.h>
#include <sys/umtx.h>

#if defined(UMTX_OP_WAIT_UINT) && defined(UMTX_OP_WAKE)
#define FREEBSD_FUTEX_IMPL 1
#endif

#elif defined(HOST_TARGET_OPENBSD) && CHECK_INCLUDE(sys/futex.h, 0)

// OpenBSD futexes (OpenBSD 6.2+)
#include <sys/futex.h>
#include <sys/time.h>

#if defined(FUTEX_WAIT) && defined(FUTEX_WAKE)
#define OPENBSD_FUTEX_IMPL 1
#endif

#elif defined(HOST_TARGET_DARWIN)

// Mac OS futexes (__ulock_wait(), OS X 10.12+)
#include "dlib.h" // For __ulock_wait() & __ulock_wake() probing

#define ULOCK_CMP_WAIT                       0x0001 // Compare and wait on a 32-bit value
#define ULOCK_WAKE_ONE                       0x0001 // Wake one thread
#define ULOCK_WAKE_ALL                       0x0101 // Wake all threads

static int (*ulock_wait)(uint32_t op, void* ptr, uint64_t val, uint32_t us) = NULL;
static int (*ulock_wake)(uint32_t op, void* ptr, uint64_t unused)           = NULL;

#define APPLE_FUTEX_IMPL                     1

// Use pthread_cond_timedwait_relative_np()
#define PTHREAD_COND_TIMEDWAIT_RELATIVE_IMPL 1

#endif

#if defined(HOST_TARGET_POSIX) && HOST_TARGET_POSIX >= 199506L && CHECK_INCLUDE(sched.h, 1)
// Use sched_yield()
#include <sched.h>
#define SCHED_YIELD_IMPL 1
#endif

#if defined(CLOCK_REALTIME) && defined(CLOCK_MONOTONIC)
// Use clock_gettime()
#define CLOCK_GETTIME_IMPL 1
#elif !defined(PTHREAD_COND_TIMEDWAIT_RELATIVE_IMPL)
// Use gettimeofday()
#include <sys/time.h>
#endif

#if !defined(PTHREAD_COND_TIMEDWAIT_RELATIVE_IMPL) && defined(CLOCK_GETTIME_IMPL) /**/                                 \
    && defined(HOST_TARGET_POSIX) && HOST_TARGET_POSIX >= 200809L
// Use pthread_condattr_setclock(CLOCK_MONOTONIC), fallback to CLOCK_REALTIME
#define PTHREAD_COND_ATTR_MONOTONIC_IMPL 1
#endif

#endif

#if defined(HOST_TARGET_WIN32)

#if defined(WIN32_WAITABLE_TIMER_IMPL)

/*
 * Precise sub-millisecond delay/timeout using high-resolution WaitableTimer (Win10 1803+)
 *
 * For whatever ridiculous reasons, high-resolution timers have observably worse timing characteristics when historical
 * "low-latency" scheduler mode is enabled via NtSetTimerResolution(5000), seemingly any sleep larger than a scheduler
 * tick is clamped to the next tick.
 *
 * Prefer to use NtSetTimerResolution(156250) for power saving reasons, and use high-resolution waitable timers for
 * anything that requires actual precise delays.
 */

static void __stdcall thread_local_waitable_timer_free(PVOID arg)
{
    if (arg) {
        CloseHandle(arg);
    }
}

static void thread_local_waitable_timer_cleanup(void)
{
    if (fls_waitable_timer) {
        fls_free(fls_waitable_timer);
        fls_waitable_timer = 0;
    }
}

static inline HANDLE thread_local_waitable_timer_init(void)
{
    HANDLE timer = NULL;
    bool   hires = false;
    if (likely(fls_waitable_timer)) {
        timer = fls_get_val(fls_waitable_timer);
        if (likely(timer)) {
            return timer;
        }
    }
    DO_ONCE_SCOPED {
        fls_alloc                  = dlib_get_symbol("kernel32.dll", "FlsAlloc");
        fls_free                   = dlib_get_symbol("kernel32.dll", "FlsFree");
        fls_get_val                = dlib_get_symbol("kernel32.dll", "FlsGetValue");
        fls_set_val                = dlib_get_symbol("kernel32.dll", "FlsSetValue");
        create_waitable_timer_ex_w = dlib_get_symbol("kernel32.dll", "CreateWaitableTimerExW");
        set_waitable_timer         = dlib_get_symbol("kernel32.dll", "SetWaitableTimer");
        nt_set_timer_resolution    = dlib_get_symbol("ntdll.dll", "NtSetTimerResolution");

        if (fls_alloc && fls_free && fls_get_val && fls_set_val && create_waitable_timer_ex_w && set_waitable_timer) {
            fls_waitable_timer = fls_alloc(thread_local_waitable_timer_free);
            call_at_deinit(thread_local_waitable_timer_cleanup);
        }
    }
    if (fls_waitable_timer) {
        // Create a high-resolution, manual reset waitable timer (Win10 1803+)
        timer = create_waitable_timer_ex_w(NULL, NULL, 0x3, 0x1F0003);
        if (timer) {
            hires = true;
        } else {
            // Create a manual reset waitable timer (Win Vista+)
            timer = create_waitable_timer_ex_w(NULL, NULL, 0x1, 0x1F0003);
        }
        if (timer) {
            // Save the waitable timer in TLS
            fls_set_val(fls_waitable_timer, timer);
        } else {
            // CreateWaitableTimerExW() failed miserably, deinit TLS
            thread_local_waitable_timer_cleanup();
        }
    }
    DO_ONCE_SCOPED {
        // Boost scheduler resolution if high-resolution timers are missing
        if (!hires && nt_set_timer_resolution) {
            ULONG cur = 0;
            nt_set_timer_resolution(5000, TRUE, &cur);
        }
    }
    return timer;
}

static inline void thread_local_waitable_timer_set(HANDLE timer, uint64_t ns)
{
    LARGE_INTEGER delay = {
        .QuadPart = -(ns / 100ULL),
    };
    set_waitable_timer(timer, &delay, 0, NULL, NULL, false);
}

#endif

HANDLE thread_local_waitable_timer(uint64_t ns)
{
#if defined(WIN32_WAITABLE_TIMER_IMPL)
    HANDLE timer = thread_local_waitable_timer_init();
    if (likely(timer)) {
        thread_local_waitable_timer_set(timer, ns);
        return timer;
    }
#else
    UNUSED(ns);
#endif
    return NULL;
}

#else

// Portable pthread_cond_timedwait() with relative monotonic nanosecond timeout

static void* pthread_cond_attr_monotonic(void)
{
    static void* attr_ptr = NULL;
#if defined(PTHREAD_COND_ATTR_MONOTONIC_IMPL)
    static pthread_condattr_t attr = {0};
    DO_ONCE_SCOPED {
        if (!pthread_condattr_init(&attr) && !pthread_condattr_setclock(&attr, CLOCK_MONOTONIC)) {
            attr_ptr = &attr;
        }
    }
#endif
    return attr_ptr;
}

static int pthread_cond_timedwait_ns_internal(pthread_cond_t* cond, pthread_mutex_t* mtx, uint64_t timeout_ns)
{
    if (timeout_ns == CONDVAR_INFINITE) {
        return pthread_cond_wait(cond, mtx);
    } else {
#if defined(PTHREAD_COND_TIMEDWAIT_RELATIVE_IMPL)
        struct timespec ts = {
            .tv_sec  = timeout_ns / 1000000000,
            .tv_nsec = timeout_ns % 1000000000,
        };
        return pthread_cond_timedwait_relative_np(cond, mtx, &ts);
#else
        struct timespec ts = {0};
#if defined(PTHREAD_COND_ATTR_MONOTONIC_IMPL)
        if (pthread_cond_attr_monotonic()) {
            clock_gettime(CLOCK_MONOTONIC, &ts);
        } else {
            // Fallback to CLOCK_REALTIME (Possibly a very old Linux kernel)
            clock_gettime(CLOCK_REALTIME, &ts);
        }
#elif defined(CLOCK_GETTIME_IMPL)
        clock_gettime(CLOCK_REALTIME, &ts);
#else
        // Some targets lack clock_gettime(), use gettimeofday()
        struct timeval tv = {0};
        gettimeofday(&tv, NULL);
        ts.tv_sec  = tv.tv_sec;
        ts.tv_nsec = tv.tv_usec * 1000U;
#endif
        // Properly handle timespec addition without an overflow
        timeout_ns += ts.tv_nsec;
        ts.tv_sec  += timeout_ns / 1000000000;
        ts.tv_nsec  = timeout_ns % 1000000000;
        return pthread_cond_timedwait(cond, mtx, &ts);
#endif
    }
}

#endif

/*
 * Threads
 */

struct thread_ctx {
#if defined(HOST_TARGET_WIN32)
    HANDLE handle;
#else
    pthread_t pthread;
#endif
};

#if defined(HOST_TARGET_WIN32)

// Wrap our thread function call to bridge C/Win32 ABIs

typedef struct {
    thread_func_t func;
    void*         arg;
} thread_call_wrap_t;

static DWORD __stdcall thread_call_wrapper(void* arg)
{
    thread_call_wrap_t wrap = *(thread_call_wrap_t*)arg;
    safe_free(arg);
    return (DWORD)(size_t)wrap.func(wrap.arg);
}

#endif

thread_ctx_t* thread_create_ex(thread_func_t func, void* arg, uint32_t stack_size)
{
    thread_ctx_t* thread = safe_new_obj(thread_ctx_t);
#if defined(HOST_TARGET_WIN32)
    DWORD               threadid = 0;
    thread_call_wrap_t* wrap     = safe_new_obj(thread_call_wrap_t);
    wrap->func                   = func;
    wrap->arg                    = arg;
    // Win9x: Passing NULL for the 'lpThreadId' parameter causes the function to fail
    thread->handle = CreateThread(NULL, stack_size, thread_call_wrapper, wrap, /**/
                                  STACK_SIZE_PARAM_IS_A_RESERVATION, &threadid);
    if (thread->handle) {
        return thread;
    }
#else
    pthread_attr_t  attr     = {0};
    pthread_attr_t* attr_ptr = NULL;
    if (stack_size && !pthread_attr_init(&attr) && !pthread_attr_setstacksize(&attr, stack_size)) {
        // Pass custom stack size if possible
        attr_ptr = &attr;
    }
    bool success = !pthread_create(&thread->pthread, attr_ptr, func, arg);
    if (attr_ptr) {
        pthread_attr_destroy(attr_ptr);
    }
    if (success) {
        return thread;
    }
#endif
    rvvm_warn("Failed to spawn thread!");
    safe_free(thread);
    return NULL;
}

thread_ctx_t* thread_create(thread_func_t func, void* arg)
{
    return thread_create_ex(func, arg, 0x10000);
}

bool thread_join(thread_ctx_t* thread)
{
    if (thread) {
#if defined(HOST_TARGET_WIN32)
        if (WaitForSingleObject(thread->handle, INFINITE) || !CloseHandle(thread->handle)) {
            rvvm_warn("Failed to join thread!");
        }
#else
        void* tmp = NULL;
        if (pthread_join(thread->pthread, &tmp)) {
            rvvm_warn("Failed to join thread!");
        }
#endif
        safe_free(thread);
        return true;
    }

    return false;
}

bool thread_detach(thread_ctx_t* thread)
{
    if (thread) {
#if defined(HOST_TARGET_WIN32)
        CloseHandle(thread->handle);
#else
        pthread_detach(thread->pthread);
#endif
        safe_free(thread);
        return true;
    }
    return false;
}

/*
 * Thread yielding, CPU relax hints
 */

void thread_sched_yield(void)
{
#if defined(HOST_TARGET_WIN32)
    Sleep(0);
#elif defined(SCHED_YIELD_IMPL)
    sched_yield();
#endif
}

void thread_cpu_relax(void)
{
#if defined(GNU_EXTS) && defined(__x86_64__)
    __asm__ volatile("pause" : : : "memory");
#elif defined(GNU_EXTS) && defined(__aarch64__)
    __asm__ volatile("isb sy" : : : "memory");
#elif defined(GNU_EXTS) && defined(__riscv)
    __asm__ volatile(".4byte 0x100000F" : : : "memory");
#elif defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    _mm_pause();
#endif
}

/*
 * Native futexes
 */

static bool thread_futex_wait_native(void* ptr, uint32_t val, uint64_t timeout_ns)
{
#if defined(LINUX_FUTEX_IMPL) || defined(FREEBSD_FUTEX_IMPL) || defined(OPENBSD_FUTEX_IMPL)
    // Prepare timespec structure
    struct timespec  ts     = {0};
    struct timespec* ts_ptr = NULL;
    if (timeout_ns != THREAD_FUTEX_INFINITE) {
        ts.tv_sec  = timeout_ns / 1000000000ULL;
        ts.tv_nsec = timeout_ns % 1000000000ULL;
        ts_ptr     = &ts;
    }
#endif

#if defined(LINUX_FUTEX_IMPL)
    sleep_low_latency(true);
    return syscall(__NR_futex, ptr, FUTEX_WAIT_PRIVATE, val, ts_ptr, NULL, 0) >= 0;
#elif defined(FREEBSD_FUTEX_IMPL)
    void* ts_size = ts_ptr ? ((void*)(uintptr_t)sizeof(ts)) : NULL;
    return _umtx_op(ptr, UMTX_OP_WAIT_UINT, val, ts_size, ts_ptr) >= 0;
#elif defined(OPENBSD_FUTEX_IMPL)
    return futex(ptr, FUTEX_WAIT, val, ts_ptr, NULL) >= 0;
#elif defined(APPLE_FUTEX_IMPL)
    uint64_t timeout_us = 0;
    if (timeout_us != THREAD_FUTEX_INFINITE) {
        timeout_us = timeout_ns / 1000ULL;
        timeout_us = EVAL_MIN(timeout_us, 0x7FFFFFFFU);
        timeout_us = EVAL_MAX(timeout_us, 1);
    }
    return ulock_wait && ulock_wait(ULOCK_CMP_WAIT, ptr, val, timeout_us) >= 0;
#else
    UNUSED(ptr);
    UNUSED(val);
    UNUSED(timeout_ns);
    return false;
#endif
}

static bool thread_futex_wake_native(void* ptr, uint32_t num)
{
#if defined(LINUX_FUTEX_IMPL)
    return syscall(__NR_futex, ptr, FUTEX_WAKE_PRIVATE, num, NULL, NULL, 0) >= 0;
#elif defined(FREEBSD_FUTEX_IMPL)
    return _umtx_op(ptr, UMTX_OP_WAKE, num, NULL, NULL) >= 0;
#elif defined(OPENBSD_FUTEX_IMPL)
    return futex(ptr, FUTEX_WAKE, num, NULL, NULL) >= 0;
#elif defined(APPLE_FUTEX_IMPL)
    return ulock_wake && ulock_wake((num > 1) ? ULOCK_WAKE_ALL : ULOCK_WAKE_ONE, ptr, 0) >= 0;
#else
    UNUSED(ptr);
    UNUSED(num);
    return false;
#endif
}

/*
 * Futex emulation fallback
 */

#define FUTEX_EMU_TABLE_SIZE 4

typedef struct {
    void*       ptr;
    cond_var_t* cond;
} futex_waiter_t;

typedef struct {
    vector_t(futex_waiter_t) waiters;
    spinlock_t               lock;
} futex_queue_t;

static bool futex_native = false;

static futex_queue_t futex_emu_table[FUTEX_EMU_TABLE_SIZE] = {0};

static inline futex_queue_t* thread_futex_emu_queue(void* ptr)
{
    size_t k  = (size_t)ptr;
    k        ^= k >> 21;
    k        ^= k >> 17;
    return &futex_emu_table[k & (FUTEX_EMU_TABLE_SIZE - 1)];
}

static bool thread_futex_wait_emu(void* ptr, uint32_t val, uint64_t ns)
{
    futex_queue_t* queue = thread_futex_emu_queue(ptr);
    futex_waiter_t self  = {
         .ptr  = ptr,
         .cond = condvar_create(),
    };
    bool ret = false;

    spin_lock_busy_loop(&queue->lock);
    if (atomic_load_uint32(ptr) == val) {
        vector_push_back(queue->waiters, self);
        spin_unlock(&queue->lock);

        ret = condvar_wait_ns(self.cond, ns);
        spin_lock_busy_loop(&queue->lock);
        vector_foreach_back (queue->waiters, i) {
            if (vector_at(queue->waiters, i).cond == self.cond) {
                vector_erase(queue->waiters, i);
                break;
            }
        }
    }
    spin_unlock(&queue->lock);

    condvar_free(self.cond);
    return ret;
}

static void thread_futex_wake_emu(void* ptr, uint32_t num)
{
    futex_queue_t* queue = thread_futex_emu_queue(ptr);
    spin_lock_busy_loop(&queue->lock);
    vector_foreach_back (queue->waiters, i) {
        if (num && vector_at(queue->waiters, i).ptr == ptr && condvar_wake(vector_at(queue->waiters, i).cond)) {
            num--;
        }
    }
    spin_unlock(&queue->lock);
}

static slow_path bool thread_futex_init_once(void)
{
    uint32_t tmp = 0;
    if (rvvm_has_arg("no_futex")) {
        return false;
    }
#if defined(APPLE_FUTEX_IMPL)
    ulock_wait = dlib_get_symbol(NULL, "__ulock_wait");
    ulock_wake = dlib_get_symbol(NULL, "__ulock_wake");
    if (!ulock_wait || !ulock_wake) {
        ulock_wait = NULL;
        ulock_wake = NULL;
    }
#endif
    if (thread_futex_wake_native(&tmp, 1)) {
        rvvm_info("Native futexes available");
        return true;
    }
    return false;
}

static bool thread_futex_is_native(void)
{
    DO_ONCE_SCOPED {
        futex_native = thread_futex_init_once();
    }
    return futex_native;
}

bool thread_futex_wait(void* ptr, uint32_t val, uint64_t timeout_ns)
{
    if (likely(ptr && timeout_ns)) {
        if (likely(thread_futex_is_native())) {
            return thread_futex_wait_native(ptr, val, timeout_ns);
        } else {
            return thread_futex_wait_emu(ptr, val, timeout_ns);
        }
    }
    return false;
}

void thread_futex_wake(void* ptr, uint32_t num)
{
    if (likely(ptr && num)) {
        if (likely(thread_futex_is_native())) {
            thread_futex_wake_native(ptr, num);
        } else {
            thread_futex_wake_emu(ptr, num);
        }
    }
}

/*
 * Conditional variables
 */

struct cond_var {
    uint32_t flag;
    uint32_t waiters;
#if defined(HOST_TARGET_WIN32)
    HANDLE event;
#else
    pthread_cond_t  cond;
    pthread_mutex_t lock;
#endif
};

static bool condvar_init_internal(cond_var_t* cond)
{
#if defined(HOST_TARGET_WIN32) && !defined(HOST_64BIT) && !defined(HOST_TARGET_WINCE)
    // Use ANSI syscall (Win9x compat)
    cond->event = CreateEventA(NULL, FALSE, FALSE, NULL);
    return !!cond->event;

#elif defined(HOST_TARGET_WIN32)
    cond->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    return !!cond->event;

#else
    return !pthread_cond_init(&cond->cond, pthread_cond_attr_monotonic()) && !pthread_mutex_init(&cond->lock, NULL);
#endif
}

cond_var_t* condvar_create(void)
{
    cond_var_t* cond = safe_new_obj(cond_var_t);
    if (condvar_init_internal(cond)) {
        return cond;
    }
    rvvm_warn("Failed to create conditional variable!");
    condvar_free(cond);
    return NULL;
}

static inline bool condvar_try_consume_signal(cond_var_t* cond)
{
    if (atomic_load_uint32_relax(&cond->flag)) {
        atomic_store_uint32(&cond->flag, 0);
        return true;
    }
    return false;
}

static bool condvar_wait_native_ns(cond_var_t* cond, uint64_t timeout_ns)
{
#if defined(HOST_TARGET_WIN32)
    if (timeout_ns == CONDVAR_INFINITE) {
        // Wait infinitely
        return !WaitForSingleObject(cond->event, INFINITE);
    } else {
        HANDLE timer = thread_local_waitable_timer(timeout_ns);
        if (timer) {
            // High-resolution timeout via waitable timer
            HANDLE handles[] = {cond->event, timer};
            return !WaitForMultipleObjects(STATIC_ARRAY_SIZE(handles), handles, FALSE, INFINITE);
        } else {
            // Fallback to millisecond-precision timeout
            return !WaitForSingleObject(cond->event, EVAL_MAX(timeout_ns / 1000000ULL, 1));
        }
    }
#else
    if (thread_futex_is_native()) {
        // Wait on a native futex when available
        return thread_futex_wait_native(&cond->flag, 0, timeout_ns);
    } else {
        // Use pthread_cond
        bool ret = false;

        pthread_mutex_lock(&cond->lock);
        ret = condvar_try_consume_signal(cond)
           || !pthread_cond_timedwait_ns_internal(&cond->cond, &cond->lock, timeout_ns);
        pthread_mutex_unlock(&cond->lock);

        return ret;
    }
#endif
}

bool condvar_wait_ns(cond_var_t* cond, uint64_t timeout_ns)
{
    if (likely(cond)) {
        if (condvar_try_consume_signal(cond)) {
            // Fast-path exit on an already signaled condvar
            return true;
        }

        if (unlikely(!timeout_ns)) {
            // Fast-path exit for condvar polling
            return false;
        }

        // Mark that a thread is about to be waiting here, otherwise wake may set signal
        // too late be consumed, but not see any waiters and so a wakeup event may be lost.
        atomic_add_uint32(&cond->waiters, 1);

        // Try to consume a signal again, since condvar_wake() could have been called
        // in-between the fast-path exit and waiter marking.
        if (condvar_try_consume_signal(cond)) {
            atomic_sub_uint32(&cond->waiters, 1);
            return true;
        }

        // Perform waiting on a kernel primitive
        bool ret = condvar_wait_native_ns(cond, timeout_ns);

        // Clear possibly danging signal
        if (condvar_try_consume_signal(cond)) {
            ret = true;
        }

        atomic_sub_uint32(&cond->waiters, 1);
        return ret;
    }
    return false;
}

bool condvar_wait(cond_var_t* cond, uint64_t timeout_ms)
{
    uint64_t timeout_ns = CONDVAR_INFINITE;
    if (timeout_ms != CONDVAR_INFINITE) {
        timeout_ns = timeout_ms * 1000000ULL;
    }
    return condvar_wait_ns(cond, timeout_ns);
}

static inline bool condvar_mark_signaled(cond_var_t* cond)
{
    return !atomic_swap_uint32(&cond->flag, 1);
}

static void condvar_wake_native(cond_var_t* cond, bool wake_all)
{
#if defined(HOST_TARGET_WIN32)
    for (uint32_t i = wake_all ? condvar_waiters(cond) : 1; i--;) {
        SetEvent(cond->event);
    }
#else
    if (thread_futex_is_native()) {
        // Wake a native futex
        thread_futex_wake_native(&cond->flag, wake_all ? -1 : 1);
    } else {
        // We aren't required to signal under the lock, but it should be taken anyways
        // to prevent lost wakeup between cond->flag check and waiting on pthread_cond
        pthread_mutex_lock(&cond->lock);
        pthread_mutex_unlock(&cond->lock);
        if (wake_all) {
            pthread_cond_broadcast(&cond->cond);
        } else {
            pthread_cond_signal(&cond->cond);
        }
    }
#endif
}

bool condvar_wake(cond_var_t* cond)
{
    if (likely(cond)) {
        if (condvar_mark_signaled(cond) && condvar_waiters(cond)) {
            // Omit wakeup syscall if there are no waiters
            condvar_wake_native(cond, false);
            return true;
        }
    }
    return false;
}

bool condvar_wake_all(cond_var_t* cond)
{
    if (likely(cond)) {
        if (condvar_mark_signaled(cond) && condvar_waiters(cond)) {
            condvar_wake_native(cond, true);
            return true;
        }
    }
    return false;
}

uint32_t condvar_waiters(cond_var_t* cond)
{
    if (likely(cond)) {
        return atomic_load_uint32(&cond->waiters);
    }
    return 0;
}

void condvar_free(cond_var_t* cond)
{
    if (likely(cond)) {
        uint32_t waiters = condvar_waiters(cond);
        if (waiters) {
            rvvm_warn("Destroying a condvar with %u waiters!", waiters);
        }
#if defined(HOST_TARGET_WIN32)
        if (cond->event) {
            CloseHandle(cond->event);
        }
#else
        pthread_cond_destroy(&cond->cond);
        pthread_mutex_destroy(&cond->lock);
#endif
        safe_free(cond);
    }
}

/*
 * Threadpool
 */

#define WORKER_THREADS 4
#define WORKQUEUE_SIZE 2048
#define WORKQUEUE_MASK (WORKQUEUE_SIZE - 1)

BUILD_ASSERT(!(WORKQUEUE_SIZE & WORKQUEUE_MASK));

typedef struct {
    uint32_t      seq;
    uint32_t      flags;
    thread_func_t func;
    void*         arg[THREAD_MAX_VA_ARGS];
} task_item_t;

typedef struct {
    task_item_t tasks[WORKQUEUE_SIZE];
    char        pad0[64];
    uint32_t    head;
    char        pad1[64];
    uint32_t    tail;
    char        pad2[64];
} work_queue_t;

static uint32_t      pool_run;
static uint32_t      pool_shut;
static work_queue_t  pool_wq;
static cond_var_t*   pool_cond;
static thread_ctx_t* pool_threads[WORKER_THREADS];

static void workqueue_init(work_queue_t* wq)
{
    for (size_t seq = 0; seq < WORKQUEUE_SIZE; ++seq) {
        atomic_store_uint32(&wq->tasks[seq].seq, seq);
    }
}

static bool workqueue_try_perform(work_queue_t* wq)
{
    uint32_t tail = atomic_load_uint32_ex(&wq->tail, ATOMIC_RELAXED);
    while (true) {
        task_item_t* task_ptr = &wq->tasks[tail & WORKQUEUE_MASK];
        uint32_t     seq      = atomic_load_uint32_ex(&task_ptr->seq, ATOMIC_ACQUIRE);
        int32_t      diff     = (int32_t)seq - (int32_t)(tail + 1);
        if (diff == 0) {
            // This is a filled task slot
            if (atomic_cas_uint32_ex(&wq->tail, tail, tail + 1, true, ATOMIC_RELAXED, ATOMIC_RELAXED)) {
                // We claimed the slot
                task_item_t task = wq->tasks[tail & WORKQUEUE_MASK];

                // Mark task slot as reusable
                atomic_store_uint32_ex(&task_ptr->seq, tail + WORKQUEUE_MASK + 1, ATOMIC_RELEASE);

                // Run the task
                if (task.flags & 2) {
                    ((thread_func_va_t)(void*)task.func)((void**)task.arg);
                } else {
                    task.func(task.arg[0]);
                }
                return true;
            }
        } else if (diff < 0) {
            // Queue is empty
            return false;
        } else {
            // Another consumer stole our task slot, reload the tail pointer
            tail = atomic_load_uint32_ex(&wq->tail, ATOMIC_RELAXED);
        }

        thread_sched_yield();
    }
}

static bool workqueue_submit(work_queue_t* wq, thread_func_t func, void** arg, unsigned arg_count, bool va)
{
    uint32_t head = atomic_load_uint32_ex(&wq->head, ATOMIC_RELAXED);
    while (true) {
        task_item_t* task_ptr = &wq->tasks[head & WORKQUEUE_MASK];
        uint32_t     seq      = atomic_load_uint32_ex(&task_ptr->seq, ATOMIC_ACQUIRE);
        int32_t      diff     = (int32_t)seq - (int32_t)head;
        if (diff == 0) {
            // This is an empty task slot
            if (atomic_cas_uint32_ex(&wq->head, head, head + 1, true, ATOMIC_RELAXED, ATOMIC_RELAXED)) {
                // We claimed the slot, fill it with data
                task_ptr->func = func;
                for (size_t i = 0; i < arg_count; ++i) {
                    task_ptr->arg[i] = arg[i];
                }
                task_ptr->flags = (va ? 2 : 0);
                // Mark the slot as filled
                atomic_store_uint32_ex(&task_ptr->seq, head + 1, ATOMIC_RELEASE);
                return true;
            }
        } else if (diff < 0) {
            // Queue is full
            break;
        } else {
            // Another producer stole our task slot, reload the head pointer
            head = atomic_load_uint32_ex(&wq->head, ATOMIC_RELAXED);
        }

        thread_sched_yield();
    }
    return false;
}

static void thread_workers_terminate(void)
{
    atomic_store_uint32(&pool_run, 0);
    // Wake & shut down all threads properly
    while (atomic_load_uint32(&pool_shut) != WORKER_THREADS) {
        condvar_wake_all(pool_cond);
        thread_sched_yield();
    }
    for (size_t i = 0; i < WORKER_THREADS; ++i) {
        thread_join(pool_threads[i]);
        pool_threads[i] = NULL;
    }
    condvar_free(pool_cond);
    pool_cond = NULL;
}

static void* threadpool_worker(void* ptr)
{
    while (atomic_load_uint32_ex(&pool_run, ATOMIC_RELAXED)) {
        while (workqueue_try_perform(&pool_wq)) {
        }
        condvar_wait(pool_cond, CONDVAR_INFINITE);
    }
    atomic_add_uint32(&pool_shut, 1);
    return ptr;
}

static void threadpool_init(void)
{
    atomic_store_uint32(&pool_shut, 0);
    atomic_store_uint32(&pool_run, 1);
    workqueue_init(&pool_wq);
    pool_cond = condvar_create();
    for (size_t i = 0; i < WORKER_THREADS; ++i) {
        pool_threads[i] = thread_create(threadpool_worker, NULL);
    }
    call_at_deinit(thread_workers_terminate);
}

static bool thread_queue_task(thread_func_t func, void** arg, unsigned arg_count, bool va)
{
    DO_ONCE(threadpool_init());

    if (workqueue_submit(&pool_wq, func, arg, arg_count, va)) {
        condvar_wake(pool_cond);
        return true;
    }

    // Still not queued!
    // Assuming entire threadpool is busy, just do a blocking task
    DO_ONCE(rvvm_warn("Blocking on workqueue task %p", func));
    return false;
}

void thread_create_task(thread_func_t func, void* arg)
{
    if (!thread_queue_task(func, &arg, 1, false)) {
        func(arg);
    }
}

void thread_create_task_va(thread_func_va_t func, void** args, unsigned arg_count)
{
    if (arg_count == 0 || arg_count > THREAD_MAX_VA_ARGS) {
        rvvm_warn("Invalid arg count in thread_create_task_va()!");
        return;
    }
    if (!thread_queue_task((thread_func_t)(void*)func, args, arg_count, true)) {
        func(args);
    }
}
