// st7796_panel.c -- see st7796_panel.h for what this is and why it has no
// SPI code in it.
//
// Source: firmware/KilnFW/Datasheets/4.0inch_SPI_Module_ST7796_MSP4030_
// MSP4031_V1.0_Keep/2-Specification/ST7796_Init.txt, the vendor's LCD_Init()
// function. Transcribed byte-for-byte in the order the vendor issues them --
// no reordering, no dropped commands, no "improved" values. The only
// deliberate omission is LCD_RESET() (a hardware reset pulse, not a command
// -- panel_spi.c's ili9488_hard_reset()/SWRESET fallback already does this
// generically for both panels) and LCD_direction()'s own MADCTL write, which
// is folded into panel_spi.c's shared apply-rotation step below rather than
// being transcribed as a fixed "portrait" MADCTL that would be immediately
// overwritten -- see that note below the byte table.
//
// Every command's operand count came directly from counting the vendor
// file's LCD_WR_DATA() calls between one LCD_WR_REG() and the next: zero
// calls means zero operands (0xC2, 0xA7, 0x13, 0x11, 0x29 below), which is
// unambiguous from the file's own structure, not a guess. No operand count
// in this table was ambiguous -- every run of LCD_WR_DATA() calls is
// terminated cleanly by the next LCD_WR_REG() or the end of LCD_Init().
#include "st7796_panel.h"

#include "settings.h"

// COLMOD (3Ah). DISPLAY_ST7796_PLAN.md Sec.12 Phase 3 asks for 0x55 (DPI and
// DBI nibbles both 101 = 16 bits/pixel, the same "set both nibbles" style
// ILI9488.c's own COLMOD write already uses) as the descriptor's declared
// value. The vendor's own LCD_Init() writes 0x05 instead -- only the DBI
// (MCU-interface) nibble set, DPI left at its power-on default of 0, which
// is fine because this board never uses the parallel DPI bus. Both encode
// the same DBI[2:0] = 101 = "16 bits/pixel" (MIPI DCS COLMOD table); they
// are functionally identical on this SPI-only wiring, and 0x3A's actual
// on-wire value below is transcribed EXACTLY as the vendor wrote it (0x05),
// per this phase's "transcribe exactly, do not improve" rule -- the 0x55 the
// plan names lives only in .colmod below, as descriptive metadata (this
// driver's write path sends whatever is in the init_seq bytes, not
// .colmod). Flagged here rather than silently reconciled.
#define ST7796_COLMOD_METADATA 0x55

