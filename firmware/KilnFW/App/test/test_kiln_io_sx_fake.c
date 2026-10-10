// Host test (campaign 7, docs/audits/HOST_TEST_COVERAGE_GAPS_2026-10-09.md
// gap 7): the REAL kiln_io.c on top of the REAL SX1509.c, with a register-level
// fake SX1509 behind i2c_master_transmit()/transmit_receive(). The existing
// test_kiln_io_owner.c stubs every SX1509_* call, so it cannot see what the
// chip actually holds; here every assertion about "relay state" is read from
// the fake chip's own registers (dir/data latch), not from the module's view.
//
// Hardware fact respected: the relay pin numbering is only what settings.h
// and kiln_io.c's logical->pin table say (logical 2<->4 swapped); this file
// makes no claim that any relay bit is the K4/heat-enable line.
//
// Not covered, deliberately: the owner task's queue/lock (the FreeRTOS stubs
// never deliver, see test_kiln_io_owner.c's header), so "concurrent commands
// do not interleave" is checked only at the transfer level: each 16-bit
// register pair write must be ONE bus transfer.
//
// K7-01..K7-04 (docs/audits/HOST_TEST_CAMPAIGN_FINDINGS_2026-10-09.md) are
// asserted below and fixed in kiln_io.c.
#include <stdint.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;
#include "test_common.h"
#include "fake_time.h"
#include "kiln_io.h"
#define SX1509_OWNER_BUILD
#include "SX1509_internal.h"
#include "settings.h"

/* ---- fake chip ---------------------------------------------------------- */
static struct {
    uint8_t reg[128];
    uint16_t ext_pins;        /* what the outside world drives on input pins */
    int fail_writes;          /* next N write transfers return fail_err, no effect */
    esp_err_t fail_err;
    int fail_forever;         /* every transfer fails */
    int die_after;            /* >=0: every transfer after this many writes fails */
    int land_then_fail;       /* write lands, then returns ESP_ERR_TIMEOUT */
    int partial_bytes;        /* >=0: apply only that many data bytes then fail */
    uint8_t stuck_set[128];   /* bits forced to 1 on every write to reg */
    int write_transfers;
    int reset_writes;
    int pair_split_violation; /* RegData/RegDir written as <2 data bytes */
} F;

static void chip_por(void)
{
    memset(F.reg, 0, sizeof(F.reg));
    F.reg[SX1509_REG_DIR_B] = F.reg[SX1509_REG_DIR_A] = 0xFF;
    F.reg[SX1509_REG_DATA_B] = F.reg[SX1509_REG_DATA_A] = 0xFF;
}
static void fake_reset(void)
{
    memset(&F, 0, sizeof(F));
    F.partial_bytes = -1;
    F.die_after = -1;
    F.fail_err = ESP_FAIL;
    chip_por();
}
static uint16_t r16(uint8_t b) { return (uint16_t)((F.reg[b] << 8) | F.reg[b + 1]); }
static uint16_t chip_dir(void) { return r16(SX1509_REG_DIR_B); }
static uint16_t chip_latch(void) { return r16(SX1509_REG_DATA_B); }
/* A pin is driven high (coil energised) only if it is an output with latch 1. */
static int chip_pin_driven_high(unsigned pin)
{
    return !(chip_dir() & (1u << pin)) && (chip_latch() & (1u << pin));
}
static uint16_t chip_pins(void)
{
    uint16_t d = chip_dir(), l = chip_latch();
    return (uint16_t)((l & ~d) | (F.ext_pins & d));
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t b, const i2c_device_config_t *c, i2c_master_dev_handle_t *o)
{ (void)b; (void)c; *o = (i2c_master_dev_handle_t)1; return ESP_OK; }
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t d) { (void)d; return ESP_OK; }
esp_err_t i2c_master_probe(i2c_master_bus_handle_t b, uint16_t a, int t) { (void)b; (void)a; (void)t; return ESP_OK; }
esp_err_t i2c_master_receive(i2c_master_dev_handle_t d, uint8_t *rx, size_t n, int t)
{ (void)d; (void)rx; (void)n; (void)t; return ESP_FAIL; }
esp_err_t i2c_owner_transfer(i2c_owner_t *o, i2c_master_dev_handle_t d, const uint8_t *tx, size_t tl, uint8_t *rx, size_t rl, int t)
{ (void)o; (void)d; (void)tx; (void)tl; (void)rx; (void)rl; (void)t; return ESP_FAIL; }
esp_err_t i2c_owner_init(i2c_owner_t *o, i2c_master_bus_handle_t b, int a, int c, int d, int e)
{ (void)o; (void)b; (void)a; (void)c; (void)d; (void)e; return ESP_FAIL; }
esp_err_t i2c_owner_deinit(i2c_owner_t *o) { (void)o; return ESP_OK; }

