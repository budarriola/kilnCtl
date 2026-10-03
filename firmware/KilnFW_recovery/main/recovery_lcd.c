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

#include "recovery_io.h"
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

// Layout (y of each line's top edge). Scale 2 => 12 px/char, 40 chars/line,
// 14 px glyph height; scale 4 title => 24 px/char, 28 px height.
#define TITLE_SCALE 4
#define TEXT_SCALE  2
#define Y_TITLE   12
#define Y_SUB     56
#define Y_BOOT    96
#define Y_RESET   128
#define Y_CRASH   160
#define Y_RELAY   192
#define Y_AUTH    212
#define Y_NET     232
#define Y_IP      264
#define Y_HELP    292

static spi_device_handle_t s_spi;
static bool s_ready = false;
static bool s_dc_data = true; // tracks D/C so we only touch the expander on change
static DMA_ATTR uint8_t s_line[PANEL_W * 2];
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;

// Network line state, recorded even before the panel is up.
static bool s_net_set = false;
static bool s_net_is_ap = false;
static char s_net_name[33];
static char s_net_ip[16];
static bool s_net_none = false;      // every Wi-Fi bring-up path failed
static bool s_auth_fallback = false; // challenge key derived from the fallback secret

// Status facts gathered once at show_message().
static int s_boot_count = -1; // -1 unreadable, 0.. = persisted count
static bool s_boot_record_present = false;
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

static esp_err_t set_dc(bool data)
{
    if (data == s_dc_data) {
        return ESP_OK;
    }
    esp_err_t err = recovery_io_set_lcd_pins(data, true);
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
    return spi_bus_add_device(LCD_SPI_HOST, &dev, &s_spi);
}

