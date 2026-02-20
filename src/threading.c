/*
threading.c - Threading, Futexes
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

// Expose clock_gettime(), pthread_condattr_setclock(), pthread_cond_timedwait_relative_np(), syscall()
#include "feature_test.h" // IWYU pragma: keep

#include "atomics.h"
#include "threading.h"
#include "utils.h"

#include "dlib.h"     // IWYU pragma: keep
#include "rvtimer.h"  // IWYU pragma: keep
#include "spinlock.h" // IWYU pragma: keep
#include "vector.h"   // IWYU pragma: keep

PUSH_OPTIMIZATION_SIZE

#if defined(_MSC_VER)
#include <intrin.h> // For _mm_pause()
#endif

/*
 * Determine threading implementation
 */

#if defined(USE_THREAD_EMU)
// Use voluntary preemption in a single thread
#elif defined(HOST_TARGET_WIN32)
// Use Win32 threads & events
#include <windows.h>
#define WIN32_THREADS_IMPL 1
#elif defined(HOST_TARGET_POSIX) && HOST_TARGET_POSIX >= 199506L
// Use pthreads
#include <pthread.h>
#include <time.h>
#define POSIX_THREADS_IMPL 1
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112LL && !defined(__STDC_NO_THREADS__) /**/                  \
    && CHECK_INCLUDE(threads.h, 1) && !defined(HOST_TARGET_DOS) && !defined(HOST_TARGET_POSIX)
// Use C11 threads
#include <threads.h>
#include <time.h>
#define C11_THREADS_IMPL 1
#elif CHECK_INCLUDE(SDL.h, 1)
// Use SDL threads
#include <SDL.h>
#define SDL_THREADS_IMPL 1
#else
#error No threading implementation found, please rebuild with USE_THREAD_EMU=1
#endif

#if defined(HOST_TARGET_POSIX) && HOST_TARGET_POSIX >= 199506L && CHECK_INCLUDE(sched.h, 1)
// Use sched_yield()
#include <sched.h>
#define SCHED_YIELD_IMPL 1
#endif

/*
 * Probe for native futex support
 */

#if !defined(USE_FUTEX_EMU) && !defined(USE_THREAD_EMU)
#if defined(HOST_TARGET_LINUX) && CHECK_INCLUDE(sys/syscall.h, 1)

// Use Linux futexes (Linux 2.6.22+)
#include <sys/syscall.h> // For __NR_futex, __NR_futex_time64
#if !defined(__NR_futex_time64) && defined(__NR_futex)
#define __NR_futex_time64 __NR_futex
#endif
#if defined(__NR_epoll_create) && CHECK_INCLUDE(linux/futex.h, 1)
#include <linux/futex.h> // For FUTEX_WAIT_PRIVATE, FUTEX_WAKE_PRIVATE
#endif
#if defined(__NR_futex_time64) && defined(FUTEX_WAIT_PRIVATE) && defined(FUTEX_WAKE_PRIVATE)
#include <errno.h>
#include <unistd.h> // For syscall()
#define LINUX_FUTEX_IMPL  1
#define NATIVE_FUTEX_IMPL 1
#endif

#elif defined(HOST_TARGET_FREEBSD) && HOST_TARGET_FREEBSD >= 11 && CHECK_INCLUDE(sys/umtx.h, 0)

// Use FreeBSD futexes (_umtx_op(), FreeBSD 11.0+)
#include <errno.h>
#include <sys/types.h>
#include <sys/umtx.h>
#if defined(UMTX_OP_WAIT_UINT) && defined(UMTX_OP_WAKE)
#define FREEBSD_FUTEX_IMPL 1
#define NATIVE_FUTEX_IMPL  1
#endif

#elif defined(HOST_TARGET_OPENBSD) && CHECK_INCLUDE(sys/futex.h, 0)

// Use OpenBSD futexes (OpenBSD 6.2+)
#include <errno.h>
#include <sys/futex.h>
#include <sys/time.h>
#if defined(FUTEX_WAIT) && defined(FUTEX_WAKE)
#define OPENBSD_FUTEX_IMPL 1
#define NATIVE_FUTEX_IMPL  1
#endif

#elif defined(HOST_TARGET_DRAGONFLYBSD)

// Use DragonFlyBSD futexes (DragonFlyBSD 1.1+)
#include <unistd.h>
#define DRAGONFLY_FUTEX_IMPL 1
#define NATIVE_FUTEX_IMPL    1

#elif defined(HOST_TARGET_DARWIN)

// Probe Mac OS futexes (__ulock_wait(), OS X 10.12+)
// Use pthread_cond_timedwait_relative_np()
#include <errno.h>
#define ULOCK_CMP_WAIT    0x0001 // Compare and wait on a 32-bit value
#define ULOCK_WAKE_ONE    0x0001 // Wake one thread
#define ULOCK_WAKE_ALL    0x0101 // Wake all threads
static int (*ulock_wait)(uint32_t op, void* ptr, uint64_t val, uint32_t us) = NULL;
static int (*ulock_wake)(uint32_t op, void* ptr, uint64_t unused)           = NULL;
#define APPLE_FUTEX_IMPL  1
#define HYBRID_FUTEX_IMPL 1

