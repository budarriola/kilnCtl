// test_link_staging.c -- host test for the REAL link_staging module
// (tasks/link_staging.c), the SET_PARAM staging area link_task.c builds
// COMMIT_CONFIG / APPLY_CONFIG_VOLATILE candidates from.
//
// docs/audits/KILNLINK_ROBUSTNESS_AUDIT_2026-10-09.md:
//   M2 -- a direct flash write (SET_CONFIG tc_type, SET_CT_CAL) made after
//         staging began was reverted by the next COMMIT, because COMMIT wrote
//         a whole staged record seeded before that write. The tests below
//         replay that exact sequence with the real link_frame_apply_set_*()
//         record builders link_task.c uses, and require the direct write to
//         survive the commit.
//   M3 -- staged edits must be discardable (ESP boot_id change);
//         link_staging_reset() is what link_task.c calls.
#include <stdio.h>
#include <string.h>

#include "test_common.h"
#include "config_params.h"
#include "config_store.h"
#include "max31856.h"
#include "tasks/link_frame.h"
#include "tasks/link_staging.h"

static link_staging_t s_st; // ~600 B, kept off the stack

static kilnlink_set_param_t mk_f32(uint16_t id, float v)
{
    kilnlink_set_param_t e;
    memset(&e, 0, sizeof(e));
    e.param_id = id;
    e.type = KILNLINK_PARAM_TYPE_F32;
    e.value.f32_val = v;
    return e;
}

static kilnlink_set_param_t mk_u8(uint16_t id, uint8_t v)
{
    kilnlink_set_param_t e;
    memset(&e, 0, sizeof(e));
    e.param_id = id;
    e.type = KILNLINK_PARAM_TYPE_U8;
    e.value.u8_val = v;
    return e;
}

static bool stage(const config_store_record_t *committed, kilnlink_set_param_t e)
{
    config_store_record_t scratch = *committed;
    return link_staging_stage(&s_st, &e, &scratch);
}

// The record link_task.c's COMMIT/APPLY builds: currently enforced record
// plus the staged edits.
static bool build_candidate(const config_store_record_t *committed, config_store_record_t *out)
{
    *out = *committed;
    return link_staging_apply(&s_st, out);
}

static void test_direct_tc_type_survives_later_commit(void)
{
    TEST_SECTION("M2: SET_CONFIG tc_type after staging began survives the next COMMIT");

    config_store_record_t committed;
    config_store_default(&committed);
    committed.tc_type = MAX31856_TC_TYPE_K;
    link_staging_reset(&s_st);

    // ESP stages an unrelated threshold.
    TEST_CHECK(stage(&committed, mk_f32(0x0104u, 1200.0f)), "abs_max_temp_c edit staged");

    // SET_CONFIG writes tc_type N straight to flash.
    config_store_record_t after;
    link_frame_apply_set_config(&committed, 0x02u /* N */, &after);
    size_t dropped = link_staging_drop_superseded(&s_st, &committed, &after);
    TEST_CHECK(dropped == 0u, "an edit of a field SET_CONFIG did not change is kept");
    committed = after; // flash truth is now the SET_CONFIG record

    config_store_record_t cand;
    TEST_CHECK(build_candidate(&committed, &cand), "candidate builds");
    TEST_CHECK(cand.tc_type == 0x02u,
               "COMMIT candidate keeps the directly written tc_type (was reverted to K before M2)");
    TEST_CHECK(cand.abs_max_temp_c == 1200.0f, "staged abs_max_temp_c is in the candidate");
}

static void test_direct_write_supersedes_earlier_edit(void)
{
    TEST_SECTION("M2: a direct write wins over an EARLIER staged edit of the same field");

    config_store_record_t committed;
    config_store_default(&committed);
    committed.tc_type = MAX31856_TC_TYPE_K;
    link_staging_reset(&s_st);

    TEST_CHECK(stage(&committed, mk_u8(0x0105u, 0x06u /* S */)), "tc_type S staged");
    TEST_CHECK(stage(&committed, mk_f32(0x0104u, 1100.0f)), "abs_max staged");

    config_store_record_t after;
    link_frame_apply_set_config(&committed, 0x02u /* N */, &after);
    TEST_CHECK(link_staging_drop_superseded(&s_st, &committed, &after) == 1u,
               "the staged tc_type edit is dropped, the abs_max edit kept");
    committed = after;

    config_store_record_t cand;
    TEST_CHECK(build_candidate(&committed, &cand), "candidate builds");
    TEST_CHECK(cand.tc_type == 0x02u, "later SET_CONFIG tc_type wins over the earlier staged S");
    TEST_CHECK(cand.abs_max_temp_c == 1100.0f, "unrelated staged edit survives");
}