static esp_err_t panel_init(void)
{
    // Hardware reset through expander IO14 (~RESET), D/C parked high.
    esp_err_t err = recovery_io_set_lcd_pins(true, true);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    (void)recovery_io_set_lcd_pins(true, false);
    vTaskDelay(pdMS_TO_TICKS(20));
    err = recovery_io_set_lcd_pins(true, true);
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
static esp_err_t draw_line(int y, int scale, uint16_t fg, uint16_t bg, const char *text)
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

    (void)draw_line(Y_TITLE, TITLE_SCALE, COL_TITLE, COL_BG, "RECOVERY MODE");
    (void)draw_line(Y_SUB, TEXT_SCALE, COL_DIM, COL_BG, "KilnFW recovery image");

    if (recovery_io_nvs_failed_mask() != 0) {
        (void)draw_line(Y_BOOT, TEXT_SCALE, COL_FAULT, COL_FAULT_BG, "NVS UNAVAILABLE");
        buf[0] = 0;
    } else if (s_boot_count >= 0) {
        snprintf(buf, sizeof(buf), "Boot guard count: %d", s_boot_count);
    } else if (s_boot_record_present) {
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

    if (recovery_io_relay_fault()) {
        (void)draw_line(Y_RELAY, TEXT_SCALE, COL_FAULT, COL_FAULT_BG, "RELAY CTRL FAULT");
    } else {
        (void)draw_line(Y_RELAY, TEXT_SCALE, COL_OK, COL_BG, "Heat: OFF");
    }

    if (s_auth_fallback) {
        (void)draw_line(Y_AUTH, TEXT_SCALE, COL_FAULT, COL_FAULT_BG, "AUTH: FALLBACK");
    } else {
        (void)draw_line(Y_AUTH, TEXT_SCALE, COL_DIM, COL_BG, "");
    }

    if (s_net_none) {
        (void)draw_line(Y_NET, TEXT_SCALE, COL_FAULT, COL_FAULT_BG, "NO NETWORK");
        (void)draw_line(Y_IP, TEXT_SCALE, COL_DIM, COL_BG, "Wi-Fi bring-up failed");
        (void)draw_line(Y_HELP, TEXT_SCALE, COL_DIM, COL_BG, "Power-cycle or use JTAG");
    } else if (s_net_set) {
        snprintf(buf, sizeof(buf), "%s: %s", s_net_is_ap ? "AP" : "WiFi", s_net_name);
        (void)draw_line(Y_NET, TEXT_SCALE, COL_TEXT, COL_BG, buf);
        snprintf(buf, sizeof(buf), "IP: %s", s_net_ip);
        (void)draw_line(Y_IP, TEXT_SCALE, COL_TEXT, COL_BG, buf);
        snprintf(buf, sizeof(buf), "Open http://%s/ to upload", s_net_ip);
        (void)draw_line(Y_HELP, TEXT_SCALE, COL_TITLE, COL_BG, buf);
    } else {
        (void)draw_line(Y_NET, TEXT_SCALE, COL_DIM, COL_BG, "Network: starting...");
        (void)draw_line(Y_IP, TEXT_SCALE, COL_DIM, COL_BG, "");
        (void)draw_line(Y_HELP, TEXT_SCALE, COL_DIM, COL_BG, "");
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
    s_boot_record_present = false;
    // Only ESP_ERR_NVS_NOT_FOUND means "no record" (count 0); any other NVS
    // error leaves s_boot_count at -1, shown as "unreadable".
    if (nvs_flash_init_partition("kiln_nvs") == ESP_OK) {
        nvs_handle_t h;
        esp_err_t oerr = nvs_open_from_partition("kiln_nvs", "kiln_cfg", NVS_READONLY, &h);
        if (oerr == ESP_OK) {
            uint8_t rec[12];
            size_t len = sizeof(rec);
            esp_err_t gerr = nvs_get_blob(h, "bootguard", rec, &len);
            if (gerr == ESP_OK) {
                s_boot_record_present = true;
                uint32_t crc_stored = (uint32_t)rec[8] | ((uint32_t)rec[9] << 8) |
                                      ((uint32_t)rec[10] << 16) | ((uint32_t)rec[11] << 24);
                if (len == sizeof(rec) && rec[0] == 1 && crc_stored == esp_rom_crc32_le(0, rec, 8)) {
                    s_boot_count = (int)((uint32_t)rec[4] | ((uint32_t)rec[5] << 8) |
                                         ((uint32_t)rec[6] << 16) | ((uint32_t)rec[7] << 24));
                }
            } else if (gerr == ESP_ERR_NVS_NOT_FOUND) {
                s_boot_count = 0; // no record: a clean (or just-cleared) counter
            }
            nvs_close(h);
        } else if (oerr == ESP_ERR_NVS_NOT_FOUND) {
            s_boot_count = 0; // namespace never created
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
    if (spi_init() != ESP_OK) {
        ESP_LOGE(TAG, "panel SPI bus init failed");
    } else if (panel_init() != ESP_OK) {
        ESP_LOGE(TAG, "panel init sequence failed (expander down or SPI error)");
    } else if (clear_screen() != ESP_OK) {
        ESP_LOGE(TAG, "panel clear failed");
    } else {
        s_ready = true;
        draw_status();
        ESP_LOGI(TAG, "status screen drawn (boot_count=%d reset=%s crash=%d relay_fault=%d)",
                 s_boot_count, recovery_reset_reason_name(s_reset_reason), s_crash_state,
                 (int)recovery_io_relay_fault());
    }
    xSemaphoreGive(s_lock);
}

void recovery_lcd_set_network(bool is_ap, const char *name, const char *ip)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_net_is_ap = is_ap;
    snprintf(s_net_name, sizeof(s_net_name), "%s", name ? name : "");
    snprintf(s_net_ip, sizeof(s_net_ip), "%s", ip ? ip : "?");
    s_net_set = true;
    s_net_none = false;
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

void recovery_lcd_set_auth_fallback(bool fallback)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_auth_fallback = fallback;
    if (s_ready) {
        draw_status();
    }
    xSemaphoreGive(s_lock);
}
