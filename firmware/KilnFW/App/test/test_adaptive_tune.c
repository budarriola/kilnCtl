// Host tests for App/drivers/adaptive_tune.c -- PID_EXPANSION_PLAN.md Phase
// 7d (learn a zone's steady-state gain from the settled dwells an ordinary
// firing already produces).
//
// #includes adaptive_tune.c directly (same convention as
// test_autotune_engine_prestart.c/test_profile_executor_prestart.c) so this
// file can reach its guard constants and would otherwise need to fake a
// dozen zones_config_*()/httpd_*() symbols with no other seam. Own
// executable (build_host_tests.ps1's sixth build+run step) for the usual
// reason: it defines the REAL zones_config_get_model()/set_model()/get_pid()/
// set_pid() bodies as a tiny in-RAM fake table, which would multiply-define
// against every other test file that fakes those same names.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "test_common.h"

#include "../drivers/MAX31856.h" // MAX31856_CHANNEL_COUNT, needed by the coupling-row fakes below,
                                  // ahead of adaptive_tune.c's own #include of it further down this file
#include "esp_err.h"

// Own executable (see this file's header comment).
int g_test_failures = 0;
int g_test_count = 0;

// ---------------------------------------------------------------------
// Fakes for adaptive_tune.c's extern dependencies, in link order it needs
// them. zones_config_* below is a tiny per-zone table this file owns
// entirely, standing in for zones_config_accessors.c's real (much larger)
// NVS-backed store -- the tests only care that adaptive_tune.c reads the
// right zone's prior model and writes back the right zone's refined one,
// which this fake is precise enough to prove (see the asymmetric-zone
// tests below, which would fail if adaptive_tune.c ever transposed a zone
// index reaching into this table).
// ---------------------------------------------------------------------
#define TEST_MAX_ZONES 5
static struct {
    float k_dc, tau_s, dead_time_s;
    float kp, ki, kd;
} s_fake_zone_cfg[TEST_MAX_ZONES];

bool zones_config_get_model(uint8_t zone_index, float *out_k_dc, float *out_tau_s, float *out_dead_time_s)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    *out_k_dc = s_fake_zone_cfg[zone_index].k_dc;
    *out_tau_s = s_fake_zone_cfg[zone_index].tau_s;
    *out_dead_time_s = s_fake_zone_cfg[zone_index].dead_time_s;
    return true;
}
bool zones_config_set_model(uint8_t zone_index, float k_dc, float tau_s, float dead_time_s)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    s_fake_zone_cfg[zone_index].k_dc = k_dc;
    s_fake_zone_cfg[zone_index].tau_s = tau_s;
    s_fake_zone_cfg[zone_index].dead_time_s = dead_time_s;
    return true;
}
bool zones_config_get_pid(uint8_t zone_index, float *out_kp, float *out_ki, float *out_kd)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    *out_kp = s_fake_zone_cfg[zone_index].kp;
    *out_ki = s_fake_zone_cfg[zone_index].ki;
    *out_kd = s_fake_zone_cfg[zone_index].kd;
    return true;
}
// H3(a): mirrors the REAL zones_config_set_pid() (zones_config_accessors.c)
// bound-and-reject behaviour, not just "accept anything" -- the real setter
// REJECTS (returns false, leaves the stored gain untouched) any of kp/ki/kd
// outside [0, ZONE_PID_GAIN_MAX], it does not silently clamp. Before this
// fix the fake accepted any finite value unconditionally, which is exactly
// this repo's idealized-test-input bug class: try_refine_ki_locked()'s
// repeated-Ki-application path (H3) had no real ceiling to run into on host,
// even though hardware does (this same bound, reached through the identical
// setter every caller in this file uses -- see that function's own comment
// on why the invalidation/bound checks live in the one shared setter).
#define TEST_ZONE_PID_GAIN_MAX 1000.0f // == zones_http.h's ZONE_PID_GAIN_MAX; redefined by hand since this
                                        // file does not include zones_http.h (see its own #include list)
bool zones_config_set_pid(uint8_t zone_index, float kp, float ki, float kd)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    if (!isfinite(kp) || !isfinite(ki) || !isfinite(kd) || kp < 0.0f || ki < 0.0f || kd < 0.0f ||
        kp > TEST_ZONE_PID_GAIN_MAX || ki > TEST_ZONE_PID_GAIN_MAX || kd > TEST_ZONE_PID_GAIN_MAX) {
        return false;
    }
    s_fake_zone_cfg[zone_index].kp = kp;
    s_fake_zone_cfg[zone_index].ki = ki;
    s_fake_zone_cfg[zone_index].kd = kd;
    return true;
}

// Coupling-row fake, same "tiny in-RAM table" convention as s_fake_zone_cfg
// above, standing in for zones_config_accessors.c's real coupling_coeff[]
// storage -- adaptive_tune.c's coupled-solve apply path
// (try_refine_coupled_locked()) reads/writes exactly this surface.
static float s_fake_coupling[TEST_MAX_ZONES][TEST_MAX_ZONES];
static float s_fake_coupling_tau[TEST_MAX_ZONES][TEST_MAX_ZONES];
static float s_fake_coupling_dead[TEST_MAX_ZONES][TEST_MAX_ZONES];

bool zones_config_get_coupling(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    for (int j = 0; j < MAX31856_CHANNEL_COUNT; j++) out_row[j] = s_fake_coupling[zone_index][j];
    return true;
}
bool zones_config_get_coupling_tau(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    for (int j = 0; j < MAX31856_CHANNEL_COUNT; j++) out_row[j] = s_fake_coupling_tau[zone_index][j];
    return true;
}
bool zones_config_get_coupling_dead_time(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    for (int j = 0; j < MAX31856_CHANNEL_COUNT; j++) out_row[j] = s_fake_coupling_dead[zone_index][j];
    return true;
}
bool zones_config_set_coupling_cell(uint8_t zone_index, uint8_t neighbor_index, float coeff, float tau_s,
                                     float dead_time_s)
{
    if (zone_index >= TEST_MAX_ZONES || neighbor_index >= TEST_MAX_ZONES) return false;
    s_fake_coupling[zone_index][neighbor_index] = coeff;
    s_fake_coupling_tau[zone_index][neighbor_index] = tau_s;
    s_fake_coupling_dead[zone_index][neighbor_index] = dead_time_s;
    return true;
}

esp_err_t uart_bridge_ext_run_on_flash_worker(void (*fn)(void *arg), void *arg)
{
    // Single-threaded host test -- just run it inline, matching the real
    // worker's "blocks the caller until the job has run" contract.
    fn(arg);
    return ESP_OK;
}

// No httpd fakes needed here any more -- adaptive_tune.c's HTTP surface
// moved to adaptive_tune_http.c (2026-09-01 split), which this file does not
// #include, so adaptive_tune.c itself now has no httpd_*/wifi_provision_
// http_* symbols left to satisfy.

#include "../drivers/adaptive_tune.c"

// ---------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------

// Quantizes to 0.1 degC -- history_pack()'s own resolution (profile_
// executor.c) and well above the MAX31856's real ~0.0078 degC LSB, so this
// is coarser-than-hardware quantization, not an idealized float. Every
// temperature fed into these tests goes through this -- see this repo's
// standing "unquantized synthetic input hides whole branches" trap.
static float q1(float c) { return roundf(c * 10.0f) / 10.0f; }

static void reset_module_state(void)
{
    memset(s_zones, 0, sizeof(s_zones));
    memset(s_fake_zone_cfg, 0, sizeof(s_fake_zone_cfg));
    memset(s_fake_coupling, 0, sizeof(s_fake_coupling));
    memset(s_fake_coupling_tau, 0, sizeof(s_fake_coupling_tau));
    memset(s_fake_coupling_dead, 0, sizeof(s_fake_coupling_dead));
    for (int i = 0; i < TEST_MAX_ZONES; i++) {
        s_fake_zone_cfg[i].tau_s = 200.0f;
        s_fake_zone_cfg[i].dead_time_s = 20.0f;
    }
    memset(s_joint_ring, 0, sizeof(s_joint_ring));
    s_joint_ring_count = 0;
    s_joint_ring_head = 0;
    s_joint_observations_lifetime = 0;
    memset(s_joint_last_duty, 0, sizeof(s_joint_last_duty));
    memset(s_joint_last_rise_c, 0, sizeof(s_joint_last_rise_c));
    memset(s_joint_last_valid, 0, sizeof(s_joint_last_valid));
    s_joint_dwell_row_committed = false;
}

// Ticks a single settled dwell into zone zi: `ticks` ticks of dt_s seconds
// each, temperature held at q1(target_c) the whole time (quantized, exactly
// as a real settled reading would round), duty held at `duty`.
static void feed_settled_dwell(uint8_t zi, float target_c, float ambient_c, float duty, int ticks, float dt_s)
{
    // One non-dwelling tick first, unconditionally, so this dwell always
    // starts its OWN fresh settle window -- otherwise a second call for the
    // same zone (simulating the next dwell in a multi-segment profile)
    // would still read as a continuation of whichever dwell came before it,
    // and recorded_this_dwell from that earlier dwell would silently
    // swallow every tick here. A real profile always ramps between two
    // dwells, which is exactly this reset edge on real hardware too.
    adaptive_tune_zone_tick(zi, q1(target_c), true, duty, false, ambient_c, dt_s);
    float c = q1(target_c);
    for (int i = 0; i < ticks; i++) {
        adaptive_tune_zone_tick(zi, c, true, duty, true, ambient_c, dt_s);
    }
}

// Ticks ALL MAX31856_CHANNEL_COUNT zones through one JOINT settled dwell --
// every zone's tick for a given loop iteration happens before the next
// iteration for any zone, so every zone settles at the same simulated
// instant (matching a real profile, where every enabled zone shares the
// same segment boundaries -- see s_joint_dwell_row_committed's own comment
// on why that assumption is what makes one dwell commit exactly one joint
// row). target_c/duty are per-zone arrays of length MAX31856_CHANNEL_COUNT.
static void feed_joint_settled_dwell(const float target_c[MAX31856_CHANNEL_COUNT], float ambient_c,
                                      const float duty[MAX31856_CHANNEL_COUNT], int ticks, float dt_s)
{
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        adaptive_tune_zone_tick(zi, q1(target_c[zi]), true, duty[zi], false, ambient_c, dt_s);
    }
    for (int i = 0; i < ticks; i++) {
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            adaptive_tune_zone_tick(zi, q1(target_c[zi]), true, duty[zi], true, ambient_c, dt_s);
        }
    }
}

// A dwell that never actually settles: temperature keeps climbing the whole
// window at a slope well above the settle floor.
static void feed_unsettled_dwell(uint8_t zi, float start_c, float ambient_c, float duty, int ticks, float dt_s,
                                  float climb_c_per_tick)
{
    adaptive_tune_zone_tick(zi, start_c, true, duty, false, ambient_c, dt_s); // fresh dwell window, see feed_settled_dwell()
    float c = start_c;
    for (int i = 0; i < ticks; i++) {
        c = q1(c + climb_c_per_tick);
        adaptive_tune_zone_tick(zi, c, true, duty, true, ambient_c, dt_s);
    }
}

static profile_firing_run_record_t make_clean_record(uint8_t profile_id, uint8_t zi, uint32_t samples)
{
    profile_firing_run_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.profile_id = profile_id;
    rec.zones[zi].active = true;
    rec.zones[zi].stats.sample_count = samples;
    rec.zones[zi].stats.excluded_sample_count = 0;
    return rec;
}

// 30s ticks, 7 ticks = 210s > ADAPTIVE_TUNE_SETTLE_MIN_S (180s).
#define DT_S 30.0f
#define SETTLE_TICKS 7

// ---------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------

static void test_unsettled_dwell_is_not_recorded(void)
{
    reset_module_state();
    s_zones[0].enabled = true;
    // Climbs 0.05 degC/tick over 30s ticks == 0.00167 C/s -- wait, that IS
    // below the floor. Use a climb clearly above SETTLE_SLOPE_FLOOR
    // (0.003 C/s): 0.2 degC/tick / 30s = 0.0067 C/s, more than 2x the floor.
    feed_unsettled_dwell(0, 100.0f, 22.0f, 0.5f, SETTLE_TICKS, DT_S, 0.2f);
    TEST_CHECK(s_zones[0].ring_count == 0, "a dwell that never stops drifting must record nothing");
}

