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
static const wdp_conn_config cfg = { .stub_major = 0, .stub_minor = 1, .build = "0.1.0" };

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
    wdp_conn_config c31 = { .build = "0123456789012345678901234567890" };   /* 31 chars */
    wdp_conn_config c32 = { .build = "01234567890123456789012345678901" };  /* 32 chars */
    wdp_conn_config cnull = { .build = NULL };
    CHECK(wdp_conn_init(&conn, &c31) == 0, "31 chars accepted");
    CHECK(wdp_conn_init(&conn, &c32) == WDP_E_TOO_BIG, "32 chars rejected");
    CHECK(wdp_conn_init(&conn, &cnull) == WDP_E_ARG, "NULL build rejected");
    CHECK(wdp_conn_init(NULL, &c31) == WDP_E_ARG, "NULL conn rejected");
}


/* ======================= Phase 2: memory commands ======================= */

/* A fake memory backend. It records every call so tests can assert on what the
 * protocol layer did (and, crucially, what it did NOT do). Reads return a
 * pattern that depends only on the address, so no 88 MiB of fake RAM is needed. */
static struct {
    int      reads, writes;
    uint32_t last_addr, last_len;
    uint8_t  wdata[WDP_MAX_PAYLOAD];
    int      fail;                    /* make the backend report an error */
} fake;

static uint8_t pat(uint32_t a) { return (uint8_t)(a * 31u + 7u); }

static int fake_read(uint32_t addr, void *dst, uint32_t len)
{
    fake.reads++;
    fake.last_addr = addr;
    fake.last_len = len;
    if (fake.fail) return -1;
    for (uint32_t i = 0; i < len; i++) ((uint8_t *)dst)[i] = pat(addr + i);
    return 0;
}

static int fake_write(uint32_t addr, const void *src, uint32_t len)
{
    fake.writes++;
    fake.last_addr = addr;
    fake.last_len = len;
    if (fake.fail) return -1;
    memcpy(fake.wdata, src, len);
    return 0;
}

static const wmem_ops fake_ops = { fake_read, fake_write };
static const wmem_ops read_only_ops = { fake_read, NULL };

static void fresh_mem(const wmem_ops *ops)
{
    memset(&fake, 0, sizeof fake);
    wdp_conn_config c = { .build = "t", .mem = ops };
    if (wdp_conn_init(&conn, &c) != 0) { printf("init failed\n"); failures++; }
}

static size_t make_read_req(uint8_t *buf, uint16_t seq, uint32_t addr, uint32_t n)
{
    uint8_t p[8];
    wdp_put_u32(p, addr);
    wdp_put_u32(p + 4, n);
    return make_req(buf, seq, WDP_CMD_MEM_READ, p, sizeof p);
}

static size_t make_write_req(uint8_t *buf, uint16_t seq, uint32_t addr,
                             const uint8_t *data, size_t n)
{
    static uint8_t p[WDP_MAX_PAYLOAD];
    wdp_put_u32(p, addr);
    memcpy(p + 4, data, n);
    return make_req(buf, seq, WDP_CMD_MEM_WRITE, p, 4 + n);
}

/* Send one request, return the reply's header; payload is at out + HEADER_SIZE. */
static wdp_header exchange(size_t n)
{
    size_t olen = 0;
    wdp_header h;
    memset(&h, 0, sizeof h);
    int rc = pump(in, n, n, out, &olen);
    if (rc != WDP_CONN_OK || olen < WDP_HEADER_SIZE || wdp_header_decode(out, &h) != WDP_E_OK) {
        printf("  (exchange: rc=%d olen=%zu)\n", rc, olen);
        failures++;
    }
    return h;
}

