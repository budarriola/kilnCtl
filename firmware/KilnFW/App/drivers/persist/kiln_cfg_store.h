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

/* Forward-declared, NOT #included: kiln_cfg_store_capture_expected_pico_
 * fields() below only needs a POINTER to this type in its prototype, and
 * this file is #included (directly or via kiln_cfg_store.c) by more host
 * test executables than any single one of them fakes safety_ceiling_sync.h
 * (which itself pulls in safety_link.h/safety_ceiling_policy.h) -- a hard
 * #include here would force every one of them to grow matching fakes for a
 * whole safety-processor-link surface most of them do not otherwise need.
 * safety_ceiling_sync.h itself declares this same tag as `struct
 * safety_ceiling_expected_param { ... }` (deliberately not anonymous) so
 * this forward declaration and that header's real definition are a
 * compatible redeclaration if a translation unit ever includes both (e.g.
 * main_control_bringup.c). */
struct safety_ceiling_expected_param;
typedef struct safety_ceiling_expected_param safety_ceiling_expected_param_t;

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

/* True if slot `id`'s saved Pico half describes a DIFFERENT hardware shape
 * than what is physically fitted right now (CT count/topology, safety
 * thermocouple presence), writing a human-readable description of the
 * difference into *msg. Read-only: asks the question without applying
 * anything.
 *
 * Exists because docs/KILN_PROFILES_PLAN.md item 5 moved the real apply onto
 * kiln_cfg_swap_apply(), which has no ack_hardware_differs parameter -- so
 * the HTTP apply handler must ask this itself, before dispatching, or
 * section 5.3's hardware-differs gate would be left with nothing reaching
 * it. Exported rather than duplicated at the call site so the compared field
 * list keeps exactly one definition.
 *
 * False for an unknown id, and false for a slot whose Pico half was never
 * captured -- both are refused further down for their own, better-worded
 * reasons, and answering "hardware differs" for a slot with no Pico half to
 * compare would point the operator at the wrong problem. */
bool kiln_cfg_store_slot_hardware_differs(int32_t id, char *msg, size_t msg_cap);

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
 * case.
 *
 * `ack_hardware_differs` (2026-09-16, plan section 5.3 table row 4, same
 * ack-header pattern as `ack_no_safety_processor`): required whenever `id`'s
 * saved safety-processor fields (`ct_installed`/`ct_topology`/
 * `safety_tc_installed`) disagree with what this controller's live
 * safety_cfg_store cache currently reports -- refused, naming the field,
 * unless true. Scope note: the plan's row 4 also names `relay_count`/
 * `thermo_count`, but those are not package fields at all (they are live
 * zones_config_accessors.h queries against this firmware's own compiled-in
 * hardware shape) and are already HARD-refused at import time, before a
 * slot can even be created, whenever a package's own zone assignments
 * exceed them -- this parameter deliberately does not re-cover them. */
bool kiln_cfg_store_apply(int32_t id, bool ack_no_safety_processor, bool ack_hardware_differs,
                          char *reason_out, size_t reason_cap);

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

/* ---- Download / upload (docs/KILN_PROFILES_PLAN.md items 3/4/14) --------
 *
 * Both build on kiln_package.c's envelope codec (kiln_package.h) plus the
 * SAME zones_config_json_validate()/zones_config_export_canonical() this
 * file's own populate_pico_half_and_hash()/kiln_cfg_store_apply() already
 * use -- no bound is duplicated, per docs/KILN_PROFILES_PLAN.md section
 * 5.2 rule 3's "single source of truth" instruction. */

/* Upper bound on kiln_cfg_store_export_package_json()'s output -- see
 * kiln_package.h's KILN_PKG_JSON_MAX_LEN, whose budget this mirrors
 * (nothing added at this layer beyond the envelope itself). Callers heap-
 * allocate this; it is never a stack buffer. */
#define KILN_CFG_EXPORT_JSON_MAX_LEN 6144u

/* Streams slot `id` out as the section 5.1 JSON envelope into `out`
 * (out_cap >= KILN_CFG_EXPORT_JSON_MAX_LEN, heap-allocated by the caller).
 * Refuses (nothing written) a half-package slot (pico_populated == 0) with
 * the same message kiln_cfg_store_get_full_package() already uses for that
 * case -- a config saved before this firmware tracked the Pico half is not
 * "everything", and downloading it would silently produce a package an
 * upload elsewhere would treat as complete. */
