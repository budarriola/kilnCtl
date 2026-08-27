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

bool relay_authority_zone_latched_blocked(uint8_t zone_index)
{
    // Always "not latched" -- none of these tests exercise the per-zone
    // latch-clearing path this stub feeds (that is today's separate landed
    // fix); returning false just lets begin_run_locked() proceed normally.
    (void)zone_index;
    return false;
}

// TODO.md 6A.6 (ownership tags): recorded, not a bare no-op, so the
// STEPPING-loop ownership tests further down can prove begin_run_locked()
// claims RELAY_OWNER_AUTOTUNE and force_relays_off() (the single release
// chokepoint every terminal path funnels through) releases it -- both
// asserted by call count, not just "did not crash". reset_owner_recorder()
// clears these between tests.
static int s_claim_calls = 0;
static uint8_t s_claim_last_mask = 0;
static relay_owner_t s_claim_last_owner = RELAY_OWNER_NONE;
static int s_release_calls = 0;
static uint8_t s_release_last_mask = 0;

static void reset_owner_recorder(void)
{
    s_claim_calls = 0;
    s_claim_last_mask = 0;
    s_claim_last_owner = RELAY_OWNER_NONE;
    s_release_calls = 0;
    s_release_last_mask = 0;
}

void relay_authority_claim_mask(uint8_t relay_mask, relay_owner_t owner)
{
    s_claim_calls++;
    s_claim_last_mask = relay_mask;
    s_claim_last_owner = owner;
}

void relay_authority_release_mask(uint8_t relay_mask)
{
    s_release_calls++;
    s_release_last_mask = relay_mask;
}

// Recorded (not a bare no-op) so the global-fault-source clearing tests
// below can prove BOTH halves of the bug fix: the trip actually asserts the
// source, and starting a new run actually clears it -- not just "did not
// crash". reset_fault_recorder() clears these between tests.
static int s_fault_calls = 0;
static uint32_t s_fault_last_source = 0;
static bool s_fault_last_assert = false;

static void reset_fault_recorder(void)
{
    s_fault_calls = 0;
    s_fault_last_source = 0;
    s_fault_last_assert = false;
}

esp_err_t safety_link_set_fault_source(SafetyLinkClass *link, uint32_t source_mask, bool assert_fault)
{
    (void)link;
    s_fault_calls++;
    s_fault_last_source = source_mask;
    s_fault_last_assert = assert_fault;
    return ESP_OK;
}

// Recorded the same way, for the relay-cycle-accounting tests below: proves
// autotune's own relay switching reaches relay_cycles.c, not just that the
// call compiles/links. reset_cycles_recorder() clears these between tests.
static int s_cycles_add_calls = 0;
static uint8_t s_cycles_last_mask = 0;
static uint32_t s_cycles_total_added = 0;

static void reset_cycles_recorder(void)
{
    s_cycles_add_calls = 0;
    s_cycles_last_mask = 0;
    s_cycles_total_added = 0;
}

