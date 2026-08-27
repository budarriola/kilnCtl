// Host tests for App/drivers/profile_executor.h's profile_executor_wd_decide()
// -- the pure decision core pulled out of profile_executor.c's
// watchdog_task_entry() (guard 9) so it could be tested without pulling in
// FreeRTOS/kiln_io/relay_authority/etc, which the rest of that file needs and
// which this host harness has no stubs for. Mirrors test_heat_interlock.c/
// test_thermal_guard.c's own pattern: exercise the pure function directly
// with a snapshot struct, no ESP-IDF dependency.
//
// What this closes: the safety processor actively tripping while the link
// stays HEALTHY used to be invisible to this watchdog entirely -- it never
// read diag_state/diag_trip_reason, so relay_authority would block new
// relay-on but the run itself kept advancing its schedule and both GUIs kept
// showing a firing in progress while the kiln cooled. These tests cover: a
// live trip while RUNNING faults the run with a worded reason; the wording
// distinguishes a trip from a dead link; a trip while IDLE does not
// fabricate a run; and the existing 30s-silence path still works
// (regression -- this refactor must not have broken it).
#include <string.h>

#include "test_common.h"
#include "../drivers/profile_executor.h"
#include "../drivers/safety_trip_words.h"

static profile_executor_wd_input_t base_input(void)
{
    profile_executor_wd_input_t in;
    memset(&in, 0, sizeof(in));
    return in;
}

static void test_safety_trip_faults_running(void)
{
    TEST_SECTION("profile_executor_wd_decide -- safety trip while RUNNING faults the run");

    profile_executor_wd_input_t in = base_input();
    in.safety_processor_tripped = true;
    in.safety_trip_reason = 2; // SAFETY_TRIP_OVER_SETPOINT (S2)
    in.state_running_or_paused = true;

    profile_executor_wd_result_t out = profile_executor_wd_decide(&in);

    TEST_CHECK(out.action == PROFILE_EXECUTOR_WD_ACTION_FAULT,
               "a live safety trip while RUNNING/PAUSED aborts the firing");
    TEST_CHECK(strstr(out.fault_reason, "safety processor tripped") != NULL,
               "fault_reason names the cause as a safety-processor trip");
    TEST_CHECK(strstr(out.fault_reason, "over setpoint") != NULL,
               "fault_reason names the trip in WORDS, not a bare numeric code");
    TEST_CHECK(strstr(out.fault_reason, "2") == NULL || strstr(out.fault_reason, "(S2)") != NULL,
               "any digit present is part of the guard label (S2), not a bare reason code");
}

static void test_safety_trip_faults_paused(void)
{
    TEST_SECTION("profile_executor_wd_decide -- safety trip while PAUSED also faults");

    profile_executor_wd_input_t in = base_input();
    in.safety_processor_tripped = true;
    in.safety_trip_reason = 8; // SAFETY_TRIP_ESTOP (S7)
    in.state_running_or_paused = true;

    profile_executor_wd_result_t out = profile_executor_wd_decide(&in);

    TEST_CHECK(out.action == PROFILE_EXECUTOR_WD_ACTION_FAULT, "PAUSED is treated the same as RUNNING");
    TEST_CHECK(strstr(out.fault_reason, "E-stop") != NULL, "reason names the E-stop guard");
}

static void test_trip_wording_distinct_from_silent_link(void)
{
    TEST_SECTION("profile_executor_wd_decide -- trip wording differs from the silent-link wording");

    profile_executor_wd_input_t trip_in = base_input();
    trip_in.safety_processor_tripped = true;
    trip_in.safety_trip_reason = 1; // overtemp
    trip_in.state_running_or_paused = true;
    profile_executor_wd_result_t trip_out = profile_executor_wd_decide(&trip_in);

    profile_executor_wd_input_t silent_in = base_input();
    silent_in.safety_link_silent_30s = true;
    silent_in.state_running_or_paused = true;
    profile_executor_wd_result_t silent_out = profile_executor_wd_decide(&silent_in);

    TEST_CHECK(strcmp(trip_out.fault_reason, silent_out.fault_reason) != 0,
               "a guard firing and a dead peer produce different fault_reason text");
    TEST_CHECK(strstr(trip_out.fault_reason, "tripped") != NULL,
               "the trip case says 'tripped' (a guard fired)");
    TEST_CHECK(strstr(silent_out.fault_reason, "silent") != NULL,
               "the silence case says 'silent' (the peer went quiet), not 'tripped'");
}

