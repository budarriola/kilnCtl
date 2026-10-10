// Host tests for tasks/thermo_inject_gate.h (guard review F1): INJECT_TC is
// refused whenever the relay is ARMED/energized, even with the TC declared
// not installed.
#include "test_common.h"

#include "../src/tasks/thermo_inject_gate.h"

static void test_inject_gate(void)
{
    TEST_SECTION("thermo_inject_gate: injection needs tc-not-installed AND relay not ARMED/energized");
    TEST_CHECK(thermo_inject_allowed(0u, false) == true, "tc not installed, relay idle -> allowed (bench)");
    TEST_CHECK(thermo_inject_allowed(0u, true) == false, "tc not installed but relay ARMED -> refused");
    TEST_CHECK(thermo_inject_allowed(1u, false) == false, "tc installed -> refused");
    TEST_CHECK(thermo_inject_allowed(1u, true) == false, "tc installed and ARMED -> refused");
}

void run_test_thermo_inject_gate(void)
{
    test_inject_gate();
}
