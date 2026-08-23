// Host tests for App/drivers/autotune_engine.c's pre-start guard.
//
// Same recovery-mode hazard as test_profile_executor_prestart.c documents in
// full: boot_guard.h's recovery mode skips autotune_engine_start(), but
// other code that DOES still run can call into this module's public API
// anyway, and every entry point used to take s_at.lock -- a FreeRTOS mutex
// that does not exist until autotune_engine_start() creates it. Taking a
// NULL mutex asserts inside FreeRTOS and panics the board (the same failure
// class profile_executor.c hit on the bench -- see that file's tests for the
// captured backtrace). begin_run_locked() (the shared entry both
// autotune_engine_run() and autotune_engine_run_relay() funnel through) and
// every other public function now test s_at.lock == NULL as their first
// statement and return a clean "not running" answer instead.
//
// #includes autotune_engine.c directly (same convention as
// test_profile_executor_prestart.c) -- see that file's header comment for
// why the stub surface below is wide even though these tests only ever
// reach the guard itself. Own executable for the same "defines the real
// zones_config_*() bodies, would multiply-define against other host tests'
// fakes" reason.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "test_common.h"

#include "esp_err.h"

// Own executable (see this file's header comment).
int g_test_failures = 0;
int g_test_count = 0;

#include "../drivers/autotune_engine.c"

// ---------------------------------------------------------------------------
// Stub bodies for every extern symbol autotune_engine.c references that
// isn't linked in for real (see build_host_tests.ps1 for this executable).
// None of these is ever actually invoked by the tests below -- every call
// site sits behind the s_at.lock == NULL guard under test -- but the whole
// translation unit must still link.
// ---------------------------------------------------------------------------

