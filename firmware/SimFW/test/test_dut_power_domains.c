// Host tests pinning the pure decision logic behind i2c_owner.c's two
// independent DUT-power relays (docs/HARDWARE.md section 3.7 / docs/BOM.md
// section 6, resolved 2026-08-20): relay #1 (EXP1_PIN_DUT_POWER_MAIN, exp1
// pin 7) drives J18/main, relay #2 (EXP1_PIN_DUT_POWER_SAFETY, exp1 pin 10)
// drives J19/safety, and the two must be independently addressable end to
// end with no code path that grounds-ties them by defaulting one command
// to "both."
//
// Why this file mirrors rather than calls the real code: i2c_owner.c is a
// FreeRTOS task file (includes FreeRTOS.h/task.h, calls hardware/i2c.h and
// drivers/mcp23017.h), so it is not part of this host-test harness's source
// list -- build_host_tests.ps1 compiles only src/sim/'s pure modules, the
// same pure/task boundary test_cmd_task_gap_closure.c's header comment
// documents for cmd_task.c. Each function below is a deliberately small,
// byte-for-byte mirror of the corresponding block in i2c_owner.c/.h (cited
// in each function's comment) -- keep the two in sync by hand if either
// changes.
#include <stdbool.h>
#include <stdint.h>

#include "test_common.h"

// --- Pin constants, mirroring i2c_owner.c's #defines exactly ---------------
#define M_EXP1_PIN_K1          0u
#define M_EXP1_PIN_FAULT_LINE  5u
#define M_EXP1_PIN_ESTOP_DRIVE 6u
#define M_EXP1_PIN_DUT_POWER_MAIN   7u
#define M_EXP1_PIN_J20_IO3     8u
#define M_EXP1_PIN_J20_IO4     9u
#define M_EXP1_PIN_DUT_POWER_SAFETY 10u
#define M_EXP1_RESERVED_MAX_PIN 7u

// Mirrors i2c_owner.c's io_pin_allowed(): exp1 pins 0..7 and pin 10 are
// reserved fixed roles rejected by the generic i2c_owner_io_*() path; pins
// 8/9 (J20) and 11..15 (true spares) are not. exp2 has no reserved pins.
typedef enum { M_EXP_1 = 0, M_EXP_2 = 1 } mirror_expander_t;

static bool mirror_io_pin_allowed(mirror_expander_t exp, uint8_t pin)
{
    if (pin > 15u) {
        return false;
    }
    if (exp == M_EXP_1 &&
        (pin <= M_EXP1_RESERVED_MAX_PIN || pin == M_EXP1_PIN_DUT_POWER_SAFETY)) {
        return false;
    }
    return true;
}

// --- Two-relay state mirror, mirroring i2c_owner.c's
// s_dut_power_main_on/s_dut_power_safety_on plus the SET_DUT_POWER_MAIN/
// SET_DUT_POWER_SAFETY command handlers in apply_pending_commands(): each
// command touches exactly one bit, never both. There is deliberately no
// "set both" mirror function, matching i2c_owner.h's documented absence of
// one. -----------------------------------------------------------------
typedef struct {
    bool main_on;
    bool safety_on;
} mirror_dut_power_t;

static void mirror_set_dut_power_main(mirror_dut_power_t *s, bool on)
{
    s->main_on = on;
}

static void mirror_set_dut_power_safety(mirror_dut_power_t *s, bool on)
{
    s->safety_on = on;
}

// Mirrors i2c_owner_set_dut_power() / SIMFW_CMD_IO_DUT_POWER_SET (0x06)'s
// deprecated-alias behavior: touches ONLY the main bit, never the safety
// bit, regardless of the safety bit's current state.
static void mirror_set_dut_power_legacy_alias(mirror_dut_power_t *s, bool on)
{
    mirror_set_dut_power_main(s, on);
}

