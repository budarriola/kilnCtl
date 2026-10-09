// Host tests for App/drivers/persist/pico_update_attempts.c -- the
// persisted, per-pair attempt-budget counter behind
// docs/PICO_AUTO_UPDATE_PLAN.md sec 7. #includes the .c directly (same
// convention as test_boot_guard.c) to reach its static helpers, and uses
// fake_kv.h's RAM-backed hal_kv fake the same way.
#include <string.h>

#include "test_common.h"

#include "fake_kv.h"

#include "../drivers/persist/pico_update_attempts.c"

static void reset_store(void)
{
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
}

static void test_crc32_reference_vector(void)
{
    uint32_t crc = crc32_compute("123456789", 9);
    TEST_CHECK(crc == 0xCBF43926u, "crc32_compute reference vector (same algorithm as boot_guard.c)");
}

static void test_record_crc_detects_corruption(void)
{
    pico_update_attempts_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.version = PUA_RECORD_VERSION;
    rec.pair_hash = 0x1234u;
    rec.attempt_count = 2;
    rec.crc32 = record_crc(&rec);
    TEST_CHECK(record_is_valid(&rec), "freshly-computed CRC validates");

    rec.attempt_count ^= 1u;
    TEST_CHECK(!record_is_valid(&rec), "a corrupted attempt_count is rejected by the CRC");
}

static void test_pair_hash_distinguishes_expected_and_observed(void)
{
    uint8_t obs_a[] = "abc123";
    uint8_t obs_b[] = "def456";

    uint32_t h1 = pico_update_attempts_pair_hash("expectedA", obs_a, (uint8_t)strlen((char *)obs_a));
    uint32_t h2 = pico_update_attempts_pair_hash("expectedA", obs_b, (uint8_t)strlen((char *)obs_b));
    uint32_t h3 = pico_update_attempts_pair_hash("expectedB", obs_a, (uint8_t)strlen((char *)obs_a));
    uint32_t h4 = pico_update_attempts_pair_hash("expectedA", obs_a, (uint8_t)strlen((char *)obs_a));

    TEST_CHECK(h1 != h2, "different observed bytes hash differently (same expected)");
    TEST_CHECK(h1 != h3, "different expected strings hash differently (same observed)");
    TEST_CHECK(h1 == h4, "identical inputs hash identically -- deterministic");

    // Concatenation-boundary check: "ab"+"c" must not collide with "a"+"bc".
    uint8_t split1_obs[] = "c";
    uint8_t split2_obs[] = "bc";
    uint32_t hs1 = pico_update_attempts_pair_hash("ab", split1_obs, 1);
    uint32_t hs2 = pico_update_attempts_pair_hash("a", split2_obs, 2);
    TEST_CHECK(hs1 != hs2,
               "'ab'+'c' and 'a'+'bc' hash differently -- the length-prefixed encoding does not "
               "collide across a shifted boundary between the two fields");
}

static void test_fresh_pair_has_no_record(void)
{
    reset_store();
    uint32_t count = 999;
    bool failed = true;
    bool have = pico_update_attempts_load(0xABCDu, &count, &failed);
    TEST_CHECK(!have, "a pair never written has no record");
    TEST_CHECK(count == 0, "out_count defaults to 0 for no record");
    TEST_CHECK(!failed, "out_failed defaults to false for no record");
}

static void test_record_attempt_increments_and_verifies(void)
{
    reset_store();
    uint32_t pair = 0x1111u;
    uint32_t new_count = 0;

    TEST_CHECK(pico_update_attempts_record_attempt(pair, 0, &new_count), "first attempt verifies");
    TEST_CHECK(new_count == 1u, "first attempt for a fresh pair starts at 1");

    TEST_CHECK(pico_update_attempts_record_attempt(pair, 0, &new_count), "second attempt verifies");
    TEST_CHECK(new_count == 2u, "second attempt for the SAME pair increments rather than resetting");

    uint32_t loaded_count = 0;
    TEST_CHECK(pico_update_attempts_load(pair, &loaded_count, NULL), "the pair now has a record");
    TEST_CHECK(loaded_count == 2u, "load reflects the persisted increments");
}

