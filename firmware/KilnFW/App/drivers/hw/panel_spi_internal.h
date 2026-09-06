// Move-only split of panel_spi.c (ROADMAP.md M15 1500-line rule). Shared
// internals between panel_spi.c (bus/device setup, transport primitives,
// bounds/blit-state guards), panel_spi_bringup.c (init-table execution,
// rotation/MADCTL, the ILI9488_init/start/deinit/reset entry points),
// panel_spi_draw.c (fill/rect/line/text) and panel_spi_blit.c (streaming
// blit + read-back). Every symbol here used to be `static` in the single
// file; each was widened to file-scope-internal (extern, panel_spi_-
// prefixed) ONLY because a caller now lives in a different translation
// unit. No behavior changed -- see each .c file's own comments for the
// datasheet/timing/byte-order reasoning that must not be disturbed.
#ifndef PANEL_SPI_INTERNAL_H
#define PANEL_SPI_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "panel_codec.h"
#include "panel_spi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Shared log tag. The string itself is deliberately left as "ILI9488" (it
 * prints for the ST7796 too) -- that is a pre-existing quirk this move-only
 * split does not touch, only the C identifier is renamed. */
extern const char *PANEL_SPI_TAG;

/* --- Command opcodes (datasheet §5.1 Command List) --- */
#define ILI9488_CMD_NOP        0x00
#define ILI9488_CMD_SWRESET    0x01
#define ILI9488_CMD_RDDID      0x04  /* 24-bit read: manufacturer/version/driver ID */
#define ILI9488_CMD_SLPIN      0x10
#define ILI9488_CMD_SLPOUT     0x11
#define ILI9488_CMD_INVOFF     0x20
#define ILI9488_CMD_INVON      0x21
#define ILI9488_CMD_DISPOFF    0x28
#define ILI9488_CMD_DISPON     0x29
#define ILI9488_CMD_CASET      0x2A  /* column (x) address window */
#define ILI9488_CMD_PASET      0x2B  /* page (y) address window */
#define ILI9488_CMD_RAMWR      0x2C  /* start writing pixels at the window origin */
#define ILI9488_CMD_MADCTL     0x36  /* memory access control -- rotation/mirror/BGR */
#define ILI9488_CMD_COLMOD     0x3A  /* interface pixel format */
#define ILI9488_CMD_IFMODE     0xB0
#define ILI9488_CMD_FRMCTR1    0xB1
#define ILI9488_CMD_INVTR      0xB4
#define ILI9488_CMD_DFC        0xB6
#define ILI9488_CMD_ENTRY_MODE 0xB7
#define ILI9488_CMD_PWCTRL1    0xC0
#define ILI9488_CMD_PWCTRL2    0xC1
#define ILI9488_CMD_VMCTRL     0xC5
#define ILI9488_CMD_PGAMCTRL   0xE0
#define ILI9488_CMD_NGAMCTRL   0xE1
#define ILI9488_CMD_ADJCTL3    0xF7

/* COLMOD parameter. Bits [6:4] = DPI (RGB interface), [2:0] = DBI (MCU
 * interface). 0x66 = 110/110 = 18 bits/pixel on both -- see the RGB666 note
 * at the top of panel_spi.c for why 0x55 (16bpp) is not an option here. */
#define ILI9488_COLMOD_RGB666 0x66

/* MADCTL bits (§5.2.30). D7 MY row order, D6 MX column order, D5 MV
 * row/column exchange, D4 ML vertical refresh order, D3 BGR colour filter
 * order, D2 MH horizontal refresh order. */
#define ILI9488_MADCTL_MY  0x80
#define ILI9488_MADCTL_MX  0x40
#define ILI9488_MADCTL_MV  0x20
#define ILI9488_MADCTL_ML  0x10
#define ILI9488_MADCTL_BGR 0x08
#define ILI9488_MADCTL_MH  0x04

/* This module's colour filter order. ASSUMPTION: BGR, which is what
 * essentially every ILI9488 breakout uses and what the BIGTREETECH module is
 * assumed to be -- the datasheet cannot tell us, because the filter order is
 * a property of the glass, not the controller. If red and blue come out
 * swapped on the bench, clear this bit; nothing else needs to change. */
