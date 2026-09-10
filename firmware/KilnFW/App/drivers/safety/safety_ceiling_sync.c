#include "safety_ceiling_sync.h"

#include <string.h>

#include "esp_log.h"

#include "MAX31856.h"
#include "hal_time.h" /* hal_time_now_us() -- HAL_INCLUDE_BOUNDARY: this file must not include esp_timer.h directly */
#include "safety_cfg_http.h"
#include "safety_cfg_store.h"
#include "zones_config_accessors.h"

static const char *TAG = "safety_ceiling_sync";

/* 2026-09-10 opus review finding A -- see safety_ceiling_policy.h's own doc
 * comment on safety_ceiling_reconcile_backoff_t for the full rationale. One
 * instance is correct here: this file is the single ESP-side reconcile
 * caller (safety_link_poll.c's safety_update_health() is the only caller of
 * safety_ceiling_sync_reconcile_on_link_up(), and there is exactly one
 * safety link / one Pico this boot). */
static safety_ceiling_reconcile_backoff_t s_reconcile_backoff = { 0 };

/* Rate-limits the WARN log below, separately from the write-retry backoff
 * itself -- same "never silent, but never a line every attempt" discipline
 * as SAFETY_CFG_STORE_REFETCH_LOG_INTERVAL_US (safety_cfg_store.c). Kept
 * independent of the retry backoff on purpose: a future tuning pass could
 * shorten the retry backoff without also having to re-derive a sane log
 * cadence. */
#define SAFETY_CEILING_SYNC_LOG_INTERVAL_US ((int64_t)30 * 1000 * 1000)
static int64_t s_last_log_us = -SAFETY_CEILING_SYNC_LOG_INTERVAL_US; /* so the very first failure logs immediately */
static uint32_t s_suppressed_log_count = 0;

/* The writer callback safety_ceiling_policy.c calls. `ctx` is the
 * SafetyLinkClass* to write through (may be NULL -- handled below, callers
 * of THIS file never reach a NULL-link writer call because both entry
 * points short-circuit on `!link` first). Reuses safety_cfg_http.c's
 * apply_pairs()/confirm_commit_landed() machinery via its public wrapper --
 * stage, commit, and a live read-back that proves the value landed, never
 * a bare ACK. */
static bool pico_ceiling_writer(void *ctx, float target_c, char *reason_out, size_t reason_cap,
                                 safety_ceiling_refusal_class_t *out_class)
{
    SafetyLinkClass *link = (SafetyLinkClass *)ctx;
    return safety_cfg_http_set_and_confirm_f32(link, SAFETY_PARAM_ID_ABS_MAX_TEMP_C, target_c, reason_out,
                                                reason_cap, out_class);
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
                                      size_t reason_cap, safety_ceiling_refusal_class_t *out_refusal_class)
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
                                              out_result, reason_out, reason_cap, out_refusal_class);
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

    /* 2026-09-10 opus review finding: the comment this replaces claimed
     * `now_us` was made static (file-scope storage, not a stack local) as a
     * stack-budget measure for safety_poll_task -- that claim was false.
     * This same function frame already carries `float new_max_temp_c[3]`
     * and `char reason[128]` (both genuine stack locals, ~140 bytes) a few
     * lines below, so moving 8 bytes of `int64_t` off the stack saves
     * nothing meaningful; safety_poll_task in fact has 4788 of 8192 bytes
     * free. `now_us` stays static anyway (harmless, not a fix for anything)
     * purely for consistency with s_last_log_us/s_suppressed_log_count
     * below, which for the same reason as always -- rate-limiting a WARN
     * log across calls -- genuinely must persist between calls; this
     * function is only ever called from safety_poll_task, never
     * concurrently, so there is no reentrancy hazard in reusing one
     * instance. Do not cite this comment as a stack-budget justification
     * for anything -- it is not one. */
    static int64_t now_us;
    now_us = (int64_t)hal_time_now_us();
    if (!safety_ceiling_reconcile_should_attempt(&s_reconcile_backoff, now_us)) {
        /* Backing off from a prior failed raise (most commonly the Pico
         * reporting ARMED, its ordinary standing state) -- skip entirely,
         * no UART round trip, no log line. See safety_ceiling_policy.h's
         * safety_ceiling_reconcile_backoff_t comment for the full rationale
         * and the window this leaves open. */
        return;
    }

    float new_max_temp_c[MAX31856_CHANNEL_COUNT];
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        float cur_max = 0.0f, cur_min = 0.0f;
        zones_config_get_temp_limits(zi, &cur_max, &cur_min);
        new_max_temp_c[zi] = cur_max;
    }
    safety_ceiling_sync_result_t result = SAFETY_CEILING_SYNC_NONE;
    char reason[128] = { 0 };
    safety_ceiling_refusal_class_t refusal_class = SAFETY_CEILING_REFUSAL_NONE;
    bool ok = safety_ceiling_sync_guard_raise(link, new_max_temp_c, MAX31856_CHANNEL_COUNT, &result, reason,
                                              sizeof(reason), &refusal_class);
    /* 2026-09-10 opus review finding: the backoff decision now runs on the
     * numeric `refusal_class` above, never on `reason`'s prose -- `reason`
     * is kept purely for the human-readable log line below. */
    safety_ceiling_reconcile_record_result(&s_reconcile_backoff, now_us, ok, refusal_class);
    if (!ok) {
        if (now_us - s_last_log_us >= SAFETY_CEILING_SYNC_LOG_INTERVAL_US) {
            ESP_LOGW(TAG,
                     "link-up reconcile: could not raise/confirm the safety processor's ceiling to match the "
                     "ESP's zone config -- %s -- backing off %lld s before retrying%s",
                     reason, (long long)(s_reconcile_backoff.backoff_until_us - now_us) / 1000000,
                     s_suppressed_log_count > 0 ? " (repeat warnings suppressed since the last one)" : "");
            s_last_log_us = now_us;
            s_suppressed_log_count = 0;
        } else {
            s_suppressed_log_count++;
        }
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
