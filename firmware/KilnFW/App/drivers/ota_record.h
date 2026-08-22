// ota_record.h -- the append-only(*) update record TODO.md 9.5/UPDATE_PROTOCOL.md
// section 4's "One update at a time, and a record of what happened" bullet
// asks for: "timestamp, processor, image SHA-256, version before and after,
// result."
//
// (*) SCOPE DECISION, read before extending this: this module keeps exactly
// ONE record -- the most recent update attempt -- not a history. The doc
// text says "append-only" and lists two ways to get there: an incrementing
// sequence number with NVS key rotation across 2-3 slots (real history), or
// "the last record persists" (a first pass). This is the first pass, and it
// deliberately takes the second, smaller option: mirrors run_state.c's own
// shape exactly (one versioned blob under one NVS key, overwritten on every
// write, load-tolerant on corruption/size-mismatch -- see run_state.c's
// header comment for why that tolerance is the right default on a kiln
// controller), rather than inventing slot-rotation machinery nothing yet
// asks to read back. If a future pass needs "how many updates has this
// board ever had" or "show me the last five", that is a real feature to
// build deliberately -- not something to half-build here by adding a
// sequence counter nobody reads.
//
// UPDATE.2026-08-21: "image SHA-256" is now real, not flagged. `image_sha256_hex`
// below is populated by hashing the image as it streams past -- ota_http.c's
// ota_esp_do_transfer() feeds every byte written via esp_ota_write() (the
// 24-byte header included) through a psa_hash_operation_t
// (PSA_ALG_SHA_256), and ota_pico_do_stage() does the same for every byte
// written into pico_img, finishing the hash once the whole body has
// streamed through and handing the raw 32 bytes to
// ota_pico_relay_start(), which is the one that actually calls
// ota_record_append() for the Pico path (see ota_pico_relay.c's `done:`
// label -- this file's own header comment used to note that NO record was
// ever written for a Pico update at all; that gap closes in the same change
// that adds the hash field, since a hash with nowhere to land would be
// exactly the "computed but not recorded" half-measure this file already
// warns against for the plain fields).
//
// What this hash is FOR, stated plainly: **a record, not a gate.** Nothing
// in this codebase compares it against an expected value and refuses a
// flash on mismatch -- there is no "expected SHA-256" to compare against
// (the operator did not supply one, and neither protocol carries one in
// today's wire format). It exists so that "which exact bytes were written
// during update N" is answerable after the fact from NVS, matching
// CommonFW/docs/UPDATE_PROTOCOL.md's "For a device that can start a fire,
// 'which firmware was running when that happened' should not depend on
// someone remembering." A future pass could turn this into a real gate
// (operator-supplied expected hash, refuse on mismatch) -- that is a
// different, larger feature (a source of truth for "expected" has to come
// from somewhere) and is not what this pass builds.
//
// version_before/version_after ARE real: version_before is
// esp_app_get_description()->version for the running image, version_after
// is esp_ota_get_partition_description() read back from the partition that
// was just written, both filled by ota_http.c's transfer handler, not
// placeholders.
//
// NO WALL CLOCK: same reasoning as run_state.h -- this board has no RTC and
// no guaranteed SNTP, so uptime_s is uptime-at-write, not an absolute
// timestamp. See run_state.h's header comment for the fuller argument.
//
// This file is ESP-IDF-coupled throughout (nvs.h) and is NOT host-tested --
// same as run_state.c and relay_cycles.c, neither of which has a host test
// file either (App/test/build_host_tests.ps1 only lists the modules that
// were deliberately split pure/impure, e.g. ota_auth.c/ota_interlock.c).
// ota_record_fill() below is pure (plain string copies, no ESP-IDF call),
// but splitting it into its own host-buildable file for that alone would be
// process for its own sake -- there is no interesting logic here to catch a
// host test would catch that reading the function wouldn't.
#ifndef KILNCTL_OTA_RECORD_H
#define KILNCTL_OTA_RECORD_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_RECORD_VERSION 2u  // bumped 2026-08-21: image_sha256_hex added below

#define OTA_RECORD_PROCESSOR_MAX     8  // "esp" or "pico" + NUL, with room to spare
#define OTA_RECORD_VERSION_STR_MAX  32  // matches esp_app_desc_t::version's own size
#define OTA_RECORD_REASON_MAX       64  // same precedent as ota_interlock.h's OTA_INTERLOCK_REASON_MAX
#define OTA_RECORD_SHA256_HEX_MAX   65  // 64 hex chars + NUL, "" if hashing was never attempted
                                         // (e.g. staging failed before any bytes streamed)

