// Host tests for App/drivers/persist/telemetry_format.c -- telemetry_format_firing()/
// telemetry_format_autotune(), the two pure line formatters telemetry_log.c
// emits over the debug UART (see that file's own doc comment for the
// dashboard_json.c-precedent reason these were split into their own no-
// FreeRTOS file). #includes telemetry_format.c directly, same convention as
// test_dashboard_json.c: exercises the REAL production function, not a
// reimplementation.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "test_common.h"

int g_test_failures = 0;
int g_test_count = 0;

#include "../drivers/persist/telemetry_format.c"

// Local mirror of telemetry_log.c's TELEMETRY_LOG_LINE_BUF (320) -- large
// enough to hold a real 3-zone/full-model line without truncating.
// telemetry_format.c itself has no such constant; that buffer is a task-
// file (telemetry_log.c) concern, not a formatter concern.
#define TELEMETRY_LOG_LINE_BUF_TEST 320

// ---------------------------------------------------------------------------
// telemetry_format_firing()
// ---------------------------------------------------------------------------

static void fill_firing_status(profile_exec_status_t *st)
{
    memset(st, 0, sizeof(*st));
    st->state = PROFILE_EXEC_RUNNING;
    st->profile_id = 7;
    st->segment_index = 2;
    st->dwelling = false;
    st->target_c = 845.5f;
    st->total_elapsed_s = 1234;

    st->zones[0].active = true;
    st->zones[0].actual_c = 840.7f;
    st->zones[0].actual_valid = true;
    st->zones[0].duty = 0.625f;
    st->zones[0].ff_hold_used_matrix = true;
    st->zones[0].ff_hold_infeasible = false;

    st->zones[1].active = true;
    st->zones[1].actual_c = 0.0f;
    st->zones[1].actual_valid = false; /* invalid reading -- guard case */
    st->zones[1].duty = 0.0f;

    st->zones[2].active = false; /* not participating in this run -- must be skipped */
    st->zones[2].actual_c = 999.0f; /* poison value: must never appear in the line */
}

static void test_firing_well_formed(void)
{
    TEST_SECTION("telemetry_format_firing() -- well-formed line, 3-zone status (one inactive, one invalid)");

    profile_exec_status_t st;
    fill_firing_status(&st);

    char out[TELEMETRY_LOG_LINE_BUF_TEST];
    int n = telemetry_format_firing(&st, out, sizeof(out));

    TEST_CHECK(n > 0, "positive length returned");
    TEST_CHECK((size_t)n == strlen(out), "returned length matches actual written length (no truncation)");
    TEST_CHECK(strstr(out, "KTEL1 FIRE") != NULL, "starts with the schema-versioned FIRE tag");
    TEST_CHECK(strstr(out, "t=1234") != NULL, "elapsed time present");
    TEST_CHECK(strstr(out, "pid=7") != NULL, "profile id present");
    TEST_CHECK(strstr(out, "seg=2") != NULL, "segment index present");
    TEST_CHECK(strstr(out, "dwell=0") != NULL, "dwelling flag present");
    TEST_CHECK(strstr(out, "tgt=845.50") != NULL, "ramped target (not a segment endpoint) present");

    // Zone 0: valid reading -- error is actual - target = 840.70 - 845.50 = -4.80
    TEST_CHECK(strstr(out, "z0_c=840.70") != NULL, "zone 0 actual_c present");
    TEST_CHECK(strstr(out, "z0_v=1") != NULL, "zone 0 actual_valid=1");
    TEST_CHECK(strstr(out, "z0_e=-4.80") != NULL, "zone 0 error computed as actual - target");
    TEST_CHECK(strstr(out, "z0_d=0.625") != NULL, "zone 0 duty present");
    TEST_CHECK(strstr(out, "z0_fm=1") != NULL, "zone 0 ff_hold_used_matrix present");
    TEST_CHECK(strstr(out, "z0_fi=0") != NULL, "zone 0 ff_hold_infeasible present");

    // Zone 1: invalid reading -- error must read as 0, not a stale/garbage
    // actual_c, and actual_valid must say so.
    TEST_CHECK(strstr(out, "z1_v=0") != NULL, "zone 1 actual_valid=0 (invalid reading)");
    TEST_CHECK(strstr(out, "z1_e=0.00") != NULL, "zone 1 error reads 0.00 when the reading is invalid, not a bogus delta");

    // Zone 2 is !active and must be skipped entirely -- its poison value
    // (999.0) must never appear anywhere in the line.
    TEST_CHECK(strstr(out, "z2_") == NULL, "inactive zone 2 is skipped entirely");
    TEST_CHECK(strstr(out, "999.0") == NULL, "inactive zone's poison actual_c never leaks into the line");
}

