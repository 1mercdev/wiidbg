/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>
#include "log_sinks.h"

uint32_t wii_timebase(void)
{
    /* "mftb" = Move From Time Base. The timebase is a free-running counter
     * the CPU increments by itself (about 60.75 MHz on the Wii), so reading it
     * needs no libc, no IOS and no locks: exactly what a clock hook that may
     * be called from an exception handler needs. We only keep the low 32 bits
     * (it wraps about every 70 s); wdlog's merge logic is wraparound-safe. */
    uint32_t tbl;
    __asm__ __volatile__("mftb %0" : "=r"(tbl));
    return tbl;
}

void sink_console(const wdlog_record *rec, void *user)
{
    (void)user;
    /* The console sink is allowed to be slow and to use printf, because
     * wdlog_flush() only calls sinks from the main loop. */
    printf("[%-5s %-5s] %s\n", wdlog_level_name(rec->level),
           wdlog_subsys_name(rec->subsys), rec->msg);
}
