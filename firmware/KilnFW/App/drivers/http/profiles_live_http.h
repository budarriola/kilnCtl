// profiles_live_http -- docs/LIVE_PROFILE_EDIT_PLAN.md pass 2: the five-route
// HTTP surface over pass 1's live_profile.c/profile_executor_live_pickup.c
// backend (landed bca094fc). Deliberately its own file, not folded into
// profiles_http.c/profiles_edit_http.c/profiles_catalog_http.c: the plan's
// section 8 sequencing note names those three as contested with other
// in-flight work, and this feature reuses their exported seam
// (profiles_http_internal.h's profiles_parse_profile_fields()/
// profiles_validate_candidate()) rather than editing inside them.
#ifndef PROFILES_LIVE_HTTP_H
#define PROFILES_LIVE_HTTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers GET /live_profile, GET+POST /api/profile/live, POST
 * /api/profile/live/fork and POST /api/profile/live/decide on the server
 * wifi_provision_http_start() already started. Call after profiles_http_start()
 * (main_network_http.c) -- this module calls profiles_http_get()/
 * profiles_http_save()/profiles_http_delete() (profiles_store.h) and
 * profiles_parse_profile_fields()/profiles_validate_candidate()
 * (profiles_http_internal.h), all of which are ready the moment
 * profiles_http_start() returns ESP_OK. */
esp_err_t profiles_live_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // PROFILES_LIVE_HTTP_H
