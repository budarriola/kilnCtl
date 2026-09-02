// Datasheet: firmware/KilnFW/Datasheets/ILI9488.pdf (343 pages), with
// firmware/KilnFW/docs/ILI9488.md as the local commentary. NOT under
// hardware/datasheets/ -- see NS2009.c's header for why that matters.
// Section citations in the comments below were verified against this
// document on 2026-08-24 (reset timing 13.4 Table 39 p308, SWRESET 5.2.2
// p150, SLPIN/SLPOUT 5.2.12/13 p165-166, COLMOD 5.2.34 p200, MADCTL
// 5.2.30 p192, SPI clock limits 17.4.3 p332).
#include "panel_spi.h"

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

static const char *TAG = "ILI9488";

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
 * ili9488_encode_pixel() below. It costs 50% more bytes on the wire than
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
 * bytes_per_pixel and ili9488_encode_pixel() are what let the SAME code
 * below (chunking, D/C batching, the blit state machine) serve either part
 * without knowing which one it is. Everything else in this comment block
 * (D/C batching, no framebuffer) still applies to both. */

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
 * at the top of this file for why 0x55 (16bpp) is not an option here. */
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

/* Standard ASCII 5x7 font, printable characters 0x20-0x7F (96 glyphs). Each
 * glyph is 5 columns; each column byte is 7 vertically-stacked pixel bits
 * (bit0 = top row). Adafruit_GFX's glcdfont.c (BSD-style license), entries
 * 32..127 -- the same table and the same layout as SSD1306.c uses.
 *
 * It is duplicated here rather than shared because SSD1306.c keeps its copy
 * file-static and that file belongs to the display driver being retired;
 * 480 bytes of .rodata is cheaper than coupling the two drivers together for
 * the lifetime of the transition. */
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
 * ili9488_run_init_sequence() below can run either this table or ST7796's
 * (st7796_panel.c) off nothing but the descriptor. Nothing here is
 * re-derived; every value is the one line above it, just regrouped. */
static const uint8_t ili9488_init_bytes[] = {
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
 * one buffer serves either panel. */
static inline void ili9488_encode_pixel(const ILI9488Class *disp, uint16_t color, uint8_t *out)
{
    if (disp->panel->bytes_per_pixel == 2) {
        panel_codec_rgb565_passthrough(color, out);
    } else {
        panel_codec_rgb565_to_rgb666(color, out);
    }
}

static inline bool ili9488_ready(const ILI9488Class *disp)
{
    return disp && disp->owner && disp->io && disp->dev && disp->lock && disp->scratch;
}

/* How many bytes one transfer may stage in the scratch. chunk_bytes is a public
 * field the header invites callers to *lower*; nothing stops one raising it,
 * and every staging path below indexes the scratch by it -- so it is clamped
 * here, once, rather than trusted in five places. */
static inline size_t ili9488_chunk(const ILI9488Class *disp)
{
    return (disp->chunk_bytes > ILI9488_SCRATCH_BYTES) ? (size_t)ILI9488_SCRATCH_BYTES
                                                       : disp->chunk_bytes;
}

/* Whole pixels (disp->panel->bytes_per_pixel bytes each) that fit in one
 * chunk -- panel_codec_chunk_pixels() does the clamp-then-divide; kept as a
 * named wrapper here so every call site reads the same. */
static inline size_t ili9488_chunk_pixels(const ILI9488Class *disp)
{
    return panel_codec_chunk_pixels(disp->chunk_bytes, ILI9488_SCRATCH_BYTES,
                                     disp->panel->bytes_per_pixel);
}

/* Generous, but finite. The longest thing held under this lock is a full-screen
 * fill: 320 transfers of 1440 bytes, well under a second even at a modest
 * clock. Waiting forever instead would let a wedged SPI owner take out every
 * task that ever draws -- including the one printing why. */
#define ILI9488_LOCK_TIMEOUT_MS 5000

static inline bool ili9488_lock(ILI9488Class *disp)
{
    if (xSemaphoreTake(disp->lock, pdMS_TO_TICKS(ILI9488_LOCK_TIMEOUT_MS)) == pdTRUE) {
        return true;
    }
    ESP_LOGE(TAG, "timed out after %dms waiting for the display lock", ILI9488_LOCK_TIMEOUT_MS);
    return false;
}

static inline void ili9488_unlock(ILI9488Class *disp)
{
    xSemaphoreGive(disp->lock);
}

/* Drive D/C, but only if it isn't already where we want it. This shadow is
 * the whole reason a full-screen fill costs two I2C transactions instead of
 * one per chunk -- see the header comment. Any failure invalidates the
 * shadow, because a failed I2C write leaves the expander in an unknown
 * state and the next call must not skip re-driving it. */
static esp_err_t ili9488_set_dc(ILI9488Class *disp, bool data)
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
        ESP_LOGE(TAG, "D/C -> %s failed: %s", data ? "data" : "command", esp_err_to_name(err));
        return err;
    }
    disp->dc_is_data = data;
    disp->dc_valid = true;
    return ESP_OK;
}

static esp_err_t ili9488_tx(ILI9488Class *disp, const uint8_t *buf, size_t len)
{
    return spi_owner_transfer(disp->owner, disp->dev, buf, len, NULL, 0, disp->cs_gpio);
}

/* One command = one D/C toggle down, one byte, one D/C toggle up, one
 * transfer carrying every parameter. Parameters are staged through the
 * driver's DMA-capable scratch so callers can pass .rodata or stack buffers
 * without worrying about DMA-capability. */
static esp_err_t ili9488_write_cmd(ILI9488Class *disp, uint8_t cmd, const uint8_t *params, size_t len)
{
    /* Bounded by the scratch, not just by chunk_bytes: the memcpy below stages
     * the parameters there, so a chunk_bytes a caller raised past the buffer
     * would turn a long parameter list into a heap overrun. */
    if (len > ili9488_chunk(disp) || (len > 0 && !params)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = ili9488_set_dc(disp, false);
    if (err != ESP_OK) return err;

    disp->scratch[0] = cmd;
    err = ili9488_tx(disp, disp->scratch, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "command 0x%02X failed: %s", cmd, esp_err_to_name(err));
        return err;
    }

    if (len == 0) {
        return ESP_OK;
    }

    err = ili9488_set_dc(disp, true);
    if (err != ESP_OK) return err;

    memcpy(disp->scratch, params, len);
    err = ili9488_tx(disp, disp->scratch, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "command 0x%02X params (%u bytes) failed: %s", cmd, (unsigned)len,
                 esp_err_to_name(err));
    }
    return err;
}

/* Sets the pixel window and issues RAMWR, leaving D/C in "data" so the caller
 * can stream pixels straight into it with no further expander traffic. On
 * return the panel's address counter is live: nothing else may talk to the
 * panel until the caller has finished, which is what the driver mutex (held
 * by every caller of this function) guarantees. */
