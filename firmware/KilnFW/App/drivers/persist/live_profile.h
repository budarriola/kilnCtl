/* live_profile.h -- docs/LIVE_PROFILE_EDIT_PLAN.md, pass 1 (backend only, no
 * HTTP routes -- those are pass 2/3).
 *
 * Owns the structural live-edit working slot (plan section 4): a profile
 * living OUTSIDE the 0..PROFILES_MAX_COUNT-1 user range and outside
 * PROFILES_MAX_COUNT itself, so a live edit can always fork even with every
 * user slot full. Deliberately NOT folded into profiles_http.c's
 * s_profiles/used_bitmap array -- that array is sized exactly
 * PROFILES_MAX_COUNT (a fixed uint8_t bitmap), and profiles_http.c/
 * profiles_edit_http.c/profiles_page.html are named in the plan's own
 * section 8 sequencing note as contested with other in-flight work, so this
 * pass adds a small, independent persistence path instead of widening that
 * array. It reuses the SAME wire format and validator as the ordinary
 * profile store (profile_encode_current_blob()/profile_decode_blob()/
 * profiles_validate_candidate() -- all from profiles_http_internal.h) so
 * there is still exactly one encoder/decoder/validator in the codebase, per
 * the owner's "one implementation" requirement; only the storage KEY is
 * new, not the format.
 *
 * Alongside the working profile, one small record (`live_edit_v1`, plan
 * section 4) tracks the pending decision -- forked from which origin, is
 * that origin a builtin (structural, not a flag -- see plan section 6), and
 * whether a decision (save-as/overwrite/discard) is still owed. Written at
 * FORK time, not at firing end, so an abort/trip/reboot before the operator
 * ever answers the prompt loses nothing (plan section 5) -- the working
 * profile and the pending record are both already durable the moment the
 * first edit is accepted.
 *
 * Split into two halves on purpose:
 *   - PURE functions (record encode/decode, the overwrite/name-collision
 *     decision rules, the edit-window rule) take/return plain structs, touch
 *     no hal_kv/FreeRTOS state, and host-test directly with no fakes beyond
 *     a caller-supplied name table.
 *   - PERSISTENCE functions (live_profile_fork(), live_profile_save_*(),
 *     live_profile_load_*(), live_profile_clear()) go through hal_kv and are
 *     exercised in host tests via the same real hal_kv host backend
 *     (hwAbstraction/host/fake_kv.c) test_profiles_http.c already links.
 */
#ifndef LIVE_PROFILE_H
#define LIVE_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "profiles_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The structural working slot's id -- outside 0..PROFILES_MAX_COUNT-1 (the
 * user range) and outside PROFILES_MAX_COUNT (today 8) itself, exactly as
 * plan section 4/owner decision 1 describes. Never compared against
 * anything that assumes PROFILES_MAX_COUNT stays 8 -- if the owner later
 * raises it, this id moves with it automatically. */
#define LIVE_EDIT_WORKING_SLOT_ID PROFILES_MAX_COUNT

/* Bump on any incompatible layout change to live_edit_record_t. A
 * version-mismatch blob is DISCARDED, never migrated (plan section 10's
 * "run_state precedent": losing a pending-decision record costs one
 * prompt, mis-parsing one would act on a wrong origin id). */
#define LIVE_EDIT_RECORD_VERSION 1u

typedef enum {
    LIVE_EDIT_DECISION_SAVE_AS = 0,
    LIVE_EDIT_DECISION_OVERWRITE = 1,
    LIVE_EDIT_DECISION_DISCARD = 2,
} live_edit_decision_kind_t;

/* One persisted record, `live_edit_v1` (12 characters -- inside the
 * 15-character NVS key limit `check_nvs_key_length.ps1` enforces). */
typedef struct {
    uint32_t version;
    uint8_t  origin_id;         /* the profile id (user 0..7, or a builtin id
                                  * >= PROFILE_BUILTIN_ID_BASE) this working
                                  * copy was forked from */
    uint8_t  working_id;        /* always LIVE_EDIT_WORKING_SLOT_ID today;
                                  * carried explicitly rather than assumed so
                                  * a future second concurrent live edit (not
                                  * in scope) would not silently reinterpret
                                  * an old record */
    uint8_t  origin_is_builtin; /* structural fact captured at fork time, plan
                                  * section 6 -- read back at decide time so an
                                  * overwrite refusal never depends on the
                                  * request re-asserting its own origin */
    uint8_t  pending;           /* nonzero: a decision (save-as/overwrite/
                                  * discard) is still owed */
    char     origin_name[PROFILE_NAME_MAX_LEN + 1];
} live_edit_record_t;

/* ---- pure: record encode/decode ------------------------------------------
 * Fixed-size wire format, version-prefixed like every other blob this
 * codebase persists. Returns bytes written / true on success; a decode of a
 * wrong-version or short blob returns false and leaves *out untouched
 * (caller's existing default -- typically "no pending record" -- stands,
 * matching the run_state precedent in this header's own doc comment). */
size_t live_edit_record_encode(const live_edit_record_t *rec, void *out, size_t cap);
bool live_edit_record_decode(const void *blob, size_t len, live_edit_record_t *out);

/* ---- pure: decision-layer rules -------------------------------------------
 * Asserted here, independent of any HTTP handler (plan section 11: "asserted
 * at the decision layer, not only the handler" -- profiles_http.c is one of
 * the few files that links into the host suite, so a rule that only exists
 * inside a target-build-only handler is a rule no host test can defend). */

