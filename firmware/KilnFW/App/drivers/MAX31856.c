#include "MAX31856.h"

#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "max31856_codec.h"
#include "settings.h"

static const char *TAG = "MAX31856";

/* SPI mode 1 = CPOL 0 / CPHA 1. The datasheet's Table 5 says the part "auto-
 * matically accommodates either clock polarity by sampling SCLK when CS
 * becomes active", but is unambiguous that "CPHA bit polarity must be set to
 * 1" -- so mode 1 or mode 3, and mode 1 is what the rest of this board's
 * documentation assumes (docs/HARDWARE.md). Data is MSB first, which is the
 * ESP-IDF default (no SPI_DEVICE_BIT_LSBFIRST flag). */
#define MAX31856_SPI_MODE 1

/* The signals cross a ribbon cable and a connector to the daughterboard, so
 * tell the driver to expect some round-trip delay on MISO rather than letting
 * it assume an ideal board and sample too early. THERMO_SPI_CLOCK_HZ is
 * Kconfig-backed and capped at 4 MHz -- see below. */
#define MAX31856_SPI_INPUT_DELAY_NS 50

/* 4 MHz is a ceiling, not a default. The part is rated to 5 MHz, so the limit
 * is not the silicon: it is the SimFW bench fixture, whose PIO/DMA MAX31856
 * slave emulation must have the first response byte ready by the master's MISO
 * sample point -- roughly 250 ns at 4 MHz, roughly 200 ns at 5 MHz. The
 * implemented DMA-fed path takes about 150-215 ns at the RP2040's stock 125 MHz
 * sysclk: margin at 4 MHz, essentially none at 5 MHz. Derivation is in
 * firmware/SimFW/docs/SPI_ACCESS_AUDIT.md section 9.
 *
 * Missing the deadline does not raise a fault. It shifts an entire register
 * burst by one byte position and yields plausible-looking wrong temperatures,
 * which is exactly why the cap is enforced here as well as by the Kconfig
 * `range` -- a hand-edited sdkconfig would otherwise slip past menuconfig.
 *
 * This says nothing about DISPLAY_SPI_CLOCK_HZ. The ILI9488 shares the bus but
 * is a separate spi_device_interface_config_t with its own, much higher clock;
 * it is not emulated by the fixture and is not constrained by this. */
#define MAX31856_SPI_MAX_CLOCK_HZ 4000000
_Static_assert(THERMO_SPI_CLOCK_HZ <= MAX31856_SPI_MAX_CLOCK_HZ,
               "THERMO_SPI_CLOCK_HZ exceeds the 4 MHz cap the SimFW slave "
               "emulation's first-byte deadline requires -- see "
               "SimFW/docs/SPI_ACCESS_AUDIT.md section 9");

/* One transaction is at most an address byte plus a 16-register burst. That
 * fits the SPI peripheral's FIFO with room to spare, so the bus is opened with
 * DMA disabled and these can be ordinary stack buffers with no DMA-capable
 * allocation or alignment requirement. */
#define MAX31856_MAX_XFER_LEN (1u + MAX31856_MAX_BURST_LEN)

/* Conversion timing, from the Electrical Characteristics tCONV row and the
 * CR1.AVGSEL notes. The 1-shot numbers are the datasheet's MAX column
 * (155ms/185ms, not the 143/169 typicals) because the only thing this is used
 * for is deciding when a result can possibly exist -- being early there would
 * mean reporting a fresh reading that is actually the previous one. The
 * automatic-mode figure is CR0's "every 100ms (nominal)" with a little margin
 * for the 50Hz case. */
#define MAX31856_TCONV_ONESHOT_60HZ_MS 155u
#define MAX31856_TCONV_ONESHOT_50HZ_MS 185u
#define MAX31856_TCONV_AUTO_60HZ_MS    100u
#define MAX31856_TCONV_AUTO_50HZ_MS    110u
/* Per extra averaged sample: (samples - 1) x 33.33ms/40ms for a 1-shot or the
 * first automatic conversion, x 16.67ms/20ms for subsequent automatic ones.
 * Rounded up to whole ms, again so the estimate never runs early. */
#define MAX31856_AVG_ADDER_ONESHOT_60HZ_MS 34u
#define MAX31856_AVG_ADDER_ONESHOT_50HZ_MS 40u
#define MAX31856_AVG_ADDER_AUTO_60HZ_MS    17u
#define MAX31856_AVG_ADDER_AUTO_50HZ_MS    20u

/* Faults worth a log line: a broken thermocouple, a miswired/overdriven input,
 * or a reading the part itself says is outside the range it can linearize.
 * The four threshold faults are deliberately NOT here -- those are limits the
 * operator asked for, and crossing one is the control loop's news, not a
 * driver warning. */
#define MAX31856_LOGGED_FAULTS \
    ((uint8_t)(THERMO_FAULT_OPEN | THERMO_FAULT_OVUV | THERMO_FAULT_TCRANGE | THERMO_FAULT_CJRANGE))

/* Which faults invalidate tc_temperature_c/cj_temperature_c now lives in
 * max31856_codec.h's max31856_fault_invalidates_tc()/_cj() -- pulled out as a
 * pure, host-tested decision (see that header's comment) rather than kept as
 * a macro here. This _Static_assert pins uart_task_ids.h's THERMO_FAULT_*
 * bit names (used everywhere else in this driver and by
 * ui_page_thermo_faults.c) to the codec's literal values, so the two headers
 * can never silently drift apart -- see max31856_codec.h's
 * MAX31856_CODEC_FAULT_* comment. */
_Static_assert(THERMO_FAULT_OPEN == MAX31856_CODEC_FAULT_OPEN &&
               THERMO_FAULT_OVUV == MAX31856_CODEC_FAULT_OVUV &&
               THERMO_FAULT_TCRANGE == MAX31856_CODEC_FAULT_TCRANGE &&
               THERMO_FAULT_CJRANGE == MAX31856_CODEC_FAULT_CJRANGE,
               "MAX31856.h's THERMO_FAULT_* bit positions drifted from "
               "max31856_codec.h's MAX31856_CODEC_FAULT_* -- they must name "
               "the same SR register bits");

/* How long any caller will wait for another caller's register sequence. The
 * longest thing held under the channel mutex is a handful of bounded SPI
 * transfers through the owner task, so a wait past this means something is
 * wedged -- and a control loop blocked forever inside a thermocouple read is a
 * control loop that can no longer turn elements off. Report and return rather
 * than wait out a deadlock. */
#define MAX31856_LOCK_TIMEOUT_MS 2000u

static bool max31856_lock(MAX31856Class *ch)
{
    if (xSemaphoreTake(ch->lock, pdMS_TO_TICKS(MAX31856_LOCK_TIMEOUT_MS)) == pdTRUE) {
        return true;
    }
    ESP_LOGE(TAG, "ch%u: timed out after %ums waiting for the channel lock", ch->channel,
             (unsigned)MAX31856_LOCK_TIMEOUT_MS);
    return false;
}

