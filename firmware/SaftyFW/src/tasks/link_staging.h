// link_staging.h -- the SET_PARAM (0x1C) staging area, kept as a list of
// pending EDITS rather than a whole staged record.
//
// Why edits, not a record (docs/audits/KILNLINK_ROBUSTNESS_AUDIT_2026-10-09.md
// M2): link_task.c used to keep a full `s_staged_config` record, seeded once
// from the committed record and handed whole to config_store_write_ex() on
// COMMIT_CONFIG (0x1D) / APPLY_CONFIG_VOLATILE (0x2D). SET_CONFIG (0x16,
// tc_type) and SET_CT_CAL (0x19) write flash directly and never refreshed
// that copy, so a later COMMIT silently wrote the stale staged values back
// over them -- a committed tc_type or CT calibration reverted with no error.
//
// With an edit list, the record COMMIT writes is always built at commit time
// as "the record that is enforced right now" (config_store_get_full_record())
// plus only the fields the ESP actually staged. A field nobody staged can
// therefore never carry an old value back over a newer committed one. On top
// of that, link_staging_drop_superseded() drops a staged edit whose field a
// later direct write changed, so the later write also wins over an EARLIER
// staged edit of the same field.
//
// Pure: no RTOS, no flash, no globals. link_task.c owns the one instance and
// only touches it from the link task's own thread.
#ifndef SAFTYFW_TASKS_LINK_STAGING_H
#define SAFTYFW_TASKS_LINK_STAGING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config_store.h"
#include "kilnlink/kilnlink_set_param.h"

#ifdef __cplusplus
extern "C" {
#endif

// One slot per config_params id at most (edits are deduplicated by id), so
// this matches config_params.c's own compile-time table bound of 72.
#define LINK_STAGING_CAPACITY 72u

typedef struct {
    kilnlink_set_param_t edits[LINK_STAGING_CAPACITY];
    size_t count;
} link_staging_t;

// Empties the staging area. Called at link_task start, on an ESP boot_id
// change (audit M3: a rebooted ESP never meant to commit what its previous
// boot staged), and after a successful COMMIT/APPLY.
void link_staging_reset(link_staging_t *st);

size_t link_staging_count(const link_staging_t *st);

// Validates `edit` by applying it with config_params_set() to `scratch`
// (which the caller fills with any valid record, normally the committed one;
// it is modified), and on success records it, replacing an earlier edit of
// the same id (last value wins). Returns false, recording nothing, when
// config_params_set() refuses (unknown id, wrong type, out of range).
bool link_staging_stage(link_staging_t *st, const kilnlink_set_param_t *edit,
                        config_store_record_t *scratch);

// Applies every staged edit, in staging order, to `*inout` (the caller
// passes the currently committed record). Returns false if any edit is
// refused -- which cannot happen for an edit link_staging_stage() accepted,
// so the caller treats it as "do not write".
bool link_staging_apply(const link_staging_t *st, config_store_record_t *inout);

// After a successful direct flash write (SET_CONFIG, SET_CT_CAL) that turned
// `before` into `after`: drops every staged edit whose field differs between
// the two (value or set-state). Returns how many edits were dropped.
size_t link_staging_drop_superseded(link_staging_t *st, const config_store_record_t *before,
                                    const config_store_record_t *after);

// Drops the staged edit for `param_id`, if any. Used for the fields a direct
// write explicitly targeted, so the direct write wins even when it wrote the
// value the field already had. Returns true when an edit was dropped.
bool link_staging_drop_id(link_staging_t *st, uint16_t param_id);

// Audit M3: whether a PUSH_CONTEXT starts a new ESP session, so the staged
// edits (which belong to the previous session) must be discarded. True when
// a boot_id was already known and either the ESP's boot_id changed (it
// rebooted) or PUSH_CONTEXT had stopped for at least the context max age
// (`context_gap`: a reboot that happened to draw the same 8-bit boot_id, or
// a link loss, both end the session too). False for the first PUSH_CONTEXT
// after the Pico boots, when nothing can be staged yet.
bool link_staging_new_esp_session(bool prev_known, uint8_t prev_boot_id, uint8_t boot_id,
                                  bool context_gap);

// The peer's last ANNOUNCE_VERSION: its protocol version plus the ESP boot_id it
// was announced under. Grouped in one struct (not loose bool/uint8_t arguments)
// so the announced boot_id cannot be swapped for the context's boot_id at the
// call site. version 0 = unknown.
typedef struct {
    bool known;
    uint8_t boot_id;
    uint16_t version;
} link_peer_announce_t;

// Pico (re)start: forget everything announced.
void link_peer_announce_clear(link_peer_announce_t *peer);
// ANNOUNCE_VERSION received: record version and the boot_id it carries.
void link_peer_announce_record(link_peer_announce_t *peer, uint8_t announce_boot_id,
                               uint16_t version);

// What link_task.c's push_context does on every PUSH_CONTEXT (pure, so the
// trigger is host-tested rather than only read in the FreeRTOS file): on a
// new ESP session, discard the staged edits; on a boot_id CHANGE additionally
// forget the peer's protocol version (set it to 0 = unknown). A rebooted ESP
// may be a different firmware (e.g. rolled back to a v16 image) whose
// ANNOUNCE_VERSION burst this Pico missed, so the previous boot's version must
// not keep selecting 31-byte DIAG / bound-clear behaviour for it. Unknown (0)
// makes every link_frame_*_supported() gate false: legacy-length frames and
// accepting an unbound (3-byte) CLEAR_TRIP -- the same state as a fresh Pico
// boot before any announce, and the ESP's next ANNOUNCE_VERSION restores it.
// The version is forgotten only when boot_id differs from the boot_id the
// version was ANNOUNCED under (peer->known/peer->boot_id): the ESP
// sends ANNOUNCE_VERSION before its first PUSH_CONTEXT, so an announce for
// this very boot_id is current and must be kept.
// A plain context gap with the same boot_id does NOT clear the version: the
// same ESP image is still talking, only silent. Returns true on a new session.
bool link_staging_apply_context_session(link_staging_t *st, link_peer_announce_t *peer,
                                        bool prev_known, uint8_t prev_boot_id, uint8_t boot_id,
                                        bool context_gap);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_LINK_STAGING_H
