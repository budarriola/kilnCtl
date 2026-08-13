#include "SSD1306.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "settings.h"

static const char *TAG = "SSD1306";

// SSD1306 control-byte prefixes (I2C).
#define SSD1306_CTRL_CMD  0x00
#define SSD1306_CTRL_DATA 0x40

// SSD1306 command opcodes used below.
#define SSD1306_CMD_DISPLAY_OFF          0xAE
#define SSD1306_CMD_DISPLAY_ON           0xAF
#define SSD1306_CMD_SET_CLOCK_DIV        0xD5
#define SSD1306_CMD_SET_MULTIPLEX        0xA8
#define SSD1306_CMD_SET_DISPLAY_OFFSET   0xD3
#define SSD1306_CMD_SET_START_LINE       0x40
#define SSD1306_CMD_CHARGE_PUMP          0x8D
#define SSD1306_CMD_MEMORY_MODE          0x20
#define SSD1306_CMD_SEGMENT_REMAP        0xA1
#define SSD1306_CMD_COM_SCAN_DEC         0xC8
#define SSD1306_CMD_SET_COM_PINS         0xDA
#define SSD1306_CMD_SET_CONTRAST         0x81
#define SSD1306_CMD_SET_PRECHARGE        0xD9
#define SSD1306_CMD_SET_VCOM_DETECT      0xDB
#define SSD1306_CMD_DISPLAY_RESUME       0xA4
#define SSD1306_CMD_NORMAL_DISPLAY       0xA6
#define SSD1306_CMD_INVERT_DISPLAY       0xA7
#define SSD1306_CMD_SET_COLUMN_ADDR      0x21
#define SSD1306_CMD_SET_PAGE_ADDR        0x22
#define SSD1306_CMD_DEACTIVATE_SCROLL    0x2E

