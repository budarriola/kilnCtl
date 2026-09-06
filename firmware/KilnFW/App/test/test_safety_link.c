// Host tests for App/drivers/safety/safety_link.h's pure, host-testable pieces --
// added alongside SaftyFW/TODO.md item 0.6 ("redefine SAFETY_FAULT_SRC_
// SAFETY_LINK as 'no telemetry within 1.5 s'"). safety_link_is_stale() is a
// `static inline`, dependency-free comparison (no locking, no hardware, no
// FreeRTOS call) -- exactly the same "pure logic lives where it can be
// host-tested" split profile_executor.h's watchdog decision function
// documents, just smaller: one comparison instead of a whole classifier.
//
// safety_link.h itself pulls in esp_err.h/freertos headers/uart_owner.h/
// uart_protocol.h purely for SafetyLinkClass's field types -- test_backup_
// import.c already proved this header includes cleanly against App/test/
// stubs/ (added 2026-08-21 for exactly that), so this file reuses the same
// stub surface rather than adding anything new.
//
// What this closes: nothing in the existing suite exercised safety_link_is_
// stale() itself, or pinned the two LINK_PROTOCOL.md sec 8 constants
// (SAFETY_LINK_STALE_MS = 1500, SAFETY_LINK_FIRING_ABORT_SILENCE_MS =
// 30000) to their documented values -- a future edit could silently drift
// either number (e.g. "helpfully" rounding 1500 to 1000, or reusing the
// firing-abort constant for the fault-source check) with nothing here to
// catch it.
#include <stdint.h>

#include "test_common.h"
#include "../drivers/safety/safety_link.h"

static void test_is_stale_boundary_at_threshold(void)
{
    TEST_SECTION("safety_link_is_stale -- exactly at the threshold is NOT yet stale");

    // "> threshold_ms", not ">=" -- an age reading exactly equal to the
    // threshold is the sample that arrived right on time, not one that
    // missed it.
    TEST_CHECK(safety_link_is_stale(1500u, 1500u) == false,
               "age == threshold is not stale (strict greater-than)");
    TEST_CHECK(safety_link_is_stale(1501u, 1500u) == true,
               "one ms past the threshold is stale");
    TEST_CHECK(safety_link_is_stale(1499u, 1500u) == false,
               "one ms short of the threshold is not stale");
}

static void test_is_stale_never_received_is_always_stale(void)
{
    TEST_SECTION("safety_link_is_stale -- SAFETY_LINK_AGE_NEVER is always stale");

    // "Absence is not proof of safety" (LINK_PROTOCOL.md sec 8): a link that
    // has never produced a frame must read stale against ANY threshold,
    // including a deliberately huge one -- there is no age at which
    // "nothing has ever arrived" becomes acceptable.
    TEST_CHECK(safety_link_is_stale(SAFETY_LINK_AGE_NEVER, SAFETY_LINK_STALE_MS) == true,
               "never-received is stale against the 1.5s threshold");
    TEST_CHECK(safety_link_is_stale(SAFETY_LINK_AGE_NEVER, SAFETY_LINK_FIRING_ABORT_SILENCE_MS) == true,
               "never-received is stale against the 30s threshold too");
    TEST_CHECK(safety_link_is_stale(SAFETY_LINK_AGE_NEVER, 0xFFFFFFFFu) == true,
               "never-received is stale against an arbitrarily large threshold");
}

static void test_is_stale_two_independent_thresholds(void)
{
    TEST_SECTION("safety_link_is_stale -- the 1.5s and 30s checks are genuinely independent");

    // LINK_PROTOCOL.md sec 8's whole point: a single dropped frame (age just
    // past 1.5s) must trip the fault-source (new relay-on refused) WITHOUT
    // also crossing the 30s firing-abort threshold. Same age, two different
    // verdicts depending on which constant it's compared against -- that's
    // what "two timeouts, not one" means in code, not just in the doc.
    uint16_t age_one_dropped_frame = 1600u; // one poll period past 1.5s
    TEST_CHECK(safety_link_is_stale(age_one_dropped_frame, SAFETY_LINK_STALE_MS) == true,
               "1.6s of silence trips the 1.5s liveness check (blocks new relay-on)");
    TEST_CHECK(safety_link_is_stale(age_one_dropped_frame, SAFETY_LINK_FIRING_ABORT_SILENCE_MS) == false,
               "the SAME 1.6s of silence must NOT trip the 30s firing-abort check -- "
               "a single dropped frame must not abort a firing already in progress");
}

