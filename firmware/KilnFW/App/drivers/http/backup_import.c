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

#include "cfg_fs_refusal_http.h"
#include "backup_http.h"
#include "backup_http_internal.h"
#include "backup_json.h"
#include "backup_restore_state.h" /* backup_import_restore_in_flight() -- 2026-09-28 A4 review
                                    * follow-up A; s_backup_restore_in_flight below is this
                                    * file's definition of it. */

#include "http_async_job.h" /* http_async_job_busy() -- refuse a restore while ct_auto_zero's
                              * async job is mid-commit, 2026-09-25 fix-then-push review */

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h> /* _Atomic bool s_backup_restore_in_flight below -- same MSVC
                         * /experimental:c11atomics requirement live_profile.c's
                         * s_live_profile_generation already needs (build_host_tests.ps1). */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "MAX31856.h"
#include "hal_time.h" /* hal_time_now_us() -- batched-save timing instrumentation, see
                        * zones_snapshot's comment below. HAL_INCLUDE_BOUNDARY: never esp_timer.h here */
#include "kiln_cfg_store.h" /* KILN_PROFILES_PLAN.md item 17 follow-up -- kiln_configs[] restore */
#include "live_profile.h" /* live_edit_name_collides() -- Opus review finding B, the pass-1
                            * dup-name pre-check below (before pass 2 writes anything) */
#include "ota_http.h" /* ota_http_check_interlocks() -- see backup_http.h's header comment */
#include "persist_scratch.h" /* persist_scratch_alloc() -- pass-1 validation scratch */
#include "profiles_http.h"
#include "relay_authority.h" /* relay_authority_heat_run_active() -- see the system_mode_gate check below */
#include "system_mode_gate.h" /* SYS_ACTION_WRITE_ZONES_CONFIG -- owner decision Q2, 2026-09-25 */
#include "system_mode_gate_http.h" /* system_mode_gate_http_send_refusal() */
#include "profiles_http_internal.h" /* profiles_http_first_free_slot() -- Opus review nit N5, shared
                                      * with profiles_http.c's own first-free-slot scan */
#include "safety_ceiling_sync.h" /* 2026-09-10: a restored backup can raise max_temp_c same as a POST -- see
                                  * the guard immediately before the zone-tuning commit loop below. */
#include "safety_cfg_write.h" /* 2026-09-16: safety_cfg_write_apply_pairs() -- the same stage/COMMIT_CONFIG/
                                * forced-read-back-confirm path safety_cfg_http.c's own POST handler uses,
                                * reused here so the Pico's i_normal_a[0..2] restore is never trusted on a
                                * bare ACK -- see the push immediately after the ceiling guard below. */
#include "update_settings.h" /* WP9: top-level "update_repo" */
#include "relay_cycles.h" /* top-level "relay_cycles" wear counters */
#include "aux_outputs_cfg.h" /* top-level "aux_outputs" (spare-relay on/off outputs) */
#include "display_power_cfg.h" /* top-level "display_power" */
#include "profiles_builtin.h" /* top-level "hidden_builtin_profiles" */
#include "ramp_assist_cfg.h" /* top-level "ramp_assist" */
#include "time_sync.h" /* top-level "tz" */
#include "unit_pref.h" /* top-level "unit" */
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
 * backup_import_apply_two_pass(), otherwise byte-for-byte identical, EVERY
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

/* Pass-1 dup-name pre-check (Opus review finding B): name_at() adapters over
 * live_edit_name_collides() for the two sources a candidate name must be
 * checked against before ANY write happens -- the rest of this import batch,
 * and existing board slots this import will not itself overwrite. */
typedef struct {
    profile_candidate_t *candidates;
    size_t count;
} import_batch_name_ctx_t;

static const char *import_batch_name_at(void *ctx_v, uint8_t id)
{
    import_batch_name_ctx_t *ctx = (import_batch_name_ctx_t *)ctx_v;
    if (id >= ctx->count) { return NULL; }
    if (ctx->candidates[id].p.name[0] == '\0') { return NULL; }
    return ctx->candidates[id].p.name;
}

typedef struct {
    const bool *write_ids; /* PROFILES_MAX_COUNT entries; true = this import writes that slot */
} import_board_name_ctx_t;

static const char *import_board_name_at(void *ctx_v, uint8_t id)
{
    import_board_name_ctx_t *ctx = (import_board_name_ctx_t *)ctx_v;
    if (id >= PROFILES_MAX_COUNT || ctx->write_ids[id]) { return NULL; }
    /* Opus review of 5dd23944, nit 5: sizeof(profile_t) is well under 1 KB
     * (name + PROFILE_MAX_SEGMENTS segments + PROFILE_MAX_ON_OFF_RULES
     * on/off rules) -- small enough to fit comfortably on the httpd task's
     * 8 KB stack -- but this MUST stay `static` regardless of size: this
     * function returns a pointer INTO scratch (`scratch.name`) that its
     * caller (live_edit_name_collides_ex(), by way of the `name_at`
     * function-pointer seam) dereferences AFTER this function has already
     * returned. A stack-local `scratch` would make that a dangling-pointer
     * read the moment anything else touches this stack frame -- undefined
     * behavior, not merely a style nit -- regardless of how small the
     * struct is. `static` is the fix here, not the thing to remove; the
     * comment it replaces already said as much ("single-threaded import
     * path; no reentrancy"), which remains the correct and necessary
     * reasoning. */
    static profile_t scratch;
    if (!profiles_http_get(id, &scratch)) { return NULL; }
    return scratch.name;
}

/* profiles_http_first_free_slot()'s predicate callback (Opus review nit N5)
 * for this file's pass-1 SIMULATED table: ctx is the caller's own
 * slot_used_sim[PROFILES_MAX_COUNT] array. */
static bool backup_import_slot_used_sim_cb(void *ctx, uint8_t id)
{
    const bool *slot_used_sim = (const bool *)ctx;
    return slot_used_sim[id];
}

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
    bool tuning_matches_pre_commit; /* file record == live record BEFORE any commit-loop setter ran */
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
// Deliberately NOT folded into backup_import_apply_two_pass()'s own two-pass
// body above: kiln config slots are validated and committed via
// kiln_cfg_store.h's own API (kiln_cfg_store_validate_package_json()/
// kiln_cfg_store_import_package_json_as()/_rename()/_delete()/
// _set_active_id_raw()), not via the profiles_http_save()/
// zones_config_set_*() calls that function's pass 2 uses -- a different
// commit surface entirely. Kept as its own small pair of functions
// (validate/plan-only vs. commit) called from backup_import_apply() around
// backup_import_apply_two_pass(), same "validate everything, THEN apply"
// discipline, just sequenced as its own step rather than interleaved with
// the profile/zone candidate arrays. No httpd_req_t anywhere in this pair --
// host-testable exactly like backup_import_apply_two_pass() itself.
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

/* 144, not 128: WP9's dry-run line "update_repo <old> -> <new>" is at most
 * 12 + 62 + 4 + 62 = 140 characters. */
#define KILN_CFG_PLAN_LINE_MAX 144
#define KILN_CFG_PLAN_MAX_LINES 32

typedef struct {
    char lines[KILN_CFG_PLAN_MAX_LINES][KILN_CFG_PLAN_LINE_MAX];
    size_t count;
} kiln_cfg_plan_t;

