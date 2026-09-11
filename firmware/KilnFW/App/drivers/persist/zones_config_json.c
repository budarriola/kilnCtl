// See zones_config_json.h for why these functions, the current zones_cfg_t
// layout, and the historical zone_cfg_v1_t..v10_t/zones_cfg_v1_t..v10_t
// on-flash snapshots (declared there, not here -- test_zones_http.c#includes
// zones_http.c textually and references several of them directly) all live
// in their own HTTP-free file.
//
// Split 2026-09-04 (ROADMAP.md M15, the 1500-line rule) into three files --
// see zones_config_json_internal.h's own doc comment for the full map. This
// file keeps the settings_source chain-walk wrapper/normalizer, zones_
// config_json_validate(), the HTTP-free field parsers, and ZONES_CFG_TAG's
// one definition (the version-migration code moved to zones_config_convert.c/
// zones_config_migrate.c, both of which reach this file's TAG via extern
// ZONES_CFG_TAG in zones_config_json_internal.h).
#include "zones_config_json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "http_form.h"
#include "kiln_io.h" /* KILN_IO_RELAY_COUNT -- zones_config_json_validate() */
#include "zone_settings_source_chain.h" /* the shared settings_source chain-walk -- see that
                                          * header's own comment for why it lives outside this
                                          * file too: test_backup_import.c's stub for
                                          * zones_config_settings_source_import_has_cycle()
                                          * calls the identical algorithm from here. */

const char *ZONES_CFG_TAG = "zones_config_json";

/* Bounded chain walk starting at `start`, following settings_source links
 * through `zones[]` (MAX31856_CHANNEL_COUNT-sized, indexed exactly like
 * zones_cfg_t::zones). Returns true if the chain revisits a zone already on
 * it -- a genuine inheritance cycle -- and false if it terminates cleanly at
 * ZONE_SETTINGS_SOURCE_CUSTOM or at a link landing on or past `thermo_count`.
 *
 * `thermo_count` bounds which zones this walk treats as "live": a link
 * pointing at index >= thermo_count is treated as terminal (no cycle via
 * that path), the SAME "unused trailing slot" discipline zones_config_json_validate()
 * and parse_zone_fields()'s own `i >= thermo_count` early return already
 * apply to those slots elsewhere in this file. This matters because a slot
 * never explicitly configured reads back as settings_source == 0 (0 is a
 * REAL, DIFFERENT value here -- see ZONE_SETTINGS_SOURCE_CUSTOM's doc
 * comment -- never a "not set" sentinel), which for zone 0 itself is
 * indistinguishable from an explicit self-reference; without this bound, an
 * unrelated unconfigured trailing zone's raw-zero byte would read as a link
 * back to zone 0 and could manufacture a false cycle out of two slots
 * neither caller ever actually linked. Every zone this file's decode/
 * migration path actually produces sets an unconfigured zone's
 * settings_source to ZONE_SETTINGS_SOURCE_CUSTOM explicitly (never leaves it
 * at raw 0 -- see convert_zone_v9()'s own "NEVER 0" comment), so in a
 * real, decoded config this bound is close to a no-op; it only matters for
 * the zero-initialized slack past thermo_count.
 *
 * Capped at MAX31856_CHANNEL_COUNT hops so this terminates even walking an
 * already-corrupt stored config (e.g. loaded off flash before this guard
 * existed) -- there are only MAX31856_CHANNEL_COUNT distinct zones, so any
 * chain that has not hit CUSTOM, a >=thermo_count link, or a repeat within
 * that many hops is, by the pigeonhole principle, about to repeat on the
 * very next hop; treating "hit the cap" as a cycle is exact, not a
 * conservative over-refusal. Self-reference (start's own link pointing back
 * at start) is caught on the very first hop, same as every other repeat --
 * this function does not special-case it, callers that want a distinct
 * error message for self-reference check that separately, first.
 *
 * The walk itself now lives in zone_settings_source_chain.h (shared with
 * test_backup_import.c's stub double, see that header's comment) -- this is
 * a thin wrapper extracting the raw settings_source bytes out of the
 * zone_cfg_t array every caller here actually has, so every existing call
 * site in this file keeps working unchanged. */
bool zones_config_json_settings_source_chain_has_cycle(const zone_cfg_t zones[MAX31856_CHANNEL_COUNT], uint8_t group,
                                            uint8_t start, uint8_t thermo_count)
{
    uint8_t sources[MAX31856_CHANNEL_COUNT];
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        sources[i] = zones[i].settings_source[group];
    }
    return zone_settings_source_chain_has_cycle(sources, start, thermo_count);
}

