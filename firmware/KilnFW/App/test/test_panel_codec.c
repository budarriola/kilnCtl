// Host tests for the pure panel pixel/command codec (../drivers/
// panel_codec.c), split out of ILI9488.c per DISPLAY_ST7796_PLAN.md Sec.6
// Step 1 -- see that header's own comment for the split rationale (same
// precedent as max31856_codec.c/test_max31856_codec.c).
//
// Every vector below was checked against ILI9488.c's ORIGINAL inline logic
// (the code these functions were extracted from, byte for byte) before the
// extraction, not re-derived from scratch -- this phase's whole point is
// "prove the emitted byte stream did not change".
#include "test_common.h"
#include "../drivers/panel_codec.h"

#include <stdint.h>

void run_test_panel_codec(void)
{
    TEST_SECTION("panel_codec");

    /* --- RGB565 -> RGB666 (ILI9488): bit-REPLICATION, not shift-and-zero -- */
    {
        uint8_t out[3];

        /* Pure white: every RGB565 bit set. Bit replication must produce
         * 0xFF/0xFF/0xFF; a naive left-shift-and-zero-fill (r5 << 3, no OR)
         * would instead produce 0xF8/0xFC/0xF8 -- visibly not-quite-white.
         * This is the vector that actually distinguishes replication from
         * the more obvious wrong implementation, so it is checked first. */
        panel_codec_rgb565_to_rgb666(0xFFFF, out);
        TEST_CHECK(out[0] == 0xFF, "rgb565->666: white R = 0xFF (bit-replicated, not 0xF8)");
        TEST_CHECK(out[1] == 0xFF, "rgb565->666: white G = 0xFF (bit-replicated, not 0xFC)");
        TEST_CHECK(out[2] == 0xFF, "rgb565->666: white B = 0xFF (bit-replicated, not 0xF8)");

        /* Pure black: every bit clear, both replication and naive shift agree
         * here, but it is still worth pinning down as the other end. */
        panel_codec_rgb565_to_rgb666(0x0000, out);
        TEST_CHECK(out[0] == 0x00, "rgb565->666: black R = 0x00");
        TEST_CHECK(out[1] == 0x00, "rgb565->666: black G = 0x00");
        TEST_CHECK(out[2] == 0x00, "rgb565->666: black B = 0x00");

        /* r5 = 0x01 (only the LOW bit of the 5-bit red field set): the
         * replicated value is (0x01 << 3) | (0x01 >> 2) = 0x08 | 0x00 =
         * 0x08. This is the vector that specifically exercises the "OR in
         * the top bits again" half of replication -- a shift-only
         * implementation gets 0x08 too by coincidence at this one value, but
         * the low-bit *source* only shows up correctly here because 0x01 >> 2
         * is 0, so it is paired with the r5 = 0x1F (all-ones) case above,
         * which is where shift-only and replication actually diverge (0xF8
         * vs 0xFF). Together the two vectors pin down both halves of the
         * formula. */
        panel_codec_rgb565_to_rgb666((uint16_t)(0x0001u << 11), out);
        TEST_CHECK(out[0] == 0x08, "rgb565->666: r5=0x01 -> R=0x08 ((1<<3)|(1>>2))");

        /* Green carries 6 bits already (g6 = 0x21 = 0b100001, the top and
         * bottom bit of the 6-bit field): replicated = (0x21<<2)|(0x21>>4) =
         * 0x84 | 0x02 = 0x86. Exercises the >>4 tail specifically (the 2 bits
         * that get replicated down for green, vs >>2 for red/blue's 3). */
        panel_codec_rgb565_to_rgb666((uint16_t)(0x21u << 5), out);
        TEST_CHECK(out[1] == 0x86, "rgb565->666: g6=0x21 -> G=0x86 ((0x21<<2)|(0x21>>4))");

        /* b5 = 0x11 (0b10001, top+bottom bit of the 5-bit field): replicated
         * = (0x11<<3)|(0x11>>2) = 0x88 | 0x04 = 0x8C. */
        panel_codec_rgb565_to_rgb666(0x0011u, out);
        TEST_CHECK(out[2] == 0x8C, "rgb565->666: b5=0x11 -> B=0x8C ((0x11<<3)|(0x11>>2))");

        /* Channel independence: a color with only green set must leave R/B
         * at 0 -- catches an accidental cross-channel mask/shift error. */
        panel_codec_rgb565_to_rgb666((uint16_t)(0x3Fu << 5), out);
        TEST_CHECK(out[0] == 0x00 && out[2] == 0x00,
                   "rgb565->666: green-only input leaves R and B at 0x00");
    }

    /* --- RGB565 passthrough (ST7796 fast path), MSB-first on the wire --- */
    /* --- FIXED 2026-09-04: was LSB-first ("u16 LE"), which is LVGL's own --- */
    /* --- in-memory byte order, not the MIPI-DCS RAMWR wire order a real --- */
    /* --- ST7796 module actually reads (see panel_codec.c's comment) --- */
    /* --- for the full story and the camera-verified bench evidence.   --- */
    {
        uint8_t out[2];
        panel_codec_rgb565_passthrough(0x1234, out);
        TEST_CHECK(out[0] == 0x12 && out[1] == 0x34,
                   "rgb565 passthrough: MSB first on the wire, no widening (0x1234 -> 12,34)");
        panel_codec_rgb565_passthrough(0xFFFF, out);
        TEST_CHECK(out[0] == 0xFF && out[1] == 0xFF, "rgb565 passthrough: 0xFFFF -> FF,FF");
        panel_codec_rgb565_passthrough(0x0000, out);
        TEST_CHECK(out[0] == 0x00 && out[1] == 0x00, "rgb565 passthrough: 0x0000 -> 00,00");
        /* Negative test, inline: a byte-swapped triple that would pass the
           OLD (buggy) LE assertion must fail this one -- proves this check
           can actually catch the regression it exists to catch. */
        panel_codec_rgb565_passthrough(0x1234, out);
        TEST_CHECK(!(out[0] == 0x34 && out[1] == 0x12),
                   "rgb565 passthrough: does NOT reproduce the old LE-on-the-wire bug");
    }

    /* --- CASET / PASET byte generation -------------------------------------
     * Big-endian x, then big-endian (x + w - 1). Matches ILI9488.c's
     * ili9488_begin_ram_write() before extraction: caset = {x>>8, x, x1>>8,
     * x1}, paset the same shape for y/h. */
    {
        uint8_t out[4];

        panel_codec_build_caset(0x0000, 480, out);
        /* x1 = 0 + 480 - 1 = 479 = 0x01DF */
        TEST_CHECK(out[0] == 0x00 && out[1] == 0x00 && out[2] == 0x01 && out[3] == 0xDF,
                   "CASET: x=0 w=480 -> 00 00 01 DF");

        panel_codec_build_caset(0x0064, 1, out);
        /* x=100=0x0064, w=1 -> x1 = 100 = 0x0064 (single-pixel window) */
        TEST_CHECK(out[0] == 0x00 && out[1] == 0x64 && out[2] == 0x00 && out[3] == 0x64,
                   "CASET: x=100 w=1 -> x1 == x (single-pixel window)");

        panel_codec_build_paset(0x0000, 320, out);
        /* y1 = 319 = 0x013F */
        TEST_CHECK(out[0] == 0x00 && out[1] == 0x00 && out[2] == 0x01 && out[3] == 0x3F,
                   "PASET: y=0 h=320 -> 00 00 01 3F");

        /* Boundary at a uint16_t rollover point that stays in range: the
         * panel's widest legal window (479+1) starting right at the top of
         * the 16-bit x1 byte split, x=254 w=2 -> x1=255=0xFF (still single
         * byte in the low half). */
        panel_codec_build_caset(254, 2, out);
        TEST_CHECK(out[2] == 0x00 && out[3] == 0xFF, "CASET: x=254 w=2 -> x1=255 (0x00FF)");
    }

    /* --- MADCTL -------------------------------------------------------------
     * Table lookup masked to the low two bits, OR'd with the panel's fixed
     * color-order bit -- matches ili9488_apply_rotation()'s
     * madctl_by_rotation[rotation & 0x03] | ILI9488_MADCTL_COLOR_ORDER. */
    {
        const uint8_t table[4] = { 0x40, 0x20, 0x80, 0xE0 }; /* MX, MV, MY, MX|MY|MV */
        const uint8_t color_order = 0x08; /* BGR bit */

        TEST_CHECK(panel_codec_madctl(table, 0, color_order) == (0x40 | 0x08),
                   "MADCTL: rotation 0 -> table[0] | color_order");
        TEST_CHECK(panel_codec_madctl(table, 1, color_order) == (0x20 | 0x08),
                   "MADCTL: rotation 1 -> table[1] | color_order");
        TEST_CHECK(panel_codec_madctl(table, 2, color_order) == (0x80 | 0x08),
                   "MADCTL: rotation 2 -> table[2] | color_order");
        TEST_CHECK(panel_codec_madctl(table, 3, color_order) == (0xE0 | 0x08),
                   "MADCTL: rotation 3 -> table[3] | color_order");
        /* Masking: rotation 4 (0b100) must behave as rotation 0, not index
         * out of the 4-entry table. */
        TEST_CHECK(panel_codec_madctl(table, 4, color_order) == (0x40 | 0x08),
                   "MADCTL: rotation 4 masks down to rotation 0 (& 0x03)");
        TEST_CHECK(panel_codec_madctl(table, 7, color_order) == (0xE0 | 0x08),
                   "MADCTL: rotation 7 masks down to rotation 3 (& 0x03)");

        TEST_CHECK(panel_codec_rotation_swaps_dimensions(0) == false, "rotation 0: no dimension swap");
        TEST_CHECK(panel_codec_rotation_swaps_dimensions(1) == true, "rotation 1: dimension swap");
        TEST_CHECK(panel_codec_rotation_swaps_dimensions(2) == false, "rotation 2: no dimension swap");
        TEST_CHECK(panel_codec_rotation_swaps_dimensions(3) == true, "rotation 3: dimension swap");
    }

    /* --- Rect-in-bounds, including the wraparound guard --------------------- */
    {
        TEST_CHECK(panel_codec_rect_in_bounds(0, 0, 480, 320, 480, 320) == true,
                   "rect_in_bounds: full-screen rect exactly fits");
        TEST_CHECK(panel_codec_rect_in_bounds(0, 0, 481, 320, 480, 320) == false,
                   "rect_in_bounds: 1px too wide is out of bounds");
        TEST_CHECK(panel_codec_rect_in_bounds(479, 0, 1, 320, 480, 320) == true,
                   "rect_in_bounds: 1px rect at the right edge fits");
        TEST_CHECK(panel_codec_rect_in_bounds(480, 0, 1, 320, 480, 320) == false,
                   "rect_in_bounds: 1px rect one past the right edge does not fit");
        TEST_CHECK(panel_codec_rect_in_bounds(0, 0, 0, 10, 480, 320) == false,
                   "rect_in_bounds: w=0 is never in bounds");
        TEST_CHECK(panel_codec_rect_in_bounds(0, 0, 10, 0, 480, 320) == false,
                   "rect_in_bounds: h=0 is never in bounds");
        /* The wraparound guard: x=0xFFFF, w=2 would wrap to 1 in 16-bit math
         * (0xFFFF + 2 = 0x10001 -> truncates to 1), which is < bounds_w and
         * would look in-range to naive 16-bit arithmetic. The 32-bit
         * intermediate must catch this. */
        TEST_CHECK(panel_codec_rect_in_bounds(0xFFFF, 0, 2, 1, 480, 320) == false,
                   "rect_in_bounds: x=0xFFFF w=2 does not wrap into range");
    }

    /* --- Chunk-pixel arithmetic, including the boundaries ------------------- */
    {
        /* Requested chunk smaller than the scratch: no clamp, floor-divide by
         * bytes_per_pixel (3, ILI9488's RGB666). 100 bytes / 3 = 33 pixels,
         * 1 byte left over -- exercises the floor, not round. */
        TEST_CHECK(panel_codec_chunk_pixels(100, 1440, 3) == 33,
                   "chunk_pixels: 100 bytes / 3 bpp floors to 33, not 33.33");

        /* Requested chunk larger than the scratch: clamps to scratch first.
         * 2000 requested, 1440 available, /3 = 480 -- the real
         * ILI9488_SCRATCH_PIXELS/ILI9488_SCRATCH_BYTES boundary. */
        TEST_CHECK(panel_codec_chunk_pixels(2000, 1440, 3) == 480,
                   "chunk_pixels: oversized request clamps to scratch_bytes first (1440/3=480)");

        /* Requested chunk exactly at the scratch size: same clamp, same
         * result -- the boundary itself, not past it. */
        TEST_CHECK(panel_codec_chunk_pixels(1440, 1440, 3) == 480,
                   "chunk_pixels: request == scratch_bytes -> full 480 pixels");

        /* chunk_bytes below one pixel: must return 0, the sentinel every
         * caller checks for "cannot make progress" (ILI9488_blit_data /
         * ili9488_push_color_run both abort rather than loop on a 0-pixel
         * chunk). 2 bytes is one less than one 3-byte RGB666 pixel. */
        TEST_CHECK(panel_codec_chunk_pixels(2, 1440, 3) == 0,
                   "chunk_pixels: chunk smaller than one pixel -> 0 (not a huge/negative wrap)");
        TEST_CHECK(panel_codec_chunk_pixels(0, 1440, 3) == 0,
                   "chunk_pixels: chunk_bytes = 0 -> 0");

        /* bytes_per_pixel = 0 would be a division by zero in a naive
         * implementation; must return 0, not crash or wrap. Not reachable
         * from either real panel today (3 or 2), but a defensive caller
         * (a hypothetical malformed panel_desc_t) must get a safe answer. */
        TEST_CHECK(panel_codec_chunk_pixels(1440, 1440, 0) == 0,
                   "chunk_pixels: bytes_per_pixel = 0 -> 0, not a divide-by-zero");

        /* 2 bytes/pixel path (ST7796, unused today but host-tested ahead of
         * Phase 3): 1440 scratch bytes / 2 = 720 pixels. */
        TEST_CHECK(panel_codec_chunk_pixels(1440, 1440, 2) == 720,
                   "chunk_pixels: 2 bpp path, 1440/2=720");
    }

    /* --- Blit overrun, at and around the exact boundary ---------------------
     * Matches ILI9488_blit_data's `pixels > pixels_total - pixels_done`. */
    {
        TEST_CHECK(panel_codec_blit_overruns(100, 1000, 0) == false,
                   "blit_overruns: well within the window is not an overrun");

        /* Exactly filling the remaining window: NOT an overrun (the last
         * legal DATA call of a blit). */
        TEST_CHECK(panel_codec_blit_overruns(100, 1000, 900) == false,
                   "blit_overruns: exactly filling the remaining window is fine");

        /* One pixel past exactly filling it: IS an overrun. This is the
         * boundary that actually matters -- off-by-one here either rejects a
         * legal last chunk or accepts a real overrun. */
        TEST_CHECK(panel_codec_blit_overruns(101, 1000, 900) == true,
                   "blit_overruns: one pixel past the remaining window IS an overrun");

        /* Window already fully sent (pixels_done == pixels_total): any more
         * pixels overrun, even zero... except zero more pixels is fine (a
         * degenerate empty DATA call is not itself an error). */
        TEST_CHECK(panel_codec_blit_overruns(1, 1000, 1000) == true,
                   "blit_overruns: window already full, any more pixels overruns");
        TEST_CHECK(panel_codec_blit_overruns(0, 1000, 1000) == false,
                   "blit_overruns: window already full, zero MORE pixels is not an overrun");

        /* pixels_done > pixels_total should never happen from the real
         * driver, but the guard must not wrap a subtraction into a huge
         * "remaining" count that then accepts an actual overrun. */
        TEST_CHECK(panel_codec_blit_overruns(1, 500, 600) == true,
                   "blit_overruns: pixels_done > pixels_total still rejects more pixels");
    }
}
