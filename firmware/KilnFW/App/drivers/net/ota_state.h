// ota_state.h -- the OTA state/interlock query API non-http layers are
// allowed to depend on: "is an update in progress on either processor, and
// if so refuse this heat-causing action", plus the interlock/auth/progress
// accessors non-httpd callers need. Split out of ota_http.h (an http-layer
// header) per docs/HW_ABSTRACTION.md's "drivers/ layering" items 2 and
// 8 -- item 2 moved ota_http_heat_blocked_by_update() here for
// kiln_io_owner.c/profile_executor.c/profile_executor_run.c; item 9 moved
// the rest of this file's declarations here for autotune_engine_internal.h,
// factory_reset.c, kiln_cfg_store.c, ota_pico_relay.c and
// zones_current_sweep_task.c, none of which register httpd routes -- except
// the three httpd_req_t-shaped accessors (ota_http_authenticate_request(),
// ota_http_req_ack_no_safety(), ota_http_send_interlock_refusal()), which
// moved back to ota_http.h since every caller of those already holds a live
// httpd_req_t and includes esp_http_server.h anyway. ota_http.h includes
// this header so existing http-layer callers are unaffected; every
// implementation stays in ota_http.c, which already owns the update-mutex/
// auth/progress state these read.
#ifndef OTA_STATE_H
#define OTA_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ota_interlock.h"

#ifdef __cplusplus
extern "C" {
#endif

// The context a client authenticates for -- CommonFW/docs/UPDATE_PROTOCOL.md
// section 2 step 2's literal "esp"/"pico"/"esp-rollback"/"recovery" HMAC
// context string, and also which of the four independent per-endpoint
// lockout states applies. See ota_http.c for the full per-value rationale
// (this type moved here from ota_http.h per item 9 above; the doc comment
// stayed there since it also documents ota_http_verify_request(), which is
// httpd-only and did not move).
typedef enum {
    OTA_HTTP_CONTEXT_ESP = 0,
    OTA_HTTP_CONTEXT_PICO,
    OTA_HTTP_CONTEXT_ESP_ROLLBACK,
    OTA_HTTP_CONTEXT_RECOVERY_EXIT,
    OTA_HTTP_CONTEXT_FACTORY_RESET,
    OTA_HTTP_CONTEXT_PICO_ROLLBACK,
    // POST /api/sw_reset (sw_reset_http.c) -- its own context, not a reuse of
    // OTA_HTTP_CONTEXT_FACTORY_RESET, even though both sit in the same
    // "danger zone"/reset-menu family and share the same interlock gate: a
    // MAC signed for the destructive erase-and-reboot route must not double
    // as authorization for the non-destructive reboot-only route, and a
    // wrong-password guess against one must not burn the other's 3-strikes
    // budget. Same reasoning as every other *_ROLLBACK/_RECOVERY_EXIT split
    // above.
    OTA_HTTP_CONTEXT_SW_RESET,
} ota_http_context_t;

// --- Heat interlock, the OTHER direction (TODO.md 9.4/ROADMAP.md M8's
// mutual interlock: "updates are not allowed while the heaters are on or a
// profile is running" AND "heating is not allowed during updates") --------
//
// Reads the single cross-processor update mutex (ota_http.h's
// ota_http_update_in_progress()) into a heat_interlock_snapshot_t and calls
// heat_interlock_check() (heat_interlock.h), the pure, host-tested half.
// Every heat-causing entry point (profile_executor_run(), autotune_engine.c's
// begin_run_locked(), kiln_io_owner.c's relay_on_blocked()) calls THIS
// function rather than re-deriving a snapshot itself, so the decision lives
// in exactly one shared predicate, mirroring ota_http_check_interlocks()'s
// own role for the opposite direction.
//
// Returns true (and fills reason_out/reason_cap, same NULL/0-tolerant
// contract as ota_http_check_interlocks()) if a heat-causing action should
// be refused because an update is in progress on either processor; false
// if it may proceed.
bool ota_http_heat_blocked_by_update(char *reason_out, size_t reason_cap);

// --- Interlock/progress accessors non-httpd callers need (item 9) --------
// Moved verbatim from ota_http.h; see ota_http.c for the implementation and
// ota_http.h's git history for the original, fuller doc comments (still
// accurate -- only the declaration site moved).
//
// The three httpd_req_t-shaped accessors that used to live here
// (ota_http_authenticate_request(), ota_http_req_ack_no_safety(),
// ota_http_send_interlock_refusal()) moved back to ota_http.h -- every
// caller of those needs esp_http_server.h anyway (they hold a live
// httpd_req_t), so keeping them here bought nothing but forced this header,
// which callers WITHOUT a request in hand (kiln_io_owner.c, profile_executor.c
// et al) also include, to drag in esp_http_server.h transitively.

// Releases whatever update-mutex claim is held, if any. Idempotent.
void ota_http_update_end(void);

// Gathers a live interlock snapshot and calls ota_interlock_check()
// (ota_interlock.h) -- see that header for OTA_INTERLOCK_* result meanings.
// reason_out/reason_cap: filled with a specific, human-readable refusal
// reason on OTA_INTERLOCK_REFUSED, untouched on OTA_INTERLOCK_OK. May be
// NULL/0. ack_no_safety_processor: pass ota_http_req_ack_no_safety()'s
// result through, or false where there is no request to read it from.
ota_interlock_result_t ota_http_check_interlocks(bool ack_no_safety_processor, char *reason_out,
                                                 size_t reason_cap);

// Phase of the most recent (or currently in-flight) POST /api/ota/esp
// transfer -- see ota_http_get_esp_progress() below.
typedef enum {
    OTA_HTTP_ESP_PHASE_IDLE = 0,   // no transfer has been attempted since boot
    OTA_HTTP_ESP_PHASE_VERIFYING,  // reading/checking the image header, before esp_ota_begin()
    OTA_HTTP_ESP_PHASE_WRITING,    // streaming the body into the OTA partition
    OTA_HTTP_ESP_PHASE_FINALIZING, // esp_ota_end() / esp_ota_set_boot_partition()
    OTA_HTTP_ESP_PHASE_DONE,       // the last transfer succeeded; boot partition set
    OTA_HTTP_ESP_PHASE_FAILED,     // the last transfer failed, or was refused/aborted
} ota_http_esp_phase_t;

// Reads back the in-RAM progress snapshot ota_esp_post_handler() updates as
// it goes. *phase_out and *percent_out (0-100) are always written if
// non-NULL. Safe to call from any task (single-writer, `static volatile`).
void ota_http_get_esp_progress(ota_http_esp_phase_t *phase_out, uint8_t *percent_out);

#ifdef __cplusplus
}
#endif

#endif // OTA_STATE_H
