// MAX31856 precision thermocouple-to-digital converter driver (SPI).
//
// Three of these live on the thermocouple daughterboard (U2/U3/U4) and reach
// the ESP32-S3 through J6: one shared SPI bus (SCLK 12 / MOSI 11 / MISO 13)
// with a per-part chip select on GPIO14/17/18, and a per-part ~FAULT output on
// GPIO38/47/48. See docs/HARDWARE.md -- it is the authority on the wiring, and
// App/drivers/settings.h turns those numbers into the THERMO_* macros this
// driver's start-up helper uses.
//
// The shape of this driver follows from the shape of the hardware:
//
//   - ONE MAX31856BusClass owns the SPI bus and the spi_owner_t worker task
//     that serializes every transfer on it. All three parts share it, so all
//     three share one FIFO-ordered queue and can never talk over each other.
//   - THREE MAX31856Class instances, one per part, each holding its own
//     spi_device_handle_t (so each gets its own device config on the shared
//     bus), its own ~CS and ~FAULT GPIO, its own channel index, its own
//     register shadows and its own mutex.
//
// Everything goes through spi_owner_transfer(); this driver never calls
// spi_device_transmit() itself. That is what keeps the display (a fourth
// device on the same bus, at a different mode and a much higher clock) from
// interleaving a transaction into the middle of one of ours.
//
// ~DRDY IS NOT VISIBLE FROM HERE. On this board each part's ~DRDY lands on the
// SX1509 I/O expander (IO8/IO9/IO10), not on a GPIO, so this driver cannot
// sample it without owning an I2C device it has no business owning. Instead it
// accepts an optional callback (MAX31856_set_drdy_provider) that whoever does
// own the expander can install; with no provider installed the driver falls
// back to a conservative elapsed-time heuristic for the "is this reading
// stale" flag. See MAX31856_drdy_provider_t below for exactly what each mode
// can and cannot promise.
//
// Datasheet references throughout name the registers as the datasheet does
// (CR0, CR1, MASK, CJHF/CJLF, LTHFTH/L, LTLFTH/L, CJTO, CJTH/CJTL,
// LTCBH/M/L, SR) -- see datasheets/ThermocoupleBoard_Sensor_Temperature/
// MAX31856.pdf, "Internal Registers" (Table 6) onward.
#ifndef MAX31856_H
#define MAX31856_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_spi_owner.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "max31856_codec.h"
#include "uart_task_ids.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Number of parts on the daughterboard. Deliberately the same symbol the wire
 * protocol uses (uart_task_ids.h) rather than a second 3 that could drift. */
#define MAX31856_CHANNEL_COUNT THERMO_CHANNEL_COUNT

/* --- Register addresses (datasheet Table 6, "Register Memory Map") ---
 * Read at 0Xh, write at 8Xh: "The registers are accessed using the 0Xh
 * addresses for reads and the 8Xh addresses for writes." MAX31856_WRITE_ADDR()
 * is that +0x80. The address auto-increments while ~CS stays low, which is
 * what makes the burst read in MAX31856_read() a single transaction. */
#define MAX31856_REG_CR0     0x00u /* config 0: CMODE/1SHOT/OCFAULT/CJ/FAULT/FAULTCLR/50-60Hz */
#define MAX31856_REG_CR1     0x01u /* config 1: AVGSEL[2:0], TC TYPE[3:0] */
#define MAX31856_REG_MASK    0x02u /* fault mask -- which faults may drive ~FAULT */
#define MAX31856_REG_CJHF    0x03u /* cold-junction high fault threshold, int8, 1 degC/LSB */
#define MAX31856_REG_CJLF    0x04u /* cold-junction low fault threshold,  int8, 1 degC/LSB */
#define MAX31856_REG_LTHFTH  0x05u /* linearized TC high threshold MSB, int16, 0.0625 degC/LSB */
#define MAX31856_REG_LTHFTL  0x06u
#define MAX31856_REG_LTLFTH  0x07u /* linearized TC low threshold MSB */
#define MAX31856_REG_LTLFTL  0x08u
#define MAX31856_REG_CJTO    0x09u /* cold-junction offset, int8, 0.0625 degC/LSB */
#define MAX31856_REG_CJTH    0x0Au /* cold-junction temperature MSB */
#define MAX31856_REG_CJTL    0x0Bu /* cold-junction temperature LSB */
#define MAX31856_REG_LTCBH   0x0Cu /* linearized TC temperature, byte 2 (MSB) */
#define MAX31856_REG_LTCBM   0x0Du /* byte 1 */
#define MAX31856_REG_LTCBL   0x0Eu /* byte 0 (LSB); low 5 bits unused */
#define MAX31856_REG_SR      0x0Fu /* fault status */
#define MAX31856_REG_COUNT   0x10u

