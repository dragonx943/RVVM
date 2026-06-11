/*
rvvm_isolation.c - Process & thread isolation
Copyright (C) 2024  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

// Expose getpwnam_r(), prctl(), pledge()
#include "feature_test.h"

#include "compiler.h"
#include "rvvm_isolation.h"
#include "utils.h"

PUSH_OPTIMIZATION_SIZE

// Do not isolate under sanitizers to prevent breakage
#if !defined(SANITIZERS_ENABLED)

#if defined(USE_ISOLATION) && (defined(HOST_TARGET_LINUX) || defined(HOST_TARGET_BSD) || defined(HOST_TARGET_SOLARIS))

#include <errno.h>  // For errno, ENOSYS, EINVAL, etc
#include <pwd.h>    // For struct passwd, getpwnam_r()
#include <string.h> // For strerror()
#include <unistd.h> // For getuid(), setgid(), setuid(), chdir()

#define ISOLATION_DROP_ROOT_IMPL 1

#endif

#if defined(USE_ISOLATION) && defined(HOST_TARGET_LINUX) && CHECK_INCLUDE(sys/prctl.h, 1)

#include <errno.h>     // For errno, ENOSYS, EINVAL, etc
#include <sys/prctl.h> // For prctl()

#define ISOLATION_PRCTL_IMPL 1

#endif

#if defined(USE_ISOLATION) && defined(HOST_TARGET_LINUX) && defined(GNU_EXTS) && defined(ISOLATION_PRCTL_IMPL)         \
    && CHECK_INCLUDE(linux/seccomp.h, 0)

#include <errno.h>         // For errno, ENOSYS, EINVAL, etc
#include <linux/filter.h>  // For struct sock_filter, struct sock_fprog, BPF_STMT, etc
#include <linux/seccomp.h> // For SECCOMP_RET_ALLOW, SECCOMP_RET_ERRNO, SECCOMP_RET_DATA, etc
#include <linux/unistd.h>  // For syscall __NR_* definitions
#include <string.h>        // For strerror()
#include <sys/mman.h>      // For PROT_EXEC, PROT_WRITE

#define ISOLATION_SECCOMP_IMPL 1

#endif

#if defined(USE_ISOLATION) && defined(HOST_TARGET_OPENBSD)

#include <unistd.h> // For pledge()

#define ISOLATION_PLEDGE_IMPL 1

#endif

#endif

#if defined(ISOLATION_DROP_ROOT_IMPL)

// Drop from root user to nobody
static void drop_root_user_once(void)
{
    /*
     * On Linux, UID/GID are per-thread properties and the raw
     * setuid syscall only applies new UID to the calling thread.
     *
     * Glibc broadcasts a signal to all existing threads to
     * implement a POSIX-compliant setuid(), however this opens
     * a whole new can of worms - an already isolated thread
     * can't setuid() anymore - so glibc aborts the process...
     *
     * Currently it's fixed by dropping root before any kind of
     * isolation happens, and it's done under a DO_ONCE() construct
     * to prevent race conditions.
     */
    if (getuid() == 0) {
        // We are root, drop to nobody
        char           buffer[256] = {0};
        struct passwd  pwd         = {0};
        struct passwd* result      = NULL;
        rvvm_info("Dropping from root user to nobody");
        if (getpwnam_r("nobody", &pwd, buffer, sizeof(buffer), &result)) {
            rvvm_info("Failed to get GID/UID of user nobody");
            return;
        }
        if (setgid(pwd.pw_gid) || setuid(pwd.pw_uid) || getuid() == 0) {
            rvvm_fatal("Failed to drop root privileges!");
        }
        UNUSED(!chdir("/"));
    }
}

static void drop_root_user(void)
{
    DO_ONCE(drop_root_user_once());
}

#endif

#if defined(ISOLATION_PRCTL_IMPL)

// Drop all the capabilities of the calling thread, prevent privilege escalation
static void drop_thread_caps(void)
{
#if defined(PR_SET_NO_NEW_PRIVS)
    // Prevent privilege escalation via setuid etc
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) && errno != ENOSYS) {
        DO_ONCE(rvvm_warn("Failed to set PR_SET_NO_NEW_PRIVS!"));
    }