static void test_settled_dwell_is_recorded(void)
{
    reset_module_state();
    s_zones[0].enabled = true;
    feed_settled_dwell(0, 100.0f, 22.0f, 0.5f, SETTLE_TICKS, DT_S);
    TEST_CHECK(s_zones[0].ring_count == 1, "a genuinely flat, settled dwell must record exactly one observation");
}

static void test_opt_in_default_off_records_nothing(void)
{
    reset_module_state();
    // Deliberately NOT setting s_zones[0].enabled -- struct-zero default,
    // same as adaptive_tune_init() would leave an unconfigured zone.
    TEST_CHECK(s_zones[0].enabled == false, "opt-in must default to OFF");
    feed_settled_dwell(0, 100.0f, 22.0f, 0.5f, SETTLE_TICKS, DT_S);
    TEST_CHECK(s_zones[0].ring_count == 0, "a settled dwell on a zone that never opted in must record nothing");

    profile_firing_run_record_t rec = make_clean_record(1, 0, 900);
    float k_before = s_fake_zone_cfg[0].k_dc;
    adaptive_tune_run_end(&rec, true);
    TEST_CHECK(s_fake_zone_cfg[0].k_dc == k_before, "run_end on an opted-out zone must never touch its model");
    TEST_CHECK(!s_zones[0].has_applied, "an opted-out zone must never report an applied refinement");
}

// ---------------------------------------------------------------------
// Public accessor tests -- these exercise EXACTLY the surface
// adaptive_tune_http.c's status/enable handlers call (adaptive_tune_get_
// enabled/set_enabled/get_status), not the internal s_zones struct
// directly, so they prove the accessor path itself, not just the module's
// internal state.
// ---------------------------------------------------------------------

static void test_default_off_for_every_zone(void)
{
    reset_module_state();
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        TEST_CHECK(adaptive_tune_get_enabled(zi) == false,
                   "every zone's opt-in must default to OFF, via the public getter");
    }
}

static void test_enable_one_zone_leaves_others_untouched(void)
{
    // Asymmetric fixture: enable ONLY zone 1, then assert zones 0 and 2
    // (neighbours on either side) stay off -- a mask off-by-one or a
    // transposed index would flip one of those two, not zone 1 itself.
    reset_module_state();
    nvs_test_clear();
    nvs_test_enable(true); // adaptive_tune_set_enabled()'s return value reflects whether the NVS save
                            // succeeded (see its own doc comment) -- the default-closed stub would make
                            // even a correct live-apply report false, so this test needs the real round trip.
    TEST_CHECK(adaptive_tune_set_enabled(1, true), "enabling zone 1 should report success");
    TEST_CHECK(adaptive_tune_get_enabled(0) == false, "zone 0 must stay off when only zone 1 is enabled");
    TEST_CHECK(adaptive_tune_get_enabled(1) == true, "zone 1 must be on");
    TEST_CHECK(adaptive_tune_get_enabled(2) == false, "zone 2 must stay off when only zone 1 is enabled");
    nvs_test_enable(false);
    nvs_test_clear();
}

static void test_enable_round_trips_through_persistence(void)
{
    // Real NVS round trip via stubs/nvs.h's opt-in stub store: enable zone 1
    // only, then reload the module exactly as a reboot would
    // (adaptive_tune_init() re-reads the en_mask byte from "flash") and
    // check the reloaded state matches what was actually written, not what
    // this test just set in RAM.
    reset_module_state();
    nvs_test_clear();
    nvs_test_enable(true);

    TEST_CHECK(adaptive_tune_set_enabled(1, true), "setting zone 1's opt-in should report success");

    memset(s_zones, 0, sizeof(s_zones)); // simulate a reboot: RAM state gone
    adaptive_tune_init();                // reload from the (stubbed) NVS namespace

    TEST_CHECK(adaptive_tune_get_enabled(0) == false, "zone 0 must reload as off");
    TEST_CHECK(adaptive_tune_get_enabled(1) == true, "zone 1 must reload as on -- this is the persisted value");
    TEST_CHECK(adaptive_tune_get_enabled(2) == false, "zone 2 must reload as off");

    nvs_test_enable(false); // leave the shared stub state as every other test in this binary expects
    nvs_test_clear();
}

static void test_status_reports_engine_held_fields_not_test_written_values(void)
{
    // Drives a real refinement through adaptive_tune_run_end() (same as
    // test_refinement_improves_gain_estimate_on_known_plant()) and then reads
    // it back ONLY through adaptive_tune_get_status() -- the same accessor
    // adaptive_tune_http.c's status handler calls. Every field checked here
    // comes from the engine's own bookkeeping (z->ring_count, z->has_applied,
    // z->prior_k_dc, z->applied_k_dc, ...), never a value this test wrote
    // into the status struct itself.
    reset_module_state();
    s_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f; // prior -- true gain 15
    const float true_k = 15.0f, ambient = 22.3f;
    const float duties[4] = {0.20f, 0.50f, 0.80f, 0.35f};
    for (int i = 0; i < 4; i++) {
        feed_settled_dwell(1, ambient + true_k * duties[i], ambient, duties[i], SETTLE_TICKS, DT_S);
    }

    adaptive_tune_zone_status_t before;
    adaptive_tune_get_status(1, &before);
    TEST_CHECK(before.enabled == true, "status must reflect this zone's opt-in");
    TEST_CHECK(before.ring_count == 4, "status must report the 4 dwell observations collected so far");
    TEST_CHECK(before.has_applied == false, "no refinement has run yet -- has_applied must still be false");

    profile_firing_run_record_t rec = make_clean_record(7, 1, 900);
    adaptive_tune_run_end(&rec, true);

    adaptive_tune_zone_status_t after;
    adaptive_tune_get_status(1, &after);
    TEST_CHECK(after.has_applied == true, "status must report the refinement as applied");
    TEST_CHECK_NEAR(after.prior_k_dc, 10.0f, 1e-4, "status's prior_k_dc must be the model K_dc before this apply");
    TEST_CHECK(after.applied_k_dc > after.prior_k_dc,
               "status's applied_k_dc must show the new (higher, toward true gain 15) K_dc");
    TEST_CHECK(after.last_delta_pct > 0.0f, "status's last_delta_pct must be positive (K_dc moved up)");
    TEST_CHECK(after.last_applied_profile_id == 7, "status must report which profile produced the applied change");
    TEST_CHECK(after.last_refusal_reason[0] == '\0', "a clean apply must leave the refusal reason empty");
}

static void test_min_observations_guard_rejects_too_few(void)
{
    reset_module_state();
    s_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;
    // Only 3 dwells (ADAPTIVE_TUNE_MIN_OBSERVATIONS is 4), otherwise a
    // perfectly good, well-spread, on-model fit.
    feed_settled_dwell(1, 25.0f, 22.0f, 0.2f, SETTLE_TICKS, DT_S);   // rise 3, u 0.2
    feed_settled_dwell(1, 27.0f, 22.0f, 0.5f, SETTLE_TICKS, DT_S);   // rise 5, u 0.5
    feed_settled_dwell(1, 30.0f, 22.0f, 0.8f, SETTLE_TICKS, DT_S);   // rise 8, u 0.8
    TEST_CHECK(s_zones[1].ring_count == 3, "setup: exactly 3 observations queued");

    profile_firing_run_record_t rec = make_clean_record(2, 1, 900);
    adaptive_tune_run_end(&rec, true);
    TEST_CHECK(s_fake_zone_cfg[1].k_dc == 10.0f, "fewer than the minimum observation count must refuse the update");
    TEST_CHECK(strstr(s_zones[1].last_refusal_reason, "observations") != NULL,
               "refusal reason should name the observation-count shortfall");
}

static void test_duty_spread_guard_rejects_clustered_observations(void)
{
    reset_module_state();
    s_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;
    // 4 observations, all essentially the same duty (spread << 0.05).
    for (int i = 0; i < 4; i++) {
        feed_settled_dwell(1, 25.0f, 22.0f, 0.50f, SETTLE_TICKS, DT_S);
    }
    TEST_CHECK(s_zones[1].ring_count == 4, "setup: 4 observations queued");

    profile_firing_run_record_t rec = make_clean_record(3, 1, 900);
    adaptive_tune_run_end(&rec, true);
    TEST_CHECK(s_fake_zone_cfg[1].k_dc == 10.0f, "clustered duty values (no spread) must refuse the update");
    TEST_CHECK(strstr(s_zones[1].last_refusal_reason, "clustered") != NULL,
               "refusal reason should name the duty-spread shortfall");
}

static void test_implausible_jump_guard_rejects_far_off_fit(void)
{
    reset_module_state();
    s_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f; // prior
    // True gain here is 80 (8x the prior) -- ADAPTIVE_TUNE_MAX_JUMP_RATIO
    // is 5x, so this must be refused as implausible, not blended in.
    feed_settled_dwell(1, 22.0f + 80.0f * 0.2f, 22.0f, 0.2f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 22.0f + 80.0f * 0.4f, 22.0f, 0.4f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 22.0f + 80.0f * 0.6f, 22.0f, 0.6f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 22.0f + 80.0f * 0.8f, 22.0f, 0.8f, SETTLE_TICKS, DT_S);

    profile_firing_run_record_t rec = make_clean_record(4, 1, 900);
    adaptive_tune_run_end(&rec, true);
    TEST_CHECK(s_fake_zone_cfg[1].k_dc == 10.0f, "a fit >5x the prior model must be refused as implausible");
    TEST_CHECK(strstr(s_zones[1].last_refusal_reason, "implausible") != NULL,
               "refusal reason should say the fit was implausible");
}

static void test_dirty_run_is_never_training_data(void)
{
    reset_module_state();
    s_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;
    // H1 fix: otherwise-perfect, well-spread observations, but with true K ==
    // 12 (NOT == prior 10) -- deliberately a MATERIAL move (blend = 10 +
    // 0.15*(12-10) = 10.3, a 3% move, well above ADAPTIVE_TUNE_MIN_MATERIAL_
    // MOVE_FRAC's 0.5% floor). The original fixture here used true K == prior
    // K == 10.0 exactly, so the blend was a zero move and F1's material-
    // change floor refused it for the WRONG reason -- masking the excluded-
    // fraction guard this test exists to cover (see the review's H1 finding:
    // with the excluded guard disabled entirely, that pre-existing fixture
    // still passed, 134/134 GREEN). With a genuinely material blend, only
    // the "not clean" / "excluded fraction" guards below can be the reason
    // has_applied stays false.
    feed_settled_dwell(1, 22.0f + 12.0f * 0.2f, 22.0f, 0.2f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 22.0f + 12.0f * 0.5f, 22.0f, 0.5f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 22.0f + 12.0f * 0.8f, 22.0f, 0.8f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 22.0f + 12.0f * 0.35f, 22.0f, 0.35f, SETTLE_TICKS, DT_S);

    profile_firing_run_record_t rec = make_clean_record(5, 1, 900);
    adaptive_tune_run_end(&rec, /*clean=*/false); // faulted or operator-stopped
    TEST_CHECK(!s_zones[1].has_applied, "a faulted/stopped-early run must never be used as training data");
    TEST_CHECK(strstr(s_zones[1].last_refusal_reason, "faulted or stopped early") != NULL,
               "refusal reason should say why (not-clean run)");

    // Same observations, this time reported clean but with heavy sensor
    // dropout (>5% excluded) -- must also refuse.
    profile_firing_run_record_t rec2 = make_clean_record(6, 1, 900);
    rec2.zones[1].stats.excluded_sample_count = 100; // 100/(900+100) = 10% > 5%
    adaptive_tune_run_end(&rec2, /*clean=*/true);
    TEST_CHECK(!s_zones[1].has_applied, "heavy excluded-sample fraction must also refuse to learn");
    TEST_CHECK(strstr(s_zones[1].last_refusal_reason, "excluded") != NULL,
               "H1: refusal reason should name the excluded-sample-fraction guard, not the material-change floor "
               "-- proves this fixture's blend was genuinely material and the excluded guard is what's under test");
}

