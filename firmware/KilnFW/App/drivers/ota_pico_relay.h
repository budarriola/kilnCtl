// ota_pico_relay.h -- TODO.md 9.5's Pico-image relay: drives the RP2040
// safety processor through CommonFW/docs/UPDATE_PROTOCOL.md section 4's
// UPDATE_BEGIN/UPDATE_DATA/UPDATE_END/UPDATE_ABORT/UPDATE_STATUS flow, from
// an image already staged in the `pico_img` partition by ota_http.c's
// POST /api/ota/pico handler (App/drivers/ota_http.c). The receiving side
// (SaftyFW's src/tasks/update_task.c) is fully built and frozen; this file
// is the ESP-side sender that talks to it, matching its wire contract
// byte-for-byte (see the header comments on safety_link.h's
// safety_link_update_status_t and this file's own UPDATE_BEGIN packer below
// for the exact layouts, both mirrored from SaftyFW since this project
// cannot #include a separate repository's headers).
//
// --- Ownership of the cross-processor update mutex is ASYMMETRIC vs the ESP
// self-update path (ota_http.c's ota_esp_post_handler()/ota_esp_do_transfer())
//
// The ESP path is synchronous: the HTTP handler holds ota_http.h's
// ota_http_update_try_begin(OTA_HTTP_CONTEXT_ESP) claim for the whole ~1-2 MB
// transfer and releases it itself, in ota_esp_do_transfer()'s single cleanup
// block, before the HTTP response is sent.
//
// The Pico path cannot work that way: UPDATE_PROTOCOL.md's own honest
// throughput math puts a full relay at ~35 s minimum (115200 baud, 248 B
// chunks, stop-and-wait's problems aside), and holding an HTTP worker/socket
// open that long risks a browser or proxy timeout for no benefit -- the
// image is already safely staged in flash by the time the relay starts, so
// there is nothing left that needs the HTTP connection. So:
//   - ota_http.c's POST /api/ota/pico handler claims the mutex
//     (ota_http_update_try_begin(OTA_HTTP_CONTEXT_PICO)) BEFORE reading any
//     body byte, exactly like the ESP path.
//   - Once the body is fully staged, it calls ota_pico_relay_start() (this
//     file) and returns an HTTP response immediately -- see ota_http.c's
//     handler for the exact status code/body chosen.
//   - ota_pico_relay_start() does NOT take ownership of releasing the mutex
//     on failure to even START (returns false): the CALLER (ota_http.c)
//     still owns cleanup in that case, since no background task exists yet
//     to do it.
//   - Once ota_pico_relay_start() returns true, ownership of "when to call
//     ota_http_update_end()" moves to the background relay task THIS module
//     starts. That task calls it exactly once, on every exit path (success,
//     refusal, timeout, internal failure) -- see ota_pico_relay.c's relay
//     task function for the single exit point that guarantees this.
//
// This is a real, documented difference from the ESP path's structure, not
// an oversight -- ota_http.c's own header comment for its POST /api/ota/pico
// handler cross-references this file for the same reason.
//
// --- What this file does NOT do ---
//
// - Does not read the browser upload or write pico_img -- that is
//   ota_http.c's job (streaming write + running CRC32 as the body arrives).
//   This module only ever READS pico_img, via esp_partition_read(), never
//   holding the whole image in RAM.
// - Does not implement SAFETY_CMD_ANNOUNCE_REBOOT -- out of scope this pass
//   (TODO.md 9.5, does not exist anywhere in CommonFW/SaftyFW yet).
// - Does not push progress over a WebSocket/SSE channel -- a poller reads
//   ota_pico_relay_get_status() back, mirroring ota_http.h's
//   ota_http_get_esp_progress() for the ESP path.
// - Alarm-TEXT suppression while relaying: DONE, 2026-08-17. The relay task
//   calls safety_link_set_update_in_progress(link, true) right before it can
//   make the link go quiet, and false again at this file's single `done:`
//   exit point, success or failure alike (see relay_task_fn()). That flag
//   only changes which ESP_LOG* line safety_link.c's safety_update_health()
//   emits for an already-down link -- SAFETY_FAULT_SRC_SAFETY_LINK keeps
//   asserting exactly as before, untouched, per UPDATE_PROTOCOL.md's "the
//   Pico update deliberately trips the liveness rule" section. There is
//   still no GUI-facing alarm surface in this codebase at all (dashboard_http.c
//   exposes no fault-source text today, only the raw bit via
//   safety_link_get_status()) -- this wiring reaches as far as a real
//   operator-facing surface currently exists (the ESP_LOG* line an operator
//   watching the serial console or log stream would see), and will need no
//   further change once a GUI fault-text surface is eventually built, since
//   it would presumably read from the same safety_link_get_status() bits.
#ifndef KILNCTL_OTA_PICO_RELAY_H
#define KILNCTL_OTA_PICO_RELAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_partition.h"

#include "safety_link.h"