#endif

#if defined(PR_CAPBSET_DROP)
    // Drop all capabilities
    for (int cap = 0; cap < 64; ++cap) {
        UNUSED(!prctl(PR_CAPBSET_DROP, cap, 0, 0, 0));
    }
#endif
}

#endif

#if defined(ISOLATION_SECCOMP_IMPL)

#if defined(__riscv) && !defined(__NR_riscv_flush_icache)
// Allow icache flush if no syscall definition is provided
#define __NR_riscv_flush_icache 259
#endif

// clang-format off

// Allow specific syscall in seccomp BPF filter
#define BPF_SECCOMP_ALLOW_SYSCALL(syscall)                                          \
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, (syscall), 0, 1),                       \
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

// Return errno in seccomp BPF filter
#define BPF_SECCOMP_ERRNO(errno)                                                    \
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ERRNO | ((errno) & SECCOMP_RET_DATA)),

// Return errno for specific syscall in seccomp BPF filter
#define BPF_SECCOMP_ERRNO_SYSCALL(syscall, errno)                                   \
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, (syscall), 0, 1),                       \
        BPF_SECCOMP_ERRNO(errno)

// May be used to block RWX mmap(), mprotect()
#define BPF_SECCOMP_BLOCK_RWX_MMAN(syscall)                                         \
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, syscall, 0, 5),                         \
        BPF_STMT(BPF_LD + BPF_W + BPF_ABS, offsetof(struct seccomp_data, args[2])), \
        BPF_STMT(BPF_ALU + BPF_AND + BPF_K, (PROT_EXEC | PROT_WRITE)),              \
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, (PROT_EXEC | PROT_WRITE), 0, 1),        \
        BPF_SECCOMP_ERRNO(EINVAL) \
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

static void seccomp_setup_syscall_filter(bool all_threads) {
    /*
     * Let's just hope this won't blow up out of nowhere.
     *
     * General idea:
     * - Any operation on already open FDs is allowed
     * - Access to filesystem is prohibited
     * - Executing other programs is prohibited
     * - RWX mmap() / mprotect() is prohibited with EINVAL
     * - IO_URING usage is prohibited
     * - Any unhandled syscall returns ENOSYS
     *
     * Most syscalls are covered under #ifdef:
     * - Some are arch-specific or name varies across architectures
     * - Allows to build on a fairly old system with missing syscalls
     */
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD + BPF_W + BPF_ABS, offsetof(struct seccomp_data, nr)),

        // Fast path for frequent syscalls
#ifdef __NR_sched_yield
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_sched_yield)
#endif
#ifdef __NR_futex
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_futex)
#endif
#ifdef __NR_futex_time64
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_futex_time64)
#endif
#ifdef __NR_futex_wake
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_futex_wake)
#endif
#ifdef __NR_futex_wait
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_futex_wait)
#endif
#ifdef __NR_futex_requeue
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_futex_requeue)
#endif
#ifdef __NR_futex_waitv
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_futex_waitv)
#endif

#ifdef __NR_ioctl
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_ioctl)
#endif
#ifdef __NR_pread64
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_pread64)
#endif
#ifdef __NR_pwrite64
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_pwrite64)
#endif
#ifdef __NR_epoll_wait
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_epoll_wait)
#endif
#ifdef __NR_epoll_pwait
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_epoll_pwait)
#endif
#ifdef __NR_epoll_pwait2
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_epoll_pwait2)
#endif
#ifdef __NR_epoll_ctl
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_epoll_ctl)
#endif
#ifdef __NR_clock_gettime
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_clock_gettime)
#endif
#ifdef __NR_nanosleep
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_nanosleep)
#endif
#ifdef __NR_read
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_read)
#endif
#ifdef __NR_write
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_write)
#endif

        // Common allowed syscalls (Sorted as in uapi/asm-generic/unistd.h)