static void test_refinement_improves_gain_estimate_on_known_plant(void)
{
    reset_module_state();
    // Asymmetric fixture, deliberately DIFFERENT numbers on two zones so a
    // transposed zone index would fail this test: zone 1's prior undershoots
    // its true gain, zone 3's prior overshoots ITS true gain, and the two
    // zones' numbers are all distinct from each other.
    s_zones[1].enabled = true;
    s_zones[2].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;   // prior, zone 1 -- true gain 15
    s_fake_zone_cfg[2].k_dc = 30.0f;   // prior, zone 3 -- true gain 20
    const float true_k1 = 15.0f, true_k3 = 20.0f, ambient = 22.3f;
    const float duties[4] = {0.20f, 0.50f, 0.80f, 0.35f};
    for (int i = 0; i < 4; i++) {
        feed_settled_dwell(1, ambient + true_k1 * duties[i], ambient, duties[i], SETTLE_TICKS, DT_S);
        feed_settled_dwell(2, ambient + true_k3 * duties[i], ambient, duties[i], SETTLE_TICKS, DT_S);
    }

    profile_firing_run_record_t rec = make_clean_record(9, 1, 900);
    rec.zones[2].active = true;
    rec.zones[2].stats.sample_count = 900;
    adaptive_tune_run_end(&rec, true);

    TEST_CHECK(s_zones[1].has_applied, "zone 1 should have applied a refinement");
    TEST_CHECK(s_zones[2].has_applied, "zone 3 should have applied a refinement");

    // Blend: prior + ALPHA*(fit-prior), fit==true_k here (exact, no noise
    // beyond 0.1 degC quantization). Zone 1 moves UP toward 15, zone 3 moves
    // DOWN toward 20 -- opposite directions, so a swapped index is obvious.
    float expect_k1 = 10.0f + 0.15f * (true_k1 - 10.0f);   // 10.75
    float expect_k3 = 30.0f + 0.15f * (true_k3 - 30.0f);   // 28.5
    TEST_CHECK_NEAR(s_fake_zone_cfg[1].k_dc, expect_k1, 0.05, "zone 1's refined K_dc should move toward its OWN true gain");
    TEST_CHECK_NEAR(s_fake_zone_cfg[2].k_dc, expect_k3, 0.05, "zone 3's refined K_dc should move toward its OWN true gain");

    // The refined estimate must be strictly CLOSER to the true gain than
    // the prior was -- the actual "improves the estimate" property, not
    // just "changed to some new number".
    TEST_CHECK(fabsf(s_fake_zone_cfg[1].k_dc - true_k1) < fabsf(10.0f - true_k1),
               "zone 1's refined estimate must be closer to the true gain than the prior was");
    TEST_CHECK(fabsf(s_fake_zone_cfg[2].k_dc - true_k3) < fabsf(30.0f - true_k3),
               "zone 3's refined estimate must be closer to the true gain than the prior was");

    // PID gains must also have moved (SIMC recomputed from the new K) --
    // not just the model triple.
    TEST_CHECK(s_fake_zone_cfg[1].kp > 0.0f, "zone 1 should have gotten nonzero recomputed PID gains");
}

static void test_per_run_move_is_bounded_even_with_many_dwells(void)
{
    reset_module_state();
    s_zones[2].enabled = true;
    s_fake_zone_cfg[2].k_dc = 10.0f;
    // true gain 45 is 4.5x the prior -- inside ADAPTIVE_TUNE_MAX_JUMP_RATIO
    // (5x, so the implausible-jump guard does not refuse it) but far enough
    // that ALPHA*(fit-prior) = 0.15*35 = 5.25 would blow past the per-run
    // cap (10*0.20 = 2.0) if nothing clamped it -- this scenario genuinely
    // exercises the cap, unlike a small move that never reaches it.
    const float true_k = 45.0f, ambient = 21.7f;
    const float duties[4] = {0.15f, 0.45f, 0.75f, 0.30f};
    for (int i = 0; i < 4; i++) {
        feed_settled_dwell(2, ambient + true_k * duties[i], ambient, duties[i], SETTLE_TICKS, DT_S);
    }
    profile_firing_run_record_t rec = make_clean_record(10, 2, 900);
    adaptive_tune_run_end(&rec, true);
    float max_move = 10.0f * ADAPTIVE_TUNE_MAX_FRACTIONAL_MOVE; // 2.0
    TEST_CHECK(s_zones[2].has_applied, "setup: the 4.5x fit must pass the jump-ratio guard and apply");
    TEST_CHECK_NEAR(s_fake_zone_cfg[2].k_dc, 10.0f + max_move, 0.05,
                     "a single run's applied move must be clamped exactly at the per-run fractional cap");
    TEST_CHECK(s_fake_zone_cfg[2].k_dc <= 10.0f + max_move + 1e-3f,
               "a single run's applied move must never exceed the per-run fractional cap");
}

// ---------------------------------------------------------------------
// Full coupled identification -- adaptive_tune_coupled_fit() pure-math tests.
// ---------------------------------------------------------------------

// The bench-measured reference matrix from PID_EXPANSION_PLAN.md section 2,
// converted from its WIRE form [stepped][affected] (as quoted there) to the
// STORAGE convention adaptive_tune_coupled_fit() and zones_config_get/set_
// coupling() both use: C[affected][stepped]. Deliberately asymmetric
// (C[0][1]=26.61 != C[1][0]=15.78, etc.) so a transposed solver recovers the
// WRONG numbers, not just "numbers" -- see test_coupled_fit_recovers_known_
// asymmetric_matrix() below.
//   wire[stepped=0] = 39.25/15.78/9.70  -> C[0][0]=39.25 C[1][0]=15.78 C[2][0]=9.70
//   wire[stepped=1] = 26.61/31.97/11.38 -> C[0][1]=26.61 C[1][1]=31.97 C[2][1]=11.38
//   wire[stepped=2] = 20.73/21.09/31.68 -> C[0][2]=20.73 C[1][2]=21.09 C[2][2]=31.68
static const float k_ref_C[3][3] = {
    {39.25f, 26.61f, 20.73f},
    {15.78f, 31.97f, 21.09f},
    {9.70f, 11.38f, 31.68f},
};

static void test_coupled_fit_refuses_underdetermined_observation_set(void)
{
    // n=3 unknowns per row, ADAPTIVE_TUNE_COUPLED_OBS_MARGIN=2 -> minimum is
    // 5 joint observations. Feed exactly 4 (n+1) -- one short -- with
    // otherwise perfectly well-conditioned, exactly-on-model data (so the
    // ONLY thing that can refuse this is the observation-count guard, not a
    // conditioning problem).
    float duty[4][MAX31856_CHANNEL_COUNT] = {
        {0.20f, 0.50f, 0.80f}, {0.50f, 0.80f, 0.20f}, {0.80f, 0.20f, 0.50f}, {0.35f, 0.65f, 0.15f}};
    float rise[4][MAX31856_CHANNEL_COUNT];
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < 3; i++) {
            float s = 0.0f;
            for (int j = 0; j < 3; j++) s += k_ref_C[i][j] * duty[k][j];
            rise[k][i] = q1(s);
        }
    }
    float out_C[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT];
    adaptive_tune_coupled_result_t r = adaptive_tune_coupled_fit(duty, rise, 4, 3, out_C);
    TEST_CHECK(r == ADAPTIVE_TUNE_COUPLED_TOO_FEW_OBSERVATIONS,
               "4 joint observations (n+1) for a 3-unknown-per-row system must refuse as underdetermined");

    // Sanity: the SAME data with one more observation (n+2 == 5, the
    // documented margin) must NOT refuse for this reason -- proves the guard
    // is checking the count, not silently failing on this data for some
    // other reason.
    float duty5[5][MAX31856_CHANNEL_COUNT];
    memcpy(duty5, duty, sizeof(duty));
    duty5[4][0] = 0.65f; duty5[4][1] = 0.15f; duty5[4][2] = 0.65f;
    float rise5[5][MAX31856_CHANNEL_COUNT];
    memcpy(rise5, rise, sizeof(rise));
    for (int i = 0; i < 3; i++) {
        float s = 0.0f;
        for (int j = 0; j < 3; j++) s += k_ref_C[i][j] * duty5[4][j];
        rise5[4][i] = q1(s);
    }
    adaptive_tune_coupled_result_t r2 = adaptive_tune_coupled_fit(duty5, rise5, 5, 3, out_C);
    TEST_CHECK(r2 == ADAPTIVE_TUNE_COUPLED_OK, "setup: 5 observations (the documented margin) must be accepted");
}

static void test_coupled_fit_refuses_ill_conditioned_observations(void)
{
    // 6 observations, but zone 0's and zone 1's duty are IDENTICAL on every
    // one -- the design matrix (duty^T * duty) is then singular (columns 0
    // and 1 are linearly dependent), however many observations are piled
    // on. Well above the observation-count floor, so this isolates the
    // conditioning guard specifically.
    float duty[6][MAX31856_CHANNEL_COUNT];
    float rise[6][MAX31856_CHANNEL_COUNT];
    float xs[6] = {0.20f, 0.35f, 0.50f, 0.65f, 0.80f, 0.30f};
    float ys[6] = {0.10f, 0.40f, 0.25f, 0.55f, 0.15f, 0.60f};
    for (int k = 0; k < 6; k++) {
        duty[k][0] = xs[k];
        duty[k][1] = xs[k]; // == column 0, always
        duty[k][2] = ys[k];
        for (int i = 0; i < 3; i++) {
            float s = 0.0f;
            for (int j = 0; j < 3; j++) s += k_ref_C[i][j] * duty[k][j];
            rise[k][i] = q1(s);
        }
    }
    float out_C[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT];
    adaptive_tune_coupled_result_t r = adaptive_tune_coupled_fit(duty, rise, 6, 3, out_C);
    TEST_CHECK(r == ADAPTIVE_TUNE_COUPLED_ILL_CONDITIONED,
               "two identical duty columns (rank-deficient design matrix) must refuse as ill-conditioned, "
               "not silently return a numeric answer");
}

static void test_coupled_fit_recovers_known_asymmetric_matrix(void)
{
    // 6 joint observations, duty combinations spanning a wide, varied range
    // (well above the 5-observation margin, well-conditioned), rises
    // generated from k_ref_C and quantized to 0.1 degC -- realistically
    // quantized synthetic input, not idealized exact floats (this repo's own
    // "unquantized synthetic input hides whole branches" trap).
    float duty[6][MAX31856_CHANNEL_COUNT] = {
        {0.20f, 0.50f, 0.80f}, {0.50f, 0.80f, 0.20f}, {0.80f, 0.20f, 0.50f},
        {0.35f, 0.65f, 0.15f}, {0.65f, 0.15f, 0.65f}, {0.15f, 0.35f, 0.35f}};
    float rise[6][MAX31856_CHANNEL_COUNT];
    for (int k = 0; k < 6; k++) {
        for (int i = 0; i < 3; i++) {
            float s = 0.0f;
            for (int j = 0; j < 3; j++) s += k_ref_C[i][j] * duty[k][j];
            rise[k][i] = q1(s);
        }
    }
    float out_C[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT];
    adaptive_tune_coupled_result_t r = adaptive_tune_coupled_fit(duty, rise, 6, 3, out_C);
    TEST_CHECK(r == ADAPTIVE_TUNE_COUPLED_OK, "setup: 6 well-spread observations on a well-conditioned matrix must solve");

    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            TEST_CHECK_NEAR(out_C[i][j], k_ref_C[i][j], 0.6,
                             "recovered coupling_coeff[affected][stepped] cell must match the known matrix "
                             "within quantization tolerance");
        }
    }
    // The orientation check that actually catches a transpose bug: C[0][1]
    // (26.61) and C[1][0] (15.78) are far enough apart that a transposed
    // solver would fail BOTH of the checks above by more than 10x this
    // tolerance, not pass by coincidence.
    TEST_CHECK(fabsf(out_C[0][1] - out_C[1][0]) > 5.0f,
               "setup: C[0][1] and C[1][0] must be genuinely different values in the fixture, or a "
               "transpose bug could not be distinguished from a correct solve");
    TEST_CHECK_NEAR(out_C[0][1], 26.61f, 0.6, "C[0][1] (affected=0 responding to stepped=1) must NOT read as C[1][0]'s value");
    TEST_CHECK_NEAR(out_C[1][0], 15.78f, 0.6, "C[1][0] (affected=1 responding to stepped=0) must NOT read as C[0][1]'s value");
}

