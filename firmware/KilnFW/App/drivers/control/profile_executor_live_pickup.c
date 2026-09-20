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
    return kind == PROFILE_LIVE_PICKUP_POLL_CHECKED || kind == PROFILE_LIVE_PICKUP_POLL_NOT_APPLICABLE ||
           kind == PROFILE_LIVE_PICKUP_POLL_LOAD_PERMANENT;
}

uint8_t profile_live_pickup_io_seg_rederive_count(uint8_t old_segment_count, uint8_t new_segment_count,
                                                   uint8_t max_segments)
{
    uint8_t n = old_segment_count;
    if (new_segment_count < n) {
        n = new_segment_count;
    }
    if (n > max_segments) {
        n = max_segments;
    }
    return n;
}

float profile_live_pickup_rederive_remaining_s(uint32_t old_dwell_min, float old_remaining_s, uint32_t new_dwell_min)
{
    float old_dwell_s = (float)(old_dwell_min * 60u);
    float elapsed_s = old_dwell_s - old_remaining_s;
    if (elapsed_s < 0.0f) {
        elapsed_s = 0.0f;
    }
    float new_dwell_s = (float)(new_dwell_min * 60u);
    float new_remaining_s = new_dwell_s - elapsed_s;
    if (new_remaining_s < 0.0f) {
        new_remaining_s = 0.0f;
    }
    return new_remaining_s;
}
