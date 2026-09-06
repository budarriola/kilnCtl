// Host tests for App/drivers/safety/watchdog_cfg.c -- the persisted "operator
// disabled the task-watchdog panic" switch behind the owner's bench-
// debugging request. #includes watchdog_cfg.c directly (same convention as
// test_boot_guard.c/test_kiln_cfg_store.c) to reach its static
// crc32_compute()/record_crc()/record_is_valid()/persist_disabled_flag()/
// load_disabled_flag() helpers and its s_wd file-scope state directly.
//
// esp_task_wdt_reconfigure() itself is NOT exercised here -- there is no
// real TWDT on the host, and apply_panic_disabled() is a thin, side-effect-
// only wrapper around it. What IS exercised, and matters far more for this
// module's own safety property, is the load/persist/CRC path: a corrupted
// or missing record must always resolve to panic ENABLED, never the other
// way.
//
// Uses fake_kv.h's RAM-backed hal_kv fake to simulate actual persistence
// across simulated reboots (HW_ABSTRACTION_PLAN.md Phase 3 item 3, the
// nvs.h -> hal_kv.h migration; this file previously used stubs/nvs.h's
// opt-in "real" blob store, same as test_boot_guard.c's original form).
#include <string.h>

#include "test_common.h"

#include "esp_err.h"

#include "fake_kv.h"
#include "fake_wdt.h"

#include "../drivers/safety/watchdog_cfg.c"

// ---------------------------------------------------------------------------
// Simulated reboot: resets everything that would be lost on a real power
// cycle (s_wd's RAM state) while leaving the stubbed NVS blob (the flash
// stand-in) exactly as it was.
// ---------------------------------------------------------------------------
static void simulate_reboot(void)
{
    s_wd.initialized = false;
    s_wd.panic_disabled = false;
}

static void test_crc32_reference_vector(void)
{
    // Standard CRC32 (IEEE 802.3/zlib) test vector: crc32("123456789") == 0xCBF43926.
    uint32_t crc = crc32_compute("123456789", 9);
    TEST_CHECK(crc == 0xCBF43926u, "crc32_compute reference vector");
}

static void test_record_crc_detects_corruption(void)
{
    watchdog_cfg_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.version = WATCHDOG_CFG_RECORD_VERSION;
    rec.panic_disabled = 1;
    rec.crc32 = record_crc(&rec);

    TEST_CHECK(record_is_valid(&rec), "freshly-computed CRC validates");

    // Flip the panic_disabled byte -- the CRC must catch this. THE negative
    // test for the CRC check itself: without it (e.g. if record_is_valid()
    // only checked `version`), a corrupted-to-1 bit would be trusted, which
    // is exactly the failure mode this module exists to avoid (a hang no
    // longer reboots the board, silently, because of a flash bit flip).
    rec.panic_disabled ^= 0xFF;
    TEST_CHECK(!record_is_valid(&rec), "a corrupted panic_disabled byte is rejected by the CRC");

    // Restore, corrupt the version instead.
    rec.panic_disabled = 1;
    rec.version = WATCHDOG_CFG_RECORD_VERSION + 1;
    TEST_CHECK(!record_is_valid(&rec), "a version mismatch is rejected");
}

static void test_default_is_panic_enabled_on_empty_nvs(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();

    watchdog_cfg_init();
    TEST_CHECK(watchdog_cfg_panic_disabled() == false,
               "empty NVS (nothing ever stored) -- the safe default is panic ENABLED, i.e. "
               "watchdog_cfg_panic_disabled() must be false");
}

static void test_persistence_round_trip(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();

    watchdog_cfg_init();
    TEST_CHECK(!watchdog_cfg_panic_disabled(), "precondition: starts enabled (safe default)");

    esp_err_t err = watchdog_cfg_set_panic_disabled(true, "test");
    TEST_CHECK(err == ESP_OK, "set_panic_disabled(true) persists successfully in the stub");
    TEST_CHECK(watchdog_cfg_panic_disabled(), "immediately reflects the new value, this boot");

    // Simulate a reboot -- the persisted value must survive it.
    simulate_reboot();
    watchdog_cfg_init();
    TEST_CHECK(watchdog_cfg_panic_disabled(),
               "watchdog_cfg_init() on the next boot reloads panic_disabled=true from NVS");

    // Flip it back and confirm THAT round-trips too, not just "true" sticking.
    err = watchdog_cfg_set_panic_disabled(false, "test");
    TEST_CHECK(err == ESP_OK, "set_panic_disabled(false) persists successfully");
    simulate_reboot();
    watchdog_cfg_init();
    TEST_CHECK(!watchdog_cfg_panic_disabled(),
               "the re-enabled value also survives a simulated reboot, not just the disabled one");
}

