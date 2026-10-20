// Host tests for App/drivers/persist/boot_history.c (FD-03): ring wrap, corrupt
// record, first boot, and the boot write path. boot_history.c is #included so
// the static helpers are reachable, backed by fake_kv's RAM store.
#include <stddef.h>
#include <string.h>

#include "test_common.h"

#include "esp_err.h"
#include "fake_kv.h"

int g_test_failures = 0;
int g_test_count = 0;

#include "../drivers/persist/boot_history.c"

static void reset_all(void)
{
    fake_kv_reset_all();
    fake_kv_set_write_safe_here(true);
    hal_kv_init_partition(KILN_NVS_PARTITION);
}

static void test_first_boot(void)
{
    TEST_SECTION("boot_history: first boot (no blob) starts a ring at seq 1");
    reset_all();
    boot_history_blob_t b;
    TEST_CHECK(!boot_history_load(&b), "no stored blob reports false");
    TEST_CHECK(b.count == 0 && b.next_seq == 1u, "empty ring, next_seq 1");
    TEST_CHECK(boot_history_record_boot(1, false, 99, 7, false, "v1.0-5-gabc1234", 0) == ESP_OK, "record ok");
    TEST_CHECK(boot_history_load(&b), "blob loads back");
    boot_history_entry_t e;
    TEST_CHECK(boot_history_blob_get(&b, 0, &e), "newest entry exists");
    TEST_CHECK(e.seq == 1u, "first boot is seq 1");
    TEST_CHECK(e.reset_reason == 1 && strcmp(boot_history_reason_name(e.reset_reason), "POWERON") == 0, "reason stored");
    TEST_CHECK(!e.prev_uptime_known && e.prev_uptime_s == 0u, "unknown uptime stored as 0/unknown");
    TEST_CHECK(e.dump_id == 0u && !e.has_crash, "no crash => dump_id 0 even if one was passed");
    TEST_CHECK(strcmp(e.fw, "abc1234") == 0, "short hash taken from -g token");
    TEST_CHECK(e.wall_time == 0u, "wall time 0 when unknown");
    TEST_CHECK(!boot_history_blob_get(&b, 1, &e), "only one entry");
}

static void test_fields_and_order(void)
{
    TEST_SECTION("boot_history: fields round-trip, newest first");
    reset_all();
    boot_history_record_boot(4, true, 345, 0xDEADu, true, "x-gfeedbeef", 1700000000u);
    boot_history_record_boot(3, true, 10, 0xDEADu, true, "x-gfeedbeef", 0);
    boot_history_blob_t b;
    boot_history_load(&b);
    boot_history_entry_t e;
    boot_history_blob_get(&b, 0, &e);
    TEST_CHECK(e.seq == 2u && e.reset_reason == 3, "newest first (seq 2)");
    boot_history_blob_get(&b, 1, &e);
    TEST_CHECK(e.seq == 1u && e.prev_uptime_known && e.prev_uptime_s == 345u && e.dump_id == 0xDEADu &&
                   e.has_crash && e.wall_time == 1700000000u,
               "older entry keeps uptime, dump_id, crash flag, wall time");
    boot_history_entry_t r[BOOT_HISTORY_RECENT];
    TEST_CHECK(boot_history_recent(r) == 2u && r[0].seq == 2u, "RAM recent cache matches");
}

static void test_wrap(void)
{
    TEST_SECTION("boot_history: wraps at 16, oldest dropped, seq keeps rising");
    reset_all();
    for (unsigned i = 0; i < BOOT_HISTORY_MAX + 5; i++) {
        boot_history_record_boot(1, false, 0, 0, false, "g", 0);
    }
    boot_history_blob_t b;
    TEST_CHECK(boot_history_load(&b), "loads");
    TEST_CHECK(b.count == BOOT_HISTORY_MAX, "count capped at 16");
    boot_history_entry_t e;
    boot_history_blob_get(&b, 0, &e);
    TEST_CHECK(e.seq == BOOT_HISTORY_MAX + 5u, "newest seq = 21");
    boot_history_blob_get(&b, BOOT_HISTORY_MAX - 1, &e);
    TEST_CHECK(e.seq == 6u, "oldest kept seq = 6");
    TEST_CHECK(!boot_history_blob_get(&b, BOOT_HISTORY_MAX, &e), "no 17th entry");
}

