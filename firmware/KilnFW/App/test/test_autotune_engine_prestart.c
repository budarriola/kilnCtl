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

/* Configurable (default false, this stub's original hardcoded behavior --
 * same convention as s_stub_max_temp_c/s_test_sweep_active above) so
 * test_autotune_engine_accept_gates_on_settled() below can exercise
 * autotune_engine_accept()'s real success path, not just its early refusals
 * (every other test in this file only reaches the "before start" or
 * "no DONE result" refusals, which never call this stub at all). */
static bool s_stub_set_pid_result = false;
bool zones_config_set_pid(uint8_t zone_index, float kp, float ki, float kd)
{
    (void)zone_index; (void)kp; (void)ki; (void)kd;
    return s_stub_set_pid_result;
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
    TEST_CHECK(!autotune_engine_accept(false), "accept must return false, not crash");
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
    /* Default ambient == baseline (the "run started cold" case) for the
     * zone under test -- this helper bypasses the real SETTLING->STEPPING
     * transition entirely (see this file's own header comment), which is
     * the only place s_at.step_ambient_c is normally set, so tests that
     * don't care about the ambient-vs-baseline distinction (added
     * 2026-09-01, see finalize_fit()'s physical-plausibility comment) get
     * the old baseline-referenced behavior by default. A test that DOES
     * care (a hot-start scenario) overrides s_at.step_ambient_c itself,
     * after calling this helper. */
    if (zone == s_at.zone_index) {
        s_at.step_ambient_c = baseline_c;
    }
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

/* Same construction as test_pid_autotune.c's pid_autotune_fit_fopdt()
 * defeat-case trace (see that file's own comment for the review's original
 * example): a fast early rise crosses 28.3% of a 20C target almost
 * immediately, then an unrealistically slow creep delays the 63.2%
 * crossing deep into the trace (inflating tau into the hundreds-to-
 * thousands of seconds), then a steeper ~0.011 degC/s tail (noise, not
 * signal) pushes the raw last-sample rise to 20C. Feeding this through the
 * REAL finalize_fit() -> pid_autotune_fit_fopdt() path (not hand-setting
 * s_at.model fields, which finalize_fit() would overwrite anyway) reliably
 * produces extrapolation_converged==false and/or tau_consistent_with_
 * gain==false via the actual iteration/cap logic, for
 * test_finalize_fit_skips_coupling_persist_when_extrapolation_did_not_
 * converge() below to exercise the coupling-persist gate's real OR
 * condition, not a hand-poked field. */
static void write_defeat_case_trace_for_zone(uint8_t zone, float baseline_c, float duty_step)
{
    s_at.zone_baseline_c[zone] = baseline_c;
    s_at.zone_baseline_valid[zone] = true;
    s_at.step_duty = duty_step;
    if (zone == s_at.zone_index) {
        s_at.step_ambient_c = baseline_c;
    }
    float v = baseline_c + 1.0f;
    uint16_t i = 0;
    s_at.zone_trace[zone][i++] = (int16_t)lroundf(v * 10.0f); /* t=10s: below target28 (5.66+baseline) */
    v = baseline_c + 6.0f;
    s_at.zone_trace[zone][i++] = (int16_t)lroundf(v * 10.0f); /* t=20s: crosses target28 */
    int creep_samples = 460;
    float creep_end_v = baseline_c + 19.0f;
    for (int k = 0; k < creep_samples; k++) {
        float frac = (float)(k + 1) / (float)creep_samples;
        v = (baseline_c + 6.0f) + frac * (creep_end_v - (baseline_c + 6.0f));
        s_at.zone_trace[zone][i++] = (int16_t)lroundf(v * 10.0f);
    }
    for (int k = 0; k < 10; k++) {
        v += 0.11f;
        s_at.zone_trace[zone][i++] = (int16_t)lroundf(v * 10.0f);
    }
    s_at.trace_count = i;
}

/* FINAL REVIEW REQUIRED TEST -- the gap that let both wire defects and
 * blocker 1 (extrapolation_converged wrongly false on the sign-check
 * break) through 1385 green checks: every "clean fit is not blocked" test
 * up to this point hand-assigns s_at.model.settled/extrapolation_converged/
 * tau_consistent_with_gain = true directly, and the one test that derives
 * the flags from real data (test_pid_autotune.c) feeds sim_plant's
 * UNQUANTIZED, strictly monotonic float samples, which can never produce a
 * negative slope_end -- so neither test category could ever exercise the
 * quantization-noise sign flip the reviewer found.
 *
 * This test drives the ACTUAL firmware path end to end: a quantized
 * synthetic trace (int16 tenths-of-a-degree, exactly what record_trace_
 * sample()/unpack_zone_trace() produce on real hardware) through
 * finalize_fit() (which calls the real pid_autotune_fit_fopdt()) and then
 * through the real autotune_engine_accept(false) -- no field is hand-set.
 * The trace is a clean K=100/tau=200/L=20 response run for 8*tau (1600s,
 * matching the reviewer's own repro), quantized-flat at the end, with the
 * FINAL sample dithered by -0.1, 0.0 and +0.1 degC in turn (the exact
 * quantization noise a real MAX31856 reading rounds to). All three must
 * ACCEPT WITHOUT ack_unsettled -- a well-converged 8-tau fit is never
 * low-confidence regardless of which way its last quantization tick fell.
 *
 * Confirmed to FAIL on the -0.1 case against the pre-fix code (the sign-
 * check break at pid_autotune.c's `(candidate * rise_sign) < (raw_rise *
 * rise_sign)` left extrapolation_converged at its pessimistic `false`
 * default): see this file's own build log for the exact failing output.
 * Restoring the fix (extrapolation_converged = true on that break) makes
 * all three dithers pass. */
static void run_one_dither_case_end_to_end(float dither_c, const char *label)
{
    static MAX31856BusClass bus;
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    s_at.lock = xSemaphoreCreateMutex();
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");
    s_at.zone_index = 0;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;
    s_at.step_settled = true; /* the STEPPING-phase detector genuinely fired -- isolates the
                               * extrapolation-flag defect from the settled gate entirely */

    const float baseline_c = 25.0f;
    const float k_gain_c_per_duty = 100.0f;
    const float tau_s = 200.0f;
    const float dead_time_s = 20.0f;
    const float duty_step = 1.0f;
    const uint16_t n_samples = (uint16_t)((8.0f * tau_s) / (float)AUTOTUNE_ENGINE_SAMPLE_PERIOD_S); /* 8*tau */

    write_synthetic_fopdt_trace_for_zone(0, baseline_c, k_gain_c_per_duty, tau_s, dead_time_s, duty_step,
                                         n_samples);
    /* Flatten the trailing ASYMPTOTE_SLOPE_WINDOW_SAMPLES (10, generous
     * margin over pid_autotune.c's own 6) samples to a SINGLE quantized
     * value before dithering -- the reviewer's own repro ("settled 8tau,
     * flat end") and the exact case a real settled kiln's near-zero
     * residual slope quantizes down to. Without this, the natural
     * synthetic exponential's own tiny (sub-quantum at 8*tau, but not
     * exactly zero) residual slope can itself already be positive, which
     * masks the sign-check branch entirely -- a first version of this test
     * made exactly that mistake: it dithered the last sample of the RAW
     * exponential tail, which still had enough natural upward drift in the
     * window that slope_end never actually went negative, so the test
     * passed identically with and without blocker 1's fix (a worthless
     * negative test -- see this function's own investigation in the
     * session transcript that led to this fix). Flattening first, then
     * dithering ONLY the last sample, is what actually isolates the sign
     * flip the reviewer measured. */
    int16_t flat_value = s_at.zone_trace[0][n_samples - 11];
    for (uint16_t k = n_samples - 10; k < n_samples; k++) {
        s_at.zone_trace[0][k] = flat_value;
    }
    /* Dither ONLY the final (already-flattened) sample by the requested
     * amount, in the same 0.1 degC integer units record_trace_sample()
     * itself packs -- this is exactly the quantization-noise tick the
     * reviewer's repro isolates, not a synthetic rewrite of the whole
     * trace. */
    s_at.zone_trace[0][n_samples - 1] = flat_value + (int16_t)lroundf(dither_c * 10.0f);

    finalize_fit();

    char msg[128];
    snprintf(msg, sizeof(msg), "%s: the direct fit must succeed (a clean 8*tau trace)", label);
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, msg);
    if (s_at.state != AUTOTUNE_ENGINE_DONE) {
        return;
    }
    snprintf(msg, sizeof(msg), "%s: sanity -- this run is genuinely settled (isolating the "
                              "extrapolation flags)", label);
    TEST_CHECK(s_at.model.settled, msg);

    s_stub_set_pid_result = true;
    bool accepted = autotune_engine_accept(false);
    snprintf(msg, sizeof(msg),
            "%s: a well-converged 8*tau fit must ACCEPT WITHOUT ack_unsettled (converged=%d "
            "tau_ok=%d) -- quantization noise on the last sample must not gate acceptance",
            label, (int)s_at.model.extrapolation_converged, (int)s_at.model.tau_consistent_with_gain);
    TEST_CHECK(accepted, msg);
    s_stub_set_pid_result = false;
}