static void test_corrupted_record_falls_back_to_safe_default(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();

    watchdog_cfg_init();
    esp_err_t err = watchdog_cfg_set_panic_disabled(true, "test");
    TEST_CHECK(err == ESP_OK, "precondition: panic_disabled=true is persisted");

    // Corrupt the persisted blob by reading it back, flipping a byte, and
    // writing the flipped bytes back through the real hal_kv_get_blob/
    // set_blob/commit round trip -- simulates a brownout-mid-write / bit-flip
    // without needing fake_kv-internal storage access (see
    // test_boot_guard.c's identical approach and comment).
    watchdog_cfg_record_t corrupt_rec;
    size_t corrupt_len = sizeof(corrupt_rec);
    hal_kv_handle_t corrupt_h;
    TEST_CHECK(hal_kv_open(&corrupt_h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
               "precondition: the watchdog_cfg namespace opens");
    TEST_CHECK(hal_kv_get_blob(&corrupt_h, NVS_KEY_REC, &corrupt_rec, &corrupt_len) == HAL_OK
                   && corrupt_len == sizeof(corrupt_rec),
               "precondition: something is actually stored");
    ((uint8_t *)&corrupt_rec)[1] ^= 0xFF;
    TEST_CHECK(hal_kv_set_blob(&corrupt_h, NVS_KEY_REC, &corrupt_rec, sizeof(corrupt_rec)) == HAL_OK
                   && hal_kv_commit(&corrupt_h) == HAL_OK,
               "corrupted bytes written back");
    hal_kv_close(&corrupt_h);

    simulate_reboot();
    watchdog_cfg_init();
    TEST_CHECK(!watchdog_cfg_panic_disabled(),
               "a corrupted record must NOT be trusted to still say panic_disabled=true -- this is "
               "the exact failure this module's CRC exists to catch: a corrupted-to-garbage record "
               "must resolve to the SAFE default (panic ENABLED), not to whatever the bit flip left "
               "behind");
}

// HAL Phase 3 item 7: apply_panic_disabled() now calls
// hal_wdt_set_panic_disabled() (interface/hal_wdt.h) instead of
// esp_task_wdt_reconfigure() directly. This is the one place this module's
// host tests can now observe a real HAL side effect (against fake_wdt.c)
// rather than only the load/persist/CRC path around it -- confirms the
// live value set through watchdog_cfg_set_panic_disabled() actually reaches
// the HAL, not just s_wd's own RAM copy.
static void test_set_panic_disabled_reaches_hal_wdt(void)
{
    fake_wdt_reset_all();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    simulate_reboot();
    watchdog_cfg_init();

    esp_err_t err = watchdog_cfg_set_panic_disabled(true, "test");
    TEST_CHECK(err == ESP_OK, "set_panic_disabled(true) succeeds");
    TEST_CHECK(fake_wdt_get_panic_disabled() == true,
               "hal_wdt_set_panic_disabled(true) reached the HAL (fake_wdt) -- the migrated call, "
               "not just watchdog_cfg's own RAM copy");

    err = watchdog_cfg_set_panic_disabled(false, "test");
    TEST_CHECK(err == ESP_OK, "set_panic_disabled(false) succeeds");
    TEST_CHECK(fake_wdt_get_panic_disabled() == false,
               "hal_wdt_set_panic_disabled(false) also reached the HAL, not just the initial call");
}

void run_test_watchdog_cfg(void)
{
    test_crc32_reference_vector();
    test_record_crc_detects_corruption();
    test_default_is_panic_enabled_on_empty_nvs();
    test_persistence_round_trip();
    test_corrupted_record_falls_back_to_safe_default();
    test_set_panic_disabled_reaches_hal_wdt();
}
