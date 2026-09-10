#include "safety_ceiling_policy.h"

#include <stdio.h>
#include <string.h>

/* Small epsilon for float compares -- these values arrive via a %.9g wire
 * round trip (safety_cfg_http.c's existing convention for every f32 param),
 * so an exact `==` between "what we computed" and "what came back" is
 * appropriate at the confirm layer (param_value_equal() there does a bit-
 * for-bit memcmp on purpose) but NOT appropriate here, where we are
 * comparing a value we just computed against one that arrived from a
 * completely separate computation (or a live fetch) that may carry
 * ordinary float rounding noise. 0.01 C is far below anything a real
 * thermocouple channel or this UI resolves. */
#define SAFETY_CEILING_EPSILON_C 0.01f

float safety_ceiling_policy_target_c(const float *max_temp_c, size_t n)
{
    float best = 0.0f;
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        float v = max_temp_c[i];
        /* Not merely "not NaN": a zero or negative ceiling is the
         * "this zone cannot fire" sentinel (see this header's own comment)
         * and must never participate in the maximum. */
        if (!(v > 0.0f)) {
            continue;
        }
        if (!any || v > best) {
            best = v;
            any = true;
        }
    }
    if (!any) {
        return 0.0f;
    }
    return best + SAFETY_CEILING_HEADROOM_C;
}

bool safety_ceiling_policy_guard_raise(float current_pico_ceiling_c, bool current_known,
                                        const float *new_max_temp_c, size_t new_n,
                                        safety_ceiling_writer_fn writer, void *writer_ctx,
                                        safety_ceiling_sync_result_t *out_result, char *reason_out,
                                        size_t reason_cap)
{
    safety_ceiling_sync_result_t local_result = SAFETY_CEILING_SYNC_NONE;
    if (reason_out && reason_cap > 0) {
        reason_out[0] = '\0';
    }

    float new_target = safety_ceiling_policy_target_c(new_max_temp_c, new_n);
    if (new_target <= 0.0f) {
        /* No zone has a real ceiling in the proposed config -- nothing to
         * derive a Pico target from. Do NOT invent a default (this
         * header's own rule); leave the Pico's current value exactly as
         * it is and let the commit proceed. */
        if (out_result) {
            *out_result = local_result;
        }
        return true;
    }

    /* current_known == false is treated as "assume the worst" -- i.e. as
     * if the current ceiling were 0, which always forces the raise path
     * below rather than risk skipping a needed one. */
    float effective_current = current_known ? current_pico_ceiling_c : 0.0f;

    if (new_target <= effective_current + SAFETY_CEILING_EPSILON_C) {
        /* Already satisfied (or a lowering case, handled separately,
         * post-commit, by safety_ceiling_policy_apply_lower()). */
        if (out_result) {
            *out_result = local_result;
        }
        return true;
    }

    /* A raise is needed. The Pico MUST confirm this BEFORE the caller is
     * allowed to commit the new zone config -- see this header's top
     * comment for why the order matters. */
    bool ok = writer && writer(writer_ctx, new_target, reason_out, reason_cap);
    if (!ok) {
        if (reason_out && reason_cap > 0 && reason_out[0] == '\0') {
            snprintf(reason_out, reason_cap, "no writer available to raise the safety processor's ceiling");
        }
        if (out_result) {
            *out_result = SAFETY_CEILING_SYNC_RAISE_FAILED;
        }
        return false;
    }
    if (out_result) {
        *out_result = SAFETY_CEILING_SYNC_RAISED;
    }
    return true;
}

void safety_ceiling_policy_apply_lower(float current_pico_ceiling_c, bool current_known,
                                        const float *new_max_temp_c, size_t new_n,
                                        safety_ceiling_writer_fn writer, void *writer_ctx,
                                        safety_ceiling_sync_result_t *out_result, char *reason_out,
                                        size_t reason_cap)
{
    safety_ceiling_sync_result_t local_result = SAFETY_CEILING_SYNC_NONE;
    if (reason_out && reason_cap > 0) {
        reason_out[0] = '\0';
    }

    float new_target = safety_ceiling_policy_target_c(new_max_temp_c, new_n);
    if (new_target <= 0.0f) {
        /* No zone has a real ceiling -- never touch the Pico's ceiling on
         * behalf of an all-zero config (same "do not invent a default"
         * rule as the raise path). */
        if (out_result) {
            *out_result = local_result;
        }
        return;
    }

    if (!current_known) {
        /* Unknown current value: nothing SAFE to lower toward without
         * first knowing what it is -- skip rather than guess. This
         * differs from the raise path's "assume the worst" deliberately:
         * an unknown current value only ever makes a RAISE more urgent to
         * attempt (safe direction to guess), never a LOWER (guessing wrong
         * there could write a ceiling lower than the Pico's real one,
         * which is the one thing this whole feature must never do). */
        if (out_result) {
            *out_result = local_result;
        }
        return;
    }

    if (new_target >= current_pico_ceiling_c - SAFETY_CEILING_EPSILON_C) {
        /* Not a lowering case (equal, or actually a raise that the caller
         * should have already handled via guard_raise() before commit). */
        if (out_result) {
            *out_result = local_result;
        }
        return;
    }

    bool ok = writer && writer(writer_ctx, new_target, reason_out, reason_cap);
    if (!ok) {
        if (reason_out && reason_cap > 0 && reason_out[0] == '\0') {
            snprintf(reason_out, reason_cap, "no writer available to lower the safety processor's ceiling");
        }
        if (out_result) {
            *out_result = SAFETY_CEILING_SYNC_LOWER_FAILED;
        }
        return;
    }
    if (out_result) {
        *out_result = SAFETY_CEILING_SYNC_LOWERED;
    }
}