static void apply_write(uint8_t reg, const uint8_t *v, size_t n)
{
    static uint8_t seq;
    for (size_t i = 0; i < n; i++) {
        unsigned r = (unsigned)reg + (unsigned)i;
        if (r > 0x7F) break;
        if (r == SX1509_REG_RESET) {
            F.reset_writes++;
            if (v[i] == 0x12) seq = 1;
            else if (v[i] == 0x34 && seq == 1) { chip_por(); seq = 0; }
            else seq = 0;
            continue;
        }
        F.reg[r] = (uint8_t)(v[i] | F.stuck_set[r]);
    }
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t d, const uint8_t *tx, size_t n, int t)
{
    (void)d; (void)t;
    if (F.fail_forever) return F.fail_err;
    if (F.die_after >= 0 && F.write_transfers >= F.die_after) return F.fail_err;
    F.write_transfers++;
    if (n >= 1 && (tx[0] == SX1509_REG_DATA_B || tx[0] == SX1509_REG_DIR_B) && n - 1 < 2) F.pair_split_violation++;
    if (F.fail_writes > 0) { F.fail_writes--; return F.fail_err; }
    if (F.partial_bytes >= 0) {
        size_t k = (size_t)F.partial_bytes < n - 1 ? (size_t)F.partial_bytes : n - 1;
        apply_write(tx[0], tx + 1, k);
        return ESP_FAIL;
    }
    apply_write(tx[0], tx + 1, n - 1);
    if (F.land_then_fail) return ESP_ERR_TIMEOUT;
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t d, const uint8_t *tx, size_t tl, uint8_t *rx, size_t rl, int t)
{
    (void)d; (void)t; (void)tl;
    if (F.fail_forever) return F.fail_err;
    if (F.die_after >= 0 && F.write_transfers >= F.die_after) return F.fail_err;
    for (size_t i = 0; i < rl; i++) {
        unsigned r = (unsigned)tx[0] + (unsigned)i;
        if (r == SX1509_REG_DATA_B) rx[i] = (uint8_t)(chip_pins() >> 8);
        else if (r == SX1509_REG_DATA_A) rx[i] = (uint8_t)(chip_pins() & 0xFF);
        else rx[i] = r < 128 ? F.reg[r] : 0;
    }
    return ESP_OK;
}

/* ---- fixtures ----------------------------------------------------------- */
static SX1509Class g_exp;
static kiln_io_t g_io;

static void setup(void)
{
    fake_reset();
    fake_time_reset_all();
    memset(&g_exp, 0, sizeof(g_exp));
    g_exp.dev = (i2c_master_dev_handle_t)1;
    g_exp.addr = SX1509_ADDR_00;
    g_exp.irq_gpio = -1;
    g_exp.reset_gpio = -1;
    g_exp.dir_shadow = SX1509_DIR_POWER_ON_STATE;
    g_exp.data_shadow = SX1509_DATA_POWER_ON_STATE;
    memset(&g_io, 0, sizeof(g_io));
}
static void setup_ready(void)
{
    setup();
    esp_err_t e = kiln_io_init(&g_io, &g_exp);
    TEST_CHECK(e == ESP_OK, "kiln_io_init against the fake chip succeeds");
}
/* logical relay n (1-based) -> physical pin, per kiln_io.c's own table */
static unsigned phys_pin(unsigned logical)
{
    static const unsigned t[4] = { SX1509_RELAY1_PIN, SX1509_RELAY2_PIN, SX1509_RELAY3_PIN, SX1509_RELAY4_PIN };
    static const unsigned swap[4] = { 0, 3, 2, 1 };
    return t[swap[logical - 1]];
}
static uint8_t chip_relays_logical(void)
{
    uint8_t m = 0;
    for (unsigned n = 1; n <= 4; n++) if (chip_pin_driven_high(phys_pin(n))) m |= (uint8_t)(1u << (n - 1));
    return m;
}