#ifdef __NR_eventfd
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_eventfd)
#endif
#ifdef __NR_eventfd2
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_eventfd2)
#endif
#ifdef __NR_epoll_create1
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_epoll_create1)
#endif
#ifdef __NR_dup
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_dup)
#endif
#ifdef __NR_dup2
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_dup2)
#endif
#ifdef __NR_dup3
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_dup3)
#endif
#ifdef __NR_fcntl
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_fcntl)
#endif
#ifdef __NR_fcntl64
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_fcntl64)
#endif
#ifdef __NR_ioprio_set
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_ioprio_set)
#endif
#ifdef __NR_ioprio_get
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_ioprio_get)
#endif
#ifdef __NR_flock
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_flock)
#endif
#ifdef __NR_ftruncate
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_ftruncate)
#endif
#ifdef __NR_ftruncate64
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_ftruncate64)
#endif
#ifdef __NR_fallocate
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_fallocate)
#endif
#ifdef __NR_close
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_close)
#endif
#ifdef __NR_pipe
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_pipe)
#endif
#ifdef __NR_pipe2
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_pipe2)
#endif
#ifdef __NR_lseek
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_lseek)
#endif
#ifdef __NR__llseek
        BPF_SECCOMP_ALLOW_SYSCALL(__NR__llseek)
#endif
#ifdef __NR_readv
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_readv)
#endif
#ifdef __NR_writev
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_writev)
#endif
#ifdef __NR_preadv
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_preadv)
#endif
#ifdef __NR_pwritev
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_pwritev)
#endif
#ifdef __NR_sendfile
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_sendfile)
#endif
#ifdef __NR_sendfile64
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_sendfile64)
#endif
#ifdef __NR_select
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_select)
#endif
#ifdef __NR_pselect6
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_pselect6)
#endif
#ifdef __NR__newselect
        BPF_SECCOMP_ALLOW_SYSCALL(__NR__newselect)
