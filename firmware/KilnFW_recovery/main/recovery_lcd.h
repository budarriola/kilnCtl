// recovery_lcd.h -- the one static line of recovery-mode text on the panel
// (docs/OTA_SINGLE_SLOT_PLAN.md section 3 item 4 / section 9.2, owner-decided
// against the plan's own recommendation of a dark panel).
//
// SCOPE OF THIS PASS. This is a deliberately independent, minimal driver --
// NOT a port of firmware/KilnFW's App/drivers/hw/panel_spi*.c family, which
// pulls in touch (FT6336U/NS2009), panel auto-detection and the full
// SX1509 register library (~1,050 lines) to serve a UI far beyond one
// static line. Pulling that whole stack into the one image whose entire
// value is having almost nothing that can fail would be exactly backwards
// -- see section 3's bar ("anything beyond receive-an-image-and-write-it
// must justify itself"). What IS reused, because getting it wrong risks
// nothing running at all: the real, hardware-confirmed pin facts from
// App/drivers/hw/settings.h and the SX1509 register map from
// App/drivers/hw/SX1509.h --
//   - Shared SPI bus: SCLK=GPIO12, MOSI=GPIO11 (MISO unused for a
//     write-only panel), panel ~CS=GPIO21, all Kconfig-default values on
//     this board (KILNCTL_SPI_SCLK_IO/MOSI_IO, KILNCTL_DISPLAY_CS_IO).
//   - Panel DC and RESET are NOT raw ESP GPIOs -- they are bits 6 and 7 of
//     the SX1509 I/O expander's bank B (pins 14/15, SX1509_LCD_DC_PIN/
//     SX1509_LCD_RESET_PIN in settings.h), reached over I2C
//     (SDA=GPIO8, SCL=GPIO9, address 0x3E) via three register writes
//     (RegDirB=0x0E, RegDataB=0x10) -- there is no need for the full
//     SX1509 driver's LED/interrupt/pull-up machinery just to hold two
//     pins as outputs.
// No touch, no LVGL, no panel auto-detection: exactly one init sequence,
// one fill, one small bitmap-font string blit. Never bench-verified on real
// hardware as of this pass -- see the module's own header note, same
// discipline as ST7796_get_panel_desc()'s STOP-block disclaimer.
#ifndef RECOVERY_LCD_H
#define RECOVERY_LCD_H

#ifdef __cplusplus
extern "C" {
#endif

// Brings up the shared SPI bus, the SX1509's two GPIO lines, resets and
// initializes the panel, clears it, and draws the fixed recovery-mode
// message. Logs and returns on any failure rather than aborting -- a
// display fault must never prevent Wi-Fi/HTTP/OTA from coming up, since
// those are the actual job (section 3: "anything beyond receive-an-image-
// and-write-it must justify itself").
void recovery_lcd_show_message(void);

#ifdef __cplusplus
}
#endif

#endif // RECOVERY_LCD_H
