// zones_config_query -- narrow slice of zones_config_accessors.h
// (HW_ABSTRACTION.md pre-reorg layering item 11, 2026-09-05).
// zones_config_accessors.h is a 1300+ line persist-tier header; sim_backend.c
// (a hardware-abstraction-boundary file) only ever needed one read-only
// query out of it -- zones_config_get_thermo_count(). This header holds
// just that declaration so a query-only caller does not have to pull in
// the full persist/config-write surface. Definition is unchanged, still in
// zones_config_accessors.c. zones_config_accessors.h includes this header
// so every existing include of it keeps working unmodified.
#ifndef ZONES_CONFIG_QUERY_H
#define ZONES_CONFIG_QUERY_H

#include <stdbool.h>
#include <stdint.h>

/* How many of the MAX31856_CHANNEL_COUNT channels currently have a zone
 * configured. See zones_config_accessors.h for the full doc comment. */
uint8_t zones_config_get_thermo_count(void);

#endif // ZONES_CONFIG_QUERY_H
