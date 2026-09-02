// See zones_config_json.h for why these functions, the current zones_cfg_t
// layout, and the historical zone_cfg_v1_t..v10_t/zones_cfg_v1_t..v10_t
// on-flash snapshots (declared there, not here -- test_zones_http.c#includes
// zones_http.c textually and references several of them directly) all live
// in their own HTTP-free file.
#include "zones_config_json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crc.h"
#include "esp_log.h"

#include "http_form.h"
#include "kiln_io.h" /* KILN_IO_RELAY_COUNT -- zones_config_json_validate() */
#include "zone_settings_source_chain.h" /* the shared settings_source chain-walk -- see that
                                          * header's own comment for why it lives outside this
                                          * file too: test_backup_import.c's stub for
                                          * zones_config_settings_source_import_has_cycle()
                                          * calls the identical algorithm from here. */

static const char *TAG = "zones_config_json";

static size_t expected_len_for_version(uint8_t version)
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
static void convert_zone_v1(const zone_cfg_v1_t *s, zone_cfg_t *d, uint8_t chan_idx)
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

static void convert_zone_v3(const zone_cfg_v3_t *s, zone_cfg_t *d, uint8_t chan_idx)
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

static void convert_zone_v4(const zone_cfg_v4_t *s, zone_cfg_t *d)
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

static void convert_zone_v5(const zone_cfg_v5_t *s, zone_cfg_t *d)
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
 * checked `len` against expected_len_for_version(version), so the memcpy of
 * `blob` into each local, exactly-sized historical struct below is safe. */
/* v6/v7 -> current. Every field the old layout had is copied by name; the nine
 * v8 additions get 0, which is already their "not configured, use the firmware
 * default" meaning, so a board upgrading from v7 behaves exactly as it did. */
static void convert_zone_v7(const zone_cfg_v7_t *s, zone_cfg_t *d)
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
static void convert_zone_v8(const zone_cfg_v8_t *s, zone_cfg_t *d)
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
static void convert_zone_v9(const zone_cfg_v9_t *s, zone_cfg_t *d)
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
static void convert_zone_v10(const zone_cfg_v10_t *s, zone_cfg_t *d, uint8_t chan_idx)
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
static void convert_zone_v11(const zone_cfg_v11_t *s, zone_cfg_t *d)
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
static void convert_zone_v12(const zone_cfg_v12_t *s, zone_cfg_t *d)
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
static void convert_zone_v13(const zone_cfg_v13_t *s, zone_cfg_t *d)
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

/* Versions 1-7 predate the nine timing-override fields entirely -- there is
 * nothing to migrate, so every zone gets pointed at one synthesized, all-zero
 * "Default" profile (0 in each of the nine fields is already that field's own
 * "use the firmware default" meaning -- see zone_timing_profile_t's comment),
 * matching exactly how those boards already behaved pre-upgrade. Relies on
 * convert_versioned_blob_to_current()'s own memset(out, 0, sizeof(*out)) at
 * entry to have already zeroed timing_profiles[0]'s nine floats and every
 * zone's timing_profile index (0) -- this only needs to name the profile. */
static void set_default_timing_profile(zones_cfg_t *out)
{
    out->timing_profile_count = 1;
    snprintf(out->timing_profiles[0].name, sizeof(out->timing_profiles[0].name), "Default");
}

