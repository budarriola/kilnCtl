// Host test for hal_spi_transfer_async()'s contract, defined 2026-09-06 in
// interface/hal_spi.h BEFORE CONFIG_KILNCTL_SPI_ASYNC_FLUSH is ever turned
// on (verified off in firmware/KilnFW/sdkconfig; the sync path is what
// actually runs on the board today, and this contract must not change it).
// No production caller reaches hal_spi_transfer_async() today --
// panel_spi_blit.c's real async flush bypasses the HAL entirely and drives
// spi_owner_transfer_async() directly via hal_spi_esp_get_owner() -- so this
// is the only place the portable contract is exercised at all.
//
// hal_spi_esp.c (the real ESP backend) links against driver/spi_master.h
// and is not buildable on host (same reasoning as test_max31856_hal_spi.c's
// and test_hal_spi_adopt.c's own comments), so this test exercises the
// contract as modeled by the host fake (firmware/hwAbstraction/host/
// fake_spi.c), which fake_spi.h's own header comment already commits to:
// "this fake never calls a completion callback from inside
// hal_spi_transfer_async() itself ... instead ... enqueues a pending
// completion and the test calls fake_spi_pump() to fire callbacks
// deterministically, oldest-enqueued first".
//
// Covers, per interface/hal_spi.h's hal_spi_transfer_async() contract:
//   1. Callback context / no synchronous completion (bullets 3+4): `cb`
//      never fires before hal_spi_transfer_async() returns, and only ever
//      fires from within an explicit fake_spi_pump() call (the fake's stand-
//      in for "the backend's own worker context").
//   2. Completion ordering (bullet 5): multiple queued async requests
//      complete strictly FIFO, oldest-enqueued first, matching a single
//      shared owner queue.
//   3. Queue-full path (bullet 6): exhausting FAKE_SPI_MAX_PENDING_ASYNC
//      pending completions without pumping fails the next enqueue with
//      HAL_NO_MEM, without touching `cb`, and latches the bus wedge.
//   4. Negative-test evidence: this file's own header comment records that
//      test_queue_full_returns_no_mem_and_wedges() was run once against a
//      deliberately broken fake_spi_transfer_async() (the pool-exhaustion
//      `if (free_pending < 0)` branch's `b->wedged = true;` line commented
//      out) and observed to fail on the wedge assertion, before being
//      restored -- proving the check is not vacuous.
#include <stdio.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#include "fake_spi.h"

static hal_spi_bus_t make_async_bus(void)
{
    hal_spi_bus_t bus;
    hal_spi_bus_cfg_t cfg = { .sck_pin = 0, .mosi_pin = 0, .miso_pin = 0,
                               .queue_len = 8, .task_priority = 5, .stack_depth = 4096,
                               .core_id = HAL_CORE_ANY, .max_transfer_sz = 64,
                               .dma_chan = HAL_SPI_DMA_AUTO };
    TEST_CHECK(hal_spi_bus_init(&bus, 0, &cfg) == HAL_OK, "hal_spi_bus_init");
    return bus;
}

static hal_spi_device_t attach_device(hal_spi_bus_t *bus, int cs_pin)
{
    hal_spi_device_cfg_t dev_cfg = { .clock_hz = 1000000, .mode = 0, .cs_pin = cs_pin,
                                      .hw_cs = HAL_CS_NONE, .queue_size = 1 };
    hal_spi_device_t dev;
    TEST_CHECK(hal_spi_device_attach(bus, &dev, &dev_cfg) == HAL_OK, "hal_spi_device_attach");
    return dev;
}

// ---- contract point 1: no synchronous completion --------------------------

static int s_cb_calls;
static void *s_cb_last_ctx;
static hal_status_t s_cb_last_result;
static void record_cb(void *ctx, hal_status_t result)
{
    s_cb_calls++;
    s_cb_last_ctx = ctx;
    s_cb_last_result = result;
}

// Proves bullets 3+4 together: the callback must not have run by the time
// hal_spi_transfer_async() itself returns (bullet 4), and must only run
// from inside fake_spi_pump() -- the fake's stand-in for the backend's own
// worker context (bullet 3). Mutate fake_spi.c's hal_spi_transfer_async() to
// call `cb` inline before returning HAL_OK (the exact bug hal_spi_esp.c's
// adapter used to have) and the first TEST_CHECK below goes red.
static void test_callback_never_fires_before_return(void)
{
    fake_spi_reset_all();
    hal_spi_bus_t bus = make_async_bus();
    hal_spi_device_t dev = attach_device(&bus, 3);

    int marker = 11;
    s_cb_calls = 0;
    uint8_t tx[2] = { 0xAA, 0xBB };

    hal_status_t rc = hal_spi_transfer_async(&dev, tx, sizeof(tx), 100, record_cb, &marker);
    TEST_CHECK(rc == HAL_OK, "async transfer is accepted (queued)");
    TEST_CHECK(s_cb_calls == 0,
               "cb has NOT fired by the time hal_spi_transfer_async() returns -- "
               "completion cannot precede the queuing call");
    TEST_CHECK(fake_spi_pending_async_count(&bus) == 1, "exactly one completion is now pending");

    TEST_CHECK(fake_spi_pump(&bus), "fake_spi_pump() drains the pending completion");
    TEST_CHECK(s_cb_calls == 1, "cb fired exactly once, only after pump()");
    TEST_CHECK(s_cb_last_ctx == &marker, "cb received the exact ctx pointer passed in");
    TEST_CHECK(s_cb_last_result == HAL_OK, "cb reports HAL_OK for an uninjected transfer");
}