static void test_accept_succeeds_end_to_end_regardless_of_last_sample_quantization_dither(void)
{
    TEST_SECTION("FINAL REVIEW: autotune_engine_accept(false) must succeed end-to-end on a clean "
                 "8*tau fit whether the last (quantized) sample dithers -0.1, 0.0, or +0.1 degC");
    run_one_dither_case_end_to_end(-0.1f, "dither -0.1C");
    run_one_dither_case_end_to_end(0.0f, "dither 0.0C");
    run_one_dither_case_end_to_end(+0.1f, "dither +0.1C");
}

static void test_finalize_fit_uses_the_requested_rule(void)
{
    TEST_SECTION("finalize_fit() uses s_at.step_rule, not a hardcoded SIMC -- PID_EXPANSION_PLAN.md Phase 1");
    // K=50 degC/duty, tau=200s, L=20s (well above
    // AUTOTUNE_COHEN_COON_MIN_DEAD_TIME_S so Cohen-Coon does not refuse on
    // dead time), 60 samples * 10s/sample = 600s -- comfortably past 5*tau
    // so both the two-point crossing fit AND the settle band would succeed.
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;
    write_synthetic_fopdt_trace(/*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/50.0f, /*tau_s=*/200.0f,
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
    write_synthetic_fopdt_trace(/*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/50.0f, /*tau_s=*/200.0f,
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
    // 2026-09-02 review fix (item 4): coupling persist is now gated on
    // model.settled (s_at.step_settled) -- this test is about the persist
    // MECHANISM (right cell, right value, diagonal untouched), not about
    // that gate, so simulate the honest detector having genuinely fired.
    s_at.step_settled = true;

    // Zone 1 (self): K=50, tau=200s, L=20s -- the direct fit finalize_fit()
    // needs to succeed before it ever reaches the cross-zone loop.
    write_synthetic_fopdt_trace_for_zone(1, /*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/50.0f, /*tau_s=*/200.0f,
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
    // See test_finalize_fit_persists_valid_cross_gain_cells()'s own comment:
    // this test is about the flash-worker ROUTING, not the settled gate.
    s_at.step_settled = true;

    write_synthetic_fopdt_trace_for_zone(1, /*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/50.0f, /*tau_s=*/200.0f,
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

/* Item (4), round-2 review: coupling-cell persistence used to run
 * unconditionally on every DONE run, INCLUDING one that reached DONE via
 * the max-duration backstop with model.settled==false -- the same
 * low-confidence peer traces the (B)/(C) refusals guard on the direct fit,
 * persisted to flash before the operator ever sees an Accept button. Same
 * trace shape as test_finalize_fit_persists_valid_cross_gain_cells() above,
 * but WITHOUT setting s_at.step_settled -- must persist NOTHING. */
static void test_finalize_fit_skips_coupling_persist_when_unsettled(void)
{
    TEST_SECTION("(4) finalize_fit() does NOT persist cross-gain coupling cells when the fit never "
                 "genuinely settled -- the same fits used to slip past this gate entirely");
    memset(&s_at, 0, sizeof(s_at));
    reset_coupling_cell_calls();
    g_flash_worker_submit_calls = 0;
    s_stub_thermo_count = 3;
    s_at.zone_index = 1;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;
    s_at.step_settled = false; /* the case under test -- max-duration backstop */

    write_synthetic_fopdt_trace_for_zone(1, /*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/50.0f, /*tau_s=*/200.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/22.0f, /*k_gain_c_per_duty=*/0.5f, /*tau_s=*/150.0f,
                                         /*dead_time_s=*/15.0f, /*duty_step=*/1.0f, /*n_samples=*/60);

    finalize_fit();
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "the direct fit itself still succeeds -- settled gates "
                                                    "persistence, not the fit");
    TEST_CHECK(!s_at.model.settled, "sanity: this run really is unsettled");
    TEST_CHECK(!s_coupling_cell_calls[0][1].called,
              "zone 0's cross-gain cell must NOT be persisted when the run never settled");
    TEST_CHECK(g_flash_worker_submit_calls == 0,
              "no job at all must reach the flash worker for an unsettled run's coupling cells");

    s_stub_thermo_count = 0;
}

/* Round-3 follow-up: the coupling-persist gate was extended to require
 * extrapolation_converged AND tau_consistent_with_gain too, not just
 * settled (see finalize_fit()'s own "Argued explicitly" comment for why).
 * Drives the REAL pid_autotune_fit_fopdt() through a defeat-case trace
 * (write_defeat_case_trace_for_zone(), same construction as test_pid_
 * autotune.c's own defeat case) on the DIRECT zone, with step_settled=true
 * so settled alone would NOT have blocked persistence -- only the
 * extension is under test here. Confirmed to FAIL (coupling persisted
 * anyway) when finalize_fit()'s coupling-persist condition is temporarily
 * stubbed back to `if (s_at.model.settled)` (the round-2-only gate) --
 * restoring the three-flag condition makes it pass again. */
static void test_finalize_fit_skips_coupling_persist_when_extrapolation_did_not_converge(void)
{
    TEST_SECTION("(round-3) finalize_fit() does NOT persist cross-gain coupling cells when the "
                 "direct fit's extrapolation did not converge / tau is inconsistent, even though "
                 "the run itself settled");
    memset(&s_at, 0, sizeof(s_at));
    reset_coupling_cell_calls();
    g_flash_worker_submit_calls = 0;
    s_stub_thermo_count = 2;
    s_at.zone_index = 1;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;
    s_at.step_settled = true; /* isolates the extension: settled alone must NOT be enough here */

    write_defeat_case_trace_for_zone(1, /*baseline_c=*/25.0f, /*duty_step=*/1.0f);
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/22.0f, /*k_gain_c_per_duty=*/0.5f, /*tau_s=*/150.0f,
                                         /*dead_time_s=*/15.0f, /*duty_step=*/1.0f,
                                         /*n_samples=*/s_at.trace_count);

    finalize_fit();
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "the direct fit itself still succeeds");
    TEST_CHECK(s_at.model.settled, "sanity: settled is true -- isolating the NEW conditions specifically");
    TEST_CHECK(!s_at.model.extrapolation_converged || !s_at.model.tau_consistent_with_gain,
              "sanity: the defeat-case trace really does produce an unconverged/tau-inconsistent "
              "fit -- if this fails, the trace doesn't reproduce the defeat case");
    TEST_CHECK(!s_coupling_cell_calls[0][1].called,
              "zone 0's cross-gain cell must NOT be persisted when the direct fit's extrapolation "
              "is not fully trustworthy, even though the run settled");
    TEST_CHECK(g_flash_worker_submit_calls == 0,
              "no job at all must reach the flash worker for this run's coupling cells");

    s_stub_thermo_count = 0;
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

    write_synthetic_fopdt_trace_for_zone(1, /*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/50.0f, /*tau_s=*/200.0f,
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

// ---------------------------------------------------------------------------
// 2026-08-31/2026-09-01: honest settle detection (A), minimum-excursion (B)
// and physical-plausibility (C) tests -- the overshoot fix and its
// 2026-09-01 review-fix follow-up (asymptote extrapolation, the settled
// flag actually gating accept(), criterion 2's removal, filtered/early-
// region-only peak-slope tracking, a re-derived noise floor, span-scaled
// minimum excursion, an honest guard-headroom justification, ambient- not
// baseline-referenced plausibility, and the AUTOTUNE_ENGINE_MAX_SAMPLES
// latent defect). See autotune_engine.c's SETTLE_RELATIVE_SLOPE_FRAC/
// AUTOTUNE_MIN_RISE_FRACTION_OF_SPAN comments and finalize_fit()'s/
// pid_autotune_fit_fopdt()'s own comments for the full defect and fix.
// ---------------------------------------------------------------------------

/* Deliberate, clearly-labelled REPLICA of the pre-fix fixed-band detector
 * (its constant, SETTLE_CHECK_BAND_C, no longer exists in production code --
 * this is NOT a call into any real function) used only to prove the
 * negative half of test_settle_detector_rejects_a_slow_constant_ramp_the_
 * old_one_accepted() below: that the OLD logic really would have declared
 * the test's synthetic ramp settled, so the new detector's refusal is a
 * genuine behavior change and not a test that would have passed either way. */
#define OLD_SETTLE_CHECK_BAND_C 1.0f
static bool old_fixed_band_would_settle(uint8_t zone, uint16_t trace_count)
{
    if (trace_count < MIN_STEPPING_SAMPLES_BEFORE_SETTLE_CHECK || trace_count < SETTLE_CHECK_SAMPLES) {
        return false;
    }
    float lo = 1e9f, hi = -1e9f;
    for (uint16_t i = trace_count - SETTLE_CHECK_SAMPLES; i < trace_count; i++) {
        int16_t dc = s_at.zone_trace[zone][i];
        if (dc == AUTOTUNE_TRACE_TEMP_INVALID) continue;
        float v = (float)dc / 10.0f;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    return (hi - lo) <= OLD_SETTLE_CHECK_BAND_C;
}

// Drives step_settle_check_locked() (the real production seam, see its own
// comment in autotune_engine.c for why this is called directly rather than
// through 1000+ real ticks) one synthetic sample at a time over a slow,
// CONSTANT-rate ramp: flat within 1.0C over any 60s (6-sample) window, but
// unmistakably still climbing over the run as a whole (30C of rise by the
// end). This is exactly the shape of trace a kiln's true multi-minute time
// constant produces early on, and exactly what the fixed 1.0C absolute band
// mistook for steady state.
static void test_settle_detector_rejects_a_slow_constant_ramp_the_old_one_accepted(void)
{
    TEST_SECTION("(A) settle detector -- a slow constant-rate ramp, flat within 1.0C over any 60s "
                 "window but still clearly rising overall, must be REJECTED by the new relative-slope "
                 "detector even though the OLD fixed-band detector would have accepted it");
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    s_at.zone_baseline_c[0] = 25.0f;
    s_at.zone_baseline_valid[0] = true;

    const float baseline_c = 25.0f;
    const float rate_c_per_s = 0.015f; /* 0.15C/10s sample -> 0.75C over the 50s (6-sample) settle window */
    const uint16_t n_samples = 200;    /* 2000s of stepping -- rise reaches 30C, unmistakably still climbing */

    bool new_detector_ever_fired = false;
    bool old_detector_would_have_fired = false;
    uint16_t old_detector_first_fired_at = 0;

    for (uint16_t i = 0; i < n_samples; i++) {
        float t_s = (float)i * (float)AUTOTUNE_ENGINE_SAMPLE_PERIOD_S;
        float v = baseline_c + rate_c_per_s * t_s;
        s_at.zone_trace[0][i] = (int16_t)lroundf(v * 10.0f);
        s_at.trace_count = i + 1;
        s_at.elapsed_s = (uint32_t)t_s;

        if (!old_detector_would_have_fired && old_fixed_band_would_settle(0, s_at.trace_count)) {
            old_detector_would_have_fired = true;
            old_detector_first_fired_at = s_at.trace_count;
        }
        if (step_settle_check_locked()) {
            new_detector_ever_fired = true;
        }
    }

    TEST_CHECK(old_detector_would_have_fired,
              "sanity: the OLD fixed-band detector really would have accepted this ramp as settled -- "
              "if this fails, the test trace doesn't reproduce the defect");
    TEST_CHECK(old_detector_first_fired_at == MIN_STEPPING_SAMPLES_BEFORE_SETTLE_CHECK,
              "sanity: the old detector fired at the earliest possible sample (120s in), i.e. deep mid-transient");
    TEST_CHECK(!new_detector_ever_fired,
              "the new relative-slope detector must NEVER declare this constant-rate ramp settled over "
              "the whole 2000s run -- its recent slope never decays relative to its own peak, so "
              "criterion 1 never holds");
}

/* MANDATORY per review: a POSITIVE test that the detector actually fires on
 * a genuine clean FOPDT step response (K=100, tau=200s, L=20s -- the same
 * shape write_synthetic_fopdt_trace_for_zone() builds, but driven sample by
 * sample through the real step_settle_check_locked() seam here since that
 * helper only writes the whole array at once and this test needs to watch
 * WHEN it first fires). Confirmed to FAIL (never fires) when step_settle_
 * check_locked()'s body is temporarily replaced with a bare `return false;`
 * -- restoring the real body makes it pass again; see this test's own
 * TEST_CHECK for the exact assertion that flips. */
static void test_settle_detector_fires_on_a_clean_exponential_no_earlier_than_3tau(void)
{
    TEST_SECTION("(A) settle detector -- POSITIVE: fires on a genuine clean FOPDT step response, "
                 "no earlier than ~3*tau past dead time");
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;

    const float baseline_c = 25.0f;
    const float k_gain_c_per_duty = 100.0f;
    const float tau_s = 200.0f;
    const float dead_time_s = 20.0f;
    const float duty_step = 1.0f;
    const uint16_t n_samples = 120; /* 1200s -- comfortably past dead_time + 3*tau (=620s) */

    int32_t fired_at_sample = -1;
    for (uint16_t i = 0; i < n_samples; i++) {
        float t_s = (float)i * (float)AUTOTUNE_ENGINE_SAMPLE_PERIOD_S;
        float rise = (t_s <= dead_time_s)
                         ? 0.0f
                         : k_gain_c_per_duty * duty_step * (1.0f - expf(-(t_s - dead_time_s) / tau_s));
        s_at.zone_trace[0][i] = (int16_t)lroundf((baseline_c + rise) * 10.0f);
        s_at.trace_count = i + 1;
        s_at.elapsed_s = (uint32_t)t_s;
        if (fired_at_sample < 0 && step_settle_check_locked()) {
            fired_at_sample = (int32_t)i;
        }
    }

    TEST_CHECK(fired_at_sample >= 0,
              "the detector must fire at some point on a trace that genuinely reaches steady state -- "
              "if this fails with the real (non-stubbed) detector body, something is badly wrong, not "
              "just the review's specific complaints");
    if (fired_at_sample >= 0) {
        float fired_at_t_s = (float)fired_at_sample * (float)AUTOTUNE_ENGINE_SAMPLE_PERIOD_S;
        TEST_CHECK(fired_at_t_s >= dead_time_s + 3.0f * tau_s,
                  "must not fire earlier than dead_time + 3*tau (620s) -- firing on a genuine "
                  "exponential before the response has actually decayed enough would be exactly the "
                  "original mid-transient-settle defect, just with a real trace instead of a ramp");
    }
}

/* Covers the early "peak never cleared the noise floor" gate (bails out
 * before any ratio/floor comparison at all): a perfectly flat trace (a
 * sensor that never moved, or heat that never actually applied) must never
 * be declared settled, however long it runs -- there is nothing here for
 * "settled" to mean. */
static void test_settle_detector_never_fires_on_a_perfectly_flat_trace(void)
{
    TEST_SECTION("(A) settle detector -- a perfectly flat trace (peak slope never clears the noise "
                 "floor) must never fire, however long it runs");
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    const uint16_t n_samples = 60;
    for (uint16_t i = 0; i < n_samples; i++) {
        s_at.zone_trace[0][i] = (int16_t)lroundf(25.0f * 10.0f); /* bit-identical every sample */
        s_at.trace_count = i + 1;
        s_at.elapsed_s = (uint32_t)i * AUTOTUNE_ENGINE_SAMPLE_PERIOD_S;
        TEST_CHECK(!step_settle_check_locked(),
                  "a trace that has never moved at all must never be declared settled");
    }
}

/* Item (5) -- the re-derived noise floor. Constructs a trace whose PEAK
 * slope (0.01 C/s, set in the first PEAK_SLOPE_WINDOW_SAMPLES) clears the
 * floor easily, but whose RECENT slope settles to exactly one trace
 * quantization step over the recent-slope window (0.1C / 50s = 0.002 C/s)
 * -- deliberately too small a ratio to peak (0.002 / 0.01 = 20%, well above
 * SETTLE_RELATIVE_SLOPE_FRAC=4%) for criterion 1's ratio arm to fire, so
 * ONLY the absolute-floor arm can settle this trace. The OLD floor (0.0008)
 * is SMALLER than this 0.002 quantum and would never have caught it; the
 * NEW floor (0.003) is LARGER and does. Confirmed to FAIL when
 * SETTLE_ABS_SLOPE_FLOOR_C_PER_S is temporarily reverted to 0.0008f --
 * restoring 0.003f makes it pass again; see the TEST_CHECK below. */
static void test_settle_detector_floor_arm_reachable_at_one_quantum(void)
{
    TEST_SECTION("(A) settle detector -- item (5): the re-derived absolute floor must be reachable by "
                 "a real (if minimal) one-quantum recent slope, unlike the old 0.0008 constant");
    TEST_CHECK(SETTLE_ABS_SLOPE_FLOOR_C_PER_S > 0.1f / (float)((SETTLE_CHECK_SAMPLES - 1) * AUTOTUNE_ENGINE_SAMPLE_PERIOD_S),
              "sanity: the floor constant itself must exceed one trace-quantization step over the "
              "recent-slope window, or the arithmetic in this test's own header comment is wrong");

    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    float v = 25.0f;
    uint16_t i = 0;
    /* Peak-setting segment: +0.1C every sample for 5 steps (6 samples,
     * PEAK_SLOPE_WINDOW_SAMPLES) -> 0.5C over 50s = 0.01 C/s peak, captured
     * within the early PEAK_SLOPE_SEARCH_SAMPLES region. */
    for (; i < PEAK_SLOPE_WINDOW_SAMPLES; i++) {
        s_at.zone_trace[0][i] = (int16_t)lroundf(v * 10.0f);
        v += 0.1f;
        s_at.trace_count = i + 1;
        s_at.elapsed_s = (uint32_t)i * AUTOTUNE_ENGINE_SAMPLE_PERIOD_S;
        (void)step_settle_check_locked();
    }
    /* Continuation segment: +0.02C every sample (0.002 C/s = one recent-
     * slope-window quantum) for the rest of the trace. */
    bool fired = false;
    for (; i < 60; i++) {
        s_at.zone_trace[0][i] = (int16_t)lroundf(v * 10.0f);
        v += 0.02f;
        s_at.trace_count = i + 1;
        s_at.elapsed_s = (uint32_t)i * AUTOTUNE_ENGINE_SAMPLE_PERIOD_S;
        if (step_settle_check_locked()) {
            fired = true;
        }
    }
    TEST_CHECK(fired,
              "must fire once the recent-slope window is entirely inside the 0.002 C/s continuation -- "
              "the ratio arm (4% of the 0.01 C/s peak = 0.0004) cannot reach this, so only a floor "
              "big enough to cover one real quantization step can");
}

/* Item (9) -- the AUTOTUNE_ENGINE_MAX_SAMPLES latent defect. Simulates a
 * trace that hit the buffer cap: trace_count pinned at AUTOTUNE_ENGINE_
 * MAX_SAMPLES with the trailing window frozen (bit-identical samples, since
 * record_trace_sample() stopped appending -- see that function's own
 * comment). Without the explicit guard this would read recent_slope==0 and
 * fire true on a trace that may still have been climbing the instant it was
 * truncated; WITH the guard it must return false unconditionally so the
 * max-duration backstop handles it (and finalize_fit() marks it unsettled).
 * Confirmed to FAIL (fires true) when the `trace_count >=
 * AUTOTUNE_ENGINE_MAX_SAMPLES` guard is temporarily removed from step_
 * settle_check_locked() -- restoring it makes this pass again. */
static void test_settle_detector_refuses_a_frozen_full_trace(void)
{
    TEST_SECTION("(A) settle detector -- item (9): a trace pinned at AUTOTUNE_ENGINE_MAX_SAMPLES with a "
                 "frozen trailing window must NOT be declared settled just because recent_slope reads 0");
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    s_at.trace_count = AUTOTUNE_ENGINE_MAX_SAMPLES;
    /* A real peak earlier in the (never-recorded, out of window) trace --
     * doesn't matter for this test since the guard must bail before this is
     * even read, but set it anyway so a broken guard would fall through to
     * a realistic (not degenerate-zero-peak) settle evaluation, making a
     * regression here fail for the RIGHT reason (fires true) rather than an
     * unrelated one (peak==0 gate). */
    s_at.step_peak_slope_c_per_s = 0.5f;
    s_at.step_peak_slope_at_s = 100u;
    s_at.elapsed_s = 14400u; /* well past the peak, plausible late-run value */
    for (uint16_t i = AUTOTUNE_ENGINE_MAX_SAMPLES - SETTLE_CHECK_SAMPLES; i < AUTOTUNE_ENGINE_MAX_SAMPLES; i++) {
        s_at.zone_trace[0][i] = (int16_t)lroundf(180.0f * 10.0f); /* identical, frozen value */
    }
    TEST_CHECK(!step_settle_check_locked(),
              "a full, frozen trace must never be declared settled -- recent_slope==0 here means the "
              "buffer stopped recording, not that the plant stopped moving");
}

/* Item (5), round-2 review: dead time > 300s (PEAK_SLOPE_SEARCH_SAMPLES*
 * AUTOTUNE_ENGINE_SAMPLE_PERIOD_S, the OLD fixed search-window cutoff) used
 * to permanently disable the peak gate -- the response never got a chance
 * to be recorded as the peak, so step_peak_slope_c_per_s stayed ~0 and the
 * detector could never fire, however cleanly the plant went on to settle.
 * This trace has a 400s dead time (dead_time_s=400 > 300) followed by a
 * clean K=100/tau=100 response; the detector MUST still eventually fire,
 * proving onset anchoring (not a fixed sample-count cutoff) is what
 * actually gates the peak search now. Confirmed to FAIL (never fires) when
 * step_onset_seen's own gate is temporarily stubbed to require trace_count
 * <= PEAK_SLOPE_SEARCH_SAMPLES (the old fixed-window behavior) instead of
 * the onset-relative window -- restoring the real onset-anchored gate makes
 * it pass again; see the TEST_CHECK below. */
static void test_settle_detector_fires_with_dead_time_over_300s(void)
{
    TEST_SECTION("(5) settle detector -- a dead time > 300s (the old fixed peak-search cutoff) must "
                 "NOT permanently disable the detector; it must still fire once the response starts");
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;

    const float baseline_c = 25.0f;
    const float k_gain_c_per_duty = 100.0f;
    const float tau_s = 100.0f;
    const float dead_time_s = 400.0f; /* > PEAK_SLOPE_SEARCH_SAMPLES*10s=300s -- the whole point */
    const float duty_step = 1.0f;
    const uint16_t n_samples = 120; /* 1200s -- dead_time(400) + ~3*tau(300) = 700s, comfortable margin */

    int32_t fired_at_sample = -1;
    for (uint16_t i = 0; i < n_samples; i++) {
        float t_s = (float)i * (float)AUTOTUNE_ENGINE_SAMPLE_PERIOD_S;
        float rise = (t_s <= dead_time_s)
                         ? 0.0f
                         : k_gain_c_per_duty * duty_step * (1.0f - expf(-(t_s - dead_time_s) / tau_s));
        s_at.zone_trace[0][i] = (int16_t)lroundf((baseline_c + rise) * 10.0f);
        s_at.trace_count = i + 1;
        s_at.elapsed_s = (uint32_t)t_s;
        if (fired_at_sample < 0 && step_settle_check_locked()) {
            fired_at_sample = (int32_t)i;
        }
    }

    TEST_CHECK(fired_at_sample >= 0,
              "must eventually fire despite a 400s dead time -- the OLD fixed-30-sample search window "
              "would leave step_peak_slope_c_per_s at 0 forever and this would never fire");
    TEST_CHECK(s_at.step_onset_seen, "response onset must have been detected at all");
    if (s_at.step_onset_seen) {
        TEST_CHECK(s_at.step_onset_trace_count > PEAK_SLOPE_SEARCH_SAMPLES,
                  "sanity: onset really did occur past sample 30 (the old fixed cutoff) -- if this "
                  "fails, the trace's dead time isn't actually exercising the >300s case");
    }
}

/* Item (5) sub-case, round-2 review: two coincidental quantization ticks
 * (0.1 + 0.1 degC) landing inside one PEAK_SLOPE_WINDOW_SAMPLES window
 * DURING dead time give a windowed slope of 0.2/50=0.004 degC/s -- above
 * the OLD-style bare noise floor (0.003) but must NOT be mistaken for
 * response onset (RESPONSE_ONSET_SLOPE_C_PER_S=0.009 is specifically 3x the
 * floor so this case falls short). Confirmed to FAIL (onset falsely
 * latches) when RESPONSE_ONSET_SLOPE_C_PER_S is temporarily stubbed down to
 * SETTLE_ABS_SLOPE_FLOOR_C_PER_S (1x instead of 3x) -- restoring the real
 * 3x margin makes it pass again; see the TEST_CHECK below. */
static void test_settle_detector_ignores_two_quantum_dead_time_noise(void)
{
    TEST_SECTION("(5) settle detector -- two coincidental quantization ticks during dead time must "
                 "NOT be mistaken for response onset");
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;

    float v = 25.0f;
    uint16_t i = 0;
    /* Flat for a while (well past MIN_STEPPING_SAMPLES_BEFORE_SETTLE_CHECK),
     * simulating dead time. */
    for (; i < 20; i++) {
        s_at.zone_trace[0][i] = (int16_t)lroundf(v * 10.0f);
        s_at.trace_count = i + 1;
        s_at.elapsed_s = (uint32_t)i * AUTOTUNE_ENGINE_SAMPLE_PERIOD_S;
        (void)step_settle_check_locked();
    }
    /* Two quantization ticks (+0.1C each) landing within the next
     * PEAK_SLOPE_WINDOW_SAMPLES-sample window, then flat again -- 0.2C
     * total over the 6-sample/50s window = 0.004 degC/s. */
    v += 0.1f;
    s_at.zone_trace[0][i] = (int16_t)lroundf(v * 10.0f);
    s_at.trace_count = ++i;
    s_at.elapsed_s = (uint32_t)(i - 1) * AUTOTUNE_ENGINE_SAMPLE_PERIOD_S;
    (void)step_settle_check_locked();
    v += 0.1f;
    s_at.zone_trace[0][i] = (int16_t)lroundf(v * 10.0f);
    s_at.trace_count = ++i;
    s_at.elapsed_s = (uint32_t)(i - 1) * AUTOTUNE_ENGINE_SAMPLE_PERIOD_S;
    (void)step_settle_check_locked();
    for (; i < 30; i++) {
        s_at.zone_trace[0][i] = (int16_t)lroundf(v * 10.0f); /* flat again */
        s_at.trace_count = i + 1;
        s_at.elapsed_s = (uint32_t)i * AUTOTUNE_ENGINE_SAMPLE_PERIOD_S;
        (void)step_settle_check_locked();
    }

    TEST_CHECK(!s_at.step_onset_seen,
              "two coincidental quantization ticks (0.004 degC/s) must NOT clear "
              "RESPONSE_ONSET_SLOPE_C_PER_S (0.009 degC/s) -- a false onset here is exactly the "
              "spurious mid-dead-time settle this sub-case guards against");
}

/* Item (2) -- autotune_engine_accept()'s new ack_unsettled gate. Constructs
 * a DONE, STEP-method result whose model.settled is false (as if finalize_
 * fit() had reached DONE via the max-duration backstop) and proves accept()
 * refuses it without an explicit ack, then accepts it with one. Confirmed
 * to FAIL (both calls return true) when the `!s_at.model.settled &&
 * !ack_unsettled` gate is temporarily stubbed to `if (0)` in autotune_
 * engine.c -- restoring the real gate makes this pass again. */
static void test_autotune_engine_accept_gates_on_settled(void)
{
    TEST_SECTION("(2) autotune_engine_accept() refuses an UNSETTLED fit without ack_unsettled=true, "
                 "and accepts it with one");
    static MAX31856BusClass bus;
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    s_at.lock = xSemaphoreCreateMutex();
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");
    s_at.state = AUTOTUNE_ENGINE_DONE;
    s_at.method = AUTOTUNE_METHOD_STEP;
    s_at.zone_index = 0;
    s_at.model.valid = true;
    s_at.model.settled = false; /* the case under test -- isolated: the other two read true */
    s_at.model.extrapolation_converged = true;
    s_at.model.tau_consistent_with_gain = true;
    s_at.model.k_gain_c_per_duty = 10.0f;
    s_at.model.tau_s = 100.0f;
    s_at.model.dead_time_s = 5.0f;

    s_stub_set_pid_result = true; /* so a would-be-successful accept has something to report */
    bool refused = autotune_engine_accept(false);
    TEST_CHECK(!refused, "an unsettled STEP result must be refused without ack_unsettled");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "a refused accept must not have reset the engine to IDLE");

    bool accepted = autotune_engine_accept(true);
    TEST_CHECK(accepted, "the SAME unsettled result must be accepted once ack_unsettled=true");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_IDLE, "a successful accept resets the engine to IDLE");
    s_stub_set_pid_result = false; /* restore this file's default for every other test */
}

/* Round-3 follow-up: the SAME gate, isolating extrapolation_converged ==
 * false (settled and tau_consistent both true) -- proves the gate was
 * genuinely EXTENDED to this flag, not left checking settled alone.
 * Confirmed to FAIL (both calls return true) when the `!converged` half of
 * the gate's OR is temporarily stubbed to `false` in autotune_engine.c --
 * restoring it makes this pass again. */
static void test_autotune_engine_accept_gates_on_extrapolation_converged(void)
{
    TEST_SECTION("(round-3) autotune_engine_accept() refuses a fit whose extrapolation did not "
                 "converge, without ack_unsettled=true, and accepts it with one");
    static MAX31856BusClass bus;
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    s_at.lock = xSemaphoreCreateMutex();
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");
    s_at.state = AUTOTUNE_ENGINE_DONE;
    s_at.method = AUTOTUNE_METHOD_STEP;
    s_at.zone_index = 0;
    s_at.model.valid = true;
    s_at.model.settled = true;
    s_at.model.extrapolation_converged = false; /* the case under test -- isolated */
    s_at.model.tau_consistent_with_gain = true;
    s_at.model.k_gain_c_per_duty = 10.0f;
    s_at.model.tau_s = 100.0f;
    s_at.model.dead_time_s = 5.0f;

    s_stub_set_pid_result = true;
    bool refused = autotune_engine_accept(false);
    TEST_CHECK(!refused, "an unconverged extrapolation must be refused without ack_unsettled, even "
                         "though settled and tau_consistent are both true");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "a refused accept must not have reset the engine to IDLE");

    bool accepted = autotune_engine_accept(true);
    TEST_CHECK(accepted, "the SAME result must be accepted once ack_unsettled=true");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_IDLE, "a successful accept resets the engine to IDLE");
    s_stub_set_pid_result = false;
}

/* Round-3 follow-up: the SAME gate, isolating tau_consistent_with_gain ==
 * false (settled and extrapolation_converged both true). Confirmed to FAIL
 * (both calls return true) when the `!tau_ok` half of the gate's OR is
 * temporarily stubbed to `false` in autotune_engine.c -- restoring it makes
 * this pass again. */
static void test_autotune_engine_accept_gates_on_tau_consistent(void)
{
    TEST_SECTION("(round-3) autotune_engine_accept() refuses a fit whose tau is inconsistent with "
                 "the corrected gain, without ack_unsettled=true, and accepts it with one");
    static MAX31856BusClass bus;
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    s_at.lock = xSemaphoreCreateMutex();
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");
    s_at.state = AUTOTUNE_ENGINE_DONE;
    s_at.method = AUTOTUNE_METHOD_STEP;
    s_at.zone_index = 0;
    s_at.model.valid = true;
    s_at.model.settled = true;
    s_at.model.extrapolation_converged = true;
    s_at.model.tau_consistent_with_gain = false; /* the case under test -- isolated */
    s_at.model.k_gain_c_per_duty = 10.0f;
    s_at.model.tau_s = 100.0f;
    s_at.model.dead_time_s = 5.0f;

    s_stub_set_pid_result = true;
    bool refused = autotune_engine_accept(false);
    TEST_CHECK(!refused, "a tau-inconsistent fit must be refused without ack_unsettled, even though "
                         "settled and extrapolation_converged are both true");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "a refused accept must not have reset the engine to IDLE");

    bool accepted = autotune_engine_accept(true);
    TEST_CHECK(accepted, "the SAME result must be accepted once ack_unsettled=true");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_IDLE, "a successful accept resets the engine to IDLE");
    s_stub_set_pid_result = false;
}

