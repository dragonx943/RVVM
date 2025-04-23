/*
threading.c - Threading, Futexes, Conditional variables, Threadpool
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

// For pthread_condattr_setclock(), pthread_cond_timedwait_relative_np()
#define _GNU_SOURCE
#define _BSD_SOURCE
#define _DEFAULT_SOURCE

#include "threading.h"
#include "compiler.h"

#if defined(_WIN32)

// Use Win32 threads, Win32 Events
// Use RtlWaitOnAddress() for futexes if present
#include <windows.h>

#include "dlib.h" // For runtime system features probing

#if !defined(UNDER_CE)

// Win32 futexes (RtlWaitOnAddress(), Win8+)
static BOOL (*__stdcall rtl_wait_on_addr)(const void*, const void*, size_t, const LARGE_INTEGER*) = NULL;
static void (*__stdcall rtl_wake_by_addr_single)(const void*)                                     = NULL;
static void (*__stdcall rtl_wake_by_addr_all)(const void*)                                        = NULL;

// Precise waitable timers (CreateWaitableTimerExW(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION), Win10 1803+)
static HANDLE (*__stdcall create_waitable_timer_ex_w)(void*, LPCWSTR, DWORD, DWORD)                    = NULL;
static BOOL   (*__stdcall set_waitable_timer)(HANDLE, const LARGE_INTEGER*, LONG, void*, LPVOID, BOOL) = NULL;

#define WIN32_FUTEX_IMPL          1
#define WIN32_WAITABLE_TIMER_IMPL 1

#endif

#else

// Use pthreads, pthread_cond
// Use Linux futexes if present, even for condvar
#include <pthread.h> // For pthread, pthread_cond, pthread_mutex
#include <time.h>    // For clock_gettime(), if CLOCK_REALTIME/CLOCK_MONOTONIC are defined

#if defined(__linux__) && CHECK_INCLUDE(linux/futex.h, 1) && CHECK_INCLUDE(sys/syscall.h, 1)
#include <linux/futex.h> // For FUTEX_WAIT_PRIVATE, FUTEX_WAKE_PRIVATE
#include <sys/syscall.h> // For __NR_futex
#include <unistd.h>      // For syscall()
#if !defined(__NR_futex) && defined(__NR_futex_time64)
#define __NR_futex __NR_futex_time64
#endif
#if defined(__NR_futex) && defined(FUTEX_WAIT_PRIVATE) && defined(FUTEX_WAKE_PRIVATE)
// Linux futexes available
#define LINUX_FUTEX_IMPL 1
#endif
#endif

#if defined(__FreeBSD__) && CHECK_INCLUDE(sys/types.h, 1) && CHECK_INCLUDE(sys/umtx.h, 1)
#include <sys/types.h> // For _umtx_op
#include <sys/umtx.h>  // For UMTX_OP_WAIT_UINT_PRIVATE, UMTX_OP_WAKE_PRIVATE
#if defined(UMTX_OP_WAIT_UINT_PRIVATE) && defined(UMTX_OP_WAKE_PRIVATE)
// FreeBSD futexes available
#define FREEBSD_FUTEX_IMPL 1
#endif
#endif

#if defined(__APPLE__)
#include "dlib.h"                                   // For __ulock_wait() probing

// Mac OS futexes (__ulock_wait(), OS X 10.12+)
#define ULOCK_WAIT                           0x0001 // Compare and wait on a 32-bit value
#define ULOCK_WAKE                           0x0001 // Wake one thread
#define ULOCK_WAKE_ALL                       0x0101 // Wake all threads
static int (*ulock_wait)(uint32_t op, void* ptr, uint64_t val, uint32_t us) = NULL;
static int (*ulock_wake)(uint32_t op, void* ptr, uint64_t unused)           = NULL;

#define APPLE_FUTEX_IMPL                     1

// Use pthread_cond_timedwait_relative_np()
#define PTHREAD_COND_TIMEDWAIT_RELATIVE_IMPL 1

#endif

#if !defined(PTHREAD_COND_TIMEDWAIT_RELATIVE_IMPL)
// We need absolute clock timestamps for pthread_cond_timedwait(), preferably monotonic.
// To set a pthread_cond clock other than CLOCK_REALTIME, POSIX 2008+ is required.

#if defined(CLOCK_MONOTONIC)
#include <unistd.h> // For _POSIX_VERSION
#if _POSIX_VERSION >= 200809
#define PTHREAD_COND_CLOCK_MONOTONIC CLOCK_MONOTONIC
#endif
#endif

#if !defined(PTHREAD_COND_CLOCK_MONOTONIC) && !defined(CLOCK_REALTIME)
#include <sys/time.h> // For gettimeofday()
#endif

static void condvar_fill_timespec(struct timespec* ts)
{
#if defined(PTHREAD_COND_CLOCK_MONOTONIC)
    clock_gettime(PTHREAD_COND_CLOCK_MONOTONIC, ts);
#elif defined(CLOCK_REALTIME)
    clock_gettime(CLOCK_REALTIME, ts);
#else
    // Some targets lack clock_gettime(), use gettimeofday()
    struct timeval tv = {0};
    gettimeofday(&tv, NULL);
    ts->tv_sec  = tv.tv_sec;
    ts->tv_nsec = tv.tv_usec * 1000;
#endif
}

#endif

#endif

// RVVM internal headers come after system headers because of safe_free()
#include "atomics.h"
#include "rvtimer.h"
#include "utils.h"

/*
 * Threads
 */

