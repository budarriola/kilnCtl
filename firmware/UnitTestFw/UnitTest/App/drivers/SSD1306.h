// SSD1306 I2C monochrome OLED display driver, with printf-style text support.
//
// Unlike DcDac, this driver does NOT create its own I2C bus: the ESP32's I2C0
// peripheral is already claimed elsewhere (see DcDac_init / mcp4728_i2c_task),
// and ESP-IDF only allows one i2c_master_bus_handle_t per physical bus. The
// caller must pass in an already-created bus handle; SSD1306_init just attaches
// this device to it via i2c_master_bus_add_device() and creates its own
// i2c_owner_t wrapping that same bus handle for FIFO-ordered transfers,
// independent of any other device's queue on the same bus.
#ifndef SSD1306_H
#define SSD1306_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "i2c_owner.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SSD1306_DEFAULT_I2C_ADDR 0x3C  // common default; 0x3D on some boards

// Font cell geometry: 5x7 glyph bitmap plus 1px inter-glyph gap = 6px wide,
// rendered into an 8px-tall row (7 glyph rows + 1px blank row below).
#define SSD1306_FONT_GLYPH_WIDTH  5
#define SSD1306_FONT_GLYPH_HEIGHT 7
#define SSD1306_FONT_CELL_WIDTH   6
#define SSD1306_FONT_CELL_HEIGHT  8

typedef struct {
    i2c_master_dev_handle_t dev;
    i2c_owner_t owner;
    bool owner_initialized;
    uint8_t addr;
    uint8_t width;              // pixels, e.g. 128
    uint8_t height;             // pixels, e.g. 64 (must be a multiple of 8)
    uint8_t pages;              // height / 8
    uint8_t *framebuffer;       // heap-allocated, width*pages bytes, SSD1306 page-addressed layout (each byte = 8 vertically-stacked pixels, LSB = top)
    uint8_t cursor_col;         // pixel column of next glyph
    uint8_t cursor_row;         // text row (0..pages/8-1 if using an 8px-tall font -- your call on font height, see below)
} SSD1306Class;

// Attaches the OLED as a second device on an already-created I2C bus (see
// header comment above) and brings the panel up (init sequence + cleared,
// displayed framebuffer left blank -- caller still needs SSD1306_display()
// after drawing). height must be a multiple of 8 (e.g. 32 or 64).
esp_err_t SSD1306_init(SSD1306Class *oled, i2c_master_bus_handle_t bus, uint8_t addr, uint8_t width, uint8_t height);
esp_err_t SSD1306_deinit(SSD1306Class *oled);

/* Single-call bootstrap: SSD1306_init (using the Kconfig-configured
 * address/width/height) plus a one-line boot message so there's visible
 * output before anything connects over UART. Logs and returns the init
 * error rather than asserting -- the OLED isn't critical enough to abort
 * app_main over. */
esp_err_t SSD1306_start(SSD1306Class *oled, i2c_master_bus_handle_t bus);

esp_err_t SSD1306_clear(SSD1306Class *oled);                 // clears the in-RAM framebuffer only
esp_err_t SSD1306_display(SSD1306Class *oled);                // flushes the whole framebuffer to the panel over I2C
esp_err_t SSD1306_set_cursor(SSD1306Class *oled, uint8_t col, uint8_t row); // text-cell coordinates
esp_err_t SSD1306_set_contrast(SSD1306Class *oled, uint8_t contrast);
esp_err_t SSD1306_set_invert(SSD1306Class *oled, bool invert);
esp_err_t SSD1306_set_power(SSD1306Class *oled, bool on);      // display on/off (sleep)

// Text rendering into the framebuffer at the cursor (does not touch the
// panel until SSD1306_display() is called). Auto-wraps at the right edge to
// the next text row; wraps from the bottom row back to row 0 (no scrolling).
esp_err_t SSD1306_write_char(SSD1306Class *oled, char c);
esp_err_t SSD1306_write_str(SSD1306Class *oled, const char *str);

// The actual ask: printf-style text. Renders into the framebuffer (does NOT
// auto-call SSD1306_display -- caller controls when it hits the wire, same
// as write_str). variadic + va_list forms.
esp_err_t SSD1306_printf(SSD1306Class *oled, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
esp_err_t SSD1306_vprintf(SSD1306Class *oled, const char *fmt, va_list args);

#ifdef __cplusplus
}
#endif

#endif // SSD1306_H