static void test_already_faulted_retries_relay_off_without_reason_change(void)
{
    TEST_SECTION("profile_executor_wd_decide -- trip still latched while already FAULTED retries relay-off only");

    profile_executor_wd_input_t in = base_input();
    in.safety_processor_tripped = true;
    in.safety_trip_reason = 1;
    in.state_faulted = true; // already faulted, not running/paused

    profile_executor_wd_result_t out = profile_executor_wd_decide(&in);

    TEST_CHECK(out.action == PROFILE_EXECUTOR_WD_ACTION_RETRY_RELAYS_OFF,
               "an already-FAULTED run with the trip still latched keeps retrying relay-off, "
               "does not re-fault or touch fault_reason");
    TEST_CHECK(out.fault_reason[0] == '\0', "fault_reason is left untouched (empty) on a retry action");
}

static void test_idle_trip_does_not_fabricate_run(void)
{
    TEST_SECTION("profile_executor_wd_decide -- trip while IDLE does not fabricate a FAULTED run");

    profile_executor_wd_input_t in = base_input();
    in.safety_processor_tripped = true;
    in.safety_trip_reason = 1;
    // state_running_or_paused and state_faulted both false -> IDLE/DONE

    profile_executor_wd_result_t out = profile_executor_wd_decide(&in);

    TEST_CHECK(out.action == PROFILE_EXECUTOR_WD_ACTION_LOG_IDLE_TRIP,
               "a trip reported with nothing running/paused/faulted only logs -- it never "
               "manufactures a run or a fault_reason out of nothing");
    TEST_CHECK(out.fault_reason[0] == '\0', "no fault_reason is produced for the idle-trip case");
}

static void test_no_trip_no_silence_idle_is_a_true_no_op(void)
{
    TEST_SECTION("profile_executor_wd_decide -- nothing wrong, idle -- true no-op");

    profile_executor_wd_input_t in = base_input();
    profile_executor_wd_result_t out = profile_executor_wd_decide(&in);

    TEST_CHECK(out.action == PROFILE_EXECUTOR_WD_ACTION_NONE, "a healthy, untripped, idle tick does nothing");
}

// ---- Regression: the pre-existing 30s-silence abort path must still work
// exactly as before this refactor (the same behavior now lives inside this
// pure function instead of an inline if/else chain in watchdog_task_entry()).
static void test_silent_link_30s_still_faults_running(void)
{
    TEST_SECTION("profile_executor_wd_decide -- REGRESSION: 30s link silence still faults a RUNNING firing");

    profile_executor_wd_input_t in = base_input();
    in.safety_link_silent_30s = true;
    in.state_running_or_paused = true;

    profile_executor_wd_result_t out = profile_executor_wd_decide(&in);

    TEST_CHECK(out.action == PROFILE_EXECUTOR_WD_ACTION_FAULT, "30s of silence still aborts a RUNNING firing");
    TEST_CHECK(strstr(out.fault_reason, "silent") != NULL, "fault_reason still describes link silence");
}

static void test_silent_link_brief_does_not_fault(void)
{
    TEST_SECTION("profile_executor_wd_decide -- a BRIEF safety-link silence (not yet 30s) does NOT abort");

    // The nuisance-abort case this task's brief calls out explicitly: a
    // single dropped telemetry frame recovers well inside the 30s window,
    // and the caller's own safety_link_is_stale(age, SAFETY_LINK_FIRING_
    // ABORT_SILENCE_MS) check is what keeps safety_link_silent_30s false
    // until the outage is actually sustained -- this function must treat
    // that false exactly like "nothing wrong", same as the PC-link analog
    // (test_pc_link_brief_loss_does_not_fault) below. A twelve-hour firing
    // must survive one missed frame.
    profile_executor_wd_input_t in = base_input();
    in.safety_link_silent_30s = false; // caller's own timer hasn't crossed 30s
    in.state_running_or_paused = true;

    profile_executor_wd_result_t out = profile_executor_wd_decide(&in);

    TEST_CHECK(out.action == PROFILE_EXECUTOR_WD_ACTION_NONE,
               "a safety-link blip that hasn't reached SAFETY_LINK_FIRING_ABORT_SILENCE_MS must not "
               "abort a firing -- a brief drop-and-recover is survivable");
    TEST_CHECK(out.fault_reason[0] == '\0', "no fault_reason is produced when nothing crossed the threshold");
}

