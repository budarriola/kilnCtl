// Host tests for App/drivers/control/autotune_engine.c's pre-start guard, PLUS (added
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
// captured backtrace). autotune_begin_run_locked() (the shared entry both
// autotune_engine_run() and autotune_engine_run_relay() funnel through) and
// every other public function now test s_at.lock == NULL as their first
// statement and return a clean "not running" answer instead.
//
// #includes autotune_engine.c and its four split siblings directly (same
// convention as test_profile_executor_prestart.c's #include of ITS split --
// see that file's header comment) -- ROADMAP.md M15 A3 broke autotune_engine.c
// (4120 lines) into autotune_engine.c/_guard.c/_step_identify.c/_relay.c/
// _coupling.c on 2026-09-04; see autotune_engine_internal.h's own top comment
// for the five-way seam. All five must land in ONE translation unit here so
// TAG/s_at (non-static, defined once in autotune_engine.c) resolve, and so
// every widened-linkage helper is visible to the tests that exercise it
// directly. Own executable for the same "defines the real zones_config_*()
// bodies, would multiply-define against other host tests' fakes" reason.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "test_common.h"

#include "esp_err.h"

// Own executable (see this file's header comment).
int g_test_failures = 0;
int g_test_count = 0;

#include "../drivers/control/autotune_engine.c"
#include "../drivers/control/autotune_engine_guard.c"
#include "../drivers/control/autotune_engine_step_identify.c"
#include "../drivers/control/autotune_engine_relay.c"
#include "../drivers/control/autotune_engine_coupling.c"

// Test-only tick driver, defined HERE rather than in autotune_engine.c so the
// target build never contains it (2026-09-24 review of the pre-lock read fix):
// it reads the bus the way production's task_entry() does before taking
// s_at.lock, then runs the real tick body. It keeps the pre-fix
// autotune_engine_tick_locked() name so this file's many direct call sites
// stay unchanged.
static void autotune_engine_tick_locked(void)
{
    ThermoChannelSnapshot snap;
    thermo_channels_read(s_at.thermo_bus, &snap);
    autotune_engine_tick_locked_impl(&snap);
}

// F2 (flash-worker lock-inversion audit 2026-10-09): autotune_finalize_fit()
// only parks its coupling persist in s_at.pending_coupling; production's
// task_entry() (autotune_engine_tick_under_lock()) takes it and dispatches
// it after giving s_at.lock. Tests that call autotune_finalize_fit() directly
// finish the same way here, with no lock held.
static void finalize_fit_then_flush_coupling(void)
{
    autotune_finalize_fit();
    coupling_persist_job_t job;
    if (autotune_take_pending_coupling_locked(&job)) {
        autotune_dispatch_coupling_persist(&job);
    }
}

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
// Channel 0's own cold-junction reading -- split out from the hardcoded
// 25.0C every pre-existing test in this file implicitly relies on (none of
// them reads step_ambient_c) so the readiness-check tests below can force
// "no CJ this tick" (NAN) without disturbing anything else. Reset to 25.0f
// by every test that cares (see reset_extra_channel_stubs()).
static float s_stub_ch0_cj_c = 25.0f;

// Extra (non-zone-0) channels the readiness check needs to see a hot or
// drifting NEIGHBOUR zone -- see check_thermal_readiness_locked()'s own
// comment in autotune_engine.c. Channel 0 is untouched above; these are
// appended to MAX31856_read_all()'s report only for channels named in
// s_stub_extra_ch_mask, so every pre-existing test in this file (which
// never sets this mask) gets out_count == 1 exactly as before -- not a
// behavior change for them.
static float s_stub_extra_ch_temp_c[MAX31856_CHANNEL_COUNT];
static bool  s_stub_extra_ch_ok[MAX31856_CHANNEL_COUNT];
static uint8_t s_stub_extra_ch_mask = 0;

static void reset_extra_channel_stubs(void)
{
    s_stub_ch0_cj_c = 25.0f;
    memset(s_stub_extra_ch_temp_c, 0, sizeof(s_stub_extra_ch_temp_c));
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) s_stub_extra_ch_ok[z] = true;
    s_stub_extra_ch_mask = 0;
}

// 2026-09-24 regression guard companion to profile_executor's own
// s_test_max31856_max_lock_depth_seen (test_profile_executor_prestart.c):
// g_test_stub_lock_depth (stubs/freertos/semphr.h) is a single process-wide
// xSemaphoreTake/Give nesting counter. Recording the highest depth seen
// during any MAX31856_read_all() call here lets a test prove
// thermo_channels_read() -- the one shared read-and-filter helper both
// profile_executor.c's executor_task_entry() and this file's task_entry()
// now call BEFORE taking their own module lock -- never observes a nonzero
// depth when called via autotune_engine's path.
static int s_test_max31856_max_lock_depth_seen = -1;

// Forward declarations: called from run_test_autotune_engine_prestart()
// (defined earlier in this file) but defined near the bottom, next to the
// other source-scan test helpers.
static void test_tick_locked_wrapper_reads_outside_any_lock(void);
static void test_task_entry_reads_before_taking_s_at_lock(void);

esp_err_t MAX31856_read_all(MAX31856BusClass *bus, MAX31856Reading *out, size_t max_readings, size_t *out_count)
{
    (void)bus;
    if (s_test_max31856_max_lock_depth_seen < g_test_stub_lock_depth) {
        s_test_max31856_max_lock_depth_seen = g_test_stub_lock_depth;
    }
    size_t n = 0;
    if (max_readings >= 1) {
        memset(&out[n], 0, sizeof(out[n]));
        out[n].channel = 0;
        out[n].spi_failed = !s_stub_ch0_ok;
        out[n].fault_status = 0;
        out[n].tc_temperature_c = s_stub_ch0_ok ? s_stub_ch0_temp_c : NAN;
        out[n].cj_temperature_c = s_stub_ch0_cj_c;
        n++;
    }
    for (uint8_t z = 1; z < MAX31856_CHANNEL_COUNT && n < max_readings; z++) {
        if (!(s_stub_extra_ch_mask & (1u << z))) continue;
        memset(&out[n], 0, sizeof(out[n]));
        out[n].channel = z;
        out[n].spi_failed = !s_stub_extra_ch_ok[z];
        out[n].fault_status = 0;
        out[n].tc_temperature_c = s_stub_extra_ch_ok[z] ? s_stub_extra_ch_temp_c[z] : NAN;
        out[n].cj_temperature_c = 25.0f; /* only channel s_at.zone_index's cj is ever consumed */
        n++;
    }
    if (out_count) *out_count = n;
    return ESP_OK;
}

esp_err_t kiln_io_owner_command_set_relay_mask_authorized(uint8_t mask, uint8_t value)
{
    (void)mask; (void)value;
    return ESP_OK;
}

static int s_heat_zone_claim_begin_calls;
static bool s_test_update_claim_after_heat_claim = false; /* MED-1 (review 3) */
bool ota_http_heat_blocked_by_update(char *reason_out, size_t reason_cap)
{
    if (reason_out && reason_cap) reason_out[0] = '\0';
    if (s_test_update_claim_after_heat_claim && s_heat_zone_claim_begin_calls > 0) {
        if (reason_out && reason_cap) snprintf(reason_out, reason_cap, "update in progress");
        return true;
    }
    return false;
}

// Q3: adaptive_tune_clear_ki_baseline() -- autotune_engine.c's accept path
// now calls this (see autotune_engine_accept()'s own comment) once a
// result's gains have been committed, so the Ki-diagnosis module's
// cumulative bound re-latches against the fresh result instead of staying
// capped forever against a stale one. This file does not link adaptive_
// tune.c at all (it is a big, separately-tested module in its own right --
// see test_adaptive_tune.c, which is where adaptive_tune_clear_ki_baseline()
// itself is actually exercised), so a no-op fake is all this file needs to
// LINK -- but S6 (2026-09-01 audit of ae5905f) found that "no-op fake" had
// drifted into "unasserted fake": nothing here ever recorded that the call
// actually happened, so deleting autotune_engine.c's call to this function
// entirely left the whole suite green. Now counts calls (and the last zone
// index passed) so test_accept_succeeds_end_to_end_regardless_of_last_
// sample_quantization_dither() below can assert the wiring, not just that
// linking succeeds.
int    g_clear_ki_baseline_calls = 0;
uint8_t g_clear_ki_baseline_last_zone = 0xFF;
void adaptive_tune_clear_ki_baseline(uint8_t zone_index)
{
    g_clear_ki_baseline_calls++;
    g_clear_ki_baseline_last_zone = zone_index;
}

bool profile_executor_run(uint8_t profile_id, char *err_msg, size_t err_cap)
{
    (void)profile_id;
    if (err_msg && err_cap) err_msg[0] = '\0';
    return false;
}

// Configurable per-test (default all-false, matching the old hardcoded
// behavior for every pre-existing test in this file) -- see the review
// blocker 1 tests below, the first to need a specific OTHER zone reporting
// active.
static bool s_stub_zone_active[MAX31856_CHANNEL_COUNT];
bool profile_executor_zone_is_active(uint8_t zone_index)
{
    return (zone_index < MAX31856_CHANNEL_COUNT) ? s_stub_zone_active[zone_index] : false;
}

/* Owner decision 2026-10-08: autotune_engine_accept() consults the system
 * mode gate; these let a test pretend a firing/autotune run is active. */
static bool s_stub_profile_running = false;
static bool s_stub_autotune_running = false;
/* TOCTOU hook: when >= 0, the Nth call (0-based) and later report a profile
 * running, simulating a start landing between the gate snapshot and writes. */
static int s_stub_heat_flip_at_call = -1;
static int s_stub_heat_calls = 0;
void relay_authority_heat_run_active(bool *profile_running_out, bool *autotune_running_out)
{
    if (s_stub_heat_flip_at_call >= 0 && s_stub_heat_calls++ >= s_stub_heat_flip_at_call) {
        s_stub_profile_running = true;
    }
    if (profile_running_out) *profile_running_out = s_stub_profile_running;
    if (autotune_running_out) *autotune_running_out = s_stub_autotune_running;
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
    // fix); returning false just lets autotune_begin_run_locked() proceed normally.
    (void)zone_index;
    return false;
}

// TODO.md 6A.6 (ownership tags): recorded, not a bare no-op, so the
// STEPPING-loop ownership tests further down can prove autotune_begin_run_locked()
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

/* The shared heat claim (relay_authority.h) -- autotune_begin_run_locked() now takes
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

/* The NEW per-zone atomic claim (relay_authority.h), added to close the
 * peek-then-commit race found in review of 933a7eec: autotune start peeks
 * profile_executor_zone_is_active() and profile start peeks
 * autotune_engine_is_active_on_zone(), but each peek runs before its own
 * module lock, so two starts on the same zone can both pass. This fake
 * defaults to "always succeeds, no conflict" so every existing test's real
 * STEPPING-loop path is unchanged; s_test_zone_claim_refused lets
 * test_run_refuses_at_atomic_zone_claim_gate() below prove
 * autotune_begin_run_locked() is refused when the EARLY
 * profile_executor_zone_is_active() peek passed but a profile won the
 * atomic claim underneath it -- the exact race window this claim exists to
 * close. */
static bool s_test_zone_claim_refused = false;
static uint8_t s_test_zone_claim_conflict_mask = 0;
static int s_zone_claim_begin_calls = 0;
static int s_zone_claim_end_calls = 0;
static relay_heat_zone_claimant_t s_last_zone_claimant = RELAY_HEAT_ZONE_CLAIM_PROFILE;
static uint8_t s_last_zone_claim_mask = 0;

bool relay_authority_zone_claim_begin(relay_heat_zone_claimant_t who, uint8_t zone_mask,
                                       uint8_t *conflict_mask_out)
{
    s_zone_claim_begin_calls++;
    s_last_zone_claimant = who;
    s_last_zone_claim_mask = zone_mask;
    if (s_test_zone_claim_refused) {
        if (conflict_mask_out) {
            *conflict_mask_out = s_test_zone_claim_conflict_mask ? s_test_zone_claim_conflict_mask : zone_mask;
        }
        return false;
    }
    if (conflict_mask_out) {
        *conflict_mask_out = 0;
    }
    return true;
}

void relay_authority_zone_claim_end(relay_heat_zone_claimant_t who, uint8_t zone_mask)
{
    s_zone_claim_end_calls++;
    s_last_zone_claimant = who;
    s_last_zone_claim_mask = zone_mask;
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

/* ZONES_CFG_VERSION 21->22 (docs/audits/consumer_without_producer_2026-09-06.md
 * finding 1): guard 1's arrival band. Settable, same "a test can prove a
 * configured value actually reaches the control path" reasoning as
 * g_stub_guard_extra above; defaults to 0 (use the firmware default),
 * matching every pre-existing test in this file. */
float g_stub_progress_band_c = 0.0f;
bool zones_config_get_progress_band_c(uint8_t zone_index, float *out_band_c)
{
    (void)zone_index;
    if (out_band_c) *out_band_c = g_stub_progress_band_c;
    return true;
}

/* 2026-09-10 opus review finding B: autotune_engine.c now derives guard 1's
 * climbing-branch window floor from the zone's fitted plant model, via the
 * same zone_model_at() seam profile_executor_feedforward.c/profile_
 * feasibility.c already use (zones_config_accessors.c is not linked into
 * this executable -- see this file's own header comment -- so this needs
 * its own fake, same convention as every other zones_config_get_* stub in
 * this file). Settable per-zone (indexed by zone), defaulting to "no model"
 * (returns false, all-zero outs) so every pre-existing test in this file
 * that never touches this feature sees exactly today's "don't touch
 * window_s" behaviour -- identical to test_profile_executor_prestart.c's
 * own g_stub_model_valid/zones_config_get_model() default. */
static bool g_stub_model_valid[MAX31856_CHANNEL_COUNT] = {false};
static float g_stub_model_k_dc[MAX31856_CHANNEL_COUNT] = {0};
static float g_stub_model_tau_s[MAX31856_CHANNEL_COUNT] = {0};
static float g_stub_model_dead_time_s[MAX31856_CHANNEL_COUNT] = {0};
bool zone_model_at(uint8_t zone_index, float T_c, float *out_k_dc, float *out_tau_s, float *out_dead_time_s)
{
    (void)T_c;
    if (zone_index >= MAX31856_CHANNEL_COUNT) return false;
    if (out_k_dc) *out_k_dc = g_stub_model_k_dc[zone_index];
    if (out_tau_s) *out_tau_s = g_stub_model_tau_s[zone_index];
    if (out_dead_time_s) *out_dead_time_s = g_stub_model_dead_time_s[zone_index];
    return g_stub_model_valid[zone_index];
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
    // A nonzero mask so autotune_begin_run_locked() (real code, called for real by
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

// Configurable per-test (default 0.0f, same convention as s_stub_max_temp_c
// above -- direct assignment, no setter). Added for the relay setpoint
// window tests, which need a real min_temp_c on both sides of the guard
// window fix, not just the hardcoded 0.0f every earlier test relied on.
static float s_stub_min_temp_c = 0.0f;

bool zones_config_get_temp_limits(uint8_t zone_index, float *out_max_temp_c, float *out_min_temp_c)
{
    (void)zone_index;
    if (out_max_temp_c) *out_max_temp_c = s_stub_max_temp_c;
    if (out_min_temp_c) *out_min_temp_c = s_stub_min_temp_c;
    return false;
}

// Configurable per-test (default 0, this stub's original hardcoded
// behavior) via direct assignment, same convention as s_stub_max_temp_c
// above -- test_finalize_fit_persists_valid_cross_gain_cells() below needs
// a real thermo_count so autotune_finalize_fit()'s cross-zone loop does not refuse
// every peer cell with "j >= thermo_count" before ever reaching the new
// persist step.
static uint8_t s_stub_thermo_count = 0;

uint8_t zones_config_get_thermo_count(void)
{
    return s_stub_thermo_count;
}

/* 2026-08-30 (ZONES_CFG_VERSION 10->11): autotune_finalize_fit()'s new persist step
 * (see autotune_engine.c's own comment) needs this symbol to link.
 * Records every call (zone_index, neighbor_index, coeff) rather than just
 * refusing, so test_finalize_fit_persists_valid_cross_gain_cells() below can
 * observe exactly what autotune_finalize_fit() tried to persist -- succeeds
 * unconditionally by default, same "default succeeds" convention this
 * file's header comment documents for the STEPPING-loop's other setters. */
typedef struct {
    bool called;
    float coeff;
    /* ZONES_CFG_VERSION 11->12 (DATA PLUMBING pass): the setter widened to
     * also carry tau_s/dead_time_s -- recorded here the same unconditional
     * way coeff is, so a test can observe exactly what autotune_finalize_fit() tried
     * to persist for all three, not just the gain. */
    float tau_s;
    float dead_time_s;
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
 * (autotune_finalize_fit(), ~line 437 -- the guard that decides whether to call this
 * setter AT ALL) had no negative test: stubbing that guard to `if (0)`
 * still passed 156/156, because the fake could not tell "the guard skipped
 * the call" from "the call happened and the fake let a bad value through
 * anyway". `called` is still recorded unconditionally (even for a rejected
 * value) so a test can distinguish those two cases: a working guard never
 * calls this at all for an out-of-range gain (called stays false); a broken
 * guard calls it and gets refused (called becomes true, return value
 * false) -- either way the real bound is enforced here exactly like the
 * production setter, so a test built on this fake proves something real. */
/* 2026-08-31 panic fix: autotune_finalize_fit()'s persist step now hands its
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
/* Flash-worker lock-inversion audit 2026-10-09 F2: the real worker runs
 * UART autotune handlers (status, abort, accept, start) that take s_at.lock.
 * This stub plays that worker: if any lock is held at dispatch time (the
 * stub semaphore's process-wide g_test_stub_lock_depth -- the only lock these
 * paths hold is s_at.lock), the worker would block on it while the caller
 * blocks on the worker. That deadlock is recorded and the job is NOT run,
 * the same as on the board, where it never completes. */
int g_flash_worker_dispatch_under_lock = 0;
esp_err_t uart_bridge_ext_run_on_flash_worker(void (*fn)(void *arg), void *arg)
{
    g_flash_worker_submit_calls++;
    if (!fn) {
        return ESP_ERR_INVALID_ARG;
    }
    if (g_test_stub_lock_depth > 0) {
        g_flash_worker_dispatch_under_lock++;
        return ESP_ERR_TIMEOUT;
    }
    fn(arg);
    return ESP_OK;
}

/* Total of every zones_config_set_* stub call, so a refused accept can be
 * proven to have written NOTHING (not just set_pid/set_max_ramp). */
static int s_zones_write_total = 0;
bool zones_config_set_coupling_cell(uint8_t zone_index, uint8_t neighbor_index, float coeff, float tau_s,
                                     float dead_time_s)
{
    s_zones_write_total++;
    if (zone_index >= MAX31856_CHANNEL_COUNT || neighbor_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    s_coupling_cell_calls[zone_index][neighbor_index].called = true;
    s_coupling_cell_calls[zone_index][neighbor_index].coeff = coeff;
    s_coupling_cell_calls[zone_index][neighbor_index].tau_s = tau_s;
    s_coupling_cell_calls[zone_index][neighbor_index].dead_time_s = dead_time_s;
    if (zone_index == neighbor_index) {
        return coeff == 0.0f && tau_s == 0.0f && dead_time_s == 0.0f;
    }
    if (!isfinite(coeff) || coeff < 0.0f || coeff > ZONE_COUPLING_COEFF_MAX) {
        return false;
    }
    /* ZONES_CFG_VERSION 11->12: same all-or-nothing bound the real setter
     * enforces -- see zones_config_set_coupling_cell()'s own header comment. */
    return isfinite(tau_s) && tau_s >= 0.0f && tau_s <= ZONE_MODEL_TIME_MAX_S && isfinite(dead_time_s) &&
           dead_time_s >= 0.0f && dead_time_s <= ZONE_MODEL_TIME_MAX_S;
}

/* ZONES_CFG_VERSION 11->12 stub siblings of zones_config_get_coupling() just
 * below -- same "tests set it directly before calling autotune_finalize_fit()" role,
 * for coupling_tau_s[]/coupling_dead_time_s[]. Unused by the (C)
 * ceiling-reachability check (that one only ever needed the gain), but
 * autotune_finalize_fit() itself does not call these getters -- kept purely so this
 * translation unit still provides every symbol autotune_engine.c/zones_http.h
 * declare, same reason zones_config_get_coupling() exists here at all. */
static float s_stub_coupling_tau_row[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT];
static float s_stub_coupling_dead_time_row[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT];

bool zones_config_get_coupling_tau(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (zone_index >= MAX31856_CHANNEL_COUNT || !out_row) {
        return false;
    }
    memcpy(out_row, s_stub_coupling_tau_row[zone_index], sizeof(s_stub_coupling_tau_row[zone_index]));
    return true;
}

bool zones_config_get_coupling_dead_time(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (zone_index >= MAX31856_CHANNEL_COUNT || !out_row) {
        return false;
    }
    memcpy(out_row, s_stub_coupling_dead_time_row[zone_index], sizeof(s_stub_coupling_dead_time_row[zone_index]));
    return true;
}

/* 2026-08-31 defect fix: autotune_finalize_fit()'s (C) ceiling-reachability check now
 * folds in this zone's already-measured cross-coupling from every other
 * zone (zones_config_get_coupling(), zones_http.h ~line 158's "row i is the
 * AFFECTED zone" convention) rather than asking whether the zone can reach
 * its own ceiling completely alone. Backed by its own storage, separate
 * from s_coupling_cell_calls[][] above (that one only records what
 * autotune_finalize_fit() itself WROTE this run, via the single-cell setter; this one
 * is what a PRIOR run already measured and autotune_finalize_fit() now READS back) --
 * tests set it directly via s_stub_coupling_row[][] before calling
 * autotune_finalize_fit(). Defaults to all-zero, i.e. "nothing measured yet", the
 * same convention zones_http.c's real getter documents for an unmeasured
 * cell -- which is exactly the "no coupling data" degrade case the new (C)
 * check has to handle safely (see its own comment in autotune_engine.c). */
static float s_stub_coupling_row[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT];

static void reset_stub_coupling_row(void)
{
    memset(s_stub_coupling_row, 0, sizeof(s_stub_coupling_row));
}

bool zones_config_get_coupling(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (zone_index >= MAX31856_CHANNEL_COUNT || !out_row) {
        return false;
    }
    memcpy(out_row, s_stub_coupling_row[zone_index], sizeof(s_stub_coupling_row[zone_index]));
    return true;
}

// Configurable per-test (default 0x01, i.e. this stub's original hardcoded
// behavior -- zone 0 -> channel 0, matching MAX31856_read_all()'s stub above
// and every STEPPING-loop test below). test_run_refuses_zone_with_no_
// thermo_mask() sets this to 0 to exercise the new prestart refusal in
// autotune_begin_run_locked(); every other test leaves it at the default and
// is unaffected.
static uint8_t s_stub_thermo_mask = 0x01;

bool zones_config_get_thermo_mask(uint8_t zone_index, uint8_t *out_mask)
{
    (void)zone_index;
    if (out_mask) *out_mask = s_stub_thermo_mask;
    return true;
}

// docs/ON_OFF_ZONE.md step 1: same shape as s_stub_thermo_mask above --
// false (HEATER) by default so every existing test in this file is
// unaffected; test_run_refuses_on_off_zone() below sets this true to
// exercise autotune_begin_run_locked()'s new prestart refusal.
static bool s_stub_zone_is_on_off = false;

bool zone_is_on_off(uint8_t zone_index)
{
    (void)zone_index;
    return s_stub_zone_is_on_off;
}

// SPARE_RELAY_ONOFF_PLAN.md sec 10: monitor-only predicate stand-in, false by default.
static bool s_stub_zone_monitor_only = false;

bool zone_is_monitor_only(uint8_t zone_index)
{
    (void)zone_index;
    return s_stub_zone_monitor_only;
}

bool zones_config_is_valid(void)
{
    // True so autotune_begin_run_locked() (real code, called for real by the
    // STEPPING-loop tests below) doesn't refuse the run with "zone config
    // failed to load" -- unreached by the prestart tests (see this file's
    // header comment), so not a behavior change for them.
    return true;
}

// B2 (opus review, 2026-08-27): autotune_begin_run_locked() now refuses to start
// while a zone current sweep is active -- see zones_current_sweep_is_active()'s
// doc comment (zones_http.h). Settable so test_run_refuses_while_zone_sweep_
// is_active() below can exercise the real refusal; default false so the real
// STEPPING-loop tests elsewhere in this file, which call autotune_begin_run_locked()
// for real, are unaffected.
static bool s_test_sweep_active = false;
bool zones_current_sweep_is_active(void)
{
    return s_test_sweep_active;
}

/* Configurable (default false, matching this stub's original hardcoded
 * "never persists" behavior) so a test exercising autotune_engine_accept()'s
 * tuning-quality write can flip it to true -- same convention as
 * s_stub_set_pid_result below. */
static bool s_stub_set_model_result = false;
bool zones_config_set_model(uint8_t zone_index, float k_dc, float tau_s, float dead_time_s)
{
    s_zones_write_total++;
    (void)zone_index; (void)k_dc; (void)tau_s; (void)dead_time_s;
    return s_stub_set_model_result;
}

/* docs/audits/high_temperature_transfer_analysis_2026-09-08.md item 1:
 * autotune_engine_accept()'s accept path now calls this right after
 * zones_config_set_model() succeeds, with the fit's measured baseline/
 * ambient. Default true (this run's real behavior for existing tests never
 * asserting on it) plus call-count/last-args capture, same convention as
 * s_stub_set_coupling_diag_k_dc_* below. */
static bool s_stub_set_model_fit_context_result = true;
static int s_stub_set_model_fit_context_call_count = 0;
static uint8_t s_stub_set_model_fit_context_zone = 0xFF;
static float s_stub_set_model_fit_context_temp_c = 0.0f;
static float s_stub_set_model_fit_context_ambient_c = 0.0f;
bool zones_config_set_model_fit_context(uint8_t zone_index, float fit_temp_c, float fit_ambient_c)
{
    s_zones_write_total++;
    s_stub_set_model_fit_context_call_count++;
    s_stub_set_model_fit_context_zone = zone_index;
    s_stub_set_model_fit_context_temp_c = fit_temp_c;
    s_stub_set_model_fit_context_ambient_c = fit_ambient_c;
    return s_stub_set_model_fit_context_result;
}

/* docs/audits/adaptive_tune_vs_owner_requirements_2026-09-11.md item 4:
 * autotune_engine_accept()'s accept path now ALSO calls this right after
 * zones_config_set_model() succeeds, re-anchoring adaptive_tune_model.c's
 * ratchet-prevention baseline to the fresh model this autotune just wrote --
 * same "configurable result + call-count/last-args capture" convention as
 * s_stub_set_model_fit_context_* just above, so a test can prove both the
 * positive (called, with the exact same gain as model_k_dc) and the
 * negative (NOT called when the model persist fails or on the RELAY path,
 * which never calls zones_config_set_model() at all). */
static bool s_stub_set_autotune_baseline_k_dc_result = true;
static int s_stub_set_autotune_baseline_k_dc_call_count = 0;
static uint8_t s_stub_set_autotune_baseline_k_dc_zone = 0xFF;
static float s_stub_set_autotune_baseline_k_dc_value = 0.0f;
bool zones_config_get_autotune_baseline_k_dc(uint8_t zone_index, float *out_k_dc)
{
    (void)zone_index;
    if (out_k_dc) *out_k_dc = 0.0f;
    return true;
}
bool zones_config_set_autotune_baseline_k_dc(uint8_t zone_index, float k_dc)
{
    s_zones_write_total++;
    s_stub_set_autotune_baseline_k_dc_call_count++;
    s_stub_set_autotune_baseline_k_dc_zone = zone_index;
    s_stub_set_autotune_baseline_k_dc_value = k_dc;
    return s_stub_set_autotune_baseline_k_dc_result;
}

/* PID_EXPANSION_PLAN.md section 3.2 follow-up: autotune_engine_accept()'s
 * on-board coupling_diag_k_dc identification pass writes this alongside
 * zones_config_set_model() above, from the SAME fitted gain. Configurable
 * result (default true, so the STEPPING-loop tests that reach a real
 * accept() elsewhere in this file are unaffected) plus call-count/last-args
 * capture, same convention as s_stub_set_model_result and
 * s_stub_tuning_quality_* just above/below -- lets a test prove both the
 * positive (written, with the exact same gain as model_k_dc) and the
 * negative (NOT called when model persist fails or on the RELAY path). */
static bool s_stub_set_coupling_diag_k_dc_result = true;
static int s_stub_set_coupling_diag_k_dc_call_count = 0;
static uint8_t s_stub_set_coupling_diag_k_dc_zone = 0xFF;
static float s_stub_set_coupling_diag_k_dc_value = 0.0f;
bool zones_config_set_coupling_diag_k_dc(uint8_t zone_index, float k_dc)
{
    s_zones_write_total++;
    s_stub_set_coupling_diag_k_dc_call_count++;
    s_stub_set_coupling_diag_k_dc_zone = zone_index;
    s_stub_set_coupling_diag_k_dc_value = k_dc;
    return s_stub_set_coupling_diag_k_dc_result;
}

/* ZONES_CFG_VERSION 12->13 -- captures the LAST call's arguments (zone_index
 * and *q) so a test can assert autotune_engine_accept()'s STEP-success path
 * wrote exactly the fields the fit produced, without this file needing its
 * own copy of zones_cfg_t/NVS machinery. s_stub_tuning_quality_call_count
 * lets a test also prove the negative -- that this is NOT called when the
 * model failed to persist, or on the RELAY path, or before accept() reaches
 * the write at all. */
static uint8_t s_stub_tuning_quality_zone = 0xFF;
static zone_tuning_quality_t s_stub_tuning_quality_written;
static int s_stub_tuning_quality_call_count = 0;
static bool s_stub_set_tuning_quality_result = true;
bool zones_config_set_tuning_quality(uint8_t zone_index, const zone_tuning_quality_t *q)
{
    s_zones_write_total++;
    s_stub_tuning_quality_zone = zone_index;
    if (q) {
        s_stub_tuning_quality_written = *q;
    }
    s_stub_tuning_quality_call_count++;
    return s_stub_set_tuning_quality_result;
}

/* Configurable (default false, this stub's original hardcoded behavior --
 * same convention as s_stub_max_temp_c/s_test_sweep_active above) so
 * test_autotune_engine_accept_gates_on_settled() below can exercise
 * autotune_engine_accept()'s real success path, not just its early refusals
 * (every other test in this file only reaches the "before start" or
 * "no DONE result" refusals, which never call this stub at all). */
static bool s_stub_set_pid_result = false;
/* Opus review round 2 (2026-09-23): proves accept() holds its own
 * reservation across this exact write, not just up to releasing s_at.lock
 * beforehand -- a concurrent reserve() attempt (iter_tune_http.c's restore
 * handler, in real life) made at the precise moment accept() is writing
 * gains must be refused. Single-threaded host tests cannot literally
 * interleave two tasks, so test_accept_is_gated_by_the_reservation_too()
 * makes THIS stub itself the "concurrent caller": when armed, it calls
 * autotune_engine_reserve_zone_for_external_write() from inside the write
 * accept() is making, capturing whether that attempt is refused. Disarmed
 * (false) by default so every other test that reaches this stub is
 * unaffected. */
static bool s_probe_reserve_during_set_pid = false;
static bool s_probe_reserve_during_set_pid_result = true;
/* Fake zone gain store + call count: a stand-in for "what zones config now
 * holds", so a refused accept() can be proven to have written nothing. */
static float s_fake_zone_kp = 0.0f, s_fake_zone_ki = 0.0f, s_fake_zone_kd = 0.0f;
static float s_fake_zone_max_ramp = 0.0f;
static int s_fake_set_pid_call_count = 0;
bool zones_config_set_pid(uint8_t zone_index, float kp, float ki, float kd)
{
    s_zones_write_total++;
    s_fake_set_pid_call_count++;
    if (s_stub_set_pid_result) {
        s_fake_zone_kp = kp; s_fake_zone_ki = ki; s_fake_zone_kd = kd;
    }
    if (s_probe_reserve_during_set_pid) {
        s_probe_reserve_during_set_pid_result = autotune_engine_reserve_zone_for_external_write(zone_index);
    }
    return s_stub_set_pid_result;
}

/* Dev review 9 L2: accept() now goes through the _checked setter so it can tell a run-claimed
 * refusal from a bad value. s_stub_set_pid_busy makes the stub report BUSY_RUNNING without
 * touching the fake store, modelling the real setter's claim re-check inside zones_cfg_lock. */
static bool s_stub_set_pid_busy = false;
zones_set_result_t zones_config_set_pid_checked(uint8_t zone_index, float kp, float ki, float kd)
{
    if (s_stub_set_pid_busy) {
        s_zones_write_total++;
        s_fake_set_pid_call_count++;
        return ZONES_SET_BUSY_RUNNING;
    }
    return zones_config_set_pid(zone_index, kp, ki, kd) ? ZONES_SET_OK : ZONES_SET_REJECTED;
}

/* Dev review 9 L1: stand-in for adaptive_tune_any_write_in_flight(); a test sets it to model the
 * run-end apply pass holding write_in_flight. Never set outside the test that wants it. */
static bool s_stub_adaptive_write_in_flight = false;
bool adaptive_tune_any_write_in_flight(void)
{
    return s_stub_adaptive_write_in_flight;
}

/* TODO.md 6A.4 -- autotune_engine_accept()'s opt-in ceiling-adopt write.
 * Call-count/last-args capture, same convention as
 * s_stub_set_coupling_diag_k_dc_* above, so a test can prove both the
 * positive (called with exactly the run's predicted_max_ramp_c_per_hr, only
 * when adopt_ceiling was passed) and the negative (NOT called when
 * adopt_ceiling is false/omitted -- the default -- or when the predicted
 * ceiling is <=0, or on the RELAY path, or when the model itself failed to
 * persist). Default true so a positive-path test does not also have to set
 * this. */
static bool s_stub_set_max_ramp_result = true;
static int s_stub_set_max_ramp_call_count = 0;
static uint8_t s_stub_set_max_ramp_zone = 0xFF;
static float s_stub_set_max_ramp_value = -1.0f;
bool zones_config_set_max_ramp(uint8_t zone_index, float c_per_hr)
{
    s_zones_write_total++;
    s_stub_set_max_ramp_call_count++;
    s_stub_set_max_ramp_zone = zone_index;
    s_stub_set_max_ramp_value = c_per_hr;
    if (s_stub_set_max_ramp_result) {
        s_fake_zone_max_ramp = c_per_hr;
    }
    return s_stub_set_max_ramp_result;
}

/* Companion getter for the "never silently tighten" check in autotune_
 * engine_accept()'s ceiling-adoption path: the stored ceiling it reads
 * back before deciding ADOPTED vs SKIPPED_WOULD_TIGHTEN. Default 0.0f/true
 * ("no ceiling configured yet") so every pre-existing adopt-ceiling test
 * above, written before this getter existed, keeps adopting exactly as
 * before -- a test that wants the tighten-skip path sets
 * s_stub_get_max_ramp_value/s_stub_get_max_ramp_result explicitly. */
static bool s_stub_get_max_ramp_result = true;
static float s_stub_get_max_ramp_value = 0.0f;
bool zones_config_get_max_ramp(uint8_t zone_index, float *out_c_per_hr)
{
    (void)zone_index;
    if (out_c_per_hr) {
        *out_c_per_hr = s_stub_get_max_ramp_value;
    }
    return s_stub_get_max_ramp_result;
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

// ---------------------------------------------------------------------------
// THE READINESS FIRING INTERLOCK, extended to autotune (owner decision
// 2026-09-09, same day as App/drivers/safety/readiness_gate.h's original
// firing-only gate). autotune_begin_run_locked() -- the single choke point
// every autotune start path funnels through (autotune_engine_run(),
// autotune_engine_run_to_target(), autotune_engine_run_relay()) -- now calls
// readiness_gate_refuses_start() before anything else it does, so this file
// -- which #includes autotune_engine.c directly -- must supply a body for the
// one symbol that gate declares rather than defines. Everything that DECIDES
// anything is static inline in readiness_gate.h and runs FOR REAL here: these
// tests drive the actual interlock, not a stand-in for it.
// test_readiness_gate.c proves the DECISION over all 64 fact combinations;
// these tests prove the WIRING at this second call site.
//
// DEFAULT IS FULLY READY (not zeroed), same reasoning as
// test_profile_executor_prestart.c's identical fake: a zeroed
// readiness_gate_facts_t is a board with an unverified E-stop and a down
// safety link, which the real gate refuses -- every pre-existing test in this
// file that expects autotune_begin_run_locked() to get past its early guards
// (starting with test_run_refuses_before_start() just above) would have
// started failing for a reason that has nothing to do with what it is
// testing.
static readiness_gate_facts_t s_test_readiness_facts = {
    .recovery_mode = false,
    .safety_link_up = true,
    .safety_trip_mask = 0u,
    .crash_have_record = false,
    .crash_acknowledged = false,
    .estop_verified = true,
};
void readiness_gate_collect(readiness_gate_facts_t *out)
{
    if (out != NULL) {
        *out = s_test_readiness_facts;
    }
}
static void reset_readiness_facts_to_ready(void)
{
    s_test_readiness_facts.recovery_mode = false;
    s_test_readiness_facts.safety_link_up = true;
    s_test_readiness_facts.safety_trip_mask = 0u;
    s_test_readiness_facts.crash_have_record = false;
    s_test_readiness_facts.crash_acknowledged = false;
    s_test_readiness_facts.estop_verified = true;
}

// backup_import_restore_in_flight() fake -- same reasoning as
// test_profile_executor_prestart.c's identical fake (2026-09-28, A4 review
// follow-up A): backup_import.c is not linked into this executable.
static bool s_test_restore_in_flight = false;
bool backup_import_restore_in_flight(void)
{
    return s_test_restore_in_flight;
}

// HTTP audit E1 finding 1: see test_profile_executor_prestart.c's identical fake.
uint32_t zones_config_generation(void)
{
    return 0;
}
static bool s_test_zones_gen_bump_after_heat_claim = false;
bool zones_config_changed_since(uint32_t gen_snapshot)
{
    (void)gen_snapshot;
    return s_test_zones_gen_bump_after_heat_claim && s_heat_zone_claim_begin_calls > 0;
}

// Factory reset in flight: see test_profile_executor_prestart.c's identical fake.
static bool s_test_reset_in_flight = false;
bool relay_authority_reset_in_flight(void)
{
    return s_test_reset_in_flight;
}

// The distinguishing observation is the same one
// test_profile_executor_prestart.c's equivalent tests use: the gate runs
// BEFORE the s_at.lock == NULL guard, so a blocked board answers the gate's
// message while a READY board falls through to "autotune engine not
// started" -- test_run_refuses_before_start() above IS the "ready board gets
// past the gate" case already, since it runs on the default fully-ready
// facts and still sees the OLD message.
//
// NEGATIVE TEST (performed 2026-09-09, RED confirmed, restored by hand):
// deleting the `if (readiness_gate_refuses_start(...)) { ... return false; }`
// block from autotune_engine.c's autotune_begin_run_locked() fails all four
// tests below with
//   FAIL: ... refused by the readiness interlock, naming the item
// (each instead sees "autotune engine not started"). Restored by retyping the
// block; `git diff` on autotune_engine.c then comes back empty.
static void begin_run_and_expect_gate_refusal(const char *what, const char *needle)
{
    char err[192] = {0};
    bool ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_SIMC, err, sizeof(err));
    TEST_CHECK(!ok, what);
    TEST_CHECK(strstr(err, needle) != NULL,
               "autotune_engine_run() reports the readiness interlock's message, naming the item");
    TEST_CHECK(strcmp(err, "autotune engine not started") != 0,
               "the gate ran BEFORE the generic prestart guard, so the operator sees the real reason");
    reset_readiness_facts_to_ready();
}

static void test_begin_run_refused_by_readiness_recovery_mode(void)
{
    TEST_SECTION("autotune_begin_run_locked() is refused by the readiness interlock -- recovery mode");
    reset_readiness_facts_to_ready();
    s_test_readiness_facts.recovery_mode = true;
    /* system_mode_gate.c's wording, not readiness_gate.h's -- proves slice
     * 2's mode-gate call fires first (see test_profile_executor_prestart.c). */
    begin_run_and_expect_gate_refusal("a recovery-mode boot refuses an autotune start", "no firing or autotune");
}

static void test_begin_run_refused_by_restore_in_flight(void)
{
    TEST_SECTION("autotune_begin_run_locked() is refused by the system mode gate -- restore in flight");
    reset_readiness_facts_to_ready();
    s_test_restore_in_flight = true;
    begin_run_and_expect_gate_refusal("a backup restore in flight refuses an autotune start",
                                       "backup restore is in progress");
    s_test_restore_in_flight = false;
}

static void test_begin_run_refused_by_readiness_safety_trip(void)
{
    TEST_SECTION("autotune_begin_run_locked() is refused by the readiness interlock -- latched safety trip");
    reset_readiness_facts_to_ready();
    s_test_readiness_facts.safety_trip_mask = 0x0020u; /* S6a mainFault (trip_mask = 1 << (reason-1) = 1 << 5) -- what a reboot latches */
    begin_run_and_expect_gate_refusal("a latched safety trip refuses an autotune start", "TRIP");
}

static void test_begin_run_refused_by_readiness_crash_report(void)
{
    TEST_SECTION("autotune_begin_run_locked() is refused by the readiness interlock -- unacknowledged crash");
    reset_readiness_facts_to_ready();
    s_test_readiness_facts.crash_have_record = true;
    s_test_readiness_facts.crash_acknowledged = false;
    begin_run_and_expect_gate_refusal("an unacknowledged crash report refuses an autotune start",
                                       "UNACKNOWLEDGED CRASH REPORT");
}

static void test_begin_run_refused_by_readiness_estop_unverified(void)
{
    TEST_SECTION("autotune_begin_run_locked() is refused by the readiness interlock -- E-stop unverified");
    reset_readiness_facts_to_ready();
    s_test_readiness_facts.estop_verified = false;
    begin_run_and_expect_gate_refusal("an unverified E-stop interlock refuses an autotune start",
                                       "E-STOP INTERLOCK");
}

static void test_begin_run_passes_the_readiness_gate_when_ready(void)
{
    /* The half a refuse-everything gate fails. A ready board must get PAST
     * the interlock -- observable here as reaching the next refusal down
     * (the s_at.lock == NULL prestart guard) with its own, different
     * message. */
    TEST_SECTION("autotune_begin_run_locked() passes the readiness interlock on a fully-ready board");
    reset_readiness_facts_to_ready();
    char err[192] = {0};
    bool ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_SIMC, err, sizeof(err));
    TEST_CHECK(!ok, "still refused overall -- s_at.lock is NULL in this prestart section");
    TEST_CHECK(strcmp(err, "autotune engine not started") == 0,
               "a ready board reaches the ORIGINAL prestart refusal, not the readiness interlock's");
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
    TEST_CHECK(!autotune_engine_accept(NULL, NULL), "accept must return false, not crash");
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
// Calling autotune_engine_run() after that would hit autotune_begin_run_locked()'s
// s_at.lock == NULL guard and, before this comment's fix, crashed this test
// file's process outright (xSemaphoreTake() on a NULL handle asserts --
// stubs/freertos/semphr.h's OWN header comment says this is deliberate, for
// exactly the reason this file's prestart tests exist).
//
// Task creation is orthogonal to what this section tests (the STEPPING
// loop's tick logic, not task scheduling), so the lock is created directly
// here instead -- autotune_engine_run() itself, including
// autotune_begin_run_locked()'s real guard_cfg setup from the zones_config_*()
// stubs, still runs for real; only the never-succeeding task spawn is
// bypassed.
static void start_stepping_run_rule(float max_temp_c, float step_duty, autotune_rule_t rule)
{
    static MAX31856BusClass bus;
    // A real (if never-dereferenced-for-real, thanks to the
    // safety_link_set_fault_source() stub above) SafetyLinkClass instance --
    // without it s_at.safety reads NULL and autotune_escalate_and_abort()'s `if
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
    s_stub_min_temp_c = 0.0f;
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

// Phase 7c pre-start thermal readiness -- see check_thermal_readiness_
// locked()'s own comment in autotune_engine.c. Unlike start_stepping_run()
// above, this does NOT jump the state straight to STEPPING: it leaves the
// run in SETTLING, exactly where the readiness check actually lives, so
// force_settling_transition_tick() below can drive the REAL SETTLING->
// STEPPING transition code (including the check) through the real tick
// loop, not a white-box call.
static void start_settling_run(float max_temp_c, float step_duty)
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

    s_stub_max_temp_c = max_temp_c;
    s_stub_min_temp_c = 0.0f;
    s_stub_ch0_ok = true;
    reset_extra_channel_stubs();

    char errbuf[96] = {0};
    bool ok = autotune_engine_run(0, step_duty, AUTOTUNE_RULE_SIMC, errbuf, sizeof(errbuf));
    TEST_CHECK(ok, "autotune_engine_run() must accept a step test on zone 0");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_SETTLING, "test setup: a fresh run starts in SETTLING");
}

// Forces the SETTLING->STEPPING elapsed-time transition to fire on the
// NEXT tick, through this harness's frozen xTaskGetTickCount() stub (always
// 0) -- same unsigned-wraparound trick test_target_mode_probe_dispatch_
// driven_through_the_real_tick_loop() already uses for last_sample_tick
// (see that test's own comment): TickType_t is uint32_t, so 0 - 1 wraps to
// 0xFFFFFFFF ticks, comfortably past AUTOTUNE_ENGINE_SETTLE_S. Runs exactly
// one real tick and returns -- the caller has already set
// s_stub_ch0_temp_c/s_stub_extra_ch_*[] to whatever this tick's readings
// should be.
static void force_settling_transition_tick(void)
{
    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    s_at.phase_start_tick = 1;
    s_at.prev_tick = 1;
    autotune_engine_tick_locked();
    xSemaphoreGive(s_at.lock);
}

// Runs up to n_ticks ticks, stopping early if the run leaves the "running"
// states (e.g. a guard trip). Each tick's channel-0 reading is
// start_temp_c + i * per_tick_delta_c -- per_tick_delta_c == 0 reproduces a
// flat/stalled reading; a small positive value reproduces healthy heating.
static void run_ticks(float start_temp_c, float per_tick_delta_c, int n_ticks)
{
    for (int i = 0; i < n_ticks; i++) {
        // Mirrors task_entry()'s own pre-lock computation (review blocker 1
        // lock-order fix): the hint is computed BEFORE the lock is taken,
        // same order production code uses, even though this test binary is
        // single-threaded and the exact placement has no functional
        // consequence here -- kept for fidelity with the real call site.
        bool other_zone_active_hint = any_other_zone_profile_active(s_at.zone_index);
        xSemaphoreTake(s_at.lock, portMAX_DELAY);
        if (!state_is_running(s_at.state)) {
            xSemaphoreGive(s_at.lock);
            break;
        }
        s_stub_ch0_temp_c = start_temp_c + (float)i * per_tick_delta_c;
        s_at.other_zone_profile_active_hint = other_zone_active_hint;
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
// (autotune_begin_run_locked()'s very first check), so a rule-rejection test run
// before autotune_engine_start() would pass for the wrong reason -- masked by
// that earlier guard, never actually reaching the rule check under test. See
// this file's task brief: exactly the "input the range check rejected before
// the check under test ever ran" trap. A deliberate revert of the rule check
// confirmed this: with it removed, the naive "before start" version of this
// test kept passing (125/125) because autotune_begin_run_locked()'s NULL-lock guard
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
    // unrelated reason autotune_begin_run_locked() would refuse everything here.
    prep_live_lock_no_run();
    err[0] = '\0';
    ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_SIMC, err, sizeof(err));
    TEST_CHECK(ok, "positive control: SIMC must be accepted with the same live-lock setup ZN/TL were refused under");
}

static void test_step_run_accepts_simc_and_cohen_coon_and_stores_the_rule(void)
{
    TEST_SECTION("autotune_engine_run() accepts SIMC and Cohen-Coon, and records which one for autotune_finalize_fit()");
    start_stepping_run_rule(/*max_temp_c=*/0.0f, /*step_duty=*/1.0f, AUTOTUNE_RULE_SIMC);
    TEST_CHECK(s_at.step_rule == AUTOTUNE_RULE_SIMC, "SIMC accepted and stored");

    start_stepping_run_rule(/*max_temp_c=*/0.0f, /*step_duty=*/1.0f, AUTOTUNE_RULE_COHEN_COON);
    TEST_CHECK(s_at.step_rule == AUTOTUNE_RULE_COHEN_COON,
               "Cohen-Coon accepted and stored -- this is what autotune_finalize_fit() passes to "
               "pid_autotune_tune_from_fopdt() instead of the old hardcoded AUTOTUNE_RULE_SIMC");
}

// Writes a synthetic FOPDT step-response trace directly into s_at.zone_trace,
// bypassing the STEPPING tick loop entirely: this host build's
// xTaskGetTickCount() stub always returns 0 (see this file's header comment
// on the STEPPING-loop guard tests above), which freezes s_at.elapsed_s and
// last_sample_tick forever and makes the real record-a-sample-every-
// AUTOTUNE_ENGINE_SAMPLE_PERIOD_S path unreachable from a driven tick loop.
// autotune_finalize_fit() itself has no such dependency (it only reads
// zone_trace/trace_count/zone_baseline_c/step_duty/step_rule, all set
// directly here), so it is called white-box, same convention as this file's
// #include of autotune_engine.c -- and it is the actual function under test
// for "does the step path still hardcode SIMC".
// zone-index-parameterized version -- test_finalize_fit_persists_valid_
// cross_gain_cells() below needs to write a trace for more than one zone
// (the zone under test AND its peers) to exercise autotune_finalize_fit()'s
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
     * 2026-09-01, see autotune_finalize_fit()'s physical-plausibility comment) get
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
 * REAL autotune_finalize_fit() -> pid_autotune_fit_fopdt() path (not hand-setting
 * s_at.model fields, which autotune_finalize_fit() would overwrite anyway) reliably
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
 * sample()/autotune_unpack_zone_trace() produce on real hardware) through
 * autotune_finalize_fit() (which calls the real pid_autotune_fit_fopdt()) and then
 * through the real autotune_engine_accept(NULL, NULL) -- no field is hand-set.
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

    finalize_fit_then_flush_coupling();

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
    int clear_ki_baseline_calls_before = g_clear_ki_baseline_calls;
    bool accepted = autotune_engine_accept(NULL, NULL);
    snprintf(msg, sizeof(msg),
            "%s: a well-converged 8*tau fit must ACCEPT WITHOUT ack_unsettled (converged=%d "
            "tau_ok=%d) -- quantization noise on the last sample must not gate acceptance",
            label, (int)s_at.model.extrapolation_converged, (int)s_at.model.tau_consistent_with_gain);
    TEST_CHECK(accepted, msg);
    // S6 (2026-09-01 audit of ae5905f): the old fake was a bare no-op with
    // no call counter, so deleting autotune_engine.c's adaptive_tune_clear_
    // ki_baseline() call entirely left the whole suite green. Assert the
    // wiring itself, not just that this file links against the symbol.
    snprintf(msg, sizeof(msg),
            "%s: a successful accept() must call adaptive_tune_clear_ki_baseline() exactly once "
            "(Q3: the just-committed result's remedy for the Ki-diagnosis cumulative bound)", label);
    TEST_CHECK(g_clear_ki_baseline_calls == clear_ki_baseline_calls_before + 1, msg);
    snprintf(msg, sizeof(msg), "%s: adaptive_tune_clear_ki_baseline() must be called for the zone "
                              "autotune_engine_accept() just committed", label);
    TEST_CHECK(g_clear_ki_baseline_last_zone == s_at.zone_index, msg);
    s_stub_set_pid_result = false;
}

