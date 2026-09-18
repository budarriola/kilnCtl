#include "pico_auto_update_state.h"

#include <stdbool.h>
#include <string.h>

/* See the header for the (deliberate, single-writer) concurrency argument.
 * `volatile` on the flag so a reader on another core cannot be handed a
 * cached copy indefinitely; the reason buffer is written before the flag is
 * raised and never cleared while the flag is up. */
static volatile bool s_blocking;
static char s_reason[PICO_AUTO_UPDATE_STATE_REASON_MAX];

bool pico_auto_update_state_is_blocking(void)
{
    return s_blocking;
}

const char *pico_auto_update_state_reason(void)
{
    return s_reason;
}

void pico_auto_update_state_set_blocking(bool blocking, const char *reason)
{
    if (blocking) {
        if (reason != NULL) {
            (void)strncpy(s_reason, reason, sizeof(s_reason) - 1u);
            s_reason[sizeof(s_reason) - 1u] = '\0';
        } else {
            s_reason[0] = '\0';
        }
        s_blocking = true;
    } else {
        s_blocking = false;
        s_reason[0] = '\0';
    }
}
