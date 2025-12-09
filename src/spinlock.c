/*
spinlock.c - Fast hybrid reader/writer lock
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

PUSH_OPTIMIZATION_SIZE

// Attemts to claim the lock before blocking in the kernel
#define SPINLOCK_USER_RETRIES 40

// Maximum allowed bounded locking time, reports a deadlock upon expiration
#define SPINLOCK_DEADLOCK_NS  10000000000ULL

#define SPINLOCK_QUIESCENT    0x00000000U // No one holds the lock
#define SPINLOCK_HAS_WRITER   0x00000001U // Writer holds the lock
#define SPINLOCK_HAS_READERS  0x7FFFFFFEU // Reader(s) hold the lock
#define SPINLOCK_HAS_WAITERS  0x80000000U // Has waiters

static slow_path void spin_lock_debug_report(spinlock_t* lock, bool crash)
{
    UNUSED(lock);
#if defined(USE_LOCK_DEBUG)
    rvvm_warn("The lock was last exclusively held at %s", lock->location);
#endif
#if defined(RVVM_VERSION)
    rvvm_warn("Version: RVVM " RVVM_VERSION);
#endif
    if (crash) {
        rvvm_fatal("Locking issue detected!");
    } else {
        stacktrace_print();
    }
}

static inline bool spin_flag_has_writer(uint32_t flag)
{
    return !!(flag & SPINLOCK_HAS_WRITER);
}

static inline bool spin_flag_has_readers(uint32_t flag)
{
    return !!(flag & SPINLOCK_HAS_READERS);
}

static inline bool spin_flag_has_waiters(uint32_t flag)
{
    return !!(flag & SPINLOCK_HAS_WAITERS);
}

static inline bool spin_lock_possibly_available(uint32_t flag, bool writer)
{
    if (writer) {
        // No other writers, readers, or waiters
        return !flag;
    } else {
        // No writers or waiters
        return !(flag & (SPINLOCK_HAS_WRITER | SPINLOCK_HAS_WAITERS));
    }
}

static inline bool spin_try_claim_lock(spinlock_t* lock, const char* location, bool writer, uint32_t waiters)
{
    if (writer) {
        uint32_t exp = SPINLOCK_QUIESCENT;
        uint32_t val = SPINLOCK_HAS_WRITER | waiters;
        if (likely(atomic_cas_uint32_try(&lock->flag, exp, val, false, ATOMIC_ACQUIRE))) {
            SPINLOCK_MARK_LOCATION(lock, location);
            return true;
        }
        return false;
    } else {
        uint32_t prev = atomic_load_uint32_relax(&lock->flag);
        uint32_t new;
        do {
            if (unlikely(!spin_lock_possibly_available(prev, false))) {
                return false;
            }
            new = (prev + 2) | waiters;
        } while (!atomic_cas_uint32_ex(&lock->flag, &prev, new, true, ATOMIC_ACQUIRE, ATOMIC_RELAXED));
        return true;
    }
}

static bool spin_lock_try_wait_user(spinlock_t* lock, const char* location, bool writer)
{
    // Spin on a lock in userspace for a few times before using a heavyweight kernel futex
    for (size_t i = 0; i < SPINLOCK_USER_RETRIES; ++i) {
        uint32_t flag = atomic_load_uint32_relax(&lock->flag);
        if (spin_lock_possibly_available(flag, writer)) {
            if (spin_try_claim_lock(lock, location, writer, 0)) {
                // Succesfully claimed the lock
                return true;
            } else {
                // Contention is going on, fallback to kernel wait
                return false;
            }
        }
        thread_cpu_relax();
    }
    return false;
}

slow_path static void spin_lock_wait_internal(spinlock_t* lock, const char* location, uint8_t flags, bool writer)
{
    rvtimer_t deadlock_timer = {0};
    bool      reset_timer    = true;

    if (spin_lock_try_wait_user(lock, location, writer)) {
        return;
    }

    while (true) {
        uint32_t flag = atomic_load_uint32_relax(&lock->flag);
        if (spin_lock_possibly_available(flag, writer)) {
            if (spin_try_claim_lock(lock, location, writer, SPINLOCK_HAS_WAITERS)) {
                // Succesfully claimed the lock, mark it for wakeup
                return;
            }

            // Contention is going on, retry
            reset_timer = true;
            thread_sched_yield();
        } else if (flags & SPINLOCK_WAIT_BUSY_LOOP) {
            thread_sched_yield();
        } else {
            if (reset_timer) {
                reset_timer = false;
                rvtimer_init(&deadlock_timer, 1000000000ULL);
            }

            // Indicate that we're waiting on this lock
            if (!spin_flag_has_waiters(flag)) {
                if (atomic_cas_uint32(&lock->flag, flag, flag | SPINLOCK_HAS_WAITERS)) {
                    flag |= SPINLOCK_HAS_WAITERS;
                }
            }

            // Wait on a futex if we succesfully marked ourselves as a waiter
            if (spin_flag_has_waiters(flag)) {
                if (thread_futex_wait(&lock->flag, flag, SPINLOCK_DEADLOCK_NS)) {
                    // Reset deadlock timer upon noticing any forward progress
                    reset_timer = true;
                }
            }

            // Check for deadlock
            if (rvtimer_get(&deadlock_timer) >= SPINLOCK_DEADLOCK_NS) {
                reset_timer = true;
                if (!location) {
                    location = "[unknown]";
                }
                if (flags & SPINLOCK_WAIT_FOREVER) {
                    rvvm_debug("Laggy %slocking at %s", writer ? "" : "reader ", location);
                } else {
                    rvvm_warn("Possible %sdeadlock at %s", writer ? "" : "reader ", location);
                    spin_lock_debug_report(lock, false);
                }
            }
        }
    }
}

slow_path void spin_lock_wait(spinlock_t* lock, const char* location, uint8_t flags)
{
    spin_lock_wait_internal(lock, location, flags, true);
}

slow_path void spin_read_lock_wait(spinlock_t* lock, const char* location, uint8_t flags)
{
    spin_lock_wait_internal(lock, location, flags, false);
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
        // Wake a reader/writer
        thread_futex_wake(&lock->flag, 1);
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
        // Wake a writer
        atomic_and_uint32(&lock->flag, ~0x80000000U);
        thread_futex_wake(&lock->flag, 1);
    }
}

POP_OPTIMIZATION_SIZE