static void test_stale_ms_constant_is_1500(void)
{
    TEST_SECTION("SAFETY_LINK_STALE_MS -- pinned at 1500 (LINK_PROTOCOL.md sec 8: '> 1.5s')");

    // Regression pin: this constant is what "the safety processor must be
    // alive to heat" actually means in code. A silent change here (e.g. to
    // 3000, matching SAFETY_LINK_UP_PERIODS * some other period) would
    // loosen the liveness guarantee without touching a single guard.
    TEST_CHECK(SAFETY_LINK_STALE_MS == 1500u, "the fault-source staleness ceiling is exactly 1500 ms");
}

static void test_firing_abort_ms_constant_is_30000(void)
{
    TEST_SECTION("SAFETY_LINK_FIRING_ABORT_SILENCE_MS -- pinned at 30000 (LINK_PROTOCOL.md sec 8)");

    TEST_CHECK(SAFETY_LINK_FIRING_ABORT_SILENCE_MS == 30000u,
               "the firing-abort silence ceiling is exactly 30000 ms");
    TEST_CHECK(SAFETY_LINK_FIRING_ABORT_SILENCE_MS > SAFETY_LINK_STALE_MS,
               "the firing-abort ceiling is strictly larger than the fault-source ceiling -- "
               "aborting a run must always be the SLOWER of the two reactions");
}

// --------------------------------------------------------------------------
// safety_drain_still_waiting() -- bug fix 2026-08-23. Root cause: safety_
// drain_inbox_ex()'s receive loop used to hard-code `wait = 0` after the
// FIRST inbox message, no matter what it was. safety_link_get_config_page()
// (and its siblings safety_link_get_ct_cal()/safety_link_send_commit_
// config()) rely on that loop to keep blocking until the ONE specific
// shared-id reply they asked for shows up -- but the Pico also emits
// periodic/unsolicited broadcasts (GET_STATUS, DIAG, POWER, TRIP_EVENT,
// FW_VERSION) on the very same inbox, and CONFIG_PAGE is this link's
// slowest reply to produce. Whenever one of those unrelated frames arrived
// before the real reply -- which live hardware measurement (2026-08-23)
// showed happening on essentially every attempt -- the loop degraded to a
// non-blocking drain immediately afterward, found the real reply not yet
// queued, and exited having burned only a few ms of the ~1.2s budget.
// safety_link_get_config_page() then reported ESP_ERR_TIMEOUT on every
// single call even though the Pico answered every single request
// (s_diag_get_config_page_handled_count == s_diag_get_config_page_seen_count
// == 203 on the bench, ESP side timing out 203/203 times).
// --------------------------------------------------------------------------

static void test_drain_wait_not_waiting_for_anything_never_blocks(void)
{
    TEST_SECTION("safety_drain_still_waiting -- a plain drain (no out-params) never keeps blocking");

    // This is safety_drain_inbox()'s own case (all out-params NULL, so every
    // want_* is false at the call site) -- the periodic poll's pre-drain and
    // GET_STATUS wait must keep their original "grab one burst, don't block
    // for more" behaviour untouched by this fix.
    TEST_CHECK(safety_drain_still_waiting(false, false, false, false, false, false, false, false, false, false) == false,
               "nothing wanted, nothing gotten -- never keep blocking");
    TEST_CHECK(safety_drain_still_waiting(false, false, false, true, false, true, false, true, false, false) == false,
               "nothing wanted even if the got_* flags are (implausibly) true -- still never block");
}