static void max31856_unlock(MAX31856Class *ch)
{
    xSemaphoreGive(ch->lock);
}

/* "Usable right now", checked by every public entry point instead of trusting
 * `initialized` on its own. A struct a failed init left half-configured, or
 * one deinit has torn down, then returns ESP_ERR_INVALID_STATE instead of
 * dereferencing a stale device handle or a deleted mutex. */
static bool max31856_ready(const MAX31856Class *ch)
{
    return ch->initialized && ch->lock && ch->dev && ch->bus && ch->bus->initialized;
}

/* --- Low-level register access ----------------------------------------
 * Every transfer goes through spi_owner_transfer_polling(), which owns the
 * queue, the ~CS bit-banging (each device's CS is a plain GPIO -- spics_io_num
 * is -1) and the actual spi_device_polling_transmit() call on its own task.
 * Nothing in this file touches the SPI driver directly except at init.
 *
 * DISPLAY_ST7796_PLAN.md 9.5: the _polling variant, not spi_owner_transfer(),
 * because every MAX31856 transfer here is <= MAX31856_MAX_XFER_LEN bytes
 * (well under the SPI FIFO) and measured IDF overhead on S3 is 11us for
 * spi_device_polling_transmit() versus 26us for the queued/ISR path -- for
 * transfers this short the queue/ISR round trip costs more than it saves.
 * This changes only which ESP-IDF call the shared owner task issues; the
 * data path, error semantics and return codes here are unchanged -- same
 * esp_err_t, same rx layout, same owner-task-is-sole-issuer invariant
 * (DISPLAY_ST7796_PLAN.md section 8). A polling transmit only ever runs on
 * the owner task's own thread (never from an ISR, never from this file's own
 * caller's task context), so its busy-wait cannot stall anything but that
 * one task for the duration of an 11us transfer. */

/* Burst read: one address byte with bit 7 clear, then `len` bytes clocked out
 * of SDO while zeros go in. The part auto-increments the address "as long as
 * CS remains low" (datasheet, Serial Interface), which is what lets
 * MAX31856_read() collect CJTH..SR in a single transaction instead of six. */
