// Host tests for firmware/SaftyFW/src/max31856_fault_pin_policy.c -- the
// pure ~FAULT-pin raw-GPIO-level-to-asserted-bool predicate max31856_read()
// calls instead of doing the polarity check inline. No pico-sdk/FreeRTOS/
// hardware dependency, same discipline as test_max31856_tc_type_policy.c and
// test_discrete_pin_policy.c.
//
// Why this matters (see max31856_fault_pin_policy.h's own comment): this
// predicate's output feeds max31856_reading_t.fault_pin_asserted, which
// safety_guards.c's S5 (thermocouple fault) reads. A flipped polarity here
// is the exact same bug class as the E-stop inversion that shipped invisible
// to host tests for GPIO9/S7 -- it would make S5 either never trip on a
// genuine MAX31856 fault (open TC, over/undervoltage, bad cold-junction
// reading) or trip permanently on a healthy, idle part.
//
// CRITICAL project rule (per this repo's "negative-test every check"
// discipline): every accept-direction check below is paired with a refuse-
// direction check that proves the predicate can actually fail, not just
// always return true.
#include "test_common.h"

#include "max31856_fault_pin_policy.h"

static void test_low_is_asserted(void)
{
    TEST_SECTION("max31856_fault_pin_asserted -- LOW (fault_gpio_high == false) is asserted");

    // ~FAULT is active-low, open-drain (KilnFW's firmware/KilnFW/App/
    // drivers/MAX31856.c line 531 and MAX31856.h line 482): the part pulls
    // the line low to report a fault. If this ever silently flips, S5 stops
    // seeing genuine faults -- exactly the direction the pin's open-drain
    // wiring exists to report.
    TEST_CHECK(max31856_fault_pin_asserted(false) == true, "LOW reads as asserted (fault)");
}

static void test_high_is_not_asserted(void)
{
    TEST_SECTION("max31856_fault_pin_asserted -- HIGH (fault_gpio_high == true) is NOT asserted");

    // A flipped polarity here would instead make S5 trip permanently on a
    // healthy, idle part -- the opposite failure shape, but just as real:
    // the board could never arm.
    TEST_CHECK(max31856_fault_pin_asserted(true) == false, "HIGH reads as not asserted (healthy)");
}

void run_test_max31856_fault_pin_policy(void)
{
    test_low_is_asserted();
    test_high_is_not_asserted();
}
