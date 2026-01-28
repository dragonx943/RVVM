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
 */

typedef struct thread_ctx thread_ctx_t;

typedef void* (*thread_func_t)(void*);

thread_ctx_t* thread_create_ex(thread_func_t func, void* arg, uint32_t stack_size);
thread_ctx_t* thread_create(thread_func_t func, void* arg);
bool          thread_join(thread_ctx_t* thread);

/*
 * Thread yielding, CPU relax hints
 */

void thread_sched_yield(void);
void thread_cpu_relax(void);

/*
 * Futexes
 */

#define THREAD_FUTEX_INFINITE ((uint64_t)-1)

#define THREAD_FUTEX_TIMEOUT  0
#define THREAD_FUTEX_WAKEUP   1
#define THREAD_FUTEX_MISMATCH 2

uint32_t thread_futex_wait(void* ptr, uint32_t val, uint64_t timeout_ns);
void     thread_futex_wake(void* ptr, uint32_t num);

/*
 * Events
 *
 * The thread_event_t structure initializes to zero.
 */

#define THREAD_EVENT_INFINITE ((uint64_t)-1)

typedef struct {
    uint32_t flag;
    uint32_t waiters;
} thread_event_t;

void     thread_event_init(thread_event_t* event);
bool     thread_event_wait(thread_event_t* event, uint64_t timeout_ns);
bool     thread_event_wake(thread_event_t* event);
uint32_t thread_event_waiters(thread_event_t* event);

/*
 * Condvar (TODO: Legacy interface, switch to events)
 */

#define CONDVAR_INFINITE ((uint64_t)-1)

typedef thread_event_t cond_var_t;

cond_var_t* condvar_create(void);
bool        condvar_wait_ns(cond_var_t* cond, uint64_t timeout_ns);
bool        condvar_wait(cond_var_t* cond, uint64_t timeout_ms);
bool        condvar_wake(cond_var_t* cond);
uint32_t    condvar_waiters(cond_var_t* cond);
void        condvar_free(cond_var_t* cond);

/*
 * Task offloading threadpool
 */

#define THREAD_MAX_VA_ARGS 8

typedef void* (*thread_func_va_t)(void**);

void thread_create_task(thread_func_t func, void* arg);
void thread_create_task_va(thread_func_va_t func, void** args, unsigned arg_count);

#endif
