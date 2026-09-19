#include "ui_page_temperature_safety.h"

#include <stdio.h>

void ui_page_temperature_safety_text(bool known, bool energized, char *out, size_t out_cap)
{
    if (!known) {
        snprintf(out, out_cap, "Safety (K4): n/a");
    } else if (energized) {
        snprintf(out, out_cap, "Safety (K4): ON");
    } else {
        snprintf(out, out_cap, "Safety (K4): off");
    }
}