static void test_capabilities_follow_the_backend(void)
{
    wdp_hello hello;
    size_t n, olen;

    fresh_mem(&fake_ops);
    n = make_req(in, 1, WDP_CMD_HELLO, NULL, 0);
    pump(in, n, n, out, &olen);
    wdp_hello_decode(out + WDP_HEADER_SIZE, olen - WDP_HEADER_SIZE, &hello);
    CHECK(hello.capabilities == (WDP_CAP_MEM_READ | WDP_CAP_MEM_WRITE), "both: 0x%x", hello.capabilities);

    fresh_mem(&read_only_ops);
    pump(in, n, n, out, &olen);
    wdp_hello_decode(out + WDP_HEADER_SIZE, olen - WDP_HEADER_SIZE, &hello);
    CHECK(hello.capabilities == WDP_CAP_MEM_READ, "read only: 0x%x", hello.capabilities);

    fresh_mem(NULL);
    pump(in, n, n, out, &olen);
    wdp_hello_decode(out + WDP_HEADER_SIZE, olen - WDP_HEADER_SIZE, &hello);
    CHECK(hello.capabilities == 0, "no backend: 0x%x", hello.capabilities);
}

static void test_mem_read_returns_the_data(void)
{
    static const struct { uint32_t addr, n; } cases[] = {
        { 0x80001000u, 100 },
        { WMEM_MEM1_BASE, 1 },
        { WMEM_MEM1_BASE + WMEM_MEM1_SIZE - 1, 1 },        /* very last byte of MEM1 */
        { WMEM_MEM2_BASE + 0x12345u, WDP_MAX_PAYLOAD },    /* biggest legal read, unaligned */
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        fresh_mem(&fake_ops);
        wdp_header h = exchange(make_read_req(in, (uint16_t)(10 + i), cases[i].addr, cases[i].n));
        CHECK(h.status == WDP_OK && h.seq == 10 + i && h.cmd == WDP_CMD_MEM_READ, "case %u header", i);
        CHECK(h.len == cases[i].n, "case %u length %u", i, h.len);
        int ok = 1;
        for (uint32_t k = 0; k < h.len; k++)
            if (out[WDP_HEADER_SIZE + k] != pat(cases[i].addr + k)) { ok = 0; break; }
        CHECK(ok, "case %u data mismatch", i);
        CHECK(fake.reads == 1 && fake.last_addr == cases[i].addr && fake.last_len == cases[i].n,
              "case %u backend call", i);
    }
}

static void test_mem_read_rejects_bad_requests_without_touching_memory(void)
{
    struct { const char *name; uint32_t addr, n; uint16_t status; } bad[] = {
        { "zero length",               0x80000000u, 0,                  WDP_ERR_BAD_PAYLOAD },
        { "longer than one frame",     0x80000000u, WDP_MAX_PAYLOAD + 1, WDP_ERR_TOO_BIG },
        { "huge length",               0x80000000u, 0xFFFFFFFFu,        WDP_ERR_TOO_BIG },
        { "address 0",                 0x00000000u, 4,                  WDP_ERR_RANGE },
        { "uncached mirror",           0xC0000000u, 4,                  WDP_ERR_RANGE },
        { "hardware registers",        0xCC000000u, 4,                  WDP_ERR_RANGE },
        { "one past the end of MEM1",  WMEM_MEM1_BASE + WMEM_MEM1_SIZE, 1, WDP_ERR_RANGE },
        { "starts in MEM1, ends past", WMEM_MEM1_BASE + WMEM_MEM1_SIZE - 2, 4, WDP_ERR_RANGE },
        { "wraps around 2^32",         0xFFFFFFF0u, 0x20,               WDP_ERR_RANGE },
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        fresh_mem(&fake_ops);
        wdp_header h = exchange(make_read_req(in, 1, bad[i].addr, bad[i].n));
        CHECK(h.status == bad[i].status, "%s: status %u, want %u", bad[i].name, h.status, bad[i].status);
        CHECK(h.len == 0, "%s: error replies carry no payload", bad[i].name);
        CHECK(fake.reads == 0 && fake.writes == 0, "%s: BACKEND WAS CALLED", bad[i].name);
    }
    /* Wrong request sizes: the payload must be exactly 8 bytes. */
    static const size_t sizes[] = { 0, 4, 7, 9, 16 };
    uint8_t junk[16] = {0};
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        fresh_mem(&fake_ops);
        wdp_header h = exchange(make_req(in, 1, WDP_CMD_MEM_READ, junk, sizes[i]));
        CHECK(h.status == WDP_ERR_BAD_PAYLOAD, "payload %zu bytes: status %u", sizes[i], h.status);
        CHECK(fake.reads == 0, "payload %zu bytes: backend called", sizes[i]);
    }
}