// Standard ASCII 5x7 font, printable characters 0x20-0x7F (96 glyphs).
// Each glyph is 5 columns; each column byte is 7 vertically-stacked pixel
// bits (bit0 = top row of the glyph). This is the widely-reproduced
// "classic" fixed-space font shipped by Adafruit_GFX as glcdfont.c (BSD-style
// license), pulled verbatim from
// https://raw.githubusercontent.com/adafruit/Adafruit-GFX-Library/master/glcdfont.c
// (bytes 160..639, i.e. entries 32..127 of that 256-glyph table) rather than
// hand-derived, and spot-checked here against known-correct values for
// digits, letters, and common punctuation.
static const uint8_t font5x7[96][5] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00 }, // ' '
    { 0x00, 0x00, 0x5F, 0x00, 0x00 }, // '!'
    { 0x00, 0x07, 0x00, 0x07, 0x00 }, // '"'
    { 0x14, 0x7F, 0x14, 0x7F, 0x14 }, // '#'
    { 0x24, 0x2A, 0x7F, 0x2A, 0x12 }, // '$'
    { 0x23, 0x13, 0x08, 0x64, 0x62 }, // '%'
    { 0x36, 0x49, 0x56, 0x20, 0x50 }, // '&'
    { 0x00, 0x08, 0x07, 0x03, 0x00 }, // '\''
    { 0x00, 0x1C, 0x22, 0x41, 0x00 }, // '('
    { 0x00, 0x41, 0x22, 0x1C, 0x00 }, // ')'
    { 0x2A, 0x1C, 0x7F, 0x1C, 0x2A }, // '*'
    { 0x08, 0x08, 0x3E, 0x08, 0x08 }, // '+'
    { 0x00, 0x80, 0x70, 0x30, 0x00 }, // ','
    { 0x08, 0x08, 0x08, 0x08, 0x08 }, // '-'
    { 0x00, 0x00, 0x60, 0x60, 0x00 }, // '.'
    { 0x20, 0x10, 0x08, 0x04, 0x02 }, // '/'
    { 0x3E, 0x51, 0x49, 0x45, 0x3E }, // '0'
    { 0x00, 0x42, 0x7F, 0x40, 0x00 }, // '1'
    { 0x72, 0x49, 0x49, 0x49, 0x46 }, // '2'
    { 0x21, 0x41, 0x49, 0x4D, 0x33 }, // '3'
    { 0x18, 0x14, 0x12, 0x7F, 0x10 }, // '4'
    { 0x27, 0x45, 0x45, 0x45, 0x39 }, // '5'
    { 0x3C, 0x4A, 0x49, 0x49, 0x31 }, // '6'
    { 0x41, 0x21, 0x11, 0x09, 0x07 }, // '7'
    { 0x36, 0x49, 0x49, 0x49, 0x36 }, // '8'
    { 0x46, 0x49, 0x49, 0x29, 0x1E }, // '9'
    { 0x00, 0x00, 0x14, 0x00, 0x00 }, // ':'
    { 0x00, 0x40, 0x34, 0x00, 0x00 }, // ';'
    { 0x00, 0x08, 0x14, 0x22, 0x41 }, // '<'
    { 0x14, 0x14, 0x14, 0x14, 0x14 }, // '='
    { 0x00, 0x41, 0x22, 0x14, 0x08 }, // '>'
    { 0x02, 0x01, 0x59, 0x09, 0x06 }, // '?'
    { 0x3E, 0x41, 0x5D, 0x59, 0x4E }, // '@'
    { 0x7C, 0x12, 0x11, 0x12, 0x7C }, // 'A'
    { 0x7F, 0x49, 0x49, 0x49, 0x36 }, // 'B'
    { 0x3E, 0x41, 0x41, 0x41, 0x22 }, // 'C'
    { 0x7F, 0x41, 0x41, 0x41, 0x3E }, // 'D'
    { 0x7F, 0x49, 0x49, 0x49, 0x41 }, // 'E'
    { 0x7F, 0x09, 0x09, 0x09, 0x01 }, // 'F'
    { 0x3E, 0x41, 0x41, 0x51, 0x73 }, // 'G'
    { 0x7F, 0x08, 0x08, 0x08, 0x7F }, // 'H'
    { 0x00, 0x41, 0x7F, 0x41, 0x00 }, // 'I'
    { 0x20, 0x40, 0x41, 0x3F, 0x01 }, // 'J'
    { 0x7F, 0x08, 0x14, 0x22, 0x41 }, // 'K'
    { 0x7F, 0x40, 0x40, 0x40, 0x40 }, // 'L'
    { 0x7F, 0x02, 0x1C, 0x02, 0x7F }, // 'M'
    { 0x7F, 0x04, 0x08, 0x10, 0x7F }, // 'N'
    { 0x3E, 0x41, 0x41, 0x41, 0x3E }, // 'O'
    { 0x7F, 0x09, 0x09, 0x09, 0x06 }, // 'P'
    { 0x3E, 0x41, 0x51, 0x21, 0x5E }, // 'Q'
    { 0x7F, 0x09, 0x19, 0x29, 0x46 }, // 'R'
    { 0x26, 0x49, 0x49, 0x49, 0x32 }, // 'S'
    { 0x03, 0x01, 0x7F, 0x01, 0x03 }, // 'T'
    { 0x3F, 0x40, 0x40, 0x40, 0x3F }, // 'U'
    { 0x1F, 0x20, 0x40, 0x20, 0x1F }, // 'V'
    { 0x3F, 0x40, 0x38, 0x40, 0x3F }, // 'W'
    { 0x63, 0x14, 0x08, 0x14, 0x63 }, // 'X'
    { 0x03, 0x04, 0x78, 0x04, 0x03 }, // 'Y'
    { 0x61, 0x59, 0x49, 0x4D, 0x43 }, // 'Z'
    { 0x00, 0x7F, 0x41, 0x41, 0x41 }, // '['
    { 0x02, 0x04, 0x08, 0x10, 0x20 }, // '\\'
    { 0x00, 0x41, 0x41, 0x41, 0x7F }, // ']'
    { 0x04, 0x02, 0x01, 0x02, 0x04 }, // '^'
    { 0x40, 0x40, 0x40, 0x40, 0x40 }, // '_'
    { 0x00, 0x03, 0x07, 0x08, 0x00 }, // '`'
    { 0x20, 0x54, 0x54, 0x78, 0x40 }, // 'a'
    { 0x7F, 0x28, 0x44, 0x44, 0x38 }, // 'b'
    { 0x38, 0x44, 0x44, 0x44, 0x28 }, // 'c'
    { 0x38, 0x44, 0x44, 0x28, 0x7F }, // 'd'
    { 0x38, 0x54, 0x54, 0x54, 0x18 }, // 'e'
    { 0x00, 0x08, 0x7E, 0x09, 0x02 }, // 'f'
    { 0x18, 0xA4, 0xA4, 0x9C, 0x78 }, // 'g'
    { 0x7F, 0x08, 0x04, 0x04, 0x78 }, // 'h'
    { 0x00, 0x44, 0x7D, 0x40, 0x00 }, // 'i'
    { 0x20, 0x40, 0x40, 0x3D, 0x00 }, // 'j'
    { 0x7F, 0x10, 0x28, 0x44, 0x00 }, // 'k'
    { 0x00, 0x41, 0x7F, 0x40, 0x00 }, // 'l'
    { 0x7C, 0x04, 0x78, 0x04, 0x78 }, // 'm'
    { 0x7C, 0x08, 0x04, 0x04, 0x78 }, // 'n'
    { 0x38, 0x44, 0x44, 0x44, 0x38 }, // 'o'
    { 0xFC, 0x18, 0x24, 0x24, 0x18 }, // 'p'
    { 0x18, 0x24, 0x24, 0x18, 0xFC }, // 'q'
    { 0x7C, 0x08, 0x04, 0x04, 0x08 }, // 'r'
    { 0x48, 0x54, 0x54, 0x54, 0x24 }, // 's'
    { 0x04, 0x04, 0x3F, 0x44, 0x24 }, // 't'
    { 0x3C, 0x40, 0x40, 0x20, 0x7C }, // 'u'
    { 0x1C, 0x20, 0x40, 0x20, 0x1C }, // 'v'
    { 0x3C, 0x40, 0x30, 0x40, 0x3C }, // 'w'
    { 0x44, 0x28, 0x10, 0x28, 0x44 }, // 'x'
    { 0x4C, 0x90, 0x90, 0x90, 0x7C }, // 'y'
    { 0x44, 0x64, 0x54, 0x4C, 0x44 }, // 'z'
    { 0x00, 0x08, 0x36, 0x41, 0x00 }, // '{'
    { 0x00, 0x00, 0x77, 0x00, 0x00 }, // '|'
    { 0x00, 0x41, 0x36, 0x08, 0x00 }, // '}'
    { 0x02, 0x01, 0x02, 0x04, 0x02 }, // '~'
    { 0x3C, 0x26, 0x23, 0x26, 0x3C }, // 0x7F (DEL) -- unused in practice but kept for table completeness
};

