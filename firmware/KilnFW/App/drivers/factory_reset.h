// factory_reset -- TODO.md 8.1's "decide what 'reset kiln config' means":
// a per-partition, explicit-scope wipe, plus a factory-default option that
// wipes all three. See factory_reset.c for the endpoint and the reboot-after-
// erase rationale. POST /api/factory_reset is challenge-response
// authenticated (OTA_HTTP_CONTEXT_FACTORY_RESET, ota_http.h) and interlocked
// the same way every other destructive OTA route is -- see reset_post_
// handler()'s own doc comment for why both gates run, and in that order.
#ifndef FACTORY_RESET_H
#define FACTORY_RESET_H

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers POST /api/factory_reset (and a "Danger zone" section rendered by
 * main_page.html) on the shared httpd instance. Must run after
 * wifi_provision_http_start() has brought that server up, same convention as
 * every other *_http_start() in this codebase. */
esp_err_t factory_reset_http_start(void);

/* Wire-facing scope IDs for the UART SYSTEM task's FACTORY_RESET subcommand
 * (uart_task_ids.h) -- numeric indices into the same kScopes[] table
 * reset_post_handler() looks up by name, so both entry points erase exactly
 * the same partition sets. Order matches kScopes[] in factory_reset.c. */
typedef enum {
    FACTORY_RESET_SCOPE_WIFI = 0,
    FACTORY_RESET_SCOPE_KILN = 1,
    FACTORY_RESET_SCOPE_PROFILES = 2,
    FACTORY_RESET_SCOPE_ALL = 3,
} factory_reset_scope_t;

/* Erases the NVS partition(s) for `scope` and schedules a reboot ~500ms out
 * (same reboot_task() this file's HTTP handler uses, so a reply already
 * queued by the caller -- HTTP response or this UART command's ACK -- has a
 * chance to actually leave before the restart). Returns the first partition
 * erase's error, if any; the reboot is scheduled unconditionally either way,
 * same reasoning as reset_post_handler(). ESP_ERR_INVALID_ARG for an
 * out-of-range scope (no erase attempted, no reboot scheduled). */
esp_err_t factory_reset_execute(factory_reset_scope_t scope);

#ifdef __cplusplus
}
#endif

#endif // FACTORY_RESET_H