/* MANDATORY positive case (explicitly requested): a fully clean result --
 * settled, converged, AND tau-consistent, all true -- must be accepted
 * WITHOUT ack_unsettled at all. The failure mode a gate like this must
 * never have is "nobody can open it even when they shouldn't need to";
 * this is the test that would catch a gate accidentally left requiring
 * ack_unsettled unconditionally (e.g. an `||` that should have been `&&`,
 * or a stray `!` on one of the three conditions). */
static void test_autotune_engine_accept_does_not_block_a_fully_clean_fit(void)
{
    TEST_SECTION("(round-3) autotune_engine_accept() must NOT require ack_unsettled for a fit that "
                 "is settled, converged, AND tau-consistent -- a gate nobody can open is worse than "
                 "no gate");
    static MAX31856BusClass bus;
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    s_at.lock = xSemaphoreCreateMutex();
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");
    s_at.state = AUTOTUNE_ENGINE_DONE;
    s_at.method = AUTOTUNE_METHOD_STEP;
    s_at.zone_index = 0;
    s_at.model.valid = true;
    s_at.model.settled = true;
    s_at.model.extrapolation_converged = true;
    s_at.model.tau_consistent_with_gain = true;
    s_at.model.k_gain_c_per_duty = 10.0f;
    s_at.model.tau_s = 100.0f;
    s_at.model.dead_time_s = 5.0f;

    s_stub_set_pid_result = true;
    bool accepted = autotune_engine_accept(false);
    TEST_CHECK(accepted, "a fully clean fit must be accepted with ack_unsettled=false -- the common, "
                         "healthy path must never require the acknowledgement checkbox");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_IDLE, "a successful accept resets the engine to IDLE");
    s_stub_set_pid_result = false;
}