/* Read-back on this panel is limited to the one status byte the controller
 * returns for an I2C read; the GDDRAM itself can only be read through the
 * parallel/SPI read strobe, which this two-wire hookup doesn't have. So a
 * framebuffer or command write can only be confirmed as far as the ACK, and
 * the single piece of state that IS verifiable is the display on/off bit.
 *
 * Status byte: D7 = BUSY, D6 = display OFF (1 = sleeping). Plenty of cheap
 * modules NACK the read or answer 0x00/0xFF, so a status read that fails or
 * comes back with one of those is treated as "no status available" rather
 * than as a failed write. */
#define SSD1306_STATUS_BUSY_BIT 0x80u
#define SSD1306_STATUS_OFF_BIT  0x40u

static esp_err_t ssd1306_transmit(SSD1306Class *oled, const uint8_t *buf, size_t len,
                                  uint32_t timeout_ms)
{
    return oled->owner_initialized
        ? i2c_owner_transfer(&oled->owner, oled->dev, buf, len, NULL, 0, timeout_ms)
        : i2c_master_transmit(oled->dev, buf, len, timeout_ms);
}

/* ESP_ERR_NOT_SUPPORTED = the panel answered, but with nothing usable. */
static esp_err_t ssd1306_read_status(SSD1306Class *oled, uint8_t *out_status)
{
    uint8_t status = 0;
    esp_err_t err = oled->owner_initialized
        ? i2c_owner_transfer(&oled->owner, oled->dev, NULL, 0, &status, 1, 500)
        : i2c_master_receive(oled->dev, &status, 1, 500);
    if (err != ESP_OK) return err;
    if (status == 0x00 || status == 0xFF) return ESP_ERR_NOT_SUPPORTED;
    *out_status = status;
    return ESP_OK;
}

