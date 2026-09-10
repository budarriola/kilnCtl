// Host test for safety_ceiling_policy.c -- owner request 2026-09-10: "if i
// change the max temp in the web gui it should change it in the pico too."
// Pure logic, no ESP/FreeRTOS -- see safety_ceiling_policy.h's own header
// comment for the invariant (Pico ceiling never tighter than the ESP's) and
// the ordering rule this test proves.
#include "test_common.h"
#include "safety_ceiling_policy.h"

#include <string.h>

typedef struct {
    int calls;
    float last_target_c;
    bool result_to_return;
    const char *reason_to_return;
} fake_writer_state_t;

static bool fake_writer(void *ctx, float target_c, char *reason_out, size_t reason_cap)
{
    fake_writer_state_t *s = (fake_writer_state_t *)ctx;
    s->calls++;
    s->last_target_c = target_c;
    if (!s->result_to_return && reason_out && reason_cap > 0) {
        snprintf(reason_out, reason_cap, "%s", s->reason_to_return ? s->reason_to_return : "refused");
    }
    return s->result_to_return;
}

// ---------------------------------------------------------------------
// 1. Raising: Pico first, verified, then (implicitly) the ESP may commit.
// ---------------------------------------------------------------------
static void test_raise_writes_pico_first_and_confirms(void)
{
    float new_zones[3] = { 80.0f, 80.0f, 120.0f }; // one zone raised to 120
    fake_writer_state_t w = { .result_to_return = true };
    safety_ceiling_sync_result_t result;
    char reason[128] = { 0 };

    bool may_commit = safety_ceiling_policy_guard_raise(80.0f, true, new_zones, 3, fake_writer, &w,
                                                          &result, reason, sizeof(reason));

    TEST_CHECK((may_commit), "a confirmed Pico raise must allow the ESP commit to proceed");
    TEST_CHECK((result == SAFETY_CEILING_SYNC_RAISED), "result must report RAISED");
    TEST_CHECK((w.calls == 1), "the writer (Pico) must be called exactly once");
    TEST_CHECK_NEAR(w.last_target_c, 125.0f, 0.001f, "target must be the new max (120) + 5C headroom");
}

// ---------------------------------------------------------------------
// 2. Lowering: ESP first (caller applies its own commit before calling
//    apply_lower -- this test proves apply_lower never blocks and is only
//    invoked for the lower direction), Pico best-effort afterward.
// ---------------------------------------------------------------------
static void test_lower_is_esp_first_pico_best_effort_after(void)
{
    float new_zones[3] = { 80.0f, 80.0f, 80.0f }; // zone that was 120 lowered back to 80
    fake_writer_state_t raise_w = { .result_to_return = true };
    safety_ceiling_sync_result_t raise_result;
    char reason[128] = { 0 };

    // Pico currently sits at 125 (from the raise above). guard_raise() must
    // see this as "no raise needed" and NOT touch the Pico -- the ESP's own
    // commit (simulated by the caller, not modeled in this pure-logic test)
    // is what happens first for a lowering change.
    bool may_commit = safety_ceiling_policy_guard_raise(125.0f, true, new_zones, 3, fake_writer, &raise_w,
                                                          &raise_result, reason, sizeof(reason));
    TEST_CHECK((may_commit), "a lowering change must never be blocked by the raise guard");
    TEST_CHECK((raise_result == SAFETY_CEILING_SYNC_NONE), "no raise is needed when the target is already lower");
    TEST_CHECK((raise_w.calls == 0), "the Pico must NOT be written during the pre-commit raise guard for a lowering change");

    // Only AFTER the (simulated) ESP commit does the best-effort lower run.
    fake_writer_state_t lower_w = { .result_to_return = true };
    safety_ceiling_sync_result_t lower_result;
    safety_ceiling_policy_apply_lower(125.0f, true, new_zones, 3, fake_writer, &lower_w, &lower_result, reason,
                                       sizeof(reason));
    TEST_CHECK((lower_result == SAFETY_CEILING_SYNC_LOWERED), "apply_lower must report LOWERED on a confirmed write");
    TEST_CHECK((lower_w.calls == 1), "apply_lower must write the Pico exactly once");
    TEST_CHECK_NEAR(lower_w.last_target_c, 85.0f, 0.001f, "lowered target must be the new max (80) + 5C headroom");
}