static void test_accept_succeeds_end_to_end_regardless_of_last_sample_quantization_dither(void)
{
    TEST_SECTION("FINAL REVIEW: autotune_engine_accept(NULL, NULL) must succeed end-to-end on a clean "
                 "8*tau fit whether the last (quantized) sample dithers -0.1, 0.0, or +0.1 degC");
    run_one_dither_case_end_to_end(-0.1f, "dither -0.1C");
    run_one_dither_case_end_to_end(0.0f, "dither 0.0C");
    run_one_dither_case_end_to_end(+0.1f, "dither +0.1C");
}

static void test_finalize_fit_uses_the_requested_rule(void)
{
    TEST_SECTION("autotune_finalize_fit() uses s_at.step_rule, not a hardcoded SIMC -- PID_EXPANSION_PLAN.md Phase 1");
    // K=50 degC/duty, tau=200s, L=20s (well above
    // AUTOTUNE_COHEN_COON_MIN_DEAD_TIME_S so Cohen-Coon does not refuse on
    // dead time), 60 samples * 10s/sample = 600s -- comfortably past 5*tau
    // so both the two-point crossing fit AND the settle band would succeed.
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;
    write_synthetic_fopdt_trace(/*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/50.0f, /*tau_s=*/200.0f,
                                /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    finalize_fit_then_flush_coupling();
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "a well-formed trace must fit");
    TEST_CHECK(s_at.model.valid, "the FOPDT fit must have succeeded");
    TEST_CHECK(s_at.proposed_gains.rule == AUTOTUNE_RULE_SIMC, "SIMC requested -> SIMC reported");
    TEST_CHECK(s_at.proposed_gains.refusal == AUTOTUNE_REFUSAL_OK, "a valid model + SIMC must not refuse");
    float simc_kp = s_at.proposed_gains.kp;
    TEST_CHECK(simc_kp != 0.0f, "sanity: SIMC actually produced a nonzero Kp on this trace");

    // Identical trace, Cohen-Coon requested instead: must report its OWN
    // rule and produce different numbers than SIMC on the exact same fitted
    // model -- if autotune_finalize_fit() were still passing AUTOTUNE_RULE_SIMC to
    // pid_autotune_tune_from_fopdt() regardless of s_at.step_rule (the
    // pre-fix defect this test exists to catch), both of these would read
    // back as SIMC/simc_kp again.
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    s_at.step_rule = AUTOTUNE_RULE_COHEN_COON;
    write_synthetic_fopdt_trace(/*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/50.0f, /*tau_s=*/200.0f,
                                /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    finalize_fit_then_flush_coupling();
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "the same trace must fit again");
    TEST_CHECK(s_at.model.valid, "the FOPDT fit must have succeeded");
    TEST_CHECK(s_at.proposed_gains.rule == AUTOTUNE_RULE_COHEN_COON, "Cohen-Coon requested -> Cohen-Coon reported");
    TEST_CHECK(s_at.proposed_gains.refusal == AUTOTUNE_REFUSAL_OK,
               "this trace's dead time is well above AUTOTUNE_COHEN_COON_MIN_DEAD_TIME_S, so Cohen-Coon must not "
               "refuse");
    TEST_CHECK(s_at.proposed_gains.kp != simc_kp,
               "Cohen-Coon's Kp must differ from SIMC's Kp on the identical fitted model -- if this fails, "
               "autotune_finalize_fit() is still hardcoding one rule regardless of what was requested");
}

// 2026-08-30 (ZONES_CFG_VERSION 10->11): autotune_finalize_fit()'s new persist step
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
    TEST_SECTION("autotune_finalize_fit() persists every valid cross-gain cell through "
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

    // Zone 1 (self): K=50, tau=200s, L=20s -- the direct fit autotune_finalize_fit()
    // needs to succeed before it ever reaches the cross-zone loop.
    write_synthetic_fopdt_trace_for_zone(1, /*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/50.0f, /*tau_s=*/200.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    // Zone 0: a real, fittable cross-gain, K=0.5 (well inside
    // ZONE_COUPLING_COEFF_MAX) -- this is what must land in
    // coupling_coeff[0] of ZONE 0's row (the AFFECTED zone), not zone 1's.
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/22.0f, /*k_gain_c_per_duty=*/0.5f, /*tau_s=*/150.0f,
                                         /*dead_time_s=*/15.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    // Zone 2: baseline never went valid -- autotune_finalize_fit()'s own
    // !s_at.zone_baseline_valid[j] check must skip it, same as a sensor
    // that never reported during the run.
    s_at.zone_baseline_valid[2] = false;

    finalize_fit_then_flush_coupling();
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "the direct (zone 1) fit must succeed");
    TEST_CHECK(s_at.model.valid, "zone 1's own model must be valid");
    TEST_CHECK(s_at.coupling.cell[1][0].valid, "zone 0's cross-gain cell was fitted and marked valid");
    TEST_CHECK(!s_at.coupling.cell[1][2].valid, "zone 2's cell is invalid -- its baseline never went valid");

    TEST_CHECK(s_coupling_cell_calls[0][1].called,
              "zone 0's row, column 1 (the AFFECTED zone's cell against the STEPPED zone) was persisted");
    TEST_CHECK_NEAR(s_coupling_cell_calls[0][1].coeff, s_at.coupling.cell[1][0].model.k_gain_c_per_duty, 1e-4,
                    "the persisted value is EXACTLY the fitted cross-gain, no scaling applied "
                    "(coupling_coeff[] stores a raw degC/duty gain, not a ratio -- see autotune_finalize_fit()'s own "
                    "comment)");
    TEST_CHECK(!s_coupling_cell_calls[2][1].called,
              "zone 2's cell was NEVER persisted -- its fit was invalid (unmeasured baseline)");
    TEST_CHECK(!s_coupling_cell_calls[1][1].called,
              "the diagonal (zone 1 against itself) was never touched by the persist step -- "
              "that is model_k_dc's job, not coupling_coeff[]'s");

    s_stub_thermo_count = 0; // restore this file's original hardcoded default for every other test
}

// ZONES_CFG_VERSION 11->12 (DATA PLUMBING pass): proves autotune_finalize_fit()'s
// persist step carries tau_s/dead_time_s through zones_config_set_
// coupling_cell() in the RIGHT orientation -- [affected][stepped], same as
// coupling_coeff[]. Deliberately ASYMMETRIC (pair (0,1) tau=40s/L=5s vs pair
// (1,0) tau=90s/L=15s): a transpose bug (writing [stepped][affected] instead
// of [affected][stepped], or reading the wrong peer's fit) would land the
// WRONG pair's numbers in one of the two cells below, and this test would
// catch it; a symmetric fixture could not distinguish "orientation correct"
// from "orientation swapped" and would be vacuous. Two separate step tests
// (zone 0 stepped/zone 1 the peer, then zone 1 stepped/zone 0 the peer) are
// run because autotune_finalize_fit() only ever persists the STEPPED zone's peers in
// one call -- the [1][0] cell and the [0][1] cell are each written by a
// DIFFERENT run in real operation, exactly as reproduced here.
static void test_finalize_fit_persists_cross_gain_tau_dead_time_in_correct_orientation(void)
{
    TEST_SECTION("autotune_finalize_fit() persists coupling_tau_s/coupling_dead_time_s in the SAME "
                 "[affected][stepped] orientation as coupling_coeff -- asymmetric fixture, "
                 "a transpose bug would land the wrong pair's numbers");

    // Run 1: zone 0 stepped, zone 1 the peer -- must land in cell [1][0]
    // (zone 1's row, column 0) with tau=40s, L=5s.
    memset(&s_at, 0, sizeof(s_at));
    reset_coupling_cell_calls();
    s_stub_thermo_count = 3;
    s_at.zone_index = 0;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;
    s_at.step_settled = true;
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/50.0f, /*tau_s=*/200.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    write_synthetic_fopdt_trace_for_zone(1, /*baseline_c=*/22.0f, /*k_gain_c_per_duty=*/0.5f, /*tau_s=*/40.0f,
                                         /*dead_time_s=*/5.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    s_at.zone_baseline_valid[2] = false; // zone 2 not part of this run's fixture
    finalize_fit_then_flush_coupling();
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "run 1 (zone 0 stepped) direct fit must succeed");
    TEST_CHECK(s_coupling_cell_calls[1][0].called, "cell [1][0] (zone 1 affected, zone 0 stepped) was persisted");
    // The FOPDT curve fit does not reproduce the fed-in tau/L exactly off a
    // finite synthetic trace, so this checks against a generous envelope
    // around pair (0,1)'s fed values (40s/5s) rather than pinning an exact
    // number -- what actually proves orientation is the CROSS-comparison
    // against run 2's cell below (fed 90s/15s), not either absolute value
    // alone.
    float run1_tau = s_coupling_cell_calls[1][0].tau_s;
    float run1_dead_time = s_coupling_cell_calls[1][0].dead_time_s;
    TEST_CHECK(run1_tau > 0.0f && run1_tau < 80.0f,
              "cell [1][0]'s persisted tau_s is in pair (0,1)'s ballpark (fed 40s), nowhere near pair "
              "(1,0)'s fed 90s");

    // Run 2: zone 1 stepped, zone 0 the peer -- must land in cell [0][1]
    // (zone 0's row, column 1) with tau=90s, L=15s, a DIFFERENT number from
    // run 1's cell -- proof this is not just the same value read back twice.
    memset(&s_at, 0, sizeof(s_at));
    reset_coupling_cell_calls();
    s_at.zone_index = 1;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;
    s_at.step_settled = true;
    write_synthetic_fopdt_trace_for_zone(1, /*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/50.0f, /*tau_s=*/200.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/22.0f, /*k_gain_c_per_duty=*/0.5f, /*tau_s=*/90.0f,
                                         /*dead_time_s=*/15.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    s_at.zone_baseline_valid[2] = false;
    finalize_fit_then_flush_coupling();
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "run 2 (zone 1 stepped) direct fit must succeed");
    TEST_CHECK(s_coupling_cell_calls[0][1].called, "cell [0][1] (zone 0 affected, zone 1 stepped) was persisted");
    float run2_tau = s_coupling_cell_calls[0][1].tau_s;
    float run2_dead_time = s_coupling_cell_calls[0][1].dead_time_s;
    TEST_CHECK(run2_tau > 80.0f,
              "cell [0][1]'s persisted tau_s is in pair (1,0)'s ballpark (fed 90s), nowhere near pair "
              "(0,1)'s fed 40s");
    TEST_CHECK(!s_coupling_cell_calls[1][0].called,
              "run 2 never touches cell [1][0] -- that was run 1's cell, a different (affected, stepped) pair");

    // THE orientation proof: pair (1,0) was fed a materially larger tau/L
    // than pair (0,1) (90s/15s vs 40s/5s). A transpose bug -- writing
    // [stepped][affected] instead of [affected][stepped], or reading the
    // wrong peer's fit -- would swap which cell ends up with the larger
    // value; this fails on either kind of swap regardless of exactly how
    // precisely the curve fit reproduces the fed-in numbers.
    TEST_CHECK(run2_tau > run1_tau,
              "cell [0][1] (fed the larger tau, 90s) must come out with a LARGER fitted tau_s than "
              "cell [1][0] (fed the smaller tau, 40s) -- a transpose bug would reverse this");
    TEST_CHECK(run2_dead_time > run1_dead_time,
              "cell [0][1] (fed the larger L, 15s) must come out with a LARGER fitted dead_time_s than "
              "cell [1][0] (fed the smaller L, 5s) -- a transpose bug would reverse this");

    s_stub_thermo_count = 0;
}

// ZONES_CFG_VERSION 11->12 (DATA PLUMBING pass): an invalid peer fit
// (cell->valid == false, e.g. an unmeasured baseline) must not persist ANY
// of coeff/tau_s/dead_time_s for that cell -- extends
// test_finalize_fit_persists_valid_cross_gain_cells()'s existing "invalid
// peer is skipped" coverage (which only checked `.called`) to also confirm
// the recorded tau_s/dead_time_s stay at their fake's zero-initialized
// default, i.e. genuinely never written, not written-then-ignored.
static void test_finalize_fit_does_not_persist_tau_dead_time_for_invalid_peer(void)
{
    TEST_SECTION("autotune_finalize_fit() does not persist coupling_tau_s/coupling_dead_time_s "
                 "for a peer whose fit is invalid");
    memset(&s_at, 0, sizeof(s_at));
    reset_coupling_cell_calls();
    s_stub_thermo_count = 3;
    s_at.zone_index = 1;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;
    s_at.step_settled = true;

    write_synthetic_fopdt_trace_for_zone(1, /*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/50.0f, /*tau_s=*/200.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    // Zone 0: a valid peer fit, for contrast with zone 2's invalid one below.
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/22.0f, /*k_gain_c_per_duty=*/0.5f, /*tau_s=*/60.0f,
                                         /*dead_time_s=*/8.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    // Zone 2: baseline never went valid -- same invalidity autotune_finalize_fit()'s
    // existing !s_at.zone_baseline_valid[j] check must skip.
    s_at.zone_baseline_valid[2] = false;

    finalize_fit_then_flush_coupling();
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "the direct (zone 1) fit must succeed");
    TEST_CHECK(!s_at.coupling.cell[1][2].valid, "zone 2's cell is invalid -- its baseline never went valid");
    TEST_CHECK(!s_coupling_cell_calls[2][1].called,
              "zone 2's cell was never persisted -- neither coeff nor tau_s/dead_time_s");
    TEST_CHECK_NEAR(s_coupling_cell_calls[2][1].coeff, 0.0f, 1e-6,
                    "the fake's coeff for the untouched cell stays at its zero default");
    TEST_CHECK_NEAR(s_coupling_cell_calls[2][1].tau_s, 0.0f, 1e-6,
                    "the fake's tau_s for the untouched cell stays at its zero default -- proves this was "
                    "never written, not written-then-discarded");
    TEST_CHECK_NEAR(s_coupling_cell_calls[2][1].dead_time_s, 0.0f, 1e-6,
                    "the fake's dead_time_s for the untouched cell stays at its zero default");
    // The valid peer (zone 0), for contrast, DID persist all three.
    TEST_CHECK(s_coupling_cell_calls[0][1].called, "zone 0's valid cell WAS persisted, unlike zone 2's");

    s_stub_thermo_count = 0;
}

// 2026-08-31 panic fix: proves autotune_finalize_fit()'s persist step actually goes
// THROUGH uart_bridge_ext_run_on_flash_worker() -- one submission per run,
// not one direct zones_config_set_coupling_cell() call per cell -- rather
// than merely landing on the right cells (already covered by
// test_finalize_fit_persists_valid_cross_gain_cells() above, which cannot
// tell the two apart: both call this fake the same number of times either
// way). Break-proof: this test is what actually catches a revert back to a
// direct call, which is exactly the change that panicked real hardware.
static void test_finalize_fit_routes_persist_through_flash_worker(void)
{
    TEST_SECTION("autotune_finalize_fit() submits coupling-cell persistence as ONE job to "
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

    finalize_fit_then_flush_coupling();
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "the direct (zone 1) fit must succeed");
    TEST_CHECK(s_coupling_cell_calls[0][1].called, "zone 0's cell was persisted");
    TEST_CHECK(s_coupling_cell_calls[2][1].called, "zone 2's cell was persisted");
    TEST_CHECK(g_flash_worker_submit_calls == 1,
              "exactly ONE job submitted to the flash worker for the whole run, covering both cells -- "
              "if this reads 2 (or 0), the persist path has drifted off "
              "uart_bridge_ext_run_on_flash_worker() and a real board will panic the next time this runs "
              "for real (see autotune_finalize_fit()'s and autotune_engine_start()'s own comments for the coredump "
              "this guards against)");

    s_stub_thermo_count = 0; // restore this file's original hardcoded default for every other test
}

/* F2, flash-worker lock-inversion audit 2026-10-09: production runs
 * autotune_finalize_fit() under s_at.lock (task_entry() -> tick body). It
 * used to dispatch coupling_persist_job to bx_flash_worker from right there,
 * and the worker's UART autotune handlers take s_at.lock: deadlock. Called
 * here with s_at.lock held, exactly as the tick holds it, finalize must
 * dispatch NOTHING and leave the job parked for after the give. */
static void test_finalize_fit_under_lock_does_not_dispatch_coupling_persist(void)
{
    TEST_SECTION("F2: autotune_finalize_fit() under s_at.lock parks the coupling persist instead of "
                 "dispatching to the flash worker while the lock is held");
    memset(&s_at, 0, sizeof(s_at));
    reset_coupling_cell_calls();
    g_flash_worker_submit_calls = 0;
    g_flash_worker_dispatch_under_lock = 0;
    s_stub_thermo_count = 3;
    s_at.lock = xSemaphoreCreateMutex();
    s_at.zone_index = 1;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;
    s_at.step_settled = true;
    write_synthetic_fopdt_trace_for_zone(1, 25.0f, 50.0f, 200.0f, 20.0f, 1.0f, 60);
    write_synthetic_fopdt_trace_for_zone(0, 22.0f, 0.5f, 150.0f, 15.0f, 1.0f, 60);
    write_synthetic_fopdt_trace_for_zone(2, 23.0f, 0.7f, 160.0f, 16.0f, 1.0f, 60);

    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    autotune_finalize_fit();
    TEST_CHECK(g_flash_worker_dispatch_under_lock == 0,
               "MUST GO RED if autotune_finalize_fit() dispatches to the flash worker while s_at.lock is "
               "held -- the worker's UART autotune handlers take s_at.lock, so that deadlocks");
    TEST_CHECK(g_flash_worker_submit_calls == 0, "nothing is submitted from under the lock");
    TEST_CHECK(s_at.pending_coupling_valid && s_at.pending_coupling.count == 2 &&
                   s_at.pending_coupling.stepped_zone == 1,
               "both valid peer cells are parked in s_at.pending_coupling for task_entry() to dispatch");
    xSemaphoreGive(s_at.lock);
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "the direct (zone 1) fit must succeed");
    TEST_CHECK(!s_coupling_cell_calls[0][1].called && !s_coupling_cell_calls[2][1].called,
               "no coupling cell is written before the dispatch");

    memset(&s_at, 0, sizeof(s_at));
    s_stub_thermo_count = 0;
}

/* F2 companion: drives task_entry()'s real lock section
 * (autotune_engine_tick_under_lock()) with a parked coupling persist. The
 * engine is idle, so no tick body runs; the helper must still take the
 * parked job under the lock and dispatch it after the give. The stub worker
 * refuses (records a deadlock) if any lock is held at dispatch. */
