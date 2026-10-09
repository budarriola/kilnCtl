// sim_backend -- runs the control stack against the host-side kiln model
// instead of real MAX31856/SX1509 hardware. TODO.md 6A.8's
// KILNCTL_SIM_PLANT item.
//
// Why this exists: the guards in thermal_guard.c cannot be trusted until
// each one has been made to fire on purpose, and the failures that matter
// most (a welded relay contact, an element that stopped heating, a
// thermocouple that fell out of the kiln body) cannot be provoked safely on
// real hardware -- nor at all on this bench unit, which has no thermocouple
// daughterboard or relay expander attached. App/test/ already proves the
// pure modules against the model on the host; this wires the SAME model
// (App/test/sim_plant.c, compiled into the firmware, not a second copy) into
// the on-target executor, so a whole firing -- HTTP start, ramp, guard trip,
// escalation, fault display, operator clear -- can be walked end to end on
// the actual board.
//
// Every entry point below compiles to nothing when CONFIG_KILNCTL_SIM_PLANT
// is off (the production default), so call sites need no #ifdef and the
// hardware path is byte-identical to a build without this file.
//
// This is a DEVELOPMENT feature. With it on, the board reports fabricated
// temperatures: `GET /api/status` and every page will show a kiln that is
// not there. The Kconfig help text says so, `GET /api/sim` reports
// `"simulated": true`, and the executor logs it at boot.
#ifndef SIM_BACKEND_H
#define SIM_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

#include "MAX31856.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_KILNCTL_SIM_PLANT

/* Compile-time constant so the hardware branch folds away in a sim build
 * and the sim branch folds away in a production one. */
static inline bool sim_backend_enabled(void) { return true; }

/* Advances the model to now (wall-clock, via esp_timer -- so it does not
 * matter how many callers read it or how often) and fills out one
 * MAX31856Reading per configured zone, using the same failure contract the
 * real driver documents: NaN temperature plus the matching fault_status bit
 * for an injected sensor fault, never a stale-but-plausible number. */
esp_err_t sim_backend_read_all(MAX31856Reading *out, size_t max_readings, size_t *out_count);

/* Called from every place that commands a zone's relay group, with what was
 * actually commanded after the safety gate -- this is the sim's only input.
 * A zone whose relay is on gets full element power unless a fault says
 * otherwise. */
void sim_backend_note_zone_relay(uint8_t zone, bool on);

/* Registers GET/POST /api/sim on the shared httpd. GET reports each zone's
 * true element temperature, reported sensor temperature, and injected
 * fault; POST {zone, fault} injects or clears one. Only present in a sim
 * build -- there is deliberately no way to reach this from a production
 * image. */
esp_err_t sim_backend_register_http(void);

#else /* !CONFIG_KILNCTL_SIM_PLANT */

static inline bool sim_backend_enabled(void) { return false; }
static inline esp_err_t sim_backend_read_all(MAX31856Reading *out, size_t max_readings, size_t *out_count)
{
    (void)out;
    (void)max_readings;
    if (out_count) *out_count = 0;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline void sim_backend_note_zone_relay(uint8_t zone, bool on)
{
    (void)zone;
    (void)on;
}
static inline esp_err_t sim_backend_register_http(void) { return ESP_OK; }

#endif /* CONFIG_KILNCTL_SIM_PLANT */

#ifdef __cplusplus
}
#endif

#endif // SIM_BACKEND_H
