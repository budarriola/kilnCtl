// test_max31856_hal_spi.c -- HAL Phase 1b host test for max31856.c's
// migration off spi_owner.h onto interface/hal_spi.h (pico backend:
// hwAbstraction/pico/spi/hal_spi_pico.c). Links the REAL max31856.c against
// fake_spi.c (hwAbstraction/host/), not a mock of max31856.c itself -- this
// is what proves the migration preserved behavior, not just that the two
// sides of the interface compile against each other.
//
// hardware/gpio.h is stubbed to inert no-ops (stubs/hardware_gpio_min/) --
// max31856_init()'s CS/~FAULT GPIO bring-up is outside this pass's scope
// (only the SPI transport moved); see that stub header's own comment.
//
// Scripted register bytes are realistic MAX31856 CR1/CJTH:CJTL/LTCBH:M:L/SR
// values (not idealized round numbers picked to dodge a masking/sign-extend
// bug -- CLAUDE.md's "idealized test input" hazard class): the CR1 readback
// byte is compared against a real written CR1 (AVGSEL<<4 | tc_type) via
// max31856_cr1_readback_check(), and the temperature registers use the same
// datasheet-derived vectors test_max31856_decode.c already validates
// (CJ 0x0C,0x00 -> +12.0degC; a non-trivial LTCB triple -> +100.0degC).
#include "test_common.h"

#include <string.h>

#include "fake_spi.h"
#include "hal_spi.h"
#include "max31856.h"

#define TEST_CS_GPIO    11u
#define TEST_FAULT_GPIO 12u

static void setup(void)
{
    fake_spi_reset_all();
    TEST_CHECK(max31856_bus_init(), "max31856_bus_init() succeeds against fake_spi");
    TEST_CHECK(max31856_init(TEST_CS_GPIO, TEST_FAULT_GPIO), "max31856_init() succeeds");
}

static void test_configure_writes_and_cr1_readback(void)
{
    TEST_SECTION("max31856_configure() -- register writes + CR1 readback over hal_spi/fake_spi");
    setup();

    hal_spi_device_t *dev = max31856_spi_device_for_test();
    TEST_CHECK(dev != NULL, "max31856_spi_device_for_test() returns a live device after bus_init");

    // CR1 = AVGSEL(4 samples, 0x02)<<4 | tc_type(K, 0x03) = 0x23. Script the
    // one read_burst(CR1, 1) max31856_configure() issues at the end: rx[0]
    // is the meaningless address-echo byte, rx[1] is the real CR1 byte --
    // matching, so max31856_tc_type_verified() should read true afterward.
    uint8_t cr1_readback_rx[2] = { 0x00u, 0x23u };
    TEST_CHECK(fake_spi_script_rx(dev, cr1_readback_rx, sizeof(cr1_readback_rx)) == HAL_OK,
               "scripting the CR1 readback response succeeds");

    TEST_CHECK(max31856_configure(MAX31856_TC_TYPE_K), "max31856_configure(K) succeeds");
    TEST_CHECK(max31856_tc_type_verified(), "CR1 readback matched -> tc_type_verified() true");

    // Transfer sequence: CR0(stopped), CR1, MASK, CR0(running), then the
    // CR1 readback burst -- exactly what max31856_configure()'s own
    // comments document, in that order, nothing extra.
    TEST_CHECK(fake_spi_transfer_count() == 5, "exactly 5 transfers for one configure() call");

    const fake_spi_transfer_record_t *t0 = fake_spi_transfer(0);
    TEST_CHECK(t0 != NULL && t0->tx_len == 2 && t0->tx[0] == MAX31856_WRITE_ADDR(MAX31856_REG_CR0) &&
                   t0->tx[1] == (uint8_t)(MAX31856_OC_MODE1 << 4),
               "transfer 0: CR0 write, conversions stopped, OCFAULT mode 1");

    const fake_spi_transfer_record_t *t1 = fake_spi_transfer(1);
    TEST_CHECK(t1 != NULL && t1->tx_len == 2 && t1->tx[0] == MAX31856_WRITE_ADDR(MAX31856_REG_CR1) &&
                   t1->tx[1] == 0x23u,
               "transfer 1: CR1 write, AVGSEL=4 | TC TYPE=K");

    const fake_spi_transfer_record_t *t2 = fake_spi_transfer(2);
    TEST_CHECK(t2 != NULL && t2->tx_len == 2 && t2->tx[0] == MAX31856_WRITE_ADDR(MAX31856_REG_MASK) &&
                   t2->tx[1] == MAX31856_DEFAULT_FAULT_MASK,
               "transfer 2: MASK write, OPEN+OVUV unmasked");

    const fake_spi_transfer_record_t *t3 = fake_spi_transfer(3);
    TEST_CHECK(t3 != NULL && t3->tx_len == 2 && t3->tx[0] == MAX31856_WRITE_ADDR(MAX31856_REG_CR0) &&
                   t3->tx[1] == (uint8_t)((MAX31856_OC_MODE1 << 4) | MAX31856_CR0_CMODE),
               "transfer 3: CR0 write, CMODE restored (automatic conversion running)");

    const fake_spi_transfer_record_t *t4 = fake_spi_transfer(4);
    TEST_CHECK(t4 != NULL && t4->kind == FAKE_SPI_XFER_POLLING && t4->tx_len == 2 &&
                   t4->rx_len == 2 && t4->tx[0] == (uint8_t)(MAX31856_REG_CR1 & 0x7Fu) &&
                   t4->rx[1] == 0x23u,
               "transfer 4: CR1 readback, polling kind, scripted byte came back through");
}

