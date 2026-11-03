#ifndef PSX_DEBUG_SOCKET_IO_H
#define PSX_DEBUG_SOCKET_IO_H

/* The worker owns its sockets until it exits. Readiness waits are cancellable;
 * they do not impose a deadline on a client sending a fragmented request. */
#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET PsxDebugSocket;
#define PSX_DEBUG_SOCKET_INVALID INVALID_SOCKET
#define psx_debug_socket_close closesocket
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
typedef int PsxDebugSocket;
#define PSX_DEBUG_SOCKET_INVALID (-1)
#define psx_debug_socket_close close
#endif
#include <string.h>

typedef int (*PsxDebugSocketRunning)(void *ctx);

static inline int psx_debug_socket_nonblocking(PsxDebugSocket s, int on) {
#ifdef _WIN32
    u_long mode = on ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL,
        on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
#endif
}

static inline int psx_debug_socket_retryable(void) {
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

static inline int psx_debug_socket_readable(PsxDebugSocket s) {
    fd_set readfds;
    struct timeval wait = {0, 200000};
#ifndef _WIN32
    if (s < 0 || s >= FD_SETSIZE) return -1;
#endif
    FD_ZERO(&readfds);
    FD_SET(s, &readfds);
#ifdef _WIN32
    int ready = select(0, &readfds, NULL, NULL, &wait);
#else
    int ready = select(s + 1, &readfds, NULL, NULL, &wait);
#endif
    return ready < 0 && psx_debug_socket_retryable() ? 0 : ready;
}

static inline PsxDebugSocket psx_debug_socket_accept(
        PsxDebugSocket listener, PsxDebugSocketRunning running, void *ctx) {
    while (running(ctx)) {
        int ready = psx_debug_socket_readable(listener);
        if (ready < 0) return PSX_DEBUG_SOCKET_INVALID;
        if (!ready || !running(ctx)) continue;
        /* The listener is nonblocking: an aborted connection between select
         * and accept must not strand the worker in a blocking accept. */
        PsxDebugSocket client = accept(listener, NULL, NULL);
        if (client == PSX_DEBUG_SOCKET_INVALID) {
            if (psx_debug_socket_retryable()) continue;
            return PSX_DEBUG_SOCKET_INVALID;
        }
        /* Winsock inherits FIONBIO. Request reads retain the existing blocking
         * connection semantics, guarded by the cancellable readiness wait. */
        if (!psx_debug_socket_nonblocking(client, 0)) {
            psx_debug_socket_close(client);
            return PSX_DEBUG_SOCKET_INVALID;
        }
        return client;
    }
    return PSX_DEBUG_SOCKET_INVALID;
}

static inline int psx_debug_socket_recv_line(PsxDebugSocket client, char *buf,
        int cap, PsxDebugSocketRunning running, void *ctx) {
    int len = 0;
    while (len < cap - 1 && running(ctx)) {
        int ready = psx_debug_socket_readable(client);
        if (ready < 0) return -1;
        if (!ready || !running(ctx)) continue;
        int n = recv(client, buf + len, cap - 1 - len, 0);
        if (n < 0 && psx_debug_socket_retryable()) continue;
        if (n <= 0) return -1;
        len += n;
        buf[len] = '\0';
        char *nl = strchr(buf, '\n');
        if (nl) { *nl = '\0'; return (int)(nl - buf); }
    }
    if (!running(ctx)) return -1;
    buf[cap - 1] = '\0';
    return cap - 1;
}
#endif
