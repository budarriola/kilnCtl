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

/* The shared heat claim (relay_authority.h) -- begin_run_locked() now takes
 * this atomically right after state_is_running() confirms a genuine start,
 * on top of (not instead of) the early, non-atomic
 * zones_current_sweep_is_active() check this file already stubs above.
 * Default succeeds so every existing test's real STEPPING-loop path is
 * unchanged; s_test_heat_zone_claim_refused lets
 * test_run_refuses_at_atomic_heat_claim_gate() below prove the LATE gate is
 * independently load-bearing. */
static bool s_test_heat_zone_claim_refused = false;
static int s_heat_zone_claim_begin_calls = 0;
static int s_heat_zone_claim_end_calls = 0;
static relay_heat_zone_claimant_t s_last_heat_zone_claimant = RELAY_HEAT_ZONE_CLAIM_PROFILE;

bool relay_authority_heat_zone_claim_begin(relay_heat_zone_claimant_t who)
{
    s_heat_zone_claim_begin_calls++;
    s_last_heat_zone_claimant = who;
    return !s_test_heat_zone_claim_refused;
}

void relay_authority_heat_zone_claim_end(relay_heat_zone_claimant_t who)
{
    s_heat_zone_claim_end_calls++;
    s_last_heat_zone_claimant = who;
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

/* Spy for the heat-enable (K4) wiring tests below. autotune_engine.c now
 * calls heat_enable_acquire()/heat_enable_release() (heat_enable.h), and
 * heat_enable.c is linked into this executable for real, so the fake goes
 * one layer down: safety_link.c's own request_enable. Its real contract --
 * enable=true refused, and NOTHING sent, on a down link; enable=false always
 * attempted -- is reproduced exactly, because the honest handling of a down
 * link is half of what these tests are for. */
static int  s_req_enable_true_calls = 0;
static int  s_req_enable_false_calls = 0;
static bool s_req_enable_link_up = true;

esp_err_t safety_link_request_enable(SafetyLinkClass *link, bool enable)
{
    (void)link;
    if (enable) {
        s_req_enable_true_calls++;
        return s_req_enable_link_up ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    s_req_enable_false_calls++;
    return ESP_OK;
}

static void reset_heat_enable_recorder(bool link_up)
{
    s_req_enable_link_up = link_up;
    heat_enable_init((SafetyLinkClass *)0x1);
    s_req_enable_true_calls = 0;
    s_req_enable_false_calls = 0;
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

/* v8 overrides. Settable so a test can prove a configured value actually
 * reaches the control path; defaults to "nothing configured", which is what
 * every pre-existing test in this file assumes. */
float g_stub_guard_extra[5] = {0, 0, 0, 0, 0};
bool g_stub_guard_extra_present = false;
float g_stub_exec_thresholds[4] = {0, 0, 0, 0};
bool g_stub_exec_thresholds_present = false;
float g_stub_pc_link_silence_ms = 0.0f;

bool zones_config_get_guard_extra(uint8_t zone_index, float *o1, float *o2, float *o3, float *o4, float *o5)
{
    (void)zone_index;
    if (o1) *o1 = g_stub_guard_extra[0];
    if (o2) *o2 = g_stub_guard_extra[1];
    if (o3) *o3 = g_stub_guard_extra[2];
    if (o4) *o4 = g_stub_guard_extra[3];
    if (o5) *o5 = g_stub_guard_extra[4];
    return g_stub_guard_extra_present;
}

bool zones_config_get_executor_thresholds(uint8_t zone_index, float *o1, float *o2, float *o3, float *o4)
{
    (void)zone_index;
    if (o1) *o1 = g_stub_exec_thresholds[0];
    if (o2) *o2 = g_stub_exec_thresholds[1];
    if (o3) *o3 = g_stub_exec_thresholds[2];
    if (o4) *o4 = g_stub_exec_thresholds[3];
    return g_stub_exec_thresholds_present;
}

bool zones_config_get_pc_link_abort_silence_ms(float *out_ms)
{
    if (out_ms) *out_ms = g_stub_pc_link_silence_ms;
    return true;
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

// Configurable per-test (default 0, this stub's original hardcoded
// behavior) via direct assignment, same convention as s_stub_max_temp_c
// above -- test_finalize_fit_persists_valid_cross_gain_cells() below needs
// a real thermo_count so finalize_fit()'s cross-zone loop does not refuse
// every peer cell with "j >= thermo_count" before ever reaching the new
// persist step.
static uint8_t s_stub_thermo_count = 0;

uint8_t zones_config_get_thermo_count(void)
{
    return s_stub_thermo_count;
}

/* 2026-08-30 (ZONES_CFG_VERSION 10->11): finalize_fit()'s new persist step
 * (see autotune_engine.c's own comment) needs this symbol to link.
 * Records every call (zone_index, neighbor_index, coeff) rather than just
 * refusing, so test_finalize_fit_persists_valid_cross_gain_cells() below can
 * observe exactly what finalize_fit() tried to persist -- succeeds
 * unconditionally by default, same "default succeeds" convention this
 * file's header comment documents for the STEPPING-loop's other setters. */
typedef struct {
    bool called;
    float coeff;
} coupling_cell_call_t;
static coupling_cell_call_t s_coupling_cell_calls[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT];

static void reset_coupling_cell_calls(void)
{
    memset(s_coupling_cell_calls, 0, sizeof(s_coupling_cell_calls));
}

/* 2026-08-31 defect fix: mirrors the real setter's bound (zones_http.c's
 * zones_config_set_coupling_cell(): finite, 0..ZONE_COUPLING_COEFF_MAX,
 * nonzero diagonal refused). Previously this fake accepted ANY value
 * unconditionally, which meant autotune_engine.c's OWN isfinite/range guard
 * (finalize_fit(), ~line 437 -- the guard that decides whether to call this
 * setter AT ALL) had no negative test: stubbing that guard to `if (0)`
 * still passed 156/156, because the fake could not tell "the guard skipped
 * the call" from "the call happened and the fake let a bad value through
 * anyway". `called` is still recorded unconditionally (even for a rejected
 * value) so a test can distinguish those two cases: a working guard never
 * calls this at all for an out-of-range gain (called stays false); a broken
 * guard calls it and gets refused (called becomes true, return value
 * false) -- either way the real bound is enforced here exactly like the
 * production setter, so a test built on this fake proves something real. */
/* 2026-08-31 panic fix: finalize_fit()'s persist step now hands its
 * coupling_persist_job_t to uart_bridge_ext_run_on_flash_worker() instead of
 * calling zones_config_set_coupling_cell() directly (see autotune_engine.c's
 * own comment for the coredump this fixes). This host build has no separate
 * flash-worker task to hand a job to -- it runs fn(arg) synchronously,
 * in-line, which is externally indistinguishable to every test above and
 * below: they only ever observe s_coupling_cell_calls[][], not which task
 * made the call. Recording the call count separately (not reusing an
 * existing counter) lets a future test assert the persist step went through
 * this path at all, not just that the cells landed. */
int g_flash_worker_submit_calls = 0;
esp_err_t uart_bridge_ext_run_on_flash_worker(void (*fn)(void *arg), void *arg)
{
    g_flash_worker_submit_calls++;
    if (!fn) {
        return ESP_ERR_INVALID_ARG;
    }
    fn(arg);
    return ESP_OK;
}

bool zones_config_set_coupling_cell(uint8_t zone_index, uint8_t neighbor_index, float coeff)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT || neighbor_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    s_coupling_cell_calls[zone_index][neighbor_index].called = true;
    s_coupling_cell_calls[zone_index][neighbor_index].coeff = coeff;
    if (zone_index == neighbor_index) {
        return coeff == 0.0f;
    }
    return isfinite(coeff) && coeff >= 0.0f && coeff <= ZONE_COUPLING_COEFF_MAX;
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

// B2 (opus review, 2026-08-27): begin_run_locked() now refuses to start
// while a zone current sweep is active -- see zones_current_sweep_is_active()'s
// doc comment (zones_http.h). Settable so test_run_refuses_while_zone_sweep_
// is_active() below can exercise the real refusal; default false so the real
// STEPPING-loop tests elsewhere in this file, which call begin_run_locked()
// for real, are unaffected.
static bool s_test_sweep_active = false;
bool zones_current_sweep_is_active(void)
{
    return s_test_sweep_active;
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
    bool ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_SIMC, err, sizeof(err));
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
static void start_stepping_run_rule(float max_temp_c, float step_duty, autotune_rule_t rule)
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
    bool ok = autotune_engine_run(0, step_duty, rule, errbuf, sizeof(errbuf));
    TEST_CHECK(ok, "autotune_engine_run() must accept a step test on zone 0");

    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    s_at.state = AUTOTUNE_ENGINE_STEPPING;
    s_at.phase_start_tick = 0;
    s_at.last_sample_tick = 0;
    xSemaphoreGive(s_at.lock);
}

static void start_stepping_run(float max_temp_c, float step_duty)
{
    start_stepping_run_rule(max_temp_c, step_duty, AUTOTUNE_RULE_SIMC);
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

// Sets up a real (non-NULL) s_at.lock, exactly like start_stepping_run_rule()
// does, but does NOT call autotune_engine_run() -- the caller does that, so
// it can inspect the return value/err_msg of a call that is expected to be
// REFUSED. This matters: a NULL s_at.lock refuses every call unconditionally
// (begin_run_locked()'s very first check), so a rule-rejection test run
// before autotune_engine_start() would pass for the wrong reason -- masked by
// that earlier guard, never actually reaching the rule check under test. See
// this file's task brief: exactly the "input the range check rejected before
// the check under test ever ran" trap. A deliberate revert of the rule check
// confirmed this: with it removed, the naive "before start" version of this
// test kept passing (125/125) because begin_run_locked()'s NULL-lock guard
// still refused the call -- this version, with a live lock, is what actually
// catches it.
static void prep_live_lock_no_run(void)
{
    static MAX31856BusClass bus;
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    bus.initialized = true;
    s_at.thermo_bus = &bus;
    s_at.safety = &safety;
    s_at.lock = xSemaphoreCreateMutex();
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");
}

static void test_run_rejects_relay_only_rules_on_step_path(void)
{
    TEST_SECTION("autotune_engine_run() refuses ZN/Tyreus-Luyben -- relay-only rules on the FOPDT step path");
    prep_live_lock_no_run();
    char err[96] = {0};
    bool ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_ZIEGLER_NICHOLS, err, sizeof(err));
    TEST_CHECK(!ok, "ZN must be refused on the step-test path -- it needs a relay (Ku/Tu) test");
    TEST_CHECK(err[0] != '\0', "an error message is filled in for the caller");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_IDLE, "a refused rule must not have started anything");

    prep_live_lock_no_run();
    err[0] = '\0';
    ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_TYREUS_LUYBEN, err, sizeof(err));
    TEST_CHECK(!ok, "Tyreus-Luyben must be refused on the step-test path for the same reason");
    TEST_CHECK(err[0] != '\0', "an error message is filled in for the caller");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_IDLE, "a refused rule must not have started anything");

    // Positive control, same live-lock setup: SIMC must still be accepted --
    // proves the two refusals above are the rule check firing, not some
    // unrelated reason begin_run_locked() would refuse everything here.
    prep_live_lock_no_run();
    err[0] = '\0';
    ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_SIMC, err, sizeof(err));
    TEST_CHECK(ok, "positive control: SIMC must be accepted with the same live-lock setup ZN/TL were refused under");
}

