// Datasheet: firmware/KilnFW/Datasheets/ILI9488.pdf (343 pages), with
// firmware/KilnFW/docs/ILI9488.md as the local commentary. NOT under
// hardware/datasheets/ -- see NS2009.c's header for why that matters.
// Section citations in the comments below were verified against this
// document on 2026-08-24 (reset timing 13.4 Table 39 p308, SWRESET 5.2.2
// p150, SLPIN/SLPOUT 5.2.12/13 p165-166, COLMOD 5.2.34 p200, MADCTL
// 5.2.30 p192, SPI clock limits 17.4.3 p332).
//
// ROADMAP.md M15 1500-line rule: this file used to hold the whole ILI9488/
// ST7796 SPI panel driver (2206 lines). It was split move-only into:
//   panel_spi.c          -- this file: bus/device setup, the low-level
//                            transport (D/C batching, chunked SPI writes,
//                            RGB565->wire pixel encode) and the bounds /
//                            blit-state guards every higher-level call goes
//                            through first.
//   panel_spi_bringup.c  -- the init-table execution, rotation/MADCTL, and
//                            the ILI9488_init/start/deinit/reset entry
//                            points, plus the panel_desc_t descriptor.
//   panel_spi_draw.c     -- rect/line/pixel drawing and the text renderer.
//   panel_spi_blit.c     -- the streaming blit (sync + async flush) and
//                            RDDID read-back.
// Shared statics widened to file-scope-internal (extern, panel_spi_
// prefixed) live in panel_spi_internal.h. No behavior changed by the split
// -- see that header and each file's own comments.
#include "panel_spi.h"
#include "panel_spi_internal.h"

#include <stdio.h>
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

const char *PANEL_SPI_TAG = "ILI9488";
/* ===================================================================
 * The two constraints this whole file is shaped around
 * ===================================================================
 *
 * (1) D/C AND ~RESET LIVE ON AN I2C EXPANDER (NORMALLY).
 *
 * On this board the panel's data/command select and reset are SX1509 pins
 * (IO14/IO15 -- which is which is a menuconfig switch, see below), not GPIOs.
 * KILNCTL_DISPLAY_DC_RESET_DIRECT_GPIO overrides this for bench wiring --
 * dc_gpio/reset_gpio in ILI9488Class are each independently either -1 (use
 * the expander, everything below applies) or a real ESP32 GPIO (nanosecond
 * toggles, no I2C, no expander dependency at all for that line). The D/C
 * batching discipline below still applies either way -- it costs nothing on
 * a direct GPIO and is what keeps things fast on the expander path.
 * A GPIO toggle is nanoseconds; an expander toggle is a whole I2C
 * transaction: address, register, byte, ACKs -- roughly 90us at 400kHz, and
 * it goes through another driver's queue on top of that.
 *
 * The consequence is that D/C transitions, not bytes, are the expensive unit
 * on this display. A naive driver that raised D/C for each parameter byte
 * would spend ~99% of its time on I2C. So:
 *
 *   - Every command is exactly ONE D/C toggle: low, send the opcode, high,
 *     send that command's ENTIRE payload in one spi_owner_transfer().
 *   - Fills, glyphs and blits leave D/C parked in "data" for their whole
 *     duration and push the pixel stream in chunks of up to chunk_bytes.
 *     A 480x320 clear is 2 D/C toggles and 320 SPI transfers, not 460,800
 *     I2C round trips.
 *   - dc_is_data/dc_valid shadow the line so back-to-back data pushes skip
 *     the I2C write entirely. dc_valid starts false so the driver never
 *     assumes a level it did not itself write.
 *
 * That the pixel stream can be split across many transfers at all is a
 * property the datasheet grants explicitly: §4.3 "Data Transfer Pause" says
 * that if CSX is released after a WHOLE byte of frame memory or parameter
 * data, the controller resumes from where it paused. spi_owner_transfer()
 * raises CS between transfers and every transfer here ends on a byte
 * boundary, so a chunked write is indistinguishable from one long one. This
 * is also what makes the streaming blit legal across separate UART frames.
 *
 * (2) THE PIXEL FORMAT IS RGB666, NOT RGB565.
 *
 * The ILI9488's command table (§5.2.34, COLMOD 3Ah) lists DBI[2:0] = 101 as
 * "16 bits/pixel", and §4.7.2 even names 65K-color as available on the
 * 4-line serial bus. But the serial data-format section that actually
 * defines the bit layout only ever defines two: 3 bit/pixel (§4.7.2.1) and
 * 18 bit/pixel (§4.7.2.2). There is no figure, anywhere in the document, for
 * 16 bit/pixel over 3- or 4-line SPI -- every 16bpp data-format figure
 * belongs to a parallel MCU bus (§4.7.3.1, §4.7.5.1) or to DPI/DSI. This
 * matches the universally-reported behaviour of the part: set COLMOD to 0x55
 * over SPI and the panel produces garbage.
 *
 * So this driver runs COLMOD = 0x66 (DBI 110, 18 bits/pixel) and sends three
 * bytes per pixel. The API still speaks RGB565 -- that is what the UART
 * protocol carries and what callers want -- and the widening happens in
 * exactly one place, panel_codec_rgb565_to_rgb666(), reached through
 * panel_spi_encode_pixel() below. It costs 50% more bytes on the wire than
 * RGB565 would; there is no alternative on this interface.
 *
 * A third thing that follows from (2): 480 x 320 x 3 bytes = 460,800 bytes.
 * There is no framebuffer, and there cannot be one. Every draw call goes
 * straight to the panel, which is why -- unlike SSD1306.c -- there is no
 * ILI9488_display() flush. Nothing is buffered, so nothing needs flushing.
 *
 * NOTE (DISPLAY_ST7796_PLAN.md Sec.12 Phase 3): (2) above is specific to the
 * ILI9488 part, not to this file any more. The ST7796 (st7796_panel.c) has a
 * real 16bpp SPI data format and runs COLMOD 0x55 -- panel_desc_t.
 * bytes_per_pixel and panel_spi_encode_pixel() are what let the SAME code
 * below (chunking, D/C batching, the blit state machine) serve either part
 * without knowing which one it is. Everything else in this comment block
 * (D/C batching, no framebuffer) still applies to both. */

