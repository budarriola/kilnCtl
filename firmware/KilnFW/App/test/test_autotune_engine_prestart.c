// Host tests for App/drivers/autotune_engine.c's pre-start guard, PLUS (added
// 2026-08-24) STEPPING-loop guard-coverage tests for the thermal_guard
// setpoint synthesis defect -- see that section below for the full story.
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
// why the stub surface below is wide even though most of the tests below
// only ever reach the guard itself. Own executable for the same "defines the
// real zones_config_*() bodies, would multiply-define against other host
// tests' fakes" reason.
#include <math.h>
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

// ---------------------------------------------------------------------------
// STEPPING-loop guard-coverage tests (autotune_engine_tick_locked()) below
// need a controllable channel-0 reading instead of this stub's original
// unconditional ESP_FAIL/count=0 -- see run_stepping_ticks() and the tests
// that call it. Defaults (sensor ok, 0.0C) are inert for every OTHER test in
// this file: none of them ever reaches this code path (see the file header
// comment), so nothing about the prestart tests above changes.
// ---------------------------------------------------------------------------
static float s_stub_ch0_temp_c = 0.0f;
static bool  s_stub_ch0_ok = true;

esp_err_t MAX31856_read_all(MAX31856BusClass *bus, MAX31856Reading *out, size_t max_readings, size_t *out_count)
{
    (void)bus;
    if (max_readings < 1) {
        if (out_count) *out_count = 0;
        return ESP_OK;
    }
    memset(&out[0], 0, sizeof(out[0]));
    out[0].channel = 0;
    out[0].spi_failed = !s_stub_ch0_ok;
    out[0].fault_status = 0;
    out[0].tc_temperature_c = s_stub_ch0_ok ? s_stub_ch0_temp_c : NAN;
    out[0].cj_temperature_c = 25.0f;
    if (out_count) *out_count = 1;
    return ESP_OK;
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
    // A nonzero mask so begin_run_locked() (real code, called for real by
    // the STEPPING-loop tests below) doesn't refuse the run with "zone has
    // no relay mask configured" -- unreached by the prestart tests (see
    // this file's header comment), so not a behavior change for them.
    (void)zone_index;
    if (out_mask) *out_mask = 0x01;
    return true;
}

bool zones_config_get_sanity_rate(uint8_t zone_index, float *out_c_per_min)
{
    (void)zone_index;
    if (out_c_per_min) *out_c_per_min = 0.0f;
    return false;
}

// Configurable per-test (default 0.0f, i.e. this stub's original hardcoded
// behavior) via set_stub_max_temp_c() below -- see that function's comment
// for why this needed to change from an unconditional 0.0f.
static float s_stub_max_temp_c = 0.0f;

bool zones_config_get_temp_limits(uint8_t zone_index, float *out_max_temp_c, float *out_min_temp_c)
{
    (void)zone_index;
    if (out_max_temp_c) *out_max_temp_c = s_stub_max_temp_c;
    if (out_min_temp_c) *out_min_temp_c = 0.0f;
    return false;
}

uint8_t zones_config_get_thermo_count(void)
{
    return 0;
}

bool zones_config_get_thermo_mask(uint8_t zone_index, uint8_t *out_mask)
{
    // Zone 0 -> channel 0 (bit 0), matching MAX31856_read_all()'s stub
    // above and every STEPPING-loop test below, which all test zone 0.
    // Unreached by the prestart tests (see this file's header comment), so
    // this is not a behavior change for them.
    (void)zone_index;
    if (out_mask) *out_mask = 0x01;
    return true;
}

