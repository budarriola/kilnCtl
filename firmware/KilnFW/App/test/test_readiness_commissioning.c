/* Host tests for readiness_http.c's "Safety processor commissioned" item.
 *
 * Why this item exists at all: before 2026-08-22 the readiness page could show
 * every light green on a board whose safety processor had never been
 * commissioned -- no trip thresholds, no TC type, nothing -- because no item
 * ever asked. These tests pin the three answers that matter, in particular the
 * one that is easy to get wrong: "link down and nothing cached" must NOT be
 * reported as unfinished operator work, because the ESP cannot distinguish it
 * from a Pico it simply has not talked to yet.
 */

#include "test_common.h"
#include "../drivers/http/readiness_http.h"
#include "../drivers/persist/cfg_fs.h"

void run_test_readiness_commissioning(void)
{
    TEST_SECTION("readiness commissioning item -- safety processor commissioned");

    /* Never fetched, link up: the Pico is answering and still has no config,
     * so this is real, actionable, unfinished commissioning. */
    TEST_CHECK(readiness_commissioning_status(true, 0, 0, 57) == READY_NOT_DONE,
                "crc=0 with the link up must read not_done");

    /* Never fetched, link down: indistinguishable from "not talked to yet".
     * Reporting not_done here would nag the operator about a task they cannot
     * perform and might already have done. */
    TEST_CHECK(readiness_commissioning_status(false, 0, 0, 57) == READY_CANNOT_YET,
                "crc=0 with the link down must read cannot_yet, not not_done");

    /* Fetched, every parameter has a value. */
    TEST_CHECK(readiness_commissioning_status(true, 0xBEEF, 0, 57) == READY_OK,
                "a real crc with no unset params must read ok");

    /* Fetched, but some parameters still have no value -- the sec-1 fields
     * with no compiled-in default read this way until actually set. */
    TEST_CHECK(readiness_commissioning_status(true, 0xBEEF, 4, 57) == READY_NOT_DONE,
                "a real crc with unset params must read not_done");

    /* One unset out of many is still not_done: this item is all-or-nothing on
     * purpose, since a single unset trip threshold is a real safety hole. */
    TEST_CHECK(readiness_commissioning_status(true, 0xBEEF, 1, 57) == READY_NOT_DONE,
                "a single unset param must still read not_done");

    /* A cached crc with an empty param table must not report ok by vacuous
     * truth ("all zero of them are set"). */
    TEST_CHECK(readiness_commissioning_status(true, 0xBEEF, 0, 0) == READY_NOT_DONE,
                "an empty param table must not read ok vacuously");

    /* The link state must not override a successful fetch: once we hold real
     * values, a momentarily-down link does not make them unknown. */
    TEST_CHECK(readiness_commissioning_status(false, 0xBEEF, 0, 57) == READY_OK,
                "cached values stay ok even when the link is momentarily down");
}

/* readiness_param_required_for_commissioning(): ct_channel_map[0..2] and
 * i_normal_a[0..2] are the only params gated by ct_installed -- a board with
 * no current transformers fitted (ct_installed == 0) can never set them, and
 * this readiness item must not count that against commissioning. Every other
 * tracked param is unconditionally required regardless of ct_installed. */
void run_test_readiness_ct_applicability(void)
{
    TEST_SECTION("readiness commissioning item -- CT param applicability");

    const uint16_t ct_ids[6] = {
        READINESS_PARAM_ID_CT_CHANNEL_MAP_0, READINESS_PARAM_ID_CT_CHANNEL_MAP_1,
        READINESS_PARAM_ID_CT_CHANNEL_MAP_2, READINESS_PARAM_ID_I_NORMAL_A_0,
        READINESS_PARAM_ID_I_NORMAL_A_1,     READINESS_PARAM_ID_I_NORMAL_A_2,
    };

    /* ct_installed == 0: none of the six CT params are applicable. */
    for (int i = 0; i < 6; i++) {
        TEST_CHECK(readiness_param_required_for_commissioning(ct_ids[i], 0, 0) == false,
                    "a CT param must not be required when ct_installed == 0");
    }

    /* ct_installed != 0: all six become required again -- ct_installed == 0
     * must not turn into a blanket "CT params never matter" suppression. */
    for (int i = 0; i < 6; i++) {
        TEST_CHECK(readiness_param_required_for_commissioning(ct_ids[i], 1, 0) == true,
                    "a CT param must be required when ct_installed != 0");
    }

    /* A non-CT param (abs_max_temp_c, id 0x0104) is required either way --
     * this is what proves the rule is applicability, not a global relaxation
     * whenever ct_installed happens to be 0. */
    TEST_CHECK(readiness_param_required_for_commissioning(0x0104u, 0, 0) == true,
                "a non-CT param must stay required when ct_installed == 0");
    TEST_CHECK(readiness_param_required_for_commissioning(0x0104u, 1, 0) == true,
                "a non-CT param must stay required when ct_installed != 0");
}