static void test_configure_cr1_mismatch_not_verified(void)
{
    TEST_SECTION("max31856_configure() -- CR1 readback mismatch leaves tc_type_verified() false");
    setup();

    hal_spi_device_t *dev = max31856_spi_device_for_test();
    // A CR1 low nibble that does NOT match TC TYPE K (0x03) -- e.g. the
    // part still reporting its power-on-default nibble (0x03 would match by
    // coincidence, so pick a genuinely different one, Type J = 0x02, with a
    // plausible AVGSEL in the high nibble so this is not the DEAD_BUS
    // 0x00/0xFF special case either).
    uint8_t mismatch_rx[2] = { 0x00u, 0x22u };
    TEST_CHECK(fake_spi_script_rx(dev, mismatch_rx, sizeof(mismatch_rx)) == HAL_OK,
               "scripting a mismatched CR1 readback succeeds");

    TEST_CHECK(max31856_configure(MAX31856_TC_TYPE_K), "max31856_configure(K) still succeeds "
               "(the writes themselves are fine; only the confirmation disagrees)");
    TEST_CHECK(!max31856_tc_type_verified(), "CR1 mismatch -> tc_type_verified() false");
}

static void test_read_decodes_temperature_and_faults(void)
{
    TEST_SECTION("max31856_read() -- CJ/TC decode + fault flags over hal_spi/fake_spi");
    setup();
    TEST_CHECK(max31856_configure(MAX31856_TC_TYPE_K), "configure() before read()");

    hal_spi_device_t *dev = max31856_spi_device_for_test();

    // Burst order per max31856_read(): CJTH, CJTL, LTCBH, LTCBM, LTCBL, SR.
    // rx[0] is the address-echo byte (discarded); CJ 0x0C,0x00 -> +12.0degC
    // and TC 0x06,0x40,0x00 -> +100.0degC are the same datasheet-derived
    // fixed-point vectors test_max31856_decode.c already validates in
    // isolation -- this test proves max31856_read() wires them together
    // through the real burst/fault-masking logic, not the decode math
    // itself again.
    uint8_t read_rx[7] = { 0x00u, 0x0Cu, 0x00u, 0x06u, 0x40u, 0x00u, 0x00u /* SR: no faults */ };
    TEST_CHECK(fake_spi_script_rx(dev, read_rx, sizeof(read_rx)) == HAL_OK,
               "scripting the temperature burst response succeeds");

    max31856_reading_t reading;
    memset(&reading, 0xAA, sizeof(reading)); // poison, so a field left unset is visible
    TEST_CHECK(max31856_read(&reading), "max31856_read() succeeds");
    TEST_CHECK(!reading.spi_failed, "spi_failed is false on a successful transfer");
    TEST_CHECK(reading.cj_temperature_c == 12.0f, "cj_temperature_c decodes to +12.0 degC");
    TEST_CHECK(reading.tc_temperature_c == 100.0f, "tc_temperature_c decodes to +100.0 degC");
    TEST_CHECK(reading.fault_status == 0x00u, "fault_status carries the scripted SR byte (no faults)");

    const fake_spi_transfer_record_t *last =
        fake_spi_transfer(fake_spi_transfer_count() - 1);
    TEST_CHECK(last != NULL && last->tx_len == 7 &&
                   last->tx[0] == (uint8_t)(MAX31856_REG_CJTH & 0x7Fu),
               "the burst transfer addressed CJTH with a 7-byte (1 addr + 6 register) length");
}

