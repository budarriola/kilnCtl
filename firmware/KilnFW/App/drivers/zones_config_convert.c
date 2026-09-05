// Per-historical-layout converters for zones_config_json.c's split (ROADMAP.md
// M15, the 1500-line rule) -- see zones_config_json_internal.h's own doc
// comment for the full three-file map. Every function here is called only
// from zones_config_migrate.c's convert_versioned_blob_to_current(); nothing
// in this file has a public entry point of its own.
#include "zones_config_json_internal.h"

#include <string.h>


size_t zones_cfg_expected_len_for_version(uint8_t version)
{
    switch (version) {
    case 1: return sizeof(zones_cfg_v1_t);
    case 2: return sizeof(zones_cfg_v2_t);
    case 3: return sizeof(zones_cfg_v3_t);
    case 4: return sizeof(zones_cfg_v4_t);
    case 5: return sizeof(zones_cfg_v5_t);
    case 6: return sizeof(zones_cfg_v6_t);
    case 7: return sizeof(zones_cfg_v7_t);
    /* NEVER sizeof(zones_cfg_t) here -- that is the CURRENT (v11) layout.
     * zones_cfg_v8_t/zones_cfg_v9_t/zones_cfg_v10_t are separate, frozen
     * snapshots of what v8/v9/v10 actually looked like; see ZONES_CFG_
     * VERSION's 8->9 comment's WARNING for the sibling profiles_http.c bug
     * that returning the current struct's size for an old version caused
     * (every profile on the owner's board rejected and marked unused, found
     * only by a hardware flash). */
    case 8: return sizeof(zones_cfg_v8_t);
    case 9: return sizeof(zones_cfg_v9_t);
    case 10: return sizeof(zones_cfg_v10_t);
    case 11: return sizeof(zones_cfg_v11_t);
    case 12: return sizeof(zones_cfg_v12_t);
    case 13: return sizeof(zones_cfg_v13_t);
    case 14: return sizeof(zones_cfg_v14_t);
    case 15: return sizeof(zones_cfg_v15_t);
    case 16: return sizeof(zones_cfg_v16_t);
    case 17: return sizeof(zones_cfg_v17_t);
    case 18: return sizeof(zones_cfg_v18_t);
    case ZONES_CFG_VERSION: return sizeof(zones_cfg_t);
    default: return 0;
    }
}

/* Field-by-field converters, one per historical zone_cfg_t layout, each
 * writing every field of a fresh CURRENT-format zone_cfg_t explicitly --
 * never a memcpy of one shape over another. Fields the source layout does
 * not have get the same safe fill-in migrate_zones_cfg_v1_to_current() used
 * to apply (see ZONES_CFG_VERSION's own comment for why each one is safe):
 * thermo_mask defaults to the legacy "zone i <-> channel i" mapping
 * (1u << chan_idx), tc_type/safety_tc_type default to THERMO_TC_K (every
 * board's actual hardware register was always hardcoded to K until 4->5), and
 * ct_mask/the 8 guard thresholds default to 0, which is already the correct
 * "not configured" meaning for every one of them. */
void convert_zone_v1(const zone_cfg_v1_t *s, zone_cfg_t *d, uint8_t chan_idx)
{
    memset(d, 0, sizeof(*d));
    memcpy(d->name, s->name, sizeof(d->name));
    d->relay_mask = s->relay_mask;
    d->cal_offset_c = s->cal_offset_c;
    d->pid_kp = s->pid_kp;
    d->pid_ki = s->pid_ki;
    d->pid_kd = s->pid_kd;
    d->max_ramp_c_per_hr = s->max_ramp_c_per_hr;
    d->sanity_rate_c_per_min = s->sanity_rate_c_per_min;
    d->control_mode = s->control_mode;
    d->max_temp_c = s->max_temp_c;
    d->min_temp_c = s->min_temp_c;
    d->heater_window_ms = s->heater_window_ms;
    d->heater_min_on_ms = s->heater_min_on_ms;
    d->heater_min_off_ms = s->heater_min_off_ms;
    d->cross_zone_max_delta_c = s->cross_zone_max_delta_c;
    d->model_k_dc = s->model_k_dc;
    d->model_tau_s = s->model_tau_s;
    d->model_dead_time_s = s->model_dead_time_s;
    /* guard thresholds: predate this layout -- 0 is the documented
     * "substitute the firmware default" meaning, not a rejection. */
    d->thermo_mask = (uint8_t)(1u << chan_idx);
    d->tc_type = THERMO_TC_K;
}

