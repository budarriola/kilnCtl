// Host tests for App/drivers/watchdog_cfg.c -- the persisted "operator
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
// Uses stubs/nvs.h's opt-in "real" blob store (nvs_test_enable(true)) to
// simulate actual persistence across simulated reboots -- see that header's
// own comment (added originally for test_safety_cfg_store.c) and
// test_boot_guard.c's identical use of it.
#include <string.h>

#include "test_common.h"

#include "esp_err.h"

#include "../drivers/watchdog_cfg.c"

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
    nvs_test_enable(true);
    nvs_test_clear();
    simulate_reboot();

    watchdog_cfg_init();
    TEST_CHECK(watchdog_cfg_panic_disabled() == false,
               "empty NVS (nothing ever stored) -- the safe default is panic ENABLED, i.e. "
               "watchdog_cfg_panic_disabled() must be false");
}

static void test_persistence_round_trip(void)
{
    nvs_test_enable(true);
    nvs_test_clear();
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
    nvs_test_enable(true);
    nvs_test_clear();
    simulate_reboot();

    watchdog_cfg_init();
    esp_err_t err = watchdog_cfg_set_panic_disabled(true, "test");
    TEST_CHECK(err == ESP_OK, "precondition: panic_disabled=true is persisted");

    TEST_CHECK(s_stub_nvs_has_blob, "precondition: something is actually stored");
    // Corrupt the stubbed "flash" blob directly -- flip a bit in the middle
    // of the stored bytes, simulating a brownout-mid-write / bit-flip.
    s_stub_nvs_blob[1] ^= 0xFF;

    simulate_reboot();
    watchdog_cfg_init();
    TEST_CHECK(!watchdog_cfg_panic_disabled(),
               "a corrupted record must NOT be trusted to still say panic_disabled=true -- this is "
               "the exact failure this module's CRC exists to catch: a corrupted-to-garbage record "
               "must resolve to the SAFE default (panic ENABLED), not to whatever the bit flip left "
               "behind");
}

void run_test_watchdog_cfg(void)
{
    test_crc32_reference_vector();
    test_record_crc_detects_corruption();
    test_default_is_panic_enabled_on_empty_nvs();
    test_persistence_round_trip();
    test_corrupted_record_falls_back_to_safe_default();
}
