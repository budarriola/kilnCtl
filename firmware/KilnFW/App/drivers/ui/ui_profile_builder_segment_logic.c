#include "ui_profile_builder_segment_logic.h"

#include <stdio.h>

#include "ui_unit_entry.h"

void ui_pbs_pad_capture(ui_pbs_pad_t *pad, unit_pref_t live_pref)
{
    pad->pref = live_pref;
}

float ui_pbs_pad_to_celsius(const ui_pbs_pad_t *pad, float typed, unit_pref_kind_t kind, float min_c, float max_c)
{
    return ui_unit_entry_to_celsius(typed, pad->pref, kind, min_c, max_c);
}

void ui_pbs_target_caption(unit_pref_t pref, char *out, size_t cap)
{
    snprintf(out, cap, "Target %s", unit_pref_suffix(pref));
}

void ui_pbs_ramp_caption(unit_pref_t pref, char *out, size_t cap)
{
    snprintf(out, cap, "Ramp %s/hr", unit_pref_suffix(pref));
}

const char *ui_pbs_limit_caption(unsigned segment_count, unsigned max_segments)
{
    if (segment_count >= max_segments) {
        return "Maximum 12 segments reached";
    }
    if (segment_count <= 1) {
        return "At least one segment is required";
    }
    return "";
}
