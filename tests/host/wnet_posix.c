/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * wnet implementation for the PC (BSD sockets). Used by tests/host/hostserver.c
 * so the REAL server loop can be exercised without a Wii or an emulator.
 * Listens on loopback only: this is a test tool, not something to expose.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../../common/wnet.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static int g_err;

static int set_nonblocking(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int would_block(int e) { return e == EAGAIN || e == EWOULDBLOCK || e == EINTR; }

int wnet_init(char *ip, size_t cap)
{
    snprintf(ip, cap, "127.0.0.1");
    return WNET_OK;
}

int wnet_listen(uint16_t port, int *listener)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { g_err = errno; return WNET_ERR; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, 4) < 0 ||
        set_nonblocking(fd) < 0) {
        g_err = errno;
        close(fd);
        return WNET_ERR;
    }
    *listener = fd;
    return WNET_OK;
}

int wnet_accept(int listener, int *client)
{
    int fd = accept(listener, NULL, NULL);
    if (fd < 0) {
        if (would_block(errno)) return WNET_AGAIN;
        g_err = errno;
        return WNET_ERR;
    }
    if (set_nonblocking(fd) < 0) { g_err = errno; close(fd); return WNET_ERR; }
    *client = fd;
    return WNET_OK;
}

int wnet_recv(int sock, void *buf, size_t cap, size_t *got)
{
    ssize_t n = recv(sock, buf, cap, 0);
    if (n > 0) { *got = (size_t)n; return WNET_OK; }
    if (n == 0) return WNET_CLOSED;
    if (would_block(errno)) return WNET_AGAIN;
    g_err = errno;
    return WNET_ERR;
}

int wnet_send(int sock, const void *buf, size_t len, size_t *sent)
{
    ssize_t n = send(sock, buf, len, MSG_NOSIGNAL);
    if (n >= 0) { *sent = (size_t)n; return WNET_OK; }
    if (would_block(errno)) return WNET_AGAIN;
    g_err = errno;
    return WNET_ERR;
}

void wnet_close(int sock) { close(sock); }

int wnet_last_error(void) { return g_err; }
