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
 * Detect most operating systems we ever wish to support.
 *
 * Any POSIX system defines HOST_TARGET_POSIX to expected POSIX version (200809L, etc):
 * - POSIX 1003.1b-1993 (Issue 4): 199309L (Introduced shared memory, nanosleep, clock_gettime, etc)
 * - POSIX 1003.1c-1995 (Issue 5): 199506L (Introduced pthreads)
 * - POSIX 1003.1-2001  (Issue 6): 200112L (Introduced pread/pwrite, CLOCK_MONOTONIC, etc)
 * - POSIX 1003.1-2008  (Issue 7): 200809L (Introduced openat, posix_spawn, pthread_cond clock attribute, etc)
 *
 * - Any BSD defines HOST_TARGET_BSD
 * - Android defines HOST_TARGET_LINUX
 * - HaikuOS defines HOST_TARGET_BEOS
 * - Illumos defines HOST_TARGET_SOLARIS
 * - FreeBSD, NetBSD, Apple define a major OS release
 *
 * - Any Win32 flavor (NT, 9x, CE) defines HOST_TARGET_WIN32
 * - Windows CE defines HOST_TARGET_WINCE
 * - Windows NT defines HOST_TARGET_WINNT to minimum expected major NT version
 * - Windows on i386 additionally defines HOST_TARGET_WIN9X
 */
#undef HOST_TARGET_POSIX
#undef HOST_TARGET_WIN32
#undef HOST_TARGET_WINNT
#undef HOST_TARGET_WINCE
#undef HOST_TARGET_WIN9X
#undef HOST_TARGET_COSMOPOLITAN
#undef HOST_TARGET_EMSCRIPTEN
#undef HOST_TARGET_SERENITY
#undef HOST_TARGET_ANDROID
#undef HOST_TARGET_ILLUMOS
#undef HOST_TARGET_SOLARIS
#undef HOST_TARGET_DRAGONFLYBSD
#undef HOST_TARGET_MIDNIGHTBSD
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
#if defined(__x86_64__) || defined(_M_X64)
/*
 * Windows NT 5.x+ on x86_64
 */
#define HOST_TARGET_WINNT 5
#elif defined(__aarch64__) || defined(_M_ARM64)
/*
 * Windows NT 6.x+ on arm64
 */
#define HOST_TARGET_WINNT 6
#elif defined(UNDER_CE) || defined(_WIN32_WCE)
/*
 * Windows CE
 */
#define HOST_TARGET_WINCE 1
#else
/*
 * Windows NT 3.x+ on i386/mips/powerpc/alpha
 */
#define HOST_TARGET_WINNT 3
#if defined(__i386__) || defined(_M_IX86)
/*
 * Either WinNT or Win9x on i386, so ask for Win9x support
 */
#define HOST_TARGET_WIN9X 1
#endif
#endif
#elif defined(__COSMOPOLITAN__)
#define HOST_TARGET_POSIX        200809L
#define HOST_TARGET_COSMOPOLITAN 1
#elif defined(__EMSCRIPTEN__)
#define HOST_TARGET_POSIX      200809L
#define HOST_TARGET_EMSCRIPTEN 1
#elif defined(__serenity__)
#define HOST_TARGET_POSIX    200809L
#define HOST_TARGET_SERENITY 1
#elif defined(__sun) && defined(__SVR4)
#define HOST_TARGET_POSIX   200809L
#define HOST_TARGET_SOLARIS 1
#if defined(__illumos__)
#define HOST_TARGET_ILLUMOS 1
#endif
#elif defined(__DragonFly__)
#define HOST_TARGET_POSIX        200809L
#define HOST_TARGET_BSD          1
#define HOST_TARGET_DRAGONFLYBSD 1
#elif defined(__MidnightBSD__)
#define HOST_TARGET_POSIX       200809L
#define HOST_TARGET_BSD         1
#define HOST_TARGET_MIDNIGHTBSD 1
#elif defined(__FreeBSD__)
#define HOST_TARGET_POSIX   200809L
#define HOST_TARGET_BSD     1
#define HOST_TARGET_FREEBSD ((__FreeBSD__ - 0) ? (__FreeBSD__ - 0) : 14)
#elif defined(__OpenBSD__)
#define HOST_TARGET_POSIX   200809L
#define HOST_TARGET_BSD     1
#define HOST_TARGET_OPENBSD 1
#elif defined(__NetBSD__)
/*
 * Need to include <sys/param.h> for NetBSD version detection, assumes NetBSD 10 otherwise
 */
#define HOST_TARGET_POSIX  200809L
#define HOST_TARGET_BSD    1
#define HOST_TARGET_NETBSD ((__NetBSD_Version__ - 0) ? ((__NetBSD_Version__ - 0) / 100000000) : 10)
#elif defined(__CYGWIN__)
/*
 * Cygwin also support Win32 API, but let's not be a Frakenstein
 */
#define HOST_TARGET_POSIX  200809L
#define HOST_TARGET_CYGWIN 1
#elif defined(__amigaos__)
/*
 * Pretty much only clock_gettime() and pthreads work with ixemul
 */
