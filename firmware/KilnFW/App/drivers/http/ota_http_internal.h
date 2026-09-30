#ifndef OTA_HTTP_INTERNAL_H
#define OTA_HTTP_INTERNAL_H

// Internal seams for the ota_http.c split (2026-09-04, ROADMAP.md M15 A3:
// "files over 1500 lines should be broken up where it makes sense" --
// ota_http.c had grown to 2508 lines). This header is NOT public API --
// ota_http.h stays that -- it exists purely so pieces that used to be one
// translation unit (and could reach each other's `static` state and helpers
// for free) can still do so now that they are four. Same shape as
// profile_executor.c's 2026-09-01 split (profile_executor_internal.h) and
// wifi_prov.c's 2026-09-04 split (wifi_prov_internal.h): every symbol
// declared below was `static` in the original single file and is widened
// to file-scope-internal linkage ONLY because a sibling .c file in this
// split now calls or reads it directly. Every widened symbol is renamed
// with an `ota_http_`/`OTA_HTTP_` prefix -- several of the original short
// names (`TAG`, `s_io`, `s_safety`, `now_ms`, `set_fail_reason`) collide
// with unrelated `static` symbols of the same name elsewhere in
// App/drivers/, which would either fail to link (clash with a non-static
// definition) or link silently and break later (clash with another file's
// same-named `static`) -- see App/drivers/net/wifi_prov_internal.h's own doc
// comment for the precedent this follows.
//
// THIS IS A MOVE-ONLY REFACTOR: no logic, ordering, naming (beyond the
// widening rename above) or visibility change beyond what moving requires.
//
//   ota_http.c       -- includes, the update-claim mutex (try_begin/end/in_progress),
//                        the interlock snapshot/refusal glue, the shared
//                        get_client_ip/send_json_clamped/set_fail_reason
//                        helpers, and ota_http_start() (route registration)
//   ota_http_esp.c   -- POST /api/ota/esp transfer + status + rollback
//                        (ota_esp_do_transfer/_post_handler,
//                        ota_esp_status_get_handler,
//                        ota_esp_rollback_post_handler,
//                        ota_rollback_reboot_task)
//   ota_http_pico.c  -- POST /api/ota/pico stage/relay + status + rollback
//                        (ota_pico_do_stage/_post_handler,
//                        ota_pico_status_get_handler,
//                        ota_pico_rollback_task/_post_handler,
//                        ota_pico_rollback_status_get_handler, and the
//                        pico-rollback async state s_pico_rollback_async)
//   ota_http_recovery.c -- boot-recovery exit and the unauthenticated
//                        interlock-state GET (ota_recovery_exit_reboot_task,
//                        ota_recovery_exit_post_handler,
//                        ota_interlock_get_handler)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "ota_http.h"
#include "ota_http_util.h" /* ota_http_hex_encode */
#include "safety_link.h"

// Shared log tag. Defined (non-static) in ota_http.c; every split file logs
// under the same "ota_http" tag the single file used to, unchanged. NOT
// named plain `TAG` -- every other driver file in App/drivers/ has its own
// `static const char *TAG`, and a global `TAG` here would clash the moment
// two translation units of this split (or this split and any other file)
// end up in the same link.
extern const char *OTA_HTTP_TAG;

// hex_encode() alias -- the same local spelling ota_http.c used before the
// split, now centralized here so every split file's call sites read
// identically to the original single file. (hex_decode()/verify_result_str()
// aliases were removed with the AP-password HMAC scheme, retired 2026-09-29.)
#define hex_encode ota_http_hex_encode
// String form of ota_http_esp_phase_t (ota_http_util.c's
// ota_http_esp_phase_str()) -- only ota_http_esp.c's status handler uses
// it, but centralized here with the other two aliases rather than left as
// a lone local #define, now that the split moved it away from its original
// neighbor (the esp status handler it used to sit directly above).
#define esp_phase_str ota_http_esp_phase_str

// Client IP formatting (used for logging by every mutating handler in the
// esp/pico/recovery files, and by ota_http.c/http_auth_http.c/
// web_auth_session_status_http.c elsewhere).
void ota_http_get_client_ip(httpd_req_t *req, char *out, size_t out_len);

// Checked variant (2026-09-18, d2c51f55 follow-up): same lookup, but
// returns true only when a real client address was genuinely determined,
// false when the caller is looking at the shared "unknown" collision
// sentinel in `out`. A caller that merely COMPARES `out` against an
// existing session's stored address can keep using the plain form above;
// a caller that STORES/binds a new session to `out` must check this
// return value and refuse rather than mint a session bound to a sentinel
// other undetermined clients also share -- see ota_http.c's definition
// and web_auth_login_http.c's login_post_handler() for the one call site
// that needs the distinction today.
bool ota_http_get_client_ip_checked(httpd_req_t *req, char *out, size_t out_len);

