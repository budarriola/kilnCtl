// Host tests for App/drivers/profile_executor.c's pre-start guard.
//
// Recovery mode (boot_guard.h) deliberately skips profile_executor_start()
// so the board can come up with just Wi-Fi and the OTA HTTP routes. Other
// code that DOES still run in that mode (safety_link.c's own poll task,
// found on the bench) calls into this module's public API anyway, and
// every one of those entry points used to take s_exec.lock -- a FreeRTOS
// mutex that does not exist until profile_executor_start() creates it.
// Taking a NULL mutex asserts inside FreeRTOS and panics the whole board
// (captured backtrace: safety_poll_task -> safety_build_and_send_context()
// -> profile_executor_get_status() -> xQueueSemaphoreTake() -> "assert
// failed: (( pxQueue ))"). Every public function now tests s_exec.lock == NULL
// as its first statement and returns a clean "not running" answer instead.
//
// This file #includes profile_executor.c directly (same convention as
// test_backup_import.c/test_zones_http.c) so it can reach and exercise the
// real guarded functions, never calling profile_executor_start() itself --
// the whole point is to prove every entry point survives being called while
// s_exec.lock is still NULL. Pulling in the whole translation unit means
// every OTHER extern symbol profile_executor.c references must still
// resolve at link time even though these tests never reach most of them
// (they all sit behind the very guard under test) -- hence the wide, mostly
// trivial stub surface below. pid.c/thermal_guard.c/heater_output.c/
// thermo_combine.c are linked in for real (see build_host_tests.ps1) since
// host tests for them already exist and this avoids yet another set of fakes
// for functions the guard never lets this file's tests reach anyway.
//
// Separate executable (own build_host_tests.ps1 step), same reason
// test_zones_http.c/test_safety_cfg_http.c are: this file defines the REAL
// zones_config_*()/profiles_http_get() etc. bodies (via #including
// profile_executor.c's dependency chain's declarations), and other host
// tests already define their OWN fakes of several of the same names --
// linking both into one binary would multiply-define those symbols.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "test_common.h"

#include "esp_err.h"

// Own executable (see this file's header comment) -- test_common.h's
// counters are defined once per host-test binary, same as test_main.c does
// for the main one.
int g_test_failures = 0;
int g_test_count = 0;

#include "../drivers/profile_executor.c"

// ---------------------------------------------------------------------------
// Stub bodies for every extern symbol profile_executor.c references that
// isn't linked in for real (see build_host_tests.ps1 for this executable).
// None of these is ever actually invoked by the tests below -- every call
// site sits behind the s_exec.lock == NULL guard under test -- but the whole
// translation unit must still link.
// ---------------------------------------------------------------------------

esp_err_t kiln_io_all_relays_off(kiln_io_t *io)
{
    (void)io;
    return ESP_OK;
}

uint8_t kiln_io_get_relay_shadow(kiln_io_t *io)
{
    (void)io;
    return 0;
}

/* Warm-start tests (PROFILES.md "Warm-start: joining a profile already at
 * temperature") need profile_executor_run()'s own start-of-run reading to
 * return real numbers instead of always failing -- settable so a test can
 * hand back canned per-channel temperatures, one MAX31856Reading per active
 * channel, same shape MAX31856_read_all() itself returns. Defaults to the
 * pre-existing behavior (ESP_FAIL, count 0) so every OTHER test in this file
 * is unaffected. */
static bool             s_test_thermo_read_ok = false;
static MAX31856Reading  s_test_thermo_readings[MAX31856_CHANNEL_COUNT];
static size_t           s_test_thermo_reading_count = 0;

esp_err_t MAX31856_read_all(MAX31856BusClass *bus, MAX31856Reading *out, size_t max_readings, size_t *out_count)
{
    (void)bus;
    if (!s_test_thermo_read_ok) {
        if (out_count) *out_count = 0;
        return ESP_FAIL;
    }
    size_t n = s_test_thermo_reading_count;
    if (n > max_readings) n = max_readings;
    if (out) memcpy(out, s_test_thermo_readings, n * sizeof(out[0]));
    if (out_count) *out_count = n;
    return ESP_OK;
}

/* Sets channel `channel`'s reading to `temp_c`, healthy (not spi_failed),
 * cj_temperature_c a plausible room temperature -- for a warm-start test
 * that wants zone `zone_index`'s combined reading to come out to `temp_c`
 * without needing a multi-channel thermo_mask fan-in (single-channel zones
 * cover every warm-start test below). */
static void set_test_thermo_reading(uint8_t channel, float temp_c)
{
    s_test_thermo_read_ok = true;
    if (channel >= MAX31856_CHANNEL_COUNT) return;
    memset(&s_test_thermo_readings[channel], 0, sizeof(s_test_thermo_readings[channel]));
    s_test_thermo_readings[channel].channel = channel;
    s_test_thermo_readings[channel].tc_temperature_c = temp_c;
    s_test_thermo_readings[channel].cj_temperature_c = 22.0f;
    s_test_thermo_readings[channel].spi_failed = false;
    if (s_test_thermo_reading_count <= channel) {
        s_test_thermo_reading_count = (size_t)channel + 1;
    }
}

static void reset_test_thermo_readings(void)
{
    s_test_thermo_read_ok = false;
    s_test_thermo_reading_count = 0;
    memset(s_test_thermo_readings, 0, sizeof(s_test_thermo_readings));
    s_exec.thermo_bus = NULL;
}

/* profile_executor_run()'s start-of-run read is gated on
 * `sim_backend_enabled() || (s_exec.thermo_bus && s_exec.thermo_bus->
 * initialized)` -- sim_backend_enabled() is compiled out false in this host
 * build (no CONFIG_KILNCTL_SIM_PLANT), so a warm-start test must also point
 * s_exec.thermo_bus at SOME initialized bus or MAX31856_read_all() above is
 * never even called. A fake, minimal bus is enough: nothing downstream of
 * the gate touches its fields, only its non-NULL-and-initialized-ness. */
static MAX31856BusClass s_test_thermo_bus;
static void arm_test_thermo_bus(void)
{
    memset(&s_test_thermo_bus, 0, sizeof(s_test_thermo_bus));
    s_test_thermo_bus.initialized = true;
    s_exec.thermo_bus = &s_test_thermo_bus;
}

bool autotune_engine_is_active_on_zone(uint8_t zone_index)
{
    (void)zone_index;
    return false;
}

/* Instrumentation for the leave-on-at-end tests below -- records every
 * relay/IO write instead of just swallowing it, so a test can assert on
 * whether a force-off write actually happened (or, for the leave-on case,
 * that it deliberately did NOT). */
static int g_relay_write_calls = 0;
static uint8_t g_last_relay_write_mask = 0;
static uint8_t g_last_relay_write_value = 0;
esp_err_t kiln_io_owner_command_set_relay_mask_authorized(uint8_t mask, uint8_t value)
{
    g_relay_write_calls++;
    g_last_relay_write_mask = mask;
    g_last_relay_write_value = value;
    return ESP_OK;
}

/* TODO relay/IO segments: io_seg_start()/io_seg_finish() call this for a
 * general-purpose IO_1..7 segment target, same as the relay stub just
 * above's role for a relay target. */
static int g_io_write_calls = 0;
static uint8_t g_last_io_write_index = 0;
static bool g_last_io_write_level = false;
esp_err_t kiln_io_owner_command_set_io(uint8_t index, bool level)
{
    g_io_write_calls++;
    g_last_io_write_index = index;
    g_last_io_write_level = level;
    return ESP_OK;
}

ota_interlock_result_t ota_http_check_interlocks(bool ack_no_safety_processor, char *reason_out, size_t reason_cap)
{
    (void)ack_no_safety_processor;
    if (reason_out && reason_cap) reason_out[0] = '\0';
    return OTA_INTERLOCK_OK;
}

bool ota_http_heat_blocked_by_update(char *reason_out, size_t reason_cap)
{
    if (reason_out && reason_cap) reason_out[0] = '\0';
    return false;
}

// Settable for B2's negative test below (test_run_refuses_while_zone_sweep_
// is_active()), which needs profile_executor_run() to get PAST this check
// and reach the new zones_current_sweep_is_active() gate. Every OTHER test
// in this file leaves this at its default (false, "no such profile"),
// matching the old hardcoded behavior exactly.
static bool s_test_profiles_http_get_ok = false;
static profile_t s_test_profiles_http_get_out;
bool profiles_http_get(uint8_t id, profile_t *out)
{
    (void)id;
    if (!s_test_profiles_http_get_ok) {
        if (out) memset(out, 0, sizeof(*out));
        return false;
    }
    if (out) *out = s_test_profiles_http_get_out;
    return true;
}

// Settable for the M13 fault-source-decode negative test below -- see
// s_test_profiles_http_get_ok's comment for the pattern. Default false
// (matching the old hardcoded behavior) for every other test in this file.
static bool s_test_relay_authority_blocked = false;
static uint32_t s_test_relay_authority_blocked_sources = 0;
bool relay_authority_on_blocked(SafetyLinkClass *safety, uint32_t *out_sources)
{
    (void)safety;
    if (out_sources) *out_sources = s_test_relay_authority_blocked ? s_test_relay_authority_blocked_sources : 0;
    return s_test_relay_authority_blocked;
}

// Configurable per-test (default false, matching the old hardcoded
// behavior for every other test in this file) -- see
// test_apply_relay_refreshes_heat_blocked_even_on_a_want_on_false_tick()
// below and the guard-scenario tests for why this needed to become
// controllable rather than a bare `return false;`.
static bool s_test_relay_authority_zone_blocked = false;
static uint32_t s_test_relay_authority_zone_blocked_sources = 0;
bool relay_authority_zone_blocked(SafetyLinkClass *safety, uint8_t zone_index, uint32_t *out_sources)
{
    (void)safety; (void)zone_index;
    if (out_sources) *out_sources = s_test_relay_authority_zone_blocked ? s_test_relay_authority_zone_blocked_sources : 0;
    return s_test_relay_authority_zone_blocked;
}

void relay_authority_set_zone_blocked(uint8_t zone_index, bool blocked)
{
    (void)zone_index; (void)blocked;
}

/* Instrumentation for the relay-claim-release tests below (see
 * test_escalate_guard_trip_*_releases_relay_claim() and
 * test_pause_keeps_claim_resume_reclaims_it()) -- these record what the real
 * relay_authority.c would have done instead of doing it, so a test can
 * assert on the exact mask/owner escalate_guard_trip()/profile_executor_
 * pause()/resume() handed over. */
static int g_relay_claim_calls = 0;
static uint8_t g_last_claim_mask = 0;
static relay_owner_t g_last_claim_owner = RELAY_OWNER_NONE;
static int g_relay_release_calls = 0;
static uint8_t g_last_release_mask = 0;

/* Overridable by test_escalate_guard_trip_all_zones_faulted_releases_relay_
 * claim() so escalate_guard_trip()'s "continue" branch (TODO.md 6A.3's
 * opt-in) can be reached without a second fake for the same symbol -- every
 * other test in this file wants the default (false, "abort the whole
 * firing"), which is why the initializer matches the existing stub's old
 * hard-coded `false`. */
static bool g_continue_on_zone_trip = false;

void relay_authority_claim_mask(uint8_t relay_mask, relay_owner_t owner)
{
    g_relay_claim_calls++;
    g_last_claim_mask = relay_mask;
    g_last_claim_owner = owner;
}

void relay_authority_release_mask(uint8_t relay_mask)
{
    g_relay_release_calls++;
    g_last_release_mask = relay_mask;
}

/* The shared heat claim (relay_authority.h) -- profile_executor_run() now
 * takes this atomically right before its final commit, on top of (not
 * instead of) the early, non-atomic zones_current_sweep_is_active() check
 * this file already stubs above. Default succeeds (false = "not blocked by
 * a sweep") so every existing test's control-path/full-real-path behavior
 * is unchanged; s_test_heat_zone_claim_refused lets
 * test_run_refuses_at_atomic_heat_claim_gate() below prove the LATE gate is
 * independently load-bearing, not just decorative alongside the early one. */
static bool s_test_heat_zone_claim_refused = false;
static int g_heat_zone_claim_begin_calls = 0;
static int g_heat_zone_claim_end_calls = 0;
static relay_heat_zone_claimant_t g_last_heat_zone_claimant = RELAY_HEAT_ZONE_CLAIM_PROFILE;

bool relay_authority_heat_zone_claim_begin(relay_heat_zone_claimant_t who)
{
    g_heat_zone_claim_begin_calls++;
    g_last_heat_zone_claimant = who;
    return !s_test_heat_zone_claim_refused;
}

void relay_authority_heat_zone_claim_end(relay_heat_zone_claimant_t who)
{
    g_heat_zone_claim_end_calls++;
    g_last_heat_zone_claimant = who;
}

esp_err_t relay_cycles_init(void)
{
    return ESP_OK;
}

void relay_cycles_add(uint8_t relay_mask, uint32_t cycles)
{
    (void)relay_mask; (void)cycles;
}

void relay_cycles_maybe_persist(void)
{
}

esp_err_t relay_cycles_flush(void)
{
    return ESP_OK;
}

esp_err_t run_state_init(void)
{
    return ESP_OK;
}

void run_state_note(run_state_phase_t phase, const run_state_snapshot_t *snap)
{
    (void)phase; (void)snap;
}

void run_state_note_progress(const run_state_snapshot_t *snap)
{
    (void)snap;
}

esp_err_t safety_link_get_status(SafetyLinkClass *link, safety_link_status_t *out)
{
    (void)link;
    if (out) memset(out, 0, sizeof(*out));
    return ESP_FAIL;
}

/* Spy state for the guard9_assert_stale_tick_fault() tests below (audit
 * 2026-08-27 item 2) -- every other test in this file leaves these unread. */
static int      g_set_fault_source_calls = 0;
static uint32_t g_last_fault_source_mask = 0;
static bool     g_last_fault_source_assert = false;

esp_err_t safety_link_set_fault_source(SafetyLinkClass *link, uint32_t source_mask, bool assert_fault)
{
    (void)link;
    g_set_fault_source_calls++;
    g_last_fault_source_mask = source_mask;
    g_last_fault_source_assert = assert_fault;
    return ESP_OK;
}

uint32_t safety_link_get_fault_sources(SafetyLinkClass *link)
{
    (void)link;
    return 0;
}

/* Spy for the heat-enable (K4) wiring tests below -- profile_executor.c now
 * calls heat_enable_acquire()/heat_enable_release() (heat_enable.h), and
 * heat_enable.c is linked into this executable for real rather than faked, so
 * the fake goes at the bottom of that stack: safety_link.c's own
 * request_enable, whose real contract is "enable=true is refused, and sends
 * nothing, on a down link; enable=false is always attempted". */
static int  g_request_enable_true_calls = 0;
static int  g_request_enable_false_calls = 0;
static bool g_request_enable_link_up = true;

esp_err_t safety_link_request_enable(SafetyLinkClass *link, bool enable)
{
    (void)link;
    if (enable) {
        g_request_enable_true_calls++;
        return g_request_enable_link_up ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    g_request_enable_false_calls++;
    return ESP_OK;
}

/* Puts heat_enable back in the state a RUNNING firing leaves it in: this
 * run holds a granted K4 request. The tests below drive profile_executor's
 * exit paths directly (they set s_exec.state by hand rather than going
 * through profile_executor_run(), which needs a whole task harness), so the
 * acquire that a real start would have done is done here instead. */
static void arm_heat_enable_as_if_running(void)
{
    g_request_enable_link_up = true;
    heat_enable_init((SafetyLinkClass *)0x1);
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);
    g_request_enable_true_calls = 0;
    g_request_enable_false_calls = 0;
}

float zones_config_apply_cal(uint8_t zone_index, float raw_c)
{
    (void)zone_index;
    return raw_c;
}

uint32_t zones_config_generation(void)
{
    return 0;
}

bool zones_config_get_continue_on_zone_trip(void)
{
    return g_continue_on_zone_trip;
}

/* Per-zone control mode, settable by test_profile_zones_have_ceiling_* below
 * (audit 2026-08-27, revised after the owner's live board reply) -- defaults
 * to all-OFF (zero-initialized, ZONE_CONTROL_MODE_OFF == 0), which matches
 * every pre-existing test in this file's assumption before this array
 * existed (this stub used to unconditionally return OFF). */
static zone_control_mode_t g_stub_control_mode[MAX31856_CHANNEL_COUNT];

bool zones_config_get_control_mode(uint8_t zone_index, zone_control_mode_t *out_mode)
{
    if (out_mode) {
        *out_mode = (zone_index < MAX31856_CHANNEL_COUNT) ? g_stub_control_mode[zone_index]
                                                            : ZONE_CONTROL_MODE_OFF;
    }
    return false;
}

/* PID_EXPANSION_PLAN.md Phase 3 wiring: profile_executor.c's
 * ZONE_CONTROL_MODE_PID_FUZZY path reads this every tick. This prestart
 * suite never gets a zone past profile_executor_run()'s pre-start guard
 * (that guard is the whole point of this file, see its header comment), so
 * the tick loop itself never runs here and this stub is only linked to
 * satisfy the symbol -- returning false (not configured) is fine. */
/* Settable for the pid_fuzzy_prepare_gains() regression tests below (opus
 * review: test_closed_loop.c's fuzzy_tick() MIRRORS this function's logic
 * instead of calling it, so a real divergence between the two would go
 * undetected there). Defaults to "not configured" (false, 0.0f), matching
 * every pre-existing test in this file, which never reaches the tick loop
 * anyway. */
static bool  s_test_fuzzy_strength_present = false;
static float s_test_fuzzy_strength_pct = 0.0f;
bool zones_config_get_fuzzy_strength_pct(uint8_t zone_index, float *out_pct)
{
    (void)zone_index;
    if (out_pct) *out_pct = s_test_fuzzy_strength_pct;
    return s_test_fuzzy_strength_present;
}