#define MAX31856_WRITE_ADDR(reg) ((uint8_t)((reg) | 0x80u))

/* Longest burst the debug READ_REG subcommand allows (uart_task_ids.h caps
 * len at 16, which is also the whole register map). */
#define MAX31856_MAX_BURST_LEN 16u

/* --- CR0 bits (datasheet "Register 00h/80h: Configuration 0 Register") --- */
#define MAX31856_CR0_CMODE     0x80u /* 1 = automatic conversion (~100ms), 0 = normally off */
#define MAX31856_CR0_ONESHOT   0x40u /* self-clearing; conversion starts when ~CS rises */
#define MAX31856_CR0_OCFAULT1  0x20u
#define MAX31856_CR0_OCFAULT0  0x10u
#define MAX31856_CR0_CJ_DISABLE 0x08u /* 1 = internal cold-junction sensor off */
#define MAX31856_CR0_FAULT_INT 0x04u /* 0 = comparator mode (default), 1 = interrupt mode */
#define MAX31856_CR0_FAULTCLR  0x02u /* self-clearing; no effect in comparator mode */
#define MAX31856_CR0_FILTER_50HZ 0x01u /* 0 = reject 60Hz, 1 = reject 50Hz */

/* CR0.OCFAULT[1:0] -- open-circuit detection timing (datasheet Table 4). The
 * detection cost depends on the source resistance of the thermocouple loop;
 * MODE1 is right for the <5k thermocouples this kiln uses and adds ~13ms to a
 * conversion with the cold-junction sensor enabled. */
#define MAX31856_OC_DISABLED 0x00u
#define MAX31856_OC_MODE1    0x01u /* Rs < 5k */
#define MAX31856_OC_MODE2    0x02u /* 5k < Rs < 40k, time constant < 2ms */
#define MAX31856_OC_MODE3    0x03u /* 5k < Rs < 40k, time constant > 2ms */

/* --- MASK register bits (datasheet "Register 02h/82h") ---
 * A 1 MASKS the fault, i.e. stops it driving the ~FAULT pin; the bit still
 * shows up in SR either way. Factory default is FFh (everything masked, so
 * ~FAULT never asserts), which would make the three ~FAULT GPIOs on this board
 * useless -- hence MAX31856_DEFAULT_FAULT_MASK below. */
#define MAX31856_MASK_OPEN    0x01u
#define MAX31856_MASK_OVUV    0x02u
#define MAX31856_MASK_TCLOW   0x04u
#define MAX31856_MASK_TCHIGH  0x08u
#define MAX31856_MASK_CJLOW   0x10u
#define MAX31856_MASK_CJHIGH  0x20u
#define MAX31856_MASK_ALL     0x3Fu /* bits 7:6 are reserved */

/* --- SR (fault status, 0Fh) bits ---
 * SR shares its low 6 bit positions/names with the MASK register above
 * (OPEN/OVUV/TCLOW/TCHIGH/CJLOW/CJHIGH) -- MAX31856_MASK_* may be read
 * directly against MAX31856Reading::fault_status for those six. SR has two
 * bits MASK does not: TCRANGE (0x40) and CJRANGE (0x80), "Register 0Fh: Fault
 * Status Register" in the datasheet -- these can never be masked off ~FAULT,
 * since MASK (02h) only has bits for the low six. No existing #define covered
 * them before this; added here so a fault-status reader (e.g.
 * ui_page_thermo_faults.c) can name all eight bits instead of six named ones
 * plus two bare hex literals. */