static bool convert_versioned_blob_to_current(uint8_t version, const void *blob, zones_cfg_t *out)
{
    memset(out, 0, sizeof(*out));
    switch (version) {
    case 1: {
        zones_cfg_v1_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = 0; /* predates this field -- 0 is its documented default */
        out->safety_tc_type = THERMO_TC_K;
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            convert_zone_v1(&src.zones[i], &out->zones[i], i);
        }
        set_default_timing_profile(out);
        return true;
    }
    case 2: {
        zones_cfg_v2_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = THERMO_TC_K;
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            convert_zone_v1(&src.zones[i], &out->zones[i], i);
        }
        set_default_timing_profile(out);
        return true;
    }
    case 3: {
        zones_cfg_v3_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = THERMO_TC_K;
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            convert_zone_v3(&src.zones[i], &out->zones[i], i);
        }
        set_default_timing_profile(out);
        return true;
    }
    case 4: {
        zones_cfg_v4_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = THERMO_TC_K;
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            convert_zone_v4(&src.zones[i], &out->zones[i]);
        }
        set_default_timing_profile(out);
        return true;
    }
    case 5: {
        zones_cfg_v5_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = src.safety_tc_type; /* real value from v5 on */
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            convert_zone_v5(&src.zones[i], &out->zones[i]);
        }
        set_default_timing_profile(out);
        return true;
    }
    case 6: {
        zones_cfg_v6_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = src.safety_tc_type;
        out->pc_link_abort_silence_ms = 0.0f; /* v6 has no such field -- 0 = firmware default */
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            convert_zone_v7(&src.zones[i], &out->zones[i]);
        }
        set_default_timing_profile(out);
        return true;
    }
    case 7: {
        zones_cfg_v7_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = src.safety_tc_type;
        out->pc_link_abort_silence_ms = 0.0f; /* v7 has no such field -- 0 = firmware default */
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            convert_zone_v7(&src.zones[i], &out->zones[i]);
        }
        set_default_timing_profile(out);
        /* src.crc32 is deliberately NOT carried over: it covered the v7 shape,
         * and nvs_save() stamps a fresh one over the current struct. */
        return true;
    }
    case 8: {
        zones_cfg_v8_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = src.safety_tc_type;
        out->pc_link_abort_silence_ms = src.pc_link_abort_silence_ms; /* real v8 value */
        /* THE actual v8->v9 migration (see ZONES_CFG_VERSION's 8->9 comment
         * for the full rationale): v8 kept the nine timing overrides PER
         * ZONE, so migrating them verbatim would mean giving every zone its
         * own private profile -- exactly the "still copied N times" state
         * the owner's request asks to eliminate. Instead, deduplicate: for
         * each zone, look for an already-allocated profile (from an earlier
         * zone in this same loop) whose nine values are ALL identical to
         * this zone's; point the zone at that profile if found, otherwise
         * allocate a fresh profile from this zone's own values. A board
         * where every zone is still at the v8 all-zero default -- the
         * owner's board today -- collapses to profile_count == 1. A board
         * with three genuinely distinct zones ends up with three profiles,
         * one per zone, which still fits: timing_profiles[] is sized
         * MAX31856_CHANNEL_COUNT, the same as zones[], so "one profile per
         * zone" is always the worst case, never an overflow. */
        uint8_t profile_count = 0;
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            const zone_cfg_v8_t *s = &src.zones[i];
            uint8_t match = profile_count; /* == "no match found yet" sentinel */
            for (uint8_t p = 0; p < profile_count; p++) {
                const zone_timing_profile_t *tp = &out->timing_profiles[p];
                if (tp->guard_progress_duty_min == s->guard_progress_duty_min &&
                    tp->guard_progress_window_s == s->guard_progress_window_s &&
                    tp->guard_drift_hysteresis_c == s->guard_drift_hysteresis_c &&
                    tp->guard_frozen_eps_c == s->guard_frozen_eps_c &&
                    tp->guard_cross_zone_period_s == s->guard_cross_zone_period_s &&
                    tp->bangbang_hysteresis_c == s->bangbang_hysteresis_c &&
                    tp->cooling_limited_margin_c == s->cooling_limited_margin_c &&
                    tp->cooling_limited_hold_s == s->cooling_limited_hold_s &&
                    tp->ramp_lock_band_c == s->ramp_lock_band_c) {
                    match = p;
                    break;
                }
            }
            if (match == profile_count) {
                /* No existing profile matches -- allocate a new one from this
                 * zone's own nine values. profile_count can never reach
                 * MAX31856_CHANNEL_COUNT before this loop's own index i does
                 * (at most one NEW profile is allocated per zone iterated),
                 * so this write is always in-bounds. */
                zone_timing_profile_t *tp = &out->timing_profiles[profile_count];
                tp->guard_progress_duty_min = s->guard_progress_duty_min;
                tp->guard_progress_window_s = s->guard_progress_window_s;
                tp->guard_drift_hysteresis_c = s->guard_drift_hysteresis_c;
                tp->guard_frozen_eps_c = s->guard_frozen_eps_c;
                tp->guard_cross_zone_period_s = s->guard_cross_zone_period_s;
                tp->bangbang_hysteresis_c = s->bangbang_hysteresis_c;
                tp->cooling_limited_margin_c = s->cooling_limited_margin_c;
                tp->cooling_limited_hold_s = s->cooling_limited_hold_s;
                tp->ramp_lock_band_c = s->ramp_lock_band_c;
                /* Named after the fact below, once it's known whether this
                 * is the lone shared "Default" (every zone matched) or one
                 * of several genuinely distinct migrated profiles. */
                profile_count++;
            }
            convert_zone_v8(s, &out->zones[i]);
            out->zones[i].timing_profile = match;
        }
        /* Name the migrated profiles so an operator recognizes them. The
         * degenerate, overwhelmingly common case -- every zone was at the
         * v8 all-zero default, i.e. nobody had touched the safety-timings
         * page yet, true of the owner's board today -- collapses to exactly
         * one profile; call it "Default" rather than "Migrated (zone 1)",
         * since it isn't really zone 1's private setting, it's simply the
         * firmware default every zone was already (implicitly) using. */
        if (profile_count == 1) {
            snprintf(out->timing_profiles[0].name, sizeof(out->timing_profiles[0].name), "Default");
        } else {
            /* "Zone %u", not "Migrated %u" -- TIMING_PROFILE_NAME_MAX_LEN is
             * only 7 (see its own comment for why), too short for "Migrated 1"
             * (10 chars) to survive without silent truncation. "Zone 1"/
             * "Zone 2"/"Zone 3" fits with room to spare and is still a name an
             * operator recognizes: each of these profiles came from exactly
             * one zone's own v8 values. */
            /* The digit is written as a single char rather than with %u so
             * the compiler can see the output length. "Zone %u" against a
             * 7-char name is fine for any real channel count, but %u's widest
             * expansion is ten digits, which the target build's
             * -Werror=format-truncation rejects on a bound it cannot prove.
             * The static assert is what actually keeps this honest: it fails
             * the build if the channel count ever reaches double digits,
             * rather than letting the name silently lose its digit. */
            _Static_assert(MAX31856_CHANNEL_COUNT <= 9,
                           "single-digit zone naming below assumes at most 9 channels");
            for (uint8_t p = 0; p < profile_count; p++) {
                snprintf(out->timing_profiles[p].name, sizeof(out->timing_profiles[p].name),
                         "Zone %c", (char)('0' + p + 1));
            }
        }
        /* profile_count is always >= 1 here: MAX31856_CHANNEL_COUNT (the
         * loop bound above) is a fixed hardware constant > 0, so the loop
         * always runs at least once and always allocates at least the first
         * zone's profile. timing_profile_count must never be 0 in a valid
         * config -- see its own comment on zones_cfg_t -- and this migration
         * never produces that. */
        out->timing_profile_count = profile_count;
        return true;
    }
    case 9: {
        /* v9 -> v10 (this pass): straightforward field-for-field carry-through
         * -- v9 already has timing_profiles[]/timing_profile_count in their
         * CURRENT shape (zone_timing_profile_t did not change), so this case
         * is nothing like case 8's dedup logic. The only thing that must be
         * gotten right is convert_zone_v9()'s explicit settings_source
         * assignment -- see its own comment and ZONES_CFG_VERSION's 9->10
         * comment. */
        zones_cfg_v9_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = src.safety_tc_type;
        out->pc_link_abort_silence_ms = src.pc_link_abort_silence_ms; /* real v9 value */
        out->timing_profile_count = src.timing_profile_count;
        memcpy(out->timing_profiles, src.timing_profiles, sizeof(out->timing_profiles));
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            convert_zone_v9(&src.zones[i], &out->zones[i]);
        }
        /* src.crc32 deliberately NOT carried over -- it covered the v9 shape;
         * nvs_save() stamps a fresh one over the current (v11) struct. */
        return true;
    }
    case 10: {
        /* v10 -> v11 (this pass): field-for-field carry-through, same shape
         * as case 9 above -- timing_profiles[]/timing_profile_count are
         * still in their CURRENT shape (zone_timing_profile_t unchanged
         * again). The one real migration is convert_zone_v10()'s folding of
         * the single coupling pair into the new row; see that function's own
         * comment and ZONES_CFG_VERSION's 10->11 comment. */
        zones_cfg_v10_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = src.safety_tc_type;
        out->pc_link_abort_silence_ms = src.pc_link_abort_silence_ms; /* real v10 value */
        out->timing_profile_count = src.timing_profile_count;
        memcpy(out->timing_profiles, src.timing_profiles, sizeof(out->timing_profiles));
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            convert_zone_v10(&src.zones[i], &out->zones[i], i);
        }
        /* src.crc32 deliberately NOT carried over -- it covered the v10
         * shape; nvs_save() stamps a fresh one over the current (v12)
         * struct. */
        return true;
    }
    case 11: {
        /* v11 -> v12 (this pass): field-for-field carry-through, same shape
         * as case 10 above -- timing_profiles[]/timing_profile_count are
         * still in their CURRENT shape. The only real migration is
         * convert_zone_v11() leaving coupling_tau_s[]/coupling_dead_time_s[]
         * at their zeroed "not measured" default; see that function's own
         * comment and ZONES_CFG_VERSION's 11->12 comment. */
        zones_cfg_v11_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = src.safety_tc_type;
        out->pc_link_abort_silence_ms = src.pc_link_abort_silence_ms; /* real v11 value */
        out->timing_profile_count = src.timing_profile_count;
        memcpy(out->timing_profiles, src.timing_profiles, sizeof(out->timing_profiles));
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            convert_zone_v11(&src.zones[i], &out->zones[i]);
        }
        /* src.crc32 deliberately NOT carried over -- it covered the v11
         * shape; nvs_save() stamps a fresh one over the current (v12)
         * struct. */
        return true;
    }
    case 12: {
        /* v12 -> v13 (this pass): field-for-field carry-through, same shape
         * as case 11 above -- timing_profiles[]/timing_profile_count are
         * still in their CURRENT shape. The only real migration is
         * convert_zone_v12() leaving every tuning_* field at its zeroed
         * "unknown" default; see that function's own comment and
         * ZONES_CFG_VERSION's 12->13 comment. */
        zones_cfg_v12_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = src.safety_tc_type;
        out->pc_link_abort_silence_ms = src.pc_link_abort_silence_ms; /* real v12 value */
        out->timing_profile_count = src.timing_profile_count;
        memcpy(out->timing_profiles, src.timing_profiles, sizeof(out->timing_profiles));
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            convert_zone_v12(&src.zones[i], &out->zones[i]);
        }
        /* src.crc32 deliberately NOT carried over -- it covered the v12
         * shape; nvs_save() stamps a fresh one over the current (v13)
         * struct. */
        return true;
    }
    case 13: {
        /* v13 -> v14 (this pass): field-for-field carry-through, same shape
         * as case 12 above -- timing_profiles[]/timing_profile_count are
         * still in their CURRENT shape. The only real migration is
         * convert_zone_v13() leaving adaptive_tune_enabled at its zeroed
         * "opted out" default; see that function's own comment and zone_
         * cfg_t::adaptive_tune_enabled's ZONES_CFG_VERSION 13->14 comment. */
        zones_cfg_v13_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = src.safety_tc_type;
        out->pc_link_abort_silence_ms = src.pc_link_abort_silence_ms; /* real v13 value */
        out->timing_profile_count = src.timing_profile_count;
        memcpy(out->timing_profiles, src.timing_profiles, sizeof(out->timing_profiles));
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            convert_zone_v13(&src.zones[i], &out->zones[i]);
        }
        /* src.crc32 deliberately NOT carried over -- it covered the v13
         * shape; nvs_save() stamps a fresh one over the current (v14)
         * struct. */
        return true;
    }
    default:
        /* No known historical (or current) layout for this version --
         * expected_len_for_version() already returned 0 for it and
         * zones_config_json_decode_blob() should never reach here; kept as a defensive
         * explicit refusal rather than silently guessing. */
        return false;
    }
}