static void test_read_open_fault_invalidates_tc_only(void)
{
    TEST_SECTION("max31856_read() -- OPEN fault invalidates tc_temperature_c, not cj_temperature_c");
    setup();
    TEST_CHECK(max31856_configure(MAX31856_TC_TYPE_K), "configure() before read()");

    hal_spi_device_t *dev = max31856_spi_device_for_test();
    // Same CJ/TC register bytes as the success case, but SR now carries
    // MAX31856_FAULT_OPEN -- a realistic single-bit fault byte, not an
    // idealized 0x00/0xFF -- so tc_temperature_c must come back NaN while
    // cj_temperature_c (unaffected by OPEN) stays a real number.
    uint8_t read_rx[7] = { 0x00u, 0x0Cu, 0x00u, 0x06u, 0x40u, 0x00u, MAX31856_FAULT_OPEN };
    TEST_CHECK(fake_spi_script_rx(dev, read_rx, sizeof(read_rx)) == HAL_OK,
               "scripting the OPEN-fault burst response succeeds");

    max31856_reading_t reading;
    TEST_CHECK(max31856_read(&reading), "max31856_read() still succeeds (the transfer worked)");
    TEST_CHECK(!reading.spi_failed, "spi_failed is false -- the bus worked, the sensor did not");
    TEST_CHECK(reading.tc_temperature_c != reading.tc_temperature_c /* NaN */,
               "tc_temperature_c is NaN when OPEN is set");
    TEST_CHECK(reading.cj_temperature_c == 12.0f,
               "cj_temperature_c is unaffected by an OPEN thermocouple fault");
    TEST_CHECK((reading.fault_status & MAX31856_FAULT_OPEN) != 0,
               "fault_status carries the OPEN bit through");
}

static void test_unscripted_read_returns_zero_not_garbage(void)
{
    TEST_SECTION("max31856_read() -- an unscripted (empty-queue) burst zero-fills, per fake_spi.h");
    setup();
    TEST_CHECK(max31856_configure(MAX31856_TC_TYPE_K), "configure() before read()");

    // fake_spi.h's documented contract for an empty scripted-rx queue: "A
    // call with no scripted response left reads back all zero bytes...not
    // an error." No fake_spi_script_rx() call here on purpose -- this
    // proves max31856_read() still succeeds and reports a real, zero-valued
    // reading (never max31856_reading_t's initial 0xAA poison) rather than
    // leaking an uninitialized rx buffer through.
    max31856_reading_t reading;
    memset(&reading, 0xAA, sizeof(reading));
    TEST_CHECK(max31856_read(&reading),
               "max31856_read() succeeds even with no scripted response queued");
    TEST_CHECK(!reading.spi_failed, "spi_failed is false -- the transfer itself still succeeded");
    TEST_CHECK(reading.fault_status == 0x00u,
               "an all-zero (unscripted) SR byte carries no fault bits");
}

// max31856_verify_live_config() -- 2026-09-23, the periodic config-drift
// check (THERMOCOUPLE.md's "automatic config re-assertion if the part is
// ever seen to have reset"). Covers all four documented outcomes.

static void test_verify_live_config_before_configure_is_read_failed(void)
{
    TEST_SECTION("max31856_verify_live_config() -- never configured -> READ_FAILED, not MATCH");
    setup();

    // No max31856_configure() call at all this bring-up -- must not report
    // MATCH just because the live (unwritten) registers happen to equal
    // max31856_init()'s power-on-default shadow seed.
    TEST_CHECK(max31856_verify_live_config() == MAX31856_LIVE_CHECK_READ_FAILED,
               "never-configured reads as READ_FAILED, never MATCH or MISMATCH");
}

