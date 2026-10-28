/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * A fake RAM for the PC test server (hostserver): two plain buffers with the
 * real sizes of MEM1 and MEM2. It exists to test PROTOCOL logic (does a write
 * followed by a read return what was written, across chunk boundaries and
 * regions?). It does not imitate caches or any other hardware behaviour.
 */
#ifndef WMEM_FAKE_H
#define WMEM_FAKE_H

#include "../../common/wmem.h"

/* Allocate and seed the fake RAM. Returns 0 or -1. */
int wmem_fake_init(void);

extern const wmem_ops wmem_fake_ops;

#endif
