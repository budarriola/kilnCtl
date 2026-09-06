// Host test for App/drivers/hw/MAX31856.c's HAL Phase 1b migration
// (docs/HW_ABSTRACTION_PLAN.md, corrected 2026-09-05): MAX31856.c no longer
// talks to esp_spi_owner.c's spi_owner_t directly, it goes through
// interface/hal_spi.h, whose host backend is
// firmware/hwAbstraction/host/fake_spi.c -- same convention as
// test_ft6336u.c did for the hal_i2c migration.
//
// Coverage:
//   1. MAX31856_bus_init()/MAX31856_init() bring up a bus/device through
//      hal_spi_bus_init()/hal_spi_device_attach() (fake_spi records both).
//   2. MAX31856_read()'s one-transaction CJTH..SR burst read, scripted with
//      REALISTIC (not idealized/all-zero) MAX31856 register bytes taken
//      straight from test_max31856_codec.c's own known-good vectors
//      (0x19,0x00 -> CJ +25.0 degC; 0x3E,0x80,0x00 -> TC +1000.0 degC), and
//      the decoded MAX31856Reading fields this produces.
//   3. The CS/transfer sequence fake_spi recorded: exactly one POLLING
//      transfer (MAX31856_read_burst uses hal_spi_transfer_polling(), never
//      the queued hal_spi_transfer()), addressed at 0x0A (CJTH, bit 7 clear
//      = read), 7 bytes out (1 address + 6 data) and 7 bytes of rx space.
//   4. Negative test (see run_negative_decode_test.ps1-style inline check
//      below): this file's own build step corrupts MAX31856_read()'s decode
//      call, confirms the test goes RED, then the harness restores the
//      original and confirms git diff is clean for that hunk -- see
//      check_max31856_hal_spi_negative_test.ps1 in this directory, which
//      drives that mutation/restore cycle around this same executable.
#include <math.h>
#include <stdio.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#include "MAX31856.h"
#include "fake_spi.h"

#define EXACT 0.0

static void reset_all(void)
{
    fake_spi_reset_all();
}

