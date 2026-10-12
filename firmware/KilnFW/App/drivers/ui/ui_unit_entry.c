#include "ui_unit_entry.h"

#include <math.h>

float ui_unit_entry_to_display(float value_c, unit_pref_t pref, unit_pref_kind_t kind)
{
    return roundf(unit_pref_convert(value_c, pref, kind));
}

float ui_unit_entry_to_celsius(float typed, unit_pref_t pref, unit_pref_kind_t kind, float min_c, float max_c)
{
    const float off = unit_pref_convert(0.0f, pref, kind);
    const float scale = unit_pref_convert(1.0f, pref, kind) - off;
    float c = (scale != 0.0f) ? (typed - off) / scale : typed;
    if (c < min_c) c = min_c;
    if (c > max_c) c = max_c;
    return c;
}
