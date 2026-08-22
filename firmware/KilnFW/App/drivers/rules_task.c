#include "rules_task.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "MAX31856.h"
#include "heat_interlock.h"
#include "kiln_io.h"
#include "kiln_io_owner.h"
#include "ota_http.h"
#include "profile_executor.h"
#include "relay_authority.h"
#include "rules_eval.h"
#include "rules_http.h"

static const char *TAG = "rules_task";

/* 1Hz, same cadence as profile_executor.c's control tick -- TEMP thresholds
 * do not need faster, and TIME conditions are specified in whole seconds. */
#define RULES_TASK_TICK_MS 1000u

_Static_assert(RULES_EVAL_RELAY_COUNT == KILN_IO_RELAY_COUNT,
               "rules_types.h's RULES_EVAL_RELAY_COUNT drifted from kiln_io.h's KILN_IO_RELAY_COUNT");
_Static_assert(RULES_EVAL_ZONE_COUNT == MAX31856_CHANNEL_COUNT,
               "rules_types.h's RULES_EVAL_ZONE_COUNT drifted from MAX31856.h's MAX31856_CHANNEL_COUNT");

static struct {
    SafetyLinkClass *safety; /* NULL if not wired -- every ON decision fails closed, see rules_task.h */
    TaskHandle_t task;

    /* Bits this task itself currently holds as RELAY_OWNER_RULE -- only
     * these bits are ever released by this task (see sync_ownership()'s
     * doc comment: never release a relay this task didn't claim). */
    uint8_t claimed_mask;

    /* Published status, read by rules_http.c's GET /api/rules/status.
     * Written only by the task below; read-only snapshot copy handed out
     * by rules_task_get_status() -- no lock, same tolerance as
     * rules_http_get_cfg() (worst case: one stale/torn read, self-corrects
     * next tick, matches this codebase's existing risk posture for
     * dashboard-poll-only state). */
    rules_task_status_t status;
} s_rules_task;

/* Only ever touches bits THIS task previously claimed (s_rules_task.claimed_
 * mask) when releasing, and only claims a relay that is currently unowned
 * or already ours -- a relay a running profile or autotune test owns first
 * is left alone and logged, not stolen. See rules_task.h's top comment for
 * the full ownership-precedence writeup this implements. */
static void sync_ownership(uint8_t want_rule_driven_mask)
{
    uint8_t to_release = (uint8_t)(s_rules_task.claimed_mask & (uint8_t)~want_rule_driven_mask);
    if (to_release) {
        relay_authority_release_mask(to_release);
        s_rules_task.claimed_mask = (uint8_t)(s_rules_task.claimed_mask & (uint8_t)~to_release);
    }

    uint8_t to_claim = (uint8_t)(want_rule_driven_mask & (uint8_t)~s_rules_task.claimed_mask);
    for (uint8_t ri = 1; ri <= RULES_EVAL_RELAY_COUNT; ri++) {
        uint8_t bit = (uint8_t)(1u << (ri - 1u));
        if ((to_claim & bit) == 0) {
            continue;
        }
        relay_owner_t owner = relay_authority_get_owner(ri);
        if (owner == RELAY_OWNER_NONE || owner == RELAY_OWNER_RULE) {
            relay_authority_claim_mask(bit, RELAY_OWNER_RULE);
            s_rules_task.claimed_mask |= bit;
        } else {
            ESP_LOGW(TAG,
                     "relay %u marked rule_driven but already owned by owner=%d -- rule engine will "
                     "not drive it until that owner releases it",
                     (unsigned)ri, (int)owner);
        }
    }
}

static void gather_inputs(rules_eval_inputs_t *in, bool *out_relay_commanded_on /* [RULES_EVAL_RELAY_COUNT] */)
{
    memset(in, 0, sizeof(*in));

    profile_exec_status_t exec;
    profile_executor_get_status(&exec);
    in->profile_running = (exec.state == PROFILE_EXEC_RUNNING);
    in->profile_elapsed_s = exec.total_elapsed_s;

    for (uint8_t zi = 0; zi < RULES_EVAL_ZONE_COUNT; zi++) {
        in->zone_temp_valid[zi] = exec.zones[zi].active && exec.zones[zi].actual_valid;
        in->zone_temp_c[zi] = exec.zones[zi].actual_c;
    }

    /* Current commanded relay state, for COND_RELAY conditions. A read
     * failure (owner task down/timeout) leaves relay_shadow at 0 (zeroed
     * above) -- COND_RELAY conditions then read every relay as OFF, which
     * cannot cause a false "other relay is on" match that would help turn
     * something ON; the worst case is a rule that should fire (because the
     * other relay really is on) failing to, which is the fail-safe
     * direction. */
    kiln_io_state_t state;
    memset(&state, 0, sizeof(state));
    esp_err_t err = kiln_io_owner_command_read(&state);
    uint8_t relay_shadow = (err == ESP_OK) ? state.relay_shadow : 0;
    for (uint8_t ri = 1; ri <= RULES_EVAL_RELAY_COUNT; ri++) {
        bool on = (relay_shadow & (1u << (ri - 1u))) != 0;
        in->relay_commanded_on[ri - 1] = on;
        if (out_relay_commanded_on) {
            out_relay_commanded_on[ri - 1] = on;
        }
    }
}

