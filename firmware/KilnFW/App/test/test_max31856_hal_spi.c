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
#include "control/thermo_channel_read.h"

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


// ---- AUDIT_THERMOCOUPLE MED-1 --------------------------------------------

static bool s_drdy_asserted;
static bool test_drdy_provider(uint8_t channel, bool *out_asserted, void *ctx)
{
    (void)channel; (void)ctx;
    *out_asserted = s_drdy_asserted;
    return true;
}

static MAX31856Reading good_reading(void)
{
    MAX31856Reading r;
    memset(&r, 0, sizeof(r));
    r.tc_temperature_c = 500.0f;
    r.cj_temperature_c = 25.0f;
    r.continuous = true;
    r.age_ms = 100u;
    return r;
}

static void test_reading_ok_staleness(void)
{
    MAX31856Reading r = good_reading();
    TEST_CHECK(thermo_reading_ok(&r), "fresh continuous reading is ok");
    r.age_ms = MAX31856_CONTROL_MAX_AGE_MS;
    TEST_CHECK(thermo_reading_ok(&r), "age exactly at the control bound is still ok");
    r.age_ms = MAX31856_CONTROL_MAX_AGE_MS + 1u;
    TEST_CHECK(!thermo_reading_ok(&r), "continuous reading older than the bound is NOT ok (frozen part)");
    r.age_ms = MAX31856_READING_AGE_UNKNOWN;
    TEST_CHECK(!thermo_reading_ok(&r), "continuous reading with no good conversion yet is NOT ok");
    r.continuous = false;
    TEST_CHECK(thermo_reading_ok(&r), "a normally-off/one-shot channel is not judged by age");
    r = good_reading(); r.spi_failed = true;
    TEST_CHECK(!thermo_reading_ok(&r), "spi_failed is not ok");
    r = good_reading(); r.tc_temperature_c = NAN;
    TEST_CHECK(!thermo_reading_ok(&r), "NaN is not ok");
    r = good_reading(); r.fault_status = THERMO_FAULT_OPEN;
    TEST_CHECK(!thermo_reading_ok(&r), "OPEN fault is not ok");
}

static void script_cfg_readback(MAX31856Class *ch, uint8_t cr0, uint8_t cr1, uint8_t mask)
{
    uint8_t rx[4] = { 0x00, cr0, cr1, mask };
    TEST_CHECK(fake_spi_script_rx(&ch->dev, rx, sizeof(rx)) == HAL_OK, "script CR0..MASK read-back");
}

static void script_temp_burst(MAX31856Class *ch)
{
    uint8_t rx[7] = { 0x00, 0x19, 0x00, 0x3E, 0x80, 0x00, 0x00 };
    TEST_CHECK(fake_spi_script_rx(&ch->dev, rx, sizeof(rx)) == HAL_OK, "script temperature burst");
}