/* Command opcodes, COLMOD/MADCTL bit values and reset/sleep timing constants
 * moved to panel_spi_internal.h (still #define, not renamed/widened -- a
 * macro has no linkage to collide over) since panel_spi_bringup.c and
 * panel_spi_blit.c need them too. */

/* Standard ASCII 5x7 font, printable characters 0x20-0x7F (96 glyphs). Each
 * glyph is 5 columns; each column byte is 7 vertically-stacked pixel bits
 * (bit0 = top row). Adafruit_GFX's glcdfont.c (BSD-style license), entries
 * 32..127 -- the same table and the same layout as SSD1306.c uses.
 *
 * It is duplicated here rather than shared because SSD1306.c keeps its copy
 * file-static and that file belongs to the display driver being retired;
 * 480 bytes of .rodata is cheaper than coupling the two drivers together for
 * the lifetime of the transition. */
const uint8_t panel_spi_font5x7[96][5] = {
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
    { 0x3C, 0x26, 0x23, 0x26, 0x3C }, // 0x7F (DEL) -- kept for table completeness
};

/* Power-on sequence, run once by init and again after any reset (a reset
 * returns the controller to its power-on defaults, including COLMOD = 06h,
 * which would silently corrupt every pixel we sent afterwards).
 *
 * The gamma/power/VCOM values below are the vendor-recommended ILI9488
 * sequence shipped with these modules, not values re-derived from the
 * datasheet -- they are panel-tuning constants whose correct values depend on
 * the glass, not on anything documented in the controller datasheet. The
 * commands that DO matter functionally and are datasheet-derived are COLMOD
 * (RGB666, see the top of this file), MADCTL (set separately by
 * set_rotation), SLPOUT and DISPON.
 *
 * ADJCTL3 (F7h) is included because every vendor sequence has it; note that
 * per §5.3.39 its only documented parameter bit, DSI_18_option, affects the
 * MIPI-DSI path, not the SPI path this board uses. It is harmless here.
 *
 * Phase 3 (DISPLAY_ST7796_PLAN.md Sec.12): this table used to be a private
 * cmd/len/params struct array read only by this file. It is now transcribed,
 * byte-for-byte identical cmd/param values, into the packed
 * [cmd][paramLen][params...] format panel_codec_init_step() decodes -- see
 * that function's comment in panel_codec.h -- so the SAME generic
 * panel_spi_run_init_sequence() below can run either this table or ST7796's
 * (st7796_panel.c) off nothing but the descriptor. Nothing here is
 * re-derived; every value is the one line above it, just regrouped. */