static esp_err_t ssd1306_send_cmd_stream_once(SSD1306Class *oled, const uint8_t *cmds, size_t len)
{
    // Build a single I2C transaction: [0x00 control][cmd bytes...]
    uint8_t *buf = malloc(len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    buf[0] = SSD1306_CTRL_CMD;
    memcpy(&buf[1], cmds, len);

    esp_err_t err = ssd1306_transmit(oled, buf, len + 1, 1000);
    free(buf);
    return err;
}

/* Nothing here is read-back verifiable (see the comment above), so this is
 * retry-on-transport-error only, reported the same way as the verified writes
 * in the other I2C drivers. */
static esp_err_t ssd1306_write_cmd_stream(SSD1306Class *oled, const uint8_t *cmds, size_t len)
{
    esp_err_t err = ESP_FAIL;
    for (unsigned attempt = 1; attempt <= I2C_WRITE_RETRY_ATTEMPTS; ++attempt) {
        err = ssd1306_send_cmd_stream_once(oled, cmds, len);
        if (err == ESP_OK) return ESP_OK;
        if (err == ESP_ERR_NO_MEM) return err;  // retrying won't conjure heap
        ESP_LOGW(TAG, "command 0x%02X (%u bytes) failed (attempt %u/%u): %s", cmds[0],
                 (unsigned)len, attempt, (unsigned)I2C_WRITE_RETRY_ATTEMPTS,
                 esp_err_to_name(err));
    }
    ESP_LOGE(TAG, "command 0x%02X failed after %u attempts: %s", cmds[0],
             (unsigned)I2C_WRITE_RETRY_ATTEMPTS, esp_err_to_name(err));
    return err;
}

static esp_err_t ssd1306_write_cmd1(SSD1306Class *oled, uint8_t cmd)
{
    return ssd1306_write_cmd_stream(oled, &cmd, 1);
}

static esp_err_t ssd1306_write_cmd2(SSD1306Class *oled, uint8_t cmd, uint8_t arg)
{
    uint8_t buf[2] = { cmd, arg };
    return ssd1306_write_cmd_stream(oled, buf, sizeof(buf));
}

esp_err_t SSD1306_init(SSD1306Class *oled, i2c_master_bus_handle_t bus, uint8_t addr, uint8_t width, uint8_t height)
{
    if (!oled || !bus || width == 0 || height == 0 || (height % 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(oled, 0, sizeof(*oled));
    oled->addr = addr;
    oled->width = width;
    oled->height = height;
    oled->pages = height / 8;

    oled->framebuffer = calloc((size_t)width * oled->pages, 1);
    if (!oled->framebuffer) {
        return ESP_ERR_NO_MEM;
    }

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_config, &oled->dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        free(oled->framebuffer);
        oled->framebuffer = NULL;
        return err;
    }

    // Independent i2c_owner on the same (already-existing) bus handle -- this
    // driver never creates or destroys the bus itself. Transactions still go
    // through the per-device handle, so a second owner task here is safe and
    // gives the OLED its own FIFO-ordered queue, decoupled from any other
    // device's traffic on the same physical bus.
    err = i2c_owner_init(&oled->owner, bus, 8, 5, 4096, tskNO_AFFINITY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_owner_init failed: %s", esp_err_to_name(err));
        i2c_master_bus_rm_device(oled->dev);
        oled->dev = NULL;
        free(oled->framebuffer);
        oled->framebuffer = NULL;
        return err;
    }
    oled->owner_initialized = true;

    uint8_t com_pins = (height == 32) ? 0x02 : 0x12;

    uint8_t init_cmds[] = {
        SSD1306_CMD_DISPLAY_OFF,
        SSD1306_CMD_SET_CLOCK_DIV, 0x80,
        SSD1306_CMD_SET_MULTIPLEX, (uint8_t)(height - 1),
        SSD1306_CMD_SET_DISPLAY_OFFSET, 0x00,
        (uint8_t)(SSD1306_CMD_SET_START_LINE | 0x00),
        SSD1306_CMD_CHARGE_PUMP, 0x14,
        SSD1306_CMD_MEMORY_MODE, 0x00,
        SSD1306_CMD_SEGMENT_REMAP,
        SSD1306_CMD_COM_SCAN_DEC,
        SSD1306_CMD_SET_COM_PINS, com_pins,
        SSD1306_CMD_SET_CONTRAST, 0xCF,
        SSD1306_CMD_SET_PRECHARGE, 0xF1,
        SSD1306_CMD_SET_VCOM_DETECT, 0x40,
        SSD1306_CMD_DISPLAY_RESUME,
        SSD1306_CMD_NORMAL_DISPLAY,
        /* Hardware scrolling is a persistent controller mode: it survives a
         * CPU-only reset (the panel keeps its own state unless power is
         * actually removed), so a re-init after e.g. a watchdog reboot could
         * otherwise come up with a leftover scroll still running and the
         * framebuffer sliding across the screen. Adafruit_SSD1306's begin()
         * sends this for the same reason, immediately before DISPLAY_ON;
         * this driver was missing it. Note this is a correctness fix, not a
         * brightness one -- every contrast/charge-pump/precharge value above
         * already matches that library exactly. */
        SSD1306_CMD_DEACTIVATE_SCROLL,
        SSD1306_CMD_DISPLAY_ON,
    };
    err = ssd1306_write_cmd_stream(oled, init_cmds, sizeof(init_cmds));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SSD1306 init command stream failed: %s", esp_err_to_name(err));
        i2c_owner_deinit(&oled->owner);
        oled->owner_initialized = false;
        i2c_master_bus_rm_device(oled->dev);
        oled->dev = NULL;
        free(oled->framebuffer);
        oled->framebuffer = NULL;
        return err;
    }

    ESP_LOGI(TAG, "SSD1306 initialized addr=0x%02X %ux%u", addr, width, height);
    return ESP_OK;
}

esp_err_t SSD1306_deinit(SSD1306Class *oled)
{
    if (!oled) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ESP_OK;
    if (oled->owner_initialized) {
        esp_err_t e = i2c_owner_deinit(&oled->owner);
        if (e != ESP_OK) err = e;
        oled->owner_initialized = false;
    }
    if (oled->dev) {
        esp_err_t e = i2c_master_bus_rm_device(oled->dev);
        if (e != ESP_OK) err = e;
        oled->dev = NULL;
    }
    // Note: the I2C bus itself belongs to whoever created it (e.g. DcDac);
    // this driver only attached a device to it and must not delete it.
    free(oled->framebuffer);
    oled->framebuffer = NULL;
    return err;
}

esp_err_t SSD1306_start(SSD1306Class *oled, i2c_master_bus_handle_t bus)
{
    esp_err_t err = SSD1306_init(oled, bus, SSD1306_I2C_ADDR, SSD1306_WIDTH, SSD1306_HEIGHT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SSD1306_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "SSD1306 initialized");
    SSD1306_clear(oled);
    SSD1306_set_cursor(oled, 0, 0);
    SSD1306_printf(oled, "kilnCtl ready");
    SSD1306_display(oled);
    return ESP_OK;
}

esp_err_t SSD1306_clear(SSD1306Class *oled)
{
    if (!oled || !oled->framebuffer) return ESP_ERR_INVALID_ARG;
    memset(oled->framebuffer, 0, (size_t)oled->width * oled->pages);
    oled->cursor_col = 0;
    oled->cursor_row = 0;
    return ESP_OK;
}

esp_err_t SSD1306_display(SSD1306Class *oled)
{
    if (!oled || !oled->framebuffer) return ESP_ERR_INVALID_ARG;

    uint8_t col_range[3] = { SSD1306_CMD_SET_COLUMN_ADDR, 0, (uint8_t)(oled->width - 1) };
    esp_err_t err = ssd1306_write_cmd_stream(oled, col_range, sizeof(col_range));
    if (err != ESP_OK) return err;

    uint8_t page_range[3] = { SSD1306_CMD_SET_PAGE_ADDR, 0, (uint8_t)(oled->pages - 1) };
    err = ssd1306_write_cmd_stream(oled, page_range, sizeof(page_range));
    if (err != ESP_OK) return err;

    size_t fb_len = (size_t)oled->width * oled->pages;
    uint8_t *buf = malloc(fb_len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    buf[0] = SSD1306_CTRL_DATA;
    memcpy(&buf[1], oled->framebuffer, fb_len);

    /* GDDRAM can't be read back over I2C, so this is retry-on-error only. */
    for (unsigned attempt = 1; attempt <= I2C_WRITE_RETRY_ATTEMPTS; ++attempt) {
        err = ssd1306_transmit(oled, buf, fb_len + 1, 2000);
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "framebuffer write failed (attempt %u/%u): %s", attempt,
                 (unsigned)I2C_WRITE_RETRY_ATTEMPTS, esp_err_to_name(err));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "framebuffer write failed after %u attempts: %s",
                 (unsigned)I2C_WRITE_RETRY_ATTEMPTS, esp_err_to_name(err));
    }
    free(buf);
    return err;
}

