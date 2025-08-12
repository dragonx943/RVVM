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
 * Detect most POSIX/Win32 systems we ever wish to support
 *
 * - Any POSIX-compliant system defines HOST_TARGET_POSIX
 * - Any Win32 flavor (NT, 9x, CE) defines HOST_TARGET_WIN32
 * - Windows CE additionally defines HOST_TARGET_WINCE
 * - Windows NT and it's small cousin 9x define HOST_TARGET_WINNT
 * - Any BSD system defines HOST_TARGET_BSD
 * - HOST_TARGET_ANDROID implies HOST_TARGET_LINUX
 * - HOST_TARGET_HAIKU implies HOST_TARGET_BEOS
 * - HOST_TARGET_ILLUMOS implies HOST_TARGET_SOLARIS
 * - FreeBSD, NetBSD, Apple define a major OS release instead of a bare literal `1`
 */
#undef HOST_TARGET_POSIX
#undef HOST_TARGET_WIN32
#undef HOST_TARGET_WINNT
#undef HOST_TARGET_WINCE
#undef HOST_TARGET_EMSCRIPTEN
#undef HOST_TARGET_SERENITY
#undef HOST_TARGET_ANDROID
#undef HOST_TARGET_ILLUMOS
#undef HOST_TARGET_SOLARIS
#undef HOST_TARGET_DFLYBSD
#undef HOST_TARGET_FREEBSD
#undef HOST_TARGET_OPENBSD
#undef HOST_TARGET_NETBSD
#undef HOST_TARGET_CYGWIN
#undef HOST_TARGET_DARWIN
#undef HOST_TARGET_AMIGA
#undef HOST_TARGET_APPLE
#undef HOST_TARGET_HAIKU
#undef HOST_TARGET_LINUX
#undef HOST_TARGET_MINIX
#undef HOST_TARGET_OS400
#undef HOST_TARGET_REDOX
#undef HOST_TARGET_BEOS
#undef HOST_TARGET_HPUX
#undef HOST_TARGET_HURD
#undef HOST_TARGET_IRIX
#undef HOST_TARGET_AIX
#undef HOST_TARGET_BSD
#undef HOST_TARGET_DOS
#undef HOST_TARGET_VMS
#undef HOST_TARGET_QNX

#if defined(_WIN32)
#define HOST_TARGET_WIN32 1
#if defined(UNDER_CE)
#define HOST_TARGET_WINCE 1
#else
/*
 * Actually either NT or 9x
 */
#define HOST_TARGET_WINNT 1
#endif
#elif defined(__EMSCRIPTEN__)
#define HOST_TARGET_POSIX      1
#define HOST_TARGET_EMSCRIPTEN 1
#elif defined(__serenity__)
#define HOST_TARGET_POSIX    1
#define HOST_TARGET_SERENITY 1
#elif defined(__sun)
#define HOST_TARGET_POSIX   1
#define HOST_TARGET_SOLARIS 1
#if defined(__illumos__)
#define HOST_TARGET_ILLUMOS 1
#endif
#elif defined(__DragonFly__)
#define HOST_TARGET_POSIX   1
#define HOST_TARGET_BSD     1
#define HOST_TARGET_DFLYBSD 1
#elif defined(__FreeBSD__)
#define HOST_TARGET_POSIX   1
#define HOST_TARGET_BSD     1
#define HOST_TARGET_FREEBSD ((__FreeBSD__ - 0) ? (__FreeBSD__ - 0) : 14)
#elif defined(__OpenBSD__)
#define HOST_TARGET_POSIX   1
#define HOST_TARGET_BSD     1
#define HOST_TARGET_OPENBSD 1
#elif defined(__NetBSD__)
/*
 * Need to include <sys/param.h> for NetBSD version detection, assumes NetBSD 10 otherwise
 */
#define HOST_TARGET_POSIX  1
#define HOST_TARGET_BSD    1
#define HOST_TARGET_NETBSD ((__NetBSD_Version__ - 0) ? ((__NetBSD_Version__ - 0) / 100000000) : 10)
#elif defined(__CYGWIN__)
/*
 * Cygwin also support Win32 API, but let's not be a Frakenstein
 */
#define HOST_TARGET_POSIX  1
#define HOST_TARGET_CYGWIN 1
#elif defined(__amigaos__)
/*
 * AmigaOS has optional POSIX compatibility layers (ixemul, clib)
 */