struct thread_ctx {
#if defined(_WIN32)
    HANDLE handle;
#else
    pthread_t pthread;
#endif
};

#if defined(_WIN32)
// Wrap our thread function call to bridge C/Win32 ABIs

typedef struct {
    thread_func_t func;
    void*         arg;
} thread_call_wrap_t;

static DWORD __stdcall thread_call_wrapper(void* ptr)
{
    thread_call_wrap_t wrap = *(thread_call_wrap_t*)ptr;
    free(ptr);
    return (DWORD)(size_t)wrap.func(wrap.arg);
}
#endif

thread_ctx_t* thread_create_ex(thread_func_t func, void* arg, uint32_t stack_size)
{
    thread_ctx_t* thread = safe_new_obj(thread_ctx_t);
#if defined(_WIN32)
    DWORD threadid = 0;
    thread_call_wrap_t* wrap = safe_new_obj(thread_call_wrap_t);
    wrap->func               = func;
    wrap->arg                = arg;

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
    free(thread);
    return NULL;
}

thread_ctx_t* thread_create(thread_func_t func, void* arg)
{
    return thread_create_ex(func, arg, 0x10000);
}

bool thread_join(thread_ctx_t* thread)
{
    if (thread) {
#if defined(_WIN32)
        if (WaitForSingleObject(thread->handle, INFINITE) || !CloseHandle(thread->handle)) {
            rvvm_warn("Failed to join thread!");
        }
#else
        void* tmp = NULL;
        if (pthread_join(thread->pthread, &tmp)) {
            rvvm_warn("Failed to join thread!");
        }
#endif
        free(thread);
        return true;
    }

    return false;
}

bool thread_detach(thread_ctx_t* thread)
{
    if (thread) {
#if defined(_WIN32)
        CloseHandle(thread->handle);
#else
        pthread_detach(thread->pthread);
#endif
        free(thread);
        return true;
    }
    return false;
}

/*
 * Futexes & Futex emulation fallback
 */

#define FUTEX_EMU_TABLE_SIZE 16

static bool         futex_native    = false;
static cond_var_t** futex_emu_table = NULL;