// ---------------------------------------------------------------------
// Integral (Ki) diagnosis -- adaptive_tune_diagnose_ki() pure-math tests.
// 10s ticks (DT_KI), matching the hardware logging cadence this repo's
// vacuity-trap note calls out; every temperature sample below goes through
// q1() (0.1 degC quantization), same convention as the rest of this file.
// ---------------------------------------------------------------------
#define DT_KI 10.0f

static void test_ki_diagnose_insufficient_below_min_samples(void)
{
    float a[8], d[8];
    for (int i = 0; i < 8; i++) { a[i] = q1(100.0f); d[i] = 0.4f; }
    adaptive_tune_ki_diag_t diag;
    TEST_CHECK(adaptive_tune_diagnose_ki(a, d, 8, DT_KI, 0.0f, 0.0f, &diag), "call must return true");
    TEST_CHECK(diag.verdict == ADAPTIVE_TUNE_KI_INSUFFICIENT, "8 samples (below the 12-sample minimum) must be INSUFFICIENT");
}

static void test_ki_diagnose_steady_offset_flags_small_ki(void)
{
    // Flat, quantized temperature (no oscillation, no drift), duty varying
    // in the middle of its range (nowhere near a rail) -- and a dwell error
    // figure the profile executor would report for a steadily hot-running
    // zone. This must read as "Ki too small", not floored (see the paired
    // floored test below, which is IDENTICAL except for the duty pattern).
    float a[16], d[16];
    for (int i = 0; i < 16; i++) {
        a[i] = q1(101.3f);
        d[i] = 0.40f + ((i % 2) ? 0.03f : -0.03f); // varying, centered ~0.40, real duty variance
    }
    adaptive_tune_ki_diag_t diag;
    TEST_CHECK(adaptive_tune_diagnose_ki(a, d, 16, DT_KI, /*dwell_err_mean_c=*/0.45f, /*dwell_err_max_c=*/0.50f, &diag),
               "call must return true");
    TEST_CHECK(diag.verdict == ADAPTIVE_TUNE_KI_OFFSET_TOO_SMALL,
               "a steady 0.45C dwell error with a non-flat, non-rail duty must diagnose as Ki too small");
    TEST_CHECK(diag.ki_correction_pct > 0.0f, "the OFFSET verdict must suggest INCREASING Ki (positive correction)");
}

static void test_ki_diagnose_floored_not_misdiagnosed_as_small_ki(void)
{
    // Same flat temperature and the SAME 0.45C/0.50C dwell error figures as
    // the OFFSET test above -- the ONLY difference is duty: pinned low and
    // essentially not moving, the -ff_hold floor's signature (pid.c). This
    // must NOT reuse the OFFSET_TOO_SMALL verdict, and must suggest no
    // correction at all.
    float a[16], d[16];
    for (int i = 0; i < 16; i++) {
        a[i] = q1(101.3f);
        d[i] = 0.02f; // pinned near the 0 rail, essentially zero variance
    }
    adaptive_tune_ki_diag_t diag;
    TEST_CHECK(adaptive_tune_diagnose_ki(a, d, 16, DT_KI, 0.45f, 0.50f, &diag), "call must return true");
    TEST_CHECK(diag.verdict == ADAPTIVE_TUNE_KI_FLOORED,
               "identical error figures but duty pinned near a rail must diagnose as FLOORED, not small Ki");
    TEST_CHECK(diag.ki_correction_pct == 0.0f, "a FLOORED verdict must never suggest a Ki correction");
}

// D2 (was test_ki_diagnose_drift_flags_large_ki): a monotonic, one-directional
// drift is NOT oscillation evidence (it crosses its own window mean only
// once) and this function is never handed the setpoint, so it cannot know
// whether the drift is approaching or receding from target. The OLD code
// called fabsf(drift) > threshold "OSCILLATING" / Ki-too-large regardless --
// backwards for the classic "still slowly settling" case, where Ki is
// actually too SMALL. This is that same monotonic-drift fixture (unchanged),
// now asserting the corrected behavior: with no dwell-error evidence handed
// in (0.0f/0.0f, as before), and no multi-crossing oscillation, the correct
// verdict is OK (no unjustified correction) -- not a wrong-signed
// "decrease Ki".
static void test_ki_diagnose_monotonic_drift_is_not_misread_as_oscillation(void)
{
    float a[18], d[18];
    for (int i = 0; i < 18; i++) {
        a[i] = q1(100.0f + 0.06f * (float)i); // +1.02C total drift over the window
        d[i] = 0.40f;
    }
    adaptive_tune_ki_diag_t diag;
    TEST_CHECK(adaptive_tune_diagnose_ki(a, d, 18, DT_KI, 0.0f, 0.0f, &diag), "call must return true");
    TEST_CHECK(diag.verdict != ADAPTIVE_TUNE_KI_OSCILLATING && diag.verdict != ADAPTIVE_TUNE_KI_LIMIT_CYCLE,
               "a one-directional (single-crossing) drift must never be read as oscillation -- only "
               "crossing/regularity evidence may produce that verdict");
    TEST_CHECK(diag.verdict == ADAPTIVE_TUNE_KI_OK, "with no dwell-error evidence supplied, a monotonic drift alone "
                                                      "must diagnose OK, not guess a (possibly wrong-signed) correction");
}

// D2, continued: the realistic version of the same scenario -- a zone still
// slowly converging on setpoint has EXACTLY this monotonic-drift shape AND
// a steady non-trivial dwell error (the profile executor's own dwell_err_
// mean/max_c). This is "the classic too-small-Ki signature" the review
// names: it must diagnose OFFSET_TOO_SMALL (Ki should INCREASE), never
// OSCILLATING/decrease -- proving the fix is not just "stop guessing" but
// "let the correctly-signed offset evidence drive the verdict instead".
static void test_ki_diagnose_monotonic_drift_with_steady_offset_flags_small_ki(void)
{
    float a[18], d[18];
    for (int i = 0; i < 18; i++) {
        a[i] = q1(100.0f + 0.06f * (float)i);
        d[i] = 0.40f + ((i % 2) ? 0.03f : -0.03f); // varying, not floored -- see the floored-not-misdiagnosed test
    }
    adaptive_tune_ki_diag_t diag;
    TEST_CHECK(adaptive_tune_diagnose_ki(a, d, 18, DT_KI, /*dwell_err_mean_c=*/0.45f, /*dwell_err_max_c=*/0.50f, &diag),
               "call must return true");
    TEST_CHECK(diag.verdict == ADAPTIVE_TUNE_KI_OFFSET_TOO_SMALL,
               "a monotonic approach-to-setpoint drift with a steady non-floored dwell error must diagnose as "
               "Ki too small, not oscillation");
    TEST_CHECK(diag.ki_correction_pct > 0.0f, "the corrected verdict must suggest INCREASING Ki, not decreasing it");
}

// D2: floored must win even when the SAME window would otherwise satisfy
// the (now-removed) drift heuristic's threshold -- proves floored is
// checked unconditionally, ahead of any other classification, not just
// ahead of the old drift branch specifically.
static void test_ki_diagnose_floored_wins_even_with_monotonic_drift(void)
{
    float a[18], d[18];
    for (int i = 0; i < 18; i++) {
        a[i] = q1(100.0f + 0.06f * (float)i); // same drifting temperature as the tests above
        d[i] = 0.02f;                          // pinned near the 0 rail, essentially zero variance
    }
    adaptive_tune_ki_diag_t diag;
    TEST_CHECK(adaptive_tune_diagnose_ki(a, d, 18, DT_KI, 0.45f, 0.50f, &diag), "call must return true");
    TEST_CHECK(diag.verdict == ADAPTIVE_TUNE_KI_FLOORED,
               "a floored duty trace must diagnose FLOORED even though the temperature trace is drifting");
    TEST_CHECK(diag.ki_correction_pct == 0.0f, "a FLOORED verdict must never suggest a Ki correction");
}

// D7: the floored conjunction (dvar < FLOOR_VARIANCE) && (near a rail) has
// two independent clauses. The existing floored/offset fixture pair only
// ever varies BOTH clauses together (flat+railed vs varying+mid-range), so
// deleting either clause from the guard would still pass every existing
// test. These two fixtures hold one clause floored-shaped and flip the
// other, so each clause is independently load-bearing.
static void test_ki_diagnose_flat_duty_mid_range_is_not_floored(void)
{
    // Flat (near-zero variance) duty, same as the floored fixture -- but
    // parked in the MIDDLE of its range, nowhere near either rail. Must NOT
    // read as floored: a mid-range flat duty is not the -ff_hold signature.
    float a[16], d[16];
    for (int i = 0; i < 16; i++) {
        a[i] = q1(101.3f);
        d[i] = 0.50f; // flat, but mid-range -- not near 0.0 or 1.0
    }
    adaptive_tune_ki_diag_t diag;
    TEST_CHECK(adaptive_tune_diagnose_ki(a, d, 16, DT_KI, 0.45f, 0.50f, &diag), "call must return true");
    TEST_CHECK(diag.verdict != ADAPTIVE_TUNE_KI_FLOORED,
               "a flat but MID-RANGE duty (not near a rail) must not diagnose as floored");
    TEST_CHECK(diag.verdict == ADAPTIVE_TUNE_KI_OFFSET_TOO_SMALL,
               "with a steady offset and no rail evidence, the correct verdict is Ki too small");
}

static void test_ki_diagnose_near_rail_but_varying_is_not_floored(void)
{
    // Duty parked near the 0 rail ON AVERAGE, but genuinely moving
    // (variance well above the floor threshold) -- the opposite flip: rail
    // clause true, variance clause false. Must NOT read as floored either.
    float a[16], d[16];
    for (int i = 0; i < 16; i++) {
        a[i] = q1(101.3f);
        d[i] = 0.01f + ((i % 2) ? 0.06f : 0.0f); // mean ~0.04 (near the 0 rail), but swinging, real variance
    }
    adaptive_tune_ki_diag_t diag;
    TEST_CHECK(adaptive_tune_diagnose_ki(a, d, 16, DT_KI, 0.45f, 0.50f, &diag), "call must return true");
    TEST_CHECK(diag.verdict != ADAPTIVE_TUNE_KI_FLOORED,
               "duty near a rail ON AVERAGE but genuinely varying must not diagnose as floored");
    TEST_CHECK(diag.verdict == ADAPTIVE_TUNE_KI_OFFSET_TOO_SMALL,
               "with a steady offset and a varying (not flat) duty, the correct verdict is Ki too small");
}

static void test_ki_diagnose_limit_cycle_yields_ku_tu(void)
{
    // A clean, regular oscillation: period 8 samples (3 full cycles across
    // 24 samples), amplitude 0.3C (well above the 0.05C noise floor),
    // quantized to 0.1C. Duty oscillates with the same period.
    float a[24], d[24];
    const float pi = 3.14159265358979f;
    for (int i = 0; i < 24; i++) {
        a[i] = q1(100.0f + 0.30f * sinf(2.0f * pi * (float)i / 8.0f));
        d[i] = 0.40f + 0.10f * sinf(2.0f * pi * (float)i / 8.0f);
    }
    adaptive_tune_ki_diag_t diag;
    TEST_CHECK(adaptive_tune_diagnose_ki(a, d, 24, DT_KI, 0.0f, 0.0f, &diag), "call must return true");
    TEST_CHECK(diag.verdict == ADAPTIVE_TUNE_KI_LIMIT_CYCLE, "a clean, regular 8-sample-period oscillation must diagnose as a limit cycle");
    TEST_CHECK(diag.zero_crossings >= 4, "setup: the fixture must actually cross its own mean at least 4 times");
    TEST_CHECK(diag.ku_estimate > 0.0f, "a limit cycle must hand over a positive Ku estimate");
    TEST_CHECK(diag.tu_estimate_s > 0.0f, "a limit cycle must hand over a positive Tu estimate");
    TEST_CHECK(diag.ki_correction_pct < 0.0f, "a limit cycle must suggest DECREASING Ki");
}

