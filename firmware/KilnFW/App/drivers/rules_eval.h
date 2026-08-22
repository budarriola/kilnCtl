// rules_eval -- the pure, host-testable half of the rule evaluator (TODO.md
// section 6's "the profile executor / control tick is where [rules] would
// actually run" gap; rules_http.c stores rules_cfg_t, this decides what it
// MEANS against live inputs). Same pure/impure split heat_interlock.h
// documents: no I/O, no ESP-IDF, no FreeRTOS -- just "given this config and
// this snapshot of the world, what should each rule-driven relay be doing
// right now." The impure half (sampling live temperatures/profile state/
// relay states, applying the result through kiln_io_owner, and claiming
// RELAY_OWNER_RULE) is rules_task.c/.h.
#ifndef RULES_EVAL_H
#define RULES_EVAL_H

#include <stdbool.h>
#include <stdint.h>

#include "rules_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One tick's worth of live inputs the evaluator needs. All arrays are
 * 0-based and sized to RULES_EVAL_ZONE_COUNT/RULES_EVAL_RELAY_COUNT. */
typedef struct {
    float zone_temp_c[RULES_EVAL_ZONE_COUNT];
    bool  zone_temp_valid[RULES_EVAL_ZONE_COUNT];   /* false = fault/no reading yet --
                                                      * see rules_eval_condition()'s TEMP
                                                      * case for why this fails a
                                                      * condition either direction (GE
                                                      * or LE), never satisfies it */
    bool     profile_running;    /* a profile is RUNNING (not PAUSED/IDLE/DONE/FAULTED) */
    uint32_t profile_elapsed_s;  /* profile_exec_status_t.total_elapsed_s; meaningless,
                                   * and not read, unless profile_running */
    bool  relay_commanded_on[RULES_EVAL_RELAY_COUNT]; /* last-known commanded state of
                                                        * EVERY relay (not just rule-driven
                                                        * ones) -- for COND_RELAY conditions
                                                        * that reference another relay */
} rules_eval_inputs_t;

/* Evaluates ONE condition against `in`. Pure, no I/O. Exposed mainly so
 * host tests can pin down each condition type/comparator/fail-safe case in
 * isolation; rules_eval_decide() is the entry point a real caller should
 * drive a relay from. A condition with an out-of-range zone_index/
 * other_relay, or type COND_NONE, is treated as never-satisfied (false) --
 * this only happens for a slot rules_http.c's own validation should already
 * have rejected at save time, so this is a defensive fallback, not an
 * expected path. */
bool rules_eval_condition(const rule_condition_t *cond, const rules_eval_inputs_t *in);

/* True if this relay's rules fire this tick: OR across up to
 * RULES_MAX_RULES_PER_RELAY rules, AND across each rule's conditions.
 * Returns false immediately, with no conditions evaluated, if
 * !cfg->rule_driven -- an unconfigured/disabled relay never "wants on" from
 * rules regardless of what stale rule slots it still holds (this is the
 * behavior the Relay & Rules page's former disclaimer was warning was NOT
 * true yet). A rule with condition_count == 0 is an unused slot and never
 * fires -- an empty AND being vacuously true would turn a relay on with
 * nothing actually configured, the opposite of the intended default. */
bool rules_eval_relay_wants_on(const relay_rules_cfg_t *cfg, const rules_eval_inputs_t *in);

/* The final answer for whether a rule-driven relay may be commanded ON this
 * tick, folding in the two hard safety gates the caller (rules_task.c) is
 * responsible for sampling before calling this: is the safety link/relay
 * authority currently clear (relay_authority_on_blocked()), and is the
 * ESP/Pico OTA heat interlock clear (ota_http_heat_blocked_by_update()).
 * Either one being false forces the answer to false regardless of what the
 * rules say -- fail toward OFF, never toward "whatever the rules last
 * wanted." Turning OFF is never gated by these two (matching
 * relay_authority.h's own "the safe direction must always be reachable"
 * rule) -- if the rules don't want the relay on, this returns false
 * unconditionally, gates or not.
 *
 * is_heater_relay is the third, independent gate for the owner's design rule
 * ("the relays that are controlled by pid/thermocouples should not be
 * controlable through rules ... they may be used as rule data though"): true
 * when the relay this decision is for belongs to ANY zone (the caller
 * computes this from zones_config_get_relay_mask() across every configured
 * zone -- see rules_task.c's per-tick recomputation for why that cannot be
 * cached once). A heater relay's answer is forced to false unconditionally,
 * exactly like the two gates above and for the identical reason: this is the
 * one place that can never be bypassed by a stale/illegal rule_driven flag a
 * config saved before the relay was assigned to a zone (or before this gate
 * existed at all). Note this only blocks COMMANDING the relay -- reading
 * another relay's commanded state via a COND_RELAY condition is untouched by
 * this parameter, which is deliberate: a heater relay may still be used as
 * rule DATA, just never as a rule TARGET.
 *
 * This is the one function rules_task.c should trust for the actual relay
 * decision; rules_eval_relay_wants_on() alone is NOT safe to drive a relay
 * from directly, since it knows nothing about the safety link, an
 * in-progress OTA update, or heater-relay ownership. */
bool rules_eval_decide(const relay_rules_cfg_t *cfg, const rules_eval_inputs_t *in,
                        bool safety_link_ok, bool heat_interlock_ok, bool is_heater_relay);

#ifdef __cplusplus
}
#endif

#endif // RULES_EVAL_H
