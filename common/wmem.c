/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * wiidbg - Wii debugger
 * Copyright (C) 2026 1mercdev
 *
 * Memory access policy. See wmem.h.
 */
#include "wmem.h"

static int inside(uint64_t addr, uint64_t len, uint32_t base, uint32_t size)
{
    /* 64-bit arithmetic: addr + len cannot overflow, so a request like
     * "address 0xFFFFFFF0, length 0x20" is compared honestly instead of
     * wrapping around to a small number that looks valid. */
    return addr >= base && addr + len <= (uint64_t)base + size;
}

int wmem_range_ok(uint32_t addr, uint32_t len)
{
    if (len == 0)
        return 0;
    return inside(addr, len, WMEM_MEM1_BASE, WMEM_MEM1_SIZE) ||
           inside(addr, len, WMEM_MEM2_BASE, WMEM_MEM2_SIZE);
}
