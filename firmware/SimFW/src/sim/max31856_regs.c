// max31856_regs.c -- see max31856_regs.h for the register map, coherency
// guarantee, and corruption-knob contract (DESIGN_NOTES.md section 3.2).
#include "max31856_regs.h"

#include <math.h>
#include <string.h>

/* xorshift32 -- same small, dependency-free, deterministic-from-seed
 * generator firmware/CommonFW/test/test_fuzz.c already uses in this repo.
 * Not cryptographic; does not need to be -- this only drives Gaussian noise
 * and bit-error injection, both of which must be reproducible from a seed,
 * never actually random. */
static uint32_t xorshift32_next(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static float xorshift32_float01(uint32_t *state)
{
    /* Top 24 bits -> [0,1). */
    return (float)(xorshift32_next(state) >> 8) * (1.0f / 16777216.0f);
}

/* Box-Muller, one value per call (discards the paired second sample --
 * simplicity over efficiency, this runs at conversion rate not sample
 * rate). Local PI constant rather than M_PI: MSVC's math.h only defines
 * M_PI behind _USE_MATH_DEFINES, which this file does not want to require
 * of every includer. */
#define MAX31856_PI_F 3.14159265358979323846f

static float gaussian(uint32_t *state, float sigma)
{
    if (sigma <= 0.0f) {
        return 0.0f;
    }
    float u1 = xorshift32_float01(state);
    float u2 = xorshift32_float01(state);
    if (u1 < 1e-7f) {
        u1 = 1e-7f;
    }
    float mag = sqrtf(-2.0f * logf(u1));
    return mag * cosf(2.0f * MAX31856_PI_F * u2) * sigma;
}

static int32_t clampi32(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* --- Register encode/decode helpers ------------------------------------- */

static int16_t decode_threshold16(uint8_t hi, uint8_t lo)
{
    return (int16_t)(((uint16_t)hi << 8) | lo);
}

static void encode_threshold16(int16_t raw, uint8_t *hi, uint8_t *lo)
{
    *hi = (uint8_t)(((uint16_t)raw >> 8) & 0xFFu);
    *lo = (uint8_t)((uint16_t)raw & 0xFFu);
}

static float threshold16_to_c(int16_t raw)
{
    return (float)raw * MAX31856_TC_THRESHOLD_C_PER_LSB;
}

static int16_t c_to_cj16(float c)
{
    /* CJTH:CJTL combined 16-bit word, low 2 bits hard 0 per datasheet;
     * value = raw16 / 256. */
    int32_t raw = (int32_t)lrintf(c / MAX31856_CJ_TEMP_C_PER_LSB);
    raw = clampi32(raw, -32768, 32767);
    raw &= ~0x3; /* low 2 bits always 0 */
    return (int16_t)raw;
}

static float cj16_to_c(int16_t raw)
{
    return (float)raw * MAX31856_CJ_TEMP_C_PER_LSB;
}

/* 19-bit two's-complement TC code, packed into the top 19 bits of a 24-bit
 * word (low 5 bits always 0) -- DESIGN_NOTES.md 3.2's "quantized to 0.0078125 degC
 * LSB". */
#define TC_CODE19_MIN (-262144)
#define TC_CODE19_MAX (262143)

static int32_t c_to_code19(float c, bool *out_range_fault)
{
    int32_t code = (int32_t)lrintf(c / MAX31856_TC_TEMP_C_PER_LSB);
    if (code < TC_CODE19_MIN || code > TC_CODE19_MAX) {
        if (out_range_fault) *out_range_fault = true;
        code = clampi32(code, TC_CODE19_MIN, TC_CODE19_MAX);
    } else if (out_range_fault) {
        *out_range_fault = false;
    }
    return code;
}

static void code19_to_ltcb(int32_t code19, uint8_t *h, uint8_t *m, uint8_t *l)
{
    uint32_t raw24 = ((uint32_t)code19 & 0x7FFFFu) << 5; /* mask to 19 bits, shift into place */
    *h = (uint8_t)((raw24 >> 16) & 0xFFu);
    *m = (uint8_t)((raw24 >> 8) & 0xFFu);
    *l = (uint8_t)(raw24 & 0xFFu);
}

/* --- Init ---------------------------------------------------------------- */

void max31856_regs_init(max31856_channel_t *ch, uint32_t rng_seed)
{
    memset(ch, 0, sizeof(*ch));
    ch->regs[MAX31856_REG_CR0] = 0x00u;
    ch->regs[MAX31856_REG_CR1] = 0x03u;
    ch->regs[MAX31856_REG_MASK] = 0xFFu;
    /* Everything else (thresholds, CJTO, CJTH/L, LTCB, SR) power-on at 0,
     * matching memset above. */
    ch->rng_state = (rng_seed != 0u) ? rng_seed : 0x9E3779B9u;
}

/* --- Write-back rules ----------------------------------------------------- */

static void apply_write_rule(max31856_channel_t *ch, uint8_t addr, uint8_t value)
{
    /* See max31856_regs.h's struct comment: any data byte the master sends
     * during a write transaction counts, even one landing on a read-only
     * address below (the write attempt itself is the observable DUT
     * behavior TC_GET_MASTER_CONFIG reports on). */
    ch->master_has_written = true;

    if (addr >= MAX31856_REG_COUNT) {
        /* 10h..7Fh are inside the part's 7-bit address space but are not
         * implemented registers (datasheet page 15: invalid addresses read
         * FFh); a write there changes nothing. The attempt still counts
         * above, same reasoning as the read-only case below. */
        return;
    }

    switch (addr) {
    case MAX31856_REG_LTCBH:
    case MAX31856_REG_LTCBM:
    case MAX31856_REG_LTCBL:
    case MAX31856_REG_SR:
        /* Read-only. Datasheet-silent writes: ignored entirely. */
        return;

    case MAX31856_REG_CR0: {
        ch->regs[MAX31856_REG_CR0] = value;
        if (value & MAX31856_CR0_FAULTCLR) {
            /* Clears latched faults immediately (has no visible effect in
             * comparator mode, where SR already tracks live conditions --
             * matches both real drivers' header comments) and self-clears. */
            ch->regs[MAX31856_REG_SR] = 0u;
            uint8_t faultclr_bit = MAX31856_CR0_FAULTCLR; /* not a constant expr below: silences C4310 */
            ch->regs[MAX31856_REG_CR0] &= (uint8_t)~faultclr_bit;
        }
        /* ONESHOT is left set here -- it self-clears in
         * max31856_regs_advance_conversion() ("self-clears after one
         * conversion", DESIGN_NOTES.md 3.2), not at write time. */
        return;
    }

    default:
        ch->regs[addr] = value;
        return;
    }
}

/* --- Byte-level transaction interface ------------------------------------ */

void max31856_regs_cs_assert(max31856_channel_t *ch, uint8_t addr_byte)
{
    ch->cs_low = true;
    ch->txn_is_write = (addr_byte & MAX31856_WRITE_BIT) != 0u;
    ch->txn_addr = MAX31856_ADDR_MASK(addr_byte);
    ch->txn_touched_drdy = false;
    if (!ch->txn_is_write) {
        /* Coherency snapshot: the whole burst reads from this copy, so a
         * conversion committed mid-transaction (which a well-behaved caller
         * never triggers, but this module does not assume) cannot tear a
         * multi-byte read. */
        memcpy(ch->read_snapshot, ch->regs, sizeof(ch->read_snapshot));
    }
}

static uint8_t apply_dead_mode(max31856_dead_mode_t mode, uint8_t byte)
{
    switch (mode) {
    case MAX31856_DEAD_ALL_ZERO: return 0x00u;
    case MAX31856_DEAD_ALL_ONE:
    case MAX31856_DEAD_HIGH_Z:   return 0xFFu;
    case MAX31856_DEAD_NONE:
    default:                     return byte;
    }
}

static uint8_t apply_bit_errors(max31856_channel_t *ch, uint8_t byte)
{
    float rate = ch->corruption.bit_error_rate;
    if (rate <= 0.0f) {
        return byte;
    }
    for (int b = 0; b < 8; b++) {
        if (xorshift32_float01(&ch->rng_state) < rate) {
            byte ^= (uint8_t)(1u << b);
        }
    }
    return byte;
}

uint8_t max31856_regs_raw_read_value(const uint8_t *regs, uint8_t addr)
{
    return (addr < MAX31856_REG_COUNT) ? regs[addr]
                                       : (uint8_t)MAX31856_INVALID_ADDR_VALUE;
}

uint8_t max31856_regs_apply_read_corruption(max31856_channel_t *ch, uint8_t byte)
{
    if (ch->corruption.dead_mode != MAX31856_DEAD_NONE) {
        return apply_dead_mode(ch->corruption.dead_mode, byte);
    }
    return apply_bit_errors(ch, byte);
}

/* Addresses whose read releases ~DRDY. LTCB always; CJTH/CJTL only while the
 * internal cold-junction sensor is enabled -- see max31856_regs.h's
 * max31856_regs_drdy_asserted() comment for the datasheet wording. */
static bool addr_releases_drdy(const max31856_channel_t *ch, uint8_t addr)
{
    switch (addr) {
    case MAX31856_REG_LTCBH:
    case MAX31856_REG_LTCBM:
    case MAX31856_REG_LTCBL:
        return true;
    case MAX31856_REG_CJTH:
    case MAX31856_REG_CJTL:
        return (ch->regs[MAX31856_REG_CR0] & MAX31856_CR0_CJ_DISABLE) == 0u;
    default:
        return false;
    }
}

uint8_t max31856_regs_clock_read_byte(max31856_channel_t *ch)
{
    if (!ch->cs_low || ch->txn_is_write) {
        return 0x00u;
    }
    uint8_t byte = max31856_regs_raw_read_value(ch->read_snapshot, ch->txn_addr);
    if (addr_releases_drdy(ch, ch->txn_addr)) {
        ch->txn_touched_drdy = true;
    }
    ch->txn_addr = (uint8_t)((ch->txn_addr + 1u) % MAX31856_ADDR_SPACE);

    return max31856_regs_apply_read_corruption(ch, byte);
}

void max31856_regs_clock_write_byte(max31856_channel_t *ch, uint8_t data_in)
{
    if (!ch->cs_low || !ch->txn_is_write) {
        return;
    }
    apply_write_rule(ch, ch->txn_addr, data_in);
    ch->txn_addr = (uint8_t)((ch->txn_addr + 1u) % MAX31856_ADDR_SPACE);
}

void max31856_regs_cs_deassert(max31856_channel_t *ch)
{
    ch->cs_low = false;
    if (ch->txn_touched_drdy) {
        /* "When a read-operation of the Linearized Thermocouple Temperature
         * register or the Cold-Junction Temperature Register (if enabled)
         * completes, DRDY returns high." -- the read completes here. */
        ch->drdy_asserted = false;
        ch->txn_touched_drdy = false;
    }
}

void max31856_regs_write_burst(max31856_channel_t *ch, uint8_t addr, const uint8_t *data, size_t len)
{
    max31856_regs_cs_assert(ch, MAX31856_WRITE_ADDR(addr));
    for (size_t i = 0; i < len; i++) {
        max31856_regs_clock_write_byte(ch, data[i]);
    }
    max31856_regs_cs_deassert(ch);
}

void max31856_regs_read_burst(max31856_channel_t *ch, uint8_t addr, uint8_t *out, size_t len)
{
    max31856_regs_cs_assert(ch, addr);
    for (size_t i = 0; i < len; i++) {
        out[i] = max31856_regs_clock_read_byte(ch);
    }
    max31856_regs_cs_deassert(ch);
}

/* --- Conversion / fault machinery ----------------------------------------- */

bool max31856_regs_advance_conversion(max31856_channel_t *ch, float true_tc_c, float true_cj_c)
{
    uint8_t cr0 = ch->regs[MAX31856_REG_CR0];

    /* --- cold junction: internal sensor unless CJ_DISABLE is set ---
     * Computed before the TC block below because corruption.shorted needs
     * the reported CJ value to build its "reads near-ambient/CJ" result
     * (DESIGN_NOTES.md 7.1 "Shorted TC"). */
    float reported_cj_c;
    if (cr0 & MAX31856_CR0_CJ_DISABLE) {
        /* Master owns CJTH:CJTL when the internal sensor is off -- do not
         * overwrite what was written, and no fault offset applies (this
         * module has nothing to add an offset to; the master's own value is
         * authoritative). */
        reported_cj_c = cj16_to_c(decode_threshold16(ch->regs[MAX31856_REG_CJTH], ch->regs[MAX31856_REG_CJTL]));
    } else {
        int8_t cjto = (int8_t)ch->regs[MAX31856_REG_CJTO];
        reported_cj_c = true_cj_c + (float)cjto * MAX31856_CJ_OFFSET_C_PER_LSB + ch->corruption.cj_fault_offset_c;
        int16_t cj_raw = c_to_cj16(reported_cj_c);
        uint8_t cjh, cjl;
        encode_threshold16(cj_raw, &cjh, &cjl);
        ch->regs[MAX31856_REG_CJTH] = cjh;
        ch->regs[MAX31856_REG_CJTL] = cjl;
        reported_cj_c = cj16_to_c(cj_raw);
    }
    bool cj_range_fault = (reported_cj_c < -55.0f || reported_cj_c > 125.0f);

    /* --- reported TC value: shorted/drift, then noise, then stuck-LTCB
     * freeze (severity order documented on max31856_corruption_t itself) --- */
    float base_tc_c = ch->corruption.shorted ? reported_cj_c : (true_tc_c + ch->corruption.drift_offset_c);
    float reported_tc_c;
    if (ch->corruption.stuck_ltcb && ch->has_reported) {
        reported_tc_c = ch->last_reported_tc_c;
    } else {
        reported_tc_c = base_tc_c + gaussian(&ch->rng_state, ch->corruption.noise_sigma_c);
    }
    ch->last_reported_tc_c = reported_tc_c;
    ch->has_reported = true;

    bool tc_range_fault = false;
    int32_t code19 = c_to_code19(reported_tc_c, &tc_range_fault);
    uint8_t h, m, l;
    code19_to_ltcb(code19, &h, &m, &l);
    ch->regs[MAX31856_REG_LTCBH] = h;
    ch->regs[MAX31856_REG_LTCBM] = m;
    ch->regs[MAX31856_REG_LTCBL] = l;

    /* --- threshold comparisons against master-written registers --- */
    int16_t tc_hi_raw = decode_threshold16(ch->regs[MAX31856_REG_LTHFTH], ch->regs[MAX31856_REG_LTHFTL]);
    int16_t tc_lo_raw = decode_threshold16(ch->regs[MAX31856_REG_LTLFTH], ch->regs[MAX31856_REG_LTLFTL]);
    float tc_hi_c = threshold16_to_c(tc_hi_raw);
    float tc_lo_c = threshold16_to_c(tc_lo_raw);
    int8_t cj_hi = (int8_t)ch->regs[MAX31856_REG_CJHF];
    int8_t cj_lo = (int8_t)ch->regs[MAX31856_REG_CJLF];

    uint8_t computed_sr = 0u;
    if (reported_tc_c > tc_hi_c) computed_sr |= MAX31856_FAULT_TCHIGH;
    if (reported_tc_c < tc_lo_c) computed_sr |= MAX31856_FAULT_TCLOW;
    if (reported_cj_c > (float)cj_hi) computed_sr |= MAX31856_FAULT_CJHIGH;
    if (reported_cj_c < (float)cj_lo) computed_sr |= MAX31856_FAULT_CJLOW;
    if (tc_range_fault) computed_sr |= MAX31856_FAULT_TCRANGE;
    if (cj_range_fault) computed_sr |= MAX31856_FAULT_CJRANGE;
    /* OPEN/OVUV: this module has no independent electrical-fault input --
     * fault_engine (or a direct test) sets those bits by driving SR through
     * corruption/override at a higher layer than "compare temp to
     * threshold". Nothing here ever sets them. */

    if (cr0 & MAX31856_CR0_FAULT_INT) {
        /* Interrupt mode: bits latch (stay set) once raised, cleared only
         * by FAULTCLR (handled in apply_write_rule). */
        ch->regs[MAX31856_REG_SR] |= computed_sr;
    } else {
        /* Comparator mode (default): SR tracks the live condition. */
        ch->regs[MAX31856_REG_SR] = computed_sr;
    }

    /* ~DRDY goes low when a new conversion result is available in LTCB. Only
     * when the part would actually have converted: CMODE set (automatic) or a
     * one-shot pending. In "normally off" mode with no one-shot the real part
     * produces no result and therefore no ~DRDY edge -- this emulator still
     * refreshes LTCB above (long-standing behaviour, deliberately not changed
     * here), but it must not lie about the pin. Note this is evaluated
     * against the CR0 sampled at entry, i.e. *before* the ONESHOT self-clear
     * below, so the one-shot's own result does raise ~DRDY. */
    if (cr0 & (MAX31856_CR0_CMODE | MAX31856_CR0_ONESHOT)) {
        ch->drdy_asserted = true;
    }

    /* ONESHOT self-clears once its one conversion has happened. */
    if (cr0 & MAX31856_CR0_ONESHOT) {
        uint8_t oneshot_bit = MAX31856_CR0_ONESHOT; /* not a constant expr below: silences C4310 */
        ch->regs[MAX31856_REG_CR0] &= (uint8_t)~oneshot_bit;
    }

    return true;
}

bool max31856_regs_drdy_asserted(const max31856_channel_t *ch)
{
    return ch->drdy_asserted;
}

bool max31856_regs_fault_pin_asserted(const max31856_channel_t *ch)
{
    if (ch->corruption.spurious_fault_pin) {
        return true;
    }
    uint8_t sr = ch->regs[MAX31856_REG_SR];
    uint8_t mask = ch->regs[MAX31856_REG_MASK];

    /* TCRANGE/CJRANGE (bits 6/7) are never maskable -- MASK only has bits
     * for the low six (both real drivers' header comments agree). */
    uint8_t maskable = sr & MAX31856_MASK_ALL & (uint8_t)~mask;
    uint8_t unmaskable = sr & (MAX31856_FAULT_TCRANGE | MAX31856_FAULT_CJRANGE);

    return (maskable != 0u) || (unmaskable != 0u);
}
