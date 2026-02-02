/*
networking.c - Network sockets (IPv4/IPv6), Event polling
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

// Expose kqueue(), kevent(), syscall(), etc
#include "feature_test.h"

#if defined(HOST_TARGET_WIN32)

// WinSock sockets

#include <winsock2.h> // For WSAStartup(), WSAGetLastError(), SetHandleInformation(), closesocket(), ioctlsocket()...
#include <ws2tcpip.h> // For AF_INET6

// Abstract away socket type differences between Win32/POSIX
typedef SOCKET net_handle_t;
typedef int    net_addrlen_t;
#define NET_HANDLE_INVALID (INVALID_SOCKET)

#else

// POSIX sockets
#include <errno.h>       // For errno, EINPROGRESS, EAGAIN, EWOULDBLOCK, EINTR, ECONNRESET
#include <fcntl.h>       // For fcntl(), F_GETFL, F_SETFL, F_SETFD, O_NONBLOCK, FD_CLOEXEC
#include <netinet/in.h>  // For struct sockaddr_in, struct sockaddr_in6, IPPROTO_TCP, IPPROTO_ICMP
#include <netinet/tcp.h> // For TCP_NODELAY
#include <signal.h>      // For signal(), SIGPIPE, SIG_DFL, SIG_IGN
#include <sys/ioctl.h>   // For ioctl()
#include <sys/select.h>  // For select()
#include <sys/socket.h>  // For socklen_t, struct sockaddr, socket(), AF_INET, SOCK_DGRAM, SOCK_STREAM, SOL_SOCKET...
#include <unistd.h>      // For close(), syscall(__NR_accept4)

#include "compiler.h"

#if CHECK_INCLUDE(sys/resource.h, 1)
#include <sys/resource.h> // For struct rlimit, getrlimit(), setrlimit(), RLIMIT_NOFILE
#endif

#if defined(HOST_TARGET_LINUX) && CHECK_INCLUDE(sys/syscall.h, 1)
#include <sys/syscall.h> // For __NR_accept4, __NR_epoll_create
#endif

#if ((defined(HOST_TARGET_LINUX) && defined(__NR_epoll_create)) || defined(HOST_TARGET_ILLUMOS)) /**/                  \
    && !defined(USE_SELECT) && CHECK_INCLUDE(sys/epoll.h, 1) && GNU_ATTRIBUTE(__packed__)

// Use epoll() for net_poll on Linux & Illumos
#include <sys/epoll.h> // For struct epoll_event, epoll_create(), epoll_ctl(), epoll_wait(), EPOLL_CTL_ADD...

#define EPOLL_NET_IMPL 1

#elif (defined(HOST_TARGET_BSD) || (defined(HOST_TARGET_DARWIN) && HOST_TARGET_DARWIN >= 11)) /**/                     \
    && !defined(USE_SELECT) && CHECK_INCLUDE(sys/event.h, 1)

// Use kqueue() for net_poll on FreeBSD, OpenBSD, NetBSD, DragonFlyBSD and Darwin (MacOS 11+)
#include <sys/event.h> // For struct kevent, kqueue(), kevent(), EV_SET(), EVFILT_READ, EVFILT_WRITE...

// Check EV_DISABLE/EV_ENABLE existence
#if defined(EV_ADD) && defined(EV_ENABLE) && defined(EV_DISABLE) && defined(EV_DELETE)
#define KQUEUE_NET_IMPL 1
#endif

#endif

// Abstract away socket type differences between Win32/POSIX
typedef int       net_handle_t;
typedef socklen_t net_addrlen_t;
#define NET_HANDLE_INVALID (-1)

#endif

// Internal headers come after system headers because of winsock
#include "mem_ops.h"
#include "networking.h"
#include "utils.h"

PUSH_OPTIMIZATION_SIZE

#if !(defined(EPOLL_NET_IMPL) || defined(KQUEUE_NET_IMPL))
// Use select() for net_poll when neither epoll/kqueue are supported
// Scales poorly, but it's a fairly portable fallback. Thread safety & other epoll-like features are well emulated.
#define SELECT_NET_IMPL 1
#endif

#if defined(AF_INET6)
// Compile IPv6 support on systems where it's actually exposed
#define IPV6_NET_IMPL 1
#endif

// Pass to send() to prevent SIGPIPE from being delivered (Linux, *BSD, Apple)
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#if defined(SELECT_NET_IMPL)

#include "hashmap.h"
#include "rvtimer.h"
#include "spinlock.h"
#include "vector.h"

typedef struct {
    net_sock_t* sock;
    net_poll_t* poll;
    void*       data;
    uint32_t    flags;
} net_monitor_t;

#if defined(USE_SELECT_GENERIC)
typedef fd_set vec_fdset_t;
#else
typedef vector_t(size_t) vec_fdset_t;
#define VEC_FDSET_BITS (sizeof(size_t) * 8)
#define VEC_FDSET_MASK (VEC_FDSET_BITS - 1)
#endif

#endif

struct net_sock {
#if defined(SELECT_NET_IMPL)
    vector_t(net_monitor_t*) watchers;
#endif
    net_handle_t fd;
    net_addr_t   addr;
};

struct net_poll {
#if defined(EPOLL_NET_IMPL) || defined(KQUEUE_NET_IMPL)
    net_handle_t fd;
#else
    // Maps socket handle to net_monitor_t*
    hashmap_t   events;
    vec_fdset_t r_set, w_set;
    net_sock_t* wake_sock[2];
    spinlock_t  lock;
    uint32_t    waiters;
#endif
};

static bool networking_available = false;

const net_addr_t net_ipv4_any_addr = {
    .type = NET_TYPE_IPV4,
};
const net_addr_t net_ipv4_local_addr = {
    .type  = NET_TYPE_IPV4,
    .ip[0] = 127,
    .ip[3] = 1,
};
const net_addr_t net_ipv6_any_addr = {
    .type = NET_TYPE_IPV6,
};
const net_addr_t net_ipv6_local_addr = {
    .type   = NET_TYPE_IPV6,
    .ip[15] = 1,
};