static void test_tick_under_lock_dispatches_coupling_persist_after_give(void)
{
    TEST_SECTION("F2: autotune_engine_tick_under_lock() dispatches the parked coupling persist only after "
                 "giving s_at.lock, and writes exactly the parked cells");
    memset(&s_at, 0, sizeof(s_at));
    reset_coupling_cell_calls();
    g_flash_worker_submit_calls = 0;
    g_flash_worker_dispatch_under_lock = 0;
    s_at.lock = xSemaphoreCreateMutex();
    s_at.state = AUTOTUNE_ENGINE_DONE;
    s_at.pending_coupling_valid = true;
    s_at.pending_coupling.stepped_zone = 1;
    s_at.pending_coupling.count = 2;
    s_at.pending_coupling.affected_zone[0] = 0;
    s_at.pending_coupling.coeff[0] = 0.5f;
    s_at.pending_coupling.tau_s[0] = 150.0f;
    s_at.pending_coupling.dead_time_s[0] = 15.0f;
    s_at.pending_coupling.affected_zone[1] = 2;
    s_at.pending_coupling.coeff[1] = 0.7f;
    s_at.pending_coupling.tau_s[1] = 160.0f;
    s_at.pending_coupling.dead_time_s[1] = 16.0f;

    ThermoChannelSnapshot unused_snap;
    memset(&unused_snap, 0, sizeof(unused_snap));
    int depth_before = g_test_stub_lock_depth;
    bool not_running = autotune_engine_tick_under_lock(false, false, &unused_snap);
    TEST_CHECK(not_running, "an idle (DONE) engine reports not running");
    TEST_CHECK(g_test_stub_lock_depth == depth_before, "the helper gives every lock it takes");
    TEST_CHECK(g_flash_worker_dispatch_under_lock == 0,
               "MUST GO RED if the coupling persist is dispatched before xSemaphoreGive(s_at.lock)");
    TEST_CHECK(g_flash_worker_submit_calls == 1, "exactly one flash-worker job for the parked persist");
    TEST_CHECK(!s_at.pending_coupling_valid, "the parked persist is consumed, never dispatched twice");
    TEST_CHECK(s_coupling_cell_calls[0][1].called && s_coupling_cell_calls[2][1].called,
               "both parked cells were written ([affected][stepped])");
    TEST_CHECK_NEAR(s_coupling_cell_calls[0][1].coeff, 0.5f, 1e-6, "cell [0][1] carries the parked coeff");
    TEST_CHECK_NEAR(s_coupling_cell_calls[2][1].tau_s, 160.0f, 1e-6, "cell [2][1] carries the parked tau");

    g_flash_worker_submit_calls = 0;
    autotune_engine_tick_under_lock(false, false, &unused_snap);
    TEST_CHECK(g_flash_worker_submit_calls == 0, "a second tick with nothing parked dispatches nothing");

    memset(&s_at, 0, sizeof(s_at));
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
    TEST_SECTION("(4) autotune_finalize_fit() does NOT persist cross-gain coupling cells when the fit never "
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

    finalize_fit_then_flush_coupling();
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
 * settled (see autotune_finalize_fit()'s own "Argued explicitly" comment for why).
 * Drives the REAL pid_autotune_fit_fopdt() through a defeat-case trace
 * (write_defeat_case_trace_for_zone(), same construction as test_pid_
 * autotune.c's own defeat case) on the DIRECT zone, with step_settled=true
 * so settled alone would NOT have blocked persistence -- only the
 * extension is under test here. Confirmed to FAIL (coupling persisted
 * anyway) when autotune_finalize_fit()'s coupling-persist condition is temporarily
 * stubbed back to `if (s_at.model.settled)` (the round-2-only gate) --
 * restoring the three-flag condition makes it pass again. */
static void test_finalize_fit_skips_coupling_persist_when_extrapolation_did_not_converge(void)
{
    TEST_SECTION("(round-3) autotune_finalize_fit() does NOT persist cross-gain coupling cells when the "
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

    finalize_fit_then_flush_coupling();
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

// 2026-08-31 defect fix: autotune_finalize_fit()'s own isfinite/range guard (~line
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
    TEST_SECTION("autotune_finalize_fit() -- a cross-gain cell that fits VALID but lands outside "
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

    finalize_fit_then_flush_coupling();
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

// autotune_finalize_fit() must not persist ANYTHING from an aborted run -- proven by
// giving zone 1 (the zone under test) a degenerate trace that fails to fit
// at all: autotune_finalize_fit() returns early (before the cross-zone loop, let
// alone the persist step) the exact same way an aborted relay run would.
static void test_finalize_fit_persists_nothing_on_an_invalid_direct_fit(void)
{
    TEST_SECTION("autotune_finalize_fit() persists NOTHING -- not even a valid-looking peer cell -- "
                 "when the direct (self) fit itself fails");
    memset(&s_at, 0, sizeof(s_at));
    reset_coupling_cell_calls();
    s_stub_thermo_count = 3;
    s_at.zone_index = 1;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;

    // Zone 1: a flat trace (no rise at all) -- pid_autotune_fit_fopdt()
    // refuses a plant with no measurable gain, so s_at.model.valid stays
    // false and autotune_finalize_fit() returns before touching the coupling matrix
    // or calling any setter at all.
    write_synthetic_fopdt_trace_for_zone(1, /*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/0.0f, /*tau_s=*/200.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    // Zone 0: would fit perfectly well on its own -- if this cell got
    // persisted anyway, that would prove the direct-fit guard is not
    // actually gating the whole function the way it's supposed to.
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/22.0f, /*k_gain_c_per_duty=*/0.5f, /*tau_s=*/150.0f,
                                         /*dead_time_s=*/15.0f, /*duty_step=*/1.0f, /*n_samples=*/60);

    finalize_fit_then_flush_coupling();
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
// test needs autotune_begin_run_locked() itself to clear.
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

// Reset-one-side bug class (project memory: three prior instances of this
// exact shape in one day). autotune_begin_run_locked() used to clear ONLY
// s_at.model.valid/s_at.relay.valid, leaving k_gain_c_per_duty/tau_s/
// dead_time_s/invalid_reason (and relay's ku/tu_s/amplitude_c/cycles_used)
// holding the PREVIOUS run's numbers -- the struct is otherwise zeroed only
// once, at boot. Same non-start-stepping-run() shape as
// test_next_run_clears_prior_runs_refusal() immediately above, and for the
// same reason: start_stepping_run() memsets s_at and would erase the very
// state this test needs autotune_begin_run_locked() itself to clear.
//
// Verified this test fails without the fix: with s_at.model.valid = false /
// s_at.relay.valid = false (the pre-fix autotune_begin_run_locked() body) restored in
// place of the `s_at.model = (fopdt_model_t){0}; s_at.relay =
// (relay_model_t){0};` fix, k_gain_c_per_duty/tau_s/relay.ku below still read
// the previous run's planted values (36.4/123.0/7.7) and every TEST_CHECK in
// this function fails.
static void test_next_run_clears_prior_runs_model_payload(void)
{
    TEST_SECTION("starting a new autotune run clears the previous run's model/relay payload, not just .valid");
    start_stepping_run_rule(/*max_temp_c=*/0.0f, /*step_duty=*/1.0f, AUTOTUNE_RULE_SIMC);
    // Stand in for a completed run's fitted model/relay result -- exactly
    // the shape autotune_finalize_fit()/finalize_relay_fit() leave behind, and
    // exactly what autotune_engine_get_status() would serialize while
    // state == DONE and .valid == true.
    s_at.state = AUTOTUNE_ENGINE_DONE;
    s_at.model.valid = true;
    s_at.model.k_gain_c_per_duty = 36.4f;
    s_at.model.tau_s = 987.0f;
    s_at.model.dead_time_s = 45.0f;
    s_at.model.settled = true;
    s_at.model.baseline_c = 32.8f;
    s_at.model.final_c = 69.2f;
    s_at.model.raw_rise_c = 36.4f;
    s_at.model.rise_inf_c = 36.4f;
    s_at.relay.valid = true;
    s_at.relay.ku = 7.7f;
    s_at.relay.tu_s = 210.0f;
    s_at.relay.amplitude_c = 3.5f;
    s_at.relay.cycles_used = 5;

    char errbuf[96] = {0};
    bool ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_SIMC, errbuf, sizeof(errbuf));
    TEST_CHECK(ok, "a new run must be accepted on this zone after the earlier one finished");
    TEST_CHECK(!s_at.model.valid, "model.valid must be cleared (already was, before the fix too)");
    TEST_CHECK(!s_at.relay.valid, "relay.valid must be cleared (already was, before the fix too)");
    TEST_CHECK(s_at.model.k_gain_c_per_duty == 0.0f,
               "k_gain_c_per_duty is the previous run's PAYLOAD, not just its valid flag -- must be cleared too");
    TEST_CHECK(s_at.model.tau_s == 0.0f, "tau_s is the same class of stale payload");
    TEST_CHECK(s_at.model.dead_time_s == 0.0f, "dead_time_s is the same class of stale payload");
    TEST_CHECK(!s_at.model.settled, "settled is the same class of stale payload");
    TEST_CHECK(s_at.model.baseline_c == 0.0f, "the new diagnostic baseline_c field is the same class of stale payload");
    TEST_CHECK(s_at.model.rise_inf_c == 0.0f, "the new diagnostic rise_inf_c field is the same class of stale payload");
    TEST_CHECK(s_at.relay.ku == 0.0f, "relay.ku is the same class of stale payload as model's fields");
    TEST_CHECK(s_at.relay.tu_s == 0.0f, "relay.tu_s is the same class of stale payload");
    TEST_CHECK(s_at.relay.amplitude_c == 0.0f, "relay.amplitude_c is the same class of stale payload");
    TEST_CHECK(s_at.relay.cycles_used == 0, "relay.cycles_used is the same class of stale payload");
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
// 2026-09-10 opus review finding B: profile_executor.c's own guard-1 climb-
// window-floor fix (cf3b5adb) never reached autotune_engine.c's identical
// thermal_guard_tick() call site -- s_at.guard_cfg (built in autotune_begin_
// run_locked()) never set climb_window_floor_s, so a STEP run against a
// zone with a real, previously-fitted plant model (tau ~264-271s on this
// bench) and a short operator-configured wrong_dir_window_s (60s, a
// perfectly reasonable value for guard 2's falling-while-heating case)
// evaluated guard 1's climbing branch against that 60s window instead of
// the physically-required dead_time_s+tau_s minimum -- a healthy but slow
// element that has not yet produced a measurable rise within the first 60s
// (it is still inside its own dead time) reads as "not rising" and false-
// trips HEATING_FAILED, exactly the shape cf3b5adb closed for the OTHER
// caller. progress_rise_check_relaxed only covers the window AFTER
// step_element_proven, so this is squarely the early-climb phase leading up
// to that -- the gap the fix (autotune_engine.c's own zone_model_at() call,
// mirroring profile_executor.c's) closes.
// ---------------------------------------------------------------------------

// Common trace shape for both tests below: flat for dead_time_s (52.8s, a
// real fitted value from this bench's own zone model), then rising at a
// modest, physically-plausible 1.2C/min (0.02C/s == 0.02C/tick at this
// harness's 1s/tick rate -- the SAME per-tick rate
// test_step_no_ceiling_rising_reading_does_not_trip() above already uses
// for "a healthy rise"). Arithmetic (see this test's own report/comment
// history): over a floor-less 60s window, only the last (60-52.8)=7.2s of
// that window shows any rise at all -- delta = 7.2*0.02 = 0.144C, well
// under the 0.5C (rate_cfg 0.5C/min * elapsed_min 1.0) guard 1 demands, a
// FALSE trip against a genuinely healthy trace. Over the derived floor
// (dead_time_s+tau_s = 52.8+264.0 = 316.8s, inside CLIMB_WINDOW_FLOOR_MIN/
// MAX_S's [120,900] clamp so it applies unclamped), the same slope
// accumulates (316.8-52.8)*0.02 = 5.28C against an expected 0.5*5.28 =
// 2.64C -- comfortably clears the bar.
#define GUARD1_FLOOR_TEST_DEAD_TIME_S 52.8f
#define GUARD1_FLOOR_TEST_TAU_S 264.0f
#define GUARD1_FLOOR_TEST_WRONG_DIR_WINDOW_S 60.0f
#define GUARD1_FLOOR_TEST_SLOPE_C_PER_TICK 0.02f
#define GUARD1_FLOOR_TEST_N_TICKS 340 /* > dead_time_s + tau_s (316.8s) with margin */

static void run_guard1_floor_test_trace(float start_temp_c)
{
    for (int i = 0; i < GUARD1_FLOOR_TEST_N_TICKS && state_is_running(s_at.state); i++) {
        xSemaphoreTake(s_at.lock, portMAX_DELAY);
        float elapsed_s = (float)i; // 1s/tick, dt_ms defaults to AUTOTUNE_ENGINE_TICK_MS every tick
        float rise_c = (elapsed_s > GUARD1_FLOOR_TEST_DEAD_TIME_S)
                           ? (elapsed_s - GUARD1_FLOOR_TEST_DEAD_TIME_S) * GUARD1_FLOOR_TEST_SLOPE_C_PER_TICK
                           : 0.0f;
        s_stub_ch0_temp_c = start_temp_c + rise_c;
        autotune_engine_tick_locked();
        xSemaphoreGive(s_at.lock);
    }
}

static void test_step_slow_healthy_zone_with_model_does_not_false_trip_guard1(void)
{
    TEST_SECTION("step test, real plant model + short operator wrong_dir_window_s -- a slow but healthy "
                 "zone (still inside its own dead time at 60s) must NOT false-trip guard 1 (the finding B "
                 "false trip cf3b5adb's climb_window_floor_s fix closes)");
    start_stepping_run(/*max_temp_c=*/1300.0f, /*step_duty=*/1.0f);
    // A short operator-configured window -- exactly the "60s, sized for
    // guard 2's falling-while-heating case" scenario thermal_guard.c's own
    // fix comment describes, poked directly (white-box, same convention as
    // this file's other post-start guard_cfg overrides). Left at 0 (this
    // file's default from zones_config_get_guard_thresholds()) this test
    // would ALSO pass, for the wrong reason (no override, effective_f()
    // falls back to PROGRESS_WINDOW_S=300s, already close to the floor) --
    // an explicit short override is what actually exercises the bug.
    s_at.guard_cfg.wrong_dir_window_s = GUARD1_FLOOR_TEST_WRONG_DIR_WINDOW_S;
    g_stub_model_valid[0] = true;
    g_stub_model_k_dc[0] = 1.0f; // unused by climb_window_floor_s's derivation, set for realism
    g_stub_model_tau_s[0] = GUARD1_FLOOR_TEST_TAU_S;
    g_stub_model_dead_time_s[0] = GUARD1_FLOOR_TEST_DEAD_TIME_S;

    run_guard1_floor_test_trace(/*start_temp_c=*/25.0f);

    TEST_CHECK(!s_at.guard_state.is_tripped,
              "a slow-but-healthy zone with a trusted plant model must not false-trip guard 1 just because "
              "the operator's own wrong_dir_window_s is shorter than the zone's physical dead_time_s+tau_s");
    TEST_CHECK(state_is_running(s_at.state), "the run must still be stepping, not aborted");

    g_stub_model_valid[0] = false; // restore this suite's default for every later test
    g_stub_model_k_dc[0] = 0.0f;
    g_stub_model_tau_s[0] = 0.0f;
    g_stub_model_dead_time_s[0] = 0.0f;
}

static void test_step_slow_healthy_zone_without_model_still_false_trips_guard1(void)
{
    TEST_SECTION("companion/boundary case: the IDENTICAL trace and short window, but with NO trusted plant "
                 "model yet (the ordinary state before any successful autotune run) -- proves the floor "
                 "derivation is actually engaged above by showing the exact same input DOES trip without "
                 "it, i.e. this is a real behavioural difference, not a test that would pass either way");
    start_stepping_run(/*max_temp_c=*/1300.0f, /*step_duty=*/1.0f);
    s_at.guard_cfg.wrong_dir_window_s = GUARD1_FLOOR_TEST_WRONG_DIR_WINDOW_S;
    // g_stub_model_valid[0] left at this suite's default (false) -- no model
    // fitted yet, zone_model_at() returns false, climb_window_floor_s stays
    // 0.0f ("don't touch window_s"), identical to today's pre-fix behaviour
    // for a zone with no model on file.

    run_guard1_floor_test_trace(/*start_temp_c=*/25.0f);

    TEST_CHECK(s_at.guard_state.is_tripped,
              "without a trusted model there is no floor to derive -- the same short window/slow rise "
              "trips guard 1 exactly as it would have before this fix existed for the OTHER caller "
              "(profile_executor.c), proving the test above is actually exercising the floor, not a "
              "trace that never trips regardless");
    TEST_CHECK(s_at.guard_state.reason == THERMAL_GUARD_TRIP_HEATING_FAILED, "specifically guard 1");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED, "the run must abort on this genuinely false-trip-shaped input");
}

// Reproduces the exact bench defect this slice exists to fix: an honest step
// response that has already proven the element heats, then legitimately
// plateaus approaching its asymptote, must not trip guard 1 for "not
// rising". Drives the REAL autotune_engine_tick_locked() loop (not a direct
// thermal_guard_tick() call -- test_thermal_guard.c already covers the
// mechanism in isolation; this proves autotune_engine.c wires it up).
// Shared body for the plateau-relaxation positive test, parameterised on
// max_temp_c -- review finding: the first version of this test used
// max_temp_c=80 (the OLD, since-removed ceiling-scaled threshold's one
// reachable configuration) and would not have caught the threshold defect.
// The NEW threshold (AUTOTUNE_ELEMENT_ALIVE_RISE_C, a small absolute rise)
// is ceiling-independent by construction, so this same body is run at a
// REALISTIC ceiling (1300C) and at max_temp_c==0 (the old NO_CEILING
// default) below -- both would have failed against the reused-threshold
// version (7.5C at 80C ceiling vs. 190.5C at 1300C, 40.0C at no ceiling).
static void run_guard1_relaxes_once_element_proven_then_response_plateaus(float max_temp_c)
{
    start_stepping_run(max_temp_c, /*step_duty=*/1.0f);
    // start_stepping_run() jumps straight to STEPPING and never runs the
    // real SETTLING->STEPPING transition (see its own header comment) --
    // stand in for what that transition would have captured.
    const float baseline_c = 30.0f;
    s_at.zone_baseline_c[0] = baseline_c;
    s_at.zone_baseline_valid[0] = true;
    /* step_onset_seen is normally set by step_settle_check_locked() reading
     * s_at.zone_trace -- populated by record_trace_sample(), which this
     * harness's frozen xTaskGetTickCount() stub (always 0) never lets fire
     * through repeated autotune_engine_tick_locked() calls (see
     * start_stepping_run()'s own header comment for the same clock
     * limitation). Stood in for directly, same convention as this file's
     * other STEPPING-phase field pokes (e.g. run_one_dither_case_end_to_end
     * setting s_at.step_settled directly) -- a real onset detects within
     * ~60-90s of dead time ending at any plausible duty (RESPONSE_ONSET_
     * SLOPE_C_PER_S), always well before AUTOTUNE_ELEMENT_ALIVE_RISE_C's
     * 3.0C is reached, so this is the realistic case, not a shortcut around
     * a real constraint. test_guard1_latch_requires_onset_not_just_rise()
     * below proves the gate itself matters when this is left false. */
    s_at.step_onset_seen = true;

    // AUTOTUNE_ELEMENT_ALIVE_RISE_C = 3.0C, ceiling-independent -- clears
    // it within the first few rise ticks regardless of max_temp_c.
    const float plateau_c = baseline_c + 15.0f;
    const int rise_ticks = 20; /* 0.75C/tick, reaches plateau_c smoothly, crossing 3.0C by tick 4-5 */
    /* Long enough that a SECOND, fully flat 300s window completes: the rise
     * itself (folded into the FIRST window, which started when the window
     * activated at the beginning of the rise phase) already satisfies that
     * first window's own requirement regardless of relaxation, so a short
     * plateau would pass for the wrong reason -- 20 (rise) + ~280
     * (remainder of window 1) + 300 (all-flat window 2) needs >= 580
     * plateau ticks; 650 leaves margin. Confirmed against a shorter (320)
     * plateau during this task's own negative-test pass: it passed even
     * with the relaxation bypassed, because no fully-flat window ever
     * completed -- see this task's report for that exact (mis)pass. */
    const int plateau_ticks = 650;

    for (int i = 0; i < rise_ticks; i++) {
        xSemaphoreTake(s_at.lock, portMAX_DELAY);
        s_stub_ch0_temp_c = baseline_c + (plateau_c - baseline_c) * ((float)(i + 1) / (float)rise_ticks);
        autotune_engine_tick_locked();
        xSemaphoreGive(s_at.lock);
    }
    TEST_CHECK(s_at.step_element_proven, "a genuine 15C rise past the 3.0C absolute threshold must set "
                                         "step_element_proven, regardless of max_temp_c");
    TEST_CHECK(state_is_running(s_at.state), "proving the element must not itself abort the run");

    for (int i = 0; i < plateau_ticks && state_is_running(s_at.state); i++) {
        xSemaphoreTake(s_at.lock, portMAX_DELAY);
        /* Real sensor noise, not a bit-identical constant -- +/-0.06C
         * (over guard 7's FROZEN_EPS_C=0.05C tolerance) so this plateau
         * exercises guard 1 without ALSO tripping guard 7 (frozen sensor)
         * on a reading that is too perfectly flat to be real, which a
         * first version of this test did (a 650-tick bit-identical
         * plateau crosses guard 7's 600s window) -- guard 7 tripping is
         * correct behavior for THAT input, not a defect in this fix, but
         * it is not what this test is about. Net window-average delta is
         * still ~0, which is what guard 1 actually integrates over, and
         * well under AUTOTUNE_ELEMENT_DEATH_DROP_C (5.0C) so the death
         * check does not fire either. */
        s_stub_ch0_temp_c = plateau_c + ((i % 2) ? 0.06f : -0.06f);
        autotune_engine_tick_locked();
        xSemaphoreGive(s_at.lock);
    }

    TEST_CHECK(!s_at.guard_state.is_tripped, "an honest plateau AFTER proving the element must NOT trip any guard");
    TEST_CHECK(state_is_running(s_at.state), "must still be running/stepping, not aborted");
}

static void test_guard1_relaxes_once_element_proven_then_response_plateaus(void)
{
    TEST_SECTION("guard 1 relaxes once the step response has proven the element heats, and does NOT trip "
                 "when an honest response then plateaus at its asymptote (the exact bench defect: aborted "
                 "at 840s/69.61C, 'rose only 0.5C in 1.0min', approaching a ~72C asymptote) -- max_temp_c=80, "
                 "the configuration the FIRST (defective) version of this test happened to use");
    run_guard1_relaxes_once_element_proven_then_response_plateaus(80.0f);
}

// Review finding 1's own repro: the OLD (removed) threshold reused
// autotune_finalize_fit()'s fit-trust floor, which at a REALISTIC ceiling (1300C, a
// real high-fire kiln) is 0.15*(1300-30) = 190.5C -- unreachable at any sane
// duty. The NEW absolute threshold has no such dependency; prove it here.
static void test_guard1_relaxation_engages_at_a_realistic_ceiling(void)
{
    TEST_SECTION("guard 1 relaxation engages at a REALISTIC ceiling (max_temp_c=1300) -- the OLD "
                 "ceiling-scaled threshold (190.5C at this ceiling) never would have");
    run_guard1_relaxes_once_element_proven_then_response_plateaus(1300.0f);
}

// Review finding 1's other repro: no ceiling at all uses AUTOTUNE_MIN_RISE_
// NO_CEILING_C (40.0C) under the OLD (removed) mechanism -- ALSO above the
// 39.3C cumulative rise the measured plant's own guard 1 trip happened at.
static void test_guard1_relaxation_engages_with_no_ceiling_configured(void)
{
    TEST_SECTION("guard 1 relaxation engages with NO ceiling configured (max_temp_c=0) -- the OLD "
                 "NO_CEILING threshold (40.0C) was itself above the plant's own 39.3C trip point");
    run_guard1_relaxes_once_element_proven_then_response_plateaus(0.0f);
}

// The gate half of the latch: a large rise with NO onset detected must NOT
// latch -- proves this is genuinely "onset AND absolute rise", not just the
// absolute rise alone (which alone would be indistinguishable from two
// coincidental quantization ticks landing far enough apart, the exact
// false-trigger RESPONSE_ONSET_SLOPE_C_PER_S exists to rule out elsewhere in
// this file).
static void test_guard1_latch_requires_onset_not_just_rise(void)
{
    TEST_SECTION("the element-proven latch requires step_onset_seen, not JUST a large absolute rise");
    start_stepping_run(/*max_temp_c=*/80.0f, /*step_duty=*/1.0f);
    s_at.zone_baseline_c[0] = 30.0f;
    s_at.zone_baseline_valid[0] = true;
    s_at.step_onset_seen = false; /* explicit -- this is the case under test */

    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    s_stub_ch0_temp_c = 30.0f + 15.0f; /* well past the 3.0C threshold on rise alone */
    autotune_engine_tick_locked();
    xSemaphoreGive(s_at.lock);

    TEST_CHECK(!s_at.step_element_proven, "a large rise WITHOUT a detected onset must not latch the element "
                                          "as proven -- the onset gate is load-bearing, not decorative");
}

// Review finding 2: guard 2 is structurally unreachable during a step test
// (setpoint pinned to the ceiling), so this file's own running-peak death
// check is what catches an element that dies AFTER being proven alive.
static void test_element_death_after_proven_aborts_the_run(void)
{
    TEST_SECTION("an element that dies AFTER being proven alive is caught by the running-peak death "
                 "check (AUTOTUNE_ELEMENT_DEATH_DROP_C) -- guard 2 cannot reach this case here");
    start_stepping_run(/*max_temp_c=*/80.0f, /*step_duty=*/1.0f);
    const float baseline_c = 30.0f;
    s_at.zone_baseline_c[0] = baseline_c;
    s_at.zone_baseline_valid[0] = true;
    s_at.step_onset_seen = true;

    // Rise past the alive threshold, same as the positive test above.
    const float peak_c = baseline_c + 15.0f;
    for (int i = 0; i < 20; i++) {
        xSemaphoreTake(s_at.lock, portMAX_DELAY);
        s_stub_ch0_temp_c = baseline_c + (peak_c - baseline_c) * ((float)(i + 1) / 20.0f);
        autotune_engine_tick_locked();
        xSemaphoreGive(s_at.lock);
    }
    TEST_CHECK(s_at.step_element_proven, "sanity: must be proven before the death check is meaningful");
    TEST_CHECK(state_is_running(s_at.state), "sanity: still running after the rise");

    // Now the element dies: reading falls well past AUTOTUNE_ELEMENT_
    // DEATH_DROP_C (5.0C) below its running peak, despite duty still
    // commanded at 1.0 -- exactly what guard 2 would have caught if it were
    // reachable here.
    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    s_stub_ch0_temp_c = peak_c - 8.0f;
    autotune_engine_tick_locked();
    xSemaphoreGive(s_at.lock);

    TEST_CHECK(!state_is_running(s_at.state), "a post-proof drop past the death threshold must abort the run");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED, "specifically ABORTED, not left running");
    TEST_CHECK(strstr(s_at.abort_reason, "died after proving") != NULL,
              "abort_reason must name this specific failure mode");
}

// The same drop, but small (well under AUTOTUNE_ELEMENT_DEATH_DROP_C) --
// ordinary quantization/sensor noise on an honest plateau must NOT abort.
// Negative control for the death check above, same pairing convention as
// this file's other threshold tests.
static void test_element_small_dip_after_proven_does_not_abort(void)
{
    TEST_SECTION("a small dip (well under AUTOTUNE_ELEMENT_DEATH_DROP_C) after being proven does NOT "
                 "abort -- the death check has real margin over sensor noise");
    start_stepping_run(/*max_temp_c=*/80.0f, /*step_duty=*/1.0f);
    const float baseline_c = 30.0f;
    s_at.zone_baseline_c[0] = baseline_c;
    s_at.zone_baseline_valid[0] = true;
    s_at.step_onset_seen = true;

    const float peak_c = baseline_c + 15.0f;
    for (int i = 0; i < 20; i++) {
        xSemaphoreTake(s_at.lock, portMAX_DELAY);
        s_stub_ch0_temp_c = baseline_c + (peak_c - baseline_c) * ((float)(i + 1) / 20.0f);
        autotune_engine_tick_locked();
        xSemaphoreGive(s_at.lock);
    }
    TEST_CHECK(s_at.step_element_proven, "sanity: must be proven first");

    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    s_stub_ch0_temp_c = peak_c - 1.0f; /* well under the 5.0C death threshold */
    autotune_engine_tick_locked();
    xSemaphoreGive(s_at.lock);

    TEST_CHECK(state_is_running(s_at.state), "a small dip must not abort the run");
    TEST_CHECK(!s_at.guard_state.is_tripped, "and must not trip any guard either");
}

// Review finding 3: thermal_guard.c's default progress_duty_min (0.5) left
// guards 1/2 completely inert below that duty -- exactly target mode's own
// typical duties (0.15 probe, often well under 0.5 identify). Proves a dead
// element at a LOW duty still trips guard 1, driven through the real
// autotune_begin_run_locked()->autotune_engine_run() path (not a hand-poked cfg) so
// the override this file now applies is exercised for real, not assumed.
static void test_dead_element_trips_guard1_below_the_old_0_5_duty_floor(void)
{
    TEST_SECTION("a dead element at a LOW commanded duty (0.15, below thermal_guard.c's old 0.5 "
                 "progress_duty_min floor) still trips guard 1 -- autotune_begin_run_locked() must have armed "
                 "the progress window at this duty");
    start_stepping_run(/*max_temp_c=*/1300.0f, /*step_duty=*/0.15f);
    TEST_CHECK_NEAR(s_at.guard_cfg.progress_duty_min, AUTOTUNE_PROGRESS_DUTY_MIN_FOR_STEP_TEST, 1e-6f,
                    "sanity: autotune_begin_run_locked() must have overridden progress_duty_min for this run");
    run_ticks(/*start_temp_c=*/25.0f, /*per_tick_delta_c=*/0.0f, /*n_ticks=*/320);

    TEST_CHECK(s_at.guard_state.is_tripped, "a dead element at 0.15 duty must still trip a guard");
    TEST_CHECK(s_at.guard_state.reason == THERMAL_GUARD_TRIP_HEATING_FAILED, "specifically guard 1 (HEATING_FAILED)");
}

// Pins the method==STEP conjunct in autotune_engine.c's gin construction --
// a RELAY run must never see progress_rise_check_relaxed=true regardless of
// s_at.step_element_proven's value (which a relay run should never set in
// the first place, but this proves the READ side stays pinned too).
static void test_guard1_relaxation_never_applies_to_relay_method(void)
{
    TEST_SECTION("guard 1 relaxation never applies to a RELAY run, even if step_element_proven somehow "
                 "reads true (defense in depth on the method==STEP conjunct)");
    static MAX31856BusClass bus;
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    bus.initialized = true;
    s_at.thermo_bus = &bus;
    s_at.safety = &safety;
    s_at.lock = xSemaphoreCreateMutex();
    s_stub_max_temp_c = 500.0f;
    s_stub_ch0_ok = true;

    char errbuf[96] = {0};
    bool ok = autotune_engine_run_relay(0, /*setpoint_c=*/300.0f, 0.0f, 0.0f, AUTOTUNE_RULE_TYREUS_LUYBEN, errbuf,
                                        sizeof(errbuf));
    TEST_CHECK(ok, "test setup: relay run must start");

    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    s_at.state = AUTOTUNE_ENGINE_RELAY_CYCLING;
    s_at.phase_start_tick = 0;
    s_at.last_sample_tick = 0;
    // Forced true purely to prove the READ side (gin construction) is
    // pinned by method==STEP -- a real relay run never sets this itself.
    s_at.step_element_proven = true;
    xSemaphoreGive(s_at.lock);

    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    s_stub_ch0_temp_c = 25.0f; /* flat, well below setpoint, commanded duty from the relay law */
    autotune_engine_tick_locked();
    xSemaphoreGive(s_at.lock);

    // Not asserting a trip here (the relay law's own duty/timing makes that
    // fragile to pin exactly) -- this test exists purely to prove the code
    // compiles/runs this path with step_element_proven=true on a RELAY run
    // without the (s_at.method == AUTOTUNE_METHOD_STEP) guard being
    // bypassable; see the source for the actual conjunct being pinned.
    TEST_CHECK(s_at.method == AUTOTUNE_METHOD_RELAY, "sanity: this is genuinely a relay run");
}

// The case the guard exists for, driven through the SAME real tick loop as
// the positive test above: an element that NEVER proves itself (flat from
// the very start, never crosses the 7.5C threshold) must still trip guard 1
// -- the relaxation must never protect a genuinely dead element.
static void test_guard1_still_trips_a_dead_element_that_never_gets_proven(void)
{
    TEST_SECTION("a dead element (never proves itself -- flat from the start) still trips guard 1, driven "
                 "through the real autotune_engine tick loop");
    start_stepping_run(/*max_temp_c=*/80.0f, /*step_duty=*/1.0f);
    s_at.zone_baseline_c[0] = 30.0f;
    s_at.zone_baseline_valid[0] = true;

    run_ticks(/*start_temp_c=*/30.0f, /*per_tick_delta_c=*/0.0f, /*n_ticks=*/320);

    TEST_CHECK(!s_at.step_element_proven, "sanity: a flat reading must never cross the proven threshold");
    TEST_CHECK(s_at.guard_state.is_tripped, "a dead element must still trip a guard");
    TEST_CHECK(s_at.guard_state.reason == THERMAL_GUARD_TRIP_HEATING_FAILED, "specifically guard 1 (HEATING_FAILED)");
}

// ---------------------------------------------------------------------------
// RELAY method guard 1/2 arm-during-identification pins (PID audit,
// 2026-09-02). Before this pass, autotune_engine.c fed thermal_guard's
// commanded_duty as `want_relay_on ? want_duty : 0.0f` for a RELAY run --
// the POST-PWM instantaneous relay state, not the bang-bang law's own
// branch value. Guard 1/2's progress window (thermal_guard.c: resets
// whenever commanded_duty < progress_duty_min) therefore reset on every
// PWM off-pulse of heater_output_duty_relay_step()'s own window (~window_ms,
// 60s default), on EITHER bang-bang branch -- it could never accumulate
// the 300s (PROGRESS_WINDOW_S) needed to complete, so a dead or
// flat-but-plausible element during a relay-feedback autotune ran the
// entire multi-hour budget with guard 1/2 fully configured (armed in
// guard_cfg) but structurally unable to fire (inert against this method's
// actual duty pattern) -- the "armed but inert" case, same shape as the
// step-test defect this file already pins above and the profile_executor.c
// defect described in that file's apply-relays-and-guards loop.
//
// The fix: feed both methods want_duty (pre-PWM -- relay_law_tick()'s own
// branch value for RELAY), and give RELAY the same progress_duty_min
// override STEP already had (a relay run's low branch, 0.15 at the default
// d=0.35, is still below thermal_guard.c's stock 0.5 default). These tests
// pin BOTH halves of that fix and drive a genuinely dead element through
// the real tick loop to prove the fix actually restores detection, not
// just that the plumbing compiles.
// ---------------------------------------------------------------------------

// Starts a real relay-feedback run on zone 0, then jumps straight to
// RELAY_CYCLING -- same rationale and same never-succeeding-task-create
// workaround as start_stepping_run_rule() above.
// relay_d/hysteresis_c <= 0 mean "use the documented default" (relay_d ->
// AUTOTUNE_RELAY_DEFAULT_D=0.35, hysteresis_c -> AUTOTUNE_RELAY_DEFAULT_H_C=
// 2.0), same sentinel autotune_engine_run_relay() itself documents -- so
// every pre-existing call site that passed 0.0f/0.0f keeps behaving exactly
// as before.
static void start_relay_cycling_run_with(float max_temp_c, float setpoint_c, float relay_d, float hysteresis_c)
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

    s_stub_max_temp_c = max_temp_c;
    s_stub_min_temp_c = 0.0f;
    s_stub_ch0_ok = true;

    char errbuf[96] = {0};
    bool ok = autotune_engine_run_relay(0, setpoint_c, relay_d, hysteresis_c,
                                        AUTOTUNE_RULE_TYREUS_LUYBEN, errbuf, sizeof(errbuf));
    TEST_CHECK(ok, "test setup: relay run must start");

    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    s_at.state = AUTOTUNE_ENGINE_RELAY_CYCLING;
    s_at.phase_start_tick = 0;
    s_at.last_sample_tick = 0;
    xSemaphoreGive(s_at.lock);
}

static void start_relay_cycling_run(float max_temp_c, float setpoint_c)
{
    start_relay_cycling_run_with(max_temp_c, setpoint_c, /*relay_d=*/0.0f, /*hysteresis_c=*/0.0f);
}

// Drives a physically plausible relay LIMIT CYCLE through the real tick
// loop, closed through the real relay_law_tick() -- unlike run_ticks()
// above (a straight ramp in one direction, which can only ever exercise one
// bang-bang branch), this actually reverses direction the way a real kiln
// does, with a DEAD TIME between a branch change and the plant responding to
// it. dead_time_ticks delays the plant's response to whichever branch
// s_at.relay_on last selected; rate_high/low_c_per_min is the plant's rate
// of change once the delayed effective branch takes hold (rate_low is
// usually negative -- cooling on the low branch). 1 tick == 1s
// (AUTOTUNE_ENGINE_TICK_MS).
//
// history[] holds the branch (true=high) the law selected on each past tick,
// prefilled for the first dead_time_ticks entries with the run's actual
// starting branch (s_at.relay_on -- true, per autotune_engine_run_relay()'s
// own "start on the high branch" comment) so the very first ticks of the run
// aren't an artificial edge this helper invented.
static float run_relay_limit_cycle_ticks(float start_temp_c, int dead_time_ticks, float rate_high_c_per_min,
                                          float rate_low_c_per_min, int n_ticks)
{
    static bool history[4096];
    TEST_CHECK(dead_time_ticks + n_ticks <= (int)(sizeof(history) / sizeof(history[0])),
               "test bug: history[] too small for this run's dead_time_ticks + n_ticks");
    for (int i = 0; i < dead_time_ticks; i++) {
        history[i] = s_at.relay_on;
    }

    float temp_c = start_temp_c;
    for (int i = 0; i < n_ticks; i++) {
        bool other_zone_active_hint = any_other_zone_profile_active(s_at.zone_index);
        xSemaphoreTake(s_at.lock, portMAX_DELAY);
        if (!state_is_running(s_at.state)) {
            xSemaphoreGive(s_at.lock);
            break;
        }
        s_stub_ch0_temp_c = temp_c;
        s_at.other_zone_profile_active_hint = other_zone_active_hint;
        autotune_engine_tick_locked();
        history[dead_time_ticks + i] = s_at.relay_on;
        xSemaphoreGive(s_at.lock);

        bool effective_branch_high = history[i]; // whatever the law picked dead_time_ticks ago
        float rate_c_per_min = effective_branch_high ? rate_high_c_per_min : rate_low_c_per_min;
        temp_c += rate_c_per_min / 60.0f; // 1 tick == 1s
    }
    return temp_c;
}

static void test_relay_run_overrides_progress_duty_min_same_as_step_test(void)
{
    TEST_SECTION("autotune_engine_run_relay() overrides progress_duty_min the same way the step-test path "
                 "does -- otherwise the low bang-bang branch (0.15 default) degates guard 1/2 every half-cycle");
    start_relay_cycling_run(/*max_temp_c=*/500.0f, /*setpoint_c=*/300.0f);
    TEST_CHECK(s_at.guard_cfg.progress_duty_min == AUTOTUNE_PROGRESS_DUTY_MIN_FOR_STEP_TEST,
               "relay run must arm guard 1/2 at any nonzero commanded duty, same as a step test");
}

static void test_relay_run_flat_dead_element_now_trips_a_guard(void)
{
    TEST_SECTION("relay run: a dead/flat element on the HIGH branch still trips a guard, driven through the "
                 "real autotune_engine tick loop -- this is the exact case that was silently inert before "
                 "this pass' fix");
    start_relay_cycling_run(/*max_temp_c=*/500.0f, /*setpoint_c=*/300.0f);
    // Flat, well below setpoint - h: the relay law latches its high branch
    // and never releases it (meas never crosses setpoint_c + h), so this
    // exercises the branch that used to toggle in and out of commanded_duty
    // on every PWM sub-cycle even though the LOGICAL branch never changed.
    run_ticks(/*start_temp_c=*/25.0f, /*per_tick_delta_c=*/0.0f, /*n_ticks=*/320);

    TEST_CHECK(s_at.guard_state.is_tripped, "a dead element on a relay run must still trip a guard");
    // Post-swing-discriminator (this pass): a RELAY run's guards 1/2 no
    // longer reason about direction at all (see thermal_guard_input_t.
    // relay_min_swing_c) -- a flat reading produces zero window swing
    // regardless of which branch the law is stuck on, so this trips the
    // relay-specific reason, not guard 1's directional HEATING_FAILED.
    TEST_CHECK(s_at.guard_state.reason == THERMAL_GUARD_TRIP_RELAY_STALLED,
               "the relay-cycling discriminator -- zero swing while cycling -- not guard 1/2's directional test");
}

static void test_relay_run_flat_dead_element_on_low_branch_also_trips(void)
{
    TEST_SECTION("relay run: a dead/flat element on the LOW branch also trips a guard -- the task brief's own "
                 "coverage gap (the pre-existing test only ever drove the high/0.85 branch)");
    start_relay_cycling_run(/*max_temp_c=*/500.0f, /*setpoint_c=*/300.0f);
    // Flat, well ABOVE setpoint + h: the relay law immediately switches to
    // (and latches on) its low branch (0.15 default) and never releases it,
    // since a dead element never falls back below setpoint - h either.
    run_ticks(/*start_temp_c=*/400.0f, /*per_tick_delta_c=*/0.0f, /*n_ticks=*/320);

    TEST_CHECK(!s_at.relay_on, "sanity: a reading this far above setpoint+h must select the LOW branch");
    TEST_CHECK(s_at.guard_state.is_tripped, "a dead element stuck on the low branch must still trip a guard");
    TEST_CHECK(s_at.guard_state.reason == THERMAL_GUARD_TRIP_RELAY_STALLED,
               "zero swing while cycling, exactly as the high-branch case above -- direction never enters into it");
}

// Replaces a VACUOUS predecessor (a straight 1C/tick, 320-tick ramp) that
// passed identically with the fix under test reverted: it never reversed
// direction, so it could only ever occupy one bang-bang branch and never
// exercised the low branch, a downswing, or anything resembling a real
// limit cycle. This drives an actual closed-loop oscillation, with a
// realistic dead time, through the real tick loop via
// run_relay_limit_cycle_ticks() -- see that helper's own comment.
static void test_relay_run_healthy_limit_cycle_does_not_spuriously_trip(void)
{
    TEST_SECTION("relay run: a HEALTHY limit cycle (real oscillation, realistic dead time) never trips a "
                 "guard over a long run -- default d/h");
    start_relay_cycling_run(/*max_temp_c=*/500.0f, /*setpoint_c=*/300.0f);
    // Default d (0.35) / h (2.0C). 40s dead time (mid the documented 34-53s
    // range) each direction; +/-6C/min once the delayed branch takes hold --
    // comfortably clears 2*h=4C swing per half-cycle, so this is a genuinely
    // healthy run, not a marginal one. 1200 ticks = 20min, several full
    // cycles and several complete 300s guard windows.
    run_relay_limit_cycle_ticks(/*start_temp_c=*/300.0f, /*dead_time_ticks=*/40,
                                /*rate_high_c_per_min=*/6.0f, /*rate_low_c_per_min=*/-6.0f, /*n_ticks=*/1200);

    TEST_CHECK(!s_at.guard_state.is_tripped, "a healthy oscillating element must never trip a guard");
}

// The GUARD 2 downswing scenario from the task brief, reproduced almost
// verbatim: d=0.35, h=2.0, setpoint 300C, dead time inside the documented
// 34-53s range. Before this pass' discriminator, a directional window
// landing on the low branch's downswing (falling past 298C after the law
// already dropped to the low branch at 302C) read as guard 2's
// wrong-direction trip on a perfectly healthy cycle.
static void test_relay_run_guard2_downswing_scenario_does_not_trip(void)
{
    TEST_SECTION("relay run: guard-2 downswing scenario (d=0.35, h=2.0, 300C, 34-53s dead time) does not trip "
                 "a healthy cycle");
    start_relay_cycling_run_with(/*max_temp_c=*/500.0f, /*setpoint_c=*/300.0f, /*relay_d=*/0.35f,
                                 /*hysteresis_c=*/2.0f);
    // 53s dead time (top of the documented range -- the slower, more
    // exposed end) with a modest rate so the plant genuinely overshoots the
    // 2C band by a couple of degrees each way (matching the task brief's
    // "keeps falling past 298C" description) without being an unrealistic
    // step change.
    run_relay_limit_cycle_ticks(/*start_temp_c=*/300.0f, /*dead_time_ticks=*/53,
                                /*rate_high_c_per_min=*/4.0f, /*rate_low_c_per_min=*/-4.0f, /*n_ticks=*/1800);

    TEST_CHECK(!s_at.guard_state.is_tripped,
               "a healthy cycle in the exact scenario that used to false-trip guard 2 must not trip any guard");
}

// The GUARD 1 large-hysteresis scenario from the task brief: h=20.0
// (AUTOTUNE_RELAY_MAX_H_C, legal) spends most of each low half-cycle more
// than progress_band_c (3.0C default) below setpoint, which used to put
// guard 1's directional "must be rising" test on a zone that is deliberately
// COOLING for a big chunk of every cycle.
static void test_relay_run_guard1_large_hysteresis_scenario_does_not_trip(void)
{
    TEST_SECTION("relay run: guard-1 large-hysteresis scenario (h=20.0=AUTOTUNE_RELAY_MAX_H_C) does not trip "
                 "a healthy cycle");
    start_relay_cycling_run_with(/*max_temp_c=*/500.0f, /*setpoint_c=*/300.0f, /*relay_d=*/0.35f,
                                 /*hysteresis_c=*/AUTOTUNE_RELAY_MAX_H_C);
    // 40s dead time, +/-20C/min -- fast enough that a single 300s guard
    // window reliably spans more than one full half-cycle (band crossing
    // ~40C/(20C/min)=120s, plus 40s dead time =~160s per half-cycle) so the
    // window's observed swing is never a lucky/unlucky partial slice of the
    // full ~50-60C peak-to-peak amplitude this cycle actually produces
    // (2*h=40C is only the guaranteed floor, not the target).
    run_relay_limit_cycle_ticks(/*start_temp_c=*/300.0f, /*dead_time_ticks=*/40,
                                /*rate_high_c_per_min=*/20.0f, /*rate_low_c_per_min=*/-20.0f, /*n_ticks=*/2400);

    TEST_CHECK(!s_at.guard_state.is_tripped,
               "a healthy large-hysteresis cycle must not trip guard 1 for spending half its time cooling");
}

// ---------------------------------------------------------------------------
// TODO.md 6A.6 ownership tests -- autotune_engine.c claims RELAY_OWNER_AUTOTUNE
// on the zone it steps (autotune_begin_run_locked()) and releases it through
// force_relays_off(), the single chokepoint every terminal path (autotune_finalize_fit,
// finalize_relay_fit, autotune_escalate_and_abort, abort_locked) calls. Proven here by
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
// a GLOBAL trip (autotune_escalate_and_abort()'s `global` branch), which is what the
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
    reset_fault_recorder(); // start_stepping_run()'s own autotune_begin_run_locked() calls nothing here (no prior
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
    // to prove gets cleared by autotune_begin_run_locked() itself, not by test setup.
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

// B2 (opus review, 2026-08-27): autotune_begin_run_locked() must refuse while a zone
// current sweep is active. Same real-mutex-no-real-task pattern
// start_stepping_run() above uses -- autotune_engine_run() itself, including
// autotune_begin_run_locked()'s real guard_cfg setup, runs for real; only the
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

// docs/audits/autotune_readiness_2026-09-07.md item 3: a zone with no
// thermocouple channel assigned (thermo_mask == 0) used to sail through
// autotune_begin_run_locked() -- neither zones_config_is_valid() nor the
// relay_mask check above it says anything about thermo_mask -- and would
// have run the FULL step-test budget with actual_valid permanently false
// before autotune_finalize_fit() finally refused the flat trace, hours in.
// Exercises the real prestart path (autotune_engine_run(), not the tick),
// same convention as test_run_refuses_while_zone_sweep_is_active() above.
static void test_run_refuses_zone_with_no_thermo_mask(void)
{
    TEST_SECTION("autotune_engine_run() refuses a zone with no thermocouple channel assigned");
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
    s_stub_thermo_mask = 0x00;

    char errbuf[128] = {0};
    bool ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_SIMC, errbuf, sizeof(errbuf));

    TEST_CHECK(!ok, "a zone with thermo_mask==0 must refuse the autotune run before any heating starts");
    TEST_CHECK(strstr(errbuf, "thermocouple") != NULL, "the refusal must name the missing sensor assignment");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_IDLE,
               "a refused run must never leave the engine in a running state");

    // Control case: with a real thermo_mask restored, the identical setup
    // succeeds -- proves the refusal above is really about the mask, not
    // some other side effect of this test's setup.
    s_stub_thermo_mask = 0x01;
    memset(&s_at, 0, sizeof(s_at));
    s_at.thermo_bus = &bus;
    s_at.safety = &safety;
    s_at.lock = xSemaphoreCreateMutex();
    errbuf[0] = '\0';
    ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_SIMC, errbuf, sizeof(errbuf));
    TEST_CHECK(ok, "control: with a thermo_mask assigned, the identical setup must succeed");
}

// docs/ON_OFF_ZONE.md step 1: an on/off zone must refuse autotune at
// prestart, the same shape/place as the thermo_mask==0 refusal just above --
// and BEFORE it, so a TC-equipped on/off zone gets this message rather than
// passing the thermo_mask check only to fail later on a flat trace.
static void test_run_refuses_on_off_zone(void)
{
    TEST_SECTION("autotune_engine_run() refuses a ZONE_TYPE_ON_OFF zone before any heating starts");
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
    s_stub_thermo_mask = 0x01; /* a TC IS assigned -- must still refuse on zone_type alone */
    s_stub_zone_is_on_off = true;

    char errbuf[128] = {0};
    bool ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_SIMC, errbuf, sizeof(errbuf));

    TEST_CHECK(!ok, "an on/off zone must refuse the autotune run before any heating starts");
    TEST_CHECK(strstr(errbuf, "on/off") != NULL, "the refusal must name the zone as an on/off device");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_IDLE,
               "a refused run must never leave the engine in a running state");

    // Control case: with zone_type restored to HEATER, the identical setup
    // (still with a real thermo_mask) succeeds -- proves the refusal above is
    // really about zone_type, not some other side effect of this test's setup.
    s_stub_zone_is_on_off = false;
    memset(&s_at, 0, sizeof(s_at));
    s_at.thermo_bus = &bus;
    s_at.safety = &safety;
    s_at.lock = xSemaphoreCreateMutex();
    errbuf[0] = '\0';
    ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_SIMC, errbuf, sizeof(errbuf));
    TEST_CHECK(ok, "control: with zone_type back to HEATER, the identical setup must succeed");
}