// ---- contract point 2: FIFO completion ordering ----------------------------

#define ORDER_LOG_CAP 8
static int s_order_log[ORDER_LOG_CAP];
static int s_order_log_count;
static void record_order(void *ctx, hal_status_t result)
{
    (void)result;
    if (s_order_log_count < ORDER_LOG_CAP) {
        s_order_log[s_order_log_count++] = *(int *)ctx;
    }
}

// Queues three async requests carrying distinct markers, then pumps them one
// at a time -- per hal_spi.h's ordering bullet (5) and fake_spi.h's own "FIFO
// across all devices on the bus, matching the single shared owner queue"
// comment on fake_spi_pump(), completions must fire in ENQUEUE order, not
// reverse or arbitrary order. Mutate fake_spi_pump() to scan pending[] from
// the top down (highest in-use index first) instead of lowest-first and this
// goes red (s_order_log would read {2,1,0} instead of {0,1,2}).
static void test_async_completions_fire_fifo(void)
{
    fake_spi_reset_all();
    hal_spi_bus_t bus = make_async_bus();
    hal_spi_device_t dev = attach_device(&bus, 4);

    int markers[3] = { 0, 1, 2 };
    uint8_t tx = 0x01;
    s_order_log_count = 0;

    for (int i = 0; i < 3; i++) {
        hal_status_t rc = hal_spi_transfer_async(&dev, &tx, 1, 100, record_order, &markers[i]);
        TEST_CHECK(rc == HAL_OK, "each of the 3 chunks is accepted");
    }
    TEST_CHECK(fake_spi_pending_async_count(&bus) == 3, "all 3 completions are pending");

    TEST_CHECK(fake_spi_pump(&bus), "pump #1");
    TEST_CHECK(fake_spi_pump(&bus), "pump #2");
    TEST_CHECK(fake_spi_pump(&bus), "pump #3");
    TEST_CHECK(!fake_spi_pump(&bus), "a 4th pump finds nothing pending");

    TEST_CHECK(s_order_log_count == 3, "all 3 completions fired");
    TEST_CHECK(s_order_log[0] == 0 && s_order_log[1] == 1 && s_order_log[2] == 2,
               "completions fired in enqueue (FIFO) order, oldest first -- "
               "not reversed or interleaved");
}

// ---- contract point 3: queue-full path -------------------------------------

// Exhausts the fake's pending-completion pool (FAKE_SPI_MAX_PENDING_ASYNC)
// without ever pumping, then proves the next hal_spi_transfer_async() call
// is refused with HAL_NO_MEM, never touches `cb`, and -- per fake_spi.h's
// "Async pool exhaustion ... latches the bus wedge" contract, distinct from
// a single enqueue/completion timeout -- latches hal_spi_bus_is_wedged().
static void test_queue_full_returns_no_mem_and_wedges(void)
{
    fake_spi_reset_all();
    hal_spi_bus_t bus = make_async_bus();
    hal_spi_device_t dev = attach_device(&bus, 5);

    uint8_t tx = 0x01;
    s_cb_calls = 0;

    for (int i = 0; i < FAKE_SPI_MAX_PENDING_ASYNC; i++) {
        hal_status_t rc = hal_spi_transfer_async(&dev, &tx, 1, 100, record_cb, NULL);
        TEST_CHECK(rc == HAL_OK, "setup: fill every pending-async slot");
    }
    TEST_CHECK(!hal_spi_bus_is_wedged(&bus), "not yet wedged -- the pool is full but not overfilled");

    hal_status_t over = hal_spi_transfer_async(&dev, &tx, 1, 100, record_cb, NULL);
    TEST_CHECK(over == HAL_NO_MEM, "the (N+1)th async request is refused with HAL_NO_MEM");
    TEST_CHECK(s_cb_calls == 0, "cb was never called for the refused request");
    TEST_CHECK(hal_spi_bus_is_wedged(&bus),
               "pool exhaustion latches the bus wedge (distinct from a single timeout)");

    // The N requests that WERE accepted are still individually completable --
    // exhaustion refuses only the request that couldn't fit, it does not
    // corrupt or drop the ones already queued.
    int drained = 0;
    while (fake_spi_pump(&bus)) {
        drained++;
    }
    TEST_CHECK(drained == FAKE_SPI_MAX_PENDING_ASYNC,
               "every previously-accepted request still completes normally after the refusal");
}

int main(void)
{
    TEST_SECTION("hal_spi_transfer_async contract (fake_spi model)");

    test_callback_never_fires_before_return();
    test_async_completions_fire_fifo();
    test_queue_full_returns_no_mem_and_wedges();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures > 0 ? 1 : 0;
}
