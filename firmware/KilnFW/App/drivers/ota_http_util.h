// ota_http_util -- pure, dependency-light helpers pulled out of ota_http.c
// following dashboard_json.c's precedent (see that file's header comment for
// the general shape of this split in this codebase): none of these
// functions touch httpd_req_t, NVS, esp_ota_ops.h, or any hardware/RTOS
// state -- they only transform bytes/enums the caller already has in hand.
// Moving them here makes them reachable from a plain host test without
// dragging in the rest of ota_http.c's ESP-IDF surface.
//
// Verbatim move: signatures, bodies, and behavior are unchanged from their
// former `static` definitions in ota_http.c. This split changes where they
// are DEFINED (and gives them external linkage so both ota_http.c and a
// host test can call them), not what they do.
#ifndef OTA_HTTP_UTIL_H
#define OTA_HTTP_UTIL_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "ota_http.h" /* ota_http_esp_phase_t, ota_http_verify_result_t */
#include "safety_link.h" /* safety_link_rollback_outcome_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Encodes `len` bytes as 2*len lowercase hex characters into out, which
 * must be at least 2*len + 1 bytes (NUL-terminated). */
void ota_http_hex_encode(const uint8_t *in, size_t len, char *out);

/* Inverse of ota_http_hex_encode(): decodes exactly `hex_len` hex characters
 * (must be even) into hex_len/2 bytes. Returns false on any non-hex
 * character or odd length, leaving `out` in an unspecified state (callers
 * must check the return value before trusting `out`). */
bool ota_http_hex_decode(const char *hex, size_t hex_len, uint8_t *out);

/* Human-readable string for an ota_http_esp_phase_t. */
const char *ota_http_esp_phase_str(ota_http_esp_phase_t phase);

/* Human-readable string for an ota_http_verify_result_t. */
const char *ota_http_verify_result_str(ota_http_verify_result_t r);

/* Human-readable string for kilnlink_rollback_result_reason_t (wire-carried
 * as a plain uint8_t -- see caller's own doc comment for why). Any value
 * outside the known set reads as "unknown reason" rather than indexing out
 * of bounds or aliasing a real one. */
const char *ota_http_pico_rollback_reason_str(uint8_t reason_code);

/* Formats the JSON body for POST /api/ota/pico/rollback's response from a
 * safety_link_rollback_outcome_t + reason code. Returns snprintf's return
 * value (the would-be length, same convention as snprintf itself). */
int ota_http_pico_rollback_format_body(safety_link_rollback_outcome_t outcome, uint8_t reason_code,
                                        char *body, size_t cap);

#ifdef __cplusplus
}
#endif

#endif // OTA_HTTP_UTIL_H