bool zones_config_get_cross_zone_delta(uint8_t zone_index, float *out_max_delta_c)
{
    (void)zone_index;
    if (out_max_delta_c) *out_max_delta_c = 0.0f;
    return false;
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

/* Settable per zone (defaults to 0, matching every pre-existing test in this
 * file, which relies on the ramp-ceiling check refusing any nonzero rate --
 * see test_run_refuses_at_atomic_heat_claim_gate()'s own comment on using a
 * zero-rate dwell to route around this stub). Warm-start tests need a real
 * ceiling so a >0 ramp_c_per_hr segment is actually feasible. */
static float g_stub_max_ramp_c_per_hr[MAX31856_CHANNEL_COUNT];
bool zones_config_get_max_ramp(uint8_t zone_index, float *out_c_per_hr)
{
    if (out_c_per_hr) *out_c_per_hr = (zone_index < MAX31856_CHANNEL_COUNT) ? g_stub_max_ramp_c_per_hr[zone_index] : 0.0f;
    return false;
}

uint8_t zones_config_get_max_simultaneous_relays(void)
{
    return 0;
}

bool zones_config_get_model(uint8_t zone_index, float *out_k_dc, float *out_tau_s, float *out_dead_time_s)
{
    (void)zone_index;
    if (out_k_dc) *out_k_dc = 0.0f;
    if (out_tau_s) *out_tau_s = 0.0f;
    if (out_dead_time_s) *out_dead_time_s = 0.0f;
    return false;
}

/* PID_EXPANSION_PLAN.md section 2c/Phase 3b: settable coupling matrix, one
 * row per zone, defaulting to all-zero (every pre-existing test never
 * touches this and gets exactly today's zero-coefficient feedforward). Test
 * fills g_stub_coupling_present[zi] to control whether the getter reports
 * "no row" (false) vs. "a real, possibly all-zero row" (true) -- both are
 * distinct legal states zones_http.h's real getter can return. */
static float g_stub_coupling[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT];
static bool  g_stub_coupling_present[MAX31856_CHANNEL_COUNT] = {true, true, true};
bool zones_config_get_coupling(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (zone_index >= MAX31856_CHANNEL_COUNT) return false;
    if (!g_stub_coupling_present[zone_index]) return false;
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        out_row[j] = g_stub_coupling[zone_index][j];
    }
    return true;
}

bool zones_config_get_pid(uint8_t zone_index, float *out_kp, float *out_ki, float *out_kd)
{
    (void)zone_index;
    if (out_kp) *out_kp = 0.0f;
    if (out_ki) *out_ki = 0.0f;
    if (out_kd) *out_kd = 0.0f;
    return false;
}

// Configurable per-test via g_stub_relay_mask (default all-zero, matching
// every pre-existing test in this file that never touches it -- apply_relay()
// early-returns with mask==0, which was fine before any test needed to
// drive apply_relay() itself for real). test_apply_relay_refreshes_heat_
// blocked_even_on_a_want_on_false_tick() and
// test_authority_block_at_partial_duty_guard3_arms_and_trips_on_a_welded_
// relay() below are the first tests that call apply_relay() directly and
// need it to proceed past the mask check.
static uint8_t g_stub_relay_mask[MAX31856_CHANNEL_COUNT];
bool zones_config_get_relay_mask(uint8_t zone_index, uint8_t *out_mask)
{
    uint8_t mask = (zone_index < MAX31856_CHANNEL_COUNT) ? g_stub_relay_mask[zone_index] : 0;
    if (out_mask) *out_mask = mask;
    return mask != 0;
}

bool zones_config_get_sanity_rate(uint8_t zone_index, float *out_c_per_min)
{
    (void)zone_index;
    if (out_c_per_min) *out_c_per_min = 0.0f;
    return false;
}

/* Per-zone max_temp_c, settable by test_profile_zones_have_ceiling_* below
 * (audit 2026-08-27 item 1/2) -- defaults to all-zero, which is what every
 * pre-existing test in this file that never touches this array assumes
 * (an unconfigured zone, guard 5's "no ceiling" reading). */
static float g_stub_max_temp_c[MAX31856_CHANNEL_COUNT];

bool zones_config_get_temp_limits(uint8_t zone_index, float *out_max_temp_c, float *out_min_temp_c)
{
    if (out_max_temp_c) *out_max_temp_c = (zone_index < MAX31856_CHANNEL_COUNT) ? g_stub_max_temp_c[zone_index] : 0.0f;
    if (out_min_temp_c) *out_min_temp_c = 0.0f;
    return false;
}

uint8_t zones_config_get_thermo_count(void)
{
    return 0;
}

/* Settable per zone (defaults to 0/false, matching every pre-existing test
 * in this file that never touches this array) -- warm-start tests need
 * zone N's thermo_mask to actually name a channel so thermo_combine() (real,
 * linked for this executable) produces a valid combined reading from the
 * canned MAX31856_read_all() data above. Bit i = channel i, same convention
 * as the real zones_http.c-owned mask. */
static uint8_t g_stub_thermo_mask[MAX31856_CHANNEL_COUNT];
bool zones_config_get_thermo_mask(uint8_t zone_index, uint8_t *out_mask)
{
    if (out_mask) *out_mask = (zone_index < MAX31856_CHANNEL_COUNT) ? g_stub_thermo_mask[zone_index] : 0;
    return zone_index < MAX31856_CHANNEL_COUNT && g_stub_thermo_mask[zone_index] != 0;
}

// Settable for B2's negative test below -- see s_test_profiles_http_get_ok's
// comment. Default false (matching the old hardcoded behavior) for every
// other test in this file.
static bool s_test_zones_config_valid = false;
bool zones_config_is_valid(void)
{
    return s_test_zones_config_valid;
}

// B2 (opus review, 2026-08-27): profile_executor_run() now refuses to start
// while a zone current sweep is active -- see zones_current_sweep_is_active()'s
// doc comment (zones_http.h). Settable so test_run_refuses_while_zone_sweep_
// is_active() below can exercise the real refusal; every other test in this
// file leaves it at its default (false), so profile_executor_run() staying
// unreachable in prestart tests (s_exec.lock == NULL refuses first) is
// unaffected.
static bool s_test_sweep_active = false;
bool zones_current_sweep_is_active(void)
{
    return s_test_sweep_active;
}

// ---------------------------------------------------------------------------
// Tests -- profile_executor_start() is DELIBERATELY never called anywhere in
// this file. s_exec is a static struct with internal linkage in
// profile_executor.c, zero-initialized by the C runtime before main() runs
// (same as a real cold boot before app_main() calls profile_executor_start()),
// so s_exec.lock reads NULL here exactly as it does on a real board that has
// skipped profile_executor_start() for recovery mode.
// ---------------------------------------------------------------------------

static void test_run_refuses_before_start(void)
{
    TEST_SECTION("profile_executor_run() before start() -- refused, no crash");
    char err[96] = {0};
    bool ok = profile_executor_run(0, err, sizeof(err));
    TEST_CHECK(!ok, "must refuse, not crash, when s_exec.lock is NULL");
    TEST_CHECK(err[0] != '\0', "an error message is filled in for the caller");
}

static void test_halt_is_a_silent_noop_before_start(void)
{
    TEST_SECTION("profile_executor_halt() before start() -- returns, no crash");
    profile_executor_halt(); // must simply return
    TEST_CHECK(true, "reached this line without crashing");
}

static void test_pause_resume_refuse_before_start(void)
{
    TEST_SECTION("profile_executor_pause()/resume() before start() -- refused, no crash");
    TEST_CHECK(!profile_executor_pause(), "pause must return false, not crash");
    TEST_CHECK(!profile_executor_resume(), "resume must return false, not crash");
}

static void test_zone_is_active_false_before_start(void)
{
    TEST_SECTION("profile_executor_zone_is_active() before start() -- false, no crash");
    TEST_CHECK(!profile_executor_zone_is_active(0), "no zone can be active before the executor has ever started");
}

static void test_get_history_empty_before_start(void)
{
    TEST_SECTION("profile_executor_get_history_count()/get_history() before start() -- empty, no crash");
    TEST_CHECK(profile_executor_get_history_count() == 0, "history count must read 0, not garbage");
    profile_history_entry_t buf[4];
    size_t n = profile_executor_get_history(buf, 0, 4);
    TEST_CHECK(n == 0, "get_history must copy nothing before the executor has ever started");
}

static void test_get_status_reports_well_formed_idle_before_start(void)
{
    TEST_SECTION("profile_executor_get_status() before start() -- well-formed IDLE snapshot, no crash");
    // Deliberately poisoned first, so a pass here proves the function itself
    // zeroed/filled the struct rather than the caller's stack happening to
    // already be zero -- this is exactly the "never hand back uninitialised
    // stack memory" requirement.
    profile_exec_status_t out;
    memset(&out, 0xAA, sizeof(out));

    profile_executor_get_status(&out);

    TEST_CHECK(out.state == PROFILE_EXEC_IDLE, "state must read IDLE, the module's existing idle sentinel");
    TEST_CHECK(out.profile_id == 0, "profile_id must not carry poisoned stack bytes");
    TEST_CHECK(out.zone_mask == 0, "zone_mask must not carry poisoned stack bytes");
    TEST_CHECK(out.segment_index == 0, "segment_index must not carry poisoned stack bytes");
    TEST_CHECK(out.total_elapsed_s == 0, "total_elapsed_s must not carry poisoned stack bytes");
    TEST_CHECK(out.fault_reason[0] == '\0', "fault_reason must not carry poisoned stack bytes");
    bool any_zone_active = false;
    for (int i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        if (out.zones[i].active) any_zone_active = true;
    }
    TEST_CHECK(!any_zone_active, "no zone may read active in a pre-start snapshot");
}

// ---------------------------------------------------------------------------
// Relay-claim-release tests (defect fix, 2026-08-27) -- see profile_
// executor.c's release_profile_relay_claim() doc comment.
//
// escalate_guard_trip() is a plain static function, not a FreeRTOS task
// loop, and does not itself check s_exec.lock -- so unlike the guard tested
// above, it IS reachable directly from a host test without ever calling
// profile_executor_start(). This is the honest limit of what this harness
// can prove: the control task's tick loop (executor_task_entry, where the
// DONE transition and the PID zone/sensor defect both live) and the
// watchdog task loop (watchdog_task_entry, where the guard-9-forced FAULTED
// transition lives) are real `for (;;) { vTaskDelay(...); ... }` bodies that
// would spin forever if called directly -- there is no seam to call just
// one tick's worth of either without restructuring the module, which this
// pass was told not to do. escalate_guard_trip() shares the exact same
// release_profile_relay_claim() call this fix added to those three
// unreachable sites, so proving it here is the closest honest proxy for all
// four sites' correctness -- but it is NOT proof that the DONE and
// watchdog-FAULT call sites are actually wired up; that was checked by
// reading the diff, not by a test that can reach them.
//
// Each test resets the whole s_exec struct and the claim/release spy
// counters first, so these are independent of each other and of every test
// above (none of which touches zones[]/claimed_relay_mask).
static void reset_relay_claim_test_state(void)
{
    memset(&s_exec, 0, sizeof(s_exec));
    g_relay_claim_calls = 0;
    g_last_claim_mask = 0;
    g_last_claim_owner = RELAY_OWNER_NONE;
    g_relay_release_calls = 0;
    g_last_release_mask = 0;
    g_continue_on_zone_trip = false;
    g_heat_zone_claim_begin_calls = 0;
    g_heat_zone_claim_end_calls = 0;
    g_last_heat_zone_claimant = RELAY_HEAT_ZONE_CLAIM_PROFILE;
}

static void test_escalate_guard_trip_global_releases_relay_claim(void)
{
    TEST_SECTION("escalate_guard_trip() GLOBAL trip -- releases the run's relay claim");
    reset_relay_claim_test_state();
    s_exec.zones[0].active = true;
    s_exec.claimed_relay_mask = 0x03;

    bool run_faulted = escalate_guard_trip(0, THERMAL_GUARD_TRIP_MAX_TEMP, "over-temp");

    TEST_CHECK(run_faulted, "a global reason must fault the whole run");
    TEST_CHECK(s_exec.state == PROFILE_EXEC_FAULTED, "state must be FAULTED");
    TEST_CHECK(g_relay_release_calls == 1, "relay_authority_release_mask() must be called exactly once");
    TEST_CHECK(g_last_release_mask == 0x03, "must release exactly claimed_relay_mask, not some other mask");
}

static void test_escalate_guard_trip_abort_policy_releases_relay_claim(void)
{
    TEST_SECTION("escalate_guard_trip() per-zone trip, abort-whole-firing policy -- releases the relay claim");
    reset_relay_claim_test_state();
    /* g_continue_on_zone_trip is false (the default, TODO.md 6A.3's "abort
     * the whole firing" policy), and only zone 0 is active, so a single
     * per-zone trip takes the abort-policy branch and ends the run. */
    s_exec.zones[0].active = true;
    s_exec.claimed_relay_mask = 0x01;

    bool run_faulted = escalate_guard_trip(0, THERMAL_GUARD_TRIP_HEATING_FAILED, "zone 0 guard 1");

    TEST_CHECK(run_faulted, "abort-whole-firing policy must fault the run on a single zone's trip");
    TEST_CHECK(s_exec.state == PROFILE_EXEC_FAULTED, "state must be FAULTED");
    TEST_CHECK(g_relay_release_calls == 1, "relay_authority_release_mask() must be called exactly once");
    TEST_CHECK(g_last_release_mask == 0x01, "must release exactly claimed_relay_mask");
}

static void test_escalate_guard_trip_all_zones_faulted_releases_relay_claim(void)
{
    TEST_SECTION("escalate_guard_trip() per-zone trip, continue-on-trip policy -- "
                 "releases only once EVERY active zone has faulted");
    reset_relay_claim_test_state();
    g_continue_on_zone_trip = true; /* opt-in: the run continues on other active zones */
    s_exec.zones[0].active = true;
    s_exec.zones[1].active = true;
    s_exec.claimed_relay_mask = 0x0F;

    bool run_faulted_after_first = escalate_guard_trip(0, THERMAL_GUARD_TRIP_HEATING_FAILED, "zone 0 guard 1");
    TEST_CHECK(!run_faulted_after_first, "zone 1 is still healthy -- the run must NOT fault yet");
    TEST_CHECK(s_exec.state != PROFILE_EXEC_FAULTED, "state must not be FAULTED while zone 1 is still active");
    TEST_CHECK(g_relay_release_calls == 0,
              "the claim must NOT be released while the run is still in progress on zone 1 -- "
              "this is the same 'a paused/still-running run keeps its claim' property the fix must preserve");

    bool run_faulted_after_second = escalate_guard_trip(1, THERMAL_GUARD_TRIP_HEATING_FAILED, "zone 1 guard 1");
    TEST_CHECK(run_faulted_after_second, "the last active zone faulting must end the run");
    TEST_CHECK(s_exec.state == PROFILE_EXEC_FAULTED, "state must be FAULTED once every active zone has faulted");
    TEST_CHECK(g_relay_release_calls == 1, "relay_authority_release_mask() must be called exactly once, on the "
                                            "trip that actually ends the run");
    TEST_CHECK(g_last_release_mask == 0x0F, "must release exactly claimed_relay_mask");
}

static void test_pause_keeps_claim_resume_reclaims_it(void)
{
    TEST_SECTION("profile_executor_pause()/resume() -- pause hands the claim to MANUAL (not NONE), "
                 "resume reclaims PROFILE");
    reset_relay_claim_test_state();
    /* Unlike the escalate_guard_trip() tests above, pause()/resume() DO
     * check s_exec.lock first, so this test needs a real (stub) mutex --
     * see stubs/freertos/semphr.h's xSemaphoreCreateMutex(), which is safe
     * to call directly in a single-threaded host test. */
    s_exec.lock = xSemaphoreCreateMutex();
    s_exec.state = PROFILE_EXEC_RUNNING;
    s_exec.claimed_relay_mask = 0x05;

    bool paused = profile_executor_pause();
    TEST_CHECK(paused, "pause() must succeed from RUNNING");
    TEST_CHECK(s_exec.state == PROFILE_EXEC_PAUSED, "state must be PAUSED");
    TEST_CHECK(g_relay_release_calls == 0, "pause() must NEVER release the claim -- a paused run is still a "
                                            "run in progress (TODO.md section 0)");
    TEST_CHECK(g_relay_claim_calls == 1 && g_last_claim_owner == RELAY_OWNER_MANUAL && g_last_claim_mask == 0x05,
              "pause() must hand the claim to RELAY_OWNER_MANUAL, not release it");

    bool resumed = profile_executor_resume();
    TEST_CHECK(resumed, "resume() must succeed from PAUSED");
    TEST_CHECK(s_exec.state == PROFILE_EXEC_RUNNING, "state must be back to RUNNING");
    TEST_CHECK(g_relay_release_calls == 0, "resume() must not release the claim either");
    TEST_CHECK(g_last_claim_owner == RELAY_OWNER_PROFILE && g_last_claim_mask == 0x05,
              "resume() must reclaim RELAY_OWNER_PROFILE over the same mask");
}

// guard9_assert_stale_tick_fault() tests (audit 2026-08-27 item 2) --
// guard9_assert_stale_tick_fault() is a plain static function, same
// "reachable without a real task loop" case as escalate_guard_trip() above
// (see that block's comment); watchdog_task_entry() itself is the
// unreachable for(;;) loop this was factored out of.
static void test_guard9_asserts_and_ors_global_fault_source(void)
{
    TEST_SECTION("guard9_assert_stale_tick_fault() -- asserts SAFETY_FAULT_SRC_APP and OR's it into "
                 "global_fault_source");
    reset_relay_claim_test_state();
    /* Non-NULL just to take the `if (s_exec.safety)` branch -- the stub
     * safety_link_set_fault_source() above never dereferences it. */
    s_exec.safety = (SafetyLinkClass *)0x1;
    g_set_fault_source_calls = 0;

    guard9_assert_stale_tick_fault();

    TEST_CHECK(g_set_fault_source_calls == 1, "must call safety_link_set_fault_source() exactly once");
    TEST_CHECK(g_last_fault_source_mask == SAFETY_FAULT_SRC_APP, "must assert exactly SAFETY_FAULT_SRC_APP");
    TEST_CHECK(g_last_fault_source_assert == true, "must assert (true), not clear");
    TEST_CHECK(s_exec.global_fault_source == SAFETY_FAULT_SRC_APP,
              "must record the bit in global_fault_source, same bookkeeping escalate_guard_trip() uses, "
              "so clear_this_runs_faults() (profile_executor_halt()) knows to release it -- the defect "
              "this fixes: nothing ever deasserted SAFETY_FAULT_SRC_APP before, latching heat off board-wide "
              "until reboot");
}

static void test_guard9_ors_without_clobbering_an_earlier_global_trip(void)
{
    TEST_SECTION("guard9_assert_stale_tick_fault() -- OR's into an existing global_fault_source instead of "
                 "overwriting it");
    reset_relay_claim_test_state();
    s_exec.safety = (SafetyLinkClass *)0x1;
    /* A global guard trip (e.g. THERMAL_SANITY) already asserted before the
     * control task's tick went stale -- both bits must still be present
     * afterwards, or clear_this_runs_faults() would only ever clear whichever
     * one this function last wrote, permanently losing the other. */
    s_exec.global_fault_source = SAFETY_FAULT_SRC_THERMAL_SANITY;

    guard9_assert_stale_tick_fault();

    TEST_CHECK(s_exec.global_fault_source == (SAFETY_FAULT_SRC_THERMAL_SANITY | SAFETY_FAULT_SRC_APP),
              "both the earlier trip's bit and SAFETY_FAULT_SRC_APP must survive");
}