static void test_firing_all_invalid(void)
{
    TEST_SECTION("telemetry_format_firing() -- every active zone's reading invalid");

    profile_exec_status_t st;
    memset(&st, 0, sizeof(st));
    st.state = PROFILE_EXEC_RUNNING;
    st.target_c = 500.0f;
    st.zones[0].active = true;
    st.zones[0].actual_valid = false;

    char out[TELEMETRY_LOG_LINE_BUF_TEST];
    int n = telemetry_format_firing(&st, out, sizeof(out));

    TEST_CHECK(n > 0, "still produces a line (header + the one zone) even with no valid reading");
    TEST_CHECK(strstr(out, "z0_v=0") != NULL, "zone 0 reported invalid");
    TEST_CHECK(strstr(out, "z0_c=0.00") != NULL, "zone 0 actual_c reads 0.00, not a NaN/garbage value, when invalid");
}

// ---------------------------------------------------------------------------
// telemetry_format_autotune()
// ---------------------------------------------------------------------------

static void test_autotune_stepping(void)
{
    TEST_SECTION("telemetry_format_autotune() -- STEPPING, no model yet (in-progress line)");

    autotune_engine_status_t st;
    memset(&st, 0, sizeof(st));
    st.state = AUTOTUNE_ENGINE_STEPPING;
    st.method = AUTOTUNE_METHOD_STEP;
    st.zone_index = 1;
    st.elapsed_s = 620;
    st.sample_count = 62;
    st.actual_c = 612.3f;
    st.actual_valid = true;
    st.duty = 0.5f;

    char out[TELEMETRY_LOG_LINE_BUF_TEST];
    int n = telemetry_format_autotune(&st, out, sizeof(out));

    TEST_CHECK(n > 0, "positive length returned");
    TEST_CHECK((size_t)n == strlen(out), "returned length matches actual written length");
    TEST_CHECK(strstr(out, "KTEL1 TUNE") != NULL, "starts with the schema-versioned TUNE tag");
    TEST_CHECK(strstr(out, "zone=1") != NULL, "zone index present");
    TEST_CHECK(strstr(out, "n=62") != NULL, "sample_count present");
    TEST_CHECK(strstr(out, "c=612.30") != NULL, "actual_c present");
    TEST_CHECK(strstr(out, "duty=0.500") != NULL, "commanded duty present");
    // No model fields on an in-progress line -- DONE-only content must be absent.
    TEST_CHECK(strstr(out, "k=") == NULL, "no fitted-model fields while still STEPPING");
}