static void test_min_excursion_refuses_a_fit_below_the_rise_floor(void)
{
    TEST_SECTION("(B) minimum-excursion requirement -- a fit whose total rise is below the "
                 "(now span-scaled) minimum must be refused, through the existing abort-reason channel");

    /* No-ceiling case: threshold is the bare AUTOTUNE_MIN_RISE_NO_CEILING_C
     * fallback (40.0C). K=1.0, duty=1.0 -> rise=1.0C: comfortably above
     * pid_autotune_fit_fopdt()'s OWN 0.5C noise floor (the two-point fit
     * itself succeeds), but far below the fallback. */
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;
    s_at.guard_cfg.max_temp_c = 0.0f;
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/1.0f, /*tau_s=*/200.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    finalize_fit();
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED, "a below-floor rise must abort the run, not accept the fit");
    TEST_CHECK(strstr(s_at.abort_reason, "rise") != NULL,
              "the refusal is surfaced through the existing abort_reason channel with a specific reason "
              "mentioning the rise");

    /* Span-scaled case -- the whole point of item (6): a 22C rise (one of
     * the TWO REAL incidents this pass exists to fix, k_dc=21.74 -- see
     * this file's autotune_engine.c AUTOTUNE_MIN_RISE_FRACTION_OF_SPAN
     * comment) must be refused when the configured max_temp_c gives it a
     * large headroom (1200C nameplate, 25C baseline -> headroom 1175C,
     * 15% of that is 176.25C -- 22C is nowhere close). The OLD bare 3.0C
     * constant would have accepted this rise outright; this is exactly the
     * regression the reviewer's audit caught. */
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;
    s_at.guard_cfg.max_temp_c = 1200.0f;
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/22.0f, /*tau_s=*/200.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    finalize_fit();
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED,
              "a 22C rise (one of the two real incident gains) must be refused against a 1200C-ceiling "
              "zone's scaled threshold -- the old bare 3.0C floor would have accepted it");
    TEST_CHECK(strstr(s_at.abort_reason, "rise") != NULL, "refusal reason mentions the rise, same channel");

    /* Positive control: a rise comfortably above the SAME zone's scaled
     * (B) threshold (176.25C) AND large enough that ambient(25C) + K also
     * clears the (C) plausibility check against the same 1200C ceiling
     * (K=1200 -> implied max 1225C) -- proves (B)'s threshold is a real,
     * fraction-scaled minimum, not a disguised blanket refusal, without
     * accidentally exercising (C)'s unrelated check instead. */
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;
    s_at.guard_cfg.max_temp_c = 1200.0f;
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/1200.0f, /*tau_s=*/200.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    finalize_fit();
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE,
              "positive control: a 1200C rise against the same 1200C-ceiling zone clears both the "
              "scaled (B) threshold and the (C) plausibility check, and must still fit");
}