static void test_step_run_accepts_simc_and_cohen_coon_and_stores_the_rule(void)
{
    TEST_SECTION("autotune_engine_run() accepts SIMC and Cohen-Coon, and records which one for finalize_fit()");
    start_stepping_run_rule(/*max_temp_c=*/0.0f, /*step_duty=*/1.0f, AUTOTUNE_RULE_SIMC);
    TEST_CHECK(s_at.step_rule == AUTOTUNE_RULE_SIMC, "SIMC accepted and stored");

    start_stepping_run_rule(/*max_temp_c=*/0.0f, /*step_duty=*/1.0f, AUTOTUNE_RULE_COHEN_COON);
    TEST_CHECK(s_at.step_rule == AUTOTUNE_RULE_COHEN_COON,
               "Cohen-Coon accepted and stored -- this is what finalize_fit() passes to "
               "pid_autotune_tune_from_fopdt() instead of the old hardcoded AUTOTUNE_RULE_SIMC");
}

// Writes a synthetic FOPDT step-response trace directly into s_at.zone_trace,
// bypassing the STEPPING tick loop entirely: this host build's
// xTaskGetTickCount() stub always returns 0 (see this file's header comment
// on the STEPPING-loop guard tests above), which freezes s_at.elapsed_s and
// last_sample_tick forever and makes the real record-a-sample-every-
// AUTOTUNE_ENGINE_SAMPLE_PERIOD_S path unreachable from a driven tick loop.
// finalize_fit() itself has no such dependency (it only reads
// zone_trace/trace_count/zone_baseline_c/step_duty/step_rule, all set
// directly here), so it is called white-box, same convention as this file's
// #include of autotune_engine.c -- and it is the actual function under test
// for "does the step path still hardcode SIMC".
// zone-index-parameterized version -- test_finalize_fit_persists_valid_
// cross_gain_cells() below needs to write a trace for more than one zone
// (the zone under test AND its peers) to exercise finalize_fit()'s
// cross-zone loop, not just the single self-fit write_synthetic_fopdt_trace()
// (zone 0 only) below was originally written for.
static void write_synthetic_fopdt_trace_for_zone(uint8_t zone, float baseline_c, float k_gain_c_per_duty,
                                                  float tau_s, float dead_time_s, float duty_step,
                                                  uint16_t n_samples)
{
    s_at.zone_baseline_c[zone] = baseline_c;
    s_at.zone_baseline_valid[zone] = true;
    s_at.step_duty = duty_step;
    s_at.trace_count = n_samples;
    for (uint16_t i = 0; i < n_samples; i++) {
        float t_s = (float)(i * AUTOTUNE_ENGINE_SAMPLE_PERIOD_S);
        float rise = (t_s <= dead_time_s)
                         ? 0.0f
                         : k_gain_c_per_duty * duty_step * (1.0f - expf(-(t_s - dead_time_s) / tau_s));
        s_at.zone_trace[zone][i] = (int16_t)lroundf((baseline_c + rise) * 10.0f);
    }
}