#elif defined(__wasm__) && GNU_BUILTIN(__builtin_wasm_memory_atomic_wait32)

// Use WASM futexes (__builtin_wasm_memory_atomic_wait32)
#define WASM_FUTEX_IMPL   1
#define NATIVE_FUTEX_IMPL 1

#endif
#endif

/*
 * Probe for futex emulation helpers
 */

#if !defined(NATIVE_FUTEX_IMPL) && defined(HOST_TARGET_POSIX)
#if defined(USE_FUTEX_OVER_PIPE)
// Use pipe(), select(), close() for futex emulation
#include <sys/select.h>
#include <unistd.h>
#elif defined(CLOCK_MONOTONIC) && defined(HOST_TARGET_POSIX) && HOST_TARGET_POSIX >= 200809L
// Use clock_gettime() for pthread_cond time reference
#define CLOCK_GETTIME_IMPL 1
#elif !defined(HOST_TARGET_DARWIN)
// Use gettimeofday() for pthread_cond time reference
#include <sys/time.h>
#endif
#endif

/*
 * Threads
 */

struct thread_ctx {
#if defined(WIN32_THREADS_IMPL)
    HANDLE handle;
#elif defined(POSIX_THREADS_IMPL)
    pthread_t pthread;
#elif defined(C11_THREADS_IMPL)
    thrd_t thrd;
#elif defined(SDL_THREADS_IMPL)
    SDL_Thread* thread;
#endif
};

#if defined(WIN32_THREADS_IMPL)
#define THREAD_RET DWORD
#define THREAD_ABI __stdcall
#elif defined(C11_THREADS_IMPL) || defined(SDL_THREADS_IMPL)
#define THREAD_RET int
#define THREAD_ABI
#endif

#if defined(THREAD_RET)
typedef struct {
    thread_func_t func;
    void*         arg;
} func_wrap_t;

static void* func_wrap(thread_func_t func, void* arg)
{
    func_wrap_t* wrap = safe_new_obj(func_wrap_t);
    // Wrap our thread function call to bridge ABIs
    wrap->func = func;
    wrap->arg  = arg;
    return (void*)wrap;
}

static THREAD_RET THREAD_ABI func_unwrap(void* arg)
{
    func_wrap_t wrap = *(func_wrap_t*)arg;
    safe_free(arg);
    return (THREAD_RET)(size_t)wrap.func(wrap.arg);
}
#endif

thread_ctx_t* thread_create_ex(thread_func_t func, void* arg, uint32_t stack_size)
{
#if defined(USE_THREAD_EMU)
    UNUSED(func && arg && stack_size);
#else
    thread_ctx_t* thread = safe_new_obj(thread_ctx_t);
    bool          result = false;
#if defined(SANITIZERS_ENABLED)
    // Sanitizers consume a lot of extra stack space, use default stack size
    stack_size = 0;
#endif
#if defined(WIN32_THREADS_IMPL)
    // Win9x: Passing NULL for the 'lpThreadId' parameter causes the function to fail
    DWORD tid      = 0;
    thread->handle = CreateThread(NULL, stack_size, func_unwrap, func_wrap(func, arg), /**/
                                  STACK_SIZE_PARAM_IS_A_RESERVATION, &tid);
    result         = !!thread->handle;
#elif defined(POSIX_THREADS_IMPL)
    pthread_attr_t* aptr = NULL;
    pthread_attr_t  attr = ZERO_INIT;
    if (stack_size && !pthread_attr_init(&attr) && !pthread_attr_setstacksize(&attr, stack_size)) {
        // Pass custom stack size if possible
        aptr = &attr;
    }
    result = !pthread_create(&thread->pthread, aptr, func, arg);
    if (aptr) {
        pthread_attr_destroy(aptr);
    }
#elif defined(C11_THREADS_IMPL)
    result = thrd_create(&thread->thrd, func_unwrap, func_wrap(func, arg)) == thrd_success;
#elif defined(SDL_THREADS_IMPL)
    thread->thread = SDL_CreateThread(func_unwrap, func_wrap(func, arg));
    result         = !!thread->thread;
#endif
    UNUSED(stack_size);
    if (result) {
        return thread;
    }
    rvvm_warn("Failed to spawn thread %p", (void*)func);
    safe_free(thread);
#endif
    return NULL;
}

thread_ctx_t* thread_create(thread_func_t func, void* arg)
{
    return thread_create_ex(func, arg, 0x10000);
}