static void test_reserved_pins_reject_both_relay_bits(void)
{
    TEST_SECTION("dut-power domains -- exp1 pins 7 (main) and 10 (safety) are both reserved");

    TEST_CHECK(!mirror_io_pin_allowed(M_EXP_1, M_EXP1_PIN_DUT_POWER_MAIN),
               "generic IO path refuses exp1 pin 7 (DUT-power main)");
    TEST_CHECK(!mirror_io_pin_allowed(M_EXP_1, M_EXP1_PIN_DUT_POWER_SAFETY),
               "generic IO path refuses exp1 pin 10 (DUT-power safety) -- a test cannot silently "
               "repurpose the safety relay's bit through the generic write path");
    TEST_CHECK(!mirror_io_pin_allowed(M_EXP_1, M_EXP1_PIN_K1), "relay-sense pin 0 still reserved");
    TEST_CHECK(!mirror_io_pin_allowed(M_EXP_1, M_EXP1_PIN_FAULT_LINE), "fault-line pin 5 still reserved");
    TEST_CHECK(!mirror_io_pin_allowed(M_EXP_1, M_EXP1_PIN_ESTOP_DRIVE), "E-stop drive pin 6 still reserved");
}

static void test_j20_and_spares_still_generic(void)
{
    TEST_SECTION("dut-power domains -- reserving pin 10 does not swallow J20 or the remaining spares");

    TEST_CHECK(mirror_io_pin_allowed(M_EXP_1, M_EXP1_PIN_J20_IO3), "exp1 pin 8 (J20 IO_3) stays generic");
    TEST_CHECK(mirror_io_pin_allowed(M_EXP_1, M_EXP1_PIN_J20_IO4), "exp1 pin 9 (J20 IO_4) stays generic");
    TEST_CHECK(mirror_io_pin_allowed(M_EXP_1, 11u), "exp1 pin 11 (first true spare after pin 10) stays generic");
    TEST_CHECK(mirror_io_pin_allowed(M_EXP_1, 15u), "exp1 pin 15 (last spare) stays generic");
    TEST_CHECK(mirror_io_pin_allowed(M_EXP_2, M_EXP1_PIN_DUT_POWER_SAFETY),
               "pin 10 on exp2 (0x21) is NOT reserved -- the reservation is exp1-only, by expander");
}

static void test_relays_are_independently_settable(void)
{
    TEST_SECTION("dut-power domains -- main and safety relays are independently commanded");

    mirror_dut_power_t s = {0};
    TEST_CHECK(!s.main_on && !s.safety_on, "both relays default off at boot");

    mirror_set_dut_power_main(&s, true);
    TEST_CHECK(s.main_on, "SET_DUT_POWER_MAIN(on) sets the main bit");
    TEST_CHECK(!s.safety_on, "...and leaves the safety bit untouched (still off)");

    mirror_set_dut_power_safety(&s, true);
    TEST_CHECK(s.main_on && s.safety_on, "SET_DUT_POWER_SAFETY(on) sets the safety bit, main stays on");

    mirror_set_dut_power_main(&s, false);
    TEST_CHECK(!s.main_on && s.safety_on,
               "turning the main relay off leaves the safety relay on -- no ganged behavior in either "
               "direction (this is the scenario the fixture exists to support: brown out one domain "
               "while the other stays powered)");

    mirror_set_dut_power_safety(&s, false);
    TEST_CHECK(!s.main_on && !s.safety_on, "both off again after independently clearing each");
}

static void test_legacy_alias_touches_main_only(void)
{
    TEST_SECTION("dut-power domains -- the legacy DUT_POWER_SET/0x06 alias means main-only, not \"both\"");

    mirror_dut_power_t s = {0};
    mirror_set_dut_power_safety(&s, true);
    TEST_CHECK(s.safety_on, "safety relay independently turned on first");

    mirror_set_dut_power_legacy_alias(&s, true);
    TEST_CHECK(s.main_on, "legacy alias turns the main relay on");
    TEST_CHECK(s.safety_on, "legacy alias leaves the safety relay exactly as it was (still on) -- it "
                             "does NOT redefine itself to mean \"both relays\"");

    mirror_set_dut_power_legacy_alias(&s, false);
    TEST_CHECK(!s.main_on, "legacy alias turns the main relay back off");
    TEST_CHECK(s.safety_on, "safety relay is completely unaffected by the legacy command in either direction -- "
                             "a caller using only the old command can never accidentally bond the two domains");
}

void run_test_dut_power_domains(void)
{
    test_reserved_pins_reject_both_relay_bits();
    test_j20_and_spares_still_generic();
    test_relays_are_independently_settable();
    test_legacy_alias_touches_main_only();
}
