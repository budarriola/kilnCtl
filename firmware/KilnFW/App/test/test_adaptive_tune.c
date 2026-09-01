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

#include "esp_err.h"
#include "esp_http_server.h"

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
bool zones_config_set_pid(uint8_t zone_index, float kp, float ki, float kd)
{
    if (zone_index >= TEST_MAX_ZONES) return false;
    s_fake_zone_cfg[zone_index].kp = kp;
    s_fake_zone_cfg[zone_index].ki = ki;
    s_fake_zone_cfg[zone_index].kd = kd;
    return true;
}

esp_err_t uart_bridge_ext_run_on_flash_worker(void (*fn)(void *arg), void *arg)
{
    // Single-threaded host test -- just run it inline, matching the real
    // worker's "blocks the caller until the job has run" contract.
    fn(arg);
    return ESP_OK;
}

httpd_handle_t wifi_provision_http_get_server(void) { return NULL; }
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler)
{
    (void)handle;
    (void)uri_handler;
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type) { (void)r; (void)type; return ESP_OK; }
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s) { (void)r; (void)s; return ESP_OK; }
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t error, const char *msg)
{
    (void)r; (void)error; (void)msg;
    return ESP_OK;
}
int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len) { (void)r; (void)buf; (void)buf_len; return 0; }

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
    for (int i = 0; i < TEST_MAX_ZONES; i++) {
        s_fake_zone_cfg[i].tau_s = 200.0f;
        s_fake_zone_cfg[i].dead_time_s = 20.0f;
    }
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
    // Otherwise-perfect, well-spread, on-model observations (true K == 10,
    // exactly the prior -- would apply cleanly if the run were clean).
    feed_settled_dwell(1, 22.0f + 10.0f * 0.2f, 22.0f, 0.2f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 22.0f + 10.0f * 0.5f, 22.0f, 0.5f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 22.0f + 10.0f * 0.8f, 22.0f, 0.8f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 22.0f + 10.0f * 0.35f, 22.0f, 0.35f, SETTLE_TICKS, DT_S);

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

void run_test_adaptive_tune(void)
{
    TEST_SECTION("adaptive_tune: settling");
    test_unsettled_dwell_is_not_recorded();
    test_settled_dwell_is_recorded();

    TEST_SECTION("adaptive_tune: opt-in default off");
    test_opt_in_default_off_records_nothing();

    TEST_SECTION("adaptive_tune: guards");
    test_min_observations_guard_rejects_too_few();
    test_duty_spread_guard_rejects_clustered_observations();
    test_implausible_jump_guard_rejects_far_off_fit();
    test_dirty_run_is_never_training_data();
    test_per_run_move_is_bounded_even_with_many_dwells();

    TEST_SECTION("adaptive_tune: refinement improves the estimate");
    test_refinement_improves_gain_estimate_on_known_plant();
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