static void net_init_once(void)
{
#if defined(HOST_TARGET_WIN32)
    // Try to initialize WinSock 2.2, fallback to 2.1
    WSADATA wsaData = {0};
    if (WSAStartup(0x0202, &wsaData) && WSAStartup(0x0201, &wsaData)) {
        rvvm_error("Failed to initialize WinSock2");
        return;
    }
#elif defined(SIGPIPE)
    // Ignore SIGPIPE (Do not crash on writes to closed socket)
    void* handler = signal(SIGPIPE, SIG_IGN);
    if (handler && handler != (void*)SIG_DFL && handler != (void*)SIG_IGN) {
        // Revert signal handler already set by someone else
        signal(SIGPIPE, handler);
    }
#endif
#if defined(HOST_TARGET_POSIX) && defined(RLIMIT_NOFILE)
    struct rlimit rlim = {0};
    if (!getrlimit(RLIMIT_NOFILE, (void*)&rlim)) {
        if (rlim.rlim_max < 2048) {
            rlim.rlim_max = 8192;
        }
        if (rlim.rlim_cur < rlim.rlim_max) {
            rlim.rlim_cur = rlim.rlim_max;
            if (!setrlimit(RLIMIT_NOFILE, (void*)&rlim)) {
                rvvm_info("Raising RLIMIT_NOFILE to %u", (uint32_t)rlim.rlim_cur);
            }
        }
    }
#endif
    networking_available = true;
}

// Initialize networking automatically
static bool net_init(void)
{
    DO_ONCE(net_init_once());
    return networking_available;
}

/*
 * Internal socket helpers
 */

// Address types conversion (net_addr_t <-> sockaddr_in/sockaddr_in6)
// NOTE: Must operate on a zeroed structure
static void net_sockaddr_from_addr(struct sockaddr_in* sock_addr, const net_addr_t* addr)
{
    sock_addr->sin_family = AF_INET;
    if (addr) {
        write_uint16_be_m(&sock_addr->sin_port, addr->port);
        memcpy(&sock_addr->sin_addr.s_addr, addr->ip, 4);
    }
}

static void net_addr_from_sockaddr(net_addr_t* addr, const struct sockaddr_in* sock_addr)
{
    addr->type = NET_TYPE_IPV4;
    addr->port = read_uint16_be_m(&sock_addr->sin_port);
    memcpy(addr->ip, &sock_addr->sin_addr.s_addr, 4);
}

#if defined(IPV6_NET_IMPL)

static void net_sockaddr6_from_addr(struct sockaddr_in6* sock_addr, const net_addr_t* addr)
{
    sock_addr->sin6_family = AF_INET6;
    write_uint16_be_m(&sock_addr->sin6_port, addr->port);
    memcpy(&sock_addr->sin6_addr.s6_addr, addr->ip, 16);
}

static void net_addr_from_sockaddr6(net_addr_t* addr, const struct sockaddr_in6* sock_addr)
{
    addr->type = NET_TYPE_IPV6;
    addr->port = read_uint16_be_m(&sock_addr->sin6_port);
    memcpy(addr->ip, &sock_addr->sin6_addr.s6_addr, 16);
}

#endif

// Wrappers for generic operations on socket handles
static void net_handle_close(net_handle_t fd)
{
#if defined(HOST_TARGET_WIN32)
    closesocket(fd);
#else
    close(fd);
#endif
}

static bool net_handle_set_blocking(net_handle_t fd, bool block)
{
#if defined(HOST_TARGET_WIN32)
    u_long nb = block ? 0 : 1;
    return !ioctlsocket(fd, FIONBIO, &nb);
#elif defined(FIONBIO)
    // Use a single syscall instead of fcntl implementation
    int nb = block ? 0 : 1;
    return !ioctl(fd, FIONBIO, &nb);
#elif defined(F_SETFL) && defined(O_NONBLOCK)
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    flags = block ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    return !fcntl(fd, F_SETFL, flags);
#else
    UNUSED(fd);
    if (!block) {
        rvvm_warn("Non-blocking sockets are not supported on this OS");
    }
    return block;
#endif
}

static void net_handle_set_cloexec(net_handle_t fd)
{
#if defined(HOST_TARGET_WINNT) && defined(HOST_64BIT) && defined(HANDLE_FLAG_INHERIT)
    SetHandleInformation((HANDLE)fd, HANDLE_FLAG_INHERIT, 0);
#elif defined(F_SETFD) && defined(FD_CLOEXEC)
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
    UNUSED(fd);
}

// Set CLOEXEC flag on created sockets to prevent handle leaking
// Optimize nonblocking connects on modern Linux and *BSD
// Set TCP_NODELAY for low-latency TCP transmission, inherited in accept()
static net_handle_t net_handle_create_ex(int domain, int type, int proto, bool block)
{
    net_handle_t fd = NET_HANDLE_INVALID;
    if (net_init()) {
#if defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK)
        // Create a blocking/non-blocking CLOEXEC socket in a single syscall (Linux, FreeBSD, OpenBSD, NetBSD...)
        fd = socket(domain, type | SOCK_CLOEXEC | (block ? 0 : SOCK_NONBLOCK), proto);
#endif
        if (fd == NET_HANDLE_INVALID) {
            // Fallback to separate syscalls for socket & CLOEXEC
            fd = socket(domain, type, proto);
            if (fd != NET_HANDLE_INVALID) {
                // Mark the socket as non-blocking if needed
                if (!block && !net_handle_set_blocking(fd, false)) {
                    // Failed to set non-blocking mode
                    net_handle_close(fd);
                    return NET_HANDLE_INVALID;
                }
                net_handle_set_cloexec(fd);
            }
        }
        if (fd != NET_HANDLE_INVALID && type == SOCK_STREAM) {
            int enable = 1;
            UNUSED(enable);
#if defined(IPPROTO_TCP) && defined(TCP_NODELAY)
            // Enable TCP_NODELAY on TCP sockets
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void*)&enable, sizeof(enable));
#endif
#if defined(SOL_SOCKET) && defined(SO_OOBINLINE)
            // Enable SO_OOBINLINE on TCP sockets
            setsockopt(fd, SOL_SOCKET, SO_OOBINLINE, (void*)&enable, sizeof(enable));
#endif
        }
    }
    return fd;
}

// Set CLOEXEC flag on accepted sockets, uniformly propagate blocking mode as on BSD stack
static net_handle_t net_handle_accept_ex(net_handle_t listener, void* sock_addr, net_addrlen_t* addr_len)
{
    net_handle_t fd = NET_HANDLE_INVALID;
#if defined(HOST_TARGET_LINUX)
    // NOTE: Linux accept(2) does not inherit nonblocking flag on accepted socket
    bool nonblock = !!(fcntl(listener, F_GETFL, 0) & O_NONBLOCK);
#if defined(__NR_accept4) && defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK)
    // Use accept4() syscall to inherit non-blocking mode & set CLOEXEC at once
    fd = syscall(__NR_accept4, listener, sock_addr, addr_len, SOCK_CLOEXEC | (nonblock ? SOCK_NONBLOCK : 0));
#endif
#endif
    if (fd == NET_HANDLE_INVALID) {
        // Fallback to separate syscalls for accept & CLOEXEC
        fd = accept(listener, sock_addr, addr_len);
        if (fd != NET_HANDLE_INVALID) {
            net_handle_set_cloexec(fd);
#if defined(HOST_TARGET_LINUX)
            // Always inherit non-blocking mode properly
            if (nonblock && !net_handle_set_blocking(fd, false)) {
                // Failed to set non-blocking mode
                net_handle_close(fd);
                return NET_HANDLE_INVALID;
            }
#endif
        }
    }
    return fd;
}