static void write_synthetic_fopdt_trace(float baseline_c, float k_gain_c_per_duty, float tau_s, float dead_time_s,
                                        float duty_step, uint16_t n_samples)
{
    write_synthetic_fopdt_trace_for_zone(0, baseline_c, k_gain_c_per_duty, tau_s, dead_time_s, duty_step, n_samples);
}

static void test_finalize_fit_uses_the_requested_rule(void)
{
    TEST_SECTION("finalize_fit() uses s_at.step_rule, not a hardcoded SIMC -- PID_EXPANSION_PLAN.md Phase 1");
    // K=2 degC/duty, tau=200s, L=20s (well above
    // AUTOTUNE_COHEN_COON_MIN_DEAD_TIME_S so Cohen-Coon does not refuse on
    // dead time), 60 samples * 10s/sample = 600s -- comfortably past 5*tau
    // so both the two-point crossing fit AND the settle band would succeed.
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;
    write_synthetic_fopdt_trace(/*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/2.0f, /*tau_s=*/200.0f,
                                /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    finalize_fit();
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "a well-formed trace must fit");
    TEST_CHECK(s_at.model.valid, "the FOPDT fit must have succeeded");
    TEST_CHECK(s_at.proposed_gains.rule == AUTOTUNE_RULE_SIMC, "SIMC requested -> SIMC reported");
    TEST_CHECK(s_at.proposed_gains.refusal == AUTOTUNE_REFUSAL_OK, "a valid model + SIMC must not refuse");
    float simc_kp = s_at.proposed_gains.kp;
    TEST_CHECK(simc_kp != 0.0f, "sanity: SIMC actually produced a nonzero Kp on this trace");

    // Identical trace, Cohen-Coon requested instead: must report its OWN
    // rule and produce different numbers than SIMC on the exact same fitted
    // model -- if finalize_fit() were still passing AUTOTUNE_RULE_SIMC to
    // pid_autotune_tune_from_fopdt() regardless of s_at.step_rule (the
    // pre-fix defect this test exists to catch), both of these would read
    // back as SIMC/simc_kp again.
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    s_at.step_rule = AUTOTUNE_RULE_COHEN_COON;
    write_synthetic_fopdt_trace(/*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/2.0f, /*tau_s=*/200.0f,
                                /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    finalize_fit();
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "the same trace must fit again");
    TEST_CHECK(s_at.model.valid, "the FOPDT fit must have succeeded");
    TEST_CHECK(s_at.proposed_gains.rule == AUTOTUNE_RULE_COHEN_COON, "Cohen-Coon requested -> Cohen-Coon reported");
    TEST_CHECK(s_at.proposed_gains.refusal == AUTOTUNE_REFUSAL_OK,
               "this trace's dead time is well above AUTOTUNE_COHEN_COON_MIN_DEAD_TIME_S, so Cohen-Coon must not "
               "refuse");
    TEST_CHECK(s_at.proposed_gains.kp != simc_kp,
               "Cohen-Coon's Kp must differ from SIMC's Kp on the identical fitted model -- if this fails, "
               "finalize_fit() is still hardcoding one rule regardless of what was requested");
}

