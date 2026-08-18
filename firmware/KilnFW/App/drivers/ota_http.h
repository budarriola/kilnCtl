// ota_http.h -- CommonFW/docs/UPDATE_PROTOCOL.md section 2's challenge-
// response authentication, wired to a real HTTP endpoint, PLUS TODO.md 9.4's
// interlocks and the single cross-processor update mutex, PLUS TODO.md 9.5's
// ESP self-update transfer. The pure state machines (nonce lifecycle/lockout
// in ota_auth.h/.c, the interlock precondition check in ota_interlock.h/.c)
// are already host-tested; this file is the ESP-IDF/mbedTLS/httpd/FreeRTOS
// glue around all three.
//
// Serves GET /api/ota/challenge and POST /api/ota/esp. This file does NOT
// implement POST /api/ota/pico -- that needs the Pico-image relay through
// the pico_img staging partition and the UPDATE_* frame codecs
// (UPDATE_PROTOCOL.md section 4), a separate, larger, unbuilt piece of work.
//
// The update-in-progress mutex (ota_http_update_try_begin()/_end()) is real
// in-RAM state and IS now acquired -- by ota_esp_post_handler() (ota_http.c),
// on the first byte of a transfer, released on every exit path via a single
// cleanup path (success, failure, or abort all funnel through it -- see
// ota_http.c's transfer handler for why it is written that way rather than
// releasing the mutex at each return). Whichever future pass adds
// POST /api/ota/pico must follow the identical pattern: claim the mutex
// before reading any body, release it exactly once no matter how the
// transfer ends.
#ifndef OTA_HTTP_H
#define OTA_HTTP_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

#include "MAX31856.h"
#include "kiln_io.h"
#include "ota_interlock.h"
#include "safety_link.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers GET /api/ota/challenge on the server wifi_provision_http.c
// already started. Call after wifi_prov_start() (this endpoint reads
// wifi_prov_get_ap_password() indirectly through ota_http_verify_request(),
// so the AP password must already be loaded, though the challenge handler
// itself does not need the password -- only verification does).
//
// `io_or_null`/`thermo_bus_or_null`/`safety_or_null` are the same pointers
// main.c already hands to dashboard_http_start() -- NULL-tolerant, same
// convention as every other driver's *_start(). ota_http_check_interlocks()
// below reads them directly (kiln_io_read()/MAX31856_read_all(), the same
// way dashboard_http.c's /api/status does) rather than through
// profile_executor_get_status(), so the interlock's per-zone temperature/
// heater-commanded checks see the kiln's actual current state whether or
// not a profile happens to be running -- see that function's doc comment
// for why profile_executor's own zones[] array is the wrong source. With no
// safety link this boot, the link is treated as down (safe default, matches
// "no valid data -> refuse" -- see ota_interlock.h), not as an exception.
esp_err_t ota_http_start(kiln_io_t *io_or_null, MAX31856BusClass *thermo_bus_or_null,
                          SafetyLinkClass *safety_or_null);

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

// --- Single cross-processor update mutex (TODO.md 9.4/9.5) ---------------
//
// One update at a time, across BOTH processors -- not one mutex per
// context. "A second browser tab, or an agent racing a human, must be
// refused rather than interleaved" (UPDATE_PROTOCOL.md section 4), and
// TODO.md 9.4 is explicit that starting a Pico update while an ESP update is
// mid-flight (or vice versa) must be refused exactly like starting a second
// ESP update while the first is still running -- there is only one slot.
//
// In-RAM only (like s_nonce/s_lockout_* above) -- an update in progress
// cannot survive a reboot in any state worth resuming anyway (see
// UPDATE_PROTOCOL.md section 5's recovery table: "Power lost mid-transfer:
// old slot still active, nothing changed"), so there is nothing to persist.

// Attempts to claim the mutex for `ctx`. Returns true and records `ctx` as
// the in-progress processor if nothing was already claimed; returns false
// (no state change) if a claim is already held, by either context. Callers
// (TODO.md 9.5's future transfer handlers) must call ota_http_update_end()
// exactly once for every successful claim, on every exit path (success,
// abort, failure, disconnect) -- an unreleased claim would permanently
// refuse every future update until reboot.
bool ota_http_update_try_begin(ota_http_context_t ctx);

// Releases whatever claim is held, if any. Idempotent -- calling this with
// no claim held is a harmless no-op, so a future handler's cleanup path can
// call it unconditionally rather than tracking whether it actually won the
// claim.
void ota_http_update_end(void);

