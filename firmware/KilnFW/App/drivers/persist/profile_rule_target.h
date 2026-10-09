// profile_rule_target -- the one place that knows how a profile on/off rule's
// zone_index byte addresses an aux relay (docs/SPARE_RELAY_ONOFF_PLAN.md
// sections 1.3 and 6). Header-only and dependency-free so the validator, the
// executor (WP-3), the HTTP handlers and host tests share one definition and
// cannot drift.
//
// Wire encoding (no PROFILE_VERSION bump, same profile_on_off_rule_t layout):
//   zone_index 0..2   a zone (must be typed ON_OFF, unchanged)
//   zone_index 8..11  aux relay 1..4 (PROFILE_RULE_TARGET_AUX_BASE + relay - 1)
//   anything else     invalid
// Old firmware rejects 8..11 when SAVING a rule, but a rule already in NVS is not
// re-validated on load: after a rollback it is silently inert, not loudly refused.
#ifndef PROFILE_RULE_TARGET_H
#define PROFILE_RULE_TARGET_H

#include <stdbool.h>
#include <stdint.h>

#define PROFILE_RULE_TARGET_AUX_BASE 8u
#define PROFILE_RULE_TARGET_AUX_COUNT 4u /* == AUX_OUTPUTS_COUNT == KILN_IO_RELAY_COUNT */

#ifdef __cplusplus
extern "C" {
#endif

static inline bool profile_rule_target_is_aux(uint8_t target)
{
    return target >= PROFILE_RULE_TARGET_AUX_BASE &&
           target < (uint8_t)(PROFILE_RULE_TARGET_AUX_BASE + PROFILE_RULE_TARGET_AUX_COUNT);
}

/* 1-based relay (1..4) for an aux target; 0 if `target` is not an aux target. */
static inline uint8_t profile_rule_target_aux_relay(uint8_t target)
{
    return profile_rule_target_is_aux(target) ? (uint8_t)(target - PROFILE_RULE_TARGET_AUX_BASE + 1u) : 0u;
}

/* Wire byte for aux relay 1..4; 0xFF if out of range. */
static inline uint8_t profile_rule_target_from_aux_relay(uint8_t relay_1_4)
{
    return (relay_1_4 >= 1u && relay_1_4 <= PROFILE_RULE_TARGET_AUX_COUNT)
               ? (uint8_t)(PROFILE_RULE_TARGET_AUX_BASE + relay_1_4 - 1u)
               : 0xFFu;
}

#ifdef __cplusplus
}
#endif

#endif // PROFILE_RULE_TARGET_H
