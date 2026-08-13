// factory_reset -- TODO.md 8.1's "decide what 'reset kiln config' means":
// a per-partition, explicit-scope wipe, plus a factory-default option that
// wipes all three. See factory_reset.c for the endpoint and the reboot-after-
// erase rationale.
#ifndef FACTORY_RESET_H
#define FACTORY_RESET_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers POST /api/factory_reset (and a "Danger zone" section rendered by
 * main_page.html) on the shared httpd instance. Must run after
 * wifi_provision_http_start() has brought that server up, same convention as
 * every other *_http_start() in this codebase. */
esp_err_t factory_reset_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // FACTORY_RESET_H