static void test_new_pair_gets_a_fresh_budget(void)
{
    // Plan sec 7's explicit requirement: "a new mismatch gets its own fresh
    // budget and a stuck one cannot ratchet" -- pin that a DIFFERENT pair
    // does not inherit a prior pair's count, even though this is a
    // single-slot record (only one pair remembered at a time).
    reset_store();
    uint32_t pair_a = 0xAAAAu;
    uint32_t pair_b = 0xBBBBu;
    uint32_t new_count = 0;

    for (int i = 0; i < 3; i++) {
        TEST_CHECK(pico_update_attempts_record_attempt(pair_a, 0, &new_count), "pair A attempt verifies");
    }
    TEST_CHECK(new_count == 3u, "pair A ran up to 3 attempts");

    // NEGATIVE-TEST-SHAPED CHECK: without the pair_hash discriminator (i.e.
    // if the record only ever tracked a bare count regardless of which pair
    // it was for), pair B's first attempt would wrongly read as continuing
    // pair A's count of 3 -- this is exactly the "reset one side of a pair"
    // class CLAUDE.md warns about, applied to two DIFFERENT real mismatches
    // sharing one persisted slot.
    TEST_CHECK(pico_update_attempts_record_attempt(pair_b, 0, &new_count), "pair B's first attempt verifies");
    TEST_CHECK(new_count == 1u,
               "pair B starts at 1, NOT 4 -- a new (expected, observed) pair must not inherit an "
               "unrelated prior pair's attempt count");

    uint32_t pair_a_count_now = 999;
    bool pair_a_have = pico_update_attempts_load(pair_a, &pair_a_count_now, NULL);
    TEST_CHECK(!pair_a_have,
               "pair A's record is gone now that the single slot holds pair B -- loading pair A "
               "reports no record (a fresh budget), never a stale nonzero count from before the "
               "slot was overwritten by a different pair");
}

static void test_record_failure_sets_flag_without_touching_count(void)
{
    reset_store();
    uint32_t pair = 0x2222u;
    uint32_t new_count = 0;
    TEST_CHECK(pico_update_attempts_record_attempt(pair, 0, &new_count), "one attempt recorded");
    TEST_CHECK(new_count == 1u, "count is 1 before the failure");

    TEST_CHECK(pico_update_attempts_record_failure(pair), "record_failure verifies");

    uint32_t count_after = 0;
    bool failed_after = false;
    TEST_CHECK(pico_update_attempts_load(pair, &count_after, &failed_after), "record still present");
    TEST_CHECK(count_after == 1u, "record_failure does not change the attempt count");
    TEST_CHECK(failed_after, "record_failure sets the terminal-failure flag");
}

static void test_attempt_after_failure_preserves_failed_flag(void)
{
    // pico_auto_update.h's decide() checks prior_attempt_failed BEFORE the
    // budget, so once a pair is marked failed it must STAY marked failed on
    // a subsequent recorded attempt for the same pair -- clearing it would
    // silently let a permanently-abandoned pair look retryable again.
    reset_store();
    uint32_t pair = 0x3333u;
    uint32_t new_count = 0;
    TEST_CHECK(pico_update_attempts_record_attempt(pair, 0, &new_count), "attempt 1");
    TEST_CHECK(pico_update_attempts_record_failure(pair), "marked failed");
    TEST_CHECK(pico_update_attempts_record_attempt(pair, 0, &new_count), "attempt 2, same pair");

    bool failed_after = false;
    TEST_CHECK(pico_update_attempts_load(pair, NULL, &failed_after), "record still present");
    TEST_CHECK(failed_after,
               "the failed flag survives a later recorded attempt for the SAME pair -- it must not "
               "be silently cleared, or a permanently-abandoned pair would look retryable again");
}

static void test_clear_removes_the_record(void)
{
    reset_store();
    uint32_t pair = 0x4444u;
    uint32_t new_count = 0;
    TEST_CHECK(pico_update_attempts_record_attempt(pair, 0, &new_count), "attempt recorded");
    TEST_CHECK(pico_update_attempts_clear(), "clear reports verified");

    uint32_t count_after = 999;
    bool have = pico_update_attempts_load(pair, &count_after, NULL);
    TEST_CHECK(!have, "after clear, the pair has no record");
    TEST_CHECK(count_after == 0, "and out_count defaults to 0");

    // Clearing an already-empty store must also report success (nothing to
    // clear is not a failure).
    TEST_CHECK(pico_update_attempts_clear(), "clearing an already-clear store still verifies");
}

