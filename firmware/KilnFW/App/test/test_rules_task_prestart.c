// Host tests for App/drivers/rules_task.c's pre-start contract.
//
// Unlike profile_executor.c and autotune_engine.c, rules_task.c holds no
// FreeRTOS mutex at all (see App/drivers/rules_task.h's own doc comment on
// rules_task_get_status(): "Always succeeds even if rules_task_start() was
// never called or failed"). s_rules_task is a static struct with internal
// linkage, zero-initialized by the C runtime before main() runs -- the same
// state rules_task_start()'s own memset(&s_rules_task, 0, ...) puts it back
// into -- and rules_task_get_status() only ever copies *out = s_rules_task.status
// with no lock and no NULL-handle dereference anywhere in the read path. So
// there is nothing to guard here: this file exists to PROVE that claim by
// calling the public API before rules_task_start() has ever run, the same
// way test_profile_executor_prestart.c/test_autotune_engine_prestart.c prove
// their modules' guards, rather than to add a guard rules_task.c does not
// need. If a future change to this module ever adds a lock/handle that
// isn't NULL-safe before rules_task_start(), this test is what should catch
// it.
//
// #includes rules_task.c directly (same convention as the other two
// pre-start test files) so the real static s_rules_task storage and the
// real rules_task_get_status() are under test, not a hand-rolled copy. Own
// executable for the same "defines real zones_config_*()/etc bodies, would
// multiply-define against other host tests' fakes" reason.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "test_common.h"

#include "esp_err.h"

// Own executable (see this file's header comment).
int g_test_failures = 0;
int g_test_count = 0;

#include "../drivers/rules_task.c"

// ---------------------------------------------------------------------------
// Stub bodies for every extern symbol rules_task.c references that isn't
// linked in for real (see build_host_tests.ps1 for this executable). None of
// these is ever actually invoked by the test below -- rules_task_start() is
// never called, so rules_task_entry()/rules_watchdog_entry() never run, and
// rules_task_get_status() itself calls nothing -- but the whole translation
// unit must still link.
// ---------------------------------------------------------------------------

esp_err_t kiln_io_owner_command_read(kiln_io_state_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    return ESP_FAIL;
}

esp_err_t kiln_io_owner_command_set_relay_mask_authorized(uint8_t mask, uint8_t value)
{
    (void)mask; (void)value;
    return ESP_OK;
}

bool ota_http_heat_blocked_by_update(char *reason_out, size_t reason_cap)
{
    if (reason_out && reason_cap) reason_out[0] = '\0';
    return false;
}

void profile_executor_get_status(profile_exec_status_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
}

void relay_authority_claim_mask(uint8_t relay_mask, relay_owner_t owner)
{
    (void)relay_mask; (void)owner;
}

relay_owner_t relay_authority_get_owner(uint8_t relay_index)
{
    (void)relay_index;
    return RELAY_OWNER_NONE;
}

bool relay_authority_on_blocked(SafetyLinkClass *safety, uint32_t *out_sources)
{
    (void)safety;
    if (out_sources) *out_sources = 0;
    return false;
}

void relay_authority_release_mask(uint8_t relay_mask)
{
    (void)relay_mask;
}

void rules_http_get_cfg(rules_cfg_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
}

esp_err_t thermo_owner_command_read_all(MAX31856Reading *out, size_t max_readings, size_t *out_count)
{
    (void)out; (void)max_readings;
    if (out_count) *out_count = 0;
    return ESP_FAIL;
}

float zones_config_apply_cal(uint8_t zone_index, float raw_c)
{
    (void)zone_index;
    return raw_c;
}

bool zones_config_get_relay_mask(uint8_t zone_index, uint8_t *out_mask)
{
    (void)zone_index;
    if (out_mask) *out_mask = 0;
    return false;
}

uint8_t zones_config_get_thermo_count(void)
{
    return 0;
}

bool zones_config_get_thermo_mask(uint8_t zone_index, uint8_t *out_mask)
{
    (void)zone_index;
    if (out_mask) *out_mask = 0;
    return false;
}

// ---------------------------------------------------------------------------
// Test -- rules_task_start() is DELIBERATELY never called anywhere in this
// file. s_rules_task reads zero exactly as it would on a real board that
// skipped rules_task_start() for recovery mode.
// ---------------------------------------------------------------------------

static void test_get_status_reports_well_formed_inert_before_start(void)
{
    TEST_SECTION("rules_task_get_status() before start() -- well-formed inert snapshot, no crash");
    // Poisoned first, same reasoning as the other two modules' equivalent
    // tests: a pass here proves the read path itself is zero/well-formed,
    // not that the caller's stack happened to already be zero.
    rules_task_status_t out;
    memset(&out, 0xAA, sizeof(out));

    rules_task_get_status(&out);

    TEST_CHECK(!out.safety_link_ok, "safety_link_ok must not carry poisoned stack bytes");
    TEST_CHECK(!out.heat_interlock_ok, "heat_interlock_ok must not carry poisoned stack bytes");
    TEST_CHECK(!out.watchdog_forced_off, "watchdog_forced_off must not carry poisoned stack bytes");
    for (int i = 0; i < RULES_EVAL_RELAY_COUNT; i++) {
        TEST_CHECK(!out.relays[i].rule_driven, "relay rule_driven must read false before rules_task_start()");
        TEST_CHECK(!out.relays[i].rule_wants_on, "relay rule_wants_on must read false before rules_task_start()");
        TEST_CHECK(!out.relays[i].commanded_on, "relay commanded_on must read false before rules_task_start()");
        TEST_CHECK(!out.relays[i].owned_by_rules, "relay owned_by_rules must read false before rules_task_start()");
        TEST_CHECK(!out.relays[i].heater_owned, "relay heater_owned must read false before rules_task_start()");
    }
}

void run_test_rules_task_prestart(void)
{
    test_get_status_reports_well_formed_inert_before_start();
}

int main(void)
{
    run_test_rules_task_prestart();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
