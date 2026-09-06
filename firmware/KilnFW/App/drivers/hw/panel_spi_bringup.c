// ROADMAP.md M15 1500-line rule: move-only split of panel_spi.c. This file
// owns the panel bring-up path: the vendor init-table execution
// (panel_spi_run_init_sequence(), decoding panel_desc_t.init_seq generically
// -- see panel_codec_init_step()), rotation/MADCTL handling
// (panel_spi_apply_rotation()), hard/soft reset, and every public entry
// point that (re)establishes panel state: ILI9488_init/start/deinit/reset,
// set_power/set_rotation/set_invert/get_dimensions, and the ILI9488
// panel_desc_t descriptor itself. See panel_spi.c's header comment for the
// full file map and panel_spi_internal.h for the shared declarations.
//
// EXTREME CARE (2026-09-04, settled after a long debugging effort): MADCTL
// color order for the ST7796 is 0x00 (RGB), measured, not assumed (f493bf8).
// INVON is deliberately absent from the vendor init sequence. SPI write
// clock is the Kconfig default (20 MHz); an overclock hypothesis was tested
// and refuted (07cad60). None of that behavior is touched by this split.
#include "panel_spi.h"
#include "panel_spi_internal.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "FT6336U.h"
#include "NS2009.h"
#include "panel_codec.h"
#include "panel_detect.h"
#include "settings.h"
#include "st7796_panel.h"


/* ===================================================================
 * Init / deinit
 * =================================================================== */

/* MADCTL per rotation is panel_spi_madctl_by_rotation[] (panel_spi.c) --
 * moved there because its only use is populating the panel_desc_t
 * descriptor's .madctl field, and a descriptor built as a static initializer
 * needs that table's actual values available at compile time, in the same
 * translation unit (an extern array's element values are not a constant
 * expression, even with a known size). apply_rotation() below never reads
 * the table directly: it reads disp->panel->madctl, the descriptor's own
 * (already-populated) copy. */
static esp_err_t panel_spi_apply_rotation(ILI9488Class *disp, uint8_t rotation)
{
    uint8_t madctl = panel_codec_madctl(disp->panel->madctl, rotation, disp->panel->color_order_bit);
    esp_err_t err = panel_spi_write_cmd(disp, ILI9488_CMD_MADCTL, &madctl, 1);
    if (err != ESP_OK) return err;

    disp->madctl = madctl;
    disp->rotation = (uint8_t)(rotation & 0x03);
    if (panel_codec_rotation_swaps_dimensions(disp->rotation)) {
        disp->width = disp->panel_height;
        disp->height = disp->panel_width;
    } else {
        disp->width = disp->panel_width;
        disp->height = disp->panel_height;
    }
    return ESP_OK;
}

/* The full bring-up, factored out because a reset (hard or soft) drops the
 * controller back to power-on defaults and has to re-run all of it.
 *
 * Phase 3: runs disp->panel->init_seq generically, decoding it with
 * panel_codec_init_step() (see that function's comment for the packed
 * format) instead of walking a private struct array -- this is what makes
 * the same function correct for both ILI9488 and ST7796. A malformed table
 * (declared param length running past init_len) is treated as an init
 * failure rather than read past the buffer -- unreachable from either real
 * descriptor below, but panel_codec_init_step() is host-tested against it
 * directly since nothing here would otherwise exercise that path. */
