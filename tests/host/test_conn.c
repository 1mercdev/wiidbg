/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Host-side tests for common/wdp_conn.c, the protocol state machine.
 * No sockets involved: bytes go in through wdp_conn_feed(), come out through
 * wdp_conn_out(). That is the whole point of the sans-I/O design.
 */
#include <stdio.h>
#include <string.h>
#include "../../common/wdp_conn.h"

static int failures;
#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            failures++;                                   \
            printf("  FAIL %s:%d: ", __func__, __LINE__); \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

static wdp_conn conn;      /* ~32 KiB: static, not on the stack */
static const wdp_conn_config cfg = { 0, 1, 0x0, "0.1.0" };

static void fresh(void)
{
    int rc = wdp_conn_init(&conn, &cfg);
    if (rc != 0) { printf("init failed: %d\n", rc); failures++; }
}

/* Build one request frame into buf; returns its total size. */
static size_t make_req(uint8_t *buf, uint16_t seq, uint16_t cmd,
                       const uint8_t *payload, size_t plen)
{
    wdp_header h;
    wdp_header_init(&h, WDP_KIND_REQUEST, seq, cmd, 0, (uint32_t)plen);
    wdp_header_encode(&h, buf);
    if (plen)
        memcpy(buf + WDP_HEADER_SIZE, payload, plen);
    return WDP_HEADER_SIZE + plen;
}

/* Play the role of the server loop: offer `in` to the connection in chunks of
 * `chunk` bytes, drain everything it wants to send into out[]. Returns the
 * last wdp_conn_feed() result. */
static int pump(const uint8_t *in, size_t n, size_t chunk, uint8_t *out, size_t *out_len)
{
    size_t off = 0, olen = 0;
    int rc = WDP_CONN_OK;
    while (off < n) {
        size_t end = off + chunk < n ? off + chunk : n;
        size_t pos = off;
        while (pos < end) {
            size_t used = 0;
            rc = wdp_conn_feed(&conn, in + pos, end - pos, &used);
            pos += used;
            size_t ol;
            const uint8_t *o = wdp_conn_out(&conn, &ol);
            if (ol) { memcpy(out + olen, o, ol); olen += ol; wdp_conn_out_consume(&conn, ol); }
            if (rc != WDP_CONN_OK) { *out_len = olen; return rc; }
            if (used == 0 && ol == 0) {      /* no progress: would loop forever */
                *out_len = olen;
                return -999;
            }
        }
        off = end;
    }
    *out_len = olen;
    return rc;
}

#define OUTCAP (3 * (WDP_HEADER_SIZE + WDP_MAX_PAYLOAD))
static uint8_t out[OUTCAP];
static uint8_t in[OUTCAP];

static void test_hello_reply(void)
{
    fresh();
    size_t n = make_req(in, 7, WDP_CMD_HELLO, NULL, 0), olen = 0;
    int rc = pump(in, n, n, out, &olen);
    CHECK(rc == WDP_CONN_OK, "rc %d", rc);
    wdp_header h;
    CHECK(olen >= WDP_HEADER_SIZE && wdp_header_decode(out, &h) == WDP_E_OK, "reply header");
    CHECK(h.kind == WDP_KIND_RESPONSE && h.seq == 7 && h.cmd == WDP_CMD_HELLO &&
          h.status == WDP_OK && h.version == WDP_VERSION, "reply fields");
    CHECK(olen == WDP_HEADER_SIZE + h.len, "length matches header");
    wdp_hello hello;
    CHECK(wdp_hello_decode(out + WDP_HEADER_SIZE, h.len, &hello) == WDP_E_OK, "hello decode");
    CHECK(hello.stub_major == 0 && hello.stub_minor == 1 && hello.capabilities == 0 &&
          hello.max_payload == WDP_MAX_PAYLOAD && !strcmp(hello.build, "0.1.0"), "hello content");
}

static void test_hello_rejects_payload(void)
{
    fresh();
    uint8_t junk[3] = {1, 2, 3};
    size_t n = make_req(in, 9, WDP_CMD_HELLO, junk, sizeof junk), olen = 0;
    int rc = pump(in, n, n, out, &olen);
    wdp_header h;
    wdp_header_decode(out, &h);
    CHECK(rc == WDP_CONN_OK, "connection stays open, rc %d", rc);
    CHECK(h.status == WDP_ERR_BAD_PAYLOAD && h.len == 0 && h.seq == 9, "status %u", h.status);
}

static void test_ping_echo_many_sizes(void)
{
    static const size_t sizes[] = {0, 1, 15, 16, 17, 1000, WDP_MAX_PAYLOAD};
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        fresh();
        uint8_t payload[WDP_MAX_PAYLOAD];
        for (size_t k = 0; k < sizes[i]; k++) payload[k] = (uint8_t)(k * 7 + 3);
        size_t n = make_req(in, (uint16_t)(100 + i), WDP_CMD_PING, payload, sizes[i]), olen = 0;
        int rc = pump(in, n, n, out, &olen);
        wdp_header h;
        wdp_header_decode(out, &h);
        CHECK(rc == WDP_CONN_OK, "size %zu rc %d", sizes[i], rc);
        CHECK(h.status == WDP_OK && h.len == sizes[i] && h.seq == 100 + i, "size %zu header", sizes[i]);
        CHECK(olen == WDP_HEADER_SIZE + sizes[i], "size %zu out length %zu", sizes[i], olen);
        CHECK(memcmp(out + WDP_HEADER_SIZE, payload, sizes[i]) == 0, "size %zu echo mismatch", sizes[i]);
    }
}

