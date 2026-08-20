// Host tests for max31856_regs.c: register read/write correctness,
// auto-increment addressing, fault-bit computation against threshold
// crossings, and the corruption knobs (PLAN.md section 13.1).
#include <string.h>

#include "test_common.h"
#include "../src/sim/max31856_regs.h"

/* Independent (test-side) encode of a threshold register pair, mirroring
 * the datasheet's 0.0625 degC/LSB int16 format -- deliberately not calling
 * into max31856_regs.c's own encoder, so a bug shared by both sides would
 * not hide behind agreement. */
static void encode_threshold(float c, uint8_t *hi, uint8_t *lo)
{
    int16_t raw = (int16_t)(c / 0.0625f);
    *hi = (uint8_t)(((uint16_t)raw >> 8) & 0xFFu);
    *lo = (uint8_t)((uint16_t)raw & 0xFFu);
}

static void test_write_read_verbatim(void)
{
    TEST_SECTION("max31856_regs -- write/read verbatim (configuration readback)");

    max31856_channel_t ch;
    max31856_regs_init(&ch, 1);

    /* Power-on defaults per datasheet. */
    TEST_CHECK(ch.regs[MAX31856_REG_CR0] == 0x00u, "power-on CR0 == 00h");
    TEST_CHECK(ch.regs[MAX31856_REG_CR1] == 0x03u, "power-on CR1 == 03h");
    TEST_CHECK(ch.regs[MAX31856_REG_MASK] == 0xFFu, "power-on MASK == FFh");

    /* CR1 (TC type + AVGSEL) reads back exactly what was written -- this is
     * the "assert the DUT configured TC type correctly" test point PLAN.md
     * 3.2 calls out. */
    max31856_regs_write_burst(&ch, MAX31856_REG_CR1, (const uint8_t[]){0x23u}, 1);
    uint8_t readback = 0;
    max31856_regs_read_burst(&ch, MAX31856_REG_CR1, &readback, 1);
    TEST_CHECK(readback == 0x23u, "CR1 write then read returns exactly what was written");

    /* MASK likewise. */
    max31856_regs_write_burst(&ch, MAX31856_REG_MASK, (const uint8_t[]){0xC0u | MAX31856_MASK_TCLOW}, 1);
    max31856_regs_read_burst(&ch, MAX31856_REG_MASK, &readback, 1);
    TEST_CHECK(readback == (uint8_t)(0xC0u | MAX31856_MASK_TCLOW), "MASK write then read returns exactly what was written");
}

static void test_auto_increment(void)
{
    TEST_SECTION("max31856_regs -- auto-increment addressing");

    max31856_channel_t ch;
    max31856_regs_init(&ch, 2);

    /* One auto-incrementing write burst across CJHF, CJLF, LTHFTH, LTHFTL,
     * LTLFTH, LTLFTL (6 registers, one transaction), then one auto-
     * incrementing read burst covering the same span. */
    const uint8_t data[6] = {0x0Au, 0xF6u, 0x01u, 0x40u, 0x00u, 0xC0u};
    max31856_regs_write_burst(&ch, MAX31856_REG_CJHF, data, sizeof(data));

    uint8_t out[6] = {0};
    max31856_regs_read_burst(&ch, MAX31856_REG_CJHF, out, sizeof(out));
    TEST_CHECK(memcmp(data, out, sizeof(data)) == 0, "6-register auto-increment burst round-trips exactly");

    /* Address wraps at MAX31856_REG_COUNT (matches the real part reading
     * past SR back to CR0). Write a marker at CR0, then read starting near
     * the end of the map across the wrap. */
    max31856_regs_write_burst(&ch, MAX31856_REG_CR0, (const uint8_t[]){0x81u}, 1);
    uint8_t wrap_out[3] = {0};
    /* SR (0x0F) is read-only/live, so read LTCBM(0x0D), LTCBL(0x0E), SR(0x0F) then wrap to CR0(0x00). */
    uint8_t wrap4[4] = {0};
    max31856_regs_read_burst(&ch, MAX31856_REG_LTCBL, wrap4, 4); /* LTCBL, SR, CR0(wrap), CR1 */
    TEST_CHECK(wrap4[2] == 0x81u, "auto-increment address wraps from SR (0x0F) back to CR0 (0x00)");
    (void)wrap_out;
}