#define HOST_TARGET_AMIGA 1
#elif defined(__APPLE__)
#define HOST_TARGET_POSIX 200112L
#define HOST_TARGET_APPLE                                                                                              \
    ((__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ - 0) /**/                                                          \
         ? ((__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ - 0) / 100)                                                 \
         : 11)
#define HOST_TARGET_DARWIN HOST_TARGET_APPLE
#elif defined(__HAIKU__)
/*
 * Consider Haiku compatible with BeOS if we ever have BeOS-specific code
 */
#define HOST_TARGET_POSIX 200809L
#define HOST_TARGET_HAIKU 1
#define HOST_TARGET_BEOS  1
#elif defined(__linux__)
#define HOST_TARGET_POSIX 200809L
#define HOST_TARGET_LINUX 1
#if defined(__ANDROID__)
#define HOST_TARGET_ANDROID 1
#endif
#elif defined(__minix)
#define HOST_TARGET_POSIX 200112L
#define HOST_TARGET_MINIX 1
#elif defined(__redox__)
#define HOST_TARGET_POSIX 200809L
#define HOST_TARGET_REDOX 1
#elif defined(__OS400__)
#define HOST_TARGET_POSIX 200112L
#define HOST_TARGET_OS400 1
#elif defined(__BEOS__)
/*
 * BeOS lacks much of POSIX (No mmap/pthreads)
 */
#define HOST_TARGET_BEOS 1
#elif defined(__hpux) || defined(_hpux)
#define HOST_TARGET_POSIX 200112L
#define HOST_TARGET_HPUX  1
#elif defined(__GNU__) || defined(__gnu_hurd__)
#define HOST_TARGET_POSIX 200809L
#define HOST_TARGET_HURD  1
#elif defined(__IRIX__) || defined(__sgi)
#define HOST_TARGET_POSIX 199506L
#define HOST_TARGET_IRIX  1
#elif defined(__AIX)
#define HOST_TARGET_POSIX 200112L
#define HOST_TARGET_AIX   1
#elif defined(__MSDOS__) || defined(__MSDOS) || defined(__DJGPP__) || defined(__DJGPP)
#define HOST_TARGET_DOS 1
#elif defined(__VMS)
#define HOST_TARGET_POSIX 199506L
#define HOST_TARGET_VMS   1
#elif defined(__QNX__)
#define HOST_TARGET_POSIX 200809L
#define HOST_TARGET_QNX   1
#elif defined(__unix__) || defined(__unix) || defined(unix)
/*
 * An unknown Unix variant. Who are you, warrior?
 */
#define HOST_TARGET_POSIX 200112L
#endif

/*
 * NOTE: Defining _POSIX_C_SOURCE has a negative effect at least on following systems:
 * - MacOS, Solaris, FreeBSD, DragonFlyBSD, NetBSD
 *
 * Defining _POSIX_C_SOURCE on those system completely hides system extensions, like
 * MAP_ANON and other widely used interfaces.
 *
 * The following systems are verified to not require _POSIX_C_SOURCE in presence of other macros:
 * - Cosmopolitan, Emscripten, Serenity, Solaris, *BSD, Cygwin, Darwin, Haiku, Linux, GNU Hurd
 *
 * If any platform needs to define _POSIX_C_SOURCE in future, it should be done here.
 *
 * Expected features from defining _POSIX_C_SOURCE=200809L (POSIX 1003.1-2008):
 * - Exposes pread(), pwrite(), readlink(), O_CLOEXEC
 * - Exposes posix_fallocate() on Linux, FreeBSD 9+, NetBSD 7+
 * - Exposes fdatasync() on Linux, FreeBSD 12+, NetBSD
 * - Exposes clock_gettime(), nanosleep(), sched_yield()
 * - Exposes fileno()
 * - Exposes pthread_condattr_setclock()
 */
#undef _POSIX_SOURCE
#undef _XOPEN_SOURCE
#undef _POSIX_C_SOURCE
#undef _XOPEN_SOURCE_EXTENDED

/*
 * Allow non-standard language extensions
 */
#undef __STRICT_ANSI__

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
 * Enable AIX extensions
 */
#undef _ALL_SOURCE
#define _ALL_SOURCE 1

/*
 * Enable general Solaris extensions
 */
#undef __EXTENSIONS__
#define __EXTENSIONS__ 1

/*
 * Enable POSIX-compatible threading semantics on Solaris
 */
#undef _POSIX_PTHREAD_SEMANTICS
#define _POSIX_PTHREAD_SEMANTICS 1

/*
 * Enable reentrant standard library semantics (For errno, stdio)
 */
#undef _REENTRANT
#define _REENTRANT 1

/*
 * Force 64-bit file offsets & time (off_t & time_t)
 */
#undef _TIME_BITS
#define _TIME_BITS 64
#undef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64

/*
 * Expose strict minimal WinAPI subset from <windows.h>
 */
#undef STRICT
#define STRICT 1
#undef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1

#endif