static void test_live_check_reconfig(MAX31856BusClass *bus, MAX31856Class *ch)
{
    (void)bus;
    MAX31856Config cfg;
    MAX31856_config_default(&cfg);
    cfg.auto_convert = true;
    TEST_CHECK(MAX31856_configure(ch, &cfg) == ESP_OK, "configure channel (auto convert)");
    TEST_CHECK(MAX31856_set_fault_mask(ch, MAX31856_DEFAULT_FAULT_MASK) == ESP_OK, "set fault mask");
    TEST_CHECK(MAX31856_set_drdy_provider(bus, test_drdy_provider, NULL) == ESP_OK, "install DRDY provider");
    const uint8_t c0 = ch->cr0_shadow, c1 = ch->cr1_shadow, m = ch->mask_shadow;
    TEST_CHECK((c0 & MAX31856_CR0_CMODE) != 0, "shadow is auto-convert");

    size_t n = fake_spi_transfer_count();
    TEST_CHECK(MAX31856_live_check(ch, 1000u) == MAX31856_LIVE_NOT_DUE, "first call only arms the cadence");
    TEST_CHECK(MAX31856_live_check(ch, 1000u + MAX31856_LIVE_CHECK_INTERVAL_MS - 1u) == MAX31856_LIVE_NOT_DUE,
               "not due before the interval");
    TEST_CHECK(fake_spi_transfer_count() == n, "no SPI traffic while not due");

    uint32_t t = 1000u + MAX31856_LIVE_CHECK_INTERVAL_MS;
    script_cfg_readback(ch, c0, c1, m);
    TEST_CHECK(MAX31856_live_check(ch, t) == MAX31856_LIVE_OK, "matching read-back -> OK");
    TEST_CHECK(fake_spi_transfer_count() == n + 1, "exactly one read-back burst");
    const fake_spi_transfer_record_t *rec = fake_spi_transfer(n);
    TEST_CHECK(rec && rec->tx[0] == MAX31856_REG_CR0 && rec->tx_len == 4, "read-back burst starts at CR0, 3 registers");

    t += MAX31856_LIVE_CHECK_INTERVAL_MS;
    script_cfg_readback(ch, (uint8_t)(c0 | MAX31856_CR0_ONESHOT | MAX31856_CR0_FAULTCLR), c1, (uint8_t)(m ^ 0xC0u));
    TEST_CHECK(MAX31856_live_check(ch, t) == MAX31856_LIVE_OK,
               "self-clearing CR0 bits and reserved MASK bits are not a mismatch");

    /* A healthy channel with a fresh conversion on record before the reset. */
    {
        MAX31856Reading pre;
        s_drdy_asserted = true;
        script_temp_burst(ch);
        TEST_CHECK(MAX31856_read(ch, &pre) == ESP_OK && thermo_reading_ok(&pre) && ch->has_good_result,
                   "healthy channel reads ok with a good conversion on record");
    }

    /* Part power-on-reset: CR0 = 00h (normally off), CR1 = 03h, MASK = FFh. */
    t += MAX31856_LIVE_CHECK_INTERVAL_MS;
    n = fake_spi_transfer_count();
    /* LOW-5: the first mismatching read-back changes nothing (one corrupted SPI
     * read must not cost the invalid window); a second one confirms. */
    script_cfg_readback(ch, 0x00u, 0x03u, 0xFFu);
    TEST_CHECK(MAX31856_live_check(ch, t) == MAX31856_LIVE_SUSPECT, "first CR0 mismatch -> SUSPECT only");
    TEST_CHECK(fake_spi_transfer_count() == n + 1, "suspect: just the read-back, no writes");
    TEST_CHECK(ch->has_good_result, "suspect: good-result history kept");
    TEST_CHECK(ch->live_mismatch_count == 0u, "suspect: not counted yet");
    TEST_CHECK(MAX31856_live_check(ch, t + MAX31856_LIVE_RECHECK_MS - 1u) == MAX31856_LIVE_NOT_DUE,
               "re-check not due before MAX31856_LIVE_RECHECK_MS");
    /* A matching re-check clears the suspicion: a later single mismatch is suspect again. */
    script_cfg_readback(ch, c0, c1, m);
    TEST_CHECK(MAX31856_live_check(ch, t + MAX31856_LIVE_RECHECK_MS) == MAX31856_LIVE_OK, "re-check matches -> OK");
    TEST_CHECK(ch->has_good_result && ch->live_mismatch_count == 0u, "transient glitch changed nothing");
    t += MAX31856_LIVE_CHECK_INTERVAL_MS + MAX31856_LIVE_RECHECK_MS;
    script_cfg_readback(ch, 0x00u, 0x03u, 0xFFu);
    TEST_CHECK(MAX31856_live_check(ch, t) == MAX31856_LIVE_SUSPECT, "mismatch again -> SUSPECT (not stale-confirmed)");
    t += MAX31856_LIVE_RECHECK_MS;
    n = fake_spi_transfer_count();
    script_cfg_readback(ch, 0x00u, 0x03u, 0xFFu);
    TEST_CHECK(MAX31856_live_check(ch, t) == MAX31856_LIVE_RECONFIGURED, "second consecutive mismatch -> reconfigured");
    TEST_CHECK(ch->live_mismatch_count == 1u, "mismatch counted");
    TEST_CHECK(fake_spi_transfer_count() == n + 1 + 4, "one read-back plus stop/CR1/MASK/CR0 writes");
    const fake_spi_transfer_record_t *last = fake_spi_transfer(n + 4);
    TEST_CHECK(last && last->tx[0] == (0x80u | MAX31856_REG_CR0) && last->tx[1] == c0,
               "final write restores CR0 (CMODE back on) from the shadow");
    const fake_spi_transfer_record_t *w1 = fake_spi_transfer(n + 1);
    TEST_CHECK(w1 && w1->tx[0] == (0x80u | MAX31856_REG_CR0) && (w1->tx[1] & MAX31856_CR0_CMODE) == 0,
               "first write stops conversions");
    TEST_CHECK(ch->cr0_shadow == c0 && ch->cr1_shadow == c1 && ch->mask_shadow == m, "shadows unchanged");

    /* Invalid until a fresh post-reconfigure conversion. */
    MAX31856Reading r;
    s_drdy_asserted = false;
    script_temp_burst(ch);
    TEST_CHECK(MAX31856_read(ch, &r) == ESP_OK, "read after reconfigure");
    TEST_CHECK(r.age_ms == MAX31856_READING_AGE_UNKNOWN, "age UNKNOWN until a fresh conversion");
    TEST_CHECK(!thermo_reading_ok(&r), "reading is NOT ok before a fresh post-reconfigure conversion");
    s_drdy_asserted = true;
    script_temp_burst(ch);
    TEST_CHECK(MAX31856_read(ch, &r) == ESP_OK, "read with DRDY asserted");
    TEST_CHECK(thermo_reading_ok(&r), "reading is ok again after a fresh conversion");

    /* CR1 and MASK mismatches alone also trigger it. */
    t += MAX31856_LIVE_CHECK_INTERVAL_MS;
    script_cfg_readback(ch, c0, (uint8_t)(c1 ^ 0x10u), m);
    TEST_CHECK(MAX31856_live_check(ch, t) == MAX31856_LIVE_SUSPECT, "CR1 mismatch -> suspect");
    t += MAX31856_LIVE_RECHECK_MS;
    script_cfg_readback(ch, c0, (uint8_t)(c1 ^ 0x10u), m);
    TEST_CHECK(MAX31856_live_check(ch, t) == MAX31856_LIVE_RECONFIGURED, "CR1 mismatch confirmed -> reconfigured");
    t += MAX31856_LIVE_CHECK_INTERVAL_MS;
    script_cfg_readback(ch, c0, c1, (uint8_t)(m ^ 0x01u));
    TEST_CHECK(MAX31856_live_check(ch, t) == MAX31856_LIVE_SUSPECT, "MASK mismatch -> suspect");
    t += MAX31856_LIVE_RECHECK_MS;
    script_cfg_readback(ch, c0, c1, (uint8_t)(m ^ 0x01u));
    TEST_CHECK(MAX31856_live_check(ch, t) == MAX31856_LIVE_RECONFIGURED, "MASK mismatch confirmed -> reconfigured");
}