static esp_err_t max31856_read_burst(MAX31856Class *ch, uint8_t reg, uint8_t *out, size_t len)
{
    /* `out` is checked here as well as at the public entry points: this helper
     * is reached from several of them and a NULL landing in the memcpy below
     * would be a fault inside a transfer that had already happened. */
    if (!out || len == 0 || len > MAX31856_MAX_BURST_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t tx[MAX31856_MAX_XFER_LEN];
    uint8_t rx[MAX31856_MAX_XFER_LEN];
    memset(tx, 0, sizeof(tx));
    memset(rx, 0, sizeof(rx));
    tx[0] = (uint8_t)(reg & 0x7Fu); /* bit 7 = 0 selects a read */

    esp_err_t err = spi_owner_transfer_polling(&ch->bus->owner, ch->dev, tx, len + 1u, rx, len + 1u,
                                       ch->cs_gpio);
    if (err != ESP_OK) {
        return err;
    }

    /* rx[0] is whatever SDO held while the address was going out (high-Z, so
     * meaningless); the register data starts one byte later. */
    memcpy(out, &rx[1], len);
    return ESP_OK;
}

/* Burst write: address byte with bit 7 set (the 8Xh alias of the same
 * register), then the data bytes, same auto-increment. */
static esp_err_t max31856_write_burst(MAX31856Class *ch, uint8_t reg, const uint8_t *data,
                                      size_t len)
{
    if (!data || len == 0 || len > MAX31856_MAX_BURST_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t tx[MAX31856_MAX_XFER_LEN];
    tx[0] = MAX31856_WRITE_ADDR(reg);
    memcpy(&tx[1], data, len);

    return spi_owner_transfer_polling(&ch->bus->owner, ch->dev, tx, len + 1u, NULL, 0, ch->cs_gpio);
}

static esp_err_t max31856_write_u8(MAX31856Class *ch, uint8_t reg, uint8_t value)
{
    return max31856_write_burst(ch, reg, &value, 1);
}

/* --- Conversion-timing bookkeeping ------------------------------------- */

static uint8_t max31856_sample_count(uint8_t cr1)
{
    /* CR1.AVGSEL[2:0]: 000 = 1, 001 = 2, 010 = 4, 011 = 8, 1xx = 16. */
    uint8_t avgsel = (uint8_t)((cr1 >> 4) & 0x07u);
    return (avgsel >= 4u) ? 16u : (uint8_t)(1u << avgsel);
}

/* Unlocked core of MAX31856_conversion_time_ms(); the public wrapper takes the
 * mutex. */
static uint32_t max31856_conv_ms(const MAX31856Class *ch, bool one_shot)
{
    bool filter_50hz = (ch->cr0_shadow & MAX31856_CR0_FILTER_50HZ) != 0;
    uint32_t samples = max31856_sample_count(ch->cr1_shadow);
    uint32_t base;
    uint32_t adder;

    if (one_shot) {
        base = filter_50hz ? MAX31856_TCONV_ONESHOT_50HZ_MS : MAX31856_TCONV_ONESHOT_60HZ_MS;
        adder = filter_50hz ? MAX31856_AVG_ADDER_ONESHOT_50HZ_MS
                            : MAX31856_AVG_ADDER_ONESHOT_60HZ_MS;
    } else {
        base = filter_50hz ? MAX31856_TCONV_AUTO_50HZ_MS : MAX31856_TCONV_AUTO_60HZ_MS;
        adder = filter_50hz ? MAX31856_AVG_ADDER_AUTO_50HZ_MS : MAX31856_AVG_ADDER_AUTO_60HZ_MS;
    }

    return base + (samples - 1u) * adder;
}

/* Marks "a conversion is on its way, and the earliest its result can exist is
 * now + t". Called whenever conversions are (re)started or a one-shot is
 * triggered; one_shot selects the longer timing, which is also correct for the
 * first conversion after CMODE goes high. Caller holds the mutex. */
static void max31856_arm_result(MAX31856Class *ch, bool one_shot)
{
    ch->next_result_due_tick = xTaskGetTickCount() + pdMS_TO_TICKS(max31856_conv_ms(ch, one_shot));
    ch->result_pending = true;
}

/* Marks "nothing is converting": normally-off mode with no one-shot in
 * flight, so no reading can ever be newer than the last one. */
static void max31856_disarm_result(MAX31856Class *ch)
{
    ch->result_pending = false;
}

/* Tick-wrap-safe "is the armed result due yet". The subtraction is done in the
 * unsigned tick type and reinterpreted as signed, so it stays correct across
 * the ~497-day wrap of a 32-bit tick counter at 100Hz. */
static bool max31856_result_due(const MAX31856Class *ch)
{
    if (!ch->result_pending) {
        return false;
    }
    TickType_t now = xTaskGetTickCount();
    return (int32_t)(now - ch->next_result_due_tick) >= 0;
}

/* Milliseconds since this channel last produced a usable conversion, for
 * MAX31856Reading::age_ms -- see MAX31856.h for why this exists alongside
 * `stale`. Same unsigned-subtract-then-reinterpret trick as
 * max31856_result_due() so it survives the tick counter wrapping; a negative
 * difference (only reachable if the tick counter went backwards) is reported
 * as 0 rather than as an enormous age. Caller holds the mutex. */
static uint32_t max31856_age_ms(const MAX31856Class *ch)
{
    if (!ch->has_good_result) {
        return MAX31856_READING_AGE_UNKNOWN;
    }
    int32_t elapsed = (int32_t)(xTaskGetTickCount() - ch->last_good_tick);
    if (elapsed < 0) {
        return 0;
    }
    uint64_t ms = (uint64_t)(uint32_t)elapsed * portTICK_PERIOD_MS;
    /* Only reachable after weeks without a single good conversion, at which
     * point the exact figure is meaningless and "unknown" is the honest
     * answer -- and it is already far past KILN_TEMP_STALE_AGE_MS either
     * way. */
    return (ms >= MAX31856_READING_AGE_UNKNOWN) ? MAX31856_READING_AGE_UNKNOWN : (uint32_t)ms;
}

/* --- Fault logging ----------------------------------------------------- */

/* Logs only when the interesting bits change, so a thermocouple that is
 * unplugged for an hour produces one WARN rather than one per auto-report
 * tick. Caller holds the channel mutex. */
static void max31856_log_faults(MAX31856Class *ch, uint8_t sr)
{
    uint8_t now = (uint8_t)(sr & MAX31856_LOGGED_FAULTS);
    uint8_t before = (uint8_t)(ch->logged_fault_status & MAX31856_LOGGED_FAULTS);
    if (now == before) {
        return;
    }
    ch->logged_fault_status = sr;

    if (now == 0u) {
        ESP_LOGI(TAG, "ch%u: hardware faults cleared (SR=0x%02X)", ch->channel, sr);
        return;
    }

    ESP_LOGW(TAG, "ch%u: SR=0x%02X%s%s%s%s", ch->channel, sr,
             (now & THERMO_FAULT_OPEN) ? " OPEN(thermocouple open circuit)" : "",
             (now & THERMO_FAULT_OVUV) ? " OVUV(input over/under voltage, conversions suspended)" : "",
             (now & THERMO_FAULT_TCRANGE) ? " TCRANGE(hot junction outside this type's range)" : "",
             (now & THERMO_FAULT_CJRANGE) ? " CJRANGE(cold junction outside -55..+125C)" : "");
}

/* --- Bus -------------------------------------------------------------- */

esp_err_t MAX31856_bus_init(MAX31856BusClass *bus,
                            spi_host_device_t host,
                            int sclk_gpio,
                            int mosi_gpio,
                            int miso_gpio)
{
    if (!bus) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bus->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* A caller is allowed to install the DRDY provider before the bus comes
     * up (the expander may well be started first), so carry those two fields
     * across the wipe instead of silently discarding them. */
    MAX31856_drdy_provider_t saved_provider = bus->drdy_provider;
    void *saved_ctx = bus->drdy_ctx;

    memset(bus, 0, sizeof(*bus));
    bus->host = host;
    bus->drdy_provider = saved_provider;
    bus->drdy_ctx = saved_ctx;

    spi_bus_config_t bus_config = {
        .mosi_io_num = mosi_gpio,
        .miso_io_num = miso_gpio,
        .sclk_io_num = sclk_gpio,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        /* Address byte + the longest burst. Nothing this driver sends is
         * bigger, and with DMA disabled the hardware FIFO caps a transfer at
         * 64 bytes anyway. NOTE for whoever brings up the ILI9488 on this same
         * bus: a framebuffer blit wants DMA and a much larger max_transfer_sz,
         * and only the first spi_bus_initialize() on a host takes effect -- so
         * the display driver should initialize the bus with its own (larger)
         * config and let MAX31856_bus_init find it already up, which is the
         * case handled just below. */
        .max_transfer_sz = MAX31856_MAX_XFER_LEN,
    };

    /* DMA disabled: every transfer here is <= 17 bytes, which fits the FIFO,
     * and it keeps the tx/rx buffers in this file ordinary stack arrays with
     * no DMA-capable-memory or alignment requirements. */
    esp_err_t err = spi_bus_initialize(host, &bus_config, SPI_DMA_DISABLED);
    if (err == ESP_ERR_INVALID_STATE) {
        /* Someone else (the ILI9488 driver) already brought this host up. That
         * is the normal case on this board -- one bus, four chip selects -- so
         * reuse it and, crucially, remember that we do not own it: freeing a
         * bus out from under the display would be worse than leaking it. */
        ESP_LOGI(TAG, "SPI host %d already initialized; sharing it", (int)host);
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    } else {
        bus->bus_owned = true;
    }

    /* Queue deep enough that all three channels plus a UART-driven raw
     * register poke can be in flight without a caller blocking on the queue
     * itself (each caller still blocks on its own completion semaphore). */
    /* DISPLAY_ST7796_PLAN.md 9.3/9.6: CONFIG_KILNCTL_SPI_DMA_USE_PSRAM and
     * CONFIG_KILNCTL_SPI_ASYNC_FLUSH, both default OFF -- see
     * esp_spi_owner.h's spi_owner_t::dma_use_psram/async_flush comments. */
    err = spi_owner_init(&bus->owner, host, 8, 5, 4096, tskNO_AFFINITY,
                          KILNCTL_SPI_DMA_USE_PSRAM ? true : false,
                          KILNCTL_SPI_ASYNC_FLUSH ? true : false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_owner_init failed: %s", esp_err_to_name(err));
        if (bus->bus_owned) {
            spi_bus_free(host);
            bus->bus_owned = false;
        }
        return err;
    }
    bus->owner_initialized = true;
    bus->initialized = true;
    return ESP_OK;
}

esp_err_t MAX31856_bus_deinit(MAX31856BusClass *bus)
{
    if (!bus) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;

    /* Channels first: each removes its own SPI device, and none of them may
     * outlive the owner task they post transfers to. */
    for (size_t i = 0; i < MAX31856_CHANNEL_COUNT; ++i) {
        if (bus->channels[i]) {
            esp_err_t e = MAX31856_deinit(bus->channels[i]);
            if (e != ESP_OK) {
                err = e;
            }
        }
    }

    if (bus->owner_initialized) {
        esp_err_t e = spi_owner_deinit(&bus->owner);
        if (e != ESP_OK) {
            err = e;
        }
        bus->owner_initialized = false;
    }

    if (bus->bus_owned) {
        esp_err_t e = spi_bus_free(bus->host);
        if (e != ESP_OK) {
            err = e;
        }
        bus->bus_owned = false;
    }

    bus->initialized = false;
    return err;
}

MAX31856Class *MAX31856_bus_channel(MAX31856BusClass *bus, uint8_t channel)
{
    if (!bus || channel >= MAX31856_CHANNEL_COUNT) {
        return NULL;
    }
    MAX31856Class *ch = bus->channels[channel];
    return (ch && ch->initialized) ? ch : NULL;
}

bool MAX31856_bus_spi_wedged(const MAX31856BusClass *bus)
{
    if (!bus || !bus->owner_initialized) {
        return false;
    }
    return spi_owner_is_wedged(&bus->owner);
}

esp_err_t MAX31856_set_drdy_provider(MAX31856BusClass *bus,
                                     MAX31856_drdy_provider_t fn,
                                     void *ctx)
{
    if (!bus) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Plain pointer stores of two words that are only ever read inside a
     * channel mutex; installing the provider is a start-up action, not
     * something a control loop does mid-flight. */
    bus->drdy_provider = fn;
    bus->drdy_ctx = ctx;
    return ESP_OK;
}

/* --- Channel bring-up -------------------------------------------------- */

esp_err_t MAX31856_init(MAX31856Class *ch,
                        MAX31856BusClass *bus,
                        uint8_t channel,
                        int cs_gpio,
                        int fault_gpio)
{
    if (!ch || !bus || channel >= MAX31856_CHANNEL_COUNT ||
        !GPIO_IS_VALID_OUTPUT_GPIO(cs_gpio) ||
        (fault_gpio >= 0 && !GPIO_IS_VALID_GPIO(fault_gpio))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!bus->initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Re-initializing a live channel would memset away the SPI device handle
     * and the mutex below without freeing either, and leave the bus registry
     * pointing at a struct whose contents just changed underneath it. Deinit
     * first, deliberately, or not at all. */
    if (ch->initialized || ch->dev || ch->lock) {
        ESP_LOGE(TAG, "ch%u: init called on a channel that is already up", channel);
        return ESP_ERR_INVALID_STATE;
    }
    if (bus->channels[channel] && bus->channels[channel] != ch) {
        ESP_LOGE(TAG, "ch%u: another instance already owns this channel", channel);
        return ESP_ERR_INVALID_STATE;
    }

    memset(ch, 0, sizeof(*ch));
    ch->bus = bus;
    ch->channel = channel;
#if KILNCTL_SPI_HARDWARE_CS
    /* DISPLAY_ST7796_PLAN.md 9.4: the SPI peripheral (spics_io_num, set below
     * on dev_config) owns this pin, so every spi_owner_transfer_polling()
     * call site in this file must pass the -1 sentinel esp_spi_owner.c
     * treats as "do not bit-bang" instead of the real GPIO number. */
    ch->cs_gpio = -1;
#else
    ch->cs_gpio = cs_gpio;
#endif
    ch->fault_gpio = fault_gpio;

    esp_err_t err;
#if !KILNCTL_SPI_HARDWARE_CS
    /* ~CS is bit-banged by spi_owner around each transfer, so it has to idle
     * high from the moment it becomes an output -- a CS left low would let the
     * part latch whatever the display is clocking out. */
    gpio_config_t cs_conf = {
        .pin_bit_mask = (1ULL << (unsigned)cs_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&cs_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ch%u: gpio_config(cs=%d) failed: %s", channel, cs_gpio,
                 esp_err_to_name(err));
        return err;
    }
    gpio_set_level((gpio_num_t)cs_gpio, 1);
#else
    /* DISPLAY_ST7796_PLAN.md 9.4: this pin is now driven by the SPI
     * peripheral via spics_io_num below -- configuring/driving it as a plain
     * GPIO here would fight the peripheral on the same wire. */
    err = ESP_OK;
#endif

    /* ~FAULT is an active-low output from the part with nothing pulling it up
     * on the daughterboard, so the internal pull-up is doing real work here.
     * No interrupt is attached: the level is sampled during MAX31856_read()
     * and reported in the reading, which is enough for a signal that only
     * changes at conversion rate and is always corroborated by SR. */
    if (fault_gpio >= 0) {
        gpio_config_t fault_conf = {
            .pin_bit_mask = (1ULL << (unsigned)fault_gpio),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&fault_conf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ch%u: gpio_config(fault=%d) failed: %s", channel, fault_gpio,
                     esp_err_to_name(err));
            return err;
        }
    }

    spi_device_interface_config_t dev_config = {
        .clock_speed_hz = THERMO_SPI_CLOCK_HZ,
        .mode = MAX31856_SPI_MODE,
#if KILNCTL_SPI_HARDWARE_CS
        .spics_io_num = cs_gpio, /* DISPLAY_ST7796_PLAN.md 9.4: peripheral drives CS */
#else
        .spics_io_num = -1, /* CS driven by spi_owner, not the SPI peripheral */
#endif
        .queue_size = 1,
        .input_delay_ns = MAX31856_SPI_INPUT_DELAY_NS,
    };
    err = spi_bus_add_device(bus->host, &dev_config, &ch->dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ch%u: spi_bus_add_device failed: %s", channel, esp_err_to_name(err));
        ch->dev = NULL;
        return err;
    }

    ch->lock = xSemaphoreCreateMutex();
    if (!ch->lock) {
        ESP_LOGE(TAG, "ch%u: failed to allocate mutex", channel);
        spi_bus_remove_device(ch->dev);
        ch->dev = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Seed the shadows with the part's documented power-on defaults (Table 6:
     * CR0 = 00h, CR1 = 03h -> type K, 1 sample, MASK = FFh -> every fault
     * masked off the ~FAULT pin). No register is written here: the caller may
     * want a different configuration anyway, and MAX31856_configure() rewrites
     * both control registers unconditionally. */
    ch->cr0_shadow = 0x00u;
    ch->cr1_shadow = 0x03u;
    ch->mask_shadow = 0xFFu;
    max31856_disarm_result(ch); /* CMODE = 0 out of reset: nothing is converting */
    ch->logged_fault_status = 0u;
    ch->initialized = true;
    bus->channels[channel] = ch;
    return ESP_OK;
}

esp_err_t MAX31856_deinit(MAX31856Class *ch)
{
    if (!ch) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;

    /* Take the lock before tearing anything down, so a transfer that is
     * already in flight finishes first: freeing the SPI device or deleting the
     * mutex out from under MAX31856_read() would fault in another task. The
     * `initialized` flag is cleared while still holding it, which is what stops
     * a caller that passed max31856_ready() a moment ago from proceeding. */
    bool locked = ch->lock && max31856_lock(ch);
    ch->initialized = false;

    /* Leave the part not converting rather than free-running into a dead
     * driver -- best effort; a part that is already unreachable stays that
     * way and the error is ignored on the way out. */
    if (ch->dev && ch->bus && ch->bus->initialized) {
        (void)max31856_write_u8(ch, MAX31856_REG_CR0,
                                (uint8_t)(ch->cr0_shadow & ~MAX31856_CR0_CMODE));
    }

    if (ch->bus && ch->channel < MAX31856_CHANNEL_COUNT &&
        ch->bus->channels[ch->channel] == ch) {
        ch->bus->channels[ch->channel] = NULL;
    }

    if (ch->dev) {
        esp_err_t e = spi_bus_remove_device(ch->dev);
        if (e != ESP_OK) {
            err = e;
        }
        ch->dev = NULL;
    }

    /* Device handle gone first, mutex last: nothing can start a new transfer
     * once dev is NULL, so this is the point at which the lock has no more
     * work to protect. */
    if (ch->lock) {
        SemaphoreHandle_t lock = ch->lock;
        ch->lock = NULL;
        if (locked) {
            xSemaphoreGive(lock);
        }
        vSemaphoreDelete(lock);
    }

    return err;
}

void MAX31856_config_default(MAX31856Config *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->tc_type = THERMO_TC_K;  /* what this kiln ships with */
    cfg->avg_mode = THERMO_AVG_1;
    cfg->filter_50hz = false;    /* 60Hz mains */
    cfg->oc_detect = MAX31856_OC_MODE1; /* kiln thermocouple loops are well under 5k */
    cfg->cj_sensor_disabled = false;
    cfg->interrupt_fault_mode = false; /* comparator mode: fault bits self-clear */
#ifdef CONFIG_KILNCTL_THERMO_DEFAULT_AUTO_CONVERT
    cfg->auto_convert = true;
#else
    cfg->auto_convert = false;
#endif
}

esp_err_t MAX31856_start_all(MAX31856BusClass *bus, MAX31856Class *channels)
{
    if (!bus || !channels) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = MAX31856_bus_init(bus, KILN_SPI_HOST, KILN_SPI_SCLK_IO, KILN_SPI_MOSI_IO,
                                      KILN_SPI_MISO_IO);
    if (err != ESP_OK) {
        return err;
    }

    /* Rotated one position vs the CS0/CS1/CS2 order HARDWARE.md's J6/J5 pin
     * table describes: on the physically populated thermocouple daughterboard,
     * the screw terminal silkscreened "N" is actually wired to the MAX31856
     * that CS(N-1 mod 3) drives, not CS(N). Confirmed on the bench by
     * unplugging each terminal in turn and noting which logical channel
     * faulted (terminal 2 -> ch1 faulted, terminal 1 -> ch0, terminal 0 ->
     * ch2 -- a fixed rotation, not a random miswire). Rotating this array so
     * logical channel index i reads the chip actually fed by terminal i
     * fixes it at the one place every downstream consumer (zone thermo_mask
     * bits, the dashboard's "Channel N", LCD, MCP tools) reads from --
     * nothing downstream needs its own compensating remap. */
    static const int cs_pins[MAX31856_CHANNEL_COUNT] = {
        THERMO_CS2_IO, THERMO_CS0_IO, THERMO_CS1_IO,
    };
    static const int fault_pins[MAX31856_CHANNEL_COUNT] = {
        THERMO_FAULT2_IO, THERMO_FAULT0_IO, THERMO_FAULT1_IO,
    };

    MAX31856Config cfg;
    MAX31856_config_default(&cfg);

    esp_err_t last_err = ESP_OK;
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; ++i) {
        MAX31856Class *ch = &channels[i];

        esp_err_t e = MAX31856_init(ch, bus, i, cs_pins[i], fault_pins[i]);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "ch%u: init failed: %s", i, esp_err_to_name(e));
            last_err = e;
            continue;
        }

        e = MAX31856_configure(ch, &cfg);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "ch%u: configure failed: %s", i, esp_err_to_name(e));
            MAX31856_deinit(ch);
            last_err = e;
            continue;
        }

        /* Let open-circuit and over/undervoltage reach the ~FAULT pin; the
         * part masks everything by default (MASK = FFh), which would leave the
         * three ~FAULT GPIOs permanently idle. */
        e = MAX31856_set_fault_mask(ch, MAX31856_DEFAULT_FAULT_MASK);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "ch%u: fault mask write failed: %s", i, esp_err_to_name(e));
            MAX31856_deinit(ch);
            last_err = e;
            continue;
        }

        /* Presence check. A missing part (unpopulated channel, unplugged
         * daughterboard) leaves MISO floating or pulled, so CR1 reads back as
         * 00h or FFh -- neither of which is the 03h (type K, 1 sample) that
         * MAX31856_config_default just wrote. Verifying a register we wrote
         * ourselves also catches a bus that clocks but garbles, which a bare
         * "did the transfer return ESP_OK" check would not: spi_owner_transfer
         * succeeds happily against an empty socket. */
        uint8_t cr1 = 0;
        e = MAX31856_read_reg(ch, MAX31856_REG_CR1, &cr1, 1);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "ch%u: CR1 read-back failed: %s", i, esp_err_to_name(e));
            MAX31856_deinit(ch);
            last_err = e;
            continue;
        }
        if (cr1 != ch->cr1_shadow) {
            ESP_LOGE(TAG, "ch%u: CR1 read back 0x%02X, expected 0x%02X -- part not responding on CS%d",
                     i, cr1, ch->cr1_shadow, cs_pins[i]);
            MAX31856_deinit(ch);
            last_err = ESP_ERR_INVALID_RESPONSE;
            continue;
        }

        ESP_LOGI(TAG, "ch%u ready: CS=%d ~FAULT=%d CR0=0x%02X CR1=0x%02X (%s, %u sample(s), %s)",
                 i, cs_pins[i], fault_pins[i], ch->cr0_shadow, ch->cr1_shadow,
                 (ch->cr0_shadow & MAX31856_CR0_CMODE) ? "auto" : "one-shot",
                 (unsigned)max31856_sample_count(ch->cr1_shadow),
                 (ch->cr0_shadow & MAX31856_CR0_FILTER_50HZ) ? "50Hz" : "60Hz");
    }

    return last_err;
}

