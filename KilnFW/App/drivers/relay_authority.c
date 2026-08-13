#include "relay_authority.h"

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

void relay_authority_set_zone_blocked(uint8_t zone_index, bool blocked)
{
    if (zone_index < RELAY_AUTHORITY_MAX_ZONES) {
        s_zone_blocked[zone_index] = blocked;
    }
}