void convert_zone_v3(const zone_cfg_v3_t *s, zone_cfg_t *d, uint8_t chan_idx)
{
    memset(d, 0, sizeof(*d));
    memcpy(d->name, s->name, sizeof(d->name));
    d->relay_mask = s->relay_mask;
    d->cal_offset_c = s->cal_offset_c;
    d->pid_kp = s->pid_kp;
    d->pid_ki = s->pid_ki;
    d->pid_kd = s->pid_kd;
    d->max_ramp_c_per_hr = s->max_ramp_c_per_hr;
    d->sanity_rate_c_per_min = s->sanity_rate_c_per_min;
    d->control_mode = s->control_mode;
    d->max_temp_c = s->max_temp_c;
    d->min_temp_c = s->min_temp_c;
    d->heater_window_ms = s->heater_window_ms;
    d->heater_min_on_ms = s->heater_min_on_ms;
    d->heater_min_off_ms = s->heater_min_off_ms;
    d->guard_wrong_dir_window_s = s->guard_wrong_dir_window_s;
    d->guard_wrong_dir_rate_c_per_min = s->guard_wrong_dir_rate_c_per_min;
    d->guard_off_settle_s = s->guard_off_settle_s;
    d->guard_runaway_rate_c_per_min = s->guard_runaway_rate_c_per_min;
    d->guard_runaway_margin_c = s->guard_runaway_margin_c;
    d->guard_drift_period_s = s->guard_drift_period_s;
    d->guard_sensor_fault_debounce_ticks = s->guard_sensor_fault_debounce_ticks;
    d->guard_frozen_window_s = s->guard_frozen_window_s;
    d->cross_zone_max_delta_c = s->cross_zone_max_delta_c;
    d->model_k_dc = s->model_k_dc;
    d->model_tau_s = s->model_tau_s;
    d->model_dead_time_s = s->model_dead_time_s;
    d->thermo_mask = (uint8_t)(1u << chan_idx);
    d->tc_type = THERMO_TC_K;
}

void convert_zone_v4(const zone_cfg_v4_t *s, zone_cfg_t *d)
{
    memset(d, 0, sizeof(*d));
    memcpy(d->name, s->name, sizeof(d->name));
    d->relay_mask = s->relay_mask;
    d->cal_offset_c = s->cal_offset_c;
    d->pid_kp = s->pid_kp;
    d->pid_ki = s->pid_ki;
    d->pid_kd = s->pid_kd;
    d->max_ramp_c_per_hr = s->max_ramp_c_per_hr;
    d->sanity_rate_c_per_min = s->sanity_rate_c_per_min;
    d->control_mode = s->control_mode;
    d->max_temp_c = s->max_temp_c;
    d->min_temp_c = s->min_temp_c;
    d->heater_window_ms = s->heater_window_ms;
    d->heater_min_on_ms = s->heater_min_on_ms;
    d->heater_min_off_ms = s->heater_min_off_ms;
    d->guard_wrong_dir_window_s = s->guard_wrong_dir_window_s;
    d->guard_wrong_dir_rate_c_per_min = s->guard_wrong_dir_rate_c_per_min;
    d->guard_off_settle_s = s->guard_off_settle_s;
    d->guard_runaway_rate_c_per_min = s->guard_runaway_rate_c_per_min;
    d->guard_runaway_margin_c = s->guard_runaway_margin_c;
    d->guard_drift_period_s = s->guard_drift_period_s;
    d->guard_sensor_fault_debounce_ticks = s->guard_sensor_fault_debounce_ticks;
    d->guard_frozen_window_s = s->guard_frozen_window_s;
    d->cross_zone_max_delta_c = s->cross_zone_max_delta_c;
    d->model_k_dc = s->model_k_dc;
    d->model_tau_s = s->model_tau_s;
    d->model_dead_time_s = s->model_dead_time_s;
    d->thermo_mask = s->thermo_mask; /* real, operator-set value */
    d->tc_type = THERMO_TC_K;        /* predates this field */
}

void convert_zone_v5(const zone_cfg_v5_t *s, zone_cfg_t *d)
{
    memset(d, 0, sizeof(*d));
    memcpy(d->name, s->name, sizeof(d->name));
    d->relay_mask = s->relay_mask;
    d->cal_offset_c = s->cal_offset_c;
    d->pid_kp = s->pid_kp;
    d->pid_ki = s->pid_ki;
    d->pid_kd = s->pid_kd;
    d->max_ramp_c_per_hr = s->max_ramp_c_per_hr;
    d->sanity_rate_c_per_min = s->sanity_rate_c_per_min;
    d->control_mode = s->control_mode;
    d->max_temp_c = s->max_temp_c;
    d->min_temp_c = s->min_temp_c;
    d->heater_window_ms = s->heater_window_ms;
    d->heater_min_on_ms = s->heater_min_on_ms;
    d->heater_min_off_ms = s->heater_min_off_ms;
    d->guard_wrong_dir_window_s = s->guard_wrong_dir_window_s;
    d->guard_wrong_dir_rate_c_per_min = s->guard_wrong_dir_rate_c_per_min;
    d->guard_off_settle_s = s->guard_off_settle_s;
    d->guard_runaway_rate_c_per_min = s->guard_runaway_rate_c_per_min;
    d->guard_runaway_margin_c = s->guard_runaway_margin_c;
    d->guard_drift_period_s = s->guard_drift_period_s;
    d->guard_sensor_fault_debounce_ticks = s->guard_sensor_fault_debounce_ticks;
    d->guard_frozen_window_s = s->guard_frozen_window_s;
    d->cross_zone_max_delta_c = s->cross_zone_max_delta_c;
    d->tc_type = s->tc_type; /* real, operator-set value */
    d->model_k_dc = s->model_k_dc;
    d->model_tau_s = s->model_tau_s;
    d->model_dead_time_s = s->model_dead_time_s;
    d->thermo_mask = s->thermo_mask; /* real, operator-set value */
}

