/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * wiidbg - Wii debugger
 * Copyright (C) 2026 1mercdev
 *
 * WDP frame codec. See wdp.h and docs/protocol.md.
 *
 * Why not just `write(sock, &header, sizeof header)`?
 *   1. Struct layout (padding) is compiler-defined; the wire format must not be.
 *   2. Even on the big-endian Wii, a struct copy would silently break the day
 *      someone builds the host tools on a little-endian PC. Explicit
 *      serialisation makes the wire format a *specification*, not an accident.
 */
#include "wdp.h"

void wdp_header_init(wdp_header *h, uint8_t kind, uint16_t seq,
                     uint16_t cmd, uint16_t status, uint32_t len)
{
    h->magic   = WDP_MAGIC;
    h->version = WDP_VERSION;
    h->kind    = kind;
    h->seq     = seq;
    h->cmd     = cmd;
    h->status  = status;
    h->flags   = 0;
    h->len     = len;
}

int wdp_header_encode(const wdp_header *h, uint8_t out[WDP_HEADER_SIZE])
{
    if (!h || !out)
        return WDP_E_ARG;

    /* Layout (offsets in bytes, all multi-byte fields big-endian):
     *  0 magic(2)  2 version(1)  3 kind(1)  4 seq(2)  6 cmd(2)
     *  8 status(2) 10 flags(2)   12 len(4)
     * Offsets 0..2 (magic + version) must NEVER move in any future version:
     * that is what lets any receiver detect a version mismatch. */
    wdp_put_u16(out + 0,  h->magic);
    out[2] = h->version;
    out[3] = h->kind;
    wdp_put_u16(out + 4,  h->seq);
    wdp_put_u16(out + 6,  h->cmd);
    wdp_put_u16(out + 8,  h->status);
    wdp_put_u16(out + 10, h->flags);
    wdp_put_u32(out + 12, h->len);
    return WDP_E_OK;
}

int wdp_header_decode(const uint8_t in[WDP_HEADER_SIZE], wdp_header *h)
{
    if (!in || !h)
        return WDP_E_ARG;

    /* Parse into a local first so *h is never left half-written on error. */
    wdp_header t;
    t.magic   = wdp_get_u16(in + 0);
    t.version = in[2];
    t.kind    = in[3];
    t.seq     = wdp_get_u16(in + 4);
    t.cmd     = wdp_get_u16(in + 6);
    t.status  = wdp_get_u16(in + 8);
    t.flags   = wdp_get_u16(in + 10);
    t.len     = wdp_get_u32(in + 12);

    /* Validate in the order a receiver would want to diagnose problems:
     * "is this even our protocol?" before "is this frame sane?". */
    if (t.magic != WDP_MAGIC)
        return WDP_E_MAGIC;
    if (t.kind >= WDP_KIND__COUNT)
        return WDP_E_KIND;
    /* Unknown flags might change how the payload is interpreted (e.g. a
     * future "payload is compressed" bit). Guessing would misparse, so we
     * refuse. Version 1 defines no flags. */
    if (t.flags != 0)
        return WDP_E_FLAGS;
    /* Check the length BEFORE anyone allocates or reads that many bytes. On a
     * console with 24 MiB of fast RAM, trusting a 32-bit length from the
     * network is how you get a remote crash. */
    if (t.len > WDP_MAX_PAYLOAD)
        return WDP_E_TOO_BIG;

    *h = t;
    return WDP_E_OK;
}

const char *wdp_err_str(int err)
{
    switch (err) {
    case WDP_E_OK:      return "ok";
    case WDP_E_ARG:     return "bad argument";
    case WDP_E_MAGIC:   return "bad magic (not a WDP stream)";
    case WDP_E_KIND:    return "unknown frame kind";
    case WDP_E_FLAGS:   return "unsupported flags set";
    case WDP_E_TOO_BIG: return "payload length exceeds maximum";
    default:            return "unknown error";
    }
}