static void test_drain_wait_config_page_wanted_not_yet_captured_keeps_waiting(void)
{
    TEST_SECTION("safety_drain_still_waiting -- CONFIG_PAGE wanted but not yet captured: keep waiting");

    // This is exactly the bug: safety_link_get_config_page() wants
    // CONFIG_PAGE, an unrelated frame (e.g. DIAG) was just dispatched, and
    // CONFIG_PAGE has not arrived yet. The pre-fix code would abandon the
    // wait here; the fix must not.
    TEST_CHECK(safety_drain_still_waiting(false, false, false, false, /*want_config_page=*/true,
                                           /*got_config_page=*/false, false, false, false, false) == true,
               "an unrelated frame must not end the wait while CONFIG_PAGE is still outstanding");
}

static void test_drain_wait_config_page_captured_stops_waiting(void)
{
    TEST_SECTION("safety_drain_still_waiting -- CONFIG_PAGE wanted and captured: stop waiting");

    TEST_CHECK(safety_drain_still_waiting(false, false, false, false, /*want_config_page=*/true,
                                           /*got_config_page=*/true, false, false, false, false) == false,
               "once the wanted CONFIG_PAGE reply has been captured, degrade to a zero-wait drain");
}

static void test_drain_wait_ct_cal_and_commit_rejected_same_shape(void)
{
    TEST_SECTION("safety_drain_still_waiting -- CT_CAL and COMMIT_CONFIG_REJECTED share the identical shape");

    // safety_link_get_ct_cal() and safety_link_send_commit_config() hit the
    // exact same bug as safety_link_get_config_page() -- same drain loop,
    // same premature-degrade failure mode -- so the fix must cover them too.
    TEST_CHECK(safety_drain_still_waiting(false, false, /*want_ct_cal=*/true, /*got_ct_cal=*/false, false, false, false,
                                           false, false, false) == true,
               "CT_CAL wanted, not yet captured -- keep waiting (safety_link_get_ct_cal())");
    TEST_CHECK(safety_drain_still_waiting(false, false, /*want_ct_cal=*/true, /*got_ct_cal=*/true, false,
                                           false, false, false, false, false) == false,
               "CT_CAL wanted and captured -- stop waiting");
    TEST_CHECK(safety_drain_still_waiting(false, false, false, false, false, false,
                                           /*want_commit_rejected=*/true,
                                           /*got_commit_rejected=*/false, false, false) == true,
               "COMMIT_CONFIG_REJECTED wanted, not yet captured -- keep waiting "
               "(safety_link_send_commit_config())");
    TEST_CHECK(safety_drain_still_waiting(false, false, false, false, false, false,
                                           /*want_commit_rejected=*/true,
                                           /*got_commit_rejected=*/true, false, false) == false,
               "COMMIT_CONFIG_REJECTED wanted and captured -- stop waiting");
}

static void test_drain_wait_only_the_wanted_reply_gates_waiting(void)
{
    TEST_SECTION("safety_drain_still_waiting -- an unrequested type's got_* flag is never consulted");

    // Every real call site passes at most one non-NULL out-param pair, so
    // in practice at most one want_* is ever true -- but the function must
    // not accidentally gate on a got_* flag for a type that was never
    // requested (a caller might pass got_ct_cal=false there simply because
    // it never touched that local at all).
    TEST_CHECK(safety_drain_still_waiting(/*want_status=*/false, /*got_status=*/false,
                                           /*want_ct_cal=*/false, /*got_ct_cal=*/false,
                                           /*want_config_page=*/true, /*got_config_page=*/true,
                                           /*want_commit_rejected=*/false,
                                           /*got_commit_rejected=*/false, false, false) == false,
               "only CONFIG_PAGE was wanted and it was captured -- the untouched "
               "ct_cal/commit_rejected flags must not force continued waiting");
}