/* Every-load fixup, LOAD PATH ONLY (nvs_load_from() -- never
 * zones_config_import_blob(), which shares zones_config_json_decode_blob() with the
 * load path but must REJECT a cycle in pass 1 instead, per this file's own
 * two-pass-import discipline -- see that function's own comment). A cycle
 * could only reach flash by loading through firmware that predates the
 * chain-walk guards above, or by direct NVS tampering -- either way, a
 * config that was valid before this pass shipped must keep booting, not get
 * wiped: zones_config_json_validate() rejecting on this path would drive
 * zones_config_json_decode_blob() to the CORRUPT/wipe outcome and destroy an entire
 * commissioned config over one stale UI-only provenance link (this repo has
 * lost configs to exactly that shape of overreaction twice already -- see
 * raise_heater_timing_to_floors()'s neighboring precedent). So: collapse,
 * don't reject. ONLY the zones actually ON the cycle get their link reset to
 * ZONE_SETTINGS_SOURCE_CUSTOM (the same "collapse to Custom" resolution
 * zones_page.html's client-side resolveTerminal() already performs on a
 * stale page load) and a loud log line naming each -- a zone that merely
 * LEADS INTO a cycle (its own chain is fine, it just happens to walk into
 * one) is left untouched, since breaking any single edge on the cycle itself
 * already frees every lead-in zone's chain too. Getting this wrong is not
 * just cosmetic: resetting `start`'s own link (an earlier version of this
 * function did exactly that) is index-order dependent -- given a stored
 * 1<->2 cycle with zone 0 -> 1 merely leading into it, the walk starting at
 * i=0 hits the cycle and would reset zone 0's OWN link first, even though
 * zone 0 was never part of the cycle and breaking 1<->2 alone would have
 * sufficed. Identifying the exact cycle membership (the walked path from the
 * first repeated node onward, not every node visited on the way there)
 * avoids that: this reaches the same fixed point regardless of which index
 * is scanned first, and breaking one link at a time is guaranteed to
 * terminate within MAX31856_CHANNEL_COUNT passes since each pass strictly
 * shrinks the number of zones still mid-chain. */
void zones_config_json_normalize_settings_source_cycles(zones_cfg_t *cfg, const char *partition)
{
    uint8_t thermo_count = cfg->thermo_count > MAX31856_CHANNEL_COUNT ? MAX31856_CHANNEL_COUNT : cfg->thermo_count;
    /* Each of the SRC_GROUP_COUNT groups has its own, entirely independent
     * chain -- a cycle in one group says nothing about any other -- so this
     * whole walk-and-collapse runs once per group. */
    for (uint8_t group = 0; group < SRC_GROUP_COUNT; group++) {
        for (uint8_t i = 0; i < thermo_count; i++) {
            if (!zones_config_json_settings_source_chain_has_cycle(cfg->zones, group, i, thermo_count)) {
                continue;
            }
            /* Re-walk from i, this time recording the path in order: the first
             * repeated node's position marks where the cycle actually starts,
             * and only that node plus everything walked after it are ON the
             * cycle -- everything recorded before it is a lead-in and must not
             * be touched. */
            uint8_t path[MAX31856_CHANNEL_COUNT];
            uint8_t path_len = 0;
            uint8_t cur = i;
            uint8_t cycle_start_pos = 0;
            /* <= MAX31856_CHANNEL_COUNT, not <: with COUNT distinct zones, path[]
             * can hold at most COUNT entries before the pigeonhole principle
             * guarantees a repeat -- the repeat is only OBSERVED on the hop that
             * revisits it, which is one iteration past the one that appended the
             * COUNT-th distinct entry. A `hop < MAX31856_CHANNEL_COUNT` bound
             * here stops exactly one iteration too early and would silently
             * treat a genuine cycle as "terminated cleanly," leaving it
             * unbroken. */
            for (uint8_t hop = 0; hop <= MAX31856_CHANNEL_COUNT; hop++) {
                uint8_t repeat_pos = 0;
                bool repeated = false;
                for (uint8_t p = 0; p < path_len; p++) {
                    if (path[p] == cur) {
                        repeat_pos = p;
                        repeated = true;
                        break;
                    }
                }
                if (repeated) {
                    cycle_start_pos = repeat_pos;
                    break;
                }
                path[path_len++] = cur;
                uint8_t src = cfg->zones[cur].settings_source[group];
                if (src == ZONE_SETTINGS_SOURCE_CUSTOM || src >= thermo_count) {
                    break; /* terminates cleanly -- can only happen if an earlier loop
                            * iteration already fixed the cycle this start used to reach */
                }
                cur = src;
            }
            for (uint8_t p = cycle_start_pos; p < path_len; p++) {
                uint8_t zone = path[p];
                ESP_LOGW(ZONES_CFG_TAG, "zones_cfg from '%s': zone %u group %u's settings_source chain forms a "
                              "cycle -- collapsing zone %u group %u to Custom (was %u)",
                         partition, (unsigned)zone, (unsigned)group, (unsigned)zone, (unsigned)group,
                         (unsigned)cfg->zones[zone].settings_source[group]);
                cfg->zones[zone].settings_source[group] = ZONE_SETTINGS_SOURCE_CUSTOM;
            }
        }
    }
}
/* Validates every field of `cand` -- a fully migrated, CURRENT-version
 * zones_cfg_t -- against the exact bounds parse_zone_fields()/
 * zones_config_set_*() enforce on a live POST. Used only by
 * zones_config_import_blob() below; a config stored by kiln_cfg_store.c may
 * have been saved years ago, under looser bounds, or by firmware this build
 * has since tightened, so it is re-checked here rather than trusted because
 * it was valid once. */