static void test_mem_write_delivers_exactly_the_bytes(void)
{
    uint8_t data[WDP_MAX_PAYLOAD - 4];
    for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 13 + 5);

    static const struct { uint32_t addr; size_t n; } cases[] = {
        { 0x90000010u, 5 },
        { WMEM_MEM1_BASE, 1 },
        { WMEM_MEM1_BASE + WMEM_MEM1_SIZE - 1, 1 },
        { WMEM_MEM2_BASE + WMEM_MEM2_SIZE - 16380, 16380 },   /* biggest legal write, ends on the last byte */
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        fresh_mem(&fake_ops);
        wdp_header h = exchange(make_write_req(in, (uint16_t)(20 + i), cases[i].addr, data, cases[i].n));
        CHECK(h.status == WDP_OK && h.len == 0 && h.seq == 20 + i, "case %u reply", i);
        CHECK(fake.writes == 1 && fake.last_addr == cases[i].addr && fake.last_len == cases[i].n,
              "case %u backend call (addr 0x%x len %u)", i, fake.last_addr, fake.last_len);
        CHECK(memcmp(fake.wdata, data, cases[i].n) == 0, "case %u bytes differ", i);
    }
}

static void test_mem_write_rejects_bad_requests_without_touching_memory(void)
{
    uint8_t data[64] = {1, 2, 3};
    struct { const char *name; uint32_t addr; size_t n; } bad[] = {
        { "address 0",               0x00000000u, 4 },
        { "hardware registers",      0xCC000000u, 4 },
        { "just before MEM1",        WMEM_MEM1_BASE - 1, 1 },
        { "one past the end of MEM2", WMEM_MEM2_BASE + WMEM_MEM2_SIZE, 1 },
        { "starts inside, ends past", WMEM_MEM2_BASE + WMEM_MEM2_SIZE - 3, 8 },
        { "wraps around 2^32",       0xFFFFFFFCu, 16 },
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        fresh_mem(&fake_ops);
        wdp_header h = exchange(make_write_req(in, 1, bad[i].addr, data, bad[i].n));
        CHECK(h.status == WDP_ERR_RANGE, "%s: status %u", bad[i].name, h.status);
        CHECK(fake.writes == 0, "%s: BACKEND WAS CALLED, memory would have been modified", bad[i].name);
    }
    /* Payload too short to contain an address plus at least one data byte. */
    for (size_t n = 0; n <= 4; n++) {
        fresh_mem(&fake_ops);
        wdp_header h = exchange(make_req(in, 1, WDP_CMD_MEM_WRITE, data, n));
        CHECK(h.status == WDP_ERR_BAD_PAYLOAD, "payload %zu bytes: status %u", n, h.status);
        CHECK(fake.writes == 0, "payload %zu bytes: backend called", n);
    }
}

static void test_backend_failure_becomes_internal_error(void)
{
    uint8_t data[4] = {0};
    fresh_mem(&fake_ops);
    fake.fail = 1;
    wdp_header h = exchange(make_read_req(in, 1, 0x80000000u, 4));
    CHECK(h.status == WDP_ERR_INTERNAL && h.len == 0, "read failure: status %u", h.status);
    h = exchange(make_write_req(in, 2, 0x80000000u, data, 4));
    CHECK(h.status == WDP_ERR_INTERNAL, "write failure: status %u", h.status);
}

