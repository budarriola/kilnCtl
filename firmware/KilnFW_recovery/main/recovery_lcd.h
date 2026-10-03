// recovery_lcd.h -- static recovery-mode status screen on the 480x320 ST7796
// panel (docs/OTA_SINGLE_SLOT_PLAN.md section 3 item 4 / section 9.2).
//
// Independent, minimal driver -- NOT a port of firmware/KilnFW's
// App/drivers/hw/panel_*.c stack (touch, panel auto-detect, full SX1509
// library). Facts mirrored from the main app, each verified against source:
//   - Panel: ST7796 (MSP4031 module), native 320x480, driven landscape via
//     MADCTL 0x28 = MV|BGR (st7796_panel.c madctl[1]=0x20 for rotation 1, OR
//     the BGR bit 0x08 from .color_order_bit). The init table below is
//     st7796_panel.c's vendor sequence verbatim except MADCTL.
//   - COLMOD 0x05 (16 bpp), RGB565 sent big-endian (high byte first) --
//     panel_codec.c's byte-order fix (2026-09-04).
//   - SPI2: SCLK=GPIO12, MOSI=GPIO11, ~CS=GPIO21, 20 MHz, mode 0
//     (settings.h / Kconfig KILNCTL_SPI_SCLK_IO, MOSI_IO, DISPLAY_CS_IO).
//   - D/C = SX1509 IO15, ~RESET = SX1509 IO14 (settings.h SX1509_LCD_DC_PIN /
//     SX1509_LCD_RESET_PIN, docs/DISPLAY_ST7796_WIRING.md). The earlier
//     recovery driver had these swapped and used ILI9488 init/geometry.
//     Both pins are driven through recovery_io.c, which owns the expander.
//   - Backlight: ESP GPIO15 (KILNCTL_BACKLIGHT_GPIO), flying wire, active
//     high; plain GPIO high here (no PWM).
//
// Memory: one 960-byte DMA line buffer (480 px * 2 B), no framebuffer.
#ifndef RECOVERY_LCD_H
#define RECOVERY_LCD_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Brings up SPI + panel and draws the full status screen (title, boot_guard
// count, reset reason, crash/coredump presence, relay state, network line,
// upload instruction). Requires recovery_io_hold_relays_off() to have run
// (it owns I2C and the expander). Logs and returns on failure; a display
// fault must never stop Wi-Fi/HTTP/OTA from coming up.
void recovery_lcd_show_message(void);

// Updates the network line and the "Open http://<ip>/" instruction and
// redraws the screen. `name` is the station SSID or the AP's own SSID; `ip`
// is dotted-quad text. Safe to call from any task, before or after
// recovery_lcd_show_message() (it then only records the values), and with
// the panel down (no-op).
void recovery_lcd_set_network(bool is_ap, const char *name, const char *ip);

// Shows "NO NETWORK" in place of the network lines (every Wi-Fi bring-up path
// failed). A later recovery_lcd_set_network() clears it. Same safety rules.
void recovery_lcd_set_no_network(void);

// Shows/clears "AUTH: FALLBACK": the challenge key is derived from the
// eFuse-MAC fallback secret because wifi_nvs holds no usable ap_pass.
void recovery_lcd_set_auth_fallback(bool fallback);

#ifdef __cplusplus
}
#endif

#endif // RECOVERY_LCD_H
