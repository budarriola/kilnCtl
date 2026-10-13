#ifndef UI_UNIT_ENTRY_H
#define UI_UNIT_ENTRY_H

/* Pure display-unit <-> Celsius conversion for editable number pads (LCD review N8).
 * The builder's Target/Ramp pads show and accept the display unit and always STORE
 * Celsius. The affine map is derived from unit_pref_convert() itself (value at 0 and
 * 1), so ABSOLUTE (+32) and RATE (no offset) both invert exactly. */

#include "unit_pref.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Celsius value -> display unit, rounded to whole degrees (pad decimals = 0). */
float ui_unit_entry_to_display(float value_c, unit_pref_t pref, unit_pref_kind_t kind);

/* Value typed in the display unit -> Celsius, clamped to [min_c, max_c] so whole-degree
 * rounding in F can never store a value outside the profile bounds. */
float ui_unit_entry_to_celsius(float typed, unit_pref_t pref, unit_pref_kind_t kind, float min_c, float max_c);

#ifdef __cplusplus
}
#endif

#endif
