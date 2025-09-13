/*
stacktrace.c - Stacktrace (Using dynamically loaded libbacktrace)
Copyright (C) 2024  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "feature_test.h"

#include "compiler.h"
#include "stacktrace.h"

PUSH_OPTIMIZATION_SIZE

// Do not set signal handlers under sanitizers to prevent breakage
#if !defined(USE_NO_STACKTRACE) && defined(USE_NO_DLIB)
#define USE_NO_STACKTRACE 1
#endif

#if !defined(USE_NO_STACKTRACE)

#include "dlib.h"
#include <stdio.h>

#if defined(HOST_TARGET_POSIX) && !defined(SANITIZERS_ENABLED) /**/                                                    \
    && !defined(HOST_TARGET_HAIKU) && CHECK_INCLUDE(signal.h, 1)

// Set up stacktrace dump on SIGSEGV/SIGBUS/SIGILL/SIGFPE via sigaction()
#include "rvtimer.h"
#include <signal.h>

#ifndef SIGILL
#define SIGILL 4
#endif
#ifndef SIGFPE
#define SIGFPE 8
#endif
#ifndef SIGBUS
#define SIGBUS 10
#endif
#ifndef SIGSEGV
#define SIGSEGV 11
#endif

#if defined(SA_SIGINFO)
#define STACKTRACE_SIGACTION_IMPL 1
#endif

#endif

#if defined(HOST_TARGET_WINNT) && !defined(SANITIZERS_ENABLED)

// Set up stacktrace dump on SEH via SetUnhandledExceptionFilter()
#include "rvtimer.h"
#include <windows.h>

#if defined(EXCEPTION_ACCESS_VIOLATION)
#define STACKTRACE_WIN32_SEH_IMPL 1
#endif

#endif

// Internal headers come after system headers because of safe_free()
#include "utils.h"

/*
 * libbacktrace boilerplace
 */

struct backtrace_state;

typedef void (*backtrace_error_callback)(void* data, const char* msg, int errnum);
typedef int  (*backtrace_full_callback)(void* data, uintptr_t pc, //
                                       const char* filename, int lineno, const char* function);

static struct backtrace_state* (*backtrace_create_state)(const char* filename, int threaded,
                                                         backtrace_error_callback error_callback, void* data)
    = NULL;
static int (*backtrace_full)(struct backtrace_state* state, int skip, backtrace_full_callback callback,
                             backtrace_error_callback error_callback, void* data)
    = NULL;
static void (*backtrace_print)(struct backtrace_state* state, int skip, FILE* file) = NULL;

static struct backtrace_state* bt_state = NULL;

static void backtrace_dummy_error(void* data, const char* msg, int errnum)
{
    UNUSED(data);
    UNUSED(msg);
    UNUSED(errnum);
}

static int backtrace_dummy_callback(void* data, uintptr_t pc, const char* filename, int lineno, const char* function)
{
    UNUSED(data);
    UNUSED(pc);
    UNUSED(filename);
    UNUSED(lineno);
    UNUSED(function);
    return 0;
}

/*
 * Fatal signal stacktraces
 */

#if defined(STACKTRACE_SIGACTION_IMPL)

static void bt_signal_handler(int sig)
{
    switch (sig) {
        case SIGSEGV:
            rvvm_warn("Fatal signal: Segmentation fault!");
            break;
        case SIGBUS:
            rvvm_warn("Fatal signal: Bus fault - Misaligned access or mmapped IO error!");
            break;
        case SIGILL:
            rvvm_warn("Fatal signal: Illegal instruction!");
            break;
        case SIGFPE:
            rvvm_warn("Fatal signal: Arithmetic exception!");
            break;
        default:
            rvvm_warn("Fatal signal %d", sig);
            break;
    }
    rvvm_warn("Halting - Please attach a debugger");
    stacktrace_print();
    while (true) {
        sleep_ms(1000);
    }
}

