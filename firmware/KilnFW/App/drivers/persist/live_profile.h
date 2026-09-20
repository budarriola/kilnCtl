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

/* Top-level decision-layer entry point (plan section 5/6/10's `decide`
 * action), combining the pure rules below into one call per decision kind.
 * Returns true if the decision may proceed -- the caller (pass 2's HTTP
 * handler) then performs whatever kind-specific storage step is its own
 * scope (writing a new user slot for save-as, overwriting the origin slot
 * for overwrite) and, on success, calls live_profile_clear() to retire the
 * working slot. This function performs NO persistence itself and leaves the
 * record/working slot untouched either way; false means refused, with a
 * human-readable reason in err.
 *
 * SAVE_AS: refuses on a name collision (live_edit_name_collides(),
 *   exclude_id 0xFF -- always a fresh slot, never a rename target).
 * OVERWRITE: refuses (a) structurally, checked FIRST, if the record's
 *   origin is a builtin or outside the user id range (live_edit_can_
 *   overwrite()) -- so a caller cannot bypass the structural refusal by
 *   also setting confirm -- and (b) if confirm is false (owner decision
 *   3/section 6: "user profile requires an explicit confirm flag").
 *   candidate_name/confirm are ignored for the kinds that don't use them.
 * DISCARD: always allowed -- plan section 5: "Discard deletes the working
 *   slot," unconditionally.
 */
bool live_edit_decide(live_edit_decision_kind_t action, const live_edit_record_t *rec, const char *candidate_name,
                       bool confirm, const char *(*name_at)(void *ctx, uint8_t id), void *ctx, char *err,
                       size_t err_cap);

/* Plan section 5: abort/trip/reboot handling. "GET /api/profile/live reports
 * pending_decision whenever the record says pending and the executor is not
 * RUNNING." A DONE, a HALTED, a FAULTED trip and a reboot that interrupted
 * the firing all land in "not RUNNING" and all raise the same prompt --
 * deliberately a single uniform condition, never a per-reason special case,
 * so this predicate takes only the two facts that matter and nothing about
 * how the run ended. */
bool live_edit_should_prompt(const live_edit_record_t *rec, bool executor_running);

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

/* Same as live_edit_name_collides() above, plus an explicit `include_builtins`
 * switch (Opus review of 5dd23944, finding 1/BLOCKER). live_edit_name_collides()
 * itself is now a thin wrapper that always passes true, preserving its
 * original behavior for its one remaining caller, live_edit_decide()'s
 * LIVE_EDIT_DECISION_SAVE_AS path. Every USER-SLOT save (profiles_http_save(),
 * profile_post_handler(), and the single- and batch-import paths in
 * profiles_export_http.c/backup_import.c) must call this directly with
 * include_builtins=false: a user profile saved under a builtin's name/code
 * is not a collision -- it is exactly what the "Copy builtin" button
 * produces on purpose, and refusing it also made an existing user slot
 * already named like a builtin permanently unrenamable/uneditable, and
 * refused importing a backup that legitimately contains one. */
bool live_edit_name_collides_ex(const char *candidate_name, const char *(*name_at)(void *ctx, uint8_t id), void *ctx,
                                 uint8_t exclude_id, bool include_builtins, char *err, size_t err_cap);

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
 *   - candidate->segment_count may drop as low as segment_index + 1 (every
 *     future segment deleted -- legal, ends the firing cleanly at the end of
 *     the current segment) but never fewer than that: the implementation
 *     refuses segment_index >= candidate->segment_count, since the running
 *     segment itself can never be deleted out from under the run
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

/* HIGH (review, 2026-09-19): tri-state so the caller (profile_executor.c's
 * reload_live_profile_if_changed()) can tell a DEFINITIVE "nothing to adopt"
 * answer apart from a TRANSIENT one that deserves a retry -- collapsing both
 * into a single bool made every future tick re-poll forever once the
 * definitive case was reached (e.g. live_profile_clear() bumps the
 * generation on discard/save-as/overwrite while leaving no pending record
 * behind: the old bool-false answer looked identical to a transient NVS
 * hiccup, so the caller never dared consume the generation and re-did the
 * malloc + blocking NVS read on the control task every single tick,
 * forever). */
typedef enum {
    LIVE_PROFILE_LOAD_OK = 0,
    /* No record at all, the record isn't pending, or its origin_id does not
     * match expect_origin_id -- all three are definitive facts about the
     * PERSISTED state, independent of anything transient, so the caller may
     * safely consume the generation it observed and stop re-polling for
     * this same edit. */
    LIVE_PROFILE_LOAD_NONE_FOR_ORIGIN,
    /* Pass-3 review fix (2026-09-19): a record IS pending for expect_origin_id
     * and the working blob's STORAGE was reached (hal_kv_open succeeded), but
     * its CONTENT can never be adopted -- the blob is missing (HAL_NOT_FOUND),
     * the wrong length (HAL_INVALID_SIZE), or fails profile_decode_blob().
     * None of these will resolve themselves on a retry, so unlike
     * LIVE_PROFILE_LOAD_TRANSIENT just below, this IS definitive and DOES
     * consume the generation; the caller also logs once at ESP_LOGE and
     * records a refusal so GET /api/profile/live can surface it to the
     * operator, since silently dropping a pending-but-unloadable edit would
     * otherwise look identical to nothing having happened. */
    LIVE_PROFILE_LOAD_PERMANENT,
    /* A record IS pending for expect_origin_id, and the failure to load it
     * is one that MAY resolve on its own next tick: hal_kv_open failed (the
     * partition/namespace itself could not be opened) or the heap allocation
     * for the decode buffer failed. This is not an answer about whether an
     * edit exists, so the caller must NOT consume the generation and should
     * retry on the next tick. */
    LIVE_PROFILE_LOAD_TRANSIENT,
} live_profile_load_result_t;

/* MEDIUM-1 (review): same as live_profile_load_working() but also refuses
 * (LIVE_PROFILE_LOAD_NONE_FOR_ORIGIN, *out untouched) unless the pending
 * record's origin_id equals expect_origin_id -- the pickup-side half of the
 * stale-record guard live_profile_fork() applies on the fork side. The
 * RUNNING executor is the only caller that knows which profile it is
 * actually running, so it is the only caller that can supply this. */
live_profile_load_result_t live_profile_load_working_for_origin(uint8_t expect_origin_id, profile_t *out);

/* MEDIUM-2 (review): true iff a pending record exists whose origin_id is
 * origin_id. profile_executor_run() uses this to decide how to seed
 * s_exec.live_edit_generation at run start -- see its call site and
 * live_profile_generation()'s own doc comment below for why the plain
 * "reboot ends any run" seeding used to be wrong for a warm-started run. */
bool live_profile_has_pending_for_origin(uint8_t origin_id);

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
 * control-tick by profile_executor_live_pickup's caller. Never persisted,
 * and reset to 0 across a reboot -- CORRECTION (MEDIUM-2, review): a reboot
 * does NOT always end the run this counter is tracking, since a firing can
 * warm-start-resume across one (profile_executor_plan_warm_start()). A
 * working copy forked/saved just before that reboot is still on disk with
 * this RAM counter back at 0, so profile_executor_run() must not seed its
 * own baseline straight from this value without first checking
 * live_profile_has_pending_for_origin() -- see that function and
 * profile_executor_run.c's call site. Atomic: bumped from the HTTP task,
 * polled from the executor task. */
uint32_t live_profile_generation(void);

#ifdef __cplusplus
}
#endif

#endif /* LIVE_PROFILE_H */
