#include "pico_auto_update_state.h"

#include <stdbool.h>
#include <string.h>

/* See the header for the (deliberate, single-writer) concurrency argument.
 * `volatile` on the flag so a reader on another core cannot be handed a
 * cached copy indefinitely; the reason buffer is written before the flag is
 * raised and never cleared while the flag is up. */
static volatile bool s_blocking;
static char s_reason[PICO_AUTO_UPDATE_STATE_REASON_MAX];

/* Same single-writer-per-boot convention as s_blocking/s_reason above, for
 * the separate non-blocking budget-spent warning (review finding F2). */
static volatile bool s_warning;
static char s_warning_reason[PICO_AUTO_UPDATE_STATE_REASON_MAX];

/* Bug found 2026-09-21: see the header comment on
 * pico_auto_update_state_set_last_decision() for why this exists -- the
 * readiness page must not claim a match by default. */
static volatile bool s_decision_is_match;
static char s_last_decision[PICO_AUTO_UPDATE_STATE_REASON_MAX];

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

bool pico_auto_update_state_is_warning(void)
{
    return s_warning;
}

const char *pico_auto_update_state_warning_reason(void)
{
    return s_warning_reason;
}

void pico_auto_update_state_set_warning(const char *reason)
{
    if (reason != NULL && reason[0] != '\0') {
        (void)strncpy(s_warning_reason, reason, sizeof(s_warning_reason) - 1u);
        s_warning_reason[sizeof(s_warning_reason) - 1u] = '\0';
        s_warning = true;
    } else {
        s_warning = false;
        s_warning_reason[0] = '\0';
    }
}

void pico_auto_update_state_set_last_decision(const char *reason, bool is_match)
{
    if (reason != NULL) {
        (void)strncpy(s_last_decision, reason, sizeof(s_last_decision) - 1u);
        s_last_decision[sizeof(s_last_decision) - 1u] = '\0';
    } else {
        s_last_decision[0] = '\0';
    }
    s_decision_is_match = is_match;
}

const char *pico_auto_update_state_last_decision(void)
{
    return s_last_decision;
}

bool pico_auto_update_state_decision_is_match(void)
{
    return s_decision_is_match;
}