static void test_direct_ct_cal_survives_later_commit(void)
{
    TEST_SECTION("M2: SET_CT_CAL after staging began survives the next COMMIT");

    config_store_record_t committed;
    config_store_default(&committed);
    link_staging_reset(&s_st);

    // ct_cal[1].gain staged to 1.5, ct_cal[0].gain staged to 0.9.
    TEST_CHECK(stage(&committed, mk_f32(0x0311u, 1.5f)), "ct_cal[1].gain staged");
    TEST_CHECK(stage(&committed, mk_f32(0x0310u, 0.9f)), "ct_cal[0].gain staged");

    // SET_CT_CAL channel 1 -> gain 2.25, offset 0.125, calibrated.
    config_store_record_t after;
    link_frame_apply_set_ct_cal(&committed, 1u, true, 2.25f, 0.125f, &after);
    size_t dropped = link_staging_drop_superseded(&s_st, &committed, &after);
    TEST_CHECK(dropped == 1u, "only the ct_cal[1].gain edit is superseded");
    committed = after;

    config_store_record_t cand;
    TEST_CHECK(build_candidate(&committed, &cand), "candidate builds");
    TEST_CHECK(cand.ct_cal[1].gain == 2.25f, "directly written ct_cal[1].gain survives");
    TEST_CHECK(cand.ct_cal[1].offset == 0.125f, "directly written ct_cal[1].offset survives");
    TEST_CHECK(cand.ct_cal[1].calibrated, "directly written ct_cal[1].calibrated survives");
    TEST_CHECK(cand.ct_cal[0].gain == 0.9f, "staged ct_cal[0].gain is in the candidate");
}

static void test_drop_id_same_value(void)
{
    TEST_SECTION("M2: drop_id removes a targeted edit even when the value did not change");

    config_store_record_t committed;
    config_store_default(&committed);
    committed.tc_type = MAX31856_TC_TYPE_K;
    committed.fields_set |= CONFIG_STORE_SET_TC_TYPE;
    link_staging_reset(&s_st);
    TEST_CHECK(stage(&committed, mk_u8(0x0105u, 0x06u)), "tc_type S staged");

    config_store_record_t after;
    link_frame_apply_set_config(&committed, committed.tc_type, &after);
    // Same tc_type re-written: the field compare sees no change and keeps it.
    TEST_CHECK(link_staging_drop_superseded(&s_st, &committed, &after) == 0u,
               "an unchanged field is not superseded by the compare");
    TEST_CHECK(link_staging_drop_id(&s_st, 0x0105u), "drop_id removes the targeted tc_type edit");
    TEST_CHECK(link_staging_count(&s_st) == 0u, "nothing left staged");
    TEST_CHECK(!link_staging_drop_id(&s_st, 0x0105u), "drop_id of an absent id reports false");
}