// The record persisted to NVS. Explicit reserved padding for alignment, same
// discipline run_state.c's header comment insists on for its own record --
// a silent layout change here would look like corruption to the loader
// rather than fail the build, so the size is pinned below.
typedef struct {
    uint8_t  version;                                  // OTA_RECORD_VERSION
    uint8_t  reserved0[3];
    uint32_t uptime_s;                                  // esp_timer_get_time()/1e6 at write time
    char     processor[OTA_RECORD_PROCESSOR_MAX];       // "esp" or "pico", NUL-terminated
    char     version_before[OTA_RECORD_VERSION_STR_MAX]; // running image's esp_app_desc_t.version,
                                                          // "" if esp_app_get_description() had none
    char     version_after[OTA_RECORD_VERSION_STR_MAX];  // the just-written image's version, read back
                                                          // from the partition -- "" if the transfer
                                                          // never reached a fully-written image
    uint8_t  success;                                    // 1 = update completed, boot partition set
    uint8_t  reserved1[3];
    char     reason[OTA_RECORD_REASON_MAX];               // "ok", or the specific failure -- never a
                                                            // generic string, same convention as
                                                            // ota_interlock.h's refusal reasons
    char     image_sha256_hex[OTA_RECORD_SHA256_HEX_MAX]; // lowercase hex, SHA-256 over every byte
                                                            // written (ESP: esp_ota_write() calls,
                                                            // header included; Pico: esp_partition_write()
                                                            // calls into pico_img) -- "" if hashing was
                                                            // never attempted for this record. A RECORD,
                                                            // not a gate -- see this file's header comment.
    uint8_t  reserved2[3];                                 // pads to a 4-byte-aligned total, same
                                                            // explicit-padding discipline as reserved0/1
                                                            // above rather than relying on invisible
                                                            // compiler tail padding
} ota_record_t;

// Fills `out` from the given fields, NUL-terminating and truncating any
// string that doesn't fit rather than overflowing. Pure -- no ESP-IDF call,
// no I/O -- see this file's header comment for why it isn't split out and
// host-tested separately.
// `image_sha256_hex_or_null`: lowercase hex string (any length; truncated to
// fit OTA_RECORD_SHA256_HEX_MAX-1 like every other string field here), or
// NULL/"" if no hash was computed for this record (e.g. staging failed
// before any bytes were hashed) -- stored as "" in that case, never a
// placeholder that could be mistaken for a real hash.
void ota_record_fill(ota_record_t *out, uint32_t uptime_s, const char *processor,
                      const char *version_before, const char *version_after, bool success,
                      const char *reason, const char *image_sha256_hex_or_null);

// Overwrites the single stored record in KILN_NVS_PARTITION (same partition
// run_state.c/relay_cycles.c/zones_http.c already share, each managing its
// own key). "Append-only" here means the record of the LAST update
// persists -- see this file's header comment. Logs loudly on failure (an
// update whose own record didn't save is not otherwise reported anywhere);
// callers should treat this as best-effort and not fail the update over it,
// same as run_state.c's own persist failures.
esp_err_t ota_record_append(const ota_record_t *rec);

// Reads back the single stored record from KILN_NVS_PARTITION. Returns
// ESP_OK and fills `*out` if a record exists and its size matches
// sizeof(ota_record_t) exactly (same "load-tolerant, never trust a
// size-mismatched blob as if it were current" discipline run_state.c's own
// loader uses -- a layout change bumps OTA_RECORD_VERSION and the old blob
// is simply treated as absent, not misread). Returns ESP_ERR_NVS_NOT_FOUND
// if no update has ever run this boot-image's NVS lifetime (nothing has
// ever called ota_record_append()), or any other nvs_* error verbatim on a
// genuine read failure -- callers must not treat a non-ESP_OK return as "an
// update happened but is unreadable," only as "no current record." Never
// itself logs at error level for the NOT_FOUND case (that is the normal,
// expected state on a board that has never been updated), unlike
// ota_record_append()'s failure logging.
esp_err_t ota_record_load(ota_record_t *out);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_OTA_RECORD_H
