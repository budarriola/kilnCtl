// Host test for safety_ceiling_policy.c -- owner request 2026-09-10: "if i
// change the max temp in the web gui it should change it in the pico too."
// Pure logic, no ESP/FreeRTOS -- see safety_ceiling_policy.h's own header
// comment for the invariant (Pico ceiling never tighter than the ESP's) and
// the ordering rule this test proves.
//
// 2026-09-10 opus review finding B fix: the reconcile backoff no longer
// classifies a refusal by `strstr(reason, "ARMED")` -- it classifies on the
// numeric safety_ceiling_refusal_class_t the writer reports directly. The
// tests below were rewritten accordingly: fake_writer() now reports a class
// (not just a reason string), and the reconcile-backoff tests drive
// safety_ceiling_reconcile_record_result() with a class, never a string.
#include "test_common.h"
#include "safety_ceiling_policy.h"

#include <string.h>

typedef struct {
    int calls;
    float last_target_c;
    bool result_to_return;
    const char *reason_to_return;
    safety_ceiling_refusal_class_t class_to_return;
} fake_writer_state_t;

static bool fake_writer(void *ctx, float target_c, char *reason_out, size_t reason_cap,
                         safety_ceiling_refusal_class_t *out_class)
{
    fake_writer_state_t *s = (fake_writer_state_t *)ctx;
    s->calls++;
    s->last_target_c = target_c;
    if (!s->result_to_return) {
        if (reason_out && reason_cap > 0) {
            snprintf(reason_out, reason_cap, "%s", s->reason_to_return ? s->reason_to_return : "refused");
        }
        if (out_class) {
            *out_class = s->class_to_return;
        }
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
                                                          &result, reason, sizeof(reason), NULL);

    TEST_CHECK((may_commit), "a confirmed Pico raise must allow the ESP commit to proceed");
    TEST_CHECK((result == SAFETY_CEILING_SYNC_RAISED), "result must report RAISED");
    TEST_CHECK((w.calls == 1), "the writer (Pico) must be called exactly once");
    // 2026-09-10 owner correction: "the intent of the web page setting was
    // to put a hard cutoff." No headroom is added any more -- the Pico's
    // target is the ESP's own configured maximum, exactly.
    TEST_CHECK_NEAR(w.last_target_c, 120.0f, 0.001f, "target must be the new max (120) EXACTLY -- no headroom");
}