static void test_guard9_fault_source_cleared_on_halt(void)
{
    TEST_SECTION("guard9_assert_stale_tick_fault() then profile_executor_halt() -- the operator's halt "
                 "deasserts SAFETY_FAULT_SRC_APP, closing the loop this defect left open");
    reset_relay_claim_test_state();
    s_exec.lock = xSemaphoreCreateMutex();
    s_exec.safety = (SafetyLinkClass *)0x1;
    s_exec.state = PROFILE_EXEC_FAULTED; /* watchdog's own WD_ACTION_FAULT transition */
    s_exec.claimed_relay_mask = 0x0F;

    guard9_assert_stale_tick_fault();
    TEST_CHECK(s_exec.global_fault_source == SAFETY_FAULT_SRC_APP, "sanity: the assert above landed");

    g_set_fault_source_calls = 0;
    profile_executor_halt();

    TEST_CHECK(g_set_fault_source_calls == 1, "halt() must call safety_link_set_fault_source() to clear it");
    TEST_CHECK(g_last_fault_source_mask == SAFETY_FAULT_SRC_APP, "must clear exactly the mask that was asserted");
    TEST_CHECK(g_last_fault_source_assert == false, "must clear (false), not assert again");
    TEST_CHECK(s_exec.global_fault_source == 0, "global_fault_source must be back to 0 after halt()");
}

// profile_zones_have_ceiling() tests (audit 2026-08-27 items 1/2: "Guard 5's
// absolute ceiling is off by default" / "max_temp_c == 0 and
// max_ramp_c_per_hr == 0 mean opposite things"). This is a plain static
// function over a profile_t and the zones_config_get_temp_limits() stub
// above -- reachable without profile_executor_start()'s full harness, same
// "factor the check out so a host test can drive it directly" reasoning as
// escalate_guard_trip()/guard9_assert_stale_tick_fault() elsewhere in this
// file.
static void test_profile_zones_have_ceiling_refuses_on_zero(void)
{
    TEST_SECTION("profile_zones_have_ceiling() -- refuses when a zone that CAN heat has max_temp_c == 0 "
                 "(the exact defect the 2026-08-27 audit named: guard 5 permanently a no-op)");
    memset(g_stub_max_temp_c, 0, sizeof(g_stub_max_temp_c));
    memset(g_stub_control_mode, 0, sizeof(g_stub_control_mode));
    g_stub_control_mode[0] = ZONE_CONTROL_MODE_BANGBANG; /* zone 0 CAN command heat */
    profile_t p;
    memset(&p, 0, sizeof(p));
    p.zone_mask = 0x01; /* zone 0 only */

    uint8_t missing_zone = 0xFF;
    bool ok = profile_zones_have_ceiling(&p, &missing_zone);

    TEST_CHECK(!ok, "must refuse -- zone 0 can heat and its max_temp_c is 0 (never commissioned)");
    TEST_CHECK(missing_zone == 0, "must name zone 0 as the offender");
}

static void test_profile_zones_have_ceiling_passes_when_configured(void)
{
    TEST_SECTION("profile_zones_have_ceiling() -- a heating zone with a real ceiling fires exactly as "
                 "before (a change that blocks every firing is not a fix). Mirrors the owner's live "
                 "board read (GET /api/zones, 2026-08-27): zone0 max_temp_c=1300, control_mode=2 (PID)");
    memset(g_stub_max_temp_c, 0, sizeof(g_stub_max_temp_c));
    memset(g_stub_control_mode, 0, sizeof(g_stub_control_mode));
    g_stub_max_temp_c[0] = 1300.0f; /* operator commissioned zone 0 with a real ceiling */
    g_stub_control_mode[0] = ZONE_CONTROL_MODE_PID;
    profile_t p;
    memset(&p, 0, sizeof(p));
    p.zone_mask = 0x01;

    uint8_t missing_zone = 0xFF;
    bool ok = profile_zones_have_ceiling(&p, &missing_zone);

    TEST_CHECK(ok, "must NOT refuse -- zone 0 has a real, operator-set 1300C ceiling");
    TEST_CHECK(missing_zone == 0xFF, "out_missing_zone must be left untouched on success");
}

static void test_profile_zones_have_ceiling_ignores_inactive_zones(void)
{
    TEST_SECTION("profile_zones_have_ceiling() -- a zone NOT in zone_mask never blocks the firing, "
                 "even with max_temp_c == 0 and a mode that could heat");
    memset(g_stub_max_temp_c, 0, sizeof(g_stub_max_temp_c));
    memset(g_stub_control_mode, 0, sizeof(g_stub_control_mode));
    g_stub_max_temp_c[0] = 1200.0f;
    g_stub_control_mode[0] = ZONE_CONTROL_MODE_BANGBANG;
    /* g_stub_max_temp_c[1] stays 0 and g_stub_control_mode[1] stays OFF, but the point of THIS test
     * is that zone 1 is excluded from the mask -- see the next test for the OFF-but-in-mask case. */
    profile_t p;
    memset(&p, 0, sizeof(p));
    p.zone_mask = 0x01; /* zone 0 only -- zone 1 excluded */

    uint8_t missing_zone = 0xFF;
    bool ok = profile_zones_have_ceiling(&p, &missing_zone);

    TEST_CHECK(ok, "must NOT refuse -- the only active zone (0) has a real ceiling");
}

static void test_profile_zones_have_ceiling_ignores_off_zones_in_mask(void)
{
    TEST_SECTION("profile_zones_have_ceiling() -- a zone IN zone_mask but control_mode OFF never "
                 "blocks the firing even with max_temp_c == 0: OFF cannot command a relay "
                 "(zones_http.h's ZONE_CONTROL_MODE_OFF doc comment, confirmed against "
                 "heater_output_duty()'s switch), so guard 5 has nothing to protect on that zone. "
                 "Reproduces the owner's live board exactly (GET /api/zones, 2026-08-27): zone1 and "
                 "zone2 both read max_temp_c=0.0, control_mode=0 (OFF) -- refusing over either of "
                 "them would be a nuisance refusal SAFETY_MODEL.md's doctrine warns against, on a "
                 "board where the only zone that actually heats (zone0) already has a ceiling");
    memset(g_stub_max_temp_c, 0, sizeof(g_stub_max_temp_c));
    memset(g_stub_control_mode, 0, sizeof(g_stub_control_mode));
    g_stub_max_temp_c[0] = 1300.0f;
    g_stub_control_mode[0] = ZONE_CONTROL_MODE_PID;
    /* zone1/zone2: max_temp_c 0, control_mode OFF (both arrays' zero-init default) -- IN the mask. */
    profile_t p;
    memset(&p, 0, sizeof(p));
    p.zone_mask = 0x07; /* zones 0, 1, 2 -- matches the owner's 3-zone board */

    uint8_t missing_zone = 0xFF;
    bool ok = profile_zones_have_ceiling(&p, &missing_zone);

    TEST_CHECK(ok, "must NOT refuse -- zones 1/2 are OFF and cannot heat, zone 0 (the only zone that "
                   "can) has a real ceiling");
    TEST_CHECK(missing_zone == 0xFF, "out_missing_zone must be left untouched on success");
}

static void test_profile_zones_have_ceiling_still_refuses_on_zero_when_off_zone_is_healthy(void)
{
    TEST_SECTION("profile_zones_have_ceiling() -- the OFF carve-out does not blind the check to a "
                 "DIFFERENT zone that CAN heat and has no ceiling (proves the two zones are checked "
                 "independently, not that the function just gave up refusing)");
    memset(g_stub_max_temp_c, 0, sizeof(g_stub_max_temp_c));
    memset(g_stub_control_mode, 0, sizeof(g_stub_control_mode));
    /* zone0: OFF, no ceiling -- fine, cannot heat. */
    /* zone1: CAN heat (BANGBANG), no ceiling -- must still refuse. */
    g_stub_control_mode[1] = ZONE_CONTROL_MODE_BANGBANG;
    profile_t p;
    memset(&p, 0, sizeof(p));
    p.zone_mask = 0x03; /* zones 0 and 1 */

    uint8_t missing_zone = 0xFF;
    bool ok = profile_zones_have_ceiling(&p, &missing_zone);

    TEST_CHECK(!ok, "must refuse -- zone 1 can heat and has no ceiling, regardless of zone 0's OFF state");
    TEST_CHECK(missing_zone == 1, "must name zone 1, the actual offender");
}

// ---------------------------------------------------------------------------
// TODO relay/IO segments -- mandatory negative test #4: a non-blocking
// segment with "leave on" UNSET really is forced off at run end, on the
// DONE path AND on a fault/halt path. Plus the positive case (leave_on_at_end
// SET is honored ONLY on the DONE/honor_leave_on=true path) and the
// io_segs_force_all_off() sweep used by every FAULTED/HALT/watchdog call
// site, which must reach general-purpose IO_1..7 too (kiln_io_all_relays_off()
// alone never does).
// ---------------------------------------------------------------------------

static void reset_io_seg_test_state(void)
{
    memset(&s_exec, 0, sizeof(s_exec));
    s_exec.io = (kiln_io_t *)0x1; /* non-NULL dummy -- these two stubs ignore it entirely */
    g_relay_write_calls = 0;
    g_last_relay_write_mask = 0;
    g_last_relay_write_value = 0;
    g_io_write_calls = 0;
    g_last_io_write_index = 0;
    g_last_io_write_level = false;
    g_relay_claim_calls = 0;
    g_relay_release_calls = 0;
}

static void test_io_seg_finish_default_forces_off_on_done(void)
{
    TEST_SECTION("io_seg_finish -- leave_on_at_end UNSET (the mandatory default) is forced off even on the "
                 "honor_leave_on=true (DONE) path");
    reset_io_seg_test_state();

    profile_segment_t seg;
    memset(&seg, 0, sizeof(seg));
    seg.seg_kind = PROFILE_SEG_KIND_RELAY_IO;
    seg.io_target = 2; /* relay 2 */
    seg.io_state = 1;  /* ON */
    seg.io_blocking = 0;
    seg.io_leave_on_at_end = 0; /* the mandatory default -- see profiles_http.h */
    seg.dwell_min = 5;

    io_seg_start(0, &seg);
    TEST_CHECK(g_relay_write_calls == 1, "starting the segment writes the relay ON once");
    TEST_CHECK(g_last_relay_write_value != 0, "the write commanded it ON");

    /* Simulate the run reaching its clean DONE end while this segment is
     * still active -- exactly the segment-stepping block's io_segs_force_
     * all_off(true) call site. */
    io_seg_finish(0, /*honor_leave_on=*/true);

    TEST_CHECK(g_relay_write_calls == 2, "DONE with leave_on_at_end UNSET must still force the relay OFF");
    TEST_CHECK(g_last_relay_write_value == 0, "the force-off write commands it OFF, not left as-is");
    TEST_CHECK(!s_exec.io_segs[0].active, "the segment is no longer tracked as active");
}

static void test_io_seg_finish_leave_on_honored_only_on_done(void)
{
    TEST_SECTION("io_seg_finish -- leave_on_at_end SET is honored on the DONE path, forced off on every "
                 "other (FAULT/HALT-style) path");
    reset_io_seg_test_state();

    profile_segment_t seg;
    memset(&seg, 0, sizeof(seg));
    seg.seg_kind = PROFILE_SEG_KIND_RELAY_IO;
    seg.io_target = 3; /* relay 3 */
    seg.io_state = 1;
    seg.io_blocking = 0;
    seg.io_leave_on_at_end = 1;
    seg.dwell_min = 5;

    io_seg_start(0, &seg);
    TEST_CHECK(g_relay_write_calls == 1, "starting the segment writes the relay ON once");

    io_seg_finish(0, true); /* the DONE path */
    TEST_CHECK(g_relay_write_calls == 1, "DONE path with leave_on_at_end SET must NOT write the relay off");
    TEST_CHECK(!s_exec.io_segs[0].active, "still marked finished/handed off, even though left energized");
    TEST_CHECK(g_relay_release_calls >= 1, "ownership is released so the relay becomes manually reachable, "
                                          "not stranded as unowned-but-still-PROFILE-claimed");

    reset_io_seg_test_state();
    io_seg_start(0, &seg); /* same segment, same leave_on_at_end=1 */
    TEST_CHECK(g_relay_write_calls == 1, "starting the segment writes the relay ON once (second setup)");
    io_seg_finish(0, false); /* every FAULTED/HALT/watchdog call site passes honor_leave_on=false */
    TEST_CHECK(g_relay_write_calls == 2, "a FAULT/HALT-style finish forces the relay off regardless of "
                                        "leave_on_at_end -- the safe default wins over the segment's own "
                                        "preference on an abnormal stop");
    TEST_CHECK(g_last_relay_write_value == 0, "the force-off write commands it OFF");
}

static void test_io_segs_force_all_off_sweeps_general_io_too(void)
{
    TEST_SECTION("io_segs_force_all_off -- a FAULTED/HALT sweep forces off every active relay/IO segment, "
                 "general-purpose IO_1..7 included (kiln_io_all_relays_off() alone never reaches those pins)");
    reset_io_seg_test_state();

    profile_segment_t seg_relay, seg_io;
    memset(&seg_relay, 0, sizeof(seg_relay));
    seg_relay.seg_kind = PROFILE_SEG_KIND_RELAY_IO;
    seg_relay.io_target = 1;
    seg_relay.io_state = 1;
    seg_relay.io_leave_on_at_end = 1; /* even set, an abnormal-stop sweep must ignore it */

    memset(&seg_io, 0, sizeof(seg_io));
    seg_io.seg_kind = PROFILE_SEG_KIND_RELAY_IO;
    seg_io.io_target = PROFILE_IO_TARGET_IO_BASE; /* IO_1 */
    seg_io.io_state = 1;
    seg_io.io_leave_on_at_end = 1;

    io_seg_start(0, &seg_relay);
    io_seg_start(1, &seg_io);
    TEST_CHECK(g_relay_write_calls == 1 && g_io_write_calls == 1, "both segments applied their ON command");

    io_segs_force_all_off(false); /* the HALT/FAULTED/watchdog call */

    TEST_CHECK(g_relay_write_calls == 2, "the relay segment was force-off written");
    TEST_CHECK(g_last_relay_write_value == 0, "...commanding it OFF");
    TEST_CHECK(g_io_write_calls == 2, "the general-purpose IO segment was ALSO force-off written");
    TEST_CHECK(!g_last_io_write_level, "...commanding it OFF");
    TEST_CHECK(!s_exec.io_segs[0].active && !s_exec.io_segs[1].active, "both segments end inactive");
}

// B2 (opus review, 2026-08-27): profile_executor_run() must refuse while a
// zone current sweep is active. Reachable without profile_executor_start()'s
// full harness the same way the pause/resume tests above are: a stub mutex
// in s_exec.lock is enough to get past the "not started" guard, and every
// check profile_executor_run() makes BEFORE reaching the new B2 gate
// (profiles_http_get/segment_count/zone_mask/zones_config_is_valid/
// ota_http_heat_blocked_by_update) is stubbed to pass cleanly above.
static void test_run_refuses_while_zone_sweep_is_active(void)
{
    TEST_SECTION("profile_executor_run() refuses while a zone current sweep is active (B2)");
    reset_relay_claim_test_state();
    s_exec.lock = xSemaphoreCreateMutex();
    s_exec.state = PROFILE_EXEC_IDLE;

    memset(&s_test_profiles_http_get_out, 0, sizeof(s_test_profiles_http_get_out));
    s_test_profiles_http_get_out.zone_mask = 0x01;
    s_test_profiles_http_get_out.segment_count = 1;
    s_test_profiles_http_get_ok = true;
    s_test_zones_config_valid = true;
    s_test_sweep_active = true;

    char err[128];
    err[0] = '\0';
    bool ok = profile_executor_run(0, err, sizeof(err));

    TEST_CHECK(!ok, "B2: a live zone sweep must refuse the firing, not merely warn");
    TEST_CHECK(strstr(err, "sweep") != NULL, "the refusal must name the sweep specifically");
    TEST_CHECK(s_exec.state == PROFILE_EXEC_IDLE, "a refused run must never transition out of IDLE");

    // Control case: with the sweep NOT active, the same setup must NOT be
    // refused for this reason (it will still fail further down this
    // function's real logic, which this stub surface does not fully satisfy
    // -- the point here is only that it is not refused for the SWEEP reason).
    s_test_sweep_active = false;
    err[0] = '\0';
    profile_executor_run(0, err, sizeof(err));
    TEST_CHECK(strstr(err, "sweep") == NULL,
              "control: with no sweep active, the refusal (if any) must not claim a sweep is running");

    s_test_profiles_http_get_ok = false;
    s_test_zones_config_valid = false;
    s_test_sweep_active = false;
}

// ROADMAP.md M13: the "heat is blocked" refusal used to show the operator a
// bare fault-source hex value (0x%02X) instead of naming what tripped it --
// the same defect class S6a was the motivating example for. Proves the fix
// is real: relay_authority_on_blocked() returning a real mask must produce a
// message containing an actual decoded source name, never the literal "0x"
// a hex format specifier would leave behind.
static void test_run_decodes_fault_sources_instead_of_hex(void)
{
    TEST_SECTION("profile_executor_run() decodes fault sources for the operator (M13)");
    reset_relay_claim_test_state();
    s_exec.lock = xSemaphoreCreateMutex();
    s_exec.state = PROFILE_EXEC_IDLE;

    memset(&s_test_profiles_http_get_out, 0, sizeof(s_test_profiles_http_get_out));
    s_test_profiles_http_get_out.zone_mask = 0x01;
    s_test_profiles_http_get_out.segment_count = 1;
    s_test_profiles_http_get_ok = true;
    s_test_zones_config_valid = true;
    s_test_relay_authority_blocked = true;
    s_test_relay_authority_blocked_sources = 0x02u; // "PC control link lost" -- safety_trip_words.h bit 0x02

    char err[128];
    err[0] = '\0';
    bool ok = profile_executor_run(0, err, sizeof(err));

    TEST_CHECK(!ok, "a blocked relay authority must refuse the run");
    TEST_CHECK(strstr(err, "0x") == NULL, "M13: the message must not fall back to a bare hex value");
    TEST_CHECK(strstr(err, "PC control link lost") != NULL,
              "the message must name the actual source, not just say something is blocked");

    s_test_profiles_http_get_ok = false;
    s_test_zones_config_valid = false;
    s_test_relay_authority_blocked = false;
    s_test_relay_authority_blocked_sources = 0;
}

