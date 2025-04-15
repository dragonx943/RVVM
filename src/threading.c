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
// Use WaitOnAddress for futexes if present
#include <windows.h>

#include "dlib.h" // For runtime system features probing

#else
// Use pthreads, pthread_cond
// Use Linux futexes if present, even for condvar
#include <pthread.h> // For pthread, pthread_cond, pthread_mutex
#include <time.h>    // For clock_gettime(), if CLOCK_REALTIME/CLOCK_MONOTONIC are defined

#if defined(__linux__) && CHECK_INCLUDE(linux/futex.h, 1) && CHECK_INCLUDE(sys/syscall.h, 1)
// Linux futexes available
#include <linux/futex.h>
#include <sys/syscall.h>
#if !defined(__NR_futex) && defined(__NR_futex_time64)
#define __NR_futex __NR_futex_time64
#endif
#if defined(__NR_futex) && defined(FUTEX_WAIT_PRIVATE) && defined(FUTEX_WAKE_PRIVATE)
#define FUTEX_LINUX_IMPL 1
#endif
#endif

#if defined(__APPLE__)
// Use pthread_cond_timedwait_relative_np()
#define PTHREAD_COND_TIMEDWAIT_RELATIVE_IMPL 1

#else
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
    thread_call_wrap_t* wrap = safe_new_obj(thread_call_wrap_t);
    wrap->func               = func;
    wrap->arg                = arg;

    thread->handle = CreateThread(NULL, stack_size, thread_call_wrapper, wrap, /**/
                                  STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
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
        DWORD tmp = 0;
        WaitForSingleObject(thread->handle, INFINITE);
        if (!GetExitCodeThread(thread->handle, &tmp) || !CloseHandle(thread->handle)) {
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

#if defined(_WIN32) && !defined(UNDER_CE)

// Win32 futexes (Win8+)
static BOOL (*__stdcall wait_on_addr)(void*, void*, size_t, uint32_t) = NULL;
static void (*__stdcall wake_by_addr_single)(void*)                   = NULL;
static void (*__stdcall wake_by_addr_all)(void*)                      = NULL;

#define WIN32_FUTEX_IMPL 1

#endif

static cond_var_t* futex_emu_cond = NULL;

static bool thread_futex_wait_native(void* ptr, uint32_t val, uint64_t timeout_ns)
{
#if defined(WIN32_FUTEX_IMPL)
    uint32_t timeout_ms = INFINITE;
    if (timeout_ns != ((uint64_t)-1)) {
        timeout_ms = timeout_ns / 1000;
    }
    return wait_on_addr(ptr, &val, 4, timeout_ms);
#elif defined(FUTEX_LINUX_IMPL)
    struct timespec  ts     = {0};
    struct timespec* ts_ptr = NULL;
    if (timeout_ns != ((uint64_t)-1)) {
        ts.tv_sec  = timeout_ns / 1000000000ULL;
        ts.tv_nsec = timeout_ns % 1000000000ULL;
        ts_ptr     = &ts;
    }
    return !syscall(__NR_futex, ptr, FUTEX_WAIT_PRIVATE, val, ts_ptr, NULL, 0);
#else
    UNUSED(ptr);
    UNUSED(val);
    UNUSED(timeout_ns);
    return false;
#endif
}

static bool thread_futex_wake_native(void* ptr, uint32_t num)
{
#if defined(WIN32_FUTEX_IMPL)
    if (num > 1) {
        wake_by_addr_all(ptr);
    } else {
        wake_by_addr_single(ptr);
    }
    return true;
#elif defined(FUTEX_LINUX_IMPL)
    return syscall(__NR_futex, ptr, FUTEX_WAKE_PRIVATE, num, NULL, NULL, 0) >= 0;
#else
    UNUSED(ptr);
    UNUSED(num);
    return false;
#endif
}

static bool thread_futex_emu_wait(void* ptr, uint32_t val, uint64_t timeout_ns)
{
    if (timeout_ns != ((uint64_t)-1)) {
        bool        match = true;
        rvtimer_t   timer = {0};
        rvtimecmp_t cmp   = {0};
        rvtimer_init(&timer, 1000000000ULL);
        rvtimecmp_init(&cmp, &timer);
        rvtimecmp_set(&cmp, timeout_ns);

        do {
            match = (atomic_load_uint32(ptr) == val);
            if (match) {
                condvar_wait_ns(futex_emu_cond, rvtimecmp_delay_ns(&cmp));
            }
        } while (match && !rvtimecmp_pending(&cmp));
        return !match;
    } else {
        while (atomic_load_uint32(ptr) == val) {
            condvar_wait_ns(futex_emu_cond, CONDVAR_INFINITE);
        }
        return true;
    }
}

static bool thread_futex_init_once(void)
{
    uint32_t tmp = 0;

#if defined(WIN32_FUTEX_IMPL)
    wait_on_addr        = dlib_get_symbol("api-ms-win-core-synch-l1-2-0.dll", "WaitOnAddress");
    wake_by_addr_single = dlib_get_symbol("api-ms-win-core-synch-l1-2-0.dll", "WakeByAddressSingle");
    wake_by_addr_all    = dlib_get_symbol("api-ms-win-core-synch-l1-2-0.dll", "WakeByAddressAll");
    if (!wait_on_addr || !wake_by_addr_single || !wake_by_addr_all) {
        wait_on_addr        = NULL;
        wake_by_addr_single = NULL;
        wake_by_addr_all    = NULL;
    }
#endif

    if (thread_futex_wake_native(&tmp, 1)) {
        return true;
    }

    // Fallback to futex emulation via shared conditional variable
    // Leaks a condvar handle on library unload, but whatever
    futex_emu_cond = condvar_create();
    return false;
}

static bool thread_futex_is_native(void)
{
    bool native = 0;
    DO_ONCE_SCOPED {
        native = thread_futex_init_once();
    }
    return !!native;
}

bool thread_futex_wait(void* ptr, uint32_t val, uint64_t timeout_ns)
{
    if (likely(ptr)) {
        if (thread_futex_is_native()) {
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
        if (thread_futex_is_native()) {
            thread_futex_wake_native(ptr, num);
        } else {
            condvar_wake_all(futex_emu_cond);
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

#if defined(_WIN32) && !defined(UNDER_CE)

static HANDLE (*__stdcall create_waitable_timer_ex_w)(void*, void*, uint32_t, uint32_t) = NULL;

#endif

bool condvar_init_internal(cond_var_t* cond)
{
#if defined(_WIN32)
#if !defined(UNDER_CE)
    DO_ONCE_SCOPED {
        create_waitable_timer_ex_w = dlib_get_symbol("kernel32.dll", "CreateWaitableTimerExW");
    }
    if (create_waitable_timer_ex_w) {
        // Create a high resolution, manual reset waitable timer (Win10 1803+)
        cond->timer = create_waitable_timer_ex_w(NULL, NULL, 0x3, 0x1F0003);
    }
#endif
    cond->event = CreateEventW(NULL, FALSE, FALSE, NULL);
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
        atomic_store_uint32_relax(&cond->flag, 0);
        return true;
    }
    return false;
}

static bool condvar_wait_native_ns(cond_var_t* cond, uint64_t timeout_ns, uint32_t prev_waiters)
{
    // Enter low-latency sleep mode for delays <15ms
    sleep_low_latency(timeout_ns < 15000000);
    UNUSED(prev_waiters);

#if defined(_WIN32)
    if (timeout_ns == CONDVAR_INFINITE) {
        return !WaitForSingleObject(cond->event, INFINITE);
    }

#if !defined(UNDER_CE)
    if (timeout_ns < 15000000 && cond->timer && !prev_waiters) {
        // Nanosecond precision timeout using high-resolution WaitableTimer (Win10 1803+)
        LARGE_INTEGER delay = {
            .QuadPart = -(timeout_ns / 100ULL),
        };
        HANDLE handles[] = {
            cond->event,
            cond->timer,
        };
        if (SetWaitableTimer(cond->timer, &delay, 0, NULL, NULL, false)) {
            return !WaitForMultipleObjects(STATIC_ARRAY_SIZE(handles), handles, FALSE, INFINITE);
        }
    }
#endif

    // Coarse ms precision timeout
    return !WaitForSingleObject(cond->event, EVAL_MAX(timeout_ns / 1000000, 1));

#else

    if (thread_futex_is_native()) {
        // Use native futexes when available
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

static inline void condvar_mark_signaled(cond_var_t* cond)
{
    atomic_store_uint32_relax(&cond->flag, 1);
}

static void condvar_wake_native(cond_var_t* cond, bool all)
{
#if defined(_WIN32)
    for (uint32_t i = all ? condvar_waiters(cond) : 1; i--;) {
        SetEvent(cond->event);
    }
#else
    if (thread_futex_is_native()) {
        // Wake via native futexes
        thread_futex_wake_native(&cond->flag, all ? -1 : 1);
    } else {
        // We aren't required to signal under the lock, but it should be taken anyways
        // to prevent lost wakeup between cond->flag check and waiting on pthread_cond
        pthread_mutex_lock(&cond->lock);
        pthread_mutex_unlock(&cond->lock);
        if (all) {
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
        condvar_mark_signaled(cond);
        if (condvar_waiters(cond)) {
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
        condvar_mark_signaled(cond);
        if (condvar_waiters(cond)) {
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