bool zones_config_json_validate(const zones_cfg_t *cand, const char **err_reason)
{
    if (cand->thermo_count > MAX31856_CHANNEL_COUNT) {
        *err_reason = "thermo_count out of range";
        return false;
    }
    if (cand->relay_count > KILN_IO_RELAY_COUNT) {
        *err_reason = "relay_count out of range";
        return false;
    }
    if (cand->max_simultaneous_relays > KILN_IO_RELAY_COUNT) {
        *err_reason = "max_simultaneous_relays out of range";
        return false;
    }
    if (cand->safety_tc_type > ZONE_TC_TYPE_MAX_REAL) {
        *err_reason = "safety_tc_type out of range";
        return false;
    }
    if (!isfinite(cand->pc_link_abort_silence_ms) || cand->pc_link_abort_silence_ms < 0.0f ||
        cand->pc_link_abort_silence_ms > ZONE_PC_LINK_SILENCE_MS_MAX) {
        *err_reason = "pc_link_abort_silence_ms out of range";
        return false;
    }
    /* 2026-08-27 (ZONES_CFG_VERSION 8->9): timing_profile_count must be at
     * least 1 -- every zone_cfg_t::timing_profile, including a freshly
     * zero-initialized zone's 0, must resolve to a real profile -- and at
     * most MAX31856_CHANNEL_COUNT, the array's fixed capacity (see
     * zones_cfg_t::timing_profile_count's own comment for why that bound,
     * not some larger operator-facing limit, is the correct ceiling). Each
     * IN-USE profile's nine fields get the exact same per-field bounds these
     * nine had as zone_cfg_t fields before this pass -- only WHERE they live
     * changed, not their validation. */
    if (cand->timing_profile_count < 1 || cand->timing_profile_count > MAX31856_CHANNEL_COUNT) {
        *err_reason = "timing_profile_count out of range";
        return false;
    }
    for (uint8_t p = 0; p < cand->timing_profile_count; p++) {
        const zone_timing_profile_t *tp = &cand->timing_profiles[p];
        if (!isfinite(tp->guard_progress_duty_min) || tp->guard_progress_duty_min < 0.0f ||
            tp->guard_progress_duty_min > ZONE_GUARD_DUTY_MAX) {
            *err_reason = "timing profile guard_progress_duty_min out of range";
            return false;
        }
        if (!isfinite(tp->guard_progress_window_s) || tp->guard_progress_window_s < 0.0f ||
            tp->guard_progress_window_s > ZONE_GUARD_TIME_S_MAX) {
            *err_reason = "timing profile guard_progress_window_s out of range";
            return false;
        }
        if (!isfinite(tp->guard_drift_hysteresis_c) || tp->guard_drift_hysteresis_c < 0.0f ||
            tp->guard_drift_hysteresis_c > ZONE_GUARD_MARGIN_C_MAX) {
            *err_reason = "timing profile guard_drift_hysteresis_c out of range";
            return false;
        }
        if (!isfinite(tp->guard_frozen_eps_c) || tp->guard_frozen_eps_c < 0.0f ||
            tp->guard_frozen_eps_c > ZONE_GUARD_EPS_C_MAX) {
            *err_reason = "timing profile guard_frozen_eps_c out of range";
            return false;
        }
        if (!isfinite(tp->guard_cross_zone_period_s) || tp->guard_cross_zone_period_s < 0.0f ||
            tp->guard_cross_zone_period_s > ZONE_GUARD_TIME_S_MAX) {
            *err_reason = "timing profile guard_cross_zone_period_s out of range";
            return false;
        }
        if (!isfinite(tp->bangbang_hysteresis_c) || tp->bangbang_hysteresis_c < 0.0f ||
            tp->bangbang_hysteresis_c > ZONE_GUARD_MARGIN_C_MAX) {
            *err_reason = "timing profile bangbang_hysteresis_c out of range";
            return false;
        }
        if (!isfinite(tp->cooling_limited_margin_c) || tp->cooling_limited_margin_c < 0.0f ||
            tp->cooling_limited_margin_c > ZONE_GUARD_MARGIN_C_MAX) {
            *err_reason = "timing profile cooling_limited_margin_c out of range";
            return false;
        }
        if (!isfinite(tp->cooling_limited_hold_s) || tp->cooling_limited_hold_s < 0.0f ||
            tp->cooling_limited_hold_s > ZONE_GUARD_TIME_S_MAX) {
            *err_reason = "timing profile cooling_limited_hold_s out of range";
            return false;
        }
        if (!isfinite(tp->ramp_lock_band_c) || tp->ramp_lock_band_c < 0.0f ||
            tp->ramp_lock_band_c > ZONE_GUARD_MARGIN_C_MAX) {
            *err_reason = "timing profile ramp_lock_band_c out of range";
            return false;
        }
    }
    uint8_t relay_valid_bits =
        cand->relay_count >= 8 ? 0xFF : (uint8_t)((1u << cand->relay_count) - 1u);
    uint8_t thermo_valid_bits =
        cand->thermo_count >= 8 ? 0xFF : (uint8_t)((1u << cand->thermo_count) - 1u);
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        const zone_cfg_t *z = &cand->zones[i];
        /* tc_type is per-CHANNEL, meaningful past thermo_count too -- same
         * reasoning parse_zone_fields() applies (see its own comment). */
        if (z->tc_type > ZONE_TC_TYPE_MAX_REAL) {
            *err_reason = "zone tc_type out of range";
            return false;
        }
        if (i >= cand->thermo_count) {
            continue; /* unused trailing slot -- matches parse_zone_fields()'s early return */
        }
        if ((z->relay_mask & ~relay_valid_bits) != 0) {
            *err_reason = "zone relay_mask references an unconfigured relay";
            return false;
        }
        if ((z->thermo_mask & ~thermo_valid_bits) != 0) {
            *err_reason = "zone thermo_mask references an unconfigured thermocouple channel";
            return false;
        }
        {
            uint8_t ct_valid_bits = (uint8_t)((1u << ZONE_CT_CHANNEL_COUNT) - 1u);
            if ((z->ct_mask & ~ct_valid_bits) != 0) {
                *err_reason = "zone ct_mask references an unconfigured current-sense channel";
                return false;
            }
        }
        /* 2026-08-27 (ZONES_CFG_VERSION 8->9): must reference a profile that
         * actually exists in THIS candidate config -- timing_profile_count
         * was already bounds-checked above, so this is a straight index
         * check, same shape as thermo_mask/ct_mask's bit-range checks. */
        if (z->timing_profile >= cand->timing_profile_count) {
            *err_reason = "zone timing_profile references a timing profile that doesn't exist";
            return false;
        }
        if (!isfinite(z->cal_offset_c) || z->cal_offset_c < ZONE_CAL_OFFSET_MIN_C ||
            z->cal_offset_c > ZONE_CAL_OFFSET_MAX_C) {
            *err_reason = "zone cal_offset_c out of range";
            return false;
        }
        if (!isfinite(z->pid_kp) || z->pid_kp < 0.0f || z->pid_kp > 1000.0f) {
            *err_reason = "zone pid_kp out of range";
            return false;
        }
        if (!isfinite(z->pid_ki) || z->pid_ki < 0.0f || z->pid_ki > 1000.0f) {
            *err_reason = "zone pid_ki out of range";
            return false;
        }
        if (!isfinite(z->pid_kd) || z->pid_kd < 0.0f || z->pid_kd > 1000.0f) {
            *err_reason = "zone pid_kd out of range";
            return false;
        }
        if (!isfinite(z->max_ramp_c_per_hr) || z->max_ramp_c_per_hr < 0.0f ||
            z->max_ramp_c_per_hr > ZONE_MAX_RAMP_C_PER_HR_MAX) {
            *err_reason = "zone max_ramp_c_per_hr out of range";
            return false;
        }
        if (!isfinite(z->sanity_rate_c_per_min) || z->sanity_rate_c_per_min < 0.0f ||
            z->sanity_rate_c_per_min > ZONE_SANITY_RATE_MAX_C_PER_MIN) {
            *err_reason = "zone sanity_rate_c_per_min out of range";
            return false;
        }
        if (z->control_mode > (uint8_t)ZONE_CONTROL_MODE_PID_FUZZY) {
            *err_reason = "zone control_mode out of range";
            return false;
        }
        if (z->relay_type > ZONE_RELAY_TYPE_MAX) {
            *err_reason = "zone relay_type out of range";
            return false;
        }
        /* docs/ON_OFF_ZONE_PLAN.md step 6: the UI is the first writer of
         * these five fields, so this is their first validation too. Same
         * "refused, never clamped" discipline as every other field here --
         * zone_type/failsafe_state are boolean-ish (0/1), 0 is always legal
         * for hyst_c/min_on_s/min_off_s (the getters substitute the plan
         * default at read time, see zones_config_get_hyst_c()'s own
         * comment), so the same [0, MAX] then (0, MIN)-sliver split as
         * progress_band_c/error_band_c is used. */
        if (z->zone_type > (uint8_t)ZONE_TYPE_ON_OFF) {
            *err_reason = "zone zone_type out of range";
            return false;
        }
        if (z->failsafe_state > 1) {
            *err_reason = "zone failsafe_state out of range";
            return false;
        }
        if (!isfinite(z->hyst_c) || z->hyst_c < 0.0f || z->hyst_c > ZONE_HYST_C_MAX ||
            (z->hyst_c != 0.0f && z->hyst_c < ZONE_HYST_C_MIN)) {
            *err_reason = "zone hyst_c out of range";
            return false;
        }
        if (z->min_on_s > ZONE_MIN_ON_OFF_S_MAX ||
            (z->min_on_s != 0 && z->min_on_s < ZONE_MIN_ON_OFF_S_MIN)) {
            *err_reason = "zone min_on_s out of range";
            return false;
        }
        if (z->min_off_s > ZONE_MIN_ON_OFF_S_MAX ||
            (z->min_off_s != 0 && z->min_off_s < ZONE_MIN_ON_OFF_S_MIN)) {
            *err_reason = "zone min_off_s out of range";
            return false;
        }
        /* ZONES_CFG_VERSION 23->24: model_fit_temp_c/model_fit_ambient_c.
         * ZONE_MODEL_FIT_TEMP_UNKNOWN (-273.15f) is always legal -- it is the
         * documented "no context recorded" sentinel, same "sentinel is
         * always legal" discipline model_k_dc's own all-zero encoding uses.
         * Anything else must be finite and inside the same generous
         * physically-plausible range zones_config_set_model_fit_context()
         * enforces at write time -- kept in sync with that function
         * deliberately rather than sharing one helper, since one lives in
         * zones_config_accessors.c (RAM-only setters) and this one runs over
         * a blob that may never have gone through that setter at all (a
         * migrated or hand-crafted one). */
        if (z->model_fit_temp_c != ZONE_MODEL_FIT_TEMP_UNKNOWN &&
            (!isfinite(z->model_fit_temp_c) || z->model_fit_temp_c <= -50.0f || z->model_fit_temp_c >= 1300.0f)) {
            *err_reason = "zone model_fit_temp_c out of range";
            return false;
        }
        if (z->model_fit_ambient_c != ZONE_MODEL_FIT_TEMP_UNKNOWN &&
            (!isfinite(z->model_fit_ambient_c) || z->model_fit_ambient_c <= -50.0f ||
             z->model_fit_ambient_c >= 1300.0f)) {
            *err_reason = "zone model_fit_ambient_c out of range";
            return false;
        }
        /* ZONES_CFG_VERSION 24->25: coil_power_w. 0 is always legal (the
         * documented "not overridden, use an equal share of the sum
         * nameplate" sentinel); anything else must be finite and inside the
         * same bound zones_config_set_coil_power_w() enforces at write
         * time -- kept in sync deliberately, same reasoning as
         * model_fit_temp_c's own comment just above. */
        if (z->coil_power_w != 0.0f &&
            (!isfinite(z->coil_power_w) || z->coil_power_w < ZONE_COIL_POWER_W_MIN ||
             z->coil_power_w > ZONE_COIL_POWER_W_MAX)) {
            *err_reason = "zone coil_power_w out of range";
            return false;
        }
        /* ZONES_CFG_VERSION 25->26: autotune_baseline_k_dc. 0 is always legal
         * (the documented "no baseline recorded yet" sentinel, same
         * convention model_k_dc itself already uses); anything else must be
         * finite, non-negative, and no larger than ZONE_AUTOTUNE_K_DC_MAX --
         * the same physically-derived ceiling adaptive_tune_model.c's
         * blend/plausibility path enforces before ever writing this field,
         * kept in sync deliberately, same reasoning as coil_power_w's own
         * comment just above. */
        if (z->autotune_baseline_k_dc != 0.0f &&
            (!isfinite(z->autotune_baseline_k_dc) || z->autotune_baseline_k_dc < 0.0f ||
             z->autotune_baseline_k_dc > ZONE_AUTOTUNE_K_DC_MAX)) {
            *err_reason = "zone autotune_baseline_k_dc out of range";
            return false;
        }
        if (!isfinite(z->max_temp_c) || z->max_temp_c < 0.0f || z->max_temp_c > ZONE_MAX_TEMP_C_MAX) {
            *err_reason = "zone max_temp_c out of range";
            return false;
        }
        if (!isfinite(z->min_temp_c) || z->min_temp_c < ZONE_MIN_TEMP_C_MIN ||
            z->min_temp_c > ZONE_MIN_TEMP_C_MAX) {
            *err_reason = "zone min_temp_c out of range";
            return false;
        }
        if (!isfinite(z->heater_window_ms) || z->heater_window_ms < 0.0f ||
            z->heater_window_ms > ZONE_HEATER_WINDOW_MS_MAX) {
            *err_reason = "zone heater_window_ms out of range";
            return false;
        }
        if (!isfinite(z->heater_min_on_ms) || z->heater_min_on_ms < 0.0f ||
            z->heater_min_on_ms > ZONE_HEATER_MIN_ON_OFF_MS_MAX) {
            *err_reason = "zone heater_min_on_ms out of range";
            return false;
        }
        if (z->heater_min_on_ms > 0.0f && z->heater_min_on_ms < ZONE_HEATER_MIN_ON_MS_FLOOR) {
            *err_reason = "zone heater_min_on_ms below the 10000 ms relay-protection floor";
            return false;
        }
        if (!isfinite(z->heater_min_off_ms) || z->heater_min_off_ms < 0.0f ||
            z->heater_min_off_ms > ZONE_HEATER_MIN_ON_OFF_MS_MAX) {
            *err_reason = "zone heater_min_off_ms out of range";
            return false;
        }
        if (z->heater_window_ms > 0.0f && z->heater_window_ms < zone_required_window_ms(z->heater_min_on_ms)) {
            *err_reason = "zone heater_window_ms shorter than 3x its heater_min_on_ms";
            return false;
        }
        if (!isfinite(z->guard_wrong_dir_window_s) || z->guard_wrong_dir_window_s < 0.0f ||
            z->guard_wrong_dir_window_s > ZONE_GUARD_TIME_S_MAX) {
            *err_reason = "zone guard_wrong_dir_window_s out of range";
            return false;
        }
        if (!isfinite(z->guard_wrong_dir_rate_c_per_min) || z->guard_wrong_dir_rate_c_per_min < 0.0f ||
            z->guard_wrong_dir_rate_c_per_min > ZONE_GUARD_RATE_C_PER_MIN_MAX) {
            *err_reason = "zone guard_wrong_dir_rate_c_per_min out of range";
            return false;
        }
        if (!isfinite(z->guard_off_settle_s) || z->guard_off_settle_s < 0.0f ||
            z->guard_off_settle_s > ZONE_GUARD_TIME_S_MAX) {
            *err_reason = "zone guard_off_settle_s out of range";
            return false;
        }
        if (!isfinite(z->guard_runaway_rate_c_per_min) || z->guard_runaway_rate_c_per_min < 0.0f ||
            z->guard_runaway_rate_c_per_min > ZONE_GUARD_RATE_C_PER_MIN_MAX) {
            *err_reason = "zone guard_runaway_rate_c_per_min out of range";
            return false;
        }
        if (!isfinite(z->guard_runaway_margin_c) || z->guard_runaway_margin_c < 0.0f ||
            z->guard_runaway_margin_c > ZONE_GUARD_MARGIN_C_MAX) {
            *err_reason = "zone guard_runaway_margin_c out of range";
            return false;
        }
        if (!isfinite(z->guard_drift_period_s) || z->guard_drift_period_s < 0.0f ||
            z->guard_drift_period_s > ZONE_GUARD_TIME_S_MAX) {
            *err_reason = "zone guard_drift_period_s out of range";
            return false;
        }
        if (!isfinite(z->guard_sensor_fault_debounce_ticks) || z->guard_sensor_fault_debounce_ticks < 0.0f ||
            z->guard_sensor_fault_debounce_ticks > ZONE_GUARD_DEBOUNCE_TICKS_MAX) {
            *err_reason = "zone guard_sensor_fault_debounce_ticks out of range";
            return false;
        }
        if (!isfinite(z->guard_frozen_window_s) || z->guard_frozen_window_s < 0.0f ||
            z->guard_frozen_window_s > ZONE_GUARD_TIME_S_MAX) {
            *err_reason = "zone guard_frozen_window_s out of range";
            return false;
        }
        if (!isfinite(z->cross_zone_max_delta_c) || z->cross_zone_max_delta_c < 0.0f ||
            z->cross_zone_max_delta_c > ZONE_CROSS_ZONE_DELTA_C_MAX) {
            *err_reason = "zone cross_zone_max_delta_c out of range";
            return false;
        }
        /* The nine timing-override checks that used to live here moved to the
         * timing_profiles[] loop above -- z->timing_profile's own bound check,
         * a few lines up, is what stands in their place at THIS per-zone spot
         * now (see ZONES_CFG_VERSION's 8->9 comment). */
        if (!isfinite(z->model_k_dc) || !isfinite(z->model_tau_s) || !isfinite(z->model_dead_time_s) ||
            z->model_k_dc < 0.0f || z->model_tau_s < 0.0f || z->model_dead_time_s < 0.0f ||
            z->model_k_dc > ZONE_MODEL_K_MAX || z->model_tau_s > ZONE_MODEL_TIME_MAX_S ||
            z->model_dead_time_s > ZONE_MODEL_TIME_MAX_S) {
            *err_reason = "zone plant model out of range";
            return false;
        }
        /* 2026-08-30 (ZONES_CFG_VERSION 9->10, PID_EXPANSION_PLAN.md Phase 2) */
        if (!isfinite(z->fuzzy_strength_pct) || z->fuzzy_strength_pct < 0.0f ||
            z->fuzzy_strength_pct > ZONE_FUZZY_STRENGTH_PCT_MAX) {
            *err_reason = "zone fuzzy_strength_pct out of range";
            return false;
        }
        /* 2026-08-30 (ZONES_CFG_VERSION 10->11): row, not a pair -- every
         * cell checked, diagonal (this zone's own index) held to exactly 0.
         * See zone_cfg_t::coupling_coeff's own doc comment for why the
         * off-diagonal bound stayed non-negative. */
        for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
            if (!isfinite(z->coupling_coeff[j])) {
                *err_reason = "zone coupling_coeff out of range";
                return false;
            }
            if (j == i) {
                if (z->coupling_coeff[j] != 0.0f) {
                    *err_reason = "zone coupling_coeff diagonal must be 0";
                    return false;
                }
                continue;
            }
            if (z->coupling_coeff[j] < 0.0f || z->coupling_coeff[j] > ZONE_COUPLING_COEFF_MAX) {
                *err_reason = "zone coupling_coeff out of range";
                return false;
            }
        }
        /* ZONES_CFG_VERSION 11->12: coupling_tau_s[]/coupling_dead_time_s[],
         * same row shape and same diagonal-must-be-0 rule as coupling_coeff[]
         * just above, bounded by ZONE_MODEL_TIME_MAX_S -- the identical bound
         * the diagonal (self) model_tau_s/model_dead_time_s check uses above
         * in this same function. See coupling_tau_s[]'s own doc comment for
         * why a cross-zone time constant shares its self-zone counterpart's
         * ceiling. */
        for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
            if (!isfinite(z->coupling_tau_s[j]) || !isfinite(z->coupling_dead_time_s[j])) {
                *err_reason = "zone coupling_tau_s/coupling_dead_time_s out of range";
                return false;
            }
            if (j == i) {
                if (z->coupling_tau_s[j] != 0.0f || z->coupling_dead_time_s[j] != 0.0f) {
                    *err_reason = "zone coupling_tau_s/coupling_dead_time_s diagonal must be 0";
                    return false;
                }
                continue;
            }
            if (z->coupling_tau_s[j] < 0.0f || z->coupling_tau_s[j] > ZONE_MODEL_TIME_MAX_S ||
                z->coupling_dead_time_s[j] < 0.0f || z->coupling_dead_time_s[j] > ZONE_MODEL_TIME_MAX_S) {
                *err_reason = "zone coupling_tau_s/coupling_dead_time_s out of range";
                return false;
            }
        }
        /* ZONES_CFG_VERSION 14->15: coupling_diag_k_dc, the diagonal cell of
         * the SAME coupling identification -- see that field's own doc
         * comment for why it is a separate field from model_k_dc/ff_k_dc.
         * No diagonal-must-be-0 rule here (unlike coupling_coeff[]/
         * coupling_tau_s[]/coupling_dead_time_s[] above): this field IS the
         * diagonal, not a row with an unused diagonal cell. Bounded by
         * ZONE_MODEL_K_MAX, the same ceiling model_k_dc uses above in this
         * same function -- same physical quantity, different fit. */
        if (!isfinite(z->coupling_diag_k_dc) || z->coupling_diag_k_dc < 0.0f ||
            z->coupling_diag_k_dc > ZONE_MODEL_K_MAX) {
            *err_reason = "zone coupling_diag_k_dc out of range";
            return false;
        }
        /* ZONES_CFG_VERSION 16->17: same "0 = use the firmware default"
         * sentinel convention the removed global scalar had -- see ZONE_
         * EASE_OFF_WINDOW_MULT_MIN/MAX/DEFAULT's own comment. 0 is explicitly
         * legal here (a fresh/migrated zone's memset(0) default), everything
         * else must fall within [MIN, MAX]. Per-zone now, checked inside this
         * per-zone loop rather than once at the top of this function. */
        if (!isfinite(z->ease_off_window_mult) ||
            (z->ease_off_window_mult != 0.0f &&
             (z->ease_off_window_mult < ZONE_EASE_OFF_WINDOW_MULT_MIN ||
              z->ease_off_window_mult > ZONE_EASE_OFF_WINDOW_MULT_MAX))) {
            *err_reason = "zone ease_off_window_mult out of range";
            return false;
        }
        /* ZONES_CFG_VERSION 17->18 (PID_EXPANSION_PLAN.md sec 3.6d): 0 is
         * "uncapped" -- NOT the same sentinel meaning as ease_off_window_
         * mult's 0 just above (which substitutes a firmware default) -- see
         * ZONE_APPROACH_RATE_CAP_C_PER_HR_MIN's own comment. Everything else
         * must fall within [MIN, MAX]. */
        if (!isfinite(z->approach_rate_cap_c_per_hr) ||
            (z->approach_rate_cap_c_per_hr != 0.0f &&
             (z->approach_rate_cap_c_per_hr < ZONE_APPROACH_RATE_CAP_C_PER_HR_MIN ||
              z->approach_rate_cap_c_per_hr > ZONE_APPROACH_RATE_CAP_C_PER_HR_MAX))) {
            *err_reason = "zone approach_rate_cap_c_per_hr out of range";
            return false;
        }
        /* ZONES_CFG_VERSION 18->19 (PID_EXPANSION_PLAN.md sec 3.6g): same
         * "0 = use the firmware default" sentinel convention as ease_off_
         * window_mult above (NOT approach_rate_cap_c_per_hr's "0 = off with
         * no substitute" convention just above) -- see ZONE_ERROR_BAND_C_
         * MIN/MAX/DEFAULT's own comment. */
        if (!isfinite(z->error_band_c) ||
            (z->error_band_c != 0.0f &&
             (z->error_band_c < ZONE_ERROR_BAND_C_MIN || z->error_band_c > ZONE_ERROR_BAND_C_MAX))) {
            *err_reason = "zone error_band_c out of range";
            return false;
        }
        if (!isfinite(z->rate_band_c_per_s) ||
            (z->rate_band_c_per_s != 0.0f &&
             (z->rate_band_c_per_s < ZONE_RATE_BAND_C_PER_S_MIN ||
              z->rate_band_c_per_s > ZONE_RATE_BAND_C_PER_S_MAX))) {
            *err_reason = "zone rate_band_c_per_s out of range";
            return false;
        }
        /* ZONES_CFG_VERSION 21->22 (docs/audits/consumer_without_producer_
         * 2026-09-06.md finding 1): same "0 = use the firmware default"
         * sentinel convention as error_band_c/rate_band_c_per_s just
         * above -- see ZONE_PROGRESS_BAND_C_MIN/MAX/DEFAULT's own comment. */
        if (!isfinite(z->progress_band_c) ||
            (z->progress_band_c != 0.0f &&
             (z->progress_band_c < ZONE_PROGRESS_BAND_C_MIN || z->progress_band_c > ZONE_PROGRESS_BAND_C_MAX))) {
            *err_reason = "zone progress_band_c out of range";
            return false;
        }
        /* settings_source[group]: either the CUSTOM sentinel, or a real zone
         * index -- never checked against thermo_count (the dropdown offers
         * every *configured* zone at save time, a page-level decision, not a
         * storage-layer one; a zone later disabled by lowering thermo_count
         * still leaves a readable, in-range index here). Checked
         * independently for each of the SRC_GROUP_COUNT groups. */
        for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
            if (z->settings_source[g] != ZONE_SETTINGS_SOURCE_CUSTOM && z->settings_source[g] >= MAX31856_CHANNEL_COUNT) {
                *err_reason = "zone settings_source references a zone that doesn't exist";
                return false;
            }
        }
        /* ZONES_CFG_VERSION 12->13: the tuning-quality record (set 1 -- see
         * zone_cfg_t::tuning_valid's own doc comment). tuning_valid gates
         * every other tuning_* field: when it is false (0), a migrated-old-
         * config or a manually-edited zone reads "unknown", and the values
         * below are not meaningful -- so only bounds-check them (never a
         * cross-field consistency check that would refuse a legitimately
         * zeroed/unknown record) regardless of tuning_valid, same
         * "reject nothing that came off flash as garbage, but don't demand a
         * populated record either" discipline the rest of this loop applies
         * to every other optional field. */
        if (z->tuning_valid > 1) {
            *err_reason = "zone tuning_valid out of range";
            return false;
        }
        if (z->tuning_method > 1) { /* autotune_method_t: 0=STEP, 1=RELAY */
            *err_reason = "zone tuning_method out of range";
            return false;
        }
        if (z->tuning_rule > 3) { /* autotune_rule_t: SIMC/ZN/Tyreus-Luyben/Cohen-Coon, 0-3 */
            *err_reason = "zone tuning_rule out of range";
            return false;
        }
        if (z->tuning_settled > 1 || z->tuning_extrapolation_converged > 1 || z->tuning_tau_consistent > 1) {
            *err_reason = "zone tuning quality flag out of range";
            return false;
        }
        if (!isfinite(z->tuning_baseline_c) || !isfinite(z->tuning_step_ambient_c) ||
            !isfinite(z->tuning_raw_rise_c) || !isfinite(z->tuning_rise_inf_c)) {
            *err_reason = "zone tuning quality temperature out of range";
            return false;
        }
    }
    return true;
}
bool zones_config_json_parse_u8_field(const char *body, const char *key, long min, long max, uint8_t *out)
{
    char val[8];
    int len = http_form_find_field(body, key, val, sizeof(val));
    if (len <= 0) {
        return false;
    }
    char *end = NULL;
    long v = strtol(val, &end, 10);
    /* *end != '\0' catches trailing garbage after a valid numeric prefix
     * (e.g. "1200X" -> strtol happily returns 1200 with end pointing at 'X')
     * -- end == val alone only rejects "no digits at all", not "some digits
     * then junk". Every operator-settable field this function backs is
     * safety-relevant (see this module's header note), so a value that
     * isn't ENTIRELY the number it claims to be must be refused outright,
     * not silently truncated to whatever numeric prefix happened to parse. */
    if (end == val || *end != '\0' || v < min || v > max) {
        return false;
    }
    *out = (uint8_t)v;
    return true;
}