bool kiln_cfg_store_export_package_json(int32_t id, char *out, size_t out_cap, size_t *out_len,
                                        char *reason_out, size_t reason_cap);

/* Validates `json` (a section 5.1 envelope) and, only if EVERY check below
 * passes, creates a brand-new slot for it -- never overwrites an existing
 * one, never applies it to the live config (section 5.3: "Upload writes
 * into a new slot only, and never applies"). On any refusal, nothing is
 * written and reason_out names the SPECIFIC reason (envelope-level via
 * kiln_package_import_json(), or one of the checks below):
 *   - hash: the declared pkg_hash must match kiln_package_compute_hash()
 *     recomputed from the decoded ESP half's CANONICAL form (H1) and the
 *     decoded Pico half, in that order (section 5.1's "one CRC over the
 *     whole package").
 *   - ESP-half validity: zones_config_json_validate() on the decoded
 *     candidate -- the single source of truth for "is this a valid
 *     zones_cfg_t", never re-derived here.
 *   - Pico-half validity: every param id must be one this firmware's own
 *     CONFIG_PARAM_TABLE mirror (safety_cfg_store_lookup()) recognises --
 *     an unknown id is refused, never silently skipped (section 5.2 rule 5).
 *   - Compatibility (section 5.2a, a DISTINCT reported failure from the
 *     validity checks above): every zone's thermo channel index must be
 *     < this build's MAX31856_CHANNEL_COUNT, and every `relay_mask` bit
 *     must be < this build's live relay_count -- both name the property,
 *     the package's value, and this controller's value.
 * A package that is merely DIFFERENT (different names/gains/a CT-less
 * package on a CT-equipped controller) is accepted -- only a property this
 * hardware genuinely cannot satisfy is refused (section 5.2a's own
 * "over-strict compatibility check... will be worked around" warning). */
bool kiln_cfg_store_import_package_json(const char *json, int32_t *out_id, char *reason_out,
                                        size_t reason_cap);

/* Restore follow-up (backup_import.c, item 17): same as
 * kiln_cfg_store_import_package_json() but the created slot's name is
 * `name_override` (already trimmed/validated/disambiguated by the caller
 * via kiln_cfg_store_name_would_collide()) instead of the name embedded in
 * `json`. Needed for the "different identity, same name" collision case --
 * the file's own name collides with an existing, different board slot by
 * definition, so calling the plain function would always refuse; this lets
 * the restore path create the slot under its own already-unique name in one
 * step. Pass NULL for name_override to get identical behaviour to
 * kiln_cfg_store_import_package_json() (that function is now a thin
 * wrapper over this one). name_override, if non-NULL, is still length- and
 * charset-validated the same way any other stored name is. */
bool kiln_cfg_store_import_package_json_as(const char *json, const char *name_override,
                                           int32_t *out_id, char *reason_out, size_t reason_cap);

/* Validate-only seam (docs/KILN_PROFILES_PLAN.md item 17, backup-restore
 * follow-up): runs every check kiln_cfg_store_import_package_json() runs
 * (envelope, ESP-half validity, Pico-half param-id validity, hash,
 * foreign-board calibration reset, hardware compatibility) but never
 * creates a slot, writes NVS, or checks quarantine. On success writes the
 * package's NORMALIZED name into name_out, its pkg_schema into
 * *out_pkg_schema, and the pkg_hash a slot created from it would actually
 * carry (post foreign-board reset if applicable) into *out_pkg_hash --
 * exactly the identity backup_import.c's merge/mirror matching needs
 * to compare a file's slot against this board's own
 * kiln_cfg_store_get_package_identity() output, before deciding whether to
 * create, update, or rename anything. Returns false (outputs untouched) on
 * any refusal, with the same specific reason kiln_cfg_store_import_
 * package_json() would give for the identical input. */
bool kiln_cfg_store_validate_package_json(const char *json, char *name_out, size_t name_cap,
                                          uint16_t *out_pkg_schema, uint32_t *out_pkg_hash, char *reason_out,
                                          size_t reason_cap);

