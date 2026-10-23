/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * wiidbg - Wii debugger
 * Copyright (C) 2026 1mercdev
 *
 * wdlog implementation. Read wdlog.h first.
 *
 * CONCURRENCY MODEL (the part worth understanding)
 * ------------------------------------------------
 * The Wii's PowerPC core "Broadway" is a SINGLE core. There are no threads
 * running truly in parallel on it, so we do not need atomic read-modify-write
 * instructions or memory barriers for ordering between cores. The only
 * "concurrency" is an exception handler (DSI, trace, ...) interrupting the
 * main code at an arbitrary instruction.
 *
 * That gives one rule: a data structure may have at most ONE writer per
 * variable. We obey it by giving each context its own ring:
 *
 *      normal code  --writes-->  ring[NORMAL]  --\
 *                                                  >-- wdlog_flush() --> sinks
 *      exception    --writes-->  ring[EXC]     --/
 *
 * Inside one ring it is a classic single-producer/single-consumer queue:
 * only the producer writes `head`, only the consumer writes `tail`. If an
 * exception fires while normal code is mid-write, it touches the OTHER ring,
 * so nothing is corrupted. We still need a *compiler* barrier so the compiler
 * does not reorder "fill slot" after "publish head".
 *
 * (If wiidbg ever targets a multicore platform this reasoning no longer holds
 *  and real memory barriers would be required. Single-core is a design
 *  assumption, stated here on purpose.)
 */
#include "wdlog.h"

#include <stdarg.h>
#include <stdio.h>

#define COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")

typedef struct {
    wdlog_record      slot[WDLOG_RING_SLOTS];
    volatile uint32_t head;     /* written by the producer only */
    volatile uint32_t tail;     /* written by the consumer only */
    volatile uint32_t dropped;  /* written by the producer only */
} wdlog_ring;

typedef struct {
    wdlog_sink_fn fn;
    void         *user;
    wdlog_level   max_level;
} wdlog_sink;

static wdlog_ring     g_ring[2];   /* indexed by wdlog_ctx */
static wdlog_sink     g_sink[WDLOG_MAX_SINKS];
static unsigned       g_nsinks;
static wdlog_level    g_max_level;     /* most verbose level any sink wants */
static wdlog_clock_fn g_clock;
static uint32_t       g_reported_drops;

_Static_assert((WDLOG_RING_SLOTS & (WDLOG_RING_SLOTS - 1)) == 0,
               "WDLOG_RING_SLOTS must be a power of two (we use & as modulo)");

void wdlog_init(wdlog_clock_fn clock)
{
    for (unsigned r = 0; r < 2; r++) {
        g_ring[r].head = g_ring[r].tail = g_ring[r].dropped = 0;
    }
    g_nsinks = 0;
    g_max_level = WDLOG_ERROR;
    g_clock = clock;
    g_reported_drops = 0;
}

int wdlog_add_sink(wdlog_sink_fn fn, void *user, wdlog_level max_level)
{
    if (!fn || g_nsinks >= WDLOG_MAX_SINKS)
        return -1;
    g_sink[g_nsinks].fn = fn;
    g_sink[g_nsinks].user = user;
    g_sink[g_nsinks].max_level = max_level;
    g_nsinks++;
    if (max_level > g_max_level)
        g_max_level = max_level;
    return 0;
}

/* Reserve the next slot of a ring, or NULL if it is full. */
static wdlog_record *ring_reserve(wdlog_ring *r)
{
    if (r->head - r->tail >= WDLOG_RING_SLOTS) {
        r->dropped++;      /* never block a producer: count the loss instead */
        return NULL;
    }
    return &r->slot[r->head & (WDLOG_RING_SLOTS - 1)];
}

/* Make the filled slot visible to the consumer. */
static void ring_publish(wdlog_ring *r)
{
    COMPILER_BARRIER();    /* slot contents must be complete before head moves */
    r->head = r->head + 1;
}

void wdlog_write(wdlog_level lvl, wdlog_subsys sub, const char *fmt, ...)
{
    /* Cheap early-out: if no sink wants this level, skip the formatting. */
    if (lvl > g_max_level)
        return;

    wdlog_ring *r = &g_ring[WDLOG_CTX_NORMAL];
    wdlog_record *rec = ring_reserve(r);
    if (!rec)
        return;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(rec->msg, WDLOG_MSG_MAX, fmt, ap);
    va_end(ap);
    if (n < 0)
        n = 0;
    if ((unsigned)n >= WDLOG_MSG_MAX)
        n = WDLOG_MSG_MAX - 1;          /* truncated by vsnprintf */

    rec->stamp  = g_clock ? g_clock() : 0;
    rec->level  = (uint8_t)lvl;
    rec->subsys = (uint8_t)sub;
    rec->ctx    = WDLOG_CTX_NORMAL;
    rec->len    = (uint8_t)n;
    ring_publish(r);
}