static void test_dedup_reject_reset(void)
{
    TEST_SECTION("staging: last value wins, refused edits are not recorded, reset empties");

    config_store_record_t committed;
    config_store_default(&committed);
    link_staging_reset(&s_st);

    TEST_CHECK(stage(&committed, mk_f32(0x0104u, 1000.0f)), "first edit");
    TEST_CHECK(stage(&committed, mk_f32(0x0104u, 1250.0f)), "second edit, same id");
    TEST_CHECK(link_staging_count(&s_st) == 1u, "deduplicated by id");
    config_store_record_t cand;
    TEST_CHECK(build_candidate(&committed, &cand), "candidate builds");
    TEST_CHECK(cand.abs_max_temp_c == 1250.0f, "last staged value wins");

    TEST_CHECK(!stage(&committed, mk_f32(0x0104u, -5.0f)),
               "abs_max_temp_c <= 0 refused at stage time");
    TEST_CHECK(!stage(&committed, mk_u8(0x0104u, 3u)), "wrong type refused");
    TEST_CHECK(!stage(&committed, mk_u8(0x7FFFu, 3u)), "unknown id refused");
    TEST_CHECK(link_staging_count(&s_st) == 1u, "refused edits recorded nothing");
    TEST_CHECK(build_candidate(&committed, &cand) && cand.abs_max_temp_c == 1250.0f,
               "a refused edit did not replace the accepted value");

    link_staging_reset(&s_st);
    TEST_CHECK(link_staging_count(&s_st) == 0u, "reset discards every staged edit (M3)");
    TEST_CHECK(build_candidate(&committed, &cand) &&
                   cand.abs_max_temp_c == committed.abs_max_temp_c,
               "after reset the candidate is exactly the committed record");
}

static void test_capacity_covers_table(void)
{
    TEST_SECTION("staging: capacity holds one edit for every config_params id");

    config_store_record_t committed;
    config_store_default(&committed);
    link_staging_reset(&s_st);

    size_t n = config_params_count();
    TEST_CHECK(n <= LINK_STAGING_CAPACITY, "table fits the staging capacity");
    size_t staged = 0u;
    for (size_t i = 0; i < n; ++i) {
        uint16_t id = 0u;
        uint8_t type = 0u;
        TEST_CHECK(config_params_id_at(i, &id, &type), "id_at");
        kilnlink_set_param_t e;
        memset(&e, 0, sizeof(e));
        e.param_id = id;
        TEST_CHECK(config_params_get(&committed, id, &e.type, &e.value), "get");
        // A few defaults are deliberately unset sentinels config_params_set
        // refuses (abs_max 0); stage a valid value for those instead.
        if (!stage(&committed, e)) {
            if (e.type == KILNLINK_PARAM_TYPE_F32) {
                e.value.f32_val = 1.0f;
            } else {
                e.value.u8_val = 0u;
            }
            if (!stage(&committed, e)) {
                continue;
            }
        }
        staged++;
    }
    TEST_CHECK(link_staging_count(&s_st) == staged, "every accepted id got its own slot");
    TEST_CHECK(staged + 2u >= n, "nearly every table id stages (capacity is not the limit)");
}

static void test_new_esp_session(void)
{
    TEST_SECTION("M3: a new ESP session (boot_id change or context gap) discards staging");

    TEST_CHECK(!link_staging_new_esp_session(false, 0u, 0x5Au, false),
               "first PUSH_CONTEXT after Pico boot is not a new session");
    TEST_CHECK(!link_staging_new_esp_session(false, 0u, 0x5Au, true),
               "first PUSH_CONTEXT is not a new session even after a long silence");
    TEST_CHECK(!link_staging_new_esp_session(true, 0x5Au, 0x5Au, false),
               "same boot_id, no gap: same session, staging kept");
    TEST_CHECK(link_staging_new_esp_session(true, 0x5Au, 0x5Bu, false),
               "boot_id changed: the ESP rebooted, staging discarded");
    TEST_CHECK(link_staging_new_esp_session(true, 0x5Au, 0x5Au, true),
               "same boot_id after a context gap (reboot drew the same id, or link loss): "
               "staging discarded");

    // The sequence link_task.c runs: stage, ESP reboots, new session commits.
    config_store_record_t committed;
    config_store_default(&committed);
    committed.abs_max_temp_c = 1000.0f;
    link_staging_reset(&s_st);
    TEST_CHECK(stage(&committed, mk_f32(0x0104u, 1300.0f)), "old session stages a looser value");
    if (link_staging_new_esp_session(true, 0x11u, 0x22u, false)) {
        link_staging_reset(&s_st);
    }
    config_store_record_t cand;
    TEST_CHECK(build_candidate(&committed, &cand), "candidate builds");
    TEST_CHECK(cand.abs_max_temp_c == 1000.0f,
               "the new session's COMMIT does not carry the abandoned staged value");
}