// --------------------------------------------------------------------------
// safety_drain_still_waiting() -- round 3, 2026-08-24. The round-1 fix above
// gave CT_CAL/CONFIG_PAGE/COMMIT_CONFIG_REJECTED a way to say what they were
// waiting for, and left the FOURTH caller -- the ordinary GET_STATUS poll --
// with none. A plain safety_drain_inbox() passes no out-params, so every
// want_* was false, this predicate returned false after the first frame, and
// the wait degraded to zero exactly as the round-1 bug did. The Pico sends 4
// STATUS frames per DIAG and per POWER push on the same inbox, so a DIAG or
// POWER arriving first ended the poll with got_status false and counted a
// stats.timeouts -- milliseconds before the STATUS it asked for landed.
//
// Bench measurement, 2026-08-24, commit 8d1b015 at 9600 baud / 500 ms poll:
// sent 14471, received 14472, crc/framing errors 0, timeouts 3616, diag
// applied 3618, power applied 3618. Timeouts tracking the DIAG/POWER pushes
// 1:1, with zero CRC errors and a STATUS count equal to the send count, is
// the signature: a quarter of every poll reported as a failure on a link that
// answered every single request.
// --------------------------------------------------------------------------

static void test_drain_wait_status_wanted_not_yet_captured_keeps_waiting(void)
{
    TEST_SECTION("safety_drain_still_waiting -- STATUS wanted but not yet captured: keep waiting");

    // The round-3 bug itself: the poll wants a STATUS, an unrelated DIAG or
    // POWER push was just dispatched, and the STATUS has not arrived yet.
    TEST_CHECK(safety_drain_still_waiting(/*want_status=*/true, /*got_status=*/false, false, false,
                                           false, false, false, false, false, false) == true,
               "a DIAG/POWER push must not end the poll while its STATUS is still outstanding");
}

static void test_drain_wait_status_captured_stops_waiting(void)
{
    TEST_SECTION("safety_drain_still_waiting -- STATUS wanted and captured: stop waiting");

    TEST_CHECK(safety_drain_still_waiting(/*want_status=*/true, /*got_status=*/true, false, false,
                                           false, false, false, false, false, false) == false,
               "once the STATUS has been applied, degrade to a zero-wait drain");
}

static void test_drain_wait_opportunistic_drains_are_unchanged(void)
{
    TEST_SECTION("safety_drain_still_waiting -- an opportunistic drain still never blocks for STATUS");

    // safety_drain_inbox() (want_status false) is used by every pre-drain and
    // by the non-expect_status path, all of which must keep their original
    // "take what has arrived, do not block" behaviour. This is the assertion
    // that fails if someone makes want_status default to true for all callers
    // rather than only the poll.
    TEST_CHECK(safety_drain_still_waiting(/*want_status=*/false, /*got_status=*/false, false, false,
                                           false, false, false, false, false, false) == false,
               "want_status false: never keep blocking, whatever arrived");
    TEST_CHECK(safety_drain_still_waiting(/*want_status=*/false, /*got_status=*/true, false, false,
                                           false, false, false, false, false, false) == false,
               "want_status false with a status incidentally applied: still never block");
}

// --------------------------------------------------------------------------
// safety_link_rollback_infer_outcome() -- the opus-review "blocking defect"
// fix. Previously, safety_link_send_rollback_ex() inferred ACCEPTED from
// plain silence (no reply within SAFETY_LINK_REPLY_TIMEOUT_MS) gated by a
// version read of the WRONG side (this ESP's cached view of the Pico's
// protocol_version, when what actually decides whether the Pico SENDS a
// refusal is the Pico's own cached view of the ESP's protocol_version -- a
// value this ESP cannot observe at all). The fix: silence alone proves
// nothing; the ONLY accepted positive evidence for ACCEPTED is the peer's
// boot_id changing (a direct, direction-agnostic observation of a reboot),
// and a wire-confirmed refusal always outranks it. This function is the
// pure decision at the center of that fix, extracted specifically so the
// inference itself can be pinned here without needing safety_link.c's
// FreeRTOS-timed send-burst/boot_id-watch loop (which this test file, by
// its own scope, does not attempt to drive -- see test_safety_link_
// compile.c's header comment on what it does NOT close).
// --------------------------------------------------------------------------