static bool net_handle_bind(net_handle_t fd, const net_addr_t* addr)
{
    if (!addr || addr->type == NET_TYPE_IPV4) {
        struct sockaddr_in sock_addr = {0};
        net_sockaddr_from_addr(&sock_addr, addr);
        return !bind(fd, (void*)&sock_addr, sizeof(sock_addr));
#if defined(IPV6_NET_IMPL)
    } else if (addr->type == NET_TYPE_IPV6) {
#if defined(IPPROTO_IPV6) && defined(IPV6_V6ONLY)
        // Disable dual-stack explicitly (may be configurable in future)
        int v6only = 1;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (void*)&v6only, sizeof(v6only));
#endif
        struct sockaddr_in6 sock_addr = {0};
        net_sockaddr6_from_addr(&sock_addr, addr);
        return !bind(fd, (void*)&sock_addr, sizeof(sock_addr));
#endif
    }
    return false;
}

static inline bool net_conn_initiated(void)
{
#if defined(HOST_TARGET_WIN32)
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EINPROGRESS;
#endif
}

static bool net_handle_connect(net_handle_t fd, const net_addr_t* addr)
{
    if (!addr || addr->type == NET_TYPE_IPV4) {
        struct sockaddr_in sock_addr = {0};
        net_sockaddr_from_addr(&sock_addr, addr);
        return !connect(fd, (struct sockaddr*)&sock_addr, sizeof(sock_addr)) || net_conn_initiated();
#if defined(IPV6_NET_IMPL)
    } else if (addr->type == NET_TYPE_IPV6) {
        struct sockaddr_in6 sock_addr = {0};
        net_sockaddr6_from_addr(&sock_addr, addr);
        return !connect(fd, (struct sockaddr*)&sock_addr, sizeof(sock_addr)) || net_conn_initiated();
#endif
    }
    return false;
}

// For simpler TCP/UDP socket creation
static inline net_handle_t net_handle_create(const net_addr_t* addr, bool tcp, bool block)
{
    if (!addr || addr->type == NET_TYPE_IPV4) {
        return net_handle_create_ex(AF_INET, tcp ? SOCK_STREAM : SOCK_DGRAM, 0, block);
#if defined(IPV6_NET_IMPL)
    } else if (addr->type == NET_TYPE_IPV6) {
        return net_handle_create_ex(AF_INET6, tcp ? SOCK_STREAM : SOCK_DGRAM, 0, block);
#endif
    }
    return NET_HANDLE_INVALID;
}

// Wrap native handles in net_sock_t
static net_sock_t* net_handle_wrap(net_handle_t fd)
{
    if (fd != NET_HANDLE_INVALID) {
        net_sock_t* sock = safe_new_obj(net_sock_t);
        sock->fd         = fd;
        return sock;
    }
    return NULL;
}

// Wrap assigned local address after net_handle_bind()
static net_sock_t* net_handle_wrap_local_addr(net_handle_t fd, const net_addr_t* addr)
{
    net_sock_t* sock = net_handle_wrap(fd);
    if (sock) {
        if (!addr || addr->type == NET_TYPE_IPV4) {
            struct sockaddr_in sock_addr = {0};
            net_addrlen_t      addr_len  = sizeof(struct sockaddr_in);
            // Win32 getsockname may not set sin_family/sin_addr...
            net_sockaddr_from_addr(&sock_addr, addr);
            getsockname(sock->fd, (struct sockaddr*)&sock_addr, &addr_len);
            net_addr_from_sockaddr(&sock->addr, &sock_addr);
#if defined(IPV6_NET_IMPL)
        } else if (addr->type == NET_TYPE_IPV6) {
            struct sockaddr_in6 sock_addr = {0};
            net_addrlen_t       addr_len  = sizeof(struct sockaddr_in6);
            net_sockaddr6_from_addr(&sock_addr, addr);
            getsockname(sock->fd, (struct sockaddr*)&sock_addr, &addr_len);
            net_addr_from_sockaddr6(&sock->addr, &sock_addr);
#endif
        }
    }
    return sock;
}

static net_sock_t* net_handle_wrap_remote_addr(net_handle_t fd, const net_addr_t* addr)
{
    net_sock_t* sock = net_handle_wrap(fd);
    if (sock) {
        sock->addr = *addr;
    }
    return sock;
}

static int32_t net_last_error(void)
{
#if defined(HOST_TARGET_WIN32)
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK || err == WSAEINTR) {
        return NET_ERR_BLOCK;
    }
    if (err == WSAECONNRESET) {
        return NET_ERR_RESET;
    }
    return NET_ERR_UNKNOWN;
#else
    int err = errno;
    if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) {
        return NET_ERR_BLOCK;
    }
    if (err == ECONNRESET) {
        return NET_ERR_RESET;
    }
    return NET_ERR_UNKNOWN;
#endif
}

#if defined(SELECT_NET_IMPL)

/*
 * Internal net_poll select() implementation helpers
 */

// Wake up net_poll_wait() loop to update watched sockets
static void net_poll_select_wake(net_poll_t* poll)
{
    char tmp = 0;
    net_tcp_send(poll->wake_sock[1], &tmp, sizeof(tmp));
}

/*
 * Optimized select() fd_set abstractions
 */

static void vec_fdset_mod(vec_fdset_t* vec, size_t fd, bool add)
{
#if defined(USE_SELECT_GENERIC)
    if (add) {
        FD_SET(fd, vec);
    } else {
        FD_CLR(fd, vec);
    }
#elif defined(HOST_TARGET_WIN32)
    if (add) {
        vector_insert(*vec, EVAL_MAX(vector_size(*vec), 1), fd);
    } else {
        vector_foreach_back (*vec, i) {
            if (vector_at(*vec, i) == fd && i) {
                vector_erase(*vec, i);
                break;
            }
        }
    }
    vector_put(*vec, 0, EVAL_MAX(vector_size(*vec), 1) - 1);
#else
    size_t index = fd / VEC_FDSET_BITS;
    if (index >= vector_size(*vec)) {
        vector_emplace(*vec, index);
    }
    if (add) {
        vector_at(*vec, index) |= (1ULL << (fd & VEC_FDSET_MASK));
    } else {
        vector_at(*vec, index) &= ~(1ULL << (fd & VEC_FDSET_MASK));
    }
#endif
}