// Packed [cmd][paramLen][params...] steps -- see panel_codec_init_step()'s
// comment in panel_codec.h for the format panel_spi.c's
// ili9488_run_init_sequence() decodes. Grouping matches the vendor file: one
// LCD_WR_REG() followed by however many LCD_WR_DATA() calls precede the next
// LCD_WR_REG().
//
// The vendor table includes a MADCTL (0x36) write of 0x48 (MX|BGR, hardcoded
// "portrait") and, at the very end, NORON (0x13), SLPOUT (0x11) and DISPON
// (0x29) with no delay shown between them. Both are transcribed here exactly
// as written rather than trimmed to "just the parts panel_spi.c doesn't
// already do generically", because trimming is exactly the kind of
// improve-by-omission this phase is not allowed to do. The result is safe,
// not merely tolerated:
//   - ili9488_run_init_sequence() always calls apply_rotation() (a fresh
//     MADCTL write for the CONFIGURED rotation) immediately after running
//     init_seq, superseding this table's hardcoded 0x48 -- the exact same
//     order the vendor's own LCD_Init() uses (LCD_Init() ends by calling
//     LCD_direction(), which re-writes MADCTL for the caller's chosen
//     direction). Nothing here is inventing that step; it mirrors the
//     vendor's own flow.
//   - ili9488_run_init_sequence() always follows init_seq with an explicit
//     SLPOUT + a full 120ms datasheet-mandated wait + DISPON + a further
//     wait, unconditionally, for both panels -- identical to what it always
//     did for the ILI9488, whose own table has never included SLPOUT/DISPON.
//     Re-issuing SLPOUT on an already-out-of-sleep panel and DISPON on an
//     already-on display are both no-ops per the ST7796 datasheet's own
//     command semantics -- so running the vendor's un-delayed 0x11/0x29
//     first and then the driver's own delayed pair second is redundant, not
//     incorrect, and it is the only way to add the datasheet's required
//     settling delay the raw vendor dump does not show without special-
//     casing one panel's post-init tail against the other's.
static const uint8_t st7796_init_bytes[] = {
    0xF0, 1, 0xC3,
    0xF0, 1, 0x96,
    0x36, 1, 0x48,
    0x3A, 1, 0x05,
    0xB0, 1, 0x80,
    0xB6, 2, 0x00, 0x02,
    0xB5, 4, 0x02, 0x03, 0x00, 0x04,
    0xB1, 2, 0x80, 0x10,
    0xB4, 1, 0x00,
    0xB7, 1, 0xC6,
    0xC5, 1, 0x1C,
    0xE4, 1, 0x31,
    0xE8, 8, 0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33,
    0xC2, 0,
    0xA7, 0,
    0xE0, 14, 0xF0, 0x09, 0x13, 0x12, 0x12, 0x2B, 0x3C, 0x44,
              0x4B, 0x1B, 0x18, 0x17, 0x1D, 0x21,
    0xE1, 14, 0xF0, 0x09, 0x13, 0x0C, 0x0D, 0x27, 0x3B, 0x44,
              0x4D, 0x0B, 0x17, 0x17, 0x1D, 0x21,
    0xF0, 1, 0x3C,
    0xF0, 1, 0x69,
    /* DEVIATION from the byte-for-byte vendor transcription above, added
     * 2026-09-04 after real bench hardware: the vendor's LCD_Init() never
     * sends INVON/INVOFF (0x21/0x20) at all, leaving the panel on its
     * power-on-reset inversion default -- and on this physical MSP4031 unit
     * that default renders as garish, wrong-contrast color (bench report:
     * "crazy contrast", not merely off-hue), the same failure mode
     * TFT_eSPI/Adafruit-style ST7796 drivers work around by explicitly
     * calling invertDisplay(true) (INVON, 0x21) during init -- this is a
     * known per-batch ST7796 clone quirk, not something the vendor's own
     * demo file would show if their sample unit happened to reset into the
     * other state. INVON takes no parameters. If a future panel batch needs
     * the opposite, this is the first thing to flip back to 0x20/omit. */
    0x21, 0,
    0x13, 0,
    0x11, 0,
    0x29, 0,
};

// MADCTL per rotation (before the BGR color-order bit, ORed in separately by
// panel_codec_madctl() -- see ILI9488.c's own table for the convention this
// mirrors). Derived from the vendor's LCD_direction(), which ORs a fixed
// (1<<3) BGR bit into every case:
//   case 0: (1<<3)|(1<<6)             = BGR|MX          -> MX        = 0x40
//   case 1: (1<<3)|(1<<5)             = BGR|MV          -> MV        = 0x20
//   case 2: (1<<3)|(1<<7)             = BGR|MY          -> MY        = 0x80
//   case 3: (1<<3)|(1<<7)|(1<<6)|(1<<5) = BGR|MY|MX|MV  -> MY|MX|MV  = 0xE0
// These are bit-for-bit the same MX/MV/MY values ILI9488.c's own
// ili9488_madctl_by_rotation[] table uses (both parts implement the same
// MIPI MADCTL bit positions). Spelled out directly in st7796_panel_desc's
// .madctl below (not as a named array indexed into it) because MSVC's host
// build rejects a static initializer that indexes another static const
// array as "not a constant expression" -- see that field's own comment.

