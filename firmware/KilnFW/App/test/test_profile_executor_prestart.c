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

esp_err_t MAX31856_read_all(MAX31856BusClass *bus, MAX31856Reading *out, size_t max_readings, size_t *out_count)
{
    (void)bus; (void)out; (void)max_readings;
    if (out_count) *out_count = 0;
    return ESP_FAIL;
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

bool zones_config_get_max_ramp(uint8_t zone_index, float *out_c_per_hr)
{
    (void)zone_index;
    if (out_c_per_hr) *out_c_per_hr = 0.0f;
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

bool zones_config_get_pid(uint8_t zone_index, float *out_kp, float *out_ki, float *out_kd)
{
    (void)zone_index;
    if (out_kp) *out_kp = 0.0f;
    if (out_ki) *out_ki = 0.0f;
    if (out_kd) *out_kd = 0.0f;
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

bool zones_config_get_thermo_mask(uint8_t zone_index, uint8_t *out_mask)
{
    (void)zone_index;
    if (out_mask) *out_mask = 0;
    return false;
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