static void bt_set_signal_handler(int sig)
{
    struct sigaction sa_old = {0};
    struct sigaction sa     = {.sa_handler = bt_signal_handler};
    sigaction(sig, NULL, &sa_old);
    if (!(sa_old.sa_flags & SA_SIGINFO)) {
        void* prev = sa_old.sa_handler;
        if (prev == NULL || prev == (void*)SIG_IGN || prev == (void*)SIG_DFL) {
            // Signal not used
            sigaction(sig, &sa, NULL);
        }
    }
}

static void bt_set_handlers(void)
{
    bt_set_signal_handler(SIGSEGV);
    bt_set_signal_handler(SIGBUS);
    bt_set_signal_handler(SIGILL);
    bt_set_signal_handler(SIGFPE);
}

#endif

#if defined(STACKTRACE_WIN32_SEH_IMPL)

static LPTOP_LEVEL_EXCEPTION_FILTER seh_prev_handler = NULL;

static LONG CALLBACK bt_seh_dummy(EXCEPTION_POINTERS* ptrs)
{
    UNUSED(ptrs);
    return 0;
}

static LONG CALLBACK bt_seh_handler(EXCEPTION_POINTERS* ptrs)
{
    if (seh_prev_handler) {
        LONG code = seh_prev_handler(ptrs);
        if (code) {
            return code;
        }
    } else {
        // Spin until a proper top-level SEH handler is established
        return -1;
    }
    switch (ptrs->ExceptionRecord->ExceptionCode) {
        case EXCEPTION_STACK_OVERFLOW:
            rvvm_warn("Fatal signal: Stack overflow!");
            break;
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
            rvvm_warn("Fatal signal: Segmentation fault!");
            break;
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
            rvvm_warn("Fatal signal: Bus fault - Misaligned access or mmapped IO error!");
            break;
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            rvvm_warn("Fatal signal: Illegal instruction!");
            break;
        case EXCEPTION_INT_OVERFLOW:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            rvvm_warn("Fatal signal: Arithmetic exception!");
            break;
    }
    rvvm_warn("Halting - Please attach a debugger");
    stacktrace_print();
    while (true) {
        sleep_ms(1000);
    }
    return 0;
}

static void bt_revert_handlers(void)
{
    SetUnhandledExceptionFilter(seh_prev_handler);
}

static void bt_set_handlers(void)
{
    seh_prev_handler = SetUnhandledExceptionFilter(bt_seh_handler);
    call_at_deinit(bt_revert_handlers);
    if (!seh_prev_handler) {
        seh_prev_handler = bt_seh_dummy;
    }
}

#endif

/*
 * Initialize libbacktrace
 */

static void backtrace_init_once(void)
{
    if (rvvm_has_arg("no_stacktrace")) {
        return;
    }

    dlib_ctx_t* libbt = dlib_open("backtrace", DLIB_NAME_PROBE);

    backtrace_create_state = dlib_resolve(libbt, "backtrace_create_state");
    backtrace_full         = dlib_resolve(libbt, "backtrace_full");
    backtrace_print        = dlib_resolve(libbt, "backtrace_print");

    dlib_close(libbt);

    if (backtrace_create_state) {
        bt_state = backtrace_create_state(NULL, true, backtrace_dummy_error, NULL);
    }
    if (backtrace_full && bt_state) {
        // Preload backtracing data, isolation is enabled later on
        backtrace_full(bt_state, 0, backtrace_dummy_callback, backtrace_dummy_error, NULL);
    }

#if defined(STACKTRACE_SIGACTION_IMPL) || defined(STACKTRACE_WIN32_SEH_IMPL)
    bt_set_handlers();
#endif
}

void stacktrace_init(void)
{
    DO_ONCE(backtrace_init_once());
}

void stacktrace_print(void)
{
    stacktrace_init();
    if (backtrace_print && bt_state) {
        rvvm_warn("Stacktrace:");
        backtrace_print(bt_state, 0, stderr);
    } else {
        rvvm_warn("Please install libbacktrace to obtain debug stacktraces");
    }
}

#else

void stacktrace_init(void) {}

void stacktrace_print(void) {}

#endif

POP_OPTIMIZATION_SIZE