/* --- Configuration ----------------------------------------------------- */

esp_err_t MAX31856_configure(MAX31856Class *ch, const MAX31856Config *cfg)
{
    if (!ch || !cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!max31856_ready(ch)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (cfg->tc_type > 0x0Fu || cfg->avg_mode > 0x07u || cfg->oc_detect > 0x03u) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t cr1 = (uint8_t)(((cfg->avg_mode & 0x07u) << 4) | (cfg->tc_type & 0x0Fu));

    /* CR0 with CMODE deliberately left out; it goes back in at the end. */
    uint8_t cr0_stopped = (uint8_t)((cfg->oc_detect & 0x03u) << 4);
    if (cfg->filter_50hz) {
        cr0_stopped |= MAX31856_CR0_FILTER_50HZ;
    }
    if (cfg->cj_sensor_disabled) {
        cr0_stopped |= MAX31856_CR0_CJ_DISABLE;
    }
    if (cfg->interrupt_fault_mode) {
        cr0_stopped |= MAX31856_CR0_FAULT_INT;
    }

    if (!max31856_lock(ch)) {
        return ESP_ERR_TIMEOUT;
    }

    /* Step 1: conversions off, with the new 50/60Hz bit already in place.
     * The datasheet is explicit about this one: "Change the notch frequency
     * only while in the 'Normally Off' mode - not in the Automatic Conversion
     * mode." Writing CR0 with CMODE = 0 satisfies that whether or not the
     * filter bit actually changed, which is cheaper to reason about than
     * comparing against the shadow and conditionally stopping. */
    esp_err_t err = max31856_write_u8(ch, MAX31856_REG_CR0, cr0_stopped);
    if (err == ESP_OK) {
        ch->cr0_shadow = cr0_stopped;
        max31856_disarm_result(ch); /* nothing is converting right now */

        /* Step 2: CR1 while still stopped -- "The Thermocouple Voltage
         * Conversion Averaging Mode settings should not be changed while
         * conversions are taking place." */
        err = max31856_write_u8(ch, MAX31856_REG_CR1, cr1);
        if (err == ESP_OK) {
            ch->cr1_shadow = cr1;
        }
    }

    /* Step 3: restore the requested conversion mode. */
    if (err == ESP_OK && cfg->auto_convert) {
        uint8_t cr0_running = (uint8_t)(cr0_stopped | MAX31856_CR0_CMODE);
        err = max31856_write_u8(ch, MAX31856_REG_CR0, cr0_running);
        if (err == ESP_OK) {
            ch->cr0_shadow = cr0_running;
            /* The first conversion after CMODE goes high takes the longer
             * 1-shot time, not the steady-state automatic one. */
            max31856_arm_result(ch, true);
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ch%u: configure failed: %s", ch->channel, esp_err_to_name(err));
    }

    max31856_unlock(ch);
    return err;
}

esp_err_t MAX31856_config_channel(MAX31856Class *ch,
                                  uint8_t tc_type,
                                  uint8_t avg_mode,
                                  bool filter_50hz,
                                  bool auto_convert)
{
    /* Range-checked here as well as in MAX31856_configure, so the wire's
     * CONFIG_CHANNEL cannot silently have its type or averaging code truncated
     * into a different, valid-looking setting. */
    if (tc_type > 0x0Fu || avg_mode > 0x07u) {
        return ESP_ERR_INVALID_ARG;
    }
    MAX31856Config cfg;
    MAX31856_config_default(&cfg);
    cfg.tc_type = tc_type;
    cfg.avg_mode = avg_mode;
    cfg.filter_50hz = filter_50hz;
    cfg.auto_convert = auto_convert;
    return MAX31856_configure(ch, &cfg);
}

esp_err_t MAX31856_get_config(MAX31856Class *ch, MAX31856Config *out_cfg)
{
    if (!ch || !out_cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!max31856_ready(ch)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!max31856_lock(ch)) {
        return ESP_ERR_TIMEOUT;
    }
    uint8_t cr0 = ch->cr0_shadow;
    uint8_t cr1 = ch->cr1_shadow;
    max31856_unlock(ch);

    memset(out_cfg, 0, sizeof(*out_cfg));
    out_cfg->tc_type = (uint8_t)(cr1 & 0x0Fu);
    out_cfg->avg_mode = (uint8_t)((cr1 >> 4) & 0x07u);
    out_cfg->filter_50hz = (cr0 & MAX31856_CR0_FILTER_50HZ) != 0;
    out_cfg->auto_convert = (cr0 & MAX31856_CR0_CMODE) != 0;
    out_cfg->oc_detect = (uint8_t)((cr0 >> 4) & 0x03u);
    out_cfg->cj_sensor_disabled = (cr0 & MAX31856_CR0_CJ_DISABLE) != 0;
    out_cfg->interrupt_fault_mode = (cr0 & MAX31856_CR0_FAULT_INT) != 0;
    return ESP_OK;
}

esp_err_t MAX31856_set_conversion_mode(MAX31856Class *ch, bool automatic)
{
    if (!ch) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!max31856_ready(ch)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!max31856_lock(ch)) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t cr0 = ch->cr0_shadow;
    if (automatic) {
        cr0 |= MAX31856_CR0_CMODE;
    } else {
        cr0 = (uint8_t)(cr0 & ~MAX31856_CR0_CMODE);
    }

    esp_err_t err = max31856_write_u8(ch, MAX31856_REG_CR0, cr0);
    if (err == ESP_OK) {
        ch->cr0_shadow = cr0;
        if (automatic) {
            max31856_arm_result(ch, true); /* first auto conversion = 1-shot timing */
        } else {
            max31856_disarm_result(ch);
        }
    }

    max31856_unlock(ch);
    return err;
}

esp_err_t MAX31856_set_thresholds(MAX31856Class *ch,
                                  float tc_high_c,
                                  float tc_low_c,
                                  int8_t cj_high_c,
                                  int8_t cj_low_c)
{
    if (!ch) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!max31856_ready(ch)) {
        return ESP_ERR_INVALID_STATE;
    }
    /* A NaN threshold would encode as 0 degC -- a threshold in the middle of
     * the working range, silently. Reject it instead: the caller asked for
     * something the register cannot express. */
    if (isnan(tc_high_c) || isnan(tc_low_c)) {
        return ESP_ERR_INVALID_ARG;
    }

    int16_t tc_high = max31856_encode_tc_threshold(tc_high_c);
    int16_t tc_low = max31856_encode_tc_threshold(tc_low_c);

    /* CJHF/CJLF are plain 8-bit two's complement at 1 degC per LSB (sign +
     * 2^6..2^0), so the wire's int8 degC needs no scaling at all. They are
     * adjacent, so one burst write covers both. */
    uint8_t cj_bytes[2] = { (uint8_t)cj_high_c, (uint8_t)cj_low_c };
    /* LTHFTH:LTHFTL and LTLFTH:LTLFTL are likewise adjacent pairs, MSB first
     * ("Data is read from or written to the registers MSB first"). */
    uint8_t tc_high_bytes[2] = { (uint8_t)((uint16_t)tc_high >> 8), (uint8_t)(tc_high & 0xFF) };
    uint8_t tc_low_bytes[2] = { (uint8_t)((uint16_t)tc_low >> 8), (uint8_t)(tc_low & 0xFF) };

    if (!max31856_lock(ch)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = max31856_write_burst(ch, MAX31856_REG_CJHF, cj_bytes, sizeof(cj_bytes));
    if (err == ESP_OK) {
        err = max31856_write_burst(ch, MAX31856_REG_LTHFTH, tc_high_bytes, sizeof(tc_high_bytes));
    }
    if (err == ESP_OK) {
        err = max31856_write_burst(ch, MAX31856_REG_LTLFTH, tc_low_bytes, sizeof(tc_low_bytes));
    }
    max31856_unlock(ch);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ch%u: threshold write failed: %s", ch->channel, esp_err_to_name(err));
    }
    return err;
}

