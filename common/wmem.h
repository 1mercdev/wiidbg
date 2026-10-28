/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * wiidbg - Wii debugger
 * Copyright (C) 2026 1mercdev
 *
 * wmem: memory access policy and backend interface.
 *
 * TWO separate things live here, on purpose:
 *
 *   1. POLICY (wmem_range_ok): which addresses a client may touch. Pure logic,
 *      portable, tested exhaustively on the PC. The protocol layer calls it
 *      BEFORE any memory is accessed, so there is exactly one place that decides.
 *
 *   2. MECHANISM (wmem_ops): how bytes are actually read and written. Platform
 *      code: the Wii version copies memory and maintains the CPU caches; test
 *      versions use plain buffers. The backend may assume the range was already
 *      validated.
 *
 * Why not let the backend validate too? Two checks can disagree. One policy,
 * one place, one set of tests.
 */
#ifndef WMEM_H
#define WMEM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The regions a client may access: the Wii's two RAM areas, as seen through
 * the CPU's CACHED mirror (the 0x8xxxxxxx / 0x9xxxxxxx addresses programs use).
 *
 *   MEM1  0x80000000 .. 0x817FFFFF   24 MiB  fast 1T-SRAM
 *   MEM2  0x90000000 .. 0x93FFFFFF   64 MiB  GDDR3, bulk memory
 *
 * Deliberately NOT allowed (yet):
 *   - hardware registers (0xCC000000...): reading some of them has side effects,
 *     and an unmapped access would crash the stub.
 *   - the uncached mirrors (0xC0000000 / 0xD0000000).
 *   - anything else: unmapped addresses raise a CPU exception.
 */
#define WMEM_MEM1_BASE  0x80000000u
#define WMEM_MEM1_SIZE  0x01800000u
#define WMEM_MEM2_BASE  0x90000000u
#define WMEM_MEM2_SIZE  0x04000000u

/* Returns 1 if the whole range [addr, addr+len) lies inside ONE allowed region,
 * else 0. A range of length 0 is never "ok". Overflow past 0xFFFFFFFF is handled
 * (it is rejected, not wrapped). */
int wmem_range_ok(uint32_t addr, uint32_t len);

/* The platform backend. Both functions return 0 on success, negative on failure.
 * They are only ever called with a range that wmem_range_ok() accepted. */
typedef struct wmem_ops {
    int (*read)(uint32_t addr, void *dst, uint32_t len);
    int (*write)(uint32_t addr, const void *src, uint32_t len);
} wmem_ops;

#ifdef __cplusplus
}
#endif

#endif /* WMEM_H */
