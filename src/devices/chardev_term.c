/*
chardev_term.c - Terminal backend for UART
Copyright (C) 2023  LekKit <github.com/LekKit>
                    宋文武 <iyzsong@envs.net>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

// Expose termios, ONLCR, O_NOCTTY, O_CLOEXEC
#include "feature_test.h"

#include "chardev.h"
#include "ringbuf.h"
#include "spinlock.h"
#include "utils.h"

#include <stdio.h> // For fputs(), setvbuf()

PUSH_OPTIMIZATION_SIZE

#if defined(HOST_TARGET_POSIX) && !defined(HOST_TARGET_VMS)

#include <fcntl.h>    // For open()
#include <sys/time.h> // For select()
#include <termios.h>  // For tcgetattr(), tcsetattr(), etc
#include <unistd.h>   // For read(), write(), close()

#if defined(HOST_TARGET_EMSCRIPTEN)
#include "mem_ops.h"
#include "threading.h"
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOCTTY
#define O_NOCTTY 0
#endif

#ifndef ONLCR
#define ONLCR 0
#endif
#ifndef OPOST
#define OPOST 0
#endif
#ifndef CREAD
#define CREAD 0
#endif

static bool posix_fd_ready(int fd, bool write)
{
#if defined(HOST_TARGET_EMSCRIPTEN)
    UNUSED(fd);
    return write;
#else
    struct timeval tv = {0};
    if (fd >= 0 && fd < FD_SETSIZE) {
        fd_set  fds  = {0};
        fd_set* rfds = write ? NULL : &fds;
        fd_set* wfds = write ? &fds : NULL;
        FD_SET(fd, &fds);
        return select(fd + 1, rfds, wfds, NULL, &tv) > 0 && FD_ISSET(fd, &fds);
    }
    DO_ONCE(rvvm_warn("Invalid terminal fd!"));
    return false;
#endif
}

#define POSIX_TERM_IMPL 1

#elif defined(HOST_TARGET_WINNT)

// For GetStdHandle(), ReadFile(), WriteFile(), SetConsoleMode(), GetNumberOfConsoleInputEvents()
#include <windows.h>

// For AllocConsole(), GetConsoleSelectionInfo() probing
#include "dlib.h"

typedef struct {
    DWORD      flags;
    COORD      anchor;
    SMALL_RECT selection;
} console_selection_info_t;

static BOOL (*__stdcall get_console_selection_info)(void*) = NULL;

// Check for console selection (quick edit) to ideally prevent locking up in WriteFile()
static bool win32_console_busy(void)
{
    console_selection_info_t csi = {0};
    if (get_console_selection_info) {
        get_console_selection_info(&csi);
    }
    return !!csi.flags;
}

#define WIN32_TERM_IMPL 1

#elif defined(HOST_TARGET_DOS) && CHECK_INCLUDE(conio.h, 1)

#include <conio.h>

#define DOS_TERM_IMPL 1

#else

// Fallback to stdio
#warning No terminal input support!

#endif

typedef struct {
    chardev_t  chardev;
    ringbuf_t  rx, tx;
    spinlock_t lock;

    uint32_t flags;
    int      rfd, wfd;
    bool     ctrl_a;
} chardev_term_t;

/*
 * OS-specific terminal handling
 */

#if defined(POSIX_TERM_IMPL)

static struct termios orig_term_opts = {0};

#elif defined(WIN32_TERM_IMPL)

static DWORD orig_input_mode  = 0;
static DWORD orig_output_mode = 0;

#endif

#if defined(HOST_TARGET_EMSCRIPTEN)

/*
 * Workaround the usual Emscripten bullshit (select is now apparently broken)
 */

static char     input_buffer[1024];
static uint32_t input_amount = 0;

