/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * wiidbg - Wii debugger
 * Copyright (C) 2026 1mercdev
 *
 * wnet for the Wii, on top of libogc's network API (<network.h>).
 *
 * BACKGROUND: the Wii's TCP/IP stack does not run on the main CPU. It runs on
 * IOS, on the separate ARM coprocessor ("Starlet"). Every net_*() call below
 * is really a message sent to IOS, which does the work and sends back a
 * result. Negative results are errors, already converted to -errno values by
 * libogc.
 *
 * SOURCES: this file was checked against libogc's own gc/network.h and
 * libogc/network_wii.c (devkitPro/libogc, master). Facts it relies on:
 *   - IOS_O_NONBLOCK (0x04) is private to network_wii.c, so we define it here.
 *   - F_GETFL / F_SETFL come from <fcntl.h>, which network.h does not include.
 *     net_fcntl() just forwards them to IOS; any other command is -EINVAL.
 *   - net_socket() sets non-blocking mode the same way we do below.
 *   - net_bind()/net_accept() convert to the Wii's 8-byte sockaddr themselves
 *     and ignore the caller's address length, so we pass the normal struct.
 *   - "Nothing available yet" is reported as -EAGAIN (IOS code -6).
 *   - Errors libogc does not know come back as -10000 + (IOS code).
 * Still ASSUMED, not read: that a non-blocking net_accept() with no pending
 * connection also reports -EAGAIN. If it does not, the log will say
 * "accept failed (error N)" once; add N to is_again().
 */
#include <errno.h>
#include <fcntl.h>      /* F_GETFL, F_SETFL */
#include <network.h>
#include <stdbool.h>    /* true */
#include <stdio.h>
#include <string.h>

#include "wnet.h"

/* IOS's value for "this socket is non-blocking". libogc keeps it private, so
 * the public headers do not provide it. */
#ifndef IOS_O_NONBLOCK
#define IOS_O_NONBLOCK 0x04
#endif

static int g_err;

/* "Nothing available right now" on a non-blocking socket. */
static int is_again(s32 r)
{
    return r == -EAGAIN;
}

/* Same read-modify-write libogc's own net_socket() uses for SOCK_NONBLOCK. */
static int set_nonblocking(s32 s)
{
    s32 flags = net_fcntl(s, F_GETFL, 0);
    if (flags < 0) { g_err = (int)flags; return -1; }

    s32 r = net_fcntl(s, F_SETFL, (u32)flags | IOS_O_NONBLOCK);
    if (r < 0) { g_err = (int)r; return -1; }
    return 0;
}

int wnet_init(char *ip, size_t cap)
{
    char local[16] = {0}, mask[16] = {0}, gateway[16] = {0};

    /* if_config() runs net_init() (Wi-Fi/Ethernet as configured in the Wii's
     * own system settings), then DHCP, and waits (here: up to 20 attempts).
     * It blocks, which is fine at startup. */
    s32 r = if_config(local, mask, gateway, true, 20);
    if (r < 0) { g_err = (int)r; return WNET_ERR; }

    snprintf(ip, cap, "%s", local);
    return WNET_OK;
}

int wnet_listen(uint16_t port, int *listener)
{
    s32 s = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s < 0) { g_err = (int)s; return WNET_ERR; }

    /* Lets us re-bind quickly when the stub is relaunched during development.
     * Not essential, so a failure here is ignored. */
    int one = 1;
    net_setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = INADDR_ANY;

    s32 r = net_bind(s, (struct sockaddr *)&sa, sizeof sa);
    if (r < 0) { g_err = (int)r; net_close(s); return WNET_ERR; }

    r = net_listen(s, 2);
    if (r < 0) { g_err = (int)r; net_close(s); return WNET_ERR; }

    if (set_nonblocking(s) < 0) { net_close(s); return WNET_ERR; }

    *listener = (int)s;
    return WNET_OK;
}

int wnet_accept(int listener, int *client)
{
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof peer;    /* libogc requires at least 8 bytes */
    memset(&peer, 0, sizeof peer);

    s32 c = net_accept(listener, (struct sockaddr *)&peer, &peer_len);
    if (c < 0) {
        if (is_again(c)) return WNET_AGAIN;
        g_err = (int)c;
        return WNET_ERR;
    }
    /* An accepted socket is a new socket: set it up again. */
    if (set_nonblocking(c) < 0) { net_close(c); return WNET_ERR; }
    *client = (int)c;
    return WNET_OK;
}

int wnet_recv(int sock, void *buf, size_t cap, size_t *got)
{
    s32 n = net_recv(sock, buf, (s32)cap, 0);
    if (n > 0) { *got = (size_t)n; return WNET_OK; }
    if (n == 0) return WNET_CLOSED;          /* peer closed the connection */
    if (is_again(n)) return WNET_AGAIN;
    g_err = (int)n;
    return WNET_ERR;
}

int wnet_send(int sock, const void *buf, size_t len, size_t *sent)
{
    s32 n = net_send(sock, buf, (s32)len, 0);
    if (n >= 0) { *sent = (size_t)n; return WNET_OK; }
    if (is_again(n)) return WNET_AGAIN;
    g_err = (int)n;
    return WNET_ERR;
}

void wnet_close(int sock)
{
    net_close(sock);
}

int wnet_last_error(void)
{
    return g_err;
}