static void test_without_a_backend_commands_are_unsupported(void)
{
    uint8_t data[4] = {0};
    fresh_mem(NULL);
    wdp_header h = exchange(make_read_req(in, 1, 0x80000000u, 4));
    CHECK(h.status == WDP_ERR_UNSUPPORTED, "read: %u", h.status);
    h = exchange(make_write_req(in, 2, 0x80000000u, data, 4));
    CHECK(h.status == WDP_ERR_UNSUPPORTED, "write: %u", h.status);

    fresh_mem(&read_only_ops);                       /* partial backend */
    h = exchange(make_write_req(in, 3, 0x80000000u, data, 4));
    CHECK(h.status == WDP_ERR_UNSUPPORTED, "write on read-only backend: %u", h.status);
    h = exchange(make_read_req(in, 4, 0x80000000u, 4));
    CHECK(h.status == WDP_OK, "read on read-only backend: %u", h.status);
}

static void test_a_failed_request_does_not_break_the_connection(void)
{
    fresh_mem(&fake_ops);
    size_t n = make_read_req(in, 1, 0x00000000u, 4);                       /* refused */
    n += make_req(in + n, 2, WDP_CMD_PING, (const uint8_t *)"ok", 2);      /* then fine */
    n += make_read_req(in + n, 3, 0x80000000u, 2);                         /* and a good read */
    size_t olen = 0;
    int rc = pump(in, n, n, out, &olen);
    CHECK(rc == WDP_CONN_OK, "rc %d", rc);
    wdp_header a, b, c;
    wdp_header_decode(out, &a);
    wdp_header_decode(out + WDP_HEADER_SIZE, &b);
    wdp_header_decode(out + 2 * WDP_HEADER_SIZE + 2, &c);
    CHECK(a.status == WDP_ERR_RANGE && b.status == WDP_OK && c.status == WDP_OK &&
          c.len == 2, "statuses %u %u %u", a.status, b.status, c.status);
}

static void test_oversize_write_is_a_framing_error(void)
{
    /* A write payload of MAX+1 bytes is rejected by the header check, before
     * any byte of it is read, and the connection is dropped: the backend is
     * never involved. Clients must respect max_payload - 4. */
    fresh_mem(&fake_ops);
    uint8_t hdr[WDP_HEADER_SIZE];
    wdp_header h;
    wdp_header_init(&h, WDP_KIND_REQUEST, 1, WDP_CMD_MEM_WRITE, 0, WDP_MAX_PAYLOAD + 1);
    wdp_header_encode(&h, hdr);
    size_t used = 0;
    int rc = wdp_conn_feed(&conn, hdr, sizeof hdr, &used);
    CHECK(rc == WDP_E_TOO_BIG, "rc %d", rc);
    CHECK(fake.writes == 0, "backend untouched");
}

static void test_memory_requests_survive_any_chunking(void)
{
    uint8_t data[1000];
    for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i ^ 0x5A);
    size_t n = make_write_req(in, 1, 0x80002000u, data, sizeof data);

    static const size_t chunks[] = { 1, 3, 16, 17, 999 };
    for (unsigned i = 0; i < sizeof chunks / sizeof chunks[0]; i++) {
        fresh_mem(&fake_ops);
        size_t olen = 0;
        int rc = pump(in, n, chunks[i], out, &olen);
        CHECK(rc == WDP_CONN_OK, "chunk %zu rc %d", chunks[i], rc);
        CHECK(fake.writes == 1 && fake.last_len == sizeof data &&
              memcmp(fake.wdata, data, sizeof data) == 0, "chunk %zu: write delivered whole, exactly once", chunks[i]);
    }
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
    test_capabilities_follow_the_backend();
    test_mem_read_returns_the_data();
    test_mem_read_rejects_bad_requests_without_touching_memory();
    test_mem_write_delivers_exactly_the_bytes();
    test_mem_write_rejects_bad_requests_without_touching_memory();
    test_backend_failure_becomes_internal_error();
    test_without_a_backend_commands_are_unsupported();
    test_a_failed_request_does_not_break_the_connection();
    test_oversize_write_is_a_framing_error();
    test_memory_requests_survive_any_chunking();
    printf("test_conn: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
