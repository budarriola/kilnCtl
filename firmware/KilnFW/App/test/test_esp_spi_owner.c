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

// Ring-buffer opt-in for stubs/freertos/queue.h's xQueueSend()/xQueueReceive()
// -- test_uart_log_bridge.c (also linked into this same executable) DEFINES
// this global; declared extern here, not defined a second time. Used below to
// reach spi_owner_transfer()'s SECOND timeout branch (completion wait, not
// enqueue), which needs xQueueSend() to actually succeed.
extern int g_stub_queue_ring_enabled;

#include "../drivers/espInterfaces/esp_spi_owner.c"

#include <stdint.h>

// Defined here (declared extern in stubs/driver/spi_master.h), same
// convention as g_stub_queue_send_calls -- DISPLAY_ST7796_PLAN.md 9.5's
// dispatch tests below need to tell spi_device_transmit() and
// spi_device_polling_transmit() apart.
int g_stub_spi_transmit_calls = 0;
int g_stub_spi_polling_transmit_calls = 0;
unsigned int g_stub_spi_transmit_last_flags = 0;
unsigned int g_stub_spi_polling_transmit_last_flags = 0;
// g_stub_gpio_set_level_calls itself is `static`, defined directly in
// stubs/driver/gpio.h -- see that header's comment for why (unlike the two
// counters above, no cross-TU extern/definition split here).