static esp_err_t panel_spi_run_init_sequence(ILI9488Class *disp)
{
    const panel_desc_t *panel = disp->panel;
    size_t offset = 0;
    uint8_t cmd;
    const uint8_t *params;
    uint8_t param_len;
    while (panel_codec_init_step(panel->init_seq, panel->init_len, &offset,
                                  &cmd, &params, &param_len)) {
        esp_err_t err = panel_spi_write_cmd(disp, cmd, params, param_len);
        if (err != ESP_OK) {
            ESP_LOGE(PANEL_SPI_TAG, "init step 0x%02X failed: %s", cmd, esp_err_to_name(err));
            return err;
        }
    }
    if (offset != panel->init_len) {
        ESP_LOGE(PANEL_SPI_TAG, "%s init_seq is malformed (stopped at byte %u of %u)",
                 panel->name, (unsigned)offset, (unsigned)panel->init_len);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = panel_spi_apply_rotation(disp, disp->rotation);
    if (err != ESP_OK) return err;

    err = panel_spi_write_cmd(disp, ILI9488_CMD_SLPOUT, NULL, 0);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(ILI9488_SLEEP_WAIT_MS));  /* datasheet: 120ms before the next command */

    err = panel_spi_write_cmd(disp, ILI9488_CMD_DISPON, NULL, 0);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(20));

    return ESP_OK;
}

/* Pulses ~RESET, via disp->reset_gpio directly if bench-wired (>= 0) or the
 * expander otherwise. On the expander path this can genuinely fail to mean
 * anything: if the module pinout is right, J2 pin 1 is the touch
 * controller's interrupt and the panel's reset is not brought out to the
 * connector at all, so the "reset" expander pin is driving a touch IRQ into
 * a controller this firmware doesn't talk to. That is why the software
 * reset path exists and why init does not treat a failed hardware reset as
 * fatal. See docs/HARDWARE.md and KILNCTL_DISPLAY_SWAP_DC_RESET. A bench-wired
 * reset_gpio has no such ambiguity -- it's a dedicated wire to this pin. */