#ifdef __cplusplus
extern "C" {
#endif

// Looks up (and caches, since a partition table entry cannot change without
// a reboot) the `pico_img` staging partition (partitions.csv: data, subtype
// ESP_PARTITION_SUBTYPE_DATA_UNDEFINED (0x06), 896K at 0x610000). Returns
// NULL if the running partition table has no such entry -- should never
// happen on a board built from this repo's partitions.csv, but this is
// checked rather than assumed (a board flashed with an older/different
// table is a real possibility during bring-up). Shared by both halves of
// the relay: ota_http.c's POST /api/ota/pico handler uses this to WRITE the
// staged image, this module's relay task uses the SAME pointer to READ it
// back chunk-by-chunk -- one partition lookup, not two independent ones
// that could theoretically disagree about which partition "pico_img" means.
const esp_partition_t *ota_pico_img_partition(void);

typedef enum {
    OTA_PICO_RELAY_PHASE_IDLE = 0,      // no relay has been attempted since boot
    OTA_PICO_RELAY_PHASE_BEGIN,         // UPDATE_BEGIN sent, waiting for the Pico's first reply
    OTA_PICO_RELAY_PHASE_ERASING,       // Pico confirmed ERASING, waiting for RECEIVING
    OTA_PICO_RELAY_PHASE_SENDING,       // streaming UPDATE_DATA, first (sequential) pass
    OTA_PICO_RELAY_PHASE_RETRANSMIT,    // resending chunks the Pico's gap reports named
    OTA_PICO_RELAY_PHASE_FINISHING,     // UPDATE_END sent, waiting for COMPLETE/FAILED/ABORTED
    OTA_PICO_RELAY_PHASE_DONE,          // the last relay succeeded
    OTA_PICO_RELAY_PHASE_FAILED,        // the last relay failed, was refused, or was aborted
} ota_pico_relay_phase_t;

#define OTA_PICO_RELAY_ERROR_MAX 128

typedef struct {
    ota_pico_relay_phase_t phase;
    uint8_t percent; // 0-100; meaningful from SENDING onward, otherwise the last value reached
    char    last_error[OTA_PICO_RELAY_ERROR_MAX]; // "" while IDLE, "ok" on success, a specific
                                                    // reason otherwise -- never a generic string,
                                                    // same convention as ota_interlock.h/ota_record.h
} ota_pico_relay_status_t;

// Human-readable name for a phase, for logging/JSON -- never NULL, an
// unrecognised value (should be unreachable) maps to "unknown".
const char *ota_pico_relay_phase_str(ota_pico_relay_phase_t phase);

// Starts the background relay task (App/drivers/ota_http.c's POST
// /api/ota/pico handler is the only intended caller). `link` must already
// be safety_link_start()'d. `image_length`/`image_crc32` describe the bytes
// already sitting in pico_img[0, image_length) -- this function does not
// re-validate them against the partition beyond a size check.
//
// `version16_or_null`: copied verbatim (space-padded if shorter than 16
// bytes, truncated if longer) into UPDATE_BEGIN's 16-byte `version` field
// (SaftyFW's src/update/image_header.h). NULL means "use this module's own
// placeholder" -- see ota_pico_relay.c's header comment on
// OTA_PICO_RELAY_DEFAULT_VERSION for why there is no better value available
// this pass (the raw uploaded body carries no filename or embedded version
// string this code parses).
//
// `image_sha256_or_null`: 32 raw bytes (the caller, ota_http.c's
// ota_pico_do_stage(), already hashed the staged image with PSA while
// writing it into pico_img), or NULL if hashing failed/was skipped. This
// module hex-encodes it (or leaves the field "" if NULL) into the
// ota_record_t it appends once the relay reaches a terminal state -- see
// relay_task_fn()'s `done:` label. Copied (not retained by pointer): the
// caller's buffer does not need to outlive this call.
//
// Returns true if the task was started -- from that point on, THIS
// function's caller must NOT call ota_http_update_end() itself; the relay
// task now owns that (see this header's own top comment on the mutex
// ownership asymmetry vs the ESP path). Returns false (no task started, the
// mutex claim is untouched, the CALLER remains responsible for releasing
// it) if a relay is already running, `link`/`image_length` are invalid, or
// the FreeRTOS task could not be created.
bool ota_pico_relay_start(SafetyLinkClass *link, uint32_t image_length, uint32_t image_crc32,
                           const char *version16_or_null, const uint8_t image_sha256_or_null[32]);

// Reads back the current/last relay status. Always writes *out (a zeroed,
// PHASE_IDLE, empty-last_error struct before the first ever call). Safe
// from any task: single-writer (the relay task, and only while it is the
// sole task running -- ota_pico_relay_start() refuses a second concurrent
// start), guarded internally the same lightweight way
// ota_http.c's `volatile` progress fields are, except this status includes
// a string field (last_error), which a bare `volatile` cannot make
// atomic -- see ota_pico_relay.c for the small critical-section guard this
// uses instead.
void ota_pico_relay_get_status(ota_pico_relay_status_t *out);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_OTA_PICO_RELAY_H