#define HOST_TARGET_POSIX 1
#define HOST_TARGET_AMIGA 1
#elif defined(__APPLE__)
#define HOST_TARGET_POSIX 1
#define HOST_TARGET_APPLE                                                                                              \
    ((__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ - 0) /**/                                                          \
         ? ((__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ - 0) / 100)                                                 \
         : 11)
#define HOST_TARGET_DARWIN HOST_TARGET_APPLE
#elif defined(__HAIKU__)
/*
 * Consider Haiku compatible with BeOS if we ever have BeOS-specific code
 */
#define HOST_TARGET_POSIX 1
#define HOST_TARGET_HAIKU 1
#define HOST_TARGET_BEOS  1
#elif defined(__linux__)
#define HOST_TARGET_POSIX 1
#define HOST_TARGET_LINUX 1
#if defined(__ANDROID__)
#define HOST_TARGET_ANDROID 1
#endif
#elif defined(__minix)
#define HOST_TARGET_POSIX 1
#define HOST_TARGET_MINIX 1
#elif defined(__redox__)
#define HOST_TARGET_POSIX 1
#define HOST_TARGET_REDOX 1
#elif defined(__OS400__)
#define HOST_TARGET_POSIX 1
#define HOST_TARGET_OS400 1
#elif defined(__BEOS__)
#define HOST_TARGET_POSIX 1
#define HOST_TARGET_BEOS  1
#elif defined(__hpux) || defined(_hpux)
#define HOST_TARGET_POSIX 1
#define HOST_TARGET_HPUX  1
#elif defined(__GNU__) || defined(__gnu_hurd__)
#define HOST_TARGET_POSIX 1
#define HOST_TARGET_HURD  1
#elif defined(__sgi)
#define HOST_TARGET_POSIX 1
#define HOST_TARGET_IRIX  1
#elif defined(__AIX)
#define HOST_TARGET_POSIX 1
#define HOST_TARGET_VMS   1
#elif defined(__MSDOS__) || defined(__MSDOS) || defined(__DJGPP__) || defined(__DJGPP)
#define HOST_TARGET_DOS 1
#elif defined(__VMS)
#define HOST_TARGET_POSIX 1
#define HOST_TARGET_VMS   1
#elif defined(__QNX__)
#define HOST_TARGET_POSIX 1
#define HOST_TARGET_QNX   1
#elif defined(__unix__) || defined(__unix) || defined(unix)
/*
 * An unknown Unix variant. Who are you, warrior?
 */
#define HOST_TARGET_POSIX 1
#endif

/*
 * Enable POSIX.1-2008
 * - Exposes pread(), pwrite(), readlink(), O_CLOEXEC
 * - Exposes posix_fallocate() on Linux, FreeBSD 9+, NetBSD 7+
 * - Exposes fdatasync() on Linux, FreeBSD 12+, NetBSD
 * - Exposes clock_gettime(), nanosleep(), sched_yield()
 * - Exposes fileno()
 * - Exposes pthread_condattr_setclock()
 *
 * NOTE: Defining _POSIX_C_SOURCE has a negative effect at least of MacOS, Solaris, FreeBSD, NetBSD:
 * - Defining _POSIX_C_SOURCE hides all system extensions, including even MAP_ANON
 * - Undefining _POSIX_C_SOURCE specifically on those systems actually exposes latest POSIX
 */
#undef _POSIX_SOURCE
#undef _POSIX_C_SOURCE
#if !defined(HOST_TARGET_APPLE) && !defined(HOST_TARGET_SOLARIS) /**/                                                  \
    && !defined(HOST_TARGET_FREEBSD) && !defined(HOST_TARGET_NETBSD)
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
#define _GNU_SOURCE 1
#undef _BSD_SOURCE
#define _BSD_SOURCE 1
#undef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1

/*
 * Enable Darwin extensions
 * - Exposes pthread_cond_timedwait_relative_np()
 * - Exposes F_PUNCHHOLE, F_BARRIERFSYNC, F_FULLFSYNC fcntl() codes
 * - Allows unlimited nfds to select(), other systems already have this
 */
#undef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#undef _DARWIN_UNLIMITED_SELECT
#define _DARWIN_UNLIMITED_SELECT 1

/*
 * Enable NetBSD extensions
 * - Exposes fdiscard() on NetBSD 7+, kqueue(), kevent()
 */
#undef _NETBSD_SOURCE
#define _NETBSD_SOURCE 1

/*
 * Enable POSIX-compatible threading semantics on Solaris
 */
#undef _POSIX_PTHREAD_SEMANTICS
#define _POSIX_PTHREAD_SEMANTICS 1

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

#endif
