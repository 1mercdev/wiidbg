/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * wiidbg - Wii debugger
 * Copyright (C) 2026 1mercdev
 *
 * wnet: the PLATFORM boundary for networking. Six functions, all non-blocking.
 *
 * Everything above this line (frame codec, protocol state machine, server loop)
 * is portable C. Everything below it is platform code, implemented twice:
 *     stub/source/wnet_wii.c      libogc sockets, runs on the console
 *     tests/host/wnet_posix.c     BSD sockets, runs on your PC (tests)
 * If a libogc call turns out to behave differently than expected, this is the
 * ONLY place that needs fixing.
 *
 * Handles are plain ints. The functions never block: if an operation cannot
 * complete right now they return WNET_AGAIN and the caller tries again later.
 * Blocking would freeze the main loop, which also reads the controller and
 * flushes the log.
 */
#ifndef WNET_H
#define WNET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Results. Non-negative values are "normal outcomes", negative means error. */
#define WNET_OK      0   /* done; output parameters are valid                */
#define WNET_AGAIN   1   /* nothing to do right now; try again later         */
#define WNET_CLOSED  2   /* recv only: the peer closed the connection         */
#define WNET_ERR   (-1)  /* failure; see wnet_last_error() for the raw code  */

/* Bring the network up (Wi-Fi + DHCP on the Wii). Writes the local IP as text
 * into ip[0..cap). Returns WNET_OK or WNET_ERR. May take seconds: call once at
 * startup, not in the main loop. */
int wnet_init(char *ip, size_t cap);

/* Create a non-blocking listening TCP socket on `port`. */
int wnet_listen(uint16_t port, int *listener);

/* Accept one pending connection if there is one. */
int wnet_accept(int listener, int *client);

/* Receive up to `cap` bytes. WNET_OK means *got > 0. */
int wnet_recv(int sock, void *buf, size_t cap, size_t *got);

/* Send up to `len` bytes. WNET_OK means *sent bytes were accepted (it may be
 * fewer than `len`: the caller must send the rest later). */
int wnet_send(int sock, const void *buf, size_t len, size_t *sent);

void wnet_close(int sock);

/* The platform's raw error number from the most recent WNET_ERR, for logs. */
int wnet_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* WNET_H */
