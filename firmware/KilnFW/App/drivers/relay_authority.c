#include "relay_authority.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

/* Deliberately not MAX31856_CHANNEL_COUNT -- this module stays free of a
 * MAX31856.h dependency (see relay_authority.h's minimal-include
 * philosophy); 3 is that constant's actual value today and the bound is
 * only ever used to reject an obviously-bad zone_index, not to size
 * anything hardware-facing. */
#define RELAY_AUTHORITY_MAX_ZONES 3u

static bool s_zone_blocked[RELAY_AUTHORITY_MAX_ZONES];

bool relay_authority_on_blocked(SafetyLinkClass *safety, uint32_t *out_sources)
{
    if (!safety) {
        if (out_sources) {
            *out_sources = SAFETY_FAULT_SRC_APP;
        }
        return true;
    }
    uint32_t sources = safety_link_get_fault_sources(safety);
    if (out_sources) {
        *out_sources = sources;
    }
    return sources != 0u;
}

bool relay_authority_zone_blocked(SafetyLinkClass *safety, uint8_t zone_index, uint32_t *out_sources)
{
    if (relay_authority_on_blocked(safety, out_sources)) {
        return true; /* global check is final when it blocks -- see header */
    }
    if (zone_index < RELAY_AUTHORITY_MAX_ZONES && s_zone_blocked[zone_index]) {
        if (out_sources) {
            *out_sources = SAFETY_FAULT_SRC_THERMAL_SANITY;
        }
        return true;
    }
    return false;
}

bool relay_authority_zone_latched_blocked(uint8_t zone_index)
{
    return (zone_index < RELAY_AUTHORITY_MAX_ZONES) && s_zone_blocked[zone_index];
}

uint8_t relay_authority_latched_blocked_mask(void)
{
    uint8_t mask = 0;
    for (uint8_t i = 0; i < RELAY_AUTHORITY_MAX_ZONES; i++) {
        if (s_zone_blocked[i]) mask |= (uint8_t)(1u << i);
    }
    return mask;
}

void relay_authority_set_zone_blocked(uint8_t zone_index, bool blocked)
{
    if (zone_index < RELAY_AUTHORITY_MAX_ZONES) {
        s_zone_blocked[zone_index] = blocked;
    }
}

/* Deliberately not KILN_IO_RELAY_COUNT -- same minimal-include reasoning as
 * RELAY_AUTHORITY_MAX_ZONES above; 4 is that constant's actual value today. */
#define RELAY_AUTHORITY_MAX_RELAYS 4u

static relay_owner_t s_relay_owner[RELAY_AUTHORITY_MAX_RELAYS];

relay_owner_t relay_authority_get_owner(uint8_t relay_index)
{
    if (relay_index < 1u || relay_index > RELAY_AUTHORITY_MAX_RELAYS) {
        return RELAY_OWNER_NONE;
    }
    return s_relay_owner[relay_index - 1u];
}

void relay_authority_claim_mask(uint8_t relay_mask, relay_owner_t owner)
{
    for (uint8_t i = 0; i < RELAY_AUTHORITY_MAX_RELAYS; i++) {
        if (relay_mask & (1u << i)) {
            s_relay_owner[i] = owner;
        }
    }
}

void relay_authority_release_mask(uint8_t relay_mask)
{
    relay_authority_claim_mask(relay_mask, RELAY_OWNER_NONE);
}

bool relay_authority_manual_blocked_by_owner(uint8_t relay_index)
{
    relay_owner_t owner = relay_authority_get_owner(relay_index);
    return owner == RELAY_OWNER_PROFILE || owner == RELAY_OWNER_RULE || owner == RELAY_OWNER_AUTOTUNE;
}

/* ---- The single shared heat claim -- see relay_authority.h's doc comment
 * above relay_heat_zone_claimant_t for the full "why". A portMUX spinlock,
 * not a FreeRTOS mutex: every critical section below is a handful of plain
 * bool reads/writes with no blocking call inside, exactly the case
 * portENTER_CRITICAL/portEXIT_CRITICAL exist for, and it needs no create/
 * destroy lifecycle -- this module already has none (s_zone_blocked/
 * s_relay_owner above are plain statics), so a static portMUX_INITIALIZER_
 * UNLOCKED fits its existing "no init function" shape instead of adding
 * one just for this. */
static portMUX_TYPE s_heat_claim_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_heat_profile_active = false;
static bool s_heat_autotune_active = false;
static bool s_heat_sweep_active = false;

bool relay_authority_heat_zone_claim_begin(relay_heat_zone_claimant_t who)
{
    bool ok;
    portENTER_CRITICAL(&s_heat_claim_mux);
    if (s_heat_sweep_active) {
        ok = false;
    } else {
        if (who == RELAY_HEAT_ZONE_CLAIM_AUTOTUNE) {
            s_heat_autotune_active = true;
        } else {
            s_heat_profile_active = true;
        }
        ok = true;
    }
    portEXIT_CRITICAL(&s_heat_claim_mux);
    return ok;
}

void relay_authority_heat_zone_claim_end(relay_heat_zone_claimant_t who)
{
    portENTER_CRITICAL(&s_heat_claim_mux);
    if (who == RELAY_HEAT_ZONE_CLAIM_AUTOTUNE) {
        s_heat_autotune_active = false;
    } else {
        s_heat_profile_active = false;
    }
    portEXIT_CRITICAL(&s_heat_claim_mux);
}

relay_heat_sweep_claim_result_t relay_authority_heat_sweep_claim_begin(void)
{
    relay_heat_sweep_claim_result_t result;
    portENTER_CRITICAL(&s_heat_claim_mux);
    if (s_heat_profile_active) {
        result = RELAY_HEAT_SWEEP_CLAIM_REFUSE_PROFILE;
    } else if (s_heat_autotune_active) {
        result = RELAY_HEAT_SWEEP_CLAIM_REFUSE_AUTOTUNE;
    } else {
        s_heat_sweep_active = true;
        result = RELAY_HEAT_SWEEP_CLAIM_OK;
    }
    portEXIT_CRITICAL(&s_heat_claim_mux);
    return result;
}

void relay_authority_heat_sweep_claim_end(void)
{
    portENTER_CRITICAL(&s_heat_claim_mux);
    s_heat_sweep_active = false;
    portEXIT_CRITICAL(&s_heat_claim_mux);
}
