// test_commissioning_gate.c -- ROADMAP.md M12, "An uncommissioned safety
// processor refuses heating enable". Host tests for src/commissioning_gate.c,
// the pure decision safety_core_request_enable() consults before forwarding
// an ON request to relay_owner.
//
// The commissioning here is driven through the REAL wire path's own
// functions (config_params_set() per SET_PARAM id, then
// config_params_finalize_ct_channel_map() + config_params_all_required_set()
// exactly as link_task.c's COMMIT_CONFIG handler does), not by poking
// fields_set by hand -- otherwise the "after commissioning, heat is allowed"
// case would prove only that the test can set a bit.
#include <string.h>

#include "commissioning_gate.h"
#include "config_params.h"
#include "config_store.h"
#include "kilnlink/kilnlink_param_value.h"
#include "test_common.h"

// Stages every parameter config_params_all_required_set() requires, in the
// same order and through the same calls a real SET_PARAM/COMMIT_CONFIG
// session uses. Mirrors test_config_store.c's own sequence for the same set
// of ids (docs/COMMISSIONING.md section 2.1's table).
static void stage_full_commissioning(config_store_record_t *rec)
{
    kilnlink_param_value_t v;
    v.u8_val = CONFIG_STORE_TC_SOURCE_OWN_J7;
    config_params_set(rec, 0x0101u, KILNLINK_PARAM_TYPE_U8, v); // tc_source
    v.u8_val = 0u;
    config_params_set(rec, 0x0102u, KILNLINK_PARAM_TYPE_U8, v); // borrowed_zone_index
    v.u8_val = CONFIG_STORE_TC_PLACEMENT_CHAMBER_AGREED;
    config_params_set(rec, 0x0103u, KILNLINK_PARAM_TYPE_U8, v); // tc_placement_mode
    v.f32_val = 1300.0f;
    config_params_set(rec, 0x0104u, KILNLINK_PARAM_TYPE_F32, v); // abs_max_temp_c
    v.u8_val = CONFIG_STORE_DEFAULT_TC_TYPE;
    config_params_set(rec, 0x0105u, KILNLINK_PARAM_TYPE_U8, v); // tc_type
    v.u8_val = 0u;
    config_params_set(rec, 0x0106u, KILNLINK_PARAM_TYPE_U8, v);
    v.u8_val = 1u;
    config_params_set(rec, 0x0107u, KILNLINK_PARAM_TYPE_U8, v);
    v.u8_val = 2u;
    config_params_set(rec, 0x0108u, KILNLINK_PARAM_TYPE_U8, v); // ct_channel_map 0..2
    v.f32_val = 5.0f;
    config_params_set(rec, 0x0204u, KILNLINK_PARAM_TYPE_F32, v); // max_rate_c_per_min
    v.f32_val = 240.0f;
    config_params_set(rec, 0x030Eu, KILNLINK_PARAM_TYPE_F32, v); // mains_voltage_v
    config_params_finalize_ct_channel_map(rec);
    v.u8_val = 1u;
    config_params_set(rec, 0x0109u, KILNLINK_PARAM_TYPE_U8, v); // ct_installed = yes,
                                                                 // the strict answer --
                                                                 // this helper stages the
                                                                 // FULL, CT-fitted board
    // What link_task.c's COMMIT_CONFIG handler writes into the record:
    //   to_write.calibration_missing = !config_params_all_required_set(&to_write);
    rec->calibration_missing = !config_params_all_required_set(rec);
}

// Everything stage_full_commissioning() stages EXCEPT the three
// ct_channel_map ids and the ct_installed answer -- the real bench board this
// pass was written for, where no CT is physically fitted and so nothing can
// honestly confirm which relay a channel watches.
static void stage_full_commissioning_except_ct_map(config_store_record_t *rec)
{
    kilnlink_param_value_t v;
    v.u8_val = CONFIG_STORE_TC_SOURCE_OWN_J7;
    config_params_set(rec, 0x0101u, KILNLINK_PARAM_TYPE_U8, v);
    v.u8_val = 0u;
    config_params_set(rec, 0x0102u, KILNLINK_PARAM_TYPE_U8, v);
    v.u8_val = CONFIG_STORE_TC_PLACEMENT_CHAMBER_AGREED;
    config_params_set(rec, 0x0103u, KILNLINK_PARAM_TYPE_U8, v);
    v.f32_val = 1300.0f;
    config_params_set(rec, 0x0104u, KILNLINK_PARAM_TYPE_F32, v);
    v.u8_val = CONFIG_STORE_DEFAULT_TC_TYPE;
    config_params_set(rec, 0x0105u, KILNLINK_PARAM_TYPE_U8, v);
    v.f32_val = 5.0f;
    config_params_set(rec, 0x0204u, KILNLINK_PARAM_TYPE_F32, v);
    v.f32_val = 240.0f;
    config_params_set(rec, 0x030Eu, KILNLINK_PARAM_TYPE_F32, v);
    config_params_finalize_ct_channel_map(rec); // a no-op here: nothing staged
    rec->calibration_missing = !config_params_all_required_set(rec);
}

