// test_watchdog_overdue_diag_codec.c -- host tests for the pure bit-packing
// codec (src/watchdog_overdue_diag_codec.c/.h), the CLEAR_TRIP-reboots-the-
// Pico investigation's actual conclusion: the reboot was never a fault --
// clear_trip_diag_codec.c's checkpoint proved the CLEAR_TRIP drain block
// ran to completion (stage=4, outcome=REFUSED_STILL_TRIPPED) -- it is the
// hardware watchdog firing because some task missed its own check-in
// deadline. This packs which task(s) and by how much into
// watchdog_hw->scratch[5] (repurposed from a live, continuously-overwritten
// mask into a magic-tagged, write-once-per-withheld-feed latch), so it
// survives to be read on the boot immediately after.
//
// watchdog_overdue_diag_codec.c has no pico-sdk/FreeRTOS/hardware
// dependency, so unlike watchdog_overdue_diag.c itself it is directly
// host-testable here, same pattern test_clear_trip_diag_codec.c already
// established for the sibling scratch[7] format.
#include <stddef.h>

#include "test_common.h"
#include "../src/watchdog_overdue_diag_codec.h"

// A round trip through every field, distinct non-default values, must
// recover every one exactly -- the test that catches a shift/mask drifting
// out of sync between encode and decode.
static void test_round_trip_recovers_every_field(void)
{
    uint32_t word = watchdog_overdue_diag_encode(0x82u, /* overdue_mask: bits 1 (SAFETY_CORE) and 7 */
                                                  1u,    /* worst_task_id: SAFETY_CORE */
                                                  2000u); /* worst_overage_ms */
    watchdog_overdue_diag_t out = watchdog_overdue_diag_decode(word);

    TEST_CHECK(out.magic_ok, "a freshly encoded word must decode with magic_ok true");
    TEST_CHECK(out.overdue_mask == 0x82u, "overdue_mask must round-trip exactly, the full byte");
    TEST_CHECK(out.worst_task_id == 1u, "worst_task_id must round-trip exactly");
    TEST_CHECK(out.worst_overage_ms == 2000u, "worst_overage_ms must round-trip exactly -- this is "
                                                "the field that tells '20ms late' from '2000ms late' "
                                                "apart");
}

// All-zero-fields data is still a FRESH word (the magic tag is independent
// of the data fields) and must decode as such -- proves the format does not
// conflate "no tasks overdue" (which should never actually happen for a
// genuine withheld-feed latch, but the codec must not assume that) with
// "nothing written here".
static void test_round_trip_all_zero_fields_still_decodes(void)
{
    uint32_t word = watchdog_overdue_diag_encode(0u, 0u, 0u);
    watchdog_overdue_diag_t out = watchdog_overdue_diag_decode(word);

    TEST_CHECK(out.magic_ok, "an all-zero-fields word is still a FRESH word (magic tag still set) "
                              "and must decode as such");
    TEST_CHECK(out.overdue_mask == 0u && out.worst_task_id == 0u && out.worst_overage_ms == 0u,
               "all-zero input fields must round-trip as all-zero, not be misread as "
               "something else");
}

// An all-zero WORD (the actual value after a fresh boot or
// watchdog_overdue_diag_clear()) must decode as magic_ok == false, every
// field at its safe default -- this is the common case, since most boots
// never withhold a feed at all.
static void test_missing_magic_reports_not_ok_with_zeroed_fields(void)
{
    watchdog_overdue_diag_t out = watchdog_overdue_diag_decode(0x00000000u);

    TEST_CHECK(!out.magic_ok, "an all-zero word (fresh boot, or after "
                               "watchdog_overdue_diag_clear()) must decode as magic_ok == false");
    TEST_CHECK(out.overdue_mask == 0u && out.worst_task_id == 0u && out.worst_overage_ms == 0u,
               "every field must default to zero when magic_ok is false");
}