bool zones_config_json_parse_float_field(const char *body, const char *key, float min, float max, float *out)
{
    char val[24];
    int len = http_form_find_field(body, key, val, sizeof(val));
    if (len <= 0) {
        return false;
    }
    char *end = NULL;
    float v = strtof(val, &end);
    /* *end != '\0' -- same trailing-garbage rejection as zones_config_json_parse_u8_field()
     * above; see its comment. NaN is already correctly rejected here, and
     * inf is caught incidentally by the finite min/max bounds -- neither of
     * those is what this check is for. */
    if (end == val || *end != '\0' || isnan(v) || v < min || v > max) {
        return false;
    }
    *out = v;
    return true;
}

/* Parses and validates zone index i's 7 fields from body into *z. thermo_count
 * is the just-parsed candidate count (not yet committed) -- zones at or past
 * it are still parsed (so a round-trip GET/POST of an unused zone block
 * doesn't need special-casing on the page) but not checked against
 * relay_count, since a shrunk relay_count would otherwise reject fields the
 * page never showed for a zone the submission isn't even claiming to use. */
/* Was the key present in the body at all?
 *
 * http_form_find_field() returns >0 for a real value, 0 for a present-but-
 * empty "key=", and -2 when the value is longer than the probe buffer. The
 * four v10 fields below preserve-on-omit, so treating 0 or -2 as "omitted"
 * would silently answer 200 to a blank or over-long value and keep the old
 * number -- exactly the swallow PID_EXPANSION_PLAN.md's Phase 4 forbids
 * ("refuse at the door, never clamp or ignore"). Present-but-unparseable is
 * an error; only a genuinely absent key (-1) is an omission.
 *
 * The older guard/timing fields above still use a bare `> 0` probe. That is
 * pre-existing behavior with its own callers and is deliberately left alone
 * here rather than changed as a side effect of adding these four. */
