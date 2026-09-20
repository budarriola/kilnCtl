/* profile_executor_live_pickup.c -- see the header for the design. */

#include "profile_executor_live_pickup.h"

#include <string.h>

#include "live_profile.h"

profile_live_pickup_result_t profile_executor_live_pickup_check(
    const profile_t *running, const profile_t *candidate, uint8_t segment_index,
    bool (*validate_hard)(void *ctx, const profile_t *candidate, char *err_msg, size_t err_cap), void *validate_ctx,
    char *err_msg, size_t err_cap)
{
    if (live_edit_check_window(running, candidate, segment_index, err_msg, err_cap)) {
        return PROFILE_LIVE_PICKUP_REFUSED_WINDOW;
    }

    if (validate_hard && !validate_hard(validate_ctx, candidate, err_msg, err_cap)) {
        return PROFILE_LIVE_PICKUP_REFUSED_INVALID;
    }

    return PROFILE_LIVE_PICKUP_OK;
}

bool profile_live_pickup_should_advance_generation(profile_live_pickup_poll_outcome_kind_t kind,
                                                    profile_live_pickup_result_t result)
{
    (void)result; /* every CHECKED result (OK or a REFUSED_*) advances alike -- see header comment */
    return kind == PROFILE_LIVE_PICKUP_POLL_CHECKED;
}