static void test_fault_bits_from_thresholds(void)
{
    TEST_SECTION("max31856_regs -- SR fault bits computed from simulated temps vs thresholds");

    max31856_channel_t ch;
    max31856_regs_init(&ch, 3);

    uint8_t hi, lo;
    encode_threshold(100.0f, &hi, &lo);
    max31856_regs_write_burst(&ch, MAX31856_REG_LTHFTH, (const uint8_t[]){hi, lo}, 2);
    encode_threshold(0.0f, &hi, &lo);
    max31856_regs_write_burst(&ch, MAX31856_REG_LTLFTH, (const uint8_t[]){hi, lo}, 2);
    max31856_regs_write_burst(&ch, MAX31856_REG_CJHF, (const uint8_t[]){(uint8_t)60}, 1);
    max31856_regs_write_burst(&ch, MAX31856_REG_CJLF, (const uint8_t[]){(uint8_t)(int8_t)10}, 1);

    /* Comfortably inside all four thresholds: no fault bits. */
    max31856_regs_advance_conversion(&ch, 50.0f, 25.0f);
    TEST_CHECK((ch.regs[MAX31856_REG_SR] & 0x3Fu) == 0u, "readings within thresholds: no threshold fault bits");

    /* Above the TC-high threshold. */
    max31856_regs_advance_conversion(&ch, 150.0f, 25.0f);
    TEST_CHECK(ch.regs[MAX31856_REG_SR] & MAX31856_FAULT_TCHIGH, "TC reading above LTHFTH sets TCHIGH");

    /* Below the TC-low threshold. */
    max31856_regs_advance_conversion(&ch, -10.0f, 25.0f);
    TEST_CHECK(ch.regs[MAX31856_REG_SR] & MAX31856_FAULT_TCLOW, "TC reading below LTLFTH/L sets TCLOW");
    TEST_CHECK(!(ch.regs[MAX31856_REG_SR] & MAX31856_FAULT_TCHIGH), "comparator mode: TCHIGH clears once no longer over threshold");

    /* CJ high/low. */
    max31856_regs_advance_conversion(&ch, 50.0f, 90.0f);
    TEST_CHECK(ch.regs[MAX31856_REG_SR] & MAX31856_FAULT_CJHIGH, "CJ reading above CJHF sets CJHIGH");

    max31856_regs_advance_conversion(&ch, 50.0f, -5.0f);
    TEST_CHECK(ch.regs[MAX31856_REG_SR] & MAX31856_FAULT_CJLOW, "CJ reading below CJLF sets CJLOW");

    /* Back to clean: comparator-mode SR is self-clearing, no residue. */
    max31856_regs_advance_conversion(&ch, 50.0f, 25.0f);
    TEST_CHECK((ch.regs[MAX31856_REG_SR] & 0x3Fu) == 0u, "comparator mode: SR returns to clean once conditions clear");
}

static void test_interrupt_mode_latches_until_faultclr(void)
{
    TEST_SECTION("max31856_regs -- CR0.FAULT_INT latches SR until FAULTCLR");

    max31856_channel_t ch;
    max31856_regs_init(&ch, 4);

    uint8_t hi, lo;
    encode_threshold(100.0f, &hi, &lo);
    max31856_regs_write_burst(&ch, MAX31856_REG_LTHFTH, (const uint8_t[]){hi, lo}, 2);
    max31856_regs_write_burst(&ch, MAX31856_REG_CR0, (const uint8_t[]){MAX31856_CR0_FAULT_INT}, 1);

    max31856_regs_advance_conversion(&ch, 150.0f, 25.0f); /* over threshold */
    TEST_CHECK(ch.regs[MAX31856_REG_SR] & MAX31856_FAULT_TCHIGH, "interrupt mode: TCHIGH sets on the over-threshold conversion");

    max31856_regs_advance_conversion(&ch, 50.0f, 25.0f); /* back under threshold */
    TEST_CHECK(ch.regs[MAX31856_REG_SR] & MAX31856_FAULT_TCHIGH, "interrupt mode: TCHIGH stays latched even once the condition clears");

    /* FAULTCLR (write CR0 with the bit set) clears it and self-clears. */
    uint8_t cr0_val = MAX31856_CR0_FAULT_INT | MAX31856_CR0_FAULTCLR;
    max31856_regs_write_burst(&ch, MAX31856_REG_CR0, &cr0_val, 1);
    TEST_CHECK((ch.regs[MAX31856_REG_CR0] & MAX31856_CR0_FAULTCLR) == 0u, "FAULTCLR self-clears in CR0 after being written");
    TEST_CHECK(ch.regs[MAX31856_REG_SR] == 0u, "FAULTCLR clears the latched SR");
}

