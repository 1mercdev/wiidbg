/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Host-side tests for the memory access policy (common/wmem.c).
 * This is the code that decides whether a client may touch an address, so the
 * tests concentrate on boundaries: one byte inside, one byte outside.
 */
#include <stdio.h>
#include "../../common/wmem.h"

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

#define MEM1_END (WMEM_MEM1_BASE + WMEM_MEM1_SIZE)   /* first address AFTER MEM1 */
#define MEM2_END (WMEM_MEM2_BASE + WMEM_MEM2_SIZE)

static void test_inside_each_region(void)
{
    CHECK(wmem_range_ok(WMEM_MEM1_BASE, 1), "first byte of MEM1");
    CHECK(wmem_range_ok(WMEM_MEM1_BASE, WMEM_MEM1_SIZE), "all of MEM1");
    CHECK(wmem_range_ok(MEM1_END - 1, 1), "last byte of MEM1");
    CHECK(wmem_range_ok(WMEM_MEM2_BASE, 1), "first byte of MEM2");
    CHECK(wmem_range_ok(WMEM_MEM2_BASE, WMEM_MEM2_SIZE), "all of MEM2");
    CHECK(wmem_range_ok(MEM2_END - 1, 1), "last byte of MEM2");
    CHECK(wmem_range_ok(0x80001234u, 0x4000), "typical read in MEM1");
    CHECK(wmem_range_ok(0x90001235u, 3), "unaligned address in MEM2");
}

static void test_one_byte_outside(void)
{
    CHECK(!wmem_range_ok(WMEM_MEM1_BASE - 1, 1), "byte before MEM1");
    CHECK(!wmem_range_ok(MEM1_END, 1), "byte after MEM1");
    CHECK(!wmem_range_ok(WMEM_MEM2_BASE - 1, 1), "byte before MEM2");
    CHECK(!wmem_range_ok(MEM2_END, 1), "byte after MEM2");
}

static void test_range_must_not_poke_out_of_the_end(void)
{
    CHECK(!wmem_range_ok(MEM1_END - 1, 2), "starts inside MEM1, ends 1 byte past");
    CHECK(!wmem_range_ok(WMEM_MEM1_BASE, WMEM_MEM1_SIZE + 1), "MEM1 plus one byte");
    CHECK(!wmem_range_ok(MEM2_END - 4, 5), "MEM2 end, 1 over");
    CHECK(!wmem_range_ok(WMEM_MEM1_BASE - 1, 2), "starts 1 before MEM1, ends inside");
}

static void test_never_spans_two_regions(void)
{
    /* Even a range that starts in MEM1 and is long enough to reach MEM2's base
     * must be refused: the gap between them is not memory. */
    CHECK(!wmem_range_ok(WMEM_MEM1_BASE, WMEM_MEM2_BASE - WMEM_MEM1_BASE + 1),
          "range from MEM1 through the gap into MEM2");
    CHECK(!wmem_range_ok(MEM1_END, WMEM_MEM2_BASE - MEM1_END), "the gap itself");
}

static void test_zero_length_is_never_ok(void)
{
    CHECK(!wmem_range_ok(WMEM_MEM1_BASE, 0), "empty range inside MEM1");
    CHECK(!wmem_range_ok(0, 0), "empty range at 0");
}

static void test_wraparound_is_rejected(void)
{
    /* addr + len overflows 32 bits. With naive 32-bit math these would wrap to
     * small numbers and look valid. */
    CHECK(!wmem_range_ok(0xFFFFFFF0u, 0x20), "wraps past 2^32");
    CHECK(!wmem_range_ok(0xFFFFFFFFu, 2), "last address plus 2");
    CHECK(!wmem_range_ok(WMEM_MEM2_BASE, 0xFFFFFFFFu), "huge length from MEM2 base");
    CHECK(!wmem_range_ok(0x80000010u, 0xFFFFFFF0u), "length chosen to wrap into MEM1");
    CHECK(!wmem_range_ok(0xFFFFFFFFu, 0xFFFFFFFFu), "all ones");
}

static void test_forbidden_areas(void)
{
    CHECK(!wmem_range_ok(0x00000000u, 4), "address 0 (null)");
    CHECK(!wmem_range_ok(0x0C000000u, 4), "physical-style MEM1 address");
    CHECK(!wmem_range_ok(0xC0000000u, 4), "uncached MEM1 mirror");
    CHECK(!wmem_range_ok(0xD0000000u, 4), "uncached MEM2 mirror");
    CHECK(!wmem_range_ok(0xCC000000u, 4), "hardware registers");
    CHECK(!wmem_range_ok(0xCD000000u, 4), "Hollywood registers");
    CHECK(!wmem_range_ok(0xE0000000u, 4), "high memory");
}

int main(void)
{
    test_inside_each_region();
    test_one_byte_outside();
    test_range_must_not_poke_out_of_the_end();
    test_never_spans_two_regions();
    test_zero_length_is_never_ok();
    test_wraparound_is_rejected();
    test_forbidden_areas();
    printf("test_wmem: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
