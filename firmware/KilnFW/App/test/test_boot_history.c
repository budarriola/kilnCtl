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
    memset(&s_pending, 0, sizeof(s_pending));
    s_coalesced = false;
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
        boot_history_record_boot((uint8_t)(1 + (i & 1u)), false, 0, 0, false, "g", 0); /* alternate: no coalescing */
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
    b.crc32 = BLOB_CRC(&b);
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

static void test_fw01_fw02_coalesce(void)
{
    TEST_SECTION("boot_history: FW-01 boot loop of 100 identical boots coalesces, FW-02 no backfill write");
    reset_all();
    for (unsigned i = 0; i < 100; i++) {
        boot_history_record_boot(7, false, 5, 0, false, "x-gabc", 0);
    }
    unsigned writes = fake_kv_set_call_count();
    TEST_CHECK(writes <= 8u, "FW-01: 100 identical boots cause at most 8 NVS writes");
    TEST_CHECK(writes >= 2u, "FW-01: the count is still flushed periodically (not only the first boot)");
    boot_history_blob_t b;
    boot_history_entry_t e;
    boot_history_load(&b);
    TEST_CHECK(b.count == 1 && boot_history_blob_get(&b, 0, &e) && e.seq == 1u, "FW-01: one entry, no new seq per repeat");
    TEST_CHECK(e.repeat >= 80 && e.repeat <= 99, "FW-01: persisted repeat count is within one flush of the truth");
    boot_history_entry_t r[BOOT_HISTORY_RECENT];
    TEST_CHECK(boot_history_recent(r) == 1u && r[0].repeat == 99, "FW-01: RAM cache has the exact repeat (99)");
    unsigned before = fake_kv_set_call_count();
    TEST_CHECK(boot_history_backfill_wall_time(1700000000u) != ESP_OK, "FW-02: coalesced boot refuses the backfill");
    TEST_CHECK(fake_kv_set_call_count() == before, "FW-02: and writes nothing");

    /* a different reason is a new entry, and folds the pending repeats into that single write */
    boot_history_record_boot(3, false, 5, 0, false, "x-gabc", 0);
    boot_history_load(&b);
    TEST_CHECK(b.count == 2, "FW-01: different reason starts a new entry");
    boot_history_blob_get(&b, 1, &e);
    TEST_CHECK(e.repeat == 99, "FW-01: pending repeats folded into the previous entry on the next real write");
    /* a different firmware is a new entry too */
    boot_history_record_boot(3, false, 5, 0, false, "x-gdef", 0);
    boot_history_load(&b);
    TEST_CHECK(b.count == 3, "FW-01: different firmware starts a new entry");
    /* v1 blob (524 B) is discarded cleanly */
    reset_all();
    uint8_t v1[524];
    memset(v1, 0, sizeof(v1));
    v1[0] = 1;
    v1[1] = 1;
    hal_kv_handle_t h;
    hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    hal_kv_set_blob(&h, NVS_KEY_BOOT_HIST, v1, sizeof(v1));
    hal_kv_commit(&h);
    hal_kv_close(&h);
    TEST_CHECK(!boot_history_load(&b) && b.count == 0, "FW-01: old v1 blob discarded to an empty ring");
    TEST_CHECK(boot_history_record_boot(1, false, 0, 0, false, "g", 0) == ESP_OK, "FW-01: first boot after a v1 blob records");
}

static void test_fv01_fv03(void)
{
    TEST_SECTION("boot_history: FV-01 unique per-boot id across coalesced boots, FV-03 crash boot gets its own entry");
    reset_all();
    uint32_t seen[8];
    for (unsigned i = 0; i < 6; i++) {
        boot_history_record_boot(7, false, 5, 0, false, "x-gabc", 0);
        seen[i] = (boot_history_latest_seq() << 8) | boot_history_latest_rep();
        for (unsigned j = 0; j < i; j++) {
            TEST_CHECK(seen[j] != seen[i], "FV-01: every coalesced boot has a distinct (seq, rep) id");
        }
    }
    TEST_CHECK(boot_history_latest_seq() == 1u, "FV-01: seq itself is still shared by the coalesced boots");
    boot_history_record_boot(7, false, 5, 0x11u, true, "x-gabc", 0);
    boot_history_blob_t b;
    boot_history_entry_t e;
    boot_history_load(&b);
    TEST_CHECK(b.count == 2 && boot_history_blob_get(&b, 0, &e) && e.has_crash == 1 && e.dump_id == 0x11u,
               "FV-03: same reason + fw but has_crash differs => distinct entry");
}