#define MAX31856_FAULT_TCRANGE 0x40u
#define MAX31856_FAULT_CJRANGE 0x80u

/* What MAX31856_start_all writes into MASK: open-circuit and over/undervoltage
 * unmasked (a broken thermocouple or a miswired input is exactly what the
 * ~FAULT line exists to report, and neither is a threshold the operator
 * configured), the four threshold faults left masked until somebody actually
 * sets thresholds -- they power up at full scale, so unmasking them would only
 * ever produce false quiet, and once set they are better handled by the
 * control loop reading SR than by an edge on a shared-ish GPIO. */
#define MAX31856_DEFAULT_FAULT_MASK \
    ((uint8_t)(MAX31856_MASK_ALL & ~(MAX31856_MASK_OPEN | MAX31856_MASK_OVUV)))

/* --- Fixed-point scales (datasheet register bit-weight tables) ---
 * MAX31856_TC_THRESHOLD_C_PER_LSB, MAX31856_CJ_TEMP_C_PER_LSB and
 * MAX31856_TC_TEMP_C_PER_LSB live in max31856_codec.h (included above),
 * alongside the decode/encode functions that use them -- see that header for
 * why the split. CJTO's offset scale below is not part of that codec (it is
 * a plain int8, not one of the three fixed-point conversions moved out) so it
 * stays here with everything else this header owns. */
/* CJTO: sign + 2^2..2^-4 => 1/16 degC per LSB, so +-8 degC of range. */
#define MAX31856_CJ_OFFSET_C_PER_LSB    0.0625f
#define MAX31856_CJ_OFFSET_MIN_C        (-8.0f)
#define MAX31856_CJ_OFFSET_MAX_C        (7.9375f)

/* Optional hook for observing ~DRDY, which on this board is an SX1509 pin and
 * so is unreachable from this driver (see the file header). Return true if
 * *out_asserted was filled in (true = ~DRDY low = a conversion result the
 * driver has not read yet); return false if the state is unknown -- an I2C
 * error, the expander not started yet -- and the driver will fall back to its
 * elapsed-time heuristic rather than guessing.
 *
 * Called from whatever task calls MAX31856_read(), with that channel's mutex
 * held, so it must not call back into this driver. */
typedef bool (*MAX31856_drdy_provider_t)(uint8_t channel, bool *out_asserted, void *ctx);

/* Everything CR0/CR1 hold that a caller might reasonably want to choose.
 * MAX31856_config_default() fills this with the kiln's settings; the UART
 * bridge's CONFIG_CHANNEL only carries the first four fields, which is what
 * MAX31856_config_channel() exists for. */
typedef struct {
    uint8_t tc_type;             /* CR1.TC[3:0]     -- THERMO_TC_* */
    uint8_t avg_mode;            /* CR1.AVGSEL[2:0] -- THERMO_AVG_* */
    bool filter_50hz;            /* CR0.50/60Hz: false = reject 60Hz */
    bool auto_convert;           /* CR0.CMODE: true = continuous ~100ms conversions */
    uint8_t oc_detect;           /* CR0.OCFAULT[1:0] -- MAX31856_OC_* */
    bool cj_sensor_disabled;     /* CR0.CJ: true only if feeding CJ from elsewhere */
    bool interrupt_fault_mode;   /* CR0.FAULT: false = comparator (self-clearing) mode */
} MAX31856Config;

