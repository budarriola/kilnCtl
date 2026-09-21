// Host tests for App/drivers/net/pico_auto_update_state.h/.c -- the shared
// blocking/warning verdict readiness_gate.c and readiness_http.c both read.
//
// Review finding F2 (owner decision, option c, 2026-09-20): exhausting the
// Pico auto-update attempt budget must surface as a non-blocking
// /readiness WARNING, never as a permanent firing refusal. The decision
// function itself (pico_auto_update.h, test_pico_auto_update_decision.c)
// is unchanged -- it still reports ABANDONED_BUDGET_SPENT as one of its
// unrecoverable causes. What changed is how pico_auto_update_boot.c's
// switch CONSUMES that outcome: it now calls
// pico_auto_update_state_set_warning() instead of set_blocking(true), and
// leaves is_blocking() at its false default. That boot-time switch itself
// lives in pico_auto_update_boot.c, which pulls in FreeRTOS/ESP-IDF task
// plumbing and is not part of this host-test build (same class as the
// HTTP-handler files -- see this repo's "HTTP handlers are target-build
// only" note); this file instead proves the CONTRACT the state module
// offers that switch: blocking and warning are independent flags, a
// warning never implies blocking, and the module's own zero-initialized
// default (nothing decided yet) reads as neither blocking nor warning --
// so a boot that reaches ABANDONED_BUDGET_SPENT and calls only
// set_warning() leaves is_blocking() false, exactly as readiness_gate.h
// requires.
//
// Opus review, 2026-09-20 (same pass as the record_failure fix in
// pico_auto_update_boot.c): ABANDONED_PRIOR_FAILED joined BUDGET_SPENT as
// non-blocking too, for the same reason -- the only clear for that cause is
// a verified MATCH, which an abandoned pair can never attempt again to
// reach, so a persisted (possibly stale) failure flag must not be able to
// latch a permanent refusal either. NO_IMAGE and CHAIN_GAP are the only two
// ABANDONED_* causes still consumed with set_blocking(true).
#include <string.h>

#include "test_common.h"

#include "../drivers/net/pico_auto_update_state.h"
#include "../drivers/net/pico_auto_update_state.c"

static void test_default_state_is_neither_blocking_nor_warning(void)
{
    TEST_CHECK(!pico_auto_update_state_is_blocking(), "default: not blocking");
    TEST_CHECK(!pico_auto_update_state_is_warning(), "default: no warning");
    TEST_CHECK(strcmp(pico_auto_update_state_reason(), "") == 0, "default: empty blocking reason");
    TEST_CHECK(strcmp(pico_auto_update_state_warning_reason(), "") == 0, "default: empty warning reason");
}

static void test_budget_spent_sets_warning_without_blocking(void)
{
    // The exact sequence pico_auto_update_boot.c's ABANDONED_BUDGET_SPENT
    // case now performs (F2): a warning naming the attempt count/commit,
    // and blocking explicitly left/forced false -- never set_blocking(true).
    pico_auto_update_state_set_blocking(false, NULL);
    pico_auto_update_state_set_warning("Pico update budget spent (3/3 attempts) against abc123");

    TEST_CHECK(!pico_auto_update_state_is_blocking(),
              "F2: a spent update budget must NEVER block firing -- readiness_gate.h's interlock "
              "reads only is_blocking(), so this is what actually keeps the board fireable");
    TEST_CHECK(pico_auto_update_state_is_warning(), "F2: the spent budget IS surfaced, as a warning");
    TEST_CHECK(strstr(pico_auto_update_state_warning_reason(), "3/3 attempts") != NULL,
              "the warning names the attempt count");
    TEST_CHECK(strstr(pico_auto_update_state_warning_reason(), "abc123") != NULL,
              "the warning names the commit that could not be matched");

    pico_auto_update_state_set_warning(NULL); // restore
}

static void test_other_unrecoverable_causes_still_block(void)
{
    // The two REMAINING ABANDONED_* causes (NO_IMAGE, CHAIN_GAP) keep the
    // pre-F2 behavior: set_blocking(true) with no warning. PRIOR_FAILED
    // moved to the non-blocking/warning group alongside BUDGET_SPENT in the
    // 2026-09-20 Opus review pass -- see test_prior_failed_sets_warning_
    // without_blocking() below.
    pico_auto_update_state_set_blocking(true, "no usable embedded/staged image");
    TEST_CHECK(pico_auto_update_state_is_blocking(), "an image/chain-gap cause still blocks");
    TEST_CHECK(!pico_auto_update_state_is_warning(),
              "blocking for one of the other causes does not, by itself, raise the separate warning flag");
    TEST_CHECK(strcmp(pico_auto_update_state_reason(), "no usable embedded/staged image") == 0,
              "the blocking reason is recorded verbatim");

    pico_auto_update_state_set_blocking(false, NULL); // restore
}