esp_err_t MAX31856_read_all(MAX31856BusClass *bus, MAX31856Reading *out, size_t max_readings, size_t *out_count)
{
    (void)bus; (void)out; (void)max_readings;
    if (out_count) *out_count = 0;
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

bool profile_executor_run(uint8_t profile_id, char *err_msg, size_t err_cap)
{
    (void)profile_id;
    if (err_msg && err_cap) err_msg[0] = '\0';
    return false;
}

bool profile_executor_zone_is_active(uint8_t zone_index)
{
    (void)zone_index;
    return false;
}

bool relay_authority_on_blocked(SafetyLinkClass *safety, uint32_t *out_sources)
{
    (void)safety;
    if (out_sources) *out_sources = 0;
    return false;
}

bool relay_authority_zone_blocked(SafetyLinkClass *safety, uint8_t zone_index, uint32_t *out_sources)
{
    (void)safety; (void)zone_index;
    if (out_sources) *out_sources = 0;
    return false;
}

void relay_authority_set_zone_blocked(uint8_t zone_index, bool blocked)
{
    (void)zone_index; (void)blocked;
}

void relay_authority_claim_mask(uint8_t relay_mask, relay_owner_t owner)
{
    (void)relay_mask; (void)owner;
}

void relay_authority_release_mask(uint8_t relay_mask)
{
    (void)relay_mask;
}

esp_err_t safety_link_set_fault_source(SafetyLinkClass *link, uint32_t source_mask, bool assert_fault)
{
    (void)link; (void)source_mask; (void)assert_fault;
    return ESP_OK;
}

float zones_config_apply_cal(uint8_t zone_index, float raw_c)
{
    (void)zone_index;
    return raw_c;
}

bool zones_config_get_guard_thresholds(uint8_t zone_index, float *o1, float *o2, float *o3, float *o4, float *o5,
                                       float *o6, float *o7, float *o8)
{
    (void)zone_index;
    if (o1) *o1 = 0; if (o2) *o2 = 0; if (o3) *o3 = 0; if (o4) *o4 = 0;
    if (o5) *o5 = 0; if (o6) *o6 = 0; if (o7) *o7 = 0; if (o8) *o8 = 0;
    return false;
}

bool zones_config_get_heater_cfg(uint8_t zone_index, float *out_window_ms, float *out_min_on_ms, float *out_min_off_ms)
{
    (void)zone_index;
    if (out_window_ms) *out_window_ms = 0.0f;
    if (out_min_on_ms) *out_min_on_ms = 0.0f;
    if (out_min_off_ms) *out_min_off_ms = 0.0f;
    return false;
}

bool zones_config_get_relay_mask(uint8_t zone_index, uint8_t *out_mask)
{
    (void)zone_index;
    if (out_mask) *out_mask = 0;
    return false;
}

bool zones_config_get_sanity_rate(uint8_t zone_index, float *out_c_per_min)
{
    (void)zone_index;
    if (out_c_per_min) *out_c_per_min = 0.0f;
    return false;
}

bool zones_config_get_temp_limits(uint8_t zone_index, float *out_max_temp_c, float *out_min_temp_c)
{
    (void)zone_index;
    if (out_max_temp_c) *out_max_temp_c = 0.0f;
    if (out_min_temp_c) *out_min_temp_c = 0.0f;
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

bool zones_config_is_valid(void)
{
    return false;
}

bool zones_config_set_model(uint8_t zone_index, float k_dc, float tau_s, float dead_time_s)
{
    (void)zone_index; (void)k_dc; (void)tau_s; (void)dead_time_s;
    return false;
}

bool zones_config_set_pid(uint8_t zone_index, float kp, float ki, float kd)
{
    (void)zone_index; (void)kp; (void)ki; (void)kd;
    return false;
}

// ---------------------------------------------------------------------------
// Tests -- autotune_engine_start() is DELIBERATELY never called anywhere in
// this file. s_at is a static struct with internal linkage in
// autotune_engine.c, zero-initialized by the C runtime before main() runs,
// so s_at.lock reads NULL here exactly as it does on a real board that has
// skipped autotune_engine_start() for recovery mode.
// ---------------------------------------------------------------------------

static void test_run_refuses_before_start(void)
{
    TEST_SECTION("autotune_engine_run() before start() -- refused, no crash");
    char err[96] = {0};
    bool ok = autotune_engine_run(0, 0.5f, err, sizeof(err));
    TEST_CHECK(!ok, "must refuse, not crash, when s_at.lock is NULL");
    TEST_CHECK(err[0] != '\0', "an error message is filled in for the caller");
}

static void test_run_relay_refuses_before_start(void)
{
    TEST_SECTION("autotune_engine_run_relay() before start() -- refused, no crash");
    char err[96] = {0};
    bool ok = autotune_engine_run_relay(0, 900.0f, 0.0f, 0.0f, AUTOTUNE_RULE_TYREUS_LUYBEN, err, sizeof(err));
    TEST_CHECK(!ok, "must refuse, not crash, when s_at.lock is NULL");
    TEST_CHECK(err[0] != '\0', "an error message is filled in for the caller");
}

static void test_abort_is_a_silent_noop_before_start(void)
{
    TEST_SECTION("autotune_engine_abort() before start() -- returns, no crash");
    autotune_engine_abort("test"); // must simply return
    TEST_CHECK(true, "reached this line without crashing");
}

static void test_accept_refuses_before_start(void)
{
    TEST_SECTION("autotune_engine_accept() before start() -- refused, no crash");
    TEST_CHECK(!autotune_engine_accept(), "accept must return false, not crash");
}

static void test_is_active_false_before_start(void)
{
    TEST_SECTION("autotune_engine_is_active()/is_active_on_zone() before start() -- false, no crash");
    TEST_CHECK(!autotune_engine_is_active(), "engine cannot be active before it has ever started");
    TEST_CHECK(!autotune_engine_is_active_on_zone(0), "no zone can be active before the engine has ever started");
}

static void test_get_trace_empty_before_start(void)
{
    TEST_SECTION("autotune_engine_get_trace() before start() -- empty, no crash");
    autotune_sample_t buf[4];
    size_t n = autotune_engine_get_trace(buf, 0, 4);
    TEST_CHECK(n == 0, "get_trace must copy nothing before the engine has ever started");
}

static void test_get_coupling_matrix_empty_before_start(void)
{
    TEST_SECTION("autotune_engine_get_coupling_matrix() before start() -- all-invalid, no crash");
    autotune_coupling_matrix_t m;
    memset(&m, 0xAA, sizeof(m));
    autotune_engine_get_coupling_matrix(&m);
    bool any_valid = false;
    for (int i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        for (int j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
            if (m.cell[i][j].valid) any_valid = true;
        }
    }
    TEST_CHECK(!any_valid, "no coupling cell may read valid before the engine has ever started");
}

static void test_get_status_reports_well_formed_idle_before_start(void)
{
    TEST_SECTION("autotune_engine_get_status() before start() -- well-formed IDLE snapshot, no crash");
    // Poisoned first, same reasoning as profile_executor's equivalent test:
    // a pass here proves the function zeroed/filled the struct itself.
    autotune_engine_status_t out;
    memset(&out, 0xAA, sizeof(out));

    autotune_engine_get_status(&out);

    TEST_CHECK(out.state == AUTOTUNE_ENGINE_IDLE, "state must read IDLE, the module's existing idle sentinel");
    TEST_CHECK(out.zone_index == 0, "zone_index must not carry poisoned stack bytes");
    TEST_CHECK(out.elapsed_s == 0, "elapsed_s must not carry poisoned stack bytes");
    TEST_CHECK(out.sample_count == 0, "sample_count must not carry poisoned stack bytes");
    TEST_CHECK(out.abort_reason[0] == '\0', "abort_reason must not carry poisoned stack bytes");
}

void run_test_autotune_engine_prestart(void)
{
    test_run_refuses_before_start();
    test_run_relay_refuses_before_start();
    test_abort_is_a_silent_noop_before_start();
    test_accept_refuses_before_start();
    test_is_active_false_before_start();
    test_get_trace_empty_before_start();
    test_get_coupling_matrix_empty_before_start();
    test_get_status_reports_well_formed_idle_before_start();
}

int main(void)
{
    run_test_autotune_engine_prestart();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