/* ---- tests -------------------------------------------------------------- */
static void test_init_leaves_relays_off_and_outputs_driven(void)
{
    setup_ready();
    TEST_CHECK(chip_relays_logical() == 0, "after init no relay pin is driven high");
    for (unsigned p = 0; p < 4; p++)
        TEST_CHECK(!(chip_dir() & (1u << p)), "init makes every relay pin an OUTPUT (so OFF is actively driven)");
    TEST_CHECK(g_io.relay_shadow == 0 && g_io.initialized, "module shadow 0 and initialized");
}

static void test_init_dying_midway_never_energises_a_relay_and_is_not_ready(void)
{
    for (int n = 0; n < 30; n++) {
        setup();
        F.die_after = n;
        esp_err_t e = kiln_io_init(&g_io, &g_exp);
        TEST_CHECK(chip_relays_logical() == 0, "a bus that dies mid-init never leaves a relay energised");
        if (e != ESP_OK) {
            TEST_CHECK(!g_io.initialized, "failed init is not 'initialized'");
            F.die_after = -1;
            TEST_CHECK(kiln_io_set_relay(&g_io, 1, true) == ESP_ERR_INVALID_STATE, "relay ON refused on a half-initialised board");
            TEST_CHECK(chip_relays_logical() == 0, "and nothing was driven");
        }
    }
}

static void test_set_relay_matches_chip_for_every_relay(void)
{
    for (unsigned n = 1; n <= 4; n++) {
        setup_ready();
        esp_err_t e = kiln_io_set_relay(&g_io, (uint8_t)n, true);
        TEST_CHECK(e == ESP_OK, "set_relay ON ok");
        TEST_CHECK(chip_relays_logical() == (uint8_t)(1u << (n - 1)), "exactly the commanded relay is energised on the chip");
        TEST_CHECK(g_io.relay_shadow == chip_relays_logical(), "module relay_shadow == chip state");
        TEST_CHECK(chip_dir() == g_exp.dir_shadow, "driver dir shadow == chip RegDir");
        e = kiln_io_set_relay(&g_io, (uint8_t)n, false);
        TEST_CHECK(e == ESP_OK && chip_relays_logical() == 0 && g_io.relay_shadow == 0, "OFF drops it again");
    }
}

static void test_mask_write_preserves_other_relays_and_lcd_lines(void)
{
    setup_ready();
    uint16_t before_hi = (uint16_t)(chip_latch() & 0xFFF0u);
    TEST_CHECK(kiln_io_set_relay_mask(&g_io, 0x05, 0x05) == ESP_OK, "mask write 1+3 on");
    TEST_CHECK(kiln_io_set_relay_mask(&g_io, 0x02, 0x02) == ESP_OK, "mask write relay 2 on");
    TEST_CHECK(chip_relays_logical() == 0x07, "all three on, none dropped by a later masked write");
    TEST_CHECK((chip_latch() & 0xFFF0u) == before_hi, "non-relay latch bits (LCD D/C, reset, IO_2) untouched");
    TEST_CHECK(kiln_io_set_relay_mask(&g_io, 0x10, 0x10) == ESP_OK && chip_relays_logical() == 0x07,
               "bits above the 4 relay bits are masked off, not written");
    TEST_CHECK(kiln_io_set_relay_mask(&g_io, 0x00, 0xFF) == ESP_OK && chip_relays_logical() == 0x07,
               "empty mask is a no-op");
}

