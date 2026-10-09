// safety_pico_relay_mask -- the ONE place the ESP turns its relay shadow into the
// relay mask it tells the Pico (docs/SPARE_RELAY_ONOFF_PLAN.md WP-9, sec 14 item 11).
//
// Owner decision 2026-10-04: aux-bound (spare-relay on/off output) relays are never
// reported to the Pico. The Pico sees them as never commanded, so S3 stays fully
// active (aux current on the CT is NOT explained away: a miswire is a nuisance S3/S4
// trip that fails safe) and an aux vent does not raise S4. No kilnlink field, no
// protocol bump, no SaftyFW change.
//
// LIVE, NOT CACHED: the aux enabled mask is read on every call, so a config change
// (enable/disable/conflict force-off) takes effect on the next PUSH_CONTEXT. Do not
// snapshot it at boot ("reset one side of a pair", CLAUDE.md).
//
// Apply it to relay_now_mask BEFORE the relay_recent_mask bookkeeping, so the recent
// mask never sees aux transitions either. Any new code that puts a relay mask on the
// wire to the Pico must go through this helper.
#ifndef SAFETY_PICO_RELAY_MASK_H
#define SAFETY_PICO_RELAY_MASK_H

#include <stdint.h>

#include "../persist/aux_outputs_cfg.h"

/* Pure core: bit i = relay i+1 in both arguments. */
static inline uint8_t safety_pico_relay_mask_strip(uint8_t relay_shadow, uint8_t aux_enabled_mask)
{
    return (uint8_t)(relay_shadow & (uint8_t)~aux_enabled_mask);
}

/* Live: strips whatever aux_outputs_cfg currently reports as enabled. */
static inline uint8_t safety_pico_relay_mask(uint8_t relay_shadow)
{
    return safety_pico_relay_mask_strip(relay_shadow, aux_outputs_cfg_enabled_mask());
}

#endif // SAFETY_PICO_RELAY_MASK_H
