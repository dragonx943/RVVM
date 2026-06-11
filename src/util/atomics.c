/*
atomics.c - Atomic operations emulation
Copyright (C) 2025  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "feature_test.h" // IWYU pragma: keep

#include "atomics.h" // IWYU pragma: keep
#include "utils.h"   // IWYU pragma: keep

#if !defined(USE_THREAD_EMU)                                                                                           \
    && (defined(ATOMIC_EMU32_IMPL) || defined(ATOMIC_EMU64_IMPL) || defined(ATOMIC_EMU128_IMPL))

#if !defined(ATOMIC_EMU32_IMPL)

typedef uint32_t host_lock_t;
#define HOST_LOCK_INITIALIZER 0
#define host_lock(lock)                                                                                                \
    while (!atomic_cas_uint32(lock, 0, 1)) {                                                                           \
    }
#define host_unlock(lock) atomic_store_uint32(lock, 0)

#elif defined(HOST_TARGET_WIN32)

#include <windows.h>
typedef CRITICAL_SECTION host_lock_t;
#define HOST_LOCK_INITIALIZER {.LockCount = -1}
#define host_lock(lock)       EnterCriticalSection(lock)
#define host_unlock(lock)     LeaveCriticalSection(lock)

#elif defined(HOST_TARGET_POSIX) && HOST_TARGET_POSIX >= 199506L

#include <pthread.h>
typedef pthread_mutex_t host_lock_t;
#define HOST_LOCK_INITIALIZER PTHREAD_MUTEX_INITIALIZER
#define host_lock(lock)       pthread_mutex_lock(lock)
#define host_unlock(lock)     pthread_mutex_unlock(lock)

#else

#pragma message("Falling back to non-thread-safe atomics emulation")
typedef uint32_t host_lock_t;
#define HOST_LOCK_INITIALIZER 0
#define host_lock(lock)       ((void)lock)
#define host_unlock(lock)     ((void)lock)

#endif

static host_lock_t lock_table[8] = {
    HOST_LOCK_INITIALIZER, HOST_LOCK_INITIALIZER, HOST_LOCK_INITIALIZER, HOST_LOCK_INITIALIZER,
    HOST_LOCK_INITIALIZER, HOST_LOCK_INITIALIZER, HOST_LOCK_INITIALIZER, HOST_LOCK_INITIALIZER,
};

static inline host_lock_t* atomic_emu_get_lock(const void* ptr)
{
    size_t k  = (size_t)ptr;
    k        ^= k >> 21;
    k        ^= k >> 17;
    return &lock_table[k & (STATIC_ARRAY_SIZE(lock_table) - 1)];
}

slow_path void atomic_emu_lock(const void* ptr)
{
    host_lock(atomic_emu_get_lock(ptr));
}

slow_path void atomic_emu_unlock(const void* ptr)
{
    host_unlock(atomic_emu_get_lock(ptr));
}

#endif