// The shared heat claim's atomic gate (relay_authority.h) -- proves the LATE
// gate right before the final commit is independently load-bearing, not
// merely decorative alongside the early, non-atomic
// zones_current_sweep_is_active() check test_run_refuses_while_zone_sweep_
// is_active() above already covers. Drives profile_executor_run() all the
// way to its real final commit (unlike every other test in this file, which
// the header comment on test_run_refuses_while_zone_sweep_is_active()
// explicitly notes cannot reach that far): zone 0 gets a real 1300C ceiling
// and PID control mode so profile_zones_have_ceiling()/the n_heating_zones
// guard both pass for real, and the single segment is a zero-ramp
// PROFILE_SEG_KIND_ZONE_RAMP dwell (the zero-initialized default) so the
// ramp-ceiling loop's "rate <= 0 -> skip" branch takes it out of play
// without needing zones_config_get_max_ramp() (stubbed to always report a
// 0 C/hr ceiling) to cooperate.
static void test_run_refuses_at_atomic_heat_claim_gate(void)
{
    TEST_SECTION("profile_executor_run() -- the LATE atomic heat-claim gate refuses even when the EARLY "
                 "zones_current_sweep_is_active() check passed (the race window the claim exists to close)");
    reset_relay_claim_test_state();
    s_exec.lock = xSemaphoreCreateMutex();
    s_exec.state = PROFILE_EXEC_IDLE;

    memset(&s_test_profiles_http_get_out, 0, sizeof(s_test_profiles_http_get_out));
    s_test_profiles_http_get_out.zone_mask = 0x01;
    s_test_profiles_http_get_out.segment_count = 1;
    s_test_profiles_http_get_out.segments[0].seg_kind = PROFILE_SEG_KIND_ZONE_RAMP;
    s_test_profiles_http_get_out.segments[0].ramp_c_per_hr = 0.0f; /* dwell -- skips the ramp-ceiling check */
    s_test_profiles_http_get_out.segments[0].target_c = 100.0f;
    s_test_profiles_http_get_ok = true;
    s_test_zones_config_valid = true;
    s_test_sweep_active = false; /* the EARLY check passes -- this is the race window itself */

    memset(g_stub_max_temp_c, 0, sizeof(g_stub_max_temp_c));
    memset(g_stub_control_mode, 0, sizeof(g_stub_control_mode));
    g_stub_max_temp_c[0] = 1300.0f;
    g_stub_control_mode[0] = ZONE_CONTROL_MODE_PID;

    // RED: force the atomic gate itself to refuse, simulating a sweep that
    // won the race and claimed exclusivity in the window between the early
    // check above and this call.
    s_test_heat_zone_claim_refused = true;
    char err[128];
    err[0] = '\0';
    bool ok = profile_executor_run(0, err, sizeof(err));

    TEST_CHECK(!ok, "the atomic gate alone must be able to refuse a run the early check let through");
    TEST_CHECK(strstr(err, "sweep") != NULL, "the refusal must still name the sweep specifically");
    TEST_CHECK(s_exec.state == PROFILE_EXEC_IDLE, "a run refused at the atomic gate must never reach RUNNING");
    TEST_CHECK(g_relay_claim_calls == 0, "relay_authority_claim_mask() (the actual relay ownership grab) "
                                        "must never be reached when the atomic gate refuses");

    // GREEN: same setup, atomic gate now allows it -- proves the RED result
    // above was really the gate, not some other stub failing closed.
    s_test_heat_zone_claim_refused = false;
    s_exec.state = PROFILE_EXEC_IDLE;
    err[0] = '\0';
    ok = profile_executor_run(0, err, sizeof(err));

    TEST_CHECK(ok, "with the atomic gate allowing it, the identical setup must succeed");
    TEST_CHECK(s_exec.state == PROFILE_EXEC_RUNNING, "a successful run must reach RUNNING");
    TEST_CHECK(g_heat_zone_claim_begin_calls == 2, "the gate is attempted exactly once per profile_executor_run() call");
    TEST_CHECK(g_last_heat_zone_claimant == RELAY_HEAT_ZONE_CLAIM_PROFILE,
              "profile_executor_run() must claim as PROFILE, not AUTOTUNE");

    profile_executor_halt();
    TEST_CHECK(g_heat_zone_claim_end_calls >= 1, "halt() must release the heat claim it just took");

    s_test_profiles_http_get_ok = false;
    s_test_zones_config_valid = false;
    s_test_heat_zone_claim_refused = false;
}


// ---------------------------------------------------------------------------
// Heat-enable (K4) wiring -- the 2026-08-29 fix.
//
// profile_executor.c never asked the safety processor to permit heating at
// all: it closed its own zone relay and left K4 open, so no element current
// ever flowed on a normal firing. These tests pin the RELEASE half of the
// fix at the three exit paths this file can actually drive (a guard trip, an
// operator halt, a pause), because a release that a fault path bypasses is
// how heat gets left permitted with no run to permit it for.
//
// COVERAGE GAP, stated honestly: the ACQUIRE half lives in
// profile_executor_run(), past a full task/config/thermocouple harness this
// file deliberately never builds (see its header comment), so it is not
// exercised here. It is covered instead by test_heat_enable.c's
// exactly-once/refcount tests over the module itself, by
// check_heat_enable_wiring.ps1's source-level assertion that the call is
// present at the commit point, and by the live bench run. Same disclosure
// style as the rest of this file's "reachable without a real task loop"
// notes.
// ---------------------------------------------------------------------------

static void test_guard_trip_releases_heat_enable(void)
{
    TEST_SECTION("escalate_guard_trip() GLOBAL trip -- also gives K4 back (heat_enable release)");
    reset_relay_claim_test_state();
    arm_heat_enable_as_if_running();
    s_exec.zones[0].active = true;
    s_exec.claimed_relay_mask = 0x03;

    (void)escalate_guard_trip(0, THERMAL_GUARD_TRIP_MAX_TEMP, "over-temp");

    TEST_CHECK(g_request_enable_false_calls == 1,
               "a guard trip must send exactly one REQUEST_ENABLE(false) -- a fault path that skips "
               "this leaves the safety processor permitting heat for a run that no longer exists");
    TEST_CHECK(g_request_enable_true_calls == 0, "and must not ask for heat on the way out");
    TEST_CHECK(!heat_enable_is_granted(), "no heat request may be left standing after a trip");
}

static void test_halt_releases_heat_enable(void)
{
    TEST_SECTION("profile_executor_halt() -- gives K4 back (heat_enable release)");
    reset_relay_claim_test_state();
    arm_heat_enable_as_if_running();
    s_exec.lock = xSemaphoreCreateMutex();
    s_exec.state = PROFILE_EXEC_RUNNING;
    s_exec.claimed_relay_mask = 0x0F;

    profile_executor_halt();

    TEST_CHECK(g_request_enable_false_calls == 1, "an operator halt must release the heat-enable request");
    TEST_CHECK(!heat_enable_is_granted(), "nothing left standing");

    /* halt() is also how a DONE/FAULTED run is dismissed, and dismissing one
     * twice must not put a second frame on the wire. */
    profile_executor_halt();
    TEST_CHECK(g_request_enable_false_calls == 1, "a second halt sends nothing more");
}

static void test_pause_releases_heat_enable_and_resume_reacquires(void)
{
    TEST_SECTION("profile_executor_pause()/resume() -- pause gives K4 back, resume asks again");
    reset_relay_claim_test_state();
    arm_heat_enable_as_if_running();
    s_exec.lock = xSemaphoreCreateMutex();
    s_exec.state = PROFILE_EXEC_RUNNING;
    s_exec.claimed_relay_mask = 0x05;

    TEST_CHECK(profile_executor_pause(), "sanity: pause() succeeds from RUNNING");
    TEST_CHECK(g_request_enable_false_calls == 1,
               "pause must release K4 -- unlike the relay claim, which pause deliberately KEEPS "
               "(handed to MANUAL), leaving the safety processor permitting heat across a pause of "
               "unknown length holds open the one interlock between a stuck relay and a live element");
    TEST_CHECK(!heat_enable_is_granted(), "not granted while paused");

    TEST_CHECK(profile_executor_resume(), "sanity: resume() succeeds from PAUSED");
    TEST_CHECK(g_request_enable_true_calls == 1, "resume must ask for K4 again, exactly once");
    TEST_CHECK(heat_enable_is_granted(), "granted again after resume");
}

static void test_heat_enable_release_survives_a_down_link(void)
{
    TEST_SECTION("profile_executor_halt() -- releases K4 even when the safety link is down");
    reset_relay_claim_test_state();
    arm_heat_enable_as_if_running();
    /* The link drops mid-firing. enable=false is the fail-safe direction and
     * safety_link.c attempts it regardless -- refusing it because the link
     * looks down is the one refusal that could leave heat permitted. */
    g_request_enable_link_up = false;
    s_exec.lock = xSemaphoreCreateMutex();
    s_exec.state = PROFILE_EXEC_RUNNING;
    s_exec.claimed_relay_mask = 0x01;

    profile_executor_halt();

    TEST_CHECK(g_request_enable_false_calls == 1, "the release is still attempted on a down link");
    TEST_CHECK(!heat_enable_is_granted(), "and the request is not left standing");
}

// ---------------------------------------------------------------------------
// Warm-start (PROFILES.md "Warm-start: joining a profile already at
// temperature", owner request 2026-08-30) -- these are the only tests in
// this file that drive profile_executor_run() all the way to its real final
// commit for a profile with real segments (see
// test_run_refuses_at_atomic_heat_claim_gate()'s own comment on why that is
// otherwise rare in this file): zone 0 gets a real 1300C ceiling, PID
// control mode, a real thermo_mask, and a real (non-zero) ramp ceiling so a
// >0 ramp_c_per_hr segment is actually feasible, plus a canned MAX31856
// reading and an "initialized" thermo_bus so profile_executor_run()'s own
// start-of-run read (the one profile_executor_plan_warm_start() is fed from)
// returns real numbers instead of always failing.
// ---------------------------------------------------------------------------

/* io_seg_start()/io_seg_finish() only write to hardware `if (s_exec.io)` --
 * a non-NULL, otherwise-untouched kiln_io_t is enough for test 3 (replay)
 * below to observe the relay writes through g_relay_write_calls/
 * g_last_relay_write_value (kiln_io_owner_command_set_relay_mask_authorized()
 * is faked above and never actually dereferences its kiln_io_t* argument, so
 * this can stay zeroed). */
static kiln_io_t s_test_kiln_io;

static void warm_start_test_setup(const profile_t *p, float zone0_reading_c)
{
    reset_relay_claim_test_state();
    reset_test_thermo_readings();
    s_exec.lock = xSemaphoreCreateMutex();
    s_exec.state = PROFILE_EXEC_IDLE;
    arm_test_thermo_bus();
    memset(&s_test_kiln_io, 0, sizeof(s_test_kiln_io));
    s_exec.io = &s_test_kiln_io;

    s_test_profiles_http_get_out = *p;
    s_test_profiles_http_get_ok = true;
    s_test_zones_config_valid = true;
    s_test_sweep_active = false;
    s_test_heat_zone_claim_refused = false;

    memset(g_stub_max_temp_c, 0, sizeof(g_stub_max_temp_c));
    memset(g_stub_control_mode, 0, sizeof(g_stub_control_mode));
    memset(g_stub_thermo_mask, 0, sizeof(g_stub_thermo_mask));
    memset(g_stub_max_ramp_c_per_hr, 0, sizeof(g_stub_max_ramp_c_per_hr));
    g_stub_max_temp_c[0] = 1300.0f;
    g_stub_control_mode[0] = ZONE_CONTROL_MODE_PID;
    g_stub_thermo_mask[0] = 0x01; /* zone 0 reads channel 0 */
    g_stub_max_ramp_c_per_hr[0] = 500.0f; /* comfortably above every rate these tests use */

    set_test_thermo_reading(0, zone0_reading_c);
}

static profile_segment_t zone_ramp_seg(float target_c, float ramp_c_per_hr, uint32_t dwell_min)
{
    profile_segment_t s;
    memset(&s, 0, sizeof(s));
    s.seg_kind = PROFILE_SEG_KIND_ZONE_RAMP;
    s.target_c = target_c;
    s.ramp_c_per_hr = ramp_c_per_hr;
    s.dwell_min = dwell_min;
    return s;
}

static profile_segment_t relay_io_seg(uint8_t io_target, uint8_t io_state, uint8_t blocking, uint32_t dwell_min)
{
    profile_segment_t s;
    memset(&s, 0, sizeof(s));
    s.seg_kind = PROFILE_SEG_KIND_RELAY_IO;
    s.io_target = io_target;
    s.io_state = io_state;
    s.io_blocking = blocking;
    s.dwell_min = dwell_min;
    return s;
}

// Test 1 (mandatory coverage item 1): cold kiln -- no warm start, starts at
// segment 0, byte-identical to the pre-feature code. The kiln reads 50C,
// well below segment 0's 200C target -- run() must land exactly where it
// always did: segment 0, not dwelling, target_c seeded from the actual
// reading (50C, not the segment's target), zero elapsed, and NOT flagged as
// warm-started.
static void test_warm_start_cold_kiln_is_a_regression_noop(void)
{
    TEST_SECTION("warm-start -- a cold kiln is untouched: segment 0, offset 0, not warm-started (regression guard)");

    profile_t p;
    memset(&p, 0, sizeof(p));
    p.zone_mask = 0x01;
    p.segment_count = 2;
    p.segments[0] = zone_ramp_seg(200.0f, 100.0f, 0);
    p.segments[1] = zone_ramp_seg(600.0f, 100.0f, 10);

    warm_start_test_setup(&p, 50.0f);
    char err[128] = {0};
    bool ok = profile_executor_run(0, err, sizeof(err));

    TEST_CHECK(ok, "a well-formed run against a cold kiln must succeed");
    TEST_CHECK(!s_exec.warm_started, "a cold kiln must never be reported as warm-started");
    TEST_CHECK(s_exec.segment_index == 0, "must start at segment 0");
    TEST_CHECK(!s_exec.dwelling, "segment 0 is a ramp, not yet a dwell");
    TEST_CHECK(s_exec.segment_elapsed_s == 0, "no time has been fast-forwarded into segment 0");
    TEST_CHECK(fabsf(s_exec.target_c - 50.0f) < 0.01f,
              "target_c must seed from the actual reading (50C), exactly the pre-feature baseline_target_c "
              "behavior -- never the segment's own target");
    TEST_CHECK(s_exec.warm_start_replayed_count == 0, "nothing was skipped, so nothing was replayed");

    profile_executor_halt();
}

// Test 2 (mandatory coverage item 2): warm kiln mid-ramp -- segment 1 ramps
// 200->600C at 100C/hr and the kiln already reads 300C. run() must skip
// segment 0 entirely, enter segment 1, and command a setpoint AT current
// temperature (300C), never below it -- 300C is the exact bug this feature
// removes if it were commanded 200C instead. The entry offset (Q2) is
// reported too: 1 hour of segment 1's ramp is already "spent" reaching 300C
// from its own 200C start, at 100C/hr.
static void test_warm_start_mid_ramp_entry_never_below_current(void)
{
    TEST_SECTION("warm-start -- warm kiln mid-ramp: correct segment, correct offset, setpoint >= current (Q2)");

    profile_t p;
    memset(&p, 0, sizeof(p));
    p.zone_mask = 0x01;
    p.segment_count = 2;
    p.segments[0] = zone_ramp_seg(200.0f, 100.0f, 0);
    p.segments[1] = zone_ramp_seg(600.0f, 100.0f, 10);

    warm_start_test_setup(&p, 300.0f);
    char err[128] = {0};
    bool ok = profile_executor_run(0, err, sizeof(err));

    TEST_CHECK(ok, "a well-formed warm run must succeed");
    TEST_CHECK(s_exec.warm_started, "skipping segment 0 must be reported as a warm start");
    TEST_CHECK(s_exec.segment_index == 1, "must enter segment 1 (the one whose ramp reaches 300C), not segment 0");
    TEST_CHECK(!s_exec.dwelling, "still mid-ramp, not yet at segment 1's own 600C target");
    TEST_CHECK(s_exec.target_c >= 300.0f - 0.01f,
              "the commanded setpoint must never be below current temperature -- the exact bug this "
              "feature exists to remove");
    TEST_CHECK(fabsf(s_exec.target_c - 300.0f) < 0.01f, "entry target_c must be exactly current temperature, "
                                                         "not segment 1's own 200C start");
    TEST_CHECK(s_exec.segment_elapsed_s == 3600,
              "Q2's entry offset: (300-200)/100C/hr = 1h already spent climbing segment 1's ramp, carried "
              "as segment_elapsed_s");
    TEST_CHECK(strstr(s_exec.warm_start_reason, "segment 2") != NULL,
              "Q6: the reason string must name the (1-based) entry segment for the operator");

    profile_executor_halt();
}