static esp_err_t ili9488_begin_ram_write(ILI9488Class *disp, uint16_t x, uint16_t y,
                                         uint16_t w, uint16_t h)
{
    uint8_t caset[4];
    panel_codec_build_caset(x, w, caset);
    esp_err_t err = ili9488_write_cmd(disp, ILI9488_CMD_CASET, caset, sizeof(caset));
    if (err != ESP_OK) return err;

    uint8_t paset[4];
    panel_codec_build_paset(y, h, paset);
    err = ili9488_write_cmd(disp, ILI9488_CMD_PASET, paset, sizeof(paset));
    if (err != ESP_OK) return err;

    err = ili9488_write_cmd(disp, ILI9488_CMD_RAMWR, NULL, 0);
    if (err != ESP_OK) return err;

    /* Park D/C in data for the whole pixel stream that follows. */
    return ili9488_set_dc(disp, true);
}

/* Streams `pixels` copies of one colour into the currently open window. The
 * scratch is filled with the repeating 3-byte pattern once and then pushed as
 * many times as needed -- no per-pixel work, no per-chunk D/C toggle. */
static esp_err_t ili9488_push_color_run(ILI9488Class *disp, uint16_t color, uint32_t pixels)
{
    uint8_t bpp = disp->panel->bytes_per_pixel;
    uint8_t px[3];
    ili9488_encode_pixel(disp, color, px);

    if (pixels == 0) return ESP_OK;

    size_t chunk_pixels = ili9488_chunk_pixels(disp);
    if (chunk_pixels == 0) return ESP_ERR_INVALID_STATE; /* chunk_bytes below one pixel */
    if (chunk_pixels > pixels) chunk_pixels = (size_t)pixels;

    for (size_t i = 0; i < chunk_pixels; ++i) {
        memcpy(&disp->scratch[i * bpp], px, bpp);
    }

    while (pixels > 0) {
        size_t n = (pixels > chunk_pixels) ? chunk_pixels : (size_t)pixels;
        esp_err_t err = ili9488_tx(disp, disp->scratch, n * bpp);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "pixel run write failed: %s", esp_err_to_name(err));
            return err;
        }
        pixels -= n;
    }
    return ESP_OK;
}

/* ===================================================================
 * Bounds and blit-state guards
 * =================================================================== */

static inline bool ili9488_rect_in_bounds(const ILI9488Class *disp, uint16_t x, uint16_t y,
                                          uint16_t w, uint16_t h)
{
    /* panel_codec_rect_in_bounds() -- the w == 0/h == 0 rejection and the
     * 32-bit-arithmetic overflow guard (a caller passing x = 0xFFFF cannot
     * wrap into something that looks in-range) both moved there unchanged. */
    return panel_codec_rect_in_bounds(x, y, w, h, disp->width, disp->height);
}

static void ili9488_blit_clear_state(ILI9488Class *disp)
{
    memset(&disp->blit, 0, sizeof(disp->blit));
}

/* Every non-blit operation calls this first. A blit deliberately leaves the
 * panel's window open and its address counter mid-stream across UART frames;
 * anything else touching the panel in that gap would both corrupt its own
 * output and silently shift the rest of the image. The wire protocol says
 * this is an error that aborts the blit, so that is exactly what happens --
 * loudly, because a stuck blit is otherwise invisible from the PC side. */
static esp_err_t ili9488_reject_if_blitting(ILI9488Class *disp)
{
    if (!disp->blit.active) return ESP_OK;
    ESP_LOGE(TAG, "operation attempted with a blit open (%u/%u pixels sent); aborting the blit",
             (unsigned)disp->blit.pixels_done, (unsigned)disp->blit.pixels_total);
    ili9488_blit_clear_state(disp);
    return ESP_ERR_INVALID_STATE;
}

/* ===================================================================
 * Init / deinit
 * =================================================================== */

/* MADCTL per rotation. The panel scans 320x480 natively; MV swaps row/column
 * so 1 and 3 are the landscape orientations, and MX/MY pick which corner is
 * the origin so that (0,0) is always top-left as seen by the user.
 *
 * File scope (moved out of ili9488_apply_rotation() below, same four values)
 * so the panel_desc_t descriptor at the bottom of this file can cite the same
 * table instead of a second copy that could drift from it. */
static const uint8_t ili9488_madctl_by_rotation[4] = {
    ILI9488_MADCTL_MX,                                          /* 0: portrait  320x480 */
    ILI9488_MADCTL_MV,                                          /* 1: landscape 480x320 */
    ILI9488_MADCTL_MY,                                          /* 2: portrait  flipped */
    ILI9488_MADCTL_MX | ILI9488_MADCTL_MY | ILI9488_MADCTL_MV,  /* 3: landscape flipped */
};