bool thread_join(thread_ctx_t* thread)
{
    bool result = false;
#if defined(USE_THREAD_EMU)
    UNUSED(thread);
#else
    if (thread) {
#if defined(WIN32_THREADS_IMPL)
        result = !WaitForSingleObject(thread->handle, INFINITE) && CloseHandle(thread->handle);
#elif defined(POSIX_THREADS_IMPL)
        void* tmp = NULL;
        result    = !pthread_join(thread->pthread, &tmp);
#elif defined(C11_THREADS_IMPL)
        int tmp = 0;
        result  = thrd_join(thread->thrd, &tmp) == thrd_success;
#elif defined(SDL_THREADS_IMPL)
        int tmp = 0;
        SDL_WaitThread(thread->thread, &tmp);
        result = true;
#endif
        safe_free(thread);
        if (!result) {
            rvvm_warn("Failed to join thread");
        }
    }
#endif
    return result;
}

/*
 * Thread yielding, CPU relax hints
 */

void thread_sched_yield(void)
{
#if defined(WIN32_THREADS_IMPL)
    Sleep(0);
#elif defined(SCHED_YIELD_IMPL)
    sched_yield();
#elif defined(C11_THREADS_IMPL)
    thrd_yield();
#endif
}

void thread_cpu_relax(void)
{
#if defined(GNU_EXTS) && (defined(__i386__) || defined(__x86_64__))
    __asm__ __volatile__("pause" : : : "memory");
#elif defined(GNU_EXTS) && defined(__aarch64__)
    __asm__ __volatile__("isb sy" : : : "memory");
#elif defined(GNU_EXTS) && defined(__riscv)
    __asm__ __volatile__(".4byte 0x100000F" : : : "memory");
#elif defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    _mm_pause();
#else
    atomic_fence_ex(ATOMIC_SEQ_CST);
#endif
}

#if !defined(NATIVE_FUTEX_IMPL) && !defined(USE_THREAD_EMU)

/*
 * Host-specific futex emulation helpers
 */

#if defined(WIN32_THREADS_IMPL)

#if defined(HOST_TARGET_WINNT) && defined(THREAD_LOCAL) && !defined(USE_MINIMAL) /**/                                  \
    && ((GNU_ATTRIBUTE(__used__) && GNU_ATTRIBUTE(__section__)) || defined(COMPILER_IS_MSVC))

/*
 * Precise sub-millisecond delay/timeout using high-resolution WaitableTimer (Win10 1803+)
 *
 * For whatever ridiculous reasons, high-resolution timers have observably worse
 * timing characteristics when historical "low-latency" scheduler mode is enabled
 * via NtSetTimerResolution(5000). Seemingly any sleep larger than a scheduler
 * tick is rounded/slacked to the next tick.
 *
 * Prefer to use NtSetTimerResolution(156250) for power saving reasons, and use
 * high-resolution waitable timers for anything that requires actual precise delays.
 */

#define TLS_HANDLE_IMPL 1

// High-resolution waitable timers, Win10 1803+
static HANDLE (*__stdcall create_wtimer_ex)(void*, const void*, DWORD, DWORD)                 = NULL;
static BOOL   (*__stdcall set_wtimer)(HANDLE, const LARGE_INTEGER*, LONG, void*, void*, BOOL) = NULL;

// Scheduler resoluton handling via NtSetTimerResolution()
static int32_t (*__stdcall nt_set_timeres)(ULONG, BOOLEAN, PULONG) = NULL;

static bool tls_event_avail = false;
static bool tls_timer_avail = false;

static THREAD_LOCAL HANDLE tls_event = NULL;
static THREAD_LOCAL HANDLE tls_timer = NULL;

static BOOL __stdcall dll_callback(void* hdll, DWORD reason, void* aux)
{
    UNUSED(hdll && aux);
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            tls_event_avail = true;
            tls_timer_avail = true;
            break;
        case DLL_PROCESS_DETACH:
        case DLL_THREAD_DETACH:
            if (tls_event) {
                CloseHandle(tls_event);
                tls_event = NULL;
            }
            if (tls_timer) {
                CloseHandle(tls_timer);
                tls_timer = NULL;
            }
            break;
    }
    return TRUE;
}

#if defined(COMPILER_IS_MSVC)
#pragma const_seg(".CRT$XLB")
static void* const volatile dll_callback_ptr = dll_callback;
#pragma const_seg()
#else
static void* const volatile dll_callback_ptr __attribute__((__used__, __section__(".CRT$XLB"))) = dll_callback;
#endif

static inline void tls_timer_set(HANDLE timer, uint64_t ns)
{
    LARGE_INTEGER delay = {
        .QuadPart = -(ns / 100ULL),
    };
    set_wtimer(timer, &delay, 0, NULL, NULL, false);
}