/* Dispatches to the right typed converter for `version`, filling `out` (a
 * fresh CURRENT-format zones_cfg_t) field by field -- never a memcpy of one
 * struct shape over another. Caller (zones_config_json_decode_blob()) has already
 * checked `len` against zones_cfg_expected_len_for_version(version), so the memcpy of
 * `blob` into each local, exactly-sized historical struct below is safe. */
/* v6/v7 -> current. Every field the old layout had is copied by name; the nine
 * v8 additions get 0, which is already their "not configured, use the firmware
 * default" meaning, so a board upgrading from v7 behaves exactly as it did. */
void convert_zone_v7(const zone_cfg_v7_t *s, zone_cfg_t *d)
{
    memset(d, 0, sizeof(*d));
    memcpy(d->name, s->name, sizeof(d->name));
    d->relay_mask = s->relay_mask;
    d->cal_offset_c = s->cal_offset_c;
    d->pid_kp = s->pid_kp;
    d->pid_ki = s->pid_ki;
    d->pid_kd = s->pid_kd;
    d->max_ramp_c_per_hr = s->max_ramp_c_per_hr;
    d->sanity_rate_c_per_min = s->sanity_rate_c_per_min;
    d->control_mode = s->control_mode;
    d->max_temp_c = s->max_temp_c;
    d->min_temp_c = s->min_temp_c;
    d->heater_window_ms = s->heater_window_ms;
    d->heater_min_on_ms = s->heater_min_on_ms;
    d->heater_min_off_ms = s->heater_min_off_ms;
    d->guard_wrong_dir_window_s = s->guard_wrong_dir_window_s;
    d->guard_wrong_dir_rate_c_per_min = s->guard_wrong_dir_rate_c_per_min;
    d->guard_off_settle_s = s->guard_off_settle_s;
    d->guard_runaway_rate_c_per_min = s->guard_runaway_rate_c_per_min;
    d->guard_runaway_margin_c = s->guard_runaway_margin_c;
    d->guard_drift_period_s = s->guard_drift_period_s;
    d->guard_sensor_fault_debounce_ticks = s->guard_sensor_fault_debounce_ticks;
    d->guard_frozen_window_s = s->guard_frozen_window_s;
    d->cross_zone_max_delta_c = s->cross_zone_max_delta_c;
    d->tc_type = s->tc_type;
    d->model_k_dc = s->model_k_dc;
    d->model_tau_s = s->model_tau_s;
    d->model_dead_time_s = s->model_dead_time_s;
    d->thermo_mask = s->thermo_mask;
    d->ct_mask = s->ct_mask;
}

/* v8 -> current. Copies every field EXCEPT the nine timing overrides, which
 * moved off zone_cfg_t entirely -- the caller (convert_versioned_blob_to_
 * current()'s case 8) is responsible for setting d->timing_profile after this
 * returns, once it has decided (via dedup against the profiles already
 * allocated for earlier zones) which profile this zone's nine v8 values map
 * to. Deliberately does NOT touch d->timing_profile itself, unlike every
 * other convert_zone_v*() which fully owns its output zone -- this one field
 * is a whole-blob decision, not a per-zone one. */
void convert_zone_v8(const zone_cfg_v8_t *s, zone_cfg_t *d)
{
    memset(d, 0, sizeof(*d));
    memcpy(d->name, s->name, sizeof(d->name));
    d->relay_mask = s->relay_mask;
    d->cal_offset_c = s->cal_offset_c;
    d->pid_kp = s->pid_kp;
    d->pid_ki = s->pid_ki;
    d->pid_kd = s->pid_kd;
    d->max_ramp_c_per_hr = s->max_ramp_c_per_hr;
    d->sanity_rate_c_per_min = s->sanity_rate_c_per_min;
    d->control_mode = s->control_mode;
    d->max_temp_c = s->max_temp_c;
    d->min_temp_c = s->min_temp_c;
    d->heater_window_ms = s->heater_window_ms;
    d->heater_min_on_ms = s->heater_min_on_ms;
    d->heater_min_off_ms = s->heater_min_off_ms;
    d->guard_wrong_dir_window_s = s->guard_wrong_dir_window_s;
    d->guard_wrong_dir_rate_c_per_min = s->guard_wrong_dir_rate_c_per_min;
    d->guard_off_settle_s = s->guard_off_settle_s;
    d->guard_runaway_rate_c_per_min = s->guard_runaway_rate_c_per_min;
    d->guard_runaway_margin_c = s->guard_runaway_margin_c;
    d->guard_drift_period_s = s->guard_drift_period_s;
    d->guard_sensor_fault_debounce_ticks = s->guard_sensor_fault_debounce_ticks;
    d->guard_frozen_window_s = s->guard_frozen_window_s;
    d->cross_zone_max_delta_c = s->cross_zone_max_delta_c;
    d->tc_type = s->tc_type;
    d->model_k_dc = s->model_k_dc;
    d->model_tau_s = s->model_tau_s;
    d->model_dead_time_s = s->model_dead_time_s;
    d->thermo_mask = s->thermo_mask;
    d->ct_mask = s->ct_mask;
}

