// panel_codec.h -- pure, host-testable pixel/command encoding shared by the
// panel drivers, split out of ILI9488.c the same way max31856_codec.c is
// split out of MAX31856.c (see that header's own comment for the precedent):
// free of ESP-IDF/FreeRTOS/spi_owner includes, so test/test_panel_codec.c
// links this .c file directly with no stub layer needed.
//
// DISPLAY_ST7796_PLAN.md Sec.6 Step 1 names exactly what belongs here: RGB565
// widening/conversion per panel, CASET/PASET/RAMWR byte generation, the
// MADCTL byte for a given rotation, and the chunk-splitting/window
// bounds-overrun arithmetic that used to live inline in ILI9488_blit_data().
// Nothing in this file ever calls spi_owner_transfer() or touches a
// spi_device_handle_t -- ILI9488.c (and, from Phase 3 on, panel_spi.c) is
// where the bytes these functions produce actually go on the wire.
//
// Phase 2 (this file's introduction) keeps ILI9488.c byte-identical: every
// function below is a straight extraction of logic that already existed
// inline, not a new design. The ST7796 half of the split (the "null
// conversion" and the second MADCTL table) exists here so it is
// host-testable ahead of Phase 3, but nothing calls it yet -- there is no
// ST7796 wired to this board, and its init table/RDDID matcher are a later
// phase's job (they need bench-recorded bytes that do not exist yet).
#ifndef PANEL_CODEC_H
#define PANEL_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Pixel format conversion ---------------------------------------------
 *
 * Both take a UART-wire RGB565 uint16_t (5-6-5, the API's color type on both
 * panels) and write the bytes to push into the panel's RAMWR stream. */

/* ILI9488, COLMOD 0x66 (18 bpp/RGB666): widen each RGB565 channel to a full
 * 8 significant bits by BIT REPLICATION (not left-shift-and-zero-fill), so
 * 5-bit 0x1F becomes 0xFF rather than 0xF8 and white stays white. The
 * controller only looks at the top 6 bits of each byte; the low 2 are
 * ignored, so this is simultaneously an RGB565->RGB888 widening and a
 * correctly MSB-aligned RGB666 encoding. Extracted byte-for-byte from
 * ILI9488.c's ili9488_rgb565_to_rgb666() -- see that function's original
 * comment (now here) for the full derivation. out[] gets exactly 3 bytes:
 * R, G, B. */
void panel_codec_rgb565_to_rgb666(uint16_t color, uint8_t out[3]);

/* ST7796, COLMOD 0x55 (16 bpp/RGB565): the "null conversion" -- the wire
 * already carries RGB565, so this panel's RAMWR stream needs the same two
 * bytes the UART protocol already carries, verbatim, in the same byte order
 * (u16 little-endian, matching how ILI9488_blit_data reads its input). No
 * widening, no reordering. Exists so the panel_desc_t path (Phase 3) has a
 * pixel encoder function pointer of the same shape as the ILI9488 one to
 * select between; unused until then. */
void panel_codec_rgb565_passthrough(uint16_t color, uint8_t out[2]);

/* --- Window/command byte generation --------------------------------------
 *
 * CASET (2Ah) / PASET (2Bh) parameter blocks: start/end address, big-endian
 * 16-bit pairs, for a window of `w` (or `h`) pixels starting at `x` (or `y`).
 * Identical shape to ILI9488.c's ili9488_begin_ram_write() -- extracted so
 * the address arithmetic (x + w - 1) is host-tested on its own, including at
 * the boundary where it would wrap a uint16_t. */
void panel_codec_build_caset(uint16_t x, uint16_t w, uint8_t out[4]);
void panel_codec_build_paset(uint16_t y, uint16_t h, uint8_t out[4]);