static void test_zone_helpers(void)
{
    MAX31856Reading rd[3];
    memset(rd, 0, sizeof(rd));
    for (int i = 0; i < 3; i++) {
        rd[i].channel = (uint8_t)i;
        rd[i].tc_temperature_c = 100.0f * (float)(i + 1);
    }
    bool v = false;
    float c = thermo_zone_raw_from_readings(rd, 3, 0x01u, &v);
    TEST_CHECK(v && c == 100.0f, "identity mask 0 -> channel 0");
    c = thermo_zone_raw_from_readings(rd, 3, 0x06u, &v);
    TEST_CHECK(v && c == 250.0f, "mask 0b110 -> mean of channels 1,2 (LOW-1)");
    c = thermo_zone_raw_from_readings(rd, 3, 0x04u, &v);
    TEST_CHECK(v && c == 300.0f, "mask on channel 2 reads channel 2, not channel 0");
    rd[2].spi_failed = true;
    c = thermo_zone_raw_from_readings(rd, 3, 0x06u, &v);
    TEST_CHECK(v && c == 200.0f, "spi-failed masked channel dropped from the mean");
    c = thermo_zone_raw_from_readings(rd, 3, 0x04u, &v);
    TEST_CHECK(!v && isnan(c), "only masked channel failed -> invalid");
    c = thermo_zone_raw_from_readings(rd, 3, 0x00u, &v);
    TEST_CHECK(!v && isnan(c), "empty mask -> invalid");
    c = thermo_zone_raw_from_readings(rd, 3, 0x08u, &v);
    TEST_CHECK(!v, "mask names an absent channel -> invalid");

    TEST_CHECK(thermo_channel_cal_applies(0, 0x01u), "identity mask: calibrate");
    TEST_CHECK(thermo_channel_cal_applies(2, 0x04u), "identity mask ch2: calibrate");
    TEST_CHECK(!thermo_channel_cal_applies(0, 0x03u), "combined mask: show raw (LOW-2)");
    TEST_CHECK(!thermo_channel_cal_applies(0, 0x02u), "remapped mask: show raw (LOW-2)");
    TEST_CHECK(!thermo_channel_cal_applies(0, 0x00u), "empty mask: show raw");
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

    test_reading_ok_staleness();
    test_live_check_reconfig(&bus, &chs[1]);
    test_zone_helpers();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures == 0 ? 0 : 1;
}
