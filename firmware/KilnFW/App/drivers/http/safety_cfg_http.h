// safety_cfg_http -- docs/COMMISSIONING.md sec 3.1's HTTP surface:
//   GET  /api/safety/commissioning
//   POST /api/safety/commissioning
//   POST /api/safety/commissioning/bench_preset
// Reads/writes through safety_cfg_store.h's cache and safety_link.h's
// SET_PARAM/COMMIT_CONFIG/GET_CONFIG_PAGE wire calls -- this file owns no
// storage of its own.
#ifndef SAFETY_CFG_HTTP_H
#define SAFETY_CFG_HTTP_H

#include "esp_err.h"
#include "safety_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers this file's three routes on the server wifi_provision_http.c
 * already started -- same "call after the board it needs has come up, any
 * pointer may be NULL" convention as dashboard_http_start(). `link` may be
 * NULL (safety domain not populated this boot); every handler then answers
 * link_up=false / commissioned=false rather than crashing, same as
 * dashboard_http.c's own NULL-pointer handling for an absent board. */
esp_err_t safety_cfg_http_start(SafetyLinkClass *link_or_null);

#ifdef __cplusplus
}
#endif

#endif // SAFETY_CFG_HTTP_H