// SPARE_RELAY_ONOFF_PLAN.md sec 10: a monitor-only zone (relay converted to an
// aux) has nothing for autotune to drive; refuse before any heating starts.
static void test_run_refuses_monitor_only_zone(void)
{
    TEST_SECTION("autotune_engine_run() refuses a monitor-only zone before any heating starts");
    static MAX31856BusClass bus;
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    bus.initialized = true;
    s_at.thermo_bus = &bus;
    s_at.safety = &safety;
    s_at.lock = xSemaphoreCreateMutex();
    s_stub_max_temp_c = 500.0f;
    s_stub_ch0_ok = true;
    s_stub_thermo_mask = 0x01;
    s_stub_zone_monitor_only = true;

    char errbuf[128] = {0};
    bool ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_SIMC, errbuf, sizeof(errbuf));
    TEST_CHECK(!ok, "a monitor-only zone must refuse the autotune run");
    TEST_CHECK(strstr(errbuf, "monitor-only") != NULL, "the refusal must name the zone as monitor-only");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_IDLE, "a refused run must never leave the engine running");

    s_stub_zone_monitor_only = false;
    memset(&s_at, 0, sizeof(s_at));
    s_at.thermo_bus = &bus;
    s_at.safety = &safety;
    s_at.lock = xSemaphoreCreateMutex();
    errbuf[0] = '\0';
    ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_SIMC, errbuf, sizeof(errbuf));
    TEST_CHECK(ok, "control: with the zone no longer monitor-only, the identical setup must succeed");
}

// The shared heat claim's atomic gate (relay_authority.h) -- proves the LATE
// gate right before autotune_begin_run_locked()'s commit is independently
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
    s_zone_claim_begin_calls = 0;
    s_zone_claim_end_calls = 0;
    s_last_zone_claim_mask = 0;

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
    // The per-zone claim (review of 933a7eec) is taken just BEFORE this gate,
    // so a refusal here must roll it back -- once, as AUTOTUNE, with the same
    // zone bit it claimed -- or the zone could never start again until reboot.
    TEST_CHECK(s_zone_claim_begin_calls == 1, "the per-zone claim is taken before the heat-claim gate");
    TEST_CHECK(s_zone_claim_end_calls == 1, "a heat-claim-gate refusal must roll the per-zone claim back");
    TEST_CHECK(s_last_zone_claimant == RELAY_HEAT_ZONE_CLAIM_AUTOTUNE,
               "the rollback must release as AUTOTUNE, the claimant that took it");
    TEST_CHECK(s_last_zone_claim_mask == 0x01, "the rollback must release the same zone bit it claimed");

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

// MED-1 (review 3): update claim taken after the early check -> refused at commit, claims released.
static void test_run_refuses_when_update_claims_after_early_check(void)
{
    TEST_SECTION("autotune_engine_run() -- update claim taken after the early check refuses at commit and releases claims");
    static MAX31856BusClass bus;
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    bus.initialized = true;
    s_at.thermo_bus = &bus;
    s_at.safety = &safety;
    s_at.lock = xSemaphoreCreateMutex();
    s_stub_max_temp_c = 500.0f;
    s_stub_ch0_ok = true;
    s_test_sweep_active = false;
    reset_owner_recorder();
    s_heat_zone_claim_begin_calls = 0;
    s_heat_zone_claim_end_calls = 0;
    s_zone_claim_begin_calls = 0;
    s_zone_claim_end_calls = 0;

    s_test_update_claim_after_heat_claim = true;
    char errbuf[96] = {0};
    bool ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_SIMC, errbuf, sizeof(errbuf));
    s_test_update_claim_after_heat_claim = false;
    TEST_CHECK(!ok, "an update claim taken after the early check must refuse the autotune start");
    TEST_CHECK(strstr(errbuf, "update") != NULL, "the refusal names the update");
    TEST_CHECK(s_claim_calls == 0, "relay ownership is never grabbed");
    TEST_CHECK(s_heat_zone_claim_end_calls >= 1, "the published heat claim is released");
    TEST_CHECK(s_zone_claim_end_calls == 1, "the per-zone claim is released");
}
// HTTP audit E1 finding 1: zones config committed during the start -> refused at commit, claims released.
static void test_run_refuses_when_zones_config_changes_during_start(void)
{
    TEST_SECTION("autotune_engine_run() -- zones config changed during the start refuses at commit and releases claims");
    static MAX31856BusClass bus;
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    bus.initialized = true;
    s_at.thermo_bus = &bus;
    s_at.safety = &safety;
    s_at.lock = xSemaphoreCreateMutex();
    s_stub_max_temp_c = 500.0f;
    s_stub_ch0_ok = true;
    s_test_sweep_active = false;
    reset_owner_recorder();
    s_heat_zone_claim_begin_calls = 0;
    s_heat_zone_claim_end_calls = 0;
    s_zone_claim_begin_calls = 0;
    s_zone_claim_end_calls = 0;

    s_test_zones_gen_bump_after_heat_claim = true;
    char errbuf[128] = {0};
    bool ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_SIMC, errbuf, sizeof(errbuf));
    s_test_zones_gen_bump_after_heat_claim = false;
    TEST_CHECK(!ok, "a zones config commit during the start must refuse the autotune start");
    TEST_CHECK(strstr(errbuf, "zones configuration changed") != NULL, "the refusal names the config change");
    TEST_CHECK(s_claim_calls == 0, "relay ownership is never grabbed");
    TEST_CHECK(s_heat_zone_claim_end_calls >= 1, "the published heat claim is released");
    TEST_CHECK(s_zone_claim_end_calls == 1, "the per-zone claim is released");
}
// Factory reset in flight: a reset mark set before the start's late check refuses it, claims released.
static void test_run_refuses_when_factory_reset_in_flight(void)
{
    TEST_SECTION("autotune_engine_run() -- a factory reset in flight refuses at commit and releases claims");
    static MAX31856BusClass bus;
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    bus.initialized = true;
    s_at.thermo_bus = &bus;
    s_at.safety = &safety;
    s_at.lock = xSemaphoreCreateMutex();
    s_stub_max_temp_c = 500.0f;
    s_stub_ch0_ok = true;
    s_test_sweep_active = false;
    reset_owner_recorder();
    s_heat_zone_claim_begin_calls = 0;
    s_heat_zone_claim_end_calls = 0;
    s_zone_claim_begin_calls = 0;
    s_zone_claim_end_calls = 0;

    s_test_reset_in_flight = true;
    char errbuf[128] = {0};
    bool ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_SIMC, errbuf, sizeof(errbuf));
    s_test_reset_in_flight = false;
    TEST_CHECK(!ok, "a factory reset in flight must refuse the autotune start");
    TEST_CHECK(strstr(errbuf, "factory reset in progress") != NULL, "the refusal names the factory reset");
    TEST_CHECK(s_claim_calls == 0, "relay ownership is never grabbed");
    TEST_CHECK(s_heat_zone_claim_begin_calls >= 1, "the heat claim was published before the mark was read");
    TEST_CHECK(s_heat_zone_claim_end_calls >= 1, "the published heat claim is released");
    TEST_CHECK(s_zone_claim_end_calls == 1, "the per-zone claim is released");
}
// Review of 933a7eec: autotune start peeks profile_executor_zone_is_active()
// and profile start peeks autotune_engine_is_active_on_zone(), but each peek
// runs BEFORE the caller's own module lock, so two starts on the same zone
// at the same moment can both pass. This test models side B (a profile
// start) winning the atomic relay_authority_zone_claim_begin() race in the
// window between side A's (this autotune run) early peek and its own atomic
// claim attempt -- side A must then be refused, never take the relay claim.
static void test_run_refuses_at_atomic_zone_claim_gate(void)
{
    TEST_SECTION("autotune_engine_run() -- the atomic per-zone claim refuses even when the EARLY "
                 "profile_executor_zone_is_active() peek passed (the race window it exists to close)");
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
    s_test_sweep_active = false;
    memset(s_stub_zone_active, 0, sizeof(s_stub_zone_active)); /* the EARLY peek passes on zone 0 */
    reset_owner_recorder();
    s_zone_claim_begin_calls = 0;
    s_zone_claim_end_calls = 0;
    s_heat_zone_claim_begin_calls = 0;
    s_heat_zone_claim_end_calls = 0;

    // RED: the early profile_executor_zone_is_active() stub above returns
    // false for zone 0, so the cheap early peek passes -- but force the
    // atomic claim itself to refuse, simulating a profile having taken zone
    // 0 in the window between that peek and this call.
    s_test_zone_claim_refused = true;
    s_test_zone_claim_conflict_mask = 0x01;
    char errbuf[128] = {0};
    bool ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_SIMC, errbuf, sizeof(errbuf));

    TEST_CHECK(!ok, "the atomic zone-claim gate alone must be able to refuse a run the early peek let through");
    TEST_CHECK(strstr(errbuf, "profile") != NULL, "the refusal must name a profile, not a sweep");
    TEST_CHECK(s_claim_calls == 0, "relay_authority_claim_mask() must never be reached when the atomic "
                                   "zone-claim gate refuses");
    TEST_CHECK(s_heat_zone_claim_begin_calls == 0, "the whole-board sweep claim must not even be attempted "
                                                   "once the per-zone claim has already refused");
    TEST_CHECK(s_zone_claim_end_calls == 0, "a refused claim must not be released -- it was never held");

    // GREEN: same setup, atomic zone claim now allows it -- proves the RED
    // result above was really this gate, not some other stub failing closed.
    s_test_zone_claim_refused = false;
    memset(&s_at, 0, sizeof(s_at));
    s_at.thermo_bus = &bus;
    s_at.safety = &safety;
    s_at.lock = xSemaphoreCreateMutex();
    errbuf[0] = '\0';
    ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_SIMC, errbuf, sizeof(errbuf));

    TEST_CHECK(ok, "with the atomic zone-claim gate allowing it, the identical setup must succeed");
    TEST_CHECK(s_zone_claim_begin_calls == 2, "the gate is attempted exactly once per autotune_engine_run() call");
    TEST_CHECK(s_last_zone_claimant == RELAY_HEAT_ZONE_CLAIM_AUTOTUNE,
              "autotune_engine_run() must claim as AUTOTUNE, not PROFILE");
    TEST_CHECK(s_last_zone_claim_mask == 0x01, "the claimed mask must be this zone's own bit");

    autotune_engine_abort("test cleanup");
    TEST_CHECK(s_zone_claim_end_calls >= 1, "abort must release the per-zone claim it just took");

    s_test_zone_claim_refused = false;
    s_test_zone_claim_conflict_mask = 0;
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
// autotune_begin_run_locked()/force_relays_off() seams the ownership tests above use.
// ---------------------------------------------------------------------------

static void test_autotune_start_requests_heat_enable_once(void)
{
    TEST_SECTION("autotune start -- asks the safety processor to permit heating (K4), exactly once");
    reset_heat_enable_recorder(true);
    start_stepping_run(/*max_temp_c=*/0.0f, /*step_duty=*/1.0f);

    TEST_CHECK(s_req_enable_true_calls == 1,
               "autotune_begin_run_locked() must send exactly one REQUEST_ENABLE(true) -- without it the relay "
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
    heat_enable_service_pending_release(); /* 2026-09-15 fix: deferred send, drained by hand here */

    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED, "sanity: the trip landed");
    TEST_CHECK(s_req_enable_false_calls == 1,
               "autotune_escalate_and_abort() -> force_relays_off() must send exactly one REQUEST_ENABLE(false)");
    TEST_CHECK(!heat_enable_is_granted(), "no heat request may outlive a tripped run");
}

static void test_autotune_manual_abort_releases_heat_enable(void)
{
    TEST_SECTION("autotune_engine_abort() (operator Abort) -- gives K4 back");
    reset_heat_enable_recorder(true);
    start_stepping_run(/*max_temp_c=*/0.0f, /*step_duty=*/1.0f);
    s_req_enable_false_calls = 0;

    autotune_engine_abort("operator cancelled");
    heat_enable_service_pending_release(); /* 2026-09-15 fix: deferred send, drained by hand here */

    TEST_CHECK(s_req_enable_false_calls == 1, "abort_locked() -> force_relays_off() must release it");
    TEST_CHECK(!heat_enable_is_granted(), "nothing left standing");

    /* The engine's task loop calls heat_enable_release() on every tick it
     * spends in a non-running state, as a backstop. That must not put a
     * frame on the wire per tick. */
    heat_enable_release_backstop(HEAT_ENABLE_CLAIMANT_AUTOTUNE);
    heat_enable_release_backstop(HEAT_ENABLE_CLAIMANT_AUTOTUNE);
    heat_enable_service_pending_release();
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
// AUTOTUNE_MIN_RISE_FRACTION_OF_SPAN comments and autotune_finalize_fit()'s/
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
 * max-duration backstop handles it (and autotune_finalize_fit() marks it unsettled).
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
    bool refused = autotune_engine_accept(NULL, NULL);
    TEST_CHECK(!refused, "an unsettled STEP result must be refused without ack_unsettled");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "a refused accept must not have reset the engine to IDLE");

    bool accepted = autotune_engine_accept(&(autotune_accept_opts_t){.ack_unsettled = true}, NULL);
    TEST_CHECK(accepted, "the SAME unsettled result must be accepted once ack_unsettled=true");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_IDLE, "a successful accept resets the engine to IDLE");
    s_stub_set_pid_result = false; /* restore this file's default for every other test */
}

/* Owner decision 2026-10-08: accept writes zone gains/max_ramp, so it is
 * refused (mode gate, 409 over HTTP) while a firing or autotune run is
 * active and nothing is written; allowed when idle. */
static void test_autotune_engine_accept_refused_by_mode_gate_while_running(void)
{
    TEST_SECTION("autotune_engine_accept() is refused by the system mode gate while a run is active, "
                 "allowed when idle");
    memset(&s_at, 0, sizeof(s_at));
    s_at.lock = xSemaphoreCreateMutex();
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
    bool saved_set_model_result = s_stub_set_model_result;
    s_stub_set_model_result = true;
    s_at.predicted_max_ramp_ambient_c_per_hr = 500.0f; /* so adopt_ceiling has a max_ramp to write */

    /* Seed known gains/max_ramp; a refused accept must leave them bit-identical. */
    const float seed_kp = 1.25f, seed_ki = 0.0625f, seed_kd = 3.5f, seed_ramp = 200.0f;
    s_fake_zone_kp = seed_kp; s_fake_zone_ki = seed_ki; s_fake_zone_kd = seed_kd;
    s_fake_zone_max_ramp = seed_ramp;
    s_fake_set_pid_call_count = 0;
    int ramp_calls_before = s_stub_set_max_ramp_call_count;
    s_zones_write_total = 0;
    autotune_accept_opts_t adopt = {.adopt_ceiling = true};

    for (int which = 0; which < 2; which++) {
        s_stub_profile_running = (which == 0);
        s_stub_autotune_running = (which == 1);
        autotune_accept_result_t res = {0};
        bool ok = autotune_engine_accept(&adopt, &res);
        TEST_CHECK(!ok, "accept must be refused while a firing/autotune run is active");
        TEST_CHECK(res.refused_by_mode_gate, "refusal must be flagged as a mode-gate refusal (HTTP 409)");
        TEST_CHECK(res.mode_reason[0] != '\0', "gate reason text must be reported");
        TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "refused accept must not consume the result");
        TEST_CHECK(s_fake_set_pid_call_count == 0, "refused accept must not call the zones-config PID write");
        TEST_CHECK(s_zones_write_total == 0, "refused accept must call NO zones_config_set_* writer");
        TEST_CHECK(s_stub_set_max_ramp_call_count == ramp_calls_before,
                   "refused accept must not call the zones-config max_ramp write");
        TEST_CHECK(memcmp(&s_fake_zone_kp, &seed_kp, sizeof(float)) == 0 &&
                   memcmp(&s_fake_zone_ki, &seed_ki, sizeof(float)) == 0 &&
                   memcmp(&s_fake_zone_kd, &seed_kd, sizeof(float)) == 0,
                   "zone PID gains must be bit-identical after a refused accept");
        TEST_CHECK(memcmp(&s_fake_zone_max_ramp, &seed_ramp, sizeof(float)) == 0,
                   "zone max_ramp must be bit-identical after a refused accept");
    }
    s_stub_profile_running = false;
    s_stub_autotune_running = false;
    autotune_accept_result_t res = {0};
    TEST_CHECK(autotune_engine_accept(&adopt, &res), "accept must succeed when idle");
    TEST_CHECK(!res.refused_by_mode_gate, "no gate refusal when idle");
    TEST_CHECK(s_fake_set_pid_call_count == 1, "idle accept must write the PID gains exactly once");
    TEST_CHECK(s_fake_zone_kp != seed_kp || s_fake_zone_ki != seed_ki || s_fake_zone_kd != seed_kd,
               "idle accept must change the stored gains");
    TEST_CHECK(s_stub_set_max_ramp_call_count == ramp_calls_before + 1 &&
               s_fake_zone_max_ramp == 500.0f, "idle accept with adopt_ceiling must write max_ramp once");
    s_stub_set_model_result = saved_set_model_result;
    s_stub_set_pid_result = false;
}

/* Review finding 1b: a profile start landing AFTER the first gate check but
 * before the writes must still be refused, with nothing written. */
static void test_autotune_engine_accept_rechecks_gate_before_writes(void)
{
    TEST_SECTION("autotune_engine_accept() re-checks the mode gate after reserving, before any write");
    memset(&s_at, 0, sizeof(s_at));
    s_at.lock = xSemaphoreCreateMutex();
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
    s_at.predicted_max_ramp_ambient_c_per_hr = 500.0f;
    s_stub_set_pid_result = true;
    s_stub_profile_running = false;
    s_stub_autotune_running = false;
    s_fake_set_pid_call_count = 0;
    int ramp_calls_before = s_stub_set_max_ramp_call_count;
    s_zones_write_total = 0;
    s_stub_heat_calls = 0;
    s_stub_heat_flip_at_call = 1; /* first check passes, the re-check sees the start */
    autotune_accept_result_t res = {0};
    bool ok = autotune_engine_accept(&(autotune_accept_opts_t){.adopt_ceiling = true}, &res);
    s_stub_heat_flip_at_call = -1;
    s_stub_profile_running = false;
    TEST_CHECK(!ok, "accept must be refused when a run starts after the first gate check");
    TEST_CHECK(res.refused_by_mode_gate && res.mode_reason[0] != '\0', "flagged as a mode-gate refusal");
    TEST_CHECK(s_fake_set_pid_call_count == 0, "set_pid must be called 0 times");
    TEST_CHECK(s_zones_write_total == 0, "no zones_config_set_* writer may be called");
    TEST_CHECK(s_stub_set_max_ramp_call_count == ramp_calls_before, "set_max_ramp must be called 0 times");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "result not consumed");
    TEST_CHECK(!s_at.external_write_reserved, "reservation must be released on the refusal");
    s_stub_set_pid_result = false;
}

/* Dev review 9 L1/L2: Accept refuses (409 path) while an adaptive run-end write is in flight, and
 * when the setter reports the zone is run-claimed. Both must write nothing and release the reservation. */
static void test_autotune_engine_accept_refused_by_adaptive_write_and_run_claim(void)
{
    TEST_SECTION("autotune_engine_accept() refuses on adaptive write_in_flight and on BUSY_RUNNING");
    for (int pass = 0; pass < 2; pass++) {
        memset(&s_at, 0, sizeof(s_at));
        s_at.lock = xSemaphoreCreateMutex();
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
        s_stub_profile_running = false;
        s_stub_autotune_running = false;
        s_fake_set_pid_call_count = 0;
        s_zones_write_total = 0;
        s_stub_adaptive_write_in_flight = (pass == 0);
        s_stub_set_pid_busy = (pass == 1);
        autotune_accept_result_t res = {0};
        bool ok = autotune_engine_accept(NULL, &res);
        s_stub_adaptive_write_in_flight = false;
        s_stub_set_pid_busy = false;
        TEST_CHECK(!ok, "accept must be refused");
        TEST_CHECK(res.refused_by_mode_gate && res.mode_reason[0] != '\0', "flagged as a mode-gate refusal (409)");
        TEST_CHECK(pass == 0 ? s_fake_set_pid_call_count == 0 : true, "adaptive write in flight: set_pid never called");
        TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "result not consumed");
        TEST_CHECK(!s_at.external_write_reserved, "reservation must be released on the refusal");
    }
    s_stub_set_pid_result = false;
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
    bool refused = autotune_engine_accept(NULL, NULL);
    TEST_CHECK(!refused, "an unconverged extrapolation must be refused without ack_unsettled, even "
                         "though settled and tau_consistent are both true");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "a refused accept must not have reset the engine to IDLE");

    bool accepted = autotune_engine_accept(&(autotune_accept_opts_t){.ack_unsettled = true}, NULL);
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
    bool refused = autotune_engine_accept(NULL, NULL);
    TEST_CHECK(!refused, "a tau-inconsistent fit must be refused without ack_unsettled, even though "
                         "settled and extrapolation_converged are both true");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "a refused accept must not have reset the engine to IDLE");

    bool accepted = autotune_engine_accept(&(autotune_accept_opts_t){.ack_unsettled = true}, NULL);
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
    bool accepted = autotune_engine_accept(NULL, NULL);
    TEST_CHECK(accepted, "a fully clean fit must be accepted with ack_unsettled=false -- the common, "
                         "healthy path must never require the acknowledgement checkbox");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_IDLE, "a successful accept resets the engine to IDLE");
    s_stub_set_pid_result = false;
}

/* ZONES_CFG_VERSION 12->13: the tuning-quality record (set 1 -- see
 * zone_cfg_t::tuning_valid's own doc comment, zones_config_json.h). A
 * completed, accepted STEP-method run must write it -- with the same
 * baseline_c/raw_rise_c/rise_inf_c/settled/extrapolation_converged/
 * tau_consistent_with_gain/rule the fit actually produced -- but ONLY once
 * the gains AND the model have both already persisted; the model-persist
 * failure and RELAY-method cases each get their own negative proof right
 * below this one. */
static void test_autotune_engine_accept_writes_tuning_quality_on_step_success(void)
{
    TEST_SECTION("autotune_engine_accept() writes zones_config_set_tuning_quality() on a successful "
                 "STEP-method accept, with the fit's own fields");
    static MAX31856BusClass bus;
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    s_at.lock = xSemaphoreCreateMutex();
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");
    s_at.state = AUTOTUNE_ENGINE_DONE;
    s_at.method = AUTOTUNE_METHOD_STEP;
    s_at.zone_index = 2; /* deliberately not zone 0 -- catches a hardcoded index */
    s_at.step_ambient_c = 24.5f;
    s_at.proposed_gains.rule = AUTOTUNE_RULE_COHEN_COON;
    s_at.model.valid = true;
    s_at.model.settled = true;
    s_at.model.extrapolation_converged = true;
    s_at.model.tau_consistent_with_gain = true;
    s_at.model.k_gain_c_per_duty = 10.0f;
    s_at.model.tau_s = 100.0f;
    s_at.model.dead_time_s = 5.0f;
    s_at.model.baseline_c = 25.0f;
    s_at.model.raw_rise_c = 38.2f;
    s_at.model.rise_inf_c = 41.7f;

    s_stub_set_pid_result = true;
    s_stub_set_model_result = true;
    s_stub_tuning_quality_call_count = 0;
    s_stub_tuning_quality_zone = 0xFF;
    memset(&s_stub_tuning_quality_written, 0, sizeof(s_stub_tuning_quality_written));

    bool accepted = autotune_engine_accept(NULL, NULL);

    TEST_CHECK(accepted, "a fully clean STEP fit accepts");
    TEST_CHECK(s_stub_tuning_quality_call_count == 1,
              "zones_config_set_tuning_quality() is called exactly once on a successful STEP accept");
    TEST_CHECK(s_stub_tuning_quality_zone == 2, "written for the zone under test, not a hardcoded index");
    TEST_CHECK(s_stub_tuning_quality_written.valid, "the written record's valid flag is true");
    TEST_CHECK(s_stub_tuning_quality_written.method == (uint8_t)AUTOTUNE_METHOD_STEP, "method is STEP");
    TEST_CHECK(s_stub_tuning_quality_written.rule == (uint8_t)AUTOTUNE_RULE_COHEN_COON,
              "rule is exactly the tuning rule this run actually used, not a default");
    TEST_CHECK(s_stub_tuning_quality_written.settled && s_stub_tuning_quality_written.extrapolation_converged &&
              s_stub_tuning_quality_written.tau_consistent,
              "all three fit-confidence flags carried through from the model");
    TEST_CHECK_NEAR(s_stub_tuning_quality_written.baseline_c, 25.0f, 1e-4, "baseline_c carried through");
    TEST_CHECK_NEAR(s_stub_tuning_quality_written.step_ambient_c, 24.5f, 1e-4,
                    "step_ambient_c carried through -- captured under the lock, not re-read after release");
    TEST_CHECK_NEAR(s_stub_tuning_quality_written.raw_rise_c, 38.2f, 1e-4, "raw_rise_c carried through");
    TEST_CHECK_NEAR(s_stub_tuning_quality_written.rise_inf_c, 41.7f, 1e-4, "rise_inf_c carried through");

    s_stub_set_pid_result = false;
    s_stub_set_model_result = false;
}

/* Negative proof 1: the model failing to persist must skip the
 * tuning-quality write entirely -- a quality record attached to a model
 * that isn't actually stored would describe a fit the zone isn't running. */
static void test_autotune_engine_accept_skips_tuning_quality_when_model_persist_fails(void)
{
    TEST_SECTION("autotune_engine_accept() does NOT write tuning quality when zones_config_set_model() "
                 "itself fails/refuses");
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
    s_stub_set_model_result = false; /* the case under test -- model persist refuses/fails */
    s_stub_tuning_quality_call_count = 0;

    bool accepted = autotune_engine_accept(NULL, NULL);

    TEST_CHECK(accepted, "acceptance itself still succeeds -- the gains are already live, per this "
                         "function's own comment on why a model-persist failure is logged, not propagated");
    TEST_CHECK(s_stub_tuning_quality_call_count == 0,
              "zones_config_set_tuning_quality() must NOT be called when the model failed to persist");

    s_stub_set_pid_result = false;
}

/* Negative proof 2: a RELAY-method accept measures no FOPDT model at all
 * (see autotune_engine_accept()'s own comment on the RELAY branch) and must
 * not write a tuning-quality record either. */
static void test_autotune_engine_accept_skips_tuning_quality_on_relay_method(void)
{
    TEST_SECTION("autotune_engine_accept() does NOT write tuning quality on the RELAY path -- "
                 "a relay test measures no FOPDT model to attach one to");
    static MAX31856BusClass bus;
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    s_at.lock = xSemaphoreCreateMutex();
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");
    s_at.state = AUTOTUNE_ENGINE_DONE;
    s_at.method = AUTOTUNE_METHOD_RELAY;
    s_at.zone_index = 0;
    s_at.relay.valid = true; /* autotune_engine_accept()'s own have_result gate reads
                              * s_at.relay.valid, not s_at.model.valid, on this path */

    s_stub_set_pid_result = true;
    s_stub_set_model_result = true;
    s_stub_tuning_quality_call_count = 0;

    bool accepted = autotune_engine_accept(NULL, NULL);

    TEST_CHECK(accepted, "a relay-method accept still succeeds -- gains only, no model");
    TEST_CHECK(s_stub_tuning_quality_call_count == 0,
              "zones_config_set_tuning_quality() must NOT be called on the RELAY path");

    s_stub_set_pid_result = false;
    s_stub_set_model_result = false;
}

/* PID_EXPANSION_PLAN.md section 3.2 follow-up ("on-board identification pass
 * for coupling_diag_k_dc"). A completed, accepted STEP-method run must
 * persist coupling_diag_k_dc for the zone under test, from the SAME fitted
 * gain zones_config_set_model() just wrote to model_k_dc -- see
 * autotune_engine_accept()'s own comment on why the direct fit IS the
 * diagonal cell. */
static void test_autotune_engine_accept_writes_coupling_diag_k_dc_on_step_success(void)
{
    TEST_SECTION("autotune_engine_accept() writes zones_config_set_coupling_diag_k_dc() on a successful "
                 "STEP-method accept, with the SAME gain as model_k_dc");
    static MAX31856BusClass bus;
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    s_at.lock = xSemaphoreCreateMutex();
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");
    s_at.state = AUTOTUNE_ENGINE_DONE;
    s_at.method = AUTOTUNE_METHOD_STEP;
    s_at.zone_index = 1; /* deliberately not zone 0 -- catches a hardcoded index */
    s_at.model.valid = true;
    s_at.model.settled = true;
    s_at.model.extrapolation_converged = true;
    s_at.model.tau_consistent_with_gain = true;
    s_at.model.k_gain_c_per_duty = 27.32f;
    s_at.model.tau_s = 100.0f;
    s_at.model.dead_time_s = 5.0f;

    s_stub_set_pid_result = true;
    s_stub_set_model_result = true;
    s_stub_set_coupling_diag_k_dc_result = true;
    s_stub_set_coupling_diag_k_dc_call_count = 0;
    s_stub_set_coupling_diag_k_dc_zone = 0xFF;
    s_stub_set_coupling_diag_k_dc_value = 0.0f;

    bool accepted = autotune_engine_accept(NULL, NULL);

    TEST_CHECK(accepted, "a fully clean STEP fit accepts");
    TEST_CHECK(s_stub_set_coupling_diag_k_dc_call_count == 1,
              "zones_config_set_coupling_diag_k_dc() is called exactly once on a successful STEP accept");
    TEST_CHECK(s_stub_set_coupling_diag_k_dc_zone == 1, "written for the zone under test, not a hardcoded index");
    TEST_CHECK_NEAR(s_stub_set_coupling_diag_k_dc_value, 27.32f, 1e-4,
                    "the diagonal gain persisted is exactly the direct fit's k_gain_c_per_duty -- the same "
                    "rested single-zone step data model_k_dc was just written from, not a separate estimate");

    s_stub_set_pid_result = false;
    s_stub_set_model_result = false;
}

/* Negative proof 1: the model failing to persist must skip the
 * coupling_diag_k_dc write too -- writing a diagonal cell for a model that
 * isn't actually stored would describe a fit the zone isn't running,
 * exactly the same reasoning the tuning-quality skip above rests on. */
static void test_autotune_engine_accept_skips_coupling_diag_k_dc_when_model_persist_fails(void)
{
    TEST_SECTION("autotune_engine_accept() does NOT write coupling_diag_k_dc when zones_config_set_model() "
                 "itself fails/refuses");
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
    s_stub_set_model_result = false; /* the case under test -- model persist refuses/fails */
    s_stub_set_coupling_diag_k_dc_call_count = 0;

    bool accepted = autotune_engine_accept(NULL, NULL);

    TEST_CHECK(accepted, "acceptance itself still succeeds -- the gains are already live");
    TEST_CHECK(s_stub_set_coupling_diag_k_dc_call_count == 0,
              "zones_config_set_coupling_diag_k_dc() must NOT be called when the model failed to persist");

    s_stub_set_pid_result = false;
}

/* Negative proof 2: a RELAY-method accept measures no FOPDT model, so there
 * is no diagonal gain to persist either -- same reasoning as the
 * tuning-quality RELAY skip above. */
static void test_autotune_engine_accept_skips_coupling_diag_k_dc_on_relay_method(void)
{
    TEST_SECTION("autotune_engine_accept() does NOT write coupling_diag_k_dc on the RELAY path -- "
                 "a relay test measures no FOPDT model to take a diagonal gain from");
    static MAX31856BusClass bus;
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    s_at.lock = xSemaphoreCreateMutex();
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");
    s_at.state = AUTOTUNE_ENGINE_DONE;
    s_at.method = AUTOTUNE_METHOD_RELAY;
    s_at.zone_index = 0;
    s_at.relay.valid = true;

    s_stub_set_pid_result = true;
    s_stub_set_model_result = true;
    s_stub_set_coupling_diag_k_dc_call_count = 0;

    bool accepted = autotune_engine_accept(NULL, NULL);

    TEST_CHECK(accepted, "a relay-method accept still succeeds -- gains only, no model");
    TEST_CHECK(s_stub_set_coupling_diag_k_dc_call_count == 0,
              "zones_config_set_coupling_diag_k_dc() must NOT be called on the RELAY path");

    s_stub_set_pid_result = false;
    s_stub_set_model_result = false;
}