/* readiness_param_required_for_commissioning()'s ct_topology gate
 * (2026-09-09, mirroring firmware/SaftyFW/src/config_params.c's
 * config_params_all_required_set() fix in b5cb83a4): ct_channel_map[0..2]
 * must NOT be required on a summed-CT board, since summed mode's S14/S15
 * read relay_now_mask by zone id and never consult that map -- a
 * fully-configured summed board has no meaningful answer to give it and was
 * left permanently "not commissioned" by this ESP-side copy of the rule
 * even after the Pico side was fixed. i_normal_a[0..2] must stay gated on
 * ct_installed ONLY: unlike the map, a topology change does not remove its
 * need for a live current measurement. */
void run_test_readiness_ct_topology_applicability(void)
{
    TEST_SECTION("readiness commissioning item -- ct_topology applicability (mirrors config_params.c)");

    const uint16_t map_ids[3] = {
        READINESS_PARAM_ID_CT_CHANNEL_MAP_0, READINESS_PARAM_ID_CT_CHANNEL_MAP_1,
        READINESS_PARAM_ID_CT_CHANNEL_MAP_2,
    };
    const uint16_t normal_ids[3] = {
        READINESS_PARAM_ID_I_NORMAL_A_0, READINESS_PARAM_ID_I_NORMAL_A_1, READINESS_PARAM_ID_I_NORMAL_A_2,
    };

    /* CTs fitted, per_zone topology (0): ct_channel_map is required -- the
     * pre-existing, unconditional-on-topology behaviour. */
    for (int i = 0; i < 3; i++) {
        TEST_CHECK(readiness_param_required_for_commissioning(map_ids[i], 1, 0) == true,
                    "ct_channel_map must be required when ct_installed=1 and topology=per_zone");
    }

    /* CTs fitted, summed topology (1): ct_channel_map must NOT be required
     * -- this is the exact fully-configured-summed-board case that used to
     * read commissioning as permanently incomplete on the ESP side even
     * though the Pico itself reported commissioned:true. */
    for (int i = 0; i < 3; i++) {
        TEST_CHECK(readiness_param_required_for_commissioning(map_ids[i], 1, 1) == false,
                    "ct_channel_map must NOT be required when ct_installed=1 and topology=summed");
    }

    /* No CTs fitted at all: ct_channel_map stays not-required regardless of
     * topology (topology is meaningless without CTs in the first place). */
    for (int i = 0; i < 3; i++) {
        TEST_CHECK(readiness_param_required_for_commissioning(map_ids[i], 0, 0) == false,
                    "ct_channel_map must not be required when ct_installed=0, topology=per_zone");
        TEST_CHECK(readiness_param_required_for_commissioning(map_ids[i], 0, 1) == false,
                    "ct_channel_map must not be required when ct_installed=0, topology=summed");
    }

    /* i_normal_a must NOT be re-gated on topology -- stays required whenever
     * CTs are fitted, summed or not. */
    for (int i = 0; i < 3; i++) {
        TEST_CHECK(readiness_param_required_for_commissioning(normal_ids[i], 1, 0) == true,
                    "i_normal_a must stay required with ct_installed=1, topology=per_zone");
        TEST_CHECK(readiness_param_required_for_commissioning(normal_ids[i], 1, 1) == true,
                    "i_normal_a must stay required with ct_installed=1, topology=summed (NOT re-gated)");
    }
}

/* End-to-end through readiness_commissioning_status(): the exact scenario the
 * owner reported live -- ct_installed == 0, the six CT params unset, every
 * other one of the 65 set. The applicable-only count (59) must read OK, not
 * the raw unset-of-65 count (6) that used to fail this forever on a board
 * with no CTs fitted. */
