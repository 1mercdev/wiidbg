/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * wiidbg - Wii debugger
 * Copyright (C) 2026 1mercdev
 *
 * wdp_conn implementation. Read wdp_conn.h first.
 */
#include "wdp_conn.h"

#include <string.h>

int wdp_conn_init(wdp_conn *c, const wdp_conn_config *cfg)
{
    if (!c || !cfg || !cfg->build)
        return WDP_E_ARG;

    size_t n = strlen(cfg->build);
    if (n >= WDP_BUILD_MAX)
        return WDP_E_TOO_BIG;

    memset(c, 0, sizeof *c);
    c->hello.stub_major   = cfg->stub_major;
    c->hello.stub_minor   = cfg->stub_minor;
    c->hello.capabilities = cfg->capabilities;
    c->hello.max_payload  = WDP_MAX_PAYLOAD;
    c->mem = cfg->mem;
    /* Advertise only what we can really do: the bits follow from the backend,
     * so the HELLO reply can never claim a feature that is not there. */
    if (c->mem && c->mem->read)
        c->hello.capabilities |= WDP_CAP_MEM_READ;
    if (c->mem && c->mem->write)
        c->hello.capabilities |= WDP_CAP_MEM_WRITE;
    memcpy(c->hello.build, cfg->build, n);   /* rest already zeroed: terminated */
    return 0;
}

/* ---- response building ------------------------------------------------- */

/* Finish a response whose payload (payload_len bytes) was already written at
 * c->tx + WDP_HEADER_SIZE. Writes the header in front of it and queues it. */
static void queue_response(wdp_conn *c, uint16_t seq, uint16_t cmd,
                           uint16_t status, size_t payload_len)
{
    wdp_header h;
    wdp_header_init(&h, WDP_KIND_RESPONSE, seq, cmd, status, (uint32_t)payload_len);
    wdp_header_encode(&h, c->tx);        /* cannot fail: arguments are valid */
    c->tx_off = 0;
    c->tx_len = WDP_HEADER_SIZE + payload_len;
}

static void queue_error(wdp_conn *c, uint16_t seq, uint16_t cmd, uint16_t status)
{
    queue_response(c, seq, cmd, status, 0);
}

/* ---- request handling -------------------------------------------------- */

/* MEM_READ request:  u32 addr, u32 len.   Reply: `len` bytes of memory.
 *
 * ORDER MATTERS in this function: every check happens BEFORE the backend is
 * touched, so a bad request can never reach memory. */
static void handle_mem_read(wdp_conn *c)
{
    const wdp_header *h = &c->cur;

    if (!c->mem || !c->mem->read) {
        queue_error(c, h->seq, h->cmd, WDP_ERR_UNSUPPORTED);
        return;
    }
    if (h->len != 8) {
        queue_error(c, h->seq, h->cmd, WDP_ERR_BAD_PAYLOAD);
        return;
    }
    uint32_t addr = wdp_get_u32(c->rx);
    uint32_t n    = wdp_get_u32(c->rx + 4);

    if (n == 0) {
        queue_error(c, h->seq, h->cmd, WDP_ERR_BAD_PAYLOAD);
        return;
    }
    if (n > WDP_MAX_PAYLOAD) {          /* the reply would not fit one frame */
        queue_error(c, h->seq, h->cmd, WDP_ERR_TOO_BIG);
        return;
    }
    if (!wmem_range_ok(addr, n)) {
        queue_error(c, h->seq, h->cmd, WDP_ERR_RANGE);
        return;
    }
    /* Read straight into the reply buffer: no intermediate copy. */
    if (c->mem->read(addr, c->tx + WDP_HEADER_SIZE, n) < 0) {
        queue_error(c, h->seq, h->cmd, WDP_ERR_INTERNAL);
        return;
    }
    queue_response(c, h->seq, h->cmd, WDP_OK, n);
}

/* MEM_WRITE request: u32 addr, then the bytes to write.  Reply: empty.
 * All-or-nothing: nothing is written unless every check passes. */
static void handle_mem_write(wdp_conn *c)
{
    const wdp_header *h = &c->cur;

    if (!c->mem || !c->mem->write) {
        queue_error(c, h->seq, h->cmd, WDP_ERR_UNSUPPORTED);
        return;
    }
    if (h->len < 5) {                   /* 4 address bytes + at least 1 data byte */
        queue_error(c, h->seq, h->cmd, WDP_ERR_BAD_PAYLOAD);
        return;
    }
    uint32_t addr = wdp_get_u32(c->rx);
    uint32_t n    = h->len - 4;         /* cannot exceed MAX_PAYLOAD - 4 */

    if (!wmem_range_ok(addr, n)) {
        queue_error(c, h->seq, h->cmd, WDP_ERR_RANGE);
        return;
    }
    if (c->mem->write(addr, c->rx + 4, n) < 0) {
        queue_error(c, h->seq, h->cmd, WDP_ERR_INTERNAL);
        return;
    }
    queue_response(c, h->seq, h->cmd, WDP_OK, 0);
}