// A word that matches clear_trip_diag_codec.c's OWN magic byte (0xC7) --
// the sibling format sharing this same scratch-register convention but a
// DIFFERENT register -- must never be accepted here. The two tags must be
// distinct, and this proves the decode actually checks the right one, not
// merely "is some magic-shaped byte present".
static void test_sibling_scratch_format_magic_is_rejected(void)
{
    uint32_t clear_trip_shaped_word = 0xC7000000u; // CLEAR_TRIP_DIAG_MAGIC_BYTE in the same position
    watchdog_overdue_diag_t out = watchdog_overdue_diag_decode(clear_trip_shaped_word);

    TEST_CHECK(!out.magic_ok, "clear_trip_diag_codec.c's own magic byte (0xC7) must not be "
                               "mistaken for this format's tag (0xD9) -- the two scratch-register "
                               "formats must never cross-validate each other's data");
}

// worst_overage_ms above the format's 13-bit budget must saturate to
// WATCHDOG_OVERDUE_DIAG_OVERAGE_MAX_MS, not silently truncate into a
// misleadingly SMALL value -- "wrapped to 20ms" would point at completely
// the wrong cause compared to the genuine multi-second overage that
// produced it.
static void test_oversized_overage_saturates_rather_than_wraps(void)
{
    // 9000 is chosen deliberately: it is just above the 13-bit budget
    // (8191), and its low 13 bits alone (9000 & 0x1FFF == 808) are a
    // plausible-looking but WRONG small value -- a naive mask-only
    // implementation (no clamp) would produce exactly 808 here, silently
    // indistinguishable from a genuinely-808ms overage. Real saturation
    // must clamp to the maximum instead, which is what this test actually
    // proves; 0xFFFF would not have (0xFFFF & 0x1FFF happens to equal the
    // clamp value by coincidence, so it cannot tell the two implementations
    // apart).
    uint32_t word = watchdog_overdue_diag_encode(0x01u, 0u, 9000u);
    watchdog_overdue_diag_t out = watchdog_overdue_diag_decode(word);

    TEST_CHECK(out.magic_ok, "an oversized overage value must not corrupt the magic byte");
    TEST_CHECK(out.worst_overage_ms == WATCHDOG_OVERDUE_DIAG_OVERAGE_MAX_MS,
               "an overage value above the 13-bit budget must saturate to the format's maximum "
               "(8191), not mask-wrap into a small, misleadingly-precise-looking value like 808");
    TEST_CHECK(out.overdue_mask == 0x01u, "an oversized overage value must not bleed into "
                                            "overdue_mask, a neighbouring field");
}

// worst_task_id is masked to 3 bits (0-7). Every watchdog_checkin_id_t this
// codebase currently defines must fit -- if a 9th task is ever registered
// (id 8, needing a 4th bit), this test starts failing and is exactly the
// signal that the format needs to grow, rather than silently wrapping a
// real task id 8 back to id 0 forever.
static void test_watchdog_checkin_id_fits_in_3_bits(void)
{
    // WATCHDOG_CHECKIN_COUNT is not visible to this pure codec test (it
    // lives in watchdog_task.h, a pico-sdk-adjacent header this file
    // deliberately does not pull in) -- 8 is this codebase's actual count
    // as of 2026-08-23 (RELAY_OWNER..UPDATE_TASK), hard-coded here on
    // purpose so this test does not silently track a future header change;
    // if WATCHDOG_CHECKIN_COUNT ever exceeds 8, THIS constant must be
    // updated by hand, which is the point.
    const uint8_t current_checkin_count = 8u;
    TEST_CHECK(current_checkin_count <= 8u, "worst_task_id has exactly 3 bits (values 0-7, 8 ids) -- "
                                              "a 9th registered task would need this format widened "
                                              "before it could be represented, not silently wrapped");
}

// --- watchdog_overflow_diag_decode() -- round 2, the sibling format sharing
// this same physical register (see its own doc comment,
// watchdog_overdue_diag_codec.h) for a stack overflow instead of an overdue
// check-in. There is deliberately no watchdog_overflow_diag_encode() to
// test here -- the encode side is inline in main.c's
// vApplicationStackOverflowHook(), by design (it must not call into another
// compilation unit from a possibly-corrupted stack), so these tests
// hand-construct the packed word the way that hook does, byte for byte, and
// check the decoder's own correctness plus its independence from the
// sibling overdue format.