/* MADCTL (36h) byte for `rotation` (0-3, low two bits used -- matches
 * ILI9488.c's `rotation & 0x03` masking): looks up the panel's own 4-entry
 * MY/MX/MV table (index 0 = portrait, 1 = landscape, 2 = portrait flipped,
 * 3 = landscape flipped -- see ILI9488.c's ili9488_apply_rotation() comment
 * for why those four map that way) and ORs in the panel's fixed color-order
 * bit (BGR on the ILI9488; see ILI9488_MADCTL_COLOR_ORDER). */
uint8_t panel_codec_madctl(const uint8_t madctl_by_rotation[4], uint8_t rotation,
                            uint8_t color_order_bit);

/* True for the two rotation indices (1 and 3) whose MADCTL sets MV, i.e. the
 * ones where the panel's native width/height are swapped to get the current
 * (as-rotated) width/height -- same odd/even test ILI9488.c's
 * ili9488_apply_rotation() already does on `rotation & 0x01`. */
bool panel_codec_rotation_swaps_dimensions(uint8_t rotation);

/* --- Bounds / chunking -----------------------------------------------------
 *
 * True if the w x h rectangle at (x, y) fits entirely within a
 * bounds_w x bounds_h surface. 32-bit intermediate arithmetic so a caller
 * passing x = 0xFFFF, w = 2 cannot wrap back into range -- same reasoning as
 * ILI9488.c's ili9488_rect_in_bounds(). w == 0 or h == 0 is never in bounds
 * (there is nothing to draw). */
bool panel_codec_rect_in_bounds(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                 uint16_t bounds_w, uint16_t bounds_h);

/* How many whole pixels fit in one SPI chunk, given a requested chunk size in
 * bytes, the scratch buffer's real capacity in bytes, and the panel's bytes
 * per pixel. Two clamps, same order ILI9488.c's ili9488_chunk() + its
 * divide-by-3 callers already apply: first chunk_bytes is capped to
 * scratch_bytes (a caller-settable field the header explicitly invites being
 * LOWERED, never raised past what was actually allocated), then the result is
 * floor-divided into whole pixels. Returns 0 if the clamped chunk size is
 * smaller than one pixel -- callers must treat that as "cannot make
 * progress", not loop forever pushing zero-length transfers. */
size_t panel_codec_chunk_pixels(size_t chunk_bytes, size_t scratch_bytes, size_t bytes_per_pixel);

/* True if `pixels` more pixels would overrun a window that holds
 * `pixels_total` pixels total, of which `pixels_done` have already been
 * streamed. Same overrun test as ILI9488_blit_data's
 * `pixels > pixels_total - pixels_done` guard, pulled out so the boundary
 * (exactly filling the window, one pixel over, pixels_done already at
 * pixels_total) is host-tested directly rather than only through the
 * ESP-IDF-dependent driver. */
bool panel_codec_blit_overruns(uint32_t pixels, uint32_t pixels_total, uint32_t pixels_done);

/* --- Init-sequence byte format (DISPLAY_ST7796_PLAN.md Sec.6 Step 2/12
 * Phase 3) -------------------------------------------------------------
 *
 * panel_desc_t.init_seq is a flat "const uint8_t *, size_t" pair (see the
 * struct below), not the cmd/len/params struct array ILI9488.c used to keep
 * privately -- a flat byte buffer is what a data descriptor can hold without
 * a second, panel-specific type. The format is a run of steps back-to-back,
 * no terminator (the caller stops at init_len):
 *
 *   [cmd:1][paramLen:1][params: paramLen bytes] [cmd:1][paramLen:1]...
 *
 * Both ILI9488 and ST7796 panel_spi.c instances are transcribed into this
 * format now that Phase 3 actually runs a sequence off the descriptor. */

/* Decodes one step starting at *offset. On success, advances *offset past
 * the whole step (cmd + length byte + params) and fills out_cmd/out_params/
 * out_param_len; out_params points into `seq` (never copied) and is NULL
 * when out_param_len is 0. Returns false -- leaving *offset unchanged --
 * when there is no complete step left: fewer than 2 bytes remain (this is
 * also the normal "reached the end" signal when *offset == len), or the
 * declared paramLen would read past `len`. Never reads past `seq[len-1]`. */