static void test_autotune_done_step_model(void)
{
    TEST_SECTION("telemetry_format_autotune() -- DONE, STEP method, full fitted model + proposed gains");

    autotune_engine_status_t st;
    memset(&st, 0, sizeof(st));
    st.state = AUTOTUNE_ENGINE_DONE;
    st.method = AUTOTUNE_METHOD_STEP;
    st.zone_index = 0;
    st.elapsed_s = 910;
    st.sample_count = 91;
    st.actual_c = 705.0f;
    st.actual_valid = true;
    st.duty = 0.5f;
    st.step_ambient_c = 22.5f;

    st.model.valid = true;
    st.model.k_gain_c_per_duty = 812.3456f;
    st.model.tau_s = 1420.5f;
    st.model.dead_time_s = 38.2f;
    st.model.baseline_c = 400.0f;
    st.model.final_c = 705.0f;
    st.model.raw_rise_c = 305.0f;
    st.model.rise_inf_c = 320.0f;
    st.model.settled = true;
    st.model.tau_consistent_with_gain = true;
    st.model.extrapolation_converged = false;

    st.proposed_gains.kp = 0.0123f;
    st.proposed_gains.ki = 0.0004f;
    st.proposed_gains.kd = 1.5f;
    st.proposed_gains.rule = AUTOTUNE_RULE_SIMC;
    st.proposed_gains.refusal = AUTOTUNE_REFUSAL_OK;

    char out[TELEMETRY_LOG_LINE_BUF_TEST];
    int n = telemetry_format_autotune(&st, out, sizeof(out));

    TEST_CHECK(n > 0, "positive length returned");
    TEST_CHECK((size_t)n == strlen(out), "returned length matches actual written length (no truncation)");
    TEST_CHECK(strstr(out, "k=812.3456") != NULL, "k_gain_c_per_duty present at full precision");
    TEST_CHECK(strstr(out, "tau=1420.5") != NULL, "tau_s present");
    TEST_CHECK(strstr(out, "l=38.2") != NULL, "dead_time_s present");
    TEST_CHECK(strstr(out, "amb=22.50") != NULL, "step_ambient_c present (cold-junction reference, distinct from baseline_c)");
    TEST_CHECK(strstr(out, "settled=1") != NULL, "settled flag present");
    TEST_CHECK(strstr(out, "tauok=1") != NULL, "tau_consistent_with_gain present");
    TEST_CHECK(strstr(out, "conv=0") != NULL, "extrapolation_converged present and correctly false here");
    TEST_CHECK(strstr(out, "kp=0.0123") != NULL, "proposed kp present");
    TEST_CHECK(strstr(out, "reason=") == NULL, "no abort reason on a DONE (not ABORTED) line");
}

static void test_autotune_aborted_sanitizes_reason(void)
{
    TEST_SECTION("telemetry_format_autotune() -- ABORTED, abort_reason sanitized against embedded spaces/quotes/'='");

    autotune_engine_status_t st;
    memset(&st, 0, sizeof(st));
    st.state = AUTOTUNE_ENGINE_ABORTED;
    st.method = AUTOTUNE_METHOD_STEP;
    strncpy(st.abort_reason, "guard 6 tripped: sensor=\"open\" T=999", sizeof(st.abort_reason) - 1);

    char out[TELEMETRY_LOG_LINE_BUF_TEST];
    int n = telemetry_format_autotune(&st, out, sizeof(out));

    TEST_CHECK(n > 0, "positive length returned");
    const char *reason_kv = strstr(out, "reason=");
    TEST_CHECK(reason_kv != NULL, "reason= key present for an ABORTED line");
    if (reason_kv != NULL) {
        // Everything after "reason=" to end of line must contain no raw
        // space/tab/'='/'"' -- a naive whitespace-split parser must be able
        // to treat this as exactly one token.
        const char *p = reason_kv + strlen("reason=");
        bool clean = true;
        for (; *p != '\0'; p++) {
            if (*p == ' ' || *p == '\t' || *p == '"' || *p == '=') {
                clean = false;
                break;
            }
        }
        TEST_CHECK(clean, "sanitized reason contains no raw space/tab/quote/'=' that would break key=value parsing");
    }
}

// ---------------------------------------------------------------------------
// Buffer-overflow safety, negative-tested (repo rule: "prove a new check can
// fail"). Both formatters are exercised against buffers far smaller than a
// real line needs: correct behavior is "truncate safely, NUL-terminate,
// never write past cap-1, and report via the snprintf-style return value
// that this was truncated (return >= cap)". Deliberately undersized down to
// cap==1 and cap==0, the two edges most likely to be gotten wrong.
// ---------------------------------------------------------------------------

// Canary bytes past the nominal buffer end -- if either formatter ever
// writes past `cap`, this sentinel (never itself explicitly written by
// production code) is what a real overflow would clobber, making the test
// fail even on a build where the OS happens not to fault on the stray
// write.
#define CANARY 0x5A