/* esp_crc32_le() (same helper crash_report.c's compute_crc() uses) over the
 * struct with crc32 itself zeroed -- computed over a local copy so a caller
 * re-validating an already-loaded cfg's crc32 is never mutated by asking. */
uint32_t zones_config_json_compute_crc(const zones_cfg_t *cfg)
{
    zones_cfg_t tmp = *cfg;
    tmp.crc32 = 0;
    return esp_crc32_le(0, (const uint8_t *)&tmp, sizeof(tmp));
}


/* Raises any stored, configured (non-zero) heater_min_on_ms up to
 * ZONE_HEATER_MIN_ON_MS_FLOOR as the blob comes off flash.
 *
 * Without this, the floor added on 2026-08-28 would make previously-saved
 * configs un-round-trippable: a board carrying the old 2000 ms default would
 * report 2000 from GET /api/zones, and the very next POST of what was just
 * read -- which is exactly what the web page and zones_http_client.py both do
 * -- would be refused for a value the operator never chose. Raising on load
 * means the refusal only ever fires on a number a human actually typed.
 *
 * Deliberately not a blob-version migration: it applies on EVERY load,
 * whatever version the blob claimed, so it also catches a config restored
 * from an old backup or written by a rolled-back firmware. It only writes
 * back to flash when a later nvs_save() happens for some other reason; the
 * in-RAM value is what heater_output_duty() and GET both see, and that is
 * the property that matters. Zero is left alone -- 0 means "not configured",
 * and the default it selects is the floor already. */