static void test_corrupt(void)
{
    TEST_SECTION("boot_history: corrupt / wrong-size record restarts the ring");
    reset_all();
    boot_history_record_boot(1, false, 0, 0, false, "g", 0);
    boot_history_record_boot(1, false, 0, 0, false, "g", 0);
    /* flip one payload byte under a valid CRC */
    boot_history_blob_t b;
    boot_history_load(&b);
    b.entries[0].reset_reason ^= 0x5A;
    hal_kv_handle_t h;
    hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    hal_kv_set_blob(&h, NVS_KEY_BOOT_HIST, &b, sizeof(b));
    hal_kv_commit(&h);
    hal_kv_close(&h);
    TEST_CHECK(!boot_history_blob_valid(&b), "CRC mismatch detected");
    TEST_CHECK(!boot_history_load(&b) && b.count == 0 && b.next_seq == 1u, "load treats it as empty");
    boot_history_record_boot(2, false, 0, 0, false, "g", 0);
    boot_history_load(&b);
    TEST_CHECK(b.count == 1 && b.entries[0].seq == 1u, "new ring started at seq 1");
    /* wrong size */
    hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    uint8_t junk[10] = {1, 2, 3};
    hal_kv_set_blob(&h, NVS_KEY_BOOT_HIST, junk, sizeof(junk));
    hal_kv_commit(&h);
    hal_kv_close(&h);
    TEST_CHECK(!boot_history_load(&b) && b.count == 0, "wrong-size blob is empty ring");
    /* out-of-range head with a self-consistent CRC is still rejected */
    boot_history_blob_init(&b);
    b.head = BOOT_HISTORY_MAX;
    b.crc32 = blob_integrity(&b);
    TEST_CHECK(!boot_history_blob_valid(&b), "head out of range rejected");
}

static void test_write_failure(void)
{
    TEST_SECTION("boot_history: write failure is reported, RAM cache still filled");
    reset_all();
    fake_kv_script_next_write_status(HAL_IO);
    TEST_CHECK(boot_history_record_boot(1, false, 0, 0, false, "g", 0) != ESP_OK, "failure surfaced");
    boot_history_entry_t r[BOOT_HISTORY_RECENT];
    TEST_CHECK(boot_history_recent(r) == 1u, "recent cache holds this boot");
}

static void test_short_hash(void)
{
    TEST_SECTION("boot_history_short_hash");
    char o[BOOT_HISTORY_FW_MAX];
    boot_history_short_hash("v1.0.0-pre.3-12-g0123456789abcdef-dirty", o);
    TEST_CHECK(strcmp(o, "0123456789ab") == 0, "-g token capped at 12 chars");
    boot_history_short_hash("v1.0.0-pre.3", o);
    TEST_CHECK(strcmp(o, "v1.0.0-pre.3") == 0, "no -g: leading chars");
    boot_history_short_hash(NULL, o);
    TEST_CHECK(o[0] == '\0', "NULL => empty");
    boot_history_short_hash("a b\"c", o);
    TEST_CHECK(strcmp(o, "a") == 0, "stops at a non-token char (JSON-safe)");
    TEST_CHECK(strcmp(boot_history_reason_name(200), "UNKNOWN") == 0, "out-of-range reason");
}

static void test_bh01_bh02(void)
{
    TEST_SECTION("boot_history: BH-01 names cover the enum, BH-02 wall-time backfill");
    TEST_CHECK(strcmp(boot_history_reason_name(HAL_RESET_PWR_GLITCH), "PWR_GLITCH") == 0, "BH-01: PWR_GLITCH named");
    TEST_CHECK(strcmp(boot_history_reason_name(HAL_RESET_CPU_LOCKUP), "CPU_LOCKUP") == 0, "BH-01: CPU_LOCKUP named");
    for (int r = 1; r <= (int)HAL_RESET_CPU_LOCKUP; r++) {
        TEST_CHECK(strcmp(boot_history_reason_name((uint8_t)r), "UNKNOWN") != 0, "BH-01: every enum value has a name");
    }
    reset_all();
    boot_history_record_boot(1, false, 0, 0, false, "x-gabc", 0);
    TEST_CHECK(boot_history_backfill_wall_time(1700000000u) == ESP_OK, "BH-02: backfill ok");
    boot_history_blob_t b;
    boot_history_entry_t e;
    boot_history_load(&b);
    boot_history_blob_get(&b, 0, &e);
    TEST_CHECK(e.wall_time == 1700000000u, "BH-02: wall_time persisted on this boot's entry");
    boot_history_entry_t r[BOOT_HISTORY_RECENT];
    boot_history_recent(r);
    TEST_CHECK(r[0].wall_time == 1700000000u, "BH-02: RAM cache updated");
    TEST_CHECK(boot_history_backfill_wall_time(1700000999u) != ESP_OK, "BH-02: second backfill refused");
    boot_history_load(&b);
    boot_history_blob_get(&b, 0, &e);
    TEST_CHECK(e.wall_time == 1700000000u, "BH-02: first value kept");
}

int main(void)
{
    test_bh01_bh02();
    test_first_boot();
    test_fields_and_order();
    test_wrap();
    test_corrupt();
    test_write_failure();
    test_short_hash();
    fake_kv_reset_all();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures == 0 ? 0 : 1;
}