#if defined(USE_SELECT_GENERIC)
#define net_fdset_ptr(fdset)  ((void*)&(fdset))
#define net_fdset_free(fdset) UNUSED(fdset)
#define net_fdset_copy(fdset_dst, fdset_src)                                                                           \
    do {                                                                                                               \
        fdset_dst = fdset_src;                                                                                         \
    } while (0)
#if defined(HOST_TARGET_WIN32)
#define net_fdset_foreach(fdset, fd)                                                                                   \
    for (size_t MACRO_IDENT(iter) = 0, fd = 0;                                                                         \
         (MACRO_IDENT(iter) < (fdset).fd_count) ? (fd = (fdset).fd_array[MACRO_IDENT(iter)], 1) : 0;                   \
         ++MACRO_IDENT(iter))
#else
#define net_fdset_foreach(fdset, fd)                                                                                   \
    for (size_t fd = 0; fd < FD_SETSIZE; ++fd)                                                                         \
        if (FD_ISSET((net_handle_t)fd, &(fdset)))
#endif
#else
#define net_fdset_ptr(fdset)                 ((void*)vector_buffer(fdset))
#define net_fdset_free(fdset)                vector_free(fdset)
#define net_fdset_copy(fdset_dst, fdset_src) vector_copy(fdset_dst, fdset_src)
#if defined(HOST_TARGET_WIN32)
#define net_fdset_foreach(fdset, fd)                                                                                   \
    if (vector_size(fdset) > 1 && vector_at(fdset, 0) < vector_size(fdset))                                            \
        for (size_t MACRO_IDENT(iter) = 0, fd = 0;                                                                     \
             (MACRO_IDENT(iter) < vector_at(fdset, 0)) ? (fd = vector_at(fdset, MACRO_IDENT(iter) + 1), 1) : 0;        \
             ++MACRO_IDENT(iter))
#else
#define net_fdset_foreach(fdset, fd)                                                                                   \
    vector_foreach (fdset, MACRO_IDENT(iter))                                                                          \
        if (vector_at(fdset, MACRO_IDENT(iter)))                                                                       \
            for (size_t MACRO_IDENT(bit) = 0, fd = 0;                                                                  \
                 fd = (MACRO_IDENT(iter) * VEC_FDSET_BITS) + MACRO_IDENT(bit), MACRO_IDENT(bit) < VEC_FDSET_BITS;      \
                 ++MACRO_IDENT(bit))                                                                                   \
                if (vector_at(fdset, MACRO_IDENT(iter)) & (1ULL << MACRO_IDENT(bit)))
#endif
#endif

// Prepare fd bitsets, return nfds for select()
static int net_poll_select_prepare_nfds(net_poll_t* poll)
{
#if !defined(USE_SELECT_GENERIC) && !defined(HOST_TARGET_WIN32)
    size_t max_size = EVAL_MAX(vector_size(poll->r_set), vector_size(poll->w_set));
    for (size_t i = max_size; i--;) {
        // Shorten the bit vectors when needed
        if (vector_size(poll->r_set) > i && vector_at(poll->r_set, i)) {
            break;
        } else if (vector_size(poll->w_set) > i && vector_at(poll->w_set, i)) {
            break;
        }
        vector_erase(poll->r_set, i);
        vector_erase(poll->w_set, i);
        max_size = i;
    }
    if (vector_buffer(poll->r_set) && vector_buffer(poll->w_set)) {
        // Match sizes of fd bitsets
        vector_resize(poll->r_set, max_size);
        vector_resize(poll->w_set, max_size);
    }
    return max_size * VEC_FDSET_BITS;
#else
    UNUSED(poll);
    return FD_SETSIZE;
#endif
}

// Set poll flags, returns if wakeup is needed. MUST be called with poll->lock held!
static bool net_poll_select_update(net_poll_t* poll, net_sock_t* sock, uint32_t old_flags, uint32_t new_flags)
{
    if ((old_flags ^ new_flags) & NET_POLL_RECV) {
        vec_fdset_mod(&poll->r_set, (size_t)sock->fd, !!(new_flags & NET_POLL_RECV));
    }
    if ((old_flags ^ new_flags) & NET_POLL_SEND) {
        vec_fdset_mod(&poll->w_set, (size_t)sock->fd, !!(new_flags & NET_POLL_SEND));
    }
    return (new_flags & ~old_flags) && poll->waiters;
}

// Add socket to net_poll watchlist
static bool net_poll_select_add(net_poll_t* poll, net_sock_t* sock, const net_event_t* event)
{
    bool ret  = false;
    bool wake = false;
    scoped_spin_lock (&poll->lock) {
        if (hashmap_get_ptr(&poll->events, (size_t)sock->fd)) {
            // Socket already monitored
            break;
        }

#if defined(USE_SELECT_GENERIC) && !defined(HOST_TARGET_WIN32) || defined(HOST_TARGET_COSMOPOLITAN)
        if (sock->fd >= FD_SETSIZE) {
            // FD value too high; Cosmopolitan doesn't support nfds > FD_SETSIZE
            break;
        }
#endif

        if (event) {
            // Add new socket monitor
            net_monitor_t* monitor = safe_new_obj(net_monitor_t);
            monitor->poll          = poll;
            monitor->sock          = sock;
            monitor->flags         = event->flags | NET_POLL_RECV;
            monitor->data          = event->data;

            hashmap_put_ptr(&poll->events, (size_t)sock->fd, monitor);
            vector_push_back(sock->watchers, monitor);

            // Watch for event flags (NET_POLL_RECV is mandatory)
            wake = net_poll_select_update(poll, sock, 0, monitor->flags);
        } else {
            // Add internal wakeup socket
            net_poll_select_update(poll, sock, 0, NET_POLL_RECV);
        }
        ret = true;
    }
    if (wake) {
        // Wake up waiters in select() to watch for newly added socket
        net_poll_select_wake(poll);
    }
    return ret;
}

// Modify net_poll socket watch flags / data
static bool net_poll_select_mod(net_poll_t* poll, net_sock_t* sock, const net_event_t* event)
{
    bool ret  = false;
    bool wake = false;
    scoped_spin_lock (&poll->lock) {
        net_monitor_t* monitor = hashmap_get_ptr(&poll->events, (size_t)sock->fd);
        if (monitor) {
            uint32_t flags = event->flags | NET_POLL_RECV;
            wake           = net_poll_select_update(poll, sock, monitor->flags, flags);
            monitor->flags = flags;
            monitor->data  = event->data;
        }
    }
    if (wake) {
        // Wake up waiters in select() to watch for modified socket flags
        net_poll_select_wake(poll);
    }
    return ret;
}