static void test_rollback_infer_no_evidence_is_unknown_not_accepted(void)
{
    TEST_SECTION("safety_link_rollback_infer_outcome -- no refusal and no boot_id change "
                 "(a lost request, a lost reply, a peer too busy to answer, or a link that "
                 "simply stayed down the whole watch) is UNKNOWN, never ACCEPTED");

    TEST_CHECK(safety_link_rollback_infer_outcome(false, false, false) ==
                   SAFETY_LINK_ROLLBACK_OUTCOME_UNKNOWN_TIMEOUT,
               "no wire evidence at all -- UNKNOWN_TIMEOUT, NOT the old silence-is-accepted default");
}

static void test_rollback_infer_refusal_wins(void)
{
    TEST_SECTION("safety_link_rollback_infer_outcome -- a decoded refusal is REFUSED");

    TEST_CHECK(safety_link_rollback_infer_outcome(/*refusal_received=*/true, /*refusal_decoded_ok=*/true,
                                                   /*boot_id_changed=*/false) ==
                   SAFETY_LINK_ROLLBACK_OUTCOME_REFUSED,
               "a wire-confirmed refusal, boot_id unchanged -- REFUSED");
    TEST_CHECK(safety_link_rollback_infer_outcome(/*refusal_received=*/true, /*refusal_decoded_ok=*/true,
                                                   /*boot_id_changed=*/true) ==
                   SAFETY_LINK_ROLLBACK_OUTCOME_REFUSED,
               "a refusal frame always outranks a boot_id change that happened to coincide with it "
               "(e.g. an unrelated Pico reboot racing the same watch window) -- REFUSED, not ACCEPTED");
}

static void test_rollback_infer_undecodable_refusal_is_unknown(void)
{
    TEST_SECTION("safety_link_rollback_infer_outcome -- a ROLLBACK_RESULT-shaped frame that failed "
                 "to decode proves nothing, so it must not be reported as a refusal");

    TEST_CHECK(safety_link_rollback_infer_outcome(/*refusal_received=*/true, /*refusal_decoded_ok=*/false,
                                                   /*boot_id_changed=*/false) ==
                   SAFETY_LINK_ROLLBACK_OUTCOME_UNKNOWN_TIMEOUT,
               "id/length gate matched but the codec itself rejected it -- UNKNOWN, not REFUSED");
    TEST_CHECK(safety_link_rollback_infer_outcome(/*refusal_received=*/true, /*refusal_decoded_ok=*/false,
                                                   /*boot_id_changed=*/true) ==
                   SAFETY_LINK_ROLLBACK_OUTCOME_UNKNOWN_TIMEOUT,
               "same, even with an (unrelated) boot_id change alongside it -- an undecodable frame "
               "still must not be promoted to a refusal by a boot_id coincidence");
}

static void test_rollback_infer_boot_id_change_after_silence_is_accepted(void)
{
    TEST_SECTION("safety_link_rollback_infer_outcome -- no refusal, but the peer's boot_id changed "
                 "-- ACCEPTED (the defect-2 fix: this is a direct reboot observation, not a version "
                 "guess about which side can/would have replied)");

    TEST_CHECK(safety_link_rollback_infer_outcome(/*refusal_received=*/false, /*refusal_decoded_ok=*/false,
                                                   /*boot_id_changed=*/true) ==
                   SAFETY_LINK_ROLLBACK_OUTCOME_ACCEPTED,
               "silence plus an observed boot_id change -- ACCEPTED");
}