static void test_oneshot_self_clears(void)
{
    TEST_SECTION("max31856_regs -- CR0.1SHOT self-clears after one conversion");

    max31856_channel_t ch;
    max31856_regs_init(&ch, 5);

    max31856_regs_write_burst(&ch, MAX31856_REG_CR0, (const uint8_t[]){MAX31856_CR0_ONESHOT}, 1);
    TEST_CHECK(ch.regs[MAX31856_REG_CR0] & MAX31856_CR0_ONESHOT, "1SHOT bit set immediately after being written");

    max31856_regs_advance_conversion(&ch, 50.0f, 25.0f);
    TEST_CHECK((ch.regs[MAX31856_REG_CR0] & MAX31856_CR0_ONESHOT) == 0u, "1SHOT self-clears after its one conversion");
}

static void test_read_only_registers_ignore_writes(void)
{
    TEST_SECTION("max31856_regs -- LTCB/SR are read-only");

    max31856_channel_t ch;
    max31856_regs_init(&ch, 6);
    max31856_regs_advance_conversion(&ch, 50.0f, 25.0f);
    uint8_t ltcb_before[3];
    memcpy(ltcb_before, &ch.regs[MAX31856_REG_LTCBH], 3);
    uint8_t sr_before = ch.regs[MAX31856_REG_SR];

    max31856_regs_write_burst(&ch, MAX31856_REG_LTCBH, (const uint8_t[]){0xAAu, 0xBBu, 0xCCu}, 3);
    max31856_regs_write_burst(&ch, MAX31856_REG_SR, (const uint8_t[]){0xFFu}, 1);

    TEST_CHECK(memcmp(ltcb_before, &ch.regs[MAX31856_REG_LTCBH], 3) == 0, "writes to LTCBH/M/L are ignored (read-only)");
    TEST_CHECK(ch.regs[MAX31856_REG_SR] == sr_before, "writes to SR are ignored (read-only)");
}

static void test_corruption_stuck_ltcb(void)
{
    TEST_SECTION("max31856_regs -- corruption knob: stuck LTCB");

    max31856_channel_t ch;
    max31856_regs_init(&ch, 7);

    max31856_regs_advance_conversion(&ch, 100.0f, 25.0f);
    uint8_t frozen[3];
    memcpy(frozen, &ch.regs[MAX31856_REG_LTCBH], 3);

    ch.corruption.stuck_ltcb = true;
    max31856_regs_advance_conversion(&ch, 500.0f, 25.0f); /* zone keeps moving... */
    max31856_regs_advance_conversion(&ch, 900.0f, 25.0f); /* ...but LTCB must not */
    TEST_CHECK(memcmp(frozen, &ch.regs[MAX31856_REG_LTCBH], 3) == 0,
               "stuck_ltcb freezes LTCB at its last value while the true temperature keeps changing");

    ch.corruption.stuck_ltcb = false;
    max31856_regs_advance_conversion(&ch, 900.0f, 25.0f);
    TEST_CHECK(memcmp(frozen, &ch.regs[MAX31856_REG_LTCBH], 3) != 0,
               "clearing stuck_ltcb lets LTCB resume tracking the true temperature");
}

