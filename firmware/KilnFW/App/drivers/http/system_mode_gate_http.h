// system_mode_gate_http -- the one shared HTTP sender for a system_mode_gate
// refusal (gate-slices-2/4/5 spec, 2026-09-25). Every HTTP call site that now
// calls system_mode_gate_check() (zones_http_post.c, kiln_cfg_http.c's apply
// submit, backup_import.c's apply, factory_reset.c, cfg_fs_format_http.c)
// sends its refusal through this ONE function rather than hand-rolling the
// same "409 Conflict, text/plain, reason body" three-liner five times --
// same "one choke point" discipline ota_http_send_interlock_refusal() uses
// for OTA's own 428/409 split.
//
// Owner decision 2026-09-25 (Q4): the HTTP status for a system_mode_gate
// refusal is ALWAYS 409 Conflict -- never OTA's 428, which stays reserved for
// a refusal the operator CAN answer with an ack header. A mode-gate refusal
// (a firing or autotune is active) is not answerable that way: it only clears
// once the run ends.
#ifndef SYSTEM_MODE_GATE_HTTP_H
#define SYSTEM_MODE_GATE_HTTP_H

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Sends "409 Conflict" with `reason` as a plain-text body. `reason` should be
// the string system_mode_gate_check() wrote into its own reason buffer --
// this function does not call system_mode_gate_check() itself, so a caller
// that already has locks held or other cleanup to do first controls exactly
// when this is sent. Always returns ESP_OK (matches httpd_resp_send()'s own
// contract and every other refusal sender in this codebase).
esp_err_t system_mode_gate_http_send_refusal(httpd_req_t *req, const char *reason);

#ifdef __cplusplus
}
#endif

#endif // SYSTEM_MODE_GATE_HTTP_H