static const uint8_t panel_spi_init_bytes[] = {
    ILI9488_CMD_PGAMCTRL, 15, 0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78,
                               0x4C, 0x09, 0x0A, 0x08, 0x16, 0x1A, 0x0F,
    ILI9488_CMD_NGAMCTRL, 15, 0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45,
                               0x46, 0x04, 0x0E, 0x0D, 0x35, 0x37, 0x0F,
    ILI9488_CMD_PWCTRL1,   2, 0x17, 0x15,              /* VREG1OUT / VREG2OUT */
    ILI9488_CMD_PWCTRL2,   1, 0x41,                    /* VGH/VGL step-up factor */
    ILI9488_CMD_VMCTRL,    3, 0x00, 0x12, 0x80,
    ILI9488_CMD_COLMOD,    1, ILI9488_COLMOD_RGB666,
    ILI9488_CMD_IFMODE,    1, 0x00,                    /* DBI (MCU) interface, not DPI */
    ILI9488_CMD_FRMCTR1,   1, 0xA0,                    /* ~60 Hz frame rate */
    ILI9488_CMD_INVTR,     1, 0x02,                    /* 2-dot inversion */
    ILI9488_CMD_DFC,       2, 0x02, 0x02,               /* display function control */
    ILI9488_CMD_ENTRY_MODE,1, 0xC6,
    ILI9488_CMD_ADJCTL3,   4, 0xA9, 0x51, 0x2C, 0x82,
};

/* ===================================================================
 * Low-level transport
 * =================================================================== */

bool panel_spi_ready(const ILI9488Class *disp)
{
    return disp && disp->owner && disp->io && disp->dev && disp->lock && disp->scratch;
}

/* How many bytes one transfer may stage in the scratch. chunk_bytes is a public
 * field the header invites callers to *lower*; nothing stops one raising it,
 * and every staging path below indexes the scratch by it -- so it is clamped
 * here, once, rather than trusted in five places. */
size_t panel_spi_chunk(const ILI9488Class *disp)
{
    return (disp->chunk_bytes > ILI9488_SCRATCH_BYTES) ? (size_t)ILI9488_SCRATCH_BYTES
                                                       : disp->chunk_bytes;
}

/* Whole pixels (disp->panel->bytes_per_pixel bytes each) that fit in one
 * chunk -- panel_codec_chunk_pixels() does the clamp-then-divide; kept as a
 * named wrapper here so every call site reads the same. */
size_t panel_spi_chunk_pixels(const ILI9488Class *disp)
{
    return panel_codec_chunk_pixels(disp->chunk_bytes, ILI9488_SCRATCH_BYTES,
                                     disp->panel->bytes_per_pixel);
}

/* Generous, but finite. The longest thing held under this lock is a full-screen
 * fill: 320 transfers of 1440 bytes, well under a second even at a modest
 * clock. Waiting forever instead would let a wedged SPI owner take out every
 * task that ever draws -- including the one printing why. */
#define ILI9488_LOCK_TIMEOUT_MS 5000

bool panel_spi_lock(ILI9488Class *disp)
{
    if (xSemaphoreTake(disp->lock, pdMS_TO_TICKS(ILI9488_LOCK_TIMEOUT_MS)) == pdTRUE) {
        return true;
    }
    ESP_LOGE(PANEL_SPI_TAG, "timed out after %dms waiting for the display lock", ILI9488_LOCK_TIMEOUT_MS);
    return false;
}

