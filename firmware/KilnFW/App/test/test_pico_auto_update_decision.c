// Host tests for App/drivers/net/pico_auto_update.h -- the pure decision
// logic behind docs/PICO_AUTO_UPDATE.md ("the ESP checks the Pico's
// firmware version on every boot and updates it automatically if needed",
// owner requirement 2026-09-16). Header-only, no I/O -- table-driven over
// every branch pico_auto_update_decide() names, per the plan's own sec 9
// step 3 instruction to verify each outcome directly.
#include <string.h>

#include "test_common.h"

#include "../drivers/net/pico_auto_update.h"

static pico_auto_update_inputs_t base_inputs(void)
{
    pico_auto_update_inputs_t in;
    memset(&in, 0, sizeof(in));
    in.expected_commit = "abc123";
    memcpy(in.observed_commit, "abc123", 6);
    in.observed_commit_len = 6;
    in.observed_dirty = false;
    in.fw_version_known = true;
    in.image_available = true;
    in.firing_active = false;
    in.config_chain_gap = false;
    in.attempt_count = 0;
    in.prior_attempt_failed = false;
    return in;
}

static void test_identity_matches_exact_bytes_and_clean(void)
{
    pico_auto_update_inputs_t in = base_inputs();
    TEST_CHECK(pico_auto_update_identity_matches(&in), "identical commit, not dirty -- matches");

    in.observed_dirty = true;
    TEST_CHECK(!pico_auto_update_identity_matches(&in),
               "identical commit bytes but dirty=true -- does NOT match (owner decision: dirty "
               "never matches, no ordering comparison exists on this wire)");
}

static void test_identity_mismatch_on_length_or_bytes(void)
{
    pico_auto_update_inputs_t in = base_inputs();
    in.observed_commit_len = 5; /* shorter than expected_commit's strlen */
    TEST_CHECK(!pico_auto_update_identity_matches(&in), "length mismatch never matches");

    in = base_inputs();
    in.observed_commit[0] = 'z'; /* one byte different */
    TEST_CHECK(!pico_auto_update_identity_matches(&in), "one differing byte never matches");
}

static void test_empty_expected_commit_never_matches(void)
{
    // The unstamped build-time default (plan step 1 not yet run): "" must
    // never match anything, even a Pico that (absurdly) also reports a
    // zero-length commit -- this is the safe default this file's top
    // comment names explicitly.
    pico_auto_update_inputs_t in = base_inputs();
    in.expected_commit = "";
    in.observed_commit_len = 0;
    TEST_CHECK(!pico_auto_update_identity_matches(&in),
               "an empty expected_commit never matches, even a zero-length observed commit");
}

static void test_decide_link_down_takes_priority(void)
{
    pico_auto_update_inputs_t in = base_inputs();
    in.fw_version_known = false;
    TEST_CHECK(pico_auto_update_decide(&in, NULL) == PICO_AUTO_UPDATE_LINK_DOWN,
               "no FW_VERSION yet -- LINK_DOWN regardless of any other field");

    // NULL inputs pointer must also decide LINK_DOWN, not crash -- the
    // decision function's own documented NULL-safety.
    TEST_CHECK(pico_auto_update_decide(NULL, NULL) == PICO_AUTO_UPDATE_LINK_DOWN,
               "NULL inputs pointer is treated as link-down, not a crash");
}

static void test_decide_match_when_identity_equal(void)
{
    pico_auto_update_inputs_t in = base_inputs();
    const char *reason = NULL;
    TEST_CHECK(pico_auto_update_decide(&in, &reason) == PICO_AUTO_UPDATE_MATCH,
               "matching identity -- MATCH, no action");
    TEST_CHECK(reason != NULL, "a reason string is always provided");
}

static void test_decide_defers_for_firing_even_with_budget_spent(void)
{
    // THE ordering-sensitive case (plan sec 4): a live firing must defer,
    // never abandon, even when EVERY unrecoverable cause is also true. This
    // is the negative test for "firing_active checked before the
    // unrecoverable causes" -- if that order were flipped, a firing
    // in-progress would be wrongly interrupted by a budget/image/chain
    // finding that only needs to matter at the NEXT start.
    pico_auto_update_inputs_t in = base_inputs();
    in.observed_commit[0] = 'z'; /* force a mismatch */
    in.firing_active = true;
    in.image_available = false;
    in.config_chain_gap = true;
    in.prior_attempt_failed = true;
    in.attempt_count = PICO_AUTO_UPDATE_ATTEMPT_BUDGET;
    TEST_CHECK(pico_auto_update_decide(&in, NULL) == PICO_AUTO_UPDATE_DEFER_FIRING,
               "a firing in progress defers, even though every unrecoverable cause is ALSO true "
               "this same boot -- firing_active must be checked before any ABANDONED_* branch");
}