/* Property: the way bytes are split on the wire must not change the answers. */
static void test_chunking_does_not_matter(void)
{
    uint8_t p100[100];
    for (int k = 0; k < 100; k++) p100[k] = (uint8_t)(255 - k);
    size_t n = 0;
    n += make_req(in + n, 1, WDP_CMD_HELLO, NULL, 0);
    n += make_req(in + n, 2, WDP_CMD_PING, p100, sizeof p100);
    n += make_req(in + n, 3, 0x9999, NULL, 0);            /* unknown command */
    n += make_req(in + n, 4, WDP_CMD_PING, NULL, 0);

    static uint8_t ref[OUTCAP];
    size_t ref_len = 0;
    fresh();
    CHECK(pump(in, n, n, ref, &ref_len) == WDP_CONN_OK, "bulk");

    static const size_t chunks[] = {1, 2, 7, 16, 17, 50};
    for (unsigned i = 0; i < sizeof chunks / sizeof chunks[0]; i++) {
        size_t olen = 0;
        fresh();
        int rc = pump(in, n, chunks[i], out, &olen);
        CHECK(rc == WDP_CONN_OK, "chunk %zu rc %d", chunks[i], rc);
        CHECK(olen == ref_len && memcmp(out, ref, ref_len) == 0, "chunk %zu output differs", chunks[i]);
    }
}

static void test_unknown_command_keeps_connection(void)
{
    fresh();
    size_t n = make_req(in, 5, 0xBEEF, NULL, 0);
    n += make_req(in + n, 6, WDP_CMD_PING, (const uint8_t *)"hi", 2);
    size_t olen = 0;
    int rc = pump(in, n, n, out, &olen);
    wdp_header a, b;
    wdp_header_decode(out, &a);
    wdp_header_decode(out + WDP_HEADER_SIZE, &b);
    CHECK(rc == WDP_CONN_OK, "rc %d", rc);
    CHECK(a.status == WDP_ERR_UNKNOWN_CMD && a.seq == 5 && a.cmd == 0xBEEF, "unknown reply");
    CHECK(b.status == WDP_OK && b.seq == 6 && b.len == 2, "next request still served");
}

static void test_bad_version_replies_then_closes(void)
{
    fresh();
    size_t n = make_req(in, 11, WDP_CMD_HELLO, NULL, 0), olen = 0;
    in[2] = 9;                                   /* a version we do not speak */
    int rc = pump(in, n, n, out, &olen);
    wdp_header h;
    CHECK(rc == WDP_CONN_CLOSE, "rc %d", rc);
    CHECK(olen == WDP_HEADER_SIZE + 1, "olen %zu", olen);
    CHECK(wdp_header_decode(out, &h) == WDP_E_OK, "reply decodes");
    CHECK(h.status == WDP_ERR_BAD_VERSION && h.seq == 11 && h.version == WDP_VERSION, "reply fields");
    CHECK(out[WDP_HEADER_SIZE] == WDP_VERSION, "payload carries the version we speak");

    /* Once closing, nothing more is accepted. */
    size_t used = 99;
    uint8_t more[4] = {0};
    CHECK(wdp_conn_feed(&conn, more, sizeof more, &used) == WDP_CONN_CLOSE && used == 0, "closed for good");
}

static void test_version_checked_before_other_fields(void)
{
    /* Wrong version AND garbage flags: the version error must win, because a
     * newer protocol may legitimately use those fields differently. */
    fresh();
    size_t n = make_req(in, 1, WDP_CMD_HELLO, NULL, 0), olen = 0;
    in[2] = 2;
    in[10] = 0xFF;                               /* flags */
    int rc = pump(in, n, n, out, &olen);
    wdp_header h;
    wdp_header_decode(out, &h);
    CHECK(rc == WDP_CONN_CLOSE && h.status == WDP_ERR_BAD_VERSION, "rc %d status %u", rc, h.status);
}

static void test_fatal_framing_errors(void)
{
    struct { const char *name; int offset; uint8_t val; int want; } t[] = {
        { "bad magic",  0, 0x00, WDP_E_MAGIC },
        { "bad kind",   3, 0x07, WDP_E_KIND },
        { "bad flags", 11, 0x01, WDP_E_FLAGS },
        { "response sent to stub", 3, WDP_KIND_RESPONSE, WDP_CONN_E_NOT_REQUEST },
        { "event sent to stub",    3, WDP_KIND_EVENT,    WDP_CONN_E_NOT_REQUEST },
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        fresh();
        size_t n = make_req(in, 1, WDP_CMD_PING, NULL, 0), olen = 0;
        in[t[i].offset] = t[i].val;
        int rc = pump(in, n, n, out, &olen);
        CHECK(rc == t[i].want, "%s: rc %d (%s), want %d", t[i].name, rc, wdp_conn_strerror(rc), t[i].want);
        CHECK(olen == 0, "%s: must not answer", t[i].name);
    }
}