void run_test_readiness_ct_installed_zero_reads_ok(void)
{
    TEST_SECTION("readiness commissioning item -- ct_installed=0 end to end");

    uint8_t ct_installed_value = 0;
    size_t applicable = 0, unset = 0;
    /* 65 total params: the 6 CT ids (unset, not applicable) plus 59 others
     * (all set). */
    const uint16_t ct_ids[6] = {
        READINESS_PARAM_ID_CT_CHANNEL_MAP_0, READINESS_PARAM_ID_CT_CHANNEL_MAP_1,
        READINESS_PARAM_ID_CT_CHANNEL_MAP_2, READINESS_PARAM_ID_I_NORMAL_A_0,
        READINESS_PARAM_ID_I_NORMAL_A_1,     READINESS_PARAM_ID_I_NORMAL_A_2,
    };
    for (int i = 0; i < 6; i++) {
        if (readiness_param_required_for_commissioning(ct_ids[i], ct_installed_value, 0)) {
            applicable++;
            unset++; /* these six are unset on this board */
        }
    }
    for (int i = 0; i < 59; i++) {
        /* every non-CT id is required and set on this board */
        applicable++;
    }

    TEST_CHECK(readiness_commissioning_status(true, 0xBEEF, unset, applicable) == READY_OK,
                "ct_installed=0 with only the 6 CT params unset must read ok");

    /* Same 65-param board but ct_installed == 1: now the six CT params ARE
     * applicable and still unset, so this must read not_done. */
    ct_installed_value = 1;
    applicable = 0;
    unset = 0;
    for (int i = 0; i < 6; i++) {
        if (readiness_param_required_for_commissioning(ct_ids[i], ct_installed_value, 0)) {
            applicable++;
            unset++;
        }
    }
    applicable += 59;
    TEST_CHECK(readiness_commissioning_status(true, 0xBEEF, unset, applicable) == READY_NOT_DONE,
                "ct_installed=1 with the same 6 params unset must still read not_done");

    /* A genuinely missing NON-CT param must still read not_done regardless of
     * ct_installed -- proves this is applicability, not a blanket
     * suppression of the whole item whenever ct_installed == 0. */
    applicable = 0;
    unset = 0;
    for (int i = 0; i < 6; i++) {
        if (readiness_param_required_for_commissioning(ct_ids[i], 0, 0)) {
            applicable++;
        }
    }
    applicable += 59; /* all 59 non-CT params applicable */
    unset = 1;         /* one of those 59 (a non-CT param) has no value */
    TEST_CHECK(readiness_commissioning_status(true, 0xBEEF, unset, applicable) == READY_NOT_DONE,
                "a missing non-CT param must read not_done even when ct_installed == 0");
}

/* Host tests for readiness_http.h's readiness_guard_max_temp_status()
 * (TODO.md 96, 2026-09-06): max_temp_c == 0 must read not_done -- not ok or
 * deliberately_off -- on any zone that can still heat, because
 * profile_executor_run()'s guard-5 refusal already blocks a firing start on
 * exactly that condition. Only when every zone with max_temp_c == 0 is OFF
 * (cannot heat, cannot trip the refusal) is 0 a legitimate, permanent
 * choice. */
void run_test_readiness_guard_max_temp(void)
{
    TEST_SECTION("readiness guard_max_temp item -- fail-open/fail-closed reconciliation");

    /* No zones configured yet: cannot_yet, same as every other item gated on
     * thermo_count. */
    TEST_CHECK(readiness_guard_max_temp_status(0, 0, 0) == READY_CANNOT_YET,
                "thermo_count == 0 must read cannot_yet");

    /* Every zone has an explicit ceiling: ok. */
    TEST_CHECK(readiness_guard_max_temp_status(3, 3, 0) == READY_OK,
                "all zones with an explicit ceiling must read ok");

    /* One heating zone with max_temp_c == 0: this is the exact case that
     * used to read ok/deliberately_off while a firing start would be
     * refused -- must now read not_done regardless of the other zones. */
    TEST_CHECK(readiness_guard_max_temp_status(3, 2, 1) == READY_NOT_DONE,
                "a heating zone with max_temp_c == 0 must read not_done, matching the start-time refusal");

    /* Every zone reads max_temp_c == 0, but none of them can heat (all
     * heating_unset_count == 0): a genuinely all-OFF, no-ceiling board is a
     * legitimate deliberately_off, not a nag. */
    TEST_CHECK(readiness_guard_max_temp_status(2, 0, 0) == READY_DELIBERATELY_OFF,
                "an all-OFF board with max_temp_c == 0 on every zone must read deliberately_off, not not_done");

    /* Mixed: one zone set, one zone OFF with max_temp_c == 0, no heating zone
     * left unset -- still a legitimate deliberately_off, not ok (set_count
     * != thermo_count) and not not_done (nothing heating is unset). */
    TEST_CHECK(readiness_guard_max_temp_status(2, 1, 0) == READY_DELIBERATELY_OFF,
                "a mix of an explicit ceiling and an OFF zone with none must read deliberately_off");
}