static slow_path HANDLE tls_timer_init(void)
{
    HANDLE timer = NULL;
    if (!create_wtimer_ex || !set_wtimer) {
        create_wtimer_ex = dlib_get_symbol("kernel32.dll", "CreateWaitableTimerExW");
        set_wtimer       = dlib_get_symbol("kernel32.dll", "SetWaitableTimer");
    }
    if (create_wtimer_ex && set_wtimer) {
        // Create a high-resolution, manual reset waitable timer (Win10 1803+)
        timer = create_wtimer_ex(NULL, NULL, 0x3, 0x1F0003);
        if (timer == INVALID_HANDLE_VALUE) {
            timer = NULL;
        }
        if (timer) {
            return timer;
        }
        // Create a manual reset waitable timer (Win Vista+)
        timer = create_wtimer_ex(NULL, NULL, 0x1, 0x1F0003);
        if (timer == INVALID_HANDLE_VALUE) {
            timer = NULL;
        }
    }
    tls_timer_avail = !!timer;
    if (!nt_set_timeres) {
        // Boost scheduler resolution if high-resolution timers are missing
        nt_set_timeres = dlib_get_symbol("ntdll.dll", "NtSetTimerResolution");
        if (nt_set_timeres) {
            ULONG cur = 0;
            nt_set_timeres(5000, TRUE, &cur);
        }
    }
    return timer;
}

#endif

static HANDLE thread_local_event(void)
{
    HANDLE event = NULL;
#if defined(TLS_HANDLE_IMPL)
    event = tls_event;
    if (likely(event)) {
        return event;
    }
#endif
#if defined(HOST_TARGET_WIN9X)
    // Use ANSI syscall (Win9x compat)
    event = CreateEventA(NULL, FALSE, FALSE, NULL);
#else
    event = CreateEventW(NULL, FALSE, FALSE, NULL);
#endif
    if (event == INVALID_HANDLE_VALUE) {
        event = NULL;
    }
#if defined(TLS_HANDLE_IMPL)
    if (tls_event_avail) {
        tls_event = event;
    }
#endif
    return event;
}

static void thread_local_event_dispose(HANDLE event)
{
#if defined(TLS_HANDLE_IMPL)
    if (tls_event_avail) {
        ResetEvent(event);
        return;
    }
#endif
    if (event) {
        CloseHandle(event);
    }
}

HANDLE thread_local_waitable_timer(uint64_t ns)
{
#if defined(TLS_HANDLE_IMPL)
    HANDLE timer = tls_timer;
    if (unlikely(!timer && tls_timer_avail)) {
        timer     = tls_timer_init();
        tls_timer = timer;
    }
    if (likely(timer)) {
        tls_timer_set(timer, ns);
        return timer;
    }
#endif
    UNUSED(ns);
    return NULL;
}

#elif defined(POSIX_THREADS_IMPL) && !defined(USE_FUTEX_OVER_PIPE)

static void* pcond_attr_monotonic(void)
{
    static void* attr_ptr = NULL;
#if defined(CLOCK_GETTIME_IMPL)
    if (!attr_ptr) {
        static pthread_condattr_t attr = ZERO_INIT;
        if (!pthread_condattr_init(&attr) && !pthread_condattr_setclock(&attr, CLOCK_MONOTONIC)) {
            attr_ptr = &attr;
        }
    }
#endif
    return attr_ptr;
}

static int pcond_timedwait_ns(pthread_cond_t* cond, pthread_mutex_t* mtx, uint64_t ns)
{
    if (ns == ((uint64_t)-1)) {
        return pthread_cond_wait(cond, mtx);
    } else {
#if defined(HOST_TARGET_DARWIN)
        struct timespec ts = {
            .tv_sec  = ns / 1000000000ULL,
            .tv_nsec = ns % 1000000000ULL,
        };
        return pthread_cond_timedwait_relative_np(cond, mtx, &ts);
#else
        struct timespec ts = ZERO_INIT;
#if defined(CLOCK_GETTIME_IMPL)
        if (pcond_attr_monotonic()) {
            clock_gettime(CLOCK_MONOTONIC, &ts);
        } else {
            // Fallback to CLOCK_REALTIME (Possibly a very old Linux kernel)
            clock_gettime(CLOCK_REALTIME, &ts);
        }
#else
        // Some targets lack clock_gettime(), use gettimeofday()
        struct timeval tv = ZERO_INIT;
        gettimeofday(&tv, NULL);
        ts.tv_sec  = tv.tv_sec;
        ts.tv_nsec = tv.tv_usec * 1000U;
#endif
        ns += ts.tv_nsec;
        // Properly handle timespec addition without an overflow
        ts.tv_sec  += ns / 1000000000ULL;
        ts.tv_nsec  = ns % 1000000000ULL;
        return pthread_cond_timedwait(cond, mtx, &ts);
#endif
    }
}

#endif

/*
 * Futex emulation glue
 */

