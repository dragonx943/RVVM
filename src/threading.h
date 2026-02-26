/*
threading.h - Threading, Futexes
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef LEKKIT_THREADING_H
#define LEKKIT_THREADING_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Threads
 *
 * Note: Detaching a thread is unsafe in unloadable libraries.
 * Note: Use rvvm_interrupt_syscall() to interrupt a syscall
 *       currently blocking the thread, if possible.
 */

typedef struct rvvm_thread_intrnl rvvm_thread_t;

typedef void* (*rvvm_thread_func_t)(void*);

rvvm_thread_t* rvvm_thread_create_ex(rvvm_thread_func_t func, void* arg, uint32_t stack_size);
rvvm_thread_t* rvvm_thread_create(rvvm_thread_func_t func, void* arg);
void           rvvm_thread_join(rvvm_thread_t* thread);
void           rvvm_thread_detach(rvvm_thread_t* thread);
void           rvvm_interrupt_syscall(rvvm_thread_t* thread);

/*
 * Yielding, CPU relax hints
 */

void rvvm_sched_yield(void);
void rvvm_cpu_relax(void);

/*
 * Futexes
 */

#define RVVM_FUTEX_INFINITE ((uint64_t)-1)

#define RVVM_FUTEX_TIMEOUT  0x00
#define RVVM_FUTEX_WAKEUP   0x01
#define RVVM_FUTEX_MISMATCH 0x02

uint32_t rvvm_futex_wait(void* ptr, uint32_t val, uint64_t timeout_ns);
void     rvvm_futex_wake(void* ptr, uint32_t num);

/*
 * Events
 *
 * Note: The sync_event_t structure initializes to zero.
 */

#define RVVM_EVENT_INFINITE RVVM_FUTEX_INFINITE

#define RVVM_EVENT_INIT     {0}

typedef struct {
    uint32_t flag;
    uint32_t waiters;
} rvvm_event_t;

void     rvvm_event_init(rvvm_event_t* event);
bool     rvvm_event_wait(rvvm_event_t* event, uint64_t timeout_ns);
bool     rvvm_event_wake(rvvm_event_t* event);
uint32_t rvvm_event_waiters(rvvm_event_t* event);

/*
 * Threaded tasks (Similar to threaded interrupts)
 */

typedef void (*rvvm_task_cb_intrnl_t)(void*);

typedef struct rvvm_task_intrnl rvvm_task_t;

rvvm_task_t* rvvm_task_init(rvvm_task_cb_intrnl_t cb, void* data);
void         rvvm_task_wake(rvvm_task_t* task);
void         rvvm_task_free(rvvm_task_t* task);

/*
 * Legacy interfaces
 * TODO: Cleanup
 */

#define thread_ctx_t          rvvm_thread_t
#define thread_create         rvvm_thread_create
#define thread_join           rvvm_thread_join
#define thread_detach         rvvm_thread_detach

#define thread_sched_yield    rvvm_sched_yield
#define thread_cpu_relax      rvvm_cpu_relax

#define THREAD_FUTEX_INFINITE RVVM_FUTEX_INFINITE
#define THREAD_FUTEX_TIMEOUT  RVVM_FUTEX_TIMEOUT
#define THREAD_FUTEX_WAKEUP   RVVM_FUTEX_WAKEUP
#define THREAD_FUTEX_MISMATCH RVVM_FUTEX_MISMATCH

#define thread_futex_wait     rvvm_futex_wait
#define thread_futex_wake     rvvm_futex_wake

#define CONDVAR_INFINITE      RVVM_EVENT_INFINITE

#define cond_var_t            rvvm_event_t
#define condvar_create()      safe_new_obj(cond_var_t)
#define condvar_wait_ns       rvvm_event_wait
#define condvar_wait(cond, timeout_ms)                                                                                 \
    condvar_wait_ns(cond, (timeout_ms == (uint64_t)-1) ? (uint64_t)-1 : timeout_ms * 1000000ULL)
#define condvar_wake       rvvm_event_wake
#define condvar_waiters    rvvm_event_waiters
#define condvar_free       safe_free

/*
 * Threadpool
 *
 * TODO: Replace with threaded tasks
 */

#define THREAD_MAX_VA_ARGS 8

typedef void* (*rvvm_thread_func_va_t)(void**);

void thread_create_task(rvvm_thread_func_t func, void* arg);
void thread_create_task_va(rvvm_thread_func_va_t func, void** args, unsigned arg_count);

#endif