/* readiness_safety_trip_status(): 2026-09-08 live dry run
 * (docs/audits/setup_wizard_live_dryrun_2026-09-08.md) found the wizard
 * reporting complete=true, reasons=[] on a board with an ACTIVE safety trip
 * (S5 latched, diag_trip_mask nonzero) because no readiness item ever asked
 * whether the safety processor was currently tripped -- only whether the
 * link to it was up. These tests pin the fix. */
void run_test_readiness_safety_trip(void)
{
    TEST_SECTION("readiness safety_trip item -- active trip must block completeness");

    /* The exact live-board case this item exists to catch: link up, a guard
     * latched (any nonzero mask). Must be blocking, not a bare pass. */
    TEST_CHECK(readiness_safety_trip_status(true, 0x0010 /* S5 */) == READY_NOT_DONE,
                "an active trip with the link up must read not_done, not ok");

    /* Link up, nothing tripped: genuinely ready. */
    TEST_CHECK(readiness_safety_trip_status(true, 0x0000) == READY_OK,
                "no trip with the link up must read ok");

    /* Link down: the ESP cannot trust a stale/absent diag_trip_mask, so this
     * must read cannot_yet, not a confident ok -- and NOT not_done either,
     * since that would be nagging about a state that might already be fine. */
    TEST_CHECK(readiness_safety_trip_status(false, 0x0000) == READY_CANNOT_YET,
                "link down must read cannot_yet regardless of the cached mask");
    TEST_CHECK(readiness_safety_trip_status(false, 0x0010) == READY_CANNOT_YET,
                "link down must read cannot_yet even if a stale mask looks tripped");
}

/* readiness_crash_report_status(): 2026-09-08 follow-on to safety_trip
 * above -- capability_preflight already refuses to start a run on a board
 * with an unacknowledged crash; this item closes the gap so readiness
 * agrees rather than showing green while another layer refuses. */
void run_test_readiness_crash_report(void)
{
    TEST_SECTION("readiness crash_report item -- unacknowledged crash must block completeness");

    TEST_CHECK(readiness_crash_report_status(false, false) == READY_OK,
                "no crash record at all must read ok");
    TEST_CHECK(readiness_crash_report_status(true, false) == READY_NOT_DONE,
                "an unacknowledged crash record must read not_done, not ok");
    TEST_CHECK(readiness_crash_report_status(true, true) == READY_OK,
                "an acknowledged crash record must read ok");
}

/* readiness_recovery_mode_status(): a board in recovery mode has skipped
 * starting profile_executor/autotune_engine/rules_task and cannot fire
 * regardless of every other item's state. */
void run_test_readiness_recovery_mode(void)
{
    TEST_SECTION("readiness recovery_mode item -- recovery boot must block completeness");

    TEST_CHECK(readiness_recovery_mode_status(false) == READY_OK,
                "a normal boot must read ok");
    TEST_CHECK(readiness_recovery_mode_status(true) == READY_NOT_DONE,
                "a recovery-mode boot must read not_done, not ok");
}

/* readiness_cfg_fs_status(): owner decision 2026-10-06 ("Refuse, and prompt
 * the format"). cfg is the only save target, so an unmounted cfg means every
 * save route refuses (503). The item must be NOT_DONE (not OK, not the old
 * DELIBERATELY_OFF that hid it as informational) and its detail must point at
 * POST /api/cfgfs/format_confirm. It stays non-gating for firing: only the
 * four gate items block, and cfg_fs is not one of them. */