typedef struct {
    void* ptr;
#if defined(WIN32_THREADS_IMPL)
    HANDLE event;
#elif defined(USE_FUTEX_OVER_PIPE)
    int pipe[2];
#elif defined(POSIX_THREADS_IMPL)
    pthread_cond_t cond;
#elif defined(C11_THREADS_IMPL)
    cnd_t cnd;
#elif defined(SDL_THREADS_IMPL)
    SDL_cond* cond;
#endif
} futex_entry_t;

typedef struct {
    vector_t(futex_entry_t*) waiters;
#if defined(POSIX_THREADS_IMPL)
    pthread_mutex_t mutex;
#elif defined(C11_THREADS_IMPL)
    mtx_t mtx;
#elif defined(SDL_THREADS_IMPL)
    SDL_mutex* mutex;
#else
    spinlock_t lock;
#endif
} futex_queue_t;

#if defined(POSIX_THREADS_IMPL)
#define FUTEX_EMU_ENTRY_INIT {ZERO_INIT, PTHREAD_MUTEX_INITIALIZER}
#else
#define FUTEX_EMU_ENTRY_INIT ZERO_INIT
#endif

static futex_queue_t futex_emu_table[8] = {
    FUTEX_EMU_ENTRY_INIT, FUTEX_EMU_ENTRY_INIT, FUTEX_EMU_ENTRY_INIT, FUTEX_EMU_ENTRY_INIT,
    FUTEX_EMU_ENTRY_INIT, FUTEX_EMU_ENTRY_INIT, FUTEX_EMU_ENTRY_INIT, FUTEX_EMU_ENTRY_INIT,
};

static void futex_emu_init(void)
{
#if defined(C11_THREADS_IMPL) || defined(SDL_THREADS_IMPL)
    DO_ONCE_SCOPED {
        for (size_t i = 0; i < STATIC_ARRAY_SIZE(futex_emu_table); ++i) {
#if defined(C11_THREADS_IMPL)
            mtx_init(&futex_emu_table[i].mtx, mtx_plain);
#elif defined(SDL_THREADS_IMPL)
            futex_emu_table[i].mutex = SDL_CreateMutex();
#endif
        }
    }
#endif
}

static inline void futex_queue_lock(futex_queue_t* queue)
{
#if defined(POSIX_THREADS_IMPL)
    pthread_mutex_lock(&queue->mutex);
#elif defined(C11_THREADS_IMPL)
    mtx_lock(&queue->mtx);
#elif defined(SDL_THREADS_IMPL)
    SDL_LockMutex(queue->mutex);
#else
    spin_lock_busy_loop(&queue->lock);
#endif
}

static inline void futex_queue_unlock(futex_queue_t* queue)
{
#if defined(POSIX_THREADS_IMPL)
    pthread_mutex_unlock(&queue->mutex);
#elif defined(C11_THREADS_IMPL)
    mtx_unlock(&queue->mtx);
#elif defined(SDL_THREADS_IMPL)
    SDL_UnlockMutex(queue->mutex);
#else
    spin_unlock_busy_loop(&queue->lock);
#endif
}

static inline futex_entry_t* futex_entry_init(void* ptr)
{
    futex_entry_t* waiter = safe_new_obj(futex_entry_t);
#if defined(WIN32_THREADS_IMPL)
    waiter->event = thread_local_event();
#elif defined(USE_FUTEX_OVER_PIPE)
    pipe(waiter->pipe);
#elif defined(POSIX_THREADS_IMPL)
    pthread_cond_init(&waiter->cond, pcond_attr_monotonic());
#elif defined(C11_THREADS_IMPL)
    cnd_init(&waiter->cnd);
#elif defined(SDL_THREADS_IMPL)
    waiter->cond = SDL_CreateCond();
#endif
    waiter->ptr = ptr;
    return waiter;
}

