// ota_http.h -- TODO.md 9.4's interlocks and the single cross-processor
// update mutex, PLUS TODO.md 9.5's ESP self-update transfer. The
// AP-password HMAC challenge/response scheme this file used to also
// implement (CommonFW/docs/UPDATE_PROTOCOL.md section 2) was retired
// 2026-09-29 -- WEB_AUTH_PLAN.md item 2b, owner decision "Retire; open when
// login off": route_tier_table.h's ADMIN tier is now the sole gate on every
// route below, on or off, same as every other ADMIN route. The interlock
// precondition check (ota_interlock.h/.c) is still host-tested; this file
// is the ESP-IDF/httpd/FreeRTOS glue around it and the transfer/mutex logic.
//
// Serves POST /api/ota/esp, POST /api/ota/pico, and
// GET /api/ota/pico/status. The Pico path stages the browser upload into
// the `pico_img` partition here (streamed write + running CRC32), then
// hands off to App/drivers/net/ota_pico_relay.c's background task, which speaks
// the actual UPDATE_BEGIN/UPDATE_DATA/UPDATE_END/UPDATE_ABORT/UPDATE_STATUS
// protocol over the isolated link (CommonFW/docs/UPDATE_PROTOCOL.md section
// 4) -- see ota_pico_relay.h's header comment for why that handoff means
// this file does NOT release the update mutex on the success path for the
// Pico endpoint, unlike the ESP endpoint below.
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
#include "heat_interlock.h"
#include "kiln_io.h"
#include "ota_interlock.h"
#include "ota_state.h"
#include "safety_link.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers the OTA/factory-reset/sw-reset routes on the server
// wifi_provision_http.c already started.
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

// The context tags which cross-processor "update in progress" this route
// acts on -- ota_http_context_t itself lives in ota_state.h
// (docs/HW_ABSTRACTION.md item 9), still used for the update-claim mutex
// (ota_http_update_try_begin()/_end()) and by factory_reset.c and friends.
// It USED to also select an HMAC context string and a per-endpoint lockout
// state for the AP-password challenge/response scheme retired 2026-09-29
// (see this file's top comment) -- that part of its job is gone.

// ota_http_authenticate_request() -- the always-true auth stub every
// mutating handler in this split used to call before anything else -- was
// deleted 2026-09-29 along with the AP-password HMAC scheme itself.
// route_tier_table.h's ADMIN tier is the only auth gate on these routes now;
// each handler calls ota_http_get_client_ip() directly for its IP-logging
// line (see ota_http_internal.h).

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
//
// Declared in ota_state.h now (docs/HW_ABSTRACTION.md item 9) --
// ota_pico_relay.c is the non-httpd caller. Included above so existing
// callers of this header are unaffected.

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
// POST /api/ota/{esp,pico} call this first (ADMIN tier only, since the
// AP-password HMAC was retired 2026-09-29).
//
// reason_out/reason_cap: same contract as ota_interlock_check() -- filled
// with a specific, human-readable refusal reason on OTA_INTERLOCK_REFUSED,
// untouched on OTA_INTERLOCK_OK. May be NULL/0.
//
// ack_no_safety_processor: pass through the per-request acknowledgement
// token described by ota_interlock_snapshot_t::operator_ack_no_safety_
// processor. Pass false at any call site that has no way to show the
// operator the warning and collect an answer -- false is the pre-2026-08-22
// behaviour exactly, and the resulting OTA_INTERLOCK_REFUSED_NEEDS_ACK is
// still a refusal to every caller that only tests `!= OTA_INTERLOCK_OK`.
//
// Declared in ota_state.h now (docs/HW_ABSTRACTION.md item 9) --
// factory_reset.c and kiln_cfg_store.c are the non-httpd callers. Included
// above so existing callers of this header are unaffected.

// True when this request carries the "X-Ota-Ack-No-Safety: 1" header -- the
// per-request token an operator's confirmation dialog sets to answer the
// "no safety processor is watching this kiln" warning. Feed the result
// straight into ota_http_check_interlocks()'s first argument.
//
// Lives here, and is read from a header rather than a body field, so that
// every route needing it reads it identically whether its body is a form
// (POST /api/kiln_configs/apply), JSON (POST /api/backup/import), or a raw
// firmware image (POST /api/ota/esp) -- and so a route that checks the
// interlock BEFORE reading its body, as several do, can still see it.
bool ota_http_req_ack_no_safety(httpd_req_t *req);

// Sends the correct refusal for a non-OK ota_http_check_interlocks() result:
// 428 Precondition Required for OTA_INTERLOCK_REFUSED_NEEDS_ACK (the caller
// may retry with the header above after warning the operator), 409 Conflict
// for every other refusal (nothing to acknowledge -- the kiln is busy or
// hot). Body is `reason` as text/plain in both cases.
esp_err_t ota_http_send_interlock_refusal(httpd_req_t *req, ota_interlock_result_t r,
                                          const char *reason);