// True if a claim is currently held. If out_ctx is non-NULL and a claim is
// held, writes which context holds it.
bool ota_http_update_in_progress(ota_http_context_t *out_ctx);

// --- Interlocks (TODO.md 9.4, UPDATE_PROTOCOL.md section 1) --------------
//
// Gathers a live snapshot -- profile_executor_get_status() for run state
// only, autotune_engine_is_active(), safety_link_get_status() against the
// pointer passed to ota_http_start(), run_state_boot_record_interrupted(),
// ota_http_update_in_progress() above, and per-zone temperature/heater-
// commanded state read directly via MAX31856_read_all()/kiln_io_read()
// (zones_config_get_thermo_count()/_get_relay_mask() map channels to zones)
// -- into an ota_interlock_snapshot_t and calls ota_interlock_check()
// (ota_interlock.h) -- the pure, host-tested precondition logic lives
// there; this function is purely the ESP-IDF glue that collects the inputs
// it needs.
//
// Deliberately takes no `ctx` parameter: TODO.md 9.4 requires "both update
// paths refused unless..." the same list, with no per-path variant, so
// there is nothing for a context argument to change. (The mutex check
// inside does still correctly refuse a same-context double-start, since
// ota_http_update_in_progress() reports true regardless of which context
// holds the claim -- see ota_interlock_snapshot_t::other_update_in_progress'
// doc comment.)
//
// A future POST /api/ota/{esp,pico} handler should call this AFTER
// ota_http_verify_request() succeeds, not before -- see ota_interlock.h's
// header comment for why (an unauthenticated interlock check would leak
// live kiln telemetry, e.g. "zone 2 is at 340 C", to anyone who can reach
// the endpoint, which is a worse leak than this design already accepts for
// a wrong password).
//
// reason_out/reason_cap: same contract as ota_interlock_check() -- filled
// with a specific, human-readable refusal reason on OTA_INTERLOCK_REFUSED,
// untouched on OTA_INTERLOCK_OK. May be NULL/0.
ota_interlock_result_t ota_http_check_interlocks(char *reason_out, size_t reason_cap);

// --- POST /api/ota/esp (TODO.md 9.5) -- the ESP's own self-update ---------
//
// Wire contract: the client GETs a challenge, computes the MAC per
// ota_http_verify_request()'s doc comment above (context "esp"), then POSTs
// the raw ESP-IDF image bytes as the body with the MAC carried in a request
// header rather than the URL or a form field:
//
//   X-Ota-Mac: <64 hex chars -- the 32-byte HMAC-SHA256, hex-encoded>
//
// A header keeps the MAC out of the URL (proxy/browser-history exposure,
// same reasoning UPDATE_PROTOCOL.md section 2 gives for not sending the
// password itself in a form POST) and leaves the body a pure byte stream --
// no multipart/form parser standing between the socket and
// esp_ota_write(), which matters because the body can be over a megabyte
// and is written to flash as it arrives, never buffered whole (see
// ota_http.c's handler for the streaming/verification details: image magic
// and chip ID are checked from the first sizeof(esp_image_header_t) bytes
// BEFORE esp_ota_begin() is called, per UPDATE_PROTOCOL.md section 3's "ESP
// image magic and chip ID checked before esp_ota_begin()").
//
// Registered by ota_http_start() alongside the challenge handler. No
// separate public entry point is exposed here -- unlike
// ota_http_verify_request()/ota_http_check_interlocks(), which future
// handlers (POST /api/ota/pico) also need to call, this transfer logic is
// specific to the ESP's own image and has no other caller.

// Phase of the most recent (or currently in-flight) POST /api/ota/esp
// transfer. Not a push channel (WebSocket/SSE is out of scope this pass,
// see ota_http.c) -- a poller reads this back via ota_http_get_esp_progress()
// below, which TODO.md 9.6's future web page can call on an interval.
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
// non-NULL; percent_out is only meaningful while phase is WRITING or
// FINALIZING and otherwise holds whatever value the last transfer reached.
// Safe to call from any task -- both fields are `static volatile`, read
// without a lock (single-writer -- only ota_esp_post_handler() ever writes
// them, and torn reads of a phase enum / uint8_t percentage are not a
// correctness problem the way torn reads of the nonce/lockout state would
// be).
void ota_http_get_esp_progress(ota_http_esp_phase_t *phase_out, uint8_t *percent_out);

#ifdef __cplusplus
}
#endif

#endif // OTA_HTTP_H