static inline void futex_entry_wait(futex_queue_t* queue, futex_entry_t* waiter, uint64_t ns)
{
#if defined(WIN32_THREADS_IMPL)
    futex_queue_unlock(queue);
    if (ns == ((uint64_t)-1) && waiter->event) {
        // Wait infinitely
        WaitForSingleObject(waiter->event, INFINITE);
    } else if (waiter->event) {
        HANDLE timer = thread_local_waitable_timer(ns);
        if (timer) {
            // High-resolution timeout via waitable timer
            HANDLE hndls[] = {waiter->event, timer};
            size_t hndls_l = STATIC_ARRAY_SIZE(hndls);
            // Wait on both event & timer handles
            WaitForMultipleObjects(hndls_l, hndls, FALSE, INFINITE);
        } else {
            // Fallback to millisecond-precision timeout
            WaitForSingleObject(waiter->event, EVAL_MAX(ns / 1000000ULL, 1));
        }
    }
    futex_queue_lock(queue);
#elif defined(USE_FUTEX_OVER_PIPE)
    futex_queue_unlock(queue);
    if (waiter->pipe[0]) {
        struct timeval* tp = NULL;
        struct timeval  tv = ZERO_INIT;
        fd_set          fds;
        if (ns != ((uint64_t)-1)) {
            tv.tv_sec  = ns / 1000000000ULL;
            tv.tv_usec = (ns % 1000000000ULL) / 1000ULL;
            tp         = &tv;
        }
        FD_ZERO(&fds);
        FD_SET(waiter->pipe[0], &fds);
        select(waiter->pipe[0] + 1, &fds, NULL, NULL, tp);
    }
    futex_queue_lock(queue);
#elif defined(POSIX_THREADS_IMPL)
    pcond_timedwait_ns(&waiter->cond, &queue->mutex, ns);
#elif defined(C11_THREADS_IMPL)
    if (ns == ((uint64_t)-1)) {
        cnd_wait(&waiter->cnd, &queue->mtx);
    } else {
        struct timespec ts = ZERO_INIT;
        timespec_get(&ts, TIME_UTC);
        ns         += ts.tv_nsec;
        ts.tv_sec  += ns / 1000000000ULL;
        ts.tv_nsec  = ns % 1000000000ULL;
        cnd_timedwait(&waiter->cnd, &queue->mtx, &ts);
    }
#elif defined(SDL_THREADS_IMPL)
    if (ns == ((uint64_t)-1)) {
        SDL_CondWait(waiter->cond, queue->mutex);
    } else {
        SDL_CondWaitTimeout(waiter->cond, queue->mutex, EVAL_MAX(ns / 1000000ULL, 1));
    }
#endif
}

static inline void futex_entry_wake(futex_entry_t* waiter)
{
#if defined(WIN32_THREADS_IMPL)
    if (waiter->event) {
        SetEvent(waiter->event);
    }
#elif defined(USE_FUTEX_OVER_PIPE)
    if (waiter->pipe[1]) {
        close(waiter->pipe[1]);
        waiter->pipe[1] = 0;
    }
#elif defined(POSIX_THREADS_IMPL)
    pthread_cond_signal(&waiter->cond);
#elif defined(C11_THREADS_IMPL)
    cnd_signal(&waiter->cnd);
#elif defined(SDL_THREADS_IMPL)
    SDL_CondSignal(waiter->cond);
#endif
}

static inline void futex_entry_free(futex_entry_t* waiter)
{
#if defined(WIN32_THREADS_IMPL)
    thread_local_event_dispose(waiter->event);
#elif defined(USE_FUTEX_OVER_PIPE)
    if (waiter->pipe[0]) {
        close(waiter->pipe[0]);
    }
    if (waiter->pipe[1]) {
        close(waiter->pipe[1]);
    }
#elif defined(POSIX_THREADS_IMPL)
    pthread_cond_destroy(&waiter->cond);
#elif defined(C11_THREADS_IMPL)
    cnd_destroy(&waiter->cnd);
#elif defined(SDL_THREADS_IMPL)
    SDL_DestroyCond(waiter->cond);
#endif
    safe_free(waiter);
}

/*
 * Futex emulation core
 */

static inline futex_queue_t* futex_get_queue(void* ptr)
{
    size_t k  = (size_t)ptr;
    k        ^= k >> 21;
    k        ^= k >> 17;
    return &futex_emu_table[k & (STATIC_ARRAY_SIZE(futex_emu_table) - 1)];
}

static uint32_t thread_futex_emu_wait(void* ptr, uint32_t val, uint64_t ns)
{
    futex_queue_t* queue = futex_get_queue(ptr);
    futex_entry_t* self  = futex_entry_init(ptr);
    uint32_t       wake  = THREAD_FUTEX_MISMATCH;

    futex_emu_init();
    futex_queue_lock(queue);
    if (atomic_load_uint32_relax(ptr) == val) {
        vector_push_back(queue->waiters, self);
        futex_entry_wait(queue, self, ns);
        if (!self->ptr) {
            wake = THREAD_FUTEX_WAKEUP;
        } else {
            wake = THREAD_FUTEX_TIMEOUT;
            vector_foreach_back (queue->waiters, i) {
                if (vector_at(queue->waiters, i) == self) {
                    vector_erase(queue->waiters, i);
                    break;
                }
            }
        }
    }
    futex_queue_unlock(queue);
    futex_entry_free(self);

    return wake;
}

static uint32_t thread_futex_emu_wake(void* ptr, uint32_t num)
{
    futex_queue_t* queue = futex_get_queue(ptr);
    uint32_t       wake  = 0;

    futex_emu_init();
    futex_queue_lock(queue);
    vector_foreach_back (queue->waiters, i) {
        futex_entry_t* waiter = vector_at(queue->waiters, i);
        if (wake >= num) {
            break;
        }
        if (waiter->ptr == ptr) {
            waiter->ptr = NULL;
            vector_erase(queue->waiters, i);
            futex_entry_wake(waiter);
            wake++;
        }
    }
    futex_queue_unlock(queue);

    return wake;
}