/* Section 2.4's auto-save, in its simplest correct form: if a kiln config is
 * currently marked active, re-saves the CURRENTLY LIVE zones config over
 * that same slot (id unchanged, name unchanged) via the existing, already
 * flash-worker-safe kiln_cfg_store_save_current() path -- the identical
 * "export blob, recapture Pico half, recompute pkg_hash, persist" sequence
 * a manual re-save already performs, just triggered automatically. This is
 * why it needs no new NVS write path of its own and adds no new
 * flash_worker_lint.py allowlist entry: it is a new CALLER of an already-
 * reviewed function, not a new write. No-ops (returns true, nothing
 * touched) when no config is currently active -- the common case before an
 * operator has saved a first kiln identity. `reason_out` is filled only on
 * an actual failed re-save (the same failure modes kiln_cfg_store_save_
 * current() itself can hit), never for the active-config-absent no-op. */
bool kiln_cfg_store_autosave_from_live(char *reason_out, size_t reason_cap);

/* 2026-09-15 review (review_divergence_rework_c1d2c526_2026-09-15.md, HIGH 1):
 * true iff the most recent autosave attempt deferred recapturing the active
 * slot's Pico half because an ESP/Pico config divergence (ceiling or
 * standing) was latched at the time -- the ESP half is still saved
 * regardless (see kiln_cfg_store_autosave_from_live()'s own comment for why
 * the two are split). Cleared once the deferred recapture actually
 * completes, via either a later kiln_cfg_store_autosave_from_live() call
 * that finds the divergence cleared, or kiln_cfg_store_recapture_pico_half_
 * confirmed() below. This module never polls its own dirty flag -- it has
 * no task of its own and must not dispatch to the flash worker on a timer
 * it owns -- so an external periodic caller is expected to check this and
 * retry kiln_cfg_store_autosave_from_live() once the divergence-visibility
 * surface (safety_ceiling_sync_is_diverged()/is_standing_diverged())
 * reports clear.
 *
 * That poller is safety_poll_task (safety_link_poll.c's
 * safety_poll_service_pico_half_recapture()), as of the 2026-09-15
 * review follow-up (review_divergence_wiring_60d6552f_2026-09-15.md,
 * HIGH 1). This comment previously named "the web status handler and/or
 * LCD refresh callback ... never safety_poll_task", on the grounds that
 * safety_poll_task's stack is PSRAM-backed and an NVS write from a
 * PSRAM stack aborts. That reasoning applies to calling
 * kiln_cfg_store_autosave_from_live() INLINE, and the poller does not do
 * that: it dispatches onto bx_flash_worker's internal-SRAM stack, which
 * is the sanctioned pattern for exactly this situation (flash_worker.h)
 * and which the httpd/LVGL callers need too -- autosave_from_live()'s
 * call depth already exceeded the LVGL task's own fixed stack budget.
 * The LCD refresh callback was a worse owner for an unrelated and more
 * serious reason: its timer is created inside the home page's lazy
 * build(), so on a boot that never opens that page the poll did not
 * exist at all. */
bool kiln_cfg_store_pico_half_recapture_pending(void);

/* 2026-09-15 review (review_divergence_rework_c1d2c526_2026-09-15.md,
 * MEDIUM 3 / HIGH3 race): for a caller that has JUST performed and confirmed
 * (via readback) its own push of the active slot's expected Pico fields to
 * the real Pico -- e.g. safety_cfg_http.c's commissioning handler,
 * immediately after its own set+commit+confirm sequence -- and therefore
 * needs the active slot's captured Pico half to be recaptured to match RIGHT
 * NOW, without waiting for (or being blocked by) whatever the standing-
 * divergence latch currently reads. Unlike kiln_cfg_store_autosave_from_
 * live(), this function does NOT check safety_ceiling_sync_is_diverged()/
 * is_standing_diverged() before recapturing -- gating it on that latch would
 * deadlock exactly this caller, since the confirmed push this function is
 * meant to record is often the very thing that would clear the divergence.
 * A freshly-confirmed push cannot be laundering a stale/reverted value the
 * way an ordinary unrelated-edit autosave could. Still honors the swap-
 * pending gate (a swap transaction mid-flight is an unrelated reason to
 * defer) and still targets the active slot. Clears the pending-recapture
 * flag above on success. Returns false only on an actual failed re-save
 * (the same failure modes kiln_cfg_store_save_current() itself can hit);
 * callers should check and log the result rather than discard it. */
