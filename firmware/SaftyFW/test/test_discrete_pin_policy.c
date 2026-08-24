// Host tests for discrete_pin_policy.h -- the pure GPIO-level-to-logical-
// meaning mapping for E-stop (GPIO9) and mainFault (GPIO10), pulled out of
// discrete_task.c so the 2026-08-24 E-stop polarity bug (S7 silently
// disabled in both directions by an errant `!`) has a regression test that
// pins the electrical fact down in terms a reader can check directly
// against docs/HARDWARE.md section 5.
#include "test_common.h"
#include "../src/discrete_pin_policy.h"

// --- E-stop (GPIO9) -- ACTIVE HIGH for stop -------------------------------
// HARDWARE.md section 5: R10 1k pull-up to 3.3v_Safty, normally-CLOSED
// contact to GND_Safty. A closed (healthy) loop pulls the pin LOW. Anything
// that opens the loop -- a real button press, a cut wire, or nothing fitted
// to the connector at all -- lets the pull-up win and the pin reads HIGH.
// All three are electrically identical and MUST all read as asserted; that
// identity is the entire safety argument for this wiring, so each case gets
// its own named check rather than one "HIGH -> true" generic test.

static void test_estop_pressed_reads_high_asserted(void)
{
    TEST_SECTION("E-stop -- button genuinely PRESSED opens the loop -> GPIO9 HIGH -> asserted");
    TEST_CHECK(discrete_pin_policy_estop_asserted(true),
               "a pressed E-stop button opens the normally-closed loop -- pin reads HIGH, and "
               "this must decode as E-stop asserted (STOP)");
}

static void test_estop_cut_wire_reads_high_asserted(void)
{
    TEST_SECTION("E-stop -- CUT WIRE opens the loop identically -> GPIO9 HIGH -> asserted");
    TEST_CHECK(discrete_pin_policy_estop_asserted(true),
               "a severed E-stop wire is electrically indistinguishable from a pressed button -- "
               "pin reads HIGH, and this must ALSO decode as asserted (STOP): a cut cable failing "
               "healthy is exactly the hazard this normally-closed wiring exists to prevent");
}

static void test_estop_nothing_fitted_reads_high_asserted(void)
{
    TEST_SECTION("E-stop -- NOTHING FITTED to the connector -> GPIO9 HIGH -> asserted");
    TEST_CHECK(discrete_pin_policy_estop_asserted(true),
               "an unpopulated/unwired E-stop connector leaves the loop open the same way a cut "
               "wire does -- pin reads HIGH, and this must ALSO decode as asserted (STOP), not as "
               "a benign default");
}

static void test_estop_healthy_reads_low_not_asserted(void)
{
    TEST_SECTION("E-stop -- closed, healthy loop -> GPIO9 LOW -> NOT asserted");
    TEST_CHECK(!discrete_pin_policy_estop_asserted(false),
               "an intact, closed normally-closed loop pulls the pin LOW -- this is the ONLY "
               "reading that must decode as healthy");
}

// --- mainFault (GPIO10) -- ACTIVE LOW -------------------------------------
// U1's open-collector output pulls the line LOW when it asserts; R8's
// pull-up holds it HIGH otherwise. The OPPOSITE sense from E-stop above --
// this is the polarity the original bug's header comment got wrong by
// claiming both discretes were active-low, so both pins are asserted here,
// not just one, to catch a regression that swaps which pin gets inverted.

static void test_main_fault_asserted_reads_low(void)
{
    TEST_SECTION("mainFault -- U1 asserts (pulls line LOW) -> GPIO10 LOW -> asserted");
    TEST_CHECK(discrete_pin_policy_main_fault_asserted(false),
               "U1's open-collector output pulls GPIO10 LOW when mainFault is asserted");
}

static void test_main_fault_healthy_reads_high(void)
{
    TEST_SECTION("mainFault -- healthy (R8 pull-up wins) -> GPIO10 HIGH -> NOT asserted");
    TEST_CHECK(!discrete_pin_policy_main_fault_asserted(true),
               "R8's pull-up holds GPIO10 HIGH when U1 is not asserting -- must read healthy");
}

// --- cross-check: the two pins must NOT share a polarity ------------------
// The bug this suite exists to catch was exactly "both discretes treated as
// active-low" -- a test that only ever checks E-stop, or only ever checks
// mainFault, cannot distinguish "both pins correct" from "both pins wrongly
// unified to the same sense". This test feeds the SAME raw level to both
// functions and requires opposite answers.

static void test_estop_and_main_fault_have_opposite_polarity(void)
{
    TEST_SECTION("E-stop and mainFault decode the SAME raw HIGH/LOW level oppositely -- this is "
                  "exactly the distinction the original bug's comment (both discretes "
                  "\"active-low\") got wrong");

    // Raw level HIGH: E-stop says asserted (active high), mainFault says
    // NOT asserted (active low) -- opposite answers to the same input.
    TEST_CHECK(discrete_pin_policy_estop_asserted(true) != discrete_pin_policy_main_fault_asserted(true),
               "gpio==HIGH must decode oppositely for the two pins (estop: asserted, "
               "mainFault: healthy)");

    // Raw level LOW: E-stop says healthy, mainFault says asserted.
    TEST_CHECK(discrete_pin_policy_estop_asserted(false) != discrete_pin_policy_main_fault_asserted(false),
               "gpio==LOW must decode oppositely for the two pins (estop: healthy, "
               "mainFault: asserted)");
}

void run_test_discrete_pin_policy(void)
{
    test_estop_pressed_reads_high_asserted();
    test_estop_cut_wire_reads_high_asserted();
    test_estop_nothing_fitted_reads_high_asserted();
    test_estop_healthy_reads_low_not_asserted();

    test_main_fault_asserted_reads_low();
    test_main_fault_healthy_reads_high();

    test_estop_and_main_fault_have_opposite_polarity();
}