// ---------------------------------------------------------------------
// 1b. Equality: a Pico ceiling already exactly equal to the new ESP
//     target must be treated as satisfied -- no raise, nothing written.
//     Equality is NOT "tighter than", so the standing invariant holds.
// ---------------------------------------------------------------------
static void test_equal_ceiling_is_satisfied_no_write(void)
{
    float new_zones[3] = { 80.0f, 80.0f, 80.0f };
    fake_writer_state_t w = { .result_to_return = true };
    safety_ceiling_sync_result_t result;
    char reason[128] = { 0 };

    // Pico already sits at exactly 80, the same as the new target.
    bool may_commit = safety_ceiling_policy_guard_raise(80.0f, true, new_zones, 3, fake_writer, &w,
                                                          &result, reason, sizeof(reason), NULL);

    TEST_CHECK((may_commit), "an already-equal ceiling must never block a commit");
    TEST_CHECK((result == SAFETY_CEILING_SYNC_NONE), "an already-equal ceiling must report NONE (no raise needed)");
    TEST_CHECK((w.calls == 0), "the Pico must NOT be re-written when its ceiling already exactly equals the target");
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

    // Pico currently sits at 120 (from the raise above -- no headroom
    // added any more, see test 1). guard_raise() must see this as "no
    // raise needed" and NOT touch the Pico -- the ESP's own commit
    // (simulated by the caller, not modeled in this pure-logic test) is
    // what happens first for a lowering change.
    bool may_commit = safety_ceiling_policy_guard_raise(120.0f, true, new_zones, 3, fake_writer, &raise_w,
                                                          &raise_result, reason, sizeof(reason), NULL);
    TEST_CHECK((may_commit), "a lowering change must never be blocked by the raise guard");
    TEST_CHECK((raise_result == SAFETY_CEILING_SYNC_NONE), "no raise is needed when the target is already lower");
    TEST_CHECK((raise_w.calls == 0), "the Pico must NOT be written during the pre-commit raise guard for a lowering change");

    // Only AFTER the (simulated) ESP commit does the best-effort lower run.
    fake_writer_state_t lower_w = { .result_to_return = true };
    safety_ceiling_sync_result_t lower_result;
    safety_ceiling_policy_apply_lower(120.0f, true, new_zones, 3, fake_writer, &lower_w, &lower_result, reason,
                                       sizeof(reason));
    TEST_CHECK((lower_result == SAFETY_CEILING_SYNC_LOWERED), "apply_lower must report LOWERED on a confirmed write");
    TEST_CHECK((lower_w.calls == 1), "apply_lower must write the Pico exactly once");
    TEST_CHECK_NEAR(lower_w.last_target_c, 80.0f, 0.001f, "lowered target must be the new max (80) EXACTLY -- no headroom");
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
                                                    "values were staged but NOT written",
                               .class_to_return = SAFETY_CEILING_REFUSAL_ARMED };
    safety_ceiling_sync_result_t result;
    char reason[256] = { 0 };
    safety_ceiling_refusal_class_t refusal_class = SAFETY_CEILING_REFUSAL_NONE;

    bool may_commit = safety_ceiling_policy_guard_raise(80.0f, true, new_zones, 3, fake_writer, &w,
                                                          &result, reason, sizeof(reason), &refusal_class);

    TEST_CHECK(!may_commit, "an unconfirmed Pico raise must block the ESP commit -- this is the whole point "
                             "of doing the Pico write BEFORE applying the zone change");
    TEST_CHECK((result == SAFETY_CEILING_SYNC_RAISE_FAILED), "result must report RAISE_FAILED");
    TEST_CHECK((strlen(reason) > 0), "the operator-facing reason must be populated, not left silent");
    TEST_CHECK((strstr(reason, "ARMED") != NULL),
                "the ARMED refusal (the Pico's real, ordinary standing-state refusal) must reach the "
                "reason text verbatim enough to be recognisable, not be swallowed into a generic error");
    TEST_CHECK((refusal_class == SAFETY_CEILING_REFUSAL_ARMED),
               "out_refusal_class must carry the writer's own machine-readable classification through "
               "unchanged -- this, not reason's prose, is what the reconcile backoff must act on");
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
    bool may_commit = safety_ceiling_policy_guard_raise(80.0f, true, all_zero, 3, fake_writer, &w, &result, reason,
                                                          sizeof(reason), NULL);
    TEST_CHECK((may_commit), "an all-zero zone config must never block a commit");
    TEST_CHECK((result == SAFETY_CEILING_SYNC_NONE), "an all-zero zone config must report NONE");
    TEST_CHECK((w.calls == 0), "the Pico must never be written on behalf of an all-zero zone config");

    // Mixed: one real zone, two zero (disabled) zones -- the zero zones
    // must not drag the maximum down, and must not count as "0 is the max".
    float mixed[3] = { 0.0f, 150.0f, 0.0f };
    float target = safety_ceiling_policy_target_c(mixed, 3);
    TEST_CHECK_NEAR(target, 150.0f, 0.001f, "the maximum must come from the one real zone (150) EXACTLY, ignoring the zero zones, no headroom");
}