#endif

/*
 * Native futexes
 */

#if defined(NATIVE_FUTEX_IMPL) || defined(HYBRID_FUTEX_IMPL)

#define FUTEX_RET(syscall, error)                                                                                      \
    if ((syscall) >= 0) {                                                                                              \
        return THREAD_FUTEX_WAKEUP;                                                                                    \
    } else if (errno == error) {                                                                                       \
        return THREAD_FUTEX_MISMATCH;                                                                                  \
    }

static uint32_t thread_futex_native_wait(void* ptr, uint32_t val, uint64_t ns)
{
#if defined(LINUX_FUTEX_IMPL) || defined(FREEBSD_FUTEX_IMPL) || defined(OPENBSD_FUTEX_IMPL)
    // Prepare timespec structure
    struct timespec  ts = ZERO_INIT;
    struct timespec* tp = NULL;
    if (ns != ((uint64_t)-1)) {
        ts.tv_sec  = ns / 1000000000ULL;
        ts.tv_nsec = ns % 1000000000ULL;
        tp         = &ts;
    }
#elif defined(DRAGONFLY_FUTEX_IMPL) || defined(APPLE_FUTEX_IMPL)
    // Prepare microsecond timeout, zero means infinity
    uint64_t us = 0;
    if (ns != ((uint64_t)-1)) {
        us = ns / 1000ULL;
        us = EVAL_MIN(us, 0x7FFFFFFFU);
        us = EVAL_MAX(us, 1);
    }
#endif

#if defined(LINUX_FUTEX_IMPL)
    sleep_low_latency(true);
    FUTEX_RET(syscall(__NR_futex_time64, ptr, FUTEX_WAIT_PRIVATE, val, tp, NULL, 0), EAGAIN)
#elif defined(FREEBSD_FUTEX_IMPL)
    void* ts_size = tp ? ((void*)(size_t)sizeof(ts)) : NULL;
    FUTEX_RET(_umtx_op(ptr, UMTX_OP_WAIT_UINT, val, ts_size, tp), EAGAIN)
#elif defined(OPENBSD_FUTEX_IMPL)
    FUTEX_RET(futex(ptr, FUTEX_WAIT, val, tp, NULL), EAGAIN)
#elif defined(DRAGONFLY_FUTEX_IMPL)
    FUTEX_RET(umtx_sleep(ptr, val, us), EBUSY)
#elif defined(APPLE_FUTEX_IMPL)
    if (ulock_wait) {
        FUTEX_RET(ulock_wait(ULOCK_CMP_WAIT, ptr, val, us), EAGAIN)
    }
#elif defined(WASM_FUTEX_IMPL)
    switch (__builtin_wasm_memory_atomic_wait32(ptr, val, ns)) {
        case 0:
            return THREAD_FUTEX_WAKEUP;
        case 1:
            return THREAD_FUTEX_MISMATCH;
    }
#endif
    UNUSED(ptr && val && ns);
    return THREAD_FUTEX_TIMEOUT;
}

static bool thread_futex_native_wake(void* ptr, uint32_t num)
{
#if defined(LINUX_FUTEX_IMPL)
    return syscall(__NR_futex_time64, ptr, FUTEX_WAKE_PRIVATE, num, NULL, NULL, 0) >= 0;
#elif defined(FREEBSD_FUTEX_IMPL)
    return _umtx_op(ptr, UMTX_OP_WAKE, num, NULL, NULL) >= 0;
#elif defined(OPENBSD_FUTEX_IMPL)
    return futex(ptr, FUTEX_WAKE, num, NULL, NULL) >= 0;
#elif defined(DRAGONFLY_FUTEX_IMPL)
    return umtx_wakeup(ptr, num) >= 0;
#elif defined(APPLE_FUTEX_IMPL)
    return ulock_wake && ulock_wake((num > 1) ? ULOCK_WAKE_ALL : ULOCK_WAKE_ONE, ptr, 0) >= 0;
#elif defined(WASM_FUTEX_IMPL)
    return __builtin_wasm_memory_atomic_notify(ptr, num) >= 0;
#else
    UNUSED(ptr && num);
    return false;
#endif
}

#endif

/*
 * Hybrid futexes (Runtime probing with emulation fallback)
 */

#if defined(HYBRID_FUTEX_IMPL)

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
    return thread_futex_native_wake(&tmp, 1);
}

static bool thread_futex_is_native(void)
{
    static bool futex_native = false;
    DO_ONCE_SCOPED {
        futex_native = thread_futex_init_once();
    }
    return futex_native;
}

#endif

/*
 * Futexes
 */