/* v9 -> current (v10). Copies every field a v9 zone had, unchanged, by name --
 * v9 already carries timing_profile, so unlike convert_zone_v8() this one
 * needs no help from its caller deciding which profile to point at. THE
 * field this function exists to get right: d->settings_source. d starts
 * zeroed by the memset() below, and 0 is a real, DIFFERENT, WRONG value for
 * this one field (see ZONES_CFG_VERSION's 9->10 comment and
 * zone_cfg_t::settings_source's own comment for why) -- every other new field
 * (the three floats) is correctly served by the zero the memset already
 * leaves, but settings_source is explicitly set to ZONE_SETTINGS_SOURCE_CUSTOM
 * here rather than trusted to fall out of zero-initialization, so a future
 * edit to this function's field list cannot silently reintroduce the "every
 * zone claims to copy zone 0" bug by omission. */
void convert_zone_v9(const zone_cfg_v9_t *s, zone_cfg_t *d)
{
    memset(d, 0, sizeof(*d));
    memcpy(d->name, s->name, sizeof(d->name));
    d->relay_mask = s->relay_mask;
    d->cal_offset_c = s->cal_offset_c;
    d->pid_kp = s->pid_kp;
    d->pid_ki = s->pid_ki;
    d->pid_kd = s->pid_kd;
    d->max_ramp_c_per_hr = s->max_ramp_c_per_hr;
    d->sanity_rate_c_per_min = s->sanity_rate_c_per_min;
    d->control_mode = s->control_mode;
    d->max_temp_c = s->max_temp_c;
    d->min_temp_c = s->min_temp_c;
    d->heater_window_ms = s->heater_window_ms;
    d->heater_min_on_ms = s->heater_min_on_ms;
    d->heater_min_off_ms = s->heater_min_off_ms;
    d->guard_wrong_dir_window_s = s->guard_wrong_dir_window_s;
    d->guard_wrong_dir_rate_c_per_min = s->guard_wrong_dir_rate_c_per_min;
    d->guard_off_settle_s = s->guard_off_settle_s;
    d->guard_runaway_rate_c_per_min = s->guard_runaway_rate_c_per_min;
    d->guard_runaway_margin_c = s->guard_runaway_margin_c;
    d->guard_drift_period_s = s->guard_drift_period_s;
    d->guard_sensor_fault_debounce_ticks = s->guard_sensor_fault_debounce_ticks;
    d->guard_frozen_window_s = s->guard_frozen_window_s;
    d->cross_zone_max_delta_c = s->cross_zone_max_delta_c;
    d->tc_type = s->tc_type;
    d->model_k_dc = s->model_k_dc;
    d->model_tau_s = s->model_tau_s;
    d->model_dead_time_s = s->model_dead_time_s;
    d->thermo_mask = s->thermo_mask;
    d->ct_mask = s->ct_mask;
    d->timing_profile = s->timing_profile;
    /* fuzzy_strength_pct/coupling_coeff[]: not touched -- the memset above
     * already left them at 0, which is each field's own documented "not
     * configured" default (see zone_cfg_t's comment). v9 predates coupling
     * entirely (not even the single-pair v10 shape), so there is nothing to
     * map into any row cell here -- unlike convert_zone_v10() below. */
    d->settings_source = ZONE_SETTINGS_SOURCE_CUSTOM; /* NEVER 0 -- see this function's own comment */
}

/* v10 -> v11: field-for-field carry-through, same shape as convert_zone_v9()
 * above, PLUS the one real migration this bump exists for -- folding v10's
 * single (coupling_coeff, coupling_neighbor_zone) pair into the right cell
 * of the new row. If a v10 zone had 0 coupling_coeff (never measured, the
 * default), the destination row is left all-zero by the memset below, same
 * "no coupling measured" meaning either way. A v10 board never had
 * coupling_neighbor_zone == its own zone index (parse_zone_fields()/
 * zones_config_set_coupling() never allowed self-reference before this
 * pass either, since a zone was never its own neighbor), so the target cell
 * is always off-diagonal and there is no ambiguity with the diagonal-must-
 * stay-zero rule. */