static void test_firing_undersized_buffer_never_overflows(void)
{
    TEST_SECTION("telemetry_format_firing() -- deliberately undersized buffer never overflows");

    profile_exec_status_t st;
    fill_firing_status(&st);

    for (size_t cap = 0; cap <= 8; cap++) {
        char buf[16];
        memset(buf, CANARY, sizeof(buf));
        int n = telemetry_format_firing(&st, buf, cap);

        // Real full-line length must be far bigger than any cap tested here.
        TEST_CHECK((size_t)n > cap || cap == 0, "return value signals truncation (n >= cap) for an undersized buffer");

        if (cap > 0) {
            // NUL-terminated somewhere within [0, cap-1].
            bool terminated = false;
            for (size_t i = 0; i < cap; i++) {
                if (buf[i] == '\0') {
                    terminated = true;
                    break;
                }
            }
            TEST_CHECK(terminated, "buffer is NUL-terminated within [0, cap) even when truncated");
        }
        // Nothing beyond byte index `cap` may ever have been touched --
        // every canary byte from cap onward must be untouched.
        bool canaries_intact = true;
        for (size_t i = cap; i < sizeof(buf); i++) {
            if ((unsigned char)buf[i] != CANARY) {
                canaries_intact = false;
                break;
            }
        }
        TEST_CHECK(canaries_intact, "no byte at or past cap was ever written (no overflow)");
    }
}

static void test_autotune_undersized_buffer_never_overflows(void)
{
    TEST_SECTION("telemetry_format_autotune() -- deliberately undersized buffer never overflows (DONE, longest shape)");

    autotune_engine_status_t st;
    memset(&st, 0, sizeof(st));
    st.state = AUTOTUNE_ENGINE_DONE;
    st.method = AUTOTUNE_METHOD_STEP;
    st.model.valid = true;
    st.model.k_gain_c_per_duty = 812.3456f;
    st.model.tau_s = 1420.5f;
    st.model.dead_time_s = 38.2f;

    for (size_t cap = 0; cap <= 10; cap++) {
        char buf[16];
        memset(buf, CANARY, sizeof(buf));
        int n = telemetry_format_autotune(&st, buf, cap);

        TEST_CHECK((size_t)n > cap || cap == 0, "return value signals truncation (n >= cap) for an undersized buffer");

        if (cap > 0) {
            bool terminated = false;
            for (size_t i = 0; i < cap; i++) {
                if (buf[i] == '\0') {
                    terminated = true;
                    break;
                }
            }
            TEST_CHECK(terminated, "buffer is NUL-terminated within [0, cap) even when truncated");
        }
        bool canaries_intact = true;
        for (size_t i = cap; i < sizeof(buf); i++) {
            if ((unsigned char)buf[i] != CANARY) {
                canaries_intact = false;
                break;
            }
        }
        TEST_CHECK(canaries_intact, "no byte at or past cap was ever written (no overflow)");
    }
}

// ---------------------------------------------------------------------------
// PID_EXPANSION_PLAN.md sec 7.1/7.4: telemetry_ramp_lag_encode_rate_byte()/
// telemetry_ramp_lag_event_for_transition() -- the EVENT_CODE_FIRING_RAMP_
// LAG_STARTED/CLEARED note encoding and edge detector, split into this
// already-host-tested file for the same reason telemetry_format_firing()
// above was (telemetry_log.c itself needs FreeRTOS/ESP_LOGI).

static void test_encode_rate_byte_never_produces_zero(void)
{
    TEST_SECTION("telemetry_ramp_lag_encode_rate_byte() -- must never return 0 "
                 "(event_log_emit()'s strncpy() would truncate the note on an embedded NUL)");
    TEST_CHECK(telemetry_ramp_lag_encode_rate_byte(0.0f) == 128, "0 C/hr encodes to the +128 offset midpoint");
    TEST_CHECK(telemetry_ramp_lag_encode_rate_byte(-128.0f) != 0,
              "the exact value that WOULD raw-encode to 0 (-128 + 128) must be clamped away from it");
    TEST_CHECK(telemetry_ramp_lag_encode_rate_byte(-128.0f) == 2, "clamped to -126, encodes to 2");
    TEST_CHECK(telemetry_ramp_lag_encode_rate_byte(9999.0f) == 254, "large positive rate clamps to 126 -> 254");
    TEST_CHECK(telemetry_ramp_lag_encode_rate_byte(-9999.0f) == 2, "large negative rate clamps to -126 -> 2");
    TEST_CHECK(telemetry_ramp_lag_encode_rate_byte(50.4f) == 178, "rounds to nearest whole degree before offsetting");
}

