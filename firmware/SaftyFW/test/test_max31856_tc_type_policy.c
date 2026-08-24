// Host tests for firmware/SaftyFW/src/max31856_tc_type_policy.c -- the pure
// CR1.TC TYPE[3:0] range predicate max31856_configure() refuses on. No
// pico-sdk/FreeRTOS/hardware dependency (max31856.h itself is a plain
// register-map/struct header), same discipline as
// test_uart_owner_tx_policy.c.
//
// CRITICAL project rule (per this repo's "negative-test every check"
// discipline): every accept-direction check below is paired with a refuse-
// direction check that proves the predicate can actually fail, not just
// always return true.
#include "test_common.h"

#include "max31856.h"
#include "max31856_tc_type_policy.h"

static void test_accepts_every_real_type(void)
{
    TEST_SECTION("max31856_tc_type_is_valid -- accepts every real thermocouple type (0x00-0x07)");

    for (uint8_t tc_type = 0x00u; tc_type <= 0x07u; tc_type++) {
        TEST_CHECK(max31856_tc_type_is_valid(tc_type), "0x00-0x07 (real thermocouple type) accepted");
    }
    TEST_CHECK(max31856_tc_type_is_valid(MAX31856_TC_TYPE_B), "MAX31856_TC_TYPE_B accepted");
    TEST_CHECK(max31856_tc_type_is_valid(MAX31856_TC_TYPE_K), "MAX31856_TC_TYPE_K accepted");
    TEST_CHECK(max31856_tc_type_is_valid(MAX31856_TC_TYPE_T), "MAX31856_TC_TYPE_T (the boundary) accepted");
}

static void test_refuses_every_voltage_mode_code(void)
{
    TEST_SECTION("max31856_tc_type_is_valid -- refuses every Voltage Mode code (0x08-0x0F), "
                  "individually, not a spot check");

    // Datasheet page 20's CR1 field table: 10xx = Voltage Mode gain 8
    // (0x08-0x0B), 11xx = Voltage Mode gain 32 (0x0C-0x0F). All eight values
    // must be refused -- this is the exact defect being fixed (the codebase
    // rule: prove a new check can actually fail, individually, for a bound
    // like this rather than a single spot value).
    TEST_CHECK(!max31856_tc_type_is_valid(0x08u), "0x08 (voltage mode, gain 8) refused");
    TEST_CHECK(!max31856_tc_type_is_valid(0x09u), "0x09 (voltage mode, gain 8) refused");
    TEST_CHECK(!max31856_tc_type_is_valid(0x0Au), "0x0A (voltage mode, gain 8) refused");
    TEST_CHECK(!max31856_tc_type_is_valid(0x0Bu), "0x0B (voltage mode, gain 8) refused");
    TEST_CHECK(!max31856_tc_type_is_valid(0x0Cu), "0x0C (voltage mode, gain 32) refused");
    TEST_CHECK(!max31856_tc_type_is_valid(0x0Du), "0x0D (voltage mode, gain 32) refused");
    TEST_CHECK(!max31856_tc_type_is_valid(0x0Eu), "0x0E (voltage mode, gain 32) refused");
    TEST_CHECK(!max31856_tc_type_is_valid(0x0Fu), "0x0F (voltage mode, gain 32) refused");
}

static void test_refuses_out_of_range_bytes(void)
{
    TEST_SECTION("max31856_tc_type_is_valid -- refuses bytes above the 4-bit field's own range");

    // These cannot occur from a real CR1.TC TYPE[3:0] nibble, but the
    // predicate is handed an arbitrary byte from flash/the wire, not a
    // guaranteed-4-bit value -- must still refuse.
    TEST_CHECK(!max31856_tc_type_is_valid(0xFFu), "0xFF refused");
    TEST_CHECK(!max31856_tc_type_is_valid(0x10u), "0x10 (just past the 4-bit field) refused");
    TEST_CHECK(!max31856_tc_type_is_valid(0x80u), "0x80 refused");
}

void run_test_max31856_tc_type_policy(void)
{
    test_accepts_every_real_type();
    test_refuses_every_voltage_mode_code();
    test_refuses_out_of_range_bytes();
}
