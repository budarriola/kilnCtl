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
}