esp_err_t MAX31856_set_cj_offset(MAX31856Class *ch, float offset_c)
{
    if (!ch || isnan(offset_c)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!max31856_ready(ch)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* CJTO is sign + 2^2..2^-4: 1/16 degC per LSB over -8.0 .. +7.9375 degC.
     * Anything further out simply has no encoding, so clamp loudly rather than
     * wrapping the two's-complement value into a plausible-looking offset of
     * the wrong sign. */
    float clamped = offset_c;
    if (clamped > MAX31856_CJ_OFFSET_MAX_C) {
        clamped = MAX31856_CJ_OFFSET_MAX_C;
    } else if (clamped < MAX31856_CJ_OFFSET_MIN_C) {
        clamped = MAX31856_CJ_OFFSET_MIN_C;
    }
    if (clamped != offset_c) {
        ESP_LOGW(TAG, "ch%u: CJ offset %.4f degC out of range, clamped to %.4f", ch->channel,
                 (double)offset_c, (double)clamped);
    }

    int8_t raw = (int8_t)roundf(clamped / MAX31856_CJ_OFFSET_C_PER_LSB);

    if (!max31856_lock(ch)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = max31856_write_u8(ch, MAX31856_REG_CJTO, (uint8_t)raw);
    max31856_unlock(ch);
    return err;
}

esp_err_t MAX31856_set_fault_mask(MAX31856Class *ch, uint8_t mask)
{
    if (!ch) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!max31856_ready(ch)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Bits 7:6 are reserved; write them as the factory default 1s do rather
     * than inventing values for read-only bits. */
    uint8_t value = (uint8_t)((mask & MAX31856_MASK_ALL) | 0xC0u);

    if (!max31856_lock(ch)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = max31856_write_u8(ch, MAX31856_REG_MASK, value);
    if (err == ESP_OK) {
        ch->mask_shadow = value;
    }
    max31856_unlock(ch);
    return err;
}

/* --- Measurement ------------------------------------------------------- */

esp_err_t MAX31856_trigger_one_shot(MAX31856Class *ch)
{
    if (!ch) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!max31856_ready(ch)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!max31856_lock(ch)) {
        return ESP_ERR_TIMEOUT;
    }

    /* 1SHOT self-clears in hardware ("This bit self clears to 0"), so it is
     * written on top of the shadow but never stored in it -- otherwise every
     * later CR0 write would re-trigger a conversion. The conversion actually
     * starts when ~CS rises at the end of this transaction, which spi_owner
     * does for us. */
    esp_err_t err = max31856_write_u8(ch, MAX31856_REG_CR0,
                                      (uint8_t)(ch->cr0_shadow | MAX31856_CR0_ONESHOT));
    if (err == ESP_OK) {
        max31856_arm_result(ch, true);
    }

    max31856_unlock(ch);
    return err;
}

esp_err_t MAX31856_read(MAX31856Class *ch, MAX31856Reading *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Fill in the failure answer first, so every early return below is already
     * honest: NaN temperatures, no fault bits, spi_failed set. */
    memset(out, 0, sizeof(*out));
    out->tc_temperature_c = NAN;
    out->cj_temperature_c = NAN;
    out->spi_failed = true;
    out->stale = true;
    out->age_ms = MAX31856_READING_AGE_UNKNOWN;

    if (!ch) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!max31856_ready(ch)) {
        return ESP_ERR_INVALID_STATE;
    }
    out->channel = ch->channel;

    /* A finite wait: a caller that cannot get the lock gets the failure answer
     * already staged above (NaN, spi_failed) rather than blocking forever
     * behind whatever is wedged. */
    if (!max31856_lock(ch)) {
        return ESP_ERR_TIMEOUT;
    }

    out->fault_pin_asserted = (ch->fault_gpio >= 0) &&
                              (gpio_get_level((gpio_num_t)ch->fault_gpio) == 0);

    /* Sample ~DRDY *before* the burst: reading CJTH/CJTL (or the LTCB
     * registers) is exactly what releases ~DRDY back high, so asking
     * afterwards would always report "no new data". */
    bool drdy_known = false;
    bool drdy_asserted = false;
    if (ch->bus->drdy_provider) {
        drdy_known = ch->bus->drdy_provider(ch->channel, &drdy_asserted, ch->bus->drdy_ctx);
    }

    /* One transaction, six registers: CJTH, CJTL, LTCBH, LTCBM, LTCBL, SR.
     * Doing this as separate single-byte reads would not only cost six
     * transactions, it would let a conversion land between them and hand back
     * a cold-junction value from one conversion and a hot-junction value from
     * the next. */
    uint8_t buf[6];
    esp_err_t err = max31856_read_burst(ch, MAX31856_REG_CJTH, buf, sizeof(buf));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ch%u: temperature burst read failed: %s", ch->channel,
                 esp_err_to_name(err));
        /* The temperatures stay NaN (staged above) -- but how long it has
         * been since this channel last had a good one is still known, and is
         * exactly what a UI needs to decide between "one bad poll" and "this
         * sensor has been gone for a while". */
        out->age_ms = max31856_age_ms(ch);
        max31856_unlock(ch);
        return err;
    }

    out->cj_temperature_c = max31856_decode_cj(buf[0], buf[1]);
    out->tc_temperature_c = max31856_decode_tc(buf[2], buf[3], buf[4]);
    out->fault_status = buf[5];
    out->spi_failed = false;

    /* The transfer succeeded, which says nothing about whether the number it
     * carried means anything. An open thermocouple, an over/undervoltage input
     * (conversions suspended, so LTCB still holds the pre-fault value), a hot
     * junction outside the type's linearization range, OR an out-of-range
     * cold junction (LTCB is cold-junction-COMPENSATED in hardware, so a bad
     * CJ reading taints the hot-junction number too -- see
     * max31856_fault_invalidates_tc()'s 2026-08-27 doc comment) all leave a
     * perfectly plausible-looking but meaningless temperature in the
     * register. Report NaN for those: the fault bits say why, and no caller
     * can mistake a NaN for a cold kiln, nor a plausible-but-wrong number for
     * a real one. */
    if (max31856_fault_invalidates_tc(out->fault_status)) {
        out->tc_temperature_c = NAN;
    }
    if (max31856_fault_invalidates_cj(out->fault_status)) {
        out->cj_temperature_c = NAN;
    }
    /* A decode that produced a non-finite float has no business being reported
     * as a temperature either -- belt and braces around the fixed-point maths
     * above, which cannot currently produce one. */
    if (!isfinite(out->tc_temperature_c) && !isnan(out->tc_temperature_c)) {
        out->tc_temperature_c = NAN;
    }

    /* Freshness. With a DRDY provider this is the hardware's own answer; the
     * fallback can only say "not enough time has passed for a new conversion
     * to exist", which catches polling faster than the part converts but not a
     * part that has stopped converting altogether. That limitation is why the
     * provider hook exists -- see MAX31856_drdy_provider_t. */
    bool fresh = drdy_known ? drdy_asserted : max31856_result_due(ch);
    out->stale = !fresh;

    if (fresh) {
        if (ch->cr0_shadow & MAX31856_CR0_CMODE) {
            /* Still free-running: the next result is one steady-state
             * conversion away (the shorter of the two timings -- the pipeline
             * is already primed). */
            ch->next_result_due_tick =
                xTaskGetTickCount() + pdMS_TO_TICKS(max31856_conv_ms(ch, false));
            ch->result_pending = true;
        } else {
            max31856_disarm_result(ch); /* nothing more coming until asked */
        }
    }

    /* Age bookkeeping. A conversion only counts as "good" if it is new AND
     * carried a usable number: an open thermocouple keeps answering forever,
     * and letting that refresh the age would hide a dead sensor behind a
     * permanently young reading. */
    if (fresh && !isnan(out->tc_temperature_c)) {
        ch->last_good_tick = xTaskGetTickCount();
        ch->has_good_result = true;
    }
    out->age_ms = max31856_age_ms(ch);

    max31856_log_faults(ch, out->fault_status);

    max31856_unlock(ch);
    return ESP_OK;
}

