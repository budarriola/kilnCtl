// Import side of backup_http.c's split (2026-09-04, ROADMAP.md M15's
// 1500-line item) -- see backup_http_internal.h for the full split map.
// MOVE-ONLY: backup_import_apply() (the two-pass validate-then-commit JSON
// parser) and POST /api/backup/import's upload handler, unchanged apart
// from widening backup_import_post_handler() from `static` to file-scope-
// internal linkage (declared in backup_http_internal.h) so backup_http.c's
// route table can name it, and from calling the JSON reader helpers under
// their new `backup_json_` prefix now that they live in backup_json.c/.h
// (see that header's own comment on why the rename happened even though no
// collision was found for those particular names).
//
// ---- Minimal JSON reader, import side --------------------------------------
//
// The reader itself (backup_json_skip_ws/_value, backup_json_obj_find,
// backup_json_arr_first/_next, backup_json_field_num/_opt_num/_str) now
// lives in backup_json.c/.h; see that header's comment for why this file
// hand-rolls JSON parsing at all rather than depending on a library.
//
// ---- Import: validate everything, THEN apply -------------------------------
//
// Two full passes over `body`. Pass 1 (backup_import_apply()'s first half)
// parses every profile and every zone entry into local candidate arrays and
// validates every one of them -- range bounds via
// profiles_http_get_bounds(), the same zone_mask-must-select-a-configured-
// zone and ramp-vs-zone-ceiling-feasibility rules profiles_http_save()
// itself enforces (duplicated here deliberately, not called speculatively,
// so a failure on profile 6 of 8 is caught before profile 0 is ever
// written) -- and refuses the WHOLE import on the first problem found,
// writing nothing. Pass 2 (backup_import_apply()'s second half) runs only
// if pass 1 fully succeeded, and commits every candidate via the same
// profiles_http_save()/zones_config_set_*() calls the UI's own pages use.

#include "backup_http.h"
#include "backup_http_internal.h"
#include "backup_json.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "MAX31856.h"
#include "kiln_cfg_store.h" /* KILN_PROFILES_PLAN.md item 17 follow-up -- kiln_configs[] restore */
#include "ota_http.h" /* ota_http_check_interlocks() -- see backup_http.h's header comment */
#include "profiles_http.h"
#include "safety_ceiling_sync.h" /* 2026-09-10: a restored backup can raise max_temp_c same as a POST -- see
                                  * the guard immediately before the zone-tuning commit loop below. */
#include "safety_cfg_write.h" /* 2026-09-16: safety_cfg_write_apply_pairs() -- the same stage/COMMIT_CONFIG/
                                * forced-read-back-confirm path safety_cfg_http.c's own POST handler uses,
                                * reused here so the Pico's i_normal_a[0..2] restore is never trusted on a
                                * bare ACK -- see the push immediately after the ceiling guard below. */
#include "zones_config_accessors.h"
#include "zones_config_json.h" /* relay_type/ease_off_window_mult/approach_rate_cap/
                                 * error_band_c/rate_band_c_per_s setters --
                                 * 2026-09-16 backup-round-trip-gap closure */
#include "zones_http_internal.h" /* s_hw_safety, zone_normals_set() */

/* ---- backup_import_apply()'s two big candidate arrays: heap, not stack ----
 *
 * 2026-09-08 check_httpd_task_stack_budget.py measured this function's own
 * frame ($constprop$0) at 4656 B -- the single largest frame in the deepest
 * httpd_worker path (backup_import_post_handler, 7952 B of an 8192 B stack,
 * 240 B free). The two locals below, `candidates[PROFILES_MAX_COUNT]`
 * (profile_candidate_t, ~428 B each = ~3.4 KB) and
 * `zone_candidates[MAX31856_CHANNEL_COUNT]` (zone_candidate_t, smaller but
 * still non-trivial), together account for essentially all of that frame.
 * Moved to heap (PSRAM preferred, same MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT
 * convention backup_import_post_handler already uses for its own `body`
 * buffer just below) via the backup_import_apply() wrapper: it allocates
 * both, calls the actual two-pass validate-then-commit logic (renamed
 * backup_import_apply_locked(), otherwise byte-for-byte identical, EVERY
 * `return false`/`return true` untouched), then frees both on every path.
 * An allocation failure here happens strictly BEFORE either array is
 * touched or any profile/zone config is read -- it returns false with an
 * "out of memory" err_msg (caller sends a clean 400, not a 500 or a panic;
 * see backup_import_post_handler's `if (!ok)` branch), so it is
 * indistinguishable from any other pass-1 validation refusal: nothing is
 * ever half-applied. The two-pass validate-then-commit split itself, and
 * every check inside it, is unchanged. */
typedef struct {
    bool has_id;
    uint8_t id;
    profile_t p;
} profile_candidate_t;

typedef struct {
    uint8_t index;
    float kp, ki, kd;
    bool has_model;
    float k_dc, tau_s, dead_time_s;
    bool has_tc;
    uint8_t tc_type;
    /* Version 2 (2026-08-21): everything else zones_http.h gained a
     * setter for this pass. Each has its own has_* flag, same
     * optional-per-field convention as has_model/has_tc above -- see
     * backup_json_field_opt_num()'s comment for why "absent" must not be an
     * error. */
    bool has_name;
    /* +2, not +1: backup_json_field_str() silently truncates to cap-1 bytes with
     * no way to tell the caller it did so, so a buffer sized exactly
     * ZONE_NAME_MAX_LEN+1 could never actually observe an overlong name
     * -- it would just come back pre-truncated to a fit, and the "name
     * too long" check below would be permanently unreachable (dead)
     * code. Sizing one byte larger than the real limit means ANY name
     * whose true length exceeds ZONE_NAME_MAX_LEN still results in
     * strlen(name) == ZONE_NAME_MAX_LEN+1 after the copy (truncated to
     * fit this buffer, but still detectably over the limit), so the
     * length check that follows can actually fire. See this pass's
     * report for the negative test that proves it does. */
    char name[ZONE_NAME_MAX_LEN + 2];
    bool has_relay_mask;
    uint8_t relay_mask;
    bool has_thermo_mask;
    uint8_t thermo_mask;
    bool has_ct_mask;
    uint8_t ct_mask;
    bool has_cal;
    float cal_offset_c;
    bool has_ramp;
    float max_ramp_c_per_hr;
    bool has_sanity;
    float sanity_rate_c_per_min;
    bool has_mode;
    uint8_t control_mode;
    /* max_temp_c/min_temp_c are a bundled pair (zones_config_set_temp_limits()
     * takes both together) -- either both are present in the import or
     * neither is, same "all-or-nothing" rule TODO already applies to
     * model_k_dc/tau_s/dead_time_s just above. */
    bool has_temp_limits;
    float max_temp_c, min_temp_c;
    /* heater_window_ms/min_on_ms/min_off_ms -- same bundled-pair rule. */
    bool has_heater_cfg;
    float heater_window_ms, heater_min_on_ms, heater_min_off_ms;
    /* The 8 guard-threshold overrides -- same bundled-pair rule, all 8
     * or none (zones_config_set_guard_thresholds() takes all 8
     * together). */
    bool has_guard;
    float guard_wrong_dir_window_s, guard_wrong_dir_rate_c_per_min, guard_off_settle_s,
        guard_runaway_rate_c_per_min, guard_runaway_margin_c, guard_drift_period_s,
        guard_sensor_fault_debounce_ticks, guard_frozen_window_s;
    bool has_cross_zone;
    float cross_zone_max_delta_c;
    /* Version 3 (2026-08-30): PID_EXPANSION_PLAN.md Phase 2/4's four new
     * fields. The three floats follow the ordinary optional-field
     * convention (absent -> not written, so an older board's stored value
     * survives an older-format import untouched). settings_source is
     * different -- see this struct's field and backup_import_apply()'s
     * own comment: an ABSENT settings_source must still be written as
     * ZONE_SETTINGS_SOURCE_CUSTOM on a fresh zone, so it carries no
     * has_settings_source flag at all; instead settings_source itself is
     * pre-seeded to ZONE_SETTINGS_SOURCE_CUSTOM by memset+explicit
     * default below, and is simply overwritten when the key is present. */
    bool has_fuzzy_strength;
    float fuzzy_strength_pct;
    /* Version 4 (2026-08-30, same-day follow-up): per-cell presence and
     * value, not a bundled pair -- see BACKUP_FORMAT_VERSION's 3->4
     * comment. has_coupling_cell[j]/coupling_row[j] track neighbor j
     * independently, so an import can update just the cells a backup
     * actually has values for (a version-3 body has at most one). */
    bool has_coupling_cell[MAX31856_CHANNEL_COUNT];
    float coupling_row[MAX31856_CHANNEL_COUNT];
    /* ZONES_CFG_VERSION 11->12 (DATA PLUMBING pass): the tau/dead-time
     * siblings of coupling_row above, same per-cell presence tracking.
     * NOT gated on has_coupling_cell[j] -- an export may in principle
     * carry a coeff without a matching tau/L key (or vice versa) from a
     * hand-edited body, and each is independently optional/preserved. */
    bool has_coupling_tau_cell[MAX31856_CHANNEL_COUNT];
    float coupling_tau_row[MAX31856_CHANNEL_COUNT];
    bool has_coupling_dead_cell[MAX31856_CHANNEL_COUNT];
    float coupling_dead_row[MAX31856_CHANNEL_COUNT];
    /* ZONES_CFG_VERSION 14->15 (PID_EXPANSION_PLAN.md 3.2 follow-up): the
     * coupling identification's own diagonal cell -- ordinary optional-
     * field convention, same as has_fuzzy_strength above (absent -> not
     * written, an older board's stored value survives untouched). */
    bool has_coupling_diag_k_dc;
    float coupling_diag_k_dc;
    /* Opus review of 5672719 (item 4): one value per SRC_GROUP_COUNT
     * group, not a single scalar fanned out to all five -- see
     * backup_export.c's matching comment on the "settings_source_g%u"
     * keys. Each defaults to ZONE_SETTINGS_SOURCE_CUSTOM (see comment
     * above); an older (version <=4) backup that only has the legacy
     * "settings_source" scalar has every group set to that one value
     * instead, preserving the old fan-out behavior for old backups. */
    uint8_t settings_source[SRC_GROUP_COUNT];
    /* 2026-09-16 backup-round-trip-gap closure: every field below has a
     * public getter+setter pair (verified directly against
     * zones_config_accessors.h/zones_config_json.h) and was exported but
     * never restorable -- same "worse than never offered" class the
     * version-2 fields above already closed once. Ordinary independently-
     * optional fields, no bundling (each backing setter takes exactly one
     * of these). */
    bool has_ease_off_window_mult;
    float ease_off_window_mult;
    bool has_approach_rate_cap;
    float approach_rate_cap_c_per_hr;
    bool has_error_band_c;
    float error_band_c;
    bool has_rate_band_c_per_s;
    float rate_band_c_per_s;
    bool has_relay_type;
    uint8_t relay_type;
    bool has_progress_band_c;
    float progress_band_c;
    bool has_zone_type;
    uint8_t zone_type;
    /* model_fit_temp_c/model_fit_ambient_c are a bundled pair, same as
     * temp_limits/heater_cfg above -- zones_config_set_model_fit_context()
     * takes both together. */
    bool has_model_fit_context;
    float model_fit_temp_c, model_fit_ambient_c;
    bool has_coil_power_w;
    float coil_power_w;
    bool has_autotune_baseline_k_dc;
    float autotune_baseline_k_dc;
    bool has_adaptive_tune_enabled;
    bool adaptive_tune_enabled;
    /* tuning_* provenance family -- bundled as a whole (mirrors
     * zone_tuning_quality_t/zones_config_set_tuning_quality()'s own
     * all-fields-at-once contract). tuning_seq is deliberately NOT here --
     * no accessor anywhere exposes it, so it cannot be restored; see the
     * backup round-trip report. */
    bool has_tuning_quality;
    zone_tuning_quality_t tuning_quality;
    /* CT normals -- the owner's own named example. Not bundled with
     * anything; zone_normals_set() takes just the one amps value. */
    bool has_normal_current_a;
    float normal_current_a;
    /* The Pico's OWN i_normal_a[index] (0x031A + index) -- the S14/S15
     * arming baseline, a SEPARATE store from normal_current_a above (see
     * backup_export.c's matching comment). Not committed via zones_config_
     * set_*() like every other field in this struct -- it is pushed to the
     * safety processor itself, over the safety link, in its own guarded
     * block right after the ceiling-raise guard (see backup_import_apply_
     * locked()'s commit section below). */
    bool has_safety_i_normal_a;
    float safety_i_normal_a;
    /* 2026-09-16 backup-round-trip-gap closure, group 1/2: failsafe_state/
     * hyst_c/min_on_s/min_off_s -- new setters added this pass. Ordinary
     * independently-optional fields, same shape as ease_off_window_mult
     * etc. above. min_on_s/min_off_s are the SECONDS fields
     * (zone_cfg_t::min_on_s/min_off_s), distinct from heater_cfg's
     * milliseconds window/min_on_ms/min_off_ms bundle. */
    bool has_failsafe_state;
    bool failsafe_state;
    bool has_hyst_c;
    float hyst_c;
    bool has_min_on_s;
    uint16_t min_on_s;
    bool has_min_off_s;
    uint16_t min_off_s;
    /* Group 3: timing_profile is this zone's index into the top-level
     * timing_profiles[] bundle (parsed separately, see
     * s_timing_profile_candidates below) -- committed only after that
     * bundle is committed, so the index it names already exists. */
    bool has_timing_profile;
    uint8_t timing_profile;
} zone_candidate_t;

/* Group 3: the named timing_profiles[] bundle itself. A small fixed array,
 * same MAX31856_CHANNEL_COUNT bound as zones_cfg_t::timing_profiles[] --
 * parsed in pass 1, committed in pass 2 via
 * zones_config_set_timing_profile_raw(), same two-pass shape as every other
 * candidate array in this file. Kept as a plain local array (not inside
 * zone_candidate_t) since it is not per-zone -- one bundle shared by every
 * zone's timing_profile index. */
typedef struct {
    bool present;
    char name[TIMING_PROFILE_NAME_MAX_LEN + 1];
    float progress_duty_min, progress_window_s, drift_hysteresis_c, frozen_eps_c, cross_zone_period_s,
        bangbang_hysteresis_c, cooling_limited_margin_c, cooling_limited_hold_s, ramp_lock_band_c;
} timing_profile_candidate_t;

// ---- kiln_configs[] restore (KILN_PROFILES_PLAN.md item 17 follow-up) -----
//
// Deliberately NOT folded into backup_import_apply_locked()'s own two-pass
// body above: kiln config slots are validated and committed via
// kiln_cfg_store.h's own API (kiln_cfg_store_validate_package_json()/
// kiln_cfg_store_import_package_json_as()/_rename()/_delete()/
// _set_active_id_raw()), not via the profiles_http_save()/
// zones_config_set_*() calls that function's pass 2 uses -- a different
// commit surface entirely. Kept as its own small pair of functions
// (validate/plan-only vs. commit) called from backup_import_apply() around
// backup_import_apply_locked(), same "validate everything, THEN apply"
// discipline, just sequenced as its own step rather than interleaved with
// the profile/zone candidate arrays. No httpd_req_t anywhere in this pair --
// host-testable exactly like backup_import_apply_locked() itself.
//
// Absent "kiln_configs" key entirely = pre-item-17 backup: do nothing, in
// either mode, exactly today's behaviour (old backups must keep restoring
// unchanged). PRESENT-but-empty ("kiln_configs":[]) is an explicit "this
// board should have no saved kiln configs": MERGE makes no changes (nothing
// to add, nothing matches to update); MIRROR deletes every existing slot
// (naming each one first, like any other mirror deletion).
typedef enum {
    KILN_CFG_RESTORE_MERGE = 0,
    KILN_CFG_RESTORE_MIRROR = 1,
} kiln_cfg_restore_mode_t;