// DISPLAY_ST7796_PLAN.md Sec.4's DISPOFF-blank-color question is still open
// ("Does the ST7796 blank black or white on DISPOFF + sleep-in?") -- nothing
// has been bench-measured. The ILI9488 blanks WHITE via DISPOFF (bench
// finding 2026-08-17, screen_idle.h), which is why screen_idle currently
// paints black over RAMWR instead of using set_power(false)/DISPOFF as a
// blanking strategy. blank_via_power_off is not read by anything yet
// (screen_idle.c is out of scope this phase and still does the RAMWR-paint
// strategy unconditionally, per panel), so this field is inert either way
// today -- but the SAFE default, until the bench answer exists, is to match
// the ILI9488's strategy (false: do not rely on power-off for blanking)
// rather than assume the ST7796 behaves oppositely. NEEDS BENCH CONFIRMATION
// (Sec.4) before anything ever reads this field to pick a blanking path.
static const panel_desc_t st7796_panel_desc = {
    .name = "ST7796",
    .panel_width = ST7796_PANEL_WIDTH,
    .panel_height = ST7796_PANEL_HEIGHT,
    .colmod = ST7796_COLMOD_METADATA,
    .bytes_per_pixel = 2,
    .init_seq = st7796_init_bytes,
    .init_len = sizeof(st7796_init_bytes),
    /* Spelled out (not `st7796_madctl_by_rotation[i]`) because MSVC's host
     * build rejects a static initializer that indexes another static const
     * array as "not a constant expression" -- GCC accepts it as an
     * extension, but the values must match st7796_madctl_by_rotation[]
     * above exactly (checked by test_st7796_panel.c). */
    .madctl = { 0x40, 0x20, 0x80, 0x80 | 0x40 | 0x20 },
    /* RDDID captured 2026-09-04 (MSP4031 wired to J2, AUTO-fragment
     * bootstrap procedure, DISPLAY_ST7796_PLAN.md Sec.4): 0x00 0x00 0x00,
     * the same MISO-not-driven read the ILI9488 row already hit -- not a
     * usable ID (panel_detect_id_equals() already refuses to match
     * all-0x00/all-0xFF for exactly this reason). Touch-address
     * corroboration during that same boot DID work (FT6336U answered at
     * 0x38, NS2009 absent at 0x48/0x49), so panel_detect_choose()'s touch
     * tiebreak is the only signal that still functions on this board's
     * wiring -- RDDID cannot distinguish either panel here. Stays NULL,
     * same reasoning as ili9488_panel_desc.id_matches above. */
    /* 2026-09-04, bench report on commit 16fe9ed: blue rendered as purple
     * on the real MSP4031 -- the wrong MADCTL color-filter-order bit for
     * this panel's glass. This module's glass is RGB, not the ILI9488's
     * BGR -- 0x00 (bit clear) instead of ILI9488_MADCTL_COLOR_ORDER (BGR,
     * 0x08). Panel_codec.h's color_order_bit field comment has the full
     * writeup on why this had to move off the old shared constant. Awaiting
     * owner visual confirmation on real hardware. */
    .color_order_bit = 0x00,
    .id_matches = NULL,
    .blank_via_power_off = false, /* safe default; NEEDS BENCH CONFIRMATION, see above */
    /* FT6336U's OWN bench-tuned Kconfig knobs (settings.h) -- deliberately
     * NOT TOUCH_CAL_SWAP_XY/INVERT_X/INVERT_Y, which are the NS2009-tuned
     * values for the other panel; see panel_codec.h's touch_swap_xy field
     * comment for why sharing one knob regressed touch here. */
    .touch_swap_xy = TOUCH_CAP_SWAP_XY,
    .touch_invert_x = TOUCH_CAP_INVERT_X,
    .touch_invert_y = TOUCH_CAP_INVERT_Y,
};

const panel_desc_t *ST7796_get_panel_desc(void)
{
    return &st7796_panel_desc;
}
