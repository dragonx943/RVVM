/*
spinlock.h - Fast hybrid reader/writer lock
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef LEKKIT_SPINLOCK_H
#define LEKKIT_SPINLOCK_H

#include "atomics.h"
#include "compiler.h"
#include <stddef.h>

// Lock structure
typedef struct {
    uint32_t flag;
#if defined(USE_LOCK_DEBUG)
    const char* location;
#endif
} spinlock_t;

// Static lock initializer
#define SPINLOCK_INIT ZERO_INIT

// Initialize a lock dynamically
static inline void spin_init(spinlock_t* lock)
{
    lock->flag = 0x0;
#if defined(USE_LOCK_DEBUG)
    lock->location = NULL;
#endif
}

/*
 * Internal implementation details
 *
 * Lock flags meaning:
 * 0x00000000: Quiescent state (No one holds the lock)
 * 0x00000001: Writer holds the lock
 * 0x7FFFFFFE: Reader count
 * 0x80000000: There are waiters
 */

// Flags for spin_lock_wait()
#define SPINLOCK_WAIT_FOREVER   0x01 // Omit deadlock debugging upon timeout
#define SPINLOCK_WAIT_BUSY_LOOP 0x02 // Omit kernel futex wait, perform busy loop

// Slow path for kernel wait / wake on contended lock, and lock misuse detection
slow_path void spin_lock_wait(spinlock_t* lock, const char* location, uint8_t flags);
slow_path void spin_lock_wake(spinlock_t* lock, uint32_t prev);

slow_path void spin_read_lock_wait(spinlock_t* lock, const char* location, uint8_t flags);
slow_path void spin_read_lock_wake(spinlock_t* lock, uint32_t prev);

#if defined(USE_LOCK_DEBUG)

#define SPINLOCK_DEBUG_LOCATION SOURCE_LINE
#define SPINLOCK_MARK_LOCATION(lock, location)                                                                         \
    do {                                                                                                               \
        lock->location = location;                                                                                     \
    } while (0)

#else

#define SPINLOCK_DEBUG_LOCATION NULL
#define SPINLOCK_MARK_LOCATION(lock, location)                                                                         \
    do {                                                                                                               \
        UNUSED(lock);                                                                                                  \
        UNUSED(location);                                                                                              \
    } while (0)

#endif

/*
 * Writer locking
 */

static forceinline bool spin_try_lock_internal(spinlock_t* lock, const char* location)
{
    if (likely(atomic_cas_uint32_try(&lock->flag, 0x0, 0x1, false, ATOMIC_ACQUIRE))) {
        SPINLOCK_MARK_LOCATION(lock, location);
        return true;
    }
    return false;
}

static forceinline void spin_lock_internal(spinlock_t* lock, const char* location, uint8_t flags)
{
    // Use weak CAS in fast path
    if (likely(atomic_cas_uint32_try(&lock->flag, 0x0, 0x1, true, ATOMIC_ACQUIRE))) {
        SPINLOCK_MARK_LOCATION(lock, location);
    } else {
        spin_lock_wait(lock, location, flags);
    }
}

// Try to claim the writer lock
#define spin_try_lock(lock)  spin_try_lock_internal(lock, SPINLOCK_DEBUG_LOCATION)

// Perform writer locking on small, bounded critical section
// Reports a deadlock upon waiting for too long
#define spin_lock(lock)      spin_lock_internal(lock, SPINLOCK_DEBUG_LOCATION, 0)

// Perform writer locking around heavy operation, wait indefinitely
#define spin_lock_slow(lock) spin_lock_internal(lock, SPINLOCK_DEBUG_LOCATION, SPINLOCK_WAIT_FOREVER)

// Release the writer lock
static forceinline void spin_unlock(spinlock_t* lock)
{
    uint32_t prev = atomic_swap_uint32_ex(&lock->flag, 0x0, ATOMIC_RELEASE);
    if (unlikely(prev != 0x1)) {
        // Waiters are present, or invalid usage detected (Not locked / Locked as a reader)
        spin_lock_wake(lock, prev);
    }
}

/*
 * Low-level busy loop locking, for special places where sleeping is not allowed
 */

#define spin_lock_busy_loop(lock)   spin_lock_internal(lock, SPINLOCK_DEBUG_LOCATION, SPINLOCK_WAIT_BUSY_LOOP)

#define spin_unlock_busy_loop(lock) atomic_store_uint32(&(lock)->flag, 0x0);

/*
 * Reader locking
 */

// Try to claim the reader lock
static forceinline bool spin_try_read_lock(spinlock_t* lock)
{
    uint32_t prev = atomic_load_uint32_relax(&lock->flag);
    do {
        if (unlikely(prev & 0x80000001U)) {
            // Writer owns the lock, writer waiters are present or too much readers (sic!)
            return false;
        }
    } while (!atomic_cas_uint32_ex(&lock->flag, &prev, prev + 0x2, true, ATOMIC_ACQUIRE, ATOMIC_RELAXED));
    return true;
}

static forceinline void spin_read_lock_internal(spinlock_t* lock, const char* location, uint8_t flags)
{
    if (unlikely(!spin_try_read_lock(lock))) {
        spin_read_lock_wait(lock, location, flags);
    }
}

// Perform reader locking on small, bounded critical section
// Reports a deadlock upon waiting for too long
#define spin_read_lock(lock)      spin_read_lock_internal(lock, SPINLOCK_DEBUG_LOCATION, 0)

// Perform reader locking around heavy operation, wait indefinitely
#define spin_read_lock_slow(lock) spin_read_lock_internal(lock, SPINLOCK_DEBUG_LOCATION, SPINLOCK_WAIT_FOREVER)

// Release the reader lock
static forceinline void spin_read_unlock(spinlock_t* lock)
{
    uint32_t prev = atomic_sub_uint32_ex(&lock->flag, 0x2, ATOMIC_RELEASE);
    if (unlikely(((int32_t)prev) < 0x2)) {
        // Waiters are present, or invalid usage detected (Not locked / Locked as a writer)
        spin_read_lock_wake(lock, prev);
    }
}

/*
 * Scoped locking helpers, may be exited via break
 *
 * scoped_spin_lock(&lock) {
 *     do_something_under_lock();
 *
 *     if (need_exit_under_lock()) {
 *         break;
 *     }
 *
 *     do_something_under_lock();
 * }
 *
 * scoped_spin_try_lock(&lock) {
 *     do_something_under_lock();
 * } else {
 *     do_something_when_locking_failed();
 * }
 */

#define scoped_spin_lock(lock)           SCOPED_HELPER (spin_lock(lock), spin_unlock(lock))
#define scoped_spin_try_lock(lock)       POST_COND (spin_try_lock(lock), spin_unlock(lock))

#define scoped_spin_read_lock(lock)      SCOPED_HELPER (spin_read_lock(lock), spin_read_unlock(lock))
#define scoped_spin_try_read_lock(lock)  POST_COND (spin_try_read_lock(lock), spin_read_unlock(lock))

#define scoped_spin_lock_slow(lock)      SCOPED_HELPER (spin_lock_slow(lock), spin_unlock(lock))
#define scoped_spin_read_lock_slow(lock) SCOPED_HELPER (spin_read_lock_slow(lock), spin_read_unlock(lock))

#endif