esp_err_t MAX31856_read_all(MAX31856BusClass *bus,
                            MAX31856Reading *out,
                            size_t max_readings,
                            size_t *out_count)
{
    if (!bus || !out || max_readings == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!bus->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t count = 0;
    esp_err_t first_err = ESP_OK;

    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT && count < max_readings; ++i) {
        MAX31856Class *ch = MAX31856_bus_channel(bus, i);
        if (!ch) {
            continue; /* never came up; the caller reports it as absent, not as 0 degC */
        }
        esp_err_t err = MAX31856_read(ch, &out[count]);
        if (err != ESP_OK && first_err == ESP_OK) {
            first_err = err;
        }
        ++count;
    }

    if (out_count) {
        *out_count = count;
    }
    return first_err;
}

esp_err_t MAX31856_read_faults(MAX31856Class *ch, uint8_t *out_sr, uint8_t *out_mask)
{
    if (!ch) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!max31856_ready(ch)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!max31856_lock(ch)) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t sr = 0;
    esp_err_t err = max31856_read_burst(ch, MAX31856_REG_SR, &sr, 1);
    if (err == ESP_OK) {
        max31856_log_faults(ch, sr);
        if (out_sr) {
            *out_sr = sr;
        }
    } else if (out_sr) {
        *out_sr = 0;
    }

    /* MASK comes from the shadow rather than a second transaction: it only
     * ever changes through MAX31856_set_fault_mask/MAX31856_write_reg, both of
     * which update the shadow under this same mutex. */
    if (out_mask) {
        *out_mask = ch->mask_shadow;
    }

    max31856_unlock(ch);
    return err;
}

