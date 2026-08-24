// max31856.c -- see max31856.h for what changed vs. the KilnFW original and
// why. The register math, fixed-point conversions and fault-invalidation
// logic below are ported line-for-line from
// firmware/KilnFW/App/drivers/MAX31856.c; only the transport (spi_owner.h
// instead of esp_spi_owner.h) and the multi-channel bookkeeping are gone.
#include "max31856.h"

#include <math.h>
#include <string.h>

#include "hardware/gpio.h"

#include "max31856_decode.h"
#include "max31856_tc_type_policy.h"
#include "spi_owner.h"

// Longest burst this driver ever does: address byte + 6-register temperature
// burst, or address byte + 1 register register write. 8 bytes covers both
// with room to spare.
#define MAX31856_MAX_XFER_LEN 8u

// Steady-state automatic-mode conversion timing, 60Hz notch, AVGSEL = 4
// samples -- the only configuration THERMOCOUPLE.md section 5 allows, so
// unlike KilnFW's driver this does not need to branch on filter/avg mode.
// tCONV base is the datasheet's MAX column (100ms nominal auto-mode, per
// KilnFW's MAX31856_TCONV_AUTO_60HZ_MS), plus (4-1) steady-state adders of
// 16.67ms rounded up to 17ms (KilnFW's MAX31856_AVG_ADDER_AUTO_60HZ_MS).
#define MAX31856_CONV_MS_AUTO_60HZ_4AVG (100u + 3u * 17u) // 151ms

// Faults that make the linearized hot-junction number meaningless rather
// than merely interesting -- ported from MAX31856_TC_INVALIDATING_FAULTS.
#define MAX31856_TC_INVALIDATING_FAULTS \
    ((uint8_t)(MAX31856_FAULT_OPEN | MAX31856_FAULT_OVUV | MAX31856_FAULT_TCRANGE))

static bool s_initialized = false;
static uint8_t s_cs_gpio;
static int s_fault_gpio = -1; // -1 = not wired, matches KilnFW's convention
static uint8_t s_cr0_shadow;
static uint8_t s_cr1_shadow;

static bool max31856_write_u8(uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = { MAX31856_WRITE_ADDR(reg), value };
    return spi_owner_transfer(tx, NULL, sizeof(tx));
}

static bool max31856_read_burst(uint8_t reg, uint8_t *out, size_t len)
{
    if (!out || len == 0 || (1u + len) > MAX31856_MAX_XFER_LEN) {
        return false;
    }

    uint8_t tx[MAX31856_MAX_XFER_LEN];
    uint8_t rx[MAX31856_MAX_XFER_LEN];
    memset(tx, 0, sizeof(tx));
    tx[0] = (uint8_t)(reg & 0x7Fu); // bit 7 clear selects a read

    if (!spi_owner_transfer(tx, rx, len + 1u)) {
        return false;
    }

    // rx[0] is whatever SDO held while the address byte went out (high-Z,
    // meaningless); register data starts one byte later -- same as KilnFW's
    // max31856_read_burst().
    memcpy(out, &rx[1], len);
    return true;
}

// CJTH:CJTL / LTCBH:LTCBM:LTCBL fixed-point decode math now lives in
// max31856_decode.c/.h -- pure, host-tested (test/test_max31856_decode.c),
// same split-out-for-host-testing pattern as max31856_tc_type_policy.h.

bool max31856_init(uint8_t cs_gpio, uint8_t fault_gpio)
{
    // ~CS idles high, output latch set before direction -- same
    // "never briefly undriven" discipline used for GPIO6 and spi_owner's CS0.
    gpio_init(cs_gpio);
    gpio_put(cs_gpio, 1);
    gpio_set_dir(cs_gpio, GPIO_OUT);
    s_cs_gpio = cs_gpio;

    // ~FAULT: input with the internal pull-up. THERMOCOUPLE.md section 1
    // says R1 (external, 10k) already does this job on this board; the
    // internal pull-up is redundant belt-and-braces, matching KilnFW's own
    // reasoning for its (non-redundant) case.
    gpio_init(fault_gpio);
    gpio_set_dir(fault_gpio, GPIO_IN);
    gpio_pull_up(fault_gpio);
    s_fault_gpio = (int)fault_gpio;

    // Power-on defaults (datasheet Table 6): CR0 = 00h, CR1 = 03h (type K,
    // 1 sample), MASK = FFh. No register written here -- max31856_configure()
    // rewrites CR0/CR1/MASK unconditionally.
    s_cr0_shadow = 0x00u;
    s_cr1_shadow = 0x03u;

    s_initialized = true;
    return true;
}

