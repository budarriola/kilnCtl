// Host tests for App/drivers/persist/trip_history.c (FD-05): first trip, ring wrap, dedupe,
// corrupt blob, pending queue and the flush write path. trip_history.c is #included so the
// statics are reachable; fake_kv backs NVS and the flash_hop stubs stand in for bx_flash_worker.
#include <stddef.h>
#include <math.h>
#include <string.h>

#include "test_common.h"

#include "esp_err.h"
#include "fake_kv.h"
#include "flash_hop_host_stub.h"

int g_test_failures = 0;
int g_test_count = 0;

#include "../drivers/persist/trip_history.c"

static void reset_all(void)
{
    fake_kv_reset_all();
    fake_kv_set_write_safe_here(true);
    hal_kv_init_partition(KILN_NVS_PARTITION);
    flash_hop_stub_reset();
    s_pend_head = 0;
    s_pend_tail = 0;
    s_dropped = 0;
}

static trip_history_entry_t mk(uint8_t trip_seq, uint8_t reason, uint32_t pico_ms)
{
    trip_history_entry_t e;
    memset(&e, 0, sizeof(e));
    e.pico_trip_seq = trip_seq;
    e.reason = reason;
    e.pico_uptime_ms = pico_ms;
    e.zone = TRIP_HISTORY_ZONE_UNKNOWN;
    e.esp_uptime_s = 100u + trip_seq;
    e.utc = 0u;
    e.boot_seq = 5u;
    e.safety_tc_c = 1234.5f;
    e.threshold = 1200.0f;
    return e;
}

static void test_first_trip(void)
{
    TEST_SECTION("trip_history: first trip into an empty store");
    reset_all();
    trip_history_blob_t b;
    TEST_CHECK(!trip_history_load(&b), "no stored blob reports false");
    TEST_CHECK(b.count == 0 && b.next_seq == 1u, "empty ring, next_seq 1");
    trip_history_entry_t e = mk(1, 6, 5000);
    TEST_CHECK(trip_history_note(&e), "note accepted");
    TEST_CHECK(g_flash_hop_stub_dispatches == 0u, "note does no flash work");
    TEST_CHECK(trip_history_flush() == 1u, "flush records one");
    TEST_CHECK(trip_history_load(&b) && b.count == 1, "blob loads back with one entry");
    trip_history_entry_t g;
    TEST_CHECK(trip_history_blob_get(&b, 0, &g), "newest exists");
    TEST_CHECK(g.seq == 1u && g.reason == 6 && g.mask == 0x20u && g.zone == TRIP_HISTORY_ZONE_UNKNOWN &&
                   g.boot_seq == 5u && g.utc == 0u && g.esp_uptime_s == 101u && g.pico_uptime_ms == 5000u,
               "fields stored (mask = 1<<(reason-1), S6a 0x0020)");
    TEST_CHECK(!trip_history_blob_get(&b, 1, &g), "only one entry");
    TEST_CHECK(trip_history_flush() == 0u, "empty queue flush is a no-op");
}

static void test_dedupe(void)
{
    TEST_SECTION("trip_history: repeated latched trip writes once");
    reset_all();
    trip_history_entry_t e = mk(3, 2, 777);
    trip_history_note(&e);
    TEST_CHECK(trip_history_flush() == 1u, "first copy recorded");
    fake_kv_script_next_write_status(HAL_NO_MEM); /* trap: any write attempt consumes this */
    trip_history_note(&e);
    trip_history_note(&e);
    TEST_CHECK(trip_history_flush() == 0u, "resends of the same trip recorded 0 times");
    trip_history_entry_t e2 = mk(4, 2, 900);
    trip_history_note(&e2);
    TEST_CHECK(trip_history_flush() == 0u, "trap still armed => the duplicate flush made no write, the new trip's write hit it");
    trip_history_note(&e2);
    TEST_CHECK(trip_history_flush() == 1u, "a different trip_seq is a new trip");
    trip_history_entry_t e3 = mk(3, 2, 5000); /* Pico rebooted: same seq/reason, different uptime */
    trip_history_note(&e3);
    TEST_CHECK(trip_history_flush() == 1u, "same seq+reason with a different Pico uptime is a new trip");
    trip_history_blob_t b;
    trip_history_load(&b);
    TEST_CHECK(b.count == 3, "three distinct trips in the ring");
}

