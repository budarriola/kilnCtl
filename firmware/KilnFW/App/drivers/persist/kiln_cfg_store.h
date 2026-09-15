// kiln_cfg_store -- persisted, named "kiln config" slots (owner-report,
// 2026-08-21 follow-up): "save kiln profiles with different relay,
// thermocouple, and PID configs ... survive a programming cycle ... allow
// creating a config from an existing one."
//
// NAMING: this is a KILN CONFIG, never a "profile" -- that word already
// means a firing SCHEDULE elsewhere in this codebase (profiles_http.c,
// profile_executor.c, the 28 built-in schedules). A kiln config is a named,
// saved snapshot of the WHOLE zones_http.c config -- relay/thermocouple
// wiring, PID gains, FOPDT model, control mode, heater timing, every guard
// threshold, cross-zone delta, per-channel tc_type, and safety_tc_type -- for
// every zone. This module owns naming/slots/persistence of that snapshot; it
// does not know or care what the bytes inside a snapshot mean -- that is
// zones_http.h's zones_config_export_blob()/zones_config_import_blob(),
// which alone understand the zones_cfg_t layout, its version history, and
// its field bounds. Keeping that split means this module never has to be
// touched when a zone field is added -- only its opaque blob's max size
// (ZONES_CONFIG_BLOB_MAX_SIZE) might, and that's zones_http.h's own
// compile-time assertion to enforce, not this module's.
//
// Persisted in `kiln_nvs` (0x18D000, 64KB -- see partitions.csv), the SAME
// partition zones/rules/relay_cycles/run_state already live in, for the
// identical reason: TODO.md 8.1/the 2026-08-13 NVS-partition split moved
// every user-set parameter that must survive reprogramming into its own data
// partition specifically BECAUSE flashing this board only ever rewrites the
// bootloader/partition-table/app partitions (verified against the actual
// flash args this project's OTA/JTAG flow uses) -- every `data` partition,
// kiln_nvs included, is untouched by an ordinary reprogram. Do not "helpfully"
// move this module's storage to the default `nvs` partition or a new one:
// that would either lose this reasoning or require re-verifying it from
// scratch for no benefit. See KILN_CFG_STORE_NVS_PARTITION's definition in
// kiln_cfg_store.c for the same note next to the code that actually opens it.
//
// SAFETY: APPLYING a config rewrites relay wiring, thermocouple assignment,
// PID gains, and guard thresholds out from under a potentially-running
// kiln. kiln_cfg_store_apply() below enforces the "no firing running, no
// heaters on" interlock ITSELF, internally, via ota_http_check_interlocks()
// (ota_http.h) -- a caller CANNOT compile this module without linking that
// check in, and cannot bypass it by forgetting to call it first. This was
// deliberately moved from "every caller must remember to check first" (the
// original design) to "the store enforces it, callers may additionally
// pre-check for a better-worded message" after a review pass flagged that a
// convention-only interlock is one refactor away from silently vanishing --
// a future caller (the UART bridge, an MCP tool, a backup restore path, a
// factory-reset routine) that simply never calls
// ota_http_check_interlocks() would otherwise reconfigure a running kiln
// with no error and no warning. kiln_cfg_http.c's apply handler (and the
// LCD's confirm callback) still call ota_http_check_interlocks() themselves
// BEFORE calling kiln_cfg_store_apply() -- that pre-check costs nothing and
// lets each surface show a better message before the user commits to the
// action, but it is redundant, not load-bearing: kiln_cfg_store_apply()'s
// own internal check is the backstop that cannot be skipped. See that
// function's own doc comment.
#ifndef KILN_CFG_STORE_H
#define KILN_CFG_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "kiln_package.h" /* kiln_pkg_safety_t -- kiln_cfg_store_get_full_package()'s pico_out, item 5 */