static void test_silent_link_30s_still_retries_when_already_faulted(void)
{
    TEST_SECTION("profile_executor_wd_decide -- REGRESSION: 30s silence retry-while-faulted still works");

    profile_executor_wd_input_t in = base_input();
    in.safety_link_silent_30s = true;
    in.state_faulted = true;

    profile_executor_wd_result_t out = profile_executor_wd_decide(&in);

    TEST_CHECK(out.action == PROFILE_EXECUTOR_WD_ACTION_RETRY_RELAYS_OFF,
               "still-silent link on an already-faulted run keeps retrying relay-off, same as before");
}

// ---- New: PC/UART control link loss (profile_executor.h's
// PROFILE_EXECUTOR_PC_LINK_ABORT_SILENCE_MS). uart_bridge.c's link_watchdog_
// task already force-drops relays and asserts SAFETY_FAULT_SRC_PC_LINK 5s
// into an outage (UART_BRIDGE_LINK_TIMEOUT_MS) -- but until now nothing told
// THIS module, so a firing stayed RUNNING, kept advancing its ramp/dwell
// schedule, and both GUIs kept showing progress while the elements were
// actually being forced off every 250ms and the kiln cooled. The caller
// (watchdog_task_entry()) is the one that times the sustained-outage window
// against PROFILE_EXECUTOR_PC_LINK_ABORT_SILENCE_MS -- pc_link_down_sustained
// arrives here already thresholded, same convention as safety_link_silent_30s
// above, so a "brief outage" is simply pc_link_down_sustained == false.
static void test_pc_link_sustained_loss_faults_running(void)
{
    TEST_SECTION("profile_executor_wd_decide -- sustained PC link loss while RUNNING faults the run");

    profile_executor_wd_input_t in = base_input();
    in.pc_link_down_sustained = true;
    in.state_running_or_paused = true;

    profile_executor_wd_result_t out = profile_executor_wd_decide(&in);

    TEST_CHECK(out.action == PROFILE_EXECUTOR_WD_ACTION_FAULT,
               "a sustained PC-link outage aborts a RUNNING/PAUSED firing");
    TEST_CHECK(strstr(out.fault_reason, "PC control link") != NULL,
               "fault_reason names the PC link specifically");
    TEST_CHECK(strstr(out.fault_reason, "silent") != NULL, "fault_reason describes it as silence");
}

static void test_pc_link_brief_loss_does_not_fault(void)
{
    TEST_SECTION("profile_executor_wd_decide -- a brief PC link loss (not yet sustained) does NOT fault");

    profile_executor_wd_input_t in = base_input();
    in.pc_link_down_sustained = false; // caller's own timer hasn't crossed the threshold yet
    in.state_running_or_paused = true;

    profile_executor_wd_result_t out = profile_executor_wd_decide(&in);

    TEST_CHECK(out.action == PROFILE_EXECUTOR_WD_ACTION_NONE,
               "a PC-link blip that hasn't reached PROFILE_EXECUTOR_PC_LINK_ABORT_SILENCE_MS must not "
               "abort a firing -- a brief drop-and-recover is survivable");
}

static void test_pc_link_loss_while_idle_does_not_fabricate_run(void)
{
    TEST_SECTION("profile_executor_wd_decide -- sustained PC link loss while IDLE does not fabricate a run");

    profile_executor_wd_input_t in = base_input();
    in.pc_link_down_sustained = true;
    // state_running_or_paused and state_faulted both false -> IDLE/DONE

    profile_executor_wd_result_t out = profile_executor_wd_decide(&in);

    TEST_CHECK(out.action == PROFILE_EXECUTOR_WD_ACTION_NONE,
               "PC link loss with nothing running/paused/faulted must not manufacture a FAULTED run");
    TEST_CHECK(out.fault_reason[0] == '\0', "no fault_reason is produced for the idle case");
}