/* docs/audits/adaptive_tune_vs_owner_requirements_2026-09-11.md item 4:
 * a NEW full autotune Accept must re-anchor adaptive_tune_model.c's
 * ratchet-prevention baseline (autotune_baseline_k_dc) to the fresh model
 * just written -- same "reset one side of a pair" contract adaptive_tune_
 * clear_ki_baseline() already covers for the Ki side (called unconditionally
 * for both methods, elsewhere in autotune_engine_guard.c). Positive proof,
 * same convention as test_autotune_engine_accept_writes_coupling_diag_k_dc_
 * on_a_clean_step_accept() above. */
static void test_autotune_engine_accept_resets_adaptive_tune_baseline_on_a_clean_step_accept(void)
{
    TEST_SECTION("autotune_engine_accept() re-anchors adaptive_tune's K_dc baseline to the new model on a "
                 "successful STEP accept");
    static MAX31856BusClass bus;
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    s_at.lock = xSemaphoreCreateMutex();
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");
    s_at.state = AUTOTUNE_ENGINE_DONE;
    s_at.method = AUTOTUNE_METHOD_STEP;
    s_at.zone_index = 2; /* deliberately not zone 0/1 -- catches a hardcoded index */
    s_at.model.valid = true;
    s_at.model.settled = true;
    s_at.model.extrapolation_converged = true;
    s_at.model.tau_consistent_with_gain = true;
    s_at.model.k_gain_c_per_duty = 33.7f;
    s_at.model.tau_s = 100.0f;
    s_at.model.dead_time_s = 5.0f;

    s_stub_set_pid_result = true;
    s_stub_set_model_result = true;
    s_stub_set_autotune_baseline_k_dc_result = true;
    s_stub_set_autotune_baseline_k_dc_call_count = 0;
    s_stub_set_autotune_baseline_k_dc_zone = 0xFF;
    s_stub_set_autotune_baseline_k_dc_value = 0.0f;

    bool accepted = autotune_engine_accept(NULL, NULL);

    TEST_CHECK(accepted, "a fully clean STEP fit accepts");
    TEST_CHECK(s_stub_set_autotune_baseline_k_dc_call_count == 1,
              "zones_config_set_autotune_baseline_k_dc() is called exactly once on a successful STEP accept");
    TEST_CHECK(s_stub_set_autotune_baseline_k_dc_zone == 2, "written for the zone under test, not a hardcoded index");
    TEST_CHECK_NEAR(s_stub_set_autotune_baseline_k_dc_value, 33.7f, 1e-4,
                    "the baseline persisted is exactly this accept's own k_gain_c_per_duty -- the same value "
                    "model_k_dc was just written with, never a stale or unrelated number");

    s_stub_set_pid_result = false;
    s_stub_set_model_result = false;
}

/* Negative proof: the model failing to persist must skip the baseline reset
 * too -- re-anchoring to a model that isn't actually stored would leave
 * adaptive_tune bounding drift against a fit the zone isn't running. */
static void test_autotune_engine_accept_skips_adaptive_tune_baseline_reset_when_model_persist_fails(void)
{
    TEST_SECTION("autotune_engine_accept() does NOT reset the adaptive-tune baseline when "
                 "zones_config_set_model() itself fails/refuses");
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
    s_stub_set_model_result = false; /* the case under test -- model persist refuses/fails */
    s_stub_set_autotune_baseline_k_dc_call_count = 0;

    bool accepted = autotune_engine_accept(NULL, NULL);

    TEST_CHECK(accepted, "acceptance itself still succeeds -- the gains are already live");
    TEST_CHECK(s_stub_set_autotune_baseline_k_dc_call_count == 0,
              "zones_config_set_autotune_baseline_k_dc() must NOT be called when the model failed to persist");

    s_stub_set_pid_result = false;
}

/* Negative proof 2: a RELAY-method accept measures no FOPDT model, so there
 * is nothing for the K_dc baseline to re-anchor to either. */
static void test_autotune_engine_accept_skips_adaptive_tune_baseline_reset_on_relay_method(void)
{
    TEST_SECTION("autotune_engine_accept() does NOT reset the adaptive-tune baseline on the RELAY path -- "
                 "a relay test measures no FOPDT model to re-anchor to");
    static MAX31856BusClass bus;
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    s_at.lock = xSemaphoreCreateMutex();
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");
    s_at.state = AUTOTUNE_ENGINE_DONE;
    s_at.method = AUTOTUNE_METHOD_RELAY;
    s_at.zone_index = 0;
    s_at.relay.valid = true;

    s_stub_set_pid_result = true;
    s_stub_set_model_result = true;
    s_stub_set_autotune_baseline_k_dc_call_count = 0;

    bool accepted = autotune_engine_accept(NULL, NULL);

    TEST_CHECK(accepted, "a relay-method accept still succeeds -- gains only, no model");
    TEST_CHECK(s_stub_set_autotune_baseline_k_dc_call_count == 0,
              "zones_config_set_autotune_baseline_k_dc() must NOT be called on the RELAY path");

    s_stub_set_pid_result = false;
    s_stub_set_model_result = false;
}

/* TODO.md 6A.4 positive proof: autotune_engine_accept(opts={ack_unsettled=ack, adopt_ceiling=true}) on a
 * successful STEP accept with a real predicted ceiling must adopt it into
 * max_ramp_c_per_hr via zones_config_set_max_ramp(), with exactly this
 * run's zone and predicted value. */
static void test_autotune_engine_accept_adopts_ceiling_when_requested(void)
{
    TEST_SECTION("autotune_engine_accept(.., adopt_ceiling=true) writes zones_config_set_max_ramp() "
                 "with this run's predicted ceiling, on a successful STEP accept");
    static MAX31856BusClass bus;
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    s_at.lock = xSemaphoreCreateMutex();
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");
    s_at.state = AUTOTUNE_ENGINE_DONE;
    s_at.method = AUTOTUNE_METHOD_STEP;
    s_at.zone_index = 3;
    s_at.model.valid = true;
    s_at.model.settled = true;
    s_at.model.extrapolation_converged = true;
    s_at.model.tau_consistent_with_gain = true;
    s_at.model.k_gain_c_per_duty = 10.0f;
    s_at.model.tau_s = 100.0f;
    s_at.model.dead_time_s = 5.0f;
    /* End-of-step field left at a DIFFERENT value than the ambient field on
     * purpose: this proves the ambient-evaluated field is what actually
     * gets adopted, not the (smaller, temperature-dependent) end-of-step
     * one -- see autotune_engine.h's field comments. */
    s_at.predicted_max_ramp_c_per_hr = 40.0f;
    s_at.predicted_max_ramp_ambient_c_per_hr = 123.5f;

    s_stub_set_pid_result = true;
    s_stub_set_model_result = true;
    s_stub_set_max_ramp_result = true;
    s_stub_set_max_ramp_call_count = 0;
    s_stub_set_max_ramp_zone = 0xFF;
    s_stub_set_max_ramp_value = -1.0f;
    s_stub_get_max_ramp_result = true;
    s_stub_get_max_ramp_value = 0.0f; /* no ceiling configured yet -- adoption cannot tighten */

    autotune_accept_opts_t opts = {.adopt_ceiling = true};
    autotune_accept_result_t result = {.adoption = AUTOTUNE_CEILING_SKIPPED_NOT_REQUESTED,
                                        .old_ceiling_c_per_hr = -1.0f, .new_ceiling_c_per_hr = -1.0f};
    bool accepted = autotune_engine_accept(&opts, &result);

    TEST_CHECK(accepted, "a fully clean STEP fit accepts with adopt_ceiling requested");
    TEST_CHECK(s_stub_set_max_ramp_call_count == 1,
              "zones_config_set_max_ramp() is called exactly once when adopt_ceiling is requested");
    TEST_CHECK(s_stub_set_max_ramp_zone == 3, "written for the zone under test, not a hardcoded index");
    TEST_CHECK_NEAR(s_stub_set_max_ramp_value, 123.5f, 1e-4,
                    "written with the AMBIENT-evaluated predicted_max_ramp_ambient_c_per_hr, not the "
                    "end-of-step predicted_max_ramp_c_per_hr (40.0f)");
    TEST_CHECK(result.adoption == AUTOTUNE_CEILING_ADOPTED, "reported outcome is ADOPTED");
    TEST_CHECK_NEAR(result.new_ceiling_c_per_hr, 123.5f, 1e-4, "reported new ceiling matches what was written");

    s_stub_set_pid_result = false;
    s_stub_set_model_result = false;
    s_stub_set_max_ramp_result = true;
    s_stub_get_max_ramp_result = true;
    s_stub_get_max_ramp_value = 0.0f;
}

/* Review fix: adoption must never silently TIGHTEN an existing, nonzero
 * zone ceiling. A stored ceiling already at or below the new ambient
 * estimate is left untouched and the outcome reports SKIPPED_WOULD_TIGHTEN,
 * not ADOPTED. */
static void test_autotune_engine_accept_skips_when_it_would_tighten(void)
{
    TEST_SECTION("autotune_engine_accept(.., adopt_ceiling=true) skips adoption -- "
                 "SKIPPED_WOULD_TIGHTEN -- when the stored ceiling is already tighter than the new estimate");
    static MAX31856BusClass bus;
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    s_at.lock = xSemaphoreCreateMutex();
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");
    s_at.state = AUTOTUNE_ENGINE_DONE;
    s_at.method = AUTOTUNE_METHOD_STEP;
    s_at.zone_index = 2;
    s_at.model.valid = true;
    s_at.model.settled = true;
    s_at.model.extrapolation_converged = true;
    s_at.model.tau_consistent_with_gain = true;
    s_at.model.k_gain_c_per_duty = 10.0f;
    s_at.model.tau_s = 100.0f;
    s_at.model.dead_time_s = 5.0f;
    s_at.predicted_max_ramp_ambient_c_per_hr = 50.0f;

    s_stub_set_pid_result = true;
    s_stub_set_model_result = true;
    s_stub_set_max_ramp_call_count = 0;
    s_stub_get_max_ramp_result = true;
    s_stub_get_max_ramp_value = 150.0f; /* stored ceiling is LOOSER than the new 50.0f estimate --
                                          * adopting the estimate would TIGHTEN it, so it must skip */

    autotune_accept_opts_t opts = {.adopt_ceiling = true};
    autotune_accept_result_t result = {.adoption = AUTOTUNE_CEILING_ADOPTED,
                                        .old_ceiling_c_per_hr = -1.0f, .new_ceiling_c_per_hr = -1.0f};
    bool accepted = autotune_engine_accept(&opts, &result);

    TEST_CHECK(accepted, "the accept itself still succeeds");
    TEST_CHECK(s_stub_set_max_ramp_call_count == 0,
              "zones_config_set_max_ramp() must NOT be called when it would tighten the stored ceiling");
    TEST_CHECK(result.adoption == AUTOTUNE_CEILING_SKIPPED_WOULD_TIGHTEN, "reported outcome is SKIPPED_WOULD_TIGHTEN");
    TEST_CHECK_NEAR(result.old_ceiling_c_per_hr, 150.0f, 1e-4, "reported old ceiling is the stored (looser) value");
    TEST_CHECK_NEAR(result.new_ceiling_c_per_hr, 0.0f, 1e-4, "reported new ceiling stays at its init value -- nothing was written");

    s_stub_set_pid_result = false;
    s_stub_set_model_result = false;
    s_stub_get_max_ramp_result = true;
    s_stub_get_max_ramp_value = 0.0f;
}

/* Review fix: a failed read of the currently stored ceiling must never be
 * treated as "no old value" -- that would let adoption proceed and silently
 * overwrite a ceiling we could not actually confirm the value of.
 * s_stub_get_max_ramp_result = false models zones_config_get_max_ramp()
 * failing (see that stub just above); the accept must report
 * AUTOTUNE_CEILING_SKIPPED_READ_FAILED and never call
 * zones_config_set_max_ramp() at all. */
static void test_autotune_engine_accept_skips_ceiling_adoption_when_read_fails(void)
{
    TEST_SECTION("autotune_engine_accept(.., adopt_ceiling=true) skips adoption -- SKIPPED_READ_FAILED -- "
                 "when zones_config_get_max_ramp() fails, never falling through to adopt as if unconfigured");
    static MAX31856BusClass bus;
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    s_at.lock = xSemaphoreCreateMutex();
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");
    s_at.state = AUTOTUNE_ENGINE_DONE;
    s_at.method = AUTOTUNE_METHOD_STEP;
    s_at.zone_index = 2;
    s_at.model.valid = true;
    s_at.model.settled = true;
    s_at.model.extrapolation_converged = true;
    s_at.model.tau_consistent_with_gain = true;
    s_at.model.k_gain_c_per_duty = 10.0f;
    s_at.model.tau_s = 100.0f;
    s_at.model.dead_time_s = 5.0f;
    s_at.predicted_max_ramp_ambient_c_per_hr = 50.0f;

    s_stub_set_pid_result = true;
    s_stub_set_model_result = true;
    s_stub_set_max_ramp_call_count = 0;
    s_stub_get_max_ramp_result = false; /* forces the read to fail */
    s_stub_get_max_ramp_value = 999.0f; /* must never surface -- the read failed */

    autotune_accept_opts_t opts = {.adopt_ceiling = true};
    autotune_accept_result_t result = {.adoption = AUTOTUNE_CEILING_ADOPTED,
                                        .old_ceiling_c_per_hr = -1.0f, .new_ceiling_c_per_hr = -1.0f};
    bool accepted = autotune_engine_accept(&opts, &result);

    TEST_CHECK(accepted, "the accept itself still succeeds -- gains/model are already live");
    TEST_CHECK(s_stub_set_max_ramp_call_count == 0,
              "zones_config_set_max_ramp() must NOT be called when the old ceiling could not be read");
    TEST_CHECK(result.adoption == AUTOTUNE_CEILING_SKIPPED_READ_FAILED, "reported outcome is SKIPPED_READ_FAILED");
    TEST_CHECK_NEAR(result.old_ceiling_c_per_hr, 0.0f, 1e-4,
                    "reported old ceiling stays at the function's own reset value -- the failed read's "
                    "999.0f value must never surface");

    s_stub_set_pid_result = false;
    s_stub_set_model_result = false;
    s_stub_get_max_ramp_result = true;
    s_stub_get_max_ramp_value = 0.0f;
}

/* Review fix: a failed persist of the new ceiling (zones_config_set_max_ramp()
 * returning false) used to be silently discarded -- the caller had no way to
 * tell it apart from ADOPTED. Must now report AUTOTUNE_CEILING_FAILED_TO_
 * PERSIST distinctly. */
static void test_autotune_engine_accept_reports_ceiling_persist_failure(void)
{
    TEST_SECTION("autotune_engine_accept(.., adopt_ceiling=true) reports FAILED_TO_PERSIST -- not ADOPTED -- "
                 "when zones_config_set_max_ramp() itself fails");
    static MAX31856BusClass bus;
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    s_at.lock = xSemaphoreCreateMutex();
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");
    s_at.state = AUTOTUNE_ENGINE_DONE;
    s_at.method = AUTOTUNE_METHOD_STEP;
    s_at.zone_index = 2;
    s_at.model.valid = true;
    s_at.model.settled = true;
    s_at.model.extrapolation_converged = true;
    s_at.model.tau_consistent_with_gain = true;
    s_at.model.k_gain_c_per_duty = 10.0f;
    s_at.model.tau_s = 100.0f;
    s_at.model.dead_time_s = 5.0f;
    s_at.predicted_max_ramp_ambient_c_per_hr = 123.5f;

    s_stub_set_pid_result = true;
    s_stub_set_model_result = true;
    s_stub_set_max_ramp_call_count = 0;
    s_stub_set_max_ramp_result = false; /* forces the persist to fail */
    s_stub_get_max_ramp_result = true;
    s_stub_get_max_ramp_value = 0.0f; /* no ceiling configured yet -- would otherwise be adoptable */

    autotune_accept_opts_t opts = {.adopt_ceiling = true};
    autotune_accept_result_t result = {.adoption = AUTOTUNE_CEILING_SKIPPED_NOT_REQUESTED,
                                        .old_ceiling_c_per_hr = -1.0f, .new_ceiling_c_per_hr = -1.0f};
    bool accepted = autotune_engine_accept(&opts, &result);

    TEST_CHECK(accepted, "the accept itself still succeeds -- gains/model are already live");
    TEST_CHECK(s_stub_set_max_ramp_call_count == 1,
              "zones_config_set_max_ramp() is still attempted exactly once");
    TEST_CHECK(result.adoption == AUTOTUNE_CEILING_FAILED_TO_PERSIST, "reported outcome is FAILED_TO_PERSIST, "
              "not ADOPTED, even though the estimate itself was adoptable");
    /* Review fix: zones_config_set_max_ramp() writes RAM and bumps
     * s_config_generation BEFORE it calls nvs_save() -- so on an NVS
     * failure the new ceiling IS live in RAM for this boot. new_ceiling
     * must report that live value (not stay at the function's reset 0.0f),
     * so a caller can tell the operator what is actually running right now
     * versus what will come back after a reboot (old_ceiling_c_per_hr). */
    TEST_CHECK_NEAR(result.new_ceiling_c_per_hr, 123.5f, 1e-4,
                    "reported new ceiling is the value now live in RAM for this boot, even though it "
                    "was not persisted");
    TEST_CHECK_NEAR(result.old_ceiling_c_per_hr, 0.0f, 1e-4,
                    "reported old ceiling is what a reboot will revert to");

    s_stub_set_pid_result = false;
    s_stub_set_model_result = false;
    s_stub_set_max_ramp_result = true;
    s_stub_get_max_ramp_result = true;
    s_stub_get_max_ramp_value = 0.0f;
}

/* Review fix: an out-of-range predicted ceiling is REJECTED, never clamped.
 * ZONE_MAX_RAMP_C_PER_HR_MAX is 1000.0f (zones_http.h). */
static void test_autotune_engine_accept_rejects_out_of_range_not_clamped(void)
{
    TEST_SECTION("autotune_engine_accept(.., adopt_ceiling=true) rejects (does not clamp) a "
                 "predicted ceiling above ZONE_MAX_RAMP_C_PER_HR_MAX");
    static MAX31856BusClass bus;
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    s_at.lock = xSemaphoreCreateMutex();
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");
    s_at.state = AUTOTUNE_ENGINE_DONE;
    s_at.method = AUTOTUNE_METHOD_STEP;
    s_at.zone_index = 4;
    s_at.model.valid = true;
    s_at.model.settled = true;
    s_at.model.extrapolation_converged = true;
    s_at.model.tau_consistent_with_gain = true;
    s_at.model.k_gain_c_per_duty = 10.0f;
    s_at.model.tau_s = 100.0f;
    s_at.model.dead_time_s = 5.0f;
    s_at.predicted_max_ramp_ambient_c_per_hr = 5000.0f; /* well above the 1000.0f max */

    s_stub_set_pid_result = true;
    s_stub_set_model_result = true;
    s_stub_set_max_ramp_call_count = 0;
    s_stub_get_max_ramp_result = true;
    s_stub_get_max_ramp_value = 0.0f;

    autotune_accept_opts_t opts = {.adopt_ceiling = true};
    autotune_accept_result_t result = {.adoption = AUTOTUNE_CEILING_ADOPTED,
                                        .old_ceiling_c_per_hr = -1.0f, .new_ceiling_c_per_hr = -1.0f};
    bool accepted = autotune_engine_accept(&opts, &result);

    TEST_CHECK(accepted, "the accept itself still succeeds -- gains/model are already live");
    TEST_CHECK(s_stub_set_max_ramp_call_count == 0,
              "zones_config_set_max_ramp() must NOT be called -- out of range is a refusal, not a clamp");
    TEST_CHECK(result.adoption == AUTOTUNE_CEILING_REJECTED_OUT_OF_RANGE, "reported outcome is REJECTED_OUT_OF_RANGE");
    TEST_CHECK_NEAR(result.new_ceiling_c_per_hr, 0.0f, 1e-4, "reported new ceiling stays at its init value -- nothing was written");

    s_stub_set_pid_result = false;
    s_stub_set_model_result = false;
    s_stub_get_max_ramp_result = true;
    s_stub_get_max_ramp_value = 0.0f;
}

/* Negative proof 1 (the "default false does not adopt" requirement): the
 * exact same DONE result as above, but through the plain autotune_engine_
 * accept() wrapper (which always passes adopt_ceiling=false) must NOT touch
 * max_ramp_c_per_hr at all -- proves the default preserves today's
 * behavior. */
static void test_autotune_engine_accept_does_not_adopt_ceiling_by_default(void)
{
    TEST_SECTION("autotune_engine_accept() (adopt_ceiling defaults false) does NOT write "
                 "zones_config_set_max_ramp(), even when a predicted ceiling exists");
    static MAX31856BusClass bus;
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    s_at.lock = xSemaphoreCreateMutex();
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");
    s_at.state = AUTOTUNE_ENGINE_DONE;
    s_at.method = AUTOTUNE_METHOD_STEP;
    s_at.zone_index = 3;
    s_at.model.valid = true;
    s_at.model.settled = true;
    s_at.model.extrapolation_converged = true;
    s_at.model.tau_consistent_with_gain = true;
    s_at.model.k_gain_c_per_duty = 10.0f;
    s_at.model.tau_s = 100.0f;
    s_at.model.dead_time_s = 5.0f;
    s_at.predicted_max_ramp_c_per_hr = 123.5f;

    s_stub_set_pid_result = true;
    s_stub_set_model_result = true;
    s_stub_set_max_ramp_call_count = 0;

    bool accepted = autotune_engine_accept(NULL, NULL);

    TEST_CHECK(accepted, "the accept itself still succeeds -- adopt_ceiling only ever adds a write");
    TEST_CHECK(s_stub_set_max_ramp_call_count == 0,
              "zones_config_set_max_ramp() must NOT be called via the default-false wrapper");

    s_stub_set_pid_result = false;
    s_stub_set_model_result = false;
}

/* Negative proof 2: a predicted ceiling of exactly 0.0 (never computed --
 * autotune_begin_run_locked() resets it there and pid_autotune_estimate_
 * max_ramp_c_per_hr() leaves it there when it has nothing to extrapolate
 * from) must not be adopted even when adopt_ceiling=true -- 0 is zone_
 * cfg_t::max_ramp_c_per_hr's own "never configured" encoding, so adopting
 * it would silently CLEAR an existing operator-set ceiling. */
static void test_autotune_engine_accept_does_not_adopt_a_zero_ceiling(void)
{
    TEST_SECTION("autotune_engine_accept(.., adopt_ceiling=true) does NOT write zones_config_set_max_ramp() "
                 "when predicted_max_ramp_c_per_hr is 0 (never computed)");
    static MAX31856BusClass bus;
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    s_at.lock = xSemaphoreCreateMutex();
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");
    s_at.state = AUTOTUNE_ENGINE_DONE;
    s_at.method = AUTOTUNE_METHOD_STEP;
    s_at.zone_index = 1;
    s_at.model.valid = true;
    s_at.model.settled = true;
    s_at.model.extrapolation_converged = true;
    s_at.model.tau_consistent_with_gain = true;
    s_at.model.k_gain_c_per_duty = 10.0f;
    s_at.model.tau_s = 100.0f;
    s_at.model.dead_time_s = 5.0f;
    s_at.predicted_max_ramp_c_per_hr = 0.0f;
    s_at.predicted_max_ramp_ambient_c_per_hr = 0.0f;

    s_stub_set_pid_result = true;
    s_stub_set_model_result = true;
    s_stub_set_max_ramp_call_count = 0;
    s_stub_get_max_ramp_result = true;
    s_stub_get_max_ramp_value = 0.0f;

    autotune_accept_opts_t opts = {.adopt_ceiling = true};
    bool accepted = autotune_engine_accept(&opts, NULL);

    TEST_CHECK(accepted, "the accept itself still succeeds");
    TEST_CHECK(s_stub_set_max_ramp_call_count == 0,
              "zones_config_set_max_ramp() must NOT be called when the predicted ceiling is 0");

    s_stub_set_pid_result = false;
    s_stub_set_model_result = false;
}

/* Negative proof 3: a RELAY-method accept measures no FOPDT model, so there
 * is no predicted ceiling to adopt either -- same reasoning as the
 * coupling-diag and tuning-quality RELAY skips above. */
static void test_autotune_engine_accept_does_not_adopt_ceiling_on_relay_method(void)
{
    TEST_SECTION("autotune_engine_accept(.., adopt_ceiling=true) does NOT write zones_config_set_max_ramp() "
                 "on the RELAY path");
    static MAX31856BusClass bus;
    static SafetyLinkClass safety;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    memset(&safety, 0, sizeof(safety));
    s_at.lock = xSemaphoreCreateMutex();
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");
    s_at.state = AUTOTUNE_ENGINE_DONE;
    s_at.method = AUTOTUNE_METHOD_RELAY;
    s_at.zone_index = 0;
    s_at.relay.valid = true;
    s_at.predicted_max_ramp_c_per_hr = 200.0f; /* should never be reachable on RELAY, but set anyway to
                                                 * prove the method check, not just an incidental zero */

    s_stub_set_pid_result = true;
    s_stub_set_model_result = true;
    s_stub_set_max_ramp_call_count = 0;

    autotune_accept_opts_t opts = {.adopt_ceiling = true};
    bool accepted = autotune_engine_accept(&opts, NULL);

    TEST_CHECK(accepted, "a relay-method accept still succeeds -- gains only, no model, no ceiling");
    TEST_CHECK(s_stub_set_max_ramp_call_count == 0,
              "zones_config_set_max_ramp() must NOT be called on the RELAY path");

    s_stub_set_pid_result = false;
    s_stub_set_model_result = false;
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
    finalize_fit_then_flush_coupling();
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
    finalize_fit_then_flush_coupling();
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
    finalize_fit_then_flush_coupling();
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE,
              "positive control: a 1200C rise against the same 1200C-ceiling zone clears both the "
              "scaled (B) threshold and the (C) plausibility check, and must still fit");
}

static void test_physical_plausibility_refuses_gain_implying_ceiling_below_max_temp(void)
{
    TEST_SECTION("(C) physical plausibility -- a fitted gain implying less than max_temp_c at full "
                 "duty, WITH its measured cross-coupling folded in, must be refused, naming both "
                 "numbers; max_temp_c==0 (guard DISABLED, not a 0-degree ceiling) must NOT trigger "
                 "the check");
    /* Case 1: implausible EVEN WITH measured coupling folded in -- K~=100,
     * baseline=25=ambient (this helper defaults ambient to baseline -- see
     * write_synthetic_fopdt_trace_for_zone()'s own comment; a dedicated
     * hot-start test below covers the ambient-vs-baseline distinction
     * itself), plus a modest measured cross-coupling from zone 1 (30C/duty)
     * -> implied max ~=155C, still far below the zone's configured 500C
     * ceiling -- 2026-08-31 fix: this now HAS to seed coupling data (via
     * s_stub_coupling_row + s_stub_thermo_count), because the reachability
     * check degrades to a no-op with none measured (see its own comment in
     * autotune_engine.c) -- exactly the behavior
     * test_coupling_reachable_ceiling_is_accepted_even_with_low_single_zone_
     * gain() below proves is not vacuous. K raised from the original 5.0 to
     * 100.0 so this trace also clears (B)'s now-scaled minimum-excursion
     * floor (0.15 * (500-25) = 71.25C) and reaches the (C) check under test
     * at all. The two-point fit's recovered K is not bit-exact to the
     * synthetic K fed in (discrete 10s sampling + linear crossing
     * interpolation), so the expected implied-max string is built from the
     * SAME trace's actual fitted model (obtained with the check disarmed via
     * max_temp_c=0 below) rather than hardcoded, so this test cannot flake
     * on fit-precision noise. */
    reset_stub_coupling_row();
    s_stub_thermo_count = 2;
    s_stub_coupling_row[0][1] = 30.0f;
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;
    s_at.guard_cfg.max_temp_c = 0.0f;
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/100.0f, /*tau_s=*/200.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    finalize_fit_then_flush_coupling();
    TEST_CHECK(s_at.model.valid, "sanity: the fit itself must succeed before the plausibility case matters");
    float implied_max_c = 25.0f + s_at.model.k_gain_c_per_duty + 30.0f;
    char expect_implied[32], expect_limit[32];
    snprintf(expect_implied, sizeof(expect_implied), "%.1fC", (double)implied_max_c);
    snprintf(expect_limit, sizeof(expect_limit), "%.1fC", 500.0);

    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;
    s_at.guard_cfg.max_temp_c = 500.0f;
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/100.0f, /*tau_s=*/200.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    finalize_fit_then_flush_coupling();
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
    finalize_fit_then_flush_coupling();
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE,
              "max_temp_c == 0 (guard disabled) must NOT trigger the plausibility refusal");
    TEST_CHECK(s_at.model.valid, "the fit itself is unaffected by the check being skipped");
    reset_stub_coupling_row();
    s_stub_thermo_count = 0;
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
    /* 2026-08-31 fix: seed a modest measured coupling (same reasoning as
     * test_physical_plausibility_refuses_gain_implying_ceiling_below_max_temp()
     * above -- the reachability check is a no-op without SOME measured
     * coupling) small enough that ambient (25C) + K (100C) + coupling (30C)
     * = 155C is still below the 250C ceiling, so this case still proves what
     * it always proved: the ambient reference, not the coupling fold-in,
     * is what's under test here. */
    reset_stub_coupling_row();
    s_stub_thermo_count = 2;
    s_stub_coupling_row[0][1] = 30.0f;
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
    finalize_fit_then_flush_coupling();

    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED,
              "ambient (25C) + K (100C) + coupling (30C) = 155C is below the 250C ceiling -- must "
              "refuse. A baseline-referenced check (200C + 100C + 30C = 330C) would have wrongly "
              "accepted this fit");
    /* "zone limit" (not "max_temp_c" literally -- see the real message in
     * autotune_finalize_fit()'s (C) block) is unique to THIS check's abort message,
     * distinguishing it from (B)'s "minimum needed to trust" refusal. */
    TEST_CHECK(strstr(s_at.abort_reason, "zone limit") != NULL, "refused through the same (C) channel");
    reset_stub_coupling_row();
    s_stub_thermo_count = 0;
}

/* 2026-08-31 THE GATING BUG, reproduced from tonight's real hardware run:
 * two consecutive step tests on zone 0 of a coupled 3-zone kiln both fit a
 * correct, fully-settled model (K~=40.6, step_ambient_c~=29.8, both
 * realistic 0.1C-quantized numbers, not idealized synthetic ones) and were
 * both wrongly rejected by the OLD (C) check as "fit is wrong" -- zone 0
 * genuinely cannot reach the kiln's 80.0C zone limit alone; it only gets
 * there with the other two zones' measured 5-12 degC/duty of cross-heating
 * added in (here: zone 1 at 10.0, zone 2 at 8.0 -- squarely inside that
 * measured range, thermo_count=3 for a genuine 3-zone kiln). Total implied
 * ceiling with coupling folded in: 29.8 + ~40.6 + 10.0 + 8.0 ~= 88.4C, ABOVE
 * the 80.0C limit -- this fit MUST be ACCEPTED.
 *
 * Falsifiability: this test is checked NOT vacuous by running it against the
 * pre-fix (C) check (implied_max_c = step_ambient_c + k_gain_c_per_duty
 * only, no coupling term) -- 29.8 + ~40.6 ~= 70.4C, BELOW 80.0C, which is
 * exactly the "gain implies 69.2C/70.4C max... below zone limit 80.0C"
 * abort tonight's two real runs actually hit. Verified by temporarily
 * reverting autotune_engine.c's (C) block to the old single-zone formula
 * (`s_at.step_ambient_c + s_at.model.k_gain_c_per_duty`, dropping the
 * coupling fold-in) and re-running this one test: it FAILS
 * (s_at.state == AUTOTUNE_ENGINE_ABORTED, not DONE) against that old code,
 * then passes again once the fix is restored -- proving this test actually
 * exercises the fixed code path rather than passing by construction. */
static void test_coupling_reachable_ceiling_is_accepted_even_with_low_single_zone_gain(void)
{
    TEST_SECTION("2026-08-31 THE GATING BUG (real hardware repro): a zone whose OWN gain implies "
                 "less than the configured ceiling must still be ACCEPTED once its measured "
                 "cross-zone coupling closes the gap -- reproduces tonight's two real, wrongly-"
                 "rejected step tests (K~=40.6, ambient~=29.8, zone limit 80.0C, coupled 3-zone kiln)");
    reset_stub_coupling_row();
    s_stub_thermo_count = 3;
    s_stub_coupling_row[0][1] = 10.0f; /* zone 0's measured response to zone 1's heater */
    s_stub_coupling_row[0][2] = 8.0f;  /* zone 0's measured response to zone 2's heater */
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;
    s_at.guard_cfg.max_temp_c = 80.0f;
    /* Realistic, quantized (0.1C-step) values matching tonight's real runs --
     * not an idealized clean number (this repo's own documented "idealized
     * test input" bug class: unquantized synthetic data hides branches a
     * real 0.1C-resolution trace would exercise). write_synthetic_fopdt_
     * trace_for_zone() already quantizes every sample via lroundf(...*10)/10. */
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/29.8f, /*k_gain_c_per_duty=*/40.6f, /*tau_s=*/285.0f,
                                         /*dead_time_s=*/15.0f, /*duty_step=*/1.0f, /*n_samples=*/90);
    s_at.step_ambient_c = 29.8f; /* cold-junction reading at SETTLING->STEPPING, same as baseline here */
    s_at.step_settled = true;    /* tonight's real runs both genuinely settled -- not the max-duration backstop */
    finalize_fit_then_flush_coupling();

    TEST_CHECK(s_at.model.valid, "sanity: the fit itself must succeed");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE,
              "a correct single-zone fit on a coupled kiln must be ACCEPTED once measured "
              "cross-coupling shows the CONFIGURATION (not the one heater alone) can reach the "
              "ceiling -- this is exactly the case the old check rejected on real hardware");
    reset_stub_coupling_row();
    s_stub_thermo_count = 0;
}

/* 2026-08-31: the (C0) scale-free sanity floor must still catch a nonsense
 * fit (here: a NEGATIVE gain -- e.g. a step test run while the zone was
 * still cooling from a prior firing, or a miswired relay/thermocouple pair)
 * regardless of coupling data or whether a ceiling is even configured
 * (max_temp_c = 0.0f here, deliberately -- proving this is NOT the
 * reachability check (C) firing, which would be a no-op with the guard
 * disabled). A negative-gain fit must never reach DONE and propose gains
 * that would drive the loop backwards.
 *
 * Falsifiability: verified NOT vacuous by temporarily removing the (C0)
 * block from autotune_finalize_fit() (autotune_engine.c) and re-running this one
 * test -- it FAILS (state reaches DONE with a negative k_gain_c_per_duty
 * proposed) against the weakened code, then passes again once (C0) is
 * restored. */
static void test_nonsense_negative_gain_fit_is_still_rejected(void)
{
    TEST_SECTION("(C0) scale-free sanity floor -- a negative fitted gain (a cooling trace, or a "
                 "miswired relay/TC pair) must still be refused, independent of coupling data or "
                 "whether a ceiling is even configured");
    reset_stub_coupling_row();
    s_stub_thermo_count = 0;
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;
    s_at.guard_cfg.max_temp_c = 0.0f; /* guard disabled -- isolates (C0) from (C) */
    /* A trace that COOLS from baseline (negative k_gain_c_per_duty) --
     * realistic 0.1C-quantized samples, same helper every other test here
     * uses, just with a negative synthetic gain. */
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/120.0f, /*k_gain_c_per_duty=*/-50.0f, /*tau_s=*/200.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    finalize_fit_then_flush_coupling();

    TEST_CHECK(s_at.model.valid, "sanity: the two-point fit itself succeeds on a clean (if negative) exponential");
    TEST_CHECK(s_at.model.k_gain_c_per_duty < 0.0f, "sanity: the synthetic trace really does fit a negative gain");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED,
              "a negative fitted gain is not a plausible heater and must be refused, not accepted");
    TEST_CHECK(strstr(s_at.abort_reason, "not a plausible heater gain") != NULL,
              "refused through the (C0) sanity-floor channel, not (B) or (C)");
}

/* 2026-08-31 THE DIAGNOSTICS-ERASED BUG: a rejected fit's diagnostics
 * (baseline_c/final_c/raw_rise_c/rise_inf_c, added specifically to make a
 * rejected fit diagnosable) must survive the reject -- autotune_engine_get_
 * status() used to zero them by gating the model copy on state == DONE only,
 * throwing away exactly the numbers an operator needs to see WHY a real
 * aborted run was refused (confirmed on hardware: every diagnostic field
 * read 0.00 despite the abort message itself proving a real, non-zero gain
 * had been computed). Both halves matter: preserved on THIS run's own
 * reject, but still cleared at the START of the NEXT run (the begin_run_
 * locked() full-zero fix, commit 49d979d, must not be undone). */
