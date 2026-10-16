// test_ct_split_map_guards.c -- CT audit 2026-10-14 HIGH-1 / LOW-1.
//
// HIGH-1: a committed split zone_ct_channel map is back-filled to ct_topology
// SUMMED, and the topology-only fitted predicate then called channels 0/1
// "not fitted", blinding S3/S9/S6b/S11/S14/S15. These tests build a REAL split
// map through the SET_PARAM -> finalize -> commit back-fill path (not by
// injecting amps_valid, as test_safety_guards.c:test_s14_s15_two_ct_split
// does) and push the record-derived facts into safety_guards_tick().
// safety_core.c itself is not host-compilable; it computes these facts with
// exactly the helpers used below (call-site routing is pinned separately by
// test_ct_summed_topology_call_sites.c).
//
// LOW-1: k_ct_v_per_a / gain lower bound at SET_PARAM and at validate.
#include <stdbool.h>
#include <string.h>

#include "test_common.h"

#include "config_params.h"
#include "config_store.h"
#include "safety_guards.h"

static bool setu8(config_store_record_t *r, uint16_t id, uint8_t v)
{
    kilnlink_param_value_t pv;
    memset(&pv, 0, sizeof(pv));
    pv.u8_val = v;
    return config_params_set(r, id, KILNLINK_PARAM_TYPE_U8, pv);
}

static bool setf(config_store_record_t *r, uint16_t id, float v)
{
    kilnlink_param_value_t pv;
    memset(&pv, 0, sizeof(pv));
    pv.f32_val = v;
    return config_params_set(r, id, KILNLINK_PARAM_TYPE_F32, pv);
}

/* Commit a map exactly as COMMIT_CONFIG does: SET_PARAM x3, finalize, back-fill. */
static void commit_map(config_store_record_t *r, uint8_t m0, uint8_t m1, uint8_t m2)
{
    config_store_default(r);
    TEST_CHECK(setu8(r, 0x0320u, m0) && setu8(r, 0x0321u, m1) && setu8(r, 0x0322u, m2),
               "zone_ct_channel SET_PARAM accepted");
    config_params_finalize_zone_ct_channel(r);
    config_store_backfill_legacy_ct_topology(r);
}

static void base_guard_cfg(safety_guard_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->tc_placement_valid = true;
    cfg->tc_placement_mode = SAFETY_TC_EXTERNAL_OVERHEAT;
    cfg->abs_max_temp_c = 1300.0f;
    cfg->tc_source = SAFETY_TC_SOURCE_OWN_J7;
}

static void test_split_map_fitted_and_presence(void)
{
    TEST_SECTION("committed split {0,0,1}: channels 0/1 fitted, channel 2 not; "
                 "presence on 0/1 reaches the guards");
    config_store_record_t r;
    commit_map(&r, 0u, 0u, 1u);
    TEST_CHECK(r.ct_topology == CONFIG_STORE_CT_TOPOLOGY_SUMMED,
               "precondition: commit back-fills the split to SUMMED");
    uint8_t mask = config_store_fitted_channel_mask(&r);
    TEST_CHECK(mask == 0x3u, "fitted mask is channels 0 and 1 (NOT channel 2 only)");

    bool p0[3] = { true, false, false };
    bool p1[3] = { false, true, false };
    bool p2[3] = { false, false, true };
    TEST_CHECK(config_store_mask_current_present_to_fitted_mask(p0, mask),
               "current on channel 0 is present");
    TEST_CHECK(config_store_mask_current_present_to_fitted_mask(p1, mask),
               "current on channel 1 is present");
    TEST_CHECK(!config_store_mask_current_present_to_fitted_mask(p2, mask),
               "noise on the unwired channel 2 is masked out");

    /* End to end into S3: current on channel 0 with nothing commanded trips. */
    safety_guard_state_t s;
    safety_guards_reset(&s);
    safety_guard_cfg_t cfg;
    base_guard_cfg(&cfg);
    safety_guard_input_t in;
    memset(&in, 0, sizeof(in));
    in.tc_valid = true;
    in.tc_c = 20.0f;
    in.cj_c = 25.0f;
    in.current_sensing_commissioned = true;
    in.sample_counter_advancing = true;
    in.link_up = true;
    in.context_valid = true;
    in.any_current_present = config_store_mask_current_present_to_fitted_mask(p0, mask);
    in.dt_s = 5.0f;
    bool tripped = false;
    for (int i = 0; i < 5 && !tripped; i++) {
        tripped = safety_guards_tick(&s, &cfg, &in);
    }
    TEST_CHECK(tripped && s.reason == SAFETY_TRIP_LOAD_STUCK_ON,
               "S3 trips on welded-SSR current read on channel 0 of a split map");
}