static void test_pc_link_already_faulted_retries_relay_off(void)
{
    TEST_SECTION("profile_executor_wd_decide -- PC link still down while already FAULTED retries relay-off only");

    profile_executor_wd_input_t in = base_input();
    in.pc_link_down_sustained = true;
    in.state_faulted = true;

    profile_executor_wd_result_t out = profile_executor_wd_decide(&in);

    TEST_CHECK(out.action == PROFILE_EXECUTOR_WD_ACTION_RETRY_RELAYS_OFF,
               "an already-FAULTED run with the PC link still down keeps retrying relay-off, does not "
               "re-fault or touch fault_reason");
    TEST_CHECK(out.fault_reason[0] == '\0', "fault_reason is left untouched (empty) on a retry action");
}

static void test_pc_link_wording_distinct_from_safety_link_and_trip(void)
{
    TEST_SECTION("profile_executor_wd_decide -- PC-link wording differs from safety-link-silence and "
                 "safety-trip wording");

    profile_executor_wd_input_t pc_in = base_input();
    pc_in.pc_link_down_sustained = true;
    pc_in.state_running_or_paused = true;
    profile_executor_wd_result_t pc_out = profile_executor_wd_decide(&pc_in);

    profile_executor_wd_input_t silent_in = base_input();
    silent_in.safety_link_silent_30s = true;
    silent_in.state_running_or_paused = true;
    profile_executor_wd_result_t silent_out = profile_executor_wd_decide(&silent_in);

    profile_executor_wd_input_t trip_in = base_input();
    trip_in.safety_processor_tripped = true;
    trip_in.safety_trip_reason = 1;
    trip_in.state_running_or_paused = true;
    profile_executor_wd_result_t trip_out = profile_executor_wd_decide(&trip_in);

    TEST_CHECK(strcmp(pc_out.fault_reason, silent_out.fault_reason) != 0,
               "PC-link loss and safety-link silence produce different fault_reason text");
    TEST_CHECK(strcmp(pc_out.fault_reason, trip_out.fault_reason) != 0,
               "PC-link loss and a safety-processor trip produce different fault_reason text");
    TEST_CHECK(strstr(pc_out.fault_reason, "PC control link") != NULL,
               "only the PC-link case names the PC link");
    TEST_CHECK(strstr(silent_out.fault_reason, "PC control link") == NULL,
               "the safety-link-silence case does not say 'PC control link'");
    TEST_CHECK(strstr(trip_out.fault_reason, "PC control link") == NULL,
               "the safety-trip case does not say 'PC control link'");
}

static void test_tick_stale_still_faults_running_and_takes_priority(void)
{
    TEST_SECTION("profile_executor_wd_decide -- REGRESSION: control-task tick-stale still faults, "
                 "and outranks a simultaneous safety trip");

    profile_executor_wd_input_t in = base_input();
    in.tick_stale = true;
    in.tick_stale_ms = 12345;
    in.state_running_or_paused = true;
    // Also tripped, to prove tick_stale is checked first (matches
    // watchdog_task_entry()'s own ordering: guard 9's own liveness check
    // always runs before the safety-trip classification).
    in.safety_processor_tripped = true;
    in.safety_trip_reason = 1;

    profile_executor_wd_result_t out = profile_executor_wd_decide(&in);

    TEST_CHECK(out.action == PROFILE_EXECUTOR_WD_ACTION_FAULT, "tick-stale still faults a RUNNING firing");
    TEST_CHECK(strstr(out.fault_reason, "tick stale") != NULL,
               "tick_stale's own wording wins over the safety-trip wording when both are true this tick");
    TEST_CHECK(strstr(out.fault_reason, "12345") != NULL, "the stale duration is reported");
}

