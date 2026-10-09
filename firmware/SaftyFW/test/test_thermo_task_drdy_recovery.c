// Host tests for firmware/SaftyFW/src/tasks/thermo_task_drdy_recovery.c --
// the missed-~DRDY-edge recovery decision (docs/audits/
// safety_tc_drdy_stall_2026-09-08.md). No pico-sdk/FreeRTOS/hardware
// dependency: gpio_get()'s live value is passed in as a plain uint32_t, same
// discipline as the other max31856_*_policy host tests.
#include "test_common.h"

#include "../src/tasks/thermo_task_drdy_recovery.h"

// The boot-time race (and any later missed edge): the notify-wait timed out
// (notifications == 0), the bench-only assume-ready path is off, and ~DRDY
// itself reads asserted (active-low, 0) right now -- a completed conversion
// whose falling edge nobody caught. Must recover.
static void test_recovers_when_drdy_already_asserted(void)
{
    TEST_SECTION("thermo_task_drdy_recovery: recovers a missed edge (DRDY already low)");

    bool recover = thermo_task_drdy_missed_edge(/*notifications=*/0, /*assume_ready=*/false,
                                                 /*drdy_pin_level=*/0u);
    TEST_CHECK(recover, "DRDY asserted at timeout -> treat as a missed edge, read now");
}

// A genuinely silent/dead part: the notify-wait timed out AND the pin reads
// HIGH (the external pull-up, R2 -- no conversion has ever completed). Must
// NOT recover -- this is exactly the case that has to keep falling through
// to thermo_task_fn()'s pre-existing DRDY-silence branch so S5 sees a real
// invalid reading, not a fabricated one.
static void test_does_not_recover_when_genuinely_silent(void)
{
    TEST_SECTION("thermo_task_drdy_recovery: does NOT fabricate a reading for a silent part");

    bool recover = thermo_task_drdy_missed_edge(/*notifications=*/0, /*assume_ready=*/false,
                                                 /*drdy_pin_level=*/1u);
    TEST_CHECK(!recover, "DRDY still HIGH at timeout -> genuinely silent, no recovery");
}

// A real DRDY-triggered wake (notifications != 0) never needs this decision
// at all -- thermo_task_fn()'s own notified branch already handles it. Must
// stay false regardless of the pin level, so this function is provably
// inert on the normal, working path.
static void test_inert_when_actually_notified(void)
{
    TEST_SECTION("thermo_task_drdy_recovery: inert on a real notification");

    TEST_CHECK(!thermo_task_drdy_missed_edge(/*notifications=*/1, false, 0u),
               "a real notification never asks for edge-recovery, even with DRDY low");
    TEST_CHECK(!thermo_task_drdy_missed_edge(/*notifications=*/1, false, 1u),
               "a real notification never asks for edge-recovery, even with DRDY high");
}

// The bench-only SAFTYFW_THERMO_ASSUME_DRDY path, when it already claimed
// this iteration (assume_ready == true), must not be double-counted by this
// decision -- it stays false even if DRDY also happens to read low.
static void test_inert_when_assume_ready_already_claimed(void)
{
    TEST_SECTION("thermo_task_drdy_recovery: inert once assume_ready already fired");

    TEST_CHECK(!thermo_task_drdy_missed_edge(/*notifications=*/0, /*assume_ready=*/true, 0u),
               "assume_ready already true -> this decision does not also fire");
}

// Negative-test procedure for this file's coverage (per project standing
// practice: every check must be shown capable of failing, not just
// confirmed to pass) was done by hand against the PRODUCTION function in
// thermo_task_drdy_recovery.c, not a test-local mirror: temporarily changed
// `return drdy_pin_level == 0;` to `return false;` (reproducing exactly the
// pre-fix, stuck-latched behaviour from the audit -- an edge-triggered IRQ
// armed on an already-low pin that never recovers), reran the suite, and
// test_recovers_when_drdy_already_asserted() failed with:
//   FAIL: DRDY asserted at timeout -> treat as a missed edge, read now
// Then reversed the edit by hand (restored `drdy_pin_level == 0`) and
// confirmed `git diff` for thermo_task_drdy_recovery.c is empty.
void run_test_thermo_task_drdy_recovery(void)
{
    test_recovers_when_drdy_already_asserted();
    test_does_not_recover_when_genuinely_silent();
    test_inert_when_actually_notified();
    test_inert_when_assume_ready_already_claimed();
}