// ---------------------------------------------------------------------------
// Write-lies coverage, mirroring test_boot_guard.c's
// test_reset_counter_recovers_from_a_single_lying_write() /
// _refuses_success_when_every_write_lies(): a HAL_OK write whose value does
// not actually reach flash must not be trusted, per the 2026-09-08 audit
// this module's header explicitly cites as the reason to copy the pattern
// rather than trust a bare return code.
// ---------------------------------------------------------------------------
static void test_record_attempt_recovers_from_a_single_lying_write(void)
{
    reset_store();
    uint32_t pair = 0x5555u;
    uint32_t new_count = 0;

    fake_kv_script_silent_set_noops(1u);
    TEST_CHECK(pico_update_attempts_record_attempt(pair, 0, &new_count),
               "the bounded erase-then-retry recovers when only the first write lies");

    uint32_t loaded = 0;
    TEST_CHECK(pico_update_attempts_load(pair, &loaded, NULL), "record actually landed");
    TEST_CHECK(loaded == 1u, "the genuinely-persisted count is 1, not just a claimed return value");
}

static void test_record_attempt_refuses_success_when_every_write_lies(void)
{
    reset_store();
    uint32_t pair = 0x6666u;
    uint32_t new_count = 0;

    fake_kv_script_silent_set_noops(2u); /* both the first attempt AND the retry lie */
    bool verified = pico_update_attempts_record_attempt(pair, 0, &new_count);
    TEST_CHECK(!verified,
               "record_attempt reports FAILURE when neither the write nor its retry actually took "
               "-- a return-code-only version would have wrongly reported success here, which is "
               "exactly the 2026-09-08 boot_guard failure shape this module copies the fix for");
}

// Embedded-slot alternation (docs/PICO_AUTO_UPDATE_PLAN.md sec 9 step 4): a
// fresh pair always starts at slot 0 (A); each recorded attempt flips which
// slot the NEXT attempt for the SAME pair should try, and that choice
// survives a simulated reboot (a fresh in-memory struct, same NVS-backed
// fake_kv store) since it is read back from the persisted record, not from
// any process-lifetime state.
static void test_next_slot_alternates_and_survives_reboot(void)
{
    reset_store();
    uint32_t pair = 0xA1B2C3D4u;
    int slot = -1;

    bool have = pico_update_attempts_next_slot(pair, false, false, &slot);
    TEST_CHECK(!have, "a fresh pair has no prior record");
    TEST_CHECK(slot == 0, "a fresh pair's first suggested slot is A (0)");

    uint32_t new_count = 0;
    TEST_CHECK(pico_update_attempts_record_attempt(pair, 0, &new_count), "attempt 1 tries slot A");

    // Simulate a reboot: nothing but the NVS-backed fake_kv store survives.
    have = pico_update_attempts_next_slot(pair, false, false, &slot);
    TEST_CHECK(have, "the slot A attempt is readable after a simulated reboot");
    TEST_CHECK(slot == 1, "after slot A was tried, the next suggested slot is B (1)");

    TEST_CHECK(pico_update_attempts_record_attempt(pair, 1, &new_count), "attempt 2 tries slot B");
    have = pico_update_attempts_next_slot(pair, false, false, &slot);
    TEST_CHECK(have, "the slot B attempt is readable after a simulated reboot");
    TEST_CHECK(slot == 0, "after slot B was tried, the next suggested slot flips back to A (0)");

    // A different pair (e.g. a corrected expected commit) is a fresh budget
    // per this module's own header comment, and must ALSO be a fresh
    // alternation -- it must not inherit slot B's "next is A" from the
    // unrelated pair above.
    uint32_t other_pair = 0xDEADBEEFu;
    have = pico_update_attempts_next_slot(other_pair, false, false, &slot);
    TEST_CHECK(!have, "an unrelated pair has no record of its own");
    TEST_CHECK(slot == 0, "an unrelated pair's first suggested slot is still A (0), not inherited");
}