static void raise_heater_timing_to_floors(zones_cfg_t *cfg)
{
    for (size_t zi = 0; zi < sizeof(cfg->zones) / sizeof(cfg->zones[0]); ++zi) {
        float v = cfg->zones[zi].heater_min_on_ms;
        if (isfinite(v) && v > 0.0f && v < ZONE_HEATER_MIN_ON_MS_FLOOR) {
            ESP_LOGW(TAG, "zone %u heater_min_on_ms %.0f ms is below the %.0f ms relay-protection "
                          "floor -- raising it (stored config predates the floor)",
                     (unsigned)zi, (double)v, (double)ZONE_HEATER_MIN_ON_MS_FLOOR);
            cfg->zones[zi].heater_min_on_ms = ZONE_HEATER_MIN_ON_MS_FLOOR;
        }
        /* Then the window, against the min_on just settled above. Same
         * round-trip argument, and one more reason of its own: a stored
         * window this short does not merely read back a number the kiln is
         * not using, it makes the zone unable to heat at any duty under
         * ~1.0 (2026-08-29, found on this bench's zone 0 at 2000 ms). Order
         * matters -- the required window is computed from the raised
         * min_on, never the sub-floor one. */
        float w = cfg->zones[zi].heater_window_ms;
        float need = zone_required_window_ms(cfg->zones[zi].heater_min_on_ms);
        if (isfinite(w) && w > 0.0f && w < need) {
            ESP_LOGW(TAG, "zone %u heater_window_ms %.0f ms is shorter than %.0f ms (%.0fx its "
                          "%.0f ms minimum on-time) -- raising it; no fractional duty could be "
                          "rendered in a window that short",
                     (unsigned)zi, (double)w, (double)need, (double)ZONE_HEATER_WINDOW_MIN_MULTIPLE,
                     (double)cfg->zones[zi].heater_min_on_ms);
            cfg->zones[zi].heater_window_ms = need;
        }
    }
}

