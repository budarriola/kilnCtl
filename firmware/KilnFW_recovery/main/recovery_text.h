// recovery_text.h -- 5x7 bitmap font + scanline renderer for the recovery
// status screen. Pure C, no ESP-IDF includes, so it is host-compilable
// (there is no host-test setup in firmware/KilnFW_recovery today; this split
// is what would make one cheap to add).
//
// Font: the public-domain "classic" 5x7 table (ASCII 0x20..0x7E), 5 columns
// per glyph, bit 0 of each column byte = top row. Glyph cell is 6 columns
// wide (5 + 1 blank spacing column) and 7 rows tall.
#ifndef RECOVERY_TEXT_H
#define RECOVERY_TEXT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RECOVERY_FONT_FIRST 0x20
#define RECOVERY_FONT_LAST  0x7E
#define RECOVERY_FONT_W     5
#define RECOVERY_FONT_H     7
#define RECOVERY_FONT_ADV   6 // glyph columns + 1 spacing column, before scale

// Returns the 5 column bytes for `c`, or NULL when `c` is outside
// 0x20..0x7E (callers draw '?' instead).
const uint8_t *recovery_font_glyph(char c);

// How many characters of `scale` fit across `width_px`.
int recovery_text_chars_per_line(int width_px, int scale);

// Renders ONE pixel row of a string into `out` as big-endian RGB565 (the
// byte order the ST7796 wants on the wire), exactly `width_px` pixels wide
// (2 * width_px bytes). `glyph_row` is 0..6; each glyph row is meant to be
// sent `scale` times by the caller. The string is truncated to what fits and
// the remainder of the line is filled with `bg`, so redrawing a shorter
// string over a longer one needs no separate clear. Characters outside the
// font render as '?'.
void recovery_text_scanline(const char *s, int scale, int glyph_row, uint16_t fg, uint16_t bg,
                            int width_px, uint8_t *out);

// Short name for an esp_reset_reason_t value (passed as int so this file stays
// free of ESP-IDF includes; numeric values are esp_system.h's enum and are
// pinned by the host test). Never returns NULL; unknown values give "UNKNOWN".
const char *recovery_reset_reason_name(int reason);

#ifdef __cplusplus
}
#endif

#endif // RECOVERY_TEXT_H