// Remove socket from specific net_poll watchlist (Or any net_poll instance if from_poll == NULL)
static bool net_poll_select_remove(net_poll_t* from_poll, net_sock_t* sock)
{
    bool ret = false;
    vector_foreach_back (sock->watchers, i) {
        net_monitor_t* monitor = vector_at(sock->watchers, i);
        if (monitor->poll == from_poll || !from_poll) {
            net_poll_t* poll = monitor->poll;
            scoped_spin_lock (&poll->lock) {
                // Stop watching events on this socket
                net_poll_select_update(poll, sock, monitor->flags, 0);

                // Unlink watcher from net_poll
                hashmap_erase(&poll->events, (size_t)sock->fd);
            }
            // Unlink watcher from socket
            vector_erase(sock->watchers, i);
            safe_free(monitor);
            ret = true;
            if (from_poll) {
                // Break if we only wanted to remove socket from specific net_poll
                break;
            }
        }
    }
    return ret;
}

static size_t net_poll_select_wait(net_poll_t* poll, net_event_t* events, size_t size, uint32_t wait_ms)
{
    size_t      ret   = 0;
    rvtimer_t   timer = {0};
    rvtimecmp_t cmp   = {0};

    if (wait_ms != NET_POLL_INF) {
        // Initizlize timeout timer & comparator
        rvtimer_init(&timer, 1000);
        rvtimecmp_init(&cmp, &timer);
        rvtimecmp_set(&cmp, wait_ms);
    }

    do {
        vec_fdset_t     r_ready = {0};
        vec_fdset_t     w_ready = {0};
        struct timeval  tv      = {0};
        struct timeval* tv_ptr  = NULL;

        if (wait_ms != NET_POLL_INF) {
            tv.tv_sec  = (wait_ms / 1000);
            tv.tv_usec = (wait_ms % 1000) * 1000;
            tv_ptr     = &tv;
        }

        spin_lock(&poll->lock);
        int nfds = net_poll_select_prepare_nfds(poll);
        net_fdset_copy(r_ready, poll->r_set);
        net_fdset_copy(w_ready, poll->w_set);
        poll->waiters++;
        spin_unlock(&poll->lock);

        // Marked this thread as waiting; Unlock data structures during select()
        bool wake = select(nfds, net_fdset_ptr(r_ready), net_fdset_ptr(w_ready), NULL, tv_ptr) > 0;

        spin_lock(&poll->lock);
        poll->waiters--;

        if (wake) {
            // Received some events
            net_fdset_foreach(w_ready, fd)
            {
                net_monitor_t* monitor = hashmap_get_ptr(&poll->events, fd);
                if (monitor && (monitor->flags & NET_POLL_SEND) && ret < size) {
                    events[ret].data  = monitor->data;
                    events[ret].flags = NET_POLL_SEND;
                    ret++;
                }
            }
            size_t merge_end = ret;
            net_fdset_foreach(r_ready, fd)
            {
                net_monitor_t* monitor = hashmap_get_ptr(&poll->events, fd);
                if (monitor && (monitor->flags & NET_POLL_RECV)) {
                    bool merged = false;
                    for (size_t j = 0; j < merge_end; ++j) {
                        if (events[j].data == monitor->data) {
                            // Swap events to speed up future merging
                            if (j != --merge_end) {
                                events[j] = events[merge_end];
                            }
                            events[merge_end].data  = monitor->data;
                            events[merge_end].flags = NET_POLL_RECV | NET_POLL_SEND;
                            merged                  = true;
                            break;
                        }
                    }
                    if (!merged && ret < size) {
                        events[ret].data  = monitor->data;
                        events[ret].flags = NET_POLL_RECV;
                        ret++;
                    }
                } else if (fd == (size_t)poll->wake_sock[0]->fd) {
                    char tmp[256] = {0};
                    net_tcp_recv(poll->wake_sock[0], tmp, sizeof(tmp));
                }
            }
        }
        spin_unlock(&poll->lock);

        net_fdset_free(r_ready);
        net_fdset_free(w_ready);

        if (!ret && wait_ms != NET_POLL_INF) {
            wait_ms = rvtimecmp_delay(&cmp);
        }
    } while (!ret && (wait_ms == NET_POLL_INF || wait_ms));

    return ret;
}

#endif

/*
 * Public networking socket API
 */

size_t net_parse_ipv6(net_addr_t* addr, const char* str)
{
    net_addr_t  result      = {0};
    const char* parse       = str;
    bool        bracket     = parse[0] == '[';
    bool        skip_colon  = false;
    const char* colon_pair  = rvvm_strfind(parse, "::");
    size_t      bytes       = 0;
    size_t      right_start = 0;
    if (bracket) {
        parse++;
    }
    for (; bytes < 16; bytes += 2) {
        if (parse == colon_pair) {
            // Record location of encountered ::, skip it like a group
            parse       += 2;
            right_start  = bytes;
            skip_colon   = false;
            continue;
        } else if (skip_colon && parse[0] == ':') {
            // If we are beyond first hex group, skip prepending :
            parse++;
        } else if (parse[0] == 0 || (bracket && parse[0] == ']')) {
            // End of IPv6
            break;
        }
        // Parse hex group
        size_t   len = 0;
        uint16_t hex = str_to_uint_base(parse, &len, 16);
        if (!len || len > 4) {
            // Hex parsing failed or pair too long
            return 0;
        }
        write_uint16_be_m(result.ip + bytes, hex);
        parse      += len;
        skip_colon  = true;
    }
    if (colon_pair) {
        // Align fields at the right of colon pair to end of IPv6, zero hole
        memmove(result.ip + 16 - (bytes - right_start), result.ip + right_start, bytes - right_start);
        memset(result.ip + right_start, 0, 16 - bytes);
    } else if (bytes != 16) {
        // Not enough hex groups and no colon pair
        return 0;
    }
    if (bracket) {
        if (parse[0] != ']') {
            // Missing closing ]
            return 0;
        }
        parse++;
    }
    if (addr) {
        memcpy(addr, &result, sizeof(net_addr_t));
        addr->type = NET_TYPE_IPV6;
    }
    return parse - str;
}

size_t net_parse_ipv4(net_addr_t* addr, const char* str)
{
    net_addr_t  result = {0};
    const char* parse  = str;
    for (size_t i = 0; i < 4; ++i) {
        size_t len   = 0;
        result.ip[i] = str_to_uint_base(parse, &len, 10);
        if (!len) {
            // Integer parsing failed
            return 0;
        }
        parse += len;
        if (i < 3 && parse[0] == '.') {
            parse++;
        }
    }
    if (addr) {
        memcpy(addr, &result, sizeof(net_addr_t));
        addr->type = NET_TYPE_IPV4;
    }
    return parse - str;
}

