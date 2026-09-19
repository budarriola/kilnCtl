#include "ui_page_home_rail.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

bool ui_page_home_kiln_suffix_visible(uint8_t config_count)
{
    return config_count >= 2;
}

void ui_page_home_append_kiln_suffix(char *status_buf, size_t status_buf_cap, uint8_t config_count,
                                      bool has_active_name, const char *active_name)
{
    if (!ui_page_home_kiln_suffix_visible(config_count)) {
        return;
    }
    size_t used = strlen(status_buf);
    if (used >= status_buf_cap) {
        return;
    }
    if (has_active_name && active_name != NULL) {
        snprintf(status_buf + used, status_buf_cap - used, "  Kiln: %s", active_name);
    } else {
        snprintf(status_buf + used, status_buf_cap - used, "  Kiln: (none)");
    }
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

void ui_page_home_rail_format_zone_temp(bool valid, float temp_c, char *out, size_t out_cap)
{
    if (!valid || isnan(temp_c)) {
        snprintf(out, out_cap, "--.-");
        return;
    }
    snprintf(out, out_cap, "%.1f", (double)temp_c);
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