// Test 3 (mandatory coverage item 3): a skipped RELAY_IO segment's command
// is replayed, and the replayed segment is still registered for the
// end-of-run sweep so it cannot be left energized with nothing owning it
// (the owner's Q1 decision, verbatim in PROFILES.md). Segment 0 opens relay
// 1; segments 1-2 are the same ramp as test 2, entered warm at segment 2.
static void test_warm_start_replays_skipped_relay_io_and_registers_it(void)
{
    TEST_SECTION("warm-start -- skipped RELAY_IO segments are replayed, in order, and swept off at run end (Q1)");

    profile_t p;
    memset(&p, 0, sizeof(p));
    p.zone_mask = 0x01;
    p.segment_count = 3;
    p.segments[0] = relay_io_seg(PROFILE_IO_TARGET_RELAY_BASE, 1 /* ON */, 1 /* blocking */, 5);
    p.segments[1] = zone_ramp_seg(200.0f, 100.0f, 0);
    p.segments[2] = zone_ramp_seg(600.0f, 100.0f, 10);

    warm_start_test_setup(&p, 300.0f);
    g_relay_write_calls = 0;
    char err[128] = {0};
    bool ok = profile_executor_run(0, err, sizeof(err));

    TEST_CHECK(ok, "a well-formed run must succeed");
    TEST_CHECK(s_exec.warm_started, "must be reported as warm-started");
    TEST_CHECK(s_exec.segment_index == 2, "must enter segment 2 (mirrors test 2, just with an IO segment ahead of it)");
    TEST_CHECK(s_exec.warm_start_replayed_count == 1, "exactly one RELAY_IO segment was skipped");
    TEST_CHECK(s_exec.warm_start_replayed_segments[0] == 0, "it must name segment 0 (0-based)");
    TEST_CHECK(g_relay_write_calls >= 1, "the skipped segment's ON command must actually have been written to hardware");
    TEST_CHECK(g_last_relay_write_value == 0x01, "relay 1's bit must have been commanded ON (replayed, not skipped)");
    TEST_CHECK(s_exec.io_segs[0].active, "the replayed segment must be registered active -- io_seg_finish()'s "
                                        "end-of-run sweep is the only thing that may still turn it off");
    TEST_CHECK(s_exec.io_segs[0].blocking,
              "the replayed segment's own hold/dwell_min must NOT be restarted (Q1) -- forcing it to look "
              "blocking to io_segs_tick() is what keeps that timer from ever touching it again this run");

    // The end-of-run sweep must still retire it -- prove a relay this warm
    // start energized is not left owned by nothing once the run ends.
    g_relay_write_calls = 0;
    g_last_relay_write_value = 0xFF; /* poison -- must be overwritten by a real off-write below */
    profile_executor_halt();
    TEST_CHECK(g_relay_write_calls >= 1, "halt()'s end-of-run sweep must still touch this relay");
    TEST_CHECK((g_last_relay_write_value & 0x01) == 0, "and must force it OFF -- nothing may hold it energized "
                                                        "with the run gone");
}

// Test 4 (mandatory coverage item 4): a dwell segment matched by warm-start
// is not shortened just because the kiln has already reached (or passed) its
// target -- Q3, "a soak is time at temperature, not time spent arriving
// there". Segment 1 is a real 20-minute soak at 280C; the kiln is already
// past it (300C) but a descending segment 2 follows, so this also proves the
// soak segment is the one landed on, not the ascent scanning straight past
// it into the cooldown leg.
static void test_warm_start_reached_dwell_is_not_shortened(void)
{
    TEST_SECTION("warm-start -- an already-reached dwell still runs its FULL configured soak (Q3)");

    profile_t p;
    memset(&p, 0, sizeof(p));
    p.zone_mask = 0x01;
    p.segment_count = 3;
    p.segments[0] = zone_ramp_seg(200.0f, 100.0f, 5);
    p.segments[1] = zone_ramp_seg(280.0f, 100.0f, 20); /* the soak -- 20 real minutes */
    p.segments[2] = zone_ramp_seg(100.0f, 100.0f, 0);  /* descending -- ends the leading ascent (Q5) */

    warm_start_test_setup(&p, 300.0f); /* past the whole ascending leg (200, then 280) */
    char err[128] = {0};
    bool ok = profile_executor_run(0, err, sizeof(err));

    TEST_CHECK(ok, "a well-formed run must succeed");
    TEST_CHECK(s_exec.warm_started, "must be reported as warm-started");
    TEST_CHECK(s_exec.segment_index == 1, "must land on the soak segment (index 1), not skip past it into the "
                                          "descending segment 2");
    TEST_CHECK(s_exec.dwelling, "already past 280C -- entered directly as a dwell, not a ramp");
    TEST_CHECK(s_exec.segment_elapsed_s == 0,
              "the full 20-minute soak must still be ahead of it -- 0 elapsed, NOT pre-credited/shortened "
              "just because the kiln already reads past the target");

    profile_executor_halt();
}

// Test 5 (mandatory coverage item 5): a descending (cool-down/anneal)
// profile started hot must NOT jump into its cooling leg -- Q5, "scan only
// the leading ascent... stop at the first descent". Segment 0 peaks at
// 800C; segment 1 cools to 200C. The kiln is even hotter than the peak
// (850C), so this also exercises the "hotter than the whole leading ascent"
// landing (still segment 0, never segment 1).
static void test_warm_start_descending_profile_does_not_jump_into_cooldown(void)
{
    TEST_SECTION("warm-start -- a hot kiln on a descending profile stays out of the cooling leg (Q5)");

    profile_t p;
    memset(&p, 0, sizeof(p));
    p.zone_mask = 0x01;
    p.segment_count = 2;
    p.segments[0] = zone_ramp_seg(800.0f, 100.0f, 20);
    p.segments[1] = zone_ramp_seg(200.0f, 100.0f, 0); /* the cooling leg -- must never be entered here */

    warm_start_test_setup(&p, 850.0f); /* hotter than the whole leading ascent (just segment 0) */
    char err[128] = {0};
    bool ok = profile_executor_run(0, err, sizeof(err));

    TEST_CHECK(ok, "a well-formed run must succeed");
    TEST_CHECK(s_exec.segment_index == 0, "must never land on segment 1 (the cooling leg) -- the leading "
                                          "ascent is only segment 0");
    TEST_CHECK(s_exec.segment_index != 1, "explicitly: not the descending segment");

    profile_executor_halt();
}

// Test 6 (mandatory coverage item 6): the kiln is hotter than every segment
// in the whole profile -- the "hotter than everything" decision (see
// profile_executor_plan_warm_start()'s doc comment for the full reasoning):
// land on the LAST segment of the profile's leading ascent, entered as a
// dwell, rather than refusing to start or fabricating a jump past the end.
// A purely-ascending 2-segment profile, kiln hotter than both.
static void test_warm_start_hotter_than_entire_profile_lands_on_last_segment(void)
{
    TEST_SECTION("warm-start -- kiln hotter than the whole profile: lands on the last segment, does not refuse "
                 "to start (hotter-than-everything decision)");

    profile_t p;
    memset(&p, 0, sizeof(p));
    p.zone_mask = 0x01;
    p.segment_count = 2;
    p.segments[0] = zone_ramp_seg(200.0f, 100.0f, 5);
    p.segments[1] = zone_ramp_seg(280.0f, 100.0f, 15);

    warm_start_test_setup(&p, 900.0f); /* hotter than every segment in the profile */
    char err[128] = {0};
    bool ok = profile_executor_run(0, err, sizeof(err));

    TEST_CHECK(ok, "hotter-than-everything must NOT refuse to start");
    TEST_CHECK(s_exec.state == PROFILE_EXEC_RUNNING, "and must actually reach RUNNING");
    TEST_CHECK(s_exec.warm_started, "must be reported as warm-started");
    TEST_CHECK(s_exec.segment_index == 1, "lands on the LAST segment of the profile (index 1), not the first");
    TEST_CHECK(s_exec.dwelling, "entered directly as a dwell -- see Q3, the soak still runs");
    TEST_CHECK(s_exec.segment_elapsed_s == 0, "the full soak is still ahead of it, not shortened");

    profile_executor_halt();
}

// ---------------------------------------------------------------------------
// pid_fuzzy_prepare_gains() regression guard (opus review, 2026-08-30):
// test_closed_loop.c's "mandatory test 1" turned out to be a determinism
// check masquerading against a MIRROR of pid_fuzzy_prepare_gains()'s logic
// (fuzzy_tick(), local to that file) rather than a call to the real,
// production function -- so a future edit to pid_fuzzy_prepare_gains() or
// pid_fuzzy_adjust() that changed behavior would go completely undetected
// there. pid_fuzzy_prepare_gains() is `static` in profile_executor.c, which
// this file #includes wholesale (see the file header comment), so it is
// reachable directly here exactly the same way escalate_guard_trip() and
// guard9_assert_stale_tick_fault() are above -- no production code was
// changed to make this possible.
// ---------------------------------------------------------------------------

static void reset_fuzzy_gain_test_state(void)
{
    memset(&s_exec, 0, sizeof(s_exec));
    s_test_fuzzy_strength_present = false;
    s_test_fuzzy_strength_pct = 0.0f;
}

static void test_fuzzy_prepare_gains_zero_strength_is_base_gains_bit_exact(void)
{
    TEST_SECTION("pid_fuzzy_prepare_gains() -- strength_pct == 0 reproduces base gains bit-exactly "
                 "(the safety contract pid_fuzzy.h documents: at 0 strength, PID_FUZZY must be "
                 "indistinguishable from classic PID)");
    reset_fuzzy_gain_test_state();
    s_test_fuzzy_strength_present = true;
    s_test_fuzzy_strength_pct = 0.0f;

    zone_runtime_t z;
    memset(&z, 0, sizeof(z));
    z.pid_cfg.kp = 1.25f;
    z.pid_cfg.ki = 0.03f;
    z.pid_cfg.kd = 4.5f;
    z.pid_state.d_filtered = -0.05f; /* an ordinary climb, per pid.c's negated-climb convention */
    z.actual_c = 700.0f;
    s_exec.target_c = 1000.0f; /* a large POS error -- would move gains hard at nonzero strength */

    pid_cfg_t out;
    memset(&out, 0xAA, sizeof(out));
    pid_fuzzy_prepare_gains(&z, 0, &out);

    TEST_CHECK(out.kp == 1.25f, "kp must be exactly base_kp at strength 0");
    TEST_CHECK(out.ki == 0.03f, "ki must be exactly base_ki at strength 0");
    TEST_CHECK(out.kd == 4.5f, "kd must be exactly base_kd at strength 0");
}

static void test_fuzzy_prepare_gains_matches_pid_fuzzy_adjust_directly(void)
{
    TEST_SECTION("pid_fuzzy_prepare_gains() -- for a known error/rate/strength, produces exactly what "
                 "calling the production pid_fuzzy_adjust() directly with the same inputs would -- proves "
                 "the wiring (error = target - actual_c, rate = pid_state.d_filtered, strength from "
                 "zones_config_get_fuzzy_strength_pct()) is intact, not just that SOME gains come out");
    reset_fuzzy_gain_test_state();
    s_test_fuzzy_strength_present = true;
    s_test_fuzzy_strength_pct = 100.0f;

    zone_runtime_t z;
    memset(&z, 0, sizeof(z));
    z.pid_cfg.kp = 1.0f;
    z.pid_cfg.ki = 0.02f;
    z.pid_cfg.kd = 2.0f;
    z.pid_state.d_filtered = 0.0f; /* POS/large error, STEADY rate -> rule table: Kp+, Ki=, Kd= */
    z.actual_c = 700.0f;
    s_exec.target_c = 1000.0f; /* 300C error -- "large" POS bucket */

    pid_cfg_t out;
    memset(&out, 0, sizeof(out));
    pid_fuzzy_prepare_gains(&z, 0, &out);

    float expect_kp, expect_ki, expect_kd;
    pid_fuzzy_adjust(300.0f, 0.0f, 1.0f, 0.02f, 2.0f, 100, &expect_kp, &expect_ki, &expect_kd);

    TEST_CHECK(out.kp == expect_kp, "kp must equal a direct pid_fuzzy_adjust() call with the same inputs");
    TEST_CHECK(out.ki == expect_ki, "ki must equal a direct pid_fuzzy_adjust() call with the same inputs");
    TEST_CHECK(out.kd == expect_kd, "kd must equal a direct pid_fuzzy_adjust() call with the same inputs");
    /* Pin the actual rule-table direction too (POS/large x STEADY -> Kp+, Ki=, Kd=), so a change to the
     * WIRING that happened to still equal pid_fuzzy_adjust()'s output on some OTHER cell can't hide behind
     * the comparison above alone. */
    TEST_CHECK(out.kp > 1.0f, "POS/large error at STEADY rate must nudge Kp UP, per the rule table");
    TEST_CHECK(out.ki == 0.02f, "POS/large error at STEADY rate must leave Ki UNCHANGED, per the rule table");
    TEST_CHECK(out.kd == 2.0f, "POS/large error at STEADY rate must leave Kd UNCHANGED, per the rule table");
}

static void test_fuzzy_prepare_gains_nan_strength_falls_back_to_base_not_large(void)
{
    TEST_SECTION("pid_fuzzy_prepare_gains() -- a NaN strength_pct falls back to base gains (0, the most "
                 "conservative value), not the large-strength end of the range");
    reset_fuzzy_gain_test_state();
    s_test_fuzzy_strength_present = true;
    s_test_fuzzy_strength_pct = NAN;

    zone_runtime_t z;
    memset(&z, 0, sizeof(z));
    z.pid_cfg.kp = 1.0f;
    z.pid_cfg.ki = 0.02f;
    z.pid_cfg.kd = 2.0f;
    z.pid_state.d_filtered = 0.0f;
    z.actual_c = 700.0f;
    s_exec.target_c = 1000.0f; /* same large POS/STEADY case that moves Kp hard at strength 100 above */

    pid_cfg_t out;
    memset(&out, 0, sizeof(out));
    pid_fuzzy_prepare_gains(&z, 0, &out);

    TEST_CHECK(out.kp == 1.0f, "a NaN configured strength must NOT be treated as a large strength -- kp "
                              "must stay at base_kp");
    TEST_CHECK(out.ki == 0.02f, "ki must stay at base_ki");
    TEST_CHECK(out.kd == 2.0f, "kd must stay at base_kd");
}

// ---------------------------------------------------------------------------
// PID_EXPANSION_PLAN.md section 2c / Phase 3b -- cross-zone coupling
// feedforward. zone_feedforward() is static, reached directly the same way
// the fuzzy-gains tests above reach pid_fuzzy_prepare_gains() -- this file
// #includes profile_executor.c itself.

static void reset_coupling_test_state(void)
{
    memset(&s_exec, 0, sizeof(s_exec));
    memset(g_stub_coupling, 0, sizeof(g_stub_coupling));
    g_stub_coupling_present[0] = true;
    g_stub_coupling_present[1] = true;
    g_stub_coupling_present[2] = true;
}

static void test_feedforward_zero_coupling_is_bit_identical_to_no_coupling(void)
{
    TEST_SECTION("zone_feedforward() -- an all-zero coupling row (the migration default, an "
                 "un-commissioned kiln) reproduces hold+climb bit-for-bit -- the safety property "
                 "PID_EXPANSION_PLAN.md Phase 3b requires before this can ship anywhere near hardware");
    reset_coupling_test_state();

    zone_runtime_t z;
    memset(&z, 0, sizeof(z));
    z.ff_enabled = true;
    z.ff_k_dc = 100.0f;
    z.ff_tau_s = 600.0f;
    s_exec.ambient_c = 20.0f;

    s_exec.zones[0].active = true;
    s_exec.zones[0].actual_valid = true;
    s_exec.zones[0].actual_c = 150.0f;
    s_exec.zones[0].ff_enabled = true;
    s_exec.zones[0].ff_k_dc = 18.0f;
    s_exec.zones[0].control_mode = ZONE_CONTROL_MODE_PID;

    float setpoint_c = 100.0f, rate = 0.0f;
    float with_zero_row = zone_feedforward(&z, 1, setpoint_c, rate);

    float expect = (setpoint_c - s_exec.ambient_c) / z.ff_k_dc + (rate * z.ff_tau_s) / z.ff_k_dc;
    TEST_CHECK(expect > 0.0f && expect < 1.0f, "test setup sanity: baseline must sit inside the clamp "
                                               "so the clamp cannot hide a coupling bug either way");

    TEST_CHECK(with_zero_row == expect, "all-zero coupling row must not move u_ff by even one ULP");

    g_stub_coupling[1][0] = 10.887f;
    float with_nonzero_row = zone_feedforward(&z, 1, setpoint_c, rate);
    TEST_CHECK(with_nonzero_row != expect, "sanity: a nonzero coupling coefficient DOES move u_ff -- "
                                           "proves the equality check above is not vacuously true");
}

static void test_feedforward_hot_neighbor_subtracts_duty(void)
{
    TEST_SECTION("zone_feedforward() -- a neighbor running HOT (above its own setpoint) subtracts duty "
                 "from this zone (PID_EXPANSION_PLAN.md 2c's sign contract)");
    reset_coupling_test_state();

    zone_runtime_t z;
    memset(&z, 0, sizeof(z));
    z.ff_enabled = true;
    z.ff_k_dc = 100.0f;
    z.ff_tau_s = 600.0f;
    s_exec.ambient_c = 20.0f;

    s_exec.zones[0].active = true;
    s_exec.zones[0].actual_valid = true;
    s_exec.zones[0].ff_enabled = true;
    s_exec.zones[0].ff_k_dc = 18.0f;
    s_exec.zones[0].control_mode = ZONE_CONTROL_MODE_PID;

    g_stub_coupling[1][0] = 10.887f;

    float setpoint_c = 100.0f, rate = 0.0f;
    float baseline = (setpoint_c - s_exec.ambient_c) / z.ff_k_dc;

    s_exec.zones[0].actual_c = setpoint_c;
    float on_target = zone_feedforward(&z, 1, setpoint_c, rate);
    TEST_CHECK(on_target == baseline, "a neighbor exactly on its setpoint must change nothing");

    s_exec.zones[0].actual_c = setpoint_c + 20.0f;
    float hot = zone_feedforward(&z, 1, setpoint_c, rate);
    TEST_CHECK(hot < on_target, "a neighbor running hot must SUBTRACT duty from this zone");

    s_exec.zones[0].actual_c = setpoint_c - 20.0f;
    float cold = zone_feedforward(&z, 1, setpoint_c, rate);
    TEST_CHECK(cold > on_target, "a neighbor running cold must ADD duty to this zone");

    TEST_CHECK(hot > 0.0f && cold < 1.0f, "test setup sanity: neither +-20C case may hit the clamp");
    TEST_CHECK(fabsf((on_target - hot) - (cold - on_target)) < 1e-3f,
               "the +-20C cases must move duty by the same magnitude in opposite directions");
}