static void test_verify_live_config_match(void)
{
    TEST_SECTION("max31856_verify_live_config() -- live registers equal the shadow -> MATCH");
    setup();

    hal_spi_device_t *dev = max31856_spi_device_for_test();
    uint8_t cr1_readback_rx[2] = { 0x00u, 0x23u };
    TEST_CHECK(fake_spi_script_rx(dev, cr1_readback_rx, sizeof(cr1_readback_rx)) == HAL_OK,
               "scripting configure()'s own CR1 readback succeeds");
    TEST_CHECK(max31856_configure(MAX31856_TC_TYPE_K), "configure(K) succeeds");

    // Live readback of CR0+CR1: script the exact bytes configure() just
    // wrote (CR0 running == OC_MODE1<<4 | CMODE, CR1 == 0x23).
    uint8_t live_rx[3] = { 0x00u, (uint8_t)((MAX31856_OC_MODE1 << 4) | MAX31856_CR0_CMODE), 0x23u };
    TEST_CHECK(fake_spi_script_rx(dev, live_rx, sizeof(live_rx)) == HAL_OK,
               "scripting a matching live CR0/CR1 readback succeeds");

    TEST_CHECK(max31856_verify_live_config() == MAX31856_LIVE_CHECK_MATCH,
               "live registers equal to the shadow reads as MATCH");
}

static void test_verify_live_config_mismatch(void)
{
    TEST_SECTION("max31856_verify_live_config() -- live registers differ -> MISMATCH");
    setup();

    hal_spi_device_t *dev = max31856_spi_device_for_test();
    uint8_t cr1_readback_rx[2] = { 0x00u, 0x23u };
    TEST_CHECK(fake_spi_script_rx(dev, cr1_readback_rx, sizeof(cr1_readback_rx)) == HAL_OK,
               "scripting configure()'s own CR1 readback succeeds");
    TEST_CHECK(max31856_configure(MAX31856_TC_TYPE_K), "configure(K) succeeds");

    // Live readback showing the part's power-on-default CR0/CR1 (0x00, 0x03)
    // -- as if it silently reset mid-run, reverting away from the
    // commissioned shadow (CR0 running, CR1 0x23).
    uint8_t live_rx[3] = { 0x00u, 0x00u, 0x03u };
    TEST_CHECK(fake_spi_script_rx(dev, live_rx, sizeof(live_rx)) == HAL_OK,
               "scripting a power-on-default (reset) live CR0/CR1 readback succeeds");

    TEST_CHECK(max31856_verify_live_config() == MAX31856_LIVE_CHECK_MISMATCH,
               "live registers reverted to power-on defaults reads as MISMATCH");
}

static void test_verify_live_config_spi_failure_is_read_failed(void)
{
    TEST_SECTION("max31856_verify_live_config() -- SPI transfer failure -> READ_FAILED, not MISMATCH");
    setup();

    hal_spi_device_t *dev = max31856_spi_device_for_test();
    uint8_t cr1_readback_rx[2] = { 0x00u, 0x23u };
    TEST_CHECK(fake_spi_script_rx(dev, cr1_readback_rx, sizeof(cr1_readback_rx)) == HAL_OK,
               "scripting configure()'s own CR1 readback succeeds");
    TEST_CHECK(max31856_configure(MAX31856_TC_TYPE_K), "configure(K) succeeds");

    // A transient SPI hiccup on the live-check transfer itself must never be
    // reported as MISMATCH (that would falsely claim the part reset when the
    // bus, not the sensor, is the problem) -- see max31856_live_check.h's own
    // note_result() contract on why READ_FAILED must be handled separately.
    fake_spi_inject_enqueue_timeout(max31856_spi_bus_for_test(), 1);

    TEST_CHECK(max31856_verify_live_config() == MAX31856_LIVE_CHECK_READ_FAILED,
               "a failed SPI transfer reads as READ_FAILED, never MISMATCH");
}

void run_test_max31856_hal_spi(void)
{
    test_configure_writes_and_cr1_readback();
    test_configure_cr1_mismatch_not_verified();
    test_read_decodes_temperature_and_faults();
    test_read_open_fault_invalidates_tc_only();
    test_unscripted_read_returns_zero_not_garbage();
    test_verify_live_config_before_configure_is_read_failed();
    test_verify_live_config_match();
    test_verify_live_config_mismatch();
    test_verify_live_config_spi_failure_is_read_failed();
}