// ---------------------------------------------------------------------
// 5. 2026-09-10 opus review finding A: safety_ceiling_reconcile_backoff_t --
//    the pure backoff decision safety_ceiling_sync_reconcile_on_link_up()
//    (safety_ceiling_sync.c) must consult BEFORE calling guard_raise() on
//    every safety_poll_task tick, so a Pico that is ARMED (its ordinary
//    standing state) and therefore guaranteed to refuse a raise does not
//    get retried twice a second forever.
//
//    Finding B fix: the class the backoff switches on is now the numeric
//    safety_ceiling_refusal_class_t, never a substring match against a
//    human-readable reason. These tests drive record_result() with a
//    class directly.
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
    // Chosen so now % JITTER_US == 0 -- pins the jitter term at exactly 0 so
    // this test can assert an EXACT boundary (base, not "base plus some
    // unknown jitter in range") without coupling to the jitter formula's
    // internals beyond "now_us % JITTER_US".
    int64_t now = 3 * SAFETY_CEILING_RECONCILE_ARMED_BACKOFF_JITTER_US;

    safety_ceiling_reconcile_record_result(&state, now, false, SAFETY_CEILING_REFUSAL_ARMED);

    TEST_CHECK(!safety_ceiling_reconcile_should_attempt(&state, now),
               "must not retry in the same instant as an ARMED refusal");
    TEST_CHECK(!safety_ceiling_reconcile_should_attempt(&state, now + SAFETY_CEILING_RECONCILE_ARMED_BACKOFF_BASE_US - 1),
               "one microsecond short of the (zero-jitter, this `now`) ARMED backoff must not yet clear it");
    TEST_CHECK(safety_ceiling_reconcile_should_attempt(&state, now + SAFETY_CEILING_RECONCILE_ARMED_BACKOFF_BASE_US),
               "the ARMED base backoff duration must clear it when jitter is exactly zero");
    // ARMED backoff is base + jitter in [0, JITTER); the base plus the FULL
    // jitter range is always enough to clear it regardless of `now`'s phase.
    TEST_CHECK(safety_ceiling_reconcile_should_attempt(
                   &state, now + SAFETY_CEILING_RECONCILE_ARMED_BACKOFF_BASE_US +
                               SAFETY_CEILING_RECONCILE_ARMED_BACKOFF_JITTER_US),
               "base + the full jitter window must always clear an ARMED backoff");
}

static void test_reconcile_backoff_armed_jitter_varies_with_now(void)
{
    // 2026-09-10 opus review finding D: a FIXED ARMED backoff can phase-lock
    // against a fixed-period PWM relay window and step over every off-window
    // forever. Prove the recorded backoff duration actually varies with
    // `now_us` (i.e. jitter is real, not a no-op constant folded into the
    // base) -- a negative-test-shaped check: an implementation that dropped
    // the `% JITTER_US` term would make every one of these deltas equal.
    safety_ceiling_reconcile_backoff_t state_a = { 0 };
    safety_ceiling_reconcile_backoff_t state_b = { 0 };
    int64_t now_a = 1000000000LL;
    int64_t now_b = now_a + 1; // one microsecond later -- different phase

    safety_ceiling_reconcile_record_result(&state_a, now_a, false, SAFETY_CEILING_REFUSAL_ARMED);
    safety_ceiling_reconcile_record_result(&state_b, now_b, false, SAFETY_CEILING_REFUSAL_ARMED);

    int64_t delay_a = state_a.backoff_until_us - now_a;
    int64_t delay_b = state_b.backoff_until_us - now_b;
    TEST_CHECK(delay_a >= SAFETY_CEILING_RECONCILE_ARMED_BACKOFF_BASE_US &&
                   delay_a < SAFETY_CEILING_RECONCILE_ARMED_BACKOFF_BASE_US + SAFETY_CEILING_RECONCILE_ARMED_BACKOFF_JITTER_US,
               "the ARMED backoff duration must fall within [base, base+jitter)");
    // Different `now_us % JITTER_US` residues -- these two specific inputs
    // are chosen (1 us apart, both far from a JITTER_US boundary) so their
    // jittered delays differ; this is a property of the deterministic
    // now_us-derived jitter, not a coincidence.
    TEST_CHECK(delay_a != delay_b || (now_a % SAFETY_CEILING_RECONCILE_ARMED_BACKOFF_JITTER_US) ==
                                          (now_b % SAFETY_CEILING_RECONCILE_ARMED_BACKOFF_JITTER_US),
               "the jitter term must actually depend on now_us -- two different now_us phases one "
               "microsecond apart must not always yield the identical backoff duration");
}