static bool thread_futex_wait_native(void* ptr, uint32_t val, uint64_t timeout_ns)
{
#if defined(WIN32_FUTEX_IMPL)
    if (likely(rtl_wait_on_addr)) {
        LARGE_INTEGER  timeout     = {0};
        LARGE_INTEGER* timeout_ptr = NULL;
        if (timeout_ns != THREAD_FUTEX_INFINITE) {
            timeout.QuadPart = -(timeout_ns / 100ULL);
            timeout_ptr      = &timeout;
        }
        sleep_low_latency(timeout_ns < 15000000);
        return rtl_wait_on_addr(ptr, &val, 4, timeout_ptr);
    }
#elif defined(LINUX_FUTEX_IMPL) || defined(FREEBSD_FUTEX_IMPL)
    struct timespec  ts     = {0};
    struct timespec* ts_ptr = NULL;
    if (timeout_ns != THREAD_FUTEX_INFINITE) {
        ts.tv_sec  = timeout_ns / 1000000000ULL;
        ts.tv_nsec = timeout_ns % 1000000000ULL;
        ts_ptr     = &ts;
    }
#if defined(LINUX_FUTEX_IMPL)
    sleep_low_latency(timeout_ns < 15000000);
    if (syscall(__NR_futex, ptr, FUTEX_WAIT_PRIVATE, val, ts_ptr, NULL, 0) >= 0) {
        return true;
    }
#elif defined(FREEBSD_FUTEX_IMPL)
    void* ts_size = ts_ptr ? ((void*)(uintptr_t)sizeof(ts)) : NULL;
    if (_umtx_op(ptr, UMTX_OP_WAIT_UINT_PRIVATE, val, ts_size, ts_ptr) >= 0) {
        return true;
    }
#endif
#elif defined(APPLE_FUTEX_IMPL)
    if (likely(ulock_wait)) {
        uint32_t timeout_us = (timeout_ns != THREAD_FUTEX_INFINITE) ? (timeout_ns / 1000ULL) : 0;
        return ulock_wait(ULOCK_WAIT, ptr, val, timeout_us) >= 0;
    }
#else
    UNUSED(ptr);
    UNUSED(val);
    UNUSED(timeout_ns);
#endif
    return false;
}

static bool thread_futex_wake_native(void* ptr, uint32_t num)
{
#if defined(WIN32_FUTEX_IMPL)
    if (likely(rtl_wait_on_addr)) {
        if (num > 1) {
            rtl_wake_by_addr_all(ptr);
        } else {
            rtl_wake_by_addr_single(ptr);
        }
        return true;
    }
#elif defined(LINUX_FUTEX_IMPL)
    if (syscall(__NR_futex, ptr, FUTEX_WAKE_PRIVATE, num, NULL, NULL, 0) >= 0) {
        return true;
    }
#elif defined(FREEBSD_FUTEX_IMPL)
    if (_umtx_op(ptr, UMTX_OP_WAKE_PRIVATE, num, NULL, NULL) >= 0) {
        return true;
    }
#elif defined(APPLE_FUTEX_IMPL)
    if (likely(ulock_wake)) {
        ulock_wake((num > 1) ? ULOCK_WAKE_ALL : ULOCK_WAKE, ptr, 0);
        return true;
    }
#else
    UNUSED(ptr);
    UNUSED(num);
#endif
    return false;
}

static cond_var_t* thread_futex_emu_cond(void* ptr)
{
    size_t k  = (size_t)ptr;
    k        ^= k >> 21;
    k        ^= k >> 17;
    return futex_emu_table[k & (FUTEX_EMU_TABLE_SIZE - 1)];
}

static bool thread_futex_emu_wait(void* ptr, uint32_t val, uint64_t timeout_ns)
{
    cond_var_t* cond  = thread_futex_emu_cond(ptr);
    rvtimer_t   timer = {0};
    rvtimecmp_t cmp   = {0};

    if (atomic_load_uint32(ptr) != val) {
        return false;
    }

    rvtimer_init(&timer, 1000000000ULL);
    rvtimecmp_init(&cmp, &timer);
    rvtimecmp_set(&cmp, timeout_ns);

    do {
        if (atomic_load_uint32(ptr) != val) {
            return true;
        }
        // Wait on shared condvar, with max timeout of 10ms in case we somehow miss a wakeup
        if (condvar_wait_ns(cond, EVAL_MIN(rvtimecmp_delay_ns(&cmp), 10000000ULL))) {
            return true;
        }
    } while (!rvtimecmp_pending(&cmp));

    return false;
}

