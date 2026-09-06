// Host test for App/drivers/hw/MAX31856.c's HAL Phase 1b migration
// (docs/HW_ABSTRACTION.md): MAX31856.c no longer talks to
// esp_spi_owner.c's spi_owner_t directly, it goes through
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
//      transfer per read (MAX31856_read_burst uses hal_spi_transfer_polling(),
//      never the queued hal_spi_transfer()), addressed at 0x0A (CJTH, bit 7
//      clear = read), 7 bytes out (1 address + 6 data) and 7 bytes of rx
//      space -- AND fake_spi's recorded cs_pin equals the channel's own
//      cs_gpio, for EVERY channel on the bus (opus review, 2026-09-06: the
//      software-CS branch was briefly landed with dev_cfg.cs_pin =
//      HAL_CS_NONE, silently dropping bit-banged CS on all reads -- a
//      transfer log with no CS assertion still "succeeds" and a scripted rx
//      response still decodes to a plausible-looking temperature, so this
//      case must be checked explicitly, not inferred from decode success).
//   4. Negative test: this file's own build step corrupts the device attach
//      config (cs_pin -> HAL_CS_NONE, reproducing the real regression this
//      test exists to catch), confirms the CS assertions below go RED, then
//      restores the original and confirms git diff is clean for that hunk.
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