/* Plan section 6: structural, not a flag. Refuses (returns false, naming the
 * reason) whenever origin_is_builtin is set OR origin_id names a builtin id
 * range (>= PROFILES_MAX_COUNT) -- both read from the persisted record,
 * never from anything a request could assert about itself. */
bool live_edit_can_overwrite(const live_edit_record_t *rec, char *err, size_t err_cap);

/* Plan owner decision 3: name collisions on save-as are refused,
 * case-insensitive and whitespace-trimmed. `name_at(ctx, id)` returns the
 * name of existing profile `id`, or NULL if `id` is not in use -- a small
 * seam so this stays pure and host-testable with a fake table rather than
 * pulling in profiles_http.c's live s_profiles array (which this pass does
 * not touch). `exclude_id` skips one id (the profile a rename targets;
 * pass 0xFF/none for a fresh save-as). Returns true (refused) with a
 * human-readable reason in `err` on a collision. NOTE: this does not (yet)
 * reuse kiln_cfg_store.c's static normalize_name()/name_collides() the plan
 * asks for -- that file was left untouched this pass (its own edit is out
 * of this pass's assigned scope), so this is a small, deliberately
 * duplicated equivalent pending that widening; flagged in
 * docs/LIVE_PROFILE_EDIT_PLAN.md as follow-up work. */
bool live_edit_name_collides(const char *candidate_name, const char *(*name_at)(void *ctx, uint8_t id), void *ctx,
                              uint8_t exclude_id, char *err, size_t err_cap);

/* Plan section 2: the edit-window rule. Compares `running` (the profile
 * currently executing, unmodified) against `candidate` (the operator's
 * proposed edit) at the given `segment_index`:
 *   - zone_mask must be identical (never editable while running)
 *   - every segment at index < segment_index must be byte-identical
 *   - the segment AT segment_index may differ in target_c/ramp_c_per_hr/
 *     dwell_min only -- seg_kind (and every io_* field) must be identical
 *   - segments beyond segment_index, and on/off rules whose segment_index is
 *     also beyond it, are unconstrained
 *   - on/off rules whose segment_index is <= the running segment_index must
 *     be byte-identical
 *   - candidate->segment_count may be <= segment_index (every future segment
 *     deleted -- legal, ends the firing cleanly at the end of the current
 *     segment) but never inserts/reorders anything at or before it
 * Returns true (refused) with the offending segment/reason in `err` on the
 * first violation found. Pure -- no zone-config lookup, no FreeRTOS. */
bool live_edit_check_window(const profile_t *running, const profile_t *candidate, uint8_t segment_index, char *err,
                             size_t err_cap);

/* ---- persistence (hal_kv, ESP + host hal_kv backend) ---------------------- */

/* Idempotent fork (plan section 10's future POST /api/profile/live/fork
 * contract, minus the HTTP handler -- that is pass 2): if a pending record
 * already exists, loads and returns the EXISTING working copy/record rather
 * than re-forking over it (a second fork must not discard an edit already
 * in flight). Otherwise copies `origin` verbatim into the working slot,
 * writes the live_edit_v1 record with pending=1, and returns the fresh
 * working copy. Fails (false, `err` set) if the write cannot be verified by
 * read-back (see live_profile_save_record()'s own doc comment) -- nothing
 * about the API expresses "no free slot" any more (plan section 4's
 * original refusal reason), since the structural slot always exists; the
 * one remaining failure mode is a storage failure. */
bool live_profile_fork(uint8_t origin_id, bool origin_is_builtin, const char *origin_name, const profile_t *origin,
                        profile_t *out_working, live_edit_record_t *out_record, char *err, size_t err_cap);

/* Overwrites the working slot's profile content in place (does not touch the
 * record) -- the accept-time write pass 2's HTTP handler will call after
 * validating a candidate. Bumps the generation counter live_profile_
 * generation() polls so a caller (the executor tick, once wired) picks the
 * edit up on the next tick. */
bool live_profile_save_working(const profile_t *p, char *err, size_t err_cap);
bool live_profile_load_working(profile_t *out);

/* Read-back verified: writes, then reads the value back and compares before
 * reporting success -- same discipline boot_guard_mark_healthy()'s
 * verify_persisted_count() fix uses (never trust a write's return code
 * alone, CLAUDE.md's "logging unchecked success" class). Returns false
 * (and leaves the previous persisted record, if any, in place as far as
 * this call can tell) if the read-back does not match. */
bool live_profile_save_record(const live_edit_record_t *rec, char *err, size_t err_cap);
bool live_profile_load_record(live_edit_record_t *out);

/* Clears the pending record and erases the working profile blob -- the
 * DISCARD decision (plan section 5's "Discard deletes the working slot"),
 * and also what a completed save-as/overwrite calls once its own write
 * lands, since the working slot's job for that edit is done. Idempotent:
 * calling it with nothing pending is not an error. */
bool live_profile_clear(char *err, size_t err_cap);

/* RAM-only generation counter, same shape as zones_config_generation() --
 * bumped by live_profile_fork()/live_profile_save_working(), polled once per
 * control-tick by profile_executor_live_pickup's caller. Never persisted:
 * a reboot ends any run, so there is nothing for a stale counter to pick up
 * across one, matching zones_config_generation()'s own precedent. */
uint32_t live_profile_generation(void);

#ifdef __cplusplus
}
#endif

#endif /* LIVE_PROFILE_H */