bool kiln_cfg_store_recapture_pico_half_confirmed(char *reason_out, size_t reason_cap);

/* docs/audits/kiln_profiles_feature_review_2026-09-15.md Defect 1: during a
 * kiln swap (kiln_cfg_swap.c), the ESP half of the INCOMING package is
 * committed via zones_config_import_blob() -> nvs_save() -> this module's
 * own autosave dispatch WHILE s_store.active_id still names the OUTGOING
 * kiln (active_id only moves to the new kiln at the swap's own step 12,
 * after the import already ran) -- so, unguarded,
 * kiln_cfg_store_autosave_from_live() writes the just-imported INCOMING
 * config over the OUTGOING kiln's saved slot. The same is true of
 * rollback()'s re-import of the pre-swap blob: active_id has not yet been
 * restored to previous_active_id when that import's autosave fires, so it
 * would target whatever slot is CURRENTLY marked active (the swap's target)
 * instead of the slot the just-imported blob actually belongs to.
 *
 * This override lets kiln_cfg_swap.c (the only caller) tell the autosave
 * dispatch which slot the config it is about to import via
 * zones_config_import_blob() actually belongs to, for the duration of that
 * one call -- kiln_cfg_store_autosave_from_live() consults this instead of
 * s_store.active_id whenever it is set. `id` may be KILN_CFG_NO_ACTIVE_ID
 * (autosave becomes a no-op, matching "nothing active" semantics) or any
 * valid slot id. Pass KILN_CFG_AUTOSAVE_OVERRIDE_NONE to clear it and go
 * back to reading s_store.active_id normally -- callers MUST clear this
 * again immediately after the guarded call returns (both the success and
 * failure paths), never leave it set across any other code. Not reentrant
 * and not stacked: kiln_cfg_swap.c never nests two guarded imports. */
#define KILN_CFG_AUTOSAVE_OVERRIDE_NONE INT32_MIN
void kiln_cfg_store_set_autosave_target_override(int32_t id_or_none_sentinel);

/* 2026-09-16 -- LOW finding 5 of docs/audits/review_autosave_slot_fix_
 * a93ee77b_2026-09-15.md ("cross-task observation"), re-affirmed by
 * docs/audits/review_autosave_rework_5bc9afb5_2026-09-15.md. The override
 * above is a plain static, read by whichever task happens to run the next
 * autosave, so an UNRELATED task's zones write landing between
 * set_autosave_target_override(id) and the matching clear used to be
 * steered by it: the interloper's autosave wrote the OUTGOING config into
 * the INCOMING slot, and populate_pico_half_and_hash() then certified that
 * slot with a freshly computed, entirely correct pkg_hash -- which is why
 * the corruption is silent rather than hash-detectable. The window is not
 * instantaneous: zones_config_import_blob() commits the RAM config and only
 * then calls nvs_save(), with a blob decode, a JSON validate and an
 * O(groups x zones) inheritance-cycle scan in between. The interloper is
 * confirmed reachable -- uart_bridge_ext_control.c dispatches
 * control_handle_message onto bx_flash_worker, and CONTROL_CMD_SET_ZONE_PID
 * / SET_ZONE_MODEL both end in the zones config's nvs_save().
 *
 * The fix is dispatcher identity, NOT suppression. Suppressing (or
 * deferring) an interloper's save for the duration of the window would
 * trade silent corruption for a silently DROPPED save: an operator's
 * SET_ZONE_PID would report success and not persist. These entry points let
 * the dispatching code state WHICH task dispatched the autosave, and the
 * override is honored only when that task is the same one that set it.
 *
 * Two fixes that look obvious here are DISPROVEN, do not re-attempt either:
 * taking kiln_cfg_store_lock()/s_swap_lock in the reader DEADLOCKS the board
 * (kiln_cfg_store_apply() holds that lock across the import, whose nvs_save()
 * dispatches to the flash worker and blocks awaiting it, so a worker job
 * waiting on the lock deadlocks against its own waiter); and gating on
 * zones_config_generation() advancing is unsound, because an interloping
 * SET_ZONE_PID bumps that counter itself before calling nvs_save().
 *
 * `dispatcher_task` is a FreeRTOS TaskHandle_t, typed void * so callers and
 * host tests need no task.h. It is a HANDLE VALUE, never a pointer into the
 * dispatching frame, so it is equally safe on the awaited dispatch branch
 * and on uart_bridge_ext_run_on_flash_worker()'s on-worker INLINE branch
 * (which is exactly the SET_ZONE_PID path).
 *
 * The plain-named functions above pass NULL and therefore NEVER honor the
 * override. That is what makes the no-`arg` POSTED flash-worker path
 * (uart_bridge_ext_post_on_flash_worker(), safety_poll_task's deferred
 * Pico-half recapture) correct by construction rather than by inspection: a
 * posted job has nowhere to carry a dispatcher, and must not be steered by
 * some other task's in-flight import. */