static void test_feedforward_invalid_neighbor_contributes_zero_never_nan(void)
{
    TEST_SECTION("zone_feedforward() -- a faulted/unread neighbor contributes exactly 0, never NaN, "
                 "even with a nonzero measured coefficient for it");
    reset_coupling_test_state();

    zone_runtime_t z;
    memset(&z, 0, sizeof(z));
    z.ff_enabled = true;
    z.ff_k_dc = 100.0f;
    z.ff_tau_s = 600.0f;
    s_exec.ambient_c = 20.0f;
    float setpoint_c = 100.0f, rate = 0.0f;
    float expect = (setpoint_c - s_exec.ambient_c) / z.ff_k_dc;

    g_stub_coupling[1][0] = 10.887f;
    s_exec.zones[0].active = false;
    s_exec.zones[0].actual_valid = true;
    s_exec.zones[0].actual_c = 1e9f;
    s_exec.zones[0].ff_enabled = true;
    s_exec.zones[0].ff_k_dc = 18.0f;
    s_exec.zones[0].control_mode = ZONE_CONTROL_MODE_PID;
    float r_inactive = zone_feedforward(&z, 1, setpoint_c, rate);
    TEST_CHECK(r_inactive == expect, "an inactive neighbor must contribute 0 despite a nonzero coefficient");
    TEST_CHECK(isfinite(r_inactive), "an inactive neighbor's absurd reading must not leak into a non-finite u_ff");

    s_exec.zones[0].active = true;
    s_exec.zones[0].actual_valid = false;
    float r_invalid = zone_feedforward(&z, 1, setpoint_c, rate);
    TEST_CHECK(r_invalid == expect, "actual_valid==false must contribute 0");

    s_exec.zones[0].actual_valid = true;
    s_exec.zones[0].actual_c = NAN;
    float r_nan = zone_feedforward(&z, 1, setpoint_c, rate);
    TEST_CHECK(isfinite(r_nan), "a NaN neighbor reading must never produce a non-finite u_ff");
    TEST_CHECK(r_nan == expect, "a NaN neighbor reading must contribute exactly 0, not just \"some finite value\"");

    s_exec.zones[0].actual_c = setpoint_c + 20.0f;
    s_exec.zones[0].ff_enabled = false;
    s_exec.zones[0].ff_k_dc = 0.0f;
    float r_no_model = zone_feedforward(&z, 1, setpoint_c, rate);
    TEST_CHECK(r_no_model == expect, "a neighbor with no identified model must contribute 0, not divide by its own zero k_dc");
    TEST_CHECK(isfinite(r_no_model), "must not produce inf/NaN from a zero neighbor k_dc");
}

static void test_feedforward_both_callers_agree_bump_transfer(void)
{
    TEST_SECTION("zone_feedforward() -- the bumpless-seed call site (seed_bumpless_with_ff(), used on "
                 "gain/model reload and resume) and the per-tick call site (pid_family_zone_tick()) must "
                 "compute the SAME feedforward for the same state, coupling term included, or bump "
                 "transfer breaks (PID_EXPANSION_PLAN.md Phase 3b's own requirement)");
    reset_coupling_test_state();

    const uint8_t zi = 1;
    zone_runtime_t z;
    memset(&z, 0, sizeof(z));
    z.ff_enabled = true;
    z.ff_k_dc = 100.0f;
    z.ff_tau_s = 600.0f;
    z.pid_cfg = (pid_cfg_t){.kp = 0.0f, .ki = 0.02f, .kd = 0.0f, .d_filter_tau_s = 1.0f, .b = 1.0f,
                            .pid_range_c = 1000.0f};
    pid_reset(&z.pid_state);
    z.actual_c = 100.0f;
    s_exec.ambient_c = 20.0f;
    s_exec.target_c = 100.0f;
    s_exec.target_rate_c_per_s = 0.0f;

    s_exec.zones[0].active = true;
    s_exec.zones[0].actual_valid = true;
    s_exec.zones[0].actual_c = 120.0f;
    s_exec.zones[0].ff_enabled = true;
    s_exec.zones[0].ff_k_dc = 18.0f;
    s_exec.zones[0].control_mode = ZONE_CONTROL_MODE_PID;
    g_stub_coupling[zi][0] = 10.887f;
    g_stub_coupling[zi][2] = 3.332f;

    /* Reference/oracle value only -- NOT fed back into the production code,
     * just used below to check what the real per-tick call site computed. */
    float u_ff_reference = zone_feedforward(&z, zi, s_exec.target_c, s_exec.target_rate_c_per_s);
    TEST_CHECK(u_ff_reference > 0.0f && u_ff_reference < 1.0f, "test setup sanity: u_ff must sit inside the clamp");

    /* Must exceed u_ff_reference: seed_bumpless_with_ff()'s own doc comment
     * notes the floor-at-0 integral makes the zone come back at exactly
     * u_ff when u_desired <= u_ff -- a real effect, not a bug, but it would
     * make this test unable to tell "both callers agree" from "the floor
     * kicked in" regardless of whether they agree. */
    float u_desired = 0.9f;
    TEST_CHECK(u_desired > u_ff_reference, "test setup sanity: u_desired must exceed u_ff so the anti-windup "
                                           "floor does not mask a caller disagreement");

    /* Call site 1 (real production code, not a stand-in): reload_zone_config()/
     * resume()'s bumpless-seed path. */
    seed_bumpless_with_ff(&z, zi, u_desired);
    TEST_CHECK(z.pid_state.integral >= 0.0f, "seeded integral must respect the anti-windup floor");

    /* Call site 2 (also real production code): pid_family_zone_tick(), the
     * actual per-tick control-loop body -- not a second direct
     * zone_feedforward() call, which would only prove this TEST computes a
     * consistent number, not that the two REAL call sites agree. A minimal
     * but valid heater_cfg keeps heater_output_duty() (called at the tail of
     * pid_family_zone_tick(), unrelated to feedforward) out of its own
     * degenerate-config corner cases. */
    z.heater_cfg.window_ms = 10000;
    z.heater_cfg.min_on_ms = 0;
    bool want_relay_on = false;
    float duty = pid_family_zone_tick(&z, zi, &z.pid_cfg, /*sensor_ok_zi=*/true, /*dt_s=*/1.0f,
                                      /*dt_ms=*/1000u, &want_relay_on);

    /* The two call sites must have used the identical feedforward number --
     * proven two ways: (1) pid_family_zone_tick()'s own reported ff term
     * (last_pid_terms.ff, what /api/control shows) equals the oracle, so its
     * internal zone_feedforward(z, zi, s_exec.target_c, s_exec.target_rate_c_per_s)
     * call used the same setpoint/rate/coupling row as the reference; and
     * (2) the resulting duty reproduces u_desired -- the actual bump-transfer
     * property this whole mechanism exists for. */
    TEST_CHECK(z.last_pid_terms.ff == u_ff_reference, "pid_family_zone_tick()'s own u_ff (the per-tick call "
              "site) must equal the reference value -- if the two call sites diverge on setpoint, rate, or "
              "coupling row, this catches it directly");
    TEST_CHECK_NEAR(duty, u_desired, 0.02, "bumpless-seeded tick (coupling engaged), run through the REAL "
                                          "per-tick call site, reproduces u_desired -- the two call sites "
                                          "computed the identical feedforward");
}

static void test_feedforward_realistic_measured_matrix_zone1_row(void)
{
    TEST_SECTION("zone_feedforward() -- realistic case using the bench-measured matrix "
                 "(PID_EXPANSION_PLAN.md Phase 0): zone 1's row is 10.887 toward zone 0 and 3.332 toward "
                 "zone 2, self-gain (K_dc) 20.969");
    reset_coupling_test_state();

    const uint8_t zi = 1;
    zone_runtime_t z;
    memset(&z, 0, sizeof(z));
    z.ff_enabled = true;
    z.ff_k_dc = 20.969f;
    z.ff_tau_s = 600.0f;
    s_exec.ambient_c = 20.0f;
    float setpoint_c = 40.0f, rate = 0.0f;

    g_stub_coupling[zi][0] = 10.887f;
    g_stub_coupling[zi][2] = 3.332f;

    s_exec.zones[0].active = true;
    s_exec.zones[0].actual_valid = true;
    s_exec.zones[0].actual_c = setpoint_c + 5.0f;
    s_exec.zones[0].ff_enabled = true;
    s_exec.zones[0].ff_k_dc = 18.0f;
    s_exec.zones[0].control_mode = ZONE_CONTROL_MODE_PID;

    s_exec.zones[2].active = true;
    s_exec.zones[2].actual_valid = true;
    s_exec.zones[2].actual_c = setpoint_c - 3.0f;
    s_exec.zones[2].ff_enabled = true;
    s_exec.zones[2].ff_k_dc = 15.0f;
    s_exec.zones[2].control_mode = ZONE_CONTROL_MODE_PID;

    float u_ff = zone_feedforward(&z, zi, setpoint_c, rate);

    float baseline = (setpoint_c - s_exec.ambient_c) / z.ff_k_dc;
    TEST_CHECK(baseline > 0.0f && baseline < 1.0f, "test setup sanity: baseline must sit inside the clamp");

    float gd0 = g_stub_coupling[zi][0] / s_exec.zones[0].ff_k_dc;
    float gd2 = g_stub_coupling[zi][2] / s_exec.zones[2].ff_k_dc;
    float expect = baseline
                  - (gd0 / z.ff_k_dc) * (s_exec.zones[0].actual_c - setpoint_c)
                  - (gd2 / z.ff_k_dc) * (s_exec.zones[2].actual_c - setpoint_c);
    TEST_CHECK(expect > 0.0f && expect < 1.0f, "test setup sanity: expected result must sit inside the clamp "
                                               "too, or this check can't tell a coupling bug from the clamp");

    TEST_CHECK(fabsf(u_ff - expect) < 1e-5f, "u_ff must match the documented "
              "-(coupling_coeff[j]/(k_dc_j*ff_k_dc))*(T_j-sp_j) sum over both neighbors");
    TEST_CHECK(u_ff < baseline, "net effect here (hot zone 0 dominates cold zone 2) must be a net duty reduction");
    TEST_CHECK(isfinite(u_ff) && u_ff >= 0.0f && u_ff <= 1.0f, "result must stay inside the existing [0,1] clamp");
}

// ---------------------------------------------------------------------------
// Opus review, BLOCKING finding: the old gate here was just
// active/actual_valid/ff_enabled/finite/>0 -- a zone in ZONE_CONTROL_MODE_OFF
// or blocked by relay_authority_zone_blocked() passed all of that despite its
// temperature error having nothing to do with its own heater. The tests below
// prove the fix (zone_qualifies_as_coupling_neighbor()), the bound, the
// filter, and the log-line fix, and that fixing all of it did not disturb the
// zero-coefficient parity property proven above.

static void test_feedforward_control_mode_off_neighbor_contributes_zero(void)
{
    TEST_SECTION("zone_feedforward() -- BLOCKING finding 1: a neighbor sitting in "
                 "ZONE_CONTROL_MODE_OFF contributes exactly 0, reproducing the reviewer's own scenario "
                 "(a ~600C gap between a shared setpoint and an OFF neighbor's reading, coefficients "
                 "c_ij=3.332, k_dc_j=23.641, z's own k_dc=20.969 -- the exact inputs the review computed "
                 "a +4.0 duty term from) instead of adding that term and getting clamped to the same "
                 "duty a real +4.0 disturbance would produce");
    reset_coupling_test_state();

    const uint8_t zi = 1;
    zone_runtime_t z;
    memset(&z, 0, sizeof(z));
    z.ff_enabled = true;
    z.ff_k_dc = 20.969f;
    z.ff_tau_s = 600.0f;

    /* setpoint == ambient and rate == 0 so hold+climb == 0 exactly -- the
     * ONLY thing that can move u_ff away from 0.0f is the coupling term.
     * Real firings never run this way (ambient is never the setpoint), but
     * pinning the baseline to a known constant makes the coupling term
     * directly visible instead of possibly overlapping with the [0,1] clamp
     * the way the reviewer's own 900C example does -- see the doc comment
     * above coupling_filter_tick() call sites for why the clamp can hide
     * this exact bug (buggy-and-fixed both saturate to 1.0 at 900C). */
    s_exec.ambient_c = 0.0f;
    float setpoint_c = 0.0f, rate = 0.0f;

    g_stub_coupling[zi][0] = 3.332f;
    s_exec.zones[0].active = true;
    s_exec.zones[0].actual_valid = true;
    s_exec.zones[0].actual_c = -600.0f; /* 600C below the shared setpoint, reviewer's own gap */
    s_exec.zones[0].ff_enabled = true;
    s_exec.zones[0].ff_k_dc = 23.641f;
    /* control_mode left at 0 == ZONE_CONTROL_MODE_OFF (memset default) --
     * exactly the reviewer's "zone 2 set to OFF" board state. */

    float u_ff_off = zone_feedforward(&z, zi, setpoint_c, rate);
    TEST_CHECK(u_ff_off == 0.0f, "an OFF neighbor 600C off setpoint must contribute exactly 0 -- the old "
              "code computed +4.0 here and only the final [0,1] clamp saved it from being visibly wrong");

    /* Sanity: the SAME neighbor state, control_mode flipped to PID, must
     * move u_ff -- proves the check above is the control_mode gate actually
     * doing something, not a coincidence of some other guard already
     * zeroing this neighbor out. Kept comfortably inside the clamp (a 20C
     * gap, not 600C) so the assertion checks the real number, not just
     * "hit the ceiling". */
    s_exec.zones[0].control_mode = ZONE_CONTROL_MODE_PID;
    s_exec.zones[0].actual_c = -20.0f;
    float u_ff_pid = zone_feedforward(&z, zi, setpoint_c, rate);
    float gd = g_stub_coupling[zi][0] / s_exec.zones[0].ff_k_dc;
    float expect_pid = -(gd / z.ff_k_dc) * (s_exec.zones[0].actual_c - setpoint_c);
    TEST_CHECK(fabsf(u_ff_pid - expect_pid) < 1e-5f, "with control_mode == PID the same neighbor DOES "
              "contribute, matching the documented formula -- proves the OFF case above is the gate, "
              "not some other exclusion");
    TEST_CHECK(u_ff_pid != 0.0f, "sanity: the PID case must not also be 0, or the check above is vacuous");

    /* BANGBANG must be excluded too -- the derivation requires the LINEAR
     * PID/FF response, which BANGBANG's on/off band is not. */
    s_exec.zones[0].control_mode = ZONE_CONTROL_MODE_BANGBANG;
    s_exec.zones[0].actual_c = -600.0f;
    float u_ff_bangbang = zone_feedforward(&z, zi, setpoint_c, rate);
    TEST_CHECK(u_ff_bangbang == 0.0f, "a BANGBANG neighbor must also contribute exactly 0 -- only the "
              "PID family closes the loop the derivation assumes");
}

static void test_feedforward_faulted_or_blocked_neighbor_contributes_zero(void)
{
    TEST_SECTION("zone_feedforward() -- BLOCKING finding 1: a neighbor that is otherwise fully "
                 "qualifying (active, PID mode, valid reading, has a model) but is faulted or "
                 "authority-blocked still contributes exactly 0");
    reset_coupling_test_state();

    const uint8_t zi = 1;
    zone_runtime_t z;
    memset(&z, 0, sizeof(z));
    z.ff_enabled = true;
    z.ff_k_dc = 20.969f;
    z.ff_tau_s = 600.0f;
    s_exec.ambient_c = 0.0f;
    float setpoint_c = 0.0f, rate = 0.0f;

    g_stub_coupling[zi][0] = 3.332f;
    s_exec.zones[0].active = true;
    s_exec.zones[0].actual_valid = true;
    s_exec.zones[0].actual_c = -20.0f;
    s_exec.zones[0].ff_enabled = true;
    s_exec.zones[0].ff_k_dc = 23.641f;
    s_exec.zones[0].control_mode = ZONE_CONTROL_MODE_PID;

    float u_ff_healthy = zone_feedforward(&z, zi, setpoint_c, rate);
    TEST_CHECK(u_ff_healthy != 0.0f, "test setup sanity: a fully-qualifying neighbor must contribute "
              "something, or the two checks below can't tell exclusion from coincidence");

    s_exec.zones[0].faulted = true;
    float u_ff_faulted = zone_feedforward(&z, zi, setpoint_c, rate);
    TEST_CHECK(u_ff_faulted == 0.0f, "a faulted neighbor must contribute exactly 0 despite passing "
              "every other check");
    s_exec.zones[0].faulted = false;

    s_exec.zones[0].heat_blocked = true;
    float u_ff_blocked = zone_feedforward(&z, zi, setpoint_c, rate);
    TEST_CHECK(u_ff_blocked == 0.0f, "a relay-authority-blocked neighbor (heat_blocked, "
              "relay_authority_zone_blocked()'s last answer) must contribute exactly 0 despite passing "
              "every other check");
    s_exec.zones[0].heat_blocked = false;

    float u_ff_recovered = zone_feedforward(&z, zi, setpoint_c, rate);
    TEST_CHECK(u_ff_recovered == u_ff_healthy, "clearing both faulted and heat_blocked must restore "
              "exactly the healthy contribution -- proves neither flag left any residual state behind");
}

static void test_feedforward_neighbor_deviation_bound_actually_bounds(void)
{
    TEST_SECTION("zone_feedforward() -- finding 2: the per-neighbor deviation bound (zn's own "
                 "pid_cfg.pid_range_c) actually caps the contribution -- an extreme deviation cannot "
                 "move u_ff further than the bound implies, not just further than the outer [0,1] clamp");
    reset_coupling_test_state();

    const uint8_t zi = 1;
    zone_runtime_t z;
    memset(&z, 0, sizeof(z));
    z.ff_enabled = true;
    z.ff_k_dc = 20.969f;
    z.ff_tau_s = 600.0f;
    s_exec.ambient_c = 0.0f;
    float setpoint_c = 0.0f, rate = 0.0f;

    g_stub_coupling[zi][0] = 3.332f;
    s_exec.zones[0].active = true;
    s_exec.zones[0].actual_valid = true;
    s_exec.zones[0].ff_enabled = true;
    s_exec.zones[0].ff_k_dc = 23.641f;
    s_exec.zones[0].control_mode = ZONE_CONTROL_MODE_PID;
    s_exec.zones[0].pid_cfg.pid_range_c = 25.0f; /* the bound under test */

    float gd = g_stub_coupling[zi][0] / s_exec.zones[0].ff_k_dc;
    float u_ff_at_bound_expect = -(gd / z.ff_k_dc) * (-25.0f);

    s_exec.zones[0].actual_c = -25.0f; /* exactly at the bound */
    float u_ff_at_bound = zone_feedforward(&z, zi, setpoint_c, rate);
    TEST_CHECK(fabsf(u_ff_at_bound - u_ff_at_bound_expect) < 1e-5f, "at exactly the bound, u_ff must "
              "match the formula evaluated at the bound -- confirms the expected-value formula below "
              "is the right oracle before using it to prove the clamp");

    s_exec.zones[0].actual_c = -500.0f; /* 20x past the bound */
    float u_ff_past_bound = zone_feedforward(&z, zi, setpoint_c, rate);
    TEST_CHECK(fabsf(u_ff_past_bound - u_ff_at_bound) < 1e-5f, "a deviation 20x past the bound must "
              "produce the SAME u_ff as exactly-at-the-bound -- proves the per-neighbor bound saturates "
              "the term, not merely the outer [0,1] clamp (which a -500C deviation would not even reach "
              "here: u_ff_at_bound is well inside [0,1])");
    TEST_CHECK(u_ff_at_bound > 0.0f && u_ff_at_bound < 1.0f, "test setup sanity: the bounded result must "
              "sit strictly inside the outer clamp, so the equality above proves the PER-NEIGHBOR bound "
              "engaged, not that both cases merely hit the same outer ceiling");

    s_exec.zones[0].actual_c = -1000000.0f; /* pathological */
    float u_ff_pathological = zone_feedforward(&z, zi, setpoint_c, rate);
    TEST_CHECK(isfinite(u_ff_pathological), "a pathological deviation must not produce inf/NaN");
    TEST_CHECK(fabsf(u_ff_pathological - u_ff_at_bound) < 1e-5f, "even a million-degree deviation "
              "produces exactly the bounded value, never more");
}