bool zones_config_is_valid(void)
{
    // True so begin_run_locked() (real code, called for real by the
    // STEPPING-loop tests below) doesn't refuse the run with "zone config
    // failed to load" -- unreached by the prestart tests (see this file's
    // header comment), so not a behavior change for them.
    return true;
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
// Pre-start tests -- autotune_engine_start() is DELIBERATELY never called by
// any of these. s_at is a static struct with internal linkage in
// autotune_engine.c, zero-initialized by the C runtime before main() runs,
// so s_at.lock reads NULL here exactly as it does on a real board that has
// skipped autotune_engine_start() for recovery mode.
//
// MUST run before the STEPPING-loop section further down, which DOES call
// autotune_engine_start() -- run_test_autotune_engine_prestart() enforces
// that ordering. Once started, s_at.lock is a real (if stub) mutex and the
// "before start" invariant these tests exist to prove no longer holds.
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

// ---------------------------------------------------------------------------
// STEPPING-loop guard-coverage tests.
//
// The defect: autotune_engine.c's task_entry() (now factored into
// autotune_engine_tick_locked(), see that function's own comment) built the
// thermal_guard_input_t setpoint for a STEP-TEST run as
//   guard_cfg.max_temp_c > 0.0f ? guard_cfg.max_temp_c : raw_c
// -- i.e. when a zone has no configured ceiling (max_temp_c == 0, the
// default for a zone the operator has never set one on, and a documented,
// deliberately non-blocking readiness state -- readiness_http.c's
// "guard_max_temp" item), the fallback was the CURRENT reading itself. That
// pins thermal_guard's `error = setpoint_c - measurement_c` at exactly 0.0f
// every tick, and error > 0.0f is guard 1's (HEATING_FAILED) branch
// selector -- so guard 1 could never fire, for the run's full 4h budget,
// with duty legitimately on. A dead element or a flat-but-plausible
// thermocouple went undetected.
//
// The fix (autotune_engine.c, STEP_TEST_GUARD_HEADROOM_C and its use site's
// comment): fall back to raw_c + a fixed positive headroom instead of bare
// raw_c, keeping error strictly positive so guard 1 -- not guard 2, whose
// own math turns out to still work correctly here but whose "falling faster
// than threshold" test is narrower than guard 1's "didn't rise enough",
// which already subsumes it -- stays live.
//
// These tests drive autotune_engine_tick_locked() directly rather than
// through task_entry()'s real FreeRTOS task: this host build's
// xTaskGetTickCount() stub always returns 0 (stubs/freertos/task.h), which
// freezes s_at.elapsed_s at 0 forever and would make the real
// SETTLING->STEPPING transition (gated on s_at.elapsed_s) never fire. The
// tests jump s_at.state to STEPPING directly instead -- white-box, same
// convention as this file's #include of autotune_engine.c itself -- and
// then only exercise the guard-relevant tick logic, which depends on dt_s
// (derived from dt_ms, NOT from the frozen tick clock) rather than on
// elapsed_s.
// ---------------------------------------------------------------------------

// Starts a real step-test run on zone 0, then jumps straight to STEPPING.
//
// Does NOT go through the public autotune_engine_start() -- that function
// cannot succeed in this host build: it creates s_at.lock only after
// xTaskCreatePinnedToCoreWithCaps() succeeds
// (stubs/freertos/idf_additions.h), and that stub deliberately always
// returns pdFAIL (see its own header comment), so autotune_engine_start()
// always takes the "task create failed" branch, deletes the lock it just
// made, and returns ESP_ERR_NO_MEM -- s_at.lock is NULL again on return.
// Calling autotune_engine_run() after that would hit begin_run_locked()'s
// s_at.lock == NULL guard and, before this comment's fix, crashed this test
// file's process outright (xSemaphoreTake() on a NULL handle asserts --
// stubs/freertos/semphr.h's OWN header comment says this is deliberate, for
// exactly the reason this file's prestart tests exist).
//
// Task creation is orthogonal to what this section tests (the STEPPING
// loop's tick logic, not task scheduling), so the lock is created directly
// here instead -- autotune_engine_run() itself, including
// begin_run_locked()'s real guard_cfg setup from the zones_config_*()
// stubs, still runs for real; only the never-succeeding task spawn is
// bypassed.
static void start_stepping_run(float max_temp_c, float step_duty)
{
    static MAX31856BusClass bus;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    bus.initialized = true;
    s_at.thermo_bus = &bus;
    s_at.lock = xSemaphoreCreateMutex();
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");

    s_stub_max_temp_c = max_temp_c;
    s_stub_ch0_ok = true;

    char errbuf[96] = {0};
    bool ok = autotune_engine_run(0, step_duty, errbuf, sizeof(errbuf));
    TEST_CHECK(ok, "autotune_engine_run() must accept a step test on zone 0");

    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    s_at.state = AUTOTUNE_ENGINE_STEPPING;
    s_at.phase_start_tick = 0;
    s_at.last_sample_tick = 0;
    xSemaphoreGive(s_at.lock);
}

// Runs up to n_ticks ticks, stopping early if the run leaves the "running"
// states (e.g. a guard trip). Each tick's channel-0 reading is
// start_temp_c + i * per_tick_delta_c -- per_tick_delta_c == 0 reproduces a
// flat/stalled reading; a small positive value reproduces healthy heating.
static void run_ticks(float start_temp_c, float per_tick_delta_c, int n_ticks)
{
    for (int i = 0; i < n_ticks; i++) {
        xSemaphoreTake(s_at.lock, portMAX_DELAY);
        if (!state_is_running(s_at.state)) {
            xSemaphoreGive(s_at.lock);
            break;
        }
        s_stub_ch0_temp_c = start_temp_c + (float)i * per_tick_delta_c;
        autotune_engine_tick_locked();
        xSemaphoreGive(s_at.lock);
    }
}

static void test_step_no_ceiling_flat_reading_trips_guard1(void)
{
    TEST_SECTION("step test, no max_temp_c configured, flat reading -- guard 1 must trip");
    // step_duty=1.0: heater_output_duty() special-cases duty==1 to on for the
    // whole window (window_ms - on_ms < min_off_ms), so commanded_duty never
    // dips below thermal_guard.c's PROGRESS_DUTY_MIN and the progress window
    // never resets mid-run -- isolates this test from heater PWM timing,
    // which is not what's under test here.
    start_stepping_run(/*max_temp_c=*/0.0f, /*step_duty=*/1.0f);
    // Guard 1's window is thermal_guard.c's PROGRESS_WINDOW_S (300s, fixed,
    // not a cfg override) at 1s/tick (AUTOTUNE_ENGINE_TICK_MS) -- 320 ticks
    // covers the window with margin.
    run_ticks(/*start_temp_c=*/25.0f, /*per_tick_delta_c=*/0.0f, /*n_ticks=*/320);

    TEST_CHECK(s_at.guard_state.is_tripped, "a flat reading under commanded heat must trip a guard");
    TEST_CHECK(s_at.guard_state.reason == THERMAL_GUARD_TRIP_HEATING_FAILED,
               "specifically guard 1 (HEATING_FAILED) -- the guard whose job this is");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED, "the run must abort, not silently continue");
}

