// ota_http.h -- CommonFW/docs/UPDATE_PROTOCOL.md section 2's challenge-
// response authentication, wired to a real HTTP endpoint. The pure state
// machine (nonce lifecycle, constant-time compare, lockout/backoff) lives in
// ota_auth.h/.c, already host-tested; this file is the ESP-IDF/mbedTLS/httpd
// glue around it.
//
// Serves GET /api/ota/challenge only. This file does NOT implement
// POST /api/ota/esp or POST /api/ota/pico -- those need the actual streamed
// esp_ota_ops write logic and the Pico-image relay through the pico_img
// staging partition, both separate, larger, unbuilt pieces of work
// (UPDATE_PROTOCOL.md sections 3/4). ota_http_verify_request() below is
// written so a future pass can call it as the first thing either POST
// handler does, without having to touch this file again.
#ifndef OTA_HTTP_H
#define OTA_HTTP_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers GET /api/ota/challenge on the server wifi_provision_http.c
// already started. Call after wifi_prov_start() (this endpoint reads
// wifi_prov_get_ap_password() indirectly through ota_http_verify_request(),
// so the AP password must already be loaded, though the challenge handler
// itself does not need the password -- only verification does).
esp_err_t ota_http_start(void);

// The context a client authenticates for -- CommonFW/docs/UPDATE_PROTOCOL.md
// section 2 step 2's literal "esp" or "pico" HMAC context string, and also
// which of the two independent per-endpoint lockout states applies.
typedef enum {
    OTA_HTTP_CONTEXT_ESP = 0,
    OTA_HTTP_CONTEXT_PICO,
} ota_http_context_t;

typedef enum {
    OTA_HTTP_VERIFY_OK = 0,
    OTA_HTTP_VERIFY_LOCKED_OUT,
    OTA_HTTP_VERIFY_NO_VALID_NONCE, // never issued, expired, or already used --
                                     // NOT counted as an auth failure, see .c
    OTA_HTTP_VERIFY_BAD_MAC,
} ota_http_verify_result_t;

// Verifies a client's claimed MAC against the currently active challenge
// nonce, for the given context. `mac` must be exactly 32 bytes (raw
// HMAC-SHA256 output, not hex/base64 -- the caller decodes the
// X-Ota-Mac request header before calling this, see ota_http.c's
// challenge/verify handlers for the wire encoding this pairs with).
//
// Always invalidates the active nonce before returning (single-use, "the
// nonce whether or not it matched" -- UPDATE_PROTOCOL.md section 2 step 4),
// EXCEPT when the result is OTA_HTTP_VERIFY_NO_VALID_NONCE, since there is
// then no valid nonce left to invalidate. Records a lockout failure only on
// OTA_HTTP_VERIFY_BAD_MAC -- a stale/reused/never-issued nonce is the
// client's timing, not a wrong-password guess, and must not count toward
// the 3-strikes lockout (seeCommonFW/docs/UPDATE_PROTOCOL.md's actual "Rate
// limiting" intent: throttle password guesses, not slow legitimate
// clients). A future caller (the eventual POST /api/ota/esp or
// /api/ota/pico handler) should refuse the request unless this returns
// OTA_HTTP_VERIFY_OK.
ota_http_verify_result_t ota_http_verify_request(ota_http_context_t ctx, const uint8_t mac[32],
                                                  const char *client_ip);

#ifdef __cplusplus
}
#endif

#endif // OTA_HTTP_H