static void test_br01_br02(void)
{
    TEST_SECTION("boot_history: BR-01 readers merge the live repeat, BR-02 kiln_nvs stats JSON");
    reset_all();
    for (unsigned i = 0; i < 100; i++) {
        boot_history_record_boot(7, false, 5, 0, false, "x-gabc", 0);
    }
    boot_history_blob_t b;
    boot_history_entry_t e;
    unsigned before = fake_kv_set_call_count();
    TEST_CHECK(boot_history_load(&b) && boot_history_blob_get(&b, 0, &e) && e.repeat < 99, "BR-01: persisted repeat lags the truth");
    boot_history_merge_live(&b);
    TEST_CHECK(boot_history_blob_get(&b, 0, &e) && e.repeat == 99, "BR-01: merged repeat is the live count (99)");
    TEST_CHECK(fake_kv_set_call_count() == before, "BR-01: merge adds no NVS writes");
    boot_history_blob_init(&b);
    boot_history_merge_live(&b);
    TEST_CHECK(b.count == 0, "BR-01: empty ring untouched");
    char js[256];
    int n = boot_history_nvs_stats_json(js, sizeof(js));
    TEST_CHECK(n > 0 && strstr(js, "\"kiln_nvs\":{\"used\":") != NULL && strstr(js, "\"free\":") && strstr(js, "\"total\":"),
               "BR-02: stats JSON has used/free/total");
    TEST_CHECK(boot_history_nvs_stats_json(js, 8) == -1, "BR-02: too-small buffer reports -1");
}

static void test_bw01_write_counter(void)
{
    TEST_SECTION("boot_history: BW-01 RAM-only kiln_nvs per-key write counter");
    reset_all();
    hal_kv_wstat_reset();
    char js[512];
    char name[HAL_KV_WSTAT_NAME_MAX];
    uint32_t cnt = 0;
    TEST_CHECK(!hal_kv_wstat_get(0, name, &cnt), "BW-01: empty after reset");
    boot_history_record_boot(7, false, 5, 0, false, "x-gabc", 0);
    TEST_CHECK(hal_kv_wstat_get(0, name, &cnt) && strcmp(name, "boot_hist") == 0 && cnt == 1u,
               "BW-01: first boot record counts one boot_hist write");
    boot_history_record_boot(7, false, 5, 0, false, "x-gabc", 0);
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, "kiln_cfg", HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK, "BW-01: open");
    uint8_t v = 1;
    hal_kv_set_blob(&h, "bootguard", &v, 1);
    hal_kv_set_blob(&h, "bootguard", &v, 1);
    hal_kv_set_blob(&h, "trip_hist", &v, 1);
    hal_kv_close(&h);
    int n = boot_history_nvs_stats_json(js, sizeof(js));
    TEST_CHECK(n > 0 && strstr(js, "\"bootguard\":2") != NULL && strstr(js, "\"trip_hist\":1") != NULL &&
                   strstr(js, "\"writes_this_boot\":{") != NULL && js[n - 1] == '}',
               "BW-01: JSON carries per-key counts");
    hal_kv_wstat_note("other_part", "zzz");
    hal_kv_wstat_get(0, name, &cnt);
    uint32_t c0 = cnt;
    hal_kv_wstat_note("other_part", "boot_hist");
    hal_kv_wstat_get(0, name, &cnt);
    TEST_CHECK(cnt == c0, "BW-01: writes to other partitions are not counted");
    hal_kv_wstat_reset();
    const char *ks[] = {"k1", "k2", "k3", "k4", "k5", "k6", "k7", "k8"};
    for (unsigned i = 0; i < 8; i++) {
        hal_kv_wstat_note(KILN_NVS_PARTITION, ks[i]);
    }
    TEST_CHECK(hal_kv_wstat_get(HAL_KV_WSTAT_SLOTS, name, &cnt) && strcmp(name, "other") == 0 && cnt == 2u,
               "BW-01: keys past the slot table fold into other");
}

int main(void)
{
    test_bw01_write_counter();
    test_br01_br02();
    test_fv01_fv03();
    test_fw01_fw02_coalesce();
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
