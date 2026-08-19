// Host tests for tx_watermark.c -- the pure admit/drop decision behind
// log_task.c's TX-reserve watermark (ROADMAP.md M5: "TX ring reserves
// capacity for telemetry; log frames dropped above the watermark and the
// drops counted"). No FreeRTOS, no pico-sdk, no uart_owner -- see
// tx_watermark.h's own header comment for why this piece is host-testable
// when log_task.c/uart_owner.c are not.
#include "test_common.h"
#include "../src/tasks/tx_watermark.h"

static void test_below_watermark_admits(void)
{
    TEST_SECTION("tx_watermark: below the reserve fraction, log frames are admitted");

    TEST_CHECK(!tx_watermark_should_drop_log(0.0f, 0.5f), "empty ring admits");
    TEST_CHECK(!tx_watermark_should_drop_log(0.25f, 0.5f), "quarter-full ring admits");
    TEST_CHECK(!tx_watermark_should_drop_log(0.4999f, 0.5f), "just under the watermark admits");
}

static void test_at_or_above_watermark_drops(void)
{
    TEST_SECTION("tx_watermark: at or above the reserve fraction, log frames are dropped");

    TEST_CHECK(tx_watermark_should_drop_log(0.5f, 0.5f), "exactly at the watermark drops");
    TEST_CHECK(tx_watermark_should_drop_log(0.75f, 0.5f), "three-quarters-full ring drops");
    TEST_CHECK(tx_watermark_should_drop_log(1.0f, 0.5f), "full ring drops");
}

static void test_reserve_fraction_boundaries(void)
{
    TEST_SECTION("tx_watermark: reserve fraction of 0.0 or 1.0 are honored exactly");

    // A 0.0 reserve fraction means "reserve everything for telemetry" -- log
    // frames are dropped at any non-negative fill, including empty.
    TEST_CHECK(tx_watermark_should_drop_log(0.0f, 0.0f), "zero reserve drops even an empty ring");

    // A 1.0 reserve fraction means "reserve nothing" -- log frames are
    // admitted right up to (but not including) completely full.
    TEST_CHECK(!tx_watermark_should_drop_log(0.9999f, 1.0f), "near-full ring still admits under a 1.0 reserve");
    TEST_CHECK(tx_watermark_should_drop_log(1.0f, 1.0f), "completely full ring drops even under a 1.0 reserve");
}

static void test_production_constant(void)
{
    TEST_SECTION("tx_watermark: log_task.c's actual LOG_TX_RESERVE_FRACTION (0.5)");

    // Mirrors log_task.c's LOG_TX_RESERVE_FRACTION without including
    // log_task.c itself (FreeRTOS-dependent, not host-buildable) -- if that
    // constant ever changes, this test's literal must be updated alongside
    // it, same as test_kilnlink_power.c mirrors link_task_send_power()'s
    // flag logic without linking link_task.c.
    const float reserve = 0.5f;

    TEST_CHECK(!tx_watermark_should_drop_log(0.10f, reserve), "light telemetry-only traffic admits logs");
    TEST_CHECK(tx_watermark_should_drop_log(0.60f, reserve), "a log burst past half-full is refused");
}

void run_test_tx_watermark(void)
{
    test_below_watermark_admits();
    test_at_or_above_watermark_drops();
    test_reserve_fraction_boundaries();
    test_production_constant();
}
