// recovery_lcd.c -- see recovery_lcd.h.
#include "recovery_lcd.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "recovery_image_check.h"
#include "recovery_io.h"
#include "recovery_lcd_policy.h"
#include "recovery_passphrase.h"
#include "recovery_text.h"

static const char *TAG = "recovery_lcd";

#define LCD_SPI_HOST   SPI2_HOST
#define LCD_SCLK_GPIO  12
#define LCD_MOSI_GPIO  11
#define LCD_CS_GPIO    21
#define LCD_BL_GPIO    15

// Landscape (MADCTL MV set): 480 wide x 320 tall.
#define PANEL_W 480
#define PANEL_H 320

// ST7796 commands
#define CMD_SLPOUT 0x11
#define CMD_NORON  0x13
#define CMD_DISPON 0x29
#define CMD_CASET  0x2A
#define CMD_RASET  0x2B
#define CMD_RAMWR  0x2C
#define CMD_MADCTL 0x36
#define CMD_COLMOD 0x3A

#define MADCTL_LANDSCAPE 0x28 // MV | BGR

// st7796_panel.c init table (vendor sequence), minus its MADCTL (0x36) entry,
// which is written separately with the landscape value, and minus its
// trailing NORON/SLPOUT/DISPON, which are issued with delays below.
// Format: cmd, nargs, args...; terminated by cmd 0x00.
static const uint8_t k_init_seq[] = {
    0xF0, 1, 0xC3,
    0xF0, 1, 0x96,
    0x3A, 1, 0x05,
    0xB0, 1, 0x80,
    0xB6, 2, 0x00, 0x02,
    0xB5, 4, 0x02, 0x03, 0x00, 0x04,
    0xB1, 2, 0x80, 0x10,
    0xB4, 1, 0x00,
    0xB7, 1, 0xC6,
    0xC5, 1, 0x1C,
    0xE4, 1, 0x31,
    0xE8, 8, 0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33,
    0xC2, 0,
    0xA7, 0,
    0xE0, 14, 0xF0, 0x09, 0x13, 0x12, 0x12, 0x2B, 0x3C, 0x44,
              0x4B, 0x1B, 0x18, 0x17, 0x1D, 0x21,
    0xE1, 14, 0xF0, 0x09, 0x13, 0x0C, 0x0D, 0x27, 0x3B, 0x44,
              0x4D, 0x0B, 0x17, 0x17, 0x1D, 0x21,
    0xF0, 1, 0x3C,
    0xF0, 1, 0x69,
    0x00,
};

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define COL_BG    RGB565(0, 0, 0)
#define COL_TITLE RGB565(255, 176, 0)
#define COL_TEXT  RGB565(235, 235, 235)
#define COL_DIM   RGB565(150, 150, 150)
#define COL_OK    RGB565(60, 220, 90)
#define COL_FAULT RGB565(255, 255, 255)
#define COL_FAULT_BG RGB565(200, 0, 0)

// Layout (y of each line's top edge), 480x320, no scrolling. Glyph height is
// 7*scale px and advance 6*scale px/char: scale 2 = 14 px / 12 px, scale 3 =
// 21 px / 18 px, scale 6 = 42 px / 36 px (12-char passphrase = 432 px wide).
#define TITLE_SCALE 3
#define TEXT_SCALE  2
#define NET_SCALE   3
#define PASS_SCALE  6
#define Y_TITLE   4
#define Y_SUB     30
#define Y_BOOT    50
#define Y_RESET   68
#define Y_CRASH   86
#define Y_RELAY   104
#define Y_JOIN    126 // "Join Wi-Fi network:" / NO NETWORK
#define Y_SSID    144
#define Y_PWLBL   172 // "Password:"
#define Y_PASS    188
#define Y_IP      238
#define Y_URL     264
#define Y_NOTE    290

