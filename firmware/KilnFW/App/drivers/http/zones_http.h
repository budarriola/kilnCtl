// zones_http -- Settings > Thermocouples & Zones page (TODO.md section 3),
// serving /settings/zones and its JSON API, plus the PID-tuning fields
// section 0.5 explicitly settled onto this same page rather than a
// separate one.
//
// HW_ABSTRACTION.md "drivers/ layering" item 1 (2026-09-05): this
// header used to also carry every zones_config_*()/zones_current_sweep_*()/
// zones_ct_*() persist-layer accessor, so the 18+ control/safety/persist
// modules that only wanted those had to include an HTTP-page header to get
// them. That surface moved to zones_config_accessors.h (included below, so
// existing HTTP callers of this header are unaffected); this file now keeps
// only handler registration (zones_http_start()) and the hardware-wiring
// setter (zones_http_set_hw()) main_network_http.c calls once at boot.
#ifndef ZONES_HTTP_H
#define ZONES_HTTP_H

#include "zones_config_accessors.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Loads zones_cfg from NVS (namespace "kiln_cfg", key "zones_cfg";
 * ESP_ERR_NVS_NOT_FOUND is not an error -- mirrors wifi_prov.c's nvs_load,
 * defaults to thermo_count/relay_count 0, i.e. nothing configured yet) and
 * registers /settings/zones + GET/POST /api/zones on the server
 * wifi_provision_http.c already started. No hardware pointers needed --
 * this is pure config CRUD, gated by MAX31856_CHANNEL_COUNT /
 * KILN_IO_RELAY_COUNT rather than which hardware actually answered. */
esp_err_t zones_http_start(void);

/* ---- Task 1/2/3 (2026-08-27+2, owner report): normal-current sweep, ------
 * CT-to-zone mapping check, and read-only safety-processor wiring display.
 * All three need live hardware -- the per-zone relay/current-sense sweep,
 * the live current comparison, and the safety link's own status -- which
 * zones_http_start() deliberately does not take (it is pure config CRUD,
 * see its own comment). Call this once, any time after zones_http_start(),
 * with whatever main.c has -- same NULL-tolerant convention as
 * dashboard_http_start()/ota_http_start(): a NULL pointer here does not
 * crash anything, it just makes zones_current_sweep_start() refuse with
 * ZONE_SWEEP_REFUSE_NO_HW and zones_get_safety_wiring()/
 * zones_ct_mapping_warn_mask() report the safe "link down" defaults. */
void zones_http_set_hw(kiln_io_t *io_or_null, MAX31856BusClass *thermo_bus_or_null,
                       SafetyLinkClass *safety_or_null);

#ifdef __cplusplus
}
#endif

#endif // ZONES_HTTP_H