esp_err_t SSD1306_set_cursor(SSD1306Class *oled, uint8_t col, uint8_t row)
{
    if (!oled) return ESP_ERR_INVALID_ARG;
    uint8_t max_col = (uint8_t)(oled->width / SSD1306_FONT_CELL_WIDTH);
    uint8_t max_row = (uint8_t)(oled->pages);
    if (col >= max_col || row >= max_row) return ESP_ERR_INVALID_ARG;
    oled->cursor_col = (uint8_t)(col * SSD1306_FONT_CELL_WIDTH);
    oled->cursor_row = row;
    return ESP_OK;
}

esp_err_t SSD1306_set_contrast(SSD1306Class *oled, uint8_t contrast)
{
    if (!oled) return ESP_ERR_INVALID_ARG;
    return ssd1306_write_cmd2(oled, SSD1306_CMD_SET_CONTRAST, contrast);
}

esp_err_t SSD1306_set_invert(SSD1306Class *oled, bool invert)
{
    if (!oled) return ESP_ERR_INVALID_ARG;
    return ssd1306_write_cmd1(oled, invert ? SSD1306_CMD_INVERT_DISPLAY : SSD1306_CMD_NORMAL_DISPLAY);
}

esp_err_t SSD1306_set_power(SSD1306Class *oled, bool on)
{
    if (!oled) return ESP_ERR_INVALID_ARG;

    /* The one SSD1306 write that can actually be confirmed: the status byte
     * carries the display on/off state back. */
    const uint8_t cmd = on ? SSD1306_CMD_DISPLAY_ON : SSD1306_CMD_DISPLAY_OFF;
    esp_err_t err = ESP_FAIL;
    for (unsigned attempt = 1; attempt <= I2C_WRITE_RETRY_ATTEMPTS; ++attempt) {
        err = ssd1306_send_cmd_stream_once(oled, &cmd, 1);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "display %s failed (attempt %u/%u): %s", on ? "on" : "off", attempt,
                     (unsigned)I2C_WRITE_RETRY_ATTEMPTS, esp_err_to_name(err));
            continue;
        }

        uint8_t status = 0;
        esp_err_t status_err = ssd1306_read_status(oled, &status);
        if (status_err != ESP_OK) {
            /* Panel won't report status -- the ACK is all there is. Not an
             * error, and not worth retrying a write that already succeeded. */
            ESP_LOGD(TAG, "display %s: no readable status (%s), accepting the ACK",
                     on ? "on" : "off", esp_err_to_name(status_err));
            return ESP_OK;
        }
        if (((status & SSD1306_STATUS_OFF_BIT) == 0) == on) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "display %s: status 0x%02X still says %s (attempt %u/%u)",
                 on ? "on" : "off", status, on ? "off" : "on", attempt,
                 (unsigned)I2C_WRITE_RETRY_ATTEMPTS);
        err = ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGE(TAG, "display %s failed after %u attempts: %s", on ? "on" : "off",
             (unsigned)I2C_WRITE_RETRY_ATTEMPTS, esp_err_to_name(err));
    return err;
}

