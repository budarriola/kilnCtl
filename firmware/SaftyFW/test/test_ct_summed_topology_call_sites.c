// test_ct_summed_topology_call_sites.c -- 2026-09-18 owner-directed fix,
// summed-CT-topology support. The behavioural coverage for the two shared
// predicates lives in test_config_store_ct_channel_fitted.c (pure functions,
// host-compilable). This file instead pins that the four real production
// call sites actually ROUTE THROUGH those shared functions rather than
// re-deriving the "is this channel fitted" / "mask current-present" rules
// inline -- the coordinator's explicit centralization requirement, and the
// same "reset one side of a pair" drift class documented elsewhere in this
// codebase. safety_core.c is not host-compilable (FreeRTOS/pico-sdk), so
// this is a source-text scan, same precedent as
// test_safety_core_ct_calibration_gate.c / test_safety_core_polarity_wiring.c.
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "test_common.h"

static char *read_file_any(const char *const *candidates, size_t count)
{
    return test_read_source_anchored(__FILE__, candidates[0], candidates, count);
}

static void test_safety_core_commissioned_gate_uses_shared_helper(void)
{
    TEST_SECTION("safety_core.c: s_current_sensing_commissioned must be assigned via "
                 "config_store_current_sensing_commissioned(), not a re-derived all-three "
                 "check");

    static const char *candidates[] = {
        "../src/tasks/safety_core.c",
        "src/tasks/safety_core.c",
        "firmware/SaftyFW/src/tasks/safety_core.c",
    };
    char *text = read_file_any(candidates, 3);
    if (!text) {
        TEST_CHECK(false, "could not locate src/tasks/safety_core.c");
        return;
    }

    TEST_CHECK(strstr(text, "s_current_sensing_commissioned = "
                            "config_store_current_sensing_commissioned(rec);") != NULL,
               "s_current_sensing_commissioned must be assigned from "
               "config_store_current_sensing_commissioned(rec) -- if this fails, the gate "
               "was re-inlined and can drift back to requiring all three channels "
               "regardless of topology, permanently disabling S9 on SUMMED boards again");

    free(text);
}

static void test_safety_core_present_masking_uses_shared_helper(void)
{
    TEST_SECTION("safety_core.c: any_current_present must be masked via "
                 "config_store_mask_current_present_to_fitted(), AND'd (narrowing only) "
                 "into the existing value");

    static const char *candidates[] = {
        "../src/tasks/safety_core.c",
        "src/tasks/safety_core.c",
        "firmware/SaftyFW/src/tasks/safety_core.c",
    };
    char *text = read_file_any(candidates, 3);
    if (!text) {
        TEST_CHECK(false, "could not locate src/tasks/safety_core.c");
        return;
    }

    const char *mask_call = strstr(
        text, "config_store_mask_current_present_to_fitted(current.present, !cts_disabled,");
    TEST_CHECK(mask_call != NULL,
               "any_current_present's masking must call "
               "config_store_mask_current_present_to_fitted(current.present, !cts_disabled, "
               "cfg_rec.ct_topology) -- if this fails, unfitted channels' idle ADC noise "
               "(16-17 counts against a 25-count margin) can set any_current_present true "
               "again, letting the unclearable S9 latch arm and trip off noise on a SUMMED "
               "board");

    // Must be AND'd into the existing any_current_present, not a bare
    // overwrite -- narrowing only, never widening past current_fresh/
    // cts_disabled's existing gates. Checked by scanning backward from the
    // mask call itself for "any_current_present &&" within a small window,
    // rather than pinning the exact whitespace between them (indentation
    // is not part of the contract being protected here).
    bool and_before_mask = false;
    if (mask_call != NULL) {
        size_t back_window = 40;
        const char *scan_start =
            (size_t)(mask_call - text) > back_window ? mask_call - back_window : text;
        char window[64];
        size_t win_len = (size_t)(mask_call - scan_start);
        if (win_len >= sizeof(window)) {
            win_len = sizeof(window) - 1;
        }
        memcpy(window, scan_start, win_len);
        window[win_len] = '\0';
        and_before_mask = (strstr(window, "any_current_present &&") != NULL);
    }
    TEST_CHECK(and_before_mask,
               "the masking assignment must read `any_current_present = "
               "any_current_present && config_store_mask_current_present_to_fitted(...)` -- "
               "an overwrite instead of an AND could widen any_current_present back past "
               "the current_fresh/cts_disabled gates already applied above it");

    free(text);
}

static void test_safety_core_s14_loop_uses_shared_helper(void)
{
    TEST_SECTION("safety_core.c: the S14 per-channel loop must skip unfitted channels via "
                 "config_store_ct_channel_fitted(), not a re-derived "
                 "`ct_topology == SUMMED && ch != 2` inline check");

    static const char *candidates[] = {
        "../src/tasks/safety_core.c",
        "src/tasks/safety_core.c",
        "firmware/SaftyFW/src/tasks/safety_core.c",
    };
    char *text = read_file_any(candidates, 3);
    if (!text) {
        TEST_CHECK(false, "could not locate src/tasks/safety_core.c");
        return;
    }

    TEST_CHECK(strstr(text, "if (!config_store_ct_channel_fitted((uint8_t)ch, true, "
                            "cfg_rec.ct_topology)) {") != NULL,
               "the S14 loop must skip a channel via "
               "config_store_ct_channel_fitted((uint8_t)ch, true, cfg_rec.ct_topology) -- "
               "if this fails, the inline `ct_summed && ch != 2` duplicate may have come "
               "back, which is exactly the two-sites-drift class this refactor closed");

    TEST_CHECK(strstr(text, "bool ct_summed = ") == NULL,
               "the old local `ct_summed` variable should be gone entirely now that the S14 "
               "loop is routed through config_store_ct_channel_fitted() -- its reappearance "
               "means the rule is being re-derived in a second place again");

    free(text);
}

static void test_current_task_any_present_uses_shared_helper(void)
{
    TEST_SECTION("current_task.c: current_task_any_current_present() must call "
                 "config_store_mask_current_present_to_fitted(), the second mandatory "
                 "masking site (link_task.c's tc_type-change gate depends on it)");

    static const char *candidates[] = {
        "../src/tasks/current_task.c",
        "src/tasks/current_task.c",
        "firmware/SaftyFW/src/tasks/current_task.c",
    };
    char *text = read_file_any(candidates, 3);
    if (!text) {
        TEST_CHECK(false, "could not locate src/tasks/current_task.c");
        return;
    }

    const char *fn_sig = strstr(text, "current_task_any_current_present(void)");
    TEST_CHECK(fn_sig != NULL, "current_task_any_current_present() must still exist");

    const char *mask_call =
        fn_sig ? strstr(fn_sig, "config_store_mask_current_present_to_fitted(") : NULL;
    TEST_CHECK(mask_call != NULL,
               "current_task_any_current_present() must call "
               "config_store_mask_current_present_to_fitted() -- if this fails, "
               "link_task.c's heat-safe-for-tc-type-change gate (which reads this function) "
               "can again see unfitted-channel noise as genuine current presence on a "
               "SUMMED board");

    free(text);
}

void run_test_ct_summed_topology_call_sites(void)
{
    test_safety_core_commissioned_gate_uses_shared_helper();
    test_safety_core_present_masking_uses_shared_helper();
    test_safety_core_s14_loop_uses_shared_helper();
    test_current_task_any_present_uses_shared_helper();
}
