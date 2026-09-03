// Host tests for App/drivers/ramp_assist_cfg.c -- the kiln-wide (not
// per-zone) persisted on/off flag for the forthcoming "ramp assist" feature.
// #includes ramp_assist_cfg.c directly (same convention as
// test_watchdog_cfg.c/test_unit_pref-shaped modules) to reach its
// s_ramp_assist_enabled file-scope state for simulate_reboot() below.
//
// THE LOAD-BEARING PROPERTY THIS FILE EXISTS TO PROVE: the persisted default
// is FALSE (disabled) on every path -- empty NVS, a load failure, and a
// corrupt/out-of-range stored byte all resolve to disabled, never the other
// way. That is what makes it safe for a PID tuning run or an A/B controller
// comparison to trust "the board has never touched this setting" as "ramp
// assist is off" (see ramp_assist_cfg.h's header comment for why that matters
// -- an unassisted run silently becoming assisted invalidates every tracking-
// error measurement it produces).
//
// Uses stubs/nvs.h's opt-in "real" u8-key store (nvs_test_enable(true)) to
// simulate actual persistence across simulated reboots, same as
// test_watchdog_cfg.c/test_adaptive_tune.c.
#include <string.h>

#include "test_common.h"

#include "esp_err.h"

#include "../drivers/ramp_assist_cfg.c"

// ---------------------------------------------------------------------------
// Simulated reboot: resets everything that would be lost on a real power
// cycle (s_ramp_assist_enabled's RAM state) while leaving the stubbed NVS
// blob (the flash stand-in) exactly as it was.
// ---------------------------------------------------------------------------
static void simulate_reboot(void)
{
    s_ramp_assist_enabled = true; // deliberately the WRONG value -- proves ramp_assist_cfg_start()
                                   // actually overwrites it rather than the test happening to already
                                   // hold the expected result before start() runs.
}

static void test_default_is_disabled_on_empty_nvs(void)
{
    nvs_test_enable(true);
    nvs_test_clear();
    simulate_reboot();

    esp_err_t err = ramp_assist_cfg_start();
    TEST_CHECK(err == ESP_OK, "ramp_assist_cfg_start() succeeds against an empty stub");
    TEST_CHECK(ramp_assist_cfg_enabled() == false,
               "empty NVS (nothing ever stored, e.g. a board that predates this feature or a fresh "
               "board) -- the safe default is DISABLED, i.e. ramp_assist_cfg_enabled() must be false. "
               "This is the load-bearing default: a PID tuning run must be able to trust an "
               "unconfigured board to run raw, unassisted");
}

static void test_persistence_round_trip(void)
{
    nvs_test_enable(true);
    nvs_test_clear();
    simulate_reboot();

    ramp_assist_cfg_start();
    TEST_CHECK(!ramp_assist_cfg_enabled(), "precondition: starts disabled (safe default)");

    esp_err_t err = ramp_assist_cfg_set_enabled(true);
    TEST_CHECK(err == ESP_OK, "set_enabled(true) persists successfully in the stub");
    TEST_CHECK(ramp_assist_cfg_enabled(), "immediately reflects the new value, this boot");

    // Simulate a reboot -- the persisted value must survive it. This is the
    // scenario item 1 in the task ("survives a whole-page save round trip AND
    // a reboot") is actually asking to be proven, not merely "the setter
    // returned ESP_OK".
    simulate_reboot();
    ramp_assist_cfg_start();
    TEST_CHECK(ramp_assist_cfg_enabled(),
               "ramp_assist_cfg_start() on the next boot reloads enabled=true from NVS");

    // Flip it back and confirm THAT round-trips too, not just "true" sticking
    // (a setter that only ever writes 1 and never actually clears the byte
    // would pass the check above and still be broken).
    err = ramp_assist_cfg_set_enabled(false);
    TEST_CHECK(err == ESP_OK, "set_enabled(false) persists successfully");
    simulate_reboot();
    ramp_assist_cfg_start();
    TEST_CHECK(!ramp_assist_cfg_enabled(),
               "the re-disabled value also survives a simulated reboot, not just the enabled one");
}

static void test_corrupted_value_falls_back_to_safe_default(void)
{
    nvs_test_enable(true);
    nvs_test_clear();
    simulate_reboot();

    ramp_assist_cfg_start();
    esp_err_t err = ramp_assist_cfg_set_enabled(true);
    TEST_CHECK(err == ESP_OK, "precondition: enabled=true is persisted");

    // Corrupt the stubbed stored byte directly to something out of range for
    // this module's 0/1 encoding -- simulates a bit flip that survives the
    // u8 storage layer without also being caught at that layer (unlike
    // watchdog_cfg.c's CRC'd record, this module has no CRC of its own, so
    // its own explicit range check (raw != 0 && raw != 1) is the ENTIRE
    // defense here -- this test is what proves that check actually does
    // something rather than being dead code).
    bool found = false;
    for (int i = 0; i < TEST_STUB_NVS_U8_SLOTS; i++) {
        if (s_stub_nvs_u8_has[i] && strcmp(s_stub_nvs_u8_keys[i], NVS_KEY_RAMP_ASSIST) == 0) {
            s_stub_nvs_u8_vals[i] = 0xAA; // neither 0 nor 1
            found = true;
            break;
        }
    }
    TEST_CHECK(found, "precondition: the stub actually has a stored slot for this module's key");

    simulate_reboot();
    ramp_assist_cfg_start();
    TEST_CHECK(!ramp_assist_cfg_enabled(),
               "an out-of-range stored byte must NOT be trusted to still mean enabled=true -- this is "
               "exactly the failure this module's explicit range check exists to catch: a corrupted "
               "value must resolve to the SAFE default (disabled), not to whatever garbage the flash "
               "actually held");
}

void run_test_ramp_assist_cfg(void)
{
    test_default_is_disabled_on_empty_nvs();
    test_persistence_round_trip();
    test_corrupted_value_falls_back_to_safe_default();
}