static esp_err_t panel_spi_hard_reset(ILI9488Class *disp)
{
    /* asserted=true -> pin low, matching kiln_io_lcd_reset's convention
     * (active-low ~RESET). Direct-GPIO mode has no "line doesn't exist"
     * uncertainty -- unlike the expander path, a bench-wired reset GPIO is
     * unambiguously real. */
    esp_err_t err = disp->reset_gpio >= 0 ? gpio_set_level((gpio_num_t)disp->reset_gpio, 0)
                                          : kiln_io_lcd_reset(disp->io, true);
    if (err != ESP_OK) {
        ESP_LOGW(PANEL_SPI_TAG, "asserting ~RESET failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(ILI9488_RESET_PULSE_MS));  /* datasheet minimum is 10us */

    err = disp->reset_gpio >= 0 ? gpio_set_level((gpio_num_t)disp->reset_gpio, 1)
                                : kiln_io_lcd_reset(disp->io, false);
    if (err != ESP_OK) {
        ESP_LOGE(PANEL_SPI_TAG, "releasing ~RESET failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(ILI9488_RESET_WAIT_MS));   /* reset cancel: 120ms */

    /* The panel's own D/C latch state after a reset is not something we can
     * observe, and the expander pin may have been re-driven; force a fresh
     * write on the next command. */
    disp->dc_valid = false;
    return ESP_OK;
}
esp_err_t ILI9488_init(ILI9488Class *disp,
                       spi_owner_t *owner,
                       spi_host_device_t host,
                       kiln_io_t *io,
                       int cs_gpio,
                       int dc_gpio,
                       int reset_gpio,
                       const panel_desc_t *panel,
                       uint16_t panel_width,
                       uint16_t panel_height,
                       uint8_t rotation,
                       int clock_hz)
{
    /* io is mandatory unless dc_gpio bypasses the expander entirely: with
     * D/C on the expander (dc_gpio == -1), no expander means no way to send
     * even a single command. Failing here is far kinder than a driver that
     * initializes "successfully" and shows nothing. `panel` must carry a
     * real init_seq and a nonzero bytes_per_pixel -- a NULL/empty descriptor
     * (like ILI9488_get_panel_desc() briefly was in Phase 2) would otherwise
     * either init a display with no bring-up sequence or divide by zero in
     * the chunk-pixels arithmetic. */
    if (!disp || !owner || (dc_gpio < 0 && !io) || panel_width == 0 || panel_height == 0 ||
        rotation > 3 || clock_hz <= 0 || !GPIO_IS_VALID_OUTPUT_GPIO(cs_gpio) ||
        (dc_gpio >= 0 && !GPIO_IS_VALID_OUTPUT_GPIO(dc_gpio)) ||
        (reset_gpio >= 0 && !GPIO_IS_VALID_OUTPUT_GPIO(reset_gpio)) ||
        !panel || !panel->init_seq || panel->init_len == 0 || panel->bytes_per_pixel == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Re-initializing a live instance would memset away the scratch pointer,
     * the mutex and both SPI device handles without freeing any of them. */
    if (disp->dev || disp->read_dev || disp->lock || disp->scratch) {
        ESP_LOGE(PANEL_SPI_TAG, "init called on an instance that is already up");
        return ESP_ERR_INVALID_STATE;
    }

    memset(disp, 0, sizeof(*disp));
    disp->owner = owner;
    disp->io = io;
#if KILNCTL_SPI_HARDWARE_CS
    /* DISPLAY_ST7796_PLAN.md 9.4: the SPI peripheral (spics_io_num, set
     * below on dev_config/read_config) now owns this pin, so every
     * spi_owner_transfer()/spi_owner_transfer_polling() call in this file
     * must pass the -1 sentinel esp_spi_owner.c treats as "do not
     * bit-bang" instead of the real GPIO number. */
    disp->cs_gpio = -1;
#else
    disp->cs_gpio = cs_gpio;
#endif
    disp->dc_gpio = dc_gpio;
    disp->reset_gpio = reset_gpio;
    disp->panel = panel;
    disp->panel_width = panel_width;
    disp->panel_height = panel_height;
    disp->rotation = rotation;
    disp->chunk_bytes = ILI9488_SCRATCH_BYTES;
    disp->text_fg = 0xFFFF;
    disp->text_bg = 0x0000;
    disp->text_size = 2;
    disp->text_opaque = true;

    esp_err_t err;
#if !KILNCTL_SPI_HARDWARE_CS
    /* CS is bit-banged by spi_owner around each transfer (spics_io_num = -1
     * below), so it must idle high whenever no transfer is in flight -- the
     * same arrangement the MAX31856 channels use on this bus. */
    gpio_config_t cs_conf = {
        .pin_bit_mask = (1ULL << cs_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&cs_conf);
    if (err != ESP_OK) {
        ESP_LOGE(PANEL_SPI_TAG, "gpio_config(cs) failed: %s", esp_err_to_name(err));
        return err;
    }
    gpio_set_level((gpio_num_t)cs_gpio, 1);
#else
    /* DISPLAY_ST7796_PLAN.md 9.4, CONFIG_KILNCTL_SPI_HARDWARE_CS: the SPI
     * peripheral drives this pin via spics_io_num below, so it must NOT also
     * be configured/driven as a plain output GPIO here -- that would fight
     * the peripheral on the same pin. disp->cs_gpio is set to -1 further
     * down so every spi_owner_transfer() call site in this file passes the
     * sentinel esp_spi_owner.c already treats as "hardware handles CS". */
    err = ESP_OK;
#endif

    /* Bench-wiring bypass: configure D/C and/or ~RESET as bare output GPIOs
     * when their respective *_gpio is >= 0, same as CS just above. Whichever
     * one stays -1 keeps going through the expander as normal -- the two are
     * independent. RESET idles high (deasserted, not held in reset) before
     * the first explicit pulse; D/C's initial level doesn't matter because
     * dc_valid starts false and forces a write on the first command anyway. */
    if (dc_gpio >= 0) {
        gpio_config_t dc_conf = {
            .pin_bit_mask = (1ULL << dc_gpio),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&dc_conf);
        if (err != ESP_OK) {
            ESP_LOGE(PANEL_SPI_TAG, "gpio_config(dc) failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    if (reset_gpio >= 0) {
        gpio_config_t reset_conf = {
            .pin_bit_mask = (1ULL << reset_gpio),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_config(&reset_conf);
        if (err != ESP_OK) {
            ESP_LOGE(PANEL_SPI_TAG, "gpio_config(reset) failed: %s", esp_err_to_name(err));
            return err;
        }
        gpio_set_level((gpio_num_t)reset_gpio, 1);
    }

    /* DMA-capable because every pixel push goes through it and the SPI
     * peripheral will DMA straight out of it. Allocated once; no draw call
     * ever touches the heap. */
    disp->scratch = heap_caps_malloc(ILI9488_SCRATCH_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!disp->scratch) {
        ESP_LOGE(PANEL_SPI_TAG, "failed to allocate the %u-byte scratch buffer", (unsigned)ILI9488_SCRATCH_BYTES);
        return ESP_ERR_NO_MEM;
    }

    disp->lock = xSemaphoreCreateMutex();
    if (!disp->lock) {
        free(disp->scratch);
        disp->scratch = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Two device configs on one CS. The panel's write clock may go to 20 MHz
     * (twc >= 50ns) but its read clock may not exceed ~6.6 MHz (trc >= 150ns,
     * §17.4.3) -- so reads get their own, slower handle rather than
     * penalizing every write. Mode 0 (CPOL=0/CPHA=0): the controller samples
     * SDA on the rising edge of SCL. */
    spi_device_interface_config_t dev_config = {
        .clock_speed_hz = clock_hz,
        .mode = 0,
#if KILNCTL_SPI_HARDWARE_CS
        /* DISPLAY_ST7796_PLAN.md 9.4: the peripheral drives CS. disp->cs_gpio
         * is set to -1 below so spi_owner_transfer() call sites in this file
         * no longer bit-bang the same pin. */
        .spics_io_num = cs_gpio,
#else
        .spics_io_num = -1,  /* CS driven by spi_owner, not the peripheral */
#endif
        .queue_size = 1,
    };
    err = spi_bus_add_device(host, &dev_config, &disp->dev);
    if (err != ESP_OK) {
        ESP_LOGE(PANEL_SPI_TAG, "spi_bus_add_device(write) failed: %s", esp_err_to_name(err));
        ILI9488_deinit(disp);
        return err;
    }

    spi_device_interface_config_t read_config = dev_config;
    read_config.clock_speed_hz = ILI9488_READ_CLOCK_HZ;
    err = spi_bus_add_device(host, &read_config, &disp->read_dev);
    if (err != ESP_OK) {
        ESP_LOGE(PANEL_SPI_TAG, "spi_bus_add_device(read) failed: %s", esp_err_to_name(err));
        ILI9488_deinit(disp);
        return err;
    }

    /* Nothing else can hold this lock yet -- the instance is not visible to
     * any other task until init returns -- but taking it keeps the whole
     * bring-up on the same path every other caller uses, and a failure here
     * means the mutex itself is broken, which is not something to draw
     * through. Unwind what has been allocated so far rather than leak it. */
    if (!panel_spi_lock(disp)) {
        ILI9488_deinit(disp);
        return ESP_ERR_TIMEOUT;
    }

    /* A hardware reset is preferred but genuinely optional here (the reset
     * line may not exist on this connector -- see panel_spi_hard_reset), so a
     * failure falls through to the software reset rather than aborting. */
    if (panel_spi_hard_reset(disp) != ESP_OK) {
        ESP_LOGW(PANEL_SPI_TAG, "hardware reset unavailable; falling back to SWRESET");
        err = panel_spi_write_cmd(disp, ILI9488_CMD_SWRESET, NULL, 0);
        if (err != ESP_OK) {
            panel_spi_unlock(disp);
            ILI9488_deinit(disp);
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(ILI9488_RESET_WAIT_MS));
    }

    err = panel_spi_run_init_sequence(disp);
    panel_spi_unlock(disp);
    if (err != ESP_OK) {
        ESP_LOGE(PANEL_SPI_TAG, "panel init failed: %s", esp_err_to_name(err));
        ILI9488_deinit(disp);
        return err;
    }

    esp_err_t clear_err = ILI9488_clear(disp, 0x0000);
    if (clear_err != ESP_OK) {
        ESP_LOGW(PANEL_SPI_TAG, "initial clear failed: %s", esp_err_to_name(clear_err));
    }

    ESP_LOGI(PANEL_SPI_TAG, "%s initialized: %ux%u (rotation %u), %u bpp, %d Hz write / %d Hz read",
             disp->panel->name, disp->width, disp->height, disp->rotation,
             disp->panel->bytes_per_pixel, clock_hz, ILI9488_READ_CLOCK_HZ);
    return ESP_OK;
}

/* Timeout for the two I2C touch-address probes done below, matching
 * NS2009_PROBE_TIMEOUT_MS (NS2009.c) -- a chip that isn't there doesn't
 * stall bring-up. */
#define PANEL_DETECT_TOUCH_PROBE_TIMEOUT_MS 50

esp_err_t ILI9488_start(ILI9488Class *disp, spi_owner_t *owner, spi_host_device_t host, kiln_io_t *io,
                        i2c_master_bus_handle_t i2c_bus)
{
    /* DISPLAY_ST7796_PLAN.md Sec.6 Step 3/Sec.12 Phase 4: with an EXPLICIT
     * Kconfig selection (ILI9488, still the default, or ST7796) probing is
     * skipped entirely -- no extra SPI or I2C traffic, same single
     * compile-time branch Phase 3 shipped. This is also the escape hatch
     * Sec.6 Step 3 point 1 asks for, for a panel whose ID register lies. */
#if CONFIG_KILNCTL_DISPLAY_PANEL_ST7796
    const panel_desc_t *panel = ST7796_get_panel_desc();
#elif CONFIG_KILNCTL_DISPLAY_PANEL_AUTO
    /* Bring up the KCONFIG DEFAULT panel first (ILI9488 -- Sec.6 Step 3
     * point 4's fallback target) so ILI9488_read_id() has a real, already
     * fully-tested bring-up path to run on: a bootstrap-with-the-wrong-
     * panel's-init-sequence risk is not one this phase takes on speculative
     * hardware, and the STOP block means an ST7796 is never physically on
     * J2 while ILI9488 is the fallback anyway. If the resolved panel turns
     * out to differ from this bootstrap, the instance is torn down and
     * re-initialized below with the correct descriptor -- ILI9488_deinit()/
     * ILI9488_init() are already the tested teardown/bring-up pair every
     * other caller uses. */
    const panel_desc_t *fallback_panel = ILI9488_get_panel_desc();
    esp_err_t boot_err = ILI9488_init(disp, owner, host, io, DISPLAY_CS_IO, DISPLAY_DC_GPIO,
                                      DISPLAY_RESET_GPIO, fallback_panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                      (uint8_t)DISPLAY_ROTATION, DISPLAY_SPI_CLOCK_HZ);
    if (boot_err != ESP_OK) {
        ESP_LOGE(PANEL_SPI_TAG, "ILI9488_init (auto-detect bootstrap) failed: %s", esp_err_to_name(boot_err));
        return boot_err;
    }

    uint8_t id[3] = { 0, 0, 0 };
    esp_err_t id_err = ILI9488_read_id(disp, id);
    if (id_err != ESP_OK) {
        ESP_LOGW(PANEL_SPI_TAG, "panel auto-detect: RDDID read failed (%s); treating as no match",
                 esp_err_to_name(id_err));
    }

    bool touch_ns2009 = false, touch_ft6336 = false;
    if (i2c_bus) {
        touch_ns2009 = (i2c_master_probe(i2c_bus, NS2009_ADDR_A0_LOW, PANEL_DETECT_TOUCH_PROBE_TIMEOUT_MS) ==
                        ESP_OK) ||
                       (i2c_master_probe(i2c_bus, NS2009_ADDR_A0_HIGH, PANEL_DETECT_TOUCH_PROBE_TIMEOUT_MS) ==
                        ESP_OK);
        touch_ft6336 =
            i2c_master_probe(i2c_bus, FT6336U_ADDR, PANEL_DETECT_TOUCH_PROBE_TIMEOUT_MS) == ESP_OK;
    }

    const panel_detect_candidate_t candidates[] = {
        { .panel = ILI9488_get_panel_desc(), .touch = PANEL_DETECT_TOUCH_NS2009 },
        { .panel = ST7796_get_panel_desc(), .touch = PANEL_DETECT_TOUCH_FT6336 },
    };
    panel_detect_result_t detect =
        panel_detect_choose(id, touch_ns2009, touch_ft6336, candidates,
                             sizeof(candidates) / sizeof(candidates[0]), fallback_panel);

    if (detect.disagreement) {
        ESP_LOGW(PANEL_SPI_TAG, "panel auto-detect: SPI ID and touch-address signals disagree -- "
                      "RDDID read 0x%02X 0x%02X 0x%02X, touch NS2009=%d FT6336=%d, resolved %s (%s)",
                 id[0], id[1], id[2], (int)touch_ns2009, (int)touch_ft6336, detect.panel->name,
                 detect.source == PANEL_DETECT_SOURCE_FALLBACK ? "fallback" : "matched");
    }
    if (detect.source == PANEL_DETECT_SOURCE_FALLBACK) {
        ESP_LOGW(PANEL_SPI_TAG, "panel auto-detect: no RDDID match (read 0x%02X 0x%02X 0x%02X against %u "
                      "candidate(s)) -- record these bytes in DISPLAY_ST7796_PLAN.md Sec.4, "
                      "booting the Kconfig default (%s) meanwhile",
                 id[0], id[1], id[2], (unsigned)detect.matched_count, detect.panel->name);
    } else {
        ESP_LOGI(PANEL_SPI_TAG, "panel auto-detect: resolved %s (RDDID 0x%02X 0x%02X 0x%02X, %s)",
                 detect.panel->name, id[0], id[1], id[2],
                 detect.source == PANEL_DETECT_SOURCE_SPI_MATCH ? "SPI match" : "touch tiebreak");
    }

    const panel_desc_t *panel = detect.panel;
    if (panel != fallback_panel) {
        ILI9488_deinit(disp);
        esp_err_t reinit_err = ILI9488_init(disp, owner, host, io, DISPLAY_CS_IO, DISPLAY_DC_GPIO,
                                            DISPLAY_RESET_GPIO, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                            (uint8_t)DISPLAY_ROTATION, DISPLAY_SPI_CLOCK_HZ);
        if (reinit_err != ESP_OK) {
            ESP_LOGE(PANEL_SPI_TAG, "ILI9488_init (auto-detect resolved panel %s) failed: %s", panel->name,
                     esp_err_to_name(reinit_err));
            return reinit_err;
        }
    }

    /* The bootstrap/resolved instance above already ran ILI9488_init() (and
     * printed its own "initialized: ..." line) -- skip straight to the boot
     * banner rather than falling into the shared init call below. */
    ILI9488_set_text_style(disp, 0xFFFF, 0x0000, 3, true);
    ILI9488_set_text_cursor(disp, 8, 8);
    ILI9488_printf(disp, "kilnCtl ready");
    return ESP_OK;
#else
    const panel_desc_t *panel = ILI9488_get_panel_desc();
#endif

    /* DISPLAY_DC_GPIO/DISPLAY_RESET_GPIO are -1 unless
     * KILNCTL_DISPLAY_DC_RESET_DIRECT_GPIO is set in menuconfig, in which
     * case ILI9488_init bypasses the expander for whichever line has a real
     * GPIO number -- see settings.h and the Kconfig help text. */
    esp_err_t err = ILI9488_init(disp, owner, host, io, DISPLAY_CS_IO,
                                 DISPLAY_DC_GPIO, DISPLAY_RESET_GPIO, panel,
                                 DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                 (uint8_t)DISPLAY_ROTATION, DISPLAY_SPI_CLOCK_HZ);
    if (err != ESP_OK) {
        ESP_LOGE(PANEL_SPI_TAG, "ILI9488_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* A boot banner, so the panel proves it is alive before anything connects
     * over UART -- the same reason SSD1306_start printed one. */
    ILI9488_set_text_style(disp, 0xFFFF, 0x0000, 3, true);
    ILI9488_set_text_cursor(disp, 8, 8);
    ILI9488_printf(disp, "kilnCtl ready");
    return ESP_OK;
}

esp_err_t ILI9488_deinit(ILI9488Class *disp)
{
    if (!disp) return ESP_ERR_INVALID_ARG;

    esp_err_t err = ESP_OK;

    /* Take the lock and drop the device handles under it, so a draw call that
     * is already inside a transfer finishes before the scratch buffer it is
     * DMAing out of, or the mutex it is holding, is freed. Clearing `dev` is
     * what makes every subsequent panel_spi_ready() fail. */
    bool locked = disp->lock && panel_spi_lock(disp);
    spi_device_handle_t dev = disp->dev;
    spi_device_handle_t read_dev = disp->read_dev;
    disp->dev = NULL;
    disp->read_dev = NULL;
    panel_spi_blit_clear_state(disp);

    if (read_dev) {
        esp_err_t e = spi_bus_remove_device(read_dev);
        if (e != ESP_OK) err = e;
    }
    if (dev) {
        esp_err_t e = spi_bus_remove_device(dev);
        if (e != ESP_OK) err = e;
    }

    /* Scratch and mutex last: with both device handles gone nothing can start
     * a new transfer, so this is the point at which they have no more work to
     * protect. */
    uint8_t *scratch = disp->scratch;
    disp->scratch = NULL;
    free(scratch);

    if (disp->lock) {
        SemaphoreHandle_t lock = disp->lock;
        disp->lock = NULL;
        if (locked) xSemaphoreGive(lock);
        vSemaphoreDelete(lock);
    }

    /* The SPI bus, its spi_owner task and the expander handle all belong to
     * whoever created them; this driver only borrowed them. */
    disp->owner = NULL;
    disp->io = NULL;
    return err;
}

/* ===================================================================
 * Panel state
 * =================================================================== */

esp_err_t ILI9488_reset(ILI9488Class *disp, bool hard)
{
    if (!panel_spi_ready(disp)) return ESP_ERR_INVALID_STATE;

    if (!panel_spi_lock(disp)) return ESP_ERR_TIMEOUT;
    /* A reset while a blit is open is not an "error" so much as an implicit
     * abandonment of the window -- but it still leaves the caller's stream
     * unfinished, so it is reported the same way. */
    panel_spi_blit_clear_state(disp);

    esp_err_t err;
    if (hard) {
        err = panel_spi_hard_reset(disp);
    } else {
        err = panel_spi_write_cmd(disp, ILI9488_CMD_SWRESET, NULL, 0);
        if (err == ESP_OK) {
            /* §5.2.2: 5ms before the next command, 120ms if the reset was
             * issued out of sleep-out mode. We cannot always know which, so
             * we always wait the long one. */
            vTaskDelay(pdMS_TO_TICKS(ILI9488_RESET_WAIT_MS));
        }
    }

    if (err == ESP_OK) {
        /* Both reset flavours restore power-on register defaults, including
         * COLMOD = 06h. Without re-running init every subsequent pixel would
         * be decoded in the wrong format. */
        err = panel_spi_run_init_sequence(disp);
    }
    panel_spi_unlock(disp);

    if (err == ESP_OK) {
        err = ILI9488_clear(disp, 0x0000);
    }
    return err;
}

esp_err_t ILI9488_set_power(ILI9488Class *disp, bool on)
{
    if (!panel_spi_ready(disp)) return ESP_ERR_INVALID_STATE;

    if (!panel_spi_lock(disp)) return ESP_ERR_TIMEOUT;
    esp_err_t err = panel_spi_reject_if_blitting(disp);
    if (err == ESP_OK) {
        if (on) {
            err = panel_spi_write_cmd(disp, ILI9488_CMD_SLPOUT, NULL, 0);
            if (err == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(ILI9488_SLEEP_WAIT_MS));
                err = panel_spi_write_cmd(disp, ILI9488_CMD_DISPON, NULL, 0);
            }
        } else {
            err = panel_spi_write_cmd(disp, ILI9488_CMD_DISPOFF, NULL, 0);
            if (err == ESP_OK) {
                err = panel_spi_write_cmd(disp, ILI9488_CMD_SLPIN, NULL, 0);
                /* §5.2.x: 120ms must elapse after SLPIN before SLPOUT is
                 * accepted. Waiting here rather than in set_power(true) means
                 * a caller toggling power twice in a row cannot violate it. */
                vTaskDelay(pdMS_TO_TICKS(ILI9488_SLEEP_WAIT_MS));
            }
        }
    }
    panel_spi_unlock(disp);
    return err;
}

esp_err_t ILI9488_set_rotation(ILI9488Class *disp, uint8_t rotation)
{
    if (!panel_spi_ready(disp)) return ESP_ERR_INVALID_STATE;
    if (rotation > 3) return ESP_ERR_INVALID_ARG;

    if (!panel_spi_lock(disp)) return ESP_ERR_TIMEOUT;
    esp_err_t err = panel_spi_reject_if_blitting(disp);
    if (err == ESP_OK) {
        err = panel_spi_apply_rotation(disp, rotation);
        if (err == ESP_OK) {
            /* Rotating does not move existing pixels -- it only changes how
             * new writes are addressed -- so the cursor is homed rather than
             * left pointing at a coordinate that may no longer exist. */
            disp->cursor_x = 0;
            disp->cursor_y = 0;
        }
    }
    panel_spi_unlock(disp);
    return err;
}

esp_err_t ILI9488_set_invert(ILI9488Class *disp, bool invert)
{
    if (!panel_spi_ready(disp)) return ESP_ERR_INVALID_STATE;

    if (!panel_spi_lock(disp)) return ESP_ERR_TIMEOUT;
    esp_err_t err = panel_spi_reject_if_blitting(disp);
    if (err == ESP_OK) {
        err = panel_spi_write_cmd(disp, invert ? ILI9488_CMD_INVON : ILI9488_CMD_INVOFF, NULL, 0);
    }
    panel_spi_unlock(disp);
    return err;
}

esp_err_t ILI9488_get_dimensions(ILI9488Class *disp, uint16_t *out_width, uint16_t *out_height)
{
    if (!disp || !out_width || !out_height) return ESP_ERR_INVALID_ARG;
    /* The readiness check is what stops the lock below being taken on a NULL
     * mutex -- this used to be the one public call that skipped it, so
     * asking an un-inited (or deinit'd) instance for its size faulted. */
    if (!panel_spi_ready(disp)) return ESP_ERR_INVALID_STATE;
    if (!panel_spi_lock(disp)) return ESP_ERR_TIMEOUT;
    *out_width = disp->width;
    *out_height = disp->height;
    panel_spi_unlock(disp);
    return ESP_OK;
}

/* The panel_desc_t descriptor (DISPLAY_ST7796_PLAN.md Sec.6 Step 2) and
 * ILI9488_get_panel_desc() live in panel_spi.c, next to panel_spi_init_bytes[]
 * -- moved there (rather than kept here with the rest of bring-up) so the
 * descriptor's `.init_len = sizeof(panel_spi_init_bytes)` can see that
 * array's real, complete type; an extern declaration of an array whose size
 * is fixed by its own initializer cannot be sizeof()'d from a different
 * translation unit. */
