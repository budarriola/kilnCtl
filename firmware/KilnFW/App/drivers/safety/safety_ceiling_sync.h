#ifndef SAFETY_CEILING_SYNC_H
#define SAFETY_CEILING_SYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "safety_ceiling_policy.h"
#include "safety_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ESP-side glue between safety_ceiling_policy.c's pure decision logic and
 * the real Pico link (safety_cfg_http.c's set+commit+confirm wrapper,
 * safety_cfg_store.c's cached abs_max_temp_c). See safety_ceiling_policy.h
 * for the invariant and ordering rule this implements; see zones_http_post.c
 * for the two call sites (before and after the zone config commit). */

/* param_id 0x0104 -- abs_max_temp_c, safety_cfg_store.c's
 * SAFETY_CFG_PARAM_TABLE. Duplicated here (not #included from that table)
 * deliberately: this file only needs the one numeric id, not the whole
 * table, and the table's own header comment already treats ids as
 * permanent wire constants safe to reference by literal elsewhere in this
 * codebase (e.g. SAFETY_PARAM_ID_ESTOP_ACTIVE_LEVEL in safety_cfg_http.c
 * follows the identical convention). */
#define SAFETY_PARAM_ID_ABS_MAX_TEMP_C 0x0104u

/* param_id 0x0105 -- tc_type, same table, same duplication convention as
 * above. 2026-09-15 review (review_divergence_fixes_b2e7017f_2026-09-15.md,
 * MEDIUM 4): the commissioning page owns the safety thermocouple type; the
 * ESP only ever READS it back and has no push path for it (owner decision --
 * do not re-propose ESP ownership here). kiln_cfg_store.c's expected-field
 * capture excludes this id from the broadened standing-divergence set for
 * exactly that reason: a param the ESP cannot push can never be corrected by
 * anything on the ESP side, so counting it as a standing mismatch only ever
 * produces a warning the operator has no ESP-side action to resolve, and (via
 * kiln_cfg_store.c's autosave gate) can wedge the deferred Pico-half
 * recapture forever. */
#define SAFETY_PARAM_ID_TC_TYPE 0x0105u

/* Call BEFORE committing a proposed new zones_cfg_t -- i.e. before the
 * line that overwrites the live config (zones_http_post.c's `s_zones.cfg =
 * tmp;`). `link` may be NULL (no safety processor this boot): treated as
 * "no invariant to maintain," always returns true, *out_result = NONE.
 *
 * Returns true iff the caller's commit may proceed (see safety_ceiling_
 * policy_guard_raise()'s own contract -- this is a thin wrapper around it).
 * On false, `reason_out` names why and the caller MUST reject the whole
 * zones POST without applying `new_max_temp_c` or anything else from that
 * submission.
 *
 * `out_refusal_class` (may be NULL) receives the machine-readable
 * classification of a failed raise -- safety_ceiling_refusal_class_t, see
 * safety_ceiling_policy.h. zones_http_post.c/backup_import.c's callers
 * pass NULL (they only need the human-readable `reason_out`); the
 * reconcile path (safety_ceiling_sync_reconcile_on_link_up() below) is the
 * one caller that needs it, to drive its backoff without parsing prose. */
bool safety_ceiling_sync_guard_raise(SafetyLinkClass *link, const float *new_max_temp_c, size_t n,
                                      safety_ceiling_sync_result_t *out_result, char *reason_out,
                                      size_t reason_cap, safety_ceiling_refusal_class_t *out_refusal_class);

/* Call AFTER a zones_cfg_t has already been committed. Best-effort --
 * never blocks, never reports a failure the caller must act on (see
 * safety_ceiling_policy_apply_lower()'s own contract). `link` may be NULL,
 * same as the guard above. */
void safety_ceiling_sync_apply_lower(SafetyLinkClass *link, const float *new_max_temp_c, size_t n,
                                      safety_ceiling_sync_result_t *out_result, char *reason_out,
                                      size_t reason_cap);

/* Reads the Pico's currently-cached abs_max_temp_c (safety_cfg_store.c's
 * live cache -- populated by GET_CONFIG_PAGE fetches, refreshed by every
 * safety_poll_task tick and by this file's own confirm-by-readback writes).
 * Returns false (out_value untouched) if the cache has never held a set
 * value for this param -- e.g. the Pico has never been fetched from this
 * boot, or from a build old enough not to report it. Exposed for the
 * zones/safety config pages to display "the Pico's ceiling currently
 * tracks: N C" alongside the ESP's own zone maximum. */
bool safety_ceiling_sync_get_current_pico_ceiling(float *out_value);

/* 2026-09-10 opus review: guard_raise()/apply_lower() above only run from
 * zones_http_post.c's POST handler, so a board that boots (or reconnects)
 * with `link == NULL` -- and every path that changes the ESP's own zone
 * max_temp_c other than that one POST handler (backup_import.c's restore
 * path being the concrete example that motivated this) -- can leave the
 * ESP's zone ceiling ABOVE the Pico's abs_max_temp_c with nothing to
 * notice or fix it, because guard_raise() treats `link == NULL` as "no
 * invariant to maintain" rather than deferring the check. The ESP's zone
 * config persists in NVS across a Pico that gets replaced, reflashed, or
 * simply comes up later than the ESP's own boot; only the ESP side knows
 * what the ceiling SHOULD be, so reconciliation has to be driven from
 * here, not the Pico.
 *
 * Call this on every safety_poll_task() tick while the link is up (same
 * call site and same level-triggered discipline the removed
 * safety_sync_tc_type() used to follow in safety_link_poll.c -- see the
 * comment there for why this is level-triggered rather than gated on the
 * down->up edge alone: a
 * down_logged-gated call would miss the very first tick after boot, when
 * the link can come up before ever having been observed down). It reads
 * the live zones_cfg_t, builds the same per-zone max_temp_c array
 * zones_http_post.c's handler would, and runs it through safety_ceiling_
 * sync_guard_raise(): a no-op if the Pico's ceiling (kept current by
 * safety_cfg_store.c's own refetch) is already wide enough -- the ordinary
 * case on every tick of a healthy link -- otherwise a real raise+confirm
 * write, logged either way. Never blocks heat or fails the boot: a failed
 * reconcile here is logged and left for the next tick or the next
 * interactive zones POST to retry, same "never silently drop it, keep
 * retrying" convention the removed safety_sync_tc_type() used to follow.
 * Requires zones_config_is_valid() internally, same gate that function
 * used, for the same reason
 * (no fabricated ceiling from a zeroed default config). */
void safety_ceiling_sync_reconcile_on_link_up(SafetyLinkClass *link);

/* 2026-09-22 (opus review, advisory adopted): identical contract to
 * safety_ceiling_sync_reconcile_on_link_up() above, except it takes this
 * file's internal reconcile lock NON-BLOCKING and simply skips the tick
 * (no-op, no log) if a swap-worker reconcile (kiln_cfg_swap.c) is already
 * in flight. Safe to skip a tick: this call is level-triggered, so the very
 * next poll tick re-evaluates the same condition. Use this from
 * safety_link_poll.c's every-tick call site; kiln_cfg_swap.c's one-shot
 * swap/rollback callers should keep using the blocking form above, since
 * they need this call to have actually completed before they read
 * safety_ceiling_sync_is_diverged() right afterward. See
 * safety_ceiling_sync.c's reconcile_on_link_up_impl() doc comment for the
 * full rationale (7a8594d: never hold or wait on a module lock across a
 * producer call this file doesn't own). */
void safety_ceiling_sync_reconcile_on_link_up_nonblocking(SafetyLinkClass *link);

/* 2026-09-22 fix: creates this file's two internal mutexes
 * (s_divergence_state_lock, s_reconcile_lock) once, statically, replacing a
 * lazy-create TOCTOU race that let safety_poll_task and
 * kiln_cfg_swap_worker each install a different mutex for the same lock on
 * dual-core ESP32-S3. Call exactly once, single-threaded, before either
 * task can be running -- main_control_bringup.c's first statement is the
 * real call site, ahead of safety_link_start(). Safe
 * to skip entirely in a host test (leaves both locks NULL, the same
 * no-lock/single-threaded default every existing host test already relied
 * on under the old lazy form). */
void safety_ceiling_sync_init(void);

/* 2026-09-14 owner decision, verbatim: "if a config doesn't land and match
 * on both sides then alarm and dissable heaters." Every reconcile tick
 * above already compares the ESP's target ceiling against the Pico's
 * confirmed one (config_divergence.h's reusable comparator); on a
 * divergence it must ACTIVELY disable heat, not merely log or block a
 * future start.
 *
 * This file deliberately does NOT #include kiln_io_owner.h/profile_
 * executor.h to reach kiln_io_owner_command_all_relays_off()/profile_
 * executor_halt() directly: this .c is compiled into more than one host
 * test executable (test_zones_http.c and others, see build_host_tests.ps1)
 * alongside DIFFERENT fake ecosystems for those subsystems, and a hard
 * dependency here would force every one of them to grow matching fakes for
 * a whole owner-command/profile-executor surface this file does not
 * otherwise need. Instead, the real ESP-side bring-up (main_control_
 * bringup.c, after kiln_io_owner_start()) installs the two real actions
 * once via this setter; a host test that wants to observe the enforcement
 * installs its own tiny counters instead (test_safety_ceiling_sync_
 * divergence.c). Defaults to NULL/no-op -- a build that never calls this
 * setter (every existing host test) sees divergence detected and logged
 * but takes no destructive action, which is the correct, safe default for
 * a test binary that has no real relays to turn off. */
typedef void (*safety_ceiling_disable_heat_fn)(void);
void safety_ceiling_sync_set_disable_heat_hooks(safety_ceiling_disable_heat_fn all_relays_off,
                                                 safety_ceiling_disable_heat_fn halt_run);

/* 2026-09-15 audit fix (docs/audits/kiln_profiles_feature_review_2026-09-15.md
 * Defect 2): the divergence check above compared ONLY abs_max_temp_c, so a
 * Pico reboot that silently reverts to its flash-persisted record (the
 * kiln-profiles swap installs the Pico half via APPLY_CONFIG_VOLATILE only
 * -- RAM-only, never flash-written, per the owner's "Pico must never leave
 * ARMED" rule -- COMMIT_CONFIG is unconditionally refused while ARMED) went
 * undetected on every OTHER commissioning parameter as long as the ceiling
 * happened to still match. This seam broadens the standing (every-tick)
 * comparison to the full record WITHOUT this file taking a hard dependency
 * on kiln_cfg_store.h/kiln_package.h -- same rationale as the disable-heat
 * hooks above: this .c links into host test executables (test_zones_http.c
 * and others) that do not also link kiln_cfg_store.c/kiln_package.c, and a
 * hard #include here would force every one of them to grow matching fakes
 * for a whole persist-layer surface this file does not otherwise need.
 *
 * The real implementation (kiln_cfg_store_capture_expected_pico_fields(),
 * kiln_cfg_store.c) decodes the ACTIVE kiln-config slot's captured Pico half
 * (kiln_pkg_safety_t, itself populated at save/import time by snapshotting
 * safety_cfg_store's cache) into this array -- i.e. "what the last-applied
 * profile expects the Pico to hold." safety_ceiling_sync.c compares each
 * entry against safety_cfg_store's LIVE cache (refreshed automatically by
 * safety_cfg_store_maybe_refetch() whenever the Pico's config_crc changes,
 * e.g. on a reboot that reverts to flash) -- never a second wire round trip
 * of its own. abs_max_temp_c is excluded from this set (it stays the
 * existing dedicated field above, never duplicated) and no kilnlink protocol
 * change is required: GET_CONFIG_PAGE already carries the full record.
 *
 * Wired once, for real, by main_control_bringup.c
 * (safety_ceiling_sync_set_expected_pico_fields_source(kiln_cfg_store_
 * capture_expected_pico_fields)). Defaults to NULL/no-op -- a build that
 * never calls this setter (every existing host test) keeps comparing
 * abs_max_temp_c only, exactly as before this fix. Returns the number of
 * fields written (0..cap); `out_fields`/`cap` follow the same "caller-owned
 * buffer, no allocation" convention as the rest of this codebase's seams. */
#define SAFETY_CEILING_SYNC_MAX_STANDING_FIELDS 96u
/* Tagged (not anonymous) deliberately: kiln_cfg_store.h forward-declares
 * this same tag (`struct safety_ceiling_expected_param;`) rather than
 * #including this whole header, to avoid pulling safety_link.h/safety_
 * ceiling_policy.h into every host-test executable that already includes
 * kiln_cfg_store.h without faking those two -- an anonymous struct would
 * make that forward declaration impossible (two independently-anonymous
 * struct typedefs of the same name are NOT a compatible redeclaration). */
typedef struct safety_ceiling_expected_param {
    uint16_t param_id;
    float value;
} safety_ceiling_expected_param_t;
typedef size_t (*safety_ceiling_expected_pico_fields_fn)(safety_ceiling_expected_param_t *out_fields, size_t cap);
void safety_ceiling_sync_set_expected_pico_fields_source(safety_ceiling_expected_pico_fields_fn fn);

/* True iff the most recent enforcement check found (and is still reporting)
 * a divergence -- i.e. heat is currently being actively held disabled by
 * this mechanism. `reason_out` (may be NULL) receives the same operator-
 * facing message the ERROR log carries. Exposed so readiness_http.c/
 * readiness_gate.c can surface the SAME live verdict this file is already
 * acting on, rather than recomputing it from scratch a second way. */
bool safety_ceiling_sync_is_diverged(char *reason_out, size_t reason_cap);

/* 2026-09-15 review (review_divergence_check_561efa3b_2026-09-15.md, HIGH 2):
 * interim scope choice, stated explicitly -- heat-off enforcement above stays
 * scoped to abs_max_temp_c only (kiln_cfg_store_apply() does not yet push the
 * broadened ~60-field Pico record, so treating a mismatch there as
 * heat-disabling would false-trip on every ordinary apply). A mismatch
 * confined to that broadened field set is still detected every tick and
 * reported here as a WARNING -- true iff currently reporting one -- never as
 * a reason to disable heat. `reason_out` (may be NULL) receives the same
 * operator-facing message the WARN log carries. */
bool safety_ceiling_sync_is_standing_diverged(char *reason_out, size_t reason_cap);

/* 2026-09-15 review (review_divergence_fixes_b2e7017f_2026-09-15.md,
 * MEDIUM 3): the safety_cfg_store_cache_generation() value as of the most
 * recent enforce_ceiling_divergence() evaluation. A caller about to
 * snapshot the live Pico-config cache into a persisted "expected" record
 * (kiln_cfg_store.c's autosave) can compare this against
 * safety_cfg_store_cache_generation() right before capturing: if they
 * differ, a refetch has landed since the divergence latches above were last
 * recomputed, so "not diverged" cannot yet be trusted for the CURRENT cache
 * contents and the capture must be deferred. Starts at 0, matching the
 * cache generation's own "never fetched" starting value, so a board that
 * has never evaluated the check at all reads as "in sync" (0 == 0) rather
 * than spuriously deferring forever. */
uint32_t safety_ceiling_sync_latch_evaluated_generation(void);

#ifdef __cplusplus
}
#endif

#endif /* SAFETY_CEILING_SYNC_H */