// F5: ADAPTIVE_TUNE_KI_OSCILLATING is reachable but previously had NO test
// asserting it is ever actually produced (only a `!=` check elsewhere).
// Two different periods spliced together (fast then slow) give plenty of
// crossings but an irregular gap spacing -- multi-crossing, but not a clean
// limit cycle.
static void test_ki_diagnose_irregular_hunting_yields_oscillating(void)
{
    float a[24], d[24];
    const float pi = 3.14159265358979f;
    for (int i = 0; i < 24; i++) {
        float t = (float)i;
        float val = (i < 12) ? (100.0f + 0.30f * sinf(2.0f * pi * t / 4.0f))
                              : (100.0f + 0.30f * sinf(2.0f * pi * (t - 12.0f) / 16.0f));
        a[i] = q1(val);
        d[i] = 0.40f;
    }
    adaptive_tune_ki_diag_t diag;
    TEST_CHECK(adaptive_tune_diagnose_ki(a, d, 24, DT_KI, 0.0f, 0.0f, &diag), "call must return true");
    TEST_CHECK(diag.zero_crossings >= ADAPTIVE_TUNE_KI_MIN_CROSSINGS,
               "setup: the fixture must actually cross its own mean at least 4 times");
    TEST_CHECK(diag.verdict == ADAPTIVE_TUNE_KI_OSCILLATING,
               "an irregular (non-regular-spacing) but multi-crossing oscillation must actually produce the "
               "OSCILLATING verdict -- not just avoid OK/LIMIT_CYCLE, which a `!=` check alone cannot prove");
    TEST_CHECK(diag.ki_correction_pct < 0.0f, "an OSCILLATING verdict must suggest DECREASING Ki, like LIMIT_CYCLE");
}

static void test_ki_diagnose_ok_when_tracking_cleanly(void)
{
    float a[16], d[16];
    for (int i = 0; i < 16; i++) { a[i] = q1(100.0f); d[i] = 0.40f; }
    adaptive_tune_ki_diag_t diag;
    TEST_CHECK(adaptive_tune_diagnose_ki(a, d, 16, DT_KI, /*dwell_err_mean_c=*/0.05f, /*dwell_err_max_c=*/0.08f, &diag),
               "call must return true");
    TEST_CHECK(diag.verdict == ADAPTIVE_TUNE_KI_OK, "flat trace, tiny dwell error, must diagnose OK (no correction)");
    TEST_CHECK(diag.ki_correction_pct == 0.0f, "an OK verdict must suggest no correction");
}

// ---------------------------------------------------------------------
// D1: a coupling cell must converge to truth across repeated runs from a
// 0.0 prior, not freeze partway. MUST FAIL on the pre-D1-fix code (that red
// was captured before applying the fix).
// ---------------------------------------------------------------------
static void test_coupling_cell_converges_from_zero_prior_over_repeated_runs(void)
{
    reset_module_state();
    s_zones[0].enabled = true;
    s_fake_zone_cfg[0].k_dc = 1.0f; // irrelevant to this test -- just needs to be nonzero/positive
    const float ambient = 20.0f;
    const float duty_pts[5][MAX31856_CHANNEL_COUNT] = {
        {0.20f, 0.50f, 0.80f}, {0.50f, 0.80f, 0.20f}, {0.80f, 0.20f, 0.50f},
        {0.35f, 0.65f, 0.15f}, {0.65f, 0.15f, 0.65f}};
    for (int k = 0; k < 5; k++) {
        float target[MAX31856_CHANNEL_COUNT];
        for (int i = 0; i < 3; i++) {
            float rise = 0.0f;
            for (int j = 0; j < 3; j++) rise += k_ref_C[i][j] * duty_pts[k][j];
            target[i] = ambient + rise;
        }
        feed_joint_settled_dwell(target, ambient, duty_pts[k], SETTLE_TICKS, DT_S);
    }
    TEST_CHECK(s_joint_ring_count == 5, "setup: 5 distinct joint dwells queued");

    // 30 runs against the SAME fixed evidence -- a stand-in for 30 firings
    // that all measured the same true coupling, exercising exactly the
    // iterated-blend convergence path the review calls out.
    for (int run = 0; run < 30; run++) {
        profile_firing_run_record_t rec = make_clean_record(20, 0, 900);
        adaptive_tune_run_end(&rec, true);
    }

    TEST_CHECK_NEAR(s_fake_coupling[0][1], k_ref_C[0][1], 1.0,
                     "coupling cell [0][1] must converge to truth across repeated runs from a 0.0 prior");
    TEST_CHECK_NEAR(s_fake_coupling[0][2], k_ref_C[0][2], 1.0,
                     "coupling cell [0][2] must converge to truth across repeated runs from a 0.0 prior");
}

// ---------------------------------------------------------------------
// F5/D3: ADAPTIVE_TUNE_COUPLING_MAX_ABS_MOVE must actually bind a single
// run's move -- zero coverage previously let it silently revert 6.0 -> 10.0
// (or anything else) with 109/109 still green. Prior is confident-but-low
// (2.0, so the near-zero branch is NOT the one exercised) and the fit is
// far enough away that the uncapped 15% blend would clear the cap
// comfortably.
// ---------------------------------------------------------------------
static void test_coupling_cell_per_run_move_is_bounded_by_abs_cap(void)
{
    reset_module_state();
    s_zones[0].enabled = true;
    s_fake_zone_cfg[0].k_dc = 1.0f;
    s_fake_coupling[0][1] = 1.0f; // confident prior (> NEAR_ZERO); uncapped blend would be
                                  // 1.0 + 0.15*(45.0-1.0) = 7.6, well past the 6.0 cap
    const float ambient = 20.0f;
    static const float k_test_C[3][3] = {
        {39.25f, 45.0f, 20.73f}, {15.78f, 31.97f, 21.09f}, {9.70f, 11.38f, 31.68f}};
    const float duty_pts[6][MAX31856_CHANNEL_COUNT] = {
        {0.20f, 0.50f, 0.80f}, {0.50f, 0.80f, 0.20f}, {0.80f, 0.20f, 0.50f},
        {0.35f, 0.65f, 0.15f}, {0.65f, 0.15f, 0.65f}, {0.15f, 0.35f, 0.35f}};
    for (int k = 0; k < 6; k++) {
        float target[MAX31856_CHANNEL_COUNT];
        for (int i = 0; i < 3; i++) {
            float rise = 0.0f;
            for (int j = 0; j < 3; j++) rise += k_test_C[i][j] * duty_pts[k][j];
            target[i] = ambient + rise;
        }
        feed_joint_settled_dwell(target, ambient, duty_pts[k], SETTLE_TICKS, DT_S);
    }
    profile_firing_run_record_t rec = make_clean_record(90, 0, 900);
    adaptive_tune_run_end(&rec, true);

    TEST_CHECK(s_zones[0].coupled_applied, "setup: the coupled solve must have applied");
    TEST_CHECK_NEAR(s_fake_coupling[0][1], 1.0f + ADAPTIVE_TUNE_COUPLING_MAX_ABS_MOVE, 0.3,
                     "D3: a single run's coupling-cell move must be clamped exactly at the per-run absolute cap");
    TEST_CHECK(s_fake_coupling[0][1] <= 1.0f + ADAPTIVE_TUNE_COUPLING_MAX_ABS_MOVE + 1e-3f,
               "D3: a single run's coupling-cell move must never exceed the per-run absolute cap");
}

// ---------------------------------------------------------------------
// F5/D1: zero coverage of either direction of the ratio guard's upper
// bound (upper = max(prior*5, ADAPTIVE_TUNE_COUPLING_IMPLAUSIBLE_ABS)).
// Accept direction: a confident-but-low prior must still accept a fit far
// past its own 5x ratio, as long as it is within the absolute plausibility
// ceiling -- otherwise a cell blended up from a near-zero prior could never
// converge past ~5.3x its own early blend step.
// ---------------------------------------------------------------------
static void test_coupling_ratio_guard_upper_bound_accepts_high_fit_from_low_confident_prior(void)
{
    reset_module_state();
    s_zones[0].enabled = true;
    s_fake_zone_cfg[0].k_dc = 1.0f;
    s_fake_coupling[0][1] = 2.0f; // confident prior -- a plain 5x ratio ceiling alone would be 10.0
    const float ambient = 20.0f;
    static const float k_test_C[3][3] = {
        {39.25f, 49.0f, 20.73f}, {15.78f, 31.97f, 21.09f}, {9.70f, 11.38f, 31.68f}};
    const float duty_pts[6][MAX31856_CHANNEL_COUNT] = {
        {0.20f, 0.50f, 0.80f}, {0.50f, 0.80f, 0.20f}, {0.80f, 0.20f, 0.50f},
        {0.35f, 0.65f, 0.15f}, {0.65f, 0.15f, 0.65f}, {0.15f, 0.35f, 0.35f}};
    for (int k = 0; k < 6; k++) {
        float target[MAX31856_CHANNEL_COUNT];
        for (int i = 0; i < 3; i++) {
            float rise = 0.0f;
            for (int j = 0; j < 3; j++) rise += k_test_C[i][j] * duty_pts[k][j];
            target[i] = ambient + rise;
        }
        feed_joint_settled_dwell(target, ambient, duty_pts[k], SETTLE_TICKS, DT_S);
    }
    profile_firing_run_record_t rec = make_clean_record(91, 0, 900);
    adaptive_tune_run_end(&rec, true);

    TEST_CHECK(s_fake_coupling[0][1] > 2.0f, "D1 (accept direction): a fit of ~49 against a confident-but-low "
                                              "prior of 2.0 (49 >> 2*5) must still move the cell UP, not be "
                                              "refused for exceeding a plain 5x ratio ceiling");
}

// F5/D1, reject direction: the widened upper bound is not unlimited -- a
// fit above the absolute plausibility ceiling must still be refused.
static void test_coupling_ratio_guard_upper_bound_rejects_fit_above_absolute_ceiling(void)
{
    reset_module_state();
    s_zones[0].enabled = true;
    s_fake_zone_cfg[0].k_dc = 1.0f;
    s_fake_coupling[0][1] = 2.0f;
    const float ambient = 20.0f;
    static const float k_test_C[3][3] = {
        {39.25f, 55.0f, 20.73f}, {15.78f, 31.97f, 21.09f}, {9.70f, 11.38f, 31.68f}};
    const float duty_pts[6][MAX31856_CHANNEL_COUNT] = {
        {0.20f, 0.50f, 0.80f}, {0.50f, 0.80f, 0.20f}, {0.80f, 0.20f, 0.50f},
        {0.35f, 0.65f, 0.15f}, {0.65f, 0.15f, 0.65f}, {0.15f, 0.35f, 0.35f}};
    for (int k = 0; k < 6; k++) {
        float target[MAX31856_CHANNEL_COUNT];
        for (int i = 0; i < 3; i++) {
            float rise = 0.0f;
            for (int j = 0; j < 3; j++) rise += k_test_C[i][j] * duty_pts[k][j];
            target[i] = ambient + rise;
        }
        feed_joint_settled_dwell(target, ambient, duty_pts[k], SETTLE_TICKS, DT_S);
    }
    profile_firing_run_record_t rec = make_clean_record(92, 0, 900);
    adaptive_tune_run_end(&rec, true);

    TEST_CHECK(s_fake_coupling[0][1] == 2.0f, "D1 (reject direction): a fit of 55 (above the 50.0 absolute "
                                               "plausibility ceiling) against a confident prior must be refused, "
                                               "not accepted -- the widened upper bound is not unlimited");
}