#endif
#ifdef __NR_poll
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_poll)
#endif
#ifdef __NR_ppoll
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_ppoll)
#endif
#ifdef __NR_signalfd
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_signalfd)
#endif
#ifdef __NR_signalfd4
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_signalfd4)
#endif
#ifdef __NR_vmsplice
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_vmsplice)
#endif
#ifdef __NR_splice
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_splice)
#endif
#ifdef __NR_tee
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_tee)
#endif
#ifdef __NR_fstat
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_fstat)
#endif
#ifdef __NR_fstat64
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_fstat64)
#endif
#ifdef __NR_sync
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_sync)
#endif
#ifdef __NR_fsync
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_fsync)
#endif
#ifdef __NR_fdatasync
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_fdatasync)
#endif
#ifdef __NR_sync_file_range
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_sync_file_range)
#endif
#ifdef __NR_sync_file_range2
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_sync_file_range2)
#endif
#ifdef __NR_timerfd_create
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_timerfd_create)
#endif
#ifdef __NR_timerfd_settime
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_timerfd_settime)
#endif
#ifdef __NR_timerfd_settime64
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_timerfd_settime64)
#endif
#ifdef __NR_timerfd_gettime
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_timerfd_gettime)
#endif
#ifdef __NR_timerfd_gettime64
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_timerfd_gettime64)
#endif
#ifdef __NR_exit
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_exit)
#endif
#ifdef __NR_exit_group
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_exit_group)
#endif
#ifdef __NR_waitid
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_waitid)
#endif
#ifdef __NR_set_tid_address
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_set_tid_address)
#endif
#ifdef __NR_set_robust_list
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_set_robust_list)
#endif
#ifdef __NR_get_robust_list
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_get_robust_list)
#endif
#ifdef __NR_alarm
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_alarm)
#endif
#ifdef __NR_getitimer
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_getitimer)
#endif
#ifdef __NR_setitimer
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_setitimer)
#endif
#ifdef __NR_timer_create
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_timer_create)
#endif
#ifdef __NR_timer_gettime
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_timer_gettime)
#endif
#ifdef __NR_timer_getoverrun
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_timer_getoverrun)
#endif
#ifdef __NR_timer_settime
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_timer_settime)
#endif
#ifdef __NR_timer_delete
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_timer_delete)
#endif
#ifdef __NR_clock_getres
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_clock_getres)
#endif
#ifdef __NR_clock_nanosleep
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_clock_nanosleep)
#endif
#ifdef __NR_pause
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_pause)
#endif
#ifdef __NR_sched_setparam
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_sched_setparam)
#endif
#ifdef __NR_sched_setscheduler
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_sched_setscheduler)
#endif
#ifdef __NR_sched_getscheduler
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_sched_getscheduler)
#endif
#ifdef __NR_sched_getparam
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_sched_getparam)
#endif
#ifdef __NR_sched_setaffinity
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_sched_setaffinity)
#endif
#ifdef __NR_sched_getaffinity
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_sched_getaffinity)
#endif
#ifdef __NR_set_thread_area
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_set_thread_area)
#endif
#ifdef __NR_get_thread_area
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_get_thread_area)
#endif
#ifdef __NR_epoll_create
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_epoll_create)
#endif
#ifdef __NR_epoll_ctl_old
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_epoll_ctl_old)
#endif
#ifdef __NR_epoll_wait_old
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_epoll_wait_old)
#endif
#ifdef __NR_sched_get_priority_max
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_sched_get_priority_max)
#endif
#ifdef __NR_sched_get_priority_min
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_sched_get_priority_min)
#endif
#ifdef __NR_sched_rr_get_interval
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_sched_rr_get_interval)
#endif
#ifdef __NR_restart_syscall
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_restart_syscall)
#endif
#ifdef __NR_tgkill
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_tgkill)
#endif
#ifdef __NR_sigaltstack
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_sigaltstack)
#endif
#ifdef __NR_rt_sigsuspend
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_rt_sigsuspend)
#endif
#ifdef __NR_rt_sigaction
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_rt_sigaction)
#endif
#ifdef __NR_rt_sigprocmask
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_rt_sigprocmask)
#endif
#ifdef __NR_rt_sigpending
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_rt_sigpending)
#endif
#ifdef __NR_rt_sigtimedwait
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_rt_sigtimedwait)
#endif
#ifdef __NR_rt_sigqueueinfo
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_rt_sigqueueinfo)
#endif
#ifdef __NR_rt_sigreturn
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_rt_sigreturn)
#endif
#ifdef __NR_setpriority
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_setpriority)
#endif
#ifdef __NR_getpriority
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_getpriority)
#endif
#ifdef __NR_time
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_time)
#endif
#ifdef __NR_times
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_times)
#endif
#ifdef __NR_getpgid
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_getpgid)
#endif
#ifdef __NR_getsid
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_getsid)
#endif
#ifdef __NR_getgroups
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_getgroups)
#endif
#ifdef __NR_uname
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_uname)
#endif
#ifdef __NR_ugetrlimit
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_ugetrlimit)
#endif
#ifdef __NR_getrlimit
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_getrlimit)
#endif
#ifdef __NR_setrlimit
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_setrlimit)
#endif
#ifdef __NR_getrusage
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_getrusage)
#endif
#ifdef __NR_prctl
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_prctl)
#endif
#ifdef __NR_getcpu
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_getcpu)
#endif
#ifdef __NR_gettimeofday
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_gettimeofday)
#endif
#ifdef __NR_getpid
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_getpid)
#endif
#ifdef __NR_getppid
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_getppid)
#endif
#ifdef __NR_getuid
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_getuid)
#endif
#ifdef __NR_geteuid
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_geteuid)
#endif
#ifdef __NR_getresuid
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_getresuid)
#endif
#ifdef __NR_getgid
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_getgid)
#endif
#ifdef __NR_getegid
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_getegid)
#endif
#ifdef __NR_getresgid
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_getresgid)
#endif
#ifdef __NR_getpgrp
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_getpgrp)
#endif
#ifdef __NR_gettid
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_gettid)
#endif
#ifdef __NR_sysinfo
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_sysinfo)
#endif
#ifdef __NR_shmget
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_shmget)
#endif
#ifdef __NR_shmat
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_shmat)
#endif
#ifdef __NR_shmctl
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_shmctl)
#endif
#ifdef __NR_shmdt
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_shmdt)
#endif

        // Allow networking