static void test_coupling_filter_tick_attenuates_a_step(void)
{
    TEST_SECTION("coupling_filter_tick() -- finding 3: a step in the neighbor's raw reading is "
                 "attenuated (low-pass, same alpha = dt/(tau+dt) formula as pid.c's D-term filter), not "
                 "passed straight through to coupling_filtered_c");

    zone_runtime_t zn;
    memset(&zn, 0, sizeof(zn));
    zn.pid_cfg.d_filter_tau_s = 10.0f;

    /* First sample: the filter has no history yet, so it snaps to the raw
     * reading exactly (same "prime, don't filter" behaviour pid.c's own
     * d_filtered gets on its first tick) -- this is the correct, documented
     * cold-start case, not a bug in the attenuation test below. */
    zn.actual_c = 0.0f;
    coupling_filter_tick(&zn, /*actual_valid_now=*/true, /*dt_s=*/1.0f);
    TEST_CHECK(zn.coupling_filter_init, "first valid tick must mark the filter initialized");
    TEST_CHECK(zn.coupling_filtered_c == 0.0f, "first valid tick snaps to the raw reading exactly");

    /* Now the actual step: raw jumps from 0 to 100 in one tick. alpha =
     * dt/(tau+dt) = 1/(10+1) = 0.090909..., so the filtered value should
     * move to ~9.09, NOT jump straight to 100. */
    zn.actual_c = 100.0f;
    coupling_filter_tick(&zn, true, 1.0f);
    float alpha = 1.0f / (10.0f + 1.0f);
    float expect_after_1 = 0.0f + alpha * (100.0f - 0.0f);
    TEST_CHECK_NEAR(zn.coupling_filtered_c, expect_after_1, 1e-4, "one tick after a 0->100 step, the "
              "filtered value must match the documented low-pass formula");
    TEST_CHECK(zn.coupling_filtered_c > 1.0f && zn.coupling_filtered_c < 20.0f, "a 100-unit step must "
              "be substantially attenuated one tick later -- proves this is a low-pass, not a "
              "pass-through (which would read exactly 100 here)");

    /* Hold the step and let the filter keep approaching -- proves it is a
     * genuine low-pass that settles over several taus, not a one-shot
     * partial update. */
    for (int i = 0; i < 200; i++) {
        coupling_filter_tick(&zn, true, 1.0f);
    }
    TEST_CHECK_NEAR(zn.coupling_filtered_c, 100.0f, 0.5, "after ~20 tau (200 ticks at tau=10s, dt=1s) "
              "the filter must have settled to within 0.5 of the raw value -- proves it tracks a held "
              "input rather than staying frozen at the attenuated first-tick value");

    /* An invalid reading must freeze the filter, not reset or chase it. */
    float frozen_at = zn.coupling_filtered_c;
    zn.actual_c = -9999.0f;
    coupling_filter_tick(&zn, /*actual_valid_now=*/false, 1.0f);
    TEST_CHECK(zn.coupling_filtered_c == frozen_at, "an invalid reading must leave coupling_filtered_c "
              "untouched (frozen), matching pid.c's own d_filtered freeze on an out-of-range tick");
}

static void test_coupling_neighbor_count_matches_runtime_qualification(void)
{
    TEST_SECTION("count_qualifying_coupling_neighbors() -- finding 4: the firing-start log's neighbor "
                 "count must equal the number zone_feedforward() will actually use, not the number of "
                 "nonzero coefficients");
    reset_coupling_test_state();

    const uint8_t zi = 1;
    g_stub_coupling[zi][0] = 3.332f;
    g_stub_coupling[zi][2] = 10.887f;

    /* Both coefficients nonzero, but neither neighbor qualifies (both left
     * at the memset default: control_mode == OFF). The OLD log logic (count
     * of nonzero coefficients) would report 2 here; the count this firing
     * will actually run with is 0. */
    s_exec.zones[0].active = true;
    s_exec.zones[0].actual_valid = true;
    s_exec.zones[0].actual_c = 50.0f;
    s_exec.zones[0].ff_enabled = true;
    s_exec.zones[0].ff_k_dc = 23.641f;
    /* control_mode left OFF */

    s_exec.zones[2].active = true;
    s_exec.zones[2].actual_valid = true;
    s_exec.zones[2].actual_c = 50.0f;
    s_exec.zones[2].ff_enabled = true;
    s_exec.zones[2].ff_k_dc = 15.0f;
    s_exec.zones[2].faulted = true; /* qualifying in every other respect except this */

    TEST_CHECK(count_qualifying_coupling_neighbors(zi) == 0, "two nonzero coefficients, zero qualifying "
              "neighbors (one OFF, one faulted) -- the old coefficient-only count would have logged "
              "\"coupling ON: 2 neighbor(s)\" for a firing where the coupling term never contributes");

    /* Qualify zone 0 only. */
    s_exec.zones[0].control_mode = ZONE_CONTROL_MODE_PID;
    TEST_CHECK(count_qualifying_coupling_neighbors(zi) == 1, "exactly one neighbor now qualifies -- the "
              "count must track the single change, not stay stuck at 0 or jump to 2");

    /* Qualify zone 2 as well (clear its fault). */
    s_exec.zones[2].faulted = false;
    s_exec.zones[2].control_mode = ZONE_CONTROL_MODE_PID;
    TEST_CHECK(count_qualifying_coupling_neighbors(zi) == 2, "both neighbors now qualify -- count must "
              "reach 2, matching the coefficient-only count exactly BECAUSE both now genuinely qualify, "
              "not because the gate was bypassed");

    /* A neighbor with a zero coefficient never counts, however well it
     * otherwise qualifies -- the zero-coefficient parity property extends to
     * the count too. */
    g_stub_coupling[zi][2] = 0.0f;
    TEST_CHECK(count_qualifying_coupling_neighbors(zi) == 1, "a zeroed coefficient drops the count back "
              "to 1 even though zone 2 still fully qualifies otherwise");
}

static void test_feedforward_zero_coefficient_parity_still_holds_with_new_gates(void)
{
    TEST_SECTION("zone_feedforward() -- regression guard: the zero-coefficient bit-identical parity "
                 "property (proven above by test_feedforward_zero_coupling_is_bit_identical_to_no_coupling) "
                 "must still hold now that neighbors also carry control_mode/faulted/heat_blocked state -- "
                 "an all-zero coupling row must be a no-op REGARDLESS of what those new fields say, since "
                 "the coefficient-zero check must short-circuit before any of them are even read");
    reset_coupling_test_state();

    zone_runtime_t z;
    memset(&z, 0, sizeof(z));
    z.ff_enabled = true;
    z.ff_k_dc = 100.0f;
    z.ff_tau_s = 600.0f;
    s_exec.ambient_c = 20.0f;
    float setpoint_c = 100.0f, rate = 0.0f;
    float expect = (setpoint_c - s_exec.ambient_c) / z.ff_k_dc;

    /* g_stub_coupling stays all-zero (reset_coupling_test_state()). Put
     * every neighbor in the most "should obviously contribute" state
     * possible -- active, valid, PID mode, healthy, a real model -- so this
     * test cannot pass by accident (every neighbor already excluded some
     * other way). */
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        if (j == 1) continue;
        s_exec.zones[j].active = true;
        s_exec.zones[j].actual_valid = true;
        s_exec.zones[j].actual_c = setpoint_c + 500.0f; /* would be a huge contribution if it counted */
        s_exec.zones[j].ff_enabled = true;
        s_exec.zones[j].ff_k_dc = 20.0f;
        s_exec.zones[j].control_mode = ZONE_CONTROL_MODE_PID;
        s_exec.zones[j].faulted = false;
        s_exec.zones[j].heat_blocked = false;
    }

    float u_ff = zone_feedforward(&z, 1, setpoint_c, rate);
    TEST_CHECK(u_ff == expect, "an all-zero coupling row must not move u_ff by even one ULP, even with "
              "every neighbor otherwise fully qualifying");
}

// ---------------------------------------------------------------------------
// profile_executor_guard_commanded_duty() -- the shipped-firmware PWM/
// progress-window defect fix (guards 1/2/3/7 all gate their multi-tick
// accumulation windows on commanded_duty, which used to be fed the POST-PWM
// relay_commanded_on state; see this file's apply-relays-and-guards loop for
// the full defect and the load-cap/authority-block reasoning). Two layers:
//   - Direct unit tests of the helper itself (the decision logic).
//   - Scenario tests proving the ACTUAL accumulation behavior differs
//     between the OLD (post-PWM) and NEW (intended-duty) expressions, for
//     guard 1 (dead element) and guard 7 (frozen sensor) specifically --
//     guard 7 has no PID-saturation escape and is the one with no other
//     cover, per the review that found this defect.
// ---------------------------------------------------------------------------

static void test_guard_commanded_duty_normal_case_passes_through(void)
{
    TEST_SECTION("profile_executor_guard_commanded_duty(): the ordinary case (not blocked) reports the "
                 "intended duty unchanged");
    float d = profile_executor_guard_commanded_duty(/*heat_blocked=*/false, /*intended_duty=*/0.6f);
    TEST_CHECK_NEAR(d, 0.6f, 1e-6f, "must pass the intended duty through");
}

static void test_guard_commanded_duty_authority_block_zeroes(void)
{
    TEST_SECTION("profile_executor_guard_commanded_duty(): heat_blocked=true reports ZERO, not the "
                 "intended duty -- a deterministic, already-reported fact, not something guard 1/2 should "
                 "re-derive");
    float d = profile_executor_guard_commanded_duty(/*heat_blocked=*/true, /*intended_duty=*/0.6f);
    TEST_CHECK_NEAR(d, 0.0f, 1e-6f, "must report zero when heat is blocked");
}

// Review defect 1's own root-cause fix: apply_relay() must refresh
// z->heat_blocked on EVERY call, want_on or not -- the first version only
// refreshed it inside `if (want_on)`, which meant a PWM off-pulse
// (apply_relay(zi, false)) left heat_blocked stale from the LAST on-pulse.
// That was "accidentally correct" while blocked (the last on-pulse's real
// answer WAS true), which is exactly why profile_executor_guard_commanded_
// duty()'s first version's want_relay_on_this_tick gate was actively wrong
// (see that function's own comment) -- but relying on staleness at all was
// the underlying defect. Drives the REAL apply_relay() (not a hand-set
// z->heat_blocked) with want_on=false while relay_authority_zone_blocked()
// answers true, and checks the field it actually writes.
static void test_apply_relay_refreshes_heat_blocked_even_on_a_want_on_false_tick(void)
{
    TEST_SECTION("apply_relay(zi, want_on=false) still refreshes z->heat_blocked for real when "
                 "relay_authority_zone_blocked() answers true -- the fix must not depend on staleness");
    memset(&s_exec, 0, sizeof(s_exec));
    s_exec.zones[0].active = true;
    g_stub_relay_mask[0] = 0x01u; /* apply_relay() early-returns on mask==0 -- must be nonzero to reach the check */
    s_test_relay_authority_zone_blocked = true;
    s_test_relay_authority_zone_blocked_sources = 0x02u;

    apply_relay(/*zi=*/0, /*want_on=*/false);

    TEST_CHECK(s_exec.zones[0].heat_blocked, "heat_blocked must read true even though this call's want_on "
                                             "was false -- the evaluation must not be gated on want_on");
    TEST_CHECK(s_exec.zones[0].heat_blocked_sources == 0x02u, "sources must be captured too, not just the bool");

    s_test_relay_authority_zone_blocked = false;
    s_test_relay_authority_zone_blocked_sources = 0;
    g_stub_relay_mask[0] = 0;
}

// Runs n_ticks of thermal_guard_tick() at a constant target duty, computing
// commanded_duty EITHER the OLD way (relay_commanded_on-gated, PWM-chopped
// via the REAL heater_output_duty(), reproducing exactly what the shipped
// defect fed the guards) OR the NEW way (profile_executor_guard_commanded_
// duty() with a constant intended duty, no PWM chop -- what an unblocked
// zone commanding a steady duty produces). measurement stays perfectly flat
// throughout -- both a dead element (guard 1, at a setpoint far above so
// the climbing branch applies) and a frozen sensor (guard 7) look
// identical: an unmoving reading. sanity_rate_c_per_min is fixed at
// thermal_guard.c's own 0.5 C/min default (no ramp in progress) unless
// overridden via the cfg parameter -- callers that need the ramp-rate cap
// build their own cfg with profile_executor_guard_sanity_rate().
static bool run_guard_scenario_cfg(bool use_old_pwm_expression, float duty, float measurement_c,
                                   float setpoint_c, int n_ticks, const thermal_guard_cfg_t *cfg,
                                   float dither_c)
{
    thermal_guard_state_t gs;
    thermal_guard_reset(&gs);

    heater_output_state_t hstate;
    heater_output_cfg_t hcfg = {.window_ms = HEATER_DEFAULT_WINDOW_MS, .min_on_ms = 0, .min_off_ms = 0};
    heater_output_reset(&hstate);

    for (int i = 0; i < n_ticks; i++) {
        float commanded;
        if (use_old_pwm_expression) {
            // The OLD, shipped expression: relay_commanded_on ? (duty>0 ?
            // duty : 1.0) : 0.0 -- relay_commanded_on IS heater_output_
            // duty()'s real PWM decision, reproduced here via the real
            // function so this is a faithful repro, not a hand-waved one.
            bool relay_on = heater_output_duty(&hstate, &hcfg, duty, PROFILE_EXECUTOR_TICK_MS);
            commanded = relay_on ? (duty > 0.0f ? duty : 1.0f) : 0.0f;
        } else {
            // The NEW expression -- an unblocked zone steadily commanding `duty`.
            commanded = profile_executor_guard_commanded_duty(/*heat_blocked=*/false, duty);
        }
        float meas = measurement_c + ((dither_c != 0.0f) ? ((i % 2) ? dither_c : -dither_c) : 0.0f);
        thermal_guard_input_t gin = {
            .sensor_ok = true,
            .measurement_c = meas,
            .setpoint_c = setpoint_c,
            .commanded_duty = commanded,
            .dt_s = (float)PROFILE_EXECUTOR_TICK_MS / 1000.0f,
        };
        if (thermal_guard_tick(&gs, cfg, &gin)) {
            return true;
        }
    }
    return false;
}

static bool run_guard_scenario(bool use_old_pwm_expression, float duty, float measurement_c, float setpoint_c,
                               int n_ticks)
{
    thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
    return run_guard_scenario_cfg(use_old_pwm_expression, duty, measurement_c, setpoint_c, n_ticks, &cfg, 0.0f);
}

static void test_guard1_never_catches_a_dead_element_at_partial_duty_under_the_old_expression(void)
{
    TEST_SECTION("MANDATORY negative test (guard 1): a genuinely dead element at duty 0.5, fed through the "
                 "OLD post-PWM commanded_duty expression, NEVER trips guard 1 within 900s -- the 300s "
                 "window can never complete against a 60s PWM window chopping 0.5 duty into 30s on/30s off");
    // setpoint far above measurement -> guard 1's climbing branch; flat
    // measurement -> zero rise, exactly a dead element.
    bool tripped = run_guard_scenario(/*use_old_pwm_expression=*/true, /*duty=*/0.5f, /*measurement_c=*/25.0f,
                                      /*setpoint_c=*/500.0f, /*n_ticks=*/900);
    TEST_CHECK(!tripped, "the OLD expression must NEVER complete guard 1's window at duty 0.5 -- this IS "
                        "the shipped defect, reproduced here through the real heater_output_duty() PWM");
}

static void test_guard1_catches_a_dead_element_at_partial_duty_with_the_fix(void)
{
    TEST_SECTION("MANDATORY negative test (guard 1), PASS side: the SAME dead element at duty 0.5 DOES trip "
                 "guard 1 with the fix (intended duty, no PWM chop)");
    bool tripped = run_guard_scenario(/*use_old_pwm_expression=*/false, /*duty=*/0.5f, /*measurement_c=*/25.0f,
                                      /*setpoint_c=*/500.0f, /*n_ticks=*/320);
    TEST_CHECK(tripped, "the fixed expression must trip guard 1 well within its 300s window at duty 0.5");
}

static void test_guard7_never_catches_a_frozen_sensor_at_partial_duty_under_the_old_expression(void)
{
    TEST_SECTION("MANDATORY negative test (guard 7, no PID-saturation escape): a frozen sensor at duty 0.5, "
                 "fed through the OLD post-PWM expression, NEVER trips guard 7 within 900s -- the 600s "
                 "window needs commanded_duty > 0 CONTINUOUSLY, which the PWM chop never provides");
    // setpoint == measurement (no rise/fall question at all -- purely
    // testing guard 7's own frozen-window logic).
    bool tripped = run_guard_scenario(/*use_old_pwm_expression=*/true, /*duty=*/0.5f, /*measurement_c=*/300.0f,
                                      /*setpoint_c=*/300.0f, /*n_ticks=*/900);
    TEST_CHECK(!tripped, "the OLD expression must NEVER complete guard 7's window at duty 0.5 -- a frozen "
                        "thermocouple during a partial-duty firing was never caught");
}

static void test_guard7_catches_a_frozen_sensor_at_partial_duty_with_the_fix(void)
{
    TEST_SECTION("MANDATORY negative test (guard 7), PASS side: the SAME frozen sensor at duty 0.5 DOES "
                 "trip guard 7 with the fix");
    bool tripped = run_guard_scenario(/*use_old_pwm_expression=*/false, /*duty=*/0.5f, /*measurement_c=*/300.0f,
                                      /*setpoint_c=*/300.0f, /*n_ticks=*/620);
    TEST_CHECK(tripped, "the fixed expression must trip guard 7 well within its 600s window at duty 0.5");
}

