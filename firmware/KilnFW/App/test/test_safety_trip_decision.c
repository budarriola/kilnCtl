// Host test for App/drivers/safety/safety_trip_decision.c -- the pure decision
// factored out of safety_link.c's safety_apply_trip_event() (2026-08-28 opus
// review: that logic had NO automated test, because no host harness links
// safety_link.c -- see safety_trip_decision.h's doc comment for the full
// "why extract instead of stub-link" reasoning).
//
// Own, THIRTEENTH separate executable (build_host_tests.ps1) -- needs no
// stub headers at all, unlike every test file above it: safety_trip_
// decision.c's only includes are stdbool.h/stdint.h.
#include <stdio.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#include "../drivers/safety/safety_trip_decision.h"

// ---------------------------------------------------------------------------
// The three cases the task brief names as the minimum bar, each stated in
// the same terms safety_apply_trip_event()'s own comment uses.
// ---------------------------------------------------------------------------

static void test_genuinely_live_new_trip_captures_mask_and_is_valid(void)
{
    TEST_SECTION("a genuinely-live new trip (a trip was already being tracked this boot, and this "
                 "trip_seq differs from it) -- captures the mask (is_new_event) and marks it valid");
    // This boot already applied an earlier trip (trip_seq 5) and is now
    // seeing trip_seq 6 -- the live, "watched it happen" case.
    safety_trip_decision_input_t in = {
        .trip_event_ever_received_before = true,
        .cached_trip_last_seq = 5,
        .incoming_trip_seq = 6,
    };
    safety_trip_decision_t out = safety_trip_decide_event(in);
    TEST_CHECK(out.is_new_event, "a differing trip_seq, with a prior event already tracked, is new");
    TEST_CHECK(out.fault_sources_valid,
              "and -- because a prior event was already being tracked -- this boot actually watched "
              "the trip_seq change happen, so the fault-source snapshot IS trustworthy");
}

static void test_dedup_resend_does_not_overwrite(void)
{
    TEST_SECTION("a dedup resend of the SAME trip_seq (LINK_PROTOCOL.md sec 6's repeat-against-loss "
                 "convention) must NOT be treated as a new event, so the cached trip_fault_sources "
                 "snapshot from the original event is never overwritten by whatever fault_sources "
                 "happens to read at resend time");
    safety_trip_decision_input_t in = {
        .trip_event_ever_received_before = true,
        .cached_trip_last_seq = 6,
        .incoming_trip_seq = 6, // same trip_seq -- a resend, not a new trip
    };
    safety_trip_decision_t out = safety_trip_decide_event(in);
    TEST_CHECK(!out.is_new_event, "a resend of the already-cached trip_seq is NOT new");
    // safety_apply_trip_event() only reads fault_sources_valid inside its
    // own `if (is_new_event)` branch (mirrored in the header's doc comment),
    // so a resend's fault_sources_valid value is never actually consulted --
    // but pin it here anyway so a future refactor that starts reading it
    // unconditionally gets an honest "no" instead of a stale "yes" left over
    // from the live case above.
    TEST_CHECK(!out.fault_sources_valid, "and is not (falsely) reported valid either");
}

static void test_first_frame_after_boot_with_already_latched_trip_is_not_valid(void)
{
    TEST_SECTION("N3 (2026-08-28 audit fix): the FIRST trip event this boot ever applies, when no prior "
                 "event was tracked this boot at all -- an ESP reboot with a trip still latched on the "
                 "Pico -- must be marked is_new_event (so it IS applied/shown) but NOT "
                 "fault_sources_valid (this boot's fault_sources at that instant reflects THIS boot's "
                 "fault lines, not whatever was asserted when the trip actually latched, possibly "
                 "minutes or boots earlier)");
    safety_trip_decision_input_t in = {
        .trip_event_ever_received_before = false, // cold boot -- nothing tracked yet this boot
        .cached_trip_last_seq = 0,                // meaningless when the flag above is false
        .incoming_trip_seq = 3,                   // whatever trip_seq the Pico happens to still be on
    };
    safety_trip_decision_t out = safety_trip_decide_event(in);
    TEST_CHECK(out.is_new_event, "the first-ever frame this boot is still applied (reason/uptime/etc "
                                 "must be recorded)");
    TEST_CHECK(!out.fault_sources_valid,
              "this exact case (the whole point of N3): the mask must be marked NOT captured, not a "
              "plausible-looking wrong value from this boot's own fault_sources");
}

// ---------------------------------------------------------------------------
// Extra coverage: the remaining corner the three cases above don't exercise.
// ---------------------------------------------------------------------------

static void test_dedup_resend_of_trip_seq_zero(void)
{
    // trip_seq is a byte; 0 is a perfectly ordinary value, not a sentinel --
    // this pins that a resend of trip_seq 0 is detected the same way as any
    // other value (an implementation that special-cased "0 means never set"
    // instead of using trip_event_ever_received_before would misfire here).
    TEST_SECTION("a resend of trip_seq 0 (not a sentinel) is still detected as a dedup, not a new event");
    safety_trip_decision_input_t in = {
        .trip_event_ever_received_before = true,
        .cached_trip_last_seq = 0,
        .incoming_trip_seq = 0,
    };
    safety_trip_decision_t out = safety_trip_decide_event(in);
    TEST_CHECK(!out.is_new_event, "trip_seq 0 == 0 is still a resend");
}

static void test_wraparound_seq_change_is_still_new(void)
{
    // trip_seq is a uint8_t that wraps (255 -> 0 on the Pico side); this
    // function does no arithmetic on it, only inequality, so wraparound
    // needs no special case -- pinned here so a future "clever" rewrite that
    // tries to detect wraparound doesn't quietly change this.
    TEST_SECTION("a trip_seq change across the uint8_t wraparound boundary (255 -> 0) is still new");
    safety_trip_decision_input_t in = {
        .trip_event_ever_received_before = true,
        .cached_trip_last_seq = 255,
        .incoming_trip_seq = 0,
    };
    safety_trip_decision_t out = safety_trip_decide_event(in);
    TEST_CHECK(out.is_new_event, "255 != 0 is a real change, wraparound or not");
    TEST_CHECK(out.fault_sources_valid, "and it is genuinely live, same as any other differing pair");
}

int main(void)
{
    TEST_SECTION("safety_trip_decide_event()");

    test_genuinely_live_new_trip_captures_mask_and_is_valid();
    test_dedup_resend_does_not_overwrite();
    test_first_frame_after_boot_with_already_latched_trip_is_not_valid();
    test_dedup_resend_of_trip_seq_zero();
    test_wraparound_seq_change_is_still_new();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures > 0 ? 1 : 0;
}