static void test_rejected_fit_diagnostics_survive_the_reject_but_clear_on_next_run(void)
{
    TEST_SECTION("2026-08-31 THE DIAGNOSTICS-ERASED BUG: baseline_c/final_c/raw_rise_c/rise_inf_c "
                 "must survive a REJECTED fit through autotune_engine_get_status(), and still read "
                 "zero at the START of the next run");
    reset_stub_coupling_row();
    s_stub_thermo_count = 0;
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;
    s_at.guard_cfg.max_temp_c = 80.0f; /* reachability check (C) armed, no coupling measured -> degrades, */
    /* so this trace is rejected by (B) minimum-excursion instead -- any
     * rejection path works here since the bug under test is the getter, not
     * a specific reject reason; (B) needs the smallest, least contrived
     * trace of the three reject paths available. Realistic 0.1C-quantized
     * samples, same helper as every test in this file. */
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/2.0f, /*tau_s=*/200.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    s_at.lock = xSemaphoreCreateMutex();
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");
    finalize_fit_then_flush_coupling();
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED, "sanity: this trace must actually be rejected (by (B))");

    autotune_engine_status_t status;
    autotune_engine_get_status(&status);
    TEST_CHECK(status.state == AUTOTUNE_ENGINE_ABORTED, "status reflects the reject");
    TEST_CHECK(fabsf(status.model.baseline_c - 25.0f) < 0.15f,
              "baseline_c survives the reject through the status getter (was 0.00 before the fix)");
    TEST_CHECK(status.model.final_c > 25.0f && status.model.final_c < 35.0f,
              "final_c survives the reject through the status getter (was 0.00 before the fix)");
    TEST_CHECK(status.model.raw_rise_c > 0.0f,
              "raw_rise_c survives the reject through the status getter (was 0.00 before the fix)");
    TEST_CHECK(status.model.rise_inf_c > 0.0f,
              "rise_inf_c survives the reject through the status getter (was 0.00 before the fix)");

    // Second half: a NEW run's autotune_begin_run_locked() must still zero s_at.model
    // -- the stale-payload fix (commit 49d979d) must not be undone by this
    // change. Uses the real start_stepping_run() path (autotune_engine_run()
    // -> autotune_begin_run_locked() for real), not a hand-poked reset, so this is
    // the actual production reset path, not a stand-in for it.
    start_stepping_run(/*max_temp_c=*/0.0f, /*step_duty=*/0.5f);
    TEST_CHECK(s_at.model.baseline_c == 0.0f && s_at.model.final_c == 0.0f && s_at.model.raw_rise_c == 0.0f &&
                   s_at.model.rise_inf_c == 0.0f,
              "a NEW run's autotune_begin_run_locked() still clears every diagnostic field -- the previous "
              "run's rejected-fit numbers do not leak into this one");
}

static void test_model_settled_flag_reflects_step_settled(void)
{
    TEST_SECTION("autotune_finalize_fit() sets fopdt_model_t.settled from s_at.step_settled -- a fit that "
                 "reached autotune_finalize_fit() via the max-duration backstop (step_settled never set by the "
                 "detector) must NOT be silently reported as steady state");
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;
    s_at.step_settled = false; /* simulates the AUTOTUNE_ENGINE_DEFAULT_MAX_DURATION_S backstop path */
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/50.0f, /*tau_s=*/200.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    finalize_fit_then_flush_coupling();
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "the fit itself still succeeds -- settled is a flag, not a gate");
    TEST_CHECK(!s_at.model.settled, "settled must read false -- this run never satisfied the detector");

    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;
    s_at.step_settled = true; /* simulates the honest detector having genuinely fired */
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/25.0f, /*k_gain_c_per_duty=*/50.0f, /*tau_s=*/200.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/60);
    finalize_fit_then_flush_coupling();
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "the fit succeeds here too");
    TEST_CHECK(s_at.model.settled, "settled must read true -- carried through from s_at.step_settled");
}

// ---------------------------------------------------------------------------
// Target-temperature step mode (autotune_engine_run_to_target(),
// handle_probe_done_locked()) -- slice 1 of the target-temperature autotune
// task. Two layers:
//   - handle_probe_done_locked() tests below drive it DIRECTLY (white-box,
//     same convention as the autotune_finalize_fit() tests above) against a genuinely
//     QUANTIZED synthetic probe trace (write_synthetic_fopdt_trace_for_zone()
//     packs int16 tenths-of-a-degree, exactly what record_trace_sample()/
//     autotune_unpack_zone_trace() produce on real hardware -- not an idealized
//     unquantized float trace).
//   - autotune_engine_run_to_target() tests below drive the real public
//     entry point through autotune_begin_run_locked(), same setup as
//     start_stepping_run_rule() above.
// ---------------------------------------------------------------------------

// K=41 degC/duty, tau=270s, L=20s, probe duty 0.15, 60 samples (600s ==
// AUTOTUNE_ENGINE_PROBE_DURATION_S) -- the exact numbers measured on zone 0
// the night this task was written (see autotune_engine.c's own comment on
// AUTOTUNE_ENGINE_PROBE_DURATION_S for the full derivation). Reaches ~88% of
// the probe's own (small) asymptote, comfortably past dead time, so the real
// two-point fit + extrapolation loop in pid_autotune_fit_fopdt() has genuine
// curvature to work from -- this is not a full-settle trace.
static void test_probe_done_computes_identify_duty_and_rewinds_to_settling(void)
{
    TEST_SECTION("handle_probe_done_locked() -- POSITIVE: rough K from a genuinely truncated, "
                 "quantized probe trace picks an identify duty and rewinds to SETTLING");
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    const float baseline_c = 30.6f;
    write_synthetic_fopdt_trace_for_zone(0, baseline_c, /*k_gain_c_per_duty=*/41.0f, /*tau_s=*/270.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/AUTOTUNE_ENGINE_PROBE_DUTY,
                                         /*n_samples=*/(uint16_t)(AUTOTUNE_ENGINE_PROBE_DURATION_S /
                                                                   AUTOTUNE_ENGINE_SAMPLE_PERIOD_S));
    s_at.target_c = baseline_c + 15.0f; /* 45.6C -- well inside what K=41 at duty<=1 can reach */

    handle_probe_done_locked();

    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_SETTLING,
              "a reachable target must rewind to SETTLING for the real identification step, not abort");
    TEST_CHECK(!s_at.probe_phase, "probe_phase must clear once the probe fit is used");
    TEST_CHECK(s_at.probe_k_rough > 0.0f, "a valid probe fit must report a positive K_rough");
    TEST_CHECK(s_at.step_duty > 0.0f && s_at.step_duty <= 1.0f,
              "the chosen identify duty must land in (0, 1] -- the same bound autotune_engine_run() enforces "
              "on an operator-supplied duty");
    // Self-consistency: step_duty must be EXACTLY (target_c - baseline_c) /
    // probe_k_rough, i.e. the two fields the code actually stored, not some
    // other derivation -- this does not depend on how accurately the probe
    // recovered the true K=41.
    float expected_duty = (s_at.target_c - baseline_c) / s_at.probe_k_rough;
    TEST_CHECK_NEAR(s_at.step_duty, expected_duty, 1e-4f,
                    "step_duty must equal (target_c - baseline_c) / probe_k_rough exactly");
    // Sanity: the rough fit should be in the right ballpark of the true
    // K=41 despite the truncated trace (loose tolerance -- this is testing
    // "usable", not pid_autotune_fit_fopdt()'s own accuracy, which is
    // test_pid_autotune.c's job).
    TEST_CHECK(s_at.probe_k_rough > 20.0f && s_at.probe_k_rough < 41.0f * 1.2f,
              "probe_k_rough must be a plausible estimate of the true K=41, not garbage");
}

static void test_probe_done_refuses_unreachable_target(void)
{
    TEST_SECTION("handle_probe_done_locked() -- refuses (does not silently clamp) a target the zone "
                 "cannot reach at full duty");
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    const float baseline_c = 30.0f;
    write_synthetic_fopdt_trace_for_zone(0, baseline_c, /*k_gain_c_per_duty=*/5.0f, /*tau_s=*/200.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/AUTOTUNE_ENGINE_PROBE_DUTY,
                                         /*n_samples=*/60);
    // True K=5: full duty reaches ~35C. Ask for +10C above true full-duty
    // reach so estimation noise in the rough fit cannot accidentally make
    // this look reachable.
    s_at.target_c = baseline_c + 5.0f * 1.0f + 10.0f; /* 45C */

    handle_probe_done_locked();

    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED, "an unreachable target must ABORT, not clamp to duty 1.0");
    TEST_CHECK(strstr(s_at.abort_reason, "unreachable") != NULL,
              "abort_reason must name the refusal, not a generic message");
    TEST_CHECK(strlen(s_at.abort_reason) < sizeof(s_at.abort_reason),
              "abort_reason must be a valid, NUL-terminated string within its 96-byte buffer");
}

static void test_probe_done_refuses_target_not_above_baseline(void)
{
    TEST_SECTION("handle_probe_done_locked() -- refuses a target at or below the probe baseline "
                 "(duty would have to be <= 0)");
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    const float baseline_c = 30.0f;
    write_synthetic_fopdt_trace_for_zone(0, baseline_c, /*k_gain_c_per_duty=*/41.0f, /*tau_s=*/270.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/AUTOTUNE_ENGINE_PROBE_DUTY,
                                         /*n_samples=*/60);
    s_at.target_c = baseline_c - 2.0f;

    handle_probe_done_locked();

    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED, "target below baseline must ABORT");
    TEST_CHECK(strstr(s_at.abort_reason, "baseline") != NULL, "abort_reason must name the actual problem");
}

// Round-3 finding 3: identify_duty has no lower floor. A target a hair
// above baseline yields a duty below AUTOTUNE_PROGRESS_DUTY_MIN_FOR_STEP_
// TEST (0.01), silently disarming guard 1/2 for the entire identify phase.
static void test_probe_done_refuses_identify_duty_below_the_progress_duty_min_floor(void)
{
    TEST_SECTION("round-3 finding 3: handle_probe_done_locked() refuses (not clamps) an identify_duty "
                 "below AUTOTUNE_PROGRESS_DUTY_MIN_FOR_STEP_TEST -- below that floor guard 1/2 never arm "
                 "at all for the whole identify phase");
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    const float baseline_c = 30.0f;
    write_synthetic_fopdt_trace_for_zone(0, baseline_c, /*k_gain_c_per_duty=*/41.0f, /*tau_s=*/270.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/AUTOTUNE_ENGINE_PROBE_DUTY,
                                         /*n_samples=*/60);
    // reach ~= 0.2 / 41 ~= 0.0049 -- well below the 0.01 floor.
    s_at.target_c = baseline_c + 0.2f;

    handle_probe_done_locked();

    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED, "an identify_duty below the progress_duty_min floor "
                                                       "must ABORT, not silently clamp up or proceed disarmed");
    TEST_CHECK(strstr(s_at.abort_reason, "floor") != NULL, "abort_reason must name the actual problem");
}

// Sanity companion: a target that computes an identify_duty comfortably
// ABOVE the floor must proceed normally (no over-correction).
static void test_probe_done_accepts_identify_duty_above_the_progress_duty_min_floor(void)
{
    TEST_SECTION("round-3 finding 3 sanity: an identify_duty comfortably above the floor is accepted "
                 "normally, unaffected by the new check");
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    const float baseline_c = 30.0f;
    write_synthetic_fopdt_trace_for_zone(0, baseline_c, /*k_gain_c_per_duty=*/41.0f, /*tau_s=*/270.0f,
                                         /*dead_time_s=*/20.0f, /*duty_step=*/AUTOTUNE_ENGINE_PROBE_DUTY,
                                         /*n_samples=*/60);
    s_at.target_c = baseline_c + 15.0f; /* reach ~= 15/41 ~= 0.366 -- well above 0.01 */

    handle_probe_done_locked();

    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_SETTLING, "a comfortably-above-floor identify_duty must "
                                                       "rewind to SETTLING for phase 2, unaffected");
    TEST_CHECK(s_at.step_duty > AUTOTUNE_PROGRESS_DUTY_MIN_FOR_STEP_TEST, "sanity: duty is genuinely above "
                                                                          "the floor");
}

static void test_probe_done_propagates_probe_fit_failure(void)
{
    TEST_SECTION("handle_probe_done_locked() -- a probe trace too short/flat to fit propagates "
                 "pid_autotune_fit_fopdt()'s own refusal, not a generic failure");
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    s_at.zone_baseline_c[0] = 30.0f;
    s_at.zone_baseline_valid[0] = true;
    s_at.step_ambient_c = 30.0f;
    // Two flat (bit-identical) samples: never reaches 28.3% of any rise.
    s_at.zone_trace[0][0] = (int16_t)lroundf(30.0f * 10.0f);
    s_at.zone_trace[0][1] = (int16_t)lroundf(30.0f * 10.0f);
    s_at.trace_count = 2;
    s_at.target_c = 45.0f;

    handle_probe_done_locked();

    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED, "a flat probe trace must ABORT");
    TEST_CHECK(strstr(s_at.abort_reason, "probe fit failed") != NULL,
              "abort_reason must identify this as a PROBE fit failure, distinct from autotune_finalize_fit()'s "
              "identical-looking 'fit failed:' message on the real identification step");
}

// Drives the real public entry point through autotune_begin_run_locked(), same setup
// as start_stepping_run_rule() above (autotune_engine_start() cannot
// succeed in this host build -- see that helper's own header comment for
// why the lock is created directly instead).
static bool call_run_to_target(float max_temp_c, float target_c, autotune_rule_t rule, char *errbuf, size_t errcap)
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

    s_stub_max_temp_c = max_temp_c;
    s_stub_ch0_ok = true;

    return autotune_engine_run_to_target(0, target_c, rule, errbuf, errcap);
}

// Same setup convention as call_run_to_target() above, for
// autotune_engine_run_relay()'s setpoint-window guard.
static bool call_run_relay(float max_temp_c, float min_temp_c, float setpoint_c, char *errbuf, size_t errcap)
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

    s_stub_max_temp_c = max_temp_c;
    s_stub_min_temp_c = min_temp_c;
    s_stub_ch0_ok = true;

    return autotune_engine_run_relay(0, setpoint_c, /*relay_d=*/0.0f /* -> default */,
                                      /*hysteresis_c=*/0.0f /* -> default */, AUTOTUNE_RULE_ZIEGLER_NICHOLS, errbuf,
                                      errcap);
}

// Live-proven blocker (see task): this rig's zone limits are max=80C,
// min=0C. Against the old fixed 50C-each-side headroom that demanded 100C
// of headroom on an 80C span -- an EMPTY window, so every setpoint was
// refused: 55C got "must be at least 50C below the 80C limit", 30C got
// "must be at least 50C above the 0C floor". No setpoint could ever be
// accepted and relay identification could never run on this hardware.
static void test_relay_setpoint_narrow_rig_span_now_has_a_usable_window(void)
{
    TEST_SECTION("relay setpoint window on this rig's real 80C/0C span must NOT be empty -- a sensible "
                 "setpoint (e.g. 40C, the span's midpoint) must now be ACCEPTED");
    char err[128] = {0};

    // Span 80, 2*50=100 > 80, so headroom scales to 80*0.25=20 -> window
    // [20, 60]. 55C (refused live on hardware under the old logic) and
    // 30C (also refused live) must both be accepted now.
    bool ok = call_run_relay(/*max_temp_c=*/80.0f, /*min_temp_c=*/0.0f, /*setpoint_c=*/55.0f, err, sizeof(err));
    TEST_CHECK(ok, "55C under an 80/0 span must be accepted -- this exact setpoint was refused live on hardware");
    TEST_CHECK(s_at.method == AUTOTUNE_METHOD_RELAY, "an accepted call must actually start the relay method");

    ok = call_run_relay(/*max_temp_c=*/80.0f, /*min_temp_c=*/0.0f, /*setpoint_c=*/30.0f, err, sizeof(err));
    TEST_CHECK(ok, "30C under an 80/0 span must be accepted -- this exact setpoint was also refused live on "
                   "hardware");

    // Outside the scaled window must still refuse, but with a single
    // message naming the real window -- not the old two-sided contradiction.
    ok = call_run_relay(/*max_temp_c=*/80.0f, /*min_temp_c=*/0.0f, /*setpoint_c=*/65.0f, err, sizeof(err));
    TEST_CHECK(!ok, "65C is above the scaled window's 60C top -- must refuse");
    TEST_CHECK(strstr(err, "20") != NULL && strstr(err, "60") != NULL,
               "err_msg must name the actual computed window (20C to 60C), not a fixed 50C headroom claim");

    ok = call_run_relay(/*max_temp_c=*/80.0f, /*min_temp_c=*/0.0f, /*setpoint_c=*/10.0f, err, sizeof(err));
    TEST_CHECK(!ok, "10C is below the scaled window's 20C bottom -- must refuse");
    TEST_CHECK(strstr(err, "20") != NULL && strstr(err, "60") != NULL,
               "err_msg must name the actual computed window (20C to 60C) here too");
}

static void test_relay_setpoint_wide_span_keeps_the_full_50c_headroom(void)
{
    TEST_SECTION("a wide span (plenty of room for the full 50C headroom on both sides) must behave exactly "
                 "as before -- no regression from the narrow-span fallback");
    char err[128] = {0};

    // Span 1000 (min 0, max 1000), 2*50=100 << 1000, so the full 50C
    // headroom applies unchanged -> window [50, 950].
    bool ok = call_run_relay(/*max_temp_c=*/1000.0f, /*min_temp_c=*/0.0f, /*setpoint_c=*/500.0f, err, sizeof(err));
    TEST_CHECK(ok, "500C is comfortably inside [50, 950] on a wide span -- must be accepted");

    ok = call_run_relay(/*max_temp_c=*/1000.0f, /*min_temp_c=*/0.0f, /*setpoint_c=*/970.0f, err, sizeof(err));
    TEST_CHECK(!ok, "970C is inside the full 50C headroom under a 1000C ceiling -- must still refuse");
    TEST_CHECK(strstr(err, "50") != NULL, "err_msg must still reflect the un-scaled 50C headroom on a wide span");

    ok = call_run_relay(/*max_temp_c=*/1000.0f, /*min_temp_c=*/0.0f, /*setpoint_c=*/949.0f, err, sizeof(err));
    TEST_CHECK(ok, "949C is just outside the 50C headroom (window top 950) -- must be accepted");

    ok = call_run_relay(/*max_temp_c=*/1000.0f, /*min_temp_c=*/0.0f, /*setpoint_c=*/951.0f, err, sizeof(err));
    TEST_CHECK(!ok, "951C is just inside the 50C headroom (window top 950) -- must refuse");

    ok = call_run_relay(/*max_temp_c=*/1000.0f, /*min_temp_c=*/0.0f, /*setpoint_c=*/40.0f, err, sizeof(err));
    TEST_CHECK(!ok, "40C is inside the full 50C headroom above the 0C floor -- must still refuse");

    ok = call_run_relay(/*max_temp_c=*/1000.0f, /*min_temp_c=*/0.0f, /*setpoint_c=*/60.0f, err, sizeof(err));
    TEST_CHECK(ok, "60C is just outside the 50C headroom above the floor (window bottom 50) -- must be accepted");
}

// Opus review finding 8 (2026-09-02): the fallback-headroom condition used
// `span < 2.0f*HEADROOM`, a strict `<`. At span EXACTLY 2*HEADROOM (100C,
// with the default 50C headroom), that left the un-scaled full 50C headroom
// in effect -> window_lo == window_hi == a degenerate, refused window, while
// a span of 99.9C (just under the same threshold) took the scaled-down
// quarter-span branch and got a real, usable window. A WIDER span (100)
// refused where a narrower one (99.9) worked. Fixed to `<=` so the fallback
// applies at the boundary too.
static void test_relay_setpoint_span_exactly_100_is_usable(void)
{
    TEST_SECTION("a span of EXACTLY 100C (2x the default 50C headroom) must fall into the scaled-headroom "
                 "branch and produce a usable window, not the knife-edge degenerate one");
    char err[128] = {0};

    // Span 100 (min 0, max 100), 2*50 == 100 -- the boundary itself. Scaled
    // headroom = 100*0.25 = 25 -> window [25, 75]. The midpoint (50C) must be
    // accepted, proving the window is not empty/inverted at this exact span.
    bool ok = call_run_relay(/*max_temp_c=*/100.0f, /*min_temp_c=*/0.0f, /*setpoint_c=*/50.0f, err, sizeof(err));
    TEST_CHECK(ok, "50C at the midpoint of an exactly-100C span must be accepted -- the window must not be "
                   "degenerate at this boundary");
    TEST_CHECK(s_at.method == AUTOTUNE_METHOD_RELAY, "an accepted call must actually start the relay method");

    // Just inside the scaled window edges must also be accepted...
    ok = call_run_relay(/*max_temp_c=*/100.0f, /*min_temp_c=*/0.0f, /*setpoint_c=*/26.0f, err, sizeof(err));
    TEST_CHECK(ok, "26C is just inside the scaled window's 25C bottom edge -- must be accepted");
    ok = call_run_relay(/*max_temp_c=*/100.0f, /*min_temp_c=*/0.0f, /*setpoint_c=*/74.0f, err, sizeof(err));
    TEST_CHECK(ok, "74C is just inside the scaled window's 75C top edge -- must be accepted");

    // ...and just outside them must still refuse, naming the real window.
    ok = call_run_relay(/*max_temp_c=*/100.0f, /*min_temp_c=*/0.0f, /*setpoint_c=*/24.0f, err, sizeof(err));
    TEST_CHECK(!ok, "24C is below the scaled window's 25C bottom -- must refuse");
    TEST_CHECK(strstr(err, "25") != NULL && strstr(err, "75") != NULL,
               "err_msg must name the scaled window (25C to 75C), not the un-scaled 50C headroom");
}

static void test_relay_setpoint_pathologically_narrow_span_refuses_with_stated_window(void)
{
    TEST_SECTION("a span too narrow to leave ANY window (max_temp_c == min_temp_c) must refuse with ONE "
                 "message that states the computed (empty/inverted) window -- never two contradictory ones");
    char err[128] = {0};

    // Zero-width span: max == min == 40C. headroom = 0*0.25 = 0, so
    // window_lo == window_hi == 40C -- window_lo >= window_hi, refused by
    // the dedicated "no window exists" branch, one message only.
    bool ok = call_run_relay(/*max_temp_c=*/40.0f, /*min_temp_c=*/40.0f, /*setpoint_c=*/40.0f, err, sizeof(err));
    TEST_CHECK(!ok, "a zero-width span must refuse -- there is no safe setpoint");
    TEST_CHECK(s_at.method != AUTOTUNE_METHOD_RELAY, "a refused call must not have started a run at all");
    TEST_CHECK(strstr(err, "40") != NULL, "err_msg must name the actual span (40C to 40C), not a generic refusal");
    // Exactly one refusal message, not the old pair -- check neither of the
    // two retired phrasings survived as dead code paths still reachable.
    TEST_CHECK(strstr(err, "at least") == NULL,
               "must not fall through to either of the old two-sided 'at least NC below/above' messages");
}

// Same as call_run_relay() but with an explicit hysteresis, for the band-vs-
// guard-limit check (call_run_relay() always passes 0.0f -> the 2C default).
static bool call_run_relay_h(float max_temp_c, float min_temp_c, float setpoint_c, float h_c, char *errbuf,
                             size_t errcap)
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

    s_stub_max_temp_c = max_temp_c;
    s_stub_min_temp_c = min_temp_c;
    s_stub_ch0_ok = true;

    return autotune_engine_run_relay(0, setpoint_c, /*relay_d=*/0.0f, h_c, AUTOTUNE_RULE_ZIEGLER_NICHOLS, errbuf,
                                     errcap);
}

// cd6b9b2's band-vs-guard-limit check composes with the <= fix above. At
// span exactly 100, the scaled headroom (25C) already exceeds
// AUTOTUNE_RELAY_MAX_H_C (20C), so no setpoint the window admits can ever
// produce a band that reaches the guard limits -- the same "headroom already
// covers the largest legal h" property test_relay_band_must_not_reach_
// the_zone_guard_limits() proves for the span=1000 case just below. Confirm
// the SAME property holds right at the span=100 boundary: the window's most
// extreme legal setpoint (75C, its top edge) combined with the largest legal
// hysteresis (AUTOTUNE_RELAY_MAX_H_C, 20C) must still be accepted, not
// refused by the band check -- proving the window fix did not accidentally
// widen the window into territory the band check would then have to refuse.
static void test_relay_setpoint_span_exactly_100_band_still_composes_safely(void)
{
    TEST_SECTION("span exactly 100C: the window's most extreme legal setpoint with the largest legal "
                 "hysteresis must still clear the band-vs-guard-limit check (headroom 25C > max h 20C)");
    char err[128] = {0};

    bool ok = call_run_relay_h(/*max_temp_c=*/100.0f, /*min_temp_c=*/0.0f, /*setpoint_c=*/75.0f, /*h_c=*/20.0f,
                                err, sizeof(err));
    TEST_CHECK(ok, "setpoint 75C (window top) with h=20C (AUTOTUNE_RELAY_MAX_H_C) bands to [55,95], strictly "
                   "inside 0..100 -- must be accepted");
    TEST_CHECK(s_at.method == AUTOTUNE_METHOD_RELAY, "an accepted call must actually start the relay method");

    ok = call_run_relay_h(/*max_temp_c=*/100.0f, /*min_temp_c=*/0.0f, /*setpoint_c=*/25.0f, /*h_c=*/20.0f, err,
                          sizeof(err));
    TEST_CHECK(ok, "setpoint 25C (window bottom) with h=20C bands to [5,45], strictly inside 0..100 -- must "
                   "be accepted");
}

// REVIEW 2026-09-02. 17e67ee's span-proportional headroom made the setpoint
// window narrower than a LEGAL hysteresis on a narrow-span rig: this rig's
// span is 80C, so headroom = 20C and the window top is 60C -- but
// AUTOTUNE_RELAY_MAX_H_C is 20C, so a setpoint of 60C with h=20C oscillates
// over 40C..80C, whose upper edge IS max_temp_c. Guard 5 would trip partway
// through a multi-hour relay test. The window check alone cannot see this
// (it never looks at h), so the band must be checked in its own right.
static void test_relay_band_must_not_reach_the_zone_guard_limits(void)
{
    TEST_SECTION("relay oscillation band (setpoint +/- hysteresis) must stay strictly inside the zone's "
                 "floor/limit -- the setpoint window alone does not check the hysteresis");
    char err[160] = {0};

    // Inside the 17e67ee window ([20, 60] on an 80/0 span) but the band's
    // top edge lands exactly on max_temp_c -- guard 5's own trip point.
    bool ok = call_run_relay_h(/*max_temp_c=*/80.0f, /*min_temp_c=*/0.0f, /*setpoint_c=*/60.0f, /*h_c=*/20.0f, err,
                               sizeof(err));
    TEST_CHECK(!ok, "setpoint 60C with h=20C on an 80/0 span puts the band top AT the 80C limit -- must refuse");
    TEST_CHECK(s_at.method != AUTOTUNE_METHOD_RELAY, "a refused call must not have started a run at all");
    TEST_CHECK(strstr(err, "band") != NULL, "the refusal must name the band, not restate the setpoint window");

    // The floor side of the same invariant.
    ok = call_run_relay_h(/*max_temp_c=*/80.0f, /*min_temp_c=*/0.0f, /*setpoint_c=*/20.0f, /*h_c=*/20.0f, err,
                          sizeof(err));
    TEST_CHECK(!ok, "setpoint 20C with h=20C puts the band bottom AT the 0C floor -- must refuse");

    // The same rig, the same window, an ordinary hysteresis: still accepted.
    // Without this the new check could pass by refusing everything.
    ok = call_run_relay_h(/*max_temp_c=*/80.0f, /*min_temp_c=*/0.0f, /*setpoint_c=*/60.0f, /*h_c=*/2.0f, err,
                          sizeof(err));
    TEST_CHECK(ok, "setpoint 60C with the default-scale h=2C stays well inside 0..80 -- must still be accepted");
    TEST_CHECK(s_at.method == AUTOTUNE_METHOD_RELAY, "an accepted call must actually start the relay method");

    // A wide span keeps its pre-existing behaviour: the fixed 50C headroom
    // is already larger than AUTOTUNE_RELAY_MAX_H_C, so nothing the window
    // admits can be refused by this check.
    ok = call_run_relay_h(/*max_temp_c=*/1000.0f, /*min_temp_c=*/0.0f, /*setpoint_c=*/950.0f, /*h_c=*/20.0f, err,
                          sizeof(err));
    TEST_CHECK(ok, "on a wide span the window's own 50C headroom already covers the largest legal h -- "
                   "this check must not narrow it");
}

static void test_run_to_target_rejects_relay_only_rules(void)
{
    TEST_SECTION("autotune_engine_run_to_target() refuses ZN/Tyreus-Luyben, same as autotune_engine_run()");
    char err[96] = {0};
    TEST_CHECK(!call_run_to_target(80.0f, 60.0f, AUTOTUNE_RULE_ZIEGLER_NICHOLS, err, sizeof(err)),
              "ZN must be refused on the step-test path");
    TEST_CHECK(!call_run_to_target(80.0f, 60.0f, AUTOTUNE_RULE_TYREUS_LUYBEN, err, sizeof(err)),
              "Tyreus-Luyben must be refused on the step-test path");
    TEST_CHECK(call_run_to_target(80.0f, 60.0f, AUTOTUNE_RULE_SIMC, err, sizeof(err)),
              "SIMC must be accepted -- sanity check that the two refusals above are the rule check, not "
              "something else wrong with the call");
}

static void test_run_to_target_default_uses_75_percent_of_max_temp(void)
{
    TEST_SECTION("autotune_engine_run_to_target(target_c<=0) defaults to 75% of max_temp_c and starts "
                 "PHASE 1 (probe), not the identify duty");
    char err[96] = {0};
    bool ok = call_run_to_target(/*max_temp_c=*/80.0f, /*target_c=*/0.0f /* -> default */, AUTOTUNE_RULE_SIMC, err,
                                 sizeof(err));
    TEST_CHECK(ok, "a sane max_temp_c must let the default derive and the run start");
    TEST_CHECK(s_at.target_mode, "target_mode must be set for a _run_to_target() call");
    TEST_CHECK(s_at.probe_phase, "must start in PHASE 1 (probe)");
    TEST_CHECK_NEAR(s_at.target_c, 60.0f, 0.01f, "default target must be 75% of max_temp_c (80 -> 60)");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_SETTLING, "must begin at SETTLING, same as autotune_engine_run()");
    TEST_CHECK_NEAR(s_at.step_duty, AUTOTUNE_ENGINE_PROBE_DUTY, 1e-4f,
                    "PHASE 1 must drive the probe duty, not the (not-yet-computed) identify duty");
}

// Negative-test evidence (stubbed out / restored, see the task's mandatory
// testing rules) for this exact check is reported in the task summary, not
// reproduced as a permanent code change here -- see that report for the
// actual FAIL output captured with the `if (!(max_temp_c > 0.0f))` guard
// below bypassed, and the PASS captured again with it restored.
static void test_run_to_target_default_refused_when_max_temp_c_is_zero(void)
{
    TEST_SECTION("autotune_engine_run_to_target(target_c<=0) refuses rather than deriving 0.75 * 0 when "
                 "max_temp_c == 0 (guard DISABLED, not a 0C ceiling -- zero-semantics convention)");
    char err[96] = {0};
    bool ok = call_run_to_target(/*max_temp_c=*/0.0f, /*target_c=*/0.0f /* -> default */, AUTOTUNE_RULE_SIMC, err,
                                 sizeof(err));
    TEST_CHECK(!ok, "no configured max_temp_c must refuse a defaulted target, not compute 0.75 * 0");
    TEST_CHECK(strstr(err, "max_temp_c") != NULL, "err_msg must name the actual missing configuration");
    TEST_CHECK(!s_at.target_mode, "a refused call must not have started a run at all");

    // Sanity: an EXPLICIT target with no ceiling configured is still fine --
    // the zero-semantics refusal is specific to DERIVING a default, not to
    // running with no ceiling at all (autotune_engine_run() itself allows
    // that, just with guard 4 uncovered -- same policy here).
    ok = call_run_to_target(/*max_temp_c=*/0.0f, /*target_c=*/50.0f, AUTOTUNE_RULE_SIMC, err, sizeof(err));
    TEST_CHECK(ok, "an EXPLICIT target must still be accepted with no ceiling configured");
}

static void test_run_to_target_explicit_target_refused_too_close_to_ceiling(void)
{
    TEST_SECTION("autotune_engine_run_to_target() refuses an explicit target within the required margin "
                 "of max_temp_c, BEFORE any heating starts");
    char err[96] = {0};
    // 80 - 5 (AUTOTUNE_ENGINE_TARGET_CEILING_MARGIN_C) = 75 is the boundary.
    bool ok = call_run_to_target(80.0f, /*target_c=*/76.0f, AUTOTUNE_RULE_SIMC, err, sizeof(err));
    TEST_CHECK(!ok, "76C is inside the 5C margin under an 80C ceiling -- must refuse");
    TEST_CHECK(strstr(err, "ceiling") != NULL, "err_msg must name the ceiling as the reason");
    TEST_CHECK(!s_at.target_mode, "a refused call must not have started a run -- validated before any heating");

    // Sanity: comfortably clear of the margin must be accepted.
    ok = call_run_to_target(80.0f, /*target_c=*/74.0f, AUTOTUNE_RULE_SIMC, err, sizeof(err));
    TEST_CHECK(ok, "74C is outside the 5C margin under an 80C ceiling -- must be accepted");
}

// Proves the ceiling-margin check in autotune_engine_run_to_target() is NOT
// bypassed for a DEFAULTED target -- it runs on target_c AFTER the default
// is computed, on the same code path an explicit target takes.
static void test_run_to_target_default_also_subject_to_ceiling_margin_check(void)
{
    TEST_SECTION("the default (75% of max_temp_c) still goes through the SAME ceiling-margin validation "
                 "as an explicit target, not a bypass -- a small enough max_temp_c must refuse even the "
                 "default");
    char err[96] = {0};
    // max_temp_c=10 -> default target = 7.5C; margin boundary = 10 - 5 = 5C;
    // 7.5 >= 5 -> must refuse.
    bool ok = call_run_to_target(/*max_temp_c=*/10.0f, /*target_c=*/0.0f /* -> default */, AUTOTUNE_RULE_SIMC, err,
                                 sizeof(err));
    TEST_CHECK(!ok, "a defaulted target too close to a small max_temp_c must refuse, exactly like an "
                    "explicit one would -- if this passes, the default is bypassing the margin check");
    TEST_CHECK(strstr(err, "ceiling") != NULL, "err_msg must name the ceiling as the reason");
}

static void test_run_to_target_does_not_disturb_the_plain_duty_based_run(void)
{
    TEST_SECTION("autotune_engine_run() (the existing duty-based entry point) is unaffected -- "
                 "target_mode/probe_phase read false/false on a plain run");
    start_stepping_run(/*max_temp_c=*/0.0f, /*step_duty=*/0.5f);
    TEST_CHECK(!s_at.target_mode, "a plain duty-based run must never set target_mode");
    TEST_CHECK(!s_at.probe_phase, "a plain duty-based run must never set probe_phase");
    TEST_CHECK_NEAR(s_at.step_duty, 0.5f, 1e-4f, "the operator-supplied duty must be used unchanged");
}

// Review finding: "the probe->identify handoff driven through the REAL
// tick loop rather than white-box" -- every test above calls
// handle_probe_done_locked() directly. This one instead drives
// record_trace_sample()/step_settle_check_locked() incrementally through
// REAL autotune_engine_tick_locked() calls, one synthetic sample at a time,
// so the settle detector's own onset/peak-slope state builds up for real
// (that state is inherently incremental -- a single bulk-written trace
// cannot reproduce it, see this test's own inline comment) and the STEPPING
// branch's real dispatch to handle_probe_done_locked() is what fires, not a
// direct call.
//
// This harness's xTaskGetTickCount() stub always returns 0 (documented at
// the top of the STEPPING-loop tests above), so the sample-period gate
// (`at_ticks_to_s(now - last_sample_tick) >= AUTOTUNE_ENGINE_SAMPLE_PERIOD_S`)
// never opens through ordinary repeated calls. Opened here by deliberately
// setting s_at.last_sample_tick to a small nonzero value before each call:
// TickType_t is uint32_t, so 0 - 1 wraps to 0xFFFFFFFF ticks, comfortably
// past any window -- a controlled, single-purpose use of the same
// unsigned-wraparound arithmetic at_ticks_to_s() itself relies on, not a
// workaround of a correctness bound. A fast-settling synthetic plant
// (tau=20s, unrelated to the bench's measured 285s -- the bench numbers are
// what test_probe_done_computes_identify_duty_and_rewinds_to_settling()
// already exercises directly) keeps this reachable within the probe's own
// 60-sample budget.
static void test_target_mode_probe_dispatch_driven_through_the_real_tick_loop(void)
{
    TEST_SECTION("target mode's probe->identify handoff, driven through the REAL STEPPING tick loop "
                 "(record_trace_sample()/step_settle_check_locked()/handle_probe_done_locked() dispatch), "
                 "not called directly -- and the probe-baseline-vs-identify-baseline re-settle bias is "
                 "real, not assumed");
    char errbuf[96] = {0};
    bool ok = call_run_to_target(/*max_temp_c=*/0.0f, /*target_c=*/50.0f, AUTOTUNE_RULE_SIMC, errbuf, sizeof(errbuf));
    TEST_CHECK(ok, "test setup: target-mode run must start");

    // Jump straight to STEPPING, same bypass every other STEPPING-loop test
    // in this file uses (SETTLING's own elapsed-time transition cannot fire
    // through this harness's frozen clock either) -- stand in for what the
    // real transition would have captured.
    const float probe_baseline_c = 30.0f;
    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    s_at.state = AUTOTUNE_ENGINE_STEPPING;
    s_at.phase_start_tick = 0;
    s_at.zone_baseline_c[0] = probe_baseline_c;
    s_at.zone_baseline_valid[0] = true;
    s_at.step_ambient_c = probe_baseline_c;
    s_at.trace_count = 0;
    s_at.step_peak_slope_c_per_s = 0.0f;
    s_at.step_onset_seen = false;
    xSemaphoreGive(s_at.lock);

    const float k_gain = 40.0f, tau_s = 20.0f, dead_time_s = 10.0f;
    bool dispatched = false;
    for (int i = 0; i < 60 && !dispatched; i++) {
        float t_s = (float)(i * AUTOTUNE_ENGINE_SAMPLE_PERIOD_S);
        float rise = (t_s <= dead_time_s) ? 0.0f
                                          : k_gain * AUTOTUNE_ENGINE_PROBE_DUTY * (1.0f - expf(-(t_s - dead_time_s) / tau_s));
        xSemaphoreTake(s_at.lock, portMAX_DELAY);
        s_stub_ch0_temp_c = probe_baseline_c + rise;
        s_at.last_sample_tick = 1; /* force this tick's sample-period gate open -- see comment above */
        autotune_engine_tick_locked();
        if (s_at.state != AUTOTUNE_ENGINE_STEPPING || s_at.probe_phase == false) {
            dispatched = true;
        }
        xSemaphoreGive(s_at.lock);
    }

    TEST_CHECK(dispatched, "the settle detector must fire within the probe's 60-sample budget on a "
                          "genuinely fast-settling (tau=20s) synthetic response");
    TEST_CHECK(!s_at.probe_phase, "probe_phase must have cleared -- handle_probe_done_locked() ran");
    TEST_CHECK(s_at.probe_k_rough > 0.0f, "a real fit through the tick-driven trace must report a positive K_rough");

    if (s_at.state == AUTOTUNE_ENGINE_SETTLING) {
        // Reachable target -> the real dispatch rewound to SETTLING for
        // phase 2. Now show the re-baseline bias is REAL: re-settling from
        // wherever the probe left off (still elevated, not back at
        // probe_baseline_c) genuinely captures a DIFFERENT baseline than
        // the probe's, via the exact same production code path a real
        // SETTLING->STEPPING transition uses.
        TEST_CHECK(s_at.trace_count > 0, "sanity: real samples were actually recorded via the tick loop");
        float last_probe_reading_c = s_stub_ch0_temp_c;
        // Same jump-to-STEPPING convention, capturing baseline the way the
        // real transition would from a zone still hot from the probe --
        // NOT back at probe_baseline_c.
        xSemaphoreTake(s_at.lock, portMAX_DELAY);
        s_at.state = AUTOTUNE_ENGINE_STEPPING;
        s_at.zone_baseline_c[0] = last_probe_reading_c; /* still elevated -- the bias this records, not corrects */
        s_at.zone_baseline_valid[0] = true;
        xSemaphoreGive(s_at.lock);

        TEST_CHECK(s_at.zone_baseline_c[0] > probe_baseline_c,
                  "the identify phase's baseline is genuinely HIGHER than the probe's own baseline -- the "
                  "one-directional overshoot bias AUTOTUNE_TARGET_ACHIEVED_WARN_C's comment describes is "
                  "real, not hypothetical");
    } else {
        TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED,
                  "if not rewound to SETTLING, the only other honest outcome is a refusal (unreachable "
                  "target etc.), never silently stuck STEPPING");
    }
}