void run_test_readiness_cfg_fs(void)
{
    TEST_SECTION("readiness cfg_fs item -- unmounted must be not_done and name the format confirmation");

    TEST_CHECK(readiness_cfg_fs_status(true) == READY_OK,
                "a mounted cfg filesystem must read ok");
    TEST_CHECK(readiness_cfg_fs_status(false) == READY_NOT_DONE,
                "an unmounted/failed cfg filesystem must read not_done");

    const char *plain = readiness_cfg_fs_detail(false, false);
    const char *pending = readiness_cfg_fs_detail(false, true);
    const char *ok = readiness_cfg_fs_detail(true, false);
    TEST_CHECK(strstr(plain, "POST /api/cfgfs/format_confirm") != NULL,
                "unmounted detail must point at POST /api/cfgfs/format_confirm");
    TEST_CHECK(strstr(pending, "POST /api/cfgfs/format_confirm") != NULL,
                "format-pending detail must point at POST /api/cfgfs/format_confirm");
    TEST_CHECK(strstr(pending, "awaiting format confirmation") != NULL,
                "format-pending detail must say it is awaiting confirmation");
    TEST_CHECK(strstr(plain, "awaiting") == NULL, "plain unmounted detail must not claim a pending confirmation");
    TEST_CHECK(strstr(ok, "format_confirm") == NULL, "mounted detail must not nag about formatting");
    TEST_CHECK(strlen(plain) < 192 && strlen(pending) < 192, "details must fit READINESS_DETAIL_MAX (192)");
    /* The same remedy the save-refusal text gives. */
    TEST_CHECK(strstr(CFG_FS_NOT_MOUNTED_TEXT, "POST /api/cfgfs/format_confirm") != NULL,
                "CFG_FS_NOT_MOUNTED_TEXT must name the format-confirm route");
    TEST_CHECK(strstr(CFG_FS_NOT_MOUNTED_TEXT, "settings storage (cfg) not mounted") != NULL,
                "CFG_FS_NOT_MOUNTED_TEXT must name the cause");
}

/* readiness_safety_context_status(): the fourth 2026-09-08 blind spot -- a
 * Pico that keeps answering GET_STATUS while PUSH_CONTEXT delivery has
 * wedged must not read as healthy just because link_up is true. */
void run_test_readiness_safety_context(void)
{
    TEST_SECTION("readiness safety_context item -- wedged command delivery must block completeness");

    /* Link down: cannot tell wedged from unknown. */
    TEST_CHECK(readiness_safety_context_status(false, false, 0) == READY_CANNOT_YET,
                "link down must read cannot_yet");
    TEST_CHECK(readiness_safety_context_status(false, true, 254) == READY_CANNOT_YET,
                "link down must read cannot_yet even with a stale-looking cached age");

    /* Link up but no DIAG frame ever received: an older Pico build, or one
     * not yet heard from -- the age field is meaningless, must not read as
     * a false healthy ok. */
    TEST_CHECK(readiness_safety_context_status(true, false, 0) == READY_CANNOT_YET,
                "no DIAG frame yet must read cannot_yet, not a false ok");

    /* Link up, DIAG frames flowing, context fresh: genuinely healthy. */
    TEST_CHECK(readiness_safety_context_status(true, true, 0) == READY_OK,
                "a fresh context age must read ok");
    TEST_CHECK(readiness_safety_context_status(true, true, READINESS_CONTEXT_STALE_100MS - 1) == READY_OK,
                "just under the stale threshold must still read ok");

    /* The exact wedge scenario: GET_STATUS keeps answering (link_up true,
     * diag frames arriving) but the context age has grown past the
     * threshold -- commands are not landing. Must block, not read as a bare
     * pass the way link_up-only checks would. */
    TEST_CHECK(readiness_safety_context_status(true, true, READINESS_CONTEXT_STALE_100MS) == READY_NOT_DONE,
                "a context age at the stale threshold must read not_done");
    TEST_CHECK(readiness_safety_context_status(true, true, 254) == READY_NOT_DONE,
                "a saturated (254) context age with the link up must read not_done");
}

void run_test_readiness_estop_verification(void)
{
    TEST_SECTION("readiness estop_verified item -- unverified interlock must block completeness");

    /* Never confirmed -- the whole point of this item: pole 1 (the line
     * contactor coil) is wiring firmware cannot see, so there is no fact to
     * check other than the operator's own deliberate confirmation. */
    TEST_CHECK(readiness_estop_verification_status(false) == READY_NOT_DONE,
                "unverified must read not_done -- always, never a partial or informational status, "
                "same as safety_trip/crash_report/recovery_mode above. NOTE: that is a CHECKLIST "
                "status only -- nothing in firmware consumes /api/readiness, so this item does not "
                "stop a firing (readiness_http.h's top comment, docs/SAFETY_CASE.md sec 3 item 10)");

    /* Confirmed -- reads ok until something invalidates it (a polarity
     * commit, or a kiln/all-scope factory reset -- see estop_verification.h). */
    TEST_CHECK(readiness_estop_verification_status(true) == READY_OK,
                "a standing confirmation must read ok");
}