// 2026-08-30 (ZONES_CFG_VERSION 10->11): finalize_fit()'s new persist step
// -- see that function's own comment in autotune_engine.c. Runs a real
// three-zone step test white-box (zone 1 stepped, zones 0 and 2 as peers,
// same synthetic-trace technique test_finalize_fit_uses_the_requested_rule()
// above uses) and checks exactly what gets committed through
// zones_config_set_coupling_cell(): a valid cross-gain lands in the AFFECTED
// zone's row at the STEPPED zone's column, an invalid/absent peer fit is
// skipped, and the diagonal is never touched at all (that is model_k_dc's
// job, not coupling_coeff[]'s).
static void test_finalize_fit_persists_valid_cross_gain_cells(void)
{
    TEST_SECTION("finalize_fit() persists every valid cross-gain cell through "
                 "zones_config_set_coupling_cell(), skips an invalid/missing peer, "
                 "and never touches the diagonal");
    memset(&s_at, 0, sizeof(s_at));
    reset_coupling_cell_calls();
    s_stub_thermo_count = 3;
    s_at.zone_index = 1; // the zone under test -- the STEPPED zone
    s_at.step_rule = AUTOTUNE_RULE_SIMC;

    // Zone 1 (self): K=2, tau=200s, L=20s -- the direct fit finalize_fit()
    // needs to succeed before it ever reaches the cross-zone loop.
    write_synthetic_fopdt_trace_for_zone(1, /*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/2.0f, /*tau_s=*/200.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    // Zone 0: a real, fittable cross-gain, K=0.5 (well inside
    // ZONE_COUPLING_COEFF_MAX) -- this is what must land in
    // coupling_coeff[0] of ZONE 0's row (the AFFECTED zone), not zone 1's.
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/22.0f, /*k_gain_c_per_duty=*/0.5f, /*tau_s=*/150.0f,
                                         /*dead_time_s=*/15.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    // Zone 2: baseline never went valid -- finalize_fit()'s own
    // !s_at.zone_baseline_valid[j] check must skip it, same as a sensor
    // that never reported during the run.
    s_at.zone_baseline_valid[2] = false;

    finalize_fit();
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "the direct (zone 1) fit must succeed");
    TEST_CHECK(s_at.model.valid, "zone 1's own model must be valid");
    TEST_CHECK(s_at.coupling.cell[1][0].valid, "zone 0's cross-gain cell was fitted and marked valid");
    TEST_CHECK(!s_at.coupling.cell[1][2].valid, "zone 2's cell is invalid -- its baseline never went valid");

    TEST_CHECK(s_coupling_cell_calls[0][1].called,
              "zone 0's row, column 1 (the AFFECTED zone's cell against the STEPPED zone) was persisted");
    TEST_CHECK_NEAR(s_coupling_cell_calls[0][1].coeff, s_at.coupling.cell[1][0].model.k_gain_c_per_duty, 1e-4,
                    "the persisted value is EXACTLY the fitted cross-gain, no scaling applied "
                    "(coupling_coeff[] stores a raw degC/duty gain, not a ratio -- see finalize_fit()'s own "
                    "comment)");
    TEST_CHECK(!s_coupling_cell_calls[2][1].called,
              "zone 2's cell was NEVER persisted -- its fit was invalid (unmeasured baseline)");
    TEST_CHECK(!s_coupling_cell_calls[1][1].called,
              "the diagonal (zone 1 against itself) was never touched by the persist step -- "
              "that is model_k_dc's job, not coupling_coeff[]'s");

    s_stub_thermo_count = 0; // restore this file's original hardcoded default for every other test
}