#define KILN_CFG_PLAN_LINE_MAX 128
#define KILN_CFG_PLAN_MAX_LINES 32

typedef struct {
    char lines[KILN_CFG_PLAN_MAX_LINES][KILN_CFG_PLAN_LINE_MAX];
    size_t count;
} kiln_cfg_plan_t;

static void kiln_cfg_plan_add(kiln_cfg_plan_t *plan, const char *fmt, ...)
{
    if (!plan || plan->count >= KILN_CFG_PLAN_MAX_LINES) {
        return; /* plan is informational for the confirm dialog -- silently
                  * capping it is acceptable; KILN_CFG_PLAN_MAX_LINES (32) is
                  * already well over 3x KILN_CFG_MAX_COUNT (10), the most
                  * create+rename+delete lines a single restore can ever
                  * produce (at most one line per board slot plus one per
                  * file entry). */
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(plan->lines[plan->count], KILN_CFG_PLAN_LINE_MAX, fmt, ap);
    va_end(ap);
    plan->count++;
}

static bool kiln_cfg_json_field_bool(const char *obj, const char *key, bool *out)
{
    const char *v = backup_json_obj_find(obj, key);
    if (!v) {
        return false;
    }
    v = backup_json_skip_ws(v);
    if (strncmp(v, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(v, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

// One parsed "kiln_configs[]" file entry.
typedef struct {
    bool has_package;    // false => this was an "omitted" (legacy, no Pico half) entry
    bool is_active;
    char name[KILN_CFG_NAME_MAX_LEN + 1]; // normalized name from validate() when has_package
    uint16_t pkg_schema;
    uint32_t pkg_hash;
    const char *package_json; // pointer into `body`, valid only when has_package
} kiln_cfg_file_entry_t;

// Suffix a colliding name until BOTH the live store (via
// kiln_cfg_store_name_would_collide(), excluding `exclude_id`) and this same
// restore's own already-claimed names (`claimed`, `claimed_count`) agree it
// is unique. Per the coordinator's explicit instruction: keep suffixing
// until would_collide() returns false, never assume one attempt suffices.
/* Hand-rolled rather than strcasecmp()/_stricmp(): this file is built both
 * by ESP-IDF's toolchain (on-target) and, unmodified, by MSVC's cl for the
 * host test harness (build_host_tests.ps1), which disagree on which
 * non-standard name/header exposes a case-insensitive compare -- same
 * portability note and same fix as kiln_cfg_store.c's names_equal_ci(). */
static bool backup_names_equal_ci(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static void kiln_cfg_unique_name(const char *base, int32_t exclude_id, char claimed[][KILN_CFG_NAME_MAX_LEN + 1],
                                  size_t claimed_count, char *out, size_t out_cap)
{
    snprintf(out, out_cap, "%s", base);
    int suffix = 1;
    for (;;) {
        bool taken = kiln_cfg_store_name_would_collide(out, exclude_id);
        if (!taken) {
            for (size_t i = 0; i < claimed_count; i++) {
                if (backup_names_equal_ci(claimed[i], out)) {
                    taken = true;
                    break;
                }
            }
        }
        if (!taken) {
            return;
        }
        suffix++;
        char base_trunc[KILN_CFG_NAME_MAX_LEN + 1];
        snprintf(base_trunc, sizeof(base_trunc), "%s", base);
        char tail[16];
        snprintf(tail, sizeof(tail), " (%d)", suffix);
        size_t max_base = KILN_CFG_NAME_MAX_LEN - strlen(tail);
        if (strlen(base_trunc) > max_base) {
            base_trunc[max_base] = '\0';
        }
        // Manual concat (not snprintf's "%s%s") -- base_trunc was already cut
        // to fit alongside tail above, but GCC's format-truncation checker
        // cannot see that arithmetic through two separate buffers; this
        // avoids the false-positive -Werror without loosening out_cap.
        out[0] = '\0';
        if (out_cap > 0) {
            size_t bt_len = strlen(base_trunc);
            size_t room = out_cap - 1;
            size_t n1 = bt_len < room ? bt_len : room;
            memcpy(out, base_trunc, n1);
            out[n1] = '\0';
            room -= n1;
            size_t tl_len = strlen(tail);
            size_t n2 = tl_len < room ? tl_len : room;
            memcpy(out + n1, tail, n2);
            out[n1 + n2] = '\0';
        }
    }
}

// Parses "kiln_configs" (if present) and, if `commit` is false, only
// validates and fills `plan`; if `commit` is true, actually performs every
// create/rename/active-reassignment/delete. Either way, on success `plan` is
// filled with a human-readable line per action taken/to-be-taken. Returns
// false (nothing written, even under commit=true, since this function does
// not touch the store until every file entry has already validated) on any
// malformed/invalid entry.
// `ack_delete_count`: the operator's X-Kiln-Config-Ack-Delete header value
// (see backup_import_post_handler()), or -1 if the header was absent. Only
// consulted when `commit` is true and MIRROR mode actually has slots slated
// for deletion (task 8/11): the caller must echo back exactly how many
// slots this restore will delete, matching "MIRROR names every deletion
// before applying" for any client, not just backup_page.html's own dry-run
// round trip -- a MIRROR POST that omits or gets the count wrong is refused
// before ANY write (rename/create/delete) happens this pass.
static bool backup_import_kiln_configs(const char *body, kiln_cfg_restore_mode_t mode, bool commit,
                                        int32_t ack_delete_count, kiln_cfg_plan_t *plan, char *err_msg,
                                        size_t err_cap)
{
    plan->count = 0;
    const char *arr = backup_json_obj_find(body, "kiln_configs");
    if (!arr) {
        return true; /* pre-item-17 backup -- untouched, either mode */
    }

    kiln_cfg_file_entry_t files[KILN_CFG_MAX_COUNT];
    size_t file_n = 0;
    const char *elem = backup_json_arr_first(arr);
    while (elem) {
        if (file_n >= KILN_CFG_MAX_COUNT) {
            snprintf(err_msg, err_cap, "kiln_configs has more than %d entries", KILN_CFG_MAX_COUNT);
            return false;
        }
        kiln_cfg_file_entry_t *f = &files[file_n];
        memset(f, 0, sizeof(*f));
        bool active_field = false;
        kiln_cfg_json_field_bool(elem, "is_active", &active_field);
        f->is_active = active_field;

        const char *pkg = backup_json_obj_find(elem, "package");
        if (pkg) {
            f->has_package = true;
            f->package_json = pkg;
            char reason[128];
            if (!kiln_cfg_store_validate_package_json(pkg, f->name, sizeof(f->name), &f->pkg_schema, &f->pkg_hash,
                                                      reason, sizeof(reason))) {
                snprintf(err_msg, err_cap, "kiln_configs[%u]: %s", (unsigned)file_n, reason);
                return false;
            }
        } else {
            f->has_package = false; /* "omitted" legacy entry -- carries no identity;
                                       * cannot be matched/preserved, see this file's
                                       * header comment above. */
        }
        file_n++;
        elem = backup_json_arr_next(elem);
    }

    // ---- Match file entries against the board's current slots -----------
    kiln_cfg_summary_t board[KILN_CFG_MAX_COUNT];
    uint8_t board_n = kiln_cfg_store_list(board, KILN_CFG_MAX_COUNT);
    bool board_matched[KILN_CFG_MAX_COUNT] = {0};
    uint16_t board_schema[KILN_CFG_MAX_COUNT];
    uint32_t board_hash[KILN_CFG_MAX_COUNT];
    bool board_has_identity[KILN_CFG_MAX_COUNT];
    for (uint8_t b = 0; b < board_n; b++) {
        bool pico_populated = false;
        board_has_identity[b] =
            kiln_cfg_store_get_package_identity(board[b].id, &pico_populated, &board_schema[b], &board_hash[b]) &&
            pico_populated;
    }

    char claimed[KILN_CFG_MAX_COUNT * 2][KILN_CFG_NAME_MAX_LEN + 1];
    size_t claimed_n = 0;
    for (uint8_t b = 0; b < board_n; b++) {
        snprintf(claimed[claimed_n++], sizeof(claimed[0]), "%.23s", board[b].name);
    }

    // Per-file-entry decision, applied in commit order below.
    typedef enum { ACT_NONE, ACT_RENAME, ACT_CREATE } file_action_t;
    file_action_t action[KILN_CFG_MAX_COUNT];
    int32_t action_board_id[KILN_CFG_MAX_COUNT]; // for ACT_RENAME: which board id
    char action_name[KILN_CFG_MAX_COUNT][KILN_CFG_NAME_MAX_LEN + 1]; // final name to use
    // Coordinator decision (assessment task 7): active-slot restoration is
    // dropped entirely. kiln_cfg_store_set_active_id_raw() is bookkeeping-
    // only for kiln_cfg_swap.c's own transaction ("does NOT apply the slot's
    // blob", per its header) -- using it here to restore `is_active` let
    // `active_id` point at a slot the live zones config could then be
    // autosaved OVER (section 2.4), silently corrupting the just-restored
    // slot the moment nothing else changed it first. `is_active` is now
    // purely informational in the plan text below; the operator re-applies
    // the desired kiln config from Kiln configs after a restore, exactly the
    // same as any other config change made outside a live-apply.

    for (size_t i = 0; i < file_n; i++) {
        kiln_cfg_file_entry_t *f = &files[i];
        action[i] = ACT_NONE;
        if (!f->has_package) {
            continue; /* legacy "omitted" entry: nothing to create/rename against */
        }

        int idx_identity = -1;
        for (uint8_t b = 0; b < board_n; b++) {
            if (!board_matched[b] && board_has_identity[b] && board_schema[b] == f->pkg_schema &&
                board_hash[b] == f->pkg_hash) {
                idx_identity = b;
                break;
            }
        }

        if (idx_identity >= 0) {
            board_matched[idx_identity] = true;
            if (backup_names_equal_ci(board[idx_identity].name, f->name)) {
                // Case 1: same identity, same name -- no-op.
                if (f->is_active) {
                    kiln_cfg_plan_add(plan,
                                     "\"%s\" was the active kiln config in the backup (informational only -- "
                                     "re-apply from Kiln configs after restore)",
                                     board[idx_identity].name);
                }
                continue;
            }
            // Case 2: same identity, different name -- file's name wins.
            action[i] = ACT_RENAME;
            action_board_id[i] = board[idx_identity].id;
            kiln_cfg_unique_name(f->name, board[idx_identity].id, claimed, claimed_n, action_name[i],
                                 sizeof(action_name[i]));
            snprintf(claimed[claimed_n++], sizeof(claimed[0]), "%.23s", action_name[i]);
            kiln_cfg_plan_add(plan, "rename \"%s\" -> \"%s\"", board[idx_identity].name, action_name[i]);
            if (f->is_active) {
                kiln_cfg_plan_add(plan,
                                 "\"%s\" was the active kiln config in the backup (informational only -- "
                                 "re-apply from Kiln configs after restore)",
                                 action_name[i]);
            }
            continue;
        }

        // No identity match: does the file's name collide with a DIFFERENT
        // board slot? Per the owner's rule, that is Case 3 -- a real
        // collision, not the same slot -- so this is always a CREATE, never
        // an update of that other slot.
        int idx_name = -1;
        for (uint8_t b = 0; b < board_n; b++) {
            if (backup_names_equal_ci(board[b].name, f->name)) {
                idx_name = b;
                break;
            }
        }
        action[i] = ACT_CREATE;
        kiln_cfg_unique_name(f->name, KILN_CFG_NO_ACTIVE_ID, claimed, claimed_n, action_name[i],
                             sizeof(action_name[i]));
        snprintf(claimed[claimed_n++], sizeof(claimed[0]), "%.23s", action_name[i]);
        if (idx_name >= 0 && strcmp(action_name[i], f->name) != 0) {
            kiln_cfg_plan_add(plan, "create \"%s\" (renamed from \"%s\" to avoid colliding with existing slot)",
                              action_name[i], f->name);
        } else {
            kiln_cfg_plan_add(plan, "create \"%s\"", action_name[i]);
        }
        // is_active is informational only (task 7): the create above never
        // touches active_id, on this build or any other -- see this
        // function's `action_name`/mapped-active comment above.
        if (f->is_active) {
            kiln_cfg_plan_add(plan,
                             "\"%s\" was the active kiln config in the backup (informational only -- "
                             "re-apply from Kiln configs after restore)",
                             action_name[i]);
        }
    }

    // ---- MIRROR: name every unmatched board slot for deletion, before ----
    // ---- deleting anything (owner requirement: never delete silently). ---
    bool board_delete[KILN_CFG_MAX_COUNT] = {0};
    if (mode == KILN_CFG_RESTORE_MIRROR) {
        for (uint8_t b = 0; b < board_n; b++) {
            if (!board_matched[b]) {
                board_delete[b] = true;
                kiln_cfg_plan_add(plan, "delete \"%s\"", board[b].name);
            }
        }
    }

    if (!commit) {
        return true; // dry-run / pass-1 validation only -- nothing written
    }

    // ---- Ack gate for MIRROR deletes (task 8/11) -- checked BEFORE any ----
    // ---- write this pass, renames/creates included, so a missing/wrong ---
    // ---- ack refuses the whole restore rather than half-applying it. The
    // ---- same match also derives the ack_no_safety_processor argument the
    // ---- delete loop below passes -- never a hardcoded `true` -- so an
    // ---- operator acknowledgement is actually OBTAINED, not merely
    // ---- asserted by comment (assessment defect 5). */
    int32_t mirror_delete_count = 0;
    for (uint8_t b = 0; b < board_n; b++) {
        if (board_delete[b]) {
            mirror_delete_count++;
        }
    }
    bool mirror_delete_ack = (ack_delete_count == mirror_delete_count);
    if (mode == KILN_CFG_RESTORE_MIRROR && mirror_delete_count > 0 && !mirror_delete_ack) {
        snprintf(err_msg, err_cap,
                "mirror restore would delete %d kiln config slot(s); refusing without "
                "X-Kiln-Config-Ack-Delete: %d",
                (int)mirror_delete_count, (int)mirror_delete_count);
        return false;
    }

    // ---- Commit: renames first, then creates, then MIRROR deletes last ---
    // ---- (so a slot slated for deletion is never renamed/recreated under
    // ---- it first). Active-slot restoration is deliberately NOT part of
    // ---- this commit -- see the `is_active` comment above.
    for (size_t i = 0; i < file_n; i++) {
        if (action[i] == ACT_RENAME) {
            if (!kiln_cfg_store_rename(action_board_id[i], action_name[i])) {
                snprintf(err_msg, err_cap, "kiln_configs[%u]: rename to \"%.23s\" failed at commit", (unsigned)i,
                         action_name[i]);
                return false;
            }
        }
    }
    for (size_t i = 0; i < file_n; i++) {
        if (action[i] == ACT_CREATE) {
            int32_t new_id = 0;
            char reason[128];
            if (!kiln_cfg_store_import_package_json_as(files[i].package_json, action_name[i], &new_id, reason,
                                                       sizeof(reason))) {
                snprintf(err_msg, err_cap, "kiln_configs[%u]: create \"%.23s\" failed at commit: %.60s", (unsigned)i,
                         action_name[i], reason);
                return false;
            }
        }
    }
    for (uint8_t b = 0; b < board_n; b++) {
        if (board_delete[b]) {
            char reason[128];
            /* ack_no_safety_processor: derived above, not hardcoded -- true
             * only once mirror_delete_ack confirmed the caller's
             * X-Kiln-Config-Ack-Delete header actually echoed this exact
             * deletion count (we would already have returned false above
             * otherwise, so this is always true by the time we get here;
             * kept explicit rather than a bare `true` so the next reader
             * doesn't have to re-derive that from the guard above). */
            if (!kiln_cfg_store_delete(board[b].id, mirror_delete_ack, reason, sizeof(reason))) {
                snprintf(err_msg, err_cap, "kiln_configs: could not delete \"%.23s\" for mirror: %.60s", board[b].name,
                         reason);
                return false;
            }
        }
    }
    return true;
}

static bool backup_import_apply_locked(const char *body, char *err_msg, size_t err_cap,
                                        profile_candidate_t *candidates, zone_candidate_t *zone_candidates,
                                        timing_profile_candidate_t *timing_profile_candidates)
{
    double dver;
    char kind[24];
    if (!backup_json_field_str(body, "kind", kind, sizeof(kind)) || strcmp(kind, "kilnctl_backup") != 0) {
        snprintf(err_msg, err_cap, "not a kilnCtl backup file (missing/wrong \"kind\")");
        return false;
    }
    if (!backup_json_field_num(body, "version", &dver) || (int)dver < BACKUP_FORMAT_VERSION_MIN ||
        (int)dver > BACKUP_FORMAT_VERSION) {
        snprintf(err_msg, err_cap, "unsupported backup version (this firmware understands versions %d-%d)",
                BACKUP_FORMAT_VERSION_MIN, BACKUP_FORMAT_VERSION);
        return false;
    }

    float bound_target_min, bound_target_max, bound_ramp_min, bound_ramp_max;
    uint32_t bound_dwell_max;
    profiles_http_get_bounds(&bound_target_min, &bound_target_max, &bound_ramp_min, &bound_ramp_max,
                             &bound_dwell_max);
    uint8_t thermo_count = zones_config_get_thermo_count();
    uint8_t valid_zone_bits = thermo_count >= 8 ? 0xFFu : (uint8_t)((1u << thermo_count) - 1u);

    /* ---- Pass 1a: profiles ---- */
    size_t candidate_count = 0;

    const char *profiles_arr = backup_json_obj_find(body, "profiles");
    for (const char *pe = backup_json_arr_first(profiles_arr); pe; pe = backup_json_arr_next(pe)) {
        if (candidate_count >= PROFILES_MAX_COUNT) {
            snprintf(err_msg, err_cap, "backup has more than %u profiles", (unsigned)PROFILES_MAX_COUNT);
            return false;
        }
        profile_candidate_t *c = &candidates[candidate_count];
        memset(c, 0, sizeof(*c));

        double did;
        c->has_id = backup_json_field_num(pe, "id", &did);
        if (c->has_id) {
            if (did < 0 || did >= PROFILES_MAX_COUNT) {
                snprintf(err_msg, err_cap, "profile entry %u: id out of range (0-%u)",
                        (unsigned)candidate_count, (unsigned)(PROFILES_MAX_COUNT - 1));
                return false;
            }
            c->id = (uint8_t)did;
        }

        /* +2, not +1: backup_json_field_str() silently truncates to cap-1 bytes with
         * no way to tell the caller it did so, so a buffer sized exactly
         * PROFILE_NAME_MAX_LEN+1 could never actually observe an overlong
         * name -- it would just come back pre-truncated to a fit, and any
         * "name too long" check below would be permanently unreachable (dead)
         * code. Same fix as the zone-name pass below (its buffer's comment
         * has the full walkthrough): sizing one byte larger than the real
         * limit means ANY name whose true length exceeds
         * PROFILE_NAME_MAX_LEN still results in strlen(name) ==
         * PROFILE_NAME_MAX_LEN+1 after the copy (truncated to fit this
         * buffer, but still detectably over the limit), so the length check
         * that follows can actually fire -- matching the "name too long"
         * rejection the interactive POST /api/profile path already gives
         * for the same input (profiles_parse_profile_fields(), via
         * http_form_find_field()'s -2 return). Without this, import
         * silently accepted what the interactive path refuses. */
        char name[PROFILE_NAME_MAX_LEN + 2];
        if (backup_json_field_str(pe, "name", name, sizeof(name))) {
            if (strlen(name) > PROFILE_NAME_MAX_LEN) {
                snprintf(err_msg, err_cap, "profile entry %u: name too long", (unsigned)candidate_count);
                return false;
            }
            strncpy(c->p.name, name, PROFILE_NAME_MAX_LEN);
            c->p.name[PROFILE_NAME_MAX_LEN] = '\0';
        }

        double dmask;
        if (!backup_json_field_num(pe, "zone_mask", &dmask) || dmask < 0 || dmask > 255) {
            snprintf(err_msg, err_cap, "profile entry %u: zone_mask missing or out of range", (unsigned)candidate_count);
            return false;
        }
        c->p.zone_mask = (uint8_t)dmask;
        if (c->p.zone_mask == 0 || (c->p.zone_mask & (uint8_t)~valid_zone_bits) != 0) {
            snprintf(err_msg, err_cap,
                    "profile entry %u: zone_mask must select at least one configured zone", (unsigned)candidate_count);
            return false;
        }

        const char *segs_arr = backup_json_obj_find(pe, "segments");
        uint8_t seg_i = 0;
        for (const char *se = backup_json_arr_first(segs_arr); se; se = backup_json_arr_next(se)) {
            if (seg_i >= PROFILE_MAX_SEGMENTS) {
                snprintf(err_msg, err_cap, "profile entry %u: more than %u segments", (unsigned)candidate_count,
                        (unsigned)PROFILE_MAX_SEGMENTS);
                return false;
            }
            double dt, dr, dd;
            if (!backup_json_field_num(se, "target_c", &dt) || dt < bound_target_min || dt > bound_target_max) {
                snprintf(err_msg, err_cap, "profile entry %u, segment %u: target_c missing or out of range",
                        (unsigned)candidate_count, (unsigned)(seg_i + 1));
                return false;
            }
            if (!backup_json_field_num(se, "ramp_c_per_hr", &dr) || dr < bound_ramp_min || dr > bound_ramp_max) {
                snprintf(err_msg, err_cap, "profile entry %u, segment %u: ramp_c_per_hr missing or out of range",
                        (unsigned)candidate_count, (unsigned)(seg_i + 1));
                return false;
            }
            if (!backup_json_field_num(se, "dwell_min", &dd) || dd < 0 || dd > bound_dwell_max) {
                snprintf(err_msg, err_cap, "profile entry %u, segment %u: dwell_min missing or out of range",
                        (unsigned)candidate_count, (unsigned)(seg_i + 1));
                return false;
            }
            c->p.segments[seg_i].target_c = (float)dt;
            c->p.segments[seg_i].ramp_c_per_hr = (float)dr;
            c->p.segments[seg_i].dwell_min = (uint32_t)dd;
            seg_i++;
        }
        if (seg_i == 0) {
            snprintf(err_msg, err_cap, "profile entry %u: no segments", (unsigned)candidate_count);
            return false;
        }
        c->p.segment_count = seg_i;

        /* Same feasibility rule profiles_http_save() enforces -- duplicated
         * here so it is caught in validation, before any profile in this
         * import has been written. */
        for (uint8_t i = 0; i < c->p.segment_count; i++) {
            float rate = c->p.segments[i].ramp_c_per_hr;
            if (rate <= 0.0f) {
                continue;
            }
            for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
                if (!(c->p.zone_mask & (1u << zi))) {
                    continue;
                }
                float ceiling = 0.0f;
                zones_config_get_max_ramp(zi, &ceiling);
                if (rate > ceiling) {
                    snprintf(err_msg, err_cap,
                            "profile entry %u, segment %u: ramp rate %.1f C/hr exceeds zone %u's %.1f C/hr ceiling",
                            (unsigned)candidate_count, (unsigned)(i + 1), (double)rate, zi, (double)ceiling);
                    return false;
                }
            }
        }

        candidate_count++;
    }

    /* ---- Pass 1b: zone tuning ---- */
    size_t zone_candidate_count = 0;

    const char *zones_arr = backup_json_obj_find(body, "zones");
    for (const char *ze = backup_json_arr_first(zones_arr); ze; ze = backup_json_arr_next(ze)) {
        if (zone_candidate_count >= MAX31856_CHANNEL_COUNT) {
            snprintf(err_msg, err_cap, "backup has more than %u zone tuning entries",
                    (unsigned)MAX31856_CHANNEL_COUNT);
            return false;
        }
        zone_candidate_t *zc = &zone_candidates[zone_candidate_count];
        memset(zc, 0, sizeof(*zc));
        /* Never 0 -- see zone_candidate_t's own comment and
         * zones_config_set_settings_source()'s identical reasoning. Set here,
         * before the "settings_source" key (if any) is parsed below, so an
         * older-format import (or a version-3 entry that simply omits the
         * key) commits this exact sentinel rather than the zero a plain
         * memset would leave. */
        for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
            zc->settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;
        }

        double didx;
        if (!backup_json_field_num(ze, "index", &didx) || didx < 0 || didx >= MAX31856_CHANNEL_COUNT) {
            snprintf(err_msg, err_cap, "zone tuning entry %u: index missing or out of range",
                    (unsigned)zone_candidate_count);
            return false;
        }
        zc->index = (uint8_t)didx;
        if (zc->index >= thermo_count) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u: channel %u is not a configured zone on this board (Thermocouples & "
                    "Zones settings)",
                    (unsigned)zone_candidate_count, zc->index);
            return false;
        }

        double dkp, dki, dkd;
        if (!backup_json_field_num(ze, "pid_kp", &dkp) || !backup_json_field_num(ze, "pid_ki", &dki) ||
            !backup_json_field_num(ze, "pid_kd", &dkd) || dkp < 0 || dki < 0 || dkd < 0) {
            snprintf(err_msg, err_cap, "zone tuning entry %u: pid_kp/pid_ki/pid_kd missing, negative, or malformed",
                    (unsigned)zone_candidate_count);
            return false;
        }
        zc->kp = (float)dkp;
        zc->ki = (float)dki;
        zc->kd = (float)dkd;

        double dk, dtau, ddead;
        bool has_k = backup_json_field_num(ze, "model_k_dc", &dk);
        bool has_tau = backup_json_field_num(ze, "model_tau_s", &dtau);
        bool has_dead = backup_json_field_num(ze, "model_dead_time_s", &ddead);
        if (has_k || has_tau || has_dead) {
            if (!(has_k && has_tau && has_dead) || dk < 0 || dtau < 0 || ddead < 0) {
                snprintf(err_msg, err_cap,
                        "zone tuning entry %u: model_k_dc/model_tau_s/model_dead_time_s must all be present "
                        "together and non-negative",
                        (unsigned)zone_candidate_count);
                return false;
            }
            /* Same ceilings zones_config_set_model() enforces at commit time
             * (ZONE_MODEL_K_MAX/ZONE_MODEL_TIME_MAX_S, now exposed by
             * zones_http.h) -- checked here, in pass 1, so an out-of-range
             * model is rejected before any earlier candidate in this same
             * import has been written. */
            if (dk > ZONE_MODEL_K_MAX || dtau > ZONE_MODEL_TIME_MAX_S || ddead > ZONE_MODEL_TIME_MAX_S) {
                snprintf(err_msg, err_cap,
                        "zone tuning entry %u: model_k_dc/model_tau_s/model_dead_time_s exceeds this firmware's "
                        "sanity bounds (K<=%.0f, tau/dead_time<=%.0fs)",
                        (unsigned)zone_candidate_count, (double)ZONE_MODEL_K_MAX, (double)ZONE_MODEL_TIME_MAX_S);
                return false;
            }
            zc->has_model = true;
            zc->k_dc = (float)dk;
            zc->tau_s = (float)dtau;
            zc->dead_time_s = (float)ddead;
        }

        double dtc;
        if (backup_json_field_num(ze, "tc_type", &dtc)) {
            if (dtc < 0 || dtc > 7) {
                snprintf(err_msg, err_cap, "zone tuning entry %u: tc_type out of range (0-7)",
                        (unsigned)zone_candidate_count);
                return false;
            }
            zc->has_tc = true;
            zc->tc_type = (uint8_t)dtc;
        }

        /* ---- Version 2 fields -- see zone_candidate_t's comment ---- */
        if (backup_json_field_str(ze, "name", zc->name, sizeof(zc->name))) {
            /* Same rejection zones_config_set_name()/parse_zone_fields()'s
             * z%u_name produce for a name over ZONE_NAME_MAX_LEN chars --
             * see zc->name's own comment for why the buffer is sized one
             * byte over the limit, which is what makes this reachable. */
            if (strlen(zc->name) > ZONE_NAME_MAX_LEN) {
                snprintf(err_msg, err_cap, "zone tuning entry %u: name too long", (unsigned)zone_candidate_count);
                return false;
            }
            zc->has_name = true;
        }

        double drelay;
        if (backup_json_field_opt_num(ze, "relay_mask", 0, 255, &drelay, &zc->has_relay_mask, "relay_mask", err_msg,
                               err_cap, (unsigned)zone_candidate_count) == false) {
            return false;
        }
        if (zc->has_relay_mask) {
            zc->relay_mask = (uint8_t)drelay;
            /* Same bound zones_config_set_relay_mask()/parse_zone_fields()'s
             * z%u_relay_mask enforce: only relays 1..relay_count on THIS
             * board may be referenced. relay_count is a board-wide setting
             * this module never writes, so it is read straight off the live
             * config via zones_config_get_relay_count() (added this pass for
             * exactly this check), same as thermo_count already was for the
             * zone_mask/thermo_mask checks elsewhere in this function. */
            uint8_t relay_count = zones_config_get_relay_count();
            uint8_t valid_relay_bits = relay_count >= 8 ? 0xFFu : (uint8_t)((1u << relay_count) - 1u);
            if ((zc->relay_mask & ~valid_relay_bits) != 0) {
                snprintf(err_msg, err_cap,
                        "zone tuning entry %u: relay_mask references an unconfigured relay",
                        (unsigned)zone_candidate_count);
                return false;
            }
        }

        double dthermo;
        if (backup_json_field_opt_num(ze, "thermo_mask", 0, 255, &dthermo, &zc->has_thermo_mask, "thermo_mask", err_msg,
                               err_cap, (unsigned)zone_candidate_count) == false) {
            return false;
        }
        if (zc->has_thermo_mask) {
            zc->thermo_mask = (uint8_t)dthermo;
            /* Same bound zones_config_set_thermo_mask()/parse_zone_fields()'s
             * z%u_thermo_mask enforce -- valid_zone_bits was already
             * computed above from thermo_count for the profile zone_mask
             * check, and is the identical bound here (both are "which
             * MAX31856 channels are configured on this board"). */
            if ((zc->thermo_mask & ~valid_zone_bits) != 0) {
                snprintf(err_msg, err_cap,
                        "zone tuning entry %u: thermo_mask references an unconfigured thermocouple channel",
                        (unsigned)zone_candidate_count);
                return false;
            }
        }

        double dct;
        if (backup_json_field_opt_num(ze, "ct_mask", 0, 255, &dct, &zc->has_ct_mask, "ct_mask", err_msg,
                               err_cap, (unsigned)zone_candidate_count) == false) {
            return false;
        }
        if (zc->has_ct_mask) {
            zc->ct_mask = (uint8_t)dct;
            /* Fixed hardware count, unlike thermo_mask/relay_mask above --
             * see zones_http.c's ZONE_CT_CHANNEL_COUNT. */
            uint8_t valid_ct_bits = (uint8_t)((1u << ZONE_CT_CHANNEL_COUNT) - 1u);
            if ((zc->ct_mask & ~valid_ct_bits) != 0) {
                snprintf(err_msg, err_cap,
                        "zone tuning entry %u: ct_mask references an unconfigured current-sense channel",
                        (unsigned)zone_candidate_count);
                return false;
            }
        }

        double dcal;
        if (backup_json_field_opt_num(ze, "cal_offset_c", (double)ZONE_CAL_OFFSET_MIN_C, (double)ZONE_CAL_OFFSET_MAX_C,
                               &dcal, &zc->has_cal, "cal_offset_c", err_msg, err_cap,
                               (unsigned)zone_candidate_count) == false) {
            return false;
        }
        if (zc->has_cal) {
            zc->cal_offset_c = (float)dcal;
        }

        double dramp;
        if (backup_json_field_opt_num(ze, "max_ramp_c_per_hr", 0, (double)ZONE_MAX_RAMP_C_PER_HR_MAX, &dramp,
                               &zc->has_ramp, "max_ramp_c_per_hr", err_msg, err_cap,
                               (unsigned)zone_candidate_count) == false) {
            return false;
        }
        if (zc->has_ramp) {
            zc->max_ramp_c_per_hr = (float)dramp;
        }

        double dsanity;
        if (backup_json_field_opt_num(ze, "sanity_rate_c_per_min", 0, (double)ZONE_SANITY_RATE_MAX_C_PER_MIN, &dsanity,
                               &zc->has_sanity, "sanity_rate_c_per_min", err_msg, err_cap,
                               (unsigned)zone_candidate_count) == false) {
            return false;
        }
        if (zc->has_sanity) {
            zc->sanity_rate_c_per_min = (float)dsanity;
        }

        double dmode;
        /* ZONE_CONTROL_MODE_PID_FUZZY (2026-08-30, PID_EXPANSION_PLAN.md
         * Phase 2/4) -- this bound must track zones_http.c's own
         * parse_zone_fields()/zones_config_set_control_mode() ceiling exactly,
         * the same "second copy of a bound drifting" hazard every other field
         * in this file is written against (see this file's own header
         * comment). */
        if (backup_json_field_opt_num(ze, "control_mode", 0, (double)ZONE_CONTROL_MODE_PID_FUZZY, &dmode, &zc->has_mode,
                               "control_mode", err_msg, err_cap, (unsigned)zone_candidate_count) == false) {
            return false;
        }
        if (zc->has_mode) {
            zc->control_mode = (uint8_t)dmode;
        }

        double dmaxt, dmint;
        bool has_maxt = backup_json_field_num(ze, "max_temp_c", &dmaxt);
        bool has_mint = backup_json_field_num(ze, "min_temp_c", &dmint);
        if (has_maxt || has_mint) {
            if (!(has_maxt && has_mint)) {
                snprintf(err_msg, err_cap,
                        "zone tuning entry %u: max_temp_c/min_temp_c must both be present together",
                        (unsigned)zone_candidate_count);
                return false;
            }
            if (dmaxt < 0 || dmaxt > (double)ZONE_MAX_TEMP_C_MAX || dmint < (double)ZONE_MIN_TEMP_C_MIN ||
                dmint > (double)ZONE_MIN_TEMP_C_MAX) {
                snprintf(err_msg, err_cap, "zone tuning entry %u: max_temp_c/min_temp_c out of range",
                        (unsigned)zone_candidate_count);
                return false;
            }
            zc->has_temp_limits = true;
            zc->max_temp_c = (float)dmaxt;
            zc->min_temp_c = (float)dmint;
        }

        double dwin, don, doff;
        bool has_win = backup_json_field_num(ze, "heater_window_ms", &dwin);
        bool has_on = backup_json_field_num(ze, "heater_min_on_ms", &don);
        bool has_off = backup_json_field_num(ze, "heater_min_off_ms", &doff);
        if (has_win || has_on || has_off) {
            if (!(has_win && has_on && has_off)) {
                snprintf(err_msg, err_cap,
                        "zone tuning entry %u: heater_window_ms/heater_min_on_ms/heater_min_off_ms must all be "
                        "present together",
                        (unsigned)zone_candidate_count);
                return false;
            }
            if (dwin < 0 || dwin > (double)ZONE_HEATER_WINDOW_MS_MAX || don < 0 ||
                don > (double)ZONE_HEATER_MIN_ON_OFF_MS_MAX || doff < 0 ||
                doff > (double)ZONE_HEATER_MIN_ON_OFF_MS_MAX) {
                snprintf(err_msg, err_cap, "zone tuning entry %u: heater timing out of range",
                        (unsigned)zone_candidate_count);
                return false;
            }
            zc->has_heater_cfg = true;
            zc->heater_window_ms = (float)dwin;
            zc->heater_min_on_ms = (float)don;
            zc->heater_min_off_ms = (float)doff;
        }

        {
            double d1, d2, d3, d4, d5, d6, d7, d8;
            bool h1 = backup_json_field_num(ze, "guard_wrong_dir_window_s", &d1);
            bool h2 = backup_json_field_num(ze, "guard_wrong_dir_rate_c_per_min", &d2);
            bool h3 = backup_json_field_num(ze, "guard_off_settle_s", &d3);
            bool h4 = backup_json_field_num(ze, "guard_runaway_rate_c_per_min", &d4);
            bool h5 = backup_json_field_num(ze, "guard_runaway_margin_c", &d5);
            bool h6 = backup_json_field_num(ze, "guard_drift_period_s", &d6);
            bool h7 = backup_json_field_num(ze, "guard_sensor_fault_debounce_ticks", &d7);
            bool h8 = backup_json_field_num(ze, "guard_frozen_window_s", &d8);
            bool any = h1 || h2 || h3 || h4 || h5 || h6 || h7 || h8;
            if (any) {
                if (!(h1 && h2 && h3 && h4 && h5 && h6 && h7 && h8)) {
                    snprintf(err_msg, err_cap,
                            "zone tuning entry %u: all 8 guard threshold overrides must be present together",
                            (unsigned)zone_candidate_count);
                    return false;
                }
                if (d1 < 0 || d1 > (double)ZONE_GUARD_TIME_S_MAX || d2 < 0 ||
                    d2 > (double)ZONE_GUARD_RATE_C_PER_MIN_MAX || d3 < 0 || d3 > (double)ZONE_GUARD_TIME_S_MAX ||
                    d4 < 0 || d4 > (double)ZONE_GUARD_RATE_C_PER_MIN_MAX || d5 < 0 ||
                    d5 > (double)ZONE_GUARD_MARGIN_C_MAX || d6 < 0 || d6 > (double)ZONE_GUARD_TIME_S_MAX ||
                    d7 < 0 || d7 > (double)ZONE_GUARD_DEBOUNCE_TICKS_MAX || d8 < 0 ||
                    d8 > (double)ZONE_GUARD_TIME_S_MAX) {
                    snprintf(err_msg, err_cap, "zone tuning entry %u: a guard threshold override is out of range",
                            (unsigned)zone_candidate_count);
                    return false;
                }
                zc->has_guard = true;
                zc->guard_wrong_dir_window_s = (float)d1;
                zc->guard_wrong_dir_rate_c_per_min = (float)d2;
                zc->guard_off_settle_s = (float)d3;
                zc->guard_runaway_rate_c_per_min = (float)d4;
                zc->guard_runaway_margin_c = (float)d5;
                zc->guard_drift_period_s = (float)d6;
                zc->guard_sensor_fault_debounce_ticks = (float)d7;
                zc->guard_frozen_window_s = (float)d8;
            }
        }

        double dxzone;
        if (backup_json_field_opt_num(ze, "cross_zone_max_delta_c", 0, (double)ZONE_CROSS_ZONE_DELTA_C_MAX, &dxzone,
                               &zc->has_cross_zone, "cross_zone_max_delta_c", err_msg, err_cap,
                               (unsigned)zone_candidate_count) == false) {
            return false;
        }
        if (zc->has_cross_zone) {
            zc->cross_zone_max_delta_c = (float)dxzone;
        }

        /* ---- Version 3 fields -- see zone_candidate_t's comment ---- */
        double dfuzzy;
        if (backup_json_field_opt_num(ze, "fuzzy_strength_pct", 0, (double)ZONE_FUZZY_STRENGTH_PCT_MAX, &dfuzzy,
                               &zc->has_fuzzy_strength, "fuzzy_strength_pct", err_msg, err_cap,
                               (unsigned)zone_candidate_count) == false) {
            return false;
        }
        if (zc->has_fuzzy_strength) {
            zc->fuzzy_strength_pct = (float)dfuzzy;
        }

        /* Version 4 (2026-08-30, same-day follow-up): per-cell indexed keys
         * coupling_c0..coupling_cN-1, read straight into the row -- see
         * BACKUP_FORMAT_VERSION's 3->4 comment and zone_candidate_t's own. */
        for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
            char ckey[16];
            snprintf(ckey, sizeof(ckey), "coupling_c%u", (unsigned)j);
            double dcell;
            if (backup_json_field_num(ze, ckey, &dcell)) {
                float max = (j == zc->index) ? 0.0f : (float)ZONE_COUPLING_COEFF_MAX;
                if (dcell < 0 || dcell > (double)max) {
                    snprintf(err_msg, err_cap, "zone tuning entry %u: %s out of range",
                            (unsigned)zone_candidate_count, ckey);
                    return false;
                }
                zc->has_coupling_cell[j] = true;
                zc->coupling_row[j] = (float)dcell;
            }
        }
        /* ZONES_CFG_VERSION 11->12 (DATA PLUMBING pass): coupling_tau_c%u/
         * coupling_dead_time_c%u, same per-cell shape as coupling_c%u just above.
         * No version bump of BACKUP_FORMAT_VERSION -- these are purely
         * additive optional keys, same as fuzzy_strength_pct/coupling_c%u
         * were when they landed (an older export simply never has them, and
         * commit's has_* guard leaves the currently-stored value alone). */
        for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
            char ckey[24];
            snprintf(ckey, sizeof(ckey), "coupling_tau_c%u", (unsigned)j);
            double dtau;
            if (backup_json_field_num(ze, ckey, &dtau)) {
                float max = (j == zc->index) ? 0.0f : ZONE_MODEL_TIME_MAX_S;
                if (dtau < 0 || dtau > (double)max) {
                    snprintf(err_msg, err_cap, "zone tuning entry %u: %s out of range",
                            (unsigned)zone_candidate_count, ckey);
                    return false;
                }
                zc->has_coupling_tau_cell[j] = true;
                zc->coupling_tau_row[j] = (float)dtau;
            }
            snprintf(ckey, sizeof(ckey), "coupling_dead_time_c%u", (unsigned)j);
            double ddead;
            if (backup_json_field_num(ze, ckey, &ddead)) {
                float max = (j == zc->index) ? 0.0f : ZONE_MODEL_TIME_MAX_S;
                if (ddead < 0 || ddead > (double)max) {
                    snprintf(err_msg, err_cap, "zone tuning entry %u: %s out of range",
                            (unsigned)zone_candidate_count, ckey);
                    return false;
                }
                zc->has_coupling_dead_cell[j] = true;
                zc->coupling_dead_row[j] = (float)ddead;
            }
        }
        /* ZONES_CFG_VERSION 14->15 (PID_EXPANSION_PLAN.md 3.2 follow-up):
         * coupling_diag_k_dc, the coupling identification's own diagonal
         * cell -- ordinary optional-field convention, same as
         * fuzzy_strength_pct above. No BACKUP_FORMAT_VERSION bump, same
         * reasoning as coupling_tau_c%u/coupling_dead_time_c%u above. */
        double ddiag;
        if (backup_json_field_opt_num(ze, "coupling_diag_k_dc", 0, (double)ZONE_MODEL_K_MAX, &ddiag,
                               &zc->has_coupling_diag_k_dc, "coupling_diag_k_dc", err_msg, err_cap,
                               (unsigned)zone_candidate_count) == false) {
            return false;
        }
        if (zc->has_coupling_diag_k_dc) {
            zc->coupling_diag_k_dc = (float)ddiag;
        }
        /* Version <=3 LOSSLESS backward compat: an old export's single
         * coupling_coeff/coupling_neighbor_zone pair maps onto exactly one
         * cell of the row, same "one neighbor, everything else 0" mapping
         * zones_http.c's convert_zone_v10() applies at the NVS layer for a
         * v10 blob. Only honored if this entry did NOT already supply the
         * new per-cell keys above (a hand-edited or future body should never
         * have both; if it does, the explicit per-cell keys win and this
         * legacy pair is ignored rather than silently overwriting them). */
        double dcoeff, dneighbor;
        bool has_coeff = backup_json_field_num(ze, "coupling_coeff", &dcoeff);
        bool has_neighbor = backup_json_field_num(ze, "coupling_neighbor_zone", &dneighbor);
        if (has_coeff || has_neighbor) {
            if (!(has_coeff && has_neighbor)) {
                snprintf(err_msg, err_cap,
                        "zone tuning entry %u: coupling_coeff/coupling_neighbor_zone must both be present together",
                        (unsigned)zone_candidate_count);
                return false;
            }
            if (dcoeff < 0 || dcoeff > (double)ZONE_COUPLING_COEFF_MAX || dneighbor < 0 ||
                dneighbor > (double)(MAX31856_CHANNEL_COUNT - 1)) {
                snprintf(err_msg, err_cap, "zone tuning entry %u: coupling_coeff/coupling_neighbor_zone out of range",
                        (unsigned)zone_candidate_count);
                return false;
            }
            /* Same "reject a fractional index, never truncate" rule
             * parse_zone_fields()'s z%u_coupling_c%u enforces --
             * coupling_neighbor_zone is a zone INDEX living in a float. */
            if (dneighbor != floor(dneighbor)) {
                snprintf(err_msg, err_cap, "zone tuning entry %u: coupling_neighbor_zone must be a whole zone index",
                        (unsigned)zone_candidate_count);
                return false;
            }
            /* Reject a self-referencing legacy pair HERE, in pass 1, same as
             * every other coupling check in this function (the per-cell loop
             * above forces max=0 for j==zc->index rather than deferring to
             * pass 2). Without this, a self-referencing pair sailed through
             * pass 1 and only failed later, in pass 2 at
             * zones_config_set_coupling_cell(zc->index, zc->index, ...) --
             * which the setter refuses as a nonzero diagonal -- after
             * earlier candidates in the same import had ALREADY been
             * committed. The two-pass split exists precisely so pass 2, once
             * started, cannot fail: a validation gap here turns an import
             * error into a half-applied config. */
            if ((uint8_t)dneighbor == zc->index) {
                snprintf(err_msg, err_cap,
                        "zone tuning entry %u: coupling_neighbor_zone must not be this zone's own index",
                        (unsigned)zone_candidate_count);
                return false;
            }
            uint8_t neighbor = (uint8_t)dneighbor;
            if (!zc->has_coupling_cell[neighbor]) {
                zc->has_coupling_cell[neighbor] = true;
                zc->coupling_row[neighbor] = (float)dcoeff;
            }
        }

        /* settings_source -- UNLIKE the three floats above, an absent key
         * must NOT leave zc->settings_source[g] at 0 (see zone_candidate_t's
         * own comment): every group was already pre-seeded to
         * ZONE_SETTINGS_SOURCE_CUSTOM right after this candidate's memset,
         * above, so this block only ever OVERWRITES a group when a value for
         * it is actually present. No has_* flag: pass 2 always commits every
         * group for every candidate.
         *
         * Opus review of 5672719 (item 4): prefer the per-group
         * "settings_source_g%u" keys (0..SRC_GROUP_COUNT-1) written by this
         * build's own exporter; a backup that lacks them (anything exported
         * before this fix) falls back to the single legacy "settings_source"
         * scalar applied to every group, matching the old fan-out
         * behavior exactly for old backups. */
        {
            bool any_group_key = false;
            for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
                char key[24];
                snprintf(key, sizeof(key), "settings_source_g%u", (unsigned)g);
                double dsrc;
                if (!backup_json_field_num(ze, key, &dsrc)) {
                    continue;
                }
                any_group_key = true;
                if (dsrc < 0 || dsrc > 255) {
                    snprintf(err_msg, err_cap, "zone tuning entry %u: %s out of range",
                            (unsigned)zone_candidate_count, key);
                    return false;
                }
                if (dsrc != floor(dsrc)) {
                    snprintf(err_msg, err_cap, "zone tuning entry %u: %s must be a whole zone index",
                            (unsigned)zone_candidate_count, key);
                    return false;
                }
                uint8_t src_raw = (uint8_t)dsrc;
                if (src_raw != ZONE_SETTINGS_SOURCE_CUSTOM && src_raw >= MAX31856_CHANNEL_COUNT) {
                    snprintf(err_msg, err_cap, "zone tuning entry %u: %s references a zone that doesn't exist",
                            (unsigned)zone_candidate_count, key);
                    return false;
                }
                if (src_raw == zc->index) {
                    snprintf(err_msg, err_cap, "zone tuning entry %u: %s cannot point at itself",
                            (unsigned)zone_candidate_count, key);
                    return false;
                }
                zc->settings_source[g] = src_raw;
            }
            if (!any_group_key) {
                /* Legacy (version <=4) backup: single "settings_source"
                 * scalar, fanned out to every group -- same rules as above,
                 * checked once. */
                double dsrc;
                if (backup_json_field_num(ze, "settings_source", &dsrc)) {
                    if (dsrc < 0 || dsrc > 255) {
                        snprintf(err_msg, err_cap, "zone tuning entry %u: settings_source out of range",
                                (unsigned)zone_candidate_count);
                        return false;
                    }
                    /* Same "reject a fractional index, never truncate" rule
                     * as coupling_neighbor_zone above: settings_source is a
                     * zone INDEX (or the ZONE_SETTINGS_SOURCE_CUSTOM
                     * sentinel) living in a float here, and (uint8_t)dsrc
                     * below would otherwise silently truncate e.g. 1.7 to 1
                     * -- a hand-edited backup could make a zone inherit a
                     * DIFFERENT zone's settings than the one written in the
                     * file, with no error. */
                    if (dsrc != floor(dsrc)) {
                        snprintf(err_msg, err_cap, "zone tuning entry %u: settings_source must be a whole zone index",
                                (unsigned)zone_candidate_count);
                        return false;
                    }
                    uint8_t src_raw = (uint8_t)dsrc;
                    if (src_raw != ZONE_SETTINGS_SOURCE_CUSTOM && src_raw >= MAX31856_CHANNEL_COUNT) {
                        snprintf(err_msg, err_cap, "zone tuning entry %u: settings_source references a zone that doesn't exist",
                                (unsigned)zone_candidate_count);
                        return false;
                    }
                    if (src_raw == zc->index) {
                        snprintf(err_msg, err_cap, "zone tuning entry %u: settings_source cannot point at itself",
                                (unsigned)zone_candidate_count);
                        return false;
                    }
                    for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
                        zc->settings_source[g] = src_raw;
                    }
                }
            }
        }

        /* 2026-09-16 backup-round-trip-gap closure -- ordinary independently-
         * optional numeric fields, same shape as fuzzy_strength_pct/
         * coupling_diag_k_dc above (each has its own setter taking exactly
         * this one value). Bounds mirror each field's own zones_config_
         * set_*() range check in zones_config_json.h -- pass 1 rejects an
         * out-of-range value here, before any earlier candidate in this
         * same import is committed, same reasoning as every other bounded
         * field in this function. */
        double dease;
        if (!backup_json_field_opt_num(ze, "ease_off_window_mult", 0, (double)ZONE_EASE_OFF_WINDOW_MULT_MAX, &dease,
                               &zc->has_ease_off_window_mult, "ease_off_window_mult", err_msg, err_cap,
                               (unsigned)zone_candidate_count)) {
            return false;
        }
        if (zc->has_ease_off_window_mult) {
            zc->ease_off_window_mult = (float)dease;
        }
        double dapproach;
        if (!backup_json_field_opt_num(ze, "approach_rate_cap_c_per_hr", 0,
                               (double)ZONE_APPROACH_RATE_CAP_C_PER_HR_MAX, &dapproach,
                               &zc->has_approach_rate_cap, "approach_rate_cap_c_per_hr", err_msg, err_cap,
                               (unsigned)zone_candidate_count)) {
            return false;
        }
        if (zc->has_approach_rate_cap) {
            zc->approach_rate_cap_c_per_hr = (float)dapproach;
        }
        double derrband;
        if (!backup_json_field_opt_num(ze, "error_band_c", 0, (double)ZONE_ERROR_BAND_C_MAX, &derrband,
                               &zc->has_error_band_c, "error_band_c", err_msg, err_cap,
                               (unsigned)zone_candidate_count)) {
            return false;
        }
        if (zc->has_error_band_c) {
            zc->error_band_c = (float)derrband;
        }
        double drateband;
        if (!backup_json_field_opt_num(ze, "rate_band_c_per_s", 0, (double)ZONE_RATE_BAND_C_PER_S_MAX, &drateband,
                               &zc->has_rate_band_c_per_s, "rate_band_c_per_s", err_msg, err_cap,
                               (unsigned)zone_candidate_count)) {
            return false;
        }
        if (zc->has_rate_band_c_per_s) {
            zc->rate_band_c_per_s = (float)drateband;
        }
        double drelaytype;
        if (!backup_json_field_opt_num(ze, "relay_type", 0, (double)ZONE_RELAY_TYPE_MAX, &drelaytype,
                               &zc->has_relay_type, "relay_type", err_msg, err_cap,
                               (unsigned)zone_candidate_count)) {
            return false;
        }
        if (zc->has_relay_type) {
            zc->relay_type = (uint8_t)drelaytype;
        }
        double dprogband;
        if (!backup_json_field_opt_num(ze, "progress_band_c", 0, (double)ZONE_PROGRESS_BAND_C_MAX, &dprogband,
                               &zc->has_progress_band_c, "progress_band_c", err_msg, err_cap,
                               (unsigned)zone_candidate_count)) {
            return false;
        }
        if (zc->has_progress_band_c) {
            zc->progress_band_c = (float)dprogband;
        }
        double dzonetype;
        if (!backup_json_field_opt_num(ze, "zone_type", 0, (double)ZONE_TYPE_ON_OFF, &dzonetype,
                               &zc->has_zone_type, "zone_type", err_msg, err_cap,
                               (unsigned)zone_candidate_count)) {
            return false;
        }
        if (zc->has_zone_type) {
            zc->zone_type = (uint8_t)dzonetype;
        }
        /* model_fit_temp_c/model_fit_ambient_c -- bundled pair, same as
         * temp_limits/heater_cfg above. ZONE_MODEL_FIT_TEMP_UNKNOWN
         * (-273.15) is a real, legitimate value here (it IS the sentinel
         * the setter accepts to mean "unknown"), so the bound is widened to
         * include it rather than rejecting it as out-of-range. */
        double dfittemp, dfitambient;
        bool has_fittemp = backup_json_field_num(ze, "model_fit_temp_c", &dfittemp);
        bool has_fitambient = backup_json_field_num(ze, "model_fit_ambient_c", &dfitambient);
        if (has_fittemp || has_fitambient) {
            if (!(has_fittemp && has_fitambient)) {
                snprintf(err_msg, err_cap,
                        "zone tuning entry %u: model_fit_temp_c/model_fit_ambient_c must both be present together",
                        (unsigned)zone_candidate_count);
                return false;
            }
            zc->has_model_fit_context = true;
            zc->model_fit_temp_c = (float)dfittemp;
            zc->model_fit_ambient_c = (float)dfitambient;
        }
        double dcoilpower;
        if (!backup_json_field_opt_num(ze, "coil_power_w", 0, (double)ZONE_COIL_POWER_W_MAX, &dcoilpower,
                               &zc->has_coil_power_w, "coil_power_w", err_msg, err_cap,
                               (unsigned)zone_candidate_count)) {
            return false;
        }
        if (zc->has_coil_power_w) {
            zc->coil_power_w = (float)dcoilpower;
        }
        double dautobase;
        if (!backup_json_field_opt_num(ze, "autotune_baseline_k_dc", 0, (double)ZONE_MODEL_K_MAX, &dautobase,
                               &zc->has_autotune_baseline_k_dc, "autotune_baseline_k_dc", err_msg, err_cap,
                               (unsigned)zone_candidate_count)) {
            return false;
        }
        if (zc->has_autotune_baseline_k_dc) {
            zc->autotune_baseline_k_dc = (float)dautobase;
        }
        /* Booleans as 0/1 -- see backup_export.c's matching comment. */
        double dadaptive;
        if (!backup_json_field_opt_num(ze, "adaptive_tune_enabled", 0, 1, &dadaptive,
                               &zc->has_adaptive_tune_enabled, "adaptive_tune_enabled", err_msg, err_cap,
                               (unsigned)zone_candidate_count)) {
            return false;
        }
        if (zc->has_adaptive_tune_enabled) {
            zc->adaptive_tune_enabled = dadaptive != 0.0;
        }
        /* tuning_* provenance family -- bundled as a whole, all-or-nothing,
         * gated on tuning_valid being present and 1 (the export only ever
         * emits this whole block when the underlying run was valid; an
         * absent tuning_valid key means an older backup, or a zone that had
         * never been tuned when the backup was taken -- either way, leave
         * the target board's own tuning record untouched rather than
         * clobbering it with zeros). tuning_seq is NOT restorable (see
         * zone_candidate_t's own comment); the struct's remaining 9 fields
         * are. */
        double dtvalid;
        bool has_tvalid = backup_json_field_num(ze, "tuning_valid", &dtvalid);
        if (has_tvalid && dtvalid != 0.0) {
            double dtmethod, dtrule, dtsettled, dtextrap, dttau, dtbaseline, dtstepamb, dtrawrise, dtriseinf;
            if (!backup_json_field_num(ze, "tuning_method", &dtmethod) ||
                !backup_json_field_num(ze, "tuning_rule", &dtrule) ||
                !backup_json_field_num(ze, "tuning_settled", &dtsettled) ||
                !backup_json_field_num(ze, "tuning_extrapolation_converged", &dtextrap) ||
                !backup_json_field_num(ze, "tuning_tau_consistent", &dttau) ||
                !backup_json_field_num(ze, "tuning_baseline_c", &dtbaseline) ||
                !backup_json_field_num(ze, "tuning_step_ambient_c", &dtstepamb) ||
                !backup_json_field_num(ze, "tuning_raw_rise_c", &dtrawrise) ||
                !backup_json_field_num(ze, "tuning_rise_inf_c", &dtriseinf)) {
                snprintf(err_msg, err_cap,
                        "zone tuning entry %u: tuning_valid present but one or more tuning_* fields missing",
                        (unsigned)zone_candidate_count);
                return false;
            }
            zc->has_tuning_quality = true;
            zc->tuning_quality.valid = true;
            zc->tuning_quality.method = (uint8_t)dtmethod;
            zc->tuning_quality.rule = (uint8_t)dtrule;
            zc->tuning_quality.settled = dtsettled != 0.0;
            zc->tuning_quality.extrapolation_converged = dtextrap != 0.0;
            zc->tuning_quality.tau_consistent = dttau != 0.0;
            zc->tuning_quality.baseline_c = (float)dtbaseline;
            zc->tuning_quality.step_ambient_c = (float)dtstepamb;
            zc->tuning_quality.raw_rise_c = (float)dtrawrise;
            zc->tuning_quality.rise_inf_c = (float)dtriseinf;
        }
        /* CT normals -- the owner's own named example. zone_normals_set()
         * rejects negative/non-finite ahead of commit anyway, but the same
         * bound is checked here in pass 1 for the usual "fail before any
         * earlier candidate is committed" reason. */
        double dnormal;
        if (!backup_json_field_opt_num(ze, "normal_current_a", 0, 1000.0, &dnormal,
                               &zc->has_normal_current_a, "normal_current_a", err_msg, err_cap,
                               (unsigned)zone_candidate_count)) {
            return false;
        }
        if (zc->has_normal_current_a) {
            zc->normal_current_a = (float)dnormal;
        }
        /* The Pico's own i_normal_a[index] -- same range as normal_current_a
         * above (SaftyFW's config_params.c only enforces CHECK_F32_NONNEG on
         * this field; 1000 A is already a generous upper sanity bound, same
         * one this file already uses for the ESP-side twin). Range-checked
         * here in pass 1 for the same "fail before anything commits" reason;
         * the actual write only happens after every other pass-1 check in
         * this whole import has already passed. */
        double dsafety_inormal;
        if (!backup_json_field_opt_num(ze, "safety_i_normal_a", 0, 1000.0, &dsafety_inormal,
                               &zc->has_safety_i_normal_a, "safety_i_normal_a", err_msg, err_cap,
                               (unsigned)zone_candidate_count)) {
            return false;
        }
        if (zc->has_safety_i_normal_a) {
            zc->safety_i_normal_a = (float)dsafety_inormal;
        }

        /* 2026-09-16 backup-round-trip-gap closure, group 1/2: same
         * "bound checked here in pass 1, setter re-checks at commit"
         * pattern as normal_current_a above, using the exact bounds
         * zones_config_set_hyst_c()/set_min_on_s()/set_min_off_s()
         * themselves enforce (ZONE_HYST_C_MIN/MAX, ZONE_MIN_ON_OFF_S_MIN/
         * MAX) -- 0 is accepted here too, meaning "use firmware default",
         * same as those setters. */
        double dfailsafe;
        if (!backup_json_field_opt_num(ze, "failsafe_state", 0, 1, &dfailsafe, &zc->has_failsafe_state,
                               "failsafe_state", err_msg, err_cap, (unsigned)zone_candidate_count)) {
            return false;
        }
        if (zc->has_failsafe_state) {
            zc->failsafe_state = dfailsafe != 0.0;
        }
        double dhyst;
        if (!backup_json_field_opt_num(ze, "hyst_c", 0, (double)ZONE_HYST_C_MAX, &dhyst, &zc->has_hyst_c,
                               "hyst_c", err_msg, err_cap, (unsigned)zone_candidate_count)) {
            return false;
        }
        if (zc->has_hyst_c) {
            zc->hyst_c = (float)dhyst;
        }
        double dminon, dminoff;
        if (!backup_json_field_opt_num(ze, "min_on_s", 0, (double)ZONE_MIN_ON_OFF_S_MAX, &dminon,
                               &zc->has_min_on_s, "min_on_s", err_msg, err_cap, (unsigned)zone_candidate_count)) {
            return false;
        }
        if (zc->has_min_on_s) {
            zc->min_on_s = (uint16_t)dminon;
        }
        if (!backup_json_field_opt_num(ze, "min_off_s", 0, (double)ZONE_MIN_ON_OFF_S_MAX, &dminoff,
                               &zc->has_min_off_s, "min_off_s", err_msg, err_cap,
                               (unsigned)zone_candidate_count)) {
            return false;
        }
        if (zc->has_min_off_s) {
            zc->min_off_s = (uint16_t)dminoff;
        }
        /* Group 3: timing_profile index. Range-checked against
         * MAX31856_CHANNEL_COUNT here (the array's own physical bound);
         * the tighter "< the timing_profiles[] bundle actually being
         * imported" check happens after that bundle is parsed below,
         * since this entry may be seen before the bundle is. */
        double dtprofile;
        if (!backup_json_field_opt_num(ze, "timing_profile", 0, (double)(MAX31856_CHANNEL_COUNT - 1), &dtprofile,
                               &zc->has_timing_profile, "timing_profile", err_msg, err_cap,
                               (unsigned)zone_candidate_count)) {
            return false;
        }
        if (zc->has_timing_profile) {
            zc->timing_profile = (uint8_t)dtprofile;
        }

        zone_candidate_count++;
    }

    /* ---- Pass 1c: timing_profiles[] bundle (group 3) ----
     * A hand-edited or older backup may omit this array entirely -- every
     * zone's has_timing_profile stays independently optional either way,
     * and an absent bundle simply means no zone's timing_profile index can
     * be validated against it below (each such index is then rejected,
     * same as any other out-of-range value, rather than silently ignored). */
    size_t timing_profile_candidate_count = 0;
    {
        const char *tp_arr = backup_json_obj_find(body, "timing_profiles");
        for (const char *tpe = backup_json_arr_first(tp_arr); tpe; tpe = backup_json_arr_next(tpe)) {
            if (timing_profile_candidate_count >= MAX31856_CHANNEL_COUNT) {
                snprintf(err_msg, err_cap, "backup has more than %u timing profiles",
                        (unsigned)MAX31856_CHANNEL_COUNT);
                return false;
            }
            timing_profile_candidate_t *tp = &timing_profile_candidates[timing_profile_candidate_count];
            memset(tp, 0, sizeof(*tp));
            if (!backup_json_field_str(tpe, "name", tp->name, sizeof(tp->name))) {
                snprintf(err_msg, err_cap, "timing profile entry %u: missing/invalid \"name\"",
                        (unsigned)timing_profile_candidate_count);
                return false;
            }
            if (strlen(tp->name) >= sizeof(tp->name)) {
                snprintf(err_msg, err_cap, "timing profile entry %u: name too long",
                        (unsigned)timing_profile_candidate_count);
                return false;
            }
            double dduty, dwindow, ddrift, deps, dcross, dbang, dcoolmargin, dcoolhold, dramplock;
            if (!backup_json_field_num(tpe, "progress_duty_min", &dduty) ||
                !backup_json_field_num(tpe, "progress_window_s", &dwindow) ||
                !backup_json_field_num(tpe, "drift_hysteresis_c", &ddrift) ||
                !backup_json_field_num(tpe, "frozen_eps_c", &deps) ||
                !backup_json_field_num(tpe, "cross_zone_period_s", &dcross) ||
                !backup_json_field_num(tpe, "bangbang_hysteresis_c", &dbang) ||
                !backup_json_field_num(tpe, "cooling_limited_margin_c", &dcoolmargin) ||
                !backup_json_field_num(tpe, "cooling_limited_hold_s", &dcoolhold) ||
                !backup_json_field_num(tpe, "ramp_lock_band_c", &dramplock)) {
                snprintf(err_msg, err_cap, "timing profile entry %u: missing one or more required fields",
                        (unsigned)timing_profile_candidate_count);
                return false;
            }
            /* Same bounds zones_config_set_timing_profile_raw() itself
             * enforces -- checked here too so pass 1 fails before any
             * earlier candidate (profile or zone) is committed. */
            if (!isfinite(dduty) || dduty < 0.0 || dduty > (double)ZONE_GUARD_DUTY_MAX ||
                !isfinite(dwindow) || dwindow < 0.0 || dwindow > (double)ZONE_GUARD_TIME_S_MAX ||
                !isfinite(ddrift) || ddrift < 0.0 || ddrift > (double)ZONE_GUARD_MARGIN_C_MAX ||
                !isfinite(deps) || deps < 0.0 || deps > (double)ZONE_GUARD_EPS_C_MAX ||
                !isfinite(dcross) || dcross < 0.0 || dcross > (double)ZONE_GUARD_TIME_S_MAX ||
                !isfinite(dbang) || dbang < 0.0 || dbang > (double)ZONE_GUARD_MARGIN_C_MAX ||
                !isfinite(dcoolmargin) || dcoolmargin < 0.0 || dcoolmargin > (double)ZONE_GUARD_MARGIN_C_MAX ||
                !isfinite(dcoolhold) || dcoolhold < 0.0 || dcoolhold > (double)ZONE_GUARD_TIME_S_MAX ||
                !isfinite(dramplock) || dramplock < 0.0 || dramplock > (double)ZONE_GUARD_MARGIN_C_MAX) {
                snprintf(err_msg, err_cap, "timing profile entry %u: a field is out of range",
                        (unsigned)timing_profile_candidate_count);
                return false;
            }
            tp->present = true;
            tp->progress_duty_min = (float)dduty;
            tp->progress_window_s = (float)dwindow;
            tp->drift_hysteresis_c = (float)ddrift;
            tp->frozen_eps_c = (float)deps;
            tp->cross_zone_period_s = (float)dcross;
            tp->bangbang_hysteresis_c = (float)dbang;
            tp->cooling_limited_margin_c = (float)dcoolmargin;
            tp->cooling_limited_hold_s = (float)dcoolhold;
            tp->ramp_lock_band_c = (float)dramplock;
            timing_profile_candidate_count++;
        }
    }
    /* Cross-check every zone's timing_profile index (if present) against
     * the bundle actually being imported, before pass 2 commits anything --
     * same "fail before any earlier candidate is committed" reasoning as
     * the settings_source cross-entry check below.
     *
     * Export always emits "timing_profile" for a zone (answerable whenever
     * any timing profile exists at all, same convention as pid_kp), but a
     * board with no *custom* profiles configured beyond the implicit slot 0
     * exports an empty "timing_profiles":[] bundle. That combination --
     * has_timing_profile true, timing_profile 0, timing_profile_candidate_count
     * 0 -- must NOT reject the whole import: 0 is the always-valid implicit
     * default slot, not a dangling reference. Only reject when the backup
     * DOES carry a bundle and the index overflows it; when the bundle is
     * empty, skip validating (and, at commit time below, skip restoring)
     * every zone's timing_profile index and leave the target's own value
     * untouched. */
    for (size_t zi2 = 0; timing_profile_candidate_count > 0 && zi2 < zone_candidate_count; zi2++) {
        zone_candidate_t *zc2 = &zone_candidates[zi2];
        if (zc2->has_timing_profile && zc2->timing_profile >= timing_profile_candidate_count) {
            snprintf(err_msg, err_cap,
                    "zone entry %u: timing_profile %u is not one of the %u timing_profiles[] in this backup",
                    (unsigned)zi2, (unsigned)zc2->timing_profile, (unsigned)timing_profile_candidate_count);
            return false;
        }
    }

    /* Cross-entry pass 1 for settings_source: each candidate's own
     * self-reference/range checks above only ever look at THAT entry, same
     * gap the runtime defect test documents for parse_zone_fields() and
     * zones_config_set_settings_source() individually -- a hand-edited
     * backup that gives two (or more) zones links that only close a cycle
     * TOGETHER (e.g. zone 0's entry sets settings_source=1 and zone 1's
     * entry sets settings_source=0, neither of which cycles against the
     * live config alone) would sail through every check above and only
     * start failing partway through pass 2's commit loop below --
     * committing zone 0's link and THEN discovering zone 1's closes a
     * cycle, exactly the half-applied-import failure mode this file's
     * two-pass split exists to prevent (see this function's own header
     * comment and the self-reference check above, which cites the identical
     * reasoning). Checked here, before pass 2 starts, against every
     * candidate's proposed NEW value at once. */
    {
        /* Opus review of 5672719 (item 4): each group now carries its OWN
         * override_source (a per-group backup can legitimately point
         * different groups at different zones), so has_override/
         * override_source are rebuilt per group inside the loop below,
         * rather than once outside it -- unlike before this fix, the five
         * checks are no longer guaranteed to see identical inputs. */
        for (uint8_t group = 0; group < SRC_GROUP_COUNT; group++) {
            bool has_override[MAX31856_CHANNEL_COUNT] = {0};
            uint8_t override_source[MAX31856_CHANNEL_COUNT] = {0};
            for (size_t i = 0; i < zone_candidate_count; i++) {
                has_override[zone_candidates[i].index] = true;
                override_source[zone_candidates[i].index] = zone_candidates[i].settings_source[group];
            }
            uint8_t cycle_zone = 0;
            if (zones_config_settings_source_import_has_cycle(group, has_override, override_source, &cycle_zone)) {
                snprintf(err_msg, err_cap,
                        "zone %u's settings_source forms an inheritance cycle with this import applied",
                        (unsigned)cycle_zone);
                return false;
            }
        }
    }

    double dsafety;
    bool has_safety_tc = backup_json_field_num(body, "safety_tc_type", &dsafety);
    if (has_safety_tc && (dsafety < 0 || dsafety > 7)) {
        snprintf(err_msg, err_cap, "safety_tc_type out of range (0-7)");
        return false;
    }

    /* ---- Pass 2: everything validated -- commit ---- */
    for (size_t i = 0; i < candidate_count; i++) {
        profile_candidate_t *c = &candidates[i];
        uint8_t out_id = 0;
        char save_err[96];
        if (!profiles_http_save(c->has_id ? c->id : PROFILES_MAX_COUNT, &c->p, &out_id, NULL, save_err,
                                sizeof(save_err))) {
            /* Should not happen -- pass 1 already checked everything
             * profiles_http_save() itself checks -- but if it does (a race
             * with a concurrent change to zone config between pass 1 and
             * pass 2, say), report exactly which entry and why rather than a
             * generic failure. Any candidates before this one in the loop
             * are already committed -- see this function's header comment. */
            snprintf(err_msg, err_cap, "profile entry %u rejected at commit: %s", (unsigned)i, save_err);
            return false;
        }
    }
    /* opus review finding (LOW-MEDIUM): the settings_source commit loop below
     * uses the _no_save() variant so a mid-batch failure (an out-of-range
     * override_source pass 1 somehow missed, or a future refusal added to
     * the setter) leaves whatever pairs already committed THIS pass sitting
     * mutated in RAM with settings_source_dirty never getting persisted --
     * the live config and flash silently disagree until something else
     * happens to save. Snapshot every zone's settings_source[group] before
     * this loop starts so the failure arm can restore the exact pre-import
     * values rather than leaving a half-applied set live. */
    uint8_t settings_source_before[MAX31856_CHANNEL_COUNT][SRC_GROUP_COUNT];
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        for (uint8_t group = 0; group < SRC_GROUP_COUNT; group++) {
            if (!zones_config_get_settings_source(zi, group, &settings_source_before[zi][group])) {
                settings_source_before[zi][group] = ZONE_SETTINGS_SOURCE_CUSTOM;
            }
        }
    }
    bool settings_source_dirty = false; /* set true once any _no_save() commit below succeeds; see item 3 comment */

    /* 2026-09-10 opus review: zones_http_post.c's zones_post_handler() gates every
     * max_temp_c RAISE on safety_ceiling_sync_guard_raise() so the Pico's own
     * abs_max_temp_c ceiling is written and CONFIRMED before the ESP's own ceiling
     * is allowed to move up (see safety_ceiling_policy.h's header comment for the
     * full invariant -- the Pico ceiling must never end up TIGHTER than the ESP's).
     * A restored backup is the other place an operator can raise max_temp_c, and it
     * was reaching zones_config_set_temp_limits() directly, bypassing that guard
     * entirely -- precisely the defect 2d604d1d closed on the POST path, still open
     * here. Build the proposed per-zone ceiling (backup's new value where the entry
     * touches temp limits, else the zone's current live value) and run the same
     * guard before ANY zone-tuning field commits, so a refused raise leaves the
     * whole import uncommitted -- same "validate everything, then apply" discipline
     * this function already follows for its two candidate arrays. */
    {
        float new_max_temp_c[MAX31856_CHANNEL_COUNT];
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            float cur_max = 0.0f, cur_min = 0.0f;
            zones_config_get_temp_limits(zi, &cur_max, &cur_min);
            new_max_temp_c[zi] = cur_max;
        }
        for (size_t i = 0; i < zone_candidate_count; i++) {
            zone_candidate_t *zc = &zone_candidates[i];
            if (zc->has_temp_limits && zc->index < MAX31856_CHANNEL_COUNT) {
                new_max_temp_c[zc->index] = zc->max_temp_c;
            }
        }
        safety_ceiling_sync_result_t ceiling_result;
        char ceiling_reason[128];
        if (!safety_ceiling_sync_guard_raise(s_hw_safety, new_max_temp_c, MAX31856_CHANNEL_COUNT, &ceiling_result,
                                              ceiling_reason, sizeof(ceiling_reason), NULL)) {
            /* Fixed prefix alone is already ~110 chars against a 160-byte err_cap -- room
             * for ceiling_reason must be bounded explicitly (%.48s) rather than left open,
             * or a long Pico-side reason string silently truncates this whole message
             * instead of just the reason (-Werror=format-truncation caught the untruncated
             * version outright: 164 B possible into a 160 B buffer with NO reason appended
             * at all). */
            snprintf(err_msg, err_cap,
                    "backup would raise a zone ceiling; safety processor ceiling refused/unconfirmed: %.48s",
                    ceiling_reason);
            return false;
        }
    }

    /* 2026-09-16 config-backup round-trip gap closure: the Pico's own
     * i_normal_a[0..2] (S14/S15 arming baseline) -- see backup_export.c's
     * matching comment and zone_candidate_t::safety_i_normal_a's own comment
     * for why this is a separate store from normal_current_a/zone_normals_set()
     * below. Pushed here, BEFORE the zone-tuning commit loop (which never
     * depends on this and never gates it), via the SAME stage/COMMIT_CONFIG/
     * forced-read-back-confirm path safety_cfg_http.c's own commissioning POST
     * handler uses (safety_cfg_write_apply_pairs()) -- "never trust a bare
     * ACK" applies here exactly as it does to every other Pico config write in
     * this codebase. Nothing is attempted (n_pairs stays 0, no UART traffic at
     * all) unless the backup actually carries at least one safety_i_normal_a
     * key -- an older backup, or one taken from a board where this channel was
     * never swept, simply preserves whatever the target Pico already has,
     * same "omit preserves current" convention every other optional field in
     * this file follows, and the one that keeps this restore from ever DE-
     * ARMING a channel the backup itself says nothing about.
     *
     * abs_max_temp_c (SAFETY_PARAM_ID_ABS_MAX_TEMP_C, staged/committed by the
     * ceiling guard immediately above) is never one of these pairs -- this
     * block only ever builds param ids 0x031A + index, never the ceiling id,
     * so the two writes can never collide or be reordered against each
     * other's own guard.
     *
     * A failure here (rejected commit, ARMED refusal, a read-back that does
     * not confirm) aborts the WHOLE import loudly, before any zone-tuning
     * field below is committed -- silently completing an import that could
     * not confirm the Pico actually holds the restored baseline would be
     * exactly the "logging unchecked success" defect class this codebase has
     * been bitten by repeatedly, applied to a safety-arming value instead of
     * a log line. */
    {
        safety_cfg_post_pair_t pairs[MAX31856_CHANNEL_COUNT];
        int n_pairs = 0;
        for (size_t i = 0; i < zone_candidate_count; i++) {
            zone_candidate_t *zc = &zone_candidates[i];
            if (!zc->has_safety_i_normal_a || zc->index >= MAX31856_CHANNEL_COUNT) {
                continue;
            }
            safety_cfg_post_pair_t *pair = &pairs[n_pairs++];
            pair->param_id = (uint16_t)(0x031Au + zc->index); /* i_normal_a[index] */
            int wn = snprintf(pair->value_text, sizeof(pair->value_text), "%.6f", (double)zc->safety_i_normal_a);
            if (wn < 0 || (size_t)wn >= sizeof(pair->value_text)) {
                snprintf(err_msg, err_cap,
                        "zone tuning entry %u (channel %u): safety_i_normal_a value could not be encoded",
                        (unsigned)i, (unsigned)zc->index);
                return false;
            }
        }
        if (n_pairs > 0) {
            char reason[128];
            safety_ceiling_refusal_class_t out_class = SAFETY_CEILING_REFUSAL_NONE;
            if (!safety_cfg_write_apply_pairs(s_hw_safety, pairs, n_pairs, true /* commit */, reason,
                                              sizeof(reason), &out_class)) {
                /* err_msg's real caller buffer is 160 B (backup_http.c's
                 * POST handler); this fixed 84-byte prefix leaves only 75
                 * bytes free before the terminator, not 80 -- %.80s could
                 * write up to 80 into that 75-byte remainder
                 * (-Werror=format-truncation caught it outright: KilnFW
                 * target build broke on origin/main at 407ac1ba). Trimmed
                 * to %.75s, the actual free space, rather than widening any
                 * buffer: a longer `reason` string here (e.g. a verbose
                 * commit_reject_reason_words() sentence) is truncated, same
                 * as every other %.NNs precision cap already used in this
                 * file (%.48s above), not silently overflowed -- a
                 * truncated error MESSAGE is safe; it never reaches
                 * anything committed to config. */
                snprintf(err_msg, err_cap,
                        "backup carries a safety processor i_normal_a baseline; restore refused/unconfirmed: "
                        "%.75s",
                        reason);
                return false;
            }
        }
    }

    /* Group 3: commit the timing_profiles[] bundle BEFORE any zone's
     * timing_profile index, in slot order 0..count-1 -- each call's index
     * equals the bundle's CURRENT count on the target board at that point
     * only when it also equals timing_profile_candidate_count's own running
     * position, since zones_config_set_timing_profile_raw() both
     * overwrites an existing slot < the target's current count and grows
     * the count by exactly one when index == current count. Sequential
     * ascending order guarantees every index this loop uses is always
     * either "overwrite an existing slot" or "grow by exactly one", never
     * a gap -- matching the pass-1 cross-check above, which already
     * confirmed every zone's timing_profile index is < timing_profile_candidate_count. */
    for (size_t i = 0; i < timing_profile_candidate_count; i++) {
        timing_profile_candidate_t *tp = &timing_profile_candidates[i];
        if (!zones_config_set_timing_profile_raw((uint8_t)i, tp->name, tp->progress_duty_min,
                                                 tp->progress_window_s, tp->drift_hysteresis_c, tp->frozen_eps_c,
                                                 tp->cross_zone_period_s, tp->bangbang_hysteresis_c,
                                                 tp->cooling_limited_margin_c, tp->cooling_limited_hold_s,
                                                 tp->ramp_lock_band_c)) {
            snprintf(err_msg, err_cap, "timing profile entry %u rejected at commit", (unsigned)i);
            return false;
        }
    }

    for (size_t i = 0; i < zone_candidate_count; i++) {
        zone_candidate_t *zc = &zone_candidates[i];
        if (!zones_config_set_pid(zc->index, zc->kp, zc->ki, zc->kd)) {
            snprintf(err_msg, err_cap, "zone tuning entry %u (channel %u) rejected at commit setting PID gains",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_model && !zones_config_set_model(zc->index, zc->k_dc, zc->tau_s, zc->dead_time_s)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting the plant model -- value "
                    "outside this firmware's sanity bounds",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_tc && !zones_config_set_tc_type(zc->index, zc->tc_type)) {
            snprintf(err_msg, err_cap, "zone tuning entry %u (channel %u) rejected at commit setting tc_type",
                    (unsigned)i, zc->index);
            return false;
        }
        /* Version 2 fields -- see zone_candidate_t's comment. Every one of
         * these setters was just validated against the exact same bound in
         * pass 1 above, so a commit-time rejection here means a race with a
         * concurrent config change between the two passes (same rationale
         * as the PID/model/tc_type "should not happen" comments above), not
         * a bug in this pass's own bounds. */
        if (zc->has_name && !zones_config_set_name(zc->index, zc->name)) {
            snprintf(err_msg, err_cap, "zone tuning entry %u (channel %u) rejected at commit setting name",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_relay_mask && !zones_config_set_relay_mask(zc->index, zc->relay_mask)) {
            snprintf(err_msg, err_cap, "zone tuning entry %u (channel %u) rejected at commit setting relay_mask",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_thermo_mask && !zones_config_set_thermo_mask(zc->index, zc->thermo_mask)) {
            snprintf(err_msg, err_cap, "zone tuning entry %u (channel %u) rejected at commit setting thermo_mask",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_ct_mask && !zones_config_set_ct_mask(zc->index, zc->ct_mask)) {
            snprintf(err_msg, err_cap, "zone tuning entry %u (channel %u) rejected at commit setting ct_mask",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_cal && !zones_config_set_cal_offset(zc->index, zc->cal_offset_c)) {
            snprintf(err_msg, err_cap, "zone tuning entry %u (channel %u) rejected at commit setting cal_offset_c",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_ramp && !zones_config_set_max_ramp(zc->index, zc->max_ramp_c_per_hr)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting max_ramp_c_per_hr",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_sanity && !zones_config_set_sanity_rate(zc->index, zc->sanity_rate_c_per_min)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting sanity_rate_c_per_min",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_mode && !zones_config_set_control_mode(zc->index, (zone_control_mode_t)zc->control_mode)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting control_mode",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_temp_limits && !zones_config_set_temp_limits(zc->index, zc->max_temp_c, zc->min_temp_c)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting temp limits",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_heater_cfg &&
            !zones_config_set_heater_cfg(zc->index, zc->heater_window_ms, zc->heater_min_on_ms,
                                         zc->heater_min_off_ms)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting heater timing",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_guard &&
            !zones_config_set_guard_thresholds(zc->index, zc->guard_wrong_dir_window_s,
                                               zc->guard_wrong_dir_rate_c_per_min, zc->guard_off_settle_s,
                                               zc->guard_runaway_rate_c_per_min, zc->guard_runaway_margin_c,
                                               zc->guard_drift_period_s, zc->guard_sensor_fault_debounce_ticks,
                                               zc->guard_frozen_window_s)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting guard thresholds",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_cross_zone && !zones_config_set_cross_zone_delta(zc->index, zc->cross_zone_max_delta_c)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting cross_zone_max_delta_c",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_fuzzy_strength && !zones_config_set_fuzzy_strength_pct(zc->index, zc->fuzzy_strength_pct)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting fuzzy_strength_pct",
                    (unsigned)i, zc->index);
            return false;
        }
        /* Per-cell, not whole-row: an import that only supplies (or only
         * ever had, pre-version-4) one neighbor's coefficient must not blank
         * out this zone's OTHER already-stored neighbors -- same "omit
         * preserves the current value" convention as fuzzy_strength_pct
         * above, applied per cell instead of per field. */
        for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
            /* ZONES_CFG_VERSION 11->12: zones_config_set_coupling_cell() is
             * now all-or-nothing across all three of coeff/tau/dead_time --
             * see its own header comment. An import that only supplies coeff
             * (every pre-v12 export, and any v12 export whose autotune run
             * never fitted a trustworthy peer tau) must not wipe out
             * whichever of tau/dead_time is ALREADY stored for this cell, so
             * this reads the current value first and only overwrites the
             * pieces this import actually supplied -- same "omit preserves"
             * discipline coupling_row's own has_coupling_cell[] flag already
             * follows, just extended to three fields written together. */
            if (!zc->has_coupling_cell[j] && !zc->has_coupling_tau_cell[j] && !zc->has_coupling_dead_cell[j]) {
                continue;
            }
            float coeff = zc->coupling_row[j];
            float tau_s = zc->coupling_tau_row[j];
            float dead_time_s = zc->coupling_dead_row[j];
            if (!zc->has_coupling_cell[j] || !zc->has_coupling_tau_cell[j] || !zc->has_coupling_dead_cell[j]) {
                float cur_coeff_row[MAX31856_CHANNEL_COUNT] = {0};
                float cur_tau_row[MAX31856_CHANNEL_COUNT] = {0};
                float cur_dead_row[MAX31856_CHANNEL_COUNT] = {0};
                zones_config_get_coupling(zc->index, cur_coeff_row);
                zones_config_get_coupling_tau(zc->index, cur_tau_row);
                zones_config_get_coupling_dead_time(zc->index, cur_dead_row);
                if (!zc->has_coupling_cell[j]) {
                    coeff = cur_coeff_row[j];
                }
                if (!zc->has_coupling_tau_cell[j]) {
                    tau_s = cur_tau_row[j];
                }
                if (!zc->has_coupling_dead_cell[j]) {
                    dead_time_s = cur_dead_row[j];
                }
            }
            if (!zones_config_set_coupling_cell(zc->index, j, coeff, tau_s, dead_time_s)) {
                snprintf(err_msg, err_cap,
                        "zone tuning entry %u (channel %u) rejected at commit setting coupling_c%u",
                        (unsigned)i, zc->index, (unsigned)j);
                return false;
            }
        }
        /* ZONES_CFG_VERSION 14->15 (PID_EXPANSION_PLAN.md 3.2 follow-up):
         * coupling_diag_k_dc -- same "omit preserves the current value"
         * convention as fuzzy_strength_pct above (this is a measured
         * quantity, not a setting an absent import should reset). */
        if (zc->has_coupling_diag_k_dc &&
            !zones_config_set_coupling_diag_k_dc(zc->index, zc->coupling_diag_k_dc)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting coupling_diag_k_dc",
                    (unsigned)i, zc->index);
            return false;
        }
        /* 2026-09-16 backup-round-trip-gap closure -- "omit preserves the
         * current value" throughout, same as fuzzy_strength_pct/
         * coupling_diag_k_dc above: every one of these was independently
         * optional in pass 1, so an absent key here means an older backup
         * (or a hand-edited one), not "reset to zero". */
        if (zc->has_ease_off_window_mult &&
            !zones_config_set_ease_off_window_mult(zc->index, zc->ease_off_window_mult)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting ease_off_window_mult",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_approach_rate_cap &&
            !zones_config_set_approach_rate_cap_c_per_hr(zc->index, zc->approach_rate_cap_c_per_hr)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting approach_rate_cap_c_per_hr",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_error_band_c && !zones_config_set_error_band_c(zc->index, zc->error_band_c)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting error_band_c",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_rate_band_c_per_s && !zones_config_set_rate_band_c_per_s(zc->index, zc->rate_band_c_per_s)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting rate_band_c_per_s",
                    (unsigned)i, zc->index);
            return false;
        }
        /* zones_config_set_relay_type() pushes the new type out to
         * relay_cycles_set_type() for this zone's relays internally
         * (confirmed by direct read of zones_config_accessors.c) -- no
         * separate zones_config_push_relay_type() call needed here. */
        if (zc->has_relay_type && !zones_config_set_relay_type(zc->index, zc->relay_type)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting relay_type",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_progress_band_c && !zones_config_set_progress_band_c(zc->index, zc->progress_band_c)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting progress_band_c",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_zone_type && !zones_config_set_zone_type(zc->index, (zone_type_t)zc->zone_type)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting zone_type",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_model_fit_context &&
            !zones_config_set_model_fit_context(zc->index, zc->model_fit_temp_c, zc->model_fit_ambient_c)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting model fit context",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_coil_power_w && !zones_config_set_coil_power_w(zc->index, zc->coil_power_w)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting coil_power_w",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_autotune_baseline_k_dc &&
            !zones_config_set_autotune_baseline_k_dc(zc->index, zc->autotune_baseline_k_dc)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting autotune_baseline_k_dc",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_adaptive_tune_enabled &&
            !zones_config_set_adaptive_tune_enabled(zc->index, zc->adaptive_tune_enabled)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting adaptive_tune_enabled",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_tuning_quality && !zones_config_set_tuning_quality(zc->index, &zc->tuning_quality)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting tuning quality",
                    (unsigned)i, zc->index);
            return false;
        }
        /* CT normals -- the owner's own named example, and a SEPARATE NVS
         * store from zone_cfg_t (zone_normals_cfg_t) -- see
         * backup_export.c's matching comment. zone_normals_set() persists
         * immediately on success (its own save, not zones_config_save_now()
         * below), same as it does for the live current-sweep task's own
         * calls into it. */
        if (zc->has_normal_current_a && !zone_normals_set(zc->index, zc->normal_current_a)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting normal_current_a",
                    (unsigned)i, zc->index);
            return false;
        }
        /* 2026-09-16 backup-round-trip-gap closure, group 1/2/3. The
         * timing_profiles[] bundle itself is already committed above, so
         * zc->timing_profile (if present) already names a valid slot --
         * except when the backup carried no bundle at all
         * (timing_profile_candidate_count == 0), in which case the earlier
         * cross-check deliberately skipped validating it (see comment
         * there); restoring it here too would call the setter with an
         * index that only accidentally matches the *target's* live
         * profile_count, so skip restoring this field in that case and
         * leave the target's own timing_profile index untouched. */
        if (zc->has_failsafe_state && !zones_config_set_failsafe_state(zc->index, zc->failsafe_state)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting failsafe_state",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_hyst_c && !zones_config_set_hyst_c(zc->index, zc->hyst_c)) {
            snprintf(err_msg, err_cap, "zone tuning entry %u (channel %u) rejected at commit setting hyst_c",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_min_on_s && !zones_config_set_min_on_s(zc->index, zc->min_on_s)) {
            snprintf(err_msg, err_cap, "zone tuning entry %u (channel %u) rejected at commit setting min_on_s",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_min_off_s && !zones_config_set_min_off_s(zc->index, zc->min_off_s)) {
            snprintf(err_msg, err_cap, "zone tuning entry %u (channel %u) rejected at commit setting min_off_s",
                    (unsigned)i, zc->index);
            return false;
        }
        if (zc->has_timing_profile && timing_profile_candidate_count > 0 &&
            !zones_config_set_timing_profile_index(zc->index, zc->timing_profile)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting timing_profile",
                    (unsigned)i, zc->index);
            return false;
        }
        /* No has_* guard -- zc->settings_source is ALWAYS a real value (either
         * the imported one, or the ZONE_SETTINGS_SOURCE_CUSTOM default seeded
         * in pass 1), and always committed, matching the "older backup must
         * default this to CUSTOM, never 0" brief.
         *
         * Uses the _unchecked commit-loop variant, not
         * zones_config_set_settings_source(): that setter chain-walks
         * settings_source against the LIVE config, which here is only
         * PARTIALLY applied mid-loop (earlier entries in this same loop
         * already committed, later ones haven't yet) -- an ordinary restore
         * onto a differently-configured board can walk straight into a
         * cycle that only exists in that half-applied intermediate state
         * even though the pass-1 cross-entry check above (which validates
         * the FINAL assembled state) already accepted this exact import.
         * Concretely: live zone 0=Custom, zone 1->0; backup (from a valid
         * board) wants zone 0->1, zone 1->Custom. Pass 1 probes {0->1,
         * 1->Custom} together -- acyclic, accepted. But committing entry 0
         * first with the CHECKED setter walks live {0->1, 1->0} -- a cycle
         * -- and refuses, after zone 0's name/PID/masks/cal/ramp/limits/
         * heater cfg/coupling cells above were already written this same
         * pass. Pass 1 already proved the end state is acyclic; this commit
         * loop must not be able to fail on a state pass 1 accepted, so pass
         * 2 re-checks only bounds/self-reference (still real defenses
         * against a corrupt override_source) and skips the live-config
         * chain-walk entirely. */
        /* Item 4: zc->settings_source[group] now carries each group's own
         * value (see zone_candidate_t's own comment) instead of one scalar
         * fanned out to all five.
         *
         * Item 3 (Opus review of 5672719): the _no_save() variant is used
         * here instead of zones_config_set_settings_source_unchecked() --
         * that function calls nvs_save() on every single (zone, group) pair,
         * which for a 3-zone import meant 15 flash writes for this block
         * alone. settings_source_dirty is set below and a single
         * zones_config_save_now() call, after this whole per-zone loop
         * finishes, persists the lot in one write. */
        for (uint8_t group = 0; group < SRC_GROUP_COUNT; group++) {
            if (!zones_config_set_settings_source_unchecked_no_save(zc->index, group, zc->settings_source[group])) {
                /* opus review finding (LOW-MEDIUM): restore every zone's
                 * settings_source[] to its pre-import snapshot before
                 * returning -- otherwise whatever (zone, group) pairs this
                 * loop already committed this pass stay mutated in RAM,
                 * unpersisted (settings_source_dirty never reaches the save
                 * below), silently disagreeing with flash. Best-effort: the
                 * restore uses the same unchecked/no-save setter, so a
                 * restore failure here would itself need a restore -- but
                 * these are the exact values that were live and valid a
                 * moment ago, so failure is not expected. */
                for (uint8_t rzi = 0; rzi < MAX31856_CHANNEL_COUNT; rzi++) {
                    for (uint8_t rgroup = 0; rgroup < SRC_GROUP_COUNT; rgroup++) {
                        zones_config_set_settings_source_unchecked_no_save(rzi, rgroup,
                                                                            settings_source_before[rzi][rgroup]);
                    }
                }
                snprintf(err_msg, err_cap,
                        "zone tuning entry %u (channel %u) rejected at commit setting settings_source",
                        (unsigned)i, zc->index);
                return false;
            }
            settings_source_dirty = true;
        }
    }
    /* 2026-09-15 (Opus review item 3): safety_tc_type is no longer written
     * here. Since the F3 rework (safety_link_poll.c's 2026-09-15 comment)
     * the Pico's own commissioning page is the SOLE writer of its tc_type;
     * this ESP-side field is a deprecated read-back-only mirror. A backup
     * restore used to overwrite that mirror locally with whatever old value
     * the backup captured, which the ESP would then serve back out (GET
     * /api/zones) as if it were current -- exactly the staleness item 3
     * fixes on the read side (see zones_get_safety_pico_tc_type()). Writing
     * it here would just reintroduce the same staleness through a different
     * door immediately after. has_safety_tc/dsafety are still parsed and
     * range-checked above so an out-of-range value in an old backup still
     * fails the import loudly, rather than being silently ignored. */
    if (settings_source_dirty && !zones_config_save_now()) {
        snprintf(err_msg, err_cap, "settings_source commit succeeded live but failed to persist to flash");
        return false;
    }

    return true;
}

