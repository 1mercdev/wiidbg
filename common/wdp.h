/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * wiidbg - Wii debugger
 * Copyright (C) 2026 1mercdev
 *
 * WDP: the Wii Debug Protocol, frame layer.
 *
 * This file is deliberately PORTABLE C: it includes only <stdint.h> and
 * <stddef.h>, never <gccore.h>. That lets the exact same code be compiled
 *   (a) for the Wii by devkitPPC, and
 *   (b) on your PC by plain gcc, where tests/host/ exercises it.
 * The Python client (client/wiidbg/frame.py) re-implements the same wire
 * format; tests/vectors.txt is the single source of truth both must obey.
 *
 * The full specification lives in docs/protocol.md. Keep them in sync.
 */
#ifndef WDP_H
#define WDP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Constants -------------------------------------------------------- */

#define WDP_MAGIC        0x5744u  /* ASCII "WD" */
#define WDP_VERSION      1u       /* wire-format version, see docs/protocol.md */
#define WDP_HEADER_SIZE  16u
#define WDP_MAX_PAYLOAD  0x4000u  /* 16 KiB: bounds memory use on the Wii */
#define WDP_DEFAULT_PORT 4242u    /* TCP port the stub listens on          */
#define WDP_BUILD_MAX    32u      /* HELLO build string, incl. terminator   */

/* Who is talking, and why. */
enum wdp_kind {
    WDP_KIND_REQUEST  = 0, /* PC -> Wii: "please do X"                    */
    WDP_KIND_RESPONSE = 1, /* Wii -> PC: answer to the request with same seq */
    WDP_KIND_EVENT    = 2, /* Wii -> PC: unsolicited (watch hit, log, ...) */
    WDP_KIND__COUNT
};

/* Command IDs are grouped in ranges so new features never collide.
 * See docs/protocol.md for the allocation policy. */
enum wdp_cmd {
    WDP_CMD_HELLO     = 0x0001, /* handshake: version + capabilities        */
    WDP_CMD_PING      = 0x0002, /* echo the payload back                    */
    WDP_CMD_MEM_READ  = 0x0100, /* read memory (Phase 2)                    */
    WDP_CMD_MEM_WRITE = 0x0101  /* write memory (Phase 2)                   */
};

/* Capability bits, reported in the HELLO reply. A client asks "can you do X?"
 * by testing a bit, instead of comparing version numbers. */
enum wdp_cap {
    WDP_CAP_MEM_READ  = 1u << 0,
    WDP_CAP_MEM_WRITE = 1u << 1
};

/* Result of a request, carried in the response header. */
enum wdp_status {
    WDP_OK                 = 0,
    WDP_ERR_UNKNOWN_CMD    = 1, /* stub does not know this command        */
    WDP_ERR_BAD_PAYLOAD    = 2, /* payload malformed for this command     */
    WDP_ERR_BAD_VERSION    = 3, /* header version not supported           */
    WDP_ERR_RANGE          = 4, /* address range not allowed              */
    WDP_ERR_TOO_BIG        = 5, /* request or reply exceeds max payload   */
    WDP_ERR_BUSY           = 6, /* stub cannot service this right now     */
    WDP_ERR_UNSUPPORTED    = 7, /* known command, capability not built in */
    WDP_ERR_INTERNAL       = 8
};

/* Errors returned by the codec itself (not sent on the wire). */
enum wdp_err {
    WDP_E_OK       =  0,
    WDP_E_ARG      = -1, /* NULL pointer etc.                              */
    WDP_E_MAGIC    = -2, /* first two bytes are not "WD": not our protocol */
    WDP_E_KIND     = -3, /* kind is not request/response/event             */
    WDP_E_FLAGS    = -4, /* reserved flag bits set that we don't understand*/
    WDP_E_TOO_BIG  = -5, /* a length exceeds its allowed maximum           */
    WDP_E_SHORT    = -6  /* payload is shorter than its fields require     */
};

/* ---- Header ----------------------------------------------------------- */

/* In-memory (host) form of the header. The wire form is 16 big-endian
 * bytes; we NEVER memcpy this struct to the wire (see wdp.c for why). */
typedef struct wdp_header {
    uint16_t magic;
    uint8_t  version;
    uint8_t  kind;    /* enum wdp_kind */
    uint16_t seq;     /* request/response pairing; events use their own counter */
    uint16_t cmd;     /* enum wdp_cmd, or an event id for EVENT frames */
    uint16_t status;  /* enum wdp_status; 0 in requests and events */
    uint16_t flags;   /* reserved, must be 0 in version 1 */
    uint32_t len;     /* payload bytes following the header */
} wdp_header;

/* Fill *h with sane defaults for a new frame (magic + version set). */
void wdp_header_init(wdp_header *h, uint8_t kind, uint16_t seq,
                     uint16_t cmd, uint16_t status, uint32_t len);

/* Serialise *h into exactly WDP_HEADER_SIZE bytes. Returns WDP_E_OK or a
 * negative wdp_err. Does not validate semantics beyond what it must. */
int wdp_header_encode(const wdp_header *h, uint8_t out[WDP_HEADER_SIZE]);

/* Parse WDP_HEADER_SIZE bytes. Validates magic, kind, flags and len bound.
 * It deliberately does NOT reject an unknown `version`: the caller decides,
 * because a HELLO reply must still be possible to a client that speaks
 * a different version (see docs/protocol.md, "Versioning"). */
int wdp_header_decode(const uint8_t in[WDP_HEADER_SIZE], wdp_header *h);

/* Human-readable name of a codec error, for logs. Never returns NULL. */
const char *wdp_err_str(int err);

/* ---- HELLO payload (command WDP_CMD_HELLO, response direction) ---------- */

/* Fixed part of the HELLO response payload, followed by `build_len` bytes of
 * UTF-8 text (docs/protocol.md section 5). */
#define WDP_HELLO_FIXED_SIZE 14u

typedef struct wdp_hello {
    uint8_t  stub_major;
    uint8_t  stub_minor;
    uint32_t capabilities;           /* bitmask; no bits are defined yet */
    uint32_t max_payload;            /* largest payload the stub accepts */
    char     build[WDP_BUILD_MAX];   /* NUL-terminated, at most 31 chars */
} wdp_hello;

/* Serialise *h into out[0..cap). Returns the number of bytes written, or a
 * negative wdp_err (WDP_E_ARG, WDP_E_TOO_BIG if the build string is too long
 * or `cap` is too small). */
int wdp_hello_encode(const wdp_hello *h, uint8_t *out, size_t cap);

/* Parse a HELLO response payload of `len` bytes. Returns WDP_E_OK or a
 * negative wdp_err (WDP_E_SHORT if truncated, WDP_E_TOO_BIG if the build
 * string would not fit). Bytes after the build string are IGNORED on purpose:
 * that is how future versions can append fields without breaking old clients.
 * The reserved field is ignored too. */
int wdp_hello_decode(const uint8_t *in, size_t len, wdp_hello *h);

/* ---- Big-endian helpers (also used for payload fields) ---------------- */

/* Written byte-by-byte with shifts instead of casting pointers:
 *  - works regardless of the host CPU's endianness (Wii: big, PC: little),
 *  - works regardless of alignment (a u32* cast on a misaligned address is
 *    undefined behaviour in C and can fault on PowerPC). */
static inline void wdp_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void wdp_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline uint16_t wdp_get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline uint32_t wdp_get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

#ifdef __cplusplus
}
#endif

#endif /* WDP_H */
