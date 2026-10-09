#include "ui_page_safety_logic.h"

#include <stdio.h>

#include "../safety/safety_link.h" // SAFETY_LINK_DIAG_STATE_TRIPPED, SAFETY_LINK_STALE_MS

ui_safety_view_t ui_safety_view_derive(bool diag_ever_received, uint8_t diag_state, uint32_t diag_age_ms)
{
    ui_safety_view_t v = { .tripped_live = false, .diag_unknown = true };
    if (!diag_ever_received || diag_age_ms >= SAFETY_LINK_STALE_MS) {
        return v;
    }
    v.diag_unknown = false;
    v.tripped_live = (diag_state == SAFETY_LINK_DIAG_STATE_TRIPPED);
    return v;
}

ui_safety_clear_verdict_t ui_safety_clear_verdict(const ui_safety_view_t *view, bool has_admin)
{
    if (view == NULL || !view->tripped_live) {
        return UI_SAFETY_CLEAR_NOT_TRIPPED;
    }
    if (!has_admin) {
        return UI_SAFETY_CLEAR_NEEDS_ADMIN;
    }
    return UI_SAFETY_CLEAR_SEND;
}

void ui_safety_format_age(uint32_t age_ms, char *buf, unsigned long buf_len)
{
    uint32_t s = age_ms / 1000u;
    if (s < 60u) {
        snprintf(buf, buf_len, "%lus ago", (unsigned long)s);
    } else if (s < 3600u) {
        snprintf(buf, buf_len, "%lum%02lus ago", (unsigned long)(s / 60u), (unsigned long)(s % 60u));
    } else {
        snprintf(buf, buf_len, "%luh%02lum ago", (unsigned long)(s / 3600u), (unsigned long)((s % 3600u) / 60u));
    }
}
