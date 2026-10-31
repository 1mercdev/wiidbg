/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * wiidbg - Wii debugger
 * Copyright (C) 2026 1mercdev
 *
 * wdlog: the logging core. Portable C (host-testable), no libogc.
 * Design rationale: docs/logging.md and docs/adr/0003-logging-architecture.md
 *
 * The one idea to remember:  PRODUCERS NEVER TOUCH A DEVICE.
 *   Code that logs only copies a small record into a RAM ring buffer.
 *   Later, from the main loop, wdlog_flush() hands the records to SINKS
 *   (screen, network, a file, ...). That separation is what makes it safe
 *   to log from places where printing or networking would hang the console,
 *   such as an exception handler.
 */
#ifndef WDLOG_H
#define WDLOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WDLOG_MSG_MAX     80u  /* bytes per message, incl. terminator */
#define WDLOG_RING_SLOTS  64u  /* per ring; MUST be a power of two    */
#define WDLOG_MAX_SINKS   4u

/* Lower number = more severe. A sink with max_level INFO receives
 * ERROR, WARN and INFO, but not DEBUG or TRACE. */
typedef enum {
    WDLOG_ERROR = 0,
    WDLOG_WARN,
    WDLOG_INFO,
    WDLOG_DEBUG,
    WDLOG_TRACE
} wdlog_level;

/* Which part of the program spoke. Lets sinks/clients filter later. */
typedef enum {
    WDLOG_SUB_CORE = 0,
    WDLOG_SUB_NET,
    WDLOG_SUB_PROTO,
    WDLOG_SUB_MEM,
    WDLOG_SUB_DBG
} wdlog_subsys;

/* Where the record was produced. Normal code and exception handlers use
 * separate rings (see wdlog.c for why). */
typedef enum { WDLOG_CTX_NORMAL = 0, WDLOG_CTX_EXC = 1 } wdlog_ctx;

typedef struct wdlog_record {
    uint32_t stamp;                /* from the clock hook; orders the rings */
    uint8_t  level;
    uint8_t  subsys;
    uint8_t  ctx;
    uint8_t  len;                  /* strlen(msg) */
    char     msg[WDLOG_MSG_MAX];
} wdlog_record;

typedef void (*wdlog_sink_fn)(const wdlog_record *rec, void *user);

/* Returns a free-running 32-bit tick counter (on the Wii: the PowerPC
 * timebase). May be NULL, then all stamps are 0 and cross-ring order is
 * only approximate. Must be safe to call from an exception handler. */
typedef uint32_t (*wdlog_clock_fn)(void);

/* Reset all state and install the clock hook. */
void wdlog_init(wdlog_clock_fn clock);

/* Register a sink. Returns 0 on success, -1 if the table is full. */
int wdlog_add_sink(wdlog_sink_fn fn, void *user, wdlog_level max_level);

/* NORMAL-CONTEXT logging (printf-style). Uses vsnprintf, which is NOT
 * safe inside an exception handler. Never blocks: if the ring is full the
 * record is dropped and counted. */
void wdlog_write(wdlog_level lvl, wdlog_subsys sub, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

/* EXCEPTION-CONTEXT logging. Takes a plain string, uses no libc, writes to
 * a separate ring, and is safe to call from DSI/trace/breakpoint handlers.
 * Do structured data through the event ring, not through text. */
void wdlog_write_exc(wdlog_level lvl, wdlog_subsys sub, const char *msg);

/* Deliver pending records to the sinks, oldest first. Call from the main
 * loop ONLY (sinks may block or use IOS). Returns records delivered. */
unsigned wdlog_flush(void);

/* Records lost to full rings since init (running total). */
uint32_t wdlog_dropped_total(void);

/* Convenience macros. WDLOG_COMPILE_LEVEL strips lower-priority calls out of
 * the binary entirely: build with -DWDLOG_COMPILE_LEVEL=2 for INFO and
 * above only. Note: arguments of stripped calls are not evaluated. */
#ifndef WDLOG_COMPILE_LEVEL
#define WDLOG_COMPILE_LEVEL WDLOG_TRACE
#endif

#define WDLOG_E(sub, ...) \
    do { if (WDLOG_ERROR <= WDLOG_COMPILE_LEVEL) wdlog_write(WDLOG_ERROR, sub, __VA_ARGS__); } while (0)
#define WDLOG_W(sub, ...) \
    do { if (WDLOG_WARN  <= WDLOG_COMPILE_LEVEL) wdlog_write(WDLOG_WARN,  sub, __VA_ARGS__); } while (0)
#define WDLOG_I(sub, ...) \
    do { if (WDLOG_INFO  <= WDLOG_COMPILE_LEVEL) wdlog_write(WDLOG_INFO,  sub, __VA_ARGS__); } while (0)
#define WDLOG_D(sub, ...) \
    do { if (WDLOG_DEBUG <= WDLOG_COMPILE_LEVEL) wdlog_write(WDLOG_DEBUG, sub, __VA_ARGS__); } while (0)
#define WDLOG_T(sub, ...) \
    do { if (WDLOG_TRACE <= WDLOG_COMPILE_LEVEL) wdlog_write(WDLOG_TRACE, sub, __VA_ARGS__); } while (0)

/* Short names for sinks to print. */
const char *wdlog_level_name(unsigned level);
const char *wdlog_subsys_name(unsigned subsys);

#ifdef __cplusplus
}
#endif

#endif /* WDLOG_H */