static void test_physical_plausibility_refuses_gain_implying_ceiling_below_max_temp(void)
{
    TEST_SECTION("(C) physical plausibility -- a fitted gain implying less than max_temp_c at full "
                 "duty must be refused, naming both numbers; max_temp_c==0 (guard DISABLED, not a "
                 "0-degree ceiling) must NOT trigger the check");
    /* Case 1: implausible -- K~=100, baseline=25=ambient (this helper
     * defaults ambient to baseline -- see write_synthetic_fopdt_trace_for_
     * zone()'s own comment; a dedicated hot-start test below covers the
     * ambient-vs-baseline distinction itself) -> implied max ~=125C, but the
     * zone's configured ceiling is 500C. K raised from the original 5.0 to
     * 100.0 so this trace also clears (B)'s now-scaled minimum-excursion
     * floor (0.15 * (500-25) = 71.25C) and reaches the (C) check under test
     * at all. The two-point fit's recovered K is not bit-exact to the
     * synthetic K fed in (discrete 10s sampling + linear crossing
     * interpolation), so the expected implied-max string is built from the
     * SAME trace's actual fitted model (obtained with the check disarmed via
     * max_temp_c=0 below) rather than hardcoded, so this test cannot flake
     * on fit-precision noise. */
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;
    s_at.guard_cfg.max_temp_c = 0.0f;
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/100.0f, /*tau_s=*/200.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    finalize_fit();
    TEST_CHECK(s_at.model.valid, "sanity: the fit itself must succeed before the plausibility case matters");
    float implied_max_c = 25.0f + s_at.model.k_gain_c_per_duty;
    char expect_implied[32], expect_limit[32];
    snprintf(expect_implied, sizeof(expect_implied), "%.1fC", (double)implied_max_c);
    snprintf(expect_limit, sizeof(expect_limit), "%.1fC", 500.0);

    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;
    s_at.guard_cfg.max_temp_c = 500.0f;
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/100.0f, /*tau_s=*/200.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    finalize_fit();
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED, "an implausible gain must refuse, not accept the fit");
    TEST_CHECK(strstr(s_at.abort_reason, expect_implied) != NULL, "abort reason names the implied ceiling");
    TEST_CHECK(strstr(s_at.abort_reason, expect_limit) != NULL, "abort reason names the configured max_temp_c (500.0C)");

    /* Case 2: the ZERO-SEMANTICS TRAP -- max_temp_c == 0 means the guard is
     * DISABLED, not "a ceiling of zero degrees". The IDENTICAL implausible
     * gain must NOT be refused by this check when no ceiling is configured. */
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;
    s_at.guard_cfg.max_temp_c = 0.0f;
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/100.0f, /*tau_s=*/200.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    finalize_fit();
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE,
              "max_temp_c == 0 (guard disabled) must NOT trigger the plausibility refusal");
    TEST_CHECK(s_at.model.valid, "the fit itself is unaffected by the check being skipped");
}