// Owner decision 2026-10-02: the Pico's reported active slot wins; the
// persisted guess is used only when the wire slot is unknown.
static void test_select_slot_pure_truth_table(void)
{
    bool dis = true;
    TEST_CHECK(pico_update_attempts_select_slot(false, 0, true, false, &dis) == 1 && !dis,
               "known A, fresh pair -> next B, no disagreement without a record");
    TEST_CHECK(pico_update_attempts_select_slot(false, 0, true, true, &dis) == 0 && !dis,
               "known B, fresh pair -> next A");
    TEST_CHECK(pico_update_attempts_select_slot(true, 0, true, false, &dis) == 1 && !dis,
               "known A, last tried A -> agree, next B");
    TEST_CHECK(pico_update_attempts_select_slot(true, 1, true, true, &dis) == 0 && !dis,
               "known B, last tried B -> agree, next A");
    TEST_CHECK(pico_update_attempts_select_slot(true, 0, true, true, &dis) == 0 && dis,
               "known B but last tried A -> wire wins (A), disagreement");
    TEST_CHECK(pico_update_attempts_select_slot(true, 1, true, false, &dis) == 1 && dis,
               "known A but last tried B -> wire wins (B), disagreement");
    TEST_CHECK(pico_update_attempts_select_slot(true, 0, false, true, &dis) == 1 && !dis,
               "unknown wire ignores the wire value, falls back to the guess");
    TEST_CHECK(pico_update_attempts_select_slot(false, 0, false, false, NULL) == 0,
               "unknown wire, fresh pair -> A, NULL out_disagree tolerated");
}

static void test_next_slot_wire_known_a_and_b(void)
{
    reset_store();
    uint32_t pair = 0x11112222u;
    int slot = -1;
    bool have = pico_update_attempts_next_slot(pair, true, false, &slot);
    TEST_CHECK(!have && slot == 1, "fresh pair, Pico reports A -> push B");
    have = pico_update_attempts_next_slot(pair, true, true, &slot);
    TEST_CHECK(!have && slot == 0, "fresh pair, Pico reports B -> push A");
}

static void test_next_slot_unknown_wire_uses_guess(void)
{
    reset_store();
    uint32_t pair = 0x33334444u;
    uint32_t n = 0;
    int slot = -1;
    TEST_CHECK(pico_update_attempts_record_attempt(pair, 1, &n), "record slot B attempt");
    // The wire value passed alongside wire_known=false must be ignored.
    (void)pico_update_attempts_next_slot(pair, false, false, &slot);
    TEST_CHECK(slot == 0, "unknown wire, last tried B -> guess says A");
    (void)pico_update_attempts_next_slot(pair, false, true, &slot);
    TEST_CHECK(slot == 0, "unknown wire ignores wire_active_is_b");
}

static void test_next_slot_disagreement_wire_wins_and_guess_updated(void)
{
    reset_store();
    uint32_t pair = 0x55556666u;
    uint32_t n = 0;
    int slot = -1;
    TEST_CHECK(pico_update_attempts_record_attempt(pair, 0, &n), "attempt 1 tried A");
    TEST_CHECK(pico_update_attempts_record_attempt(pair, 0, &n) && n == 2, "attempt 2 tried A");
    TEST_CHECK(pico_update_attempts_record_failure(pair), "mark failed");

    // Guess says last=A (next B); Pico reports it is on B (next must be A).
    bool have = pico_update_attempts_next_slot(pair, true, true, &slot);
    TEST_CHECK(have, "record existed");
    TEST_CHECK(slot == 0, "disagreement: the Pico's report wins, next is A");

    pico_update_attempts_record_t rec;
    TEST_CHECK(load_record(&rec), "record still valid after the guess update");
    TEST_CHECK(rec.last_slot == 1u, "persisted guess updated to the reported active slot B");
    TEST_CHECK(rec.attempt_count == 2u && rec.failed == 1u, "count and failed flag preserved");

    // With the wire now unknown the corrected guess drives the answer: no drift.
    (void)pico_update_attempts_next_slot(pair, false, false, &slot);
    TEST_CHECK(slot == 0, "after the update the guess alone agrees (last B -> next A)");
}

void run_test_pico_update_attempts(void)
{
    test_crc32_reference_vector();
    test_record_crc_detects_corruption();
    test_pair_hash_distinguishes_expected_and_observed();
    test_fresh_pair_has_no_record();
    test_record_attempt_increments_and_verifies();
    test_new_pair_gets_a_fresh_budget();
    test_record_failure_sets_flag_without_touching_count();
    test_attempt_after_failure_preserves_failed_flag();
    test_clear_removes_the_record();
    test_record_attempt_recovers_from_a_single_lying_write();
    test_record_attempt_refuses_success_when_every_write_lies();
    test_next_slot_alternates_and_survives_reboot();
    test_select_slot_pure_truth_table();
    test_next_slot_wire_known_a_and_b();
    test_next_slot_unknown_wire_uses_guess();
    test_next_slot_disagreement_wire_wins_and_guess_updated();
}