void relay_cycles_add(uint8_t relay_mask, uint32_t cycles)
{
    s_cycles_add_calls++;
    s_cycles_last_mask = relay_mask;
    s_cycles_total_added += cycles;
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
    // A real (if never-dereferenced-for-real, thanks to the
    // safety_link_set_fault_source() stub above) SafetyLinkClass instance --
    // without it s_at.safety reads NULL and escalate_and_abort()'s `if
    // (s_at.safety)` guard skips the call entirely, which would make the
    // global-fault-source tests below pass for the wrong reason (nothing
    // called) rather than the right one (the call happened and was recorded).
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    bus.initialized = true;
    s_at.thermo_bus = &bus;
    s_at.safety = &safety;
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

// ---------------------------------------------------------------------------
// TODO.md 6A.6 ownership tests -- autotune_engine.c claims RELAY_OWNER_AUTOTUNE
// on the zone it steps (begin_run_locked()) and releases it through
// force_relays_off(), the single chokepoint every terminal path (finalize_fit,
// finalize_relay_fit, escalate_and_abort, abort_locked) calls. Proven here by
// call count against the recording stubs above -- a claim that outlives the
// run (leaked ownership blocking every future manual command on that relay)
// or a run that never claims (a manual SET_RELAY racing a live step-test,
// exactly the TODO.md 6A.6 gap) both fail these checks. Same
// start_stepping_run()/run_ticks() harness as the guard-coverage tests above.
// ---------------------------------------------------------------------------

static void test_step_test_claims_autotune_ownership_on_start(void)
{
    TEST_SECTION("autotune_engine_run() -- claims RELAY_OWNER_AUTOTUNE on the zone's mask");
    reset_owner_recorder();
    start_stepping_run(/*max_temp_c=*/0.0f, /*step_duty=*/1.0f);

    TEST_CHECK(s_claim_calls == 1, "exactly one claim on run start, not zero and not repeated per tick");
    TEST_CHECK(s_claim_last_owner == RELAY_OWNER_AUTOTUNE, "claimed as RELAY_OWNER_AUTOTUNE specifically");
    TEST_CHECK(s_claim_last_mask == 0x01, "claims the zone's configured relay mask (stub: 0x01)");
    TEST_CHECK(s_release_calls == 0, "must not release what it just claimed before the run has done anything");
}

static void test_guard_trip_releases_autotune_ownership(void)
{
    TEST_SECTION("STEPPING guard trip -- force_relays_off() releases the RELAY_OWNER_AUTOTUNE claim");
    reset_owner_recorder();
    // Same flat-reading/no-ceiling scenario as test_step_no_ceiling_flat_reading_trips_guard1()
    // above -- this test only adds the ownership-release assertion.
    start_stepping_run(/*max_temp_c=*/0.0f, /*step_duty=*/1.0f);
    TEST_CHECK(s_claim_calls == 1, "sanity: claimed once on start, same as the claim test above");
    run_ticks(/*start_temp_c=*/25.0f, /*per_tick_delta_c=*/0.0f, /*n_ticks=*/320);

    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED, "sanity: the guard-trip abort this test rides on");
    TEST_CHECK(s_release_calls == 1, "a claim that outlives an aborted run is worse than no claim at all");
    TEST_CHECK(s_release_last_mask == 0x01, "releases the same mask it claimed");
}

// Runs up to n_ticks ticks with the sensor reporting a fault every tick,
// stopping early if the run leaves the "running" states -- same shape as
// run_ticks() above, but driving thermal_guard's sensor-fault debounce
// (SENSOR_FAULT_DEBOUNCE_TICKS, 3 in thermal_guard.c) instead of a
// flat-vs-rising reading, so it trips THERMAL_GUARD_TRIP_SENSOR_INVALID --
// a GLOBAL trip (escalate_and_abort()'s `global` branch), which is what the
// global-fault-source tests below need.
static void run_bad_sensor_ticks(int n_ticks)
{
    for (int i = 0; i < n_ticks; i++) {
        xSemaphoreTake(s_at.lock, portMAX_DELAY);
        if (!state_is_running(s_at.state)) {
            xSemaphoreGive(s_at.lock);
            break;
        }
        s_stub_ch0_ok = false;
        autotune_engine_tick_locked();
        xSemaphoreGive(s_at.lock);
    }
}

// ---------------------------------------------------------------------------
// Global-fault-source clearing tests (defect 1 in the task brief): a global
// guard trip during autotune used to raise a safety fault source that
// nothing ever cleared -- relay_authority_on_blocked() then refused every
// relay-ON, board-wide, until reboot. The fix mirrors profile_executor.c's
// global_fault_source / clear_this_runs_faults() pattern: record which
// source THIS run raised, and clear it when the operator starts the next
// autotune run.
// ---------------------------------------------------------------------------

