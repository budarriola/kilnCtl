// max31856.c -- see max31856.h for what changed vs. the KilnFW original and
// why. The register math, fixed-point conversions and fault-invalidation
// logic below are ported line-for-line from
// firmware/KilnFW/App/drivers/hw/MAX31856.c; only the transport
// (hal_spi.h/pico's spi_owner.c backend instead of ESP-IDF's spi_master.h /
// esp_spi_owner.h) and the multi-channel bookkeeping are gone.
//
// HAL Phase 1b migration (docs/HW_ABSTRACTION_PLAN.md): this module used to
// call spi_owner_transfer() (firmware/hwAbstraction/pico/spi/spi_owner.h)
// directly, with main.c calling spi_owner_init() as a separate boot step.
// Both now go through interface/hal_spi.h, backed on-target by
// pico/spi/hal_spi_pico.c's thin adapter over that same spi_owner.c (byte-
// identical body, unchanged transfer/timing/retry behavior) -- see
// max31856_bus_init() below, which main.c now calls in place of
// spi_owner_init().
//
// hardware/gpio.h dropped (this pass): the only two gpio_* call sites left
// in this file were max31856_init()'s one-time ~CS idle-high setup and the
// ~FAULT input-with-pullup setup + its steady-state read -- neither is the
// per-transfer CS bit-bang (that already lives entirely inside
// hal_spi_pico.c/spi_owner.c, "CS is bit-banged by spi_owner.c itself", see
// max31856_bus_init()'s dev_cfg.hw_cs = HAL_CS_NONE comment below). Both are
// exactly the shapes hal_gpio.h's own header comment names this file for
// ("MAX31856 CS + fault-input pins (max31856.c)") and hal_gpio_pico.c is
// already wired into hwabstraction_pico -- plain migration debt, not a
// pin-level holdout. Order/polarity unchanged: put-then-set_dir for CS
// (latch-before-direction, matches hal_gpio_init_out()'s contract exactly).
#include "max31856.h"

#include <math.h>
#include <string.h>

#include "hal_gpio.h"

#include "board_pins.h" // SAFTYFW_PIN_SPI0_{SCK,MOSI,MISO,CS0}
#include "hal_spi.h"
#include "max31856_decode.h"
#include "max31856_fault_pin_policy.h" // max31856_fault_pin_asserted() -- ~FAULT pin polarity
#include "max31856_tc_range_policy.h" // max31856_cr1_readback_check() -- Part B CR1 readback verification
#include "max31856_tc_type_policy.h"

// 4 MHz / SPI mode 1 -- matches spi_owner.c's SPI_OWNER_BAUDRATE_HZ /
// SPI_CPOL_0+SPI_CPHA_1 exactly; hal_spi_pico.c's hal_spi_device_attach()
// validates a device config against these same hardwired values (see that
// file's INTERFACE MISMATCH note 2) and fails closed on any mismatch.
#define MAX31856_SPI_CLOCK_HZ 4000000u

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

// Part B (max31856_tc_range_policy.h): whether the LAST max31856_configure()
// call verified, by reading CR1 back, that the part actually accepted the
// tc_type it was asked to write. Starts false and stays false across every
// early-return path in max31856_configure() below -- only the single
// success path at the very end of that function can set it true -- so a
// caller can never observe a stale "verified" from a PRIOR configure() call
// once a new one has started, even if the new call itself fails partway.
static bool s_tc_type_verified = false;

// hal_spi.h handles for the one MAX31856 device on SPI0 -- see this file's
// top comment. hal_spi_pico.c's backing spi_owner.c is itself a process-wide
// singleton, so these are the only bus/device instances that will ever
// exist on this board.
static hal_spi_bus_t s_spi_bus;
static hal_spi_device_t s_spi_dev;

// No separate "is s_spi_dev ready" flag here -- that would be a third
// tracker of the same one fact hal_spi_pico.c's own
// hal_spi_pico_device_impl_t::attached and spi_owner.c's s_initialized
// already hold (see hal_spi_pico.c's struct comment for that pairing).
// max31856_spi_device_for_test() is test-only: every real hal_spi_* call a
// test makes through the returned pointer already re-checks the device's
// own magic+attached tag (hal_spi_pico.c) or fake_spi.c's equivalent tag
// and fails HAL_NOT_READY on its own if max31856_bus_init() has not (yet,
// or successfully) run -- adding a fourth flag here to pre-empt that would
// only be one more copy of the same fact to keep in sync, not a safety
// requirement.
hal_spi_device_t *max31856_spi_device_for_test(void)
{
    return &s_spi_dev;
}