// 2026-08-31 panic fix: proves finalize_fit()'s persist step actually goes
// THROUGH uart_bridge_ext_run_on_flash_worker() -- one submission per run,
// not one direct zones_config_set_coupling_cell() call per cell -- rather
// than merely landing on the right cells (already covered by
// test_finalize_fit_persists_valid_cross_gain_cells() above, which cannot
// tell the two apart: both call this fake the same number of times either
// way). Break-proof: this test is what actually catches a revert back to a
// direct call, which is exactly the change that panicked real hardware.
static void test_finalize_fit_routes_persist_through_flash_worker(void)
{
    TEST_SECTION("finalize_fit() submits coupling-cell persistence as ONE job to "
                 "uart_bridge_ext_run_on_flash_worker(), not a direct call per cell");
    memset(&s_at, 0, sizeof(s_at));
    reset_coupling_cell_calls();
    g_flash_worker_submit_calls = 0;
    s_stub_thermo_count = 3;
    s_at.zone_index = 1;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;

    write_synthetic_fopdt_trace_for_zone(1, /*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/2.0f, /*tau_s=*/200.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    // Two fittable peers (zone 0 and zone 2) so a per-cell submission count
    // (wrong) is distinguishable from a per-run submission count (right):
    // per-cell would read 2 here, per-run reads 1 regardless of cell count.
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/22.0f, /*k_gain_c_per_duty=*/0.5f, /*tau_s=*/150.0f,
                                         /*dead_time_s=*/15.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    write_synthetic_fopdt_trace_for_zone(2, /*baseline_c=*/23.0f, /*k_gain_c_per_duty=*/0.7f, /*tau_s=*/160.0f,
                                         /*dead_time_s=*/16.0f, /*duty_step=*/1.0f, /*n_samples=*/60);

    finalize_fit();
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "the direct (zone 1) fit must succeed");
    TEST_CHECK(s_coupling_cell_calls[0][1].called, "zone 0's cell was persisted");
    TEST_CHECK(s_coupling_cell_calls[2][1].called, "zone 2's cell was persisted");
    TEST_CHECK(g_flash_worker_submit_calls == 1,
              "exactly ONE job submitted to the flash worker for the whole run, covering both cells -- "
              "if this reads 2 (or 0), the persist path has drifted off "
              "uart_bridge_ext_run_on_flash_worker() and a real board will panic the next time this runs "
              "for real (see finalize_fit()'s and autotune_engine_start()'s own comments for the coredump "
              "this guards against)");

    s_stub_thermo_count = 0; // restore this file's original hardcoded default for every other test
}

// 2026-08-31 defect fix: finalize_fit()'s own isfinite/range guard (~line
// 437 in autotune_engine.c) must skip persisting a VALID cell whose fitted
// gain landed outside 0..ZONE_COUPLING_COEFF_MAX -- distinct from the
// "cell->valid == false" case test_finalize_fit_persists_valid_cross_gain_
// cells() already covers above (zone 2's unmeasured baseline). Here zone
// 0's peer fit itself SUCCEEDS (cell->valid is true) but recovers a gain
// far past ZONE_COUPLING_COEFF_MAX (100.0), so the guard -- not an invalid
// fit -- is what has to stop the persist call. Proven against the real
// bound now that the zones_config_set_coupling_cell() fake above enforces
// it too (see that fake's own comment): stubbing this guard to `if (0)`
// makes s_coupling_cell_calls[0][1].called flip to true (the fake then
// itself refuses the value, but the call happened at all, which is exactly
// what this test exists to catch) -- proven below by deliberately breaking
// the guard, re-running, and restoring it.
static void test_finalize_fit_skips_a_valid_fit_with_out_of_range_gain(void)
{
    TEST_SECTION("finalize_fit() -- a cross-gain cell that fits VALID but lands outside "
                 "0..ZONE_COUPLING_COEFF_MAX is skipped, not persisted");
    memset(&s_at, 0, sizeof(s_at));
    reset_coupling_cell_calls();
    s_stub_thermo_count = 3;
    s_at.zone_index = 1;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;

    write_synthetic_fopdt_trace_for_zone(1, /*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/2.0f, /*tau_s=*/200.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    // Zone 0: a fittable but WAY out-of-range gain -- ZONE_COUPLING_COEFF_MAX
    // is 100.0, this trace recovers something far past it.
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/22.0f, /*k_gain_c_per_duty=*/500.0f, /*tau_s=*/150.0f,
                                         /*dead_time_s=*/15.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    s_at.zone_baseline_valid[2] = false;

    finalize_fit();
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "the direct (zone 1) fit must still succeed");
    TEST_CHECK(s_at.coupling.cell[1][0].valid, "zone 0's cross-gain cell fit itself is valid -- the FIT is not "
              "what's wrong here, only the recovered gain's magnitude");
    TEST_CHECK(s_at.coupling.cell[1][0].model.k_gain_c_per_duty > ZONE_COUPLING_COEFF_MAX,
              "sanity: the recovered gain really is past ZONE_COUPLING_COEFF_MAX, or this test proves nothing");

    TEST_CHECK(!s_coupling_cell_calls[0][1].called,
              "the out-of-range guard must skip the call entirely -- zones_config_set_coupling_cell() "
              "was never even invoked for this cell");

    s_stub_thermo_count = 0; // restore this file's original hardcoded default for every other test
}