static void test_reason_words_are_never_a_bare_number(void)
{
    TEST_SECTION("profile_executor_safety_trip_words -- every known reason renders in words");

    // Spot-check a representative spread, including the TRIP_INEFFECTIVE
    // (S9) case that main_page.html/safety_page.html give extra emphasis --
    // this table must still name it distinctly, and an unknown code must not
    // crash or render as blank/"0".
    /* Asserted as REQUIREMENTS, not as exact literals. These two checks
     * pinned the strings "overtemp (S1)" and "TRIP_INEFFECTIVE", which the
     * tables have since improved on ("chamber over absolute temperature limit
     * (S1)", "contactor welded on, trip did NOT cut power (S9)") -- better
     * operator wording that nonetheless failed an equality check. The drift
     * went unnoticed for a long time because the host-test suite did not
     * build at all (a stub missing uart_proto_message_t), so nothing here
     * ran. What actually matters is what the section name says: the reason
     * renders in words, names its guard, and is never a bare number. */
    TEST_CHECK(strstr(profile_executor_safety_trip_words(1), "S1") != NULL &&
                   strstr(profile_executor_safety_trip_words(1), "temp") != NULL,
               "S1 is named and described, not rendered as a bare code");
    TEST_CHECK(strstr(profile_executor_safety_trip_words(10), "S9") != NULL &&
                   strstr(profile_executor_safety_trip_words(10), "welded") != NULL,
               "S9 (trip did not cut power) is called out distinctly, not just as a number");
    TEST_CHECK(strcmp(profile_executor_safety_trip_words(255), "unknown guard") == 0,
               "an out-of-range code renders as words, not garbage or a crash");
}

// ROADMAP.md "LCD shows a raw bitmask where the web shows a sentence" --
// ui_page_safety.c's "Last trip" row used to print a bare "0x%02X" for
// trip_reason; it now decodes through safety_trip_words_short() (shared with
// ui_page_home.c) the same way this file's own
// test_reason_words_are_never_a_bare_number() already holds
// profile_executor_safety_trip_words() to. Same spot-check shape, on the
// short/single-line table this time.
static void test_lcd_short_words_are_never_a_bare_number(void)
{
    TEST_SECTION("safety_trip_words_short -- every known reason renders in words, none as a bare hex byte");

    /* Same requirement-not-literal reasoning as the long table above. */
    TEST_CHECK(strstr(safety_trip_words_short(1), "S1") != NULL &&
                   strstr(safety_trip_words_short(1), "temp") != NULL,
               "S1 is named and described on the LCD's short table too");
    TEST_CHECK(strstr(safety_trip_words_short(10), "S9") != NULL &&
                   strstr(safety_trip_words_short(10), "welded") != NULL,
               "S9 is called out distinctly on the LCD's short table too");
    TEST_CHECK(strcmp(safety_trip_words_short(255), "unknown guard") == 0,
               "an out-of-range code renders as words, not garbage or a crash");
    // The actual regression this closes: the old LCD row's format string was
    // "reason 0x%02X" -- verify the decoded word for a real reason contains
    // no "0x", which a raw-hex render always would.
    TEST_CHECK(strstr(safety_trip_words_short(7), "0x") == NULL,
               "a real reason decodes to words, not a hex escape sequence");
}

void run_test_safety_watchdog(void)
{
    test_safety_trip_faults_running();
    test_safety_trip_faults_paused();
    test_trip_wording_distinct_from_silent_link();
    test_already_faulted_retries_relay_off_without_reason_change();
    test_idle_trip_does_not_fabricate_run();
    test_no_trip_no_silence_idle_is_a_true_no_op();
    test_silent_link_30s_still_faults_running();
    test_silent_link_brief_does_not_fault();
    test_silent_link_30s_still_retries_when_already_faulted();
    test_pc_link_sustained_loss_faults_running();
    test_pc_link_brief_loss_does_not_fault();
    test_pc_link_loss_while_idle_does_not_fabricate_run();
    test_pc_link_already_faulted_retries_relay_off();
    test_pc_link_wording_distinct_from_safety_link_and_trip();
    test_tick_stale_still_faults_running_and_takes_priority();
    test_reason_words_are_never_a_bare_number();
    test_lcd_short_words_are_never_a_bare_number();
}