static spi_device_handle_t s_spi;
static volatile bool s_ready = false;
static bool s_dc_data = true; // tracks D/C so we only touch the expander on change
static DMA_ATTR uint8_t s_line[PANEL_W * 2];
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;

// Network state, recorded even before the panel is up. s_net_pass is this
// boot's random SoftAP passphrase: the LCD is its only output (owner decision
// 2026-10-02), it is RAM only and never logged.
static bool s_net_set = false;
static char s_net_name[33];
static char s_net_pass[RPASS_LEN + 1];
static char s_net_ip[16];
static bool s_net_none = false;      // every Wi-Fi bring-up path failed
static bool s_net_storage_fail = false; // driver not RAM-only: AP refused
static int s_ap_down = 0;       // SoftAP stopped: do not show its credentials
static char s_err_head[24];   // generic fatal error (e.g. "HTTP FAILED"), "" = none
static char s_err_detail[28];
static bool s_drawn_relay_fault = false; // what the last draw_status() showed

// Panel health, reported read-only through GET /api/recovery/status.
static bool s_bus_up = false;   // SPI bus + device already added (never re-added)
static volatile uint32_t s_init_attempts = 0;
static volatile uint32_t s_draw_failures = 0;
static volatile bool s_expander_err = false; // last attempt failed on an expander I2C write
static int s_rereset_count = 0;              // hard re-resets done this boot (capped)
static TaskHandle_t s_lcd_task;
static int s_line_errs = 0;   // failed draw_line() calls in the current draw pass

// Status facts gathered once at show_message().
static int s_boot_count = -1; // -1 unless the record decoded, 0.. = persisted count
static rlcd_bg_state_t s_bg_state = RLCD_BG_UNREADABLE;
static int s_reset_reason = 0;
static int s_crash_state = 0; // 0 none, 1 coredump present, -1 unknown

// --- low-level panel I/O ---------------------------------------------------

static esp_err_t spi_tx(const uint8_t *buf, size_t len)
{
    spi_transaction_t t = {0};
    t.length = len * 8;
    if (len <= 4) {
        t.flags = SPI_TRANS_USE_TXDATA;
        memcpy(t.tx_data, buf, len);
    } else {
        t.tx_buffer = buf;
    }
    return spi_device_transmit(s_spi, &t);
}

// Every expander write for the LCD goes through here so an I2C failure is
// distinguishable from an SPI one (only the former justifies re-resetting the
// expander).
static esp_err_t lcd_pins(bool dc_high, bool reset_high)
{
    esp_err_t err = recovery_io_set_lcd_pins(dc_high, reset_high);
    if (err != ESP_OK) {
        s_expander_err = true;
    }
    return err;
}

static esp_err_t set_dc(bool data)
{
    if (data == s_dc_data) {
        return ESP_OK;
    }
    esp_err_t err = lcd_pins(data, true);
    if (err == ESP_OK) {
        s_dc_data = data;
    }
    return err;
}

static esp_err_t send_cmd(uint8_t cmd)
{
    esp_err_t err = set_dc(false);
    return err != ESP_OK ? err : spi_tx(&cmd, 1);
}

static esp_err_t send_data(const uint8_t *buf, size_t len)
{
    if (len == 0) {
        return ESP_OK;
    }
    esp_err_t err = set_dc(true);
    return err != ESP_OK ? err : spi_tx(buf, len);
}

static esp_err_t write_cmd(uint8_t cmd, const uint8_t *data, size_t len)
{
    esp_err_t err = send_cmd(cmd);
    return err != ESP_OK ? err : send_data(data, len);
}