void convert_zone_v10(const zone_cfg_v10_t *s, zone_cfg_t *d, uint8_t chan_idx)
{
    memset(d, 0, sizeof(*d));
    memcpy(d->name, s->name, sizeof(d->name));
    d->relay_mask = s->relay_mask;
    d->cal_offset_c = s->cal_offset_c;
    d->pid_kp = s->pid_kp;
    d->pid_ki = s->pid_ki;
    d->pid_kd = s->pid_kd;
    d->max_ramp_c_per_hr = s->max_ramp_c_per_hr;
    d->sanity_rate_c_per_min = s->sanity_rate_c_per_min;
    d->control_mode = s->control_mode;
    d->max_temp_c = s->max_temp_c;
    d->min_temp_c = s->min_temp_c;
    d->heater_window_ms = s->heater_window_ms;
    d->heater_min_on_ms = s->heater_min_on_ms;
    d->heater_min_off_ms = s->heater_min_off_ms;
    d->guard_wrong_dir_window_s = s->guard_wrong_dir_window_s;
    d->guard_wrong_dir_rate_c_per_min = s->guard_wrong_dir_rate_c_per_min;
    d->guard_off_settle_s = s->guard_off_settle_s;
    d->guard_runaway_rate_c_per_min = s->guard_runaway_rate_c_per_min;
    d->guard_runaway_margin_c = s->guard_runaway_margin_c;
    d->guard_drift_period_s = s->guard_drift_period_s;
    d->guard_sensor_fault_debounce_ticks = s->guard_sensor_fault_debounce_ticks;
    d->guard_frozen_window_s = s->guard_frozen_window_s;
    d->cross_zone_max_delta_c = s->cross_zone_max_delta_c;
    d->tc_type = s->tc_type;
    d->model_k_dc = s->model_k_dc;
    d->model_tau_s = s->model_tau_s;
    d->model_dead_time_s = s->model_dead_time_s;
    d->thermo_mask = s->thermo_mask;
    d->ct_mask = s->ct_mask;
    d->timing_profile = s->timing_profile;
    d->fuzzy_strength_pct = s->fuzzy_strength_pct;
    d->settings_source = s->settings_source; /* v10 already has this field for real -- unlike
                                               * convert_zone_v9(), never forced to the sentinel */
    /* THE migration this bump exists for -- see this function's own header
     * comment. s->coupling_neighbor_zone is validated (by v10's own setter/
     * parser, which predates this pass but enforced the identical bound) to
     * be a whole number in 0..MAX31856_CHANNEL_COUNT-1, so the cast below
     * never truncates a real fraction away. */
    if (s->coupling_coeff != 0.0f) {
        uint8_t neighbor = (uint8_t)s->coupling_neighbor_zone;
        /* Guard against a self-referencing pair, same as convert_zone_v1()'s
         * chan_idx check: a v10 blob that (however it got there) recorded
         * coupling_neighbor_zone == its own zone index would otherwise write
         * a nonzero diagonal cell here. zones_config_json_validate() rejects any
         * nonzero diagonal, and zones_config_json_decode_blob() then returns CORRUPT for
         * the whole migrated struct -- discarding the entire commissioned
         * config over one stray self-reference. Unreachable today (no board
         * has ever held a v10 blob with valid firmware writing it), but
         * costs nothing to close off. */
        if (neighbor < MAX31856_CHANNEL_COUNT && neighbor != chan_idx) {
            d->coupling_coeff[neighbor] = s->coupling_coeff;
        }
    }
}

/* v11 -> v12 (this pass): field-for-field carry-through, same shape as
 * convert_zone_v10() above -- v11 already has coupling_coeff[] in its
 * CURRENT (row) shape, so there is no cell-folding to do here. The only
 * real thing this migration does is leave coupling_tau_s[]/
 * coupling_dead_time_s[] at 0 ("not measured") via the memset below -- a v11
 * blob never stored either array, so there is nothing to carry into them;
 * see ZONES_CFG_VERSION's 11->12 comment and coupling_tau_s[]'s own doc
 * comment for why 0 is exactly the right "not measured" value here, not a
 * placeholder that needs a later fixup. */