// --- Heat interlock, the OTHER direction (TODO.md 9.4/ROADMAP.md M8's
// mutual interlock: "updates are not allowed while the heaters are on or a
// profile is running" AND "heating is not allowed during updates") --------
//
// Reads ota_http_update_in_progress() above -- the SAME single
// cross-processor update mutex ota_http_check_interlocks() reads -- into a
// heat_interlock_snapshot_t and calls heat_interlock_check()
// (heat_interlock.h), the pure, host-tested half. Every heat-causing entry
// point (profile_executor_run(), autotune_engine.c's begin_run_locked(),
// kiln_io_owner.c's relay_on_blocked()) calls THIS function rather than
// re-deriving a snapshot itself, so the decision lives in exactly one
// shared predicate, mirroring ota_http_check_interlocks()'s own role for
// the opposite direction.
//
// Returns true (and fills reason_out/reason_cap, same NULL/0-tolerant
// contract as ota_http_check_interlocks()) if a heat-causing action should
// be refused because an update is in progress on either processor; false
// if it may proceed.
//
// Declared in ota_state.h now (docs/HW_ABSTRACTION.md "drivers/
// layering" item 2) -- this is the one query control/owner code needs, and
// they should include ota_state.h directly rather than this whole
// http-layer header. Included above so existing callers of this header are
// unaffected.

// --- POST /api/ota/esp (TODO.md 9.5) -- the ESP's own self-update ---------
//
// ADMIN tier is the only auth gate (AP-password HMAC retired 2026-09-29).
// The client POSTs the raw ESP-IDF image bytes as the body -- no
// multipart/form parser standing between the socket and esp_ota_write(),
// which matters because the body can be over a megabyte and is written to
// flash as it arrives, never buffered whole (see ota_http.c's handler for
// the streaming/verification details: image magic and chip ID are checked
// from the first sizeof(esp_image_header_t) bytes BEFORE esp_ota_begin() is
// called, per UPDATE_PROTOCOL.md section 3's "ESP image magic and chip ID
// checked before esp_ota_begin()").
//
// Registered by ota_http_start() alongside the other OTA routes. No
// separate public entry point is exposed here -- unlike
// ota_http_check_interlocks(), which POST /api/ota/pico also needs to call,
// this transfer logic is specific to the ESP's own image and has no other
// caller.

// Phase of the most recent (or currently in-flight) POST /api/ota/esp
// transfer. Not a push channel (WebSocket/SSE is out of scope this pass,
// see ota_http.c) -- a poller reads this back via ota_http_get_esp_progress()
// below, which TODO.md 9.6's future web page can call on an interval.
//
// ota_http_esp_phase_t and ota_http_get_esp_progress() below are declared in
// ota_state.h now (docs/HW_ABSTRACTION.md item 9) -- ota_pico_relay.c
// is the non-httpd caller. Included above so existing callers of this
// header are unaffected.

// Reads back the in-RAM progress snapshot ota_esp_post_handler() updates as
// it goes. *phase_out and *percent_out (0-100) are always written if
// non-NULL; percent_out is only meaningful while phase is WRITING or
// FINALIZING and otherwise holds whatever value the last transfer reached.
// Safe to call from any task -- both fields are `static volatile`, read
// without a lock (single-writer -- only ota_esp_post_handler() ever writes
// them, and torn reads of a phase enum / uint8_t percentage are not a
// correctness problem the way torn reads of the nonce/lockout state would
// be).

// GET /api/ota/esp/status -- unauthenticated poll-back endpoint pairing
// GET /api/ota/pico/status below, closing the gap ota_http_client.py's
// header comment and mcp_server.py's ota_status() doc comment both flagged:
// ota_http_get_esp_progress() and the persisted ota_record.h "last update"
// blob were real, C-level state with no HTTP route. Returns
// {"phase":"<string>","percent":<0-100>,"last_update":null|{...}} --
// `phase`/`percent` straight from ota_http_get_esp_progress() above (phase
// "idle" if no transfer has been attempted since boot, matching
// OTA_HTTP_ESP_PHASE_IDLE); `last_update` is null when ota_record_load()
// reports no record (no update has ever run this NVS lifetime), or an
// object with ota_record_t's fields (processor, version_before,
// version_after, success, reason, uptime_s) when one exists -- same
// null-not-absent convention dashboard_http.c's safety_temp_c/
// enclosure_temp_c fields already use, not a new style invented here. No
// separate public entry point is exposed here, same "specific to this
// route, no other caller" reasoning as ota_pico_status_get_handler() below.