// Regression guard: duty == 0 must still leave guards 1/2/7 disarmed (no
// heat commanded -- nothing for them to check) and guard 3 (welded relay)
// ARMED (duty==0 is exactly guard 3's own precondition) -- the fix must not
// over-correct into arming 1/2/7 at zero duty, and must not have broken
// guard 3's existing duty==0 gate.
static void test_duty_zero_leaves_1_2_7_disarmed_and_3_armed(void)
{
    TEST_SECTION("duty == 0 (via the fixed expression): guards 1/2/7 stay disarmed (no over-correction), "
                 "guard 3 (welded relay) stays armed");
    thermal_guard_state_t gs;
    thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
    thermal_guard_reset(&gs);

    // A welded/shorted relay: duty commanded 0, but temperature keeps
    // rising anyway -- guard 3's own trip condition.
    float measurement_c = 25.0f;
    bool tripped = false;
    thermal_guard_trip_t reason = THERMAL_GUARD_TRIP_NONE;
    for (int i = 0; i < 200 && !tripped; i++) {
        measurement_c += 1.0f; /* well above guard 3's runaway rate at duty 0 */
        float commanded = profile_executor_guard_commanded_duty(/*heat_blocked=*/false, /*intended_duty=*/0.0f);
        TEST_CHECK_NEAR(commanded, 0.0f, 1e-6f, "sanity: duty must read exactly 0 for a zone commanding no heat");
        thermal_guard_input_t gin = {
            .sensor_ok = true,
            .measurement_c = measurement_c,
            .setpoint_c = 500.0f,
            .commanded_duty = commanded,
            .dt_s = 1.0f,
        };
        if (thermal_guard_tick(&gs, &cfg, &gin)) {
            tripped = true;
            reason = gs.reason;
        }
    }
    TEST_CHECK(tripped, "guard 3 (welded relay) must still trip at duty 0 with the fix in place");
    TEST_CHECK(reason == THERMAL_GUARD_TRIP_RUNAWAY, "specifically guard 3 (RUNAWAY), not 1/2/7 -- those "
                                                     "guards must not have been armed by a rising reading "
                                                     "at duty 0 (guard 1/2 need commanded_duty >= "
                                                     "progress_duty_min > 0; guard 7 needs commanded_duty "
                                                     "> 0)");
}

// MANDATORY (review defect 1): an authority block at PARTIAL intended duty
// (0.5), driven through the REAL heater_output_duty() PWM AND the REAL
// apply_relay() (so heat_blocked is refreshed exactly the way production
// code refreshes it, on-pulse and off-pulse alike) -- guard 3 (welded
// relay) must arm and trip on a welded contact discovered while blocked.
// Confirmed to FAIL against the pre-fix want_relay_on_this_tick-gated
// helper (see this task's own negative-test report for the captured
// output): that version alternated the guard's view of commanded_duty
// between 0.0 (on-pulse, heat_blocked freshly true) and 0.5 (off-pulse, the
// gate itself false so heat_blocked's true value was never applied),
// which never lets guard 3's contiguous duty<=0 window complete.
static void test_authority_block_at_partial_duty_guard3_arms_and_trips_on_a_welded_relay(void)
{
    TEST_SECTION("MANDATORY negative test (defect 1): an authority block at duty 0.5, driven through the "
                 "REAL heater_output_duty() PWM and apply_relay(), still lets guard 3 (welded relay) arm "
                 "and trip -- z->heat_blocked must read a genuine CONTIGUOUS true, not alternate with the "
                 "PWM period");
    memset(&s_exec, 0, sizeof(s_exec));
    s_exec.zones[0].active = true;
    g_stub_relay_mask[0] = 0x01u; /* apply_relay() early-returns on mask==0 -- must be nonzero to reach the check */
    s_exec.zones[0].guard_cfg = (thermal_guard_cfg_t){.max_temp_c = 1300.0f, .min_temp_c = -20.0f,
                                                       .sanity_rate_c_per_min = 0.5f};
    thermal_guard_reset(&s_exec.zones[0].guard_state);
    heater_output_reset(&s_exec.zones[0].heater_state);
    s_exec.zones[0].heater_cfg = (heater_output_cfg_t){.window_ms = HEATER_DEFAULT_WINDOW_MS, .min_on_ms = 0,
                                                        .min_off_ms = 0};
    s_test_relay_authority_zone_blocked = true;
    s_test_relay_authority_zone_blocked_sources = 0x02u;

    float measurement_c = 25.0f;
    bool tripped = false;
    thermal_guard_trip_t reason = THERMAL_GUARD_TRIP_NONE;
    for (int i = 0; i < 200 && !tripped; i++) {
        zone_runtime_t *z = &s_exec.zones[0];
        float intended_duty = 0.5f;
        bool want_relay_on = heater_output_duty(&z->heater_state, &z->heater_cfg, intended_duty,
                                                PROFILE_EXECUTOR_TICK_MS);
        apply_relay(/*zi=*/0, want_relay_on); /* refreshes z->heat_blocked for REAL, every tick */
        float commanded = profile_executor_guard_commanded_duty(z->heat_blocked, intended_duty);
        TEST_CHECK_NEAR(commanded, 0.0f, 1e-6f, "sanity: a genuinely blocked zone must read 0 on EVERY "
                                                "tick, on-pulse or off-pulse -- if this fails the alternation "
                                                "defect is back");
        measurement_c += 1.0f; /* welded/shorted contact: rising despite the block */
        thermal_guard_input_t gin = {
            .sensor_ok = true,
            .measurement_c = measurement_c,
            .setpoint_c = 500.0f,
            .commanded_duty = commanded,
            .dt_s = (float)PROFILE_EXECUTOR_TICK_MS / 1000.0f,
        };
        if (thermal_guard_tick(&z->guard_state, &z->guard_cfg, &gin)) {
            tripped = true;
            reason = z->guard_state.reason;
        }
    }
    TEST_CHECK(tripped, "guard 3 must trip on a welded relay discovered during an authority block");
    TEST_CHECK(reason == THERMAL_GUARD_TRIP_RUNAWAY, "specifically guard 3 (RUNAWAY)");

    s_test_relay_authority_zone_blocked = false;
    s_test_relay_authority_zone_blocked_sources = 0;
    g_stub_relay_mask[0] = 0;
}

// MANDATORY (review defect 2): a HEALTHY zone lagging a ramping setpoint by
// 5C (> PROGRESS_BAND_C's 3C, so guard 1's climbing branch applies), rising
// at 0.33 C/min (20 C/hr -- a routine ramp: candling, quartz inversion,
// thick ware) at duty 0.6 (>= PROGRESS_DUTY_MIN, so the widened window now
// reaches it), must NOT trip guard 1 within 600s. Confirmed to FAIL without
// profile_executor_guard_sanity_rate()'s ramp-rate cap (see this task's own
// negative-test report): thermal_guard.c's own bare 0.5 C/min default (30
// C/hr) is uncapped and demands more rise than a 20 C/hr ramp can honestly
// deliver from 5C behind.
static void test_healthy_ramp_lag_does_not_false_trip_guard1(void)
{
    TEST_SECTION("MANDATORY negative test (defect 2): a healthy zone 5C behind a 20 C/hr ramp, rising at "
                 "0.33 C/min, duty 0.6, must NOT trip guard 1 within 600s");
    const float ramp_c_per_hr = 20.0f;
    const float target_rate_c_per_s = ramp_c_per_hr / 3600.0f;
    /* 10% faster than the bare ramp rate, not exactly matching it -- a
     * genuinely healthy zone tracking (or slightly gaining on) a ramp, not
     * pinned to it. Deliberate margin over the exact rate: comparing
     * float-accumulated rise against a float-computed expected value at
     * EXACT equality is a coin flip on which side of "<" 600 tick-by-tick
     * additions land after normal floating-point rounding -- this test is
     * about the cap actually working, not about proving IEEE-754 addition
     * is associative. */
    const float rise_c_per_s = target_rate_c_per_s * 1.1f;

    thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
    cfg.sanity_rate_c_per_min = profile_executor_guard_sanity_rate(cfg.sanity_rate_c_per_min, target_rate_c_per_s);
    TEST_CHECK(cfg.sanity_rate_c_per_min < 0.5f, "sanity: the ramp-rate cap must actually have reduced the "
                                                 "requirement below the bare 0.5 C/min default");
    TEST_CHECK_NEAR(cfg.sanity_rate_c_per_min, ramp_c_per_hr / 60.0f, 1e-4f,
                    "sanity: capped at exactly the commanded ramp rate (20 C/hr = 0.333 C/min)");

    thermal_guard_state_t gs;
    thermal_guard_reset(&gs);
    float measurement_c = 495.0f; /* 5C behind a setpoint starting at 500 -- past PROGRESS_BAND_C (3C) */
    float setpoint_c = 500.0f;
    bool tripped = false;
    for (int i = 0; i < 600 && !tripped; i++) {
        measurement_c += rise_c_per_s; /* per-second rise matching the ramp -- healthy tracking, not catching up */
        setpoint_c += target_rate_c_per_s; /* the setpoint itself is also moving at the same rate */
        thermal_guard_input_t gin = {
            .sensor_ok = true,
            .measurement_c = measurement_c,
            .setpoint_c = setpoint_c,
            .commanded_duty = 0.6f,
            .dt_s = 1.0f,
        };
        if (thermal_guard_tick(&gs, &cfg, &gin)) {
            tripped = true;
        }
    }
    TEST_CHECK(!tripped, "a healthy zone tracking its own commanded ramp rate must not false-trip guard 1");
}

// Sanity companion to the ramp-lag test: WITHOUT the ramp-rate cap (dwell,
// target_rate_c_per_s == 0.0f, or the bare default), the SAME lag/rise
// numbers (which only match a 20 C/hr ramp, not the bare 30 C/hr
// requirement) DO trip -- proves the cap is what's actually preventing the
// false trip above, not some other change.
static void test_healthy_ramp_lag_still_trips_without_the_rate_cap(void)
{
    TEST_SECTION("negative control: the SAME lag/rise numbers DO trip guard 1 without the ramp-rate cap "
                 "(bare 0.5 C/min default) -- proves the cap above is load-bearing");
    thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
    thermal_guard_state_t gs;
    thermal_guard_reset(&gs);
    float measurement_c = 495.0f;
    float setpoint_c = 500.0f;
    const float rise_c_per_s = 20.0f / 3600.0f;
    bool tripped = false;
    for (int i = 0; i < 600 && !tripped; i++) {
        measurement_c += rise_c_per_s;
        setpoint_c += rise_c_per_s;
        thermal_guard_input_t gin = {
            .sensor_ok = true,
            .measurement_c = measurement_c,
            .setpoint_c = setpoint_c,
            .commanded_duty = 0.6f,
            .dt_s = 1.0f,
        };
        if (thermal_guard_tick(&gs, &cfg, &gin)) {
            tripped = true;
        }
    }
    TEST_CHECK(tripped, "without the cap, a 0.33 C/min rise against a 0.5 C/min bare requirement must trip");
}

// Dwell case: target_rate_c_per_s == 0.0f must NOT relax the requirement to
// zero -- a zone genuinely lagging a STATIONARY setpoint must still catch
// up at the full configured rate (the cap only narrows the requirement
// while a ramp is actually moving).
static void test_dwell_lag_still_requires_the_full_configured_rate(void)
{
    TEST_SECTION("a lagging DWELL (target_rate_c_per_s == 0) is NOT relaxed by the ramp-rate cap -- must "
                 "still trip guard 1 on a truly stalled catch-up");
    float capped = profile_executor_guard_sanity_rate(/*configured_rate_c_per_min=*/0.5f,
                                                       /*target_rate_c_per_s=*/0.0f);
    TEST_CHECK_NEAR(capped, 0.5f, 1e-6f, "a dwell (rate 0) must leave the configured rate unchanged, not "
                                        "cap it to zero");
}

static void test_guard7_does_not_false_trip_on_a_healthy_dwell_with_realistic_dither(void)
{
    TEST_SECTION("guard 7 (frozen sensor) does not false-trip on a healthy dwell with realistic sensor "
                 "dither PLUS real PID/PWM-cycling thermal ripple, now that partial duty actually runs "
                 "the full 600s contiguous window for the first time");
    /* Pure +/-0.02C sensor dither ALONE cannot ever exceed guard 7's own
     * FROZEN_EPS_C (0.05C) -- two samples each bounded within +/-0.02C of a
     * center can differ from each other by at most 0.04C < 0.05C, so no
     * pattern of sensor noise at that amplitude can ever reset the window
     * (confirmed: an earlier version of this test used dither alone and
     * never tripped for the wrong reason -- it could not have tripped no
     * matter how badly frozen the guard's window logic was, so it proved
     * nothing about the fix). A genuinely healthy dwell is not that quiet
     * in reality either -- this file's own PROGRESS_BAND_C comment
     * documents real PID/time-proportioning ripple "under 2C on this bench
     * at a 60s window and full duty". Modeled here as a modest 0.3C, 60s-
     * period ripple (comfortably inside that documented range, at a lower
     * partial duty) with +/-0.02C sensor dither on top -- realistic, and
     * large enough to periodically clear the 0.05C epsilon and legitimately
     * re-arm guard 7's window, the same way a genuinely live sensor would. */
    thermal_guard_state_t gs;
    thermal_guard_cfg_t cfg = {.max_temp_c = 1300.0f, .min_temp_c = -20.0f, .sanity_rate_c_per_min = 0.5f};
    thermal_guard_reset(&gs);
    bool tripped = false;
    for (int i = 0; i < 620 && !tripped; i++) {
        float ripple_c = 0.3f * sinf(2.0f * 3.14159265f * (float)i / 60.0f);
        float dither_c = (i % 2) ? 0.02f : -0.02f;
        thermal_guard_input_t gin = {
            .sensor_ok = true,
            .measurement_c = 300.0f + ripple_c + dither_c,
            .setpoint_c = 300.0f,
            .commanded_duty = 0.5f,
            .dt_s = 1.0f,
        };
        if (thermal_guard_tick(&gs, &cfg, &gin)) {
            tripped = true;
        }
    }
    TEST_CHECK(!tripped, "realistic dwell ripple + sensor dither must not read as frozen");
}

void run_test_profile_executor_prestart(void)
{
    test_run_refuses_before_start();
    test_halt_is_a_silent_noop_before_start();
    test_pause_resume_refuse_before_start();
    test_zone_is_active_false_before_start();
    test_get_history_empty_before_start();
    test_get_status_reports_well_formed_idle_before_start();
    test_escalate_guard_trip_global_releases_relay_claim();
    test_escalate_guard_trip_abort_policy_releases_relay_claim();
    test_escalate_guard_trip_all_zones_faulted_releases_relay_claim();
    test_pause_keeps_claim_resume_reclaims_it();
    test_guard9_asserts_and_ors_global_fault_source();
    test_guard9_ors_without_clobbering_an_earlier_global_trip();
    test_guard9_fault_source_cleared_on_halt();
    test_profile_zones_have_ceiling_refuses_on_zero();
    test_profile_zones_have_ceiling_passes_when_configured();
    test_profile_zones_have_ceiling_ignores_inactive_zones();
    test_profile_zones_have_ceiling_ignores_off_zones_in_mask();
    test_profile_zones_have_ceiling_still_refuses_on_zero_when_off_zone_is_healthy();
    test_io_seg_finish_default_forces_off_on_done();
    test_io_seg_finish_leave_on_honored_only_on_done();
    test_io_segs_force_all_off_sweeps_general_io_too();
    test_run_refuses_while_zone_sweep_is_active();
    test_run_decodes_fault_sources_instead_of_hex();
    test_run_refuses_at_atomic_heat_claim_gate();
    test_guard_trip_releases_heat_enable();
    test_halt_releases_heat_enable();
    test_pause_releases_heat_enable_and_resume_reacquires();
    test_heat_enable_release_survives_a_down_link();

    test_fuzzy_prepare_gains_zero_strength_is_base_gains_bit_exact();
    test_fuzzy_prepare_gains_matches_pid_fuzzy_adjust_directly();
    test_fuzzy_prepare_gains_nan_strength_falls_back_to_base_not_large();

    test_feedforward_zero_coupling_is_bit_identical_to_no_coupling();
    test_feedforward_hot_neighbor_subtracts_duty();
    test_feedforward_invalid_neighbor_contributes_zero_never_nan();
    test_feedforward_both_callers_agree_bump_transfer();
    test_feedforward_realistic_measured_matrix_zone1_row();

    test_feedforward_control_mode_off_neighbor_contributes_zero();
    test_feedforward_faulted_or_blocked_neighbor_contributes_zero();
    test_feedforward_neighbor_deviation_bound_actually_bounds();
    test_coupling_filter_tick_attenuates_a_step();
    test_coupling_neighbor_count_matches_runtime_qualification();
    test_feedforward_zero_coefficient_parity_still_holds_with_new_gates();

    // Warm-start (PROFILES.md, owner request 2026-08-30) -- run last: unlike
    // every test above, these drive profile_executor_run() through a real
    // full-profile start (see this block's own header comment), which
    // leaves s_exec.lock non-NULL -- test_run_refuses_before_start() and
    // friends above assume the opposite (fresh process state) and must run
    // first.
    test_warm_start_cold_kiln_is_a_regression_noop();
    test_warm_start_mid_ramp_entry_never_below_current();
    test_warm_start_replays_skipped_relay_io_and_registers_it();
    test_warm_start_reached_dwell_is_not_shortened();
    test_warm_start_descending_profile_does_not_jump_into_cooldown();
    test_warm_start_hotter_than_entire_profile_lands_on_last_segment();

    // PWM/progress-window fix -- order-independent, each re-derives its own
    // fresh thermal_guard_state_t/heater_output_state_t (or memsets s_exec).
    test_guard_commanded_duty_normal_case_passes_through();
    test_guard_commanded_duty_authority_block_zeroes();
    test_apply_relay_refreshes_heat_blocked_even_on_a_want_on_false_tick();
    test_guard1_never_catches_a_dead_element_at_partial_duty_under_the_old_expression();
    test_guard1_catches_a_dead_element_at_partial_duty_with_the_fix();
    test_guard7_never_catches_a_frozen_sensor_at_partial_duty_under_the_old_expression();
    test_guard7_catches_a_frozen_sensor_at_partial_duty_with_the_fix();
    test_duty_zero_leaves_1_2_7_disarmed_and_3_armed();
    test_authority_block_at_partial_duty_guard3_arms_and_trips_on_a_welded_relay();
    test_healthy_ramp_lag_does_not_false_trip_guard1();
    test_healthy_ramp_lag_still_trips_without_the_rate_cap();
    test_dwell_lag_still_requires_the_full_configured_rate();
    test_guard7_does_not_false_trip_on_a_healthy_dwell_with_realistic_dither();
}

int main(void)
{
    run_test_profile_executor_prestart();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
