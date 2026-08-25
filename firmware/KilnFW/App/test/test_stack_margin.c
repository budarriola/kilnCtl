// Host tests for App/drivers/stack_margin_calc.h -- the pure arithmetic
// behind TODO.md section 13's stack high-water-mark report. Everything in
// stack_margin_calc.h is a header-only inline function, no ESP-IDF, no I/O
// -- included directly here, same as test_dram_margin.c includes
// dram_margin.h.
//
// stack_margin.c's registry (uxTaskGetStackHighWaterMark(), the wire-reply
// builder in uart_bridge.c) is NOT covered here: it is FreeRTOS/hardware-
// dependent and there is no in-repo fake worth the trouble for what is, by
// design, a thin pass-through over two pure functions -- both of which ARE
// exercised below.
#include "test_common.h"

#include "../drivers/stack_margin_calc.h"

static void test_esp_idf_returns_bytes_so_the_factor_is_one(void)
{
    // ESP-IDF's uxTaskGetStackHighWaterMark() returns BYTES, a documented
    // deviation from vanilla FreeRTOS stated in its own task.h:1509 ("in bytes
    // (as opposed to words in the standard FreeRTOS documentation)"). So the
    // conversion is the identity.
    //
    // Pinned against ABSOLUTE values, never against STACK_MARGIN_WORD_BYTES
    // itself -- an assertion like
    //   TEST_CHECK(stack_margin_words_to_bytes(1) == STACK_MARGIN_WORD_BYTES)
    // moves with the constant and can never fail.
    //
    // WORTH KNOWING: the previous revision of this test was already written
    // this way, with absolute literals, and still encoded the WRONG answer (4).
    // A non-vacuous test is not automatically a correct one -- it faithfully
    // pinned a premise nobody had checked against the platform's own header.
    // What actually caught the error was the impossible reading it produced on
    // the bench, which is why stack_margin_classify() now checks for that
    // directly; see test_classify_rejects_an_impossible_reading() below.
    TEST_CHECK(stack_margin_words_to_bytes(1) == 1u,
               "ESP-IDF already returns bytes: 1 must convert to 1 (absolute check)");
    TEST_CHECK(stack_margin_words_to_bytes(0) == 0u, "0 stays 0");
    TEST_CHECK(stack_margin_words_to_bytes(1932) == 1932u,
               "a real bench reading (uart_proto_rx, 1932B free of 4096B) must pass through unscaled");
}

static void test_a_four_times_conversion_would_be_detected(void)
{
    // Negative-test evidence: the 4x factor this module actually shipped must
    // now read differently from the correct conversion, AND must produce a
    // reading that stack_margin_classify() rejects as impossible. The second
    // half is the part that would have caught the original bug on the bench
    // instead of after it.
    uint32_t real = stack_margin_words_to_bytes(1932);
    uint32_t times_four = 1932u * 4u;
    TEST_CHECK(real != times_four, "a 4x conversion must read differently from the real one");
    TEST_CHECK(stack_margin_classify(times_four, 4096) == STACK_MARGIN_LEVEL_CRITICAL,
               "the 4x-inflated reading (7728B of 4096B) must classify CRITICAL, never OK");
}

static void test_classify_rejects_an_impossible_reading(void)
{
    // The exact figures the broken build reported on the bench. More free
    // stack than the stack has cannot happen, so this must never come back OK
    // -- that is what let 311% and 328% headroom read as healthy.
    TEST_CHECK(stack_margin_classify(12752, 4096) == STACK_MARGIN_LEVEL_CRITICAL,
               "hwm 12752B against a 4096B stack is impossible: CRITICAL, not OK");
    TEST_CHECK(stack_margin_classify(13440, 4096) == STACK_MARGIN_LEVEL_CRITICAL,
               "hwm 13440B against a 4096B stack is impossible: CRITICAL, not OK");
    // One byte over is still impossible; exactly equal is not (an untouched
    // stack legitimately reads back its full depth).
    TEST_CHECK(stack_margin_classify(3073, 3072) == STACK_MARGIN_LEVEL_CRITICAL,
               "one byte above the configured depth is impossible: CRITICAL");
    TEST_CHECK(stack_margin_classify(3072, 3072) == STACK_MARGIN_LEVEL_OK,
               "exactly the configured depth is an untouched stack: OK");
}

static void test_classify_ok_at_full_headroom(void)
{
    // Untouched stack: high-water mark reads back at (or near) the full
    // configured depth. Must classify OK.
    stack_margin_level_t level = stack_margin_classify(3072, 3072);
    TEST_CHECK(level == STACK_MARGIN_LEVEL_OK, "full headroom (100%) must be OK");
}

static void test_classify_critical_near_zero_headroom(void)
{
    // Absolute case, not derived from STACK_MARGIN_CRITICAL_PCT: 100 bytes
    // of headroom left on a 3072-byte stack (~3%) is unambiguously
    // CRITICAL under any sane threshold, so this must hold even if the
    // named percentage constants are later retuned.
    stack_margin_level_t level = stack_margin_classify(100, 3072);
    TEST_CHECK(level == STACK_MARGIN_LEVEL_CRITICAL, "~3% headroom (100/3072 B) must be CRITICAL");
}

static void test_classify_low_in_the_middle_band(void)
{
    // 20% headroom sits between the two named cut points (15/30) as of this
    // writing -- documents the current boundary configuration without
    // hardcoding the constant names themselves into the assertion (the
    // assertion is against the literal 20%, i.e. 614/3072).
    stack_margin_level_t level = stack_margin_classify(614, 3072); // ~20.0%
    TEST_CHECK(level == STACK_MARGIN_LEVEL_LOW, "~20% headroom must land in the LOW band");
}

static void test_classify_zero_configured_stack_is_critical_not_a_crash(void)
{
    // A registration bug (configured_stack_bytes never set) must not divide
    // by zero, and must not silently read as "fine" -- see this function's
    // own doc comment: a bug in a stack-safety report must never present as
    // "everything's fine."
    stack_margin_level_t level = stack_margin_classify(1000, 0);
    TEST_CHECK(level == STACK_MARGIN_LEVEL_CRITICAL, "configured_stack_bytes==0 must report CRITICAL, not divide by zero");
}

static void test_classify_boundary_is_not_double_counted(void)
{
    // Exactly at the CRITICAL/LOW boundary (15%) must land on the LOW side
    // (the classify function uses strict "<", documented in the header) --
    // pins the boundary direction the same way test_dram_margin.c pins its
    // own "<" vs "<=".
    uint32_t configured = 1000;
    uint32_t hwm_at_15pct = 150; // exactly 15% of 1000
    stack_margin_level_t level = stack_margin_classify(hwm_at_15pct, configured);
    TEST_CHECK(level == STACK_MARGIN_LEVEL_LOW, "exactly at the 15%% boundary: LOW, not CRITICAL");
}

void run_test_stack_margin(void)
{
    TEST_SECTION("stack_margin_calc: word->byte conversion");
    test_esp_idf_returns_bytes_so_the_factor_is_one();
    test_a_four_times_conversion_would_be_detected();
    test_classify_rejects_an_impossible_reading();

    TEST_SECTION("stack_margin_calc: headroom classification");
    test_classify_ok_at_full_headroom();
    test_classify_critical_near_zero_headroom();
    test_classify_low_in_the_middle_band();
    test_classify_zero_configured_stack_is_critical_not_a_crash();
    test_classify_boundary_is_not_double_counted();
}