bool panel_codec_init_step(const uint8_t *seq, size_t len, size_t *offset,
                            uint8_t *out_cmd, const uint8_t **out_params,
                            uint8_t *out_param_len);

/* --- Panel descriptor (DISPLAY_ST7796_PLAN.md Sec.6 Step 2) ---------------
 *
 * Deliberately a data descriptor, not a function-pointer-per-operation
 * interface: only one panel is live per boot, and indirect calls in the
 * pixel path would cost more than they buy (see the plan doc). Phase 2
 * introduced the type and populated ONLY the ILI9488 instance, with
 * init_seq/init_len left NULL/0 (the real table was still the private
 * cmd/len/params struct array in ILI9488.c, a shape this flat pair could not
 * represent without re-deriving it).
 *
 * Phase 3 does that conversion: both the ILI9488 and ST7796 instances now
 * carry their real init_seq, in the packed byte format documented above
 * panel_codec_init_step(), and panel_spi.c (the ILI9488.c rename) runs the
 * sequence generically off whichever descriptor it was started with. */
typedef struct {
    const char *name;
    uint16_t panel_width, panel_height;   /* native, unrotated */
    uint8_t  colmod;                      /* 0x66 ILI9488, 0x55 ST7796 */
    uint8_t  bytes_per_pixel;             /* 3 or 2 */
    const uint8_t *init_seq;
    size_t init_len;
    uint8_t  madctl[4];                   /* per rotation, 0-3 -- BEFORE the
                                            * color-order bit is ORed in; see
                                            * panel_codec_madctl() */

    /* The color_order_bit argument panel_codec_madctl() ORs into madctl[]
     * above (MADCTL D3, BGR on the ILI9488) -- 2026-09-04, bench report on
     * commit 16fe9ed: blue rendered as purple on the real MSP4031, the
     * classic symptom of the wrong color-filter-order bit for THIS panel's
     * glass (a property of the glass, not the controller -- the datasheet
     * cannot say). Previously a single file-scope constant
     * (ILI9488_MADCTL_COLOR_ORDER, panel_spi.c) shared by both panels, which
     * is exactly the kind of one-value-for-two-different-things bug this
     * plan has hit before (see the touch_swap_xy fields above) -- moved
     * per-descriptor so a fix for one panel's glass cannot silently detune
     * the other's, which has been running correctly with BGR set. */
    uint8_t  color_order_bit;

    bool (*id_matches)(const uint8_t id[3]);
    bool blank_via_power_off;             /* see the DISPOFF question in Sec.4 */

    /* Touch axis mapping for whichever controller THIS panel ships with
     * (NS2009 on the ILI9488/TFT35, FT6336U on the ST7796/MSP4031) --
     * 2026-09-04, DISPLAY_ST7796_PLAN.md section 7. Deliberately live here,
     * not as one global Kconfig knob shared by both controllers: the two
     * controllers' raw-axis conventions relative to their own panel's
     * mounting are independent silicon/wiring facts, and a single shared
     * knob detunes whichever controller was calibrated second (exactly what
     * happened bringing up the FT6336U -- it silently inherited the
     * NS2009-tuned value and landed touches in the wrong place). Values are
     * still sourced from per-controller Kconfig bench knobs
     * (TOUCH_CAL_SWAP_XY/INVERT_X/INVERT_Y for NS2009,
     * TOUCH_CAP_SWAP_XY/INVERT_X/INVERT_Y for FT6336U, settings.h) -- these
     * fields exist so panel_detect_choose()'s resolved panel_desc_t is the
     * SINGLE place lvgl_port.c reads the mapping from, so auto-detect
     * switching panels switches the touch mapping with it, with no separate
     * self_calibrating branch to keep in sync by hand. */
    bool touch_swap_xy;
    bool touch_invert_x;
    bool touch_invert_y;
} panel_desc_t;

#ifdef __cplusplus
}
#endif

#endif // PANEL_CODEC_H