static void rules_task_entry(void *arg)
{
    (void)arg;

    for (;;) {
        rules_cfg_t cfg;
        rules_http_get_cfg(&cfg);

        uint8_t want_rule_driven_mask = 0;
        for (uint8_t ri = 1; ri <= RULES_EVAL_RELAY_COUNT; ri++) {
            if (cfg.relays[ri - 1].rule_driven) {
                want_rule_driven_mask |= (uint8_t)(1u << (ri - 1u));
            }
        }
        sync_ownership(want_rule_driven_mask);

        rules_eval_inputs_t in;
        bool relay_commanded_on[RULES_EVAL_RELAY_COUNT];
        gather_inputs(&in, relay_commanded_on);

        /* Both fail-safe gates, sampled once per tick, same functions
         * kiln_io_owner.c's own relay_on_blocked() combines -- see this
         * file's header comment for why this task must apply them itself
         * before calling the AUTHORIZED producer below. */
        bool safety_link_ok = !relay_authority_on_blocked(s_rules_task.safety, NULL);
        char interlock_reason[HEAT_INTERLOCK_REASON_MAX];
        bool heat_interlock_ok = !ota_http_heat_blocked_by_update(interlock_reason, sizeof(interlock_reason));

        uint8_t want_on_mask = 0;
        uint8_t decided_on_mask = 0;
        for (uint8_t ri = 1; ri <= RULES_EVAL_RELAY_COUNT; ri++) {
            uint8_t bit = (uint8_t)(1u << (ri - 1u));
            const relay_rules_cfg_t *rc = &cfg.relays[ri - 1];

            bool wants_on = rules_eval_relay_wants_on(rc, &in);
            bool decided_on = rules_eval_decide(rc, &in, safety_link_ok, heat_interlock_ok);

            if (wants_on) {
                want_on_mask |= bit;
            }

            /* Only this task's own RULE-owned relays are ever commanded --
             * a relay marked rule_driven but refused ownership by
             * sync_ownership() above (owned by PROFILE/AUTOTUNE) is left
             * strictly alone: not commanded on, not forced off, since this
             * task has no authority over it at all. */
            bool ours = (s_rules_task.claimed_mask & bit) != 0;
            if (ours && decided_on) {
                decided_on_mask |= bit;
            }

            s_rules_task.status.relays[ri - 1].rule_driven = rc->rule_driven;
            s_rules_task.status.relays[ri - 1].rule_wants_on = wants_on;
            s_rules_task.status.relays[ri - 1].owned_by_rules = ours;
            s_rules_task.status.relays[ri - 1].commanded_on = ours && decided_on;
        }
        s_rules_task.status.safety_link_ok = safety_link_ok;
        s_rules_task.status.heat_interlock_ok = heat_interlock_ok;

        if (s_rules_task.claimed_mask != 0) {
            /* AUTHORIZED producer -- this task already applied its own
             * ownership + safety-fault + OTA-interlock checks above, same
             * justification profile_executor.c's apply_relay() gives for
             * using this entry point instead of the MANUAL one. Only the
             * bits this task owns are ever named in the mask/value pair, so
             * a relay owned by someone else is untouched by this call. */
            esp_err_t err = kiln_io_owner_command_set_relay_mask_authorized(s_rules_task.claimed_mask,
                                                                             decided_on_mask);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "kiln_io_owner_command_set_relay_mask_authorized failed: %s",
                         esp_err_to_name(err));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(RULES_TASK_TICK_MS));
    }
}

esp_err_t rules_task_start(SafetyLinkClass *safety)
{
    memset(&s_rules_task, 0, sizeof(s_rules_task));
    s_rules_task.safety = safety;

    BaseType_t ok = xTaskCreatePinnedToCore(rules_task_entry, "rules_task", 3072, NULL, 4,
                                            &s_rules_task.task, tskNO_AFFINITY);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore(rules_task) failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "rule evaluator task up (safety=%p)", (void *)safety);
    return ESP_OK;
}

void rules_task_get_status(rules_task_status_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_rules_task.status;
}