int main(void)
{
    printf("-- MAX31856 (hal_spi) --\n\n");

    /* MAX31856_read()/_configure()/etc take a real (non-NULL) FreeRTOS
     * mutex -- the semphr.h host stub defaults xSemaphoreTake() on a
     * non-NULL handle to pdFALSE (see that file's comment: test_esp_spi_owner.c
     * relies on that default to simulate a completion semaphore that never
     * gets given). This test needs the opposite -- an uncontended
     * single-threaded host test actually getting its own channel lock --
     * so it opts in here. */
    g_test_stub_semaphore_take_default = 1; /* pdTRUE */

    reset_all();

    MAX31856BusClass bus = {0};
    MAX31856Class ch = {0};

    TEST_CHECK(MAX31856_bus_init(&bus, /*host=*/1, /*sclk=*/1, /*mosi=*/2, /*miso=*/3) == ESP_OK,
               "MAX31856_bus_init succeeds against fake_spi");
    TEST_CHECK(fake_spi_bus_is_live(&bus.hal_bus), "hal_spi_bus_init actually registered a live fake bus");

    TEST_CHECK(MAX31856_init(&ch, &bus, /*channel=*/0, /*cs_gpio=*/10, /*fault_gpio=*/-1) == ESP_OK,
               "MAX31856_init attaches channel 0 through hal_spi_device_attach");
    TEST_CHECK(fake_spi_device_is_live(&ch.dev), "hal_spi_device_attach actually registered a live fake device");
    TEST_CHECK(ch.dev_attached, "MAX31856Class.dev_attached set on successful attach");

    /* Realistic register bytes, not idealized zeros -- reused from
     * test_max31856_codec.c's own known-good vectors so this test's
     * expectation is pinned to the same numbers that file already proves the
     * codec produces, not a value invented here. CJTH:CJTL = 0x19,0x00 ->
     * +25.0 degC (0x1900 / 256). LTCBH:LTCBM:LTCBL = 0x3E,0x80,0x00 ->
     * +1000.0 degC (0x3E8000 / 4096). SR = 0x00 -- no faults, so neither
     * temperature is invalidated. */
    uint8_t scripted[7] = { 0x00 /* dummy byte clocked out during the address byte */,
                            0x19, 0x00,             /* CJTH, CJTL */
                            0x3E, 0x80, 0x00,       /* LTCBH, LTCBM, LTCBL */
                            0x00 };                 /* SR: no faults */
    TEST_CHECK(fake_spi_script_rx(&ch.dev, scripted, sizeof(scripted)) == HAL_OK,
               "script the CJTH..SR burst response");

    MAX31856Reading reading;
    esp_err_t read_err = MAX31856_read(&ch, &reading);
    TEST_CHECK(read_err == ESP_OK, "MAX31856_read succeeds");
    TEST_CHECK(!reading.spi_failed, "reading.spi_failed is false on a successful transfer");
    TEST_CHECK(reading.fault_status == 0x00, "reading.fault_status decoded from the scripted SR byte");
    TEST_CHECK_NEAR(reading.cj_temperature_c, 25.0, EXACT,
                     "cj_temperature_c decodes 0x19,0x00 -> +25.0 degC");
    TEST_CHECK_NEAR(reading.tc_temperature_c, 1000.0, EXACT,
                     "tc_temperature_c decodes 0x3E,0x80,0x00 -> +1000.0 degC");
    TEST_CHECK(reading.channel == 0, "reading.channel is the channel that was read");

    /* CS/transfer sequence: exactly one transfer, on the polling path
     * (MAX31856_read_burst uses hal_spi_transfer_polling(), never the queued
     * hal_spi_transfer() -- DISPLAY_ST7796_PLAN.md 9.5), addressed at CJTH
     * (0x0A) with bit 7 clear (a read), 7 bytes each way (1 address byte +
     * the 6-byte burst). */
    TEST_CHECK(fake_spi_transfer_count() == 1, "exactly one SPI transfer was issued for one MAX31856_read()");
    const fake_spi_transfer_record_t *rec = fake_spi_transfer(0);
    TEST_CHECK(rec != NULL, "the transfer record exists");
    if (rec) {
        TEST_CHECK(rec->kind == FAKE_SPI_XFER_POLLING, "the transfer used hal_spi_transfer_polling(), not hal_spi_transfer()");
        TEST_CHECK(rec->tx_len == 7, "tx_len is 1 address byte + 6 burst bytes");
        TEST_CHECK(rec->rx_len == 7, "rx_len matches tx_len (full-duplex burst)");
        TEST_CHECK(rec->tx[0] == MAX31856_REG_CJTH, "address byte is CJTH (0x0A) with the read bit (bit 7 clear)");
        TEST_CHECK((rec->tx[0] & 0x80u) == 0, "bit 7 of the address byte is clear -- a read, not a write");
    }

    /* A second read with a fault bit set (OPEN) must NaN tc_temperature_c
     * even though the burst decode itself succeeds -- proves the
     * fault-invalidates-temperature path still runs downstream of the new
     * hal_spi_transfer_polling() call, exactly as it did against the old
     * spi_owner_transfer_polling() call. */
    uint8_t scripted_fault[7] = { 0x00, 0x19, 0x00, 0x3E, 0x80, 0x00, THERMO_FAULT_OPEN };
    TEST_CHECK(fake_spi_script_rx(&ch.dev, scripted_fault, sizeof(scripted_fault)) == HAL_OK,
               "script a second burst response with OPEN faulted");
    esp_err_t read_err2 = MAX31856_read(&ch, &reading);
    TEST_CHECK(read_err2 == ESP_OK, "second MAX31856_read still succeeds (SPI transfer itself is fine)");
    TEST_CHECK(isnan(reading.tc_temperature_c), "OPEN fault NaNs tc_temperature_c even though the burst decoded fine");
    TEST_CHECK(reading.fault_status == THERMO_FAULT_OPEN, "fault_status reports OPEN");

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures == 0 ? 0 : 1;
}