void wdlog_write_exc(wdlog_level lvl, wdlog_subsys sub, const char *msg)
{
    if (lvl > g_max_level || !msg)
        return;

    wdlog_ring *r = &g_ring[WDLOG_CTX_EXC];
    wdlog_record *rec = ring_reserve(r);
    if (!rec)
        return;

    /* Hand-rolled copy: no memcpy/strlen/printf, so nothing in libc (locks,
     * malloc, reentrancy structs) can bite us inside an exception handler. */
    unsigned i = 0;
    while (msg[i] && i < WDLOG_MSG_MAX - 1) {
        rec->msg[i] = msg[i];
        i++;
    }
    rec->msg[i] = '\0';

    rec->stamp  = g_clock ? g_clock() : 0;
    rec->level  = (uint8_t)lvl;
    rec->subsys = (uint8_t)sub;
    rec->ctx    = WDLOG_CTX_EXC;
    rec->len    = (uint8_t)i;
    ring_publish(r);
}

static void dispatch(const wdlog_record *rec)
{
    for (unsigned i = 0; i < g_nsinks; i++) {
        if (rec->level <= g_sink[i].max_level)
            g_sink[i].fn(rec, g_sink[i].user);
    }
}

/* True if stamp a is at-or-before stamp b, tolerating 32-bit wraparound.
 * The Wii timebase wraps roughly every minute, so a plain a<=b would
 * misorder records around the wrap. Subtracting then reading the result as
 * SIGNED gives the right answer as long as the two stamps are less than
 * half the counter range (~35 s) apart, which holds because we flush often. */
static int stamp_not_after(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) <= 0;
}

unsigned wdlog_flush(void)
{
    unsigned delivered = 0;

    for (;;) {
        wdlog_ring *n = &g_ring[WDLOG_CTX_NORMAL];
        wdlog_ring *e = &g_ring[WDLOG_CTX_EXC];
        int have_n = n->head != n->tail;
        int have_e = e->head != e->tail;
        if (!have_n && !have_e)
            break;

        /* Merge the two rings into one time-ordered stream. */
        wdlog_ring *pick;
        if (have_n && have_e) {
            const wdlog_record *rn = &n->slot[n->tail & (WDLOG_RING_SLOTS - 1)];
            const wdlog_record *re = &e->slot[e->tail & (WDLOG_RING_SLOTS - 1)];
            pick = stamp_not_after(re->stamp, rn->stamp) ? e : n;
        } else {
            pick = have_e ? e : n;
        }

        dispatch(&pick->slot[pick->tail & (WDLOG_RING_SLOTS - 1)]);
        COMPILER_BARRIER();     /* finish reading the slot before freeing it */
        pick->tail = pick->tail + 1;
        delivered++;
    }

    /* Loss must be visible, never silent. Report new drops as a synthetic
     * record so every sink (and, later, the PC) learns about it. */
    uint32_t total = wdlog_dropped_total();
    if (total != g_reported_drops) {
        wdlog_record rec;
        int len = snprintf(rec.msg, WDLOG_MSG_MAX,
                           "%u log record(s) dropped (ring full)",
                           (unsigned)(total - g_reported_drops));
        rec.stamp  = g_clock ? g_clock() : 0;
        rec.level  = WDLOG_WARN;
        rec.subsys = WDLOG_SUB_CORE;
        rec.ctx    = WDLOG_CTX_NORMAL;
        rec.len    = (uint8_t)(len < 0 ? 0 : len);
        g_reported_drops = total;
        dispatch(&rec);
        delivered++;
    }
    return delivered;
}

uint32_t wdlog_dropped_total(void)
{
    return g_ring[0].dropped + g_ring[1].dropped;
}

const char *wdlog_level_name(unsigned level)
{
    static const char *const names[] = { "ERROR", "WARN", "INFO", "DEBUG", "TRACE" };
    return level < 5 ? names[level] : "?";
}

const char *wdlog_subsys_name(unsigned subsys)
{
    static const char *const names[] = { "core", "net", "proto", "mem", "dbg" };
    return subsys < 5 ? names[subsys] : "?";
}