static void test_reconcile_backoff_after_ordinary_failure_is_the_short_backoff(void)
{
    safety_ceiling_reconcile_backoff_t state = { 0 };
    int64_t now = 5000000000LL;

    safety_ceiling_reconcile_record_result(&state, now, false, SAFETY_CEILING_REFUSAL_OTHER);

    TEST_CHECK(!safety_ceiling_reconcile_should_attempt(&state, now),
               "must not retry in the same instant as an ordinary failure");
    TEST_CHECK(safety_ceiling_reconcile_should_attempt(&state, now + SAFETY_CEILING_RECONCILE_RETRY_BACKOFF_US),
               "the short backoff duration must clear an ordinary (OTHER-classified) failure");
    // Prove it is genuinely the SHORT one, not merely "the ARMED duration
    // also happens to have elapsed" -- a negative-test-shaped check: if the
    // implementation collapsed both branches onto the long duration, this
    // would fail where the assertion above would still pass.
    TEST_CHECK(SAFETY_CEILING_RECONCILE_RETRY_BACKOFF_US < SAFETY_CEILING_RECONCILE_ARMED_BACKOFF_BASE_US,
               "sanity: the two configured backoff constants must actually differ");
}

static void test_reconcile_backoff_storage_is_the_longest(void)
{
    // 2026-09-10 opus review finding C: KILNLINK_COMMIT_CONFIG_REJECT_STORAGE
    // (a real flash-write hardware fault on the Pico) must NOT get the short
    // backoff -- retrying faster cannot fix a failing flash write, and
    // hammering the link every few seconds forever is worse than the ARMED
    // case the backoff was originally written for.
    safety_ceiling_reconcile_backoff_t state = { 0 };
    int64_t now = 6000000000LL;

    safety_ceiling_reconcile_record_result(&state, now, false, SAFETY_CEILING_REFUSAL_STORAGE);

    TEST_CHECK(!safety_ceiling_reconcile_should_attempt(&state, now + SAFETY_CEILING_RECONCILE_RETRY_BACKOFF_US),
               "STORAGE must NOT use the short backoff -- a persistent flash fault retried every "
               "few seconds forever is the exact regression this class exists to prevent");
    TEST_CHECK(!safety_ceiling_reconcile_should_attempt(
                   &state, now + SAFETY_CEILING_RECONCILE_ARMED_BACKOFF_BASE_US +
                               SAFETY_CEILING_RECONCILE_ARMED_BACKOFF_JITTER_US),
               "STORAGE must back off longer than even the ARMED case");
    TEST_CHECK(safety_ceiling_reconcile_should_attempt(&state, now + SAFETY_CEILING_RECONCILE_STORAGE_BACKOFF_US),
               "the full STORAGE backoff duration must clear it");
}

static void test_reconcile_backoff_success_clears_it_immediately(void)
{
    safety_ceiling_reconcile_backoff_t state = { 0 };
    int64_t now = 2000000000LL;

    safety_ceiling_reconcile_record_result(&state, now, false, SAFETY_CEILING_REFUSAL_ARMED);
    TEST_CHECK(!safety_ceiling_reconcile_should_attempt(&state, now + 1),
               "sanity: the ARMED backoff must actually be pending before the success case below");

    // A relay that de-energises (or a config change that no longer needs a
    // raise) must be reflected the very next tick, not wait out an armed
    // backoff that no longer applies.
    safety_ceiling_reconcile_record_result(&state, now + 1, true, SAFETY_CEILING_REFUSAL_NONE);
    TEST_CHECK(safety_ceiling_reconcile_should_attempt(&state, now + 1),
               "a successful (or no-op) result must clear the backoff immediately, not merely shorten it");
    TEST_CHECK(safety_ceiling_reconcile_should_attempt(&state, now + 2),
               "and stay clear afterward, not just at the exact instant of success");
}