static void test_pair_write_is_a_single_bus_transfer(void)
{
    setup_ready();
    F.pair_split_violation = 0;
    F.write_transfers = 0;
    (void)kiln_io_set_relay(&g_io, 1, true);
    TEST_CHECK(F.pair_split_violation == 0, "RegData is always written as a 2-byte burst (no bank A/B split across transfers)");
    TEST_CHECK(F.write_transfers == 1, "a relay write is exactly one write transfer when the chip behaves");
}

static void test_nack_is_reported_never_success(void)
{
    setup_ready();
    F.fail_forever = 1;
    esp_err_t e = kiln_io_set_relay(&g_io, 1, true);
    TEST_CHECK(e != ESP_OK, "NACK on every transfer -> failure reported");
    TEST_CHECK(g_io.relay_shadow == 0, "module does not claim the relay is on");
    TEST_CHECK(g_io.last_i2c_failed, "last_i2c_failed latched");
    kiln_io_state_t st;
    TEST_CHECK(kiln_io_read(&g_io, &st) != ESP_OK && (st.flags & KILN_IO_FLAG_I2C_FAILED), "read on dead bus fails and flags it");
    F.fail_forever = 0;
    TEST_CHECK(chip_relays_logical() == 0, "chip unchanged");
}

static void test_timeout_error_code_propagates(void)
{
    setup_ready();
    F.fail_writes = 1000;
    F.fail_err = ESP_ERR_TIMEOUT;
    F.write_transfers = 0;
    TEST_CHECK(kiln_io_set_relay(&g_io, 3, true) == ESP_ERR_TIMEOUT, "timeout surfaces as ESP_ERR_TIMEOUT");
    TEST_CHECK(F.write_transfers >= 1 && F.write_transfers <= 30, "bounded retries (no unbounded loop)");
    TEST_CHECK(g_io.relay_shadow == 0 && chip_relays_logical() == 0, "nothing energised, nothing claimed");
}

static void test_transient_nack_retried_to_success(void)
{
    setup_ready();
    F.fail_writes = 2;
    TEST_CHECK(kiln_io_set_relay(&g_io, 2, true) == ESP_OK, "2 transient NACKs then success");
    TEST_CHECK(chip_relays_logical() == 0x02 && g_io.relay_shadow == 0x02, "end state consistent");
}

static void test_partial_write_before_bank_a_never_energises(void)
{
    setup_ready();
    F.partial_bytes = 1; /* only the bank B byte lands, then error; relays live in bank A */
    TEST_CHECK(kiln_io_set_relay(&g_io, 1, true) != ESP_OK, "partial write reported as failure");
    TEST_CHECK(chip_relays_logical() == 0 && g_io.relay_shadow == 0, "relay neither energised nor claimed on");
}

/* K7-01: a write LANDS but the transfer reports a timeout. Error is returned,
 * and the shadow must follow the chip (relay energised), not claim OFF. */
static void test_k7_01_landed_write_with_error_resyncs_from_chip(void)
{
    setup_ready();
    F.land_then_fail = 1;
    TEST_CHECK(kiln_io_set_relay(&g_io, 1, true) != ESP_OK, "K7-01: error still reported");
    F.land_then_fail = 0;
    TEST_CHECK(chip_relays_logical() == 0x01, "K7-01: (precondition) the write did land, coil energised");
    TEST_CHECK(g_io.relay_shadow == chip_relays_logical(), "K7-01: relay_shadow follows the chip, never OFF while energised");
    TEST_CHECK(kiln_io_set_relay(&g_io, 1, false) == ESP_OK && chip_relays_logical() == 0 && g_io.relay_shadow == 0,
               "K7-01: a later OFF recovers");
}

/* K7-02: a stuck-high latch bit makes the OFF fail; the shadow must read the
 * chip and show ON, and the error stays reported. */
static void test_k7_02_stuck_high_off_failure_shadow_shows_on(void)
{
    setup_ready();
    TEST_CHECK(kiln_io_set_relay(&g_io, 1, true) == ESP_OK, "relay 1 on");
    F.stuck_set[SX1509_REG_DATA_A] = (uint8_t)(1u << phys_pin(1)); /* latch bit stuck high */
    esp_err_t e = kiln_io_set_relay(&g_io, 1, false);
    TEST_CHECK(e != ESP_OK, "K7-02: failed OFF is reported");
    TEST_CHECK(chip_relays_logical() == 0x01, "K7-02: (precondition) coil still energised");
    TEST_CHECK(g_io.relay_shadow == 0x01, "K7-02: relay_shadow reports the true ON state");
}