static void test_corruption_dead_channel(void)
{
    TEST_SECTION("max31856_regs -- corruption knob: dead channel (MISO all-0/all-1)");

    max31856_channel_t ch;
    max31856_regs_init(&ch, 8);
    max31856_regs_advance_conversion(&ch, 500.0f, 25.0f); /* nonzero, non-0xFF register content */

    uint8_t out[MAX31856_REG_COUNT];

    ch.corruption.dead_mode = MAX31856_DEAD_ALL_ZERO;
    max31856_regs_read_burst(&ch, MAX31856_REG_CR0, out, MAX31856_REG_COUNT);
    bool all_zero = true;
    for (uint32_t i = 0; i < MAX31856_REG_COUNT; i++) {
        if (out[i] != 0x00u) all_zero = false;
    }
    TEST_CHECK(all_zero, "MAX31856_DEAD_ALL_ZERO forces every read byte to 0x00 regardless of register content");

    ch.corruption.dead_mode = MAX31856_DEAD_ALL_ONE;
    max31856_regs_read_burst(&ch, MAX31856_REG_CR0, out, MAX31856_REG_COUNT);
    bool all_one = true;
    for (uint32_t i = 0; i < MAX31856_REG_COUNT; i++) {
        if (out[i] != 0xFFu) all_one = false;
    }
    TEST_CHECK(all_one, "MAX31856_DEAD_ALL_ONE forces every read byte to 0xFF regardless of register content");

    /* The live register image itself is untouched by dead-mode reads -- it
     * is purely a read-side corruption. */
    ch.corruption.dead_mode = MAX31856_DEAD_NONE;
    max31856_regs_read_burst(&ch, MAX31856_REG_CR0, out, MAX31856_REG_COUNT);
    TEST_CHECK(out[MAX31856_REG_CR1] == 0x03u, "clearing dead_mode reveals the real (untouched) register image again");
}

static void test_corruption_spurious_fault_pin(void)
{
    TEST_SECTION("max31856_regs -- corruption knob: spurious ~FAULT assertion");

    max31856_channel_t ch;
    max31856_regs_init(&ch, 9);
    /* Thresholds default to 0 at power-on -- give them a sane wide band
     * first so the conversion below produces a genuinely clean SR, not one
     * that merely happens to be unmasked (MASK also defaults to FFh, fully
     * masked, which would hide the distinction this test wants to isolate). */
    uint8_t hi, lo;
    encode_threshold(200.0f, &hi, &lo);
    max31856_regs_write_burst(&ch, MAX31856_REG_LTHFTH, (const uint8_t[]){hi, lo}, 2);
    encode_threshold(-50.0f, &hi, &lo);
    max31856_regs_write_burst(&ch, MAX31856_REG_LTLFTH, (const uint8_t[]){hi, lo}, 2);
    max31856_regs_write_burst(&ch, MAX31856_REG_CJHF, (const uint8_t[]){(uint8_t)120}, 1);
    max31856_regs_write_burst(&ch, MAX31856_REG_CJLF, (const uint8_t[]){(uint8_t)(int8_t)(-40)}, 1);
    max31856_regs_write_burst(&ch, MAX31856_REG_MASK, (const uint8_t[]){0x00u}, 1); /* fully unmasked */

    max31856_regs_advance_conversion(&ch, 50.0f, 25.0f); /* well inside all four thresholds */
    TEST_CHECK((ch.regs[MAX31856_REG_SR] & 0x3Fu) == 0u, "sanity: SR is genuinely clean before the spurious-pin knob");
    TEST_CHECK(!max31856_regs_fault_pin_asserted(&ch), "clean SR, fully unmasked: ~FAULT not asserted");

    ch.corruption.spurious_fault_pin = true;
    TEST_CHECK(max31856_regs_fault_pin_asserted(&ch), "spurious_fault_pin asserts ~FAULT independent of a clean SR");
}