static void test_rollback_boot_id_unknown_before_is_not_evidence(void)
{
    TEST_SECTION("safety_link_rollback_boot_id_changed -- with no baseline (the boot_id was never "
                 "learned before the request went out) a boot_id merely BECOMING known during the "
                 "watch is not evidence of a reboot");

    // The lossy-link case this guards: link_up is true (STATUS replies are
    // landing) but every FW_VERSION reply so far has been lost, so
    // pico_boot_id_known is still false when the rollback goes out. The
    // Pico silently ignores/refuses the request and keeps running; the next
    // FW_VERSION finally gets through. Treating that as "changed" would
    // report ACCEPTED for a rollback that never happened -- the exact
    // silence-plus-an-unrelated-event class this path exists to remove.
    TEST_CHECK(safety_link_rollback_boot_id_changed(/*had_before=*/false, /*before=*/0,
                                                     /*known_now=*/true, /*now=*/7) == false,
               "unknown before, known after -- NO baseline, so no evidence, never ACCEPTED");
    TEST_CHECK(safety_link_rollback_boot_id_changed(false, 0, false, 0) == false,
               "unknown before and still unknown -- no evidence");
    TEST_CHECK(safety_link_rollback_infer_outcome(false, false,
                                                   safety_link_rollback_boot_id_changed(false, 0, true, 7)) ==
                   SAFETY_LINK_ROLLBACK_OUTCOME_UNKNOWN_TIMEOUT,
               "fed through the same inference the watch uses, that case reports UNKNOWN_TIMEOUT");
}

static void test_rollback_boot_id_real_change_is_evidence(void)
{
    TEST_SECTION("safety_link_rollback_boot_id_changed -- a known baseline that then differs IS the "
                 "reboot observation ACCEPTED rests on, and an unchanged one is not");

    TEST_CHECK(safety_link_rollback_boot_id_changed(/*had_before=*/true, /*before=*/3,
                                                     /*known_now=*/true, /*now=*/4) == true,
               "known before, different after -- a real, observed reboot");
    TEST_CHECK(safety_link_rollback_boot_id_changed(true, 3, true, 3) == false,
               "known before, identical after -- the Pico never restarted");
    TEST_CHECK(safety_link_rollback_boot_id_changed(true, 3, false, 0) == false,
               "known before but not known now -- nothing was observed, not a change");
    TEST_CHECK(safety_link_rollback_infer_outcome(false, false,
                                                   safety_link_rollback_boot_id_changed(true, 3, true, 4)) ==
                   SAFETY_LINK_ROLLBACK_OUTCOME_ACCEPTED,
               "and only that case reaches ACCEPTED through the watch's own inference");
}

// --------------------------------------------------------------------------
// safety_link_rollback_build_identity_changed() / safety_link_rollback_
// reboot_confirmed() -- opus-review finding 2, "a reboot is not a
// rollback". A boot_id change alone (the fix above) is not proof of a
// ROLLBACK specifically: an unrelated crash/watchdog/power-glitch reboot
// inside the same watch window changes the boot_id exactly the same way.
// The build identity (commit+datetime from the same FW_VERSION frame) is
// independent evidence -- a rollback reboots into a DIFFERENT image, an
// unrelated reboot comes back running the SAME one -- and ACCEPTED now
// requires both to have changed.
// --------------------------------------------------------------------------

static void test_build_identity_unknown_before_or_after_is_not_evidence(void)
{
    TEST_SECTION("safety_link_rollback_build_identity_changed -- no baseline (or no fresh "
                 "read) means no evidence, same convention as the boot_id version");

    uint8_t commit_a[4] = {1, 2, 3, 4};
    uint8_t commit_b[4] = {5, 6, 7, 8};
    uint8_t dt[2] = {9, 9};

    TEST_CHECK(safety_link_rollback_build_identity_changed(/*had_before=*/false, 4, commit_a, 2, dt,
                                                             /*known_now=*/true, 4, commit_b, 2, dt) == false,
               "no build identity known before the request went out -- no baseline, never reported changed");
    TEST_CHECK(safety_link_rollback_build_identity_changed(/*had_before=*/true, 4, commit_a, 2, dt,
                                                             /*known_now=*/false, 4, commit_b, 2, dt) == false,
               "known before but not known now (no fresh FW_VERSION parsed yet) -- nothing observed");
}