/* Called when a complete request (c->cur header + c->rx payload) is available. */
static void handle_request(wdp_conn *c)
{
    const wdp_header *h = &c->cur;
    uint8_t *reply = c->tx + WDP_HEADER_SIZE;

    switch (h->cmd) {
    case WDP_CMD_HELLO: {
        if (h->len != 0) {               /* HELLO takes no arguments */
            queue_error(c, h->seq, h->cmd, WDP_ERR_BAD_PAYLOAD);
            return;
        }
        int n = wdp_hello_encode(&c->hello, reply, WDP_MAX_PAYLOAD);
        if (n < 0)
            queue_error(c, h->seq, h->cmd, WDP_ERR_INTERNAL);
        else
            queue_response(c, h->seq, h->cmd, WDP_OK, (size_t)n);
        return;
    }
    case WDP_CMD_PING:
        /* Echo the payload. len was bounded by the header decoder, so this
         * always fits in the reply buffer. */
        memcpy(reply, c->rx, h->len);
        queue_response(c, h->seq, h->cmd, WDP_OK, h->len);
        return;
    case WDP_CMD_MEM_READ:
        handle_mem_read(c);
        return;
    case WDP_CMD_MEM_WRITE:
        handle_mem_write(c);
        return;
    default:
        /* Unknown command: say so and keep the connection. This is what lets
         * a newer client talk to an older stub (docs/protocol.md, section 3). */
        queue_error(c, h->seq, h->cmd, WDP_ERR_UNKNOWN_CMD);
        return;
    }
}

/* ---- input ------------------------------------------------------------- */

int wdp_conn_feed(wdp_conn *c, const uint8_t *data, size_t len, size_t *used)
{
    size_t u = 0;
    int rc = WDP_CONN_OK;

    while (u < len) {
        /* Backpressure: do not read more while a response is still queued. */
        if (c->tx_len > 0)
            break;
        if (c->closing) {
            rc = WDP_CONN_CLOSE;
            break;
        }

        if (!c->reading_payload) {
            /* ---- accumulate the 16 header bytes ---- */
            size_t want = WDP_HEADER_SIZE - c->got;
            size_t take = len - u < want ? len - u : want;
            memcpy(c->hdr + c->got, data + u, take);
            c->got += take;
            u += take;
            if (c->got < WDP_HEADER_SIZE)
                continue;                    /* need more bytes */
            c->got = 0;

            /* Check the two fields at fixed offsets FIRST, straight from the
             * raw bytes. This is the promise made in docs/protocol.md: magic
             * and version never move, so we can diagnose "wrong protocol" and
             * "wrong version" even for a frame whose other fields we could not
             * parse. */
            if (wdp_get_u16(c->hdr) != WDP_MAGIC) {
                rc = WDP_E_MAGIC;
                break;
            }
            if (c->hdr[2] != WDP_VERSION) {
                /* Tell the client which version we speak, then hang up: with
                 * an unknown version we cannot trust `len`, so we could not
                 * find the next frame boundary anyway. */
                c->tx[WDP_HEADER_SIZE] = (uint8_t)WDP_VERSION;
                queue_response(c, wdp_get_u16(c->hdr + 4), wdp_get_u16(c->hdr + 6),
                               WDP_ERR_BAD_VERSION, 1);
                c->closing = 1;
                rc = WDP_CONN_CLOSE;
                break;
            }

            /* Full validation: kind, flags, and the length cap. Rejecting an
             * oversize length HERE, before any payload byte is read, is the
             * point of the cap. */
            rc = wdp_header_decode(c->hdr, &c->cur);
            if (rc < 0)
                break;
            rc = WDP_CONN_OK;
            if (c->cur.kind != WDP_KIND_REQUEST) {
                rc = WDP_CONN_E_NOT_REQUEST;
                break;
            }

            if (c->cur.len == 0)
                handle_request(c);           /* no payload to wait for */
            else
                c->reading_payload = 1;
        } else {
            /* ---- accumulate the payload ---- */
            size_t want = c->cur.len - c->got;
            size_t take = len - u < want ? len - u : want;
            memcpy(c->rx + c->got, data + u, take);
            c->got += take;
            u += take;
            if (c->got < c->cur.len)
                continue;
            c->got = 0;
            c->reading_payload = 0;
            handle_request(c);
        }
    }

    *used = u;
    if (rc == WDP_CONN_OK && c->closing)
        rc = WDP_CONN_CLOSE;
    return rc;
}

/* ---- output ------------------------------------------------------------ */

const uint8_t *wdp_conn_out(const wdp_conn *c, size_t *len)
{
    *len = c->tx_len - c->tx_off;
    return c->tx + c->tx_off;
}

void wdp_conn_out_consume(wdp_conn *c, size_t n)
{
    size_t left = c->tx_len - c->tx_off;
    if (n > left)
        n = left;
    c->tx_off += n;
    if (c->tx_off == c->tx_len)
        c->tx_off = c->tx_len = 0;     /* drained: ready to accept more input */
}

const char *wdp_conn_strerror(int rc)
{
    switch (rc) {
    case WDP_CONN_OK:           return "ok";
    case WDP_CONN_CLOSE:        return "closing after reply";
    case WDP_CONN_E_NOT_REQUEST:return "client sent a non-request frame";
    default:                    return wdp_err_str(rc);
    }
}