static void test_cj_disable_makes_cjth_writable(void)
{
    TEST_SECTION("max31856_regs -- CR0.CJ_DISABLE hands CJTH:CJTL to the master");

    max31856_channel_t ch;
    max31856_regs_init(&ch, 10);

    max31856_regs_write_burst(&ch, MAX31856_REG_CR0, (const uint8_t[]){MAX31856_CR0_CJ_DISABLE}, 1);
    max31856_regs_write_burst(&ch, MAX31856_REG_CJTH, (const uint8_t[]){0x19u, 0x00u}, 2); /* 25.0 C, 1/256 LSB */

    max31856_regs_advance_conversion(&ch, 50.0f, 999.0f); /* true_cj_c must be ignored */
    TEST_CHECK(ch.regs[MAX31856_REG_CJTH] == 0x19u && ch.regs[MAX31856_REG_CJTL] == 0x00u,
               "with CJ_DISABLE set, advance_conversion leaves the master-written CJTH:CJTL alone");
}

/* Decodes the 19-bit LTCB code back to degC, independent of
 * max31856_regs.c's own encoder (same "don't call into the module under
 * test for its own answer" discipline as encode_threshold() above). */
static float decode_ltcb_c(const max31856_channel_t *ch)
{
    uint32_t raw24 = ((uint32_t)ch->regs[MAX31856_REG_LTCBH] << 16) |
                      ((uint32_t)ch->regs[MAX31856_REG_LTCBM] << 8) |
                      (uint32_t)ch->regs[MAX31856_REG_LTCBL];
    int32_t code19 = (int32_t)(raw24 >> 5);
    if (code19 & 0x40000) { /* sign-extend 19 bits */
        code19 -= 0x80000;
    }
    return (float)code19 * 0.0078125f;
}

static float decode_cj_c(const max31856_channel_t *ch)
{
    int16_t raw = (int16_t)(((uint16_t)ch->regs[MAX31856_REG_CJTH] << 8) | ch->regs[MAX31856_REG_CJTL]);
    return (float)raw / 256.0f;
}

static void test_corruption_shorted_reads_cj(void)
{
    TEST_SECTION("max31856_regs -- corruption knob: shorted TC reads near-ambient/CJ");

    max31856_channel_t ch;
    max31856_regs_init(&ch, 11);

    /* Clean baseline: reported TC tracks the true (far-from-CJ) zone temp. */
    max31856_regs_advance_conversion(&ch, 800.0f, 25.0f);
    TEST_CHECK_NEAR(decode_ltcb_c(&ch), 800.0, 0.5, "sanity: without the fault, LTCB tracks the true zone temperature");

    ch.corruption.shorted = true;
    max31856_regs_advance_conversion(&ch, 800.0f, 25.0f); /* zone still hot... */
    TEST_CHECK_NEAR(decode_ltcb_c(&ch), 25.0, 0.5, "shorted: LTCB reads near the CJ temperature regardless of true zone temp");

    /* CJ itself drifts (a plain CJTO write) and the shorted reading follows it. */
    max31856_regs_advance_conversion(&ch, 800.0f, 40.0f);
    TEST_CHECK_NEAR(decode_ltcb_c(&ch), 40.0, 0.5, "shorted: LTCB tracks CJ as CJ itself changes");

    ch.corruption.shorted = false;
    max31856_regs_advance_conversion(&ch, 800.0f, 40.0f);
    TEST_CHECK_NEAR(decode_ltcb_c(&ch), 800.0, 0.5, "clearing shorted lets LTCB resume tracking the true zone temperature");
}

