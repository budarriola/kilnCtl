// test_clear_trip_diag_codec.c -- host tests for the pure bit-packing codec
// (src/clear_trip_diag_codec.c/.h), the CLEAR_TRIP-reboots-the-Pico
// investigation's round-4 checkpoint: a stack-overflow-adjacent crash
// reboots the chip, zeroing .bss before any RAM-based diagnostic can be
// read, so this packs a checkpoint into watchdog_hw->scratch[7], the one
// register still free after boot_reason.c/startup_diag.h claimed the other
// seven -- see clear_trip_diag.h's own header comment for the full
// reasoning.
//
// clear_trip_diag_codec.c has no pico-sdk/FreeRTOS/hardware dependency, so
// unlike clear_trip_diag.c itself it is directly host-testable here, same
// pattern test_watchdog_gate.c/test_uart_owner_tx_policy.c already
// established.
#include "test_common.h"
#include "../src/clear_trip_diag_codec.h"

// A round trip through every field, all set to distinct, non-default,
// non-adjacent values, must recover every one of them exactly. This is the
// test that matters most: if the shifts/masks in encode and decode ever
// drift out of sync with each other, this is what catches it.
static void test_round_trip_recovers_every_field(void)
{
    uint32_t word = clear_trip_diag_encode(CLEAR_TRIP_DIAG_STAGE_PRE_OUTCOME_LOG, /* stage */
                                            5u,                                    /* reason */
                                            0x41u,                                 /* fault_bits: OPEN | TCRANGE */
                                            true,                                  /* tc_valid */
                                            true,                                  /* spi_failed */
                                            true,                                  /* tc_c_is_nan */
                                            2u);                                   /* outcome */
    clear_trip_diag_t out = clear_trip_diag_decode(word);

    TEST_CHECK(out.magic_ok, "a freshly encoded word must decode with magic_ok true");
    TEST_CHECK(out.stage == CLEAR_TRIP_DIAG_STAGE_PRE_OUTCOME_LOG, "stage must round-trip exactly");
    TEST_CHECK(out.reason == 5u, "reason must round-trip exactly");
    TEST_CHECK(out.fault_bits == 0x41u, "fault_bits must round-trip exactly, the full byte");
    TEST_CHECK(out.tc_valid, "tc_valid must round-trip exactly");
    TEST_CHECK(out.spi_failed, "spi_failed must round-trip exactly");
    TEST_CHECK(out.tc_c_is_nan, "tc_c_is_nan must round-trip exactly");
    TEST_CHECK(out.outcome == 2u, "outcome must round-trip exactly");
}

// Every bool field independently false, and the smallest legal integer
// fields (0), must also round-trip cleanly -- proves the bit-packing does
// not accidentally treat "all zero" data as "absent" (that is what magic_ok
// is for, tested separately below).
static void test_round_trip_all_zero_fields_still_decodes(void)
{
    uint32_t word = clear_trip_diag_encode(CLEAR_TRIP_DIAG_STAGE_NONE, 0u, 0u, false, false, false, 0u);
    clear_trip_diag_t out = clear_trip_diag_decode(word);

    TEST_CHECK(out.magic_ok, "an all-zero-fields word is still a FRESH word (magic tag still set) "
                              "and must decode as such");
    TEST_CHECK(out.stage == CLEAR_TRIP_DIAG_STAGE_NONE, "stage 0 must round-trip");
    TEST_CHECK(!out.tc_valid && !out.spi_failed && !out.tc_c_is_nan,
               "all three bool fields false must round-trip false, not be misread as true");
}

// A word with no magic tag at all (e.g. genuine power-on-uninitialised SRAM,
// which this scratch register is NOT battery-backed against, or a value
// left by a completely unrelated reset) must decode as magic_ok == false,
// with every other field at its safe zero default -- never a fabricated
// "looks plausible" value from garbage bits.
static void test_missing_magic_reports_not_ok_with_zeroed_fields(void)
{
    clear_trip_diag_t out = clear_trip_diag_decode(0x00000000u); // the actual value after a fresh boot / clear_trip_diag_clear()

    TEST_CHECK(!out.magic_ok, "an all-zero word (fresh boot, or after clear_trip_diag_clear()) "
                               "must decode as magic_ok == false");
    TEST_CHECK(out.stage == CLEAR_TRIP_DIAG_STAGE_NONE, "stage must default to NONE when magic_ok is false");
    TEST_CHECK(!out.tc_valid && !out.spi_failed && !out.tc_c_is_nan,
               "every bool field must default to false when magic_ok is false");
}

// A word that LOOKS almost right -- every bit correct except the magic byte
// -- must still be rejected. This is the exact "stale value from an
// unrelated reset" case the tag exists to catch: a boot-stage or check-in-
// mask word (this codebase's OTHER scratch-register users) landing in this
// register by some future mistake must never be misread as fresh
// CLEAR_TRIP checkpoint data just because its low bytes happen to look
// plausible.
static void test_wrong_magic_byte_rejected_even_with_plausible_low_bits(void)
{
    uint32_t good = clear_trip_diag_encode(CLEAR_TRIP_DIAG_STAGE_POST_LOG_TASK, 5u, 0x01u, true, false,
                                            true, 1u);
    uint32_t corrupted_magic = (good & 0x00FFFFFFu) | 0xAB000000u; // swap only the top byte

    clear_trip_diag_t out = clear_trip_diag_decode(corrupted_magic);

    TEST_CHECK(!out.magic_ok, "a wrong magic byte must be rejected even when every other bit is "
                               "identical to a genuine, freshly encoded word");
}

// Fields wider than their allotted bits (stage/reason/outcome are masked to
// 4/4/3 bits respectively) must be truncated by the mask, not corrupt an
// adjacent field -- proves the shifts do not overlap.
static void test_oversized_field_values_are_masked_not_bleeding_into_neighbours(void)
{
    // stage=0xFF (should truncate to its low 4 bits, 0xF -- an out-of-range
    // stage value this codebase never actually produces, but the codec must
    // not corrupt reason/fault_bits/etc just because a future caller passes
    // a bad one)
    uint32_t word = clear_trip_diag_encode(0xFFu, 3u, 0x02u, false, true, false, 1u);
    clear_trip_diag_t out = clear_trip_diag_decode(word);

    TEST_CHECK(out.magic_ok, "an oversized stage value must not corrupt the magic byte");
    TEST_CHECK(out.stage == 0xFu, "stage must be masked to its 4 allotted bits, not bleed into reason");
    TEST_CHECK(out.reason == 3u, "reason must be unaffected by an oversized stage value in a "
                                  "neighbouring field");
    TEST_CHECK(out.fault_bits == 0x02u, "fault_bits must be unaffected");
    TEST_CHECK(out.spi_failed, "spi_failed must be unaffected");
    TEST_CHECK(out.outcome == 1u, "outcome must be unaffected");
}

void run_test_clear_trip_diag_codec(void)
{
    TEST_SECTION("clear_trip_diag_codec -- the CLEAR_TRIP crash checkpoint that survives a "
                  "watchdog reset");
    test_round_trip_recovers_every_field();
    test_round_trip_all_zero_fields_still_decodes();
    test_missing_magic_reports_not_ok_with_zeroed_fields();
    test_wrong_magic_byte_rejected_even_with_plausible_low_bits();
    test_oversized_field_values_are_masked_not_bleeding_into_neighbours();
}
