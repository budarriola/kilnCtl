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

#include "ota_http.h" /* ota_http_esp_phase_t */
#include "safety_link.h" /* safety_link_rollback_outcome_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Encodes `len` bytes as 2*len lowercase hex characters into out, which
 * must be at least 2*len + 1 bytes (NUL-terminated). */
void ota_http_hex_encode(const uint8_t *in, size_t len, char *out);

/* Human-readable string for an ota_http_esp_phase_t. */
const char *ota_http_esp_phase_str(ota_http_esp_phase_t phase);

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

/* Pure core of ota_http_get_client_ip() (ota_http.c), pulled out so its
 * failure-path handling can be host-tested without dragging in lwip's
 * sockets -- ota_http.c does the real httpd_req_to_sockfd()/getpeername()/
 * inet_ntop() calls and hands this function only the outcome.
 *
 * `formatted_addr` is the already-formatted address string on success, or
 * NULL for ANY failure (httpd_req_to_sockfd() < 0, getpeername() != 0, or
 * inet_ntop() returning NULL). On every call this leaves `out` NUL-
 * terminated and holding either a copy of `formatted_addr` or the literal
 * "unknown" -- never untouched/uninitialized memory, which is what the
 * 2026-09-17 review found: the caller's `inet_ntop(...)` return value used
 * to go unchecked, so a lwip inet_ntop() failure (it does not write to its
 * output buffer on failure, same as BSD's) left the caller's stack buffer
 * holding whatever was already there, then compared via strcmp() against a
 * stored session IP -- a read of uninitialized memory feeding a security
 * comparison.
 *
 * Returns true if a real address was written, false if the shared
 * "unknown" sentinel was -- see ota_http_get_client_ip()'s own header
 * comment and web_auth_session.h's WEB_AUTH_CLIENT_IP_LEN comment for why
 * "unknown" is intentionally non-unique (every client that fails to
 * resolve an address collides on it). A caller that MINTS a new session
 * off this buffer (web_auth_login_http.c's login_post_handler(), the only
 * such call site) SHOULD treat a false return as "refuse", not "proceed",
 * to avoid binding a real session to a sentinel other undetermined clients
 * also share -- not yet wired up there as of this comment, see
 * ota_http_get_client_ip()'s own header comment for why. A caller that only
 * COMPARES against an already-existing session's stored client_ip is
 * unaffected either way. */
bool ota_http_client_ip_finalize(char *out, size_t out_len, const char *formatted_addr);

/* Pure core of ota_esp_do_transfer()'s target check (ota_http_esp.c): true only
 * when `target` (esp_ota_get_next_update_partition(NULL)) is a real partition
 * AND is not the partition this image is RUNNING from. Pointers are compared as
 * opaque identities (esp_partition_t pointers from the IDF are unique per
 * partition), so no IDF type is needed here.
 *
 * Single-slot table (partitions.csv: `app` ota_0 + factory `recovery`):
 * next-update-partition returns ota_0, which is the running `app` itself, and
 * esp_ota_begin() then fails with ESP_ERR_OTA_PARTITION_CONFLICT. An ESP image
 * push goes through the recovery image's route instead
 * (docs/OTA_SINGLE_SLOT.md); the application refuses up front with 409. */
bool ota_http_esp_target_usable(const void *target, const void *running);

/* Verdict for one step of the bounded drain that follows an early refusal
 * (ota_http_refusal_drain() in ota_http.c). `recv_ret` is the last
 * httpd_req_recv() result, `elapsed_ms` the time spent draining so far,
 * `cap_ms` the total allowed. DONE (recv_ret == 0, body fully consumed) wins
 * over the time cap; a negative recv_ret or an exhausted cap is FAIL (caller
 * returns ESP_FAIL so httpd closes the socket); otherwise CONTINUE. */
typedef enum {
    OTA_DRAIN_CONTINUE = 0,
    OTA_DRAIN_DONE,
    OTA_DRAIN_FAIL,
} ota_http_drain_verdict_t;

ota_http_drain_verdict_t ota_http_drain_verdict(int recv_ret, uint32_t elapsed_ms, uint32_t cap_ms);

#ifdef __cplusplus
}
#endif

#endif // OTA_HTTP_UTIL_H