static void test_corruption_drift_ramps_over_time(void)
{
    TEST_SECTION("max31856_regs -- corruption knob: drifting TC (calibration drift ramp)");

    max31856_channel_t ch;
    max31856_regs_init(&ch, 12);

    max31856_regs_advance_conversion(&ch, 500.0f, 25.0f);
    TEST_CHECK_NEAR(decode_ltcb_c(&ch), 500.0, 0.5, "sanity: zero drift_offset_c leaves the reading untouched");

    ch.corruption.drift_offset_c = 3.0f;
    max31856_regs_advance_conversion(&ch, 500.0f, 25.0f);
    TEST_CHECK_NEAR(decode_ltcb_c(&ch), 503.0, 0.5, "drift_offset_c adds a positive offset to the true reading");

    ch.corruption.drift_offset_c = 12.5f; /* caller re-computes a larger offset as sim time advances */
    max31856_regs_advance_conversion(&ch, 500.0f, 25.0f);
    TEST_CHECK_NEAR(decode_ltcb_c(&ch), 512.5, 0.5, "a larger drift_offset_c on the next conversion ramps the reading further");

    ch.corruption.drift_offset_c = -8.0f; /* ramp can run either direction */
    max31856_regs_advance_conversion(&ch, 500.0f, 25.0f);
    TEST_CHECK_NEAR(decode_ltcb_c(&ch), 492.0, 0.5, "drift_offset_c can be negative (ramping down)");
}

static void test_corruption_cj_fault_offset_and_sr_bits(void)
{
    TEST_SECTION("max31856_regs -- corruption knob: CJ fault (wrong CJ + CJHIGH/CJLOW SR bits)");

    max31856_channel_t ch;
    max31856_regs_init(&ch, 13);

    /* Wide-open CJ thresholds first so a clean conversion is genuinely
     * fault-free (CJHF/CJLF default to 0 at power-on, which would trip
     * immediately otherwise). */
    max31856_regs_write_burst(&ch, MAX31856_REG_CJHF, (const uint8_t[]){(uint8_t)100}, 1);
    max31856_regs_write_burst(&ch, MAX31856_REG_CJLF, (const uint8_t[]){(uint8_t)(int8_t)(-20)}, 1);

    max31856_regs_advance_conversion(&ch, 50.0f, 25.0f);
    TEST_CHECK_NEAR(decode_cj_c(&ch), 25.0, 0.5, "sanity: zero cj_fault_offset_c reports the true CJ");
    TEST_CHECK((ch.regs[MAX31856_REG_SR] & (MAX31856_FAULT_CJHIGH | MAX31856_FAULT_CJLOW)) == 0u,
               "sanity: within thresholds, no CJ fault bits");

    /* A large positive offset both reports a wrong CJ and crosses CJHF. */
    ch.corruption.cj_fault_offset_c = 90.0f;
    max31856_regs_advance_conversion(&ch, 50.0f, 25.0f);
    TEST_CHECK_NEAR(decode_cj_c(&ch), 115.0, 0.5, "cj_fault_offset_c reports a wrong CJ temperature (true + offset)");
    TEST_CHECK(ch.regs[MAX31856_REG_SR] & MAX31856_FAULT_CJHIGH,
               "cj_fault_offset_c large enough to cross CJHF sets CJHIGH -- no separate SR-forcing needed");

    /* A large negative offset crosses CJLF instead. */
    ch.corruption.cj_fault_offset_c = -60.0f;
    max31856_regs_advance_conversion(&ch, 50.0f, 25.0f);
    TEST_CHECK_NEAR(decode_cj_c(&ch), -35.0, 0.5, "cj_fault_offset_c can be negative");
    TEST_CHECK(ch.regs[MAX31856_REG_SR] & MAX31856_FAULT_CJLOW, "cj_fault_offset_c crossing CJLF sets CJLOW");

    /* CJ_DISABLE: the offset must NOT apply -- the master owns CJTH:CJTL. */
    max31856_regs_write_burst(&ch, MAX31856_REG_CR0, (const uint8_t[]){MAX31856_CR0_CJ_DISABLE}, 1);
    max31856_regs_write_burst(&ch, MAX31856_REG_CJTH, (const uint8_t[]){0x19u, 0x00u}, 2); /* 25.0 C */
    ch.corruption.cj_fault_offset_c = 999.0f;
    max31856_regs_advance_conversion(&ch, 50.0f, 25.0f);
    TEST_CHECK_NEAR(decode_cj_c(&ch), 25.0, 0.5, "CJ_DISABLE: cj_fault_offset_c has no effect, master-written CJTH:CJTL wins");
}

