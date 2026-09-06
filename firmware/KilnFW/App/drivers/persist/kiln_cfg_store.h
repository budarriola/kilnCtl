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

#ifdef __cplusplus
extern "C" {
#endif

/* Operator-entered label for a saved kiln config. Matches ZONE_NAME_MAX_LEN's
 * order of magnitude (zones_http.h) -- a config name is shown in a list
 * alongside per-zone names on the LCD's small screen, so it gets no more
 * generous a budget than those already do. */
#define KILN_CFG_NAME_MAX_LEN 23

/* How many named kiln configs this board can have saved at once. Sized
 * against kiln_nvs's budget: each stored entry is
 * (1 name + 1 id + 1 blob_len + ZONES_CONFIG_BLOB_MAX_SIZE=640 blob) bytes,
 * see kiln_cfg_store.c's sizeof(kiln_cfg_store_blob_t) comment for the exact
 * arithmetic -- 8 slots lands comfortably under 5KB total, a small slice of
 * the 64KB kiln_nvs partition that also holds zones_cfg (~380B),
 * rules_cfg, relay_cyc, and run_state (each well under 100B). Bump this if
 * an owner asks for more saved configs than 8, not preemptively -- widening
 * it grows every board's persisted blob whether or not the extra slots are
 * ever used. */
#define KILN_CFG_MAX_COUNT 8

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
 * does not exist. */
bool kiln_cfg_store_delete(int32_t id);

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

#ifdef __cplusplus
}
#endif

#endif // KILN_CFG_STORE_H