static void test_decide_abandoned_no_image(void)
{
    pico_auto_update_inputs_t in = base_inputs();
    in.observed_commit[0] = 'z';
    in.image_available = false;
    TEST_CHECK(pico_auto_update_decide(&in, NULL) == PICO_AUTO_UPDATE_ABANDONED_NO_IMAGE,
               "mismatch, no firing, no embedded image -- ABANDONED_NO_IMAGE");
    TEST_CHECK(pico_auto_update_decision_is_unrecoverable(PICO_AUTO_UPDATE_ABANDONED_NO_IMAGE),
               "ABANDONED_NO_IMAGE is one of the unrecoverable outcomes");
}

static void test_decide_abandoned_chain_gap(void)
{
    pico_auto_update_inputs_t in = base_inputs();
    in.observed_commit[0] = 'z';
    in.config_chain_gap = true;
    TEST_CHECK(pico_auto_update_decide(&in, NULL) == PICO_AUTO_UPDATE_ABANDONED_CHAIN_GAP,
               "mismatch, image available, but a config-migration chain gap -- ABANDONED_CHAIN_GAP");
    TEST_CHECK(pico_auto_update_decision_is_unrecoverable(PICO_AUTO_UPDATE_ABANDONED_CHAIN_GAP),
               "ABANDONED_CHAIN_GAP is unrecoverable");
}

static void test_decide_abandoned_prior_failed(void)
{
    pico_auto_update_inputs_t in = base_inputs();
    in.observed_commit[0] = 'z';
    in.prior_attempt_failed = true;
    TEST_CHECK(pico_auto_update_decide(&in, NULL) == PICO_AUTO_UPDATE_ABANDONED_PRIOR_FAILED,
               "the last attempt for this pair terminally failed -- ABANDONED_PRIOR_FAILED, "
               "checked BEFORE the budget (a single terminal failure abandons regardless of "
               "remaining budget)");
    TEST_CHECK(pico_auto_update_decision_is_unrecoverable(PICO_AUTO_UPDATE_ABANDONED_PRIOR_FAILED),
               "ABANDONED_PRIOR_FAILED is unrecoverable");
}

static void test_decide_abandoned_budget_spent_exact_boundary(void)
{
    pico_auto_update_inputs_t in = base_inputs();
    in.observed_commit[0] = 'z';

    in.attempt_count = PICO_AUTO_UPDATE_ATTEMPT_BUDGET - 1u;
    TEST_CHECK(pico_auto_update_decide(&in, NULL) == PICO_AUTO_UPDATE_NEEDED,
               "one attempt short of budget -- still NEEDED");

    in.attempt_count = PICO_AUTO_UPDATE_ATTEMPT_BUDGET;
    TEST_CHECK(pico_auto_update_decide(&in, NULL) == PICO_AUTO_UPDATE_ABANDONED_BUDGET_SPENT,
               "attempt_count == budget (owner decision 10.2: 3) -- ABANDONED_BUDGET_SPENT, the "
               "exact crossing point");
    TEST_CHECK(pico_auto_update_decision_is_unrecoverable(PICO_AUTO_UPDATE_ABANDONED_BUDGET_SPENT),
               "ABANDONED_BUDGET_SPENT is unrecoverable");

    in.attempt_count = PICO_AUTO_UPDATE_ATTEMPT_BUDGET + 10u;
    TEST_CHECK(pico_auto_update_decide(&in, NULL) == PICO_AUTO_UPDATE_ABANDONED_BUDGET_SPENT,
               "well past budget stays abandoned");
}

static void test_decide_needed_when_mismatch_and_everything_else_clear(void)
{
    pico_auto_update_inputs_t in = base_inputs();
    in.observed_commit[0] = 'z';
    TEST_CHECK(pico_auto_update_decide(&in, NULL) == PICO_AUTO_UPDATE_NEEDED,
               "plain mismatch, image available, no chain gap, no prior failure, budget remains "
               "-- NEEDED");
    TEST_CHECK(!pico_auto_update_decision_is_unrecoverable(PICO_AUTO_UPDATE_NEEDED),
               "NEEDED is not one of the unrecoverable outcomes -- the refuse-to-fire interlock "
               "must NOT trip on a plain, budget-remaining mismatch");
    TEST_CHECK(!pico_auto_update_decision_is_unrecoverable(PICO_AUTO_UPDATE_MATCH),
               "MATCH is not unrecoverable");
    TEST_CHECK(!pico_auto_update_decision_is_unrecoverable(PICO_AUTO_UPDATE_DEFER_FIRING),
               "DEFER_FIRING is not unrecoverable");
    TEST_CHECK(!pico_auto_update_decision_is_unrecoverable(PICO_AUTO_UPDATE_LINK_DOWN),
               "LINK_DOWN is not unrecoverable");
}

void run_test_pico_auto_update_decision(void)
{
    test_identity_matches_exact_bytes_and_clean();
    test_identity_mismatch_on_length_or_bytes();
    test_empty_expected_commit_never_matches();
    test_decide_link_down_takes_priority();
    test_decide_match_when_identity_equal();
    test_decide_defers_for_firing_even_with_budget_spent();
    test_decide_abandoned_no_image();
    test_decide_abandoned_chain_gap();
    test_decide_abandoned_prior_failed();
    test_decide_abandoned_budget_spent_exact_boundary();
    test_decide_needed_when_mismatch_and_everything_else_clear();
}
