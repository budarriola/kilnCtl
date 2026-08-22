#include "rules_eval.h"

#include <stddef.h>

bool rules_eval_condition(const rule_condition_t *cond, const rules_eval_inputs_t *in)
{
    if (cond == NULL || in == NULL) {
        return false;
    }

    switch (cond->type) {
    case COND_TEMP: {
        if (cond->zone_index >= RULES_EVAL_ZONE_COUNT) {
            return false; /* out of range -- rules_http.c's own validation should
                           * already reject this at save time; defensive only */
        }
        if (!in->zone_temp_valid[cond->zone_index]) {
            /* A fault/no-reading zone never satisfies a TEMP condition,
             * whether the comparator is GE or LE: for GE, an unknown
             * temperature must never be treated as "at or above threshold"
             * (that would be the direction that turns heat ON on a fault);
             * for LE, this firmware likewise cannot PROVE the real value is
             * at or below threshold from missing data, so failing safe here
             * too keeps the rule from firing on either comparator rather
             * than only the heat-causing one. */
            return false;
        }
        float t = in->zone_temp_c[cond->zone_index];
        return cond->cmp == CMP_GE ? (t >= cond->threshold_c) : (t <= cond->threshold_c);
    }
    case COND_TIME: {
        if (!in->profile_running) {
            /* No profile running -- "elapsed since profile start" has no
             * meaning. Failing safe (false) rather than treating it as 0
             * elapsed (which would make "TIME LE 0" spuriously true with no
             * profile running at all). TIME conditions only ever fire
             * during a running profile -- documented behavior, not a bug. */
            return false;
        }
        return cond->cmp == CMP_GE ? (in->profile_elapsed_s >= cond->seconds)
                                    : (in->profile_elapsed_s <= cond->seconds);
    }
    case COND_RELAY: {
        if (cond->other_relay < 1 || cond->other_relay > RULES_EVAL_RELAY_COUNT) {
            return false; /* out of range -- defensive only, see COND_TEMP's comment */
        }
        bool on = in->relay_commanded_on[cond->other_relay - 1];
        return on == cond->other_relay_state;
    }
    case COND_NONE:
    default:
        return false;
    }
}

bool rules_eval_relay_wants_on(const relay_rules_cfg_t *cfg, const rules_eval_inputs_t *in)
{
    if (cfg == NULL || in == NULL || !cfg->rule_driven) {
        return false;
    }

    for (uint8_t k = 0; k < RULES_MAX_RULES_PER_RELAY; k++) {
        const rule_t *rule = &cfg->rules[k];
        if (rule->condition_count == 0) {
            continue; /* unused slot -- never fires (see header comment) */
        }
        bool all_true = true;
        for (uint8_t c = 0; c < rule->condition_count; c++) {
            if (!rules_eval_condition(&rule->conditions[c], in)) {
                all_true = false;
                break;
            }
        }
        if (all_true) {
            return true; /* OR across rules -- this one alone is enough */
        }
    }
    return false;
}

bool rules_eval_decide(const relay_rules_cfg_t *cfg, const rules_eval_inputs_t *in,
                        bool safety_link_ok, bool heat_interlock_ok)
{
    if (!rules_eval_relay_wants_on(cfg, in)) {
        return false; /* OFF is never gated -- if rules don't want it on, done */
    }
    return safety_link_ok && heat_interlock_ok;
}