static void test_split_map_s14_arm_gets_amps_valid(void)
{
    TEST_SECTION("committed split {0,0,1}: S14/S15 amps_valid derived from the record "
                 "arms the split attribution");
    config_store_record_t r;
    commit_map(&r, 0u, 0u, 1u);
    r.k_ct_v_per_a[0] = 0.1f;
    r.k_ct_v_per_a[1] = 0.1f;

    safety_guard_cfg_t cfg;
    base_guard_cfg(&cfg);
    cfg.zone_ct_channel_valid = true;
    config_store_effective_zone_ct_channel(&r, cfg.zone_ct_channel);
    for (int z = 0; z < 3; z++) {
        cfg.i_normal_valid[z] = true;
    }
    cfg.i_normal_a[0] = 10.03f;
    cfg.i_normal_a[1] = 5.07f;
    cfg.i_normal_a[2] = 7.02f;

    safety_guard_input_t in;
    memset(&in, 0, sizeof(in));
    in.tc_valid = true;
    in.tc_c = 20.0f;
    in.cj_c = 25.0f;
    in.sample_counter_advancing = true;
    in.link_up = true;
    in.context_valid = true;
    in.dt_s = 5.0f;
    const float amps[3] = { 4.91f, 6.98f, 0.0f };
    for (uint8_t ch = 0; ch < 3u; ch++) {
        /* Same derivation safety_core_build_input() performs. */
        if (config_store_ct_channel_fitted_rec(&r, ch) && r.k_ct_v_per_a[ch] > 0.0f) {
            in.amps_valid[ch] = true;
            in.amps[ch] = amps[ch];
        }
    }
    TEST_CHECK(in.amps_valid[0] && in.amps_valid[1] && !in.amps_valid[2],
               "amps_valid is true for channels 0/1 and false for unwired channel 2");
    in.relay_commanded_now_for_zone[0] = true;
    in.relay_commanded_now_for_zone[1] = true;
    in.relay_commanded_now_for_zone[2] = true;
    safety_guard_state_t s;
    safety_guards_reset(&s);
    for (int i = 0; i < 20; i++) {
        safety_guards_tick(&s, &cfg, &in);
    }
    TEST_CHECK(s.s15_warn[0] && s.s15_warn[1] && !s.s15_warn[2],
               "channel 0's deficit flags its member zones 0 and 1 only");
}

static void test_commissioned_covers_every_fitted_channel(void)
{
    TEST_SECTION("S9 commissioned: every fitted channel of the committed map needs k_ct");
    config_store_record_t r;
    commit_map(&r, 0u, 0u, 1u);
    r.k_ct_v_per_a[2] = 0.1f; /* the only value the OLD gate looked at */
    TEST_CHECK(!config_store_current_sensing_commissioned(&r),
               "k_ct only on unwired channel 2 does not commission a {0,0,1} board");
    r.k_ct_v_per_a[0] = 0.1f;
    TEST_CHECK(!config_store_current_sensing_commissioned(&r),
               "channel 1 still uncalibrated: not commissioned");
    r.k_ct_v_per_a[1] = 0.1f;
    TEST_CHECK(config_store_current_sensing_commissioned(&r),
               "channels 0 and 1 calibrated: commissioned (channel 2 unwired, not required)");

    commit_map(&r, 1u, 1u, 1u); /* single shared CT on jack 1 */
    TEST_CHECK(config_store_fitted_channel_mask(&r) == 0x2u, "{1,1,1}: only channel 1 fitted");
    r.k_ct_v_per_a[1] = 0.1f;
    TEST_CHECK(config_store_current_sensing_commissioned(&r),
               "{1,1,1} with channel 1 calibrated is commissioned");
}