static void test_global_guard_trip_asserts_fault_source(void)
{
    TEST_SECTION("STEPPING sensor-invalid trip (global) -- fault source asserted and recorded");
    reset_fault_recorder();
    start_stepping_run(/*max_temp_c=*/0.0f, /*step_duty=*/1.0f);
    reset_fault_recorder(); // start_stepping_run()'s own begin_run_locked() calls nothing here (no prior
                            // fault to clear), but reset again anyway so this test only sees the trip's own call.

    run_bad_sensor_ticks(/*n_ticks=*/10); // SENSOR_FAULT_DEBOUNCE_TICKS is 3; 10 gives margin.

    TEST_CHECK(s_at.guard_state.is_tripped, "a persistently bad sensor must trip a guard");
    TEST_CHECK(s_at.guard_state.reason == THERMAL_GUARD_TRIP_SENSOR_INVALID, "specifically guard 6 (SENSOR_INVALID)");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED, "the run must abort");
    TEST_CHECK(s_at.global_fault_source == SAFETY_FAULT_SRC_THERMO,
               "global_fault_source must record what THIS run raised, mirroring profile_executor.c");
    TEST_CHECK(s_fault_calls == 1, "exactly one safety_link_set_fault_source() call for the trip");
    TEST_CHECK(s_fault_last_source == SAFETY_FAULT_SRC_THERMO, "asserted the sensor-invalid source");
    TEST_CHECK(s_fault_last_assert == true, "asserted (true), not cleared");
}

static void test_next_autotune_run_clears_prior_global_fault_source(void)
{
    TEST_SECTION("starting a new autotune run clears a fault source an earlier run left asserted");
    reset_fault_recorder();
    start_stepping_run(/*max_temp_c=*/0.0f, /*step_duty=*/1.0f);
    reset_fault_recorder();
    run_bad_sensor_ticks(/*n_ticks=*/10);
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED, "sanity: the trip this test rides on landed");
    TEST_CHECK(s_at.global_fault_source == SAFETY_FAULT_SRC_THERMO, "sanity: fault source recorded by the trip");

    // The bug: WITHOUT the fix, nothing above this line ever clears. The
    // regression this test exists to catch is exactly "starts a new run and
    // the board-wide mask never returns to clean" -- deliberately NOT calling
    // start_stepping_run() again here, because that memsets s_at and would
    // erase the very state (global_fault_source, s_at.lock) this test needs
    // to prove gets cleared by begin_run_locked() itself, not by test setup.
    reset_fault_recorder();
    char errbuf[96] = {0};
    bool ok = autotune_engine_run(0, 1.0f, errbuf, sizeof(errbuf));

    TEST_CHECK(ok, "a new autotune run must be accepted on this zone after the earlier one aborted");
    TEST_CHECK(s_at.global_fault_source == 0,
               "global_fault_source must be cleared by the operator starting a new run");
    TEST_CHECK(s_fault_calls == 1, "exactly one safety_link_set_fault_source() call to clear it");
    TEST_CHECK(s_fault_last_source == SAFETY_FAULT_SRC_THERMO, "clears the SAME source the earlier run raised");
    TEST_CHECK(s_fault_last_assert == false, "cleared (false), not re-asserted");
}

// ---------------------------------------------------------------------------
// Relay-cycle-accounting tests (defect 2 in the task brief): autotune's own
// relay switching used to be invisible to relay_cycles.c -- confirmed on the
// bench, a full step-test autotune left relay_cycles completely unchanged.
// The fix mirrors profile_executor.c's cycles_reported high-water-mark
// pattern: report only the delta in heater_state.cycle_count since the last
// tick.
// ---------------------------------------------------------------------------

