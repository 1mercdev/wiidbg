/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Host-side test for common/wdp.c. Runs on your PC with plain gcc: no Wii,
 * no emulator. Reads tests/vectors.txt, the same file the Python tests read.
 *
 * Usage: test_wdp <path/to/vectors.txt>
 */
#include <stdio.h>
#include <string.h>
#include "../../common/wdp.h"

static int failures;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            failures++;                                               \
            printf("  FAIL line %d: ", line_no);                      \
            printf(__VA_ARGS__);                                      \
            printf("\n");                                             \
        }                                                             \
    } while (0)

static int hex_to_bytes(const char *hex, uint8_t *out, size_t n)
{
    if (strlen(hex) != n * 2)
        return -1;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1)
            return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

static int err_from_name(const char *name)
{
    if (!strcmp(name, "MAGIC"))   return WDP_E_MAGIC;
    if (!strcmp(name, "KIND"))    return WDP_E_KIND;
    if (!strcmp(name, "FLAGS"))   return WDP_E_FLAGS;
    if (!strcmp(name, "TOO_BIG")) return WDP_E_TOO_BIG;
    return 1; /* never a valid wdp_err */
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s vectors.txt\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror("vectors.txt"); return 2; }

    char buf[256];
    int line_no = 0, valid_n = 0, invalid_n = 0, hello_n = 0;

    while (fgets(buf, sizeof buf, f)) {
        line_no++;
        if (buf[0] == '#' || buf[0] == '\n' || buf[0] == '\0')
            continue;

        char kind_word[16], hex[256];
        if (sscanf(buf, "%15s %255s", kind_word, hex) != 2) {
            failures++;
            printf("  FAIL line %d: unparsable vector\n", line_no);
            continue;
        }

        uint8_t wire[WDP_HEADER_SIZE];
        int is_header_vector = !strcmp(kind_word, "valid") || !strcmp(kind_word, "invalid");
        if (is_header_vector)
            CHECK(hex_to_bytes(hex, wire, sizeof wire) == 0, "bad hex");

        if (!strcmp(kind_word, "valid")) {
            unsigned ver, kind, seq, cmd, status, flags, len;
            char dummy[16];
            int n = sscanf(buf, "%15s %255s %u %u %u %u %u %u %u", dummy, hex,
                           &ver, &kind, &seq, &cmd, &status, &flags, &len);
            CHECK(n == 9, "valid vector needs 7 numeric fields");

            wdp_header h;
            int rc = wdp_header_decode(wire, &h);
            CHECK(rc == WDP_E_OK, "decode failed: %s", wdp_err_str(rc));
            if (rc == WDP_E_OK) {
                CHECK(h.version == ver,   "version %u != %u", h.version, ver);
                CHECK(h.kind == kind,     "kind %u != %u", h.kind, kind);
                CHECK(h.seq == seq,       "seq %u != %u", h.seq, seq);
                CHECK(h.cmd == cmd,       "cmd %u != %u", h.cmd, cmd);
                CHECK(h.status == status, "status %u != %u", h.status, status);
                CHECK(h.flags == flags,   "flags %u != %u", h.flags, flags);
                CHECK(h.len == len,       "len %u != %u", h.len, len);

                /* Round trip: encode(decode(x)) must give x back exactly. */
                uint8_t again[WDP_HEADER_SIZE];
                CHECK(wdp_header_encode(&h, again) == WDP_E_OK, "encode failed");
                CHECK(memcmp(again, wire, sizeof wire) == 0, "round-trip mismatch");
            }
            valid_n++;
        } else if (!strcmp(kind_word, "invalid")) {
            char name[16], dummy[16];
            int n = sscanf(buf, "%15s %255s %15s", dummy, hex, name);
            CHECK(n == 3, "invalid vector needs an error name");
            wdp_header h;
            int want = err_from_name(name);
            int rc = wdp_header_decode(wire, &h);
            CHECK(rc == want, "expected %s (%d), got %s (%d)", name, want,
                  wdp_err_str(rc), rc);
            invalid_n++;
        } else if (!strcmp(kind_word, "hello")) {
            char dummy[16], build[64];
            unsigned major, minor, caps, maxp;
            int n = sscanf(buf, "%15s %255s %u %u %u %u %63s", dummy, hex,
                           &major, &minor, &caps, &maxp, build);
            CHECK(n == 7, "hello vector needs 5 fields after the hex");
            if (!strcmp(build, "-"))
                build[0] = '\0';

            uint8_t payload[128];
            size_t plen = strlen(hex) / 2;
            CHECK(plen <= sizeof payload && hex_to_bytes(hex, payload, plen) == 0, "bad hello hex");

            wdp_hello h;
            int rc = wdp_hello_decode(payload, plen, &h);
            CHECK(rc == WDP_E_OK, "hello decode failed: %s", wdp_err_str(rc));
            if (rc == WDP_E_OK) {
                CHECK(h.stub_major == major, "major");
                CHECK(h.stub_minor == minor, "minor");
                CHECK(h.capabilities == caps, "caps");
                CHECK(h.max_payload == maxp, "max_payload");
                CHECK(!strcmp(h.build, build), "build '%s' != '%s'", h.build, build);

                /* Re-encoding must reproduce the leading bytes of the payload. */
                uint8_t again[128];
                int en = wdp_hello_encode(&h, again, sizeof again);
                CHECK(en > 0 && (size_t)en <= plen, "encode length %d", en);
                if (en > 0 && (size_t)en <= plen)
                    CHECK(memcmp(again, payload, (size_t)en) == 0, "hello round-trip mismatch");
            }
            hello_n++;
        } else if (!strcmp(kind_word, "hello_invalid")) {
            char dummy[16], name[16];
            int n = sscanf(buf, "%15s %255s %15s", dummy, hex, name);
            CHECK(n == 3, "hello_invalid needs an error name");
            uint8_t payload[128];
            size_t plen = !strcmp(hex, "-") ? 0 : strlen(hex) / 2;
            CHECK(plen <= sizeof payload && (plen == 0 || hex_to_bytes(hex, payload, plen) == 0), "bad hex");
            wdp_hello h;
            int want = !strcmp(name, "SHORT") ? WDP_E_SHORT : err_from_name(name);
            int rc = wdp_hello_decode(payload, plen, &h);
            CHECK(rc == want, "expected %s (%d), got %s (%d)", name, want, wdp_err_str(rc), rc);
            hello_n++;
        } else {
            CHECK(0, "unknown vector type '%s'", kind_word);
        }
    }
    fclose(f);

    /* A few direct API checks that don't belong in the shared file. */
    {
        int line_no = -1;
        uint8_t out_big[64];
        wdp_header h;
        uint8_t out[WDP_HEADER_SIZE];
        CHECK(wdp_header_decode(NULL, &h) == WDP_E_ARG, "NULL input");
        CHECK(wdp_header_encode(NULL, out) == WDP_E_ARG, "NULL header");

        /* On a failed decode the output must be left untouched. */
        wdp_header sentinel;
        wdp_header_init(&sentinel, WDP_KIND_EVENT, 77, 5, 0, 0);
        wdp_header copy = sentinel;
        uint8_t junk[WDP_HEADER_SIZE] = {0};
        CHECK(wdp_header_decode(junk, &copy) == WDP_E_MAGIC, "junk should fail");
        CHECK(memcmp(&copy, &sentinel, sizeof copy) == 0,
              "failed decode modified its output");

        /* HELLO encode refuses what it cannot represent. */
        wdp_hello hh;
        memset(&hh, 0, sizeof hh);
        memset(hh.build, 'z', WDP_BUILD_MAX);            /* no terminator at all */
        CHECK(wdp_hello_encode(&hh, out_big, sizeof out_big) == WDP_E_TOO_BIG, "unterminated build");
        memset(&hh, 0, sizeof hh);
        memcpy(hh.build, "abc", 4);
        CHECK(wdp_hello_encode(&hh, out_big, WDP_HELLO_FIXED_SIZE + 2) == WDP_E_TOO_BIG, "small buffer");
        CHECK(wdp_hello_encode(&hh, out_big, WDP_HELLO_FIXED_SIZE + 3) == (int)(WDP_HELLO_FIXED_SIZE + 3), "exact fit");

        /* Big-endian helpers: independent of host byte order. */
        uint8_t b[4];
        wdp_put_u32(b, 0x01020304u);
        CHECK(b[0] == 1 && b[1] == 2 && b[2] == 3 && b[3] == 4, "put_u32 order");
        CHECK(wdp_get_u32(b) == 0x01020304u, "get_u32 round trip");
        wdp_put_u16(b, 0xA1B2u);
        CHECK(b[0] == 0xA1 && b[1] == 0xB2, "put_u16 order");
    }

    printf("test_wdp: %d header + %d invalid-header + %d hello vectors, %d failure(s)\n",
           valid_n, invalid_n, hello_n, failures);
    return failures ? 1 : 0;
}