static void* input_worker(void* arg)
{
    int fd = (size_t)arg;
    while (true) {
        uint32_t tmp = atomic_load_uint32(&input_amount);
        if (tmp) {
            thread_futex_wait(&input_amount, tmp, -1);
        } else {
            tmp = read(fd, input_buffer, sizeof(input_buffer));
            atomic_store_uint32(&input_amount, EVAL_MAX(tmp, 0));
        }
    }
    return arg;
}

#endif

static size_t term_read_raw(chardev_term_t* term, char* buffer, size_t size)
{
    UNUSED(term && buffer && size);
#if defined(HOST_TARGET_EMSCRIPTEN)
    DO_ONCE(thread_create(input_worker, (void*)(size_t)term->rfd));
    uint32_t ret = EVAL_MIN(atomic_load_uint32(&input_amount), size);
    if (ret) {
        memcpy(buffer, input_buffer, ret);
        if (atomic_sub_uint32(&input_amount, ret) == ret) {
            thread_futex_wake(&input_amount, 1);
        }
    }
    return ret;
#elif defined(POSIX_TERM_IMPL)
    if (posix_fd_ready(term->rfd, false)) {
        int ret = read(term->rfd, buffer, size);
        return EVAL_MAX(ret, 0);
    }
    return 0;
#elif defined(WIN32_TERM_IMPL)
    HANDLE console = GetStdHandle(STD_INPUT_HANDLE);
    if (size && console && console != INVALID_HANDLE_VALUE) {
        size_t ret    = 0;
        DWORD  events = 0;
        GetNumberOfConsoleInputEvents(console, &events);
        if (events) {
            INPUT_RECORD* ir = safe_new_arr(INPUT_RECORD, events);
            if (PeekConsoleInputW(console, ir, events, &events)) {
                // Pulled widechar console events, convert to UTF-8
                DWORD pos = 0;
                for (; pos < events; ++pos) {
                    if (ir[pos].EventType == KEY_EVENT && ir[pos].Event.KeyEvent.bKeyDown) {
                        size_t tmp = utf8_encode_code_point(buffer + ret, size - ret, //
                                                            ir[pos].Event.KeyEvent.uChar.UnicodeChar);
                        if (!tmp && (size - ret) < 4) {
                            // The buffer is full
                            break;
                        }
                        ret += tmp;
                    }
                }
                // Skip events
                ReadConsoleInputW(console, ir, pos, &pos);
#if defined(HOST_TARGET_WIN9X)
            } else if (ReadConsoleInputA(console, ir, size, &events)) {
                // Fallback to ANSI syscall (Win9x compat)
                for (DWORD pos = 0; pos < events; ++pos) {
                    if (ir[pos].EventType == KEY_EVENT && ir[pos].Event.KeyEvent.bKeyDown && ret < size) {
                        buffer[ret++] = ir[pos].Event.KeyEvent.uChar.AsciiChar;
                    }
                }
#endif
            }
            safe_free(ir);
            return ret;
        }
    }
    return 0;
#elif defined(DOS_TERM_IMPL)
    size_t ret = 0;
    while (ret < size && kbhit()) {
        buffer[ret++] = getch();
    }
    return ret;
#else
    // No way to implement non-blocking input using stdio
    return 0;
#endif
}

static size_t term_write_raw(chardev_term_t* term, const char* buffer, size_t size)
{
#if defined(POSIX_TERM_IMPL)
    if (posix_fd_ready(term->wfd, true)) {
        int ret = write(term->wfd, buffer, size);
        return EVAL_MAX(ret, 0);
    }
#elif defined(WIN32_TERM_IMPL)
    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  count   = size;
    if (size) {
        if (console && console != INVALID_HANDLE_VALUE && !win32_console_busy()) {
            WriteFile(console, buffer, size, &count, NULL);
        }
        return count;
    }
#else
    char str[256] = {0};
    if (size) {
        size_t ret = EVAL_MIN(size, sizeof(str) - 1);
        rvvm_strlcpy(str, buffer, ret + 1);
        fputs(str, stdout);
        return ret;
    }
#endif
    UNUSED(term);
    return 0;
}

