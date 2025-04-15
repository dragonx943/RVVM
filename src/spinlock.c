/*
spinlock.c - Hybrid Spinlock
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "spinlock.h"
#include "rvtimer.h"
#include "stacktrace.h"
#include "threading.h"
#include "utils.h"

// Maximum allowed bounded locking time, reports a deadlock upon expiration
#define SPINLOCK_MAX_MS  10000

// Attemts to claim the lock before blocking in the kernel
#define SPINLOCK_RETRIES 40

static no_inline void spin_lock_debug_report(spinlock_t* lock, bool crash)
{
#ifdef USE_SPINLOCK_DEBUG
    rvvm_warn("The lock was previously held at %s", lock->location);
#else
    UNUSED(lock);
#endif
#ifdef RVVM_VERSION
    rvvm_warn("Version: RVVM v" RVVM_VERSION);
#endif
    if (crash) {
        rvvm_fatal("Locking issue detected!");
    } else {
        stacktrace_print();
    }
}

static inline bool spin_flag_has_writer(uint32_t flag)
{
    return !!(flag & 0x1U);
}

static inline bool spin_flag_has_readers(uint32_t flag)
{
    return !!(flag & ~0x80000001U);
}

static inline bool spin_flag_has_waiters(uint32_t flag)
{
    return !!(flag & 0x80000000U);
}

static inline bool spin_flag_writer_welcome(uint32_t flag)
{
    return !flag;
}

static inline bool spin_flag_readers_welcome(uint32_t flag)
{
    return !(flag & 0x80000001U);
}

static inline bool spin_lock_possibly_available(uint32_t flag, bool writer)
{
    if (writer) {
        return spin_flag_writer_welcome(flag);
    } else {
        return spin_flag_readers_welcome(flag);
    }
}

static inline bool spin_try_claim_internal(spinlock_t* lock, const char* location, bool writer)
{
    if (writer) {
        return spin_try_lock_internal(lock, location);
    } else {
        return spin_try_read_lock(lock);
    }
}

slow_path static void spin_lock_wait_internal(spinlock_t* lock, const char* location, bool writer)
{
    rvtimer_t deadlock_timer       = {0};
    bool      reset_deadlock_timer = true;

    for (size_t i = 0; i < SPINLOCK_RETRIES; ++i) {
        // Read lock flag until there's any chance to grab it
        // Improves performance due to cacheline bouncing elimination
        uint32_t flag = atomic_load_uint32_ex(&lock->flag, ATOMIC_RELAXED);
        if (spin_lock_possibly_available(flag, writer)) {
            if (spin_try_claim_internal(lock, location, writer)) {
                // Succesfully claimed the lock
                return;
            }
            // Contention is going on, fallback to kernel wait
            break;
        }
#if defined(GNU_EXTS) && defined(__x86_64__)
        __asm__ volatile("pause");
#elif defined(GNU_EXTS) && defined(__aarch64__)
        __asm__ volatile("isb sy");
#endif
    }

    while (true) {
        uint32_t flag = atomic_load_uint32_ex(&lock->flag, ATOMIC_RELAXED);
        if (spin_lock_possibly_available(flag, writer)) {
            if (spin_try_claim_internal(lock, location, writer)) {
                // Succesfully claimed the lock
                return;
            }
            // Contention is going on, retry
            reset_deadlock_timer = true;
        } else {
            // Indicate that we're waiting on this lock
            if (spin_flag_has_waiters(flag) || atomic_cas_uint32(&lock->flag, flag, flag | 0x80000000U)) {
                // Wait till wakeup from lock owner
                if (thread_futex_wait(&lock->flag, flag, -1) || !spin_flag_has_waiters(flag)) {
                    // Reset deadlock timer upon noticing any forward progress
                    reset_deadlock_timer = true;
                }
            }
        }
        if (reset_deadlock_timer) {
            rvtimer_init(&deadlock_timer, 1000);
            reset_deadlock_timer = false;
        }
        if (location && rvtimer_get(&deadlock_timer) > SPINLOCK_MAX_MS) {
            rvvm_warn("Possible %sdeadlock at %s", writer ? "" : "reader ", location);
            spin_lock_debug_report(lock, false);
            reset_deadlock_timer = true;
        }
    }
}

slow_path void spin_lock_wait(spinlock_t* lock, const char* location)
{
    spin_lock_wait_internal(lock, location, true);
}

slow_path void spin_read_lock_wait(spinlock_t* lock, const char* location)
{
    spin_lock_wait_internal(lock, location, false);
}

slow_path void spin_lock_wake(spinlock_t* lock, uint32_t prev)
{
    if (spin_flag_has_readers(prev)) {
        rvvm_warn("Mismatched unlock of a reader lock!");
        spin_lock_debug_report(lock, true);
    } else if (!spin_flag_has_writer(prev)) {
        rvvm_warn("Unlock of a non-locked writer lock!");
        spin_lock_debug_report(lock, true);
    } else if (spin_flag_has_waiters(prev)) {
        // Wake all readers or waiter
        thread_futex_wake(&lock->flag, -1);
    }
}

slow_path void spin_read_lock_wake(spinlock_t* lock, uint32_t prev)
{
    if (spin_flag_has_writer(prev)) {
        rvvm_warn("Mismatched unlock of a writer lock!");
        spin_lock_debug_report(lock, true);
    } else if (!spin_flag_has_readers(prev)) {
        rvvm_warn("Unlock of a non-locked reader lock!");
        spin_lock_debug_report(lock, true);
    } else if (spin_flag_has_waiters(prev)) {
        // Wake writer
        atomic_and_uint32(&lock->flag, ~0x80000000U);
        thread_futex_wake(&lock->flag, 1);
    }
}