// Draws one glyph column-byte into the page-addressed framebuffer at pixel
// (x, page*8). The 5x7 font only uses bits 0..6 of each column byte (7 rows);
// bit 7 is always 0, giving the blank row below each glyph for free.
static void ssd1306_draw_glyph(SSD1306Class *oled, uint8_t x, uint8_t page, unsigned char c)
{
    const uint8_t *glyph = font5x7[(uint8_t)c - 0x20];
    for (int col = 0; col < SSD1306_FONT_GLYPH_WIDTH; ++col) {
        uint8_t px = (uint8_t)(x + col);
        if (px >= oled->width) break;
        oled->framebuffer[(size_t)page * oled->width + px] = glyph[col];
    }
    // 1px blank gap column after the glyph.
    uint8_t gap_x = (uint8_t)(x + SSD1306_FONT_GLYPH_WIDTH);
    if (gap_x < oled->width) {
        oled->framebuffer[(size_t)page * oled->width + gap_x] = 0x00;
    }
}

esp_err_t SSD1306_write_char(SSD1306Class *oled, char c)
{
    if (!oled || !oled->framebuffer) return ESP_ERR_INVALID_ARG;

    if (c == '\n') {
        oled->cursor_col = 0;
        oled->cursor_row = (uint8_t)((oled->cursor_row + 1) % oled->pages);
        return ESP_OK;
    }
    if (c == '\r') {
        oled->cursor_col = 0;
        return ESP_OK;
    }

    unsigned char uc = (unsigned char)c;
    if (uc < 0x20 || uc > 0x7F) {
        uc = '?';
    }

    if ((uint32_t)oled->cursor_col + SSD1306_FONT_CELL_WIDTH > oled->width) {
        oled->cursor_col = 0;
        oled->cursor_row = (uint8_t)((oled->cursor_row + 1) % oled->pages);
    }

    ssd1306_draw_glyph(oled, oled->cursor_col, oled->cursor_row, uc);
    oled->cursor_col = (uint8_t)(oled->cursor_col + SSD1306_FONT_CELL_WIDTH);
    return ESP_OK;
}

esp_err_t SSD1306_write_str(SSD1306Class *oled, const char *str)
{
    if (!oled || !str) return ESP_ERR_INVALID_ARG;
    for (const char *p = str; *p; ++p) {
        esp_err_t err = SSD1306_write_char(oled, *p);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

esp_err_t SSD1306_vprintf(SSD1306Class *oled, const char *fmt, va_list args)
{
    if (!oled || !fmt) return ESP_ERR_INVALID_ARG;

    va_list args_copy;
    va_copy(args_copy, args);
    int needed = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);
    if (needed < 0) return ESP_FAIL;

    char *buf = malloc((size_t)needed + 1);
    if (!buf) return ESP_ERR_NO_MEM;

    vsnprintf(buf, (size_t)needed + 1, fmt, args);
    esp_err_t err = SSD1306_write_str(oled, buf);
    free(buf);
    return err;
}

esp_err_t SSD1306_printf(SSD1306Class *oled, const char *fmt, ...)
{
    if (!oled || !fmt) return ESP_ERR_INVALID_ARG;
    va_list args;
    va_start(args, fmt);
    esp_err_t err = SSD1306_vprintf(oled, fmt, args);
    va_end(args);
    return err;
}