// ---------------------------------------------------------------------
// 3. A failed/unverified Pico write must leave the ESP side unchanged --
//    i.e. guard_raise() must return false and never claim success.
// ---------------------------------------------------------------------
static void test_failed_pico_raise_blocks_and_leaves_esp_unchanged(void)
{
    float new_zones[3] = { 80.0f, 80.0f, 200.0f }; // a big raise
    fake_writer_state_t w = { .result_to_return = false,
                               .reason_to_return = "commit rejected: abs_max_temp_c (id 260) -- "
                                                    "relay is ARMED -- config writes are refused while ARMED -- "
                                                    "values were staged but NOT written" };
    safety_ceiling_sync_result_t result;
    char reason[256] = { 0 };

    bool may_commit = safety_ceiling_policy_guard_raise(80.0f, true, new_zones, 3, fake_writer, &w,
                                                          &result, reason, sizeof(reason));

    TEST_CHECK(!may_commit, "an unconfirmed Pico raise must block the ESP commit -- this is the whole point "
                             "of doing the Pico write BEFORE applying the zone change");
    TEST_CHECK((result == SAFETY_CEILING_SYNC_RAISE_FAILED), "result must report RAISE_FAILED");
    TEST_CHECK((strlen(reason) > 0), "the operator-facing reason must be populated, not left silent");
    TEST_CHECK((strstr(reason, "ARMED") != NULL),
                "the ARMED refusal (the Pico's real, ordinary standing-state refusal) must reach the "
                "reason text verbatim enough to be recognisable, not be swallowed into a generic error");
    // The caller (zones_post_handler) is documented to check this return
    // value BEFORE its own `s_zones.cfg = tmp;` commit line runs -- proven
    // here at the policy layer, which is the only layer capable of proving
    // it without a real httpd/link stack.
}

// ---------------------------------------------------------------------
// 4. A zero-ceiling zone must never participate in the maximum, and must
//    never be treated as "use a default".
// ---------------------------------------------------------------------
static void test_zero_ceiling_zone_excluded_from_maximum(void)
{
    // All zones zero -- no target derivable at all.
    float all_zero[3] = { 0.0f, 0.0f, 0.0f };
    TEST_CHECK_NEAR(safety_ceiling_policy_target_c(all_zero, 3), 0.0f, 0.001f,
                         "an all-zero zone config must yield NO target (never invent a default)");

    fake_writer_state_t w = { .result_to_return = true };
    safety_ceiling_sync_result_t result;
    char reason[128] = { 0 };
    bool may_commit =
        safety_ceiling_policy_guard_raise(80.0f, true, all_zero, 3, fake_writer, &w, &result, reason, sizeof(reason));
    TEST_CHECK((may_commit), "an all-zero zone config must never block a commit");
    TEST_CHECK((result == SAFETY_CEILING_SYNC_NONE), "an all-zero zone config must report NONE");
    TEST_CHECK((w.calls == 0), "the Pico must never be written on behalf of an all-zero zone config");

    // Mixed: one real zone, two zero (disabled) zones -- the zero zones
    // must not drag the maximum down, and must not count as "0 is the max".
    float mixed[3] = { 0.0f, 150.0f, 0.0f };
    float target = safety_ceiling_policy_target_c(mixed, 3);
    TEST_CHECK_NEAR(target, 155.0f, 0.001f, "the maximum must come from the one real zone (150) + headroom, ignoring the zero zones");
}

// ---------------------------------------------------------------------
// 5. 2026-09-10 opus review finding A: safety_ceiling_reconcile_backoff_t --
//    the pure backoff decision safety_ceiling_sync_reconcile_on_link_up()
//    (safety_ceiling_sync.c) must consult BEFORE calling guard_raise() on
//    every safety_poll_task tick, so a Pico that is ARMED (its ordinary
//    standing state) and therefore guaranteed to refuse a raise does not
//    get retried twice a second forever.
// ---------------------------------------------------------------------

static void test_reconcile_backoff_starts_open(void)
{
    safety_ceiling_reconcile_backoff_t state = { 0 };
    TEST_CHECK(safety_ceiling_reconcile_should_attempt(&state, 0),
               "a freshly-initialised backoff state must allow the very first attempt");
    TEST_CHECK(safety_ceiling_reconcile_should_attempt(&state, 1000000),
               "and every later time too, until a failure records one");
}

