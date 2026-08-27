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

esp_err_t kiln_io_owner_command_set_relay_mask_authorized(uint8_t mask, uint8_t value)
{
    (void)mask; (void)value;
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

bool profiles_http_get(uint8_t id, profile_t *out)
{
    (void)id;
    if (out) memset(out, 0, sizeof(*out));
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

esp_err_t safety_link_set_fault_source(SafetyLinkClass *link, uint32_t source_mask, bool assert_fault)
{
    (void)link; (void)source_mask; (void)assert_fault;
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

bool zones_config_get_control_mode(uint8_t zone_index, zone_control_mode_t *out_mode)
{
    (void)zone_index;
    if (out_mode) *out_mode = ZONE_CONTROL_MODE_OFF;
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