uint32_t thread_futex_wait(void* ptr, uint32_t val, uint64_t timeout_ns)
{
    if (likely(ptr && timeout_ns)) {
#if defined(USE_THREAD_EMU)
        if (atomic_load_uint32(ptr) != val) {
            return THREAD_FUTEX_MISMATCH;
        }
        sleep_ns(timeout_ns);
#elif defined(NATIVE_FUTEX_IMPL)
        return thread_futex_native_wait(ptr, val, timeout_ns);
#else
#if defined(HYBRID_FUTEX_IMPL)
        if (likely(thread_futex_is_native())) {
            return thread_futex_native_wait(ptr, val, timeout_ns);
        }
#endif
        return thread_futex_emu_wait(ptr, val, timeout_ns);
#endif
    }
    return THREAD_FUTEX_TIMEOUT;
}

void thread_futex_wake(void* ptr, uint32_t num)
{
    UNUSED(ptr && num);
#if !defined(USE_THREAD_EMU)
    if (likely(ptr && num)) {
#if defined(NATIVE_FUTEX_IMPL)
        thread_futex_native_wake(ptr, num);
#else
#if defined(HYBRID_FUTEX_IMPL)
        if (likely(thread_futex_is_native())) {
            thread_futex_native_wake(ptr, num);
            return;
        }
#endif
        thread_futex_emu_wake(ptr, num);
#endif
    }
#endif
}

/*
 * Events
 */

static inline bool event_update_flag(thread_event_t* event, uint32_t flag)
{
    if (atomic_load_uint32_relax(&event->flag) != flag) {
        return atomic_swap_uint32(&event->flag, flag) != flag;
    }
    return false;
}

void thread_event_init(thread_event_t* event)
{
    if (likely(event)) {
        atomic_store_uint32_relax(&event->flag, 0);
        atomic_store_uint32_relax(&event->waiters, 0);
    }
}

bool thread_event_wait(thread_event_t* event, uint64_t timeout_ns)
{
    bool woken = false;
    if (likely(event)) {
        if (event_update_flag(event, 0)) {
            // Fast-path exit on an already signaled event
            return true;
        } else if (unlikely(!timeout_ns)) {
            // Fast-path exit for no timeout
            return false;
        }

        // Mark that a thread is about to be waiting here
        atomic_add_uint32(&event->waiters, 1);
        if (!event_update_flag(event, 0)) {
            // Perform waiting on a kernel primitive
            woken = !!thread_futex_wait(&event->flag, 0, timeout_ns);

            // Clear possibly danging signaled flag
            woken = event_update_flag(event, 0) || woken;
        }
        atomic_sub_uint32(&event->waiters, 1);
    }
    return woken;
}

bool thread_event_wake(thread_event_t* event)
{
    if (likely(event)) {
        if (event_update_flag(event, 1)) {
            if (thread_event_waiters(event)) {
                thread_futex_wake(&event->flag, 1);
            }
            return true;
        }
    }
    return false;
}

uint32_t thread_event_waiters(thread_event_t* event)
{
    if (likely(event)) {
        return atomic_load_uint32_relax(&event->waiters);
    }
    return 0;
}

/*
 * Condvars (Legacy)
 */

cond_var_t* condvar_create(void)
{
    return safe_new_obj(cond_var_t);
}

bool condvar_wait_ns(cond_var_t* cond, uint64_t timeout_ns)
{
    return thread_event_wait(cond, timeout_ns);
}

bool condvar_wait(cond_var_t* cond, uint64_t timeout_ms)
{
    uint64_t timeout_ns = CONDVAR_INFINITE;
    if (timeout_ms != CONDVAR_INFINITE) {
        timeout_ns = timeout_ms * 1000000ULL;
    }
    return condvar_wait_ns(cond, timeout_ns);
}

bool condvar_wake(cond_var_t* cond)
{
    return thread_event_wake(cond);
}

uint32_t condvar_waiters(cond_var_t* cond)
{
    return thread_event_waiters(cond);
}

void condvar_free(cond_var_t* cond)
{
    if (likely(cond)) {
        uint32_t waiters = condvar_waiters(cond);
        if (waiters) {
            rvvm_warn("Destroying a condvar with %u waiters!", waiters);
        }
        safe_free(cond);
    }
}

/*
 * Threadpool
 */

#if defined(USE_THREAD_EMU)

void thread_create_task(thread_func_t func, void* arg)
{
    func(arg);
}

void thread_create_task_va(thread_func_va_t func, void** args, unsigned arg_count)
{
    UNUSED(arg_count);
    func(args);
}

#else

#define WORKER_THREADS 4
#define WORKQUEUE_SIZE 256
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
    uint32_t    head;
    uint32_t    tail;
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
            if (atomic_cas_uint32_try(&wq->tail, tail, tail + 1, true, ATOMIC_RELAXED)) {
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
            if (atomic_cas_uint32_try(&wq->head, head, head + 1, true, ATOMIC_RELAXED)) {
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
        condvar_wake(pool_cond);
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

#endif

POP_OPTIMIZATION_SIZE
