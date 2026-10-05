// aux_outputs_http -- HTTP wrapper for the spare-relay aux outputs
// (docs/SPARE_RELAY_ONOFF_PLAN.md WP-2). All decisions live in aux_outputs_http_core.c;
// this file reads the body, claims the commissioning single-flight guard around a config
// write, and sends the reply.
//
//   GET  /api/aux_outputs         ADMIN -- per-relay config + conflict/quarantine state
//   POST /api/aux_outputs         ADMIN -- one relay's config (409 mid-run, 409 on zone conflict)
//   POST /api/aux_outputs/manual  ADMIN -- manual on/off of an ENABLED aux relay, idle only
#ifndef AUX_OUTPUTS_HTTP_H
#define AUX_OUTPUTS_HTTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Boot wiring. MUST run AFTER kiln_cfg_store_init() (the package re-import there can still
 * change zone relay_masks): loads the aux store reconciled against the zones union,
 * registers aux_outputs_cfg_enabled_mask as the zones-validate provider, then registers the
 * three routes. The store/provider half runs even if route registration fails. */
esp_err_t aux_outputs_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // AUX_OUTPUTS_HTTP_H