static void test_lag_event_no_edge_returns_none(void)
{
    TEST_SECTION("telemetry_ramp_lag_event_for_transition() -- prev==cur is never an event");
    int32_t arg = -1;
    uint8_t note[EVENT_LOG_NOTE_LEN];
    memset(note, 0xAA, sizeof(note));
    int code = telemetry_ramp_lag_event_for_transition(false, false, 200.0f, 10.0f, 8.0f, 0.0f, 0.0f, 0.0f,
                                                        &arg, note);
    TEST_CHECK(code == -1, "sustained false->false must not be an event");
    code = telemetry_ramp_lag_event_for_transition(true, true, 200.0f, 10.0f, 8.0f, 30.0f, 10.0f, 8.0f,
                                                    &arg, note);
    TEST_CHECK(code == -1, "sustained true->true must not be an event");
}

static void test_lag_event_started_uses_current_fields(void)
{
    TEST_SECTION("telemetry_ramp_lag_event_for_transition() -- rising edge (STARTED) reports the "
                 "CURRENT tick's temperature and rates, arg = actual_c*100");
    int32_t arg = 0;
    uint8_t note[EVENT_LOG_NOTE_LEN];
    memset(note, 0xAA, sizeof(note));
    int code = telemetry_ramp_lag_event_for_transition(false, true, 312.5f, 60.0f, 12.0f,
                                                        /*prev_held_s*/ 0.0f, /*prev_commanded*/ 0.0f,
                                                        /*prev_achieved*/ 0.0f, &arg, note);
    TEST_CHECK(code == EVENT_CODE_FIRING_RAMP_LAG_STARTED, "rising edge must report STARTED");
    TEST_CHECK(arg == 31250, "arg must be actual_c*100 rounded (312.5 -> 31250)");
    TEST_CHECK(note[0] == telemetry_ramp_lag_encode_rate_byte(60.0f), "note[0] must be the CURRENT commanded rate");
    TEST_CHECK(note[1] == telemetry_ramp_lag_encode_rate_byte(12.0f), "note[1] must be the CURRENT achieved rate");
}

static void test_lag_event_cleared_uses_remembered_prev_fields(void)
{
    TEST_SECTION("telemetry_ramp_lag_event_for_transition() -- falling edge (CLEARED) reports the "
                 "CALLER-REMEMBERED previous fields, NOT the current (already-reset-to-0) ones -- "
                 "this is the bug this function's own doc comment exists to prevent");
    int32_t arg = 0;
    uint8_t note[EVENT_LOG_NOTE_LEN];
    memset(note, 0xAA, sizeof(note));
    /* Current-tick fields are 0/0 (as profile_exec_zone_status_t genuinely
     * reads the instant ramp_lag_sustained clears) -- only the prev_*
     * arguments carry real numbers. */
    int code = telemetry_ramp_lag_event_for_transition(true, false, 305.0f, /*cur_commanded*/ 0.0f,
                                                        /*cur_achieved*/ 0.0f, /*prev_held_s*/ 145.0f,
                                                        /*prev_commanded*/ 60.0f, /*prev_achieved*/ 22.0f,
                                                        &arg, note);
    TEST_CHECK(code == EVENT_CODE_FIRING_RAMP_LAG_CLEARED, "falling edge must report CLEARED");
    TEST_CHECK(arg == 145, "arg must be the remembered held duration, not 0");
    TEST_CHECK(note[0] == telemetry_ramp_lag_encode_rate_byte(60.0f),
              "note[0] must be the REMEMBERED commanded rate (60), not the current-tick 0");
    TEST_CHECK(note[1] == telemetry_ramp_lag_encode_rate_byte(22.0f),
              "note[1] must be the REMEMBERED achieved rate (22), not the current-tick 0");
}

int main(void)
{
    test_firing_well_formed();
    test_firing_all_invalid();
    test_autotune_stepping();
    test_autotune_done_step_model();
    test_autotune_aborted_sanitizes_reason();
    test_firing_undersized_buffer_never_overflows();
    test_autotune_undersized_buffer_never_overflows();
    test_encode_rate_byte_never_produces_zero();
    test_lag_event_no_edge_returns_none();
    test_lag_event_started_uses_current_fields();
    test_lag_event_cleared_uses_remembered_prev_fields();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures == 0 ? 0 : 1;
}