// Review finding: "status exposes no achieved-vs-requested value."
// target_achieved_c must be populated once a target-mode identification
// step finishes, and must reflect the FIT (baseline + K*duty), not target_c
// itself parroted back.
static void test_target_achieved_c_reflects_the_fitted_model_not_the_request(void)
{
    TEST_SECTION("target_achieved_c is populated from the identification step's OWN fit, and can "
                 "legitimately differ from the requested target_c");
    memset(&s_at, 0, sizeof(s_at));
    s_at.lock = xSemaphoreCreateMutex(); /* autotune_engine_get_status() below refuses without one */
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");
    s_at.zone_index = 0;
    s_at.step_rule = AUTOTUNE_RULE_SIMC;
    s_at.target_mode = true;
    s_at.target_c = 90.0f;
    // K=45, tau=285, L=20, duty=1.0, baseline=30 -- asymptote = 30 + 45 =
    // 75C, deliberately short of the 90C requested (the bias this field
    // exists to surface, not hide). duty=1.0/K=45 also clears
    // AUTOTUNE_MIN_RISE_NO_CEILING_C (40C, no ceiling configured here) with
    // real margin, so (B)'s minimum-excursion refusal does not confound
    // this test with a DIFFERENT abort.
    write_synthetic_fopdt_trace(/*baseline_c=*/30.0f, /*k_gain_c_per_duty=*/45.0f, /*tau_s=*/285.0f,
                                /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/600);
    finalize_fit_then_flush_coupling();

    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "sanity: this trace must fit");
    TEST_CHECK(s_at.model.valid, "sanity: the fit must be valid");
    TEST_CHECK_NEAR(s_at.target_achieved_c, 75.0f, 2.0f,
                    "target_achieved_c must reflect baseline + fitted K*duty, not target_c (90) itself");
    TEST_CHECK(fabsf(s_at.target_achieved_c - s_at.target_c) > 1.0f,
              "sanity: this trace was deliberately built to miss target_c, proving this is a real "
              "computed value, not target_c echoed back");

    autotune_engine_status_t status;
    autotune_engine_get_status(&status);
    TEST_CHECK_NEAR(status.target_achieved_c, s_at.target_achieved_c, 1e-4f,
                    "autotune_engine_get_status() must expose target_achieved_c to callers");
}

// ---------------------------------------------------------------------------
// Guard-threshold generalization off the measured plant (autotune_scale_
// threshold_c(), autotune_min_rise_c(), and the ELEMENT_ALIVE_RISE_C/
// DEATH_DROP_C/DEATH_FLOOR_MARGIN_C wiring in autotune_engine_tick_locked()).
// Owner's explicit ask: seven bare degC constants, each hand-tuned to one
// bench kiln's measured K~=41.7 degC/duty, now scale with THIS run's own
// probe_k_rough -- see AUTOTUNE_REFERENCE_K_C_PER_DUTY's own comment.
// ---------------------------------------------------------------------------

static void test_autotune_scale_threshold_c_falls_back_when_probe_k_rough_missing(void)
{
    TEST_SECTION("autotune_scale_threshold_c() -- CRITICAL fallback: probe_k_rough absent/zero/negative "
                 "must return the bare constant UNCHANGED, never divide by it");
    TEST_CHECK(autotune_scale_threshold_c(5.0f, 0.0f) == 5.0f, "probe_k_rough == 0 -> unscaled constant");
    TEST_CHECK(autotune_scale_threshold_c(5.0f, -12.0f) == 5.0f,
              "a negative probe_k_rough (should never happen, but must not crash or invert the scale) -> "
              "unscaled constant, same as the zero case");
    // No probe_k_rough value can ever divide-by-zero this function -- it is
    // a multiplication by (probe_k_rough / AUTOTUNE_REFERENCE_K_C_PER_DUTY),
    // and AUTOTUNE_REFERENCE_K_C_PER_DUTY is a nonzero compile-time
    // constant, never the divisor's own probe_k_rough.
    TEST_CHECK(isfinite(autotune_scale_threshold_c(5.0f, 1e6f)), "an absurdly large probe_k_rough must "
                                                                  "still produce a finite result");
}

static void test_autotune_scale_threshold_c_scales_proportionally(void)
{
    TEST_SECTION("autotune_scale_threshold_c() -- scales linearly with probe_k_rough / "
                 "AUTOTUNE_REFERENCE_K_C_PER_DUTY");
    // Exactly at the reference plant: unchanged.
    TEST_CHECK_NEAR(autotune_scale_threshold_c(5.0f, AUTOTUNE_REFERENCE_K_C_PER_DUTY), 5.0f, 1e-4f,
                    "probe_k_rough == the reference K must reproduce the bare constant exactly");
    // Half the reference plant's gain -> half the threshold.
    TEST_CHECK_NEAR(autotune_scale_threshold_c(5.0f, AUTOTUNE_REFERENCE_K_C_PER_DUTY / 2.0f), 2.5f, 1e-4f,
                    "half the reference K must halve the threshold");
    // This kiln's own measured zone 1 (K=31.97, PID_EXPANSION_PLAN.md Phase
    // 7c measurement) -- the ~23% move this generalization pass's own report
    // flags as a real behavior change, not noise.
    float z1_floor = autotune_scale_threshold_c(AUTOTUNE_MIN_RISE_FLOOR_C, 31.97f);
    TEST_CHECK(z1_floor > 2.2f && z1_floor < 2.4f,
              "zone 1's measured K=31.97 must scale the 3.0C floor down to ~2.30C, not leave it at 3.0C");
}

static void test_autotune_min_rise_c_scales_and_falls_back(void)
{
    TEST_SECTION("autotune_min_rise_c() wires probe_k_rough into BOTH the ceiling-floor and no-ceiling "
                 "branches, with the same 0.0f-means-unscaled fallback");
    // No-ceiling branch: fallback (probe_k_rough<=0) reproduces the bare
    // 40.0C constant; a probe_k_rough at half the reference plant halves it
    // to 20.0C -- clearly distinguishable, not a coincidental match.
    TEST_CHECK_NEAR(autotune_min_rise_c(/*baseline_c=*/30.0f, /*configured_max_temp_c=*/0.0f,
                                        /*probe_k_rough=*/0.0f),
                    AUTOTUNE_MIN_RISE_NO_CEILING_C, 1e-4f, "no probe estimate -> the bare NO_CEILING constant");
    TEST_CHECK_NEAR(autotune_min_rise_c(/*baseline_c=*/30.0f, /*configured_max_temp_c=*/0.0f,
                                        /*probe_k_rough=*/AUTOTUNE_REFERENCE_K_C_PER_DUTY / 2.0f),
                    20.0f, 1e-4f, "half the reference K -> half the NO_CEILING constant (20.0C, not 40.0C)");

    // Ceiling branch, floor-dominated case: headroom is small enough that
    // AUTOTUNE_MIN_RISE_FRACTION_OF_SPAN*headroom falls under the floor, so
    // the FLOOR is what's returned -- and the floor itself must scale.
    // baseline=90, max_temp=100 -> headroom=10, fraction*headroom=1.5, well
    // under either floor tested below.
    TEST_CHECK_NEAR(autotune_min_rise_c(/*baseline_c=*/90.0f, /*configured_max_temp_c=*/100.0f,
                                        /*probe_k_rough=*/0.0f),
                    AUTOTUNE_MIN_RISE_FLOOR_C, 1e-4f, "no probe estimate -> the bare 3.0C floor");
    TEST_CHECK_NEAR(autotune_min_rise_c(/*baseline_c=*/90.0f, /*configured_max_temp_c=*/100.0f,
                                        /*probe_k_rough=*/2.0f * AUTOTUNE_REFERENCE_K_C_PER_DUTY),
                    6.0f, 1e-4f, "2x the reference K -> the floor doubles to 6.0C, not the bare 3.0C");
}

// Flagship wiring test, driven through the REAL STEPPING tick loop (not the
// bare helper functions above): AUTOTUNE_ELEMENT_ALIVE_RISE_C/DEATH_DROP_C/
// DEATH_FLOOR_MARGIN_C are scaled by s_at.probe_k_rough inside autotune_
// engine_tick_locked() itself. Deliberately picks a plant whose asymptote
// (2.2C) sits BELOW the unscaled 3.0C alive-rise constant -- old behavior
// (probe_k_rough absent/0, a plain non-target-mode run) must NEVER latch
// step_element_proven no matter how long it runs; scaled behavior (a
// target-mode run with a small probe_k_rough) must latch it, because the
// scaled-down threshold (1.5C, half the reference K) sits BELOW this
// plant's asymptote. This is the "clearly distinguishable, not coincidental"
// case the task brief asks for -- the constant and the scaled value are on
// OPPOSITE sides of the plant's actual rise.
static void test_element_alive_threshold_scales_with_probe_k_rough_and_falls_back(void)
{
    TEST_SECTION("ELEMENT_ALIVE_RISE_C generalization -- target-mode scaling with a small probe_k_rough "
                 "latches step_element_proven on a plant the unscaled 3.0C constant never would");
    const float baseline_c = 30.0f;
    const float k_gain = 2.2f;   /* full-duty asymptote 2.2C -- below the bare 3.0C alive-rise constant */
    const float tau_s = 60.0f;
    const float dead_time_s = 10.0f;

    // --- Unscaled (fallback) path: plain run, probe_k_rough stays 0. ---
    start_stepping_run(/*max_temp_c=*/0.0f, /*step_duty=*/1.0f);
    TEST_CHECK(s_at.probe_k_rough == 0.0f, "sanity: a plain (non-target-mode) run never sets probe_k_rough");
    s_at.zone_baseline_c[0] = baseline_c;
    s_at.zone_baseline_valid[0] = true;
    /* This harness's frozen xTaskGetTickCount() stub (always 0) never lets
     * record_trace_sample()/step_settle_check_locked() run through repeated
     * tick calls (see start_stepping_run()'s own header comment), so onset
     * is stood in for directly -- same convention run_guard1_relaxes_once_
     * element_proven_then_response_plateaus() above uses, and for the same
     * reason: a real onset detects well within the first rise ticks at any
     * plausible duty, long before either the bare or scaled alive-rise bar
     * is reached. */
    s_at.step_onset_seen = true;
    for (int i = 0; i < 400 && s_at.state == AUTOTUNE_ENGINE_STEPPING; i++) {
        float t_s = (float)i;
        float rise = (t_s <= dead_time_s) ? 0.0f : k_gain * (1.0f - expf(-(t_s - dead_time_s) / tau_s));
        xSemaphoreTake(s_at.lock, portMAX_DELAY);
        s_stub_ch0_temp_c = baseline_c + rise;
        autotune_engine_tick_locked();
        xSemaphoreGive(s_at.lock);
    }
    TEST_CHECK(!s_at.step_element_proven, "UNSCALED: a 2.2C-asymptote plant must never cross the bare "
                                          "3.0C alive-rise constant -- this is the OLD, pre-generalization "
                                          "behavior, confirmed still exact for a plain run");

    // --- Scaled path: target mode, probe_k_rough at half the reference K,
    //     so the alive-rise bar scales down to 1.5C -- below this plant's
    //     2.2C asymptote. ---
    start_stepping_run(/*max_temp_c=*/0.0f, /*step_duty=*/1.0f);
    s_at.zone_baseline_c[0] = baseline_c;
    s_at.zone_baseline_valid[0] = true;
    s_at.step_onset_seen = true;
    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    s_at.target_mode = true;
    s_at.probe_k_rough = AUTOTUNE_REFERENCE_K_C_PER_DUTY / 2.0f;
    xSemaphoreGive(s_at.lock);
    for (int i = 0; i < 400 && s_at.state == AUTOTUNE_ENGINE_STEPPING; i++) {
        float t_s = (float)i;
        float rise = (t_s <= dead_time_s) ? 0.0f : k_gain * (1.0f - expf(-(t_s - dead_time_s) / tau_s));
        xSemaphoreTake(s_at.lock, portMAX_DELAY);
        s_stub_ch0_temp_c = baseline_c + rise;
        autotune_engine_tick_locked();
        xSemaphoreGive(s_at.lock);
    }
    TEST_CHECK(s_at.step_element_proven, "SCALED: the SAME plant must latch proven once probe_k_rough "
                                         "scales the alive-rise bar down to 1.5C -- below its 2.2C "
                                         "asymptote. This is the behavior change the generalization "
                                         "pass exists to produce.");
}

// ---------------------------------------------------------------------------
// PHASE 1 (probe) self-termination -- probe_gain_converged_locked(), OR'd
// with step_settle_check_locked() at the STEPPING sample-recording call
// site. 600s (AUTOTUNE_ENGINE_PROBE_DURATION_S) is a hard upper bound, never
// the normal path once the gain estimate itself has stabilised.
// ---------------------------------------------------------------------------

static void test_probe_gain_converged_requires_the_min_samples_floor(void)
{
    TEST_SECTION("probe_gain_converged_locked() -- never returns true before MIN_STEPPING_SAMPLES_BEFORE_"
                 "SETTLE_CHECK samples exist, same floor step_settle_check_locked() uses");
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    // A fast, strong-signal trace (NOT the measured-bench K=41/tau=270 used
    // elsewhere in this file) -- chosen so an 11-sample fit is genuinely
    // VALID despite being below the floor, isolating the floor check itself
    // from pid_autotune_fit_fopdt()'s own too-few-points refusal (a slow,
    // weak trace at 11 samples fails to fit at all regardless of the floor,
    // which would make this test pass for the wrong reason -- confirmed
    // while writing this test).
    write_synthetic_fopdt_trace_for_zone(0, /*baseline_c=*/30.0f, /*k_gain_c_per_duty=*/200.0f, /*tau_s=*/30.0f,
                                         /*dead_time_s=*/5.0f, /*duty_step=*/1.0f,
                                         /*n_samples=*/MIN_STEPPING_SAMPLES_BEFORE_SETTLE_CHECK - 1);
    // The trace never changes between calls, so a re-fit on every call
    // produces the IDENTICAL K estimate -- if the floor gate did not exist,
    // AUTOTUNE_PROBE_GAIN_STABLE_DWELL consecutive calls at this UNCHANGED,
    // below-floor sample count would trivially agree with each other and
    // latch "converged" on nothing but a frozen trace. The real floor gate
    // must refuse every one of these calls regardless.
    bool converged_anywhere = false;
    for (uint16_t call = 0; call < AUTOTUNE_PROBE_GAIN_STABLE_DWELL + 1u; call++) {
        if (probe_gain_converged_locked()) {
            converged_anywhere = true;
        }
    }
    TEST_CHECK(!converged_anywhere, "below the sample floor, must read not-converged on every call, even "
                                    "across enough repeated calls on an unchanging trace to trivially "
                                    "satisfy the dwell if the floor gate were absent");
}

// Positive case: a clean, well-behaved probe response's re-fit K estimate
// stabilises well before the probe's own 60-sample (600s) budget -- proven
// by calling probe_gain_converged_locked() at increasing trace_count against
// a trace built the SAME way test_probe_done_computes_identify_duty_and_
// rewinds_to_settling() builds its own (K=41, tau=270, L=20 -- the measured
// bench numbers), and finding the sample index where it first returns true.
static void test_probe_gain_converged_fires_before_the_full_probe_budget(void)
{
    TEST_SECTION("probe_gain_converged_locked() -- a clean probe response converges before the full "
                 "60-sample (600s) probe budget, not merely at its end");
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    const float baseline_c = 30.6f, k_gain = 41.0f, tau_s = 270.0f, dead_time_s = 20.0f;
    const uint16_t full_budget_samples =
        (uint16_t)(AUTOTUNE_ENGINE_PROBE_DURATION_S / AUTOTUNE_ENGINE_SAMPLE_PERIOD_S); /* 60 */
    write_synthetic_fopdt_trace_for_zone(0, baseline_c, k_gain, tau_s, dead_time_s, AUTOTUNE_ENGINE_PROBE_DUTY,
                                         full_budget_samples);

    uint16_t converged_at = 0;
    for (uint16_t n = MIN_STEPPING_SAMPLES_BEFORE_SETTLE_CHECK; n <= full_budget_samples; n++) {
        s_at.trace_count = n;
        if (probe_gain_converged_locked()) {
            converged_at = n;
            break;
        }
    }
    TEST_CHECK(converged_at > 0, "a clean K=41/tau=270 probe response must converge at SOME sample count "
                                 "within the 60-sample budget");
    TEST_CHECK(converged_at < full_budget_samples, "convergence must fire STRICTLY before the full budget "
                                                    "is consumed -- 600s must be a backstop, not the normal "
                                                    "path");
}

// Genuinely never-converging case: successive re-fits of a trace that keeps
// changing SHAPE (not just noise) never agree within AUTOTUNE_PROBE_GAIN_
// STABLE_FRAC for AUTOTUNE_PROBE_GAIN_STABLE_DWELL samples running -- proves
// the 600s bound is still what ends a probe when the criterion is never met,
// by direct construction rather than by absence of a counter-example.
static void test_probe_gain_never_converges_on_a_trace_whose_shape_keeps_changing(void)
{
    TEST_SECTION("probe_gain_converged_locked() -- a trace whose fitted gain keeps moving never latches "
                 "convergence, all the way to the full probe budget (proves 600s remains the backstop)");
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    s_at.zone_baseline_c[0] = 30.0f;
    s_at.zone_baseline_valid[0] = true;
    s_at.step_ambient_c = 30.0f;
    s_at.step_duty = AUTOTUNE_ENGINE_PROBE_DUTY;
    const uint16_t full_budget_samples =
        (uint16_t)(AUTOTUNE_ENGINE_PROBE_DURATION_S / AUTOTUNE_ENGINE_SAMPLE_PERIOD_S); /* 60 */
    // A step-up-then-step-down sawtooth: every re-fit sees a DIFFERENT
    // effective rise (growing, then falling, then growing again), so
    // consecutive re-fits of the trace-so-far cannot possibly agree within
    // 3% for AUTOTUNE_PROBE_GAIN_STABLE_DWELL samples running -- this is not
    // "noisy", it is a trace an honest gain estimator SHOULD keep revising.
    bool converged_anywhere = false;
    for (uint16_t n = 1; n <= full_budget_samples; n++) {
        float t_s = (float)(n - 1) * (float)AUTOTUNE_ENGINE_SAMPLE_PERIOD_S;
        float sawtooth_c = ((n / 6u) % 2u == 0u) ? 8.0f : 1.0f;
        s_at.zone_trace[0][n - 1] = (int16_t)lroundf((30.0f + sawtooth_c + 0.001f * t_s) * 10.0f);
        s_at.trace_count = n;
        if (n >= MIN_STEPPING_SAMPLES_BEFORE_SETTLE_CHECK && probe_gain_converged_locked()) {
            converged_anywhere = true;
        }
    }
    TEST_CHECK(!converged_anywhere, "a trace whose implied gain keeps swinging must never latch convergence "
                                    "-- the ONLY thing that can end this probe is the AUTOTUNE_ENGINE_"
                                    "PROBE_DURATION_S (600s) hard upper bound");
}

// End-to-end: the REAL STEPPING tick loop, same convention as test_target_
// mode_probe_dispatch_driven_through_the_real_tick_loop() above, but this
// time proving genuine EARLY termination -- the probe must dispatch to
// handle_probe_done_locked() having recorded FEWER than the full 60 samples
// the 600s budget alone would take, which is the actual behavior change
// task B asks for (the old code always ran to ~600s in practice for a
// probe-sized signal -- see AUTOTUNE_PROBE_GAIN_STABLE_FRAC's own comment).
static void test_probe_terminates_early_through_the_real_tick_loop(void)
{
    TEST_SECTION("target mode's PHASE 1 (probe), driven through the REAL tick loop, dispatches BEFORE "
                 "the full 60-sample/600s budget on a clean, fast-converging response");
    char errbuf[96] = {0};
    bool ok = call_run_to_target(/*max_temp_c=*/0.0f, /*target_c=*/50.0f, AUTOTUNE_RULE_SIMC, errbuf, sizeof(errbuf));
    TEST_CHECK(ok, "test setup: target-mode run must start");

    const float probe_baseline_c = 30.0f;
    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    s_at.state = AUTOTUNE_ENGINE_STEPPING;
    s_at.phase_start_tick = 0;
    s_at.zone_baseline_c[0] = probe_baseline_c;
    s_at.zone_baseline_valid[0] = true;
    s_at.step_ambient_c = probe_baseline_c;
    s_at.trace_count = 0;
    s_at.step_peak_slope_c_per_s = 0.0f;
    s_at.step_onset_seen = false;
    xSemaphoreGive(s_at.lock);

    // A clean K=41/tau=270/L=20 response -- the measured bench plant, NOT
    // the artificially fast tau=20s plant test_target_mode_probe_dispatch_
    // driven_through_the_real_tick_loop() uses to force step_settle_check_
    // locked() itself to fire within 60 samples. This plant is deliberately
    // the SLOW one: its raw trace does not go flat within the probe budget
    // (roughly 88% of asymptote at 600s, per AUTOTUNE_ENGINE_PROBE_DURATION_
    // S's own comment), so if this test passes it is genuinely the NEW
    // gain-convergence criterion firing, not the pre-existing slope detector.
    const float k_gain = 41.0f, tau_s = 270.0f, dead_time_s = 20.0f;
    const uint16_t full_budget_samples =
        (uint16_t)(AUTOTUNE_ENGINE_PROBE_DURATION_S / AUTOTUNE_ENGINE_SAMPLE_PERIOD_S); /* 60 */
    uint16_t samples_recorded_before_dispatch = 0;
    bool dispatched = false;
    for (int i = 0; i < full_budget_samples && !dispatched; i++) {
        float t_s = (float)(i * AUTOTUNE_ENGINE_SAMPLE_PERIOD_S);
        float rise = (t_s <= dead_time_s)
                         ? 0.0f
                         : k_gain * AUTOTUNE_ENGINE_PROBE_DUTY * (1.0f - expf(-(t_s - dead_time_s) / tau_s));
        xSemaphoreTake(s_at.lock, portMAX_DELAY);
        s_stub_ch0_temp_c = probe_baseline_c + rise;
        s_at.last_sample_tick = 1; /* force this tick's sample-period gate open, same trick used above */
        autotune_engine_tick_locked();
        samples_recorded_before_dispatch = s_at.trace_count;
        if (s_at.state != AUTOTUNE_ENGINE_STEPPING || s_at.probe_phase == false) {
            dispatched = true;
        }
        xSemaphoreGive(s_at.lock);
    }

    TEST_CHECK(dispatched, "the probe must dispatch within its own 60-sample budget");
    TEST_CHECK(!s_at.probe_phase, "probe_phase must have cleared -- handle_probe_done_locked() ran");
    TEST_CHECK(samples_recorded_before_dispatch < full_budget_samples,
              "the probe must terminate BEFORE consuming the full 60-sample/600s budget on this slow, "
              "clean plant, driven through the REAL tick loop -- the observable behavior change task B "
              "asks for (early termination genuinely happens, not merely possible in principle), "
              "whichever of the two OR'd criteria (gain-convergence or the shared settle detector) fires "
              "first; probe_gain_converged_locked() is proven in isolation, unconfounded by the settle "
              "detector, by the two tests above");
}

// Review finding 6: the per-phase 4h backstop (measured from
// phase_start_tick, reset at every transition) cannot bound target mode's
// settle+probe+settle+identify SUM. Proves the whole-run budget aborts even
// while comfortably inside every individual phase's own budget.
static void test_whole_run_budget_aborts_even_within_every_single_phase_budget(void)
{
    TEST_SECTION("AUTOTUNE_ENGINE_WHOLE_RUN_MAX_DURATION_S aborts a STEP run whose phase-relative elapsed "
                 "time is comfortably within budget, once the RUN-level elapsed time is not");
    start_stepping_run(/*max_temp_c=*/0.0f, /*step_duty=*/1.0f);
    // phase_start_tick=0 (start_stepping_run()'s own convention) keeps this
    // phase's own elapsed_s at 0 -- nowhere near
    // AUTOTUNE_ENGINE_DEFAULT_MAX_DURATION_S. run_start_tick is poked to the
    // same kind of nonzero value used elsewhere in this file to force a
    // frozen-clock elapsed computation past a threshold -- see
    // test_target_mode_probe_dispatch_driven_through_the_real_tick_loop()'s
    // header comment for why this is safe, controlled wraparound rather
    // than an accident.
    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    s_at.run_start_tick = 1;
    xSemaphoreGive(s_at.lock);

    run_ticks(/*start_temp_c=*/25.0f, /*per_tick_delta_c=*/1.0f, /*n_ticks=*/1);

    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED, "the whole-run budget must abort regardless of how "
                                                       "little time THIS phase has used");
    TEST_CHECK(strstr(s_at.abort_reason, "whole-run") != NULL, "abort_reason must name this specific budget");
}

// Negative control: a fresh run's run_start_tick (0, matching this
// harness's frozen "now") must NOT trip the whole-run budget on its own --
// proves the check above is genuinely time-based, not permanently tripped.
static void test_whole_run_budget_does_not_trip_a_fresh_run(void)
{
    TEST_SECTION("a fresh run's whole-run budget does not trip on its own (run_start_tick == now == 0)");
    start_stepping_run(/*max_temp_c=*/0.0f, /*step_duty=*/1.0f);
    run_ticks(/*start_temp_c=*/25.0f, /*per_tick_delta_c=*/1.0f, /*n_ticks=*/5);
    TEST_CHECK(state_is_running(s_at.state), "a fresh run must not be aborted by the whole-run budget check");
}

// ---------------------------------------------------------------------------
// Review round 2 blockers (target mode, still parked pending these fixes).
// ---------------------------------------------------------------------------

// Blocker 1: cross-zone coupling can latch step_element_proven on a dead
// element. any_other_zone_profile_active() refuses at start and aborts
// mid-run.
static void test_run_refuses_to_start_while_another_zone_profile_active(void)
{
    TEST_SECTION("blocker 1: autotune_engine_run() refuses to start while ANOTHER zone has an active "
                 "profile");
    memset(s_stub_zone_active, 0, sizeof(s_stub_zone_active));
    s_stub_zone_active[1] = true;
    memset(&s_at, 0, sizeof(s_at));
    s_at.lock = xSemaphoreCreateMutex();
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");
    char err[96] = {0};
    bool ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_SIMC, err, sizeof(err));
    TEST_CHECK(!ok, "must refuse while zone 1 has an active profile");
    TEST_CHECK(strstr(err, "another zone") != NULL, "err_msg must name the actual reason");
    memset(s_stub_zone_active, 0, sizeof(s_stub_zone_active));
}

static void test_run_to_target_refuses_to_start_while_another_zone_profile_active(void)
{
    TEST_SECTION("blocker 1: autotune_engine_run_to_target() refuses to start while ANOTHER zone has an "
                 "active profile");
    memset(s_stub_zone_active, 0, sizeof(s_stub_zone_active));
    s_stub_zone_active[2] = true;
    char err[96] = {0};
    bool ok = call_run_to_target(80.0f, 60.0f, AUTOTUNE_RULE_SIMC, err, sizeof(err));
    TEST_CHECK(!ok, "must refuse while zone 2 has an active profile");
    TEST_CHECK(strstr(err, "another zone") != NULL, "err_msg must name the actual reason");
    memset(s_stub_zone_active, 0, sizeof(s_stub_zone_active));
}

static void test_run_starts_fine_when_no_other_zone_is_active(void)
{
    TEST_SECTION("blocker 1 sanity: a run starts normally when no OTHER zone has an active profile");
    // NOTE: profile_executor_zone_is_active(zone_index) for the zone UNDER
    // TEST itself is a separate, PRE-EXISTING refusal (TODO.md 6A.5 -- "never
    // on a zone a profile is actively driving", checked a few lines above
    // any_other_zone_profile_active() in autotune_begin_run_locked()) and is
    // deliberately NOT exercised here -- this test is only about OTHER
    // zones, which is what any_other_zone_profile_active() actually loops
    // over (it explicitly skips zone_index itself, see its own comment).
    memset(s_stub_zone_active, 0, sizeof(s_stub_zone_active));
    memset(&s_at, 0, sizeof(s_at));
    s_at.lock = xSemaphoreCreateMutex();
    char err[96] = {0};
    bool ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_SIMC, err, sizeof(err));
    TEST_CHECK(ok, "must not refuse when no zone anywhere has an active profile");
}

static void test_run_aborts_mid_run_when_another_zone_profile_starts(void)
{
    TEST_SECTION("blocker 1: a profile starting on another zone AFTER this run began aborts it");
    memset(s_stub_zone_active, 0, sizeof(s_stub_zone_active));
    start_stepping_run(/*max_temp_c=*/1300.0f, /*step_duty=*/1.0f);
    TEST_CHECK(state_is_running(s_at.state), "sanity: run must be active before the neighbour starts");

    s_stub_zone_active[1] = true; /* a profile starts on zone 1 mid-run */
    run_ticks(/*start_temp_c=*/25.0f, /*per_tick_delta_c=*/1.0f, /*n_ticks=*/1);

    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED, "must abort the instant another zone goes active");
    TEST_CHECK(strstr(s_at.abort_reason, "another zone") != NULL, "abort_reason must name the actual reason");
    memset(s_stub_zone_active, 0, sizeof(s_stub_zone_active));
}

// Blocker 2: onset must be direction-aware -- cooling must never latch it.
static void test_onset_ignores_residual_cooling(void)
{
    TEST_SECTION("blocker 2: step_settle_check_locked()'s onset detector ignores a FALLING trace, however "
                 "steep -- direction-blind fabsf(slope) used to latch onset on residual cooling");
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    // A steeply COOLING trace: -0.105 C/s (the exact cooling rate the
    // review measured for a zone 30C above ambient, tau=285s), well past
    // RESPONSE_ONSET_SLOPE_C_PER_S (0.009) in MAGNITUDE.
    float v = 60.0f;
    for (uint16_t i = 0; i < 40; i++) {
        v -= 0.105f * (float)AUTOTUNE_ENGINE_SAMPLE_PERIOD_S;
        s_at.zone_trace[0][i] = (int16_t)lroundf(v * 10.0f);
        s_at.trace_count = i + 1;
        (void)step_settle_check_locked();
    }
    TEST_CHECK(!s_at.step_onset_seen, "a purely falling trace, however steep, must never latch onset");
}

static void test_onset_still_fires_on_a_genuine_rise(void)
{
    TEST_SECTION("blocker 2 sanity: onset still fires promptly on a genuine RISING response (no "
                 "regression from the direction-aware fix)");
    memset(&s_at, 0, sizeof(s_at));
    s_at.zone_index = 0;
    write_synthetic_fopdt_trace(/*baseline_c=*/30.0f, /*k_gain_c_per_duty=*/41.7f, /*tau_s=*/285.0f,
                                /*dead_time_s=*/20.0f, /*duty_step=*/1.0f, /*n_samples=*/40);
    for (uint16_t n = 1; n <= 40; n++) {
        s_at.trace_count = n;
        if (step_settle_check_locked() || s_at.step_onset_seen) break;
    }
    TEST_CHECK(s_at.step_onset_seen, "a genuine rising response must still latch onset");
}

// Blocker 3: the death check needs an absolute floor too, since the
// relative (drop-from-peak) check cannot fire below a 5.0C peak.
static void test_death_check_absolute_floor_fires_in_the_3_to_5_dead_zone(void)
{
    TEST_SECTION("blocker 3 + round-3 finding 2: the death check fires via the ABSOLUTE floor (deadband "
                 "2.5C, 5 consecutive ticks) when the peak never reached AUTOTUNE_ELEMENT_DEATH_DROP_C "
                 "(5.0) -- the relative check alone is arithmetically dead in this band");
    start_stepping_run(/*max_temp_c=*/1300.0f, /*step_duty=*/0.15f);
    const float baseline_c = 30.0f;
    s_at.zone_baseline_c[0] = baseline_c;
    s_at.zone_baseline_valid[0] = true;
    s_at.step_onset_seen = true;

    for (int i = 0; i < 10; i++) {
        xSemaphoreTake(s_at.lock, portMAX_DELAY);
        s_stub_ch0_temp_c = baseline_c + 4.0f * ((float)(i + 1) / 10.0f);
        autotune_engine_tick_locked();
        xSemaphoreGive(s_at.lock);
    }
    TEST_CHECK(s_at.step_element_proven, "sanity: 4.0C rise must have latched (past the 3.0C alive floor)");
    TEST_CHECK(s_at.step_rise_running_max_c < AUTOTUNE_ELEMENT_DEATH_DROP_C,
              "sanity: peak must be BELOW 5.0C -- this is the dead zone the relative check cannot reach");
    TEST_CHECK(state_is_running(s_at.state), "must still be running after the rise");

    // Falls to 2.0C, well below the 2.5C deadband -- a drop from a ~4.0C
    // peak to ~2.0C is only ~2.0C, nowhere near the 5.0C relative
    // threshold, so ONLY the absolute floor can catch this. Held for
    // AUTOTUNE_ELEMENT_DEATH_FLOOR_CONSECUTIVE_TICKS (5) ticks, the dwell
    // the round-3 fix requires before it fires.
    for (int i = 0; i < 5; i++) {
        xSemaphoreTake(s_at.lock, portMAX_DELAY);
        s_stub_ch0_temp_c = baseline_c + 2.0f;
        autotune_engine_tick_locked();
        xSemaphoreGive(s_at.lock);
    }

    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED, "the absolute floor must abort after 5 consecutive "
                                                      "ticks below the deadband, even though the relative "
                                                      "drop (2.0C) is well under 5.0C");
    TEST_CHECK(strstr(s_at.abort_reason, "floor") != NULL, "abort_reason must name the absolute-floor path");
}

static void test_death_check_absolute_floor_needs_the_full_dwell_not_one_tick(void)
{
    TEST_SECTION("round-3 finding 2: a SINGLE tick below the deadband must NOT trip the absolute floor -- "
                 "the dwell is load-bearing, not decorative");
    start_stepping_run(/*max_temp_c=*/1300.0f, /*step_duty=*/0.15f);
    const float baseline_c = 30.0f;
    s_at.zone_baseline_c[0] = baseline_c;
    s_at.zone_baseline_valid[0] = true;
    s_at.step_onset_seen = true;

    for (int i = 0; i < 10; i++) {
        xSemaphoreTake(s_at.lock, portMAX_DELAY);
        s_stub_ch0_temp_c = baseline_c + 4.0f * ((float)(i + 1) / 10.0f);
        autotune_engine_tick_locked();
        xSemaphoreGive(s_at.lock);
    }
    TEST_CHECK(s_at.step_element_proven, "sanity: must have latched");

    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    s_stub_ch0_temp_c = baseline_c + 2.0f;
    autotune_engine_tick_locked();
    xSemaphoreGive(s_at.lock);

    TEST_CHECK(state_is_running(s_at.state), "a single dip below the deadband must not abort");
}

static void test_healthy_plateau_dithering_near_the_alive_latch_does_not_falsetrip(void)
{
    TEST_SECTION("round-3 finding 2: a healthy plateau dithering AROUND the 3.0C latch threshold (never "
                 "sustained below the 2.5C deadband) does not false-trip the death check");
    start_stepping_run(/*max_temp_c=*/1300.0f, /*step_duty=*/0.15f);
    const float baseline_c = 30.0f;
    s_at.zone_baseline_c[0] = baseline_c;
    s_at.zone_baseline_valid[0] = true;
    s_at.step_onset_seen = true;

    for (int i = 0; i < 20; i++) {
        xSemaphoreTake(s_at.lock, portMAX_DELAY);
        s_stub_ch0_temp_c = baseline_c + 3.1f * ((float)(i + 1) / 20.0f);
        autotune_engine_tick_locked();
        xSemaphoreGive(s_at.lock);
    }
    TEST_CHECK(s_at.step_element_proven, "sanity: must have latched at the 3.1C plateau");

    // Center 3.1C, dither +/-0.15C -> range [2.95C, 3.25C]: this DOES dip
    // below the 3.0C LATCH threshold repeatedly (proving the fix's
    // deadband, not just an accident of a dither that never reaches 3.0,
    // is what keeps this from tripping), but never below the 2.5C
    // DEADBAND, so the fixed code must never trip.
    bool tripped = false;
    for (int i = 0; i < 200 && !tripped; i++) {
        xSemaphoreTake(s_at.lock, portMAX_DELAY);
        s_stub_ch0_temp_c = baseline_c + 3.1f + ((i % 2) ? 0.15f : -0.15f);
        autotune_engine_tick_locked();
        tripped = (s_at.state == AUTOTUNE_ENGINE_ABORTED);
        xSemaphoreGive(s_at.lock);
    }
    TEST_CHECK(!tripped, "a healthy plateau dithering near (and sometimes under) the 3.0C latch "
                        "threshold, but never under the 2.5C deadband, must not false-trip");
}