// finalize_fit() must not persist ANYTHING from an aborted run -- proven by
// giving zone 1 (the zone under test) a degenerate trace that fails to fit
// at all: finalize_fit() returns early (before the cross-zone loop, let
// alone the persist step) the exact same way an aborted relay run would.
static void test_finalize_fit_persists_nothing_on_an_invalid_direct_fit(void)
{
    TEST_SECTION("finalize_fit() persists NOTHING -- not even a valid-looking peer cell -- "
                 "when the direct (self) fit itself fails");
    memset(&s_at, 0, sizeof(s_at));
    reset_coupling_cell_calls();
    s_stub_thermo_count = 3;
    s_at.zone_index = 1;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;

    // Zone 1: a flat trace (no rise at all) -- pid_autotune_fit_fopdt()
    // refuses a plant with no measurable gain, so s_at.model.valid stays
    // false and finalize_fit() returns before touching the coupling matrix
    // or calling any setter at all.
    write_synthetic_fopdt_trace_for_zone(1, /*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/0.0f, /*tau_s=*/200.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    // Zone 0: would fit perfectly well on its own -- if this cell got
    // persisted anyway, that would prove the direct-fit guard is not
    // actually gating the whole function the way it's supposed to.
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/22.0f, /*k_gain_c_per_duty=*/0.5f, /*tau_s=*/150.0f,
                                         /*dead_time_s=*/15.0f, /*duty_step=*/1.0f, /*n_samples=*/60);

    finalize_fit();
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED, "a flat, unfittable direct trace must abort the run");
    TEST_CHECK(!s_at.model.valid, "the direct model must be invalid");
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
            char msg[96];
            snprintf(msg, sizeof(msg), "coupling cell [%u][%u] was never persisted from an aborted fit", i, j);
            TEST_CHECK(!s_coupling_cell_calls[i][j].called, msg);
        }
    }

    s_stub_thermo_count = 0; // restore this file's original hardcoded default for every other test
}

// /api/autotune serializes proposed_gains.refusal/.refusal_reason, and unlike
// kp/ki/kd those have no companion `valid` flag the page can discount them
// with -- a nonempty refusal_reason reads as THIS run's verdict. So a run that
// refused must not leave its verdict lying around for the next run to report.
// Same shape as test_next_autotune_run_clears_prior_global_fault_source()
// above, and for the same reason it does NOT call start_stepping_run() to set
// up the second run: that memsets s_at and would erase the very state this
// test needs begin_run_locked() itself to clear.
static void test_next_run_clears_prior_runs_refusal(void)
{
    TEST_SECTION("starting a new autotune run clears the previous run's refusal/refusal_reason");
    start_stepping_run_rule(/*max_temp_c=*/0.0f, /*step_duty=*/1.0f, AUTOTUNE_RULE_SIMC);
    // Stand in for a completed run whose tuning rule refused the fitted model
    // (pid_autotune.c fills exactly these two fields and leaves kp/ki/kd at 0).
    s_at.state = AUTOTUNE_ENGINE_DONE;
    s_at.proposed_gains.refusal = AUTOTUNE_REFUSAL_DEAD_TIME_TOO_SMALL;
    snprintf(s_at.proposed_gains.refusal_reason, sizeof(s_at.proposed_gains.refusal_reason),
             "dead time 3.0s is below the 5.0s Cohen-Coon needs");
    s_at.predicted_max_ramp_c_per_hr = 123.0f;

    char errbuf[96] = {0};
    bool ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_SIMC, errbuf, sizeof(errbuf));
    TEST_CHECK(ok, "a new run must be accepted on this zone after the earlier one finished");
    TEST_CHECK(s_at.proposed_gains.refusal == AUTOTUNE_REFUSAL_OK,
               "the new run must report no refusal until IT produces one -- not the previous run's verdict");
    TEST_CHECK(s_at.proposed_gains.refusal_reason[0] == '\0',
               "and no refusal_reason text: /api/autotune would serialize it verbatim as this run's");
    TEST_CHECK(s_at.predicted_max_ramp_c_per_hr == 0.0f,
               "the predicted ramp is the same class of stale number and is cleared with it");
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
    bool ok = autotune_engine_run(0, 1.0f, AUTOTUNE_RULE_SIMC, errbuf, sizeof(errbuf));

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