/* Wrapper: heap-allocates the two big candidate arrays (PSRAM preferred, see
 * this file's header comment above profile_candidate_t) and hands them to
 * backup_import_apply_locked(), which is otherwise byte-for-byte the
 * previous backup_import_apply() body. An allocation failure here is
 * reported exactly like any other pass-1 validation failure -- false plus an
 * err_msg, nothing touched -- so backup_import_post_handler's existing
 * "!ok -> 400, err_msg body" path handles it without change.
 *
 * `*partial_write_out` (task 6): the assessment's defect 2 was that
 * kiln_configs[] used to commit AFTER profiles/zones, so a mid-restore
 * failure there left profiles/zones (and an arbitrary already-applied
 * prefix of the kiln-config actions) persisted while the handler still
 * replied 400 -- a code that promises nothing changed. Fixed by committing
 * kiln_configs[] FIRST: if that fails, profiles/zones are still completely
 * untouched (the common, honest "nothing written" case, still a 400). Only
 * once kiln_configs[] has fully committed do we touch profiles/zones; if
 * THAT then fails, kiln_configs[] changes have already landed, so this sets
 * *partial_write_out = true and the caller reports a distinct 500 naming
 * what landed rather than a 400 implying nothing did. Always set on entry;
 * never left indeterminate on any return path. */