void run_test_commissioning_gate(void)
{
    TEST_SECTION("commissioning_gate -- an uncommissioned safety processor refuses heating enable");

    // (a) A fresh, never-commissioned board.
    config_store_record_t rec;
    config_store_default(&rec);
    TEST_CHECK(rec.calibration_missing,
               "a default record starts calibration_missing (config_store_default's contract)");
    TEST_CHECK(!commissioning_gate_is_commissioned(&rec),
               "a fresh default record is NOT commissioned");
    TEST_CHECK(commissioning_gate_energize_allowed(true, &rec) == false,
               "uncommissioned: an ON (heating enable) request is REFUSED");
    TEST_CHECK(commissioning_gate_energize_allowed(false, &rec) == true,
               "uncommissioned: an OFF request is still allowed -- de-energizing is never gated");

    // A NULL record must not be a way to get heat.
    TEST_CHECK(!commissioning_gate_is_commissioned(NULL), "NULL record is not commissioned");
    TEST_CHECK(commissioning_gate_energize_allowed(true, NULL) == false,
               "NULL record: an ON request is refused");
    TEST_CHECK(commissioning_gate_energize_allowed(false, NULL) == true,
               "NULL record: an OFF request is still allowed");

    // (b) After a real, complete commissioning pass, heat is allowed.
    stage_full_commissioning(&rec);
    TEST_CHECK(!rec.calibration_missing,
               "a complete COMMIT_CONFIG clears calibration_missing");
    TEST_CHECK(commissioning_gate_is_commissioned(&rec),
               "a fully commissioned record IS commissioned");
    TEST_CHECK(commissioning_gate_energize_allowed(true, &rec) == true,
               "commissioned: an ON (heating enable) request is ALLOWED");

    // A PARTIAL commission must not read as commissioned -- otherwise the
    // check above would pass for any record that had been written at all.
    // COMMISSIONING.md section 4.1: "a bench preset must not look
    // commissioned".
    TEST_SECTION("commissioning_gate -- a partial commission is still uncommissioned");
    config_store_record_t partial;
    config_store_default(&partial);
    kilnlink_param_value_t v;
    v.f32_val = 1300.0f;
    config_params_set(&partial, 0x0104u, KILNLINK_PARAM_TYPE_F32, v); // abs_max_temp_c only
    partial.calibration_missing = !config_params_all_required_set(&partial);
    TEST_CHECK(partial.calibration_missing, "a one-field commit leaves calibration_missing set");
    TEST_CHECK(commissioning_gate_energize_allowed(true, &partial) == false,
               "partially commissioned: an ON request is REFUSED");

    // The two sources of truth must AGREE. A record whose stored
    // calibration_missing byte claims "commissioned" while fields_set does
    // not back that up (a garbled byte, a migration, a future writer that
    // forgot to recompute) must land on REFUSE, not on "close enough".
    TEST_SECTION("commissioning_gate -- stored flag and fields_set must agree");
    config_store_record_t lying;
    config_store_default(&lying);
    lying.calibration_missing = false; // claims commissioned; fields_set says otherwise
    TEST_CHECK(!config_params_all_required_set(&lying),
               "the lying record's fields_set does not actually back the claim");
    TEST_CHECK(!commissioning_gate_is_commissioned(&lying),
               "flag says commissioned but no required field is set: NOT commissioned");
    TEST_CHECK(commissioning_gate_energize_allowed(true, &lying) == false,
               "flag/fields_set disagreement: an ON request is REFUSED");

    // ...and the other direction: every required bit set, but the stored
    // flag still says calibration_missing (e.g. a v1 record migrated
    // forward, where config_store.c FORCES the flag true). Also refuse.
    config_store_record_t flagged;
    config_store_default(&flagged);
    stage_full_commissioning(&flagged);
    flagged.calibration_missing = true; // as a v1->v2 migration forces it
    TEST_CHECK(config_params_all_required_set(&flagged),
               "the migrated record's fields_set does look complete");
    TEST_CHECK(!commissioning_gate_is_commissioned(&flagged),
               "stored calibration_missing still set: NOT commissioned, whatever fields_set says");
    TEST_CHECK(commissioning_gate_energize_allowed(true, &flagged) == false,
               "migrated/forced-flag record: an ON request is REFUSED");

    // --- The CT-less board (ROADMAP.md M12, "CTs are optional hardware") ----
    // The gate itself is unchanged -- it delegates to
    // config_params_all_required_set() -- but this is the end-to-end
    // statement of the behaviour the change exists to deliver, and the
    // negative half below is what stops it being a blanket escape hatch.
    TEST_SECTION("commissioning_gate -- a board with no CTs fitted can be commissioned");

    config_store_record_t no_ct;
    config_store_default(&no_ct);
    stage_full_commissioning_except_ct_map(&no_ct);

    // Negative first: same record, ct_installed never answered.
    TEST_CHECK(commissioning_gate_energize_allowed(true, &no_ct) == false,
               "no CT map and the CT question unanswered: an ON request is still REFUSED");

    kilnlink_param_value_t ctv;
    config_store_record_t claims_ct = no_ct;
    ctv.u8_val = 1u;
    config_params_set(&claims_ct, 0x0109u, KILNLINK_PARAM_TYPE_U8, ctv);
    claims_ct.calibration_missing = !config_params_all_required_set(&claims_ct);
    TEST_CHECK(commissioning_gate_energize_allowed(true, &claims_ct) == false,
               "CTs declared FITTED but never mapped: an ON request is still REFUSED");

    // Positive: the declared-absent board commissions and may heat.
    ctv.u8_val = 0u;
    config_params_set(&no_ct, 0x0109u, KILNLINK_PARAM_TYPE_U8, ctv);
    no_ct.calibration_missing = !config_params_all_required_set(&no_ct);
    TEST_CHECK(!no_ct.calibration_missing,
               "declaring no CTs fitted clears calibration_missing with no ct_channel_map");
    TEST_CHECK(commissioning_gate_is_commissioned(&no_ct),
               "a CT-less board IS commissioned once the CT question is answered 'no'");
    TEST_CHECK(commissioning_gate_energize_allowed(true, &no_ct) == true,
               "CTs declared absent: an ON (heating enable) request is ALLOWED");
    TEST_CHECK(commissioning_gate_energize_allowed(false, &no_ct) == true,
               "CTs declared absent: an OFF request is allowed, as always");
}