static void test_prior_failed_sets_warning_without_blocking(void)
{
    // The exact sequence pico_auto_update_boot.c's ABANDONED_PRIOR_FAILED
    // case now performs, mirroring ABANDONED_BUDGET_SPENT's F2 fix: a
    // warning naming the pair, blocking left/forced false.
    pico_auto_update_state_set_blocking(false, NULL);
    pico_auto_update_state_set_warning(
        "Pico update: prior attempt for this pair reported a terminal failure (against abc123)");

    TEST_CHECK(!pico_auto_update_state_is_blocking(),
              "a prior-failed pair must never block firing -- only a warning, same rationale as F2's "
              "budget-spent fix: the only clear (a verified MATCH) is unreachable once abandoned");
    TEST_CHECK(pico_auto_update_state_is_warning(), "the prior failure IS surfaced, as a warning");
    TEST_CHECK(strstr(pico_auto_update_state_warning_reason(), "abc123") != NULL,
              "the warning names the commit pair");

    pico_auto_update_state_set_warning(NULL); // restore
}

static void test_set_blocking_false_clears_reason(void)
{
    pico_auto_update_state_set_blocking(true, "temporary");
    pico_auto_update_state_set_blocking(false, NULL);
    TEST_CHECK(!pico_auto_update_state_is_blocking(), "cleared: not blocking");
    TEST_CHECK(strcmp(pico_auto_update_state_reason(), "") == 0, "cleared: reason reset to empty");
}

// Bug found 2026-09-21 (bench triage, readiness_http.c): the readiness page
// used to render a hardcoded "matches" sentence for every non-blocking,
// non-warning outcome, whether or not a comparison actually ran and
// matched. These tests prove the state module's side of the fix: the
// decision string/flag are independent of blocking/warning, default to
// "nothing decided" (empty/false), and are set verbatim by the boot task's
// switch (pico_auto_update_boot.c, not part of this host-test build --
// same "HTTP handlers are target-build only" class -- so this file proves
// the CONTRACT that switch relies on, same pattern as the rest of this
// file).
static void test_default_decision_is_not_a_match(void)
{
    // Nothing has called set_last_decision() yet this test process (module
    // statics are zero-initialized): readiness_http.c must NOT default to
    // "matches" here -- this is exactly the bug. Empty string + false is
    // "not yet evaluated", which the HTTP handler renders as its own
    // distinct "not yet evaluated this boot" sentence, never "matches".
    TEST_CHECK(!pico_auto_update_state_decision_is_match(), "default: not a match");
    TEST_CHECK(strcmp(pico_auto_update_state_last_decision(), "") == 0, "default: empty decision string");
}

static void test_set_last_decision_match_reads_back(void)
{
    pico_auto_update_state_set_last_decision("identity confirmed against abc123", true);
    TEST_CHECK(pico_auto_update_state_decision_is_match(), "MATCH: decision_is_match() true");
    TEST_CHECK(strcmp(pico_auto_update_state_last_decision(), "identity confirmed against abc123") == 0,
              "MATCH: reason recorded verbatim");
}

static void test_set_last_decision_non_match_reads_back_false(void)
{
    // A DEFER_FIRING/LINK_DOWN/etc outcome: a reason is still recorded (for
    // display), but is_match must read false -- this is the exact
    // distinction readiness_http.c's fix depends on to avoid claiming a
    // match it never confirmed.
    pico_auto_update_state_set_last_decision("link down -- no FW_VERSION observed yet", false);
    TEST_CHECK(!pico_auto_update_state_decision_is_match(), "non-MATCH: decision_is_match() false");
    TEST_CHECK(strcmp(pico_auto_update_state_last_decision(), "link down -- no FW_VERSION observed yet") == 0,
              "non-MATCH: reason still recorded for display");
}

static void test_set_last_decision_overwrites_previous(void)
{
    // Same single-writer-per-boot convention as set_blocking/set_warning:
    // a later call fully replaces the earlier one, including flipping
    // is_match back from true to false (a MATCH outcome must not "stick"
    // if somehow re-evaluated within one process's lifetime -- host-test
    // only concern, but proves no latching bug crept in).
    pico_auto_update_state_set_last_decision("identity confirmed against def456", true);
    TEST_CHECK(pico_auto_update_state_decision_is_match(), "first call: match true");
    pico_auto_update_state_set_last_decision("budget spent (3/3) against def456", false);
    TEST_CHECK(!pico_auto_update_state_decision_is_match(), "second call: match flips to false");
    TEST_CHECK(strcmp(pico_auto_update_state_last_decision(), "budget spent (3/3) against def456") == 0,
              "second call: reason fully replaced, not appended");
}

static void test_set_last_decision_null_reason_clears_string(void)
{
    pico_auto_update_state_set_last_decision("something", true);
    pico_auto_update_state_set_last_decision(NULL, false);
    TEST_CHECK(strcmp(pico_auto_update_state_last_decision(), "") == 0, "NULL reason clears the string");
    TEST_CHECK(!pico_auto_update_state_decision_is_match(), "NULL reason: is_match reads false");
}

void run_test_pico_auto_update_state(void)
{
    test_default_state_is_neither_blocking_nor_warning();
    test_budget_spent_sets_warning_without_blocking();
    test_other_unrecoverable_causes_still_block();
    test_prior_failed_sets_warning_without_blocking();
    test_set_blocking_false_clears_reason();
    // Restore the decision-tracking statics to their zero-init default
    // BEFORE test_default_decision_is_not_a_match() runs -- it must run
    // first among the decision tests to actually observe the default, so
    // it is listed immediately here, ahead of the calls that mutate it.
    test_default_decision_is_not_a_match();
    test_set_last_decision_match_reads_back();
    test_set_last_decision_non_match_reads_back_false();
    test_set_last_decision_overwrites_previous();
    test_set_last_decision_null_reason_clears_string();
}