static esp_err_t set_window(int x0, int y0, int x1, int y1)
{
    uint8_t ca[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
    uint8_t ra[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1};
    esp_err_t err = write_cmd(CMD_CASET, ca, 4);
    if (err != ESP_OK) {
        return err;
    }
    err = write_cmd(CMD_RASET, ra, 4);
    return err != ESP_OK ? err : send_cmd(CMD_RAMWR);
}

static esp_err_t spi_init(void)
{
    if (s_bus_up) {
        return ESP_OK; // a retry must not re-initialise the bus or re-add the device
    }
    // The three MAX31856 ~CS lines (KILNCTL_THERMO_CS0/1/2_IO = GPIO14/17/18,
    // App/drivers/Kconfig) share this SPI bus; park them high as outputs so
    // panel traffic can never select a thermocouple chip.
    static const int tc_cs[] = {14, 17, 18};
    for (size_t i = 0; i < sizeof(tc_cs) / sizeof(tc_cs[0]); i++) {
        gpio_set_level(tc_cs[i], 1);
        gpio_config_t cs = {.pin_bit_mask = 1ULL << tc_cs[i], .mode = GPIO_MODE_OUTPUT};
        gpio_config(&cs);
        gpio_set_level(tc_cs[i], 1);
    }
    spi_bus_config_t bus = {
        .mosi_io_num = LCD_MOSI_GPIO,
        .miso_io_num = -1,
        .sclk_io_num = LCD_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = (int)sizeof(s_line),
    };
    esp_err_t err = spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 20 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = LCD_CS_GPIO,
        .queue_size = 2,
    };
    err = spi_bus_add_device(LCD_SPI_HOST, &dev, &s_spi);
    if (err != ESP_OK) {
        (void)spi_bus_free(LCD_SPI_HOST); // so the next attempt starts clean
        return err;
    }
    s_bus_up = true;
    return ESP_OK;
}