void convert_zone_v11(const zone_cfg_v11_t *s, zone_cfg_t *d)
{
    memset(d, 0, sizeof(*d));
    memcpy(d->name, s->name, sizeof(d->name));
    d->relay_mask = s->relay_mask;
    d->cal_offset_c = s->cal_offset_c;
    d->pid_kp = s->pid_kp;
    d->pid_ki = s->pid_ki;
    d->pid_kd = s->pid_kd;
    d->max_ramp_c_per_hr = s->max_ramp_c_per_hr;
    d->sanity_rate_c_per_min = s->sanity_rate_c_per_min;
    d->control_mode = s->control_mode;
    d->max_temp_c = s->max_temp_c;
    d->min_temp_c = s->min_temp_c;
    d->heater_window_ms = s->heater_window_ms;
    d->heater_min_on_ms = s->heater_min_on_ms;
    d->heater_min_off_ms = s->heater_min_off_ms;
    d->guard_wrong_dir_window_s = s->guard_wrong_dir_window_s;
    d->guard_wrong_dir_rate_c_per_min = s->guard_wrong_dir_rate_c_per_min;
    d->guard_off_settle_s = s->guard_off_settle_s;
    d->guard_runaway_rate_c_per_min = s->guard_runaway_rate_c_per_min;
    d->guard_runaway_margin_c = s->guard_runaway_margin_c;
    d->guard_drift_period_s = s->guard_drift_period_s;
    d->guard_sensor_fault_debounce_ticks = s->guard_sensor_fault_debounce_ticks;
    d->guard_frozen_window_s = s->guard_frozen_window_s;
    d->cross_zone_max_delta_c = s->cross_zone_max_delta_c;
    d->tc_type = s->tc_type;
    d->model_k_dc = s->model_k_dc;
    d->model_tau_s = s->model_tau_s;
    d->model_dead_time_s = s->model_dead_time_s;
    d->thermo_mask = s->thermo_mask;
    d->ct_mask = s->ct_mask;
    d->timing_profile = s->timing_profile;
    d->fuzzy_strength_pct = s->fuzzy_strength_pct;
    memcpy(d->coupling_coeff, s->coupling_coeff, sizeof(d->coupling_coeff));
    /* coupling_tau_s[]/coupling_dead_time_s[]: not touched -- the memset
     * above already left them at 0, same "not touched" pattern
     * convert_zone_v9()'s own comment uses for fuzzy_strength_pct/
     * coupling_coeff[] there. */
    d->settings_source = s->settings_source;
    /* tuning_*: not touched -- the memset above already left tuning_valid at
     * 0 ("unknown"), same convention. A v11 blob never stored a tuning
     * quality record, so there is nothing to carry into it -- see
     * ZONES_CFG_VERSION's 12->13 comment (zone_cfg_t) for why 0/unknown here
     * is exactly right rather than a placeholder needing a later fixup. */
}

/* v12 -> v13 (this pass): field-for-field carry-through, same shape as
 * convert_zone_v11() above -- v12 already has coupling_tau_s[]/
 * coupling_dead_time_s[] in their CURRENT shape, so there is no folding to
 * do here either. The only real migration is convert_zone_v12() leaving
 * every tuning_* field at its zeroed "unknown" default via the memset below
 * -- a v12 blob never stored a tuning quality record at all, so there is
 * nothing to carry into it; see ZONES_CFG_VERSION's 12->13 comment
 * (zone_cfg_t) for why 0/tuning_valid==0 is exactly the right "unknown"
 * value here, not a placeholder that needs a later fixup, and specifically
 * why it must NOT read as a real (if zero-valued) measurement. */
void convert_zone_v12(const zone_cfg_v12_t *s, zone_cfg_t *d)
{
    memset(d, 0, sizeof(*d));
    memcpy(d->name, s->name, sizeof(d->name));
    d->relay_mask = s->relay_mask;
    d->cal_offset_c = s->cal_offset_c;
    d->pid_kp = s->pid_kp;
    d->pid_ki = s->pid_ki;
    d->pid_kd = s->pid_kd;
    d->max_ramp_c_per_hr = s->max_ramp_c_per_hr;
    d->sanity_rate_c_per_min = s->sanity_rate_c_per_min;
    d->control_mode = s->control_mode;
    d->max_temp_c = s->max_temp_c;
    d->min_temp_c = s->min_temp_c;
    d->heater_window_ms = s->heater_window_ms;
    d->heater_min_on_ms = s->heater_min_on_ms;
    d->heater_min_off_ms = s->heater_min_off_ms;
    d->guard_wrong_dir_window_s = s->guard_wrong_dir_window_s;
    d->guard_wrong_dir_rate_c_per_min = s->guard_wrong_dir_rate_c_per_min;
    d->guard_off_settle_s = s->guard_off_settle_s;
    d->guard_runaway_rate_c_per_min = s->guard_runaway_rate_c_per_min;
    d->guard_runaway_margin_c = s->guard_runaway_margin_c;
    d->guard_drift_period_s = s->guard_drift_period_s;
    d->guard_sensor_fault_debounce_ticks = s->guard_sensor_fault_debounce_ticks;
    d->guard_frozen_window_s = s->guard_frozen_window_s;
    d->cross_zone_max_delta_c = s->cross_zone_max_delta_c;
    d->tc_type = s->tc_type;
    d->model_k_dc = s->model_k_dc;
    d->model_tau_s = s->model_tau_s;
    d->model_dead_time_s = s->model_dead_time_s;
    d->thermo_mask = s->thermo_mask;
    d->ct_mask = s->ct_mask;
    d->timing_profile = s->timing_profile;
    d->fuzzy_strength_pct = s->fuzzy_strength_pct;
    memcpy(d->coupling_coeff, s->coupling_coeff, sizeof(d->coupling_coeff));
    memcpy(d->coupling_tau_s, s->coupling_tau_s, sizeof(d->coupling_tau_s));
    memcpy(d->coupling_dead_time_s, s->coupling_dead_time_s, sizeof(d->coupling_dead_time_s));
    d->settings_source = s->settings_source;
    /* tuning_*: not touched -- the memset above already left tuning_valid at
     * 0 ("unknown"). */
}