// B2 (opus review, 2026-08-27): begin_run_locked() must refuse while a zone
// current sweep is active. Same real-mutex-no-real-task pattern
// start_stepping_run() above uses -- autotune_engine_run() itself, including
// begin_run_locked()'s real guard_cfg setup, runs for real; only the
// never-succeeding task spawn is bypassed.
static void test_run_refuses_while_zone_sweep_is_active(void)
{
    TEST_SECTION("autotune_engine_run() refuses while a zone current sweep is active (B2)");
    static MAX31856BusClass bus;
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    bus.initialized = true;
    s_at.thermo_bus = &bus;
    s_at.safety = &safety;
    s_at.lock = xSemaphoreCreateMutex();
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");

    s_stub_max_temp_c = 500.0f;
    s_stub_ch0_ok = true;
    s_test_sweep_active = true;

    char errbuf[96] = {0};
    bool ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_SIMC, errbuf, sizeof(errbuf));

    TEST_CHECK(!ok, "B2: a live zone sweep must refuse the autotune run, not merely warn");
    TEST_CHECK(strstr(errbuf, "sweep") != NULL, "the refusal must name the sweep specifically");

    // Control case: with no sweep active, the identical setup succeeds --
    // proves the refusal above is really about the sweep, not some other
    // side effect of this test's setup.
    s_test_sweep_active = false;
    memset(&s_at, 0, sizeof(s_at));
    s_at.thermo_bus = &bus;
    s_at.safety = &safety;
    s_at.lock = xSemaphoreCreateMutex();
    errbuf[0] = '\0';
    ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_SIMC, errbuf, sizeof(errbuf));
    TEST_CHECK(ok, "control: with no sweep active, the identical setup must succeed");
}

// The shared heat claim's atomic gate (relay_authority.h) -- proves the LATE
// gate right before begin_run_locked()'s commit is independently
// load-bearing, not merely decorative alongside the early
// zones_current_sweep_is_active() check test_run_refuses_while_zone_sweep_
// is_active() above already covers. Same real-full-path setup as that
// test's control case, which is known to reach the actual commit.
static void test_run_refuses_at_atomic_heat_claim_gate(void)
{
    TEST_SECTION("autotune_engine_run() -- the LATE atomic heat-claim gate refuses even when the EARLY "
                 "zones_current_sweep_is_active() check passed (the race window the claim exists to close)");
    static MAX31856BusClass bus;
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    bus.initialized = true;
    s_at.thermo_bus = &bus;
    s_at.safety = &safety;
    s_at.lock = xSemaphoreCreateMutex();
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");

    s_stub_max_temp_c = 500.0f;
    s_stub_ch0_ok = true;
    s_test_sweep_active = false; /* the EARLY check passes -- this is the race window itself */
    reset_owner_recorder();
    s_heat_zone_claim_begin_calls = 0;
    s_heat_zone_claim_end_calls = 0;

    // RED: force the atomic gate itself to refuse, simulating a sweep that
    // won the race and claimed exclusivity in the window between the early
    // check above and this call.
    s_test_heat_zone_claim_refused = true;
    char errbuf[96] = {0};
    bool ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_SIMC, errbuf, sizeof(errbuf));

    TEST_CHECK(!ok, "the atomic gate alone must be able to refuse a run the early check let through");
    TEST_CHECK(strstr(errbuf, "sweep") != NULL, "the refusal must still name the sweep specifically");
    TEST_CHECK(s_claim_calls == 0, "relay_authority_claim_mask() (the actual relay ownership grab) "
                                   "must never be reached when the atomic gate refuses");

    // GREEN: same setup, atomic gate now allows it -- proves the RED result
    // above was really the gate, not some other stub failing closed.
    s_test_heat_zone_claim_refused = false;
    memset(&s_at, 0, sizeof(s_at));
    s_at.thermo_bus = &bus;
    s_at.safety = &safety;
    s_at.lock = xSemaphoreCreateMutex();
    errbuf[0] = '\0';
    ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_SIMC, errbuf, sizeof(errbuf));

    TEST_CHECK(ok, "with the atomic gate allowing it, the identical setup must succeed");
    TEST_CHECK(s_heat_zone_claim_begin_calls == 2, "the gate is attempted exactly once per autotune_engine_run() call");
    TEST_CHECK(s_last_heat_zone_claimant == RELAY_HEAT_ZONE_CLAIM_AUTOTUNE,
              "autotune_engine_run() must claim as AUTOTUNE, not PROFILE");

    autotune_engine_abort("test cleanup");
    TEST_CHECK(s_heat_zone_claim_end_calls >= 1, "abort must release the heat claim it just took");

    s_test_heat_zone_claim_refused = false;
}