// ---------------------------------------------------------------------
// D4: the joint-observation floor must count DISTINCT dwells, not rows --
// N simultaneously-enabled zones settling on the SAME dwell must contribute
// exactly ONE joint row, not N near-identical ones.
// ---------------------------------------------------------------------
static void test_joint_floor_counts_distinct_dwells_not_rows(void)
{
    reset_module_state();
    s_zones[0].enabled = true;
    s_zones[1].enabled = true;
    s_zones[2].enabled = true;
    const float ambient = 20.0f;
    const float duty_pts[2][MAX31856_CHANNEL_COUNT] = {{0.20f, 0.50f, 0.80f}, {0.50f, 0.80f, 0.20f}};
    for (int k = 0; k < 2; k++) {
        float target[MAX31856_CHANNEL_COUNT];
        for (int i = 0; i < 3; i++) {
            float rise = 0.0f;
            for (int j = 0; j < 3; j++) rise += k_ref_C[i][j] * duty_pts[k][j];
            target[i] = ambient + rise;
        }
        feed_joint_settled_dwell(target, ambient, duty_pts[k], SETTLE_TICKS, DT_S);
    }
    // The regression this guards: with 3 zones enabled and 2 distinct
    // dwells, a per-zone (rather than per-dwell) commit would leave 6 rows
    // in the ring here, not 2 -- silently clearing the 5-observation floor
    // with only 2 real operating points for 3 unknowns.
    TEST_CHECK(s_joint_ring_count == 2, "2 distinct dwells with 3 zones enabled must commit exactly 2 joint rows, "
                                         "not one per zone");

    profile_firing_run_record_t rec = make_clean_record(21, 0, 900);
    rec.zones[1].active = true;
    rec.zones[1].stats.sample_count = 900;
    rec.zones[2].active = true;
    rec.zones[2].stats.sample_count = 900;
    adaptive_tune_run_end(&rec, true);
    TEST_CHECK(!s_zones[0].coupled_applied, "2 distinct dwells (below the 5-observation margin) must refuse the "
                                             "coupled solve, not silently accept an underdetermined fit");
    // F4: the check above passes even under the REVERTED D4 gate (per-zone
    // rather than per-dwell counting), because 3 zones x 2 dwells = 6
    // near-duplicate rows are then singular (identical columns) and get
    // refused as ILL_CONDITIONED anyway -- a DIFFERENT refusal path than
    // the one this test is meant to guard. Pin the actual refusal reason so
    // a D4 regression (which would still leave coupled_applied false, but
    // for the wrong reason) is caught here instead of silently passing.
    TEST_CHECK(strstr(s_zones[0].coupled_refusal_reason, "joint dwell observations") != NULL,
               "the coupled solve must refuse specifically for TOO FEW joint observations (2 distinct dwells "
               "against the 5-observation floor) -- a D4 regression to per-zone counting would instead leave 6 "
               "near-duplicate rows that clear the floor and refuse as ill-conditioned instead, which the plain "
               "!coupled_applied check above cannot tell apart from this");
}

// ---------------------------------------------------------------------
// D6: try_refine_coupled_locked()'s OWN indexing (reading C[zi][j] and
// calling set_coupling_cell(zi, j, ...)) has no coverage from the pure-math
// adaptive_tune_coupled_fit() tests above -- a transpose at either call
// site would still pass all of them. This drives the full apply path (via
// adaptive_tune_run_end()) against the same asymmetric bench matrix and
// checks the fake coupling TABLE lands cells in the storage orientation
// (coupling_coeff[affected][stepped]), not swapped.
// ---------------------------------------------------------------------
static void test_coupled_apply_writes_cells_in_storage_orientation(void)
{
    reset_module_state();
    s_zones[0].enabled = true;
    s_zones[1].enabled = true;
    const float ambient = 20.0f;
    const float duty_pts[6][MAX31856_CHANNEL_COUNT] = {
        {0.20f, 0.50f, 0.80f}, {0.50f, 0.80f, 0.20f}, {0.80f, 0.20f, 0.50f},
        {0.35f, 0.65f, 0.15f}, {0.65f, 0.15f, 0.65f}, {0.15f, 0.35f, 0.35f}};
    for (int k = 0; k < 6; k++) {
        float target[MAX31856_CHANNEL_COUNT];
        for (int i = 0; i < 3; i++) {
            float rise = 0.0f;
            for (int j = 0; j < 3; j++) rise += k_ref_C[i][j] * duty_pts[k][j];
            target[i] = ambient + rise;
        }
        feed_joint_settled_dwell(target, ambient, duty_pts[k], SETTLE_TICKS, DT_S);
    }
    TEST_CHECK(s_joint_ring_count == 6, "setup: 6 distinct joint dwells queued");

    profile_firing_run_record_t rec = make_clean_record(22, 0, 900);
    rec.zones[1].active = true;
    rec.zones[1].stats.sample_count = 900;
    adaptive_tune_run_end(&rec, true);

    TEST_CHECK(s_zones[0].coupled_applied, "setup: zone 0's coupled solve must have applied");
    TEST_CHECK(s_zones[1].coupled_applied, "setup: zone 1's coupled solve must have applied");

    // One run's 15% blend from a 0.0 prior: expect ~= ALPHA * truth for
    // each cell. [0][1] (26.61) and [1][0] (15.78) are far enough apart
    // (their blended values differ by > 1.0) that a transposed read of
    // C[][] or a transposed set_coupling_cell() call would land the WRONG
    // number in the fake table, not just a slightly-off one.
    float expect_01 = ADAPTIVE_TUNE_COUPLING_BLEND_ALPHA * k_ref_C[0][1];
    float expect_10 = ADAPTIVE_TUNE_COUPLING_BLEND_ALPHA * k_ref_C[1][0];
    TEST_CHECK(fabsf(expect_01 - expect_10) > 1.0f,
               "setup: expected [0][1] and [1][0] blended values must be genuinely different, or a transpose bug "
               "could not be distinguished from a correct apply");
    TEST_CHECK_NEAR(s_fake_coupling[0][1], expect_01, 0.3,
                     "fake_coupling[0][1] (affected=0 responding to stepped=1) must NOT read as [1][0]'s value");
    TEST_CHECK_NEAR(s_fake_coupling[1][0], expect_10, 0.3,
                     "fake_coupling[1][0] (affected=1 responding to stepped=0) must NOT read as [0][1]'s value");
}

// ---------------------------------------------------------------------
// D5: the model/PID refinement (SIMC recompute) and the Ki diagnosis must
// not stack in the same run -- the Ki diagnosis's trace evidence was
// gathered under the OLD Ki, so applying its +/-20% scale on top of a
// JUST-rewritten SIMC Ki would double up an unrelated correction.
// ---------------------------------------------------------------------
static void test_ki_diagnosis_skipped_same_run_as_model_refine(void)
{
    reset_module_state();
    s_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f; // prior -- true gain 15, well within jump/spread guards
    s_fake_zone_cfg[1].ki = 1.0f;    // a positive Ki to refine -- needed below to prove the fixture is capable
    const float true_k = 15.0f, ambient = 22.3f;
    const float duties[4] = {0.20f, 0.50f, 0.80f, 0.35f};
    for (int i = 0; i < 4; i++) {
        // Last dwell runs long (20 ticks, not just SETTLE_TICKS) so its
        // trailing trace clears ADAPTIVE_TUNE_KI_MIN_SAMPLES -- the Ki
        // diagnosis reads only the MOST RECENT dwell's trace (reset every
        // dwell entry), so this is what a real, capable fixture needs.
        int ticks = (i == 3) ? 20 : SETTLE_TICKS;
        feed_settled_dwell(1, ambient + true_k * duties[i], ambient, duties[i], ticks, DT_S);
    }
    profile_firing_run_record_t rec = make_clean_record(11, 1, 900);
    // A steady, non-floored, mid-range-duty dwell error -- exactly the
    // OFFSET_TOO_SMALL shape (duty 0.35 last dwell is nowhere near a rail).
    rec.zones[1].stats.dwell_err_mean_c = 0.45f;
    rec.zones[1].stats.dwell_err_max_c = 0.50f;
    adaptive_tune_run_end(&rec, true);

    TEST_CHECK(s_zones[1].has_applied, "setup: the diagonal model refinement must have applied this run");
    TEST_CHECK(!s_zones[1].ki_applied, "the Ki diagnosis must NOT also apply in the same run as the model refine");
    TEST_CHECK(strstr(s_zones[1].ki_refusal_reason, "skipped") != NULL,
               "the Ki refusal reason should say it was skipped because the model refine already ran this run");

    // F4: the two checks above pass EVEN WITHOUT the D5 skip gate, because
    // this fixture's dwells (SETTLE_TICKS=7 each) never clear
    // ADAPTIVE_TUNE_KI_MIN_SAMPLES on their own -- the "!ki_applied" check
    // was vacuous, only the "skipped" reason-string check was load-bearing.
    // Prove this fixture is now genuinely capable of an applied Ki
    // correction by calling try_refine_ki_locked() directly (this file
    // #includes adaptive_tune.c, so its static functions are reachable),
    // bypassing the D5 gate entirely, on the SAME trace/stats this run just
    // produced.
    try_refine_ki_locked(1, &rec.zones[1].stats);
    TEST_CHECK(s_zones[1].ki_applied, "setup: this fixture's trace/stats must genuinely trigger an applied Ki "
                                       "correction when nothing skips it -- otherwise the !ki_applied check above "
                                       "would pass regardless of whether the D5 gate does anything at all");
}

// ---------------------------------------------------------------------
// F1: try_refine_zone_locked() used to report "applied" on ANY nonzero
// blend, however small -- an asymptotically-converging sequence of fits
// (each blend closer to the true gain, never exactly equal) kept
// model_refined true forever, permanently starving try_refine_ki_locked()
// of a turn. MUST FAIL on pre-F1-fix code: 8+ consecutive well-formed runs
// against the same true gain never produce a run where the model refine
// reports no material change, so the Ki refusal reason always says
// "skipped".
// ---------------------------------------------------------------------
static void test_ki_diagnosis_eventually_runs_after_repeated_converging_refinements(void)
{
    reset_module_state();
    s_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;
    s_fake_zone_cfg[1].ki = 1.0f;
    const float true_k = 15.0f, ambient = 22.3f;
    const float duties[4] = {0.20f, 0.50f, 0.80f, 0.35f};

    bool ki_ever_ran = false;
    for (int run = 0; run < 30; run++) {
        for (int i = 0; i < 4; i++) {
            feed_settled_dwell(1, ambient + true_k * duties[i], ambient, duties[i], SETTLE_TICKS, DT_S);
        }
        profile_firing_run_record_t rec = make_clean_record(70 + run, 1, 900);
        adaptive_tune_run_end(&rec, true);
        if (strstr(s_zones[1].ki_refusal_reason, "skipped") == NULL) {
            ki_ever_ran = true;
        }
    }
    TEST_CHECK(ki_ever_ran, "F1: across 30 runs converging on the same true gain, the model refine must eventually "
                             "report no material change so the Ki diagnosis gets a genuine turn -- it must never "
                             "be permanently starved");
    // H4(b): the blend has a PERMANENT steady-state residual, not a transient
    // one -- see ADAPTIVE_TUNE_MIN_MATERIAL_MOVE_FRAC's corrected comment in
    // adaptive_tune.c. At the material-change floor's freeze point,
    // k/k_true == ALPHA/(ALPHA+MIN_MATERIAL_MOVE_FRAC) == 0.15/0.155, so
    // k_dc parks at 15.0 * (0.15/0.155) = 14.516 and never moves further --
    // measured 14.578 here (close enough given the 0.1 degC quantization
    // this fixture's temperatures go through). 0.5 tolerance left only 0.078
    // of slack around that measured value, a standing flake risk. Widened to
    // 0.6 -- still comfortably tighter than the 1.59 residual produced when
    // MIN_MATERIAL_MOVE_FRAC is mistakenly retuned to 0.02f (freeze point
    // 13.41, |15-13.41| = 1.59 >> 0.6), so this assertion still catches that
    // regression; it must NOT be loosened further without re-checking against
    // that mutation.
    TEST_CHECK_NEAR(s_fake_zone_cfg[1].k_dc, true_k, 0.6,
                     "setup: the repeated refinements must actually have converged k_dc close to the true gain "
                     "(within its expected permanent residual), or 'never applying again' would be trivially true "
                     "for the wrong reason");
}

