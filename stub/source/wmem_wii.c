/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * wiidbg - Wii debugger
 * Copyright (C) 2026 1mercdev
 *
 * Wii memory backend: the code that really touches RAM.
 *
 * Both functions are only ever called with a range that wmem_range_ok() already
 * accepted (common/wmem.c), so they do not validate it again.
 *
 * WHY CACHES MATTER HERE
 * ----------------------
 * The CPU does not read RAM directly. It goes through two small, fast caches:
 * a data cache (for data) and an instruction cache (for code being executed).
 *
 *   - A normal store lands in the DATA cache first and reaches RAM later. Anything
 *     that reads RAM without going through the CPU (the GPU, DMA, the IOS
 *     coprocessor) can miss it. DCFlushRange() pushes modified cache lines out to
 *     RAM so they cannot.
 *   - The instruction cache is not updated by stores at all. If we patch code, the
 *     CPU may keep executing its old cached copy. ICInvalidateRange() drops the
 *     cached copy so the new instructions are fetched.
 *
 * Writing "copy, flush, invalidate" is the standard idiom for patching memory on
 * the Wii. Doing all three is harmless for plain data and essential for code.
 *
 * READS go through the CPU's normal cached view, i.e. they return exactly what the
 * running program itself would see. (Seeing the raw RAM contents, without the
 * cache, would need the uncached mirror addresses, which are not allowed yet.)
 *
 * The function signatures were checked against libogc's cache.h documentation:
 *   void DCFlushRange(void *startaddress, u32 len);
 *   void ICInvalidateRange(void *startaddress, u32 len);
 * Dolphin is lenient about missing cache maintenance, so a mistake here would
 * only show on real hardware.
 */
#include <gccore.h>
#include <string.h>

#include "wmem_wii.h"

static int wii_read(uint32_t addr, void *dst, uint32_t len)
{
    memcpy(dst, (const void *)addr, len);
    return 0;
}

static int wii_write(uint32_t addr, const void *src, uint32_t len)
{
    void *p = (void *)addr;

    memcpy(p, src, len);
    DCFlushRange(p, len);          /* data cache -> RAM */
    ICInvalidateRange(p, len);     /* forget any stale copy of this as code */
    return 0;
}

const wmem_ops wmem_wii_ops = { wii_read, wii_write };