esp_err_t MAX31856_clear_faults(MAX31856Class *ch)
{
    if (!ch) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!max31856_ready(ch)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!max31856_lock(ch)) {
        return ESP_ERR_TIMEOUT;
    }

    if (!(ch->cr0_shadow & MAX31856_CR0_FAULT_INT)) {
        /* Comparator mode. FAULTCLR "has no effect in comparator mode"; the
         * status bits track the condition and drop on their own. Say so once
         * at DEBUG and still write the bit, so the command is never silently
         * a no-op from the caller's point of view. */
        ESP_LOGD(TAG, "ch%u: FAULTCLR in comparator mode has no effect; faults self-clear",
                 ch->channel);
    }

    /* Like 1SHOT, FAULTCLR self-clears and must not enter the shadow. */
    esp_err_t err = max31856_write_u8(ch, MAX31856_REG_CR0,
                                      (uint8_t)(ch->cr0_shadow | MAX31856_CR0_FAULTCLR));
    if (err == ESP_OK) {
        /* Let the next real fault log again. */
        ch->logged_fault_status = 0u;
    }

    max31856_unlock(ch);
    return err;
}

bool MAX31856_fault_pin_asserted(const MAX31856Class *ch)
{
    if (!ch || !ch->initialized || ch->fault_gpio < 0) {
        return false;
    }
    return gpio_get_level((gpio_num_t)ch->fault_gpio) == 0; /* active low */
}