static void test_wedge_latches_and_fails_fast(void)
{
    spi_owner_t owner;
    esp_err_t init_err = spi_owner_init(&owner, 0 /*host*/, 4 /*queue_len*/, 5 /*priority*/,
                                         2048 /*stack*/, -1 /*core*/,
                                         /*dma_use_psram=*/false,
                                         /*async_flush=*/false);
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
    // BRANCH DISCRIMINATOR (opus review, commit 9fc55d9, M2): this test's own
    // header comment already explained that without g_stub_queue_ring_enabled
    // pinned to 0, a leaked-on ring mode makes xQueueSend() succeed instead
    // of failing, silently diverting this test into the SECOND (completion-
    // wait) timeout branch below -- which also returns ESP_ERR_TIMEOUT, also
    // latches wedged, and also leaves g_stub_queue_send_calls == 1, so every
    // check above this line still passes while the enqueue-failure branch
    // this test is named for never actually ran. The two branches leave
    // different refcounts behind (enqueue failure releases BOTH halves right
    // here -- see esp_spi_owner.c's own comment on that path -- so refcount
    // goes to 0; the completion-timeout branch releases only the client's
    // half, leaving 1 -- see test_completion_timeout_orphans_slot() below).
    // This assertion is also M3's uncovered-path check: deleting the second
    // owner_slot_pool_release() call on the enqueue-failure path in
    // esp_spi_owner.c would leave slot_refcount[0] == 1 here instead of 0.
    TEST_CHECK(owner.slot_refcount[0] == 0,
               "enqueue failure released BOTH halves of the slot -- refcount 0, proving this ran "
               "the enqueue-failure branch (not the completion-timeout branch, which leaves 1) and "
               "did not leak the slot");

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

// opus review, commit f3a1600, G1/G3: covers the SECOND timeout branch --
// the request WAS accepted by the queue (xQueueSend succeeds) but the owner
// task never answers (xSemaphoreTake on the slot's completion semaphore
// times out; the stub's xSemaphoreTake() is unconditionally pdFALSE, and
// nothing in this host-test process ever runs spi_owner_task() to give it --
// see this file's own header comment). Before the G1 fix this path left a
// StaticSemaphore_t and an esp_err_t* pointing into THIS function's stack
// frame in the request queue forever; after the fix, the slot is
// module-owned (owner.slots[]/owner.slot_refcount[]), so a late completion
// would land somewhere still valid instead of corrupting a reused stack
// frame. What is directly observable from here (owner_slot_pool.h's
// invariant, reached by #including esp_spi_owner.c) is that the CLIENT side
// releases only its own half of the slot's refcount on this timeout --
// refcount goes from 2 to 1, not to 0 -- leaving the slot "orphaned" (held
// open for whatever the owner task's own, still-pending release will
// eventually do) rather than freed while a write into it might still be
// coming.
static void test_completion_timeout_orphans_slot(void)
{
    // Enabled BEFORE spi_owner_init(), not after: xQueueCreate() (stubs/
    // freertos/queue.h) is what latches the ring's capacity/count from ITS
    // OWN queue_len argument, and spi_owner_init() is what calls it. Turning
    // ring mode on only after init would leave this test's queue with
    // whatever capacity/count some earlier test's xQueueCreate() call last
    // latched -- exactly the leaked-global class this file's own
    // g_stub_queue_ring_enabled extern comment warns about, just at a
    // different global (found by hitting it here first).
    g_stub_queue_ring_enabled = 1; // this test's own opt-in -- see this
                                    // file's extern declaration comment.
                                    // Reset in the section runner below so it
                                    // never leaks into another test file, the
                                    // exact bug this test exists to avoid
                                    // repeating (test_uart_log_bridge.c's
                                    // 2026-09-01 fix).

    spi_owner_t owner;
    esp_err_t init_err = spi_owner_init(&owner, 0 /*host*/, 4 /*queue_len*/, 5 /*priority*/,
                                         2048 /*stack*/, -1 /*core*/,
                                         /*dma_use_psram=*/false,
                                         /*async_flush=*/false);
    TEST_CHECK(init_err == ESP_OK, "spi_owner_init succeeds against the host stubs");

    uint8_t tx[4] = { 9, 8, 7, 6 };
    esp_err_t result =
        spi_owner_transfer(&owner, (spi_device_handle_t)0x1, tx, sizeof(tx), NULL, 0, /*cs_pin=*/5);

    TEST_CHECK(result == ESP_ERR_TIMEOUT,
               "enqueue succeeds but the owner never answers -- completion wait times out");
    TEST_CHECK(owner.wedged, "that timeout latches owner.wedged");
    // THE LOAD-BEARING CHECK: refcount 1, not 0 -- the client released only
    // its own half. A 0 here would mean the slot was freed (and its
    // semaphore drained / result reset) while the "owner" might still write
    // into it -- reintroducing the exact hazard G1 removed, just moved from
    // the caller's stack into this pool.
    TEST_CHECK(owner.slot_refcount[0] == 1,
               "client-side release on a completion timeout leaves the slot orphaned (refcount 1), not freed");

    g_stub_queue_ring_enabled = 0;
}

// opus review, commit 9fc55d9, M1: the pool must be sized queue_len + 1, not
// queue_len -- spi_owner_task() gives the slot's completion semaphore BEFORE
// it takes slot_lock to release the slot (see esp_spi_owner.c's tail
// comment), so a request can be dequeued (freeing a queue slot, letting a
// new caller's xQueueSend() succeed) while the just-dequeued request still
// holds its pool slot. Proven here directly against owner_slot_pool_alloc()
// (no FreeRTOS task ever runs in this host-test process -- see this file's
// header comment), which is exactly the bookkeeping spi_owner_transfer()
// itself calls: queue_len + 1 successful allocations must be possible before
// the pool is exhausted.
static void test_pool_sized_queue_len_plus_one(void)
{
    spi_owner_t owner;
    UBaseType_t queue_len = 4;
    esp_err_t init_err = spi_owner_init(&owner, 0 /*host*/, queue_len, 5 /*priority*/,
                                         2048 /*stack*/, -1 /*core*/,
                                         /*dma_use_psram=*/false,
                                         /*async_flush=*/false);
    TEST_CHECK(init_err == ESP_OK, "spi_owner_init succeeds against the host stubs");
    TEST_CHECK(owner.slot_count == (size_t)queue_len + 1,
               "pool is sized queue_len + 1, not queue_len");

    for (UBaseType_t i = 0; i < queue_len + 1; i++) {
        int idx = owner_slot_pool_alloc(owner.slot_refcount, owner.slot_count);
        TEST_CHECK(idx >= 0, "the pool can serve queue_len + 1 concurrent slots");
    }
    // One more than queue_len + 1 must still fail -- the pool is bounded, not
    // unlimited; this is what M1's fix protects against exhausting silently.
    int over = owner_slot_pool_alloc(owner.slot_refcount, owner.slot_count);
    TEST_CHECK(over < 0, "the (queue_len + 2)th concurrent slot is correctly refused");
}

// opus review, commit 9fc55d9, M1: pool exhaustion inside spi_owner_transfer()
// must fail closed (one transfer refused) rather than latching the shared
// owner wedged -- a wedge here would permanently kill the display AND every
// thermocouple channel over a transient resource condition, not a genuinely
// stuck bus. Forces exhaustion directly (allocate every slot behind
// spi_owner_transfer()'s back, exactly like test_pool_sized_queue_len_plus_one
// above) rather than trying to race real concurrent callers, which this
// single-threaded host-test process cannot do.
static void test_pool_exhaustion_fails_closed_not_wedged(void)
{
    spi_owner_t owner;
    UBaseType_t queue_len = 4;
    esp_err_t init_err = spi_owner_init(&owner, 0 /*host*/, queue_len, 5 /*priority*/,
                                         2048 /*stack*/, -1 /*core*/,
                                         /*dma_use_psram=*/false,
                                         /*async_flush=*/false);
    TEST_CHECK(init_err == ESP_OK, "spi_owner_init succeeds against the host stubs");

    for (UBaseType_t i = 0; i < queue_len + 1; i++) {
        int idx = owner_slot_pool_alloc(owner.slot_refcount, owner.slot_count);
        TEST_CHECK(idx >= 0, "setup: fill every pool slot ahead of the call under test");
    }

    uint8_t tx[4] = { 1, 2, 3, 4 };
    esp_err_t result =
        spi_owner_transfer(&owner, (spi_device_handle_t)0x1, tx, sizeof(tx), NULL, 0, /*cs_pin=*/5);
    TEST_CHECK(result == ESP_ERR_NO_MEM, "pool exhaustion fails this one transfer with ESP_ERR_NO_MEM");
    // THE LOAD-BEARING CHECK: unlike a genuine enqueue/completion timeout,
    // this must NOT latch wedged -- an exhausted pool is a transient resource
    // condition, not a wedged bus (see esp_spi_owner.c's own comment on this
    // path).
    TEST_CHECK(!owner.wedged, "pool exhaustion does NOT latch owner.wedged");
}

// DISPLAY_ST7796_PLAN.md 9.5: MAX31856 register transfers move to
// spi_device_polling_transmit() (11us measured vs 26us for the queued/ISR
// path); display flush transfers stay on spi_device_transmit(). The dispatch
// lives in spi_owner_task() itself (`request.use_polling ? ... : ...`), which
// -- unlike spi_owner_transfer()'s caller-side logic covered above -- this
// project's host stubs normally never reach (freertos/task.h's
// xTaskCreatePinnedToCore() never invokes the task function). Reached here
// anyway by calling spi_owner_task() directly: it is an ordinary C function
// (not a real FreeRTOS task) that loops on xQueueReceive() until it dequeues
// a shutdown request, so preloading the stub's ring queue (stubs/freertos/
// queue.h's g_stub_queue_ring_enabled opt-in) with one transfer followed by a
// shutdown request lets it run to completion synchronously in this
// single-threaded host process, exactly as this file's header comment
// anticipated might be needed for the SUCCESS path.
static void test_owner_task_dispatches_polling_vs_queued(void)
{
    g_stub_queue_ring_enabled = 1;

    spi_owner_t owner;
    esp_err_t init_err = spi_owner_init(&owner, 0 /*host*/, 4 /*queue_len*/, 5 /*priority*/,
                                         2048 /*stack*/, -1 /*core*/,
                                         /*dma_use_psram=*/false,
                                         /*async_flush=*/false);
    TEST_CHECK(init_err == ESP_OK, "spi_owner_init succeeds against the host stubs");

    uint8_t tx[4] = { 1, 2, 3, 4 };
    int idx = owner_slot_pool_alloc(owner.slot_refcount, owner.slot_count);
    TEST_CHECK(idx >= 0, "setup: a slot for the hand-built polling request");

    spi_owner_request_t polling_req;
    memset(&polling_req, 0, sizeof(polling_req));
    polling_req.device = (spi_device_handle_t)0x1;
    polling_req.tx_buffer = tx;
    polling_req.tx_length = sizeof(tx);
    polling_req.cs_pin = 5;
    polling_req.slot = idx;
    polling_req.use_polling = true;

    spi_owner_request_t shutdown_req;
    memset(&shutdown_req, 0, sizeof(shutdown_req));
    shutdown_req.shutdown = true;

    TEST_CHECK(xQueueSend(owner.request_queue, &polling_req, 0) == pdTRUE,
               "setup: polling request accepted by the ring");
    TEST_CHECK(xQueueSend(owner.request_queue, &shutdown_req, 0) == pdTRUE,
               "setup: shutdown request accepted by the ring");

    g_stub_spi_transmit_calls = 0;
    g_stub_spi_polling_transmit_calls = 0;

    spi_owner_task(&owner); // returns once it dequeues the shutdown request

    TEST_CHECK(g_stub_spi_polling_transmit_calls == 1,
               "use_polling=true dispatched through spi_device_polling_transmit()");
    TEST_CHECK(g_stub_spi_transmit_calls == 0,
               "use_polling=true did NOT also go through spi_device_transmit()");

    g_stub_queue_ring_enabled = 0;
}

// Same mechanism, use_polling=false -- the display flush path this session
// did NOT move to polling (9.5 is scoped to MAX31856 only; see esp_spi_owner.c's
// comment on spi_owner_transfer() vs spi_owner_transfer_polling()). Proves
// the two dispatch arms are actually distinguished, not just that the
// polling arm fires -- flip request.use_polling in esp_spi_owner.c's
// spi_owner_task() and this goes red (g_stub_spi_transmit_calls would read 0).
static void test_owner_task_dispatches_queued_when_not_polling(void)
{
    g_stub_queue_ring_enabled = 1;

    spi_owner_t owner;
    esp_err_t init_err = spi_owner_init(&owner, 0 /*host*/, 4 /*queue_len*/, 5 /*priority*/,
                                         2048 /*stack*/, -1 /*core*/,
                                         /*dma_use_psram=*/false,
                                         /*async_flush=*/false);
    TEST_CHECK(init_err == ESP_OK, "spi_owner_init succeeds against the host stubs");

    uint8_t tx[4] = { 9, 9, 9, 9 };
    int idx = owner_slot_pool_alloc(owner.slot_refcount, owner.slot_count);
    TEST_CHECK(idx >= 0, "setup: a slot for the hand-built queued request");

    spi_owner_request_t queued_req;
    memset(&queued_req, 0, sizeof(queued_req));
    queued_req.device = (spi_device_handle_t)0x1;
    queued_req.tx_buffer = tx;
    queued_req.tx_length = sizeof(tx);
    queued_req.cs_pin = 5;
    queued_req.slot = idx;
    queued_req.use_polling = false;

    spi_owner_request_t shutdown_req;
    memset(&shutdown_req, 0, sizeof(shutdown_req));
    shutdown_req.shutdown = true;

    TEST_CHECK(xQueueSend(owner.request_queue, &queued_req, 0) == pdTRUE,
               "setup: queued request accepted by the ring");
    TEST_CHECK(xQueueSend(owner.request_queue, &shutdown_req, 0) == pdTRUE,
               "setup: shutdown request accepted by the ring");

    g_stub_spi_transmit_calls = 0;
    g_stub_spi_polling_transmit_calls = 0;

    spi_owner_task(&owner);

    TEST_CHECK(g_stub_spi_transmit_calls == 1,
               "use_polling=false dispatched through spi_device_transmit()");
    TEST_CHECK(g_stub_spi_polling_transmit_calls == 0,
               "use_polling=false did NOT also go through spi_device_polling_transmit()");

    g_stub_queue_ring_enabled = 0;
}

// DISPLAY_ST7796_PLAN.md 9.4: cs_pin < 0 is the sentinel a caller built under
// CONFIG_KILNCTL_SPI_HARDWARE_CS passes once a device's CS is handed to the
// SPI peripheral via spics_io_num -- esp_spi_owner.c must NOT also bit-bang
// that GPIO, or the driver-level CS and the owner's manual toggling would
// fight each other on the same live pin. Same "hand-build the request, run
// spi_owner_task() directly" technique as the dispatch tests above. Mutate
// esp_spi_owner.c's `bool bitbang_cs = request.cs_pin >= 0;` to an
// unconditional true and this goes red (g_stub_gpio_set_level_calls reads 2
// instead of 0).
static void test_owner_task_skips_gpio_when_cs_pin_negative(void)
{
    g_stub_queue_ring_enabled = 1;

    spi_owner_t owner;
    esp_err_t init_err = spi_owner_init(&owner, 0 /*host*/, 4 /*queue_len*/, 5 /*priority*/,
                                         2048 /*stack*/, -1 /*core*/,
                                         /*dma_use_psram=*/false,
                                         /*async_flush=*/false);
    TEST_CHECK(init_err == ESP_OK, "spi_owner_init succeeds against the host stubs");

    uint8_t tx[4] = { 1, 2, 3, 4 };
    int idx = owner_slot_pool_alloc(owner.slot_refcount, owner.slot_count);
    TEST_CHECK(idx >= 0, "setup: a slot for the hand-built hardware-CS request");

    spi_owner_request_t hw_cs_req;
    memset(&hw_cs_req, 0, sizeof(hw_cs_req));
    hw_cs_req.device = (spi_device_handle_t)0x1;
    hw_cs_req.tx_buffer = tx;
    hw_cs_req.tx_length = sizeof(tx);
    hw_cs_req.cs_pin = -1; /* hardware CS: peripheral owns this pin */
    hw_cs_req.slot = idx;

    spi_owner_request_t shutdown_req;
    memset(&shutdown_req, 0, sizeof(shutdown_req));
    shutdown_req.shutdown = true;

    TEST_CHECK(xQueueSend(owner.request_queue, &hw_cs_req, 0) == pdTRUE,
               "setup: hardware-CS request accepted by the ring");
    TEST_CHECK(xQueueSend(owner.request_queue, &shutdown_req, 0) == pdTRUE,
               "setup: shutdown request accepted by the ring");

    g_stub_gpio_set_level_calls = 0;

    spi_owner_task(&owner);

    TEST_CHECK(g_stub_gpio_set_level_calls == 0,
               "cs_pin=-1 never touches gpio_set_level -- hardware CS owns this pin");

    g_stub_queue_ring_enabled = 0;
}

// Negative control for the test above, proving it can actually fail: a
// request with a real GPIO (today's only production shape -- every
// spi_bus_add_device() call site in the tree still sets spics_io_num = -1)
// must still bit-bang CS exactly twice (assert then deassert), same as
// before this pass touched esp_spi_owner.c at all.
static void test_owner_task_bitbangs_cs_when_cs_pin_valid(void)
{
    g_stub_queue_ring_enabled = 1;

    spi_owner_t owner;
    esp_err_t init_err = spi_owner_init(&owner, 0 /*host*/, 4 /*queue_len*/, 5 /*priority*/,
                                         2048 /*stack*/, -1 /*core*/,
                                         /*dma_use_psram=*/false,
                                         /*async_flush=*/false);
    TEST_CHECK(init_err == ESP_OK, "spi_owner_init succeeds against the host stubs");

    uint8_t tx[4] = { 1, 2, 3, 4 };
    int idx = owner_slot_pool_alloc(owner.slot_refcount, owner.slot_count);
    TEST_CHECK(idx >= 0, "setup: a slot for the hand-built bit-banged-CS request");

    spi_owner_request_t sw_cs_req;
    memset(&sw_cs_req, 0, sizeof(sw_cs_req));
    sw_cs_req.device = (spi_device_handle_t)0x1;
    sw_cs_req.tx_buffer = tx;
    sw_cs_req.tx_length = sizeof(tx);
    sw_cs_req.cs_pin = 5; /* today's only production shape */
    sw_cs_req.slot = idx;

    spi_owner_request_t shutdown_req;
    memset(&shutdown_req, 0, sizeof(shutdown_req));
    shutdown_req.shutdown = true;

    TEST_CHECK(xQueueSend(owner.request_queue, &sw_cs_req, 0) == pdTRUE,
               "setup: bit-banged-CS request accepted by the ring");
    TEST_CHECK(xQueueSend(owner.request_queue, &shutdown_req, 0) == pdTRUE,
               "setup: shutdown request accepted by the ring");

    g_stub_gpio_set_level_calls = 0;

    spi_owner_task(&owner);

    TEST_CHECK(g_stub_gpio_set_level_calls == 2,
               "cs_pin>=0 still bit-bangs CS assert+deassert, unchanged from before this pass");

    g_stub_queue_ring_enabled = 0;
}

// DISPLAY_ST7796_PLAN.md 9.3: owner->dma_use_psram (CONFIG_KILNCTL_SPI_DMA_USE_PSRAM,
// default OFF) must reach the transaction's flags on the queued (display
// flush) path. Mutate esp_spi_owner.c's flag-setting `if` to check the wrong
// thing (or delete it) and this goes red (last_flags reads 0 instead of
// SPI_TRANS_DMA_USE_PSRAM).
static void test_owner_task_sets_dma_psram_flag_on_queued_path(void)
{
    g_stub_queue_ring_enabled = 1;

    spi_owner_t owner;
    esp_err_t init_err = spi_owner_init(&owner, 0 /*host*/, 4 /*queue_len*/, 5 /*priority*/,
                                         2048 /*stack*/, -1 /*core*/,
                                         /*dma_use_psram=*/true,
                                         /*async_flush=*/false);
    TEST_CHECK(init_err == ESP_OK, "spi_owner_init succeeds against the host stubs");
    TEST_CHECK(owner.dma_use_psram, "owner.dma_use_psram reflects the init-time argument");

    uint8_t tx[4] = { 1, 2, 3, 4 };
    int idx = owner_slot_pool_alloc(owner.slot_refcount, owner.slot_count);
    TEST_CHECK(idx >= 0, "setup: a slot for the hand-built queued request");

    spi_owner_request_t queued_req;
    memset(&queued_req, 0, sizeof(queued_req));
    queued_req.device = (spi_device_handle_t)0x1;
    queued_req.tx_buffer = tx;
    queued_req.tx_length = sizeof(tx);
    queued_req.cs_pin = 5;
    queued_req.slot = idx;
    queued_req.use_polling = false;

    spi_owner_request_t shutdown_req;
    memset(&shutdown_req, 0, sizeof(shutdown_req));
    shutdown_req.shutdown = true;

    TEST_CHECK(xQueueSend(owner.request_queue, &queued_req, 0) == pdTRUE,
               "setup: queued request accepted by the ring");
    TEST_CHECK(xQueueSend(owner.request_queue, &shutdown_req, 0) == pdTRUE,
               "setup: shutdown request accepted by the ring");

    g_stub_spi_transmit_last_flags = 0xFFFFFFFFu; /* poison, so a missed write is visible */

    spi_owner_task(&owner);

    TEST_CHECK((g_stub_spi_transmit_last_flags & SPI_TRANS_DMA_USE_PSRAM) != 0,
               "dma_use_psram=true sets SPI_TRANS_DMA_USE_PSRAM on the queued-path transaction");

    g_stub_queue_ring_enabled = 0;
}

// Negative control: the SAME owner (dma_use_psram=true) must NOT set the
// flag on a polling (MAX31856) transfer -- that path never carries a
// PSRAM-backed buffer (see esp_spi_owner.c's comment on the `!request.use_polling`
// guard). Proves the guard is actually conditioned on use_polling, not just
// always-on once owner->dma_use_psram is true.
static void test_owner_task_never_sets_dma_psram_flag_on_polling_path(void)
{
    g_stub_queue_ring_enabled = 1;

    spi_owner_t owner;
    esp_err_t init_err = spi_owner_init(&owner, 0 /*host*/, 4 /*queue_len*/, 5 /*priority*/,
                                         2048 /*stack*/, -1 /*core*/,
                                         /*dma_use_psram=*/true,
                                         /*async_flush=*/false);
    TEST_CHECK(init_err == ESP_OK, "spi_owner_init succeeds against the host stubs");

    uint8_t tx[4] = { 1, 2, 3, 4 };
    int idx = owner_slot_pool_alloc(owner.slot_refcount, owner.slot_count);
    TEST_CHECK(idx >= 0, "setup: a slot for the hand-built polling request");

    spi_owner_request_t polling_req;
    memset(&polling_req, 0, sizeof(polling_req));
    polling_req.device = (spi_device_handle_t)0x1;
    polling_req.tx_buffer = tx;
    polling_req.tx_length = sizeof(tx);
    polling_req.cs_pin = 5;
    polling_req.slot = idx;
    polling_req.use_polling = true;

    spi_owner_request_t shutdown_req;
    memset(&shutdown_req, 0, sizeof(shutdown_req));
    shutdown_req.shutdown = true;

    TEST_CHECK(xQueueSend(owner.request_queue, &polling_req, 0) == pdTRUE,
               "setup: polling request accepted by the ring");
    TEST_CHECK(xQueueSend(owner.request_queue, &shutdown_req, 0) == pdTRUE,
               "setup: shutdown request accepted by the ring");

    g_stub_spi_polling_transmit_last_flags = 0xFFFFFFFFu; /* poison */

    spi_owner_task(&owner);

    TEST_CHECK((g_stub_spi_polling_transmit_last_flags & SPI_TRANS_DMA_USE_PSRAM) == 0,
               "dma_use_psram=true does NOT set the flag on the polling (MAX31856) path");

    g_stub_queue_ring_enabled = 0;
}

// DISPLAY_ST7796_PLAN.md 9.6: default OFF (async_flush=false, i.e.
// CONFIG_KILNCTL_SPI_ASYNC_FLUSH default n) must make
// spi_owner_transfer_async() refuse outright, without ever touching the
// request queue -- exactly the behavior every existing caller in today's
// tree relies on (none of them call this function; if this guard were ever
// deleted or inverted, a caller that later opts in on a real board would
// silently get no async behavior at all, or -- if the guard's SENSE were
// flipped -- a bench-verified-only path would start running against a
// caller that never asked for it). Mutate the `if (!owner->async_flush)`
// check to `if (owner->async_flush)` and this test goes red (returns ESP_OK
// and a queue send happens instead of the immediate refusal).
static void test_async_transfer_refused_when_flag_off(void)
{
    spi_owner_t owner;
    esp_err_t init_err = spi_owner_init(&owner, 0 /*host*/, 4 /*queue_len*/, 5 /*priority*/,
                                         2048 /*stack*/, -1 /*core*/,
                                         /*dma_use_psram=*/false,
                                         /*async_flush=*/false);
    TEST_CHECK(init_err == ESP_OK, "spi_owner_init succeeds against the host stubs");
    TEST_CHECK(!owner.async_flush, "owner.async_flush reflects the init-time argument (false)");

    uint8_t tx[4] = { 1, 2, 3, 4 };
    g_stub_queue_send_calls = 0;

    esp_err_t err = spi_owner_transfer_async(&owner, (spi_device_handle_t)0x1, tx, sizeof(tx),
                                              /*cs_pin=*/5, NULL, NULL);
    TEST_CHECK(err == ESP_ERR_NOT_SUPPORTED,
               "async transfer against an owner with async_flush=false is refused outright");
    TEST_CHECK(g_stub_queue_send_calls == 0,
               "the refusal never touches xQueueSend() -- no request was queued");
}

// Positive control, same owner shape as the dma_use_psram tests above: with
// async_flush=true, a hand-built async request run through spi_owner_task()
// must (a) still perform the transfer via spi_device_transmit() exactly like
// a synchronous request, and (b) invoke the caller-supplied async_cb with
// the transfer's result, from the owner task's own call -- proving the
// callback actually fires rather than being silently dropped. Mutate the
// `if (request.async && request.async_cb)` guard in esp_spi_owner.c (delete
// it, or the call inside it) and this goes red (the callback counter stays
// 0).
static int s_async_cb_calls;
static esp_err_t s_async_cb_last_result;
static void *s_async_cb_last_ctx;
static void test_async_done_cb(void *ctx, esp_err_t result)
{
    s_async_cb_calls++;
    s_async_cb_last_result = result;
    s_async_cb_last_ctx = ctx;
}

static void test_owner_task_fires_async_callback(void)
{
    g_stub_queue_ring_enabled = 1;

    spi_owner_t owner;
    esp_err_t init_err = spi_owner_init(&owner, 0 /*host*/, 4 /*queue_len*/, 5 /*priority*/,
                                         2048 /*stack*/, -1 /*core*/,
                                         /*dma_use_psram=*/false,
                                         /*async_flush=*/true);
    TEST_CHECK(init_err == ESP_OK, "spi_owner_init succeeds against the host stubs");

    uint8_t tx[4] = { 9, 9, 9, 9 };
    int idx = owner_slot_pool_alloc(owner.slot_refcount, owner.slot_count);
    TEST_CHECK(idx >= 0, "setup: a slot for the hand-built async request");

    int marker = 42;
    s_async_cb_calls = 0;
    s_async_cb_last_result = ESP_FAIL;
    s_async_cb_last_ctx = NULL;

    spi_owner_request_t async_req;
    memset(&async_req, 0, sizeof(async_req));
    async_req.device = (spi_device_handle_t)0x1;
    async_req.tx_buffer = tx;
    async_req.tx_length = sizeof(tx);
    async_req.cs_pin = 5;
    async_req.slot = idx;
    async_req.async = true;
    async_req.async_cb = test_async_done_cb;
    async_req.async_ctx = &marker;

    spi_owner_request_t shutdown_req;
    memset(&shutdown_req, 0, sizeof(shutdown_req));
    shutdown_req.shutdown = true;

    TEST_CHECK(xQueueSend(owner.request_queue, &async_req, 0) == pdTRUE,
               "setup: async request accepted by the ring");
    TEST_CHECK(xQueueSend(owner.request_queue, &shutdown_req, 0) == pdTRUE,
               "setup: shutdown request accepted by the ring");

    spi_owner_task(&owner);

    TEST_CHECK(s_async_cb_calls == 1, "async_cb fired exactly once for the async request");
    TEST_CHECK(s_async_cb_last_result == ESP_OK,
               "async_cb was given the transfer's own result (the stub's spi_device_transmit succeeds)");
    TEST_CHECK(s_async_cb_last_ctx == &marker, "async_cb received the exact ctx pointer the request carried");

    g_stub_queue_ring_enabled = 0;
}

// Negative control, same shape as 9.3's polling-path control above: a
// SYNCHRONOUS request (async=false, the default/every other request in this
// file) must never call async_cb, even though owner.async_flush is true --
// proves the callback is gated on request.async, not just on the owner-wide
// flag.
static void test_owner_task_never_fires_async_callback_on_sync_request(void)
{
    g_stub_queue_ring_enabled = 1;

    spi_owner_t owner;
    esp_err_t init_err = spi_owner_init(&owner, 0 /*host*/, 4 /*queue_len*/, 5 /*priority*/,
                                         2048 /*stack*/, -1 /*core*/,
                                         /*dma_use_psram=*/false,
                                         /*async_flush=*/true);
    TEST_CHECK(init_err == ESP_OK, "spi_owner_init succeeds against the host stubs");

    uint8_t tx[4] = { 1, 1, 1, 1 };
    int idx = owner_slot_pool_alloc(owner.slot_refcount, owner.slot_count);
    TEST_CHECK(idx >= 0, "setup: a slot for the hand-built sync request");

    s_async_cb_calls = 0;

    spi_owner_request_t sync_req;
    memset(&sync_req, 0, sizeof(sync_req));
    sync_req.device = (spi_device_handle_t)0x1;
    sync_req.tx_buffer = tx;
    sync_req.tx_length = sizeof(tx);
    sync_req.cs_pin = 5;
    sync_req.slot = idx;
    sync_req.async = false; /* default -- every existing caller */
    sync_req.async_cb = test_async_done_cb; /* set but must not be called */
    sync_req.async_ctx = NULL;

    spi_owner_request_t shutdown_req;
    memset(&shutdown_req, 0, sizeof(shutdown_req));
    shutdown_req.shutdown = true;

    TEST_CHECK(xQueueSend(owner.request_queue, &sync_req, 0) == pdTRUE,
               "setup: sync request accepted by the ring");
    TEST_CHECK(xQueueSend(owner.request_queue, &shutdown_req, 0) == pdTRUE,
               "setup: shutdown request accepted by the ring");

    spi_owner_task(&owner);

    TEST_CHECK(s_async_cb_calls == 0,
               "a synchronous request never fires async_cb, even with an owner-wide async_flush=true "
               "and a non-NULL async_cb sitting in the request struct");

    g_stub_queue_ring_enabled = 0;
}

void run_test_esp_spi_owner(void)
{
    TEST_SECTION("esp_spi_owner");
    test_wedge_latches_and_fails_fast();
    test_completion_timeout_orphans_slot();
    test_pool_sized_queue_len_plus_one();
    test_pool_exhaustion_fails_closed_not_wedged();
    test_owner_task_dispatches_polling_vs_queued();
    test_owner_task_dispatches_queued_when_not_polling();
    test_owner_task_skips_gpio_when_cs_pin_negative();
    test_owner_task_bitbangs_cs_when_cs_pin_valid();
    test_owner_task_sets_dma_psram_flag_on_queued_path();
    test_owner_task_never_sets_dma_psram_flag_on_polling_path();
    test_async_transfer_refused_when_flag_off();
    test_owner_task_fires_async_callback();
    test_owner_task_never_fires_async_callback_on_sync_request();
}
