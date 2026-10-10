#ifndef UI_PROFILE_BUILDER_SEGMENT_LOGIC_H
#define UI_PROFILE_BUILDER_SEGMENT_LOGIC_H

/* Pure, LVGL-free seams behind ui_page_profile_builder_segment.c (review A3,
 * docs/audits/REVIEW_LCD_PCTOOLS_CAMPAIGNS_2026-10-10.md): R4 (the number pad
 * converts typed values back with the unit captured when the pad OPENED) and
 * R5 (card/pad caption text). Host-tested; the page only calls these. */

#include <stddef.h>

#include "unit_pref.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Unit the open pad was rendered in. */
typedef struct {
    unit_pref_t pref;
} ui_pbs_pad_t;

/* Call when a pad opens, with the live unit at that moment. */
void ui_pbs_pad_capture(ui_pbs_pad_t *pad, unit_pref_t live_pref);

/* Pad's typed value -> Celsius, using ONLY the captured unit (never the live one). */
float ui_pbs_pad_to_celsius(const ui_pbs_pad_t *pad, float typed, unit_pref_kind_t kind, float min_c, float max_c);

/* "Target <suffix>" / "Ramp <suffix>/hr" caption text. */
void ui_pbs_target_caption(unit_pref_t pref, char *out, size_t cap);
void ui_pbs_ramp_caption(unit_pref_t pref, char *out, size_t cap);

/* Text under the nav row: limit message for the current segment count. */
const char *ui_pbs_limit_caption(unsigned segment_count, unsigned max_segments);

#ifdef __cplusplus
}
#endif

#endif