static void test_oversize_length_rejected_before_payload(void)
{
    fresh();
    uint8_t hdr[WDP_HEADER_SIZE];
    wdp_header h;
    wdp_header_init(&h, WDP_KIND_REQUEST, 1, WDP_CMD_PING, 0, 0);
    wdp_header_encode(&h, hdr);
    wdp_put_u32(hdr + 12, 0xFFFFFFFFu);          /* claims a 4 GiB payload */
    size_t used = 0;
    int rc = wdp_conn_feed(&conn, hdr, sizeof hdr, &used);
    CHECK(rc == WDP_E_TOO_BIG, "rc %d", rc);
    CHECK(used == WDP_HEADER_SIZE, "only the header was consumed, used=%zu", used);
}

static void test_backpressure(void)
{
    fresh();
    size_t n = make_req(in, 1, WDP_CMD_PING, (const uint8_t *)"AAAA", 4);
    size_t first = n;
    n += make_req(in + n, 2, WDP_CMD_PING, (const uint8_t *)"BBBB", 4);

    size_t used = 0;
    int rc = wdp_conn_feed(&conn, in, n, &used);
    CHECK(rc == WDP_CONN_OK, "rc %d", rc);
    CHECK(used == first, "must stop after the first frame while its reply is unsent: used=%zu want %zu", used, first);

    size_t olen;
    const uint8_t *o = wdp_conn_out(&conn, &olen);
    CHECK(olen == WDP_HEADER_SIZE + 4 && o[WDP_HEADER_SIZE] == 'A', "first reply queued");

    /* While the reply is pending, no further input is taken. */
    rc = wdp_conn_feed(&conn, in + first, n - first, &used);
    CHECK(used == 0, "still blocked: used=%zu", used);

    /* Partial send: only part of the reply is drained, so still blocked. */
    wdp_conn_out_consume(&conn, 5);
    o = wdp_conn_out(&conn, &olen);
    CHECK(olen == WDP_HEADER_SIZE + 4 - 5, "partial drain tracked: %zu", olen);
    rc = wdp_conn_feed(&conn, in + first, n - first, &used);
    CHECK(used == 0, "blocked until fully drained");

    wdp_conn_out_consume(&conn, olen);           /* drain the rest */
    rc = wdp_conn_feed(&conn, in + first, n - first, &used);
    CHECK(rc == WDP_CONN_OK && used == n - first, "second request accepted: used=%zu", used);
    o = wdp_conn_out(&conn, &olen);
    CHECK(olen == WDP_HEADER_SIZE + 4 && o[WDP_HEADER_SIZE] == 'B', "second reply queued");
}

static void test_no_reply_until_payload_complete(void)
{
    fresh();
    uint8_t payload[10] = {0};
    size_t n = make_req(in, 1, WDP_CMD_PING, payload, sizeof payload);
    size_t used = 0, olen = 0;
    wdp_conn_feed(&conn, in, n - 1, &used);      /* everything but the last byte */
    wdp_conn_out(&conn, &olen);
    CHECK(used == n - 1 && olen == 0, "no reply yet (used=%zu olen=%zu)", used, olen);
    wdp_conn_feed(&conn, in + n - 1, 1, &used);
    wdp_conn_out(&conn, &olen);
    CHECK(olen == WDP_HEADER_SIZE + sizeof payload, "reply appears with the last byte");
}

static void test_init_validates_config(void)
{
    wdp_conn_config c31 = { 0, 1, 0, "0123456789012345678901234567890" };   /* 31 chars */
    wdp_conn_config c32 = { 0, 1, 0, "01234567890123456789012345678901" };  /* 32 chars */
    wdp_conn_config cnull = { 0, 1, 0, NULL };
    CHECK(wdp_conn_init(&conn, &c31) == 0, "31 chars accepted");
    CHECK(wdp_conn_init(&conn, &c32) == WDP_E_TOO_BIG, "32 chars rejected");
    CHECK(wdp_conn_init(&conn, &cnull) == WDP_E_ARG, "NULL build rejected");
    CHECK(wdp_conn_init(NULL, &c31) == WDP_E_ARG, "NULL conn rejected");
}

int main(void)
{
    test_hello_reply();
    test_hello_rejects_payload();
    test_ping_echo_many_sizes();
    test_chunking_does_not_matter();
    test_unknown_command_keeps_connection();
    test_bad_version_replies_then_closes();
    test_version_checked_before_other_fields();
    test_fatal_framing_errors();
    test_oversize_length_rejected_before_payload();
    test_backpressure();
    test_no_reply_until_payload_complete();
    test_init_validates_config();
    printf("test_conn: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