/* One channel's worth of measurement, laid out to make the UART bridge's
 * 12-byte-per-channel READ record a straight copy.
 *
 * The failure contract matters more than the happy path: if the SPI transfer
 * failed, both temperatures are NaN and spi_failed is true. They are never
 * zero and never the previous reading -- a stale-but-plausible number from a
 * kiln thermocouple is how elements get left on.
 *
 * The same rule applies to a transfer that succeeded but carried a number the
 * part itself says is meaningless: with OPEN, OVUV or TCRANGE set in
 * fault_status the hot-junction register still holds a plausible temperature
 * (open input bias, a pre-fault value frozen when conversions were suspended,
 * or a reading past the linearization range), so tc_temperature_c is reported
 * as NaN with fault_status saying why. CJRANGE does the same to
 * cj_temperature_c -- AND to tc_temperature_c (2026-08-27 fix): LTCB is the
 * LINEARIZED, cold-junction-COMPENSATED hot-junction temperature, computed in
 * hardware from whatever the cold junction measured, fault or not, so an
 * out-of-range cold junction taints the hot-junction number by an unknown
 * compensation error rather than leaving it merely stale. A plausible WRONG
 * number is worse than a NaN here -- every consumer already handles an
 * invalid reading, none can detect a quietly wrong one -- so CJRANGE NaNs
 * both temperatures, not just its own. See max31856_codec.h's
 * max31856_fault_invalidates_tc()/_cj(), which this contract is now pinned
 * to. spi_failed stays false in all of these cases -- the bus worked, the
 * sensor did not. */
typedef struct {
    uint8_t channel;
    float tc_temperature_c;   /* linearized, cold-junction-compensated; NaN if invalid */
    /* Whatever CJTH:CJTL holds, which is the value the part actually used for
     * compensation: the internal sensor's reading plus CJTO, or -- if the
     * internal sensor was disabled -- the last value written there. NaN only
     * when the read failed. */
    float cj_temperature_c;
    uint8_t fault_status;     /* SR register, THERMO_FAULT_* bits */
    bool fault_pin_asserted;  /* ~FAULT GPIO is low right now */
    bool spi_failed;          /* the transfer failed; both temperatures are NaN */
    bool stale;               /* no new conversion since the previous read (see below) */
    /* Milliseconds since this channel last produced a usable conversion (a
     * new result, read successfully, with a non-NaN hot-junction
     * temperature). MAX31856_READING_AGE_UNKNOWN when this driver has never
     * seen one for this channel, or when the reading was fabricated by a
     * layer that has no such history (a failed thermo_owner command).
     *
     * This is the number a USER-FACING layer should judge freshness by --
     * see KILN_TEMP_STALE_AGE_MS below. */
    uint32_t age_ms;
} MAX31856Reading;

/* "No new conversion this poll" vs "this number is too old to trust" are two
 * different questions and the driver only answers the first one.
 *
 * MAX31856Reading::stale is the first: it is true whenever the caller polled
 * faster than the part converts (or, with a DRDY provider, whenever ~DRDY
 * says there is nothing new). That is honest and useful INSIDE the firmware
 * -- safety_link.c uses it to count real conversions -- but it says nothing
 * about the value's usefulness: a temperature from 200 ms ago is perfectly
 * good and still gets stale = true.
 *
 * MAX31856Reading::age_ms is the second. Every user-facing surface (the web
 * /api/status JSON, the LCD pages, the UART status record the PC tools show)
 * must call a reading stale only when age_ms exceeds this single shared
 * threshold, so the three UIs always agree with each other. 10 s is a few
 * times the slowest configured conversion, so a healthy kiln never trips it
 * and a genuinely wedged sensor trips it quickly. */
#define KILN_TEMP_STALE_AGE_MS 10000u

/* age_ms when no good conversion has ever been seen -- deliberately far past
 * KILN_TEMP_STALE_AGE_MS so "never read" and "too old" need no special case
 * at the UI layer. */
#define MAX31856_READING_AGE_UNKNOWN UINT32_MAX

typedef struct MAX31856Class MAX31856Class;

/* The shared bus: SPI host, the serializing owner task, and the registry of
 * channels attached to it (so the UART bridge can go from a channel number on
 * the wire to a driver instance without keeping its own array). */