static void kiln_cfg_plan_add(kiln_cfg_plan_t *plan, const char *fmt, ...)
{
    if (!plan || plan->count >= KILN_CFG_PLAN_MAX_LINES) {
        return; /* plan is informational for the confirm dialog -- silently
                  * capping it is acceptable; KILN_CFG_PLAN_MAX_LINES (32)
                  * still exactly covers the worst case, refreshed for the
                  * HIGH 2 "keep active" line (bkfinish review): at most one
                  * line per board slot (a MIRROR "delete" or "keep active",
                  * never both) plus at most TWO per file entry (its own
                  * create/rename/no-op-active line, plus a separate
                  * "was the active kiln config" informational line when
                  * is_active is set) -- 3 * KILN_CFG_MAX_COUNT (10) = 30,
                  * still <= 32. */
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

/* True when the file's tuning-quality record already equals the live one on
 * every field this import sets. zones_config_set_tuning_quality_no_save()
 * unconditionally bumps the zone's tuning_seq, which is part of the kiln
 * package canonical bytes / pkg_hash (but deliberately never exported or
 * imported), so re-committing an identical record would change the active
 * kiln_config's hash on every restore of an unchanged backup and break
 * identity matching (a re-import would then create a duplicate slot instead
 * of the "Case 1" no-op). The export prints the four floats with
 * BACKUP_TUNING_FLOAT_FMT, so the file only carries that precision and a raw
 * float compare would miss on real values (24.53719 vs 24.537). Equality is
 * therefore judged on the text each side prints with that same format. */
static bool backup_tuning_float_matches(float live, float file)
{
    char a[BACKUP_TUNING_FLOAT_BUF];
    char b[BACKUP_TUNING_FLOAT_BUF];
    snprintf(a, sizeof(a), BACKUP_TUNING_FLOAT_FMT, (double)live);
    snprintf(b, sizeof(b), BACKUP_TUNING_FLOAT_FMT, (double)file);
    return strcmp(a, b) == 0;
}

static bool backup_tuning_quality_matches_live(uint8_t zone_index, const zone_tuning_quality_t *q)
{
    zone_tuning_quality_t live;
    if (!zones_config_get_tuning_quality(zone_index, &live) || !live.valid) {
        return false;
    }
    return live.method == q->method && live.rule == q->rule && live.settled == q->settled &&
           live.extrapolation_converged == q->extrapolation_converged &&
           live.tau_consistent == q->tau_consistent &&
           backup_tuning_float_matches(live.baseline_c, q->baseline_c) &&
           backup_tuning_float_matches(live.step_ambient_c, q->step_ambient_c) &&
           backup_tuning_float_matches(live.raw_rise_c, q->raw_rise_c) &&
           backup_tuning_float_matches(live.rise_inf_c, q->rise_inf_c);
}

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
//
// `*wrote_out` (HIGH 1, bkfinish review): set to true the moment the FIRST
// store mutation of this commit pass actually lands (a successful rename,
// create, or mirror-delete), and left alone (not reset) on every later
// return so a caller can tell "refused before touching the store" (false)
// apart from "some earlier action in this same pass already landed before a
// later one failed" (true) -- the same distinction backup_import_apply()
// already makes between its own kiln_configs-pass and profiles/zones-pass.
// Always false on entry to a commit=true call and never touched at all when
// commit is false (pass-1 validation writes nothing). Callers that don't
// care may pass NULL.
static bool backup_import_kiln_configs(const char *body, kiln_cfg_restore_mode_t mode, bool commit,
                                        int32_t ack_delete_count, bool ack_no_safety_processor,
                                        kiln_cfg_plan_t *plan, bool *wrote_out, char *err_msg, size_t err_cap)
{
    if (commit && wrote_out) {
        *wrote_out = false;
    }
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
                /* HIGH 2 (bkfinish review): kiln_cfg_store_delete() refuses
                 * the active slot unconditionally, so an unmatched ACTIVE
                 * slot must never be queued for deletion here -- otherwise
                 * dry-run shows a confirmable plan that the real commit pass
                 * below then fails at, after any earlier renames/creates/
                 * deletes in this same pass have already landed. Named in
                 * the plan either way so the operator sees the slot survives
                 * mirror mode. */
                if (board[b].is_active) {
                    kiln_cfg_plan_add(plan, "keep active \"%s\" (active slot is never deleted)", board[b].name);
                    continue;
                }
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
            char rename_reason[96];
            rename_reason[0] = '\0';
            if (!kiln_cfg_store_rename_ex(action_board_id[i], action_name[i], rename_reason, sizeof(rename_reason))) {
                snprintf(err_msg, err_cap, "kiln_configs[%u]: rename to \"%.23s\" failed at commit%s%.60s",
                         (unsigned)i, action_name[i], rename_reason[0] ? ": " : "", rename_reason);
                return false;
            }
            if (wrote_out) {
                *wrote_out = true;
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
            if (wrote_out) {
                *wrote_out = true;
            }
        }
    }
    for (uint8_t b = 0; b < board_n; b++) {
        if (board_delete[b]) {
            char reason[128];
            /* ack_no_safety_processor: this is a SEPARATE acknowledgement
             * from `mirror_delete_ack` above (MEDIUM, bkfinish review) --
             * mirror_delete_ack only confirms the operator accepted how MANY
             * slots this restore deletes (X-Kiln-Config-Ack-Delete); this
             * argument is kiln_cfg_store_delete()'s own "the safety
             * processor is absent/down and the operator has been warned"
             * gate (ota_http_check_interlocks()'s ack_no_safety_processor,
             * same X-Ota-Ack-No-Safety header backup_import_post_handler()
             * already reads for the interlock check ahead of this function),
             * threaded through as its own parameter rather than reusing the
             * delete-count ack for an unrelated precondition. */
            if (!kiln_cfg_store_delete(board[b].id, ack_no_safety_processor, reason, sizeof(reason))) {
                snprintf(err_msg, err_cap, "kiln_configs: could not delete \"%.23s\" for mirror: %.60s", board[b].name,
                         reason);
                return false;
            }
            if (wrote_out) {
                *wrote_out = true;
            }
        }
    }
    return true;
}

/* check_httpd_task_stack_budget.py: scratch for the profile-slot-simulation
 * block inside backup_import_apply_two_pass() below -- see that block's own
 * comment. Heap-allocated (PSRAM preferred) rather than a plain local so it
 * does not add to backup_import_apply_two_pass()'s own httpd_worker frame. */
typedef struct {
    bool slot_used_sim[PROFILES_MAX_COUNT];
    bool write_ids[PROFILES_MAX_COUNT];
    char coll_err[128];
    /* Scratch for the existence-probe loop below (profiles_http_get()'s
     * output is discarded -- only the bool return matters) -- folded in here
     * rather than left as a ~428 B profile_t local of that loop, so it does
     * not land back on the httpd_worker stack this whole block was heap-
     * converted to get off of. */
    profile_t tmp_slot_check;
} backup_import_slot_scratch_t;

/* Portable noinline -- same guard as kiln_cfg_swap.c's KILN_CFG_SWAP_NOINLINE:
 * MSVC (host tests) rejects GCC's __attribute__((noinline)) syntax outright.
 * Only the Xtensa GCC target build's stack depth is measured. */
#if defined(_MSC_VER)
#define BACKUP_IMPORT_NOINLINE
#else
#define BACKUP_IMPORT_NOINLINE __attribute__((noinline))
#endif

/* One segment's seg_kind and RELAY_IO fields (io_target/io_state/io_blocking/io_leave_on_at_end), absent =
 * ZONE_RAMP with all io_* zero. Shared by the profile parse in backup_import_apply_two_pass() and the pass-1
 * candidate-state check backup_import_profiles_precheck(), so both read the same fields the same way.
 * `entry` and `seg_i` are 0-based and only feed the error text. */
static BACKUP_IMPORT_NOINLINE bool backup_import_parse_seg_kind_io(const char *se, size_t entry, uint8_t seg_i,
                                                                   profile_segment_t *sg, char *err_msg,
                                                                   size_t err_cap)
{
    double dkind = 0.0, dio = 0.0;
    bool has_kind = false, hv = false;
    if (!backup_json_field_opt_num(se, "seg_kind", 0, 1, &dkind, &has_kind, "seg_kind", NULL, 0, seg_i)) {
        snprintf(err_msg, err_cap, "profile entry %u, segment %u: seg_kind out of range", (unsigned)entry,
                 (unsigned)(seg_i + 1));
        return false;
    }
    sg->seg_kind = has_kind ? (uint8_t)dkind : PROFILE_SEG_KIND_ZONE_RAMP;
    sg->io_target = sg->io_state = sg->io_blocking = sg->io_leave_on_at_end = 0;
    if (!backup_json_field_opt_num(se, "io_target", 0, 255, &dio, &hv, "io_target", NULL, 0, seg_i)) {
        snprintf(err_msg, err_cap, "profile entry %u, segment %u: io_target out of range", (unsigned)entry,
                 (unsigned)(seg_i + 1));
        return false;
    }
    if (hv) sg->io_target = (uint8_t)dio;
    hv = false;
    if (!backup_json_field_opt_num(se, "io_state", 0, 1, &dio, &hv, "io_state", NULL, 0, seg_i)) {
        snprintf(err_msg, err_cap, "profile entry %u, segment %u: io_state out of range", (unsigned)entry,
                 (unsigned)(seg_i + 1));
        return false;
    }
    if (hv) sg->io_state = (uint8_t)dio;
    hv = false;
    if (!backup_json_field_opt_num(se, "io_blocking", 0, 1, &dio, &hv, "io_blocking", NULL, 0, seg_i)) {
        snprintf(err_msg, err_cap, "profile entry %u, segment %u: io_blocking out of range", (unsigned)entry,
                 (unsigned)(seg_i + 1));
        return false;
    }
    if (hv) sg->io_blocking = (uint8_t)dio;
    hv = false;
    if (!backup_json_field_opt_num(se, "io_leave_on_at_end", 0, 1, &dio, &hv, "io_leave_on_at_end", NULL, 0,
                                   seg_i)) {
        snprintf(err_msg, err_cap, "profile entry %u, segment %u: io_leave_on_at_end out of range",
                 (unsigned)entry, (unsigned)(seg_i + 1));
        return false;
    }
    if (hv) sg->io_leave_on_at_end = (uint8_t)dio;
    return true;
}

/* One on_off_rules[] entry. Same fields/ranges as profiles_export_http.c's importer; the target/segment/aux
 * checks are validate_on_off_rules_in_state()'s job. Shared with backup_import_profiles_precheck(). */
static BACKUP_IMPORT_NOINLINE bool backup_import_parse_rule(const char *re, size_t entry, uint8_t rule_i,
                                                            profile_on_off_rule_t *r, char *err_msg, size_t err_cap)
{
    double dz = 0.0, dsg = 0.0, dv = 0.0;
    bool hv = false;
    if (!backup_json_field_num(re, "zone", &dz) || dz < 0 || dz > 255 ||
        !backup_json_field_num(re, "segment", &dsg) || dsg < 0 || dsg > 255) {
        snprintf(err_msg, err_cap, "profile entry %u, rule %u: zone/segment missing or invalid", (unsigned)entry,
                 (unsigned)(rule_i + 1));
        return false;
    }
    r->zone_index = (uint8_t)dz;
    r->segment_index = (uint8_t)dsg;
#define BK_RULE_NUM(dst, key, lo, hi, cast)                                                                    \
    do {                                                                                                       \
        dst = 0;                                                                                               \
        hv = false;                                                                                            \
        if (!backup_json_field_opt_num(re, key, lo, hi, &dv, &hv, key, NULL, 0, rule_i)) {                     \
            snprintf(err_msg, err_cap, "profile entry %u, rule %u: " key " out of range", (unsigned)entry,     \
                    (unsigned)(rule_i + 1));                                                                   \
            return false;                                                                                      \
        }                                                                                                      \
        if (hv) dst = (cast)dv;                                                                                \
    } while (0)
    BK_RULE_NUM(r->enable, "enable", 0, 1, uint8_t);
    BK_RULE_NUM(r->phase_mask, "phase_mask", 0, 255, uint8_t);
    BK_RULE_NUM(r->direction_mask, "direction_mask", 0, 255, uint8_t);
    BK_RULE_NUM(r->temp_source, "temp_source", 0, 3, uint8_t);
    BK_RULE_NUM(r->temp_cmp, "temp_cmp", 0, 255, uint8_t);
    BK_RULE_NUM(r->temp_threshold_c, "temp_c", (double)PROFILE_TARGET_C_MIN, (double)PROFILE_TARGET_C_MAX, float);
    BK_RULE_NUM(r->time_start_s, "time_start_s", 0, 65535, uint16_t);
    BK_RULE_NUM(r->time_stop_s, "time_stop_s", 0, 65535, uint16_t);
    BK_RULE_NUM(r->invert, "invert", 0, 1, uint8_t);
#undef BK_RULE_NUM
    return true;
}

/* Defined below with the update_repo/aux code; apply_locked() runs them between the zone commit and the
 * profile commit (see its commit-order comment). */
static bool backup_import_update_repo(const char *body, bool commit, kiln_cfg_plan_t *plan, char *err_msg,
                                      size_t err_cap);
static bool backup_import_relay_cycles(const char *body, bool commit, kiln_cfg_plan_t *plan, char *err_msg,
                                       size_t err_cap);
static bool backup_import_prefs(const char *body, bool commit, kiln_cfg_plan_t *plan, char *err_msg,
                               size_t err_cap);
static bool backup_import_aux_outputs_commit(const char *body, bool enable_phase, bool *wrote, char *err_msg,
                                             size_t err_cap);

/* Board topology a restore works against (owner decision 2026-10-09: a one-file restore must work after a
 * factory reset). A v7+ backup carries top-level thermo_count/relay_count. Onto an UNCONFIGURED board (live
 * thermo_count 0) the restore sets them first (apply = true) and every later zone/profile check judges
 * against them; a configured board must already match. A backup with neither key (v6 or older) leaves the
 * live counts in force. Pass 1 only: reads and validates, writes nothing. */
typedef struct {
    uint8_t thermo;
    uint8_t relay;
    bool apply;
} backup_topology_t;

static BACKUP_IMPORT_NOINLINE bool backup_import_resolve_topology(const char *body, backup_topology_t *out,
                                                                   char *err_msg, size_t err_cap)
{
    out->thermo = zones_config_get_thermo_count();
    out->relay = zones_config_get_relay_count();
    out->apply = false;
    double dt = 0.0, dr = 0.0;
    bool has_t = backup_json_field_num(body, "thermo_count", &dt);
    bool has_r = backup_json_field_num(body, "relay_count", &dr);
    if (!has_t && !has_r) {
        return true; /* older backup: keep the live counts (and the zone-count refusal in the precheck) */
    }
    if (!has_t || !has_r || dt < 0 || dt > MAX31856_CHANNEL_COUNT || dt != (double)(int)dt || dr < 0 ||
        dr > KILN_IO_RELAY_COUNT || dr != (double)(int)dr) {
        snprintf(err_msg, err_cap,
                 "backup thermo_count/relay_count must both be present integers (thermo 0-%u, relay 0-%u). "
                 "Nothing was written.",
                 (unsigned)MAX31856_CHANNEL_COUNT, (unsigned)KILN_IO_RELAY_COUNT);
        return false;
    }
    uint8_t bt = (uint8_t)dt, br = (uint8_t)dr;
    if (out->thermo == 0) {
        if (bt > 0) {
            out->thermo = bt;
            out->relay = br;
            out->apply = true;
        }
        return true;
    }
    if (bt != out->thermo || br != out->relay) {
        snprintf(err_msg, err_cap,
                 "backup board topology (thermo_count %u, relay_count %u) differs from this board's "
                 "(thermo_count %u, relay_count %u). Nothing was written.",
                 (unsigned)bt, (unsigned)br, (unsigned)out->thermo, (unsigned)out->relay);
        return false;
    }
    return true;
}

static bool backup_import_apply_two_pass(const char *body, char *err_msg, size_t err_cap,
                                        profile_candidate_t *candidates, zone_candidate_t *zone_candidates,
                                        timing_profile_candidate_t *timing_profile_candidates,
                                        bool *zones_landed_out, bool *aux_wrote_out, bool parse_only)
{
    *zones_landed_out = false;
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
    backup_topology_t topo;
    if (!backup_import_resolve_topology(body, &topo, err_msg, err_cap)) {
        return false;
    }
    uint8_t thermo_count = topo.thermo;
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
            double dt = 0.0, dr = 0.0, dd;
            profile_segment_t *sg = &c->p.segments[seg_i];
            if (!backup_import_parse_seg_kind_io(se, candidate_count, seg_i, sg, err_msg, err_cap)) {
                return false;
            }
            uint8_t kind = sg->seg_kind;
            if (kind == PROFILE_SEG_KIND_ZONE_RAMP &&
                (!backup_json_field_num(se, "target_c", &dt) || dt < bound_target_min || dt > bound_target_max)) {
                snprintf(err_msg, err_cap, "profile entry %u, segment %u: target_c missing or out of range",
                        (unsigned)candidate_count, (unsigned)(seg_i + 1));
                return false;
            }
            if (kind == PROFILE_SEG_KIND_ZONE_RAMP &&
                (!backup_json_field_num(se, "ramp_c_per_hr", &dr) || dr < bound_ramp_min || dr > bound_ramp_max)) {
                snprintf(err_msg, err_cap, "profile entry %u, segment %u: ramp_c_per_hr missing or out of range",
                        (unsigned)candidate_count, (unsigned)(seg_i + 1));
                return false;
            }
            if (kind == PROFILE_SEG_KIND_RELAY_IO) {
                /* Not meaningful for this kind, but profiles_export_http.c keeps whatever the slot holds, so
                 * keep it here too: the backup round trip stays byte-identical. Absent = 0, as before. */
                double dkeep = 0.0;
                if (backup_json_field_num(se, "target_c", &dkeep) && isfinite(dkeep)) {
                    dt = dkeep;
                }
                dkeep = 0.0;
                if (backup_json_field_num(se, "ramp_c_per_hr", &dkeep) && isfinite(dkeep)) {
                    dr = dkeep;
                }
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

        /* on_off_rules (absent key = zero rules, today's behavior). Same fields/ranges as
         * profiles_export_http.c's importer; profiles_http_save() validates targets later. */
        {
            const char *rules_arr = backup_json_obj_find(pe, "on_off_rules");
            uint8_t rule_i = 0;
            for (const char *re = backup_json_arr_first(rules_arr); re; re = backup_json_arr_next(re)) {
                if (rule_i >= PROFILE_MAX_ON_OFF_RULES) {
                    snprintf(err_msg, err_cap, "profile entry %u: too many on_off_rules", (unsigned)candidate_count);
                    return false;
                }
                if (!backup_import_parse_rule(re, candidate_count, rule_i, &c->p.on_off_rules[rule_i], err_msg,
                                              err_cap)) {
                    return false;
                }
                rule_i++;
            }
            c->p.on_off_rule_count = rule_i;
        }

        /* Same feasibility rule profiles_http_save() enforces -- duplicated
         * here so it is caught in validation, before any profile in this
         * import has been written. */
        for (uint8_t i = 0; i < c->p.segment_count; i++) {
            float rate = c->p.segments[i].ramp_c_per_hr;
            if (c->p.segments[i].seg_kind != PROFILE_SEG_KIND_ZONE_RAMP || rate <= 0.0f) {
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

    /* ---- Pass 1a-dup: refuse a duplicate/colliding profile name BEFORE any
     * profile in this import is written (Opus review finding B). Without
     * this, pass 2 below only discovers a name collision (profiles_http_save()
     * now runs live_edit_name_collides(), same as the interactive POST
     * /api/profile path) one profile at a time, mid-commit -- entries
     * 0..i-1 already landed on the board by the time entry i is refused, so
     * a board holding two same-named slots that gets exported and
     * re-imported now aborts half-applied instead of either fully applying
     * or fully refusing.
     *
     * Two kinds of collision must both be caught here:
     *   1. an intra-batch duplicate -- two candidates in THIS SAME import
     *      normalizing to the same name (import_batch_name_at()/
     *      batch_ctx below, scanned via live_edit_name_collides() itself so
     *      this shares its exact case/whitespace normalization rather than
     *      a second hand-rolled copy of it).
     *   2. a candidate colliding with an EXISTING board slot that this
     *      import will NOT itself overwrite (import_board_name_at()/
     *      board_ctx below) -- exactly profiles_http_save()'s own rule,
     *      except every id this import is about to write is excluded from
     *      the existing-slot scan (write_ids[]), so a candidate that
     *      overwrites the very slot already holding that name stays legal,
     *      same as profiles_http_save()'s exclude_id.
     *
     * write_ids[] is computed by simulating profiles_http_save()'s own
     * first-free-slot allocation for every !has_id candidate, in the same
     * candidate order pass 2 below commits them in -- the set of slots
     * pass 2 will actually touch is otherwise not knowable in advance for
     * an import that mixes explicit ids with "first free". An unnamed
     * candidate (backup_json_field_str() found no "name" key -- the import
     * format allows this, unlike the interactive POST path) is exempt from
     * both checks, matching the pre-existing behavior of every check above:
     * nothing here newly rejects an import that was previously accepted. */
    {
        /* check_httpd_task_stack_budget.py: these two PROFILES_MAX_COUNT
         * bool arrays plus the per-candidate coll_err[128] below used to be
         * plain locals of this block, contributing to this function's own
         * frame on the httpd_worker path (backup_import_post_handler ->
         * backup_import_apply_two_pass). Bundled into one heap allocation,
         * freed on every return out of this block, same "malloc + free on
         * every return path, 500-equivalent refusal on OOM" convention this
         * file already uses for candidates/zone_candidates/
         * timing_profile_candidates above. */
        backup_import_slot_scratch_t *scratch = heap_caps_malloc(sizeof(*scratch), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!scratch) {
            scratch = malloc(sizeof(*scratch));
        }
        if (!scratch) {
            snprintf(err_msg, err_cap, "out of memory (profile slot scratch)");
            return false;
        }
        bool *slot_used_sim = scratch->slot_used_sim;
        bool *write_ids = scratch->write_ids;
        for (uint8_t id = 0; id < PROFILES_MAX_COUNT; id++) {
            memset(&scratch->tmp_slot_check, 0, sizeof(scratch->tmp_slot_check));
            slot_used_sim[id] = profiles_http_get(id, &scratch->tmp_slot_check);
            write_ids[id] = false;
        }
        /* Opus review of 5dd23944, finding 2: this must simulate pass 2's
         * commit loop in EXACT candidate order, one loop, has_id and !has_id
         * candidates interleaved exactly as they appear -- not (as before)
         * a first pass marking every has_id slot used, THEN a second pass
         * allocating first-free slots for every !has_id candidate. That
         * two-pass shape lets an EARLIER !has_id candidate grab a first-free
         * slot that a LATER has_id candidate was always going to reserve
         * for itself (pass 2 below commits candidates[i] with
         * profiles_http_save(has_id ? id : PROFILES_MAX_COUNT, ...) in
         * candidate order, so a first-free allocation made here for
         * candidate i must only ever see the has_id reservations of
         * candidates BEFORE i, exactly as pass 2's live slot_used state
         * would at that point). Example the reviewer gave: board slot 0
         * used, slot 1 free, slot 3 holds "A"; candidates
         * [{no id,"A"}, {id:1,"Q"}] -- the old two-pass sim marked slot 1
         * used for candidate 1 BEFORE candidate 0's first-free scan ran, so
         * candidate 0 (no id) landed on slot 2, missing that pass 2 would
         * actually give candidate 0 slot 1 (nothing marks it used yet at
         * that point in commit order) and then collide committing candidate
         * 1 into the same slot. This single-loop simulation instead gives
         * candidate 0 slot 1 up front, matching pass 2 exactly, so this
         * check now refuses in pass 1 (a duplicate "A"/slot-1-vs-slot-3
         * name collision) with zero writes, instead of passing pass 1 and
         * colliding mid-commit in pass 2. */
        for (size_t i = 0; i < candidate_count; i++) {
            if (candidates[i].has_id) {
                write_ids[candidates[i].id] = true;
                slot_used_sim[candidates[i].id] = true;
                continue;
            }
            /* profiles_http_first_free_slot() (Opus review nit N5) -- shared
             * with profiles_http_save()'s own first-free-slot scan
             * (profiles_http.c) so the two allocation policies cannot
             * silently drift apart. This caller passes the SIMULATED
             * slot_used_sim table (this pass-1 loop's own commit-order
             * simulation, per the comment above), not the board's live
             * profiles_slot_used() state -- see the helper's own comment for
             * why that split exists. */
            int free_slot = profiles_http_first_free_slot(backup_import_slot_used_sim_cb, slot_used_sim);
            if (free_slot < 0) {
                /* profiles_http_save() would refuse this exact way at
                 * commit time ("profile storage full") -- catching it here
                 * too means the whole import is refused up front rather
                 * than partially committing the candidates before it. */
                snprintf(err_msg, err_cap, "profile storage full");
                free(scratch);
                return false;
            }
            write_ids[free_slot] = true;
            slot_used_sim[free_slot] = true;
        }

        import_batch_name_ctx_t batch_ctx = { .candidates = candidates, .count = candidate_count };
        import_board_name_ctx_t board_ctx = { .write_ids = write_ids };
        for (size_t i = 0; i < candidate_count; i++) {
            if (candidates[i].p.name[0] == '\0') {
                continue; /* unnamed candidate -- see comment above */
            }
            char *coll_err = scratch->coll_err;
            /* include_builtins=false both here and below (Opus review of
             * 5dd23944, finding 1/BLOCKER): these candidates land in USER
             * slots, and a backup legitimately containing a user copy of a
             * builtin (or an existing board slot already named like one)
             * must import, not refuse the whole restore. See
             * live_edit_name_collides_ex()'s doc comment. */
            if (live_edit_name_collides_ex(candidates[i].p.name, import_batch_name_at, &batch_ctx, (uint8_t)i, false,
                                            coll_err, sizeof(scratch->coll_err))) {
                /* -Werror=format-truncation: bound the %s width explicitly
                 * so GCC can prove this fits at every call site's err_cap,
                 * rather than assuming coll_err's full 128-byte declared
                 * size could land in err_msg -- the single-loop pass-1
                 * simulation above (finding 2) changed how this function
                 * gets inlined/constant-propagated at its callers, which is
                 * what surfaced this previously-quiet truncation risk. */
                snprintf(err_msg, err_cap, "profile entry %u: duplicate name within this import (%.80s)",
                        (unsigned)i, coll_err);
                free(scratch);
                return false;
            }
            if (live_edit_name_collides_ex(candidates[i].p.name, import_board_name_at, &board_ctx, 0xFF, false,
                                            coll_err, sizeof(scratch->coll_err))) {
                snprintf(err_msg, err_cap, "profile entry %u: %.80s", (unsigned)i, coll_err);
                free(scratch);
                return false;
            }
        }
        free(scratch);
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
        /* Two entries for one zone would make the later commit order (and the
         * pre-commit tuning compare) ambiguous; a hand-made file only. */
        for (size_t j = 0; j < zone_candidate_count; j++) {
            if (zone_candidates[j].index == zc->index) {
                snprintf(err_msg, err_cap, "zone tuning entry %u: duplicate index %u",
                         (unsigned)zone_candidate_count, zc->index);
                return false;
            }
        }
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
        /* Mirror zones_config_set_pid_no_save(): finite float, <= ZONE_PID_GAIN_MAX, so
         * pass 2 can never fail on a gain after other stores are committed. */
        if (!isfinite(dkp) || !isfinite(dki) || !isfinite(dkd) || dkp > (double)ZONE_PID_GAIN_MAX ||
            dki > (double)ZONE_PID_GAIN_MAX || dkd > (double)ZONE_PID_GAIN_MAX) {
            snprintf(err_msg, err_cap, "zone tuning entry %u: pid_kp/pid_ki/pid_kd exceed the gain ceiling",
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
            uint8_t relay_count = topo.relay;
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
            if (!isfinite(dfittemp) || !isfinite(dfitambient) || fabs(dfittemp) > 1e6 || fabs(dfitambient) > 1e6 ||
                !zones_config_model_fit_temp_valid((float)dfittemp) ||
                !zones_config_model_fit_temp_valid((float)dfitambient)) {
                snprintf(err_msg, err_cap, "zone tuning entry %u: model_fit_temp_c/model_fit_ambient_c out of range",
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
            if (!(dtmethod >= 0 && dtmethod <= 1) || !(dtrule >= 0 && dtrule <= 3) || fabs(dtbaseline) > 1e6 ||
                fabs(dtstepamb) > 1e6 || fabs(dtrawrise) > 1e6 || fabs(dtriseinf) > 1e6 ||
                !zones_config_tuning_quality_fields_valid((unsigned)dtmethod, (unsigned)dtrule, (float)dtbaseline,
                                                          (float)dtstepamb, (float)dtrawrise, (float)dtriseinf)) {
                snprintf(err_msg, err_cap, "zone tuning entry %u: tuning_* field out of range",
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
    if (parse_only) {
        return true; /* DEV_REVIEW_13 F8: every parse/validate refusal above happens before any write */
    }

    /* opus review finding (LOW-MEDIUM), originally closed with a narrow
     * settings_source[]-only snapshot/restore here: superseded below by
     * zones_snapshot, a whole-zones_cfg_t snapshot taken right before the
     * timing-profile/per-zone commit loops, which now covers every field
     * those loops touch (not just settings_source) with a single restore
     * call on any mid-batch failure. */

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
            if (topo.apply && zi < topo.thermo) {
                cur_max = zones_config_peek_slot_max_temp_c(zi); /* slot goes live with the topology */
            }
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
            uint64_t pico_roundtrip_start_us = hal_time_now_us();
            bool pico_ok = safety_cfg_write_apply_pairs(s_hw_safety, pairs, n_pairs, true /* commit */, reason,
                                              sizeof(reason), &out_class);
            ESP_LOGI(BACKUP_TAG, "backup import: Pico i_normal_a round trip (%d pairs) took %lld ms",
                    n_pairs, (long long)((hal_time_now_us() - pico_roundtrip_start_us) / 1000u));
            if (!pico_ok) {
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
    /* ---- Batched commit: everything from here down mutates zones_cfg_t
     * in RAM only (the _no_save() variants) and persists once, via a single
     * zones_config_save_now() call after the whole batch, instead of the
     * ~35-setters-times-N-zones worth of individual nvs_save() calls this
     * loop used to make (each one a CRC recompute, a cfg LittleFS write, an
     * NVS commit, and a blocking flash-worker dispatch -- see this file's
     * outer comment / the MCP tooling note on POST /api/backup/import's
     * cost). Snapshot the whole live config first so a mid-batch failure
     * anywhere below (timing profiles, per-zone fields, settings_source) can
     * restore RAM to exactly what it was before this function touched
     * anything, rather than leaving some fields committed and others not
     * with nothing persisted for any of it -- same "RAM must never run ahead
     * of NVS" discipline this file already applied narrowly to
     * settings_source_before[][] above, now covering the entire batch. Every
     * `return false` between this snapshot and the final save below must
     * restore it first. */
    zones_cfg_t zones_snapshot;
    zones_config_get_full_copy(&zones_snapshot);
    /* Topology first (unconfigured board only, planned in pass 1): the per-zone setters below refuse any
     * index >= thermo_count and any relay_mask past relay_count. RAM only; the batch's single save persists
     * it, and the snapshot above restores it on a mid-batch failure. */
    if (topo.apply && !zones_config_set_topology_no_save(topo.thermo, topo.relay)) {
        snprintf(err_msg, err_cap, "could not apply the backup's board topology (thermo_count %u, relay_count %u)",
                 (unsigned)topo.thermo, (unsigned)topo.relay);
        return false;
    }
    /* Judge "does the file's tuning record equal the live one" NOW, before
     * the loop below runs. zones_config_set_pid_no_save() invalidates the
     * zone's tuning record (tuning_valid = 0) when a gain changes beyond
     * zones_config_gain_changed()'s tolerance (equal-within-tolerance gains
     * leave it standing), so a compare made after it reads "not valid"
     * whenever the file's gains differ from the live ones, and a restore of
     * such a backup would re-commit the record and bump tuning_seq (and
     * with it the active kiln_config's pkg_hash) even when the record is
     * unchanged. 2026-10-05 bench finding:
     * the earlier fix compared at commit time, after set_pid, and was inert
     * on hardware. */
    for (size_t i = 0; i < zone_candidate_count; i++) {
        zone_candidate_t *zc = &zone_candidates[i];
        zc->tuning_matches_pre_commit =
            zc->has_tuning_quality && backup_tuning_quality_matches_live(zc->index, &zc->tuning_quality);
    }
    bool relay_type_changed[MAX31856_CHANNEL_COUNT] = {0};
    uint64_t setter_loop_start_us = hal_time_now_us();

    for (size_t i = 0; i < timing_profile_candidate_count; i++) {
        timing_profile_candidate_t *tp = &timing_profile_candidates[i];
        if (!zones_config_set_timing_profile_raw_no_save((uint8_t)i, tp->name, tp->progress_duty_min,
                                                 tp->progress_window_s, tp->drift_hysteresis_c, tp->frozen_eps_c,
                                                 tp->cross_zone_period_s, tp->bangbang_hysteresis_c,
                                                 tp->cooling_limited_margin_c, tp->cooling_limited_hold_s,
                                                 tp->ramp_lock_band_c)) {
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            snprintf(err_msg, err_cap, "timing profile entry %u rejected at commit", (unsigned)i);
            return false;
        }
    }

    for (size_t i = 0; i < zone_candidate_count; i++) {
        zone_candidate_t *zc = &zone_candidates[i];
        if (!zones_config_set_pid_no_save(zc->index, zc->kp, zc->ki, zc->kd)) {
            snprintf(err_msg, err_cap, "zone tuning entry %u (channel %u) rejected at commit setting PID gains (invalid value, or a profile/autotune run was starting -- retry in a moment)",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_model && !zones_config_set_model_no_save(zc->index, zc->k_dc, zc->tau_s, zc->dead_time_s)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting the plant model -- value "
                    "outside this firmware's sanity bounds",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_tc && !zones_config_set_tc_type_no_save(zc->index, zc->tc_type)) {
            snprintf(err_msg, err_cap, "zone tuning entry %u (channel %u) rejected at commit setting tc_type",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        /* Version 2 fields -- see zone_candidate_t's comment. Every one of
         * these setters was just validated against the exact same bound in
         * pass 1 above, so a commit-time rejection here means a race with a
         * concurrent config change between the two passes (same rationale
         * as the PID/model/tc_type "should not happen" comments above), not
         * a bug in this pass's own bounds. */
        if (zc->has_name && !zones_config_set_name_no_save(zc->index, zc->name)) {
            snprintf(err_msg, err_cap, "zone tuning entry %u (channel %u) rejected at commit setting name",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_relay_mask && !zones_config_set_relay_mask_no_save(zc->index, zc->relay_mask)) {
            snprintf(err_msg, err_cap, "zone tuning entry %u (channel %u) rejected at commit setting relay_mask",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_thermo_mask && !zones_config_set_thermo_mask_no_save(zc->index, zc->thermo_mask)) {
            snprintf(err_msg, err_cap, "zone tuning entry %u (channel %u) rejected at commit setting thermo_mask",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_ct_mask && !zones_config_set_ct_mask_no_save(zc->index, zc->ct_mask)) {
            snprintf(err_msg, err_cap, "zone tuning entry %u (channel %u) rejected at commit setting ct_mask",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_cal && !zones_config_set_cal_offset_no_save(zc->index, zc->cal_offset_c)) {
            snprintf(err_msg, err_cap, "zone tuning entry %u (channel %u) rejected at commit setting cal_offset_c",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_ramp && !zones_config_set_max_ramp_no_save(zc->index, zc->max_ramp_c_per_hr)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting max_ramp_c_per_hr",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_sanity && !zones_config_set_sanity_rate_no_save(zc->index, zc->sanity_rate_c_per_min)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting sanity_rate_c_per_min",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_mode && !zones_config_set_control_mode_no_save(zc->index, (zone_control_mode_t)zc->control_mode)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting control_mode",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_temp_limits && !zones_config_set_temp_limits_no_save(zc->index, zc->max_temp_c, zc->min_temp_c)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting temp limits",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_heater_cfg &&
            !zones_config_set_heater_cfg_no_save(zc->index, zc->heater_window_ms, zc->heater_min_on_ms,
                                         zc->heater_min_off_ms)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting heater timing",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_guard &&
            !zones_config_set_guard_thresholds_no_save(zc->index, zc->guard_wrong_dir_window_s,
                                               zc->guard_wrong_dir_rate_c_per_min, zc->guard_off_settle_s,
                                               zc->guard_runaway_rate_c_per_min, zc->guard_runaway_margin_c,
                                               zc->guard_drift_period_s, zc->guard_sensor_fault_debounce_ticks,
                                               zc->guard_frozen_window_s)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting guard thresholds",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_cross_zone && !zones_config_set_cross_zone_delta_no_save(zc->index, zc->cross_zone_max_delta_c)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting cross_zone_max_delta_c",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_fuzzy_strength && !zones_config_set_fuzzy_strength_pct_no_save(zc->index, zc->fuzzy_strength_pct)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting fuzzy_strength_pct",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        /* Per-cell, not whole-row: an import that only supplies (or only
         * ever had, pre-version-4) one neighbor's coefficient must not blank
         * out this zone's OTHER already-stored neighbors -- same "omit
         * preserves the current value" convention as fuzzy_strength_pct
         * above, applied per cell instead of per field. */
        for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
            /* ZONES_CFG_VERSION 11->12: zones_config_set_coupling_cell_no_save() is
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
                /* _raw, not the masking zones_config_get_coupling(): this
                 * value is written straight back to storage below, and the
                 * masked getter reads 0.0 for any cell touching an on/off
                 * zone (docs/ON_OFF_ZONE.md sec 1) -- an import that
                 * supplies only tau/dead_time for such a cell would then
                 * commit that 0.0 over the real stored coefficient, the same
                 * loss the export-side fix (bench A4, 2026-09-28) closed. */
                zones_config_get_coupling_raw(zc->index, cur_coeff_row);
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
            if (!zones_config_set_coupling_cell_no_save(zc->index, j, coeff, tau_s, dead_time_s)) {
                snprintf(err_msg, err_cap,
                        "zone tuning entry %u (channel %u) rejected at commit setting coupling_c%u",
                        (unsigned)i, zc->index, (unsigned)j);
                zones_config_restore_snapshot_no_save(&zones_snapshot);
                return false;
            }
        }
        /* ZONES_CFG_VERSION 14->15 (PID_EXPANSION_PLAN.md 3.2 follow-up):
         * coupling_diag_k_dc -- same "omit preserves the current value"
         * convention as fuzzy_strength_pct above (this is a measured
         * quantity, not a setting an absent import should reset). */
        if (zc->has_coupling_diag_k_dc &&
            !zones_config_set_coupling_diag_k_dc_no_save(zc->index, zc->coupling_diag_k_dc)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting coupling_diag_k_dc",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        /* 2026-09-16 backup-round-trip-gap closure -- "omit preserves the
         * current value" throughout, same as fuzzy_strength_pct/
         * coupling_diag_k_dc above: every one of these was independently
         * optional in pass 1, so an absent key here means an older backup
         * (or a hand-edited one), not "reset to zero". */
        if (zc->has_ease_off_window_mult &&
            !zones_config_set_ease_off_window_mult_no_save(zc->index, zc->ease_off_window_mult)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting ease_off_window_mult",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_approach_rate_cap &&
            !zones_config_set_approach_rate_cap_c_per_hr_no_save(zc->index, zc->approach_rate_cap_c_per_hr)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting approach_rate_cap_c_per_hr",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_error_band_c && !zones_config_set_error_band_c_no_save(zc->index, zc->error_band_c)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting error_band_c",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_rate_band_c_per_s && !zones_config_set_rate_band_c_per_s_no_save(zc->index, zc->rate_band_c_per_s)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting rate_band_c_per_s",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        /* zones_config_set_relay_type_no_save() does NOT push the new type
         * out to relay_cycles_set_type() -- that push is deferred until
         * after the single batched zones_config_save_now() call below
         * succeeds (relay_type_changed[] records which zones need it), same
         * "commit RAM, then push hardware only once the save that backs it
         * is confirmed" ordering as everything else in this batch. */
        if (zc->has_relay_type && !zones_config_set_relay_type_no_save(zc->index, zc->relay_type)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting relay_type",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_relay_type) {
            relay_type_changed[zc->index] = true;
        }
        if (zc->has_progress_band_c && !zones_config_set_progress_band_c_no_save(zc->index, zc->progress_band_c)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting progress_band_c",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_zone_type && !zones_config_set_zone_type_no_save(zc->index, (zone_type_t)zc->zone_type)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting zone_type",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_model_fit_context &&
            !zones_config_set_model_fit_context_no_save(zc->index, zc->model_fit_temp_c, zc->model_fit_ambient_c)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting model fit context",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_coil_power_w && !zones_config_set_coil_power_w_no_save(zc->index, zc->coil_power_w)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting coil_power_w",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_autotune_baseline_k_dc &&
            !zones_config_set_autotune_baseline_k_dc_no_save(zc->index, zc->autotune_baseline_k_dc)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting autotune_baseline_k_dc",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_adaptive_tune_enabled &&
            !zones_config_set_adaptive_tune_enabled_no_save(zc->index, zc->adaptive_tune_enabled)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting adaptive_tune_enabled",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_tuning_quality && zc->tuning_matches_pre_commit) {
            /* Identical record: undo set_pid's invalidation only. Live floats
             * and tuning_seq stay exactly as they were. */
            if (!zones_config_reinstate_tuning_quality_no_save(zc->index)) {
                snprintf(err_msg, err_cap,
                        "zone tuning entry %u (channel %u) rejected at commit reinstating tuning quality",
                        (unsigned)i, zc->index);
                zones_config_restore_snapshot_no_save(&zones_snapshot);
                return false;
            }
        } else if (zc->has_tuning_quality && !zones_config_set_tuning_quality_no_save(zc->index, &zc->tuning_quality)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting tuning quality",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
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
            zones_config_restore_snapshot_no_save(&zones_snapshot);
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
        if (zc->has_failsafe_state && !zones_config_set_failsafe_state_no_save(zc->index, zc->failsafe_state)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting failsafe_state",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_hyst_c && !zones_config_set_hyst_c_no_save(zc->index, zc->hyst_c)) {
            snprintf(err_msg, err_cap, "zone tuning entry %u (channel %u) rejected at commit setting hyst_c",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_min_on_s && !zones_config_set_min_on_s_no_save(zc->index, zc->min_on_s)) {
            snprintf(err_msg, err_cap, "zone tuning entry %u (channel %u) rejected at commit setting min_on_s",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_min_off_s && !zones_config_set_min_off_s_no_save(zc->index, zc->min_off_s)) {
            snprintf(err_msg, err_cap, "zone tuning entry %u (channel %u) rejected at commit setting min_off_s",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            return false;
        }
        if (zc->has_timing_profile && timing_profile_candidate_count > 0 &&
            !zones_config_set_timing_profile_index_no_save(zc->index, zc->timing_profile)) {
            snprintf(err_msg, err_cap,
                    "zone tuning entry %u (channel %u) rejected at commit setting timing_profile",
                    (unsigned)i, zc->index);
            zones_config_restore_snapshot_no_save(&zones_snapshot);
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
         * alone. Now folded into the single zones_config_save_now() call
         * after the whole batch (timing profiles + every zone's fields,
         * settings_source included) finishes, persisting the lot in one
         * write. */
        for (uint8_t group = 0; group < SRC_GROUP_COUNT; group++) {
            if (!zones_config_set_settings_source_unchecked_no_save(zc->index, group, zc->settings_source[group])) {
                /* Whole-batch restore (see zones_snapshot above) now covers
                 * this failure too -- superseding the narrower per-field
                 * settings_source_before[][] restore this block used before
                 * the whole-struct snapshot/restore pair existed; every
                 * other field this loop already committed this pass (name,
                 * PID, masks, cal, ramp, limits, coupling cells, ...) needed
                 * the same rollback and previously did not get it. */
                zones_config_restore_snapshot_no_save(&zones_snapshot);
                snprintf(err_msg, err_cap,
                        "zone tuning entry %u (channel %u) rejected at commit setting settings_source",
                        (unsigned)i, zc->index);
                return false;
            }
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
    /* Spare-relay aux conflict (docs/SPARE_RELAY_ONOFF_PLAN.md WP-2): the _no_save()
     * setters above never run zones_config_json_validate(), so its aux hook did not
     * see the relay_masks this batch just wrote. Check the post-loop live config
     * explicitly and roll the whole batch back (RAM only so far -- nothing has been
     * persisted) rather than save a config where a zone and an enabled aux output
     * own the same relay. Reads per-zone masks through the getter (no second
     * zones_cfg_t on this stack; zones_snapshot above is already one). */
    if (zone_candidate_count > 0) {
        uint8_t zones_union = 0;
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            uint8_t zmask = 0;
            if (zones_config_get_relay_mask(zi, &zmask)) {
                zones_union |= zmask;
            }
        }
        uint8_t aux_conflict = (uint8_t)(zones_union & zones_config_json_aux_enabled_mask());
        if (aux_conflict != 0) {
            zones_config_restore_snapshot_no_save(&zones_snapshot);
            snprintf(err_msg, err_cap,
                     "zone relay_mask claims relay mask 0x%02X that an aux (spare-relay) output already owns -- "
                     "disable that aux output first",
                     (unsigned)aux_conflict);
            return false;
        }
    }
    long long setter_loop_elapsed_ms = (long long)((hal_time_now_us() - setter_loop_start_us) / 1000u);
    ESP_LOGI(BACKUP_TAG,
            "backup import: setter loop (%u timing profiles, %u zones) took %lld ms",
            (unsigned)timing_profile_candidate_count, (unsigned)zone_candidate_count,
            (long long)setter_loop_elapsed_ms);

    /* Single batched save covering timing_profiles[] + the whole per-zone
     * loop above (settings_source included) -- the point of this whole
     * change. Only actually saves when this batch touched anything: a
     * no-op/empty import (candidate_count == 0 for both arrays) must not
     * pay for a flash write it has no reason to make, same as the old
     * settings_source_dirty gate this replaces. */
    if (timing_profile_candidate_count > 0 || zone_candidate_count > 0) {
        uint64_t save_start_us = hal_time_now_us();
        bool save_ok = zones_config_save_now();
        long long save_elapsed_ms = (long long)((hal_time_now_us() - save_start_us) / 1000u);
        ESP_LOGI(BACKUP_TAG, "backup import: batched zones_config_save_now() took %lld ms (ok=%d)",
                (long long)save_elapsed_ms, (int)save_ok);
        if (!save_ok) {
            /* The batch is fully committed in RAM at this point (every
             * setter above already returned true) but failed to reach
             * flash -- restoring RAM here would silently discard a config
             * the caller was just told succeeded up to this point and that
             * matches nothing on flash either way, so this reports the
             * failure loudly (partial_write, per this function's caller)
             * rather than rolling back a save that already ran; the
             * live config and flash are left exactly as nvs_save() itself
             * left them (same behavior as every other setter's own inline
             * nvs_save() before this change). */
            snprintf(err_msg, err_cap, "batch commit succeeded live but failed to persist to flash");
            return false;
        }
        /* Push relay_type changes to hardware only now that the save
         * backing them is confirmed -- see the _no_save() comment above. */
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if (relay_type_changed[zi]) {
                zones_config_push_relay_type(zi);
            }
        }
    }
    /* From here on the zones are persisted: a failure below is a partial write and must NOT put the
     * phase-1 aux disables back (the caller keys that off zones_landed_out). */
    *zones_landed_out = true;

    /* ---- Commit order: kiln_configs, aux phase 1 (disables), zones, update_repo, aux phase 2 (enables),
     * profiles LAST. profiles_http_save() re-runs validate_io_segment()/validate_on_off_rules() against
     * the LIVE config, and a profile in this backup can depend on that config as the same backup
     * leaves it: an on/off rule aimed at an aux relay this backup enables (target 8..11), a rule aimed at
     * a zone this backup types ON_OFF, a RELAY_IO segment on a relay this backup frees from a zone's
     * relay_mask. Committing profiles before those landed made the save refuse mid-loop and left a
     * half-restored board. Safety of the order: every dependency a rule has (zone typed ON_OFF, aux
     * enabled) is persisted before a rule that relies on it, so a rule can never be stored aimed at
     * a HEATER zone or an unconfigured aux output; if any earlier step fails no profile has been written
     * at all. backup_import_profiles_precheck() already proved the post-import state accepts every
     * profile, so the live re-validation below passing is expected -- it stays as the last guard. ---- */
    if (!backup_import_update_repo(body, true, NULL, err_msg, err_cap)) {
        return false; // zones already landed: reported as a partial write by the caller
    }
    if (!backup_import_aux_outputs_commit(body, true, aux_wrote_out, err_msg, err_cap)) {
        return false;
    }
    if (!backup_import_relay_cycles(body, true, NULL, err_msg, err_cap)) {
        return false; // zones already landed: reported as a partial write by the caller
    }
    if (!backup_import_prefs(body, true, NULL, err_msg, err_cap)) {
        return false; // zones already landed: reported as a partial write by the caller
    }
    /* ---- Pass 2: everything validated -- commit profiles ---- */
    for (size_t i = 0; i < candidate_count; i++) {
        profile_candidate_t *c = &candidates[i];
        uint8_t out_id = 0;
        char save_err[96];
        if (!profiles_http_save(c->has_id ? c->id : PROFILES_MAX_COUNT, &c->p, &out_id, NULL, save_err,
                                sizeof(save_err))) {
            /* Should not happen -- pass 1 (including backup_import_profiles_precheck()) already checked
             * everything profiles_http_save() itself checks -- but if it does (a race with a concurrent
             * change to zone config, say), report exactly which entry and why. Candidates before this one
             * are already committed. */
            snprintf(err_msg, err_cap, "profile entry %u rejected at commit: %s", (unsigned)i, save_err);
            return false;
        }
    }
    return true;
}

/* See the call site in backup_import_apply() below for why. Mirrors
 * zones_http_post.c's post-commit apply_lower block. NOINLINE: inlined, its
 * locals (the reason buffer and the per-zone array) land in
 * backup_import_apply()'s own frame, which sits under the much deeper
 * backup_import_apply_two_pass() chain, and push http_async_job's measured
 * depth over its check_all_task_stack_budgets.py ceiling (4784 B vs 4528 B,
 * measured). Kept out of line its frame is a sibling of that chain, not
 * stacked under it. */
static BACKUP_IMPORT_NOINLINE void backup_import_track_ceiling_lower(void)
{
    float new_max_temp_c[MAX31856_CHANNEL_COUNT];
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        float cur_max = 0.0f, cur_min = 0.0f;
        if (!zones_config_get_temp_limits(zi, &cur_max, &cur_min)) {
            cur_max = 0.0f;
        }
        new_max_temp_c[zi] = cur_max;
    }
    safety_ceiling_sync_result_t ceiling_result = SAFETY_CEILING_SYNC_NONE;
    char ceiling_reason[192];
    ceiling_reason[0] = '\0';
    safety_ceiling_sync_apply_lower(s_hw_safety, new_max_temp_c, MAX31856_CHANNEL_COUNT, &ceiling_result,
                                     ceiling_reason, sizeof(ceiling_reason));
    if (ceiling_result == SAFETY_CEILING_SYNC_LOWER_FAILED) {
        ESP_LOGW(BACKUP_TAG,
                 "backup import: safety processor ceiling not lowered to track the live zone max -- %s -- "
                 "Pico ceiling stays wider than the ESP max until the next zones save",
                 ceiling_reason);
    } else if (ceiling_result == SAFETY_CEILING_SYNC_LOWERED) {
        ESP_LOGI(BACKUP_TAG, "backup import: safety processor ceiling lowered to track the live zone max");
    }
}

/* docs/GITHUB_RELEASE_UPDATE_PLAN.md WP9: the optional top-level "update_repo"
 * string. Absent = no-op (every older export). Present: must be a string that
 * is "" (reset to the default) or a valid "owner/name"; an over-long value is
 * refused, never truncated (the buffer is one byte over the longest valid
 * repo, and a truncated copy would be longer than the maximum, so it fails
 * the validator). A backslash anywhere in the raw JSON string is refused:
 * backup_json_field_str() unescapes "\\x" to "x", so an escaped spelling could
 * otherwise turn a malformed value into a valid-looking one. A value equal to
 * the compiled-in default is treated as "" (the export spelling). commit=false
 * only validates and, when `plan` is non-NULL, records "update_repo <old> ->
 * <new>" for the dry-run response (pass 1, before the dry_run return);
 * commit=true persists (update_settings_set() itself writes nothing when the
 * value is unchanged). NOINLINE for the same stack-budget reason as
 * backup_import_track_ceiling_lower(). */
static BACKUP_IMPORT_NOINLINE bool backup_import_update_repo(const char *body, bool commit, kiln_cfg_plan_t *plan,
                                                             char *err_msg, size_t err_cap)
{
    const char *raw = backup_json_obj_find(body, "update_repo");
    if (raw == NULL) {
        return true;
    }
    if (*raw == '"') {
        for (const char *p = raw + 1; *p != '\0' && *p != '"'; p++) {
            if (*p == '\\') {
                snprintf(err_msg, err_cap, "update_repo must not contain a backslash");
                return false;
            }
        }
    }
    char repo[UPDATE_SETTINGS_REPO_MAX_LEN + 2];
    if (!backup_json_field_str(body, "update_repo", repo, sizeof(repo))) {
        snprintf(err_msg, err_cap, "update_repo must be a string");
        return false;
    }
    if (repo[0] != '\0' && !update_settings_repo_is_valid(repo)) {
        snprintf(err_msg, err_cap, "update_repo is not a valid owner/name");
        return false;
    }
    if (strcmp(repo, UPDATE_SETTINGS_DEFAULT_REPO) == 0) {
        repo[0] = '\0'; // the default spelled out is the same as unset
    }
    if (!commit && plan != NULL) {
        char old_repo[UPDATE_SETTINGS_REPO_MAX_LEN + 1];
        if (!update_settings_repo_copy(old_repo, sizeof(old_repo))) {
            old_repo[0] = '\0';
        }
        const char *new_repo = repo[0] != '\0' ? repo : UPDATE_SETTINGS_DEFAULT_REPO;
        if (strcmp(old_repo, new_repo) == 0) {
            kiln_cfg_plan_add(plan, "update_repo unchanged: %s", old_repo);
        } else {
            kiln_cfg_plan_add(plan, "update_repo %s -> %s", old_repo, new_repo);
        }
    }
    if (commit && update_settings_set(repo) != ESP_OK) {
        snprintf(err_msg, err_cap, "update_repo could not be persisted -- the rest of the restore already landed");
        return false;
    }
    return true;
}

/* Optional top-level "relay_cycles" {"hw_relays":N,"c0".."c4"} (backup_export_relay_cycles()). Absent =
 * no-op. Present: hw_relays must equal this board's KILN_IO_RELAY_COUNT (counts from a different relay
 * layout are not this board's wear history) and every c<i> must be an integer in
 * [0, RELAY_CYCLES_RESTORE_MAX_COUNT]; anything else refuses the WHOLE restore in pass 1. Commit goes
 * through relay_cycles_restore_all() with allow_lower_mask 0: a stale backup can never LOWER a live
 * wear count (it is clamped up and reported), which on a freshly factory-reset board is the same as an
 * exact restore. Types/rated overrides are not touched (relay type travels with the zones). */
/* Bit i set = relay i kept its (higher) live count on the last committed import. Reported in the commit response. */
static uint8_t s_rc_kept_mask;

static BACKUP_IMPORT_NOINLINE bool backup_import_relay_cycles(const char *body, bool commit, kiln_cfg_plan_t *plan,
                                                              char *err_msg, size_t err_cap)
{
    if (commit) {
        s_rc_kept_mask = 0;
    }
    const char *obj = backup_json_obj_find(body, "relay_cycles");
    if (obj == NULL) {
        return true;
    }
    if (*backup_json_skip_ws(obj) != '{') {
        snprintf(err_msg, err_cap, "relay_cycles must be an object");
        return false;
    }
    double hw = 0.0;
    if (!backup_json_field_num(obj, "hw_relays", &hw) || hw != (double)KILN_IO_RELAY_COUNT) {
        snprintf(err_msg, err_cap, "relay_cycles: hw_relays missing or differs from this board's %u heater relays",
                 (unsigned)KILN_IO_RELAY_COUNT);
        return false;
    }
    uint32_t counts[RELAY_CYCLES_COUNT];
    for (unsigned i = 0; i < RELAY_CYCLES_COUNT; i++) {
        char key[8];
        snprintf(key, sizeof(key), "c%u", i);
        double d = 0.0;
        if (!backup_json_field_num(obj, key, &d) || d < 0 || d > (double)RELAY_CYCLES_RESTORE_MAX_COUNT ||
            (double)(long long)d != d) {
            snprintf(err_msg, err_cap, "relay_cycles: %s missing or not an integer in range", key);
            return false;
        }
        counts[i] = (uint32_t)d;
    }
    if (!commit) {
        if (plan != NULL) {
            uint32_t live[RELAY_CYCLES_COUNT];
            relay_cycles_get_all(live);
            for (unsigned i = 0; i < RELAY_CYCLES_COUNT; i++) {
                if (counts[i] < live[i]) {
                    kiln_cfg_plan_add(plan, "relay_cycles c%u: backup %lu is below live %lu, live count is kept", i,
                                      (unsigned long)counts[i], (unsigned long)live[i]);
                }
            }
        }
        return true;
    }
    uint32_t live_now[RELAY_CYCLES_COUNT];
    relay_cycles_get_all(live_now);
    s_rc_kept_mask = 0;
    if (!relay_cycles_restore_all(counts, 0, NULL)) {
        snprintf(err_msg, err_cap,
                 "relay_cycles could not be persisted -- kiln_configs, zones, update_repo and aux_outputs already "
                 "landed; preferences and profiles were NOT written");
        return false;
    }
    for (unsigned i = 0; i < RELAY_CYCLES_COUNT; i++) {
        if (counts[i] < live_now[i]) {
            s_rc_kept_mask |= (uint8_t)(1u << i); /* live > backup: live count kept */
        }
    }
    return true;
}

/* Operator preferences (docs/audits/BACKUP_CFGFS_COVERAGE_AUDIT_2026-10-09.md gaps 1+2): the
 * optional top-level keys backup_export_prefs() writes -- unit, ramp_assist, display_power,
 * hidden_builtin_profiles, tz, relay_names. Each absent key preserves the live value. Present
 * keys are held to the validation of their live path (unit_pref_set / ramp_assist / display_power
 * range + timeout enum / time_sync_tz_is_valid / relay<N>_name <= RELAY_NAME_MAX_LEN and
 * relay<N>_type < RELAY_DEVICE_TYPE_COUNT, as POST /api/zones) and ANY invalid value refuses the
 * WHOLE restore in pass 1 (commit=false). With commit=true the same parse is applied through the
 * validated setters. Parsed state is a few hundred bytes; nothing large lives here. */
/* Width of profiles_builtin.c's uint32_t hidden mask (one bit per catalogue entry, 28 today). */
#define BACKUP_HIDDEN_MASK_BITS 32u
_Static_assert(PROFILE_BUILTIN_ID_BASE + BACKUP_HIDDEN_MASK_BITS - 1u <= 255u, "hidden ids must fit uint8_t");
typedef struct {
    bool has_unit, has_ramp, has_display, has_hidden, has_tz, has_names;
    unit_pref_t unit;
    bool ramp;
    uint8_t brightness;
    display_timeout_setting_t timeout;
    bool keep_on, on_error;
    bool hidden[BACKUP_HIDDEN_MASK_BITS]; /* indexed by builtin id - PROFILE_BUILTIN_ID_BASE */
    char tz[TIME_SYNC_TZ_MAX_LEN + 2];
    bool name_set[KILN_IO_RELAY_COUNT];
    char name[KILN_IO_RELAY_COUNT][RELAY_NAME_MAX_LEN + 2];
    bool type_set[KILN_IO_RELAY_COUNT];
    relay_device_type_t type[KILN_IO_RELAY_COUNT];
} backup_prefs_t;

static bool backup_prefs_int(const char *v, double lo, double hi, double *out)
{
    char *end = NULL;
    if (v == NULL || *v == '"' || *v == '\0') {
        return false;
    }
    double d = strtod(v, &end);
    if (end == v || d < lo || d > hi || d != (double)(long long)d) {
        return false;
    }
    *out = d;
    return true;
}

/* v points into raw JSON, so the literal is followed by a delimiter, never more
 * identifier characters ("trueXYZ" is not true). */
static bool backup_prefs_literal(const char *v, const char *lit)
{
    size_t n = strlen(lit);
    return strncmp(v, lit, n) == 0 && !isalnum((unsigned char)v[n]) && v[n] != '_';
}

static bool backup_prefs_bool(const char *v, bool *out)
{
    if (v != NULL && backup_prefs_literal(v, "true")) {
        *out = true;
        return true;
    }
    if (v != NULL && backup_prefs_literal(v, "false")) {
        *out = false;
        return true;
    }
    return false;
}

static BACKUP_IMPORT_NOINLINE bool backup_prefs_parse(const char *body, backup_prefs_t *p, char *err_msg,
                                                      size_t err_cap)
{
    memset(p, 0, sizeof(*p));
    double d;
    const char *v = backup_json_obj_find(body, "unit");
    if (v != NULL) {
        if (!backup_prefs_int(v, 0, 1, &d)) {
            snprintf(err_msg, err_cap, "unit must be 0 (C) or 1 (F)");
            return false;
        }
        p->has_unit = true;
        p->unit = (unit_pref_t)(int)d;
    }
    v = backup_json_obj_find(body, "ramp_assist");
    if (v != NULL) {
        if (!backup_prefs_bool(v, &p->ramp)) {
            snprintf(err_msg, err_cap, "ramp_assist must be true or false");
            return false;
        }
        p->has_ramp = true;
    }
    v = backup_json_obj_find(body, "display_power");
    if (v != NULL) {
        const char *b = backup_json_obj_find(v, "brightness_percent");
        const char *t = backup_json_obj_find(v, "timeout_setting");
        double bd = 0, td = 0;
        if (*backup_json_skip_ws(v) != '{' || !backup_prefs_int(b, 0, 100, &bd) || !backup_prefs_int(t, 0, 255, &td) ||
            !display_power_timeout_setting_is_valid((display_timeout_setting_t)(int)td) ||
            !backup_prefs_bool(backup_json_obj_find(v, "keep_on_while_firing"), &p->keep_on) ||
            !backup_prefs_bool(backup_json_obj_find(v, "display_on_error"), &p->on_error)) {
            snprintf(err_msg, err_cap,
                     "display_power is not a valid {brightness_percent 0-100, timeout_setting, "
                     "keep_on_while_firing, display_on_error}");
            return false;
        }
        p->has_display = true;
        p->brightness = (uint8_t)bd;
        p->timeout = (display_timeout_setting_t)(int)td;
    }
    v = backup_json_obj_find(body, "hidden_builtin_profiles");
    if (v != NULL) {
        if (*backup_json_skip_ws(v) != '[') {
            snprintf(err_msg, err_cap, "hidden_builtin_profiles must be an array");
            return false;
        }
        for (const char *e = backup_json_arr_first(v); e != NULL; e = backup_json_arr_next(e)) {
            if (!backup_prefs_int(e, PROFILE_BUILTIN_ID_BASE, 255, &d) || !profiles_builtin_id_valid((uint8_t)d) ||
                (size_t)((uint8_t)d - PROFILE_BUILTIN_ID_BASE) >= sizeof(p->hidden)) {
                snprintf(err_msg, err_cap, "hidden_builtin_profiles holds an id that is not a builtin profile");
                return false;
            }
            p->hidden[(uint8_t)d - PROFILE_BUILTIN_ID_BASE] = true;
        }
        p->has_hidden = true;
    }
    v = backup_json_obj_find(body, "tz");
    if (v != NULL) {
        if (!backup_json_field_str(body, "tz", p->tz, sizeof(p->tz)) || strlen(p->tz) > TIME_SYNC_TZ_MAX_LEN ||
            !time_sync_tz_is_valid(p->tz)) {
            snprintf(err_msg, err_cap, "tz is not a valid POSIX TZ string");
            return false;
        }
        p->has_tz = true;
    }
    v = backup_json_obj_find(body, "relay_names");
    if (v != NULL) {
        if (*backup_json_skip_ws(v) != '[') {
            snprintf(err_msg, err_cap, "relay_names must be an array");
            return false;
        }
        unsigned n = 0;
        for (const char *e = backup_json_arr_first(v); e != NULL; e = backup_json_arr_next(e), n++) {
            double rd = 0;
            if (*backup_json_skip_ws(e) != '{' ||
                !backup_prefs_int(backup_json_obj_find(e, "relay"), 1, KILN_IO_RELAY_COUNT, &rd)) {
                snprintf(err_msg, err_cap, "relay_names[%u]: relay missing or not 1-%u", n,
                         (unsigned)KILN_IO_RELAY_COUNT);
                return false;
            }
            unsigned ri = (unsigned)rd - 1u;
            if (p->name_set[ri] || p->type_set[ri]) {
                snprintf(err_msg, err_cap, "relay_names[%u]: relay %u listed twice", n, ri + 1u);
                return false;
            }
            if (backup_json_obj_find(e, "name") != NULL) {
                if (!backup_json_field_str(e, "name", p->name[ri], sizeof(p->name[ri])) ||
                    strlen(p->name[ri]) > RELAY_NAME_MAX_LEN) {
                    snprintf(err_msg, err_cap, "relay_names[%u]: name must be a string of at most %u characters", n,
                             (unsigned)RELAY_NAME_MAX_LEN);
                    return false;
                }
                p->name_set[ri] = true;
            }
            const char *tv = backup_json_obj_find(e, "type");
            if (tv != NULL) {
                double td = 0;
                if (!backup_prefs_int(tv, 0, RELAY_DEVICE_TYPE_COUNT - 1, &td)) {
                    snprintf(err_msg, err_cap, "relay_names[%u]: type out of range", n);
                    return false;
                }
                p->type[ri] = (relay_device_type_t)(int)td;
                p->type_set[ri] = true;
            }
            if (!p->name_set[ri] && !p->type_set[ri]) {
                snprintf(err_msg, err_cap, "relay_names[%u]: neither name nor type given", n);
                return false;
            }
        }
        p->has_names = true;
    }
    return true;
}

static BACKUP_IMPORT_NOINLINE bool backup_import_prefs(const char *body, bool commit, kiln_cfg_plan_t *plan,
                                                       char *err_msg, size_t err_cap)
{
    backup_prefs_t p;
    if (!backup_prefs_parse(body, &p, err_msg, err_cap)) {
        return false;
    }
    if (p.has_hidden && g_builtin_profile_count > BACKUP_HIDDEN_MASK_BITS) {
        /* Catalogue outgrew the 32-bit mask: refuse in pass 1 (before any write), never skip entries. */
        snprintf(err_msg, err_cap, "hidden_builtin_profiles: built-in catalogue is wider than the %u-bit mask",
                 (unsigned)BACKUP_HIDDEN_MASK_BITS);
        return false;
    }
    if (!commit) {
        if (plan != NULL && (p.has_unit || p.has_ramp || p.has_display || p.has_hidden || p.has_tz || p.has_names)) {
            kiln_cfg_plan_add(plan, "preferences in this backup (unit/ramp_assist/display_power/hidden profiles/tz/"
                                    "relay names) are restored; keys absent from it are kept");
        }
        return true;
    }
    bool ok = true;
    if (p.has_unit && unit_pref_set(p.unit) != ESP_OK) {
        ok = false;
    }
    if (p.has_ramp && ramp_assist_cfg_set_enabled(p.ramp) != ESP_OK) {
        ok = false;
    }
    if (p.has_display && display_power_cfg_set(p.brightness, p.timeout, p.keep_on, p.on_error) != ESP_OK) {
        ok = false;
    }
    if (p.has_hidden) {
        for (size_t i = 0; i < g_builtin_profile_count && i < BACKUP_HIDDEN_MASK_BITS; i++) {
            uint8_t id = (uint8_t)(PROFILE_BUILTIN_ID_BASE + i);
            if (profiles_builtin_is_hidden(id) != p.hidden[i] &&
                profiles_builtin_set_hidden(id, p.hidden[i]) != ESP_OK) {
                ok = false;
            }
        }
    }
    if (p.has_tz && time_sync_set_tz(p.tz) != ESP_OK) {
        ok = false;
    }
    if (p.has_names) {
        for (uint8_t r = 0; r < KILN_IO_RELAY_COUNT; r++) {
            if (p.name_set[r] && !zones_config_set_relay_name((uint8_t)(r + 1u), p.name[r])) {
                ok = false;
            }
            if (p.type_set[r] && !zones_config_set_relay_device_type((uint8_t)(r + 1u), p.type[r])) {
                ok = false;
            }
        }
    }
    if (!ok) {
        snprintf(err_msg, err_cap, "a preference could not be persisted -- the rest of the restore already landed");
    }
    return ok;
}

/* Top-level "aux_outputs": the spare-relay on/off outputs (docs/SPARE_RELAY_ONOFF_PLAN.md), the
 * array backup_export_aux_outputs() writes. Absent key = no-op (every older export, and an export
 * from a quarantined store). Each entry is held to the same rules POST /api/aux_outputs applies:
 * relay 1..AUX_OUTPUTS_COUNT and enabled (0/1 or true/false) are required, tc_zone -1..zones-1,
 * hyst_c, min_on_s and min_off_s are optional and keep the relay's current value when omitted, all
 * within the AUX_* bounds; a relay listed twice, a non-object element or a non-array value is
 * refused. The finished entry also goes through aux_outputs_cfg_entry_valid(), the store's own
 * check. Unknown keys (the live route's "conflicted") are ignored.
 *
 * Pass 1 (backup_import_aux_outputs_validate) refuses the WHOLE restore before anything is
 * written: a malformed entry, a quarantined aux store, or an enabled aux relay that a zone
 * relay_mask of the restored configuration would also claim (one relay, one owner -- the
 * projected union uses the file's zone masks where it has them and the live ones elsewhere).
 * Commit is two phases around the zones commit so the conflict invariant holds at every step:
 * phase 1 (enable_phase=false, before zones) writes the entries that DISABLE a relay -- which can
 * never conflict, and frees a relay a restored zone is about to claim; phase 2 (enable_phase=true,
 * after zones) writes the entries that ENABLE one against the zones union as committed. An entry
 * identical to the live effective value is skipped (no flash write for an identity restore).
 * NOINLINE for the same stack-budget reason as backup_import_track_ceiling_lower(). */
typedef struct {
    bool present;
    bool has[AUX_OUTPUTS_COUNT];
    aux_output_entry_t entry[AUX_OUTPUTS_COUNT];
} backup_aux_import_t;

static BACKUP_IMPORT_NOINLINE bool backup_import_aux_num(const char *obj, const char *key, double min, double max,
                                                         bool integer, bool *has, double *out, unsigned idx,
                                                         char *err_msg, size_t err_cap)
{
    *has = false;
    if (!backup_json_obj_find(obj, key)) {
        return true;
    }
    double d = 0.0;
    if (!backup_json_field_num(obj, key, &d) || d < min || d > max || (integer && (double)(long long)d != d)) {
        snprintf(err_msg, err_cap, "aux_outputs[%u]: %s is not a valid number in range", idx, key);
        return false;
    }
    *out = d;
    *has = true;
    return true;
}

static BACKUP_IMPORT_NOINLINE bool backup_import_aux_parse(const char *body, backup_aux_import_t *out,
                                                           char *err_msg, size_t err_cap)
{
    memset(out, 0, sizeof(*out));
    const char *arr = backup_json_obj_find(body, "aux_outputs");
    if (arr == NULL) {
        return true;
    }
    if (*backup_json_skip_ws(arr) != '[') {
        snprintf(err_msg, err_cap, "aux_outputs must be an array");
        return false;
    }
    out->present = true;
    unsigned n = 0;
    for (const char *e = backup_json_arr_first(arr); e; e = backup_json_arr_next(e), n++) {
        if (*backup_json_skip_ws(e) != '{') {
            snprintf(err_msg, err_cap, "aux_outputs[%u] must be an object", n);
            return false;
        }
        double drelay = 0.0;
        if (!backup_json_field_num(e, "relay", &drelay) || drelay < 1 || drelay > AUX_OUTPUTS_COUNT ||
            (double)(long long)drelay != drelay) {
            snprintf(err_msg, err_cap, "aux_outputs[%u]: relay missing or not 1-%u", n, (unsigned)AUX_OUTPUTS_COUNT);
            return false;
        }
        uint8_t relay = (uint8_t)drelay;
        if (out->has[relay - 1]) {
            snprintf(err_msg, err_cap, "aux_outputs[%u]: relay %u listed twice", n, (unsigned)relay);
            return false;
        }
        bool enabled = false;
        double den = 0.0;
        if (!kiln_cfg_json_field_bool(e, "enabled", &enabled)) {
            if (!backup_json_field_num(e, "enabled", &den) || (den != 0.0 && den != 1.0)) {
                snprintf(err_msg, err_cap, "aux_outputs[%u]: enabled missing or not true/false", n);
                return false;
            }
            enabled = den == 1.0;
        }
        // Omitted optional fields keep the relay's current effective values, as the live route does.
        aux_output_t cur;
        memset(&cur, 0, sizeof(cur));
        (void)aux_outputs_cfg_get(relay, &cur);
        aux_output_entry_t ent;
        memset(&ent, 0, sizeof(ent));
        ent.enabled = enabled ? 1u : 0u;
        ent.tc_zone_plus1 = cur.tc_zone == AUX_TC_ZONE_NONE ? 0u : (uint8_t)(cur.tc_zone + 1u);
        ent.hyst_c = cur.hyst_c;
        ent.min_on_s = cur.min_on_s;
        ent.min_off_s = cur.min_off_s;
        bool has = false;
        double d = 0.0;
        if (!backup_import_aux_num(e, "tc_zone", -1, MAX31856_CHANNEL_COUNT - 1, true, &has, &d, n, err_msg, err_cap)) {
            return false;
        }
        if (has) {
            ent.tc_zone_plus1 = (uint8_t)(d + 1.0);
        }
        if (!backup_import_aux_num(e, "hyst_c", AUX_HYST_C_MIN, AUX_HYST_C_MAX, false, &has, &d, n, err_msg, err_cap)) {
            return false;
        }
        if (has) {
            ent.hyst_c = (float)d;
        }
        if (!backup_import_aux_num(e, "min_on_s", AUX_MIN_ON_OFF_S_MIN, AUX_MIN_ON_OFF_S_MAX, true, &has, &d, n,
                                   err_msg, err_cap)) {
            return false;
        }
        if (has) {
            ent.min_on_s = (uint16_t)d;
        }
        if (!backup_import_aux_num(e, "min_off_s", AUX_MIN_ON_OFF_S_MIN, AUX_MIN_ON_OFF_S_MAX, true, &has, &d, n,
                                   err_msg, err_cap)) {
            return false;
        }
        if (has) {
            ent.min_off_s = (uint16_t)d;
        }
        if (!aux_outputs_cfg_entry_valid(&ent)) {
            snprintf(err_msg, err_cap, "aux_outputs[%u]: entry rejected by the aux store's range check", n);
            return false;
        }
        out->has[relay - 1] = true;
        out->entry[relay - 1] = ent;
    }
    return true;
}

static bool backup_import_aux_entry_matches_live(uint8_t relay, const aux_output_entry_t *ent)
{
    aux_output_t cur;
    memset(&cur, 0, sizeof(cur));
    if (!aux_outputs_cfg_get(relay, &cur) || cur.conflicted) {
        return false; // a conflicted relay is persisted enabled but forced off live: always rewrite it
    }
    uint8_t want_tc = ent->tc_zone_plus1 == 0 ? (uint8_t)AUX_TC_ZONE_NONE : (uint8_t)(ent->tc_zone_plus1 - 1u);
    return cur.enabled == (ent->enabled != 0) && cur.tc_zone == want_tc && cur.hyst_c == ent->hyst_c &&
           cur.min_on_s == ent->min_on_s && cur.min_off_s == ent->min_off_s;
}

static BACKUP_IMPORT_NOINLINE bool backup_import_aux_outputs_validate(const char *body, kiln_cfg_plan_t *plan,
                                                                      char *err_msg, size_t err_cap)
{
    backup_aux_import_t a;
    if (!backup_import_aux_parse(body, &a, err_msg, err_cap)) {
        return false;
    }
    if (!a.present) {
        uint8_t live = aux_outputs_cfg_enabled_mask();
        if (plan != NULL && live != 0) {
            kiln_cfg_plan_add(plan, "aux_outputs not in this backup: enabled aux relay mask 0x%02X is kept",
                              (unsigned)live);
        }
        return true;
    }
    if (aux_outputs_cfg_quarantined()) {
        snprintf(err_msg, err_cap,
                 "aux_outputs cannot be restored: the aux store holds newer-firmware data and is quarantined");
        return false;
    }
    uint8_t zmask[MAX31856_CHANNEL_COUNT];
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        zmask[zi] = 0;
        (void)zones_config_get_relay_mask(zi, &zmask[zi]);
    }
    for (const char *ze = backup_json_arr_first(backup_json_obj_find(body, "zones")); ze;
         ze = backup_json_arr_next(ze)) {
        double di = 0.0, dm = 0.0;
        if (backup_json_field_num(ze, "index", &di) && di >= 0 && di < MAX31856_CHANNEL_COUNT &&
            backup_json_field_num(ze, "relay_mask", &dm) && dm >= 0 && dm <= 255) {
            zmask[(unsigned)di] = (uint8_t)dm;
        }
    }
    uint8_t zones_union = 0;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        zones_union |= zmask[zi];
    }
    uint8_t result_enabled = aux_outputs_cfg_enabled_mask();
    unsigned changed = 0, same = 0;
    for (uint8_t i = 0; i < AUX_OUTPUTS_COUNT; i++) {
        if (!a.has[i]) {
            continue;
        }
        uint8_t bit = (uint8_t)(1u << i);
        result_enabled = a.entry[i].enabled ? (uint8_t)(result_enabled | bit) : (uint8_t)(result_enabled & ~bit);
        if (backup_import_aux_entry_matches_live((uint8_t)(i + 1u), &a.entry[i])) {
            same++;
        } else {
            changed++;
        }
    }
    uint8_t conflict = (uint8_t)(result_enabled & zones_union);
    if (conflict != 0) {
        snprintf(err_msg, err_cap,
                 "aux_outputs: relay mask 0x%02X would be both an enabled aux output and claimed by a zone "
                 "relay_mask in the restored configuration",
                 (unsigned)conflict);
        return false;
    }
    if (plan != NULL) {
        kiln_cfg_plan_add(plan, "aux_outputs: %u relay(s) change, %u unchanged", changed, same);
    }
    return true;
}

/* Pass 1 for the zone tuning entries' board topology: every entry's "index" must be a configured zone on
 * THIS board. The backup carries no thermo_count (it is board topology, set on Thermocouples & Zones, and
 * has no import setter), so a board whose zones config is empty (thermo_count 0) cannot take any zone entry.
 * backup_import_apply_two_pass() rejects such an entry too, but only AFTER kiln_configs[] has been committed,
 * which made the refusal a 500 partial write (bench 2026-10-09). Refuse here, before anything is written. */
static BACKUP_IMPORT_NOINLINE bool backup_import_zone_topology_precheck(const char *body, char *err_msg,
                                                                         size_t err_cap)
{
    const char *zones_arr = backup_json_obj_find(body, "zones");
    backup_topology_t topo;
    if (!backup_import_resolve_topology(body, &topo, err_msg, err_cap)) {
        return false;
    }
    uint8_t thermo_count = topo.thermo;
    for (const char *ze = backup_json_arr_first(zones_arr); ze; ze = backup_json_arr_next(ze)) {
        double didx;
        if (!backup_json_field_num(ze, "index", &didx) || didx < 0) {
            continue; /* malformed entries get their specific message from the main parse */
        }
        if (didx >= thermo_count) {
            snprintf(err_msg, err_cap,
                     "zone %u in the backup is not a configured zone on this board (%u configured); set the "
                     "zone count under Thermocouples & Zones first. Nothing was written.",
                     (unsigned)didx, (unsigned)thermo_count);
            return false;
        }
    }
    return true;
}

/* Pass 1 for the profiles' RELAY_IO segments and on_off_rules: run the REAL save-time validators
 * (validate_io_segment_in_state()/validate_on_off_rules_in_state(), profiles_validate.c) against the
 * configuration this import WILL produce -- candidate zone_type and relay_mask per zone, and the aux
 * entries as the backup leaves them -- instead of the live one. profiles_http_save() runs the same
 * validators against the live config during the profile commit, which is too late to refuse cleanly;
 * with this check every refusal happens before anything is written (including in a dry run). Absent
 * keys keep the live value, exactly as the commit passes treat them. The scratch (state plus one
 * profile_t) lives on the heap: no new buffer on the async-job stack. */
typedef struct {
    profile_validate_state_t st;
    profile_t p;
} backup_import_precheck_scratch_t;

/* Prepend "profile entry N: " to the message already in err_msg, in place and truncating safely
 * (no bounce buffer, so no -Wformat-truncation and no stack cost). */
static void backup_import_prefix_entry(char *err_msg, size_t err_cap, unsigned entry)
{
    char prefix[32];
    int n = snprintf(prefix, sizeof(prefix), "profile entry %u: ", entry);
    if (err_cap == 0 || n < 0 || (size_t)n >= err_cap) {
        return;
    }
    size_t len = strlen(err_msg);
    size_t avail = err_cap - 1u - (size_t)n;
    if (len > avail) {
        len = avail;
    }
    memmove(err_msg + n, err_msg, len);
    memcpy(err_msg, prefix, (size_t)n);
    err_msg[(size_t)n + len] = '\0';
}

static BACKUP_IMPORT_NOINLINE bool backup_import_profiles_precheck(const char *body, char *err_msg, size_t err_cap)
{
    const char *profiles_arr = backup_json_obj_find(body, "profiles");
    if (profiles_arr == NULL || backup_json_arr_first(profiles_arr) == NULL) {
        return true;
    }
    backup_import_precheck_scratch_t *sc = persist_scratch_alloc(sizeof(*sc));
    if (sc == NULL) {
        snprintf(err_msg, err_cap, "out of memory (profile validation scratch)");
        return false;
    }
    memset(sc, 0, sizeof(*sc));
    bool ok = false;
    profile_validate_state_t *st = &sc->st;
    /* Effective topology (review L6, superseded 2026-10-09): the live count, or the backup's own
     * thermo_count when this restore is about to set it on an unconfigured board. */
    backup_topology_t topo;
    if (!backup_import_resolve_topology(body, &topo, err_msg, err_cap)) {
        free(sc);
        return false;
    }
    st->zone_count = topo.thermo;
    if (st->zone_count > MAX31856_CHANNEL_COUNT) {
        st->zone_count = MAX31856_CHANNEL_COUNT;
    }
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        zone_type_t zt = ZONE_TYPE_HEATER;
        if (zones_config_get_zone_type(zi, &zt)) {
            st->zone_type[zi] = (uint8_t)zt;
        } else {
            st->zone_type[zi] = (uint8_t)ZONE_TYPE_HEATER;
        }
        uint8_t m = 0;
        (void)zones_config_get_relay_mask(zi, &m);
        st->zone_relay_mask[zi] = m;
    }
    for (const char *ze = backup_json_arr_first(backup_json_obj_find(body, "zones")); ze;
         ze = backup_json_arr_next(ze)) {
        double di = 0.0, dv = 0.0;
        bool hv = false;
        if (!backup_json_field_num(ze, "index", &di) || di < 0 || di >= MAX31856_CHANNEL_COUNT) {
            continue; // pass 1b refuses a malformed zone entry; nothing to validate against here
        }
        unsigned zi = (unsigned)di;
        if (backup_json_field_opt_num(ze, "relay_mask", 0, 255, &dv, &hv, "relay_mask", NULL, 0, zi) && hv) {
            st->zone_relay_mask[zi] = (uint8_t)dv;
        }
        hv = false;
        if (backup_json_field_opt_num(ze, "zone_type", 0, (double)ZONE_TYPE_ON_OFF, &dv, &hv, "zone_type", NULL, 0,
                                      zi) &&
            hv) {
            st->zone_type[zi] = (uint8_t)dv;
        }
    }
    for (uint8_t relay = 1; relay <= AUX_OUTPUTS_COUNT; relay++) {
        aux_output_t cur;
        memset(&cur, 0, sizeof(cur));
        (void)aux_outputs_cfg_get(relay, &cur);
        st->aux[relay - 1u] = cur;
    }
    {
        backup_aux_import_t a;
        if (!backup_import_aux_parse(body, &a, err_msg, err_cap)) {
            goto done; // already refused by the aux pass; kept so this function is safe on its own
        }
        if (a.present) {
            for (uint8_t i = 0; i < AUX_OUTPUTS_COUNT; i++) {
                if (!a.has[i]) {
                    continue;
                }
                aux_output_t *o = &st->aux[i];
                o->enabled = a.entry[i].enabled != 0;
                o->conflicted = false;
                o->tc_zone = a.entry[i].tc_zone_plus1 == 0 ? (uint8_t)AUX_TC_ZONE_NONE
                                                          : (uint8_t)(a.entry[i].tc_zone_plus1 - 1u);
                o->hyst_c = a.entry[i].hyst_c;
                o->min_on_s = a.entry[i].min_on_s;
                o->min_off_s = a.entry[i].min_off_s;
            }
        }
    }
    size_t entry = 0;
    for (const char *pe = backup_json_arr_first(profiles_arr); pe; pe = backup_json_arr_next(pe), entry++) {
        profile_t *p = &sc->p;
        memset(p, 0, sizeof(*p));
        uint8_t seg_i = 0;
        for (const char *se = backup_json_arr_first(backup_json_obj_find(pe, "segments")); se;
             se = backup_json_arr_next(se)) {
            if (seg_i >= PROFILE_MAX_SEGMENTS) {
                snprintf(err_msg, err_cap, "profile entry %u: more than %u segments", (unsigned)entry,
                         (unsigned)PROFILE_MAX_SEGMENTS);
                goto done;
            }
            profile_segment_t *sg = &p->segments[seg_i];
            if (!backup_import_parse_seg_kind_io(se, entry, seg_i, sg, err_msg, err_cap)) {
                goto done;
            }
            if (sg->seg_kind == PROFILE_SEG_KIND_RELAY_IO &&
                !validate_io_segment_in_state(sg, (uint8_t)(seg_i + 1u), st, err_msg, err_cap)) {
                backup_import_prefix_entry(err_msg, err_cap, (unsigned)entry);
                goto done;
            }
            seg_i++;
        }
        p->segment_count = seg_i;
        uint8_t rule_i = 0;
        for (const char *re = backup_json_arr_first(backup_json_obj_find(pe, "on_off_rules")); re;
             re = backup_json_arr_next(re)) {
            if (rule_i >= PROFILE_MAX_ON_OFF_RULES) {
                snprintf(err_msg, err_cap, "profile entry %u: too many on_off_rules", (unsigned)entry);
                goto done;
            }
            if (!backup_import_parse_rule(re, entry, rule_i, &p->on_off_rules[rule_i], err_msg, err_cap)) {
                goto done;
            }
            rule_i++;
        }
        p->on_off_rule_count = rule_i;
        if (!validate_on_off_rules_in_state(p, st, err_msg, err_cap)) {
            backup_import_prefix_entry(err_msg, err_cap, (unsigned)entry);
            goto done;
        }
    }
    ok = true;
done:
    free(sc);
    return ok;
}

static uint8_t backup_import_aux_zones_union_live(void)
{
    uint8_t zones_union = 0;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        uint8_t zmask = 0;
        if (zones_config_get_relay_mask(zi, &zmask)) {
            zones_union |= zmask;
        }
    }
    return zones_union;
}

/* Undo record for phase 1 (ported from the competing WP-7 candidate, 3ba51c23): the pre-restore
 * entry of every relay phase 1 rewrote, so a restore whose profiles/zones step then fails can put
 * those relays back. static, not a local: one import runs at a time (http_async_job is
 * single-flight) and this keeps the record off http_async_job's budgeted stack. */
static struct {
    aux_output_entry_t entry[AUX_OUTPUTS_COUNT];
    uint8_t written; /* bit i = relay i+1 was rewritten by phase 1 */
} s_aux_undo;

static BACKUP_IMPORT_NOINLINE bool backup_import_aux_outputs_commit(const char *body, bool enable_phase, bool *wrote,
                                                                    char *err_msg, size_t err_cap)
{
    if (!enable_phase) {
        s_aux_undo.written = 0;
    }
    backup_aux_import_t a;
    if (!backup_import_aux_parse(body, &a, err_msg, err_cap)) {
        return false;
    }
    if (!a.present) {
        return true;
    }
    uint8_t zones_union = backup_import_aux_zones_union_live();
    for (uint8_t i = 0; i < AUX_OUTPUTS_COUNT; i++) {
        if (!a.has[i] || (a.entry[i].enabled != 0) != enable_phase ||
            backup_import_aux_entry_matches_live((uint8_t)(i + 1u), &a.entry[i])) {
            continue;
        }
        if (!enable_phase) {
            aux_output_t cur;
            memset(&cur, 0, sizeof(cur));
            (void)aux_outputs_cfg_get((uint8_t)(i + 1u), &cur);
            aux_output_entry_t *u = &s_aux_undo.entry[i];
            memset(u, 0, sizeof(*u));
            u->enabled = (cur.enabled || cur.conflicted) ? 1u : 0u; // conflicted = persisted enabled
            u->tc_zone_plus1 = cur.tc_zone == AUX_TC_ZONE_NONE ? 0u : (uint8_t)(cur.tc_zone + 1u);
            u->hyst_c = cur.hyst_c;
            u->min_on_s = cur.min_on_s;
            u->min_off_s = cur.min_off_s;
        }
        esp_err_t err = aux_outputs_cfg_set((uint8_t)(i + 1u), &a.entry[i], zones_union);
        if (err != ESP_ERR_INVALID_ARG && err != ESP_ERR_INVALID_STATE) {
            *wrote = true; // applied in RAM first even when the save then failed
            if (!enable_phase) {
                s_aux_undo.written = (uint8_t)(s_aux_undo.written | (1u << i));
            }
        }
        if (err != ESP_OK) {
            snprintf(err_msg, err_cap,
                     "aux_outputs relay %u could not be applied (%s) -- earlier parts of the restore already landed",
                     (unsigned)(i + 1u), esp_err_to_name(err));
            return false;
        }
    }
    return true;
}

/* Best-effort undo of phase 1 after the profiles/zones step failed (zones rolled back, or never
 * reached). Each relay goes back through aux_outputs_cfg_set() against the LIVE zones union, so the
 * one-owner-per-relay invariant still holds: a relay a half-applied zone now claims is refused and
 * logged, never forced. The restore is reported as a partial write either way. */
static BACKUP_IMPORT_NOINLINE void backup_import_aux_outputs_revert_phase1(void)
{
    if (s_aux_undo.written == 0) {
        return;
    }
    uint8_t zones_union = backup_import_aux_zones_union_live();
    for (uint8_t i = 0; i < AUX_OUTPUTS_COUNT; i++) {
        if ((s_aux_undo.written & (1u << i)) == 0) {
            continue;
        }
        esp_err_t err = aux_outputs_cfg_set((uint8_t)(i + 1u), &s_aux_undo.entry[i], zones_union);
        if (err != ESP_OK) {
            ESP_LOGE(BACKUP_TAG, "backup import: could not revert aux relay %u after a failed restore (%s)",
                     (unsigned)(i + 1u), esp_err_to_name(err));
        }
    }
    s_aux_undo.written = 0;
}

/* DEV_REVIEW_13 F8: run the whole pass-1 parse (profiles, zones, timing profiles) into scratch candidate arrays
 * BEFORE anything is committed, so a parse/validate refusal is a 4xx with nothing written. */
static BACKUP_IMPORT_NOINLINE bool backup_import_parse_only(const char *body, char *err_msg, size_t err_cap)
{
    profile_candidate_t *candidates = heap_caps_malloc(sizeof(profile_candidate_t) * PROFILES_MAX_COUNT,
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    zone_candidate_t *zone_candidates = heap_caps_malloc(sizeof(zone_candidate_t) * MAX31856_CHANNEL_COUNT,
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!zone_candidates) {
        zone_candidates = malloc(sizeof(zone_candidate_t) * MAX31856_CHANNEL_COUNT);
    }
    timing_profile_candidate_t *tp = heap_caps_malloc(sizeof(timing_profile_candidate_t) * MAX31856_CHANNEL_COUNT,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tp) {
        tp = malloc(sizeof(timing_profile_candidate_t) * MAX31856_CHANNEL_COUNT);
    }
    bool ok = false;
    if (!candidates || !zone_candidates || !tp) {
        snprintf(err_msg, err_cap, "out of memory validating the backup -- nothing was changed");
    } else {
        bool zl = false, aw = false;
        ok = backup_import_apply_two_pass(body, err_msg, err_cap, candidates, zone_candidates, tp, &zl, &aw, true);
    }
    free(tp);
    free(zone_candidates);
    free(candidates);
    return ok;
}

/* Wrapper: heap-allocates the two big candidate arrays (PSRAM preferred, see
 * this file's header comment above profile_candidate_t) and hands them to
 * backup_import_apply_two_pass(), which is otherwise byte-for-byte the
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
static BACKUP_IMPORT_NOINLINE bool backup_import_apply_body(const char *body, kiln_cfg_restore_mode_t mode, bool dry_run,
                                 int32_t ack_delete_count, bool ack_no_safety_processor, kiln_cfg_plan_t *plan,
                                 bool *partial_write_out, char *err_msg, size_t err_cap)
{
    *partial_write_out = false;

    // Pass 1 for kiln_configs[] runs FIRST, unconditionally, before any
    // profile/zone candidate is even parsed -- same "validate everything,
    // then apply" discipline this file's own header comment describes for
    // the profiles/zones pass, just as its own separate step (kiln_cfg_store.h's
    // commit surface is not profiles_http_save()/zones_config_set_*()). A
    // malformed kiln_configs entry refuses the WHOLE restore, including
    // profiles/zones, exactly like a malformed profile/zone entry does today.
    if (!backup_import_kiln_configs(body, mode, false, ack_delete_count, ack_no_safety_processor, plan, NULL,
                                    err_msg, err_cap)) {
        return false;
    }
    if (!backup_import_update_repo(body, false, plan, err_msg, err_cap)) {
        return false; // pass 1: malformed update_repo refuses the WHOLE restore, nothing written
    }
    if (!backup_import_relay_cycles(body, false, plan, err_msg, err_cap)) {
        return false; // pass 1: malformed relay_cycles refuses the WHOLE restore, nothing written
    }
    if (!backup_import_prefs(body, false, plan, err_msg, err_cap)) {
        return false; // pass 1: an invalid preference refuses the WHOLE restore, nothing written
    }
    if (!backup_import_aux_outputs_validate(body, plan, err_msg, err_cap)) {
        return false; // pass 1: malformed/conflicting aux_outputs refuses the WHOLE restore, nothing written
    }
    if (!backup_import_profiles_precheck(body, err_msg, err_cap)) {
        return false; // pass 1: a profile the post-import config would refuse; nothing written
    }
    if (!backup_import_zone_topology_precheck(body, err_msg, err_cap)) {
        return false; // pass 1: zone entry for a zone this board lacks; nothing written (400, not a partial write)
    }
    if (!backup_import_parse_only(body, err_msg, err_cap)) {
        return false; // pass 1: profile/zone/timing-profile parse refusal; nothing written (400, not a partial write)
    }
    if (dry_run) {
        return true; // plan filled above; nothing written anywhere, profiles/zones untouched
    }

    // Pass 2 for kiln_configs[] now runs BEFORE profiles/zones (task 6):
    // create/rename/mirror-delete kiln config slots. If this fails, nothing
    // else has been touched yet -- UNLESS `kiln_configs_wrote` comes back
    // true (HIGH 1, bkfinish review): backup_import_kiln_configs()'s own
    // commit pass can fail partway through its rename/create/delete loops
    // (e.g. the Nth of several renames fails) after an earlier action in
    // that SAME pass already landed on the store. Previously that case fell
    // through to this function's default *partial_write_out = false and the
    // handler replied 400 "nothing changed" while items 1..N-1 were already
    // persisted -- propagate it here exactly like every other partial-write
    // path in this function does.
    bool kiln_configs_wrote = false;
    if (!backup_import_kiln_configs(body, mode, true, ack_delete_count, ack_no_safety_processor, plan,
                                    &kiln_configs_wrote, err_msg, err_cap)) {
        *partial_write_out = kiln_configs_wrote;
        return false;
    }
    // aux_outputs phase 1 (the entries that DISABLE a relay) lands before zones; phase 2 follows
    // update_repo below. See the block comment above backup_import_aux_num().
    bool aux_wrote = false;
    if (!backup_import_aux_outputs_commit(body, false, &aux_wrote, err_msg, err_cap)) {
        backup_import_aux_outputs_revert_phase1();
        *partial_write_out = kiln_configs_wrote || aux_wrote;
        return false;
    }

    /* PSRAM only, no internal-DRAM fallback: at PROFILES_MAX_COUNT == 100 this
     * array is ~42.8 KB (profile_candidate_t, ~428 B each) -- a `malloc()`
     * fallback landing in internal DRAM at that size is exactly the hazard
     * PROFILE_SLOTS_100.md section 7 task 7 calls out (sockets reset
     * below ~11.9 KB of internal DRAM headroom on this board). Fail cleanly
     * with a logged error instead -- see the "out of memory" `err_msg` path
     * just below. Note this is NOT a "nothing changed" 400: we are past
     * kiln_configs[]'s commit pass, so it sets *partial_write_out (a 500
     * naming what landed), exactly like the three sibling candidate-array
     * allocation failures just below it. */
    profile_candidate_t *candidates = heap_caps_malloc(sizeof(profile_candidate_t) * PROFILES_MAX_COUNT,
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!candidates) {
        ESP_LOGE(BACKUP_TAG, "backup import: PSRAM allocation for %u profile candidates failed "
                              "(%u B) -- refusing rather than falling back to internal DRAM",
                              (unsigned)PROFILES_MAX_COUNT, (unsigned)(sizeof(profile_candidate_t) * PROFILES_MAX_COUNT));
        snprintf(err_msg, err_cap, "out of memory (profile candidates) -- kiln configs were already restored");
        backup_import_aux_outputs_revert_phase1();
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
        backup_import_aux_outputs_revert_phase1();
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
        backup_import_aux_outputs_revert_phase1();
        *partial_write_out = true;
        return false;
    }

    bool zones_landed = false;
    bool ok = backup_import_apply_two_pass(body, err_msg, err_cap, candidates, zone_candidates,
                                         timing_profile_candidates, &zones_landed, &aux_wrote, false);
    /* Pico ceiling LOWERING direction (review of 34a2da1b): run on EVERY
     * exit from backup_import_apply_two_pass(), success or failure. Two cases
     * need it. (1) A successful import that lowered a zone max_temp_c --
     * zones_http_post.c's POST handler already tracks that with
     * safety_ceiling_sync_apply_lower() after its commit, and this path
     * never did, so the Pico's abs_max_temp_c stayed above the new ESP
     * target. (2) A failure AFTER safety_ceiling_sync_guard_raise() already
     * raised and confirmed the Pico ceiling (i_normal_a push refused, or the
     * whole-snapshot rollback of a mid-batch setter failure): the ESP zone
     * max is back at its old value while the Pico sits at the raised one.
     * Either way safety_ceiling_sync.c's divergence check (exact equality
     * after normalization) flags the mismatch and disables heat, and
     * safety_ceiling_sync_reconcile_on_link_up() only ever RAISES, so
     * nothing brought it back. apply_lower() reads its target from the LIVE
     * zones config passed in (whatever RAM holds now: committed batch,
     * rolled-back snapshot, or untouched), is a no-op when that is not a
     * lowering or the Pico's current value is unknown, and only ever
     * tightens the Pico down TO the live ESP max, never below it -- so the
     * "Pico ceiling never tighter than the ESP" invariant holds. Best-effort,
     * same as the POST handler: a failure is logged, never this request's
     * own failure. */
    backup_import_track_ceiling_lower();
    if (!ok && !zones_landed) {
        backup_import_aux_outputs_revert_phase1(); // zones not persisted: put the phase-1 aux disables back
    }

    free(timing_profile_candidates);
    free(zone_candidates);
    free(candidates);
    // update_repo and aux_outputs phase 2 (the entries that ENABLE a relay) now run inside
    // backup_import_apply_two_pass(), after the zones landed and before the profiles are committed.
    if (!ok) {
        // kiln_configs[] already committed above -- this restore is a
        // partial write, not the clean "nothing changed" a 400 implies.
        *partial_write_out = true;
        return false;
    }
    return true;
}

/* Resolves the backup's board topology (pass 1, nothing written) and, for the length of the restore, tells
 * kiln_cfg_store to judge kiln_configs[] packages against it instead of the still-empty live zones config.
 * The system-mode gate and the factory-reset mark are re-checked by backup_import_job() before this runs. */
static bool backup_import_apply(const char *body, kiln_cfg_restore_mode_t mode, bool dry_run,
                                 int32_t ack_delete_count, bool ack_no_safety_processor, kiln_cfg_plan_t *plan,
                                 bool *partial_write_out, char *err_msg, size_t err_cap)
{
    *partial_write_out = false;
    /* HTTP fuzz F4/F5/F6: one complete, well-formed JSON object with no repeated top-level key, and array-typed
     * collections, before any scanner runs. */
    if (!backup_json_validate_document(body, err_msg, err_cap)) {
        return false;
    }
    static const char *const k_array_keys[] = {"profiles", "zones", "kiln_configs", "timing_profiles"};
    for (size_t ki = 0; ki < sizeof(k_array_keys) / sizeof(k_array_keys[0]); ki++) {
        if (backup_json_key_present_not_array(body, k_array_keys[ki])) {
            snprintf(err_msg, err_cap, "\"%s\" must be an array", k_array_keys[ki]);
            return false;
        }
    }
    backup_topology_t topo;
    if (!backup_import_resolve_topology(body, &topo, err_msg, err_cap)) {
        return false;
    }
    if (topo.apply && plan != NULL) {
        kiln_cfg_plan_add(plan, "board topology: set thermo_count %u, relay_count %u first", (unsigned)topo.thermo,
                          (unsigned)topo.relay);
    }
    kiln_cfg_store_restore_topology_override(topo.apply, topo.thermo, topo.relay);
    bool ok = backup_import_apply_body(body, mode, dry_run, ack_delete_count, ack_no_safety_processor, plan,
                                       partial_write_out, err_msg, err_cap);
    kiln_cfg_store_restore_topology_override(false, 0, 0);
    return ok;
}

/* Restore-in-flight flag (2026-09-28, A4 review follow-up A) -- see
 * backup_restore_state.h's doc comment for the contract. Owned entirely by
 * this file; set/cleared only in backup_import_job() below, read (lock-free)
 * by profile_executor_run.c/autotune_engine.c via the getter, through
 * system_mode_gate_check()'s SYS_ACTION_START_PROFILE/SYS_ACTION_START_AUTOTUNE
 * rule. _Atomic, never a lock: CLAUDE.md's lock-order note (s_exec.lock then
 * s_at.lock, relay_authority a leaf) has no slot for a NEW lock taken from
 * those two choke points, and a plain `bool` read from another task while
 * this one writes it would be a data race the same class
 * wifi_provision_http.c's s_httpd_open_sockets/live_profile.c's
 * s_live_profile_generation already avoid the same way. */
static _Atomic bool s_backup_restore_in_flight = false;

/* Second reason a start must refuse: a multi-write configuration change (today the zone-to-aux
 * conversion) is between its steps. Reuses every start gate and commit-point re-check that already
 * reads backup_import_restore_in_flight(), so a run cannot start in the middle of it. */
static _Atomic bool s_config_change_in_flight = false;

void backup_import_config_change_set(bool in_progress)
{
    atomic_store(&s_config_change_in_flight, in_progress);
}

bool backup_import_restore_in_flight(void)
{
    return atomic_load(&s_backup_restore_in_flight) || atomic_load(&s_config_change_in_flight);
}

/* Context for backup_import_job() below, heap-allocated (plain malloc --
 * tiny, no reason to burn PSRAM bookkeeping on it) by
 * backup_import_post_handler() and freed by the job on every exit path.
 * Everything in it came from req's headers/content_len, read on httpd_worker
 * BEFORE the handoff -- httpd_req_get_hdr_value_str()/content_len are not
 * safe to call again on the async copy for state httpd_worker already has,
 * and doing so would just be re-deriving values already in hand. */
typedef struct {
    kiln_cfg_restore_mode_t mode;
    bool dry_run;
    int32_t ack_delete_count;
    bool ack_no_safety;
    size_t content_len;
} backup_import_job_ctx_t;

/* Same two checks, same order, same refusal bytes as the top of
 * backup_import_post_handler(), re-run on the job task just before
 * backup_import_apply() -- see the call site's comment. noinline so the two
 * reason buffers' frame is gone before backup_import_apply()'s deep commit
 * chain runs on this task's 8192 B stack. Returns true if a refusal was
 * sent. */
static BACKUP_IMPORT_NOINLINE bool backup_import_job_recheck_refused(httpd_req_t *async_req, bool ack_no_safety)
{
    /* Factory-reset mark, read AFTER backup_import_job() published
     * s_backup_restore_in_flight (store-then-read, same pairing as the heat
     * sweep): a reset whose mark landed before this read is refused here
     * (flag cleared by the wrapper); a reset that sets its mark later sees
     * the restore's savers refuse under their save locks. Both orders covered. */
    if (relay_authority_reset_in_flight()) {
        ESP_LOGW(BACKUP_TAG, "backup import refused: factory reset in flight (job re-check)");
        httpd_resp_set_status(async_req, "409 Conflict");
        httpd_resp_set_type(async_req, "text/plain");
        httpd_resp_sendstr(async_req, "factory reset in progress");
        return true;
    }
    sys_mode_snapshot_t mode_snap = { 0 };
    relay_authority_heat_run_active(&mode_snap.profile_running, &mode_snap.autotune_running);
    char mode_reason[SYSTEM_MODE_GATE_REASON_MAX];
    mode_reason[0] = '\0';
    if (system_mode_gate_check(SYS_ACTION_WRITE_ZONES_CONFIG, &mode_snap, mode_reason, sizeof(mode_reason))) {
        ESP_LOGW(BACKUP_TAG, "backup import refused by system mode gate (job re-check): %s", mode_reason);
        system_mode_gate_http_send_refusal(async_req, mode_reason);
        return true;
    }
    char reason[OTA_INTERLOCK_REASON_MAX];
    ota_interlock_result_t gate = ota_http_check_interlocks(ack_no_safety, reason, sizeof(reason));
    if (gate != OTA_INTERLOCK_OK) {
        ota_http_send_interlock_refusal(async_req, gate, reason);
        return true;
    }
    return false;
}

/* The slow tail of backup_import_post_handler() (docs/HTTP_POST_OWNER_
 * MIGRATION_PLAN.md slice A4) -- runs on its own task via
 * http_async_job_try_start(), not on httpd_worker: reads the (possibly up to
 * BACKUP_BODY_MAX) body off the async copy of the connection (httpd_req_recv()
 * is supported on it, same as on the original req -- http_async_job.h's own
 * doc comment), then runs the same two-pass validate-then-commit
 * backup_import_apply() the pre-A4 inline handler called from this same
 * point onward. Every response body/status this function sends is BYTE FOR
 * BYTE what the pre-A4 inline handler sent -- backup_import_http_client.py's
 * classify_refusal() depends on these exact strings/codes, so none of them
 * moved. Must not call http_auth_*, cookie or client-IP functions
 * (http_async_job.h's doc comment); must not call
 * httpd_req_async_handler_complete() itself -- http_async_job.c's run_job()
 * does that once this function returns, on every path. ctx is freed here,
 * on every path, since backup_import_post_handler() no longer owns it once
 * http_async_job_try_start() returns HTTP_ASYNC_JOB_STARTED. */
static void backup_import_job_inner(httpd_req_t *async_req, void *arg)
{
    backup_import_job_ctx_t *ctx = (backup_import_job_ctx_t *)arg;
    kiln_cfg_restore_mode_t mode = ctx->mode;
    bool dry_run = ctx->dry_run;
    int32_t ack_delete_count = ctx->ack_delete_count;
    bool ack_no_safety = ctx->ack_no_safety;
    size_t content_len = ctx->content_len;
    free(ctx);

    /* HEAP in PSRAM, not internal DRAM -- same reasoning as the pre-A4
     * inline handler used at this same point: up to BACKUP_BODY_MAX (16384)
     * bytes, far too large for even this job task's own internal-RAM stack.
     * Freed on every return path below. */
    char *body = heap_caps_malloc(content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        httpd_resp_send_err(async_req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return;
    }
    size_t received = 0;
    while (received < content_len) {
        int ret = httpd_req_recv(async_req, body + received, content_len - received);
        if (ret <= 0) {
            /* Same reasoning as the pre-A4 inline handler: nothing has been
             * parsed or applied yet, so a truncated upload is refused with
             * nothing changed. */
            free(body);
            ESP_LOGW(BACKUP_TAG, "backup import body read failed/short: %d", ret);
            httpd_resp_send_err(async_req, HTTPD_400_BAD_REQUEST, "upload incomplete or connection dropped");
            return;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    /* TOCTOU re-check (2026-09-28 review of A4): the mode gate and interlock
     * ran on httpd_worker before the handoff, but httpd_worker is now free
     * while this job reads the body, so an HTTP profile/autotune start (or
     * a heater command) can land in between. Re-run both here, immediately
     * before the first write, with the same refusal bytes the handler
     * sends. Follow-up A (same day): by the time this line runs,
     * backup_import_job()'s wrapper has already set
     * s_backup_restore_in_flight, so a start that instead commits AFTER
     * this point -- including during backup_import_apply()'s own commit
     * pass -- is refused by profile_executor_run()'s/autotune_engine.c's
     * gate check and commit-point re-read of that flag. See
     * backup_import_job()'s comment for the store-then-read pairing that
     * makes this close the window rather than merely narrow it. */
    if (backup_import_job_recheck_refused(async_req, ack_no_safety)) {
        free(body);
        return;
    }

    /* cfg is the only save target: a real import with cfg unmounted would
     * commit nothing durable. Refuse up front (503, same text as every other
     * save route); a dry run writes nothing and is still allowed. */
    if (!dry_run && cfg_fs_http_refuse_if_unmounted(async_req)) {
        free(body);
        return;
    }

    char err_msg[160];
    /* Task 5: kiln_cfg_plan_t heap-allocated -- same reasoning as the pre-A4
     * inline handler used at this same point. Freed on every exit path
     * below. */
    kiln_cfg_plan_t *plan = heap_caps_malloc(sizeof(kiln_cfg_plan_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!plan) {
        free(body);
        httpd_resp_send_err(async_req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return;
    }
    bool partial_write = false;
    bool ok = backup_import_apply(body, mode, dry_run, ack_delete_count, ack_no_safety, plan, &partial_write, err_msg,
                                  sizeof(err_msg));
    free(body);

    if (!ok) {
        httpd_resp_set_status(async_req, partial_write ? "500 Internal Server Error" : "400 Bad Request");
        httpd_resp_set_type(async_req, "text/plain");
        httpd_resp_send(async_req, err_msg, strlen(err_msg));
        free(plan);
        return;
    }

    if (dry_run) {
        httpd_resp_set_type(async_req, "text/plain");
        char *out = heap_caps_malloc(KILN_CFG_PLAN_MAX_LINES * (KILN_CFG_PLAN_LINE_MAX + 1) + 1,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!out) {
            free(plan);
            httpd_resp_send_err(async_req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
            return;
        }
        size_t off = 0;
        out[0] = '\0';
        for (size_t i = 0; i < plan->count; i++) {
            int n = snprintf(out + off, KILN_CFG_PLAN_LINE_MAX + 2, "%s\n", plan->lines[i]);
            if (n > 0) {
                off += (size_t)n;
            }
        }
        httpd_resp_send(async_req, out, off);
        free(out);
        free(plan);
        return;
    }

    free(plan);
    httpd_resp_set_type(async_req, "application/json");
    char ok_json[96];
    int ok_n = snprintf(ok_json, sizeof(ok_json), "{\"ok\":true");
    if (s_rc_kept_mask != 0) {
        ok_n += snprintf(ok_json + ok_n, sizeof(ok_json) - (size_t)ok_n, ",\"relay_cycles_kept\":[");
        bool first = true;
        for (unsigned i = 0; i < RELAY_CYCLES_COUNT; i++) {
            if (s_rc_kept_mask & (1u << i)) {
                ok_n += snprintf(ok_json + ok_n, sizeof(ok_json) - (size_t)ok_n, first ? "%u" : ",%u", i);
                first = false;
            }
        }
        ok_n += snprintf(ok_json + ok_n, sizeof(ok_json) - (size_t)ok_n, "]");
    }
    snprintf(ok_json + ok_n, sizeof(ok_json) - (size_t)ok_n, "}");
    httpd_resp_send(async_req, ok_json, strlen(ok_json));
}

/* Thin wrapper (2026-09-28, A4 review follow-up A): sets
 * s_backup_restore_in_flight BEFORE backup_import_job_inner()'s own TOCTOU
 * re-check (backup_import_job_recheck_refused()) runs, and clears it
 * unconditionally once backup_import_job_inner() returns, on every one of
 * its exit paths (short-circuit refusal, OOM, truncated upload, pass-1
 * validation failure, a committed success, everything) -- a single set/clear
 * pair around the one call, rather than threading a clear into each of
 * backup_import_job_inner()'s several `return` statements individually.
 *
 * Ordering: this is one half of a Dekker-style pair. This side stores the
 * flag (seq_cst) and THEN reads the heat claim (backup_import_job_recheck_
 * refused() -> relay_authority_heat_run_active(), a critical section). The
 * start side (profile_executor_run()/autotune_begin_run_locked(), which
 * every HTTP, UART and LCD start funnels through) publishes its heat claim
 * (relay_authority_heat_zone_claim_begin(), a critical section) and THEN
 * re-reads this flag at its commit point, undoing the claim and refusing if
 * it is set. Whatever the interleaving, at least one side sees the other,
 * so a start and a restore never both proceed. The start side's EARLY gate
 * check (top of each function) is only the legible common-case refusal; on
 * its own it would leave a window as long as the start's pre-commit
 * validation (baseline SPI reads etc.), which is why the commit-point
 * re-read exists (reviewer fix, same day). */
static void backup_import_job(httpd_req_t *async_req, void *arg)
{
    atomic_store(&s_backup_restore_in_flight, true);
    backup_import_job_inner(async_req, arg);
    atomic_store(&s_backup_restore_in_flight, false);
}

esp_err_t backup_import_post_handler(httpd_req_t *req)
{
    /* Owner decision Q2 (docs/SYSTEM_MODE_GATE.md, 2026-09-25,
     * gate-slices-2/4/5 spec): refuse ALL zone/relay/guard config writes --
     * restoring a backup is exactly that -- while a firing or autotune run
     * is active, PAUSED included. Checked HERE, at the very top of the
     * handler, before the interlock below and before the body is even read.
     *
     * Review fix, 2026-09-25: this used to be buried inside
     * backup_import_apply(), which the interlock check below already made
     * unreachable for the same facts (ota_http_check_interlocks() already
     * refuses "a profile is running"/"autotune is running" first) -- a
     * "MODE_GATE_REFUSED:" sentinel threaded back through err_msg for a
     * refusal that could never actually happen. Moved to the top and ahead
     * of the interlock, same ordering factory_reset.c already uses (its own
     * test asserts !g_probe_interlock_called); the sentinel plumbing is
     * gone since this is now a direct, immediate 409, not a deferred
     * outcome of backup_import_apply(). */
    {
        sys_mode_snapshot_t mode_snap = { 0 };
        relay_authority_heat_run_active(&mode_snap.profile_running, &mode_snap.autotune_running);
        char mode_reason[SYSTEM_MODE_GATE_REASON_MAX];
        mode_reason[0] = '\0';
        if (system_mode_gate_check(SYS_ACTION_WRITE_ZONES_CONFIG, &mode_snap, mode_reason, sizeof(mode_reason))) {
            ESP_LOGW(BACKUP_TAG, "backup import refused by system mode gate: %s", mode_reason);
            return system_mode_gate_http_send_refusal(req, mode_reason);
        }
    }

    /* Interlock next, before reading the body at all -- "refused while a
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

    // Refuse while an http_async_job (ct_auto_zero's 10-15s measurement, or
    // this same route's own job -- see below) is running: a restore can
    // write the same safety_cfg_store/zones_config state that job commits at
    // the end of its window, and letting both proceed concurrently risks one
    // clobbering the other's write (2026-09-25 fix-then-push review, A2
    // pulled forward; kept as its own synchronous, no-allocation refusal
    // path after A4's migration onto the same helper below, same reasoning
    // as bench_preset_post_handler's own pre-check). Set explicitly rather
    // than via httpd_resp_send_err(): esp_http_server has no
    // HTTPD_409_CONFLICT enumerator (same workaround as kiln_cfg_http.c's
    // apply-in-flight refusal) -- 409 is the right code, a refusal the
    // operator cannot argue with.
    if (http_async_job_busy()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "another commissioning operation is running");
    }

    if (req->content_len <= 0 || (size_t)req->content_len > BACKUP_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }

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
    esp_err_t mode_err = httpd_req_get_hdr_value_str(req, "X-Kiln-Config-Mode", mode_val, sizeof(mode_val));
    if (mode_err == ESP_ERR_HTTPD_RESULT_TRUNC) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "X-Kiln-Config-Mode value too long");
        return ESP_OK;
    }
    if (mode_err == ESP_OK && backup_names_equal_ci(mode_val, "mirror")) {
        mode = KILN_CFG_RESTORE_MIRROR;
    }
    bool dry_run = false;
    char dry_run_val[8];
    esp_err_t dry_err = httpd_req_get_hdr_value_str(req, "X-Kiln-Config-Dry-Run", dry_run_val, sizeof(dry_run_val));
    if (dry_err == ESP_OK) {
        if (strcmp(dry_run_val, "1") == 0) {
            dry_run = true;
        } else if (strcmp(dry_run_val, "0") != 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "X-Kiln-Config-Dry-Run must be 1 or 0");
            return ESP_OK;
        }
    } else if (dry_err == ESP_ERR_HTTPD_RESULT_TRUNC) {
        /* fail closed: a truncated value must never fall through to a real import */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "X-Kiln-Config-Dry-Run value too long");
        return ESP_OK;
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
    esp_err_t ack_err =
        httpd_req_get_hdr_value_str(req, "X-Kiln-Config-Ack-Delete", ack_delete_val, sizeof(ack_delete_val));
    if (ack_err == ESP_OK) {
        char *endp = NULL;
        long parsed = strtol(ack_delete_val, &endp, 10);
        if (endp != ack_delete_val && *endp == '\0' && parsed >= 0 && parsed <= INT32_MAX) {
            ack_delete_count = (int32_t)parsed;
        }
    } else if (ack_err == ESP_ERR_HTTPD_RESULT_TRUNC) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "X-Kiln-Config-Ack-Delete value too long");
        return ESP_OK;
    }

    /* Task 4 (docs/HTTP_POST_OWNER_MIGRATION.md slice A4): hand the body
     * read plus the two-pass validate-then-commit off to backup_import_job()
     * on its own task -- up to 100 profile/zone/kiln_config slots through
     * the stores' public save functions, measured on the bench (2026-09-28,
     * A4 finding) to exceed the PC client's own request timeout and leave
     * the board's HTTP stack answering no other request (including
     * GET /api/status polls) for the duration. Everything above this point
     * (mode gate, interlock, busy check, content-length bound, header reads)
     * is fast and stays on httpd_worker; ctx carries the header-derived
     * values through since http_async_job_try_start() may hand req off to a
     * different task before this function returns, and http_auth_ / header
     * functions are not safe to call again on that task (http_async_job.h's
     * own doc comment). */
    backup_import_job_ctx_t *ctx = malloc(sizeof(backup_import_job_ctx_t));
    if (!ctx) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    ctx->mode = mode;
    ctx->dry_run = dry_run;
    ctx->ack_delete_count = ack_delete_count;
    ctx->ack_no_safety = ota_http_req_ack_no_safety(req);
    ctx->content_len = (size_t)req->content_len;

    http_async_job_start_result_t start_result =
        http_async_job_try_start(req, "http_async_job", 10240, backup_import_job, ctx);
    if (start_result == HTTP_ASYNC_JOB_STARTED) {
        return ESP_OK;
    }
    free(ctx);
    if (start_result == HTTP_ASYNC_JOB_BUSY) {
        /* Refused -- another async job (ct_auto_zero, bench_preset, or a
         * concurrent second POST to this same route) is already running.
         * req is untouched by http_async_job_try_start() in every refusal
         * case, so responding on it synchronously here is safe. Same reply
         * text the pre-check above already sends, so a caller sees no
         * difference in which branch produced it. */
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "another commissioning operation is running");
    }
    /* HTTP_ASYNC_JOB_RESOURCE_FAILURE: the async handoff itself failed
     * (httpd_req_async_handler_begin()) or the job task could not be
     * created -- an out-of-memory-shaped failure, not contention. Same
     * "out of memory" 500 the pre-A4 handler already sent for its own
     * allocation failures at this point. */
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    return ESP_OK;
}