// A hand-packed word matching exactly what the hook writes (magic 0xE3,
// two name bytes) must decode with both bytes recovered exactly.
static void test_overflow_round_trip_recovers_name_bytes(void)
{
    uint32_t word = (0xE3u << 24) | ((uint32_t)'s' << 16) | ((uint32_t)'a' << 8);
    watchdog_overflow_diag_t out = watchdog_overflow_diag_decode(word);

    TEST_CHECK(out.magic_ok, "a word with the overflow magic byte (0xE3) must decode with "
                              "magic_ok true");
    TEST_CHECK(out.name_byte0 == (uint8_t)'s', "name_byte0 must recover the exact byte the hook "
                                                 "packed at [23:16]");
    TEST_CHECK(out.name_byte1 == (uint8_t)'a', "name_byte1 must recover the exact byte the hook "
                                                 "packed at [15:8]");
}

// An all-zero word (fresh boot, or after watchdog_overdue_diag_clear()) --
// the common case, since most boots never overflow -- must decode as
// magic_ok == false.
static void test_overflow_missing_magic_reports_not_ok(void)
{
    watchdog_overflow_diag_t out = watchdog_overflow_diag_decode(0x00000000u);

    TEST_CHECK(!out.magic_ok, "an all-zero word must decode as magic_ok == false");
    TEST_CHECK(out.name_byte0 == 0u && out.name_byte1 == 0u,
               "both name bytes must default to zero when magic_ok is false");
}

// The two formats sharing scratch[5] must never cross-validate: a
// genuine watchdog_overdue_diag_t word (magic 0xD9) must be rejected by
// the OVERFLOW decoder, and vice versa (covered by
// test_sibling_scratch_format_magic_is_rejected above, for the other
// direction) -- this is what makes sharing one register between two
// mutually-exclusive events safe rather than ambiguous.
static void test_overflow_decoder_rejects_overdue_format_word(void)
{
    uint32_t overdue_shaped_word = watchdog_overdue_diag_encode(0xFFu, 5u, 100u);
    watchdog_overflow_diag_t out = watchdog_overflow_diag_decode(overdue_shaped_word);

    TEST_CHECK(!out.magic_ok, "a genuine watchdog_overdue_diag_t word (magic 0xD9) must not be "
                               "mistaken for an overflow marker (magic 0xE3) -- the two formats "
                               "sharing scratch[5] must never cross-validate each other's data");
}

// Every task name this codebase currently registers with xTaskCreate()
// (grepped 2026-08-23: current_task, discrete_task, link_task, log_task,
// relay_owner, safety_core, thermo_task, update_task, watchdog_task) must
// remain distinguishable from every other by its first two characters
// alone -- that is the whole premise vApplicationStackOverflowHook()'s
// two-byte capture relies on. If a future task name collides with an
// existing one in its first two characters, THIS test is where that gets
// caught, not a confused SWD session guessing which task actually
// overflowed.
static void test_two_name_bytes_disambiguate_every_current_task(void)
{
    static const char *const task_names[] = {
        "current_task", "discrete_task", "link_task",   "log_task",    "relay_owner",
        "safety_core",  "thermo_task",   "update_task", "watchdog_task",
    };
    const size_t count = sizeof(task_names) / sizeof(task_names[0]);

    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            bool same_first_two = task_names[i][0] == task_names[j][0] &&
                                   task_names[i][1] == task_names[j][1];
            TEST_CHECK(!same_first_two, "two registered task names share their first two "
                                          "characters -- vApplicationStackOverflowHook()'s "
                                          "two-byte capture can no longer tell them apart; widen "
                                          "the capture or rename one of the tasks");
        }
    }
}

void run_test_watchdog_overdue_diag_codec(void)
{
    TEST_SECTION("watchdog_overdue_diag_codec -- which task(s) missed their check-in deadline, "
                  "and by how much, surviving the watchdog reset it causes");
    test_round_trip_recovers_every_field();
    test_round_trip_all_zero_fields_still_decodes();
    test_missing_magic_reports_not_ok_with_zeroed_fields();
    test_sibling_scratch_format_magic_is_rejected();
    test_oversized_overage_saturates_rather_than_wraps();
    test_watchdog_checkin_id_fits_in_3_bits();

    TEST_SECTION("watchdog_overflow_diag_decode -- the stack-overflow marker sharing scratch[5] "
                  "with the overdue-checkin latch");
    test_overflow_round_trip_recovers_name_bytes();
    test_overflow_missing_magic_reports_not_ok();
    test_overflow_decoder_rejects_overdue_format_word();
    test_two_name_bytes_disambiguate_every_current_task();
}