static void term_orig_mode(void)
{
    // Perfom terminal reset to a sensible state, without clearing the screen
    // Don't send Mouse X & Y; Don't send FocusIn/FocusOut; Disable Alternate Scroll Mode;
    // Use Normal Screen Buffer; Soft terminal reset
    const char* reset = "\x1B[?1000l\x1B[?1004l\x1B[?1007l\x1B[?47l\x1B[!p";
    fputs(reset, stderr);
#if defined(POSIX_TERM_IMPL)
    tcsetattr(0, TCSAFLUSH, &orig_term_opts);
#elif defined(WIN32_TERM_IMPL)
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), orig_input_mode);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), orig_output_mode);
#endif
}

static void term_raw_mode(void)
{
#if defined(POSIX_TERM_IMPL)
    struct termios term_opts = {
        .c_oflag = OPOST | ONLCR,
        .c_cflag = CLOCAL | CREAD | CS8,
    };
    tcgetattr(0, &orig_term_opts);
    cfsetispeed(&term_opts, cfgetispeed(&orig_term_opts));
    cfsetospeed(&term_opts, cfgetospeed(&orig_term_opts));
    tcsetattr(0, TCSANOW, &term_opts);
#elif defined(WIN32_TERM_IMPL)
    DO_ONCE_SCOPED {
        BOOL (*__stdcall alloc_console)(void) = dlib_get_symbol("kernel32.dll", "AllocConsole");
        if (alloc_console) {
            // Create a console since we may be running as GUI subsystem
            alloc_console();
        }

        // Probe GetConsoleSelectionInfo() (NT 5.1+)
        get_console_selection_info = dlib_get_symbol("kernel32.dll", "GetConsoleSelectionInfo");
    }

    // Set console encoding to UTF-8
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // Save previous console mode
    GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &orig_input_mode);
    GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &orig_output_mode);

    // Pre-Win10 raw console setup; Disable quick edit via ENABLE_EXTENDED_FLAGS
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), 0x80);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), 0x1);

    // ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_EXTENDED_FLAGS
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), 0x280);
    // ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), 0x5);
#elif defined(_IONBF)
    // Disable stdio buffering
    setvbuf(stdout, NULL, _IONBF, 0);
#endif
}

static uint32_t term_count = 0;

static void term_attach(void)
{
    if (atomic_add_uint32(&term_count, 1) == 0) {
        term_raw_mode();
    }
}

static void term_detach(void)
{
    if (atomic_sub_uint32(&term_count, 1) == 1) {
        term_orig_mode();
    }
}

/*
 * Chardev handling
 */

static uint32_t term_update_flags(chardev_term_t* term)
{
    uint32_t flags = 0, flags_orig = atomic_load_uint32_relax(&term->flags);
    if (ringbuf_avail(&term->rx)) {
        flags |= CHARDEV_RX;
    }
    if (ringbuf_space(&term->tx)) {
        flags |= CHARDEV_TX;
    }
    atomic_store_uint32_relax(&term->flags, flags);
    return flags & ~flags_orig;
}

// Handles VM-related hotkeys
static void term_process_input(chardev_term_t* term, char* buffer, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        if (term->ctrl_a) {
            if (buffer[i] == 'x') {
                // Exit on Ctrl+A, x
                spin_unlock(&term->lock);
                term_orig_mode();
                exit(0);
            }
        }

        // Ctrl+A (SOH VT code)
        term->ctrl_a = buffer[i] == 1;
    }
}

static void term_pull_rx(chardev_term_t* term)
{
    char   rx_buf[256] = {0};
    size_t rx_size     = EVAL_MIN(sizeof(rx_buf), ringbuf_space(&term->rx));
    rx_size            = term_read_raw(term, rx_buf, rx_size);

    term_process_input(term, rx_buf, rx_size);
    ringbuf_write(&term->rx, rx_buf, rx_size);
}