void panel_spi_unlock(ILI9488Class *disp)
{
    xSemaphoreGive(disp->lock);
}

/* Drive D/C, but only if it isn't already where we want it. This shadow is
 * the whole reason a full-screen fill costs two I2C transactions instead of
 * one per chunk -- see the header comment. Any failure invalidates the
 * shadow, because a failed I2C write leaves the expander in an unknown
 * state and the next call must not skip re-driving it. */
esp_err_t panel_spi_set_dc(ILI9488Class *disp, bool data)
{
    if (disp->dc_valid && disp->dc_is_data == data) {
        return ESP_OK;
    }
    /* data=true -> pin high, matching kiln_io_lcd_dc's convention exactly
     * (see kiln_io.c: bank_b bit set for data) -- the panel's RS input reads
     * the same level regardless of which driver is toggling it. */
    esp_err_t err = disp->dc_gpio >= 0 ? gpio_set_level((gpio_num_t)disp->dc_gpio, data ? 1 : 0)
                                       : kiln_io_lcd_dc(disp->io, data);
    if (err != ESP_OK) {
        disp->dc_valid = false;
        ESP_LOGE(PANEL_SPI_TAG, "D/C -> %s failed: %s", data ? "data" : "command", esp_err_to_name(err));
        return err;
    }
    disp->dc_is_data = data;
    disp->dc_valid = true;
    return ESP_OK;
}

esp_err_t panel_spi_tx(ILI9488Class *disp, const uint8_t *buf, size_t len)
{
    return spi_owner_transfer(disp->owner, disp->dev, buf, len, NULL, 0, disp->cs_gpio);
}

/* One command = one D/C toggle down, one byte, one D/C toggle up, one
 * transfer carrying every parameter. Parameters are staged through the
 * driver's DMA-capable scratch so callers can pass .rodata or stack buffers
 * without worrying about DMA-capability. */