static esp_err_t ili9488_apply_rotation(ILI9488Class *disp, uint8_t rotation)
{
    uint8_t madctl = panel_codec_madctl(disp->panel->madctl, rotation, ILI9488_MADCTL_COLOR_ORDER);
    esp_err_t err = ili9488_write_cmd(disp, ILI9488_CMD_MADCTL, &madctl, 1);
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
static esp_err_t ili9488_run_init_sequence(ILI9488Class *disp)
{
    const panel_desc_t *panel = disp->panel;
    size_t offset = 0;
    uint8_t cmd;
    const uint8_t *params;
    uint8_t param_len;
    while (panel_codec_init_step(panel->init_seq, panel->init_len, &offset,
                                  &cmd, &params, &param_len)) {
        esp_err_t err = ili9488_write_cmd(disp, cmd, params, param_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "init step 0x%02X failed: %s", cmd, esp_err_to_name(err));
            return err;
        }
    }
    if (offset != panel->init_len) {
        ESP_LOGE(TAG, "%s init_seq is malformed (stopped at byte %u of %u)",
                 panel->name, (unsigned)offset, (unsigned)panel->init_len);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ili9488_apply_rotation(disp, disp->rotation);
    if (err != ESP_OK) return err;

    err = ili9488_write_cmd(disp, ILI9488_CMD_SLPOUT, NULL, 0);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(ILI9488_SLEEP_WAIT_MS));  /* datasheet: 120ms before the next command */

    err = ili9488_write_cmd(disp, ILI9488_CMD_DISPON, NULL, 0);
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
static esp_err_t ili9488_hard_reset(ILI9488Class *disp)
{
    /* asserted=true -> pin low, matching kiln_io_lcd_reset's convention
     * (active-low ~RESET). Direct-GPIO mode has no "line doesn't exist"
     * uncertainty -- unlike the expander path, a bench-wired reset GPIO is
     * unambiguously real. */
    esp_err_t err = disp->reset_gpio >= 0 ? gpio_set_level((gpio_num_t)disp->reset_gpio, 0)
                                          : kiln_io_lcd_reset(disp->io, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "asserting ~RESET failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(ILI9488_RESET_PULSE_MS));  /* datasheet minimum is 10us */

    err = disp->reset_gpio >= 0 ? gpio_set_level((gpio_num_t)disp->reset_gpio, 1)
                                : kiln_io_lcd_reset(disp->io, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "releasing ~RESET failed: %s", esp_err_to_name(err));
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
        ESP_LOGE(TAG, "init called on an instance that is already up");
        return ESP_ERR_INVALID_STATE;
    }

    memset(disp, 0, sizeof(*disp));
    disp->owner = owner;
    disp->io = io;
    disp->cs_gpio = cs_gpio;
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
    esp_err_t err = gpio_config(&cs_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(cs) failed: %s", esp_err_to_name(err));
        return err;
    }
    gpio_set_level((gpio_num_t)cs_gpio, 1);

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
            ESP_LOGE(TAG, "gpio_config(dc) failed: %s", esp_err_to_name(err));
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
            ESP_LOGE(TAG, "gpio_config(reset) failed: %s", esp_err_to_name(err));
            return err;
        }
        gpio_set_level((gpio_num_t)reset_gpio, 1);
    }

    /* DMA-capable because every pixel push goes through it and the SPI
     * peripheral will DMA straight out of it. Allocated once; no draw call
     * ever touches the heap. */
    disp->scratch = heap_caps_malloc(ILI9488_SCRATCH_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!disp->scratch) {
        ESP_LOGE(TAG, "failed to allocate the %u-byte scratch buffer", (unsigned)ILI9488_SCRATCH_BYTES);
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
        .spics_io_num = -1,  /* CS driven by spi_owner, not the peripheral */
        .queue_size = 1,
    };
    err = spi_bus_add_device(host, &dev_config, &disp->dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device(write) failed: %s", esp_err_to_name(err));
        ILI9488_deinit(disp);
        return err;
    }

    spi_device_interface_config_t read_config = dev_config;
    read_config.clock_speed_hz = ILI9488_READ_CLOCK_HZ;
    err = spi_bus_add_device(host, &read_config, &disp->read_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device(read) failed: %s", esp_err_to_name(err));
        ILI9488_deinit(disp);
        return err;
    }

    /* Nothing else can hold this lock yet -- the instance is not visible to
     * any other task until init returns -- but taking it keeps the whole
     * bring-up on the same path every other caller uses, and a failure here
     * means the mutex itself is broken, which is not something to draw
     * through. Unwind what has been allocated so far rather than leak it. */
    if (!ili9488_lock(disp)) {
        ILI9488_deinit(disp);
        return ESP_ERR_TIMEOUT;
    }

    /* A hardware reset is preferred but genuinely optional here (the reset
     * line may not exist on this connector -- see ili9488_hard_reset), so a
     * failure falls through to the software reset rather than aborting. */
    if (ili9488_hard_reset(disp) != ESP_OK) {
        ESP_LOGW(TAG, "hardware reset unavailable; falling back to SWRESET");
        err = ili9488_write_cmd(disp, ILI9488_CMD_SWRESET, NULL, 0);
        if (err != ESP_OK) {
            ili9488_unlock(disp);
            ILI9488_deinit(disp);
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(ILI9488_RESET_WAIT_MS));
    }

    err = ili9488_run_init_sequence(disp);
    ili9488_unlock(disp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed: %s", esp_err_to_name(err));
        ILI9488_deinit(disp);
        return err;
    }

    esp_err_t clear_err = ILI9488_clear(disp, 0x0000);
    if (clear_err != ESP_OK) {
        ESP_LOGW(TAG, "initial clear failed: %s", esp_err_to_name(clear_err));
    }

    ESP_LOGI(TAG, "%s initialized: %ux%u (rotation %u), %u bpp, %d Hz write / %d Hz read",
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
        ESP_LOGE(TAG, "ILI9488_init (auto-detect bootstrap) failed: %s", esp_err_to_name(boot_err));
        return boot_err;
    }

    uint8_t id[3] = { 0, 0, 0 };
    esp_err_t id_err = ILI9488_read_id(disp, id);
    if (id_err != ESP_OK) {
        ESP_LOGW(TAG, "panel auto-detect: RDDID read failed (%s); treating as no match",
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
        ESP_LOGW(TAG, "panel auto-detect: SPI ID and touch-address signals disagree -- "
                      "RDDID read 0x%02X 0x%02X 0x%02X, touch NS2009=%d FT6336=%d, resolved %s (%s)",
                 id[0], id[1], id[2], (int)touch_ns2009, (int)touch_ft6336, detect.panel->name,
                 detect.source == PANEL_DETECT_SOURCE_FALLBACK ? "fallback" : "matched");
    }
    if (detect.source == PANEL_DETECT_SOURCE_FALLBACK) {
        ESP_LOGW(TAG, "panel auto-detect: no RDDID match (read 0x%02X 0x%02X 0x%02X against %u "
                      "candidate(s)) -- record these bytes in DISPLAY_ST7796_PLAN.md Sec.4, "
                      "booting the Kconfig default (%s) meanwhile",
                 id[0], id[1], id[2], (unsigned)detect.matched_count, detect.panel->name);
    } else {
        ESP_LOGI(TAG, "panel auto-detect: resolved %s (RDDID 0x%02X 0x%02X 0x%02X, %s)",
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
            ESP_LOGE(TAG, "ILI9488_init (auto-detect resolved panel %s) failed: %s", panel->name,
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
        ESP_LOGE(TAG, "ILI9488_init failed: %s", esp_err_to_name(err));
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
     * what makes every subsequent ili9488_ready() fail. */
    bool locked = disp->lock && ili9488_lock(disp);
    spi_device_handle_t dev = disp->dev;
    spi_device_handle_t read_dev = disp->read_dev;
    disp->dev = NULL;
    disp->read_dev = NULL;
    ili9488_blit_clear_state(disp);

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
    if (!ili9488_ready(disp)) return ESP_ERR_INVALID_STATE;

    if (!ili9488_lock(disp)) return ESP_ERR_TIMEOUT;
    /* A reset while a blit is open is not an "error" so much as an implicit
     * abandonment of the window -- but it still leaves the caller's stream
     * unfinished, so it is reported the same way. */
    ili9488_blit_clear_state(disp);

    esp_err_t err;
    if (hard) {
        err = ili9488_hard_reset(disp);
    } else {
        err = ili9488_write_cmd(disp, ILI9488_CMD_SWRESET, NULL, 0);
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
        err = ili9488_run_init_sequence(disp);
    }
    ili9488_unlock(disp);

    if (err == ESP_OK) {
        err = ILI9488_clear(disp, 0x0000);
    }
    return err;
}

esp_err_t ILI9488_set_power(ILI9488Class *disp, bool on)
{
    if (!ili9488_ready(disp)) return ESP_ERR_INVALID_STATE;

    if (!ili9488_lock(disp)) return ESP_ERR_TIMEOUT;
    esp_err_t err = ili9488_reject_if_blitting(disp);
    if (err == ESP_OK) {
        if (on) {
            err = ili9488_write_cmd(disp, ILI9488_CMD_SLPOUT, NULL, 0);
            if (err == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(ILI9488_SLEEP_WAIT_MS));
                err = ili9488_write_cmd(disp, ILI9488_CMD_DISPON, NULL, 0);
            }
        } else {
            err = ili9488_write_cmd(disp, ILI9488_CMD_DISPOFF, NULL, 0);
            if (err == ESP_OK) {
                err = ili9488_write_cmd(disp, ILI9488_CMD_SLPIN, NULL, 0);
                /* §5.2.x: 120ms must elapse after SLPIN before SLPOUT is
                 * accepted. Waiting here rather than in set_power(true) means
                 * a caller toggling power twice in a row cannot violate it. */
                vTaskDelay(pdMS_TO_TICKS(ILI9488_SLEEP_WAIT_MS));
            }
        }
    }
    ili9488_unlock(disp);
    return err;
}

esp_err_t ILI9488_set_rotation(ILI9488Class *disp, uint8_t rotation)
{
    if (!ili9488_ready(disp)) return ESP_ERR_INVALID_STATE;
    if (rotation > 3) return ESP_ERR_INVALID_ARG;

    if (!ili9488_lock(disp)) return ESP_ERR_TIMEOUT;
    esp_err_t err = ili9488_reject_if_blitting(disp);
    if (err == ESP_OK) {
        err = ili9488_apply_rotation(disp, rotation);
        if (err == ESP_OK) {
            /* Rotating does not move existing pixels -- it only changes how
             * new writes are addressed -- so the cursor is homed rather than
             * left pointing at a coordinate that may no longer exist. */
            disp->cursor_x = 0;
            disp->cursor_y = 0;
        }
    }
    ili9488_unlock(disp);
    return err;
}

esp_err_t ILI9488_set_invert(ILI9488Class *disp, bool invert)
{
    if (!ili9488_ready(disp)) return ESP_ERR_INVALID_STATE;

    if (!ili9488_lock(disp)) return ESP_ERR_TIMEOUT;
    esp_err_t err = ili9488_reject_if_blitting(disp);
    if (err == ESP_OK) {
        err = ili9488_write_cmd(disp, invert ? ILI9488_CMD_INVON : ILI9488_CMD_INVOFF, NULL, 0);
    }
    ili9488_unlock(disp);
    return err;
}

esp_err_t ILI9488_get_dimensions(ILI9488Class *disp, uint16_t *out_width, uint16_t *out_height)
{
    if (!disp || !out_width || !out_height) return ESP_ERR_INVALID_ARG;
    /* The readiness check is what stops the lock below being taken on a NULL
     * mutex -- this used to be the one public call that skipped it, so
     * asking an un-inited (or deinit'd) instance for its size faulted. */
    if (!ili9488_ready(disp)) return ESP_ERR_INVALID_STATE;
    if (!ili9488_lock(disp)) return ESP_ERR_TIMEOUT;
    *out_width = disp->width;
    *out_height = disp->height;
    ili9488_unlock(disp);
    return ESP_OK;
}

/* ===================================================================
 * Drawing
 * =================================================================== */

/* Caller holds the lock and has already bounds-checked. */
static esp_err_t ili9488_fill_rect_locked(ILI9488Class *disp, uint16_t x, uint16_t y,
                                          uint16_t w, uint16_t h, uint16_t color)
{
    esp_err_t err = ili9488_begin_ram_write(disp, x, y, w, h);
    if (err != ESP_OK) return err;
    return ili9488_push_color_run(disp, color, (uint32_t)w * h);
}

esp_err_t ILI9488_fill_rect(ILI9488Class *disp, uint16_t x, uint16_t y,
                            uint16_t w, uint16_t h, uint16_t color)
{
    if (!ili9488_ready(disp)) return ESP_ERR_INVALID_STATE;

    if (!ili9488_lock(disp)) return ESP_ERR_TIMEOUT;
    esp_err_t err = ili9488_reject_if_blitting(disp);
    if (err == ESP_OK) {
        err = ili9488_rect_in_bounds(disp, x, y, w, h)
                  ? ili9488_fill_rect_locked(disp, x, y, w, h, color)
                  : ESP_ERR_INVALID_ARG;
    }
    ili9488_unlock(disp);
    return err;
}

esp_err_t ILI9488_clear(ILI9488Class *disp, uint16_t color)
{
    if (!ili9488_ready(disp)) return ESP_ERR_INVALID_STATE;

    if (!ili9488_lock(disp)) return ESP_ERR_TIMEOUT;
    esp_err_t err = ili9488_reject_if_blitting(disp);
    if (err == ESP_OK) {
        err = ili9488_fill_rect_locked(disp, 0, 0, disp->width, disp->height, color);
        if (err == ESP_OK) {
            disp->cursor_x = 0;
            disp->cursor_y = 0;
        }
    }
    ili9488_unlock(disp);
    return err;
}

esp_err_t ILI9488_draw_pixel(ILI9488Class *disp, uint16_t x, uint16_t y, uint16_t color)
{
    return ILI9488_fill_rect(disp, x, y, 1, 1, color);
}

esp_err_t ILI9488_draw_rect(ILI9488Class *disp, uint16_t x, uint16_t y,
                            uint16_t w, uint16_t h, uint16_t color)
{
    if (!ili9488_ready(disp)) return ESP_ERR_INVALID_STATE;

    if (!ili9488_lock(disp)) return ESP_ERR_TIMEOUT;
    esp_err_t err = ili9488_reject_if_blitting(disp);
    if (err != ESP_OK) {
        ili9488_unlock(disp);
        return err;
    }
    if (!ili9488_rect_in_bounds(disp, x, y, w, h)) {
        ili9488_unlock(disp);
        return ESP_ERR_INVALID_ARG;
    }

    /* Four fills rather than a rect-shaped stream: each edge is one window
     * plus one contiguous run, and the two vertical edges skip the corners
     * the horizontal ones already covered. A 1- or 2-pixel-thin rectangle
     * degenerates correctly because the horizontal edges alone cover it. */
    err = ili9488_fill_rect_locked(disp, x, y, w, 1, color);
    if (err == ESP_OK && h > 1) {
        err = ili9488_fill_rect_locked(disp, x, (uint16_t)(y + h - 1), w, 1, color);
    }
    if (err == ESP_OK && h > 2) {
        err = ili9488_fill_rect_locked(disp, x, (uint16_t)(y + 1), 1, (uint16_t)(h - 2), color);
        if (err == ESP_OK && w > 1) {
            err = ili9488_fill_rect_locked(disp, (uint16_t)(x + w - 1), (uint16_t)(y + 1),
                                           1, (uint16_t)(h - 2), color);
        }
    }
    ili9488_unlock(disp);
    return err;
}

/* Bresenham, but emitting RUNS rather than pixels.
 *
 * Every single-pixel write on this panel costs a CASET + PASET + RAMWR
 * (three commands, up to two D/C toggles) for three bytes of payload, so a
 * per-pixel line would be dominated by expander traffic. Grouping the
 * consecutive pixels that share a row (for shallow lines) or a column (for
 * steep ones) into one fill turns a 480px diagonal from 480 window setups
 * into a handful, and costs nothing for the degenerate 45-degree case where
 * every run is length 1. */
static esp_err_t ili9488_draw_line_locked(ILI9488Class *disp, int x0, int y0, int x1, int y1,
                                          uint16_t color)
{
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;

    if (dy == 0) {
        int left = (x0 < x1) ? x0 : x1;
        return ili9488_fill_rect_locked(disp, (uint16_t)left, (uint16_t)y0, (uint16_t)(dx + 1), 1, color);
    }
    if (dx == 0) {
        int top = (y0 < y1) ? y0 : y1;
        return ili9488_fill_rect_locked(disp, (uint16_t)x0, (uint16_t)top, 1, (uint16_t)(dy + 1), color);
    }

    if (dx >= dy) {
        /* Shallow: step x every iteration, y occasionally -> horizontal runs. */
        int err_acc = dx / 2;
        int y = y0;
        int run_start = x0;
        for (int x = x0;; x += sx) {
            bool last = (x == x1);
            int next_y = y;
            err_acc -= dy;
            if (err_acc < 0) {
                next_y = y + sy;
                err_acc += dx;
                /* The error term can round one step past the endpoint on the
                 * final iteration. Clamping here is what keeps a rounding
                 * artifact from becoming an out-of-bounds write. */
                if ((sy > 0 && next_y > y1) || (sy < 0 && next_y < y1)) next_y = y;
            }
            if (next_y != y || last) {
                int a = (sx > 0) ? run_start : x;
                int b = (sx > 0) ? x : run_start;
                esp_err_t err = ili9488_fill_rect_locked(disp, (uint16_t)a, (uint16_t)y,
                                                         (uint16_t)(b - a + 1), 1, color);
                if (err != ESP_OK) return err;
                run_start = x + sx;
                y = next_y;
            }
            if (last) break;
        }
    } else {
        /* Steep: step y every iteration -> vertical runs. */
        int err_acc = dy / 2;
        int x = x0;
        int run_start = y0;
        for (int y = y0;; y += sy) {
            bool last = (y == y1);
            int next_x = x;
            err_acc -= dx;
            if (err_acc < 0) {
                next_x = x + sx;
                err_acc += dy;
                if ((sx > 0 && next_x > x1) || (sx < 0 && next_x < x1)) next_x = x;
            }
            if (next_x != x || last) {
                int a = (sy > 0) ? run_start : y;
                int b = (sy > 0) ? y : run_start;
                esp_err_t err = ili9488_fill_rect_locked(disp, (uint16_t)x, (uint16_t)a,
                                                         1, (uint16_t)(b - a + 1), color);
                if (err != ESP_OK) return err;
                run_start = y + sy;
                x = next_x;
            }
            if (last) break;
        }
    }
    return ESP_OK;
}

esp_err_t ILI9488_draw_line(ILI9488Class *disp, uint16_t x0, uint16_t y0,
                            uint16_t x1, uint16_t y1, uint16_t color)
{
    if (!ili9488_ready(disp)) return ESP_ERR_INVALID_STATE;

    if (!ili9488_lock(disp)) return ESP_ERR_TIMEOUT;
    esp_err_t err = ili9488_reject_if_blitting(disp);
    if (err == ESP_OK) {
        /* Both endpoints must be on-screen. Clipping is deliberately not
         * implemented: a silently-clipped line looks like a drawing bug,
         * while a rejected one points straight at the caller. */
        if (x0 >= disp->width || x1 >= disp->width || y0 >= disp->height || y1 >= disp->height) {
            err = ESP_ERR_INVALID_ARG;
        } else {
            err = ili9488_draw_line_locked(disp, x0, y0, x1, y1, color);
        }
    }
    ili9488_unlock(disp);
    return err;
}

/* ===================================================================
 * Text
 * =================================================================== */

esp_err_t ILI9488_set_text_cursor(ILI9488Class *disp, uint16_t x, uint16_t y)
{
    if (!ili9488_ready(disp)) return ESP_ERR_INVALID_STATE;

    if (!ili9488_lock(disp)) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_OK;
    if (x >= disp->width || y >= disp->height) {
        err = ESP_ERR_INVALID_ARG;
    } else {
        disp->cursor_x = x;
        disp->cursor_y = y;
    }
    ili9488_unlock(disp);
    return err;
}

esp_err_t ILI9488_set_text_style(ILI9488Class *disp, uint16_t fg, uint16_t bg,
                                 uint8_t size, bool opaque_background)
{
    if (!ili9488_ready(disp)) return ESP_ERR_INVALID_STATE;
    if (size < 1 || size > ILI9488_TEXT_SIZE_MAX) return ESP_ERR_INVALID_ARG;

    if (!ili9488_lock(disp)) return ESP_ERR_TIMEOUT;
    disp->text_fg = fg;
    disp->text_bg = bg;
    disp->text_size = size;
    disp->text_opaque = opaque_background;
    ili9488_unlock(disp);
    return ESP_OK;
}

/* Draws one glyph cell. Two strategies, picked by whether the background is
 * being painted:
 *
 *  - Opaque: the whole cell is one window and one contiguous pixel stream
 *    (foreground and background both), built one source row at a time in the
 *    scratch and pushed `size` times. One window setup per glyph.
 *  - Transparent: only lit pixels are drawn, as horizontal runs of set bits.
 *    That is more window setups but it is the only way to leave the existing
 *    background untouched with no framebuffer to composite against.
 */
static esp_err_t ili9488_draw_glyph_locked(ILI9488Class *disp, uint16_t x, uint16_t y,
                                           unsigned char c)
{
    const uint8_t *glyph = font5x7[c - 0x20];
    uint8_t size = disp->text_size;
    uint8_t bpp = disp->panel->bytes_per_pixel;
    uint16_t cell_w = (uint16_t)(ILI9488_FONT_CELL_WIDTH * size);
    uint16_t cell_h = (uint16_t)(ILI9488_FONT_CELL_HEIGHT * size);
    size_t row_bytes = (size_t)cell_w * bpp;

    if (disp->text_opaque && row_bytes <= ili9488_chunk(disp)) {
        esp_err_t err = ili9488_begin_ram_write(disp, x, y, cell_w, cell_h);
        if (err != ESP_OK) return err;

        uint8_t fg[3], bg[3];
        ili9488_encode_pixel(disp, disp->text_fg, fg);
        ili9488_encode_pixel(disp, disp->text_bg, bg);

        for (int row = 0; row < ILI9488_FONT_CELL_HEIGHT; ++row) {
            size_t n = 0;
            for (int col = 0; col < ILI9488_FONT_CELL_WIDTH; ++col) {
                /* Column 5 is the inter-glyph gap and row 7 the blank row
                 * below, both always background -- the same 6x8 cell the
                 * SSD1306 driver renders. */
                bool lit = (col < ILI9488_FONT_GLYPH_WIDTH) &&
                           (row < ILI9488_FONT_GLYPH_HEIGHT) &&
                           ((glyph[col] >> row) & 0x01);
                const uint8_t *px = lit ? fg : bg;
                for (uint8_t s = 0; s < size; ++s) {
                    memcpy(&disp->scratch[n], px, bpp);
                    n += bpp;
                }
            }
            /* Vertical scaling is free: push the same row `size` times. */
            for (uint8_t s = 0; s < size; ++s) {
                err = ili9488_tx(disp, disp->scratch, n);
                if (err != ESP_OK) return err;
            }
        }
        return ESP_OK;
    }

    /* Transparent (or a cell too wide for the configured chunk size). */
    if (disp->text_opaque) {
        esp_err_t err = ili9488_fill_rect_locked(disp, x, y, cell_w, cell_h, disp->text_bg);
        if (err != ESP_OK) return err;
    }
    for (int row = 0; row < ILI9488_FONT_GLYPH_HEIGHT; ++row) {
        int col = 0;
        while (col < ILI9488_FONT_GLYPH_WIDTH) {
            if (!((glyph[col] >> row) & 0x01)) {
                ++col;
                continue;
            }
            int run = 0;
            while (col + run < ILI9488_FONT_GLYPH_WIDTH &&
                   ((glyph[col + run] >> row) & 0x01)) {
                ++run;
            }
            esp_err_t err = ili9488_fill_rect_locked(disp,
                                                     (uint16_t)(x + col * size),
                                                     (uint16_t)(y + row * size),
                                                     (uint16_t)(run * size),
                                                     size, disp->text_fg);
            if (err != ESP_OK) return err;
            col += run;
        }
    }
    return ESP_OK;
}

static esp_err_t ili9488_write_char_locked(ILI9488Class *disp, char c)
{
    uint8_t size = disp->text_size;
    uint16_t cell_w = (uint16_t)(ILI9488_FONT_CELL_WIDTH * size);
    uint16_t cell_h = (uint16_t)(ILI9488_FONT_CELL_HEIGHT * size);

    if (c == '\n') {
        disp->cursor_x = 0;
        disp->cursor_y = (uint16_t)(disp->cursor_y + cell_h);
        if ((uint32_t)disp->cursor_y + cell_h > disp->height) disp->cursor_y = 0;
        return ESP_OK;
    }
    if (c == '\r') {
        disp->cursor_x = 0;
        return ESP_OK;
    }

    unsigned char uc = (unsigned char)c;
    if (uc < 0x20 || uc > 0x7F) {
        uc = '?';  /* same substitution the SSD1306 driver makes */
    }

    /* Wrap at the right edge, then from the bottom back to the top. No
     * scrolling: scrolling would mean reading the panel back, and read-back
     * on this interface is capped at 6.6 MHz for 3 bytes per pixel -- a full
     * screen would take the better part of a second. */
    if ((uint32_t)disp->cursor_x + cell_w > disp->width) {
        disp->cursor_x = 0;
        disp->cursor_y = (uint16_t)(disp->cursor_y + cell_h);
    }
    if ((uint32_t)disp->cursor_y + cell_h > disp->height) {
        disp->cursor_y = 0;
    }
    if ((uint32_t)disp->cursor_x + cell_w > disp->width ||
        (uint32_t)disp->cursor_y + cell_h > disp->height) {
        /* One glyph is bigger than the whole screen -- only reachable with an
         * absurd text size on a narrow rotation, but it must not write out
         * of bounds. The vertical half matters as much as the horizontal:
         * neither ili9488_draw_glyph_locked nor ili9488_begin_ram_write
         * bounds-checks, they trust this. */
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = ili9488_draw_glyph_locked(disp, disp->cursor_x, disp->cursor_y, uc);
    if (err != ESP_OK) return err;
    disp->cursor_x = (uint16_t)(disp->cursor_x + cell_w);
    return ESP_OK;
}

esp_err_t ILI9488_write_char(ILI9488Class *disp, char c)
{
    if (!ili9488_ready(disp)) return ESP_ERR_INVALID_STATE;

    if (!ili9488_lock(disp)) return ESP_ERR_TIMEOUT;
    esp_err_t err = ili9488_reject_if_blitting(disp);
    if (err == ESP_OK) {
        err = ili9488_write_char_locked(disp, c);
    }
    ili9488_unlock(disp);
    return err;
}

/* Takes the lock once for the whole string rather than per character: a
 * half-printed string interleaved with another task's drawing is both ugly
 * and, because the cursor is shared state, wrong. */
esp_err_t ILI9488_write(ILI9488Class *disp, const char *data, size_t len)
{
    if (!ili9488_ready(disp)) return ESP_ERR_INVALID_STATE;
    if (!data && len > 0) return ESP_ERR_INVALID_ARG;

    if (!ili9488_lock(disp)) return ESP_ERR_TIMEOUT;
    esp_err_t err = ili9488_reject_if_blitting(disp);
    for (size_t i = 0; err == ESP_OK && i < len; ++i) {
        err = ili9488_write_char_locked(disp, data[i]);
    }
    ili9488_unlock(disp);
    return err;
}

esp_err_t ILI9488_write_str(ILI9488Class *disp, const char *str)
{
    if (!str) return ESP_ERR_INVALID_ARG;
    return ILI9488_write(disp, str, strlen(str));
}

esp_err_t ILI9488_vprintf(ILI9488Class *disp, const char *fmt, va_list args)
{
    if (!fmt) return ESP_ERR_INVALID_ARG;
    if (!ili9488_ready(disp)) return ESP_ERR_INVALID_STATE;

    va_list args_copy;
    va_copy(args_copy, args);
    int needed = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);
    if (needed < 0) return ESP_FAIL;

    char *buf = malloc((size_t)needed + 1);
    if (!buf) return ESP_ERR_NO_MEM;

    vsnprintf(buf, (size_t)needed + 1, fmt, args);
    esp_err_t err = ILI9488_write(disp, buf, (size_t)needed);
    free(buf);
    return err;
}

esp_err_t ILI9488_printf(ILI9488Class *disp, const char *fmt, ...)
{
    if (!fmt) return ESP_ERR_INVALID_ARG;
    if (!ili9488_ready(disp)) return ESP_ERR_INVALID_STATE;
    va_list args;
    va_start(args, fmt);
    esp_err_t err = ILI9488_vprintf(disp, fmt, args);
    va_end(args);
    return err;
}

/* ===================================================================
 * Streaming blit
 * ===================================================================
 *
 * The point of this three-call shape is that a 480x320 image is 460,800
 * bytes -- far more than one UART frame (127 bytes of payload) and far more
 * than the ESP could buffer. So the window is opened once and left open
 * while the PC dribbles pixels in over hundreds of frames.
 *
 * That is legal precisely because of the datasheet's Data Transfer Pause
 * rule (§4.3): releasing CS between whole bytes of frame memory data pauses
 * rather than aborts the write, and the controller resumes at the same
 * address. So the gaps between UART frames -- milliseconds of them -- are
 * invisible to the panel.
 *
 * The cost is that the driver is holding hardware state across calls that
 * the caller could get wrong, so every transition is checked:
 *   - DATA before BEGIN                 -> ESP_ERR_INVALID_STATE
 *   - BEGIN while already open          -> the old window is abandoned (logged)
 *   - an odd byte count (a split pixel) -> ESP_ERR_INVALID_ARG, blit aborted
 *   - more pixels than the window holds -> ESP_ERR_INVALID_SIZE, blit aborted
 *   - END with the window unfilled      -> succeeds, but logs the shortfall
 * Anything else touching the panel aborts the blit (ili9488_reject_if_blitting).
 */

esp_err_t ILI9488_blit_begin(ILI9488Class *disp, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    if (!ili9488_ready(disp)) return ESP_ERR_INVALID_STATE;

    if (!ili9488_lock(disp)) return ESP_ERR_TIMEOUT;
    if (disp->blit.active) {
        ESP_LOGW(TAG, "BLIT_BEGIN with a blit already open (%u/%u pixels); abandoning the old one",
                 (unsigned)disp->blit.pixels_done, (unsigned)disp->blit.pixels_total);
        ili9488_blit_clear_state(disp);
    }
    if (!ili9488_rect_in_bounds(disp, x, y, w, h)) {
        ili9488_unlock(disp);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ili9488_begin_ram_write(disp, x, y, w, h);
    if (err == ESP_OK) {
        disp->blit.active = true;
        disp->blit.x = x;
        disp->blit.y = y;
        disp->blit.w = w;
        disp->blit.h = h;
        disp->blit.pixels_total = (uint32_t)w * h;
        disp->blit.pixels_done = 0;
    }
    ili9488_unlock(disp);
    return err;
}

esp_err_t ILI9488_blit_data(ILI9488Class *disp, const uint8_t *data, size_t len)
{
    if (!ili9488_ready(disp)) return ESP_ERR_INVALID_STATE;
    if (!data && len > 0) return ESP_ERR_INVALID_ARG;

    if (!ili9488_lock(disp)) return ESP_ERR_TIMEOUT;

    if (!disp->blit.active) {
        ili9488_unlock(disp);
        ESP_LOGE(TAG, "BLIT_DATA with no open window");
        return ESP_ERR_INVALID_STATE;
    }
    if ((len & 1u) != 0) {
        /* A half pixel would shift every following pixel by one byte and
         * smear the rest of the image; there is no way to recover, so the
         * blit ends here. */
        ESP_LOGE(TAG, "BLIT_DATA length %u is odd (split RGB565 pixel); aborting", (unsigned)len);
        ili9488_blit_clear_state(disp);
        ili9488_unlock(disp);
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t pixels = (uint32_t)(len / 2);
    if (panel_codec_blit_overruns(pixels, disp->blit.pixels_total, disp->blit.pixels_done)) {
        ESP_LOGE(TAG, "BLIT_DATA overruns the window: %u more pixels, %u remaining; aborting",
                 (unsigned)pixels,
                 (unsigned)(disp->blit.pixels_total - disp->blit.pixels_done));
        ili9488_blit_clear_state(disp);
        ili9488_unlock(disp);
        return ESP_ERR_INVALID_SIZE;
    }

    /* D/C is already parked in "data" from blit_begin and stays there: this
     * loop is pure SPI, with zero expander traffic no matter how many chunks
     * or how many UART frames the image takes. */
    /* chunk_pixels of zero would make the loop below advance by nothing and
     * spin forever with the lock held -- reachable by lowering chunk_bytes
     * under three, which the header explicitly invites callers to do. */
    esp_err_t err = ESP_OK;
    size_t chunk_pixels = ili9488_chunk_pixels(disp);
    if (chunk_pixels == 0) {
        ESP_LOGE(TAG, "chunk_bytes (%u) is smaller than one pixel; aborting the blit",
                 (unsigned)disp->chunk_bytes);
        ili9488_blit_clear_state(disp);
        ili9488_unlock(disp);
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t bpp = disp->panel->bytes_per_pixel;
    uint32_t sent = 0;
    while (sent < pixels) {
        size_t n = (size_t)(pixels - sent);
        if (n > chunk_pixels) n = chunk_pixels;

        if (bpp == 2) {
            /* Phase 3 fast path: at COLMOD 0x55 (ST7796) the wire's RGB565
             * u16-LE IS the RAMWR byte stream -- panel_codec's "null
             * conversion" -- so there is nothing to compute per pixel. One
             * memcpy of the whole chunk replaces the per-pixel loop below,
             * which is what actually avoids widening rather than just
             * calling a no-op conversion function once per pixel. */
            memcpy(disp->scratch, &data[sent * 2], n * 2);
        } else {
            for (size_t i = 0; i < n; ++i) {
                const uint8_t *src = &data[(sent + i) * 2];
                uint16_t color = (uint16_t)(src[0] | ((uint16_t)src[1] << 8));  /* u16 LE on the wire */
                panel_codec_rgb565_to_rgb666(color, &disp->scratch[i * 3]);
            }
        }
        err = ili9488_tx(disp, disp->scratch, n * bpp);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "BLIT_DATA transfer failed: %s; aborting", esp_err_to_name(err));
            ili9488_blit_clear_state(disp);
            ili9488_unlock(disp);
            return err;
        }
        sent += n;
    }

    disp->blit.pixels_done += pixels;
    ili9488_unlock(disp);
    return ESP_OK;
}

esp_err_t ILI9488_blit_end(ILI9488Class *disp)
{
    if (!ili9488_ready(disp)) return ESP_ERR_INVALID_STATE;

    if (!ili9488_lock(disp)) return ESP_ERR_TIMEOUT;
    if (!disp->blit.active) {
        ili9488_unlock(disp);
        ESP_LOGE(TAG, "BLIT_END with no open window");
        return ESP_ERR_INVALID_STATE;
    }
    if (disp->blit.pixels_done < disp->blit.pixels_total) {
        /* Not an error: a caller is allowed to stop early, and the panel
         * simply keeps whatever was already in the unwritten part of the
         * window. Logged because it is far more often a dropped frame. */
        ESP_LOGW(TAG, "BLIT_END with %u of %u pixels written",
                 (unsigned)disp->blit.pixels_done, (unsigned)disp->blit.pixels_total);
    }
    ili9488_blit_clear_state(disp);

    /* NOP closes the memory write cleanly: per §5.2.x any new command ends
     * the RAMWR stream, and sending a harmless one now means the next
     * operation cannot accidentally be interpreted as more pixel data. */
    esp_err_t err = ili9488_write_cmd(disp, ILI9488_CMD_NOP, NULL, 0);
    ili9488_unlock(disp);
    return err;
}

esp_err_t ILI9488_blit_abort(ILI9488Class *disp)
{
    if (!ili9488_ready(disp)) return ESP_ERR_INVALID_STATE;
    if (!ili9488_lock(disp)) return ESP_ERR_TIMEOUT;
    ili9488_blit_clear_state(disp);
    esp_err_t err = ili9488_write_cmd(disp, ILI9488_CMD_NOP, NULL, 0);
    ili9488_unlock(disp);
    return err;
}

bool ILI9488_blit_active(ILI9488Class *disp)
{
    if (!disp || !disp->lock) return false;
    /* A lock we cannot take says nothing about the blit state, and this
     * function has no way to report "don't know" -- so it answers with the
     * conservative one: callers use `true` to refuse other drawing. */
    if (!ili9488_lock(disp)) return true;
    bool active = disp->blit.active;
    ili9488_unlock(disp);
    return active;
}

/* ===================================================================
 * Read-back
 * =================================================================== */

/* RDDID (04h), §5.2.3: 24 bits of ID preceded by dummy data.
 *
 * Two things about reads on this part, both from the datasheet:
 *
 *  1. The read clock is much slower than the write clock. §17.4.3 gives
 *     twc (serial clock cycle, write) >= 50ns but trc (read) >= 150ns --
 *     20 MHz vs 6.67 MHz. Clocking a read at the write speed is the classic
 *     reason an ILI9488 "has no ID": the panel is fine, the sampling is not.
 *     Hence the separate slow device handle (ILI9488_READ_CLOCK_HZ).
 *
 *  2. The dummy is ambiguous in the document. The command table (§5.2.3)
 *     calls the 1st parameter "dummy data", i.e. a whole byte, while the
 *     4-line read waveform (Figure 10) shows a single "Dummy Clock Cycle"
 *     before D23. This driver reads four bytes and discards the first, which
 *     matches the byte reading and is what byte-oriented SPI masters can
 *     actually do. If the IDs ever come back looking shifted left by one bit
 *     relative to a known-good value, the one-bit reading is the right one
 *     and this needs a bit-banged or 33-bit transfer instead.
 *
 * MISO is wired on J2 (pin 8), so a read is physically possible here -- but
 * plenty of these modules leave the panel's SDO unconnected internally or
 * share it with the touch controller, which is why an all-zero/all-ones
 * answer is reported as ESP_ERR_NOT_FOUND rather than as success. */
esp_err_t ILI9488_read_id(ILI9488Class *disp, uint8_t out_id[3])
{
    if (!ili9488_ready(disp) || !disp->read_dev) return ESP_ERR_INVALID_STATE;
    if (!out_id) return ESP_ERR_INVALID_ARG;

    if (!ili9488_lock(disp)) return ESP_ERR_TIMEOUT;
    esp_err_t err = ili9488_reject_if_blitting(disp);
    if (err != ESP_OK) {
        ili9488_unlock(disp);
        return err;
    }

    err = ili9488_set_dc(disp, false);
    if (err == ESP_OK) {
        disp->scratch[0] = ILI9488_CMD_RDDID;
        err = spi_owner_transfer(disp->owner, disp->read_dev, disp->scratch, 1, NULL, 0,
                                 disp->cs_gpio);
    }
    if (err != ESP_OK) {
        ili9488_unlock(disp);
        ESP_LOGE(TAG, "RDDID command failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Clock out four don't-care bytes to clock in dummy + ID1..ID3. tx and rx
     * use disjoint slices of the scratch because the transfer is full-duplex
     * and would otherwise overwrite the tx pattern as it reads. */
    err = ili9488_set_dc(disp, true);
    if (err == ESP_OK) {
        uint8_t *tx = disp->scratch;
        uint8_t *rx = disp->scratch + 8;
        memset(tx, 0x00, 4);
        memset(rx, 0x00, 4);
        err = spi_owner_transfer(disp->owner, disp->read_dev, tx, 4, rx, 4, disp->cs_gpio);
        if (err == ESP_OK) {
            out_id[0] = rx[1];
            out_id[1] = rx[2];
            out_id[2] = rx[3];
            if ((out_id[0] | out_id[1] | out_id[2]) == 0x00 ||
                (out_id[0] & out_id[1] & out_id[2]) == 0xFF) {
                ESP_LOGW(TAG, "RDDID returned %02X %02X %02X -- MISO is probably not driven",
                         out_id[0], out_id[1], out_id[2]);
                err = ESP_ERR_NOT_FOUND;
            }
        }
    }
    ili9488_unlock(disp);
    return err;
}

/* ===================================================================
 * Panel descriptor (DISPLAY_ST7796_PLAN.md Sec.6 Step 2)
 * ===================================================================
 *
 * Phase 3: now actually consumed -- ILI9488_start() picks this descriptor
 * (or ST7796_get_panel_desc()) and every function above reads panel_width/
 * height are still the real geometry constants; colmod/bytes_per_pixel/
 * init_seq/madctl now drive ili9488_run_init_sequence() and the pixel-encode
 * dispatch instead of being restated-but-unused metadata.
 *
 * init_seq/init_len point at ili9488_init_bytes[] above, the same values
 * ili9488_init_sequence[] held in Phase 2, transcribed into the packed
 * format panel_codec_init_step() decodes -- see that array's own comment.
 * id_matches stays NULL: Sec.4's "RDDID bytes from the ILI9488 on this
 * wiring" line is still an open checkbox, and per Sec.6 Step 3, a matcher
 * must be written against bytes actually read off this board, never
 * datasheet nominal values. That is Phase 4's job. */
static const panel_desc_t ili9488_panel_desc = {
    .name = "ILI9488",
    .panel_width = ILI9488_PANEL_WIDTH,
    .panel_height = ILI9488_PANEL_HEIGHT,
    .colmod = ILI9488_COLMOD_RGB666,
    .bytes_per_pixel = 3,
    .init_seq = ili9488_init_bytes,
    .init_len = sizeof(ili9488_init_bytes),
    .madctl = { ili9488_madctl_by_rotation[0], ili9488_madctl_by_rotation[1],
                ili9488_madctl_by_rotation[2], ili9488_madctl_by_rotation[3] },
    .id_matches = NULL,
    .blank_via_power_off = false, /* today's ILI9488_clear() fills black over
                                    * RAMWR; it does not touch DISPOFF/power. */
};

const panel_desc_t *ILI9488_get_panel_desc(void)
{
    return &ili9488_panel_desc;
}