static link_peer_announce_t mk_peer(bool known, uint8_t boot_id, uint16_t version)
{
    link_peer_announce_t p;
    link_peer_announce_clear(&p);
    if (known) {
        link_peer_announce_record(&p, boot_id, version);
    }
    return p;
}

static void test_apply_context_session(void)
{
    TEST_SECTION("push_context trigger: staging reset on new session, peer version on boot_id change");
    config_store_record_t committed;
    config_store_default(&committed);
    link_peer_announce_t pa = mk_peer(false, 0u, 0u);
    pa.version = 16u;

    link_staging_reset(&s_st);
    TEST_CHECK(stage(&committed, mk_f32(0x0104u, 1300.0f)), "stage an edit");
    TEST_CHECK(!link_staging_apply_context_session(&s_st, &pa, true, 0x11u, 0x11u, false),
               "same boot_id, no gap: no new session");
    TEST_CHECK(link_staging_count(&s_st) == 1u && pa.version == 16u, "staging and version untouched");

    TEST_CHECK(link_staging_apply_context_session(&s_st, &pa, true, 0x11u, 0x11u, true),
               ">5 s gap, same boot_id: new session");
    TEST_CHECK(link_staging_count(&s_st) == 0u, "gap discards staging");
    TEST_CHECK(pa.version == 16u, "gap alone keeps the peer protocol version");

    TEST_CHECK(stage(&committed, mk_f32(0x0104u, 1300.0f)), "stage again");
    TEST_CHECK(link_staging_apply_context_session(&s_st, &pa, true, 0x11u, 0x22u, false),
               "boot_id change: new session");
    TEST_CHECK(link_staging_count(&s_st) == 0u, "boot_id change discards staging");
    TEST_CHECK(pa.version == 0u, "boot_id change resets peer protocol version to unknown");

    pa.version = 17u;
    TEST_CHECK(!link_staging_apply_context_session(&s_st, &pa, false, 0u, 0x33u, true),
               "first context after Pico boot: nothing reset");
    TEST_CHECK(pa.version == 17u, "first context keeps the version");
}

static void test_apply_context_session_announce_order(void)
{
    TEST_SECTION("announce/context ordering: version kept only for the announced boot_id");
    link_staging_reset(&s_st);
    // Drives the same record helper link_task.c's ANNOUNCE handler calls.
    link_peer_announce_t pa = mk_peer(false, 0u, 0u);
    link_peer_announce_record(&pa, 0x22u, 17u);
    TEST_CHECK(pa.known && pa.boot_id == 0x22u && pa.version == 17u,
               "record stores the ANNOUNCE's own boot_id and version");
    TEST_CHECK(link_staging_apply_context_session(&s_st, &pa, true, 0x11u, 0x22u, false),
               "announce B then context B after context A: new session");
    TEST_CHECK(pa.version == 17u, "announced boot_id matches context: version kept");

    pa = mk_peer(true, 0x11u, 17u);
    TEST_CHECK(link_staging_apply_context_session(&s_st, &pa, true, 0x11u, 0x22u, false),
               "announce A then context B: new session");
    TEST_CHECK(pa.version == 0u, "announced boot_id differs: version forgotten");

    pa = mk_peer(false, 0u, 0u);
    pa.version = 17u;
    TEST_CHECK(link_staging_apply_context_session(&s_st, &pa, true, 0x11u, 0x22u, false),
               "no announce known, context B");
    TEST_CHECK(pa.version == 0u, "no announce this boot: version forgotten");

    link_peer_announce_record(&pa, 0x33u, 16u);
    link_peer_announce_clear(&pa);
    TEST_CHECK(!pa.known && pa.version == 0u && pa.boot_id == 0u, "clear forgets the announce");
}

void run_test_link_staging(void)
{
    test_apply_context_session();
    test_apply_context_session_announce_order();
    test_new_esp_session();
    test_direct_tc_type_survives_later_commit();
    test_direct_write_supersedes_earlier_edit();
    test_direct_ct_cal_survives_later_commit();
    test_drop_id_same_value();
    test_dedup_reject_reset();
    test_capacity_covers_table();
}
