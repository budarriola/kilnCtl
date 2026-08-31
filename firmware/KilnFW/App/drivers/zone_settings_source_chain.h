#ifndef ZONE_SETTINGS_SOURCE_CHAIN_H
#define ZONE_SETTINGS_SOURCE_CHAIN_H

#include <stdbool.h>
#include <stdint.h>

#include "MAX31856.h"
#include "zones_http.h" /* ZONE_SETTINGS_SOURCE_CUSTOM */

/* Shared implementation of the settings_source inheritance-chain walk.
 *
 * Pulled OUT of zones_http.c into its own header (2026-08-31, opus review of
 * commit b69b74a) so that test_backup_import.c's stub for
 * zones_config_settings_source_import_has_cycle() calls the SAME algorithm
 * zones_http.c's real implementation does, instead of a hand-maintained
 * re-implementation that could silently drift from it (or be silently wrong
 * from day one) without any test noticing -- see this header's own git-log
 * neighbor commit message for the concrete finding: two backup-import tests
 * would have passed identically with the shipped check deleted, because the
 * stub never called the shipped code at all. test_backup_import.c cannot
 * #include zones_http.c itself (it is compiled into the same host-test
 * executable as test_profile_feasibility.c, which already defines several of
 * zones_http.h's other non-static functions -- pulling in the whole file
 * would multiply-define those), so the walk is factored out to the one piece
 * small and stateless enough to share safely: `static inline`, no globals, no
 * hardware, nothing else in zones_http.c needed to compile it.
 *
 * `zones` here is intentionally NOT `zone_cfg_t` (zones_http.c's real struct
 * array) -- it is just the settings_source byte for each zone, so a caller
 * whose "live" state lives somewhere other than a zone_cfg_t array (this
 * header's own test-double caller included) can still call the real walk
 * without reshaping its own storage to match zones_http.c's internal layout.
 *
 * Returns true if the chain starting at `start` revisits a zone already on
 * it -- a genuine inheritance cycle -- and false if it terminates cleanly at
 * ZONE_SETTINGS_SOURCE_CUSTOM or at a link landing on or past `thermo_count`
 * (the same "unused trailing slot" bound zones_http.c's own callers apply --
 * see zones_http.c's settings_source_chain_has_cycle() history for why: an
 * unconfigured slot's settings_source == 0 is a REAL value here, not a "not
 * set" sentinel, so a slot past thermo_count must be excluded from the walk
 * rather than followed as if it were a deliberate link).
 *
 * Capped at MAX31856_CHANNEL_COUNT hops so this terminates even walking an
 * already-corrupt chain -- there are only MAX31856_CHANNEL_COUNT distinct
 * zones, so any chain that has not hit CUSTOM, a >=thermo_count link, or a
 * repeat within that many hops is, by the pigeonhole principle, about to
 * repeat on the very next hop; treating "hit the cap" as a cycle is exact,
 * not a conservative over-refusal. Self-reference is caught on the very
 * first hop, same as every other repeat -- this function does not
 * special-case it, callers that want a distinct error message for
 * self-reference check that separately, first. */
static inline bool zone_settings_source_chain_has_cycle(const uint8_t settings_source[MAX31856_CHANNEL_COUNT],
                                                         uint8_t start, uint8_t thermo_count)
{
    bool visited[MAX31856_CHANNEL_COUNT] = {0};
    visited[start] = true;
    uint8_t cur = start;
    for (uint8_t hop = 0; hop < MAX31856_CHANNEL_COUNT; hop++) {
        uint8_t src = settings_source[cur];
        if (src == ZONE_SETTINGS_SOURCE_CUSTOM) {
            return false;
        }
        if (src >= thermo_count) {
            return false; /* out-of-range, or an unused trailing slot -- not a real link to follow */
        }
        if (visited[src]) {
            return true;
        }
        visited[src] = true;
        cur = src;
    }
    return true; /* exceeded the hop cap without terminating -- see comment above */
}

/* Cross-entry pass-1 check, shared the same way as the walk above: `zones`
 * holds every zone's CURRENT settings_source; `has_override[z]`/
 * `override_source[z]` name any proposed new values for this pass (false in
 * has_override means "keep zones[z] as given"). Walks every zone's resulting
 * chain and reports whether ANY of them cycles, naming one cycling zone in
 * *out_cycle_zone if it is non-NULL. See
 * zones_http.h's zones_config_settings_source_import_has_cycle() doc comment
 * for the full "why a per-call check on a single link isn't enough for a
 * multi-entry import" rationale -- this is the shared engine that function
 * (and test_backup_import.c's stub double of it) both call. */
static inline bool zone_settings_source_chain_import_has_cycle(const uint8_t zones[MAX31856_CHANNEL_COUNT],
                                                                const bool has_override[MAX31856_CHANNEL_COUNT],
                                                                const uint8_t override_source[MAX31856_CHANNEL_COUNT],
                                                                uint8_t thermo_count, uint8_t *out_cycle_zone)
{
    uint8_t probe[MAX31856_CHANNEL_COUNT];
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        probe[i] = has_override[i] ? override_source[i] : zones[i];
    }
    if (thermo_count > MAX31856_CHANNEL_COUNT) {
        thermo_count = MAX31856_CHANNEL_COUNT;
    }
    for (uint8_t i = 0; i < thermo_count; i++) {
        if (zone_settings_source_chain_has_cycle(probe, i, thermo_count)) {
            if (out_cycle_zone) {
                *out_cycle_zone = i;
            }
            return true;
        }
    }
    return false;
}

#endif /* ZONE_SETTINGS_SOURCE_CHAIN_H */