/* (8) 2026-09-01 review fix: implied_max_c must be referenced to AMBIENT,
 * not to baseline_c -- a hot-start re-tune (the zone was already partway up
 * a firing when the step test began) makes baseline_c >> true ambient, and
 * a BASELINE-referenced check inflates implied_max_c by exactly that gap,
 * letting a bad (too-low) fit slip through plausible-looking. This trace is
 * constructed so the two references disagree about the verdict: baseline
 * (200C, a hot start) + K (100C) = 300C, ABOVE the 250C configured ceiling
 * (would PASS, wrongly); ambient (25C, the true room temperature captured
 * at SETTLING->STEPPING) + K (100C) = 125C, BELOW the ceiling (correctly
 * ABORTS). */
static void test_physical_plausibility_uses_ambient_not_baseline_on_a_hot_start(void)
{
    TEST_SECTION("(C) physical plausibility is referenced to ambient, not baseline_c -- a hot-start "
                 "re-tune must not let a bad fit through just because the zone was already warm");
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;
    s_at.guard_cfg.max_temp_c = 250.0f;
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/200.0f, /*k_gain_c_per_duty=*/100.0f, /*tau_s=*/200.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    /* Override the helper's default (ambient == baseline) with a genuine
     * cold ambient reading -- exactly what the real SETTLING->STEPPING
     * transition captures via cj_c when a run starts mid-firing. */
    s_at.step_ambient_c = 25.0f;
    finalize_fit();

    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED,
              "ambient (25C) + K (100C) = 125C is below the 250C ceiling -- must refuse. A "
              "baseline-referenced check (200C + 100C = 300C) would have wrongly accepted this fit");
    /* "zone limit" (not "max_temp_c" literally -- see the real message in
     * finalize_fit()'s (C) block) is unique to THIS check's abort message,
     * distinguishing it from (B)'s "minimum needed to trust" refusal. */
    TEST_CHECK(strstr(s_at.abort_reason, "zone limit") != NULL, "refused through the same (C) channel");
}