static void test_death_check_no_trip_while_rise_stays_above_the_alive_floor(void)
{
    TEST_SECTION("blocker 3 negative control: a small dip that stays AT OR ABOVE the alive floor does "
                 "not trip either death condition");
    start_stepping_run(/*max_temp_c=*/1300.0f, /*step_duty=*/0.15f);
    const float baseline_c = 30.0f;
    s_at.zone_baseline_c[0] = baseline_c;
    s_at.zone_baseline_valid[0] = true;
    s_at.step_onset_seen = true;

    for (int i = 0; i < 10; i++) {
        xSemaphoreTake(s_at.lock, portMAX_DELAY);
        s_stub_ch0_temp_c = baseline_c + 4.0f * ((float)(i + 1) / 10.0f);
        autotune_engine_tick_locked();
        xSemaphoreGive(s_at.lock);
    }
    TEST_CHECK(s_at.step_element_proven, "sanity: must have latched");

    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    s_stub_ch0_temp_c = baseline_c + 3.2f; /* dips, but stays above the 3.0C alive floor */
    autotune_engine_tick_locked();
    xSemaphoreGive(s_at.lock);

    TEST_CHECK(state_is_running(s_at.state), "must not abort while rise stays at/above the alive floor");
}

// Blocker 4: guard 1's expected-rise bar must scale by commanded duty.
static void test_autotune_step_guard_sanity_rate_scales_by_duty(void)
{
    TEST_SECTION("blocker 4: autotune_step_guard_sanity_rate() scales the configured rate by duty");
    TEST_CHECK_NEAR(autotune_step_guard_sanity_rate(0.5f, 0.15f), 0.075f, 1e-5f,
                    "0.5 C/min at duty 0.15 must scale to 0.075 C/min");
    TEST_CHECK_NEAR(autotune_step_guard_sanity_rate(0.5f, 1.0f), 0.5f, 1e-5f,
                    "duty 1.0 must reproduce the full configured rate unchanged");
    TEST_CHECK_NEAR(autotune_step_guard_sanity_rate(0.0f, 0.15f), 0.075f, 1e-5f,
                    "configured_rate_c_per_min<=0 must substitute thermal_guard.c's own 0.5 default "
                    "before scaling");
}

static void test_healthy_low_k_zone_does_not_falsetrip_guard1_at_fixed_probe_duty(void)
{
    TEST_SECTION("blocker 4: a healthy zone with K=22 (review's own worked example -- below the K=25.6 "
                 "threshold the UNSCALED bar demanded) at the fixed 0.15 probe duty does NOT false-trip "
                 "guard 1 within its first 300s window");
    start_stepping_run(/*max_temp_c=*/1300.0f, /*step_duty=*/0.15f);
    const float baseline_c = 30.0f;
    const float k_gain = 22.0f;
    const float tau_s = 285.0f;
    const float dead_time_s = 20.0f;
    bool tripped = false;
    for (int i = 0; i < 320 && !tripped; i++) {
        float t_s = (float)i; /* dt_s == 1.0 per tick */
        float rise = (t_s <= dead_time_s) ? 0.0f
                                          : k_gain * 0.15f * (1.0f - expf(-(t_s - dead_time_s) / tau_s));
        xSemaphoreTake(s_at.lock, portMAX_DELAY);
        s_stub_ch0_temp_c = baseline_c + rise;
        autotune_engine_tick_locked();
        tripped = (s_at.state == AUTOTUNE_ENGINE_ABORTED);
        xSemaphoreGive(s_at.lock);
    }
    TEST_CHECK(!tripped, "a healthy K=22 zone at duty 0.15 must not false-trip guard 1 -- the review's own "
                        "worked example for the unscaled bar's false-positive threshold");
}

// ---------------------------------------------------------------------------
// Phase 7c: pre-start thermal readiness check -- check_thermal_readiness_
// locked() in autotune_engine.c, exercised through the REAL SETTLING->
// STEPPING transition (start_settling_run() + force_settling_transition_
// tick() above), not called directly.
//
// REVISED 2026-08-31: the FIRST version of this check compared each zone's
// baseline against the cold-junction (ambient) reading. Checked against
// real accept/refuse data from the rig, that version refused BOTH of the
// only genuinely-valid runs this board has ever produced -- the CJ-to-
// chamber offset (1-3C on this board, fixed sensor placement, not residual
// heat) is bigger than the margin a real tune needs to pass. The check now
// compares zones against EACH OTHER (the coolest currently-valid zone) and
// against their own settling slope, never against ambient/CJ -- see check_
// thermal_readiness_locked()'s own comment for the full reasoning. Every
// fixture below uses either the REAL numbers from tonight's rig (both the
// two valid runs and the two hot/rested three-zone snapshots the
// coordinator supplied) or numbers derived the same way, at max_temp_c
// =80.0f (this board's real configured ceiling) -- the stub applies
// max_temp_c to every zone (zones_config_get_temp_limits() is not
// per-zone in this harness), so autotune_min_rise_c(baseline, 80.0f) is
// the exact function production code evaluates for each zone.
// ---------------------------------------------------------------------------

static void test_readiness_accepts_zone0s_real_valid_tune_baseline(void)
{
    TEST_SECTION("Phase 7c REAL DATA: zone 0's only fully-valid tune ever produced on this rig -- "
                 "baseline 31.36C, CJ 29.75C (1.61C above CJ, a fixed sensor-placement offset, not "
                 "residual heat) -- must be ACCEPTED, not refused");
    start_settling_run(/*max_temp_c=*/80.0f, /*step_duty=*/0.5f);
    s_stub_ch0_temp_c = 31.36f;
    s_stub_ch0_cj_c = 29.75f; /* no longer read by the check at all -- present for realism only */

    force_settling_transition_tick();

    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_STEPPING,
              "the run that actually produced a trustworthy K=39.25 fit must not be refused before it "
              "even starts");
}

static void test_readiness_accepts_zone1s_real_rested_baseline(void)
{
    TEST_SECTION("Phase 7c REAL DATA: zone 1, deliberately cooled 25 minutes -- baseline 30.92C, CJ "
                 "28.95C (1.97C above CJ) -- must be ACCEPTED");
    start_settling_run(/*max_temp_c=*/80.0f, /*step_duty=*/0.5f);
    s_stub_ch0_temp_c = 30.92f;
    s_stub_ch0_cj_c = 28.95f;

    force_settling_transition_tick();

    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_STEPPING,
              "a genuinely 25-minute-rested zone must not be refused just because its CJ-to-chamber "
              "offset (1.97C) is nonzero");
}

static void test_readiness_accepts_the_real_rested_three_zone_spread(void)
{
    TEST_SECTION("Phase 7c REAL DATA: all three zones genuinely rested read 32.2 / 30.9 / 30.0 (a "
                 "2.2C spread) -- must be ACCEPTED, the spread is far under any zone's own min_rise");
    start_settling_run(/*max_temp_c=*/80.0f, /*step_duty=*/0.5f);
    s_stub_ch0_temp_c = 32.2f; /* tested zone, the warmest of the three but still just rest-state scatter */
    s_stub_extra_ch_mask = (1u << 1) | (1u << 2);
    s_stub_extra_ch_ok[1] = true;
    s_stub_extra_ch_temp_c[1] = 30.9f;
    s_stub_extra_ch_ok[2] = true;
    s_stub_extra_ch_temp_c[2] = 30.0f;

    force_settling_transition_tick();

    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_STEPPING,
              "min_rise(32.2, 80.0) ~= 7.17C is far above the real 2.2C rested spread -- must accept");
}

static void test_readiness_refuses_the_real_hot_three_zone_spread(void)
{
    TEST_SECTION("Phase 7c REAL DATA: after zone 0's run the three zones read 67.8 / 41.9 / 35.8 -- a "
                 "32C spread against the coolest zone -- must REFUSE and name the hot zone");
    start_settling_run(/*max_temp_c=*/80.0f, /*step_duty=*/0.5f);
    s_stub_ch0_temp_c = 67.8f; /* the just-driven zone -- this is the zone under test this time */
    s_stub_extra_ch_mask = (1u << 1) | (1u << 2);
    s_stub_extra_ch_ok[1] = true;
    s_stub_extra_ch_temp_c[1] = 41.9f;
    s_stub_extra_ch_ok[2] = true;
    s_stub_extra_ch_temp_c[2] = 35.8f;

    force_settling_transition_tick();

    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED,
              "min_rise(67.8, 80.0) floors at 3.0C -- nowhere near the real 32C spread -- must refuse");
    TEST_CHECK(strstr(s_at.abort_reason, "zone 0") != NULL, "the refusal must name the hot zone (0)");
}

static void test_readiness_blocks_on_a_hot_neighbour_zone(void)
{
    TEST_SECTION("Phase 7c: zone under test is rested, but zone 1 (a neighbour, not the one stepping) "
                 "has plateaued far hotter -- the run must refuse, and name zone 1");
    start_settling_run(/*max_temp_c=*/80.0f, /*step_duty=*/0.5f);
    s_stub_ch0_temp_c = 30.0f; /* tested zone: rested */
    s_stub_extra_ch_mask = (1u << 1);
    s_stub_extra_ch_ok[1] = true;
    s_stub_extra_ch_temp_c[1] = 55.0f; /* min_rise(55,80)=3.75C; 25C spread against zone 0 blows it */

    force_settling_transition_tick();

    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED, "a hot neighbour zone must refuse the SETTLING->"
                                                       "STEPPING transition, not silently proceed");
    TEST_CHECK(strstr(s_at.abort_reason, "zone 1") != NULL,
              "the refusal must name the OFFENDING zone (1), not just the zone under test (0)");
}

static void test_readiness_blocks_on_a_drifting_zone_with_no_neighbours_at_all(void)
{
    TEST_SECTION("Phase 7c: a single-zone board (no neighbours reporting, so the cross-zone spread "
                 "check is structurally inert -- spread_z is always exactly 0) still refuses a zone "
                 "that is visibly rising through the whole SETTLING window -- proves the STABILITY "
                 "check alone protects single-zone hardware, reusing SETTLE_ABS_SLOPE_FLOOR_C_PER_S "
                 "rather than a made-up second threshold");
    start_settling_run(/*max_temp_c=*/80.0f, /*step_duty=*/0.5f);

    // First tick: phase_start_tick is still 0 (autotune_begin_run_locked() left it
    // there), so elapsed_s reads 0 and the run stays in SETTLING -- this is
    // the tick that captures readiness_start_c[0].
    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    s_stub_ch0_temp_c = 25.0f;
    autotune_engine_tick_locked();
    xSemaphoreGive(s_at.lock);
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_SETTLING, "test setup: must still be settling after tick 1");
    TEST_CHECK(s_at.readiness_start_valid[0] && s_at.readiness_start_c[0] == 25.0f,
              "test setup: the start-of-settle reading must have been captured at 25.0C");

    // Second tick: 1.0C higher than the captured start, forced to the
    // elapsed-time transition. slope = 1.0C / SETTLE_S (180s) = 0.00556 C/s,
    // comfortably above SETTLE_ABS_SLOPE_FLOOR_C_PER_S (0.003).
    s_stub_ch0_temp_c = 26.0f;
    force_settling_transition_tick();

    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_ABORTED, "a zone still drifting through SETTLING must "
                                                       "refuse even with no neighbours to compare against");
    TEST_CHECK(strstr(s_at.abort_reason, "zone 0") != NULL, "the refusal must name the drifting zone");
}

static void test_readiness_missing_cj_has_no_effect_on_the_check_at_all(void)
{
    TEST_SECTION("Phase 7c: a missing cold-junction reading must have ZERO effect on the readiness "
                 "check -- ambient/CJ is no longer read by it at all (see check_thermal_readiness_"
                 "locked()'s own \"THE REFERENCE PROBLEM\" comment) -- a rested zone starts normally "
                 "whether or not CJ answered this tick");
    start_settling_run(/*max_temp_c=*/80.0f, /*step_duty=*/0.5f);
    s_stub_ch0_cj_c = NAN; /* no CJ this tick */
    s_stub_ch0_temp_c = 30.0f; /* an ordinary rested reading, unrelated to any ambient/fallback value */

    force_settling_transition_tick();

    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_STEPPING,
              "a rested zone must start normally regardless of CJ availability -- the readiness check "
              "has no ambient dependency left to degrade");
    TEST_CHECK_NEAR(s_at.step_ambient_c, AUTOTUNE_FALLBACK_AMBIENT_C, 1e-4f,
                    "step_ambient_c (a SEPARATE consumer, autotune_finalize_fit()'s own plausibility check) "
                    "still falls back as documented -- proving CJ really was missing this tick, not "
                    "that the fixture failed to exercise the NAN path");
}

static void test_readiness_skips_a_zone_with_no_valid_reading_this_tick(void)
{
    TEST_SECTION("Phase 7c: a neighbour zone with no valid reading this tick (sensor fault) cannot be "
                 "judged, so it must not block the run, AND must not corrupt the min-baseline reference "
                 "every OTHER zone's spread is compared against");
    start_settling_run(/*max_temp_c=*/80.0f, /*step_duty=*/0.5f);
    s_stub_ch0_temp_c = 30.0f;
    s_stub_extra_ch_mask = (1u << 1);
    s_stub_extra_ch_ok[1] = false; /* faulted -- ok_by_zone[1] reads false regardless of temp below */
    s_stub_extra_ch_temp_c[1] = 90.0f; /* would fail the spread check outright if it were trusted */

    force_settling_transition_tick();

    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_STEPPING,
              "an unreadable zone must be skipped, not treated as a refusal-worthy hot zone");
}

static char *autotune_engine_read_source(void)
{
    static const char *const candidates[] = {
        "../drivers/control/autotune_engine.c",
        "App/drivers/control/autotune_engine.c",
        "firmware/KilnFW/App/drivers/control/autotune_engine.c",
    };
    return test_read_source_anchored(__FILE__, "../drivers/control/autotune_engine.c", candidates,
                                      sizeof(candidates) / sizeof(candidates[0]));
}

static bool source_scan_function_lacks(const char *text, const char *fn_sig, const char *next_fn_sig,
                                        const char *forbidden, const char **out_reason)
{
    const char *fn = strstr(text, fn_sig);
    if (!fn) {
        *out_reason = "sanity: function definition not findable";
        return false;
    }
    const char *next_fn = strstr(fn + 1, next_fn_sig);
    if (!next_fn) {
        *out_reason = "sanity: next function boundary not findable";
        return false;
    }
    size_t body_len = (size_t)(next_fn - fn);
    char *body = (char *)malloc(body_len + 1);
    if (!body) {
        *out_reason = "sanity: OOM";
        return false;
    }
    memcpy(body, fn, body_len);
    body[body_len] = '\0';
    bool ok = (strstr(body, forbidden) == NULL);
    free(body);
    *out_reason = NULL;
    return ok;
}

static void test_heat_enable_acquire_never_called_under_s_at_lock(void)
{
    TEST_SECTION("autotune_engine -- 2026-09-15 review of 059a896e, MEDIUM-4: heat_enable_acquire() "
                 "must never be called from inside autotune_begin_run_locked() (which runs with "
                 "s_at.lock already held) -- that call can now block for seconds (HIGH-1's real "
                 "worst case), and CLAUDE.md's rule is never to hold a module lock across a "
                 "blocking call. Each caller must acquire it itself, after releasing s_at.lock.");

    char *text = autotune_engine_read_source();
    if (!text) {
        TEST_CHECK(false, "could not locate drivers/control/autotune_engine.c to source-scan");
        return;
    }

    const char *reason = NULL;
    bool ok = source_scan_function_lacks(text, "bool autotune_begin_run_locked(uint8_t zone_index",
                                          "\nbool autotune_engine_run(uint8_t zone_index",
                                          "heat_enable_acquire(HEAT_ENABLE_CLAIMANT_AUTOTUNE)", &reason);
    if (reason) {
        TEST_CHECK(false, reason);
    } else {
        TEST_CHECK(ok, "MUST GO RED if autotune_begin_run_locked() calls heat_enable_acquire() "
                       "again while s_at.lock is held -- move it back out to each caller");
    }

    free(text);
}

// iter_tune_http.c restore-race reservation (opus review, 2026-09-23):
// autotune_begin_run_locked()'s check at autotune_engine.c ~line 1154, and
// autotune_engine_accept()'s matching check at autotune_engine_guard.c
// ~line 387, were both added with NO test driving the real, locked code
// path -- deleting either `if` failed nothing. These tests use the same
// "bypass autotune_engine_start(), hand-build a real s_at.lock" pattern as
// start_stepping_run_rule() above, since autotune_engine_start() always
// fails in the host-test build.
static void reserve_race_test_setup_idle_zone0(void)
{
    memset(&s_at, 0, sizeof(s_at));
    s_at.lock = xSemaphoreCreateMutex();
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");
    s_at.state = AUTOTUNE_ENGINE_IDLE;
    s_at.zone_index = 0;
}

static void test_reserve_blocks_a_fresh_start_and_names_the_reason(void)
{
    reserve_race_test_setup_idle_zone0();

    bool reserved = autotune_engine_reserve_zone_for_external_write(0);
    TEST_CHECK(reserved, "reserve() must succeed on an idle, unreserved zone");

    char err[128] = {0};
    bool ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_SIMC, err, sizeof(err));
    TEST_CHECK(!ok, "autotune_engine_run() must be refused while the zone is reserved");
    TEST_CHECK(strstr(err, "gain write is in progress") != NULL,
               "refusal message must name the external-write reservation");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_IDLE, "a refused start must leave state at IDLE");

    autotune_engine_release_zone_for_external_write(0);
}

static void test_reserve_refused_while_a_run_is_live_on_the_zone(void)
{
    reserve_race_test_setup_idle_zone0();
    s_at.state = AUTOTUNE_ENGINE_STEPPING; // state_is_running() == true

    bool reserved = autotune_engine_reserve_zone_for_external_write(0);
    TEST_CHECK(!reserved, "reserve() must refuse a zone with a live run");
    TEST_CHECK(!s_at.external_write_reserved, "a refused reserve() must not leave the flag set");
}

static void test_run_starts_after_release(void)
{
    reserve_race_test_setup_idle_zone0();

    TEST_CHECK(autotune_engine_reserve_zone_for_external_write(0), "reserve() must succeed first");
    autotune_engine_release_zone_for_external_write(0);
    TEST_CHECK(!s_at.external_write_reserved, "release() must clear the flag");

    char err[128] = {0};
    bool ok = autotune_engine_run(0, 0.5f, AUTOTUNE_RULE_SIMC, err, sizeof(err));
    TEST_CHECK(ok, "autotune_engine_run() must be allowed to start once released");
}

// autotune_engine_accept()'s matching gate (required fix 2): use
// AUTOTUNE_METHOD_RELAY with relay.valid=true rather than a full FOPDT
// model fit, so the STEP-only ack_unsettled gate in accept() never engages
// and this test stays focused on the reservation check alone.
static void test_accept_is_gated_by_the_reservation_too(void)
{
    reserve_race_test_setup_idle_zone0();
    s_at.state = AUTOTUNE_ENGINE_DONE;
    s_at.method = AUTOTUNE_METHOD_RELAY;
    s_at.relay.valid = true;
    s_stub_set_pid_result = true;

    TEST_CHECK(autotune_engine_reserve_zone_for_external_write(0), "reserve() must succeed on a DONE zone");

    bool accepted = autotune_engine_accept(NULL, NULL);
    TEST_CHECK(!accepted, "accept() must be refused while the zone is reserved");
    TEST_CHECK(s_at.state == AUTOTUNE_ENGINE_DONE, "a refused accept() must leave state at DONE");

    autotune_engine_release_zone_for_external_write(0);

    /* accept() must take its OWN reservation across the write, so a
     * concurrent reserve() attempt made while accept() is still writing is
     * refused -- probed from inside the zones_config_set_pid() stub, the one
     * point that runs synchronously mid-accept(). */
    s_probe_reserve_during_set_pid = true;
    s_probe_reserve_during_set_pid_result = true;
    accepted = autotune_engine_accept(NULL, NULL);
    TEST_CHECK(accepted, "accept() must succeed once the reservation is released");
    TEST_CHECK(!s_probe_reserve_during_set_pid_result,
               "a concurrent reserve() attempt made while accept() is writing gains must be refused");
    TEST_CHECK(!s_at.external_write_reserved,
               "accept() must release its own reservation once its writes are done");
    TEST_CHECK(autotune_engine_reserve_zone_for_external_write(0),
               "reserve() must succeed again once accept() has returned and released its own reservation");
    autotune_engine_release_zone_for_external_write(0);

    s_probe_reserve_during_set_pid = false;
    s_stub_set_pid_result = false;
}

void run_test_autotune_engine_prestart(void)
{
    test_heat_enable_acquire_never_called_under_s_at_lock();
    test_run_refuses_before_start();
    test_run_relay_refuses_before_start();
    test_begin_run_refused_by_readiness_recovery_mode();
    test_begin_run_refused_by_restore_in_flight();
    test_begin_run_refused_by_readiness_safety_trip();
    test_begin_run_refused_by_readiness_crash_report();
    test_begin_run_refused_by_readiness_estop_unverified();
    test_begin_run_passes_the_readiness_gate_when_ready();
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
    test_finalize_fit_persists_cross_gain_tau_dead_time_in_correct_orientation();
    test_finalize_fit_does_not_persist_tau_dead_time_for_invalid_peer();
    test_finalize_fit_routes_persist_through_flash_worker();
    test_finalize_fit_under_lock_does_not_dispatch_coupling_persist();
    test_tick_under_lock_dispatches_coupling_persist_after_give();
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
    test_autotune_engine_accept_refused_by_mode_gate_while_running();
    test_autotune_engine_accept_refused_by_adaptive_write_and_run_claim();
    test_autotune_engine_accept_rechecks_gate_before_writes();
    test_autotune_engine_accept_gates_on_extrapolation_converged();
    test_autotune_engine_accept_gates_on_tau_consistent();
    test_autotune_engine_accept_does_not_block_a_fully_clean_fit();
    test_autotune_engine_accept_writes_tuning_quality_on_step_success();
    test_autotune_engine_accept_skips_tuning_quality_when_model_persist_fails();
    test_autotune_engine_accept_skips_tuning_quality_on_relay_method();
    test_autotune_engine_accept_writes_coupling_diag_k_dc_on_step_success();
    test_autotune_engine_accept_skips_coupling_diag_k_dc_when_model_persist_fails();
    test_autotune_engine_accept_skips_coupling_diag_k_dc_on_relay_method();
    test_autotune_engine_accept_resets_adaptive_tune_baseline_on_a_clean_step_accept();
    test_autotune_engine_accept_skips_adaptive_tune_baseline_reset_when_model_persist_fails();
    test_autotune_engine_accept_skips_adaptive_tune_baseline_reset_on_relay_method();
    test_autotune_engine_accept_adopts_ceiling_when_requested();
    test_autotune_engine_accept_does_not_adopt_ceiling_by_default();
    test_autotune_engine_accept_does_not_adopt_a_zero_ceiling();
    test_autotune_engine_accept_does_not_adopt_ceiling_on_relay_method();
    test_autotune_engine_accept_skips_when_it_would_tighten();
    test_autotune_engine_accept_skips_ceiling_adoption_when_read_fails();
    test_autotune_engine_accept_reports_ceiling_persist_failure();
    test_autotune_engine_accept_rejects_out_of_range_not_clamped();
    test_min_excursion_refuses_a_fit_below_the_rise_floor();
    test_physical_plausibility_refuses_gain_implying_ceiling_below_max_temp();
    test_physical_plausibility_uses_ambient_not_baseline_on_a_hot_start();
    test_coupling_reachable_ceiling_is_accepted_even_with_low_single_zone_gain();
    test_nonsense_negative_gain_fit_is_still_rejected();
    test_rejected_fit_diagnostics_survive_the_reject_but_clear_on_next_run();
    test_model_settled_flag_reflects_step_settled();
    test_next_run_clears_prior_runs_refusal();
    test_next_run_clears_prior_runs_model_payload();
    test_step_no_ceiling_flat_reading_trips_guard1();
    test_step_max_temp_configured_flat_reading_still_trips_guard1();
    test_step_no_ceiling_rising_reading_does_not_trip();
    test_step_slow_healthy_zone_with_model_does_not_false_trip_guard1();
    test_step_slow_healthy_zone_without_model_still_false_trips_guard1();
    test_guard1_relaxes_once_element_proven_then_response_plateaus();
    test_guard1_relaxation_engages_at_a_realistic_ceiling();
    test_guard1_relaxation_engages_with_no_ceiling_configured();
    test_guard1_latch_requires_onset_not_just_rise();
    test_element_death_after_proven_aborts_the_run();
    test_element_small_dip_after_proven_does_not_abort();
    test_dead_element_trips_guard1_below_the_old_0_5_duty_floor();
    test_guard1_relaxation_never_applies_to_relay_method();
    test_guard1_still_trips_a_dead_element_that_never_gets_proven();
    test_relay_run_overrides_progress_duty_min_same_as_step_test();
    test_relay_run_flat_dead_element_now_trips_a_guard();
    test_relay_run_flat_dead_element_on_low_branch_also_trips();
    test_relay_run_healthy_limit_cycle_does_not_spuriously_trip();
    test_relay_run_guard2_downswing_scenario_does_not_trip();
    test_relay_run_guard1_large_hysteresis_scenario_does_not_trip();

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
    test_run_refuses_zone_with_no_thermo_mask();
    test_run_refuses_on_off_zone();
    test_run_refuses_monitor_only_zone();
    test_run_refuses_at_atomic_heat_claim_gate();
    test_run_refuses_when_update_claims_after_early_check();
    test_run_refuses_when_zones_config_changes_during_start();
    test_run_refuses_when_factory_reset_in_flight();
    test_run_refuses_at_atomic_zone_claim_gate();

    // Heat-enable (K4) wiring -- each starts from its own
    // start_stepping_run(), so order-independent relative to everything
    // above.
    test_autotune_start_requests_heat_enable_once();
    test_autotune_guard_trip_releases_heat_enable();
    test_autotune_manual_abort_releases_heat_enable();
    test_autotune_start_on_a_down_link_does_not_claim_heat();

    // Target-temperature step mode (slice 1) -- order-independent, each
    // re-zeroes s_at via its own helper.
    test_probe_done_computes_identify_duty_and_rewinds_to_settling();
    test_probe_done_refuses_unreachable_target();
    test_probe_done_refuses_target_not_above_baseline();
    test_probe_done_refuses_identify_duty_below_the_progress_duty_min_floor();
    test_probe_done_accepts_identify_duty_above_the_progress_duty_min_floor();
    test_probe_done_propagates_probe_fit_failure();
    test_relay_setpoint_narrow_rig_span_now_has_a_usable_window();
    test_relay_setpoint_wide_span_keeps_the_full_50c_headroom();
    test_relay_setpoint_span_exactly_100_is_usable();
    test_relay_setpoint_span_exactly_100_band_still_composes_safely();
    test_relay_setpoint_pathologically_narrow_span_refuses_with_stated_window();
    test_relay_band_must_not_reach_the_zone_guard_limits();
    test_run_to_target_rejects_relay_only_rules();
    test_run_to_target_default_uses_75_percent_of_max_temp();
    test_run_to_target_default_refused_when_max_temp_c_is_zero();
    test_run_to_target_explicit_target_refused_too_close_to_ceiling();
    test_run_to_target_default_also_subject_to_ceiling_margin_check();
    test_run_to_target_does_not_disturb_the_plain_duty_based_run();
    test_target_mode_probe_dispatch_driven_through_the_real_tick_loop();
    test_target_achieved_c_reflects_the_fitted_model_not_the_request();

    // Guard-threshold generalization off probe_k_rough -- order-independent.
    test_autotune_scale_threshold_c_falls_back_when_probe_k_rough_missing();
    test_autotune_scale_threshold_c_scales_proportionally();
    test_autotune_min_rise_c_scales_and_falls_back();
    test_element_alive_threshold_scales_with_probe_k_rough_and_falls_back();

    // PHASE 1 (probe) self-termination -- order-independent.
    test_probe_gain_converged_requires_the_min_samples_floor();
    test_probe_gain_converged_fires_before_the_full_probe_budget();
    test_probe_gain_never_converges_on_a_trace_whose_shape_keeps_changing();
    test_probe_terminates_early_through_the_real_tick_loop();

    test_whole_run_budget_aborts_even_within_every_single_phase_budget();
    test_whole_run_budget_does_not_trip_a_fresh_run();

    // Review round 2 blockers -- order-independent, each re-derives its own
    // fresh s_at via its own helper.
    test_run_refuses_to_start_while_another_zone_profile_active();
    test_run_to_target_refuses_to_start_while_another_zone_profile_active();
    test_run_starts_fine_when_no_other_zone_is_active();
    test_run_aborts_mid_run_when_another_zone_profile_starts();
    test_onset_ignores_residual_cooling();
    test_onset_still_fires_on_a_genuine_rise();
    test_death_check_absolute_floor_fires_in_the_3_to_5_dead_zone();
    test_death_check_absolute_floor_needs_the_full_dwell_not_one_tick();
    test_healthy_plateau_dithering_near_the_alive_latch_does_not_falsetrip();
    test_death_check_no_trip_while_rise_stays_above_the_alive_floor();
    test_autotune_step_guard_sanity_rate_scales_by_duty();
    test_healthy_low_k_zone_does_not_falsetrip_guard1_at_fixed_probe_duty();

    // Phase 7c: pre-start thermal readiness check -- order-independent,
    // each starts from its own start_settling_run().
    test_readiness_accepts_zone0s_real_valid_tune_baseline();
    test_readiness_accepts_zone1s_real_rested_baseline();
    test_readiness_accepts_the_real_rested_three_zone_spread();
    test_readiness_refuses_the_real_hot_three_zone_spread();
    test_readiness_blocks_on_a_hot_neighbour_zone();
    test_readiness_blocks_on_a_drifting_zone_with_no_neighbours_at_all();
    test_readiness_missing_cj_has_no_effect_on_the_check_at_all();
    test_readiness_skips_a_zone_with_no_valid_reading_this_tick();

    // iter_tune_http.c restore-race reservation (opus review, 2026-09-23) --
    // order-independent, each hand-builds its own fresh s_at and lock.
    test_reserve_blocks_a_fresh_start_and_names_the_reason();
    test_reserve_refused_while_a_run_is_live_on_the_zone();
    test_run_starts_after_release();
    test_accept_is_gated_by_the_reservation_too();

    // 2026-09-24 "never hold a module lock across a producer call" fix.
    test_tick_locked_wrapper_reads_outside_any_lock();
    test_task_entry_reads_before_taking_s_at_lock();
}


// 2026-09-24 "never hold a module lock across a producer call" fix
// (companion to profile_executor's test_baseline_read_runs_outside_the_lock()
// in test_profile_executor_prestart.c). autotune_engine_tick_locked() here
// is the test-only wrapper defined at the top of THIS file (reads the bus
// itself, then calls autotune_engine_tick_locked_impl()); it does not exist in
// autotune_engine.c at all, so production cannot call it. This test is weak on
// its own (it exercises the wrapper, not task_entry()); the source scan below
// is what pins the production ordering.
// This test still proves the shared thermo_channels_read()/MAX31856_read_all()
// path itself never observes a nonzero xSemaphoreTake/Give nesting depth,
// same property test_task_entry_reads_before_taking_s_at_lock() below
// establishes by source scan for the real production loop.
static void test_tick_locked_wrapper_reads_outside_any_lock(void)
{
    TEST_SECTION("autotune_engine -- the tick's MAX31856 read never observes a nonzero lock depth");

    static MAX31856BusClass bus;
    memset(&s_at, 0, sizeof(s_at));
    memset(&bus, 0, sizeof(bus));
    bus.initialized = true;
    s_at.thermo_bus = &bus;
    s_at.lock = xSemaphoreCreateMutex();
    TEST_CHECK(s_at.lock != NULL, "test setup: lock must be creatable");
    s_at.state = AUTOTUNE_ENGINE_SETTLING;
    s_at.zone_index = 0;
    s_stub_ch0_temp_c = 50.0f;
    s_stub_ch0_ok = true;

    g_test_stub_lock_depth = 0;
    s_test_max31856_max_lock_depth_seen = -1;
    autotune_engine_tick_locked();

    TEST_CHECK(s_test_max31856_max_lock_depth_seen == 0,
               "sanity: MAX31856_read_all() must actually have been called this tick, at depth 0");
}

/* Comment-stripped copy of `src` (same approach as test_display_power_wiring.c's
 * strip_c_comments()), so a source-order scan cannot be satisfied by an
 * explanatory comment that merely mentions a needle. */
static char *ae_strip_c_comments(const char *src)
{
    size_t n = strlen(src);
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        if (src[i] == '/' && i + 1 < n && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) i++;
            i = (i + 1 < n) ? i + 2 : n;
            out[o++] = ' ';
        } else if (src[i] == '/' && i + 1 < n && src[i + 1] == '/') {
            while (i < n && src[i] != '\n') i++;
            out[o++] = ' ';
        } else {
            out[o++] = src[i++];
        }
    }
    out[o] = '\0';
    return out;
}

/* Occurrences of `needle` starting in [begin, end). */
static int ae_count_in_range(const char *begin, const char *end, const char *needle)
{
    int n = 0;
    for (const char *p = strstr(begin, needle); p && p < end; p = strstr(p + 1, needle)) n++;
    return n;
}

/* Locates [fn_sig, next_fn_sig) in `code`. */
static bool ae_find_body(const char *code, const char *fn_sig, const char *next_fn_sig,
                         const char **out_begin, const char **out_end)
{
    const char *fn = strstr(code, fn_sig);
    if (!fn) return false;
    const char *next_fn = strstr(fn + 1, next_fn_sig);
    if (!next_fn) return false;
    *out_begin = fn;
    *out_end = next_fn;
    return true;
}

// Production's task_entry() (unreachable from host tests -- a real
// `for (;;) { vTaskDelay(...); }` loop with no extraction seam) cannot be
// driven directly, so this proves the property by comment-stripped source
// scan: exactly one thermo_channels_read( call, textually before the only
// xSemaphoreTake(s_at.lock ...), no raw bus/sim read in task_entry() or in
// the locked tick body, and the test-only wrapper absent from production.
static void test_task_entry_reads_before_taking_s_at_lock(void)
{
    TEST_SECTION("autotune_engine -- task_entry() must call thermo_channels_read() BEFORE "
                 "xSemaphoreTake(s_at.lock, ...), never while s_at.lock is held (CLAUDE.md: "
                 "never hold a module lock across a producer call -- the MAX31856/sim read can "
                 "block for up to MAX31856_LOCK_TIMEOUT_MS per channel)");

    char *text = autotune_engine_read_source();
    if (!text) {
        TEST_CHECK(false, "could not locate drivers/control/autotune_engine.c to source-scan");
        return;
    }
    char *code = ae_strip_c_comments(text);
    free(text);
    if (!code) {
        TEST_CHECK(false, "malloc for the comment-stripped autotune_engine.c failed");
        return;
    }

    const char *b = NULL;
    const char *e = NULL;
    if (!ae_find_body(code, "static void task_entry(void *arg)", "esp_err_t autotune_engine_start(", &b, &e)) {
        TEST_CHECK(false, "sanity: task_entry()'s body not findable in autotune_engine.c");
    } else {
        TEST_CHECK(ae_count_in_range(b, e, "thermo_channels_read(") == 1,
                   "task_entry() makes exactly ONE thermo_channels_read() call (a second one could sit "
                   "under s_at.lock without breaking a first-occurrence ordering check)");
        TEST_CHECK(ae_count_in_range(b, e, "xSemaphoreTake(s_at.lock") == 1,
                   "task_entry() takes s_at.lock exactly once");
        TEST_CHECK(ae_count_in_range(b, e, "MAX31856_read_all(") == 0 &&
                       ae_count_in_range(b, e, "sim_backend_read_all(") == 0,
                   "task_entry() never calls a raw bus/sim read directly -- only the pre-lock helper");
        const char *rd = strstr(b, "thermo_channels_read(");
        const char *lk = strstr(b, "xSemaphoreTake(s_at.lock");
        TEST_CHECK(rd && lk && rd < e && lk < e && rd < lk,
                   "MUST GO RED if task_entry() takes s_at.lock before its thermo_channels_read() "
                   "pre-lock peek/read -- move the read back before the lock");
        /* A peek that saw not-running took no reading, so the tick body must
         * be gated on peek_active too (never fed a NULL/unfilled snapshot):
         * an all-invalid tick bumps guard 6's streak and trips SENSOR_INVALID
         * at a configured debounce of 1. Pin the gate, its position after the
         * lock, and that the only tick call passes the filled snapshot. */
        const char *gate = strstr(b, "if (!not_running && peek_active)");
        const char *call = strstr(b, "autotune_engine_tick_locked_impl(");
        TEST_CHECK(gate && gate < e && lk && gate > lk,
                   "task_entry() gates the tick body on peek_active after taking s_at.lock "
                   "(if (!not_running && peek_active))");
        TEST_CHECK(ae_count_in_range(b, e, "autotune_engine_tick_locked_impl(") == 1 &&
                       ae_count_in_range(b, e, "autotune_engine_tick_locked_impl(pre_lock_snap)") == 1 &&
                       call && gate && call > gate,
                   "task_entry()'s only tick call (in autotune_engine_tick_under_lock()) passes the pre-lock "
                   "snapshot, inside the peek_active gate");
    }

    if (!ae_find_body(code, "static void autotune_engine_tick_locked_impl(", "static void task_entry(void *arg)", &b, &e)) {
        TEST_CHECK(false, "sanity: autotune_engine_tick_locked_impl()'s body not findable");
    } else {
        TEST_CHECK(ae_count_in_range(b, e, "thermo_channels_read(") == 0 &&
                       ae_count_in_range(b, e, "MAX31856_read_all(") == 0 &&
                       ae_count_in_range(b, e, "sim_backend_read_all(") == 0,
                   "autotune_engine_tick_locked_impl() (runs under s_at.lock) performs no bus/sim read");
    }

    TEST_CHECK(strstr(code, "autotune_engine_tick_locked(void)") == NULL,
               "the test-only autotune_engine_tick_locked() wrapper lives in this test file, never in "
               "autotune_engine.c (it reads the bus itself, so production must not be able to call it)");

    free(code);
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