static void test_build_identity_real_change_is_evidence(void)
{
    TEST_SECTION("safety_link_rollback_build_identity_changed -- a known baseline that then "
                 "differs (commit, datetime, or either length) IS a change; an identical "
                 "re-read is not");

    uint8_t commit_a[4] = {1, 2, 3, 4};
    uint8_t commit_b[4] = {1, 2, 3, 9}; // one byte differs
    uint8_t dt_a[3] = {5, 6, 7};
    uint8_t dt_b[3] = {5, 6, 7};

    TEST_CHECK(safety_link_rollback_build_identity_changed(true, 4, commit_a, 3, dt_a, true, 4, commit_b, 3,
                                                             dt_b) == true,
               "commit bytes differ, datetime identical -- still a change");
    TEST_CHECK(safety_link_rollback_build_identity_changed(true, 4, commit_a, 3, dt_a, true, 4, commit_a, 3,
                                                             dt_a) == false,
               "identical commit AND datetime, same lengths -- not a change (an unrelated reboot "
               "into the SAME image)");
    TEST_CHECK(safety_link_rollback_build_identity_changed(true, 4, commit_a, 3, dt_a, true, 5, commit_a, 3,
                                                             dt_a) == true,
               "commit LENGTH alone differing (even with identical overlapping bytes) is a change");
}

static void test_reboot_confirmed_requires_both_boot_id_and_build_identity(void)
{
    TEST_SECTION("safety_link_rollback_reboot_confirmed -- ACCEPTED evidence requires BOTH "
                 "the boot_id AND the build identity to have changed; either alone is not enough");

    TEST_CHECK(safety_link_rollback_reboot_confirmed(/*boot_id_changed=*/true, /*build_identity_changed=*/true) ==
                   true,
               "both changed -- a rollback into a different image, genuinely observed");
    TEST_CHECK(safety_link_rollback_reboot_confirmed(/*boot_id_changed=*/true,
                                                       /*build_identity_changed=*/false) == false,
               "boot_id changed but the build did not -- looks like an unrelated reboot, NOT confirmed");
    TEST_CHECK(safety_link_rollback_reboot_confirmed(/*boot_id_changed=*/false,
                                                       /*build_identity_changed=*/true) == false,
               "build identity 'changed' with no boot_id change cannot happen on a real link (the "
               "build identity is only ever refreshed BY a FW_VERSION frame that also refreshes the "
               "boot_id), but the function itself must still require both defensively");
    TEST_CHECK(safety_link_rollback_reboot_confirmed(false, false) == false, "neither changed -- not confirmed");
}

void run_test_safety_link(void)
{
    test_is_stale_boundary_at_threshold();
    test_is_stale_never_received_is_always_stale();
    test_is_stale_two_independent_thresholds();
    test_stale_ms_constant_is_1500();
    test_firing_abort_ms_constant_is_30000();
    test_drain_wait_not_waiting_for_anything_never_blocks();
    test_drain_wait_status_wanted_not_yet_captured_keeps_waiting();
    test_drain_wait_status_captured_stops_waiting();
    test_drain_wait_opportunistic_drains_are_unchanged();
    test_drain_wait_config_page_wanted_not_yet_captured_keeps_waiting();
    test_drain_wait_config_page_captured_stops_waiting();
    test_drain_wait_ct_cal_and_commit_rejected_same_shape();
    test_drain_wait_only_the_wanted_reply_gates_waiting();
    test_rollback_infer_no_evidence_is_unknown_not_accepted();
    test_rollback_infer_refusal_wins();
    test_rollback_infer_undecodable_refusal_is_unknown();
    test_rollback_infer_boot_id_change_after_silence_is_accepted();
    test_rollback_boot_id_unknown_before_is_not_evidence();
    test_rollback_boot_id_real_change_is_evidence();
    test_build_identity_unknown_before_or_after_is_not_evidence();
    test_build_identity_real_change_is_evidence();
    test_reboot_confirmed_requires_both_boot_id_and_build_identity();
}