static void test_model_settled_flag_reflects_step_settled(void)
{
    TEST_SECTION("finalize_fit() sets fopdt_model_t.settled from s_at.step_settled -- a fit that "
                 "reached finalize_fit() via the max-duration backstop (step_settled never set by the "
                 "detector) must NOT be silently reported as steady state");
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;
    s_at.step_settled = false; /* simulates the AUTOTUNE_ENGINE_DEFAULT_MAX_DURATION_S backstop path */
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/50.0f, /*tau_s=*/200.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    finalize_fit();
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "the fit itself still succeeds -- settled is a flag, not a gate");
    TEST_CHECK(!s_at.model.settled, "settled must read false -- this run never satisfied the detector");

    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;
    s_at.step_settled = true; /* simulates the honest detector having genuinely fired */
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/50.0f, /*tau_s=*/200.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    finalize_fit();
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "the fit succeeds here too");
    TEST_CHECK(s_at.model.settled, "settled must read true -- carried through from s_at.step_settled");
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
    test_accept_succeeds_end_to_end_regardless_of_last_sample_quantization_dither();
    test_finalize_fit_uses_the_requested_rule();
    test_finalize_fit_persists_valid_cross_gain_cells();
    test_finalize_fit_routes_persist_through_flash_worker();
    test_finalize_fit_skips_coupling_persist_when_unsettled();
    test_finalize_fit_skips_coupling_persist_when_extrapolation_did_not_converge();
    test_finalize_fit_skips_a_valid_fit_with_out_of_range_gain();
    test_finalize_fit_persists_nothing_on_an_invalid_direct_fit();
    test_settle_detector_rejects_a_slow_constant_ramp_the_old_one_accepted();
    test_settle_detector_fires_on_a_clean_exponential_no_earlier_than_3tau();
    test_settle_detector_never_fires_on_a_perfectly_flat_trace();
    test_settle_detector_floor_arm_reachable_at_one_quantum();
    test_settle_detector_refuses_a_frozen_full_trace();
    test_settle_detector_fires_with_dead_time_over_300s();
    test_settle_detector_ignores_two_quantum_dead_time_noise();
    test_autotune_engine_accept_gates_on_settled();
    test_autotune_engine_accept_gates_on_extrapolation_converged();
    test_autotune_engine_accept_gates_on_tau_consistent();
    test_autotune_engine_accept_does_not_block_a_fully_clean_fit();
    test_min_excursion_refuses_a_fit_below_the_rise_floor();
    test_physical_plausibility_refuses_gain_implying_ceiling_below_max_temp();
    test_physical_plausibility_uses_ambient_not_baseline_on_a_hot_start();
    test_model_settled_flag_reflects_step_settled();
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