/* Unreadable chip after a failed write: never guess OFF. */
static void test_k7_unknown_chip_state_does_not_claim_off(void)
{
    setup_ready();
    TEST_CHECK(kiln_io_set_relay(&g_io, 2, true) == ESP_OK, "relay 2 on");
    F.fail_forever = 1;
    TEST_CHECK(kiln_io_set_relay(&g_io, 2, false) != ESP_OK, "OFF on dead bus fails");
    TEST_CHECK(g_io.relay_shadow == 0x02, "unreadable chip: shadow keeps the last verified ON, never flips to OFF");
    F.fail_forever = 0;
}

/* K7-03: after an expander reset the relay pins are inputs. A relay ON must
 * never report success with nothing driven, and the owner's re-init
 * (kiln_io_reinit) must restore outputs, all latched OFF, verified. */
static void test_k7_03_on_after_sx_reset_never_ok_with_nothing_driven(void)
{
    setup_ready();
    TEST_CHECK(SX1509_reset(&g_exp, false) == ESP_OK, "soft reset");
    esp_err_t e = kiln_io_set_relay(&g_io, 1, true);
    if (e == ESP_OK) TEST_CHECK(chip_relays_logical() & 0x01, "K7-03: OK reported => coil really energised");
    else TEST_CHECK(chip_relays_logical() == 0 && g_io.relay_shadow == 0, "K7-03: refused, nothing driven, no ON claim");

    TEST_CHECK(kiln_io_reinit(&g_io) == ESP_OK, "K7-03: reinit after reset ok");
    TEST_CHECK((chip_dir() & 0x000F) == 0 && chip_relays_logical() == 0, "K7-03: relay pins outputs, all latched OFF");
    TEST_CHECK(kiln_io_set_relay(&g_io, 1, true) == ESP_OK, "ON ok after reinit");
    TEST_CHECK(chip_relays_logical() == 0x01 && g_io.relay_shadow == 0x01, "and the coil is really energised");
}

/* Relay latch stuck high: the latch write is unverifiable while pins are still
 * inputs, so only the post-config read-back can notice a pin driven high. */
static void test_k7_03_reinit_readback_catches_stuck_high_latch(void)
{
    setup_ready();
    (void)SX1509_reset(&g_exp, false);
    F.stuck_set[SX1509_REG_DATA_A] = 0x01;
    TEST_CHECK(kiln_io_reinit(&g_io) == ESP_ERR_INVALID_RESPONSE, "reinit read-back rejects a relay pin reading high");
    TEST_CHECK(!g_io.initialized, "board not initialised");
    TEST_CHECK(kiln_io_set_relay(&g_io, 2, true) != ESP_OK, "relay ON refused");
}
static void test_k7_03_reinit_failure_blocks_relay_on(void)
{
    setup_ready();
    (void)SX1509_reset(&g_exp, false);
    F.stuck_set[SX1509_REG_DIR_A] = 0x01; /* a relay pin can never become an output */
    TEST_CHECK(kiln_io_reinit(&g_io) != ESP_OK, "reinit fails when the direction read-back is wrong");
    TEST_CHECK(!g_io.initialized, "board is not initialised");
    TEST_CHECK(kiln_io_set_relay(&g_io, 1, true) != ESP_OK && chip_relays_logical() == 0, "relay ON refused, nothing driven");
}
static void test_all_relays_off_failfast_and_failsafe(void)
{
    setup_ready();
    (void)kiln_io_set_relay_mask(&g_io, 0x0F, 0x0F);
    TEST_CHECK(chip_relays_logical() == 0x0F, "all on");
    TEST_CHECK(kiln_io_all_relays_off(&g_io) == ESP_OK && chip_relays_logical() == 0 && g_io.relay_shadow == 0, "all off lands");
    (void)kiln_io_set_relay_mask(&g_io, 0x0F, 0x0F);
    F.fail_forever = 1;
    TEST_CHECK(kiln_io_all_relays_off(&g_io) != ESP_OK, "all-off on dead bus reported as failure, not success");
    F.fail_forever = 0;
    g_io.initialized = false;
    TEST_CHECK(kiln_io_all_relays_off(&g_io) == ESP_OK && chip_relays_logical() == 0, "all-off is allowed before init completes (fail-safe path)");
    TEST_CHECK(kiln_io_set_relay(&g_io, 1, true) == ESP_ERR_INVALID_STATE, "but ON is refused until initialised");
}