// --- POST /api/ota/pico, GET /api/ota/pico/status (TODO.md 9.5) -----------
//
// Same auth (ADMIN tier only) and check order (ota_http_check_interlocks()
// -> ota_http_update_try_begin()) as POST
// /api/ota/esp, same raw (non-multipart) byte-stream body. The difference is what happens to the
// body and how the response is shaped:
//
//   1. The body streams into the `pico_img` partition (esp_partition_write(),
//      same 4 KB static-chunk-buffer convention as the ESP path), with a
//      running CRC32 computed alongside it (ota_image_crc.h, which owns the
//      parameterization: standard CRC-32, reflected poly 0xEDB88320, init
//      0xFFFFFFFF, final XOR 0xFFFFFFFF -- the same algorithm SaftyFW's
//      bootloader/crc32.c implements, so the value this ESP sends in
//      UPDATE_BEGIN/_END is byte-for-byte what the Pico's own read-back CRC
//      will compute). Reaching that parameterization through
//      esp_rom_crc32_le() means seeding 0 and applying NO final XOR -- that
//      function performs both inversions itself. Seeding 0xFFFFFFFF and
//      XORing the result, which this path did until 2026-09-18, computes a
//      DIFFERENT variant the Pico rejects for every image; see
//      docs/audits/pico_ota_staged_crc_mismatch_2026-09-18.md.
//   2. Once the whole body is staged, ota_pico_relay_start()
//      (ota_pico_relay.h) is called to kick off the ~35+ second relay as a
//      BACKGROUND task, and this handler responds immediately -- 202
//      Accepted with a small JSON status body -- rather than holding the
//      HTTP connection open for the whole relay (UPDATE_PROTOCOL.md's own
//      "a browser or proxy may still time out" warning). This is a design
//      choice this pass made, not something UPDATE_PROTOCOL.md itself
//      specifies the shape of; a client is expected to poll
//      GET /api/ota/pico/status afterward.
//   3. From the moment ota_pico_relay_start() returns true, this handler no
//      longer owns the update mutex -- see ota_pico_relay.h's header
//      comment for the full ownership-handoff reasoning. On any staging
//      failure BEFORE that call (partition write error, oversized body,
//      etc.), this handler calls ota_http_update_end() itself, exactly
//      once, immediately, and responds with a specific error -- it never
//      leaves the mutex held on a path that does not also start the relay
//      task that would otherwise release it.
//
// GET /api/ota/pico/status takes no auth (same precedent as
// ota_http_get_esp_progress() being a plain unauthenticated getter -- it
// reveals only relay progress/phase, not kiln telemetry, so
// ota_interlock.h's reasoning for gating interlock checks behind auth does
// not apply here) and returns
// {"phase":"<string>","percent":<0-100>,"last_error":"<string>"} from
// ota_pico_relay_get_status(). No separate public entry point is exposed
// here for either handler -- same "specific to this transfer, no other
// caller" reasoning as the ESP path above.

// --- POST /api/ota/esp/rollback -- explicit revert to the previous image --
//
// TODO.md 9.9/UPDATE_PROTOCOL.md section 3's "don't auto-revert a healthy
// new image" is only half the rollback story -- esp_ota_mark_app_valid_
// cancel_rollback() (ota_rollback_confirm_task(), App/main.c) is the side
// that stops an unwanted automatic revert. This route is the OTHER half: an
// explicit, operator/agent-triggered "go back to the previous image right
// now", even though the currently running image is healthy and already
// confirmed. Genuinely rare -- normal recovery from a bad update is the
// bootloader's own automatic PENDING_VERIFY-never-confirmed revert on the
// next boot, which needs no HTTP call at all. This route exists for the
// case an operator wants to revert a currently-RUNNING, already-CONFIRMED
// image deliberately (e.g. the new version is valid but behaves worse in
// practice than the one it replaced).
//
// Same order as POST /api/ota/esp (ota_http_check_interlocks() ->
// ota_http_update_try_begin(OTA_HTTP_CONTEXT_ESP)
// -- reusing the ESP claim slot, not a separate one, since a rollback is
// exactly as disruptive to "another update in flight" as a push would be),
// PLUS one more gate specific to this route: esp_ota_check_rollback_is_
// possible() (esp_ota_ops.h) must return true, or the request is refused
// with a specific "no previous valid image to roll back to" reason rather
// than calling esp_ota_mark_app_invalid_rollback_and_reboot() blind and
// letting IT discover there is nothing to roll back to.
//
// On success: appends an ota_record (processor "esp", version_before the
// currently running image's version, version_after left "" since the
// previous image's version is not read back here, success=true, reason
// "rollback requested"), sends a 200 JSON response, THEN -- from a short-
// lived background task, same pattern factory_reset.c's reboot_task() uses
// so the HTTP response has a chance to actually reach the client's socket
// before the reboot tears the connection down -- calls
// esp_ota_mark_app_invalid_rollback_and_reboot(), which marks the current
// app partition invalid and reboots into the previous one. There is no
// "wait for the reboot" response possible (the reboot itself is the point),
// so this mirrors POST /api/ota/esp's own "report success, the actual
// effect completes after this request" shape rather than holding the
// connection open for something that can never respond.
//
// Registered by ota_http_start() alongside the other OTA routes. No
// separate public entry point exposed here, same "specific to this route,
// no other caller" reasoning as the rest of this file's handlers.

#ifdef __cplusplus
}
#endif

#endif // OTA_HTTP_H
