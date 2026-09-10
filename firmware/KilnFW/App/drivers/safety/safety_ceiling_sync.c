#include "safety_ceiling_sync.h"

#include <string.h>

#include "esp_log.h"

#include "MAX31856.h"
#include "safety_cfg_http.h"
#include "safety_cfg_store.h"
#include "zones_config_accessors.h"

static const char *TAG = "safety_ceiling_sync";

/* The writer callback safety_ceiling_policy.c calls. `ctx` is the
 * SafetyLinkClass* to write through (may be NULL -- handled below, callers
 * of THIS file never reach a NULL-link writer call because both entry
 * points short-circuit on `!link` first). Reuses safety_cfg_http.c's
 * apply_pairs()/confirm_commit_landed() machinery via its public wrapper --
 * stage, commit, and a live read-back that proves the value landed, never
 * a bare ACK. */
static bool pico_ceiling_writer(void *ctx, float target_c, char *reason_out, size_t reason_cap)
{
    SafetyLinkClass *link = (SafetyLinkClass *)ctx;
    return safety_cfg_http_set_and_confirm_f32(link, SAFETY_PARAM_ID_ABS_MAX_TEMP_C, target_c, reason_out,
                                                reason_cap);
}

bool safety_ceiling_sync_get_current_pico_ceiling(float *out_value)
{
    size_t count = safety_cfg_store_param_count();
    for (size_t i = 0; i < count; i++) {
        safety_cfg_param_t row;
        if (!safety_cfg_store_get_by_index(i, &row)) {
            continue;
        }
        if (row.param_id == SAFETY_PARAM_ID_ABS_MAX_TEMP_C) {
            if (!row.set) {
                return false;
            }
            if (out_value) {
                *out_value = row.value.f32_val;
            }
            return true;
        }
    }
    return false;
}

bool safety_ceiling_sync_guard_raise(SafetyLinkClass *link, const float *new_max_temp_c, size_t n,
                                      safety_ceiling_sync_result_t *out_result, char *reason_out,
                                      size_t reason_cap)
{
    if (!link) {
        if (out_result) {
            *out_result = SAFETY_CEILING_SYNC_NONE;
        }
        if (reason_out && reason_cap > 0) {
            reason_out[0] = '\0';
        }
        return true; /* no safety processor this boot -- nothing to guard */
    }
    float current = 0.0f;
    bool known = safety_ceiling_sync_get_current_pico_ceiling(&current);
    return safety_ceiling_policy_guard_raise(current, known, new_max_temp_c, n, pico_ceiling_writer, link,
                                              out_result, reason_out, reason_cap);
}

void safety_ceiling_sync_apply_lower(SafetyLinkClass *link, const float *new_max_temp_c, size_t n,
                                      safety_ceiling_sync_result_t *out_result, char *reason_out,
                                      size_t reason_cap)
{
    if (!link) {
        if (out_result) {
            *out_result = SAFETY_CEILING_SYNC_NONE;
        }
        if (reason_out && reason_cap > 0) {
            reason_out[0] = '\0';
        }
        return;
    }
    float current = 0.0f;
    bool known = safety_ceiling_sync_get_current_pico_ceiling(&current);
    safety_ceiling_policy_apply_lower(current, known, new_max_temp_c, n, pico_ceiling_writer, link, out_result,
                                       reason_out, reason_cap);
}

void safety_ceiling_sync_reconcile_on_link_up(SafetyLinkClass *link)
{
    if (!link) {
        return; /* nothing to reconcile against */
    }
    if (!zones_config_is_valid()) {
        return; /* same gate safety_sync_tc_type() uses -- no real config to derive a target from yet */
    }
    float new_max_temp_c[MAX31856_CHANNEL_COUNT];
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        float cur_max = 0.0f, cur_min = 0.0f;
        zones_config_get_temp_limits(zi, &cur_max, &cur_min);
        new_max_temp_c[zi] = cur_max;
    }
    safety_ceiling_sync_result_t result = SAFETY_CEILING_SYNC_NONE;
    char reason[128];
    bool ok = safety_ceiling_sync_guard_raise(link, new_max_temp_c, MAX31856_CHANNEL_COUNT, &result, reason,
                                              sizeof(reason));
    if (!ok) {
        ESP_LOGW(TAG,
                 "link-up reconcile: could not raise/confirm the safety processor's ceiling to match the ESP's "
                 "zone config -- %s -- will retry on the next link-up transition or the next zones POST",
                 reason);
        return;
    }
    if (result == SAFETY_CEILING_SYNC_RAISED) {
        ESP_LOGI(TAG,
                 "link-up reconcile: safety processor's ceiling was behind the ESP's zone config -- raised and "
                 "confirmed");
    }
    /* SAFETY_CEILING_SYNC_NONE: already wide enough (the ordinary case on a
     * healthy reconnect where nothing changed while the link was down) --
     * nothing worth logging. */
}
