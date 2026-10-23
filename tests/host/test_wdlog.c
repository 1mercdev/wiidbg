/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Host-side tests for common/wdlog.c. Each test documents one property of the
 * logging design that we promised in docs/logging.md.
 */
#include <stdio.h>
#include <string.h>
#include "../../common/wdlog.h"

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

/* A sink that records everything it receives, so tests can inspect it. */
#define CAP 256
static char     got_msg[CAP][WDLOG_MSG_MAX];
static uint8_t  got_level[CAP];
static uint8_t  got_ctx[CAP];
static unsigned got_n;

static void capture(const wdlog_record *r, void *user)
{
    (void)user;
    if (got_n < CAP) {
        memcpy(got_msg[got_n], r->msg, WDLOG_MSG_MAX);
        got_level[got_n] = r->level;
        got_ctx[got_n] = r->ctx;
        got_n++;
    }
}

/* A fake clock the tests control completely. */
static uint32_t fake_now;
static uint32_t fake_clock(void) { return fake_now; }

static void fresh(wdlog_level sink_level)
{
    got_n = 0;
    fake_now = 0;
    wdlog_init(fake_clock);
    wdlog_add_sink(capture, NULL, sink_level);
}

static void test_basic_order_and_format(void)
{
    fresh(WDLOG_TRACE);
    WDLOG_I(WDLOG_SUB_CORE, "hello %d", 1);
    WDLOG_I(WDLOG_SUB_CORE, "hello %s", "two");
    CHECK(wdlog_flush() == 2, "should deliver 2");
    CHECK(!strcmp(got_msg[0], "hello 1"), "got '%s'", got_msg[0]);
    CHECK(!strcmp(got_msg[1], "hello two"), "got '%s'", got_msg[1]);
}

static void test_sink_level_filtering(void)
{
    fresh(WDLOG_WARN);               /* only ERROR and WARN wanted */
    WDLOG_E(WDLOG_SUB_CORE, "e");
    WDLOG_W(WDLOG_SUB_CORE, "w");
    WDLOG_I(WDLOG_SUB_CORE, "i");    /* must be skipped before formatting */
    WDLOG_D(WDLOG_SUB_CORE, "d");
    wdlog_flush();
    CHECK(got_n == 2, "expected 2 records, got %u", got_n);
    CHECK(got_level[0] == WDLOG_ERROR && got_level[1] == WDLOG_WARN, "levels");
}

static void test_overflow_is_dropped_and_reported(void)
{
    fresh(WDLOG_TRACE);
    for (unsigned i = 0; i < WDLOG_RING_SLOTS + 10; i++)
        WDLOG_I(WDLOG_SUB_CORE, "m%u", i);
    CHECK(wdlog_dropped_total() == 10, "dropped=%u", wdlog_dropped_total());
    unsigned n = wdlog_flush();
    /* 64 kept + 1 synthetic "dropped" warning */
    CHECK(n == WDLOG_RING_SLOTS + 1, "delivered %u", n);
    CHECK(!strcmp(got_msg[0], "m0"), "oldest kept, newest dropped: '%s'", got_msg[0]);
    CHECK(strstr(got_msg[WDLOG_RING_SLOTS], "10 log record(s) dropped") != NULL,
          "drop notice: '%s'", got_msg[WDLOG_RING_SLOTS]);
    /* The notice must not repeat when nothing new was lost. */
    got_n = 0;
    WDLOG_I(WDLOG_SUB_CORE, "after");
    wdlog_flush();
    CHECK(got_n == 1, "no repeated drop notice, got %u records", got_n);
}

static void test_truncation_is_safe(void)
{
    fresh(WDLOG_TRACE);
    char big[300];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    wdlog_write(WDLOG_INFO, WDLOG_SUB_CORE, "%s", big);
    wdlog_flush();
    CHECK(got_n == 1, "one record");
    CHECK(strlen(got_msg[0]) == WDLOG_MSG_MAX - 1, "truncated len %zu", strlen(got_msg[0]));
}

static void test_exception_ring_merges_by_time(void)
{
    /* Each ring is internally time-ordered (a real timebase only moves
     * forward); flush() must interleave the TWO rings into one ordered stream.
     * The clock below is therefore monotonic across all four writes. */
    fresh(WDLOG_TRACE);
    fake_now = 100; WDLOG_I(WDLOG_SUB_CORE, "normal@100");
    fake_now = 105; wdlog_write_exc(WDLOG_INFO, WDLOG_SUB_DBG, "exc@105");
    fake_now = 108; WDLOG_I(WDLOG_SUB_CORE, "normal@108");
    fake_now = 110; wdlog_write_exc(WDLOG_INFO, WDLOG_SUB_DBG, "exc@110");
    wdlog_flush();
    CHECK(got_n == 4, "4 records");
    CHECK(!strcmp(got_msg[0], "normal@100"), "0:'%s'", got_msg[0]);
    CHECK(!strcmp(got_msg[1], "exc@105"),    "1:'%s'", got_msg[1]);
    CHECK(!strcmp(got_msg[2], "normal@108"), "2:'%s'", got_msg[2]);
    CHECK(!strcmp(got_msg[3], "exc@110"),    "3:'%s'", got_msg[3]);
    CHECK(got_ctx[1] == WDLOG_CTX_EXC && got_ctx[0] == WDLOG_CTX_NORMAL, "ctx tags");
}

static void test_merge_survives_clock_wraparound(void)
{
    fresh(WDLOG_TRACE);
    fake_now = 0xFFFFFFF0u; WDLOG_I(WDLOG_SUB_CORE, "before-wrap");
    fake_now = 0x00000010u; wdlog_write_exc(WDLOG_INFO, WDLOG_SUB_DBG, "after-wrap");
    wdlog_flush();
    CHECK(got_n == 2, "2 records");
    CHECK(!strcmp(got_msg[0], "before-wrap"), "0:'%s'", got_msg[0]);
    CHECK(!strcmp(got_msg[1], "after-wrap"),  "1:'%s'", got_msg[1]);
}

static void test_exc_write_truncates_and_handles_null(void)
{
    fresh(WDLOG_TRACE);
    char big[200];
    memset(big, 'y', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    wdlog_write_exc(WDLOG_ERROR, WDLOG_SUB_DBG, big);
    wdlog_write_exc(WDLOG_ERROR, WDLOG_SUB_DBG, NULL);   /* must not crash */
    wdlog_flush();
    CHECK(got_n == 1, "NULL message ignored, got %u", got_n);
    CHECK(strlen(got_msg[0]) == WDLOG_MSG_MAX - 1, "truncated");
}

static void test_sink_table_limit(void)
{
    fresh(WDLOG_TRACE);                    /* sink #1 */
    for (unsigned i = 1; i < WDLOG_MAX_SINKS; i++)
        CHECK(wdlog_add_sink(capture, NULL, WDLOG_INFO) == 0, "add %u", i);
    CHECK(wdlog_add_sink(capture, NULL, WDLOG_INFO) == -1, "table full");
    CHECK(wdlog_add_sink(NULL, NULL, WDLOG_INFO) == -1, "NULL fn rejected");
}

int main(void)
{
    test_basic_order_and_format();
    test_sink_level_filtering();
    test_overflow_is_dropped_and_reported();
    test_truncation_is_safe();
    test_exception_ring_merges_by_time();
    test_merge_survives_clock_wraparound();
    test_exc_write_truncates_and_handles_null();
    test_sink_table_limit();
    printf("test_wdlog: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
