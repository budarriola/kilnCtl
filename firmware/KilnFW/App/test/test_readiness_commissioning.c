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
#include "../drivers/readiness_http.h"

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
        TEST_CHECK(readiness_param_required_for_commissioning(ct_ids[i], 0) == false,
                    "a CT param must not be required when ct_installed == 0");
    }

    /* ct_installed != 0: all six become required again -- ct_installed == 0
     * must not turn into a blanket "CT params never matter" suppression. */
    for (int i = 0; i < 6; i++) {
        TEST_CHECK(readiness_param_required_for_commissioning(ct_ids[i], 1) == true,
                    "a CT param must be required when ct_installed != 0");
    }

    /* A non-CT param (abs_max_temp_c, id 0x0104) is required either way --
     * this is what proves the rule is applicability, not a global relaxation
     * whenever ct_installed happens to be 0. */
    TEST_CHECK(readiness_param_required_for_commissioning(0x0104u, 0) == true,
                "a non-CT param must stay required when ct_installed == 0");
    TEST_CHECK(readiness_param_required_for_commissioning(0x0104u, 1) == true,
                "a non-CT param must stay required when ct_installed != 0");
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
        if (readiness_param_required_for_commissioning(ct_ids[i], ct_installed_value)) {
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
        if (readiness_param_required_for_commissioning(ct_ids[i], ct_installed_value)) {
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
        if (readiness_param_required_for_commissioning(ct_ids[i], 0)) {
            applicable++;
        }
    }
    applicable += 59; /* all 59 non-CT params applicable */
    unset = 1;         /* one of those 59 (a non-CT param) has no value */
    TEST_CHECK(readiness_commissioning_status(true, 0xBEEF, unset, applicable) == READY_NOT_DONE,
                "a missing non-CT param must read not_done even when ct_installed == 0");
}