static void test_wrap(void)
{
    TEST_SECTION("trip_history: ring wraps at 16, newest first");
    reset_all();
    for (unsigned i = 1; i <= TRIP_HISTORY_MAX + 4; i++) {
        trip_history_entry_t e = mk((uint8_t)i, 1, 1000u + i);
        trip_history_note(&e);
        trip_history_flush();
    }
    trip_history_blob_t b;
    TEST_CHECK(trip_history_load(&b) && b.count == TRIP_HISTORY_MAX, "count capped at 16");
    trip_history_entry_t g;
    trip_history_blob_get(&b, 0, &g);
    TEST_CHECK(g.seq == TRIP_HISTORY_MAX + 4 && g.pico_trip_seq == TRIP_HISTORY_MAX + 4, "newest is the last written");
    trip_history_blob_get(&b, TRIP_HISTORY_MAX - 1, &g);
    TEST_CHECK(g.seq == 5u, "oldest kept is seq 5 (4 evicted)");
    TEST_CHECK(!trip_history_blob_get(&b, TRIP_HISTORY_MAX, &g), "no 17th entry");
    trip_history_entry_t evicted = mk(1, 1, 1001u);
    TEST_CHECK(!trip_history_blob_has(&b, &evicted), "evicted trip no longer in the dedupe set");
}

static void test_corrupt(void)
{
    TEST_SECTION("trip_history: corrupt / wrong-size / bad-head blobs restart the ring");
    reset_all();
    trip_history_entry_t e = mk(1, 1, 10);
    trip_history_note(&e);
    trip_history_flush();
    trip_history_blob_t b;
    trip_history_load(&b);

    trip_history_blob_t bad = b;
    bad.entries[0].reason ^= 0x55u; /* CRC no longer matches */
    TEST_CHECK(!trip_history_blob_valid(&bad), "payload flip fails CRC");
    bad = b;
    bad.head = TRIP_HISTORY_MAX;
    bad.crc32 = blob_integrity(&bad);
    TEST_CHECK(!trip_history_blob_valid(&bad), "head out of range invalid even with good CRC");
    bad = b;
    bad.count = TRIP_HISTORY_MAX + 1;
    bad.crc32 = blob_integrity(&bad);
    TEST_CHECK(!trip_history_blob_valid(&bad), "count out of range invalid");
    bad = b;
    bad.version = 9;
    bad.crc32 = blob_integrity(&bad);
    TEST_CHECK(!trip_history_blob_valid(&bad), "wrong version invalid");

    /* Store a CRC-broken blob; a new trip must start a fresh ring at seq 1. */
    hal_kv_handle_t h;
    hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    bad = b;
    bad.entries[0].reason ^= 0x55u;
    hal_kv_set_blob(&h, NVS_KEY_TRIP_HIST, &bad, sizeof(bad));
    hal_kv_commit(&h);
    hal_kv_close(&h);
    TEST_CHECK(!trip_history_load(&b) && b.count == 0 && b.next_seq == 1u, "corrupt stored blob loads as empty");
    trip_history_entry_t e2 = mk(2, 3, 20);
    trip_history_note(&e2);
    TEST_CHECK(trip_history_flush() == 1u, "new trip after corruption recorded");
    trip_history_load(&b);
    trip_history_entry_t g;
    trip_history_blob_get(&b, 0, &g);
    TEST_CHECK(b.count == 1 && g.seq == 1u && g.reason == 3, "fresh ring restarted at seq 1");

    /* Wrong size. */
    hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    hal_kv_set_blob(&h, NVS_KEY_TRIP_HIST, &b, sizeof(b) - 4);
    hal_kv_commit(&h);
    hal_kv_close(&h);
    TEST_CHECK(!trip_history_load(&b) && b.count == 0, "short blob loads as empty");
}

