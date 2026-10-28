/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Fake RAM for hostserver. See wmem_fake.h.
 */
#include "wmem_fake.h"

#include <stdlib.h>
#include <string.h>

static uint8_t *mem1, *mem2;

/* Seed visible to a human trying the CLI by hand:
 *     python3 -m wiidbg 127.0.0.1:<port> read 0x80000100 32
 * The rest of both regions starts as zeros. */
#define SEED_ADDR 0x80000100u
static const char seed_text[] = "wiidbg host fake RAM";

int wmem_fake_init(void)
{
    /* calloc of a large block normally maps zero pages lazily, so this costs
     * almost nothing until a test really touches the memory. */
    mem1 = calloc(1, WMEM_MEM1_SIZE);
    mem2 = calloc(1, WMEM_MEM2_SIZE);
    if (!mem1 || !mem2)
        return -1;
    memcpy(mem1 + (SEED_ADDR - WMEM_MEM1_BASE), seed_text, sizeof seed_text);
    return 0;
}

/* Map a guest address to a host pointer. The protocol layer has already
 * validated the range, so a failure here means THAT layer has a bug: we report
 * it (the client then sees ERR_INTERNAL) instead of reading out of bounds. */
static uint8_t *xlate(uint32_t addr, uint32_t len)
{
    uint64_t end = (uint64_t)addr + len;
    if (addr >= WMEM_MEM1_BASE && end <= (uint64_t)WMEM_MEM1_BASE + WMEM_MEM1_SIZE)
        return mem1 + (addr - WMEM_MEM1_BASE);
    if (addr >= WMEM_MEM2_BASE && end <= (uint64_t)WMEM_MEM2_BASE + WMEM_MEM2_SIZE)
        return mem2 + (addr - WMEM_MEM2_BASE);
    return NULL;
}

static int fake_read(uint32_t addr, void *dst, uint32_t len)
{
    uint8_t *p = xlate(addr, len);
    if (!p)
        return -1;
    memcpy(dst, p, len);
    return 0;
}

static int fake_write(uint32_t addr, const void *src, uint32_t len)
{
    uint8_t *p = xlate(addr, len);
    if (!p)
        return -1;
    memcpy(p, src, len);
    return 0;
}

const wmem_ops wmem_fake_ops = { fake_read, fake_write };