typedef struct {
    spi_host_device_t host;
    spi_owner_t owner;
    bool owner_initialized;
    /* True only if THIS driver called spi_bus_initialize(). The display shares
     * the bus; whoever gets there first initializes it and only that one frees
     * it in deinit. */
    bool bus_owned;
    bool initialized;
    MAX31856Class *channels[MAX31856_CHANNEL_COUNT];
    MAX31856_drdy_provider_t drdy_provider;
    void *drdy_ctx;
} MAX31856BusClass;

struct MAX31856Class {
    MAX31856BusClass *bus;
    spi_device_handle_t dev;
    int cs_gpio;
    int fault_gpio;          /* -1 if this channel's ~FAULT is not wired */
    uint8_t channel;         /* 0..2, as used on the wire */
    bool initialized;

    /* Register shadows. CR0/CR1 are read-modify-written constantly (one-shot
     * trigger, conversion mode, fault clear) and the part gives no way to do a
     * partial write, so the driver has to remember what it last wrote rather
     * than read-modify-write over SPI every time. MASK is shadowed for the
     * READ_FAULTS reply, which reports it alongside SR. */
    uint8_t cr0_shadow;
    uint8_t cr1_shadow;
    uint8_t mask_shadow;

    /* Freshness bookkeeping for MAX31856Reading::stale when no DRDY provider
     * is installed -- see MAX31856_read(). result_pending means a conversion
     * has been started (automatic mode running, or a one-shot triggered) whose
     * result has not been read yet; next_result_due_tick is the earliest tick
     * at which that result can possibly exist. FreeRTOS ticks rather than
     * esp_timer microseconds: the numbers involved are >= 100ms, tick
     * resolution is far finer than that, and it keeps this driver's component
     * dependencies to the ones every component already has. */
    bool result_pending;
    TickType_t next_result_due_tick;

    /* Tick of the last conversion this channel actually produced a usable
     * number from, for MAX31856Reading::age_ms. Separate from the two fields
     * above on purpose: those track what is COMING, this tracks what was
     * last GOT. has_good_result is false until the first one. */
    bool has_good_result;
    TickType_t last_good_tick;

    /* Last SR value that produced a WARN, so a permanently open thermocouple
     * logs once instead of once per auto-report tick. */
    uint8_t logged_fault_status;

    SemaphoreHandle_t lock; /* guards the shadows and any multi-transfer sequence */
};

/* --- Bring-up ---------------------------------------------------------- */

/* Single-call bootstrap for app_main, in the same spirit as SX1509_start() and
 * ILI9488_start():
 *
 *   static MAX31856BusClass thermo_bus;
 *   static MAX31856Class thermo_ch[MAX31856_CHANNEL_COUNT];
 *   MAX31856_start_all(&thermo_bus, thermo_ch);
 *
 * Initializes the bus from the KILN_SPI_* settings, attaches all three
 * channels at the THERMO_CSn / THERMO_FAULTn pins, applies
 * MAX31856_config_default() to each, and verifies each part by reading CR1
 * back. Channels that fail are logged as errors and left un-initialized; the
 * others still work, because losing one thermocouple is not a reason to have
 * no thermocouples. Returns ESP_OK only if all three came up, otherwise the
 * last error (the caller is expected to log and carry on, not to abort boot).
 *
 * `channels` must point at MAX31856_CHANNEL_COUNT structs that outlive the
 * driver -- statics, like the rest of this codebase; nothing here is
 * heap-allocated after init. */
esp_err_t MAX31856_start_all(MAX31856BusClass *bus, MAX31856Class *channels);

/* Brings up the SPI bus and the owner task. If some other driver (the display)
 * already initialized this host, that is not an error: the bus is reused and
 * only its original owner frees it. */
esp_err_t MAX31856_bus_init(MAX31856BusClass *bus,
                            spi_host_device_t host,
                            int sclk_gpio,
                            int mosi_gpio,
                            int miso_gpio);
esp_err_t MAX31856_bus_deinit(MAX31856BusClass *bus);