static void test_autotune_relay_switching_is_counted(void)
{
    TEST_SECTION("autotune's relay switching reaches relay_cycles_add(), not just profile_executor's");
    reset_cycles_recorder();
    // step_duty=1.0 (see start_stepping_run()'s callers' own comments): the
    // relay closes once at the start of STEPPING and stays closed for the
    // ticks below, so heater_state.cycle_count goes 0->1 on that edge, and
    // this run's tick loop reports that single edge.
    //
    // autotune_engine_abort() afterwards forces the relay back off, a SECOND
    // edge (1->2) -- but that one happens outside any call to
    // autotune_engine_tick_locked() (abort_locked() calls force_relays_off()
    // directly), so it is never reported this run, exactly like
    // profile_executor.c's identical cycles_reported pattern: that field's
    // accounting is likewise tick-driven only, and force_zone_relay_off()
    // calls outside a tick (its own abort/guard-trip paths) leave the same
    // kind of trailing edge unreported until a later tick happens to run.
    // This test asserts what the mirrored pattern actually delivers -- the
    // in-tick edge counted exactly once -- not a stronger guarantee neither
    // module makes.
    start_stepping_run(/*max_temp_c=*/0.0f, /*step_duty=*/1.0f);
    reset_cycles_recorder(); // start_stepping_run()'s own first tick(s) are not part of this test's count.

    // A HEALTHY, sensor-ok run this time (unlike the fault-source tests
    // above): with the sensor reading bad, heater_output_duty() is fed a
    // forced 0.0f duty and the relay never switches at all, so that scenario
    // cannot exercise this accounting. A few ticks of a normal reading (well
    // short of guard 1's 300-tick window) let the relay actually close.
    run_ticks(/*start_temp_c=*/25.0f, /*per_tick_delta_c=*/0.0f, /*n_ticks=*/5);
    uint32_t cycle_count_after_ticks = s_at.heater_state.cycle_count;

    TEST_CHECK(s_cycles_add_calls >= 1, "at least one relay_cycles_add() call -- before the fix, there were zero");
    TEST_CHECK(s_cycles_last_mask == 0x01, "reports against the zone's own relay mask (stub: 0x01)");
    TEST_CHECK(s_cycles_total_added == cycle_count_after_ticks,
               "every cycle heater_output counted DURING A TICK must reach relay_cycles.c exactly once "
               "(no drop, no double-count)");
    TEST_CHECK(cycle_count_after_ticks >= 1, "sanity: the relay actually switched at least once");

    // The abort's own edge, verified separately: heater_state.cycle_count
    // (the lifetime counter) does move, proving force_relays_off() really
    // did open the relay -- this module is not silently failing to switch it
    // -- it is simply not yet reported to relay_cycles.c, per the comment
    // above.
    autotune_engine_abort("test: cycle accounting");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED, "sanity: the run this test rides on aborted");
    TEST_CHECK(s_at.heater_state.cycle_count == cycle_count_after_ticks + 1,
               "sanity: abort's force-off is a real second edge on the lifetime counter");
}

static void test_manual_abort_releases_autotune_ownership(void)
{
    TEST_SECTION("autotune_engine_abort() (operator Abort button) -- releases the RELAY_OWNER_AUTOTUNE claim");
    reset_owner_recorder();
    start_stepping_run(/*max_temp_c=*/0.0f, /*step_duty=*/1.0f);
    TEST_CHECK(s_claim_calls == 1, "sanity: claimed once on start");

    autotune_engine_abort("operator cancelled");

    TEST_CHECK(s_release_calls == 1, "abort_locked() -> force_relays_off() must release the claim");
    TEST_CHECK(s_release_last_mask == 0x01, "releases the same mask it claimed");
    autotune_engine_status_t st;
    autotune_engine_get_status(&st);
    TEST_CHECK(st.state == AUTOTUNE_ENGINE_ABORTED, "sanity: the abort actually landed");
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

    // Ownership tests (TODO.md 6A.6) -- order-independent relative to the
    // guard tests above (each calls start_stepping_run(), which re-zeroes
    // s_at), but kept after them for narrative order: guards first, then the
    // ownership bookkeeping layered around the same run lifecycle.
    test_step_test_claims_autotune_ownership_on_start();
    test_guard_trip_releases_autotune_ownership();
    test_manual_abort_releases_autotune_ownership();

    // Defects 1 and 2 from the task brief -- order-independent relative to
    // everything above (each starts from start_stepping_run(), which
    // re-zeroes s_at), but the two global-fault-source tests must run in
    // this relative order: the second depends on the first zone's trip
    // having actually landed and the state it left behind.
    test_global_guard_trip_asserts_fault_source();
    test_next_autotune_run_clears_prior_global_fault_source();
    test_autotune_relay_switching_is_counted();
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
