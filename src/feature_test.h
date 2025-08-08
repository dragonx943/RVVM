/*
feature_test.h - Enabling & probing of feature test macros
Copyright (C) 2025  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _FEATURE_TEST_H
#define _FEATURE_TEST_H

/*
 * Enable POSIX.1-2008
 * - Exposes pread(), pwrite(), readlink(), O_CLOEXEC
 * - Exposes posix_fallocate() on Linux, FreeBSD 9+, NetBSD 7+
 * - Exposes fdatasync() on Linux, FreeBSD 12+, NetBSD
 * - Exposes clock_gettime(), nanosleep(), sched_yield()
 * - Exposes fileno()
 * - Exposes pthread_condattr_setclock()
 *
 * NOTE: Defining _POSIX_C_SOURCE has a negative effect at least of MacOS, FreeBSD, NetBSD, Solaris:
 * - Defining _POSIX_C_SOURCE hides all system extensions, including even MAP_ANON
 * - Undefining _POSIX_C_SOURCE specifically on those systems actually exposes latest POSIX
 */
#undef _POSIX_SOURCE
#undef _POSIX_C_SOURCE
#if !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(__NetBSD__) && !defined(__sun)
#define _POSIX_C_SOURCE 200809L
#endif

/*
 * Enable GNU & BSD extensions
 * - Exposes syscall(), sysconf(), madvise(), mremap(), fallocate(), ftruncate(), prctl(), pledge()
 * - Exposes kqueue(), kevent() on BSDs (Except NetBSD, where _NETBSD_SOURCE is needed)
 * - Exposes fspacectl() on FreeBSD 14+
 * - Exposes MAP_ANON / MAP_ANONYMOUS, O_CLOEXEC, O_NOATIME, O_NOCTTY, etc
 */
#undef _GNU_SOURCE
#define _GNU_SOURCE
#undef _BSD_SOURCE
#define _BSD_SOURCE
#undef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE

/*
 * Enable Darwin extensions
 * - Exposes pthread_cond_timedwait_relative_np()
 * - Exposes F_PUNCHHOLE, F_BARRIERFSYNC, F_FULLFSYNC fcntl() codes
 * - Allows unlimited nfds to select(), other systems already have this
 */
#undef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#undef _DARWIN_UNLIMITED_SELECT
#define _DARWIN_UNLIMITED_SELECT

/*
 * Enable NetBSD extensions
 * - Exposes fdiscard() on NetBSD 7+, kqueue(), kevent()
 */
#undef _NETBSD_SOURCE
#define _NETBSD_SOURCE

/*
 * Enable POSIX-compatible threading semantics on Solaris
 */
#undef _POSIX_PTHREAD_SEMANTICS
#define _POSIX_PTHREAD_SEMANTICS

/*
 * Force 64-bit file offsets & time (off_t & time_t)
 */
#undef _TIME_BITS
#define _TIME_BITS 64
#undef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64

/*
 * Expose only a minimal WinAPI subset from <windows.h>
 */
#undef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1

/*
 * Detect POSIX-compatible systems
 */
#undef HOST_TARGET_POSIX
#if defined(__unix__) || defined(__APPLE__) || defined(__HAIKU__)
#define HOST_TARGET_POSIX 1
#endif

/*
 * Detect common Win32 systems (Windows NT, Windows 9x, Windows CE)
 */
#undef HOST_TARGET_WIN32
#if defined(_WIN32)
#define HOST_TARGET_WIN32 1
#endif

/*
 * Detect Windows CE systems
 * Those have a cut down WinAPI, roughly resembling NT 3.51 without ANSI
 */
#undef HOST_TARGET_WINCE
#if defined(HOST_TARGET_WIN32) && defined(UNDER_CE)
#define HOST_TARGET_WINCE 1
#endif

#endif