static bool backup_import_apply(const char *body, kiln_cfg_restore_mode_t mode, bool dry_run,
                                 int32_t ack_delete_count, kiln_cfg_plan_t *plan, bool *partial_write_out,
                                 char *err_msg, size_t err_cap)
{
    *partial_write_out = false;

    // Pass 1 for kiln_configs[] runs FIRST, unconditionally, before any
    // profile/zone candidate is even parsed -- same "validate everything,
    // then apply" discipline this file's own header comment describes for
    // the profiles/zones pass, just as its own separate step (kiln_cfg_store.h's
    // commit surface is not profiles_http_save()/zones_config_set_*()). A
    // malformed kiln_configs entry refuses the WHOLE restore, including
    // profiles/zones, exactly like a malformed profile/zone entry does today.
    if (!backup_import_kiln_configs(body, mode, false, ack_delete_count, plan, err_msg, err_cap)) {
        return false;
    }
    if (dry_run) {
        return true; // plan filled above; nothing written anywhere, profiles/zones untouched
    }

    // Pass 2 for kiln_configs[] now runs BEFORE profiles/zones (task 6):
    // create/rename/mirror-delete kiln config slots. If this fails, nothing
    // else has been touched yet.
    if (!backup_import_kiln_configs(body, mode, true, ack_delete_count, plan, err_msg, err_cap)) {
        return false;
    }

    profile_candidate_t *candidates = heap_caps_malloc(sizeof(profile_candidate_t) * PROFILES_MAX_COUNT,
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!candidates) {
        candidates = malloc(sizeof(profile_candidate_t) * PROFILES_MAX_COUNT);
    }
    if (!candidates) {
        snprintf(err_msg, err_cap, "out of memory (profile candidates) -- kiln configs were already restored");
        *partial_write_out = true;
        return false;
    }
    zone_candidate_t *zone_candidates = heap_caps_malloc(sizeof(zone_candidate_t) * MAX31856_CHANNEL_COUNT,
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!zone_candidates) {
        zone_candidates = malloc(sizeof(zone_candidate_t) * MAX31856_CHANNEL_COUNT);
    }
    if (!zone_candidates) {
        free(candidates);
        snprintf(err_msg, err_cap, "out of memory (zone candidates) -- kiln configs were already restored");
        *partial_write_out = true;
        return false;
    }

    timing_profile_candidate_t *timing_profile_candidates =
        heap_caps_malloc(sizeof(timing_profile_candidate_t) * MAX31856_CHANNEL_COUNT,
                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!timing_profile_candidates) {
        timing_profile_candidates = malloc(sizeof(timing_profile_candidate_t) * MAX31856_CHANNEL_COUNT);
    }
    if (!timing_profile_candidates) {
        free(zone_candidates);
        free(candidates);
        snprintf(err_msg, err_cap, "out of memory (timing profile candidates) -- kiln configs were already restored");
        *partial_write_out = true;
        return false;
    }

    bool ok = backup_import_apply_locked(body, err_msg, err_cap, candidates, zone_candidates,
                                         timing_profile_candidates);

    free(timing_profile_candidates);
    free(zone_candidates);
    free(candidates);
    if (!ok) {
        // kiln_configs[] already committed above -- this restore is a
        // partial write, not the clean "nothing changed" a 400 implies.
        *partial_write_out = true;
        return false;
    }
    return true;
}