static void test_queue_and_failures(void)
{
    TEST_SECTION("trip_history: pending queue bound, hop refusal keeps the queue");
    reset_all();
    unsigned accepted = 0;
    for (unsigned i = 1; i <= TRIP_HISTORY_PENDING_MAX + 2; i++) {
        trip_history_entry_t e = mk((uint8_t)i, 1, 100u + i);
        if (trip_history_note(&e)) {
            accepted++;
        }
    }
    TEST_CHECK(accepted == TRIP_HISTORY_PENDING_MAX - 1u, "SPSC queue holds MAX-1 entries");
    TEST_CHECK(trip_history_dropped() == 3u, "overflow counted as dropped");

    g_flash_hop_stub_fail = true;
    fake_kv_set_write_safe_here(false);
    TEST_CHECK(trip_history_flush() == 0u, "flush reports 0 when the worker refuses");
    g_flash_hop_stub_fail = false;
    TEST_CHECK(trip_history_flush() == accepted, "queue was kept and is recorded on the next flush");
    flash_hop_stub_reset();
    trip_history_blob_t b;
    trip_history_load(&b);
    TEST_CHECK(b.count == accepted, "all queued trips persisted");

    TEST_SECTION("trip_history: write travels through the worker from a PSRAM-stack caller");
    reset_all();
    fake_kv_set_write_safe_here(false);
    trip_history_entry_t e = mk(9, 4, 4242);
    trip_history_note(&e);
    TEST_CHECK(trip_history_flush() == 1u && g_flash_hop_stub_dispatches == 1u, "dispatched onto the flash worker");
    fake_kv_set_write_safe_here(true);
    TEST_CHECK(trip_history_load(&b) && b.count == 1, "persisted");
    TEST_CHECK(trip_history_mask_for_reason(0) == 0u && trip_history_mask_for_reason(17) == 0u &&
                   trip_history_mask_for_reason(16) == 0x8000u,
               "mask is 0 outside reasons 1..16");
}

static void test_th01_th02(void)
{
    TEST_SECTION("trip_history: TH-01 clamp, TH-02 failed write keeps the queue");
    reset_all();
    trip_history_entry_t e = mk(1, 1, 500);
    e.safety_tc_c = 3.0e38f;
    e.threshold = -INFINITY;
    trip_history_note(&e);
    TEST_CHECK(s_pending[0].safety_tc_c == 1.0e5f && s_pending[0].threshold == -1.0e5f, "TH-01: huge/inf clamped");
    trip_history_entry_t n = mk(2, 1, 600);
    n.safety_tc_c = NAN;
    trip_history_note(&n);
    TEST_CHECK(s_pending[1].safety_tc_c == 0.0f, "TH-01: NaN becomes 0");

    fake_kv_script_next_write_status(HAL_IO);
    TEST_CHECK(trip_history_flush() == 0u, "TH-02: failed write records nothing");
    TEST_CHECK(s_pend_tail != s_pend_head, "TH-02: queue entries kept after a failed write");
    TEST_CHECK(trip_history_flush() == 2u, "TH-02: retry persists both");
    TEST_CHECK(s_pend_tail == s_pend_head, "TH-02: queue drained after success");
    trip_history_blob_t b;
    trip_history_load(&b);
    TEST_CHECK(b.count == 2u, "TH-02: exactly two persisted, no duplicates");
}

int main(void)
{
    test_th01_th02();
    test_first_trip();
    test_dedupe();
    test_wrap();
    test_corrupt();
    test_queue_and_failures();
    fake_kv_reset_all();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures == 0 ? 0 : 1;
}