static void test_legacy_maps_unchanged(void)
{
    TEST_SECTION("legacy topologies and an uncommitted map keep the old fitted set");
    config_store_record_t r;
    commit_map(&r, 2u, 2u, 2u);
    TEST_CHECK(config_store_fitted_channel_mask(&r) == 0x4u, "{2,2,2}: channel 2 only");
    commit_map(&r, 0u, 1u, 2u);
    TEST_CHECK(config_store_fitted_channel_mask(&r) == 0x7u, "{0,1,2}: all three");

    config_store_default(&r);
    r.ct_topology = CONFIG_STORE_CT_TOPOLOGY_SUMMED;
    r.fields_set &= ~(uint32_t)CONFIG_STORE_SET_ZONE_CT_CHANNEL;
    TEST_CHECK(config_store_fitted_channel_mask(&r) == 0x4u,
               "no committed map: topology SUMMED still means channel 2 only");
    r.ct_installed = 0u;
    r.fields_set |= (uint32_t)CONFIG_STORE_SET_CT_INSTALLED;
    TEST_CHECK(config_store_fitted_channel_mask(&r) == 0u, "ct_installed==0: nothing fitted");
    commit_map(&r, 0u, 0u, 1u);
    r.ct_installed = 0u;
    r.fields_set |= (uint32_t)CONFIG_STORE_SET_CT_INSTALLED;
    TEST_CHECK(config_store_fitted_channel_mask(&r) == 0u,
               "ct_installed==0 beats a committed map");
}

static void test_k_ct_gain_lower_bounds(void)
{
    TEST_SECTION("LOW-1: k_ct_v_per_a / gain refuse denormal, tiny, negative values");
    config_store_record_t r;
    config_store_default(&r);
    for (uint16_t id = 0x0308u; id <= 0x030Du; id++) {
        TEST_CHECK(setf(&r, id, 0.0f), "0 (uncommissioned) stays accepted");
        TEST_CHECK(setf(&r, id, 0.1f), "a normal value is accepted");
        TEST_CHECK(setf(&r, id, 1.0f), "1.0 is accepted");
        TEST_CHECK(!setf(&r, id, 1.0e-30f), "denormal-scale value refused");
        TEST_CHECK(!setf(&r, id, 0.00005f), "below the floor refused");
        TEST_CHECK(!setf(&r, id, -0.1f), "negative refused");
        TEST_CHECK(!setf(&r, id, 1.0e9f), "absurdly large refused");
    }
    /* Load/validate backstop. */
    const char *field = NULL;
    const char *rule = NULL;
    config_store_default(&r);
    r.k_ct_v_per_a[1] = 1.0e-30f;
    TEST_CHECK(!config_params_validate(&r, &field, &rule), "validate refuses a tiny k_ct");
    config_store_default(&r);
    r.gain[2] = 1.0e-30f;
    TEST_CHECK(!config_params_validate(&r, &field, &rule), "validate refuses a tiny gain");
    config_store_default(&r);
    TEST_CHECK(config_params_validate(&r, &field, &rule), "default record still validates");
}

void run_test_ct_split_map_guards(void)
{
    test_split_map_fitted_and_presence();
    test_split_map_s14_arm_gets_amps_valid();
    test_commissioned_covers_every_fitted_channel();
    test_legacy_maps_unchanged();
    test_k_ct_gain_lower_bounds();
}