static esp_err_t panel_init(void)
{
    // Hardware reset through expander IO14 (~RESET), D/C parked high.
    esp_err_t err = lcd_pins(true, true);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    (void)lcd_pins(true, false);
    vTaskDelay(pdMS_TO_TICKS(20));
    err = lcd_pins(true, true);
    if (err != ESP_OK) {
        return err;
    }
    s_dc_data = true;
    vTaskDelay(pdMS_TO_TICKS(120));

    for (const uint8_t *p = k_init_seq; p[0] != 0x00;) {
        uint8_t cmd = p[0];
        uint8_t n = p[1];
        err = write_cmd(cmd, p + 2, n);
        if (err != ESP_OK) {
            return err;
        }
        p += 2 + n;
    }
    uint8_t madctl = MADCTL_LANDSCAPE;
    err = write_cmd(CMD_MADCTL, &madctl, 1);
    if (err != ESP_OK) {
        return err;
    }
    err = write_cmd(CMD_NORON, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    err = write_cmd(CMD_SLPOUT, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    err = write_cmd(CMD_DISPON, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    return err;
}

// --- drawing ---------------------------------------------------------------

// Draws one full-width text line whose top edge is `y`. The whole band is
// repainted (text padded with bg), so a shorter string fully replaces a
// longer one.
static esp_err_t draw_line_raw(int y, int scale, uint16_t fg, uint16_t bg, const char *text)
{
    int h = RECOVERY_FONT_H * scale;
    esp_err_t err = set_window(0, y, PANEL_W - 1, y + h - 1);
    if (err != ESP_OK) {
        return err;
    }
    for (int row = 0; row < RECOVERY_FONT_H; row++) {
        recovery_text_scanline(text, scale, row, fg, bg, PANEL_W, s_line);
        for (int k = 0; k < scale; k++) {
            err = send_data(s_line, sizeof(s_line));
            if (err != ESP_OK) {
                return err;
            }
        }
    }
    return ESP_OK;
}

// Every draw_line() result is aggregated into s_line_errs; draw_status()
// turns any failure into s_ready=false so the retry task redraws.
static esp_err_t draw_line(int y, int scale, uint16_t fg, uint16_t bg, const char *text)
{
    esp_err_t err = draw_line_raw(y, scale, fg, bg, text);
    if (err != ESP_OK) {
        s_line_errs++;
    }
    return err;
}

static esp_err_t clear_screen(void)
{
    esp_err_t err = set_window(0, 0, PANEL_W - 1, PANEL_H - 1);
    if (err != ESP_OK) {
        return err;
    }
    recovery_text_scanline("", 1, 0, COL_BG, COL_BG, PANEL_W, s_line);
    for (int y = 0; y < PANEL_H; y++) {
        err = send_data(s_line, sizeof(s_line));
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static void draw_status(void)
{
    char buf[64];

    s_line_errs = 0;
    (void)draw_line(Y_TITLE, TITLE_SCALE, COL_TITLE, COL_BG, "RECOVERY MODE");
    (void)draw_line(Y_SUB, TEXT_SCALE, COL_DIM, COL_BG, "KilnFW recovery image");

    if (recovery_io_nvs_failed_mask() != 0) {
        (void)draw_line(Y_BOOT, TEXT_SCALE, COL_FAULT, COL_FAULT_BG, "NVS UNAVAILABLE");
        buf[0] = 0;
    } else if (s_bg_state == RLCD_BG_VALID) {
        snprintf(buf, sizeof(buf), "Boot guard count: %d", s_boot_count);
    } else if (s_bg_state == RLCD_BG_INVALID) {
        (void)draw_line(Y_BOOT, TEXT_SCALE, COL_FAULT, COL_FAULT_BG, "BOOT GUARD RECORD INVALID");
        buf[0] = 0;
    } else if (s_bg_state == RLCD_BG_UNREADABLE) {
        snprintf(buf, sizeof(buf), "Boot guard count: unreadable");
    } else {
        snprintf(buf, sizeof(buf), "Boot guard count: none");
    }
    if (buf[0]) {
        (void)draw_line(Y_BOOT, TEXT_SCALE, COL_TEXT, COL_BG, buf);
    }

    snprintf(buf, sizeof(buf), "Reset reason: %s", recovery_reset_reason_name(s_reset_reason));
    (void)draw_line(Y_RESET, TEXT_SCALE, COL_TEXT, COL_BG, buf);

    snprintf(buf, sizeof(buf), "Crash dump: %s",
             s_crash_state > 0 ? "present" : (s_crash_state == 0 ? "none" : "unknown"));
    (void)draw_line(Y_CRASH, TEXT_SCALE, COL_TEXT, COL_BG, buf);

    s_drawn_relay_fault = recovery_io_relay_fault();
    if (s_drawn_relay_fault) {
        (void)draw_line(Y_RELAY, TEXT_SCALE, COL_FAULT, COL_FAULT_BG, "RELAY CTRL FAULT");
    } else {
        (void)draw_line(Y_RELAY, TEXT_SCALE, COL_OK, COL_BG, "Heat: OFF");
    }

    if (s_err_head[0] || s_ap_down) {
        // Overrides every network state: never show a passphrase for a server
        // that is not there.
        (void)draw_line(Y_JOIN, TEXT_SCALE, COL_FAULT, COL_FAULT_BG, s_err_head[0] ? s_err_head : "AP DOWN");
        (void)draw_line(Y_SSID, TEXT_SCALE, COL_DIM, COL_BG, s_err_head[0] ? s_err_detail
                                                        : (s_ap_down == 2 ? "AP could not restart" : "restarting the SoftAP"));
        (void)draw_line(Y_PWLBL, TEXT_SCALE, COL_DIM, COL_BG, "Use JTAG if it persists");
        (void)draw_line(Y_PASS, PASS_SCALE, COL_BG, COL_BG, "");
        (void)draw_line(Y_IP, NET_SCALE, COL_BG, COL_BG, "");
        (void)draw_line(Y_URL, TEXT_SCALE, COL_BG, COL_BG, "");
        (void)draw_line(Y_NOTE, TEXT_SCALE, COL_BG, COL_BG, "");
    } else if (s_net_none || s_net_storage_fail) {
        // TEXT_SCALE: a NET_SCALE band (21 px from Y_JOIN) would overlap Y_SSID.
        (void)draw_line(Y_JOIN, TEXT_SCALE, COL_FAULT, COL_FAULT_BG,
                        s_net_storage_fail ? "WIFI STORAGE FAIL" : "NO NETWORK");
        (void)draw_line(Y_SSID, TEXT_SCALE, COL_DIM, COL_BG,
                        s_net_storage_fail ? "AP not started" : "Wi-Fi bring-up failed");
        (void)draw_line(Y_PWLBL, TEXT_SCALE, COL_DIM, COL_BG, "Power-cycle or use JTAG");
        (void)draw_line(Y_PASS, PASS_SCALE, COL_BG, COL_BG, "");
        (void)draw_line(Y_IP, NET_SCALE, COL_BG, COL_BG, "");
        (void)draw_line(Y_URL, TEXT_SCALE, COL_BG, COL_BG, "");
        (void)draw_line(Y_NOTE, TEXT_SCALE, COL_BG, COL_BG, "");
    } else if (s_net_set) {
        (void)draw_line(Y_JOIN, TEXT_SCALE, COL_DIM, COL_BG, "Join Wi-Fi network:");
        // A 32-char SSID needs scale 2 (384 px); shorter ones get scale 3.
        (void)draw_line(Y_SSID, strlen(s_net_name) <= 26 ? NET_SCALE : TEXT_SCALE, COL_TEXT, COL_BG,
                        s_net_name);
        (void)draw_line(Y_PWLBL, TEXT_SCALE, COL_DIM, COL_BG, "Password:");
        (void)draw_line(Y_PASS, PASS_SCALE, COL_TITLE, COL_BG, s_net_pass);
        snprintf(buf, sizeof(buf), "IP: %s", s_net_ip);
        (void)draw_line(Y_IP, NET_SCALE, COL_TEXT, COL_BG, buf);
        snprintf(buf, sizeof(buf), "Open http://%s/ to upload", s_net_ip);
        (void)draw_line(Y_URL, TEXT_SCALE, COL_TITLE, COL_BG, buf);
        (void)draw_line(Y_NOTE, TEXT_SCALE, COL_DIM, COL_BG, "New password each boot");
    } else {
        (void)draw_line(Y_JOIN, TEXT_SCALE, COL_DIM, COL_BG, "Network: starting...");
        (void)draw_line(Y_SSID, NET_SCALE, COL_BG, COL_BG, "");
        (void)draw_line(Y_PWLBL, TEXT_SCALE, COL_BG, COL_BG, "");
        (void)draw_line(Y_PASS, PASS_SCALE, COL_BG, COL_BG, "");
        (void)draw_line(Y_IP, NET_SCALE, COL_BG, COL_BG, "");
        (void)draw_line(Y_URL, TEXT_SCALE, COL_BG, COL_BG, "");
        (void)draw_line(Y_NOTE, TEXT_SCALE, COL_BG, COL_BG, "");
    }

    if (!rlcd_draw_ok(s_line_errs)) {
        // A dropped line may be the passphrase: not ready, so the 1 Hz task
        // re-inits and redraws the whole screen.
        s_draw_failures++;
        s_ready = false;
        ESP_LOGE(TAG, "status draw failed on %d line(s)", s_line_errs);
    }
}

// --- status gathering ------------------------------------------------------

// boot_guard.c: record is {u8 version; u8 reserved[3]; u32 boot_count;
// u32 crc32}; crc32 is the standard reflected CRC-32 over the first 8 bytes,
// which is what esp_rom_crc32_le(0, ...) computes (boot_guard.c's own comment
// says the same of its local copy).

static void gather_status(void)
{
    s_reset_reason = (int)esp_reset_reason();

    // Read-only look at the boot_guard record (recovery never writes it
    // here). The partition has to be initialised before nvs_open_from_partition().
    s_boot_count = -1;
    s_bg_state = RLCD_BG_UNREADABLE;
    // Only ESP_ERR_NVS_NOT_FOUND means "no record" (count 0); a record of the
    // wrong size/version/CRC is "invalid"; any other NVS error is "unreadable".
    if (nvs_flash_init_partition("kiln_nvs") == ESP_OK) {
        nvs_handle_t h;
        esp_err_t oerr = nvs_open_from_partition("kiln_nvs", "kiln_cfg", NVS_READONLY, &h);
        if (oerr == ESP_OK) {
            uint8_t rec[12];
            size_t len = sizeof(rec);
            esp_err_t gerr = nvs_get_blob(h, "bootguard", rec, &len);
            rlcd_get_rc_t rc = gerr == ESP_OK ? RLCD_GET_OK
                             : gerr == ESP_ERR_NVS_NOT_FOUND ? RLCD_GET_NOT_FOUND
                             : gerr == ESP_ERR_NVS_INVALID_LENGTH ? RLCD_GET_BAD_LENGTH
                                                                  : RLCD_GET_OTHER;
            uint32_t count = 0;
            bool decoded = gerr == ESP_OK && ric_boot_guard_decode(rec, len, &count) != 0;
            s_bg_state = rlcd_classify_boot_guard(rc, decoded);
            if (s_bg_state == RLCD_BG_VALID) {
                s_boot_count = (int)count;
            } else if (s_bg_state == RLCD_BG_NONE) {
                s_boot_count = 0;
            }
            nvs_close(h);
        } else if (oerr == ESP_ERR_NVS_NOT_FOUND) {
            s_bg_state = RLCD_BG_NONE; // namespace never created
            s_boot_count = 0;
        }
    }

    // Coredump presence: first word of the coredump data partition is
    // 0xFFFFFFFF while erased.
    const esp_partition_t *cd = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                         ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    s_crash_state = -1;
    if (cd) {
        uint32_t w = 0;
        if (esp_partition_read(cd, 0, &w, sizeof(w)) == ESP_OK) {
            s_crash_state = (w != 0xFFFFFFFFu) ? 1 : 0;
        }
    }
}

// --- bring-up and retry -----------------------------------------------------

// One init attempt (caller holds s_lock): bus, panel, clear, full draw. True
// only if the whole screen drew (draw_status() clears s_ready on any failure).
static bool lcd_init_once(void)
{
    s_expander_err = false;
    esp_err_t err = spi_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel SPI bus init failed: %s", esp_err_to_name(err));
        return false;
    }
    err = panel_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel init sequence failed (expander down or SPI error): %s",
                 esp_err_to_name(err));
        return false;
    }
    err = clear_screen();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel clear failed: %s", esp_err_to_name(err));
        return false;
    }
    s_ready = true;
    draw_status(); // clears s_ready again if any line failed
    return s_ready;
}

// A burst of up to RLCD_BURST_ATTEMPTS attempts (caller holds s_lock). The
// SX1509 is re-reset between attempts only after an expander I2C failure and
// only up to RLCD_MAX_RERESETS times per boot; SPI failures retry panel-only.
static bool lcd_bring_up_burst(void)
{
    for (int a = 0;; a++) {
        s_init_attempts++;
        if (a > 0 && rlcd_rereset_allowed(s_expander_err, s_rereset_count)) {
            s_rereset_count++;
            if (!recovery_io_expander_rehold()) {
                ESP_LOGW(TAG, "expander re-reset before LCD attempt %d did not verify", a + 1);
            }
        }
        bool ok = lcd_init_once();
        if (!rlcd_burst_continue(a + 1, ok)) {
            return ok;
        }
    }
}

#define LCD_TASK_STACK_BYTES 4096
#define LCD_TASK_PERIOD_MS   1000

static void lcd_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(LCD_TASK_PERIOD_MS));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (rlcd_tick_action(s_ready) == RLCD_TICK_WARN_AND_RETRY) {
            ESP_LOGE(TAG, RLCD_NOT_READY_MSG); // never the passphrase itself
            if (lcd_bring_up_burst()) {
                ESP_LOGI(TAG, "LCD recovered after %u init attempts", (unsigned)s_init_attempts);
            }
        }
        xSemaphoreGive(s_lock);
    }
}