/* v13 -> v14 (this pass): field-for-field carry-through, same shape as
 * convert_zone_v12() above, but this time EVERY field v13 has -- including
 * the tuning_* quality record -- carries a real value, since v13 already
 * stored all of it. The only field this cannot carry is adaptive_tune_
 * enabled, which v13 never stored at all; the memset in convert_versioned_
 * blob_to_current() already left it at its "opted out" 0 default, which is
 * exactly right for a blob-format migration. The REAL migration of an
 * upgrading board's actual opt-in choice happens separately, once, at boot,
 * from the OLD 'adap_tune' NVS namespace -- see zone_cfg_t::adaptive_tune_
 * enabled's own comment for why that cannot live here. */
void convert_zone_v13(const zone_cfg_v13_t *s, zone_cfg_t *d)
{
    memset(d, 0, sizeof(*d));
    memcpy(d->name, s->name, sizeof(d->name));
    d->relay_mask = s->relay_mask;
    d->cal_offset_c = s->cal_offset_c;
    d->pid_kp = s->pid_kp;
    d->pid_ki = s->pid_ki;
    d->pid_kd = s->pid_kd;
    d->max_ramp_c_per_hr = s->max_ramp_c_per_hr;
    d->sanity_rate_c_per_min = s->sanity_rate_c_per_min;
    d->control_mode = s->control_mode;
    d->max_temp_c = s->max_temp_c;
    d->min_temp_c = s->min_temp_c;
    d->heater_window_ms = s->heater_window_ms;
    d->heater_min_on_ms = s->heater_min_on_ms;
    d->heater_min_off_ms = s->heater_min_off_ms;
    d->guard_wrong_dir_window_s = s->guard_wrong_dir_window_s;
    d->guard_wrong_dir_rate_c_per_min = s->guard_wrong_dir_rate_c_per_min;
    d->guard_off_settle_s = s->guard_off_settle_s;
    d->guard_runaway_rate_c_per_min = s->guard_runaway_rate_c_per_min;
    d->guard_runaway_margin_c = s->guard_runaway_margin_c;
    d->guard_drift_period_s = s->guard_drift_period_s;
    d->guard_sensor_fault_debounce_ticks = s->guard_sensor_fault_debounce_ticks;
    d->guard_frozen_window_s = s->guard_frozen_window_s;
    d->cross_zone_max_delta_c = s->cross_zone_max_delta_c;
    d->tc_type = s->tc_type;
    d->model_k_dc = s->model_k_dc;
    d->model_tau_s = s->model_tau_s;
    d->model_dead_time_s = s->model_dead_time_s;
    d->thermo_mask = s->thermo_mask;
    d->ct_mask = s->ct_mask;
    d->timing_profile = s->timing_profile;
    d->fuzzy_strength_pct = s->fuzzy_strength_pct;
    memcpy(d->coupling_coeff, s->coupling_coeff, sizeof(d->coupling_coeff));
    memcpy(d->coupling_tau_s, s->coupling_tau_s, sizeof(d->coupling_tau_s));
    memcpy(d->coupling_dead_time_s, s->coupling_dead_time_s, sizeof(d->coupling_dead_time_s));
    d->settings_source = s->settings_source;
    d->tuning_valid = s->tuning_valid;
    d->tuning_method = s->tuning_method;
    d->tuning_rule = s->tuning_rule;
    d->tuning_settled = s->tuning_settled;
    d->tuning_extrapolation_converged = s->tuning_extrapolation_converged;
    d->tuning_tau_consistent = s->tuning_tau_consistent;
    d->tuning_baseline_c = s->tuning_baseline_c;
    d->tuning_step_ambient_c = s->tuning_step_ambient_c;
    d->tuning_raw_rise_c = s->tuning_raw_rise_c;
    d->tuning_rise_inf_c = s->tuning_rise_inf_c;
    d->tuning_seq = s->tuning_seq;
    /* adaptive_tune_enabled: not touched -- the memset above already left it
     * at 0 ("opted out"), v13's documented default for a field it never
     * had. See this function's own header comment for where the operator's
     * REAL prior choice actually gets carried forward from. */
}