// ---------------------------------------------------------------------
// 6. Regression guard for the classifier itself, at the writer-plumbing
//    layer: a writer that cannot prove exactly which numbered reason
//    applied (the honest "I don't know" case -- e.g. a stashed rejection
//    frame missed the readback window) MUST report SAFETY_CEILING_REFUSAL_
//    OTHER, never guess ARMED. guard_raise() must carry that classification
//    through to out_refusal_class UNCHANGED -- this is what makes the
//    backoff safe even when the writer cannot identify the exact cause.
// ---------------------------------------------------------------------
static void test_unclassified_failure_reason_gets_other_not_armed(void)
{
    // Reason text mentions the Pico and reads as plausibly ARMED-adjacent,
    // but the writer HONESTLY reports OTHER because it could not actually
    // prove the numeric reason (this is exactly confirm_commit_landed()'s
    // "stashed rejection frame missed the window" case in
    // safety_cfg_http.c). A classifier that (wrongly) re-derived ARMED from
    // this prose would fail this test.
    safety_ceiling_reconcile_backoff_t state = { 0 };
    int64_t now = 3000000000LL;
    fake_writer_state_t w = { .result_to_return = false,
                               .reason_to_return = "the safety processor accepted the commit but this board "
                                                    "could not read the config back to confirm it -- treating "
                                                    "the write as UNCONFIRMED, not successful",
                               .class_to_return = SAFETY_CEILING_REFUSAL_OTHER };
    float new_zones[3] = { 80.0f, 80.0f, 200.0f };
    safety_ceiling_sync_result_t result;
    char reason[256] = { 0 };
    safety_ceiling_refusal_class_t refusal_class = SAFETY_CEILING_REFUSAL_ARMED; // deliberately wrong initial value

    bool may_commit = safety_ceiling_policy_guard_raise(80.0f, true, new_zones, 3, fake_writer, &w, &result, reason,
                                                          sizeof(reason), &refusal_class);
    TEST_CHECK(!may_commit, "sanity: this is still a failed raise");
    TEST_CHECK((refusal_class == SAFETY_CEILING_REFUSAL_OTHER),
               "a writer that cannot prove the reason must report OTHER, and guard_raise() must pass "
               "that through unchanged -- never substitute a guess");

    safety_ceiling_reconcile_record_result(&state, now, false, refusal_class);
    TEST_CHECK(safety_ceiling_reconcile_should_attempt(&state, now + SAFETY_CEILING_RECONCILE_RETRY_BACKOFF_US),
               "an unproven ('OTHER') failure must use the short backoff even when its reason text is "
               "Pico-related -- it must never earn the long ARMED backoff on prose alone");
}

void run_test_safety_ceiling_policy(void)
{
    test_raise_writes_pico_first_and_confirms();
    test_equal_ceiling_is_satisfied_no_write();
    test_lower_is_esp_first_pico_best_effort_after();
    test_failed_pico_raise_blocks_and_leaves_esp_unchanged();
    test_zero_ceiling_zone_excluded_from_maximum();
    test_reconcile_backoff_starts_open();
    test_reconcile_backoff_after_armed_refusal_is_the_long_backoff();
    test_reconcile_backoff_armed_jitter_varies_with_now();
    test_reconcile_backoff_after_ordinary_failure_is_the_short_backoff();
    test_reconcile_backoff_storage_is_the_longest();
    test_reconcile_backoff_success_clears_it_immediately();
    test_unclassified_failure_reason_gets_other_not_armed();
}