void recovery_lcd_get_status(recovery_lcd_status_t *out)
{
    out->ready = s_ready;
    out->init_attempts = s_init_attempts;
    out->draw_failures = s_draw_failures;
    out->task_stack_free_bytes =
        s_lcd_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_lcd_task) : 0;
}

// --- public API ------------------------------------------------------------

void recovery_lcd_show_message(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }

    gather_status();

    // Backlight: plain GPIO high (flying-wire pin, active high).
    gpio_config_t bl = {.pin_bit_mask = 1ULL << LCD_BL_GPIO, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&bl);
    gpio_set_level(LCD_BL_GPIO, 1);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (lcd_bring_up_burst()) {
        ESP_LOGI(TAG, "status screen drawn (boot_count=%d reset=%s crash=%d relay_fault=%d)",
                 s_boot_count, recovery_reset_reason_name(s_reset_reason), s_crash_state,
                 (int)recovery_io_relay_fault());
    }
    xSemaphoreGive(s_lock);

    // Keep re-attempting while the panel is not ready: the passphrase is shown
    // nowhere else. Failure to start the task is logged; never fatal.
    if (xTaskCreate(lcd_task, "lcd_retry", LCD_TASK_STACK_BYTES, NULL, 2, &s_lcd_task) != pdPASS) {
        s_lcd_task = NULL;
        ESP_LOGE(TAG, "could not start the LCD retry task");
    }
}