uint32_t MAX31856_conversion_time_ms(MAX31856Class *ch, bool one_shot)
{
    if (!ch || !max31856_ready(ch)) {
        return 0;
    }
    if (!max31856_lock(ch)) {
        return 0; /* documented "0 means unknown"; a wedged lock is exactly that */
    }
    uint32_t ms = max31856_conv_ms(ch, one_shot);
    max31856_unlock(ch);
    return ms;
}

/* --- Raw register access ----------------------------------------------- */

esp_err_t MAX31856_read_reg(MAX31856Class *ch, uint8_t reg, uint8_t *buf, size_t len)
{
    if (!ch || !buf || len == 0 || len > MAX31856_MAX_BURST_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    /* The address auto-increment runs off the end of the 16-register map into
     * nothing; a debug READ_REG asking for it would fill `buf` with whatever
     * the part clocks out past 0x0F and call it register data. */
    if (reg >= MAX31856_REG_COUNT || (size_t)reg + len > MAX31856_REG_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!max31856_ready(ch)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!max31856_lock(ch)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = max31856_read_burst(ch, reg, buf, len);
    max31856_unlock(ch);
    return err;
}

esp_err_t MAX31856_write_reg(MAX31856Class *ch, uint8_t reg, uint8_t value)
{
    if (!ch) {
        return ESP_ERR_INVALID_ARG;
    }
    /* `reg` is documented as the 0Xh read address -- the 0x80 write bit is
     * added below. Anything with bit 7 already set, or past the register map,
     * would land on an address the part does not define. */
    if (reg >= MAX31856_REG_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!max31856_ready(ch)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!max31856_lock(ch)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = max31856_write_u8(ch, reg, value);
    if (err == ESP_OK) {
        /* Keep the shadows honest: a debug poke at CR0/CR1/MASK is a real
         * configuration change, and if the driver kept believing its old copy
         * the next read-modify-write (a one-shot trigger, say) would quietly
         * undo it. The self-clearing bits are stripped for the same reason
         * they are never stored elsewhere. */
        switch (reg & 0x7Fu) {
            case MAX31856_REG_CR0:
                ch->cr0_shadow =
                    (uint8_t)(value & ~(MAX31856_CR0_ONESHOT | MAX31856_CR0_FAULTCLR));
                if ((value & MAX31856_CR0_ONESHOT) || (value & MAX31856_CR0_CMODE)) {
                    max31856_arm_result(ch, true);
                } else {
                    max31856_disarm_result(ch);
                }
                break;
            case MAX31856_REG_CR1:
                ch->cr1_shadow = value;
                break;
            case MAX31856_REG_MASK:
                ch->mask_shadow = value;
                break;
            default:
                break;
        }
    }
    max31856_unlock(ch);
    return err;
}