// ---------------------------------------------------------------------
// F2: adaptive_tune_run_end()'s D5 skip branch must not leave ki_verdict/
// ki_correction_pct holding a PREVIOUS run's real diagnosis -- adaptive_
// tune_get_status() publishes both verbatim, and a skipped run must not be
// misreported as a live verdict for the run that actually skipped it. MUST
// FAIL on pre-F2-fix code: run 2's ki_verdict/ki_correction_pct still read
// run 1's OFFSET_TOO_SMALL/positive values.
// ---------------------------------------------------------------------
static void test_ki_status_fields_cleared_when_diagnosis_skipped(void)
{
    reset_module_state();
    s_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;
    s_fake_zone_cfg[1].ki = 1.0f;

    // Run 1: too few dwell observations for the model refine to fire (only
    // 1, well under ADAPTIVE_TUNE_MIN_OBSERVATIONS), but a single LONG
    // dwell gives the Ki trace plenty of samples, and an explicit steady,
    // non-floored offset drives a genuine OFFSET_TOO_SMALL verdict.
    feed_settled_dwell(1, 25.0f, 22.0f, 0.5f, 20, DT_S);
    TEST_CHECK(s_zones[1].ring_count < ADAPTIVE_TUNE_MIN_OBSERVATIONS,
               "setup: the model refine must not have enough observations to fire");
    profile_firing_run_record_t rec1 = make_clean_record(80, 1, 900);
    rec1.zones[1].stats.dwell_err_mean_c = 0.45f;
    rec1.zones[1].stats.dwell_err_max_c = 0.50f;
    adaptive_tune_run_end(&rec1, true);
    TEST_CHECK(!s_zones[1].has_applied, "setup: the model refine must not have applied this run");
    TEST_CHECK(s_zones[1].ki_verdict == (uint8_t)ADAPTIVE_TUNE_KI_OFFSET_TOO_SMALL,
               "setup: the Ki diagnosis must have actually run and produced a real, non-default verdict");
    TEST_CHECK(s_zones[1].ki_correction_pct > 0.0f, "setup: a real, nonzero correction must be published");

    // Run 2: feed enough well-spread, on-model observations that the model
    // refine DOES fire this time -- the Ki diagnosis must be skipped.
    const float true_k = 15.0f, ambient = 22.3f;
    const float duties[4] = {0.20f, 0.50f, 0.80f, 0.35f};
    for (int i = 0; i < 4; i++) {
        feed_settled_dwell(1, ambient + true_k * duties[i], ambient, duties[i], SETTLE_TICKS, DT_S);
    }
    profile_firing_run_record_t rec2 = make_clean_record(81, 1, 900);
    adaptive_tune_run_end(&rec2, true);
    TEST_CHECK(s_zones[1].has_applied, "setup: the model refine must have applied this run");
    TEST_CHECK(!s_zones[1].ki_applied, "the Ki diagnosis must not apply when skipped");
    TEST_CHECK(s_zones[1].ki_verdict == (uint8_t)ADAPTIVE_TUNE_KI_INSUFFICIENT,
               "F2: a skipped-this-run Ki diagnosis must not keep publishing a PREVIOUS run's verdict");
    TEST_CHECK(s_zones[1].ki_correction_pct == 0.0f,
               "F2: a skipped-this-run Ki diagnosis must not keep publishing a PREVIOUS run's correction pct");
}

// ---------------------------------------------------------------------
// H2: adaptive_tune_run_end()'s !clean and excluded-fraction paths both
// `continue` immediately, having written only the refusal-reason strings --
// leaving ki_applied/ki_verdict/ki_correction_pct/coupled_applied/
// coupled_cells_changed holding whatever a PREVIOUS run left there.
// adaptive_tune_get_status() publishes all five verbatim, so a faulted run
// could show a stale "Ki correction applied, +20%" right next to "this run
// was faulted and not used" -- exactly the run an operator is most likely to
// inspect. MUST FAIL on pre-H2-fix code (reset_run_status_locked() not
// called on these two paths).
// ---------------------------------------------------------------------
static void test_run_status_fields_cleared_on_faulted_run(void)
{
    reset_module_state();
    s_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;
    s_fake_zone_cfg[1].ki = 1.0f;

    // Run 1: clean, applies a model refine AND leaves a real, non-default Ki
    // diagnosis outcome behind is not possible in the same run (D5), so
    // drive a genuine standalone Ki-applied run instead: too few dwell
    // observations for the model refine, but a long dwell with a steady
    // offset for the Ki diagnosis to actually apply against.
    feed_settled_dwell(1, 25.0f, 22.0f, 0.5f, 20, DT_S);
    profile_firing_run_record_t rec1 = make_clean_record(90, 1, 900);
    rec1.zones[1].stats.dwell_err_mean_c = 0.45f;
    rec1.zones[1].stats.dwell_err_max_c = 0.50f;
    adaptive_tune_run_end(&rec1, true);
    TEST_CHECK(s_zones[1].ki_applied, "setup: run 1 must have genuinely applied a Ki correction");
    TEST_CHECK(s_zones[1].ki_verdict == (uint8_t)ADAPTIVE_TUNE_KI_OFFSET_TOO_SMALL,
               "setup: run 1's Ki verdict must be real (OFFSET_TOO_SMALL), not a default");
    TEST_CHECK(s_zones[1].ki_correction_pct > 0.0f, "setup: run 1's Ki correction pct must be real and nonzero");

    // Run 2: same zone, faulted. Feed the SAME on-model observations that
    // test_dirty_run_is_never_training_data() proved make a material blend,
    // so a naive reader might expect coupled_applied too -- it must not.
    feed_settled_dwell(1, 22.0f + 12.0f * 0.2f, 22.0f, 0.2f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 22.0f + 12.0f * 0.5f, 22.0f, 0.5f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 22.0f + 12.0f * 0.8f, 22.0f, 0.8f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 22.0f + 12.0f * 0.35f, 22.0f, 0.35f, SETTLE_TICKS, DT_S);
    profile_firing_run_record_t rec2 = make_clean_record(91, 1, 900);
    adaptive_tune_run_end(&rec2, /*clean=*/false);

    TEST_CHECK(!s_zones[1].ki_applied, "H2: a faulted run must not keep publishing a PREVIOUS run's ki_applied=1");
    TEST_CHECK(s_zones[1].ki_verdict == (uint8_t)ADAPTIVE_TUNE_KI_INSUFFICIENT,
               "H2: a faulted run must reset ki_verdict, not keep publishing OFFSET_TOO_SMALL from run 1");
    TEST_CHECK(s_zones[1].ki_correction_pct == 0.0f,
               "H2: a faulted run must reset ki_correction_pct, not keep publishing run 1's +20%%");
    TEST_CHECK(!s_zones[1].has_applied, "H2: a faulted run must not report has_applied from a previous run");
    TEST_CHECK(!s_zones[1].coupled_applied,
               "H2: a faulted run must not report coupled_applied from a previous run");
    TEST_CHECK(s_zones[1].coupled_cells_changed == 0,
               "H2: a faulted run must reset coupled_cells_changed, not keep publishing a previous run's count");
    TEST_CHECK(strstr(s_zones[1].ki_refusal_reason, "faulted or stopped early") != NULL,
               "H2: the Ki refusal reason must say why THIS run produced no diagnosis");
}

// H2, excluded-fraction path -- same defect, different guard. MUST FAIL on
// pre-H2-fix code the same way as the faulted-run test above.
static void test_run_status_fields_cleared_on_excluded_fraction_refusal(void)
{
    reset_module_state();
    s_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;
    s_fake_zone_cfg[1].ki = 1.0f;

    feed_settled_dwell(1, 25.0f, 22.0f, 0.5f, 20, DT_S);
    profile_firing_run_record_t rec1 = make_clean_record(92, 1, 900);
    rec1.zones[1].stats.dwell_err_mean_c = 0.45f;
    rec1.zones[1].stats.dwell_err_max_c = 0.50f;
    adaptive_tune_run_end(&rec1, true);
    TEST_CHECK(s_zones[1].ki_applied, "setup: run 1 must have genuinely applied a Ki correction");

    feed_settled_dwell(1, 22.0f + 12.0f * 0.2f, 22.0f, 0.2f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 22.0f + 12.0f * 0.5f, 22.0f, 0.5f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 22.0f + 12.0f * 0.8f, 22.0f, 0.8f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 22.0f + 12.0f * 0.35f, 22.0f, 0.35f, SETTLE_TICKS, DT_S);
    profile_firing_run_record_t rec2 = make_clean_record(93, 1, 900);
    rec2.zones[1].stats.excluded_sample_count = 100; // 100/(900+100) = 10% > 5%
    adaptive_tune_run_end(&rec2, /*clean=*/true);

    TEST_CHECK(!s_zones[1].ki_applied,
               "H2: an excluded-fraction refusal must not keep publishing a PREVIOUS run's ki_applied=1");
    TEST_CHECK(s_zones[1].ki_verdict == (uint8_t)ADAPTIVE_TUNE_KI_INSUFFICIENT,
               "H2: an excluded-fraction refusal must reset ki_verdict");
    TEST_CHECK(s_zones[1].ki_correction_pct == 0.0f,
               "H2: an excluded-fraction refusal must reset ki_correction_pct");
    TEST_CHECK(!s_zones[1].has_applied, "H2: an excluded-fraction refusal must not report a stale has_applied");
    TEST_CHECK(!s_zones[1].coupled_applied,
               "H2: an excluded-fraction refusal must not report a stale coupled_applied");
    TEST_CHECK(strstr(s_zones[1].ki_refusal_reason, "excluded") != NULL,
               "H2: the Ki refusal reason must say why THIS run produced no diagnosis");
}

// ---------------------------------------------------------------------
// H3: try_refine_ki_locked() has no cumulative bound of its own, only
// ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE per run (20%). On real hardware the
// loop is CLOSED -- a rising Ki genuinely shrinks dwell_err_mean_c on the
// next firing, so the correction is self-limiting -- and zones_config_set_
// pid() rejects a gain above ZONE_PID_GAIN_MAX as a backstop. Before H3, the
// host fake had neither: it accepted any Ki unconditionally, and no test
// modeled the closed loop, so a probe with STEADY, UNCHANGING evidence
// (idealized-test-input bug class) showed Ki compounding x1.2/run forever:
// 1.0 -> 8.92 in 12 runs with no test to catch it. This test drives
// dwell_err_mean_c FROM the zone's own just-updated Ki every run (err
// shrinks as Ki rises, modeling the real closed loop), and separately proves
// the fake's new reject-above-ZONE_PID_GAIN_MAX backstop (H3(a) fix above)
// actually holds when the loop is NOT closed (constant error, same as the
// idealized probe).
// ---------------------------------------------------------------------

// H3(b): closed-loop model. dwell_err_mean_c = KI_TEST_ERR_K / ki -- a
// simple inverse relationship standing in for "more integral action shrinks
// steady-state error," which is qualitatively what a real PID loop does.
// ki starts at 1.0 (err = 8.0, well above the 0.3 OFFSET threshold);
// convergence is expected once err drops to/below 0.3, i.e. once ki reaches
// KI_TEST_ERR_K/0.3 ~= 26.7, roughly 19 runs of the 20%/run cap
// (1.2^19 ~= 27.4) -- run well past that (60 runs) and require the loop to
// have actually STOPPED moving, not merely slowed down.
#define KI_TEST_ERR_K 8.0f

static void test_ki_diagnosis_converges_under_closed_loop_plant_feedback(void)
{
    reset_module_state();
    s_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f; // never a material-move blend below -- keep the model refine permanently
                                      // un-due so the Ki diagnosis gets a turn on EVERY run (D5 would otherwise
                                      // starve it exactly like F1's own bug, defeating the point of this test)
    s_fake_zone_cfg[1].ki = 1.0f;

    float last_ki = 1.0f;
    bool converged = false;
    int converged_at_run = -1;
    for (int run = 0; run < 60; run++) {
        float ki_before = s_fake_zone_cfg[1].ki;
        float err_mean = KI_TEST_ERR_K / ki_before;
        float err_max = err_mean * 1.1f; // steady -- inside OFFSET_MAX_OVER_MEAN(1.6), same posture as the
                                          // pure-math OFFSET tests above

        // One short dwell (keeps ring_count well under ADAPTIVE_TUNE_MIN_
        // OBSERVATIONS, so try_refine_zone_locked() never has enough data to
        // fire and D5 never starves the Ki diagnosis) plus a long dwell that
        // actually feeds the Ki trace -- same two-call shape as
        // test_ki_diagnosis_skipped_same_run_as_model_refine()'s fixture.
        feed_settled_dwell(1, 25.0f, 22.0f, 0.5f, 20, DT_S);

        profile_firing_run_record_t rec = make_clean_record(100 + run, 1, 900);
        rec.zones[1].stats.dwell_err_mean_c = err_mean;
        rec.zones[1].stats.dwell_err_max_c = err_max;
        adaptive_tune_run_end(&rec, true);

        TEST_CHECK(!s_zones[1].has_applied, "setup: the model refine must never fire in this fixture -- only "
                                             "the Ki diagnosis is under test here");

        float ki_after = s_fake_zone_cfg[1].ki;
        if (!s_zones[1].ki_applied && ki_after == ki_before && !converged) {
            converged = true;
            converged_at_run = run;
        }
        last_ki = ki_after;
    }

    TEST_CHECK(converged, "H3: under closed-loop plant feedback (error shrinking as Ki rises), the Ki diagnosis "
                           "must eventually stop applying corrections -- it must NOT compound forever the way the "
                           "idealized constant-error probe showed (1.0 -> 8.92 in 12 runs)");
    TEST_CHECK(converged_at_run >= 0 && converged_at_run < 40,
               "H3: convergence should happen within the expected ~19-run window for this fixture's error/Ki "
               "relationship, not accidentally at the very end of the 60-run loop");
    TEST_CHECK(last_ki < 50.0f, "H3: a closed loop must converge to a BOUNDED Ki, nowhere near the unclamped "
                                 "runaway (8.92 after just 12 runs) the pre-fix probe measured");

    // Re-run several more times past convergence -- Ki must genuinely have
    // STOPPED, not merely slowed (a test that only checks "less than some
    // number" cannot tell "converged" from "still growing slowly").
    for (int run = 60; run < 65; run++) {
        feed_settled_dwell(1, 25.0f, 22.0f, 0.5f, 20, DT_S);
        profile_firing_run_record_t rec = make_clean_record(100 + run, 1, 900);
        rec.zones[1].stats.dwell_err_mean_c = KI_TEST_ERR_K / s_fake_zone_cfg[1].ki;
        rec.zones[1].stats.dwell_err_max_c = rec.zones[1].stats.dwell_err_mean_c * 1.1f;
        adaptive_tune_run_end(&rec, true);
    }
    TEST_CHECK_NEAR(s_fake_zone_cfg[1].ki, last_ki, 1e-4,
                     "H3: Ki must be genuinely stable past convergence, not merely growing more slowly");
}