bool max31856_configure(uint8_t tc_type)
{
    // tc_type > MAX31856_TC_TYPE_T (0x07) selects CR1.TC TYPE[3:0]'s Voltage
    // Mode (datasheet page 20's register field table: 10xx = gain 8, 11xx =
    // gain 32) instead of a real, linearized thermocouple type. In that mode
    // LTCB holds `gain * 1.6 * 2^17 * VIN` -- a scaled input voltage -- but
    // max31856_decode_tc() below unconditionally treats LTCB as a linearized
    // temperature (MAX31856_TC_TEMP_C_PER_LSB), so this part would keep
    // handing back plausible-looking, silently WRONG degC readings forever
    // (see max31856_tc_type_policy.h's header comment for the worked
    // example: a real 1000 degC junction decodes as ~17 degC in voltage
    // mode). This board's only runtime commissioning axis is tc_type
    // (THERMOCOUPLE.md section 2) and there is no raw/voltage-mode debug
    // path here to justify ever accepting 0x08-0x0F (contrast KilnFW, which
    // deliberately does allow them behind its own separate raw debug
    // subcommand). Refusing here is the fail-closed outcome: main.c already
    // treats a false return as a logged, non-fatal probe failure that
    // surfaces at runtime through S5 ("a blind processor") -- strictly safer
    // than configuring the part into a mode whose numbers this driver cannot
    // honestly report as temperature.
    if (!s_initialized || !max31856_tc_type_is_valid(tc_type)) {
        return false;
    }

    // CR1: AVGSEL fixed at 4 samples (THERMOCOUPLE.md section 5), TC TYPE is
    // the runtime-commissioned field.
    uint8_t cr1 = (uint8_t)((MAX31856_AVGSEL_4_SAMPLES << 4) | (tc_type & 0x0Fu));

    // CR0 with CMODE deliberately left out; restored at the end. OCFAULT
    // mode 1, CJ enabled (bit clear), comparator fault mode (bit clear),
    // 60Hz notch -- THERMOCOUPLE.md section 5's table, all fixed choices for
    // this board.
    uint8_t cr0_stopped = (uint8_t)(MAX31856_OC_MODE1 << 4);

    // Step 1: conversions off, with the (fixed) 50/60Hz bit already in
    // place -- "change the notch frequency only in Normally Off mode."
    if (!max31856_write_u8(MAX31856_REG_CR0, cr0_stopped)) {
        return false;
    }
    s_cr0_shadow = cr0_stopped;

    // Step 2: CR1 while still stopped -- "averaging should not be changed
    // while conversions are taking place."
    if (!max31856_write_u8(MAX31856_REG_CR1, cr1)) {
        return false;
    }
    s_cr1_shadow = cr1;

    // Step 3: MASK. THERMOCOUPLE.md section 5's #1 trap: the reset default
    // is FFh (every fault masked off ~FAULT), which must never be left in
    // place.
    if (!max31856_write_u8(MAX31856_REG_MASK, MAX31856_DEFAULT_FAULT_MASK)) {
        return false;
    }

    // Step 4: restore CMODE = automatic conversion.
    uint8_t cr0_running = (uint8_t)(cr0_stopped | MAX31856_CR0_CMODE);
    if (!max31856_write_u8(MAX31856_REG_CR0, cr0_running)) {
        return false;
    }
    s_cr0_shadow = cr0_running;

    return true;
}

bool max31856_read(max31856_reading_t *out)
{
    if (!out) {
        return false;
    }

    // Fill in the failure answer first, so every early return below is
    // already honest -- KilnFW's "failure honesty" discipline
    // (firmware/KilnFW/docs/MAX31856.md): NaN temperatures, no fault bits,
    // spi_failed set, never 0, never a cached value.
    memset(out, 0, sizeof(*out));
    out->tc_temperature_c = NAN;
    out->cj_temperature_c = NAN;
    out->spi_failed = true;

    if (!s_initialized) {
        return false;
    }

    out->fault_pin_asserted = (s_fault_gpio >= 0) && (gpio_get(s_fault_gpio) == 0);

    // One transaction, six registers: CJTH, CJTL, LTCBH, LTCBM, LTCBL, SR.
    // Reading this burst is what releases ~DRDY back high (THERMOCOUPLE.md
    // section 1) -- thermo_task samples ~DRDY's edge before calling this, not
    // after.
    uint8_t buf[6];
    if (!max31856_read_burst(MAX31856_REG_CJTH, buf, sizeof(buf))) {
        return false;
    }

    out->cj_temperature_c = max31856_decode_cj(buf[0], buf[1]);
    out->tc_temperature_c = max31856_decode_tc(buf[2], buf[3], buf[4]);
    out->fault_status = buf[5];
    out->spi_failed = false;

    // The transfer succeeded, which says nothing about whether the number it
    // carried means anything -- ported verbatim from MAX31856_read()'s own
    // reasoning. An open thermocouple, an over/undervoltage input
    // (conversions suspended, LTCB holds the pre-fault value) or a hot
    // junction outside the type's linearization range all leave a plausible
    // temperature in the register; report NaN for those instead.
    if (out->fault_status & MAX31856_TC_INVALIDATING_FAULTS) {
        out->tc_temperature_c = NAN;
    }
    if (out->fault_status & MAX31856_FAULT_CJRANGE) {
        out->cj_temperature_c = NAN;
    }
    if (!isfinite(out->tc_temperature_c) && !isnan(out->tc_temperature_c)) {
        out->tc_temperature_c = NAN;
    }

    return true;
}

uint32_t max31856_conversion_time_ms(void)
{
    if (!s_initialized) {
        return 0;
    }
    return MAX31856_CONV_MS_AUTO_60HZ_4AVG;
}
