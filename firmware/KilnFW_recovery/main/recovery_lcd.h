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
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Read-only panel health for GET /api/recovery/status. The passphrase is
// shown only on the LCD, so a panel that is not ready means nobody can see it
// (but ready does not mean it is visible, see below).
typedef struct {
    // "Driver path OK": init and the last full draw returned ESP_OK for every
    // SPI/expander transaction. The panel is write-only (miso_io_num = -1), so
    // a dead or disconnected panel is undetectable: this does NOT prove the
    // passphrase is visible.
    bool ready;
    uint32_t init_attempts;         // cumulative init attempts (boot + retries)
    uint32_t draw_failures;         // draw passes that dropped at least one line
    uint32_t task_stack_free_bytes; // lcd_retry task stack high-water mark (0 if not running)
} recovery_lcd_status_t;
void recovery_lcd_get_status(recovery_lcd_status_t *out);

// Brings up SPI + panel and draws the full status screen (title, boot_guard
// count, reset reason, crash/coredump presence, relay state, SoftAP SSID and
// passphrase, upload instruction). Requires recovery_io_hold_relays_off() to have run
// (it owns I2C and the expander). Logs and returns on failure; a display
// fault must never stop Wi-Fi/HTTP/OTA from coming up.
void recovery_lcd_show_message(void);

// Shows the SoftAP's SSID, this boot's random WPA2 passphrase (large) and the
// AP IP, plus the "Open http://<ip>/" instruction, and redraws the screen. The
// passphrase is the recovery image's only access control (owner decision
// 2026-10-02): it is shown here and nowhere else, kept in RAM only. Safe to
// call from any task, before or after recovery_lcd_show_message() (it then
// only records the values), and with the panel down (no-op).
void recovery_lcd_set_ap(const char *ssid, const char *passphrase, const char *ip);

// Shows "NO NETWORK" in place of the network lines (every Wi-Fi bring-up path
// failed). A later recovery_lcd_set_ap() clears it. Same safety rules.
void recovery_lcd_set_no_network(void);

// Shows "WIFI STORAGE FAIL" in place of the network lines: the Wi-Fi driver
// could not be kept RAM-only, so no SoftAP (and no passphrase) exists. Same
// safety rules as recovery_lcd_set_no_network().
void recovery_lcd_set_wifi_storage_fail(void);

// Shows a fatal error banner (e.g. "HTTP FAILED") plus a short detail line in
// place of every network line (the passphrase is not drawn while it is shown;
// the RAM copy is kept so recovery_lcd_clear_error() can restore it). Use only
// for a FINAL failure. Same safety rules.
void recovery_lcd_set_error(const char *headline, const char *detail);

// Removes the banner set by recovery_lcd_set_error() and redraws.
void recovery_lcd_clear_error(void);

// SoftAP state for the LCD: RLCD_AP_UP, RLCD_AP_RESTARTING (shows "AP DOWN /
// restarting the SoftAP") or RLCD_AP_FAILED ("AP DOWN / AP could not restart").
// While not up the LCD hides the SSID/passphrase/IP of a dead AP. Safe from any task.
#define RLCD_AP_UP         0
#define RLCD_AP_RESTARTING 1
#define RLCD_AP_FAILED     2
void recovery_lcd_set_ap_state(int state);

// True once the panel was brought up and the status screen drawn. Takes the LCD
// lock, so it is a snapshot (briefly false while the retry task re-inits).
bool recovery_lcd_is_ok(void);

// Redraws the status screen if the relay-fault state changed since the last
// draw (a hold-watchdog fault latched after boot). Call from a task that may
// block on the LCD lock; never from the relay-hold task.
void recovery_lcd_poll_relay_fault(void);

#ifdef __cplusplus
}
#endif

#endif // RECOVERY_LCD_H