/* The one place a stored zones_cfg blob (from NVS or a kiln_cfg_store import)
 * is turned into a trustworthy, current-format zones_cfg_t. Implements items
 * 1-3 of the "saved securely like the others" fix: a length check against the
 * blob's OWN claimed version before anything is copied or interpreted, typed
 * per-version conversion (never a memcpy of one struct shape over another),
 * and zones_config_json_validate() run on every path, not just import. Item 4 (CRC)
 * is folded in here too, for the current-version case only -- see
 * zones_cfg_t::crc32's comment for why older versions have no CRC to check. */
zones_decode_result_t zones_config_json_decode_blob(const void *blob, size_t len, zones_cfg_t *out,
                                                const char **err_reason)
{
    static const char *unused_reason;
    const char **reason = err_reason ? err_reason : &unused_reason;
    *reason = "";
    memset(out, 0, sizeof(*out));

    if (!blob || len < sizeof(((zones_cfg_t *)0)->version)) {
        *reason = "blob missing or too short to contain a version";
        return ZONES_DECODE_CORRUPT;
    }
    uint8_t version = ((const uint8_t *)blob)[0];

    if (version > ZONES_CFG_VERSION) {
        /* Firmware-rollback case (TODO.md 8.1) -- this build does not know
         * that layout and must not guess at it. Deliberately does NOT check
         * length against anything here: an unknown newer layout could be any
         * size, and the whole point of this branch is refusing to interpret
         * it at all. */
        *reason = "this config was saved by newer firmware -- refusing rather than guessing";
        return ZONES_DECODE_NEWER;
    }

    size_t expected = expected_len_for_version(version);
    if (expected == 0) {
        *reason = "unknown/unsupported zones_cfg version";
        return ZONES_DECODE_CORRUPT;
    }
    if (len != expected) {
        *reason = "blob length does not match its claimed version -- treating as corrupt";
        return ZONES_DECODE_CORRUPT;
    }

    if (version == ZONES_CFG_VERSION) {
        memcpy(out, blob, sizeof(*out));
        uint32_t stored_crc = out->crc32;
        uint32_t computed_crc = zones_config_json_compute_crc(out);
        if (computed_crc != stored_crc) {
            memset(out, 0, sizeof(*out));
            *reason = "CRC mismatch -- treating as corrupt";
            return ZONES_DECODE_CORRUPT;
        }
    } else {
        if (!convert_versioned_blob_to_current(version, blob, out)) {
            memset(out, 0, sizeof(*out));
            *reason = "unable to convert stored version to the current layout";
            return ZONES_DECODE_CORRUPT;
        }
        out->version = ZONES_CFG_VERSION;
        /* out->crc32 stays 0 here -- a migrated struct has never been saved
         * in the current format yet, so there is no stored CRC to check
         * against. nvs_save() stamps a real one the next time this config is
         * written, current or not. */
    }

    /* Before zones_config_json_validate(), not after: zones_config_json_validate() now
     * refuses a sub-floor heater_min_on_ms, and every blob written before
     * 2026-08-28 could legally carry one (2000 ms was the old default).
     * Raising here covers BOTH decode callers -- nvs_load_from() and
     * zones_config_import_blob() -- so neither a reboot nor restoring an old
     * kiln-config slot can be refused for a number no operator ever typed,
     * while a number an operator DOES type still goes through
     * parse_zone_fields()'s refusal. */
    raise_heater_timing_to_floors(out);

    const char *validate_reason = "invalid stored config";
    if (!zones_config_json_validate(out, &validate_reason)) {
        memset(out, 0, sizeof(*out));
        *reason = validate_reason;
        return ZONES_DECODE_CORRUPT;
    }
    return ZONES_DECODE_OK;
}
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
bool zones_config_json_settings_source_chain_has_cycle(const zone_cfg_t zones[MAX31856_CHANNEL_COUNT], uint8_t start,
                                            uint8_t thermo_count)
{
    uint8_t sources[MAX31856_CHANNEL_COUNT];
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        sources[i] = zones[i].settings_source;
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
    for (uint8_t i = 0; i < thermo_count; i++) {
        if (!zones_config_json_settings_source_chain_has_cycle(cfg->zones, i, thermo_count)) {
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
            uint8_t src = cfg->zones[cur].settings_source;
            if (src == ZONE_SETTINGS_SOURCE_CUSTOM || src >= thermo_count) {
                break; /* terminates cleanly -- can only happen if an earlier loop
                        * iteration already fixed the cycle this start used to reach */
            }
            cur = src;
        }
        for (uint8_t p = cycle_start_pos; p < path_len; p++) {
            uint8_t zone = path[p];
            ESP_LOGW(TAG, "zones_cfg from '%s': zone %u's settings_source chain forms a cycle -- "
                          "collapsing zone %u to Custom (was %u)",
                     partition, (unsigned)zone, (unsigned)zone, (unsigned)cfg->zones[zone].settings_source);
            cfg->zones[zone].settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;
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
        /* settings_source: either the CUSTOM sentinel, or a real zone index --
         * never checked against thermo_count (the dropdown offers every
         * *configured* zone at save time, a page-level decision, not a
         * storage-layer one; a zone later disabled by lowering thermo_count
         * still leaves a readable, in-range index here). */
        if (z->settings_source != ZONE_SETTINGS_SOURCE_CUSTOM && z->settings_source >= MAX31856_CHANNEL_COUNT) {
            *err_reason = "zone settings_source references a zone that doesn't exist";
            return false;
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