bool max31856_bus_init(void)
{
    hal_spi_bus_cfg_t bus_cfg = { 0 };
    bus_cfg.sck_pin = SAFTYFW_PIN_SPI0_SCK;
    bus_cfg.mosi_pin = SAFTYFW_PIN_SPI0_MOSI;
    bus_cfg.miso_pin = SAFTYFW_PIN_SPI0_MISO;
    // HAL Phase 1b, "close the upward include": hal_spi_pico.c no longer
    // hardcodes/validates CS0 against board_pins.h itself -- this is now the
    // one place (inside SaftyFW, which already owns board_pins.h) that reads
    // SAFTYFW_PIN_SPI0_CS0 and forwards it down as an ordinary cfg field.
    bus_cfg.cs0_pin = SAFTYFW_PIN_SPI0_CS0;
    // Every other hal_spi_bus_cfg_t field (queue_len/task_priority/
    // stack_depth/core_id/dma_use_psram/async_flush/max_transfer_sz/
    // dma_chan) is ESP-owner-task/DMA sizing hal_spi_pico.c does not use --
    // see that file's INTERFACE MISMATCH note 1 -- left zero-initialized.
    if (hal_spi_bus_init(&s_spi_bus, 0, &bus_cfg) != HAL_OK) {
        return false;
    }

    hal_spi_device_cfg_t dev_cfg = { 0 };
    dev_cfg.clock_hz = MAX31856_SPI_CLOCK_HZ;
    dev_cfg.mode = HAL_SPI_MODE_1;
    dev_cfg.hw_cs = HAL_CS_NONE; // CS is bit-banged by spi_owner.c itself
    dev_cfg.cs_pin = SAFTYFW_PIN_SPI0_CS0;
    if (hal_spi_device_attach(&s_spi_bus, &s_spi_dev, &dev_cfg) != HAL_OK) {
        return false;
    }
    return true;
}

static bool max31856_write_u8(uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = { MAX31856_WRITE_ADDR(reg), value };
    return hal_spi_transfer_polling(&s_spi_dev, tx, sizeof(tx), NULL, 0, 0) == HAL_OK;
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

    if (hal_spi_transfer_polling(&s_spi_dev, tx, len + 1u, rx, len + 1u, 0) != HAL_OK) {
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
    // hal_gpio_init_out() sets the level BEFORE switching direction by
    // contract (hal_gpio.h's latch-before-direction rule), matching the
    // put-then-set_dir order this file used directly before this migration.
    if (hal_gpio_init_out((int)cs_gpio, true) != HAL_OK) {
        return false;
    }
    s_cs_gpio = cs_gpio;

    // ~FAULT: input with the internal pull-up. THERMOCOUPLE.md section 1
    // says R1 (external, 10k) already does this job on this board; the
    // internal pull-up is redundant belt-and-braces, matching KilnFW's own
    // reasoning for its (non-redundant) case.
    if (hal_gpio_init_in((int)fault_gpio, HAL_GPIO_PULL_UP) != HAL_OK) {
        return false;
    }
    s_fault_gpio = (int)fault_gpio;

    // Power-on defaults (datasheet Table 6): CR0 = 00h, CR1 = 03h (type K,
    // 1 sample), MASK = FFh. No register written here -- max31856_configure()
    // rewrites CR0/CR1/MASK unconditionally.
    s_cr0_shadow = 0x00u;
    s_cr1_shadow = 0x03u;
    s_tc_type_verified = false; // no configure() has run yet on this bring-up

    s_initialized = true;
    return true;
}

bool max31856_configure(uint8_t tc_type)
{
    // Cleared unconditionally at entry, before any of the early-return
    // guards below -- see this flag's own declaration comment above for why
    // that is what makes every failure path (including ones added later)
    // correct by construction rather than by remembering to set it false at
    // each new return site.
    s_tc_type_verified = false;

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

    // Part B (max31856_tc_range_policy.h): read CR1 back once, here at
    // configure-time, not on thermo_task's hot per-sample path -- this
    // function already runs only at boot and on a detected part reset
    // (THERMOCOUPLE.md's completion checklist), never per-conversion, so one
    // extra 2-byte SPI transfer here costs nothing a caller would notice,
    // while adding it to max31856_read()'s burst would cost one on every
    // single sample forever for a fact that changes at most once per
    // configure() call.
    //
    // A failed readback transfer is deliberately treated the same as a
    // MISMATCH/DEAD_BUS result (s_tc_type_verified stays false), not as a
    // reason to fail this whole function: every CR0/CR1/MASK write above
    // already succeeded, so the part IS configured as far as this function
    // can tell -- only the CONFIRMATION step failed, which is exactly the
    // "cannot prove the type" state this getter exists to report honestly,
    // not a reason to also throw away three good writes and report a boot
    // probe failure that did not actually happen.
    uint8_t cr1_readback = 0;
    if (max31856_read_burst(MAX31856_REG_CR1, &cr1_readback, 1)) {
        s_tc_type_verified =
            max31856_cr1_readback_check(tc_type, cr1_readback) == MAX31856_CR1_READBACK_MATCH;
    }

    return true;
}

bool max31856_tc_type_verified(void)
{
    return s_initialized && s_tc_type_verified;
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

    out->fault_pin_asserted =
        (s_fault_gpio >= 0) && max31856_fault_pin_asserted(hal_gpio_get(s_fault_gpio));

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