// H3(a) backstop, negative-test companion: WITHOUT closed-loop feedback (the
// idealized constant-error shape the original probe used), the reject-above-
// ZONE_PID_GAIN_MAX guard in the now-realistic fake zones_config_set_pid()
// (see its own H3(a) comment) is what eventually stops the runaway, proving
// the fake is no longer idealized. MUST FAIL if TEST_ZONE_PID_GAIN_MAX's
// reject check above is deleted (Ki would climb unbounded for all 40 runs
// instead of hitting the ceiling and refusing further growth).
static void test_ki_diagnosis_runaway_under_constant_error_is_capped_by_gain_ceiling(void)
{
    reset_module_state();
    s_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;
    s_fake_zone_cfg[1].ki = 1.0f;

    bool saw_a_refusal_after_growth = false;
    for (int run = 0; run < 60; run++) {
        feed_settled_dwell(1, 25.0f, 22.0f, 0.5f, 20, DT_S);
        profile_firing_run_record_t rec = make_clean_record(200 + run, 1, 900);
        // Constant, never-shrinking error -- the idealized-input shape the
        // review's probe used, deliberately preserved here as the NEGATIVE
        // case this module must not silently tolerate on host any more.
        rec.zones[1].stats.dwell_err_mean_c = 0.45f;
        rec.zones[1].stats.dwell_err_max_c = 0.50f;
        adaptive_tune_run_end(&rec, true);
        if (run > 30 && !s_zones[1].ki_applied &&
            strstr(s_zones[1].ki_refusal_reason, "rejected the corrected Ki") != NULL) {
            saw_a_refusal_after_growth = true;
        }
    }
    TEST_CHECK(saw_a_refusal_after_growth,
               "H3(a): under constant (non-shrinking) error, Ki must climb until zones_config_set_pid()'s "
               "gain ceiling actually refuses it -- proving the host fake is no longer idealized (it used to "
               "accept any Ki unconditionally)");
    TEST_CHECK(s_fake_zone_cfg[1].ki <= TEST_ZONE_PID_GAIN_MAX,
               "H3(a): Ki must never be left above the gain ceiling once the setter starts refusing it");
}

// ---------------------------------------------------------------------
// F3: dwelling_prev (and everything gated on it -- the settle window,
// trace reset, and s_joint_dwell_row_committed) must reflect whether a zone
// is PHYSICALLY dwelling, independent of whether any given tick's data
// happens to be trustworthy or whether the zone happens to be opted in.
// Both sub-cases below share one root cause: the old code only ever
// touched dwelling_prev from inside branches gated on enabled/actual_valid,
// so a zone that never passed those gates during its real dwell-entry
// tick(s) never recorded having entered at all.
// ---------------------------------------------------------------------

// F3(a): the data-validity half. MUST FAIL on pre-F3-fix code -- dwelling_
// prev stays false after a dwelling tick with bad data, instead of tracking
// the raw `dwelling` state.
static void test_dwelling_prev_tracks_dwelling_state_even_when_data_is_invalid(void)
{
    reset_module_state();
    s_zones[0].enabled = true;
    adaptive_tune_zone_tick(0, 100.0f, /*actual_valid=*/false, 0.5f, /*dwelling=*/true, 22.0f, DT_S);
    TEST_CHECK(s_zones[0].dwelling_prev == true,
               "a zone whose FIRST dwelling tick has bad data is still physically dwelling -- dwelling_prev must "
               "reflect that immediately, not silently stay false until the first VALID tick");

    reset_module_state();
    s_zones[0].enabled = true;
    adaptive_tune_zone_tick(0, 100.0f, true, 0.5f, true, NAN, DT_S); // NaN ambient, same claim
    TEST_CHECK(s_zones[0].dwelling_prev == true,
               "a zone whose FIRST dwelling tick has a NaN ambient reading is still physically dwelling -- "
               "dwelling_prev must reflect that immediately");
}

// F3(b): the opt-in half, and the end-to-end observable consequence of the
// same root cause -- a zone opted into adaptive tuning MID-DWELL must not
// be misread as having just entered a fresh dwell, or it wrongly reopens
// (and lets something re-commit into) an already-committed joint row from
// THIS SAME physical dwell. MUST FAIL on pre-F3-fix code: s_joint_ring_
// count ends at 2, not 1, for one physical dwell.
static void test_enabling_zone_mid_dwell_does_not_reopen_committed_joint_row(void)
{
    reset_module_state();
    s_zones[0].enabled = false; // starts opted OUT
    s_zones[1].enabled = true;
    s_zones[2].enabled = true;
    const float ambient = 20.0f;
    const float duty[MAX31856_CHANNEL_COUNT] = {0.5f, 0.5f, 0.5f};

    // Zone 0 is disabled but ticking with perfectly good data throughout --
    // a disabled zone still contributes its duty as a joint-cache NEIGHBOUR
    // column (see s_joint_last_duty[]'s own comment), it just never runs
    // its OWN settle/ring bookkeeping while opted out.
    adaptive_tune_zone_tick(0, q1(25.0f), true, duty[0], true, ambient, DT_S);
    adaptive_tune_zone_tick(1, q1(25.0f), true, duty[1], false, ambient, DT_S);
    adaptive_tune_zone_tick(2, q1(25.0f), true, duty[2], false, ambient, DT_S);
    for (int i = 0; i < SETTLE_TICKS; i++) {
        adaptive_tune_zone_tick(0, q1(25.0f), true, duty[0], true, ambient, DT_S);
        adaptive_tune_zone_tick(1, q1(25.0f), true, duty[1], true, ambient, DT_S);
        adaptive_tune_zone_tick(2, q1(25.0f), true, duty[2], true, ambient, DT_S);
    }
    TEST_CHECK(s_joint_ring_count == 1, "setup: zones 1/2 settling with zone 0 disabled-but-valid must commit "
                                         "exactly one joint row");

    // Zone 0 gets opted IN mid-dwell -- still the SAME physical dwell (it
    // was never disabled from the joint cache's perspective, only from its
    // own adaptive-tune bookkeeping).
    s_zones[0].enabled = true;
    for (int i = 0; i < SETTLE_TICKS; i++) {
        adaptive_tune_zone_tick(0, q1(25.0f), true, duty[0], true, ambient, DT_S);
        adaptive_tune_zone_tick(1, q1(25.0f), true, duty[1], true, ambient, DT_S);
        adaptive_tune_zone_tick(2, q1(25.0f), true, duty[2], true, ambient, DT_S);
    }
    TEST_CHECK(s_joint_ring_count == 1, "F3(b): opting a zone into adaptive tuning mid-dwell must NOT be misread "
                                         "as that zone entering a fresh dwell -- it must not reopen and re-commit "
                                         "an already-committed joint row from this same physical dwell");
}

void run_test_adaptive_tune(void)
{
    TEST_SECTION("adaptive_tune: settling");
    test_unsettled_dwell_is_not_recorded();
    test_settled_dwell_is_recorded();

    TEST_SECTION("adaptive_tune: opt-in default off");
    test_opt_in_default_off_records_nothing();

    TEST_SECTION("adaptive_tune: public accessor surface (same one adaptive_tune_http.c calls)");
    test_default_off_for_every_zone();
    test_enable_one_zone_leaves_others_untouched();
    test_enable_round_trips_through_persistence();
    test_status_reports_engine_held_fields_not_test_written_values();

    TEST_SECTION("adaptive_tune: guards");
    test_min_observations_guard_rejects_too_few();
    test_duty_spread_guard_rejects_clustered_observations();
    test_implausible_jump_guard_rejects_far_off_fit();
    test_dirty_run_is_never_training_data();
    test_per_run_move_is_bounded_even_with_many_dwells();

    TEST_SECTION("adaptive_tune: refinement improves the estimate");
    test_refinement_improves_gain_estimate_on_known_plant();

    TEST_SECTION("adaptive_tune: coupled identification -- pure fit");
    test_coupled_fit_refuses_underdetermined_observation_set();
    test_coupled_fit_refuses_ill_conditioned_observations();
    test_coupled_fit_recovers_known_asymmetric_matrix();

    TEST_SECTION("adaptive_tune: Ki diagnosis from dwells");
    test_ki_diagnose_insufficient_below_min_samples();
    test_ki_diagnose_steady_offset_flags_small_ki();
    test_ki_diagnose_floored_not_misdiagnosed_as_small_ki();
    test_ki_diagnose_monotonic_drift_is_not_misread_as_oscillation();
    test_ki_diagnose_monotonic_drift_with_steady_offset_flags_small_ki();
    test_ki_diagnose_floored_wins_even_with_monotonic_drift();
    test_ki_diagnose_flat_duty_mid_range_is_not_floored();
    test_ki_diagnose_near_rail_but_varying_is_not_floored();
    test_ki_diagnose_limit_cycle_yields_ku_tu();
    test_ki_diagnose_irregular_hunting_yields_oscillating();
    test_ki_diagnose_ok_when_tracking_cleanly();

    TEST_SECTION("adaptive_tune: coupling cell convergence (D1)");
    test_coupling_cell_converges_from_zero_prior_over_repeated_runs();
    test_coupling_cell_per_run_move_is_bounded_by_abs_cap();
    test_coupling_ratio_guard_upper_bound_accepts_high_fit_from_low_confident_prior();
    test_coupling_ratio_guard_upper_bound_rejects_fit_above_absolute_ceiling();

    TEST_SECTION("adaptive_tune: joint observation floor counts distinct dwells (D4)");
    test_joint_floor_counts_distinct_dwells_not_rows();

    TEST_SECTION("adaptive_tune: coupled apply orientation (D6)");
    test_coupled_apply_writes_cells_in_storage_orientation();

    TEST_SECTION("adaptive_tune: model refine and Ki diagnosis do not stack (D5)");
    test_ki_diagnosis_skipped_same_run_as_model_refine();

    TEST_SECTION("adaptive_tune: Ki diagnosis eventually gets a turn (F1)");
    test_ki_diagnosis_eventually_runs_after_repeated_converging_refinements();

    TEST_SECTION("adaptive_tune: skipped Ki diagnosis clears stale status fields (F2)");
    test_ki_status_fields_cleared_when_diagnosis_skipped();

    TEST_SECTION("adaptive_tune: full-skip run_end paths clear ALL stale status fields (H2)");
    test_run_status_fields_cleared_on_faulted_run();
    test_run_status_fields_cleared_on_excluded_fraction_refusal();

    TEST_SECTION("adaptive_tune: repeated Ki application is bounded under closed-loop feedback (H3)");
    test_ki_diagnosis_converges_under_closed_loop_plant_feedback();
    test_ki_diagnosis_runaway_under_constant_error_is_capped_by_gain_ceiling();

    TEST_SECTION("adaptive_tune: dwell-entry bookkeeping survives invalid data / late enable (F3)");
    test_dwelling_prev_tracks_dwelling_state_even_when_data_is_invalid();
    test_enabling_zone_mid_dwell_does_not_reopen_committed_joint_row();
}

int main(void)
{
    run_test_adaptive_tune();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