static void test_corruption_severity_order_shorted_beats_drift_stuck_beats_both(void)
{
    TEST_SECTION("max31856_regs -- corruption knobs compose with documented severity order");

    max31856_channel_t ch;
    max31856_regs_init(&ch, 14);

    /* shorted wins over drift: shorted ignores true_tc_c entirely (and thus
     * drift_offset_c, which only applies to true_tc_c), reading CJ instead. */
    ch.corruption.shorted = true;
    ch.corruption.drift_offset_c = 500.0f;
    max31856_regs_advance_conversion(&ch, 800.0f, 25.0f);
    TEST_CHECK_NEAR(decode_ltcb_c(&ch), 25.0, 0.5, "shorted takes precedence over drift_offset_c");

    /* stuck_ltcb wins over both: once frozen, neither shorted nor drift can
     * move the reported value. */
    ch.corruption.stuck_ltcb = true;
    float frozen = decode_ltcb_c(&ch);
    ch.corruption.shorted = false;
    ch.corruption.drift_offset_c = 0.0f;
    max31856_regs_advance_conversion(&ch, 900.0f, 90.0f); /* both true temp and CJ move a lot */
    TEST_CHECK_NEAR(decode_ltcb_c(&ch), frozen, 0.5, "stuck_ltcb takes precedence over both shorted and drift");
}

static void test_master_has_written_flag(void)
{
    TEST_SECTION("max31856_regs -- master_has_written (TC_GET_MASTER_CONFIG's "
                  "configured-vs-never-configured distinction, PLAN.md 5.2)");

    max31856_channel_t ch;
    max31856_regs_init(&ch, 15);
    TEST_CHECK(ch.master_has_written == false, "freshly init'd channel: master_has_written starts false");

    /* A pure read burst must NOT set the flag -- only a write transaction
     * counts as "the master configured something." */
    uint8_t dummy[1] = {0};
    max31856_regs_read_burst(&ch, MAX31856_REG_CR1, dummy, 1);
    TEST_CHECK(ch.master_has_written == false, "a read-only transaction does not set master_has_written");

    /* A write to a normal R/W register sets it. */
    max31856_regs_write_burst(&ch, MAX31856_REG_CR1, (const uint8_t[]){0x23u}, 1);
    TEST_CHECK(ch.master_has_written == true, "a write transaction sets master_has_written");

    /* Stays set across further transactions of any kind (monotonic, never
     * cleared except by re-init). */
    max31856_regs_read_burst(&ch, MAX31856_REG_SR, dummy, 1);
    TEST_CHECK(ch.master_has_written == true, "master_has_written stays set (monotonic) across a later read");

    /* re-init resets it. */
    max31856_regs_init(&ch, 16);
    TEST_CHECK(ch.master_has_written == false, "re-init clears master_has_written");

    /* A write attempt at a READ-ONLY address (e.g. SR) still counts -- "the
     * master attempted to write" is itself the observable behavior
     * TC_GET_MASTER_CONFIG reports on, per max31856_regs.h's struct
     * comment, even though the write has no effect on the register value. */
    uint8_t sr_before = ch.regs[MAX31856_REG_SR];
    max31856_regs_write_burst(&ch, MAX31856_REG_SR, (const uint8_t[]){0xFFu}, 1);
    TEST_CHECK(ch.master_has_written == true, "a write attempt at a read-only address still sets master_has_written");
    TEST_CHECK(ch.regs[MAX31856_REG_SR] == sr_before, "...even though the read-only register's value is unchanged");
}

void run_test_max31856_regs(void)
{
    test_write_read_verbatim();
    test_auto_increment();
    test_fault_bits_from_thresholds();
    test_interrupt_mode_latches_until_faultclr();
    test_oneshot_self_clears();
    test_read_only_registers_ignore_writes();
    test_corruption_stuck_ltcb();
    test_corruption_dead_channel();
    test_corruption_spurious_fault_pin();
    test_cj_disable_makes_cjth_writable();
    test_corruption_shorted_reads_cj();
    test_corruption_drift_ramps_over_time();
    test_corruption_cj_fault_offset_and_sr_bits();
    test_corruption_severity_order_shorted_beats_drift_stuck_beats_both();
    test_master_has_written_flag();
}