#ifdef __NR_socket
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_socket)
#endif
#ifdef __NR_socketpair
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_socketpair)
#endif
#ifdef __NR_bind
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_bind)
#endif
#ifdef __NR_listen
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_listen)
#endif
#ifdef __NR_accept
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_accept)
#endif
#ifdef __NR_connect
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_connect)
#endif
#ifdef __NR_getsockname
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_getsockname)
#endif
#ifdef __NR_getpeername
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_getpeername)
#endif
#ifdef __NR_sendto
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_sendto)
#endif
#ifdef __NR_recvfrom
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_recvfrom)
#endif
#ifdef __NR_setsockopt
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_setsockopt)
#endif
#ifdef __NR_getsockopt
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_getsockopt)
#endif
#ifdef __NR_socketcall
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_socketcall)
#endif
#ifdef __NR_shutdown
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_shutdown)
#endif
#ifdef __NR_sendmsg
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_sendmsg)
#endif
#ifdef __NR_recvmsg
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_recvmsg)
#endif
#ifdef __NR_readahead
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_readahead)
#endif
#ifdef __NR_brk
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_brk)
#endif
#ifdef __NR_munmap
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_munmap)
#endif
#ifdef __NR_mremap
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_mremap)
#endif
#ifdef __NR_clone
        // Only allow subset of clone() flags required for pthread_create()
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_clone, 0, 5),
        BPF_STMT(BPF_LD + BPF_W + BPF_ABS, offsetof(struct seccomp_data, args[0])),
        BPF_STMT(BPF_ALU + BPF_AND + BPF_K, 0x7E026000U),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, 0, 1, 0),
        BPF_SECCOMP_ERRNO(EINVAL)
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
#endif
#ifdef __NR_fadvise
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_fadvise)
#endif
#ifdef __NR_fadvise64
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_fadvise64)
#endif
#ifdef __NR_msync
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_msync)
#endif
#ifdef __NR_mlock
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_mlock)
#endif
#ifdef __NR_munlock
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_munlock)
#endif
#ifdef __NR_mincore
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_mincore)
#endif
#ifdef __NR_madvise
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_madvise)
#endif
#ifdef __NR_mbind
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_mbind)
#endif
#ifdef __NR_set_mempolicy
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_get_mempolicy)
#endif
#ifdef __NR_set_mempolicy
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_get_mempolicy)
#endif
#ifdef __NR_migrate_pages
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_migrate_pages)
#endif
#ifdef __NR_move_pages
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_move_pages)
#endif
#ifdef __NR_rt_tgsigqueueinfo
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_rt_tgsigqueueinfo)
#endif
#ifdef __NR_perf_event_open
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_perf_event_open)
#endif
#ifdef __NR_accept4
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_accept4)
#endif
#ifdef __NR_recvmmsg
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_recvmmsg)
#endif
#ifdef __NR_wait4
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_wait4)
#endif
#ifdef __NR_prlimit64
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_prlimit64)
#endif
#ifdef __NR_syncfs
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_syncfs)
#endif
#ifdef __NR_sendmmsg
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_sendmmsg)
#endif
#ifdef __NR_sched_setattr
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_sched_setattr)
#endif
#ifdef __NR_sched_getattr
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_sched_getattr)
#endif
#ifdef __NR_getrandom
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_getrandom)
#endif
#ifdef __NR_memfd_create
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_memfd_create)
#endif
#ifdef __NR_membarrier
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_membarrier)
#endif
#ifdef __NR_mlock2
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_mlock2)
#endif
#ifdef __NR_copy_file_range
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_copy_file_range)
#endif
#ifdef __NR_preadv2
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_preadv2)
#endif
#ifdef __NR_pwritev2
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_pwritev2)
#endif
#ifdef __NR_rseq
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_rseq)
#endif
#ifdef __NR_clock_gettime64
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_clock_gettime64)
#endif
#ifdef __NR_clock_getres_time64
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_clock_getres_time64)
#endif
#ifdef __NR_clock_nanosleep_time64
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_clock_nanosleep_time64)
#endif
#ifdef __NR_timer_gettime64
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_timer_gettime64)
#endif
#ifdef __NR_timer_settime64
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_timer_settime64)
#endif
#ifdef __NR_pselect6_time64
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_pselect6_time64)
#endif
#ifdef __NR_ppoll_time64
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_ppoll_time64)
#endif
#ifdef __NR_recvmmsg_time64
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_recvmmsg_time64)
#endif
#ifdef __NR_rt_sigtimedwait_time64
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_rt_sigtimedwait_time64)
#endif
#ifdef __NR_sched_rr_get_interval_time64
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_sched_rr_get_interval_time64)
#endif
#ifdef __NR_close_range
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_close_range)
#endif
#ifdef __NR_memfd_secret
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_memfd_secret)
#endif
#ifdef __NR_cachestat
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_cachestat)
#endif
#ifdef __NR_map_shadow_stack
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_map_shadow_stack)
#endif

        // Arch-specific syscalls