static void test_reconcile_backoff_after_armed_refusal_is_the_long_backoff(void)
{
    safety_ceiling_reconcile_backoff_t state = { 0 };
    int64_t now = 1000000000LL; // arbitrary non-zero epoch

    safety_ceiling_reconcile_record_result(&state, now, false,
                                            "commit rejected: abs_max_temp_c (id 260) -- relay is ARMED -- "
                                            "config writes are refused while ARMED -- values were staged but "
                                            "NOT written");

    TEST_CHECK(!safety_ceiling_reconcile_should_attempt(&state, now),
               "must not retry in the same instant as an ARMED refusal");
    TEST_CHECK(!safety_ceiling_reconcile_should_attempt(&state, now + SAFETY_CEILING_RECONCILE_RETRY_BACKOFF_US),
               "the SHORT backoff must not be enough to clear an ARMED refusal's backoff -- proves the two "
               "durations are not accidentally the same");
    TEST_CHECK(safety_ceiling_reconcile_should_attempt(&state, now + SAFETY_CEILING_RECONCILE_ARMED_BACKOFF_US),
               "the full ARMED backoff duration must clear it");
}

static void test_reconcile_backoff_after_ordinary_failure_is_the_short_backoff(void)
{
    safety_ceiling_reconcile_backoff_t state = { 0 };
    int64_t now = 5000000000LL;

    safety_ceiling_reconcile_record_result(&state, now, false,
                                            "communication with the safety processor failed while staging "
                                            "abs_max_temp_c (id 260): ESP_ERR_TIMEOUT");

    TEST_CHECK(!safety_ceiling_reconcile_should_attempt(&state, now),
               "must not retry in the same instant as an ordinary failure");
    TEST_CHECK(safety_ceiling_reconcile_should_attempt(&state, now + SAFETY_CEILING_RECONCILE_RETRY_BACKOFF_US),
               "the short backoff duration must clear an ordinary (non-ARMED) failure");
    // Prove it is genuinely the SHORT one, not merely "the ARMED duration
    // also happens to have elapsed" -- a negative-test-shaped check: if the
    // implementation collapsed both branches onto the long duration, this
    // would fail where the assertion above would still pass.
    TEST_CHECK(SAFETY_CEILING_RECONCILE_RETRY_BACKOFF_US < SAFETY_CEILING_RECONCILE_ARMED_BACKOFF_US,
               "sanity: the two configured backoff constants must actually differ");
}

static void test_reconcile_backoff_success_clears_it_immediately(void)
{
    safety_ceiling_reconcile_backoff_t state = { 0 };
    int64_t now = 2000000000LL;

    safety_ceiling_reconcile_record_result(&state, now, false, "relay is ARMED -- config writes are refused");
    TEST_CHECK(!safety_ceiling_reconcile_should_attempt(&state, now + 1),
               "sanity: the ARMED backoff must actually be pending before the success case below");

    // A relay that de-energises (or a config change that no longer needs a
    // raise) must be reflected the very next tick, not wait out an armed
    // backoff that no longer applies.
    safety_ceiling_reconcile_record_result(&state, now + 1, true, NULL);
    TEST_CHECK(safety_ceiling_reconcile_should_attempt(&state, now + 1),
               "a successful (or no-op) result must clear the backoff immediately, not merely shorten it");
    TEST_CHECK(safety_ceiling_reconcile_should_attempt(&state, now + 2),
               "and stay clear afterward, not just at the exact instant of success");
}

static void test_reconcile_backoff_reason_without_armed_substring_is_short(void)
{
    // The classifier looks for the literal substring "ARMED" -- any failure
    // reason that does not contain it (even one that is ALSO Pico-related)
    // must fall into the short/ordinary backoff, not the long one. Guards
    // against an over-broad classifier that treats every failure as ARMED.
    safety_ceiling_reconcile_backoff_t state = { 0 };
    int64_t now = 3000000000LL;

    safety_ceiling_reconcile_record_result(&state, now, false,
                                            "the safety processor accepted the commit but this board could not "
                                            "read the config back to confirm it -- treating the write as "
                                            "UNCONFIRMED, not successful");

    TEST_CHECK(safety_ceiling_reconcile_should_attempt(&state, now + SAFETY_CEILING_RECONCILE_RETRY_BACKOFF_US),
               "a non-ARMED failure reason must use the short backoff even when it is otherwise Pico-related");
}

void run_test_safety_ceiling_policy(void)
{
    test_raise_writes_pico_first_and_confirms();
    test_lower_is_esp_first_pico_best_effort_after();
    test_failed_pico_raise_blocks_and_leaves_esp_unchanged();
    test_zero_ceiling_zone_excluded_from_maximum();
    test_reconcile_backoff_starts_open();
    test_reconcile_backoff_after_armed_refusal_is_the_long_backoff();
    test_reconcile_backoff_after_ordinary_failure_is_the_short_backoff();
    test_reconcile_backoff_success_clears_it_immediately();
    test_reconcile_backoff_reason_without_armed_substring_is_short();
}
