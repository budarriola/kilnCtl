// Host tests for App/drivers/espInterfaces/esp_spi_owner.c -- specifically
// the bounded-timeout/fail-fast fix for DISPLAY_ST7796_PLAN.md section 9.9 /
// TODO.md: spi_owner_transfer() used to wait portMAX_DELAY on both the
// enqueue and the completion wait, which defeated every caller-side timeout
// above it if the owner task ever wedged (blocked forever inside
// spi_device_transmit()).
//
// #includes esp_spi_owner.c directly (same convention as
// test_owner_slot_pool.c/test_boot_guard.c) to reach spi_owner_t's `wedged`
// field. As that file's own header comment notes, this project's host-test
// FreeRTOS stubs never actually run a task (freertos/task.h's
// xTaskCreatePinnedToCore() stub never calls the function pointer it is
// given) and xQueueSend() always fails unless a test opts into the ring
// buffer (freertos/queue.h) -- so spi_owner_task() itself is never reached
// here. What IS exercised for real is spi_owner_transfer()'s caller-side
// logic: xQueueSend() failing immediately (the stub's default behavior)
// stands in for "the owner task did not accept this request within the
// timeout", which is exactly the wedge scenario the fix targets. The
// second, independent wait (xSemaphoreTake() on done_sem, guarding "the
// owner accepted the request but never finished it") cannot be reached
// without also faking a successful enqueue; g_stub_queue_send_calls below
// is enough to prove the more important invariant either way -- once wedged
// is latched, no further request is ever enqueued, which is precisely what
// keeps a wedged display flush from also corrupting an in-flight
// thermocouple transfer sharing the same owner.
#include "test_common.h"

// Test-visible queue-send counter consumed by stubs/freertos/queue.h's
// xQueueSend() stub. test_wifi_prov.c (linked into this same executable)
// already DEFINES this global -- declared extern here, not defined a
// second time, or the link fails with LNK2005 (found building this test).
extern int g_stub_queue_send_calls;

#include "../drivers/espInterfaces/esp_spi_owner.c"

#include <stdint.h>

static void test_wedge_latches_and_fails_fast(void)
{
    spi_owner_t owner;
    esp_err_t init_err = spi_owner_init(&owner, 0 /*host*/, 4 /*queue_len*/, 5 /*priority*/,
                                         2048 /*stack*/, -1 /*core*/);
    TEST_CHECK(init_err == ESP_OK, "spi_owner_init succeeds against the host stubs");
    TEST_CHECK(!owner.wedged, "a freshly-initialized owner is not wedged");

    uint8_t tx[4] = { 1, 2, 3, 4 };
    g_stub_queue_send_calls = 0;

    // xQueueSend() (stubs/freertos/queue.h) fails by default -- no test here
    // enables the ring buffer -- standing in for "the owner task's request
    // queue did not accept this within SPI_OWNER_TRANSFER_TIMEOUT_MS", i.e.
    // the owner is wedged on whatever it is currently doing.
    esp_err_t first =
        spi_owner_transfer(&owner, (spi_device_handle_t)0x1, tx, sizeof(tx), NULL, 0, /*cs_pin=*/5);
    TEST_CHECK(first == ESP_ERR_TIMEOUT, "first transfer past a wedged queue reports ESP_ERR_TIMEOUT");
    TEST_CHECK(owner.wedged, "that timeout latches owner.wedged");
    TEST_CHECK(g_stub_queue_send_calls == 1, "the first call actually reached xQueueSend() once");

    // THE LOAD-BEARING CHECK: once wedged, a second transfer must fail fast
    // -- ESP_ERR_INVALID_STATE, without ever touching the queue again. This
    // is what stops a wedged display flush from also corrupting a
    // thermocouple transfer queued behind it on the same owner: neither one
    // gets to sit blocked against a stuck task a second time.
    esp_err_t second =
        spi_owner_transfer(&owner, (spi_device_handle_t)0x1, tx, sizeof(tx), NULL, 0, /*cs_pin=*/5);
    TEST_CHECK(second == ESP_ERR_INVALID_STATE, "a transfer after wedging fails fast with ESP_ERR_INVALID_STATE");
    TEST_CHECK(g_stub_queue_send_calls == 1,
               "fail-fast means xQueueSend() was NOT called again -- still 1 total");
}

void run_test_esp_spi_owner(void)
{
    TEST_SECTION("esp_spi_owner");
    test_wedge_latches_and_fails_fast();
}