/* Attaches one part to an initialized bus: adds an SPI device (mode 1, MSB
 * first, THERMO_SPI_CLOCK_HZ -- capped at 4 MHz, not merely defaulted there;
 * MAX31856.c explains why), drives ~CS high, configures ~FAULT as an
 * input with the internal pull-up (the daughterboard's ~FAULT is an open-drain
 * output, so nothing else pulls it up), and seeds the register shadows from
 * the part's power-on defaults. Does not write any register -- call
 * MAX31856_configure() (or let MAX31856_start_all do it) for that.
 *
 * fault_gpio may be -1 to say "not wired"; then fault_pin_asserted is always
 * reported false. */
esp_err_t MAX31856_init(MAX31856Class *ch,
                        MAX31856BusClass *bus,
                        uint8_t channel,
                        int cs_gpio,
                        int fault_gpio);
esp_err_t MAX31856_deinit(MAX31856Class *ch);

/* Channel lookup for the UART bridge: NULL for an out-of-range index or a
 * channel that never came up, which is exactly the "report this channel as
 * failed" case. */
MAX31856Class *MAX31856_bus_channel(MAX31856BusClass *bus, uint8_t channel);

/* Installs (or, with fn = NULL, removes) the ~DRDY observer described above.
 * Affects every channel on the bus. */
esp_err_t MAX31856_set_drdy_provider(MAX31856BusClass *bus,
                                     MAX31856_drdy_provider_t fn,
                                     void *ctx);

/* --- Configuration ----------------------------------------------------- */

/* Type K, one sample, 60Hz rejection, open-circuit detection mode 1,
 * comparator fault mode, and automatic conversion iff
 * CONFIG_KILNCTL_THERMO_DEFAULT_AUTO_CONVERT. */
void MAX31856_config_default(MAX31856Config *cfg);

/* Rewrites CR0 and CR1. Two datasheet constraints are handled here rather than
 * being pushed onto the caller:
 *   - "Change the notch frequency only while in the Normally Off mode" (CR0
 *     50/60Hz bit), and
 *   - "The Thermocouple Voltage Conversion Averaging Mode settings should not
 *     be changed while conversions are taking place" (CR1 AVGSEL).
 * so this always writes CR0 with CMODE = 0 first, then CR1, then CR0 again
 * with the requested CMODE -- i.e. it stops conversions, reconfigures, and
 * restores the conversion mode, regardless of which fields actually changed. */
esp_err_t MAX31856_configure(MAX31856Class *ch, const MAX31856Config *cfg);

/* Exactly the four fields the wire's CONFIG_CHANNEL subcommand carries, over
 * MAX31856_config_default() for the rest. */
esp_err_t MAX31856_config_channel(MAX31856Class *ch,
                                  uint8_t tc_type,
                                  uint8_t avg_mode,
                                  bool filter_50hz,
                                  bool auto_convert);

/* Reconstructs the current configuration from the CR0/CR1 shadows. */
esp_err_t MAX31856_get_config(MAX31856Class *ch, MAX31856Config *out_cfg);

/* CR0.CMODE only. Cheaper than a full reconfigure and safe at any time (unlike
 * the filter and averaging bits). */
esp_err_t MAX31856_set_conversion_mode(MAX31856Class *ch, bool automatic);

/* Thermocouple thresholds in degC (rounded to the part's 0.0625 degC/LSB and
 * clamped to the int16 range) and cold-junction thresholds in whole degC
 * (CJHF/CJLF are plain int8, 1 degC/LSB). Writes each pair as one
 * auto-incrementing burst. */
esp_err_t MAX31856_set_thresholds(MAX31856Class *ch,
                                  float tc_high_c,
                                  float tc_low_c,
                                  int8_t cj_high_c,
                                  int8_t cj_low_c);

/* CJTO, in degC. Values outside +-8 degC are clamped (with a warning) rather
 * than rejected, since the register simply cannot express them. */
esp_err_t MAX31856_set_cj_offset(MAX31856Class *ch, float offset_c);

