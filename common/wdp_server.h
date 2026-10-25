/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * wiidbg - Wii debugger
 * Copyright (C) 2026 1mercdev
 *
 * wdp_server: the loop that connects sockets (wnet.h) to the protocol state
 * machine (wdp_conn.h). It owns the listening socket, at most one client, and
 * moves bytes between them.
 *
 * Usage: wdp_server_start() once, then wdp_server_poll() as often as you like
 * (every main-loop iteration). Poll never blocks.
 *
 * Policy decisions, deliberately simple for now:
 *   - ONE client at a time. A debugger session is exclusive.
 *   - NEWEST WINS: a new connection replaces the current one. That way a stale
 *     session (Wi-Fi dropped without a clean close) can never lock you out.
 *     (No authentication yet: anyone who can connect can take over the session.)
 */
#ifndef WDP_SERVER_H
#define WDP_SERVER_H

#include <stddef.h>
#include <stdint.h>
#include "wdp_conn.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WDP_SERVER_RX_CHUNK   2048u  /* bytes read from the socket at a time    */
#define WDP_SERVER_MAX_STEPS  16u    /* max work per poll, so the main loop     */
                                     /* keeps running even under heavy traffic  */

typedef struct wdp_server {
    wdp_conn_config cfg;          /* cfg.build points at `build` below */
    char     build[WDP_BUILD_MAX];/* the server owns its copy of the build string */
    int      active;              /* 1 between a successful start() and stop(). A
                                   * zero-filled struct is therefore harmlessly inert. */
    int      listener;            /* valid only while active */
    int      client;              /* -1 when nobody is connected */
    wdp_conn conn;
    uint8_t  rx[WDP_SERVER_RX_CHUNK];
    size_t   rx_off;              /* next unconsumed byte in rx   */
    size_t   rx_len;              /* valid bytes in rx            */
    unsigned sessions;            /* clients served since start   */
    int      last_accept_err;     /* last accept() error already logged (0 = none) */
} wdp_server;

/* Start listening. Returns 0 or a negative error. */
int wdp_server_start(wdp_server *s, uint16_t port, const wdp_conn_config *cfg);

/* Do whatever work is possible without blocking. */
void wdp_server_poll(wdp_server *s);

/* Close the client (if any) and the listener. */
void wdp_server_stop(wdp_server *s);

#ifdef __cplusplus
}
#endif

#endif /* WDP_SERVER_H */