#ifdef __cplusplus
extern "C" {
#endif

/* Operator-entered label for a saved kiln config. Matches ZONE_NAME_MAX_LEN's
 * order of magnitude (zones_http.h) -- a config name is shown in a list
 * alongside per-zone names on the LCD's small screen, so it gets no more
 * generous a budget than those already do. */
#define KILN_CFG_NAME_MAX_LEN 23

/* How many named kiln configs this board can have saved at once. Raised
 * 8 -> 10 (docs/KILN_PROFILES_PLAN.md item 1, owner request: "save up to 10
 * separate complete kiln configurations") alongside KILN_CFG_STORE_VERSION
 * 2 -> 3, which also added each entry's Pico-half package (kiln_pkg_safety_t,
 * kiln_package.h) and its recorded pkg_schema/pkg_hash identity. See
 * kiln_cfg_store.c's sizeof(kiln_cfg_store_blob_t) comment for the exact
 * arithmetic against kiln_nvs's budget -- 10 slots at the v3 entry size still
 * lands comfortably under kiln_nvs's usable ~56KB. Bump this again only on a
 * further owner request for more than 10, not preemptively -- widening it
 * grows every board's persisted blob whether or not the extra slots are ever
 * used. A v2 (8-slot, ESP-only) blob is migrated in place at boot -- see
 * migrate_store_v2_to_v3() -- so existing saved configs survive this bump. */
#define KILN_CFG_MAX_COUNT 10

/* No config is active (nothing has ever been applied/saved as the starting
 * point, or the previously-active one was deleted/failed validation at
 * boot). Never a real id -- ids start at 1 and only increase, see
 * kiln_cfg_store_save_current()'s doc comment. */
#define KILN_CFG_NO_ACTIVE_ID (-1)

/* One row of GET /api/kiln_configs' listing / the LCD's config picker.
 * is_active mirrors kiln_cfg_store_get_active_id() == id -- included here so
 * a caller building a list doesn't have to cross-reference the two calls
 * itself. */
typedef struct {
    int32_t id;
    char name[KILN_CFG_NAME_MAX_LEN + 1];
    bool is_active;
} kiln_cfg_summary_t;

/* Brings up kiln_nvs (harmless no-op if another module already has -- same
 * "each module brings up its own partition independently" convention as
 * zones_http_start()/rules_http_start()/relay_cycles_init()) and loads the
 * persisted store blob.
 *
 * TODO.md owner-report's "what happens when the active config was deleted or
 * fails validation at boot": this function re-validates the persisted active
 * id's blob via zones_config_import_blob() same as kiln_cfg_store_apply()
 * does at runtime. Three fallback outcomes, safest first:
 *   1. No active id was ever recorded (KILN_CFG_NO_ACTIVE_ID) -- ordinary
 *      first-boot-with-this-feature case. Nothing is applied; the board
 *      simply keeps whatever zones_http_start() already loaded on its own
 *      (this function is called AFTER zones_http_start() -- see main.c's
 *      call order). No half-state is possible because nothing here writes
 *      zones config at all in this branch.
 *   2. The recorded active id no longer exists (its entry was deleted, or
 *      the store blob itself failed to load) -- same as case 1: apply
 *      nothing, log a warning, fall back to whatever zones_http_start()
 *      already has live. The active id is cleared (persisted as
 *      KILN_CFG_NO_ACTIVE_ID) so a future GET /api/kiln_configs correctly
 *      reports "nothing active" instead of a dangling id.
 *   3. The recorded active id exists but its blob fails
 *      zones_config_import_blob() validation (saved by newer firmware, or an
 *      out-of-range field this build now rejects) -- same fallback as case
 *      2: apply nothing (zones_config_import_blob() itself is all-or-
 *      nothing and never half-writes on a validation failure), log the
 *      specific reason, clear the active id.
 * In every fallback the board boots on whatever zones_http_start() already
 * had -- never a half-applied kiln config, and never a hard failure of
 * app_main. This function's own failure return is for "kiln_nvs itself did
 * not come up" (matches every other *_start() in this codebase); the
 * three-outcome active-config handling above happens internally regardless,
 * best-effort, and does not change this function's return value. */
esp_err_t kiln_cfg_store_init(void);

// Read-only dual-write status for GET /api/cfgfs -- see unit_pref.h's
// unit_pref_get_dualwrite_status() for the full contract (fresh re-read of
// both sides every call, no resync side effects). The NVS side is only
// considered valid at the CURRENT KILN_CFG_STORE_VERSION and exact current
// size -- same "wrong version is simply invalid, not migrated in place"
// rule kiln_cfg_store_cfg_fs.h documents for the file side, so a v1-sized
// blob here reports nvs_valid=false rather than silently comparing against
// a migrated copy this function does not perform.
void kiln_cfg_store_get_dualwrite_status(bool *file_valid, uint32_t *file_rev, bool *nvs_valid, uint32_t *nvs_rev,
                                          bool *diverged);

/* How many configs this board can have saved at once (KILN_CFG_MAX_COUNT) --
 * GET /api/kiln_configs' "max_count" field and the LCD's picker both need
 * this to know when "save as new" must instead offer "overwrite an existing
 * one". */
uint8_t kiln_cfg_store_max_count(void);

/* Lists every saved config into out[0..min(count,out_cap)). Returns the
 * number of entries written (<= KILN_CFG_MAX_COUNT), in slot order (not
 * necessarily id order, though in practice the two agree since ids are
 * assigned in save order and slots are never reordered). */
uint8_t kiln_cfg_store_list(kiln_cfg_summary_t *out, uint8_t out_cap);

/* KILN_CFG_NO_ACTIVE_ID if nothing is currently marked active (first boot
 * before any save/apply, or the active config was deleted/failed validation
 * -- see kiln_cfg_store_init()'s fallback description). */
int32_t kiln_cfg_store_get_active_id(void);

/* Copies the saved name for `id` into *out. Same "false means cannot
 * answer" convention as zones_http.h's getters -- false for an id that does
 * not exist, or a NULL/zero-capacity out buffer, leaving *out untouched. */
bool kiln_cfg_store_get_name(int32_t id, char *out, size_t out_cap);

/* Saves the CURRENT LIVE zones config (zones_config_export_blob()) under
 * `name`.
 *
 * id_or_negative < 0: create a brand-new entry (fails with "store full" if
 * KILN_CFG_MAX_COUNT entries already exist). The new entry's id is written
 * to *out_id (may be NULL) and becomes the active id -- a config just saved
 * from the running kiln is, by construction, exactly what's live, so marking
 * it active is correct and not a state change.
 *
 * id_or_negative >= 0: overwrite that EXISTING entry's blob and name in
 * place, keeping its id -- POST .../save's `id=<int>` form field, "save over
 * an existing config" from the contract. Fails (nothing written) if no entry
 * with that id exists. Does not by itself change which id is active (an
 * overwrite of a config that wasn't already active does not make it so);
 * overwriting the currently-active entry leaves it active, now holding the
 * fresh snapshot.
 *
 * Rejects (nothing written) a name that is empty or whitespace-only after
 * trimming, longer than KILN_CFG_NAME_MAX_LEN (measured after trimming), a
 * case-insensitive duplicate of some OTHER saved config's name, or --
 * id_or_negative < 0 only -- if the store is already at KILN_CFG_MAX_COUNT.
 * Overwriting an existing entry (id_or_negative >= 0) under that SAME
 * entry's own current name is explicitly NOT a collision -- "re-save this
 * config from the current setup" must keep working. Leading/trailing
 * whitespace is trimmed from `name` before it is compared OR stored --
 * " spare" and "spare" are the same name both ways, and neither is stored
 * with the stray space. reason_out/reason_cap (may be NULL/0) are filled
 * with a specific reason on failure. */
bool kiln_cfg_store_save_current(const char *name, int32_t id_or_negative, int32_t *out_id,
                                  char *reason_out, size_t reason_cap);

/* Copies an EXISTING saved config (`src_id`) into a brand-new entry under
 * `name` -- the "create a new one from an existing one" the owner explicitly
 * asked for. Does NOT touch the live zones config or which id is active;
 * this only duplicates a stored blob under a new id/name, exactly like a
 * file "save as" -- the operator is expected to kiln_cfg_store_apply() the
 * clone (or edit the live config and re-save over it) afterward if they want
 * it live. *out_id (may be NULL) receives the new entry's id.
 *
 * Fails (nothing written) if src_id does not exist, `name` is empty/overlong
 * after trimming, `name` case-insensitively collides with some OTHER saved
 * config's name (a clone always creates a brand-new entry, so there is no
 * "own name" to exempt the way save-over-by-id has), or the store is already
 * at KILN_CFG_MAX_COUNT. Same trim-before-compare-and-store rule as
 * kiln_cfg_store_save_current(). reason_out/reason_cap (may be NULL/0)
 * filled on failure. */
bool kiln_cfg_store_clone(int32_t src_id, const char *name, int32_t *out_id, char *reason_out,
                          size_t reason_cap);

/* Makes saved config `id` the live zones config -- POST .../apply.
 *
 * SAFETY, see this header's own top comment: this function itself calls
 * ota_http_check_interlocks() (ota_http.h) FIRST, before touching
 * find_index_by_id() or anything else, and refuses (nothing applied) unless
 * it returns OTA_INTERLOCK_OK -- reusing that predicate rather than writing
 * a second, possibly-diverging one, per TODO.md's explicit instruction that
 * heat_interlock.c answers the OPPOSITE question (may heat run during an
 * update) and is not a substitute. This check cannot be skipped by a caller
 * that forgets to pre-check -- it is not a convention, it is inside this
 * function's own body. kiln_cfg_http.c's apply handler additionally
 * pre-checks the same predicate itself, purely to produce a better-worded
 * 409 before the user commits to the action; that pre-check is redundant
 * with, not a substitute for, this one.
 *
 * All-or-nothing beyond the interlock too: delegates the actual
 * field-by-field re-validation and atomic commit to
 * zones_config_import_blob() (zones_http.h) -- see that function's doc
 * comment for the full version-refuse/re-validate/commit-or-nothing
 * contract. Marks `id` active only on a successful import. Fails (nothing
 * applied, active id unchanged) if the interlock refuses, `id` does not
 * exist, or zones_config_import_blob() refuses the stored blob; reason_out/
 * reason_cap (may be NULL/0) carry the specific reason in every failure
 * case. */
bool kiln_cfg_store_apply(int32_t id, bool ack_no_safety_processor, char *reason_out,
                          size_t reason_cap);

/* Deletes saved config `id`. If it was the active one, the active id is
 * cleared to KILN_CFG_NO_ACTIVE_ID -- the LIVE zones config is untouched
 * (deleting the saved snapshot a running kiln happened to be started from
 * does not change what is currently live; it only means there is no longer a
 * saved config to reapply later). Returns false (nothing changed) if `id`
 * does not exist.
 *
 * SAFETY (docs/audits/kiln_profiles_robustness_2026-09-14.md finding H5):
 * deleting the ACTIVE config is refused outright, via the SAME interlock
 * backstop kiln_cfg_store_apply() uses (ota_http_check_interlocks(), NOT
 * heat_interlock.c -- see this header's top comment) -- checked INSIDE this
 * function, first, so a caller cannot bypass it by forgetting to pre-check,
 * exactly the same "backstop, not a convention" reasoning apply's own
 * comment gives. This did not exist before this fix: deleting the active
 * slot mid-firing cleared active_id (harmless to the live zones config
 * today, but under docs/KILN_PROFILES_PLAN.md's model it also destroys the
 * one stored copy of the Pico half the Pico is currently running from RAM,
 * and the divergence check's recorded reference). Refuses (nothing
 * deleted) if `id` is the currently active config OR the interlock itself
 * refuses (a running firing, heaters commanded, kiln hot); reason_out/
 * reason_cap (may be NULL/0) carry the specific reason in both refusal
 * cases, same convention as kiln_cfg_store_apply(). Deleting a NON-active
 * config is unaffected by this change (no interlock applied -- there is
 * nothing live to protect). */
bool kiln_cfg_store_delete(int32_t id, bool ack_no_safety_processor, char *reason_out, size_t reason_cap);

/* Renames saved config `id` in place -- does not touch its blob, its id, or
 * which config is active. Same name-length/emptiness/case-insensitive-
 * duplicate rejection as kiln_cfg_store_save_current(), with `id` itself
 * excluded from the duplicate check -- a no-op rename (or one that only
 * changes case/whitespace of the name `id` already has) is allowed, not a
 * collision. Returns false (nothing changed) if `id` does not exist or
 * `name` is invalid (including a duplicate of another entry's name). */
bool kiln_cfg_store_rename(int32_t id, const char *name);

/* True if `name` (after the same trim/case-fold normalization every write
 * path applies) would collide with some saved config OTHER than
 * `exclude_id` -- exposed purely so a caller (kiln_cfg_http.c's rename
 * handler) can produce a specific "name already used" error BEFORE calling
 * kiln_cfg_store_rename(), the same "pre-check for a better message, backed
 * by the store's own authoritative check" pattern kiln_cfg_store_apply()'s
 * callers use for the interlock. Returns false (not a collision) if `name`
 * itself is invalid -- that is a separate failure this predicate does not
 * report. */
bool kiln_cfg_store_name_would_collide(const char *name, int32_t exclude_id);

/* Package identity for slot `id` (docs/KILN_PROFILES_PLAN.md items 1/2/12) --
 * *out_pico_populated (may be NULL) is false for a slot saved by pre-v3
 * firmware and never re-saved since (kiln_cfg_entry_t's own "pico_populated"
 * comment, kiln_cfg_store_internal.h); *out_pkg_schema and *out_pkg_hash (either
 * may be NULL) are both 0 whenever *out_pico_populated is false -- neither is
 * a real identity until a Pico half has actually been captured. Returns
 * false (all outputs left untouched) if `id` does not exist. Read-only:
 * exists so a future caller (the divergence check, upload/download, the
 * picker UI) does not have to reach into this module's internal blob layout
 * to answer "does this slot have a Pico half, and what is its hash" --
 * exactly the same "public accessor over the opaque internal struct"
 * convention kiln_cfg_store_get_name() already establishes. */
bool kiln_cfg_store_get_package_identity(int32_t id, bool *out_pico_populated, uint16_t *out_pkg_schema,
                                          uint32_t *out_pkg_hash);

/* docs/audits/kiln_profiles_robustness_2026-09-14.md finding H3:
 * true iff the store failed to load at boot (a wrong-size blob, a version
 * this build cannot use or migrate, or a v1/v2-sized blob whose content
 * disagrees with its claimed size) and is therefore QUARANTINED -- every
 * save/clone/apply/delete is refused (rename too, though it has no
 * reason_out of its own to report why) until kiln_cfg_store_quarantine_
 * clear() is called. `reason_out`/`reason_cap` (may be NULL/0) receive the
 * specific reason. Returns false (reason_out untouched) when the store is
 * NOT quarantined -- including the ordinary "never saved anything" case,
 * which is not an error at all. The live zones config on this board is
 * UNAFFECTED by quarantine; only the SAVED-SLOTS store is refused writes. */
bool kiln_cfg_store_is_quarantined(char *reason_out, size_t reason_cap);

/* The ONE way out of quarantine: discards whatever bytes are on flash
 * (never salvaged, never heuristically parsed -- see the audit doc's own
 * "would not build a repair wizard" note) and starts a fresh, empty,
 * valid store. Requires `confirm_discard=true` -- the whole point is that
 * an operator, not the firmware, decides to accept the loss; passing false
 * is refused with a reason explaining the confirmation is required, and
 * changes nothing. On success (returns true): the NVS blob key and its
 * `cfg` LittleFS mirror are both overwritten with an empty, current-
 * version store (via the same nvs_save_store() every other mutation
 * uses), the quarantine is cleared, and what was discarded is logged.
 * Fails (returns false, nothing changed) if `confirm_discard` is false, or
 * if the store was not actually quarantined (nothing to clear -- calling
 * this on a healthy store is refused rather than silently accepted as a
 * no-op, since a caller doing so has almost certainly misread the store's
 * state). `reason_out`/`reason_cap` (may be NULL/0) carry the specific
 * reason on failure. */
bool kiln_cfg_store_quarantine_clear(bool confirm_discard, char *reason_out, size_t reason_cap);

/* ---- Swap-transaction support (docs/KILN_PROFILES_PLAN.md item 5, the
 * two-processor apply transaction; implemented in kiln_cfg_swap.c, a
 * SEPARATE module from this one so the transaction's crash-recovery state
 * machine and its host tests don't bloat this file's own already-large
 * test surface). These four calls exist ONLY for kiln_cfg_swap.c -- every
 * ordinary caller (kiln_cfg_http.c, the LCD) keeps using
 * kiln_cfg_store_apply() above, which is unchanged and still the whole
 * story for an ESP-only apply. */

/* Copies stored entry `id`'s raw ESP blob into `blob_out` (cap must be >=
 * ZONES_CONFIG_BLOB_MAX_SIZE) and its length into *out_len, and its
 * captured Pico half into *pico_out (may be NULL to skip). Returns false
 * (nothing written) if `id` does not exist, or if `pico_out` is non-NULL
 * and the slot's pico_populated is false (H17 -- kiln_cfg_swap.c must
 * refuse a half-package outright rather than treat an unpopulated Pico
 * half as "empty but fine"; reason_out names this explicitly so the
 * refusal at the transaction layer reads the same as kiln_cfg_store_
 * apply()'s own H17 message). */
bool kiln_cfg_store_get_full_package(int32_t id, uint8_t *blob_out, uint16_t cap, uint16_t *out_len,
                                     kiln_pkg_safety_t *pico_out, char *reason_out, size_t reason_cap);

/* Records which id the CURRENTLY LIVE zones config corresponds to, without
 * importing or validating anything itself -- used only by kiln_cfg_swap.c's
 * finalize/rollback steps, which have ALREADY committed the live config via
 * zones_config_import_blob() directly (not through kiln_cfg_store_apply(),
 * so the transaction can order the ESP commit, the Pico round trip, and the
 * ceiling ordering the way docs/KILN_PROFILES_PLAN.md section 4.2
 * specifies, which kiln_cfg_store_apply()'s own fixed internal order does
 * not support). `id` may be KILN_CFG_NO_ACTIVE_ID. Persists immediately.
 * Returns false (nothing changed) if `id` is neither KILN_CFG_NO_ACTIVE_ID
 * nor an existing entry -- this is a bookkeeping call, never a way to point
 * active_id at a nonexistent slot. */
bool kiln_cfg_store_set_active_id_raw(int32_t id, char *reason_out, size_t reason_cap);

/* H6 (docs/audits/kiln_profiles_robustness_2026-09-14.md): this module has
 * one mutex protecting `s_store`, taken internally by every existing
 * mutating call above. kiln_cfg_swap.c is a SECOND writer (the swap
 * transaction reads/snapshots/finalizes this same store around a Pico round
 * trip that can take hundreds of milliseconds) and must serialize its own
 * store-touching steps against an ordinary save/clone/rename/delete/apply
 * landing mid-transaction -- but must NEVER hold this lock across that Pico
 * round trip (project_flash_worker_reentrancy's sibling hazard: this
 * codebase has a standing rule, and a bricked-board incident behind it,
 * against holding a module lock across a producer/blocking call --
 * project_screen_idle_brick_real_cause). Take/release ONLY around the
 * snapshot-R and finalize/rollback-commit steps in kiln_cfg_swap.c; never
 * across safety_cfg_http_apply_param_pairs()/safety_cfg_store_refetch().
 * kiln_cfg_store_generation() lets a caller notice a store mutation that
 * happened during its own unlocked window (e.g. an operator deleting or
 * re-saving the very slot mid-swap) -- kiln_cfg_swap.c re-checks it right
 * before finalizing and refuses to finalize (rolling back instead) if it
 * moved, rather than blindly writing active_id over whatever is there now. */
void kiln_cfg_store_lock(void);
void kiln_cfg_store_unlock(void);
uint32_t kiln_cfg_store_generation(void);

#ifdef __cplusplus
}
#endif

#endif // KILN_CFG_STORE_H
