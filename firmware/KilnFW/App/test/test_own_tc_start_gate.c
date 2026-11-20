// Host test for drivers/control/own_tc_start_gate.h (AT-19/AT-24).
#include "test_common.h"
#include "../drivers/control/own_tc_start_gate.h"

void run_test_own_tc_start_gate(void)
{
    TEST_SECTION("AT-24: new start refused while the Pico own TC is faulted");
    TEST_CHECK(own_tc_start_must_refuse(true, 0u, THERMO_FAULT_OPEN), "OPEN refuses");
    TEST_CHECK(own_tc_start_must_refuse(true, 0u, THERMO_FAULT_OVUV), "OVUV refuses");
    TEST_CHECK(own_tc_start_must_refuse(true, 0u, THERMO_FAULT_TCRANGE), "TCRANGE refuses");
    TEST_CHECK(!own_tc_start_must_refuse(true, 0u, 0u), "healthy TC does not refuse");
    TEST_CHECK(!own_tc_start_must_refuse(true, 0u, THERMO_FAULT_CJHIGH), "cold-junction-only bit is S12's job, not refused");
    TEST_CHECK(!own_tc_start_must_refuse(true, SAFETY_FLAG_TC_NOT_INSTALLED, THERMO_FAULT_OPEN), "not_installed never refuses");
    TEST_CHECK(!own_tc_start_must_refuse(false, 0u, THERMO_FAULT_OPEN), "link down: other interlocks decide");
}