static slow_path bool thread_futex_init_once(void)
{
    uint32_t tmp = 0;

#if defined(WIN32_FUTEX_IMPL)
    rtl_wait_on_addr        = dlib_get_symbol("ntdll.dll", "RtlWaitOnAddress");
    rtl_wake_by_addr_single = dlib_get_symbol("ntdll.dll", "RtlWakeAddressSingle");
    rtl_wake_by_addr_all    = dlib_get_symbol("ntdll.dll", "RtlWakeAddressAll");
    if (!rtl_wait_on_addr || !rtl_wake_by_addr_single || !rtl_wake_by_addr_all) {
        rtl_wait_on_addr        = NULL;
        rtl_wake_by_addr_single = NULL;
        rtl_wake_by_addr_all    = NULL;
    }
#elif defined(APPLE_FUTEX_IMPL)
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

    // Fallback to futex emulation via shared conditional variable
    // Leaks a condvar handle on library unload, but whatever
    futex_emu_table = safe_new_arr(cond_var_t*, FUTEX_EMU_TABLE_SIZE);
    for (size_t i = 0; i < FUTEX_EMU_TABLE_SIZE; ++i) {
        futex_emu_table[i] = condvar_create();
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
    if (likely(ptr)) {
        if (likely(thread_futex_is_native())) {
            return thread_futex_wait_native(ptr, val, timeout_ns);
        } else {
            return thread_futex_emu_wait(ptr, val, timeout_ns);
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
            cond_var_t* cond = thread_futex_emu_cond(ptr);
            condvar_wake_all(cond);
        }
    }
}

/*
 * Conditional variables
 */

struct cond_var {
    uint32_t flag;
    uint32_t waiters;
#if defined(_WIN32)
    HANDLE event;
    HANDLE timer;
#else
    pthread_cond_t  cond;
    pthread_mutex_t lock;
#endif
};

bool condvar_init_internal(cond_var_t* cond)
{
#if defined(_WIN32)
#if defined(WIN32_WAITABLE_TIMER_IMPL)
    DO_ONCE_SCOPED {
        create_waitable_timer_ex_w = dlib_get_symbol("kernel32.dll", "CreateWaitableTimerExW");
        set_waitable_timer         = dlib_get_symbol("kernel32.dll", "SetWaitableTimer");
    }
    if (create_waitable_timer_ex_w && set_waitable_timer) {
        // Create a high resolution, manual reset waitable timer (Win10 1803+)
        cond->timer = create_waitable_timer_ex_w(NULL, NULL, 0x3, 0x1F0003);
    }
#endif
#if !defined(HOST_64BIT) && !defined(UNDER_CE)
    // Use ANSI syscall (Win9x compat)
    cond->event = CreateEventA(NULL, FALSE, FALSE, NULL);
#else
    cond->event = CreateEventW(NULL, FALSE, FALSE, NULL);
#endif
    return !!cond->event;

#elif defined(PTHREAD_COND_CLOCK_MONOTONIC)
    pthread_condattr_t attr = {0};
    if (pthread_condattr_init(&attr)) {
        return false;
    }
    if (pthread_condattr_setclock(&attr, PTHREAD_COND_CLOCK_MONOTONIC)) {
        return false;
    }
    bool ok = !pthread_cond_init(&cond->cond, &attr) && !pthread_mutex_init(&cond->lock, NULL);
    pthread_condattr_destroy(&attr);
    return ok;

#else
    return !pthread_cond_init(&cond->cond, NULL) && !pthread_mutex_init(&cond->lock, NULL);
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

static bool condvar_wait_native_ns(cond_var_t* cond, uint64_t timeout_ns, uint32_t prev_waiters)
{
    UNUSED(prev_waiters);

#if defined(_WIN32)
    if (timeout_ns == CONDVAR_INFINITE) {
        return !WaitForSingleObject(cond->event, INFINITE);
    }

#if defined(WIN32_WAITABLE_TIMER_IMPL)
    if (timeout_ns < 15000000 && cond->timer && !prev_waiters) {
        // Nanosecond precision timeout using high-resolution WaitableTimer (Win10 1803+)
        LARGE_INTEGER delay = {
            .QuadPart = -(timeout_ns / 100ULL),
        };
        HANDLE handles[] = {
            cond->event,
            cond->timer,
        };

        // For whatever ridiculous reasons, high-resolution timers have much
        // better timing characteristics with NtSetTimerResolution(156250).
        sleep_low_latency(false);

        if (set_waitable_timer(cond->timer, &delay, 0, NULL, NULL, false)) {
            return !WaitForMultipleObjects(STATIC_ARRAY_SIZE(handles), handles, FALSE, INFINITE);
        }
    }
#endif

    // Millisecond-precision timeout
    sleep_low_latency(timeout_ns < 15000000);
    return !WaitForSingleObject(cond->event, EVAL_MAX(timeout_ns / 1000000, 1));

#else

    if (thread_futex_is_native()) {
        // Wait on a native futex when available
        return thread_futex_wait_native(&cond->flag, 0, timeout_ns);
    } else {
        // Use pthread_cond
        bool ret = false;

        pthread_mutex_lock(&cond->lock);
        if (condvar_try_consume_signal(cond)) {
            ret = true;
        } else {
            if (timeout_ns == CONDVAR_INFINITE) {
                ret = !pthread_cond_wait(&cond->cond, &cond->lock);
            } else {
#if defined(PTHREAD_COND_TIMEDWAIT_RELATIVE_IMPL)
                struct timespec ts = {
                    .tv_sec  = timeout_ns / 1000000000,
                    .tv_nsec = timeout_ns % 1000000000,
                };
                ret = pthread_cond_timedwait_relative_np(&cond->cond, &cond->lock, &ts) == 0;
#else
                struct timespec ts = {0};
                condvar_fill_timespec(&ts);
                // Properly handle timespec addition without an overflow
                timeout_ns += ts.tv_nsec;
                ts.tv_sec  += timeout_ns / 1000000000;
                ts.tv_nsec  = timeout_ns % 1000000000;
                ret         = !pthread_cond_timedwait(&cond->cond, &cond->lock, &ts);
#endif
            }
        }
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
        uint32_t prev_waiters = atomic_add_uint32(&cond->waiters, 1);

        // Try to consume a signal again, since condvar_wake() could have been called
        // in-between the fast-path exit and waiter marking.
        if (condvar_try_consume_signal(cond)) {
            atomic_sub_uint32(&cond->waiters, 1);
            return true;
        }

        // Perform waiting on a kernel primitive
        bool ret = condvar_wait_native_ns(cond, timeout_ns, prev_waiters);

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
#if defined(_WIN32)
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
#if defined(_WIN32)
        if (cond->event) {
            CloseHandle(cond->event);
        }
        if (cond->timer) {
            CloseHandle(cond->timer);
        }
#else
        pthread_cond_destroy(&cond->cond);
        pthread_mutex_destroy(&cond->lock);
#endif
        free(cond);
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

        // Yield this thread timeslice
        sleep_ms(0);
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
            return false;
        } else {
            // Another producer stole our task slot, reload the head pointer
            head = atomic_load_uint32_ex(&wq->head, ATOMIC_RELAXED);
        }

        // Yield this thread timeslice
        sleep_ms(0);
    }
    return false;
}

static void thread_workers_terminate(void)
{
    atomic_store_uint32(&pool_run, 0);
    // Wake & shut down all threads properly
    while (atomic_load_uint32(&pool_shut) != WORKER_THREADS) {
        condvar_wake_all(pool_cond);
        sleep_ms(1);
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