static void test_arg_validation(void)
{
    setup_ready();
    int w = F.write_transfers;
    TEST_CHECK(kiln_io_set_relay(&g_io, 0, true) == ESP_ERR_INVALID_ARG, "relay 0");
    TEST_CHECK(kiln_io_set_relay(&g_io, 5, true) == ESP_ERR_INVALID_ARG, "relay 5");
    TEST_CHECK(kiln_io_set_relay(NULL, 1, true) == ESP_ERR_INVALID_ARG, "NULL io");
    TEST_CHECK(kiln_io_set_io_dir(&g_io, 0, true, false) == ESP_ERR_INVALID_ARG, "io 0");
    TEST_CHECK(kiln_io_set_io_dir(&g_io, 8, true, false) == ESP_ERR_INVALID_ARG, "io 8");
    TEST_CHECK(F.write_transfers == w, "bad args never reach the bus");
}

static void test_set_io_dir_never_touches_relay_pins_and_matches_chip(void)
{
    setup_ready();
    (void)kiln_io_set_relay(&g_io, 1, true);
    for (uint8_t i = 1; i <= KILN_IO_DIGITAL_COUNT; i++) {
        TEST_CHECK(kiln_io_set_io_dir(&g_io, i, false, false) == ESP_OK, "to output");
        TEST_CHECK(chip_dir() == g_exp.dir_shadow, "dir shadow == chip");
        TEST_CHECK(chip_relays_logical() == 0x01, "relay 1 still energised, no other relay disturbed");
        TEST_CHECK(kiln_io_set_io_dir(&g_io, i, true, true) == ESP_OK, "back to input w/ pullup");
        TEST_CHECK(chip_dir() == g_exp.dir_shadow && (chip_dir() & 0x000F) == 0, "relay pins stay outputs");
    }
}

static void test_set_io_dir_failure_is_reported(void)
{
    setup_ready();
    F.fail_forever = 1;
    TEST_CHECK(kiln_io_set_io_dir(&g_io, 3, false, false) != ESP_OK, "failed dir write reported");
    TEST_CHECK(g_io.last_i2c_failed, "tracked");
    F.fail_forever = 0;
    TEST_CHECK(chip_relays_logical() == 0, "relays unaffected");
}

static void test_dir_readback_mismatch_is_failure(void)
{
    setup_ready();
    F.stuck_set[SX1509_REG_DIR_A] = 0x40; /* pin 6 (IO_3) cannot be made an output */
    TEST_CHECK(kiln_io_set_io_dir(&g_io, 3, false, false) == ESP_ERR_INVALID_RESPONSE, "dir read-back mismatch reported");
}

static void test_por_then_off_commands_are_honest(void)
{
    setup_ready();
    (void)kiln_io_set_relay(&g_io, 1, true);
    chip_por(); /* expander browns out; driver unaware */
    TEST_CHECK(chip_relays_logical() == 0, "a POR de-energises the coil (inputs)");
    esp_err_t e = kiln_io_set_relay(&g_io, 2, true);
    if (e == ESP_OK) TEST_CHECK(chip_relays_logical() & 0x02, "if success is reported the coil must really be energised");
}