#define ILI9488_MADCTL_COLOR_ORDER ILI9488_MADCTL_BGR

/* Datasheet timing (§13.4 Reset Timing, and the SLPIN/SLPOUT restrictions in
 * §5.2.x): reset pulse min 10us, reset cancel 120ms; 5ms minimum between a
 * software reset and the next command, 120ms if it happened out of sleep.
 * These are rounded up generously -- they run once at boot and once per
 * explicit reset, so there is nothing to be gained by shaving them. */
#define ILI9488_RESET_PULSE_MS  20
#define ILI9488_RESET_WAIT_MS   150
#define ILI9488_SLEEP_WAIT_MS   130

/* --- transport primitives (defined in panel_spi.c) --- */
bool panel_spi_ready(const ILI9488Class *disp);
size_t panel_spi_chunk(const ILI9488Class *disp);
size_t panel_spi_chunk_pixels(const ILI9488Class *disp);
bool panel_spi_lock(ILI9488Class *disp);
void panel_spi_unlock(ILI9488Class *disp);
esp_err_t panel_spi_set_dc(ILI9488Class *disp, bool data);
esp_err_t panel_spi_tx(ILI9488Class *disp, const uint8_t *buf, size_t len);
esp_err_t panel_spi_write_cmd(ILI9488Class *disp, uint8_t cmd, const uint8_t *params, size_t len);
esp_err_t panel_spi_begin_ram_write(ILI9488Class *disp, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
esp_err_t panel_spi_push_color_run(ILI9488Class *disp, uint16_t color, uint32_t pixels);

/* --- bounds and blit-state guards (defined in panel_spi.c) --- */
bool panel_spi_rect_in_bounds(const ILI9488Class *disp, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void panel_spi_blit_clear_state(ILI9488Class *disp);
esp_err_t panel_spi_reject_if_blitting(ILI9488Class *disp);

/* --- font, used only by panel_spi_draw.c's glyph renderer, defined in
 * panel_spi.c alongside the other .rodata tables --- */
extern const uint8_t panel_spi_font5x7[96][5];

/* The one place RGB565 becomes RGB666.
 *
 * The panel wants three bytes per pixel, each carrying six significant bits
 * in its MSBs (§4.7.2.2, Figure 107: R5..R0 followed by two void bits). What
 * is written here is the full 8-bit expansion of each channel -- bit
 * replication, so 5-bit 0x1F becomes 0xFF rather than 0xF8, keeping white
 * actually white -- and the controller simply ignores the two low bits it
 * has no room for. That makes this both an RGB565->RGB888 widening and the
 * correct MSB-aligned RGB666 encoding at the same time.
 *
 * Green already has six bits and needs no rounding at all; red and blue lose
 * nothing either, since 5 bits genuinely carry less information than the 6
 * the panel can show. RGB565 is the API's limit here, not the panel's. */
/* Phase 3: the panel-selected encoder. bytes_per_pixel == 2 (ST7796, COLMOD
 * 0x55) is panel_codec_rgb565_passthrough() -- the wire already IS the wire
 * format, no widening -- anything else (3, ILI9488's RGB666) is the widening
 * above. Writes exactly disp->panel->bytes_per_pixel bytes into `out`; the
 * two callers below both keep out[] sized for the larger (3-byte) case so
 * one buffer serves either panel.
 *
 * Kept `static inline` in this shared header, exactly as it was `static
 * inline` in the single file -- each including .c file gets its own
 * internal-linkage copy, so there is no link-time widening or collision to
 * audit here, unlike the extern declarations above. */
static inline void panel_spi_encode_pixel(const ILI9488Class *disp, uint16_t color, uint8_t *out)
{
    if (disp->panel->bytes_per_pixel == 2) {
        panel_codec_rgb565_passthrough(color, out);
    } else {
        panel_codec_rgb565_to_rgb666(color, out);
    }
}

#ifdef __cplusplus
}
#endif

#endif // PANEL_SPI_INTERNAL_H