/* v14 -> current (ZONES_CFG_VERSION 14->15, this pass): field-for-field
 * carry-through of everything v14 has, including adaptive_tune_enabled --
 * the only field this cannot carry is coupling_diag_k_dc, which v14 never
 * stored at all; the memset below already leaves it at its "not measured"
 * 0 default, which is exactly right for a blob-format migration (see
 * zone_cfg_t::coupling_diag_k_dc's own comment: 0 = "not measured by the
 * coupling identification", identical to coupling_coeff[]'s convention).
 * There is no separate real-value carry-forward step the way adaptive_tune_
 * enabled needed -- this field was never stored anywhere else on an
 * upgrading board, unlike that flag's old 'adap_tune' NVS namespace. */
void convert_zone_v14(const zone_cfg_v14_t *s, zone_cfg_t *d)
{
    memset(d, 0, sizeof(*d));
    memcpy(d->name, s->name, sizeof(d->name));
    d->relay_mask = s->relay_mask;
    d->cal_offset_c = s->cal_offset_c;
    d->pid_kp = s->pid_kp;
    d->pid_ki = s->pid_ki;
    d->pid_kd = s->pid_kd;
    d->max_ramp_c_per_hr = s->max_ramp_c_per_hr;
    d->sanity_rate_c_per_min = s->sanity_rate_c_per_min;
    d->control_mode = s->control_mode;
    d->max_temp_c = s->max_temp_c;
    d->min_temp_c = s->min_temp_c;
    d->heater_window_ms = s->heater_window_ms;
    d->heater_min_on_ms = s->heater_min_on_ms;
    d->heater_min_off_ms = s->heater_min_off_ms;
    d->guard_wrong_dir_window_s = s->guard_wrong_dir_window_s;
    d->guard_wrong_dir_rate_c_per_min = s->guard_wrong_dir_rate_c_per_min;
    d->guard_off_settle_s = s->guard_off_settle_s;
    d->guard_runaway_rate_c_per_min = s->guard_runaway_rate_c_per_min;
    d->guard_runaway_margin_c = s->guard_runaway_margin_c;
    d->guard_drift_period_s = s->guard_drift_period_s;
    d->guard_sensor_fault_debounce_ticks = s->guard_sensor_fault_debounce_ticks;
    d->guard_frozen_window_s = s->guard_frozen_window_s;
    d->cross_zone_max_delta_c = s->cross_zone_max_delta_c;
    d->tc_type = s->tc_type;
    d->model_k_dc = s->model_k_dc;
    d->model_tau_s = s->model_tau_s;
    d->model_dead_time_s = s->model_dead_time_s;
    d->thermo_mask = s->thermo_mask;
    d->ct_mask = s->ct_mask;
    d->timing_profile = s->timing_profile;
    d->fuzzy_strength_pct = s->fuzzy_strength_pct;
    memcpy(d->coupling_coeff, s->coupling_coeff, sizeof(d->coupling_coeff));
    memcpy(d->coupling_tau_s, s->coupling_tau_s, sizeof(d->coupling_tau_s));
    memcpy(d->coupling_dead_time_s, s->coupling_dead_time_s, sizeof(d->coupling_dead_time_s));
    d->settings_source = s->settings_source;
    d->tuning_valid = s->tuning_valid;
    d->tuning_method = s->tuning_method;
    d->tuning_rule = s->tuning_rule;
    d->tuning_settled = s->tuning_settled;
    d->tuning_extrapolation_converged = s->tuning_extrapolation_converged;
    d->tuning_tau_consistent = s->tuning_tau_consistent;
    d->tuning_baseline_c = s->tuning_baseline_c;
    d->tuning_step_ambient_c = s->tuning_step_ambient_c;
    d->tuning_raw_rise_c = s->tuning_raw_rise_c;
    d->tuning_rise_inf_c = s->tuning_rise_inf_c;
    d->tuning_seq = s->tuning_seq;
    d->adaptive_tune_enabled = s->adaptive_tune_enabled;
    /* coupling_diag_k_dc: not touched -- the memset above already left it at
     * 0 ("not measured"), v14's documented default for a field it never had.
     * See this function's own header comment. */
}

/* Versions 1-7 predate the nine timing-override fields entirely -- there is
 * nothing to migrate, so every zone gets pointed at one synthesized, all-zero
 * "Default" profile (0 in each of the nine fields is already that field's own
 * "use the firmware default" meaning -- see zone_timing_profile_t's comment),
 * matching exactly how those boards already behaved pre-upgrade. Relies on
 * convert_versioned_blob_to_current()'s own memset(out, 0, sizeof(*out)) at
 * entry to have already zeroed timing_profiles[0]'s nine floats and every
 * zone's timing_profile index (0) -- this only needs to name the profile. */
void set_default_timing_profile(zones_cfg_t *out)
{
    out->timing_profile_count = 1;
    snprintf(out->timing_profiles[0].name, sizeof(out->timing_profiles[0].name), "Default");
}