size_t net_parse_addr(net_addr_t* addr, const char* str)
{
    const char* parse      = str;
    const char* colon      = rvvm_strfind(parse, ":");
    bool        ipv6       = colon && rvvm_strfind(colon + 1, ":"); // More than a single :
    bool        ipv4       = !!rvvm_strfind(parse, ".");
    bool        parse_port = false;
    size_t      ip_len     = 0;
    if (ipv6) {
        ip_len = net_parse_ipv6(addr, str);
        if (!ip_len) {
            return 0;
        }
    } else if (ipv4) {
        ip_len = net_parse_ipv4(addr, str);
        if (!ip_len) {
            return 0;
        }
    } else if (rvvm_strfind(parse, "localhost") == parse) {
        ip_len = rvvm_strlen("localhost");
        if (addr) {
            *addr = net_ipv4_local_addr;
        }
    } else {
        parse_port = true;
    }
    parse += ip_len;
    if (ip_len && parse[0] == ':') {
        parse_port = true;
        parse++;
    }
    if (parse_port) {
        size_t   len  = 0;
        uint16_t port = str_to_uint_base(parse, &len, 10);
        if (!len) {
            // Integer parsing failed
            return 0;
        }
        parse += len;
        if (addr) {
            addr->port = port;
        }
    }
    return parse - str;
}

net_sock_t* net_tcp_listen(const net_addr_t* addr)
{
    net_handle_t fd = net_handle_create(addr, true, true);
    if (fd != NET_HANDLE_INVALID) {
#if defined(SOL_SOCKET) && defined(SO_REUSEADDR)
        // Prevent bind errors due to TIME_WAIT
        int reuse = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void*)&reuse, sizeof(reuse));
#endif
        // Bind; Try listener backlog of 65535, fallback to 255
        if (net_handle_bind(fd, addr) && (!listen(fd, 65535) || !listen(fd, 255))) {
            return net_handle_wrap_local_addr(fd, addr);
        }
        net_handle_close(fd);
    }
    return NULL;
}

net_sock_t* net_tcp_accept(net_sock_t* listener)
{
    if (listener && listener->addr.type == NET_TYPE_IPV4) {
        struct sockaddr_in sock_addr = {0};
        net_addrlen_t      addr_len  = sizeof(struct sockaddr_in);

        net_sock_t* sock = net_handle_wrap(net_handle_accept_ex(listener->fd, &sock_addr, &addr_len));
        if (sock) {
            net_addr_from_sockaddr(&sock->addr, &sock_addr);
            return sock;
        }
#if defined(IPV6_NET_IMPL)
    } else if (listener && listener->addr.type == NET_TYPE_IPV6) {
        struct sockaddr_in6 sock_addr = {0};
        net_addrlen_t       addr_len  = sizeof(struct sockaddr_in6);

        net_sock_t* sock = net_handle_wrap(net_handle_accept_ex(listener->fd, &sock_addr, &addr_len));
        if (sock) {
            net_addr_from_sockaddr6(&sock->addr, &sock_addr);
            return sock;
        }
#endif
    }
    return NULL;
}

net_sock_t* net_tcp_connect(const net_addr_t* dst, const net_addr_t* src, bool block)
{
    if (dst) {
        // Create a nonblocking socket if needed
        net_handle_t fd = net_handle_create(dst, true, block);
        if (fd != NET_HANDLE_INVALID) {
#if defined(IPPROTO_IP) && defined(IP_BIND_ADDRESS_NO_PORT)
            if (src && !src->port) {
                // Prevent bind errors due to ephemeral port exhaustion
                // Kernel now knows we won't listen() and allows local 2-tuple reuse
                int noport = 1;
                setsockopt(fd, IPPROTO_IP, IP_BIND_ADDRESS_NO_PORT, (void*)&noport, sizeof(noport));
            }
#endif
#if defined(SOL_SOCKET) && defined(SO_REUSEADDR)
            if (src && src->port) {
                // Allow connecting to different destinations from a single local port
                int reuse = 1;
                setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void*)&reuse, sizeof(reuse));
            }
#endif
            // Bind to local address if needed, connect
            if ((!src || net_handle_bind(fd, src)) && net_handle_connect(fd, dst)) {
                return net_handle_wrap_remote_addr(fd, dst);
            }
            net_handle_close(fd);
        }
    }
    return NULL;
}

bool net_tcp_sockpair(net_sock_t* pair[2])
{
    net_sock_t* listener = net_tcp_listen(NET_IPV4_LOCAL);
    // Always return NULL sockets on failure
    pair[0] = NULL;
    pair[1] = NULL;
    if (listener) {
        pair[0] = net_tcp_connect(net_sock_addr(listener), NULL, false);
        if (pair[0]) {
            net_sock_set_blocking(pair[0], true);
            pair[1] = net_tcp_accept(listener);
        }
        net_sock_close(listener);
        if (net_tcp_status(pair[0]) && net_tcp_status(pair[1])) {
            return true;
        }
        net_sock_close(pair[0]);
        net_sock_close(pair[1]);
    }
    return false;
}

bool net_tcp_status(net_sock_t* sock)
{
    if (sock) {
        if (sock->addr.type == NET_TYPE_IPV4) {
            struct sockaddr_in sock_addr = {0};
            net_addrlen_t      addr_len  = sizeof(struct sockaddr_in);
            return !getpeername(sock->fd, (struct sockaddr*)&sock_addr, &addr_len);
#if defined(IPV6_NET_IMPL)
        } else if (sock->addr.type == NET_TYPE_IPV6) {
            struct sockaddr_in6 sock_addr = {0};
            net_addrlen_t       addr_len  = sizeof(struct sockaddr_in6);
            return !getpeername(sock->fd, (struct sockaddr*)&sock_addr, &addr_len);
#endif
        }
    }
    return false;
}

bool net_tcp_shutdown(net_sock_t* sock)
{
    return sock && !shutdown(sock->fd, 1);
}

int32_t net_tcp_send(net_sock_t* sock, const void* buffer, size_t size)
{
    if (likely(sock)) {
        int ret = send(sock->fd, buffer, size, MSG_NOSIGNAL);
        if (ret < 0) {
            return net_last_error();
        }
        return ret;
    }
    return NET_ERR_RESET;
}

int32_t net_tcp_recv(net_sock_t* sock, void* buffer, size_t size)
{
    if (likely(sock)) {
        int ret = recv(sock->fd, buffer, size, 0);
        if (ret == 0) {
            return NET_ERR_DISCONNECT;
        }
        if (ret < 0) {
            return net_last_error();
        }
        return ret;
    }
    return NET_ERR_RESET;
}

