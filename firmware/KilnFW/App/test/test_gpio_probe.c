// Host test for App/drivers/bridge/gpio_probe_denylist.h's gpio_probe_pin_is_
// denied() -- the thermocouple fault-pin gap this pass closes (verified:
// THERMO_FAULT0/1/2_IO were missing from gpio_probe.c's old inline
// `denied[]` array, alongside SAFETY_FAULT_IO which WAS already covered --
// a probe session could drive those pins and forge "no thermocouple fault"
// to anything watching them).
//
// Tests the header directly rather than #including gpio_probe.c the way
// test_boot_button.c/test_bx_worker_reentrancy.c #include their subjects:
// gpio_probe.c (CONFIG_KILNCTL_ENABLE_GPIO_PROBE build) pulls BOTH
// profile_executor.h's STUBBED espInterfaces/uart_protocol.h (found via an
// unqualified #include that falls back to test/stubs/) AND gpio_probe.h's
// REAL espInterfaces/uart_protocol.h (found via its own directory-relative
// #include) into the same translation unit -- these redefine every
// uart_protocol_t/uart_owner_t type against each other and the file cannot
// be compiled by this host harness as a whole. gpio_probe.c's own
// gpio_probe_is_denied() is now a one-line wrapper around
// gpio_probe_pin_is_denied() (see gpio_probe_denylist.h's header comment)
// specifically so the actual deny-list logic CAN be host-tested here,
// dependency-free past settings.h.
#include "test_common.h"

#include "../drivers/bridge/gpio_probe_denylist.h"

static void test_thermo_fault_pins_are_denied(void)
{
    // THIS is the gap this pass closes: before it, these three returned
    // false (allowed) even though SAFETY_FAULT_IO -- the pin the header
    // comment calls out as "the one safety-domain pin whose misuse is
    // silent and one-directional" -- was already denied. A probe session
    // could set one of these to OUTPUT and drive it, silently telling
    // anything watching the pin "no thermocouple fault" while a real fault
    // was latched underneath.
    //
    // NEGATIVE-TESTED: temporarily removing "THERMO_FAULT0_IO, THERMO_FAULT1_IO,
    // THERMO_FAULT2_IO," from gpio_probe_denylist.h's denied[] array (the
    // exact regression this pass fixed) makes all three of these fail --
    // see the task report for the captured real failure output with that
    // break in place (reverted before this file was finalized).
    TEST_CHECK(gpio_probe_pin_is_denied(THERMO_FAULT0_IO), "THERMO_FAULT0_IO must be on the deny-list");
    TEST_CHECK(gpio_probe_pin_is_denied(THERMO_FAULT1_IO), "THERMO_FAULT1_IO must be on the deny-list");
    TEST_CHECK(gpio_probe_pin_is_denied(THERMO_FAULT2_IO), "THERMO_FAULT2_IO must be on the deny-list");
}

static void test_preexisting_denied_pins_still_denied(void)
{
    // Not a regression test for THIS pass, but proves the addition above
    // didn't shadow or truncate the rest of the array -- SAFETY_FAULT_IO is
    // the pin the module's own comment names explicitly, plus one pin each
    // from the SPI and PC-link groups.
    TEST_CHECK(gpio_probe_pin_is_denied(SAFETY_FAULT_IO), "SAFETY_FAULT_IO stays denied");
    TEST_CHECK(gpio_probe_pin_is_denied(KILN_SPI_SCLK_IO), "KILN_SPI_SCLK_IO stays denied");
    TEST_CHECK(gpio_probe_pin_is_denied(UART_OWNER_TX_IO), "UART_OWNER_TX_IO stays denied");
}

static void test_ordinary_pin_not_denied(void)
{
    // A pin that appears on none of these lists (test double value, chosen
    // distinct from every CONFIG_KILNCTL_* stub value in
    // test/stubs/sdkconfig.h) must NOT be refused -- proves
    // gpio_probe_pin_is_denied() isn't vacuously "always true".
    TEST_CHECK(!gpio_probe_pin_is_denied(99), "an ordinary, unlisted pin is not denied");
}

// SAFETY_TX_IO/SAFETY_RX_IO are deliberately NOT denied -- see this
// header's own comment for why (the coordinated GPIO test needs to drive/
// read exactly these two pins through the probe).
static void test_safety_link_pins_not_denied(void)
{
    TEST_CHECK(!gpio_probe_pin_is_denied(CONFIG_KILNCTL_SAFETY_TX_IO),
               "SAFETY_TX_IO stays reachable for the coordinated GPIO test");
    TEST_CHECK(!gpio_probe_pin_is_denied(CONFIG_KILNCTL_SAFETY_RX_IO),
               "SAFETY_RX_IO stays reachable for the coordinated GPIO test");
}

void run_test_gpio_probe(void)
{
    TEST_SECTION("gpio_probe: gpio_probe_pin_is_denied() thermocouple fault pins");
    test_thermo_fault_pins_are_denied();
    test_preexisting_denied_pins_still_denied();
    test_ordinary_pin_not_denied();
    test_safety_link_pins_not_denied();
}