esp_err_t backup_import_post_handler(httpd_req_t *req)
{
    /* Interlock FIRST, before reading the body at all -- "refused while a
     * profile is running or the heaters are on" is exactly
     * ota_http_check_interlocks()'s own precondition list (profile RUNNING/
     * PAUSED, autotune active, any zone's heater commanded on, any zone over
     * temperature, the safety link down, or another update already in
     * flight) -- see backup_http.h's header comment: this file's brief named
     * heat_interlock.c/.h, but that module answers the OPPOSITE question
     * ("may heat be commanded while an OTA update is in progress"), not "may
     * a config-changing action proceed while the kiln is hot" -- the
     * predicate this endpoint actually needs is ota_interlock.c/.h via this
     * same ota_http_check_interlocks() wrapper ota_http.c's own OTA routes
     * call before touching flash. Reusing it here (rather than writing a
     * second copy of the same profile/heater/temperature check) keeps there
     * being exactly one place this decision is made, matching the reuse this
     * pass's brief actually asked for even though the specific filename
     * named was the other half of that pair.
     *
     * 2026-08-22: the ONE precondition in that list a restore may proceed
     * past is "the safety link is down", and only when the operator has
     * answered the warning dialog for this request (the
     * X-Ota-Ack-No-Safety header). Restoring a saved configuration streams
     * nothing over that link -- the rule it was inheriting is
     * UPDATE_PROTOCOL.md's about firmware TRANSFERS -- and refusing
     * outright left a board whose safety processor is absent unable to
     * restore the very configuration that gets it commissioned. Every other
     * precondition still refuses unconditionally. */
    char reason[OTA_INTERLOCK_REASON_MAX];
    ota_interlock_result_t gate = ota_http_check_interlocks(ota_http_req_ack_no_safety(req), reason,
                                                            sizeof(reason));
    if (gate != OTA_INTERLOCK_OK) {
        return ota_http_send_interlock_refusal(req, gate, reason);
    }

    if (req->content_len <= 0 || (size_t)req->content_len > BACKUP_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }

    /* HEAP in PSRAM, not internal DRAM: same fix, same reasoning as this
     * file's export-side buffer above -- up to BACKUP_BODY_MAX (16384) bytes,
     * far too large to belong in internal DRAM alongside every other
     * handler's own locals on the shared httpd_worker stack/heap. Freed on
     * every return path below. */
    char *body = heap_caps_malloc((size_t)req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, (size_t)req->content_len - received);
        if (ret <= 0) {
            /* A short/failed read means the body this handler has is
             * incomplete -- e.g. the connection dropped mid-upload. Nothing
             * has been parsed or applied yet at this point (the read loop
             * runs entirely before backup_import_apply() is ever called), so
             * a truncated upload simply gets refused with nothing changed --
             * exactly the "must not leave configuration half-applied" case,
             * satisfied here by construction rather than by a rollback. */
            free(body);
            ESP_LOGW(BACKUP_TAG, "backup import body read failed/short: %d", ret);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "upload incomplete or connection dropped");
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    /* X-Kiln-Config-Mode: merge|mirror (default merge -- the safer choice,
     * per the owner decision), X-Kiln-Config-Dry-Run: 1 -- same header-ack
     * convention as OTA_ACK_NO_SAFETY_HEADER above (ota_http.c's
     * ota_http_req_ack_no_safety()). Dry-run computes and returns the
     * merge/mirror plan (every create/rename/delete, named) WITHOUT writing
     * anything -- profiles/zones/kiln_configs all untouched -- so
     * backup_page.html's confirm dialog can show the operator exactly what
     * a real restore would do before they commit to it. */
    kiln_cfg_restore_mode_t mode = KILN_CFG_RESTORE_MERGE;
    char mode_val[8];
    if (httpd_req_get_hdr_value_str(req, "X-Kiln-Config-Mode", mode_val, sizeof(mode_val)) == ESP_OK &&
        backup_names_equal_ci(mode_val, "mirror")) {
        mode = KILN_CFG_RESTORE_MIRROR;
    }
    bool dry_run = false;
    char dry_run_val[8];
    if (httpd_req_get_hdr_value_str(req, "X-Kiln-Config-Dry-Run", dry_run_val, sizeof(dry_run_val)) == ESP_OK &&
        dry_run_val[0] == '1') {
        dry_run = true;
    }

    /* X-Kiln-Config-Ack-Delete: <count> (task 8/11): a non-dry-run MIRROR
     * restore that would delete kiln config slots refuses unless this
     * header's value exactly equals the number of slots about to be
     * deleted -- an explicit, structural acknowledgement from the caller,
     * not just the shipped page's own confirm dialog, so any client hitting
     * this route is held to the same check. Absent/unparseable defaults to
     * -1, which can never equal a real (>=0) delete count, so an omitted
     * header behaves as "not acknowledged" rather than silently trusting a
     * missing value. */
    int32_t ack_delete_count = -1;
    char ack_delete_val[16];
    if (httpd_req_get_hdr_value_str(req, "X-Kiln-Config-Ack-Delete", ack_delete_val, sizeof(ack_delete_val)) ==
        ESP_OK) {
        char *endp = NULL;
        long parsed = strtol(ack_delete_val, &endp, 10);
        if (endp != ack_delete_val && parsed >= 0 && parsed <= INT32_MAX) {
            ack_delete_count = (int32_t)parsed;
        }
    }

    char err_msg[160];
    /* Task 5: kiln_cfg_plan_t (32 * 128 = 4096 bytes, KILN_CFG_PLAN_MAX_LINES *
     * KILN_CFG_PLAN_LINE_MAX) is heap-allocated rather than a local of this
     * httpd handler -- the same reasoning as the profiles/zones candidate
     * arrays in backup_import_apply_locked() above (check_httpd_task_stack_budget.py
     * / check_all_task_stack_budgets.py grade the httpd task's 8 KB stack
     * against every handler's own locals, and this struct alone was a
     * sizable chunk of it). Freed on every exit path below. */
    kiln_cfg_plan_t *plan = heap_caps_malloc(sizeof(kiln_cfg_plan_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!plan) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    bool partial_write = false;
    bool ok = backup_import_apply(body, mode, dry_run, ack_delete_count, plan, &partial_write, err_msg,
                                  sizeof(err_msg));
    free(body);

    if (!ok) {
        /* task 6: a partial write (kiln_configs[] already committed before
         * profiles/zones failed) is a distinct 500 naming what already
         * landed -- a 400 promises nothing changed, and that would be a lie
         * here. */
        httpd_resp_set_status(req, partial_write ? "500 Internal Server Error" : "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, err_msg, strlen(err_msg));
        free(plan);
        return ESP_OK;
    }

    if (dry_run) {
        /* Plain text, one plan line per line -- deliberately not JSON: plan
         * lines are built from operator-chosen kiln config names, which
         * name_charset_and_utf8_valid() does not forbid quote/backslash
         * characters from (kiln_cfg_store.c), so treating them as JSON
         * string content would need escaping this endpoint has no other
         * reason to carry. The client already reads this as plain text (see
         * backup_page.html's dry-run fetch) and joins it into the confirm
         * dialog verbatim. An empty plan (no lines) means this restore
         * changes no kiln config slots at all. */
        httpd_resp_set_type(req, "text/plain");
        char *out = heap_caps_malloc(KILN_CFG_PLAN_MAX_LINES * (KILN_CFG_PLAN_LINE_MAX + 1) + 1,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!out) {
            free(plan);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
            return ESP_OK;
        }
        size_t off = 0;
        out[0] = '\0';
        for (size_t i = 0; i < plan->count; i++) {
            int n = snprintf(out + off, KILN_CFG_PLAN_LINE_MAX + 2, "%s\n", plan->lines[i]);
            if (n > 0) {
                off += (size_t)n;
            }
        }
        esp_err_t send_err = httpd_resp_send(req, out, off);
        free(out);
        free(plan);
        return send_err;
    }

    free(plan);
    httpd_resp_set_type(req, "application/json");
    const char *ok_json = "{\"ok\":true}";
    return httpd_resp_send(req, ok_json, strlen(ok_json));
}
