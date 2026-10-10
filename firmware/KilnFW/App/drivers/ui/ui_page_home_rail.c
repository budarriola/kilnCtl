#include "ui_page_home_rail.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

bool ui_page_home_rail_pill_on(bool io_ok, bool relay_on)
{
    return io_ok && relay_on;
}

const char *ui_page_home_rail_aux_caption(uint8_t aux_enabled_mask, uint32_t relay_idx)
{
    static const char *const k_caption[UI_PAGE_HOME_RAIL_AUX_CAPTION_COUNT] = {"A1", "A2", "A3", "A4"};
    if (relay_idx >= UI_PAGE_HOME_RAIL_AUX_CAPTION_COUNT || !(aux_enabled_mask & (1u << relay_idx))) {
        return "";
    }
    return k_caption[relay_idx];
}

int ui_page_home_rail_duty_pct(float duty_fraction)
{
    if (isnan(duty_fraction)) {
        return 0;
    }
    if (duty_fraction < 0.0f) {
        duty_fraction = 0.0f;
    }
    if (duty_fraction > 1.0f) {
        duty_fraction = 1.0f;
    }
    return (int)lroundf(duty_fraction * 100.0f);
}

void ui_page_home_rail_format_zone_temp(bool valid, bool stale, float temp_c, unit_pref_t unit, char *out,
                                        size_t out_cap)
{
    if (out == NULL || out_cap == 0) {
        return;
    }
    if (!valid || stale || isnan(temp_c)) {
        snprintf(out, out_cap, "--.-");
        return;
    }
    snprintf(out, out_cap, "%.0f%s", (double)unit_pref_convert(temp_c, unit, UNIT_PREF_KIND_ABSOLUTE),
             unit_pref_suffix(unit));
}

void ui_page_home_rail_format_kiln_watts(bool power_valid, float power_w, char *out, size_t out_cap)
{
    if (!power_valid || out_cap == 0) {
        if (out_cap > 0) {
            out[0] = '\0';
        }
        return;
    }
    snprintf(out, out_cap, "%.0f W", (double)power_w);
}

bool ui_page_home_rail_text_changed(const char *current, const char *next)
{
    if (current == NULL || next == NULL) {
        return true;
    }
    return strcmp(current, next) != 0;
}