static void test_step_max_temp_configured_flat_reading_still_trips_guard1(void)
{
    TEST_SECTION("step test, max_temp_c configured, flat reading -- guard 1 still trips (no regression)");
    // Same flat-reading scenario as above, but with a real ceiling
    // configured -- this path was never affected by the bug or the fix (the
    // fallback expression's max_temp_c > 0.0f branch is untouched), so it
    // must behave exactly as it always has.
    start_stepping_run(/*max_temp_c=*/500.0f, /*step_duty=*/1.0f);
    run_ticks(/*start_temp_c=*/25.0f, /*per_tick_delta_c=*/0.0f, /*n_ticks=*/320);

    TEST_CHECK(s_at.guard_state.is_tripped, "a flat reading under commanded heat must trip a guard");
    TEST_CHECK(s_at.guard_state.reason == THERMAL_GUARD_TRIP_HEATING_FAILED,
               "specifically guard 1 (HEATING_FAILED)");
}

static void test_step_no_ceiling_rising_reading_does_not_trip(void)
{
    TEST_SECTION("step test, no max_temp_c configured, healthy rise -- must NOT trip (no false positive)");
    // ~1.2C/min, well above the 0.5C/min default sanity rate guard 1 checks
    // against -- proves the fix (raw_c + headroom) doesn't cost false trips
    // on a normal, healthily-heating run.
    start_stepping_run(/*max_temp_c=*/0.0f, /*step_duty=*/1.0f);
    run_ticks(/*start_temp_c=*/25.0f, /*per_tick_delta_c=*/0.02f, /*n_ticks=*/320);

    TEST_CHECK(!s_at.guard_state.is_tripped, "a healthily rising reading must not trip any guard");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_STEPPING, "the run must still be stepping, not aborted");
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

    // STEPPING-loop tests LAST -- see that section's header comment: these
    // are the only tests here that call autotune_engine_start(), which
    // would invalidate the "before start" premise of everything above it.
    test_step_no_ceiling_flat_reading_trips_guard1();
    test_step_max_temp_configured_flat_reading_still_trips_guard1();
    test_step_no_ceiling_rising_reading_does_not_trip();
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
