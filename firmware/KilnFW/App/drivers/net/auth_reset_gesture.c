// auth_reset_gesture.c -- see auth_reset_gesture.h for the design rationale.
//
// Deliberately has NO dependency on web_auth_store.h or any other I/O
// module (see the header's "pure, host-testable" framing) -- the singleton
// that wires clear_credentials_fn to the real credential store lives in
// auth_reset_gesture_wiring.c instead, its own translation unit, so this
// file stays linkable into every host-test executable (including the
// combined one) without pulling in psa/crypto.h's host stub.
#include "auth_reset_gesture.h"

#include <stddef.h>

static const auth_reset_gesture_corner_t ORDER[AUTH_RESET_CORNER_COUNT] = {
    AUTH_RESET_CORNER_TOP_LEFT,
    AUTH_RESET_CORNER_TOP_RIGHT,
    AUTH_RESET_CORNER_BOTTOM_LEFT,
    AUTH_RESET_CORNER_BOTTOM_RIGHT,
};

void auth_reset_gesture_reset(auth_reset_gesture_state_t *s)
{
    if (!s) {
        return;
    }
    s->next_index = 0;
    s->sequence_started_ms = 0;
    s->armed = false;
    s->armed_at_ms = 0;
    /* clear_credentials_fn is deliberately left untouched -- see header. */
}

bool auth_reset_gesture_preconditions_met(bool estop_asserted, bool firing_active, bool heat_enabled)
{
    return estop_asserted && !firing_active && !heat_enabled;
}

static void abandon_sequence(auth_reset_gesture_state_t *s)
{
    s->next_index = 0;
    s->sequence_started_ms = 0;
}

auth_reset_gesture_tap_result_t auth_reset_gesture_on_corner_tap(
    auth_reset_gesture_state_t *s, auth_reset_gesture_corner_t corner, uint32_t now_ms,
    bool estop_asserted, bool firing_active, bool heat_enabled)
{
    if (!s) {
        return AUTH_RESET_TAP_IGNORED_PRECONDITIONS;
    }

    if (!auth_reset_gesture_preconditions_met(estop_asserted, firing_active, heat_enabled)) {
        /* Precondition failure mid-sequence aborts cleanly -- an operator
         * releasing E-stop, or a firing/heat state changing underneath the
         * sequence, must never leave a partially-armed gesture waiting. */
        abandon_sequence(s);
        return AUTH_RESET_TAP_IGNORED_PRECONDITIONS;
    }

    if (s->armed) {
        /* Already armed -- extra taps do not restart or extend anything.
         * Only confirm() or cancel() (or expiry) leaves the armed state. */
        return AUTH_RESET_TAP_IGNORED_ALREADY_ARMED;
    }

    /* Window check: if a sequence is in progress and has run past its 10 s
     * budget, abandon it before evaluating this tap (a stale in-progress
     * sequence must not be resurrected by a tap that happens to be correct
     * for its own position). */
    if (s->next_index > 0 &&
        (uint32_t)(now_ms - s->sequence_started_ms) > AUTH_RESET_GESTURE_SEQUENCE_WINDOW_MS) {
        abandon_sequence(s);
    }

    if (corner != ORDER[s->next_index]) {
        /* Wrong corner -- out-of-order or a stray tap. Abandon cleanly
         * rather than latching partial progress; this is what makes a
         * single stray touch with E-stop pressed harmless. */
        abandon_sequence(s);
        return AUTH_RESET_TAP_ABANDONED;
    }

    if (s->next_index == 0) {
        s->sequence_started_ms = now_ms;
    }
    s->next_index++;

    if (s->next_index >= AUTH_RESET_CORNER_COUNT) {
        s->armed = true;
        s->armed_at_ms = now_ms;
        s->next_index = 0;
        s->sequence_started_ms = 0;
        return AUTH_RESET_TAP_ARMED;
    }

    return AUTH_RESET_TAP_PROGRESS;
}

auth_reset_gesture_confirm_result_t auth_reset_gesture_confirm(auth_reset_gesture_state_t *s, uint32_t now_ms)
{
    if (!s || !s->armed) {
        return AUTH_RESET_CONFIRM_NOT_ARMED;
    }

    bool expired = (uint32_t)(now_ms - s->armed_at_ms) > AUTH_RESET_GESTURE_CONFIRM_WINDOW_MS;
    /* One deliberate confirmation -- whatever happens below, this call
     * always leaves the state idle. Retrying requires re-arming the whole
     * gesture, preconditions included. */
    s->armed = false;
    s->armed_at_ms = 0;

    if (expired) {
        return AUTH_RESET_CONFIRM_EXPIRED;
    }
    if (!s->clear_credentials_fn) {
        return AUTH_RESET_CONFIRM_NOT_WIRED;
    }
    if (!s->clear_credentials_fn()) {
        return AUTH_RESET_CONFIRM_CLEAR_FAILED;
    }
    return AUTH_RESET_CONFIRM_OK;
}

void auth_reset_gesture_cancel(auth_reset_gesture_state_t *s)
{
    if (!s) {
        return;
    }
    abandon_sequence(s);
    s->armed = false;
    s->armed_at_ms = 0;
}

bool auth_reset_gesture_is_armed_and_live(const auth_reset_gesture_state_t *s, uint32_t now_ms)
{
    if (!s || !s->armed) {
        return false;
    }
    return (uint32_t)(now_ms - s->armed_at_ms) <= AUTH_RESET_GESTURE_CONFIRM_WINDOW_MS;
}

/* auth_reset_gesture_singleton() is defined in auth_reset_gesture_wiring.c,
 * not here -- see this file's header comment. */