net_sock_t* net_udp_bind(const net_addr_t* addr)
{
    net_handle_t fd = net_handle_create(addr, false, true);
    if (fd != NET_HANDLE_INVALID) {
        if (net_handle_bind(fd, addr)) {
            return net_handle_wrap_local_addr(fd, addr);
        }
        net_handle_close(fd);
    }
    return NULL;
}

size_t net_udp_send(net_sock_t* sock, const void* buffer, size_t size, const net_addr_t* addr)
{
    if (likely(sock)) {
        int ret = 0;
        if (sock->addr.type == NET_TYPE_IPV4) {
            struct sockaddr_in sock_addr;
            net_sockaddr_from_addr(&sock_addr, addr);
            ret = sendto(sock->fd, buffer, size, 0, (struct sockaddr*)&sock_addr, sizeof(sock_addr));
#if defined(IPV6_NET_IMPL)
        } else if (sock->addr.type == NET_TYPE_IPV6) {
            struct sockaddr_in6 sock_addr;
            net_sockaddr6_from_addr(&sock_addr, addr);
            ret = sendto(sock->fd, buffer, size, 0, (struct sockaddr*)&sock_addr, sizeof(sock_addr));
#endif
        }
        if (ret > 0) {
            return ret;
        }
    }
    return 0;
}

int32_t net_udp_recv(net_sock_t* sock, void* buffer, size_t size, net_addr_t* addr)
{
    if (likely(sock)) {
        int ret = 0;
        if (sock->addr.type == NET_TYPE_IPV4) {
            struct sockaddr_in sock_addr = {0};
            net_addrlen_t      addr_len  = sizeof(struct sockaddr_in);

            ret = recvfrom(sock->fd, buffer, size, 0, (struct sockaddr*)&sock_addr, &addr_len);
            net_addr_from_sockaddr(addr, &sock_addr);
#if defined(IPV6_NET_IMPL)
        } else if (sock->addr.type == NET_TYPE_IPV6) {
            struct sockaddr_in6 sock_addr = {0};
            net_addrlen_t       addr_len  = sizeof(struct sockaddr_in6);

            ret = recvfrom(sock->fd, buffer, size, 0, (struct sockaddr*)&sock_addr, &addr_len);
            net_addr_from_sockaddr6(addr, &sock_addr);
#endif
        }
        if (ret < 0) {
            return net_last_error();
        }
        return ret;
    }
    return NET_ERR_RESET;
}

/*
 * DGRAM ICMP sockets, seem to work on Linux, MacOS, Windows XP+
 */

net_sock_t* net_icmp_bind(const net_addr_t* addr)
{
#if defined(AF_INET) && defined(SOCK_DGRAM) && defined(IPPROTO_ICMP) && defined(IPPROTO_ICMPV6)
    net_handle_t fd = NET_HANDLE_INVALID;
    if (!addr || addr->type == NET_TYPE_IPV4) {
        fd = net_handle_create_ex(AF_INET, SOCK_DGRAM, IPPROTO_ICMP, true);
#if defined(IPV6_NET_IMPL)
    } else if (addr->type == NET_TYPE_IPV6) {
        fd = net_handle_create_ex(AF_INET6, SOCK_DGRAM, IPPROTO_ICMPV6, true);
#endif
    }
    if (fd != NET_HANDLE_INVALID) {
        net_addr_t icmp_addr = {0};
        uint16_t   icmp_id   = 0;
        if (addr) {
            icmp_addr      = *addr;
            icmp_addr.port = 0;
            icmp_id        = addr->port;
        }
        if (net_handle_bind(fd, &icmp_addr)) {
            net_sock_t* sock = net_handle_wrap_local_addr(fd, &icmp_addr);
            if (sock) {
                sock->addr.port = icmp_id;
                return sock;
            }
        }
        net_handle_close(fd);
    }
#endif
    UNUSED(addr);
    return NULL;
}

size_t net_icmp_send(net_sock_t* sock, const void* buffer, size_t size, const net_addr_t* addr)
{
    return net_udp_send(sock, buffer, size, addr);
}

int32_t net_icmp_recv(net_sock_t* sock, void* buffer, size_t size, net_addr_t* addr)
{
    int32_t ret = net_udp_recv(sock, buffer, size, addr);
#if !defined(HOST_TARGET_LINUX)
    // Windows and MacOS keep IP header in received data packet
    int32_t ip_hdr_size = (sock->addr.type == NET_TYPE_IPV6) ? 40 : 20;
    if (sock->addr.type == NET_TYPE_IPV4 && ret > 0) {
        ip_hdr_size = (read_uint8(buffer) & 0xF) << 2;
    }
    if (ret > ip_hdr_size) {
        ret -= ip_hdr_size;
        memmove(buffer, ((uint8_t*)buffer) + ip_hdr_size, ret);
    }
#endif
    return ret;
}

uint16_t net_icmp_id(net_sock_t* sock)
{
    return sock->addr.port;
}

/*
 * Generic socket operations
 */

net_addr_t* net_sock_addr(net_sock_t* sock)
{
    return sock ? &sock->addr : NULL;
}

uint16_t net_sock_port(net_sock_t* sock)
{
    return sock ? sock->addr.port : 0;
}

bool net_sock_set_blocking(net_sock_t* sock, bool block)
{
    return sock && net_handle_set_blocking(sock->fd, block);
}

void net_sock_close(net_sock_t* sock)
{
    if (likely(sock)) {
#if defined(SELECT_NET_IMPL)
        net_poll_select_remove(NULL, sock);
        vector_free(sock->watchers);
#endif
        net_handle_close(sock->fd);
        safe_free(sock);
    }
}

/*
 * Network event polling
 */

net_poll_t* net_poll_create(void)
{
    if (!net_init()) {
        // Network subsystem initialization failed
        return NULL;
    }
    net_poll_t* poll = safe_new_obj(net_poll_t);
#if defined(EPOLL_NET_IMPL)
    poll->fd = epoll_create(16);
    if (poll->fd < 0) {
        net_poll_close(poll);
        return NULL;
    }
    net_handle_set_cloexec(poll->fd);
#elif defined(KQUEUE_NET_IMPL)
    poll->fd = kqueue();
    if (poll->fd < 0) {
        net_poll_close(poll);
        return NULL;
    }
    net_handle_set_cloexec(poll->fd);
#else
    hashmap_init(&poll->events, 16);
    if (!net_tcp_sockpair(poll->wake_sock) || !net_poll_select_add(poll, poll->wake_sock[0], NULL)) {
        // Failed to register wakeup socket
        net_poll_close(poll);
        return NULL;
    }
#endif
    return poll;
}

