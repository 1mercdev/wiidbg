/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * wiidbg - Wii debugger
 * Copyright (C) 2026 1mercdev
 *
 * wdp_conn: the protocol state machine for ONE client connection.
 *
 * "Sans-I/O" design: this code never reads or writes a socket. The caller
 *   1. feeds it the bytes that arrived          -> wdp_conn_feed()
 *   2. sends whatever it wants to transmit      -> wdp_conn_out()
 * That makes the whole protocol testable with plain function calls (no network,
 * no emulator, no threads), and lets the exact same code run on the Wii and on a
 * PC. Moving bytes is somebody else's job: see wdp_server.c and wnet.h.
 *
 * One request is served at a time. While a response is waiting to be sent,
 * wdp_conn_feed() refuses to consume more input. That is BACKPRESSURE: it keeps
 * memory bounded (one rx and one tx buffer) however fast a client sends. The
 * unread bytes simply wait in the TCP socket buffer.
 */
#ifndef WDP_CONN_H
#define WDP_CONN_H

#include <stddef.h>
#include <stdint.h>
#include "wdp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Non-negative return values of wdp_conn_feed(). */
#define WDP_CONN_OK     0  /* keep going                                     */
#define WDP_CONN_CLOSE  1  /* send the pending output, THEN close the link   */

/* Negative return values are fatal "drop the connection now" errors. They are
 * either the codec's WDP_E_* codes (bad magic, unknown kind, bad flags,
 * oversize length) or this one: */
#define WDP_CONN_E_NOT_REQUEST  (-20)  /* client sent a RESPONSE or EVENT frame */

/* What this stub tells clients about itself in HELLO. */
typedef struct wdp_conn_config {
    uint8_t     stub_major;
    uint8_t     stub_minor;
    uint32_t    capabilities;  /* WDP capability bits; none defined yet */
    const char *build;         /* e.g. "0.1.0"; at most 31 characters   */
} wdp_conn_config;

typedef struct wdp_conn {
    wdp_hello  hello;                 /* prebuilt HELLO reply content        */

    /* receive state: we are either reading a header or reading a payload */
    uint8_t    hdr[WDP_HEADER_SIZE];
    wdp_header cur;                   /* the header of the frame in progress */
    size_t     got;                   /* bytes of header/payload read so far */
    uint8_t    reading_payload;
    uint8_t    rx[WDP_MAX_PAYLOAD];

    /* transmit state: at most ONE response queued */
    uint8_t    tx[WDP_HEADER_SIZE + WDP_MAX_PAYLOAD];
    size_t     tx_len;
    size_t     tx_off;

    uint8_t    closing;               /* last response queued; close after it */
} wdp_conn;

/* Prepare a connection object (works on uninitialised memory). Returns 0, or a
 * negative wdp_err if the config is invalid (e.g. build string too long). */
int wdp_conn_init(wdp_conn *c, const wdp_conn_config *cfg);

/* Consume received bytes. Sets *used to how many of `len` bytes were taken;
 * the caller must re-offer the remainder later (after draining the output).
 * Returns WDP_CONN_OK, WDP_CONN_CLOSE, or a negative error (see above). */
int wdp_conn_feed(wdp_conn *c, const uint8_t *data, size_t len, size_t *used);

/* Bytes waiting to be transmitted. *len may be 0. The pointer stays valid until
 * the next call to feed/out_consume. */
const uint8_t *wdp_conn_out(const wdp_conn *c, size_t *len);

/* Tell the connection that `n` bytes from wdp_conn_out() were really sent. */
void wdp_conn_out_consume(wdp_conn *c, size_t n);

/* True once the connection wants to be closed after its pending output. */
static inline int wdp_conn_is_closing(const wdp_conn *c)
{
    return c->closing != 0;
}

/* Human-readable text for any value returned by wdp_conn_feed(). */
const char *wdp_conn_strerror(int rc);

#ifdef __cplusplus
}
#endif

#endif /* WDP_CONN_H */