// Formats a JSON response into `buf` (already built by the caller via
// snprintf), clamped to `cap`, logging if the caller's own snprintf()
// formatting failed or truncated. Used by every JSON-emitting handler
// across the split.
esp_err_t ota_http_send_json_clamped(httpd_req_t *req, const char *buf, int n, size_t cap);

// Formats into a generous scratch buffer, then copies (truncating rather
// than overflowing) into the caller's OTA_RECORD_REASON_MAX-sized
// destination -- see ota_http.c's definition for the -Werror=format-
// truncation rationale. Used by both the ESP and Pico transfer paths.
void ota_http_set_fail_reason(char *dst, size_t dst_cap, const char *fmt, ...);

// --- Single cross-processor safety-link pointer ----------------------------
// Read-only after ota_http_start() (App/drivers/http/ota_http.c), same
// NULL-tolerant meaning as before the split. Needed outside ota_http.c: the
// ESP rollback-reboot task (ota_http_esp.c) sends the peer an
// announce-reboot, and the whole Pico update/rollback path (ota_http_pico.c)
// talks to the safety processor directly. s_thermo_bus/s_io stay `static`
// in ota_http.c -- ota_http_check_interlocks() (unchanged, already public
// via ota_http.h) is their only reader, in the same file that assigns them.
extern SafetyLinkClass *ota_http_safety;

// --- Pico-rollback async state ---------------------------------------------
// Moved here (was a local typedef in ota_http.c right above the state
// below) because ota_http_pico.c is now this type's only reader/writer;
// ota_http.c (ota_http_start()) only creates the mutex and zeroes the
// struct. Guarded by its own small mutex, deliberately separate from the
// nonce/lockout state (ota_http.c's own s_ota_lock) and from the
// update-claim mutex (ota_http_update_try_begin()/_end()) -- this state is
// written by a background task while the handler that started it may
// already have returned and moved on to a different request, so it cannot
// share either of those locks' lifetimes.
typedef enum {
    OTA_PICO_ROLLBACK_ASYNC_IDLE = 0,       // never attempted this boot, or a
                                             // POST is still working through
                                             // its synchronous auth/interlock
                                             // checks (nothing async started yet)
    OTA_PICO_ROLLBACK_ASYNC_IN_PROGRESS,    // the background task is running
                                             // safety_link_send_rollback_ex()
    OTA_PICO_ROLLBACK_ASYNC_DONE,           // outcome/reason_code below are
                                             // valid; stays DONE (not reset to
                                             // IDLE) until a NEW rollback is
                                             // requested, so a page that polls
                                             // a little late still sees the
                                             // result rather than racing back
                                             // to IDLE first
} ota_pico_rollback_async_state_t;

typedef struct {
    ota_pico_rollback_async_state_t state;
    safety_link_rollback_outcome_t outcome;
    uint8_t reason_code;
} ota_pico_rollback_async_t;

extern SemaphoreHandle_t ota_http_pico_rollback_async_lock;
extern ota_pico_rollback_async_t ota_http_pico_rollback_async;

// --- Route handlers registered by ota_http_start() (ota_http.c) but ------
// defined in one of the other three split files. Each was `static` in the
// original single file; widened here purely so ota_http_start()'s
// httpd_uri_t table can name them. Not part of ota_http.h -- nothing
// outside this split calls a handler directly, httpd dispatches by URI.
esp_err_t ota_esp_post_handler(httpd_req_t *req);              // ota_http_esp.c
esp_err_t ota_esp_status_get_handler(httpd_req_t *req);        // ota_http_esp.c
esp_err_t ota_esp_rollback_post_handler(httpd_req_t *req);     // ota_http_esp.c
esp_err_t ota_pico_post_handler(httpd_req_t *req);             // ota_http_pico.c
esp_err_t ota_pico_status_get_handler(httpd_req_t *req);       // ota_http_pico.c
esp_err_t ota_pico_rollback_post_handler(httpd_req_t *req);    // ota_http_pico.c
esp_err_t ota_pico_rollback_status_get_handler(httpd_req_t *req); // ota_http_pico.c
esp_err_t ota_recovery_exit_post_handler(httpd_req_t *req);    // ota_http_recovery.c
esp_err_t ota_interlock_get_handler(httpd_req_t *req);         // ota_http_recovery.c
esp_err_t ota_boot_guard_reset_post_handler(httpd_req_t *req); // ota_http_recovery.c
esp_err_t ota_boot_guard_status_get_handler(httpd_req_t *req); // ota_http_recovery.c

#endif // OTA_HTTP_INTERNAL_H