/* Which faults are allowed to drive the ~FAULT pin -- MAX31856_MASK_* bits,
 * 1 = masked. Does not affect SR. */
esp_err_t MAX31856_set_fault_mask(MAX31856Class *ch, uint8_t mask);

/* --- Measurement ------------------------------------------------------- */

/* Sets CR0.1SHOT. The part starts the conversion when ~CS rises at the end of
 * this transaction and clears the bit itself; the result is not returned here.
 * Poll MAX31856_read() after MAX31856_conversion_time_ms(). No-op-ish but
 * still allowed in automatic mode, where conversions are already running. */
esp_err_t MAX31856_trigger_one_shot(MAX31856Class *ch);

/* One burst read of CJTH..SR (0x0A..0x0F, six registers, one transaction) ->
 * cold-junction temperature, linearized thermocouple temperature and fault
 * status. Always fills *out, including on failure (NaN temperatures with
 * spi_failed set), so a caller serializing to the wire never has to invent a
 * value. Returns the SPI error as well.
 *
 * Open-circuit / over-under-voltage / out-of-range faults are logged at WARN
 * with the channel number, once per change of SR rather than once per call.
 *
 * The `stale` flag: with a DRDY provider installed it is the truth -- ~DRDY low
 * means a result this driver has not read yet (and reading CJTH/CJTL here
 * releases it high again). Without one it is a conservative estimate from the
 * elapsed time since the last read versus the configured conversion time, so
 * it can only ever say "too soon for this to be new", never detect a part that
 * has silently stopped converting. */
esp_err_t MAX31856_read(MAX31856Class *ch, MAX31856Reading *out);

/* Reads every initialized channel on the bus into out[0..count-1]; channels
 * that are not initialized are skipped, not faked. Returns the first error
 * encountered (the corresponding entry still carries spi_failed/NaN). */
esp_err_t MAX31856_read_all(MAX31856BusClass *bus,
                            MAX31856Reading *out,
                            size_t max_readings,
                            size_t *out_count);

/* SR and the MASK shadow, for the READ_FAULTS reply. Either pointer may be
 * NULL. */
esp_err_t MAX31856_read_faults(MAX31856Class *ch, uint8_t *out_sr, uint8_t *out_mask);

/* Pulses CR0.FAULTCLR. Per the datasheet this "has no effect in comparator
 * mode" -- the mode this driver uses by default, where the fault bits track
 * the condition and clear themselves. Kept because the wire contract has the
 * subcommand and because interrupt mode is selectable. */
esp_err_t MAX31856_clear_faults(MAX31856Class *ch);

/* Current level of the ~FAULT GPIO: true when asserted (low). False if this
 * channel has no ~FAULT pin. Does not touch SPI. */
bool MAX31856_fault_pin_asserted(const MAX31856Class *ch);

/* How long the currently configured conversion takes, in ms -- tCONV plus the
 * averaging adder, from the datasheet's CR1.AVGSEL notes. one_shot selects the
 * (longer) 1-shot / first-auto-conversion figure over the steady-state
 * automatic one. Useful for pacing a poll loop or waiting out a one-shot. */
uint32_t MAX31856_conversion_time_ms(MAX31856Class *ch, bool one_shot);

/* --- Raw register access (the wire's debug subcommands) ---------------- */

/* Burst read of `len` (1..MAX31856_MAX_BURST_LEN) registers starting at `reg`,
 * relying on the part's address auto-increment. `reg` is the read (0Xh)
 * address; do not pre-set the 0x80 write bit. */
esp_err_t MAX31856_read_reg(MAX31856Class *ch, uint8_t reg, uint8_t *buf, size_t len);

/* Single register write. `reg` is the read (0Xh) address; the 0x80 write bit
 * is added here. Writing CR0/CR1/MASK through this path updates the matching
 * shadow, so a debug poke can't desynchronize the driver from the part. */
esp_err_t MAX31856_write_reg(MAX31856Class *ch, uint8_t reg, uint8_t value);

#ifdef __cplusplus
}
#endif

#endif // MAX31856_H
