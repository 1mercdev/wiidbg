/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Wii-specific pieces of the logging system: the clock and the sinks.
 * The portable core (ring buffers, merging, filtering) lives in common/wdlog.c.
 */
#ifndef LOG_SINKS_H
#define LOG_SINKS_H

#include <stdint.h>
#include "wdlog.h"

/* PowerPC timebase, low 32 bits. Reads a CPU register: safe anywhere. */
uint32_t wii_timebase(void);

/* Print records on the TV console (libogc console, must be initialised). */
void sink_console(const wdlog_record *rec, void *user);

#endif