/* K7-04: chip POR behind the driver's back; all_relays_off repairs direction. */
static void test_k7_04_all_relays_off_after_por_repairs_direction(void)
{
    setup_ready();
    (void)kiln_io_set_relay(&g_io, 1, true);
    chip_por();
    TEST_CHECK(kiln_io_all_relays_off(&g_io) == ESP_OK, "K7-04: all-off reports success after repairing");
    TEST_CHECK((chip_dir() & 0x000F) == 0, "K7-04: relay pins are outputs again");
    TEST_CHECK(chip_relays_logical() == 0 && g_io.relay_shadow == 0, "K7-04: relays OFF");
    TEST_CHECK(kiln_io_set_relay(&g_io, 3, true) == ESP_OK && chip_relays_logical() == 0x04, "board usable afterward");
}

static void test_k7_04_unrepairable_por_reports_failure(void)
{
    setup_ready();
    (void)kiln_io_set_relay(&g_io, 1, true);
    chip_por();
    F.stuck_set[SX1509_REG_DIR_A] = 0x01; /* repair impossible */
    TEST_CHECK(kiln_io_all_relays_off(&g_io) != ESP_OK, "K7-04: failure stays honest when repair is impossible");
    TEST_CHECK(chip_relays_logical() == 0, "no coil energised");
    TEST_CHECK(kiln_io_set_relay(&g_io, 1, true) != ESP_OK && chip_relays_logical() == 0, "ON refused afterward");
}

static void test_sx_reset_drops_relays_and_resyncs_shadows(void)
{
    setup_ready();
    (void)kiln_io_set_relay_mask(&g_io, 0x0F, 0x0F);
    F.reset_writes = 0;
    TEST_CHECK(SX1509_reset(&g_exp, false) == ESP_OK, "soft reset ok");
    TEST_CHECK(F.reset_writes == 2, "reset is two separate writes to RegReset (0x12 then 0x34)");
    TEST_CHECK(chip_relays_logical() == 0, "after reset no relay energised (safe-off)");
    TEST_CHECK(g_exp.dir_shadow == SX1509_DIR_POWER_ON_STATE && g_exp.dir_shadow == chip_dir(), "driver shadows follow the chip's POR values");
    TEST_CHECK((F.reg[SX1509_REG_MISC] & SX1509_MISC_NO_AUTOCLEAR) != 0, "RegMisc auto-clear-off restored after reset");
}

static void test_sx_reset_failure_is_reported(void)
{
    setup_ready();
    F.fail_forever = 1;
    TEST_CHECK(SX1509_reset(&g_exp, false) != ESP_OK, "reset on dead bus fails");
    F.fail_forever = 0;
    TEST_CHECK(SX1509_reset(&g_exp, true) == ESP_ERR_INVALID_STATE, "hard reset without a reset GPIO is refused");
}


int main(void)
{
    TEST_SECTION("kiln_io + SX1509 against a fake I2C expander");
    test_init_leaves_relays_off_and_outputs_driven();
    test_init_dying_midway_never_energises_a_relay_and_is_not_ready();
    test_set_relay_matches_chip_for_every_relay();
    test_mask_write_preserves_other_relays_and_lcd_lines();
    test_pair_write_is_a_single_bus_transfer();
    test_nack_is_reported_never_success();
    test_timeout_error_code_propagates();
    test_transient_nack_retried_to_success();
    test_partial_write_before_bank_a_never_energises();
    test_k7_01_landed_write_with_error_resyncs_from_chip();
    test_k7_02_stuck_high_off_failure_shadow_shows_on();
    test_k7_unknown_chip_state_does_not_claim_off();
    test_k7_03_on_after_sx_reset_never_ok_with_nothing_driven();
    test_k7_03_reinit_failure_blocks_relay_on();
    test_k7_03_reinit_readback_catches_stuck_high_latch();
    test_k7_04_all_relays_off_after_por_repairs_direction();
    test_k7_04_unrepairable_por_reports_failure();
    test_all_relays_off_failfast_and_failsafe();
    test_arg_validation();
    test_set_io_dir_never_touches_relay_pins_and_matches_chip();
    test_set_io_dir_failure_is_reported();
    test_dir_readback_mismatch_is_failure();
    test_por_then_off_commands_are_honest();
    test_sx_reset_drops_relays_and_resyncs_shadows();
    test_sx_reset_failure_is_reported();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures > 0 ? 1 : 0;
}