esp_err_t panel_spi_write_cmd(ILI9488Class *disp, uint8_t cmd, const uint8_t *params, size_t len)
{
    /* Bounded by the scratch, not just by chunk_bytes: the memcpy below stages
     * the parameters there, so a chunk_bytes a caller raised past the buffer
     * would turn a long parameter list into a heap overrun. */
    if (len > panel_spi_chunk(disp) || (len > 0 && !params)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = panel_spi_set_dc(disp, false);
    if (err != ESP_OK) return err;

    disp->scratch[0] = cmd;
    err = panel_spi_tx(disp, disp->scratch, 1);
    if (err != ESP_OK) {
        ESP_LOGE(PANEL_SPI_TAG, "command 0x%02X failed: %s", cmd, esp_err_to_name(err));
        return err;
    }

    if (len == 0) {
        return ESP_OK;
    }

    err = panel_spi_set_dc(disp, true);
    if (err != ESP_OK) return err;

    memcpy(disp->scratch, params, len);
    err = panel_spi_tx(disp, disp->scratch, len);
    if (err != ESP_OK) {
        ESP_LOGE(PANEL_SPI_TAG, "command 0x%02X params (%u bytes) failed: %s", cmd, (unsigned)len,
                 esp_err_to_name(err));
    }
    return err;
}

/* Sets the pixel window and issues RAMWR, leaving D/C in "data" so the caller
 * can stream pixels straight into it with no further expander traffic. On
 * return the panel's address counter is live: nothing else may talk to the
 * panel until the caller has finished, which is what the driver mutex (held
 * by every caller of this function) guarantees. */
esp_err_t panel_spi_begin_ram_write(ILI9488Class *disp, uint16_t x, uint16_t y,
                                         uint16_t w, uint16_t h)
{
    uint8_t caset[4];
    panel_codec_build_caset(x, w, caset);
    esp_err_t err = panel_spi_write_cmd(disp, ILI9488_CMD_CASET, caset, sizeof(caset));
    if (err != ESP_OK) return err;

    uint8_t paset[4];
    panel_codec_build_paset(y, h, paset);
    err = panel_spi_write_cmd(disp, ILI9488_CMD_PASET, paset, sizeof(paset));
    if (err != ESP_OK) return err;

    err = panel_spi_write_cmd(disp, ILI9488_CMD_RAMWR, NULL, 0);
    if (err != ESP_OK) return err;

    /* Park D/C in data for the whole pixel stream that follows. */
    return panel_spi_set_dc(disp, true);
}

/* Streams `pixels` copies of one colour into the currently open window. The
 * scratch is filled with the repeating 3-byte pattern once and then pushed as
 * many times as needed -- no per-pixel work, no per-chunk D/C toggle. */
esp_err_t panel_spi_push_color_run(ILI9488Class *disp, uint16_t color, uint32_t pixels)
{
    uint8_t bpp = disp->panel->bytes_per_pixel;
    uint8_t px[3];
    panel_spi_encode_pixel(disp, color, px);

    if (pixels == 0) return ESP_OK;

    size_t chunk_pixels = panel_spi_chunk_pixels(disp);
    if (chunk_pixels == 0) return ESP_ERR_INVALID_STATE; /* chunk_bytes below one pixel */
    if (chunk_pixels > pixels) chunk_pixels = (size_t)pixels;

    for (size_t i = 0; i < chunk_pixels; ++i) {
        memcpy(&disp->scratch[i * bpp], px, bpp);
    }

    while (pixels > 0) {
        size_t n = (pixels > chunk_pixels) ? chunk_pixels : (size_t)pixels;
        esp_err_t err = panel_spi_tx(disp, disp->scratch, n * bpp);
        if (err != ESP_OK) {
            ESP_LOGE(PANEL_SPI_TAG, "pixel run write failed: %s", esp_err_to_name(err));
            return err;
        }
        pixels -= n;
    }
    return ESP_OK;
}

/* ===================================================================
 * Bounds and blit-state guards
 * =================================================================== */

bool panel_spi_rect_in_bounds(const ILI9488Class *disp, uint16_t x, uint16_t y,
                                          uint16_t w, uint16_t h)
{
    /* panel_codec_rect_in_bounds() -- the w == 0/h == 0 rejection and the
     * 32-bit-arithmetic overflow guard (a caller passing x = 0xFFFF cannot
     * wrap into something that looks in-range) both moved there unchanged. */
    return panel_codec_rect_in_bounds(x, y, w, h, disp->width, disp->height);
}

void panel_spi_blit_clear_state(ILI9488Class *disp)
{
    memset(&disp->blit, 0, sizeof(disp->blit));
}

/* Every non-blit operation calls this first. A blit deliberately leaves the
 * panel's window open and its address counter mid-stream across UART frames;
 * anything else touching the panel in that gap would both corrupt its own
 * output and silently shift the rest of the image. The wire protocol says
 * this is an error that aborts the blit, so that is exactly what happens --
 * loudly, because a stuck blit is otherwise invisible from the PC side. */
esp_err_t panel_spi_reject_if_blitting(ILI9488Class *disp)
{
    /* DISPLAY_ST7796_PLAN.md 9.6: checked first and REFUSED, never
     * abandoned. disp->blit.active is already false during an outstanding
     * async chunk (ILI9488_blit_data_async() clears it before handing the
     * last chunk to the SPI owner), so without this check a draw call
     * landing in that gap would sail straight through the check below and
     * start writing disp->scratch (or issuing a new transfer on the same
     * device) while a DMA may still be reading it. See
     * ILI9488Class::async_pending's comment in panel_spi.h. */
    if (disp->async_pending) {
        ESP_LOGE(PANEL_SPI_TAG, "operation attempted while an async flush's last chunk is still in flight; refusing");
        return ESP_ERR_INVALID_STATE;
    }
    if (!disp->blit.active) return ESP_OK;
    ESP_LOGE(PANEL_SPI_TAG, "operation attempted with a blit open (%u/%u pixels sent); aborting the blit",
             (unsigned)disp->blit.pixels_done, (unsigned)disp->blit.pixels_total);
    panel_spi_blit_clear_state(disp);
    return ESP_ERR_INVALID_STATE;
}

/* MADCTL per rotation. The panel scans 320x480 natively; MV swaps row/column
 * so 1 and 3 are the landscape orientations, and MX/MY pick which corner is
 * the origin so that (0,0) is always top-left as seen by the user.
 *
 * File scope, used only by the panel_desc_t descriptor immediately below --
 * panel_spi_apply_rotation() (panel_spi_bringup.c) reads disp->panel->madctl
 * (the descriptor's own populated copy), never this table directly. Stays
 * `static`: nothing outside this file touches it. */
static const uint8_t panel_spi_madctl_by_rotation[4] = {
    ILI9488_MADCTL_MX,                                          /* 0: portrait  320x480 */
    ILI9488_MADCTL_MV,                                          /* 1: landscape 480x320 */
    ILI9488_MADCTL_MY,                                          /* 2: portrait  flipped */
    ILI9488_MADCTL_MX | ILI9488_MADCTL_MY | ILI9488_MADCTL_MV,  /* 3: landscape flipped */
};

/* ===================================================================
 * Panel descriptor (DISPLAY_ST7796_PLAN.md Sec.6 Step 2)
 * ===================================================================
 *
 * Phase 3: now actually consumed -- ILI9488_start() (panel_spi_bringup.c)
 * picks this descriptor (or ST7796_get_panel_desc()); panel_width/height are
 * still the real geometry constants; colmod/bytes_per_pixel/init_seq/madctl
 * now drive panel_spi_run_init_sequence() (panel_spi_bringup.c) and the
 * pixel-encode dispatch instead of being restated-but-unused metadata.
 *
 * Kept here, alongside panel_spi_init_bytes[] and panel_spi_font5x7[], rather
 * than with the rest of bring-up in panel_spi_bringup.c: `.init_len =
 * sizeof(panel_spi_init_bytes)` needs that array's real, complete type, which
 * only this translation unit has (an extern declaration of an array sized by
 * its own initializer cannot be sizeof()'d from elsewhere).
 *
 * init_seq/init_len point at panel_spi_init_bytes[] above, the same values
 * ili9488_init_sequence[] held in Phase 2, transcribed into the packed
 * format panel_codec_init_step() decodes -- see that array's own comment.
 * id_matches stays NULL, permanently: Sec.4's "RDDID bytes from the ILI9488
 * on this wiring" line was recorded 2026-09-03 as 0x00 0x00 0x00 (MISO not
 * driven on the read device handle) -- panel_detect_id_equals() refuses to
 * match on an all-0x00/all-0xFF triple by construction (that is "the read
 * failed", not "this is the ID"), so a matcher for this row would be dead
 * code. Per Sec.6 Step 3 a matcher must be written against bytes actually
 * read off this board, never datasheet nominal values, and no usable bytes
 * exist for this panel on this board. */
static const panel_desc_t ili9488_panel_desc = {
    .name = "ILI9488",
    .panel_width = ILI9488_PANEL_WIDTH,
    .panel_height = ILI9488_PANEL_HEIGHT,
    .colmod = ILI9488_COLMOD_RGB666,
    .bytes_per_pixel = 3,
    .init_seq = panel_spi_init_bytes,
    .init_len = sizeof(panel_spi_init_bytes),
    .madctl = { panel_spi_madctl_by_rotation[0], panel_spi_madctl_by_rotation[1],
                panel_spi_madctl_by_rotation[2], panel_spi_madctl_by_rotation[3] },
    .color_order_bit = ILI9488_MADCTL_COLOR_ORDER, /* BGR -- unchanged, this
                                                       * panel has always
                                                       * rendered correctly */
    .id_matches = NULL,
    .blank_via_power_off = false, /* today's ILI9488_clear() fills black over
                                    * RAMWR; it does not touch DISPOFF/power. */
};

const panel_desc_t *ILI9488_get_panel_desc(void)
{
    return &ili9488_panel_desc;
}