bool net_poll_add(net_poll_t* poll, net_sock_t* sock, const net_event_t* event)
{
    if (likely(poll && sock && event)) {
#if defined(EPOLL_NET_IMPL)
        struct epoll_event ev = {
            .events   = EPOLLIN | ((event->flags & NET_POLL_SEND) ? EPOLLOUT : 0),
            .data.ptr = event->data,
        };
        return !epoll_ctl(poll->fd, EPOLL_CTL_ADD, sock->fd, &ev);
#elif defined(KQUEUE_NET_IMPL)
        struct kevent ev[2] = {0};
        EV_SET(&ev[0], sock->fd, EVFILT_READ, EV_ADD, 0, 0, event->data);
        EV_SET(&ev[1], sock->fd, EVFILT_WRITE, //
               EV_ADD | ((event->flags & NET_POLL_SEND) ? 0 : EV_DISABLE), 0, 0, event->data);
        return !kevent(poll->fd, ev, STATIC_ARRAY_SIZE(ev), NULL, 0, NULL);
#else
        return net_poll_select_add(poll, sock, event);
#endif
    }
    return false;
}

bool net_poll_mod(net_poll_t* poll, net_sock_t* sock, const net_event_t* event)
{
    if (likely(poll && sock && event)) {
#if defined(EPOLL_NET_IMPL)
        struct epoll_event ev = {
            .events   = EPOLLIN | ((event->flags & NET_POLL_SEND) ? EPOLLOUT : 0),
            .data.ptr = event->data,
        };
        return !epoll_ctl(poll->fd, EPOLL_CTL_MOD, sock->fd, &ev);
#elif defined(KQUEUE_NET_IMPL)
        struct kevent ev[2] = {0};
        EV_SET(&ev[0], sock->fd, EVFILT_READ, EV_ENABLE, 0, 0, event->data);
        EV_SET(&ev[1], sock->fd, EVFILT_WRITE, //
               (event->flags & NET_POLL_SEND) ? EV_ENABLE : EV_DISABLE, 0, 0, event->data);
        return !kevent(poll->fd, ev, STATIC_ARRAY_SIZE(ev), NULL, 0, NULL);
#else
        return net_poll_select_mod(poll, sock, event);
#endif
    }
    return false;
}

bool net_poll_remove(net_poll_t* poll, net_sock_t* sock)
{
    if (likely(poll && sock)) {
#if defined(EPOLL_NET_IMPL)
        struct epoll_event ev = {0};
        return !epoll_ctl(poll->fd, EPOLL_CTL_DEL, sock->fd, &ev);
#elif defined(KQUEUE_NET_IMPL)
        struct kevent ev[2] = {0};
        EV_SET(&ev[0], sock->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        EV_SET(&ev[1], sock->fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        return !kevent(poll->fd, ev, STATIC_ARRAY_SIZE(ev), NULL, 0, NULL);
#else
        return net_poll_select_remove(poll, sock);
#endif
    }
    return false;
}

size_t net_poll_wait(net_poll_t* poll, net_event_t* events, size_t size, uint32_t wait_ms)
{
    size_t ret = 0;
    if (likely(poll && size)) {
#if defined(EPOLL_NET_IMPL)
        struct epoll_event ev[64];
        // Fetch events into temporary buffer
        int num_ev = epoll_wait(poll->fd, ev, EVAL_MIN(size, STATIC_ARRAY_SIZE(ev)), wait_ms);
        for (int i = 0; i < num_ev && ret < size; ++i) {
            uint32_t flags = 0;
            if (ev[i].events & EPOLLOUT) {
                flags |= NET_POLL_SEND;
            }
            if ((ev[i].events & ~EPOLLOUT) || !flags) {
                flags |= NET_POLL_RECV;
            }
            events[ret].data  = ev[i].data.ptr;
            events[ret].flags = flags;
            ret++;
        }
#elif defined(KQUEUE_NET_IMPL)
        struct kevent    ev[64];
        struct timespec  ts     = {0};
        struct timespec* ts_ptr = NULL;
        if (wait_ms != NET_POLL_INF) {
            ts.tv_sec  = (wait_ms / 1000);
            ts.tv_nsec = (wait_ms % 1000) * 1000000;
            ts_ptr     = &ts;
        }
        int num_ev = kevent(poll->fd, NULL, 0, ev, EVAL_MIN(size, STATIC_ARRAY_SIZE(ev)), ts_ptr);

        // Fill EVFILT_WRITE events, usually a very low amount
        for (int i = 0; i < num_ev; ++i) {
            if (ev[i].filter == EVFILT_WRITE && ret < size) {
                events[ret].data  = (void*)ev[i].udata;
                events[ret].flags = NET_POLL_SEND;
                ret++;
            }
        }

        // Fill EVFILT_READ events, merging with EVFILT_WRITE events as we go
        size_t merge_end = ret;
        for (int i = 0; i < num_ev; ++i) {
            if (ev[i].filter != EVFILT_WRITE) {
                void* data   = (void*)ev[i].udata;
                bool  merged = false;
                for (size_t j = 0; j < merge_end; ++j) {
                    if (events[j].data == data) {
                        // Swap events to speed up future merging
                        if (j != --merge_end) {
                            events[j] = events[merge_end];
                        }
                        events[merge_end].data  = data;
                        events[merge_end].flags = NET_POLL_RECV | NET_POLL_SEND;
                        merged                  = true;
                        break;
                    }
                }
                if (!merged && ret < size) {
                    events[ret].data  = data;
                    events[ret].flags = NET_POLL_RECV;
                    ret++;
                }
            }
        }
#else
        ret = net_poll_select_wait(poll, events, size, wait_ms);
#endif
    }
    return ret;
}

void net_poll_close(net_poll_t* poll)
{
    if (likely(poll)) {
#if defined(EPOLL_NET_IMPL) || defined(KQUEUE_NET_IMPL)
        net_handle_close(poll->fd);
#else
        // Unlink from related sockets
        hashmap_foreach (&poll->events, k, v) {
            net_monitor_t* monitor = (net_monitor_t*)v;
            net_sock_t*    sock    = monitor->sock;
            vector_foreach_back (sock->watchers, j) {
                if (vector_at(sock->watchers, j) == monitor) {
                    vector_erase(sock->watchers, j);
                    break;
                }
            }
            safe_free(monitor);
        }
        hashmap_free(&poll->events);
        net_sock_close(poll->wake_sock[0]);
        net_sock_close(poll->wake_sock[1]);
#endif
        safe_free(poll);
    }
}

POP_OPTIMIZATION_SIZE
