/*
chardev_term.c - Terminal backend for UART
Copyright (C) 2023  LekKit <github.com/LekKit>
                    宋文武 <iyzsong@envs.net>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "chardev.h"
#include <stdio.h> // For fputs(), setvbuf()

#if defined(__unix__) || defined(__APPLE__) || defined(__HAIKU__)
#include <fcntl.h>    // For open()
#include <sys/time.h> // For select()
#include <termios.h>  // For tcgetattr(), tcsetattr(), etc
#include <unistd.h>   // For read(), write(), close()

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef O_NOCTTY
#define O_NOCTTY 0
#endif

#define POSIX_TERM_IMPL 1

#elif defined(_WIN32) && !defined(UNDER_CE)
#include <conio.h>   // For _kbhit()
#include <windows.h> // For GetStdHandle(), ReadFile(), WriteFile(), SetConsoleMode(), etc

#define WIN32_TERM_IMPL 1

#else
// Fallback to stdio
#include "mem_ops.h"
#warning No terminal input support!

#endif

// RVVM internal headers come after system headers because of safe_free()
#include "ringbuf.h"
#include "spinlock.h"
#include "utils.h"

SOURCE_OPTIMIZATION_SIZE

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

static bool term_ready_for_io(chardev_term_t* term, bool write)
{
    UNUSED(term);
#if defined(POSIX_TERM_IMPL)
    struct timeval timeout = {0};
    fd_set         fds     = {0};
    int            rfd     = term ? term->rfd : 0;
    int            wfd     = term ? term->wfd : 1;
    int            fd      = write ? wfd : rfd;
    FD_SET(fd, &fds);
    return select(fd + 1, write ? NULL : &fds, write ? &fds : NULL, NULL, &timeout) > 0 && FD_ISSET(fd, &fds);
#elif defined(WIN32_TERM_IMPL)
    return write || _kbhit();
#else
    return write;
#endif
}

static size_t term_write_raw(chardev_term_t* term, const char* buffer, size_t size)
{
    UNUSED(term);
#if defined(POSIX_TERM_IMPL)
    int wfd = term ? term->wfd : 1;
    int tmp = write(wfd, buffer, size);
    return (tmp > 0) ? tmp : 0;
#elif defined(WIN32_TERM_IMPL)
    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  count   = size;
    if (!WriteFile(console, buffer, size, &count, NULL)) {
        return 0;
    }
    return count;
#else
    char   tmp[256] = {0};
    size_t ret      = EVAL_MIN(size, sizeof(tmp) - 1);
    memcpy(tmp, buffer, ret);
    fputs(tmp, stdout);
    return ret;
#endif
}

static size_t term_read_raw(chardev_term_t* term, char* buffer, size_t size)
{
    UNUSED(term);
    UNUSED(buffer);
    UNUSED(size);
#if defined(POSIX_TERM_IMPL)
    int rfd = term ? term->rfd : 0;
    int tmp = read(rfd, buffer, size);
    return (tmp > 0) ? tmp : 0;
#elif defined(WIN32_TERM_IMPL)
    HANDLE console = GetStdHandle(STD_INPUT_HANDLE);
    DWORD  count   = size;
    if (!ReadFile(console, buffer, size, &count, NULL)) {
        return 0;
    }
    return count;
#else
    // No way to implement non-blocking input using stdio
    return 0;
#endif
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
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &orig_input_mode);
    GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &orig_output_mode);

    // Pre-Win10 raw console setup
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), 0x0);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), 0x1);

    // ENABLE_VIRTUAL_TERMINAL_INPUT
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), 0x200);
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
    if (term_ready_for_io(term, false)) {
        char   rx_buf[256] = {0};
        size_t rx_size     = EVAL_MIN(sizeof(rx_buf), ringbuf_space(&term->rx));
        rx_size            = term_read_raw(term, rx_buf, rx_size);

        term_process_input(term, rx_buf, rx_size);
        ringbuf_write(&term->rx, rx_buf, rx_size);
    }
}

static void term_push_tx(chardev_term_t* term)
{
    if (term_ready_for_io(term, true)) {
        char   tx_buf[256] = {0};
        size_t tx_size     = ringbuf_peek(&term->tx, tx_buf, sizeof(tx_buf));
        tx_size            = term_write_raw(term, tx_buf, tx_size);
        ringbuf_skip(&term->tx, tx_size);
    }
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