static void term_push_tx(chardev_term_t* term)
{
    char   tx_buf[256] = {0};
    size_t tx_size     = ringbuf_peek(&term->tx, tx_buf, sizeof(tx_buf));
    tx_size            = term_write_raw(term, tx_buf, tx_size);
    ringbuf_skip(&term->tx, tx_size);
}

static void term_update(chardev_t* dev)
{
    chardev_term_t* term  = dev->data;
    uint32_t        flags = 0;

    scoped_spin_try_lock (&term->lock) {
        if (ringbuf_space(&term->rx)) {
            term_pull_rx(term);
        }

        if (ringbuf_avail(&term->tx)) {
            term_push_tx(term);
        }

        flags = term_update_flags(term);
    }

    if (flags) {
        chardev_notify(&term->chardev, flags);
    }
}

static uint32_t term_poll(chardev_t* dev)
{
    chardev_term_t* term = dev->data;
    return atomic_load_uint32_relax(&term->flags);
}

static size_t term_read(chardev_t* dev, void* buf, size_t nbytes)
{
    size_t ret = 0;
    if (term_poll(dev) & CHARDEV_RX) {
        chardev_term_t* term = dev->data;
        scoped_spin_lock (&term->lock) {
            ret = ringbuf_read(&term->rx, buf, nbytes);
            if (!ringbuf_avail(&term->rx)) {
                term_pull_rx(term);
            }
            term_update_flags(term);
        }
    }
    return ret;
}

static size_t term_write(chardev_t* dev, const void* buf, size_t nbytes)
{
    size_t          ret  = 0;
    chardev_term_t* term = dev->data;
    scoped_spin_lock (&term->lock) {
        ret = ringbuf_write(&term->tx, buf, nbytes);
        if (!ringbuf_space(&term->tx)) {
            term_push_tx(term);
        }
        term_update_flags(term);
    }
    return ret;
}

static void term_remove(chardev_t* dev)
{
    chardev_term_t* term = dev->data;
    term_update(dev);
    ringbuf_destroy(&term->rx);
    ringbuf_destroy(&term->tx);
#ifdef POSIX_TERM_IMPL
    if (term->rfd != 0) {
        close(term->rfd);
    }
    if (term->wfd != 1 && term->wfd != term->rfd) {
        close(term->wfd);
    }
#endif
    if (term->rfd == 0) {
        term_detach();
    }
    free(term);
}

PUBLIC chardev_t* chardev_term_create(void)
{
    term_attach();
    return chardev_fd_create(0, 1);
}

PUBLIC chardev_t* chardev_fd_create(int rfd, int wfd)
{
#ifndef POSIX_TERM_IMPL
    if (rfd != 0 || wfd != 1) {
        rvvm_error("No FD chardev support on non-POSIX");
        return NULL;
    }
#endif

    chardev_term_t* term = safe_new_obj(chardev_term_t);
    ringbuf_create(&term->rx, 256);
    ringbuf_create(&term->tx, 256);
    term->chardev.data   = term;
    term->chardev.read   = term_read;
    term->chardev.write  = term_write;
    term->chardev.poll   = term_poll;
    term->chardev.update = term_update;
    term->chardev.remove = term_remove;
    term->rfd            = rfd;
    term->wfd            = wfd;

    return &term->chardev;
}

PUBLIC chardev_t* chardev_pty_create(const char* path)
{
    if (rvvm_strcmp(path, "stdout")) {
        // Create a terminal character device on stdout
        return chardev_term_create();
    }

    if (rvvm_strcmp(path, "null")) {
        // NULL character device
        return NULL;
    }

#ifdef POSIX_TERM_IMPL
    int fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd >= 0) {
        return chardev_fd_create(fd, fd);
    }
    rvvm_error("Could not open PTY %s", path);
#else
    rvvm_error("No PTY chardev support on non-POSIX");
#endif
    return NULL;
}

POP_OPTIMIZATION_SIZE