// ---------------------------------------------------------------------------
// Heat-enable (K4) wiring -- the 2026-08-29 fix.
//
// autotune_engine.c never asked the safety processor to permit heating: it
// drove its own zone relay against an open K4, so the element carried no
// current and every fit was made against the trace of a kiln that was never
// heated ("response too small to fit" was the engine describing its own
// inaction, again). These tests pin both halves -- the request at the start
// of a run, and the release on every terminal path -- through the same
// begin_run_locked()/force_relays_off() seams the ownership tests above use.
// ---------------------------------------------------------------------------

static void test_autotune_start_requests_heat_enable_once(void)
{
    TEST_SECTION("autotune start -- asks the safety processor to permit heating (K4), exactly once");
    reset_heat_enable_recorder(true);
    start_stepping_run(/*max_temp_c=*/0.0f, /*step_duty=*/1.0f);

    TEST_CHECK(s_req_enable_true_calls == 1,
               "begin_run_locked() must send exactly one REQUEST_ENABLE(true) -- without it the relay "
               "closes on this board and K4 stays open, which is the whole bug");
    TEST_CHECK(heat_enable_is_granted(), "and the request must be recorded as granted");
    TEST_CHECK(heat_enable_is_held(HEAT_ENABLE_CLAIMANT_AUTOTUNE), "held by the autotune claimant");
    TEST_CHECK(s_req_enable_false_calls == 0, "and nothing released it on the way in");
}

static void test_autotune_guard_trip_releases_heat_enable(void)
{
    TEST_SECTION("autotune guard trip -- gives K4 back");
    reset_heat_enable_recorder(true);
    start_stepping_run(/*max_temp_c=*/0.0f, /*step_duty=*/1.0f);
    s_req_enable_false_calls = 0;

    run_bad_sensor_ticks(/*n_ticks=*/10);

    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED, "sanity: the trip landed");
    TEST_CHECK(s_req_enable_false_calls == 1,
               "escalate_and_abort() -> force_relays_off() must send exactly one REQUEST_ENABLE(false)");
    TEST_CHECK(!heat_enable_is_granted(), "no heat request may outlive a tripped run");
}

static void test_autotune_manual_abort_releases_heat_enable(void)
{
    TEST_SECTION("autotune_engine_abort() (operator Abort) -- gives K4 back");
    reset_heat_enable_recorder(true);
    start_stepping_run(/*max_temp_c=*/0.0f, /*step_duty=*/1.0f);
    s_req_enable_false_calls = 0;

    autotune_engine_abort("operator cancelled");

    TEST_CHECK(s_req_enable_false_calls == 1, "abort_locked() -> force_relays_off() must release it");
    TEST_CHECK(!heat_enable_is_granted(), "nothing left standing");

    /* The engine's task loop calls heat_enable_release() on every tick it
     * spends in a non-running state, as a backstop. That must not put a
     * frame on the wire per tick. */
    heat_enable_release(HEAT_ENABLE_CLAIMANT_AUTOTUNE);
    heat_enable_release(HEAT_ENABLE_CLAIMANT_AUTOTUNE);
    TEST_CHECK(s_req_enable_false_calls == 1, "the per-tick backstop is free after the first release");
}

static void test_autotune_start_on_a_down_link_does_not_claim_heat(void)
{
    TEST_SECTION("autotune start with the safety link down -- never claims a request it did not send");
    reset_heat_enable_recorder(false);
    start_stepping_run(/*max_temp_c=*/0.0f, /*step_duty=*/1.0f);

    TEST_CHECK(s_req_enable_true_calls == 1, "the request is attempted");
    TEST_CHECK(!heat_enable_is_granted(),
               "but NOT recorded as granted -- reporting success into the void is the exact bug "
               "danger_mode.c had to be fixed for");
    TEST_CHECK(heat_enable_retry_pending(), "it is visible as pending instead, and retried by the watchdog");
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
    test_run_rejects_relay_only_rules_on_step_path();
    test_step_run_accepts_simc_and_cohen_coon_and_stores_the_rule();
    test_finalize_fit_uses_the_requested_rule();
    test_finalize_fit_persists_valid_cross_gain_cells();
    test_finalize_fit_routes_persist_through_flash_worker();
    test_finalize_fit_skips_a_valid_fit_with_out_of_range_gain();
    test_finalize_fit_persists_nothing_on_an_invalid_direct_fit();
    test_next_run_clears_prior_runs_refusal();
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

    test_run_refuses_while_zone_sweep_is_active();
    test_run_refuses_at_atomic_heat_claim_gate();

    // Heat-enable (K4) wiring -- each starts from its own
    // start_stepping_run(), so order-independent relative to everything
    // above.
    test_autotune_start_requests_heat_enable_once();
    test_autotune_guard_trip_releases_heat_enable();
    test_autotune_manual_abort_releases_heat_enable();
    test_autotune_start_on_a_down_link_does_not_claim_heat();
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