void recovery_lcd_set_ap(const char *ssid, const char *passphrase, const char *ip)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_net_name, sizeof(s_net_name), "%s", ssid ? ssid : "");
    snprintf(s_net_pass, sizeof(s_net_pass), "%s", passphrase ? passphrase : "");
    snprintf(s_net_ip, sizeof(s_net_ip), "%s", ip ? ip : "?");
    s_net_set = true;
    s_net_none = false;
    s_net_storage_fail = false;
    if (s_ready) {
        draw_status();
    }
    xSemaphoreGive(s_lock);
}

void recovery_lcd_set_no_network(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_net_none = true;
    if (s_ready) {
        draw_status();
    }
    xSemaphoreGive(s_lock);
}

void recovery_lcd_set_wifi_storage_fail(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_net_storage_fail = true;
    if (s_ready) {
        draw_status();
    }
    xSemaphoreGive(s_lock);
}

void recovery_lcd_set_error(const char *headline, const char *detail)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_err_head, sizeof(s_err_head), "%s", headline && headline[0] ? headline : "ERROR");
    snprintf(s_err_detail, sizeof(s_err_detail), "%s", detail ? detail : "");
    if (s_ready) {
        draw_status();
    }
    xSemaphoreGive(s_lock);
}

void recovery_lcd_clear_error(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool had = s_err_head[0] != 0;
    s_err_head[0] = 0;
    s_err_detail[0] = 0;
    if (had && s_ready) {
        draw_status();
    }
    xSemaphoreGive(s_lock);
}

void recovery_lcd_set_ap_state(int state)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = s_ap_down != state;
    s_ap_down = state;
    if (changed && s_ready) {
        draw_status();
    }
    xSemaphoreGive(s_lock);
}

bool recovery_lcd_is_ok(void)
{
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_ready;
    xSemaphoreGive(s_lock);
    return ok;
}

void recovery_lcd_poll_relay_fault(void)
{
    // The relay-hold task (3 KiB stack) must never draw. The lcd_retry task only
    // acts while the panel is not ready, so the HTTP task calls this (cheap, no-op unless the fault state
    // changed since the last draw) from the status route.
    if (!s_lock || !s_ready || recovery_io_relay_fault() == s_drawn_relay_fault) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    draw_status();
    xSemaphoreGive(s_lock);
}