#ifdef __NR_riscv_flush_icache
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_riscv_flush_icache)
#endif

        // mmap(), mprotect()
#ifdef __NR_mmap2
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_mmap2) // Used by some 32-bit arches (i386, arm)
#endif
#ifdef __NR_mmap
        BPF_SECCOMP_ALLOW_SYSCALL(__NR_mmap) // Missing on some 32-bit arches (riscv32, arm)
#endif
#ifdef __NR_mprotect
        BPF_SECCOMP_BLOCK_RWX_MMAN(__NR_mprotect)
#endif

        // Return ENOENT for open(), openat()
#ifdef __NR_open
        BPF_SECCOMP_ERRNO_SYSCALL(__NR_open, ENOENT)
#endif
#ifdef __NR_openat
        BPF_SECCOMP_ERRNO_SYSCALL(__NR_openat, ENOENT)
#endif

        // Return ENOSYS for unhandled syscalls
        BPF_SECCOMP_ERRNO(ENOSYS)
    };

    struct sock_fprog prog = {
        .filter = filter,
        .len    = STATIC_ARRAY_SIZE(filter),
    };

    int flags = all_threads ? SECCOMP_FILTER_FLAG_TSYNC : 0;

    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, flags) && errno != ENOSYS) {
        // Seccomp not available on this system
        DO_ONCE(rvvm_info("Failed to enforce seccomp syscall filter: %s!", strerror(errno)));
    }
}

// clang-format on

#endif

static void restrict_process_once(void)
{
#if defined(ISOLATION_DROP_ROOT_IMPL)
    drop_root_user();
#endif
#if defined(ISOLATION_PRCTL_IMPL)
    drop_thread_caps();
#endif
#if defined(ISOLATION_SECCOMP_IMPL)
    seccomp_setup_syscall_filter(true);
#elif defined(ISOLATION_PLEDGE_IMPL)
    if (pledge("stdio tmppath flock prot_exec inet dns unix sendfd recvfd tty audio drm vmm error", "")) {
        rvvm_warn("Failed to enforce pledge: %s!", strerror(errno));
    }
#endif
}

void rvvm_restrict_this_thread(void)
{
#if defined(ISOLATION_DROP_ROOT_IMPL)
    drop_root_user();
#endif
#if defined(ISOLATION_PRCTL_IMPL)
    drop_thread_caps();
#endif
#if defined(ISOLATION_SECCOMP_IMPL)
    seccomp_setup_syscall_filter(false);
#endif
    // No per-thread pledge on OpenBSD :c
}

PUBLIC void rvvm_restrict_process(void)
{
    DO_ONCE(restrict_process_once());
}

POP_OPTIMIZATION_SIZE