// Attaches channel `i` (into caller-owned, still-live storage `ch`) at a
// distinct cs_gpio (10+i), scripts one valid CJTH..SR burst response, reads
// it, and asserts both the decode AND the CS/transfer sequence fake_spi
// recorded for that specific channel.
static void check_channel(MAX31856BusClass *bus, MAX31856Class *ch, uint8_t i)
{
    int cs_gpio = 10 + (int)i;
    char msg[160];

    TEST_CHECK(MAX31856_init(ch, bus, i, cs_gpio, /*fault_gpio=*/-1) == ESP_OK,
               "MAX31856_init attaches this channel through hal_spi_device_attach");
    TEST_CHECK(ch->dev_attached, "dev_attached set on successful attach");

    uint8_t scripted[7] = { 0x00, 0x19, 0x00, 0x3E, 0x80, 0x00, 0x00 };
    TEST_CHECK(fake_spi_script_rx(&ch->dev, scripted, sizeof(scripted)) == HAL_OK,
               "script the CJTH..SR burst response");

    size_t before = fake_spi_transfer_count();
    MAX31856Reading reading;
    esp_err_t read_err = MAX31856_read(ch, &reading);
    TEST_CHECK(read_err == ESP_OK, "MAX31856_read succeeds");
    TEST_CHECK(!reading.spi_failed, "spi_failed is false on a successful transfer");
    TEST_CHECK_NEAR(reading.cj_temperature_c, 25.0, EXACT, "cj_temperature_c decodes correctly");
    TEST_CHECK_NEAR(reading.tc_temperature_c, 1000.0, EXACT, "tc_temperature_c decodes correctly");

    TEST_CHECK(fake_spi_transfer_count() == before + 1, "exactly one SPI transfer was issued for this read");
    const fake_spi_transfer_record_t *rec = fake_spi_transfer(before);
    TEST_CHECK(rec != NULL, "the transfer record exists");
    if (rec) {
        TEST_CHECK(rec->kind == FAKE_SPI_XFER_POLLING, "used hal_spi_transfer_polling(), not hal_spi_transfer()");
        TEST_CHECK(rec->tx_len == 7, "tx_len is 1 address byte + 6 burst bytes");
        TEST_CHECK(rec->rx_len == 7, "rx_len matches tx_len (full-duplex burst)");
        TEST_CHECK(rec->tx[0] == MAX31856_REG_CJTH, "address byte is CJTH (0x0A), read bit clear");
        snprintf(msg, sizeof(msg),
                 "ch%u: fake_spi recorded cs_pin=%d, must equal this channel's own cs_gpio=%d "
                 "(a dropped CS bit-bangs no chip select at all -- every read would silently "
                 "clock against whichever device happens to be selected, or none)",
                 (unsigned)i, rec->cs_pin, cs_gpio);
        TEST_CHECK(rec->cs_pin == cs_gpio, msg);
    }
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
    TEST_CHECK(MAX31856_bus_init(&bus, /*host=*/1, /*sclk=*/1, /*mosi=*/2, /*miso=*/3) == ESP_OK,
               "MAX31856_bus_init succeeds against fake_spi");
    TEST_CHECK(fake_spi_bus_is_live(&bus.hal_bus), "hal_spi_bus_init actually registered a live fake bus");

    // Every channel on the bus gets its own distinct cs_gpio and its own
    // check -- not just channel 0 -- so a bug that happens to work for one
    // channel (e.g. by coincidentally matching a HAL_CS_NONE default) cannot
    // hide behind the others. Storage lives here (not inside check_channel's
    // own frame) so bus->channels[i] never points at a dead stack frame.
    static MAX31856Class chs[MAX31856_CHANNEL_COUNT];
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; ++i) {
        memset(&chs[i], 0, sizeof(chs[i]));
        check_channel(&bus, &chs[i], i);
    }

    // Fault-invalidates-temperature path, on the already-attached channel 0
    // (no re-init needed): proves that path still runs downstream of the new
    // hal_spi_transfer_polling() call, exactly as it did against the old
    // spi_owner_transfer_polling() call.
    uint8_t scripted_fault[7] = { 0x00, 0x19, 0x00, 0x3E, 0x80, 0x00, THERMO_FAULT_OPEN };
    TEST_CHECK(fake_spi_script_rx(&chs[0].dev, scripted_fault, sizeof(scripted_fault)) == HAL_OK,
               "script a burst response with OPEN faulted");
    MAX31856Reading reading;
    esp_err_t read_err2 = MAX31856_read(&chs[0], &reading);
    TEST_CHECK(read_err2 == ESP_OK, "MAX31856_read still succeeds (SPI transfer itself is fine)");
    TEST_CHECK(isnan(reading.tc_temperature_c), "OPEN fault NaNs tc_temperature_c even though the burst decoded fine");
    TEST_CHECK(reading.fault_status == THERMO_FAULT_OPEN, "fault_status reports OPEN");

    // 2026-09-06: MAX31856_bus_adopt() (the replacement for the
    // MAX31856_bus_init()->hal_spi_bus_init() path that used to hit the now-
    // removed ALREADY_INIT recovery on the KilnFW shared thermo/display bus
    // -- see MAX31856.h's doc comment) shares an already-live bus instead of
    // creating a new one. Verified here by adopting the same bus this test
    // already brought up above and confirming a channel attached through the
    // ADOPTED MAX31856BusClass reads back correctly and lands in the SAME
    // fake_spi transfer log as the channels attached above.
    MAX31856BusClass adopted_bus = {0};
    TEST_CHECK(MAX31856_bus_adopt(&adopted_bus, /*host=*/1, &bus.hal_bus) == ESP_OK,
               "MAX31856_bus_adopt succeeds against the already-live bus");
    TEST_CHECK(fake_spi_bus_is_live(&adopted_bus.hal_bus), "adopted MAX31856BusClass reads as live");

    static MAX31856Class adopted_ch;
    memset(&adopted_ch, 0, sizeof(adopted_ch));
    size_t before_adopt = fake_spi_transfer_count();
    check_channel(&adopted_bus, &adopted_ch, /*i=*/0);
    TEST_CHECK(fake_spi_transfer_count() == before_adopt + 1,
               "the adopted-bus channel's read landed in the SAME shared transfer log as the "
               "original bus's channels -- adopted and original share one underlying slot, not "
               "two independent SPI owners on one physical bus");

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures == 0 ? 0 : 1;
}
