// aux_outputs_conflict -- the pure "one relay, one owner" predicate for the
// spare-relay on/off outputs (docs/SPARE_RELAY_ONOFF_PLAN.md section 3).
// Header-only and dependency-free so zones_config_json.c, the host tests and
// the aux store can all share ONE definition of the invariant: a relay bit may
// be in at most one of {the union of every zone's relay_mask, the set of
// enabled aux outputs}.
#ifndef AUX_OUTPUTS_CONFLICT_H
#define AUX_OUTPUTS_CONFLICT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bitmask of relays claimed by BOTH sides; 0 = no conflict. Bit i = relay i+1. */
static inline uint8_t aux_outputs_relay_conflict_mask(uint8_t zones_relay_union, uint8_t aux_enabled_mask)
{
    return (uint8_t)(zones_relay_union & aux_enabled_mask);
}

static inline bool aux_outputs_relay_conflict(uint8_t zones_relay_union, uint8_t aux_enabled_mask)
{
    return aux_outputs_relay_conflict_mask(zones_relay_union, aux_enabled_mask) != 0;
}

#ifdef __cplusplus
}
#endif

#endif // AUX_OUTPUTS_CONFLICT_H