bool zones_config_json_field_present(const char *body, const char *key)
{
    char probe[8];
    return http_form_find_field(body, key, probe, sizeof(probe)) != -1;
}

bool zones_config_json_parse_timing_profile_fields(const char *body, uint8_t p, zone_timing_profile_t *tp,
                                        const char **err_reason)
{
    char key[24];

    snprintf(key, sizeof(key), "tp%u_name", p);
    char name[TIMING_PROFILE_NAME_MAX_LEN + 1];
    int name_len = http_form_find_field(body, key, name, sizeof(name));
    if (name_len == -2) {
        *err_reason = "timing profile name too long";
        return false;
    }
    if (name_len < 0) {
        /* Caller already confirmed presence via its own probe immediately
         * before calling this -- reaching "absent" here would mean the body
         * changed between those two reads, which cannot happen (both read
         * the same `body` pointer within one request). Kept as an explicit
         * refusal rather than assumed unreachable. */
        *err_reason = "timing profile name missing";
        return false;
    }
    strncpy(tp->name, name, TIMING_PROFILE_NAME_MAX_LEN);
    tp->name[TIMING_PROFILE_NAME_MAX_LEN] = '\0';

    snprintf(key, sizeof(key), "tp%u_progressduty", p);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_DUTY_MAX, &tp->guard_progress_duty_min)) {
        *err_reason = "timing profile guard_progress_duty_min missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "tp%u_progresswindow", p);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_TIME_S_MAX, &tp->guard_progress_window_s)) {
        *err_reason = "timing profile guard_progress_window_s missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "tp%u_drifthyst", p);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_MARGIN_C_MAX, &tp->guard_drift_hysteresis_c)) {
        *err_reason = "timing profile guard_drift_hysteresis_c missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "tp%u_frozeneps", p);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_EPS_C_MAX, &tp->guard_frozen_eps_c)) {
        *err_reason = "timing profile guard_frozen_eps_c missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "tp%u_xzoneperiod", p);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_TIME_S_MAX, &tp->guard_cross_zone_period_s)) {
        *err_reason = "timing profile guard_cross_zone_period_s missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "tp%u_bbhyst", p);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_MARGIN_C_MAX, &tp->bangbang_hysteresis_c)) {
        *err_reason = "timing profile bangbang_hysteresis_c missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "tp%u_coolmargin", p);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_MARGIN_C_MAX, &tp->cooling_limited_margin_c)) {
        *err_reason = "timing profile cooling_limited_margin_c missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "tp%u_coolhold", p);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_TIME_S_MAX, &tp->cooling_limited_hold_s)) {
        *err_reason = "timing profile cooling_limited_hold_s missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "tp%u_ramplock", p);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_MARGIN_C_MAX, &tp->ramp_lock_band_c)) {
        *err_reason = "timing profile ramp_lock_band_c missing or out of range";
        return false;
    }
    return true;
}