bool kiln_cfg_store_autosave_from_live_for_dispatcher(void *dispatcher_task, char *reason_out,
                                                      size_t reason_cap);
bool kiln_cfg_store_recapture_pico_half_confirmed_for_dispatcher(void *dispatcher_task, char *reason_out,
                                                                 size_t reason_cap);

/* 2026-09-15 review (review_autosave_rework_5bc9afb5_2026-09-15.md, MEDIUM):
 * kiln_cfg_swap.c moves active_id to target_id right after content is
 * proven live on both sides, BEFORE its own ceiling/arming divergence check
 * runs (and, on that check's failure, active_id stays at target_id with the
 * pending record left at ESP_DONE for boot recovery to retry). An ordinary
 * autosave landing in that window recaptures the Pico's CURRENT (possibly
 * still-old/reverted) live values into target_id's slot, which is exactly
 * the value the divergence check is about to compare against as "expected"
 * -- making them match and clearing the latch before boot recovery ever
 * gets a real answer, so it later "matches", finalizes, and pushes the
 * wrong config to Pico flash. Checking is_diverged()/is_standing_diverged()
 * alone is not enough: this window can occur BEFORE either flag has even
 * been recomputed for the current transaction, or a racing recapture can
 * itself be what clears an already-latched flag. A pending swap record is
 * true for the WHOLE transaction regardless of when divergence is computed,
 * so kiln_cfg_store_autosave_from_live() also consults this seam (set by
 * kiln_cfg_swap.c at bring-up, avoiding a circular #include between the two
 * persist-layer modules) and suppresses autosave whenever it reports true,
 * exactly like an active divergence latch. */
void kiln_cfg_store_set_swap_pending_source(bool (*fn)(void));

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

/* ---- Standing ESP/Pico config-divergence fix (docs/audits/
 * kiln_profiles_feature_review_2026-09-15.md Defect 2) -- the real
 * implementation of safety_ceiling_sync.h's safety_ceiling_expected_pico_
 * fields_fn seam. Kept separate from every call above: this is the ONLY
 * function in this file that exists purely to be wired into another
 * module's seam, never called directly by kiln_cfg_http.c/the LCD/kiln_cfg_
 * swap.c.
 *
 * Decodes the ACTIVE slot's captured Pico half (kiln_cfg_store_get_full_
 * package(), same call kiln_cfg_swap.c's own snapshot step uses, with
 * blob_out=NULL since only the Pico half is needed here) into `out`,
 * skipping unset entries (KILN_PKG_PARAM_FLAG_SET clear) and
 * SAFETY_PARAM_ID_ABS_MAX_TEMP_C (that field stays the existing dedicated
 * comparison in safety_ceiling_sync.c, never duplicated here). Returns 0
 * (no fields written) if there is no active slot, the slot has no populated
 * Pico half (legacy half-package), or `cap` is 0 -- all treated as "nothing
 * to broaden the check with," never an error safety_ceiling_sync.c needs to
 * react to; the abs_max_temp_c-only comparison it already performs
 * unconditionally is the correct fallback for a board that has never saved
 * a kiln-config slot yet. */
size_t kiln_cfg_store_capture_expected_pico_fields(safety_ceiling_expected_param_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif // KILN_CFG_STORE_H
