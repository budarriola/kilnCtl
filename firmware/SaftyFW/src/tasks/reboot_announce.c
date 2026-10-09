#include "reboot_announce.h"

static bool     s_known  = false;
static uint32_t s_at_ms  = 0;

void reboot_announce_mark(uint32_t now_ms)
{
    s_at_ms = now_ms;
    s_known = true;
}

bool reboot_announce_get(uint32_t *out_at_ms)
{
    if (!s_known) {
        return false;
    }
    if (out_at_ms) {
        *out_at_ms = s_at_ms;
    }
    return true;
}

void reboot_announce_reset_for_test(void)
{
    s_known = false;
    s_at_ms = 0;
}
