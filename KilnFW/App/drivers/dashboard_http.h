// dashboard_http -- the Dashboard page's data API: live thermocouple/relay
// status and manual relay control, served over the same HTTP server
// wifi_provision_http.c already owns (see wifi_provision_http_get_server()).
//
// Reads the *same* driver instances the UART bridge reads (kiln_io_t,
// MAX31856BusClass) rather than duplicating ownership of them -- this is a
// second reader/caller alongside the UART bridge, not a second owner.
// Manual relay-ON commands go through relay_authority_on_blocked()
// (App/drivers/relay_authority.c), the same chokepoint the UART bridge uses,
// so a safety fault refuses this path exactly like it refuses the PC link.
//
// Scope (TODO.md section 2): live status/manual control, plus
// start/stop/pause/status for a running profile (GET/POST /api/profile_exec*),
// which reads and drives App/drivers/profile_executor.c -- this module owns
// no execution state of its own, same "one reader, one owner" split as
// zones_http.c/profiles_http.c. No temperature graph yet (needs the history
// buffer, TODO.md section 0 -- designed but has no writer yet); that one is
// still deliberately absent rather than stubbed.
#ifndef DASHBOARD_HTTP_H
#define DASHBOARD_HTTP_H

#include "esp_err.h"
#include "kiln_io.h"
#include "MAX31856.h"
#include "safety_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the dashboard's routes on the server wifi_provision_http.c
 * already started. Any pointer may be NULL (board not attached/not up) --
 * GET /api/status reports that honestly rather than faking data, matching
 * app_main's existing "partial hardware is still worth reporting"
 * convention for boot_fault_sources. Call after the I2C/SPI/thermo
 * bring-up block in app_main, once it's known what actually came up. */
esp_err_t dashboard_http_start(kiln_io_t *io_or_null, MAX31856BusClass *thermo_bus_or_null,
                               SafetyLinkClass *safety_or_null);

#ifdef __cplusplus
}
#endif

#endif // DASHBOARD_HTTP_H
