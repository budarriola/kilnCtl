#include "zones_http_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "MAX31856.h"
#include "zone_settings_source_chain.h" /* zones_config_settings_source_import_has_cycle() shares the
                                          * chain-walk algorithm with this file's setters. */

/* ---- Public getters (profiles_http.c) ------------------------------------ */

bool zones_config_get_max_ramp(uint8_t zone_index, float *out_c_per_hr)
{
    if (!out_c_per_hr || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_c_per_hr = s_zones.cfg.zones[zone_index].max_ramp_c_per_hr;
    return true;
}

/* Same bound parse_zone_fields()'s z%u_ramp enforces. 0 is legal (the
 * documented "never configured" encoding). */
bool zones_config_set_max_ramp(uint8_t zone_index, float c_per_hr)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(c_per_hr) || c_per_hr < 0.0f || c_per_hr > ZONE_MAX_RAMP_C_PER_HR_MAX) {
        return false;
    }
    s_zones.cfg.zones[zone_index].max_ramp_c_per_hr = c_per_hr;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

bool zones_config_get_cal_offset(uint8_t zone_index, float *out_cal_offset_c)
{
    if (!out_cal_offset_c || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_cal_offset_c = s_zones.cfg.zones[zone_index].cal_offset_c;
    return true;
}

/* Same bound parse_zone_fields()'s z%u_cal enforces. */
bool zones_config_set_cal_offset(uint8_t zone_index, float cal_offset_c)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(cal_offset_c) || cal_offset_c < ZONE_CAL_OFFSET_MIN_C || cal_offset_c > ZONE_CAL_OFFSET_MAX_C) {
        return false;
    }
    s_zones.cfg.zones[zone_index].cal_offset_c = cal_offset_c;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

/* PID_EXPANSION_PLAN.md 3.3 consolidation -- see zones_http.h's own doc
 * comment on this pair. zone_index >= thermo_count reads as "not enabled",
 * same convention every other simple getter in this file uses (see
 * zones_config_get_max_ramp() just above), not a special "cannot answer"
 * case -- opt-in has one meaning (on/off) at every valid index and a safe
 * default (off) everywhere else. */
bool zones_config_get_adaptive_tune_enabled(uint8_t zone_index)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    return s_zones.cfg.zones[zone_index].adaptive_tune_enabled != 0;
}

bool zones_config_set_adaptive_tune_enabled(uint8_t zone_index, bool enabled)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    s_zones.cfg.zones[zone_index].adaptive_tune_enabled = enabled ? 1 : 0;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

uint32_t zones_config_generation(void)
{
    return s_config_generation;
}

/* TODO.md 8.2 "Tie it to the guards, not only the UI" -- see
 * s_zones_config_valid's comment for the exact rule. Consulted by
 * profile_executor.c/autotune_engine.c before allowing a run to start, and
 * reported on /api/status (dashboard_http.c) as zones_config_valid. */
bool zones_config_is_valid(void)
{
    return s_zones_config_valid;
}

uint8_t zones_config_get_thermo_count(void)
{
    return s_zones.cfg.thermo_count;
}

uint8_t zones_config_get_relay_count(void)
{
    return s_zones.cfg.relay_count;
}

uint8_t zones_config_get_max_simultaneous_relays(void)
{
    return s_zones.cfg.max_simultaneous_relays;
}

bool zones_config_get_continue_on_zone_trip(void)
{
    return s_zones.cfg.continue_on_zone_trip != 0;
}

/* 2026-08-21, TODO.md owner-report item 3 -- safety_link.c's consumer (see
 * its safety_sync_tc_type()): the RP2040 safety processor's own,
 * independent thermocouple type, as last saved on this page. Always
 * answerable, unlike the per-zone getters below -- this is a global setting
 * with a real value from the moment NVS first loads (defaulting to
 * THERMO_TC_K either via a fresh zero-init struct reading 0/THERMO_TC_B...
 * no: see below) -- so, unlike zones_config_get_max_ramp() and friends,
 * there is no "cannot answer" case to report via a bool return; the return
 * value exists only so this getter's shape matches every other one in this
 * file and a future caller doesn't have to special-case it.
 *
 * IMPORTANT: on a board that has never loaded a valid zones config at all
 * (s_zones_config_valid false -- first boot, corrupt NVS, a refused
 * newer-than-firmware blob), s_zones.cfg is the zeroed default, and 0 here
 * decodes as THERMO_TC_B, NOT THERMO_TC_K -- unlike MAX31856.c's own
 * hardcoded boot default. safety_link.c's caller MUST check
 * zones_config_is_valid() itself before trusting this value for anything
 * other than "what would get sent if asked to sync right now" -- see that
 * file's safety_sync_tc_type() for how it actually guards this. */
bool zones_config_get_safety_tc_type(uint8_t *out_tc_type)
{
    if (!out_tc_type) {
        return false;
    }
    *out_tc_type = s_zones.cfg.safety_tc_type;
    return true;
}

/* 2026-08-21, LCD item 1 (TODO.md owner-report item 1/4's on-device half):
 * the web zones page (zones_page.html) gained per-channel thermocouple type
 * selection this same day, but only as an HTTP form field -- there was no
 * public getter/setter an LCD page could call, only the POST body parser's
 * private z%u_tctype handling above. Follows zones_config_get_pid()'s/
 * zones_config_set_pid()'s exact shape (same zone_index bounds check, same
 * "false means cannot answer" convention, setter bumps s_config_generation
 * before nvs_save() so a running profile's next tick sees the new value
 * whether or not the flash write itself succeeds) so this module keeps
 * exactly one accessor pattern rather than growing a second one for LCD
 * callers specifically.
 *
 * Deliberately does NOT call thermo_owner_command_config_channel() to push
 * the new type to hardware immediately -- checked zones_http.c's own POST
 * /api/zones handler (the web page's save path) before writing this, and it
 * doesn't either: the only place this module ever calls that function is
 * zones_http_start()'s boot-time apply, further down this file. So an LCD
 * write and a web-page save now have the SAME behaviour (persisted
 * immediately, taken into the running MAX31856 register only on the next
 * boot) rather than the LCD accidentally doing more than the web form it is
 * mirroring. If that boot-only gap is ever closed for the web path, this
 * setter should gain the same live-apply call at the same time -- not
 * silently drift ahead of it. */
bool zones_config_get_tc_type(uint8_t zone_index, uint8_t *out_tc_type)
{
    if (!out_tc_type || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_tc_type = s_zones.cfg.zones[zone_index].tc_type;
    return true;
}

bool zones_config_set_tc_type(uint8_t zone_index, uint8_t tc_type)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (tc_type > ZONE_TC_TYPE_MAX_REAL) {
        /* Same bound the POST parser enforces (parse_zone_fields()) -- a
         * voltage-input mode is never a legal choice from an
         * operator-facing "thermocouple type" control, LCD or web alike. */
        return false;
    }
    s_zones.cfg.zones[zone_index].tc_type = tc_type;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

/* Setter half of zones_config_get_safety_tc_type() above -- same "global,
 * not per-zone" setting (the RP2040 safety processor's own MAX31856-equivalent
 * type), same bound as the per-channel setter just above, same generation/
 * nvs_save shape as every other setter in this file. safety_link.c's
 * safety_sync_tc_type() is the consumer that notices the generation bump and
 * re-mirrors this to the Pico -- this function does not talk to the safety
 * link itself, matching the POST handler's own division of labor (this
 * module owns storage only). */
bool zones_config_set_safety_tc_type(uint8_t tc_type)
{
    if (tc_type > ZONE_TC_TYPE_MAX_REAL) {
        return false;
    }
    s_zones.cfg.safety_tc_type = tc_type;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

bool zones_config_get_relay_mask(uint8_t zone_index, uint8_t *out_mask)
{
    if (!out_mask || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_mask = s_zones.cfg.zones[zone_index].relay_mask;
    return true;
}

/* Same bound parse_zone_fields()'s z%u_relay_mask handling enforces --
 * relay_mask may only reference relays 1..relay_count. */
bool zones_config_set_relay_mask(uint8_t zone_index, uint8_t relay_mask)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    uint8_t valid_bits = s_zones.cfg.relay_count >= 8 ? 0xFF : (uint8_t)((1u << s_zones.cfg.relay_count) - 1u);
    if ((relay_mask & ~valid_bits) != 0) {
        return false;
    }
    s_zones.cfg.zones[zone_index].relay_mask = relay_mask;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

bool zones_config_get_thermo_mask(uint8_t zone_index, uint8_t *out_mask)
{
    if (!out_mask || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_mask = s_zones.cfg.zones[zone_index].thermo_mask;
    return true;
}

/* Same bound parse_zone_fields()'s explicit z%u_thermo_mask handling
 * enforces -- thermo_mask may only reference channels 1..thermo_count.
 * Unlike the POST handler this setter has no "omitted means preserve the
 * legacy mapping" case -- see this function's header comment. */
bool zones_config_set_thermo_mask(uint8_t zone_index, uint8_t thermo_mask)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    uint8_t valid_bits =
        s_zones.cfg.thermo_count >= 8 ? 0xFF : (uint8_t)((1u << s_zones.cfg.thermo_count) - 1u);
    if ((thermo_mask & ~valid_bits) != 0) {
        return false;
    }
    s_zones.cfg.zones[zone_index].thermo_mask = thermo_mask;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

bool zones_config_get_ct_mask(uint8_t zone_index, uint8_t *out_mask)
{
    if (!out_mask || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_mask = s_zones.cfg.zones[zone_index].ct_mask;
    return true;
}

/* Same bound parse_zone_fields()'s z%u_ct_mask handling enforces -- ct_mask
 * may only reference channels 1..ZONE_CT_CHANNEL_COUNT, a fixed hardware
 * count (not relay_count/thermo_count-relative like the two setters above). */
bool zones_config_set_ct_mask(uint8_t zone_index, uint8_t ct_mask)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    uint8_t valid_bits = (uint8_t)((1u << ZONE_CT_CHANNEL_COUNT) - 1u);
    if ((ct_mask & ~valid_bits) != 0) {
        return false;
    }
    s_zones.cfg.zones[zone_index].ct_mask = ct_mask;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

bool zones_config_get_name(uint8_t zone_index, char *out, size_t out_cap)
{
    if (!out || out_cap == 0 || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    strncpy(out, s_zones.cfg.zones[zone_index].name, out_cap - 1);
    out[out_cap - 1] = '\0';
    return true;
}

/* Same rejection parse_zone_fields() produces for an overlong z%u_name
 * (http_form_find_field() returning -2), applied to a NUL-terminated C
 * string. NULL is treated as an empty name (clears it), matching a POST that
 * omits the field. */
bool zones_config_set_name(uint8_t zone_index, const char *name)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    size_t len = name ? strlen(name) : 0;
    if (len > ZONE_NAME_MAX_LEN) {
        return false;
    }
    strncpy(s_zones.cfg.zones[zone_index].name, name ? name : "", ZONE_NAME_MAX_LEN);
    s_zones.cfg.zones[zone_index].name[ZONE_NAME_MAX_LEN] = '\0';
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

/* See zones_http.h's doc comment on this pair, and this file's relay-names
 * section header comment for the full design rationale (separate NVS blob,
 * name kept across zone reassignment). */
bool zones_config_get_relay_name(uint8_t relay_n, char *out, size_t out_cap)
{
    if (relay_n < 1 || relay_n > KILN_IO_RELAY_COUNT || !out || out_cap == 0) {
        return false;
    }
    strncpy(out, s_relay_names.cfg.names[relay_n - 1], out_cap - 1);
    out[out_cap - 1] = '\0';
    return true;
}

bool zones_config_set_relay_name(uint8_t relay_n, const char *name)
{
    if (relay_n < 1 || relay_n > KILN_IO_RELAY_COUNT) {
        return false;
    }
    size_t len = name ? strlen(name) : 0;
    if (len > RELAY_NAME_MAX_LEN) {
        return false;
    }
    strncpy(s_relay_names.cfg.names[relay_n - 1], name ? name : "", RELAY_NAME_MAX_LEN);
    s_relay_names.cfg.names[relay_n - 1][RELAY_NAME_MAX_LEN] = '\0';
    return relay_names_save() == ESP_OK;
}

bool zones_config_get_pid(uint8_t zone_index, float *out_kp, float *out_ki, float *out_kd)
{
    if (!out_kp || !out_ki || !out_kd || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    const zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    *out_kp = z->pid_kp;
    *out_ki = z->pid_ki;
    *out_kd = z->pid_kd;
    return true;
}

bool zones_config_set_pid(uint8_t zone_index, float kp, float ki, float kd)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    /* Same bound parse_zone_fields()'s z%u_kp/z%u_ki/z%u_kd handling enforces
     * (ZONE_PID_GAIN_MAX, zones_http.h) -- this used to check only isfinite()
     * and >= 0.0f, with no upper bound, which meant a caller reaching this
     * setter directly (the LCD UI, PcTools/MCP, and now POST /api/zones/pid)
     * could exceed what the web form's own POST /api/zones path would ever
     * accept for the identical field. Closed 2026-08-31 alongside the new
     * narrow PID-only endpoint precisely because that endpoint is a second
     * caller of this same code -- exactly the moment a latent bound mismatch
     * like this stops being harmless. */
    if (!isfinite(kp) || !isfinite(ki) || !isfinite(kd) || kp < 0.0f || ki < 0.0f || kd < 0.0f ||
        kp > ZONE_PID_GAIN_MAX || ki > ZONE_PID_GAIN_MAX || kd > ZONE_PID_GAIN_MAX) {
        return false;
    }
    zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    z->pid_kp = kp;
    z->pid_ki = ki;
    z->pid_kd = kd;
    /* ZONES_CFG_VERSION 12->13: invalidate the tuning-quality record (set 1
     * -- see zone_cfg_t::tuning_valid's own doc comment) on EVERY gain
     * change, unconditionally, regardless of caller -- autotune's own
     * accept() path, a manual POST /api/zones/pid edit, adaptive_tune.c's
     * blended re-tune, backup_http.c's restore, or the LCD UI/uart_bridge_
     * ext.c path. All of them reach gains through this one setter, which is
     * exactly why the invalidation lives HERE and not duplicated at every
     * call site -- this repo's recurring "reset-one-side" bug class (state
     * updated on one path of a pair and not the other) is precisely the
     * shape a per-caller invalidation would risk. autotune_engine.c's own
     * accept() path re-establishes a fresh record via zones_config_set_
     * tuning_quality() immediately after this call (and after the model is
     * also persisted), so the invalidate-then-repopulate ordering never
     * leaves a stale-but-valid-looking record visible in between; every
     * OTHER caller simply leaves it invalidated, since none of them have a
     * new fit to attach. A stale quality record pinned to hand-edited gains
     * would be worse than none -- see zone_tuning_quality_t's own comment. */
    z->tuning_valid = 0;
    /* Bumped before the NVS write, not after it: the gains are already live
     * for the next control tick at this point, so a running profile must
     * re-read them (TODO.md 6A.7) whether or not the save succeeds. Note
     * this returns false on a save failure while the config change stands --
     * unchanged behaviour, and the generation reflects the in-RAM truth. */
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

/* See zones_http.h -- Phase 3 control-loop wiring's read of the fuzzy
 * adjustment-strength knob. */
bool zones_config_get_fuzzy_strength_pct(uint8_t zone_index, float *out_pct)
{
    if (!out_pct || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_pct = s_zones.cfg.zones[zone_index].fuzzy_strength_pct;
    return true;
}

/* Writer for the getter above. Same bound parse_zone_fields()'s
 * z%u_fuzzy_strength enforces (0..ZONE_FUZZY_STRENGTH_PCT_MAX) -- refused,
 * never clamped, same discipline as every other setter in this file. */
bool zones_config_set_fuzzy_strength_pct(uint8_t zone_index, float pct)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(pct) || pct < 0.0f || pct > ZONE_FUZZY_STRENGTH_PCT_MAX) {
        return false;
    }
    s_zones.cfg.zones[zone_index].fuzzy_strength_pct = pct;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

/* PID_EXPANSION_PLAN.md section 3.2 follow-up (ZONES_CFG_VERSION 14->15) --
 * see zone_cfg_t::coupling_diag_k_dc's own doc comment for what this is and
 * why it is a separate field from model_k_dc/ff_k_dc. STORAGE ONLY: no
 * control-loop consumer reads this yet, same as coupling_coeff[] was itself
 * pure storage for one pass before profile_executor.c's Phase 3b wired it
 * into the feedforward. */
bool zones_config_get_coupling_diag_k_dc(uint8_t zone_index, float *out_k_dc)
{
    if (!out_k_dc || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_k_dc = s_zones.cfg.zones[zone_index].coupling_diag_k_dc;
    return true;
}

/* Writer for the getter above. Same bound validate_zones_cfg()'s
 * coupling_diag_k_dc check enforces (0..ZONE_MODEL_K_MAX) -- refused, never
 * clamped, matching every other setter in this file. backup_http.c's import
 * needs this to round-trip the field, the same reason every other setter in
 * this file exists. */
bool zones_config_set_coupling_diag_k_dc(uint8_t zone_index, float k_dc)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(k_dc) || k_dc < 0.0f || k_dc > ZONE_MODEL_K_MAX) {
        return false;
    }
    s_zones.cfg.zones[zone_index].coupling_diag_k_dc = k_dc;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

/* Row-based (ZONES_CFG_VERSION 10->11) -- see zone_cfg_t::coupling_coeff's
 * own doc comment for what each cell means. out_row must have room for
 * MAX31856_CHANNEL_COUNT floats; the diagonal (out_row[zone_index]) is
 * always 0 on return, same "unused, stays zero" rule the storage itself
 * enforces. */
bool zones_config_get_coupling(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (!out_row || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    const zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    memcpy(out_row, z->coupling_coeff, sizeof(z->coupling_coeff));
    return true;
}

/* ZONES_CFG_VERSION 11->12 siblings of the getter above -- identical shape
 * and orientation, for coupling_tau_s[]/coupling_dead_time_s[]. */
bool zones_config_get_coupling_tau(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (!out_row || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    const zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    memcpy(out_row, z->coupling_tau_s, sizeof(z->coupling_tau_s));
    return true;
}

bool zones_config_get_coupling_dead_time(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (!out_row || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    const zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    memcpy(out_row, z->coupling_dead_time_s, sizeof(z->coupling_dead_time_s));
    return true;
}

/* Whole-row setter -- every cell checked before ANY is written, same
 * "no half-updated group" discipline as zones_config_set_model()/
 * zones_config_set_temp_limits(). Bounds match parse_zone_fields()'s
 * z%u_coupling_c%u: each off-diagonal cell finite and in
 * 0..ZONE_COUPLING_COEFF_MAX (see that macro's doc comment for why this
 * stayed non-negative rather than gaining an independent sign). The
 * diagonal MUST be exactly 0 -- a zone's response to its own heater is
 * model_k_dc, not a coupling cell, and a nonzero diagonal would be
 * ambiguous with a real (if coincidentally equal) cross-gain. */
bool zones_config_set_coupling(uint8_t zone_index, const float row[MAX31856_CHANNEL_COUNT])
{
    if (!row || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        if (!isfinite(row[j])) {
            return false;
        }
        if (j == zone_index) {
            if (row[j] != 0.0f) {
                return false;
            }
            continue;
        }
        if (row[j] < 0.0f || row[j] > ZONE_COUPLING_COEFF_MAX) {
            return false;
        }
    }
    zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    memcpy(z->coupling_coeff, row, sizeof(z->coupling_coeff));
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

/* Single-cell setter -- lets a caller update ONE neighbor's measured
 * coupling without clobbering the rest of the row's already-stored cells.
 * autotune_engine.c's finalize_fit() needs exactly this: a single relay run
 * on zone i only measures i's effect on each OTHER zone j, one cell of zone
 * j's row at a time, and must never wipe out zone j's other, previously
 * measured neighbors just because this run didn't touch them. Same bounds
 * as the whole-row setter above, applied to the one cell being written. */
bool zones_config_set_coupling_cell(uint8_t zone_index, uint8_t neighbor_index, float coeff, float tau_s,
                                     float dead_time_s)
{
    if (zone_index >= s_zones.cfg.thermo_count || neighbor_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    if (!isfinite(coeff) || !isfinite(tau_s) || !isfinite(dead_time_s)) {
        return false;
    }
    if (neighbor_index == zone_index) {
        if (coeff != 0.0f || tau_s != 0.0f || dead_time_s != 0.0f) {
            return false;
        }
        return true; /* writing the diagonal to 0 is a no-op, not an error */
    }
    if (coeff < 0.0f || coeff > ZONE_COUPLING_COEFF_MAX) {
        return false;
    }
    /* ZONES_CFG_VERSION 11->12: all-or-nothing with coeff above -- an
     * out-of-range tau_s/dead_time_s must refuse the WHOLE cell, not just
     * silently leave the two new fields unwritten while coeff still lands.
     * See zones_config_set_coupling_cell()'s own header comment. */
    if (tau_s < 0.0f || tau_s > ZONE_MODEL_TIME_MAX_S || dead_time_s < 0.0f ||
        dead_time_s > ZONE_MODEL_TIME_MAX_S) {
        return false;
    }
    zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    z->coupling_coeff[neighbor_index] = coeff;
    z->coupling_tau_s[neighbor_index] = tau_s;
    z->coupling_dead_time_s[neighbor_index] = dead_time_s;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

bool zones_config_get_settings_source(uint8_t zone_index, uint8_t *out_settings_source)
{
    if (!out_settings_source || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_settings_source = s_zones.cfg.zones[zone_index].settings_source;
    return true;
}

/* Setter for the getter above. Same rule parse_zone_fields()'s
 * z%u_settings_source enforces: either ZONE_SETTINGS_SOURCE_CUSTOM (0xFF) or
 * a real zone index < MAX31856_CHANNEL_COUNT that is NOT zone_index itself
 * (self-reference is the degenerate inheritance cycle, refused here for the
 * identical reason parse_zone_fields() refuses it) -- AND, past that, does
 * not close a longer cycle through any OTHER zone's already-stored link
 * (zones_config_json_settings_source_chain_has_cycle() on a probe copy with this one link
 * applied). Only this one link changes here, so a cycle can only be newly
 * created running THROUGH zone_index -- walking the chain starting there,
 * against every other zone's live stored value, is sufficient; it does not
 * need to check every zone. */
bool zones_config_set_settings_source(uint8_t zone_index, uint8_t settings_source)
{
    /* zone_index is bounds-checked against thermo_count, same as every
     * other per-zone setter in this file -- but thermo_count itself is only
     * ever trusted up to MAX31856_CHANNEL_COUNT elsewhere (see
     * zones_config_settings_source_import_has_cycle() and
     * zones_config_json_normalize_settings_source_cycles(), which both clamp it before using
     * it as a bound). This site did not: a corrupt thermo_count >
     * MAX31856_CHANNEL_COUNT (direct NVS tampering, or firmware that
     * predates zones_config_json_validate()'s own range check on it) would let a
     * zone_index >= MAX31856_CHANNEL_COUNT pass this check and then index
     * probe[]/s_zones.cfg.zones[] -- both fixed
     * MAX31856_CHANNEL_COUNT-sized arrays -- out of bounds on the very next
     * line, before the chain-walk below is even reached. Clamping here (not
     * just in the chain-walk's own thermo_count argument, which is a
     * separate, secondary consistency fix below) is what actually closes
     * that hole. */
    uint8_t thermo_count = s_zones.cfg.thermo_count > MAX31856_CHANNEL_COUNT ? MAX31856_CHANNEL_COUNT
                                                                             : s_zones.cfg.thermo_count;
    if (zone_index >= thermo_count) {
        return false;
    }
    if (settings_source != ZONE_SETTINGS_SOURCE_CUSTOM && settings_source >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    if (settings_source == zone_index) {
        return false;
    }
    zone_cfg_t probe[MAX31856_CHANNEL_COUNT];
    memcpy(probe, s_zones.cfg.zones, sizeof(probe));
    probe[zone_index].settings_source = settings_source;
    if (zones_config_json_settings_source_chain_has_cycle(probe, zone_index, thermo_count)) {
        return false;
    }
    s_zones.cfg.zones[zone_index].settings_source = settings_source;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

/* Commit-loop counterpart to zones_config_set_settings_source() for a
 * multi-entry import/whole-page write that has ALREADY passed
 * zones_config_settings_source_import_has_cycle() against the full proposed
 * set. That pre-check validates the FINAL assembled state; this setter skips
 * the chain-walk zones_config_set_settings_source() runs against the
 * PARTIALLY APPLIED live config while a multi-entry commit loop is
 * mid-flight, because that walk can spuriously refuse an intermediate state
 * a valid two-zone swap (e.g. live 0->1,1->0 changing to 0->2,1->CUSTOM: pass
 * 1 accepts the final state, but committing zone 0 first makes the walk see
 * live {0->2,1->0}, no cycle there -- the actual failure case is the reverse
 * order or a longer swap, see backup_http.c's restore-scenario comment)
 * without any live-config help from an in-progress commit. Only bounds and
 * self-reference are re-checked here (still real defenses against a
 * corrupt/malicious override_source entry slipping past pass 1); the cycle
 * walk itself is intentionally omitted so this call cannot fail for a
 * zone/value pair pass 1 already accepted, keeping the two-pass invariant
 * this file's import/whole-page paths depend on: pass 2 must not be able to
 * fail. Callers MUST have run zones_config_settings_source_import_has_cycle()
 * over every candidate in this commit loop first -- this function trusts
 * that check, it does not repeat it. */
bool zones_config_set_settings_source_unchecked(uint8_t zone_index, uint8_t settings_source)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (settings_source != ZONE_SETTINGS_SOURCE_CUSTOM && settings_source >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    if (settings_source == zone_index) {
        return false;
    }
    s_zones.cfg.zones[zone_index].settings_source = settings_source;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

bool zones_config_settings_source_import_has_cycle(const bool has_override[MAX31856_CHANNEL_COUNT],
                                                    const uint8_t override_source[MAX31856_CHANNEL_COUNT],
                                                    uint8_t *out_cycle_zone)
{
    zone_cfg_t probe[MAX31856_CHANNEL_COUNT];
    memcpy(probe, s_zones.cfg.zones, sizeof(probe));
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        if (has_override[i]) {
            probe[i].settings_source = override_source[i];
        }
    }
    uint8_t thermo_count = s_zones.cfg.thermo_count > MAX31856_CHANNEL_COUNT ? MAX31856_CHANNEL_COUNT
                                                                             : s_zones.cfg.thermo_count;
    for (uint8_t i = 0; i < thermo_count; i++) {
        if (zones_config_json_settings_source_chain_has_cycle(probe, i, thermo_count)) {
            if (out_cycle_zone) {
                *out_cycle_zone = i;
            }
            return true;
        }
    }
    return false;
}

bool zones_config_get_sanity_rate(uint8_t zone_index, float *out_c_per_min)
{
    if (!out_c_per_min || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_c_per_min = s_zones.cfg.zones[zone_index].sanity_rate_c_per_min;
    return true;
}

/* Same bound parse_zone_fields()'s z%u_sanity enforces. 0 is legal (the
 * documented "never configured" encoding). */
bool zones_config_set_sanity_rate(uint8_t zone_index, float c_per_min)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(c_per_min) || c_per_min < 0.0f || c_per_min > ZONE_SANITY_RATE_MAX_C_PER_MIN) {
        return false;
    }
    s_zones.cfg.zones[zone_index].sanity_rate_c_per_min = c_per_min;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

bool zones_config_get_control_mode(uint8_t zone_index, zone_control_mode_t *out_mode)
{
    if (!out_mode || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_mode = (zone_control_mode_t)s_zones.cfg.zones[zone_index].control_mode;
    return true;
}

/* Same bound parse_zone_fields()'s z%u_mode enforces (0-3). */
bool zones_config_set_control_mode(uint8_t zone_index, zone_control_mode_t mode)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if ((unsigned)mode > (unsigned)ZONE_CONTROL_MODE_PID_FUZZY) {
        return false;
    }
    s_zones.cfg.zones[zone_index].control_mode = (uint8_t)mode;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

bool zones_config_get_temp_limits(uint8_t zone_index, float *out_max_temp_c, float *out_min_temp_c)
{
    if (!out_max_temp_c || !out_min_temp_c || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    const zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    *out_max_temp_c = z->max_temp_c;
    *out_min_temp_c = z->min_temp_c;
    return true;
}

/* Bundled setter -- see zones_http.h's comment on this pair for why NO
 * max_temp_c >= min_temp_c cross-check is added here: parse_zone_fields()
 * (the POST /api/zones authority) does not enforce one either, so this
 * setter matches it exactly rather than becoming stricter than the page it
 * mirrors. Both fields checked before either is written, same
 * reject-nothing-half-applied discipline as zones_config_set_model(). */
bool zones_config_set_temp_limits(uint8_t zone_index, float max_temp_c, float min_temp_c)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(max_temp_c) || max_temp_c < 0.0f || max_temp_c > ZONE_MAX_TEMP_C_MAX) {
        return false;
    }
    if (!isfinite(min_temp_c) || min_temp_c < ZONE_MIN_TEMP_C_MIN || min_temp_c > ZONE_MIN_TEMP_C_MAX) {
        return false;
    }
    zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    z->max_temp_c = max_temp_c;
    z->min_temp_c = min_temp_c;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

bool zones_config_get_heater_cfg(uint8_t zone_index, float *out_window_ms, float *out_min_on_ms,
                                 float *out_min_off_ms)
{
    if (!out_window_ms || !out_min_on_ms || !out_min_off_ms || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    const zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    *out_window_ms = z->heater_window_ms;
    *out_min_on_ms = z->heater_min_on_ms;
    *out_min_off_ms = z->heater_min_off_ms;
    return true;
}

/* Bundled setter, same "no half-updated group" discipline as
 * zones_config_set_model()/zones_config_set_temp_limits(). No
 * min_on_ms/min_off_ms-vs-window_ms cross-check: parse_zone_fields() (the
 * POST authority) does not enforce one either -- see
 * zones_config_set_temp_limits()'s comment for the identical reasoning. */
bool zones_config_set_heater_cfg(uint8_t zone_index, float window_ms, float min_on_ms, float min_off_ms)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(window_ms) || window_ms < 0.0f || window_ms > ZONE_HEATER_WINDOW_MS_MAX) {
        return false;
    }
    if (!isfinite(min_on_ms) || min_on_ms < 0.0f || min_on_ms > ZONE_HEATER_MIN_ON_OFF_MS_MAX) {
        return false;
    }
    /* Same disjoint rule parse_zone_fields() enforces -- this setter is a
     * second door into the same field (it is reachable without going through
     * a POST body) and must not be the loose one. */
    if (min_on_ms > 0.0f && min_on_ms < ZONE_HEATER_MIN_ON_MS_FLOOR) {
        return false;
    }
    if (!isfinite(min_off_ms) || min_off_ms < 0.0f || min_off_ms > ZONE_HEATER_MIN_ON_OFF_MS_MAX) {
        return false;
    }
    /* Window-vs-min-on relationship, same disjoint rule parse_zone_fields()
     * enforces. This setter is the door backup import comes through, and a
     * backup written before 2026-08-29 can easily carry a 2000 ms window. */
    if (window_ms > 0.0f && window_ms < zone_required_window_ms(min_on_ms)) {
        return false;
    }
    zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    z->heater_window_ms = window_ms;
    z->heater_min_on_ms = min_on_ms;
    z->heater_min_off_ms = min_off_ms;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

bool zones_config_get_guard_thresholds(uint8_t zone_index, float *out_wrong_dir_window_s,
                                       float *out_wrong_dir_rate_c_per_min, float *out_off_settle_s,
                                       float *out_runaway_rate_c_per_min, float *out_runaway_margin_c,
                                       float *out_drift_period_s, float *out_sensor_fault_debounce_ticks,
                                       float *out_frozen_window_s)
{
    if (!out_wrong_dir_window_s || !out_wrong_dir_rate_c_per_min || !out_off_settle_s ||
        !out_runaway_rate_c_per_min || !out_runaway_margin_c || !out_drift_period_s ||
        !out_sensor_fault_debounce_ticks || !out_frozen_window_s || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    const zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    *out_wrong_dir_window_s = z->guard_wrong_dir_window_s;
    *out_wrong_dir_rate_c_per_min = z->guard_wrong_dir_rate_c_per_min;
    *out_off_settle_s = z->guard_off_settle_s;
    *out_runaway_rate_c_per_min = z->guard_runaway_rate_c_per_min;
    *out_runaway_margin_c = z->guard_runaway_margin_c;
    *out_drift_period_s = z->guard_drift_period_s;
    *out_sensor_fault_debounce_ticks = z->guard_sensor_fault_debounce_ticks;
    *out_frozen_window_s = z->guard_frozen_window_s;
    return true;
}

/* The five per-zone guard overrides added in v8 (see zone_cfg_t). Bundled for
 * the same reason the 8 above are: thermal_guard.c reads them as one group
 * when it builds a thermal_guard_cfg_t. Every one keeps the "0 = use the
 * module's named default" convention, so this getter reports the stored value
 * verbatim and the substitution stays where it belongs -- in the module that
 * owns the default.
 *
 * SIGNATURE UNCHANGED by ZONES_CFG_VERSION 9 (2026-08-27, timing profiles --
 * see zones_http.h's own note on this pair): zone_index now resolves to
 * zone_cfg_t::timing_profile first, then to that slot in
 * zones_cfg_t::timing_profiles[], instead of reading the nine fields directly
 * off zone_cfg_t -- neither caller had to change. */
bool zones_config_get_guard_extra(uint8_t zone_index, float *out_progress_duty_min,
                                  float *out_progress_window_s, float *out_drift_hysteresis_c,
                                  float *out_frozen_eps_c, float *out_cross_zone_period_s)
{
    if (!out_progress_duty_min || !out_progress_window_s || !out_drift_hysteresis_c ||
        !out_frozen_eps_c || !out_cross_zone_period_s || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    uint8_t p = s_zones.cfg.zones[zone_index].timing_profile;
    /* Defensive, not expected in practice: every path that can write
     * s_zones.cfg (POST /api/zones, NVS load via zones_config_json_validate(), a
     * kiln-config import) already bounds timing_profile against
     * timing_profile_count before it is ever stored. A getter must still
     * never read past the array on the strength of that alone. */
    if (p >= s_zones.cfg.timing_profile_count) {
        return false;
    }
    const zone_timing_profile_t *tp = &s_zones.cfg.timing_profiles[p];
    *out_progress_duty_min = tp->guard_progress_duty_min;
    *out_progress_window_s = tp->guard_progress_window_s;
    *out_drift_hysteresis_c = tp->guard_drift_hysteresis_c;
    *out_frozen_eps_c = tp->guard_frozen_eps_c;
    *out_cross_zone_period_s = tp->guard_cross_zone_period_s;
    return true;
}

/* The four per-zone executor overrides added in v8 -- profile_executor.c's
 * side of the same pass. Same 0-means-default convention, and the same
 * "signature unchanged, only the internal lookup moved to timing_profiles[]"
 * note as zones_config_get_guard_extra() above applies here too. */
bool zones_config_get_executor_thresholds(uint8_t zone_index, float *out_bangbang_hysteresis_c,
                                          float *out_cooling_limited_margin_c,
                                          float *out_cooling_limited_hold_s, float *out_ramp_lock_band_c)
{
    if (!out_bangbang_hysteresis_c || !out_cooling_limited_margin_c || !out_cooling_limited_hold_s ||
        !out_ramp_lock_band_c || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    uint8_t p = s_zones.cfg.zones[zone_index].timing_profile;
    if (p >= s_zones.cfg.timing_profile_count) {
        return false; /* defensive -- see zones_config_get_guard_extra()'s identical comment */
    }
    const zone_timing_profile_t *tp = &s_zones.cfg.timing_profiles[p];
    *out_bangbang_hysteresis_c = tp->bangbang_hysteresis_c;
    *out_cooling_limited_margin_c = tp->cooling_limited_margin_c;
    *out_cooling_limited_hold_s = tp->cooling_limited_hold_s;
    *out_ramp_lock_band_c = tp->ramp_lock_band_c;
    return true;
}

/* The one global v8 override. Not gated on thermo_count -- the PC link exists
 * whether or not a single zone has been configured. */
bool zones_config_get_pc_link_abort_silence_ms(float *out_ms)
{
    if (!out_ms) {
        return false;
    }
    *out_ms = s_zones.cfg.pc_link_abort_silence_ms;
    return true;
}

/* ZONES_CFG_VERSION 16->17: PER-ZONE as of this pass (was a single global
 * scalar at 15->16 -- see zone_cfg_t::ease_off_window_mult's own comment for
 * why z0 needed its own reach). Gated on MAX31856_CHANNEL_COUNT, not
 * thermo_count -- same reasoning as zones_config_get_executor_thresholds()
 * above: a caller with a valid zone index should get an answer even for a
 * zone past the currently-configured count, and every other per-zone
 * accessor in this file already follows that rule.
 *
 * Unlike a bare field read, this one is defensive: s_zones.cfg.zones[zone_
 * index].ease_off_window_mult reads 0.0f in the zeroed, not-yet-configured
 * s_zones.cfg (first boot, corrupt NVS, a refused newer-than-firmware blob --
 * see zones_http.c's own zeroing on load failure) -- 0 IS a legal, validated
 * value (the same "use the firmware default" sentinel pc_link_abort_
 * silence_ms uses, see ZONE_EASE_OFF_WINDOW_MULT_MIN/MAX/DEFAULT's own
 * comment), but it is not a value profile_executor_feedforward.c's
 * zone_taper_climb_rate() can safely divide by -- THIS is where the sentinel
 * actually gets resolved into the real 2.0 default, not at storage time.
 * Falling back to the default for anything else outside [MIN, MAX] too (not
 * just exactly 0) covers a hypothetical stored value from before this range
 * was enforced -- always answer with a value zone_taper_climb_rate() can
 * safely use, never with whatever raw bytes happen to be sitting in
 * s_zones.cfg. */
bool zones_config_get_ease_off_window_mult(uint8_t zone_index, float *out_mult)
{
    if (!out_mult || zone_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    float v = s_zones.cfg.zones[zone_index].ease_off_window_mult;
    if (v == 0.0f) {
        v = ZONE_EASE_OFF_WINDOW_MULT_DEFAULT; /* the sentinel */
    } else if (!isfinite(v) || v < ZONE_EASE_OFF_WINDOW_MULT_MIN || v > ZONE_EASE_OFF_WINDOW_MULT_MAX) {
        v = ZONE_EASE_OFF_WINDOW_MULT_DEFAULT; /* defensive: not a value that should ever be on flash */
    }
    *out_mult = v;
    return true;
}

/* Writer for the getter above. Refused, never clamped, matching every other
 * setter in this file (e.g. zones_config_set_coupling_diag_k_dc() just
 * below) -- an A/B campaign that asks for an out-of-range multiplier needs
 * to find out its request was rejected, not silently get a different number
 * substituted for it. 0 is accepted (same sentinel zones_config_json_
 * validate() allows) as an explicit "reset to the firmware default" -- an
 * A/B campaign ending an arm should be able to ask for that directly rather
 * than having to know and pass 2.0 by hand. Setting one zone's value never
 * touches any other zone's -- that independence is the entire point of this
 * pass. */
bool zones_config_set_ease_off_window_mult(uint8_t zone_index, float mult)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT || !isfinite(mult) ||
        (mult != 0.0f && (mult < ZONE_EASE_OFF_WINDOW_MULT_MIN || mult > ZONE_EASE_OFF_WINDOW_MULT_MAX))) {
        return false;
    }
    s_zones.cfg.zones[zone_index].ease_off_window_mult = mult;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

/* ZONES_CFG_VERSION 17->18 (PID_EXPANSION_PLAN.md sec 3.6d / PER_ZONE_
 * TARGET_DESIGN_STUDY.md option (b)): per-zone approach-rate cap. Gated on
 * MAX31856_CHANNEL_COUNT, same reasoning as zones_config_get_ease_off_
 * window_mult() just above.
 *
 * UNLIKE that getter, 0 is returned VERBATIM here, not resolved into some
 * other in-range value -- 0 IS "uncapped," the field's own documented
 * meaning (see ZONE_APPROACH_RATE_CAP_C_PER_HR_MIN's own comment for why
 * there is no sensible default cap to substitute the way ease_off_window_
 * mult substitutes 2.0). A stored value that is finite, nonzero, and
 * outside [MIN, MAX] -- only reachable via direct NVS tampering or a
 * rollback from firmware with a wider range -- is defensively treated as
 * uncapped (0), the safe answer for THIS field, rather than clamped into
 * range (which would silently narrow the shared ramp for a value nobody
 * actually configured). */
bool zones_config_get_approach_rate_cap_c_per_hr(uint8_t zone_index, float *out_cap_c_per_hr)
{
    if (!out_cap_c_per_hr || zone_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    float v = s_zones.cfg.zones[zone_index].approach_rate_cap_c_per_hr;
    if (v != 0.0f && (!isfinite(v) || v < ZONE_APPROACH_RATE_CAP_C_PER_HR_MIN ||
                       v > ZONE_APPROACH_RATE_CAP_C_PER_HR_MAX)) {
        v = 0.0f; /* defensive: not a value that should ever be on flash -- treat as uncapped */
    }
    *out_cap_c_per_hr = v;
    return true;
}

/* Writer for the getter above. Refused, never clamped, matching every other
 * setter in this file. 0 is accepted as an explicit "remove this zone's cap"
 * -- an A/B campaign ending an arm should be able to ask for that directly.
 * Setting one zone's value never touches any other zone's. */
bool zones_config_set_approach_rate_cap_c_per_hr(uint8_t zone_index, float cap_c_per_hr)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT || !isfinite(cap_c_per_hr) ||
        (cap_c_per_hr != 0.0f && (cap_c_per_hr < ZONE_APPROACH_RATE_CAP_C_PER_HR_MIN ||
                                   cap_c_per_hr > ZONE_APPROACH_RATE_CAP_C_PER_HR_MAX))) {
        return false;
    }
    s_zones.cfg.zones[zone_index].approach_rate_cap_c_per_hr = cap_c_per_hr;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

/* ZONES_CFG_VERSION 18->19 (PID_EXPANSION_PLAN.md sec 3.6g): per-zone
 * fuzzy-PID membership-band widths. Gated on MAX31856_CHANNEL_COUNT, same
 * reasoning as zones_config_get_ease_off_window_mult() above.
 *
 * Like that getter (and UNLIKE zones_config_get_approach_rate_cap_c_per_hr()
 * just above), 0 -- and anything else outside [MIN, MAX] -- resolves to the
 * field's own firmware DEFAULT: there is no "no band" answer a fuzzy
 * membership function can give the way "uncapped" is a real answer for a
 * rate limiter, so this getter always hands the caller a finite, positive
 * width it can pass straight to pid_fuzzy_adjust(). */
bool zones_config_get_error_band_c(uint8_t zone_index, float *out_band_c)
{
    if (!out_band_c || zone_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    float v = s_zones.cfg.zones[zone_index].error_band_c;
    if (v == 0.0f) {
        v = ZONE_ERROR_BAND_C_DEFAULT; /* the sentinel */
    } else if (!isfinite(v) || v < ZONE_ERROR_BAND_C_MIN || v > ZONE_ERROR_BAND_C_MAX) {
        v = ZONE_ERROR_BAND_C_DEFAULT; /* defensive: not a value that should ever be on flash */
    }
    *out_band_c = v;
    return true;
}

/* Writer for the getter above. Refused, never clamped, matching every other
 * setter in this file. 0 is accepted as an explicit "reset to the firmware
 * default." Setting one zone's value never touches any other zone's. */
bool zones_config_set_error_band_c(uint8_t zone_index, float band_c)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT || !isfinite(band_c) ||
        (band_c != 0.0f && (band_c < ZONE_ERROR_BAND_C_MIN || band_c > ZONE_ERROR_BAND_C_MAX))) {
        return false;
    }
    s_zones.cfg.zones[zone_index].error_band_c = band_c;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

/* Same shape as zones_config_get_error_band_c()/_set_error_band_c() just
 * above, for the rate axis. */
bool zones_config_get_rate_band_c_per_s(uint8_t zone_index, float *out_band_c_per_s)
{
    if (!out_band_c_per_s || zone_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    float v = s_zones.cfg.zones[zone_index].rate_band_c_per_s;
    if (v == 0.0f) {
        v = ZONE_RATE_BAND_C_PER_S_DEFAULT; /* the sentinel */
    } else if (!isfinite(v) || v < ZONE_RATE_BAND_C_PER_S_MIN || v > ZONE_RATE_BAND_C_PER_S_MAX) {
        v = ZONE_RATE_BAND_C_PER_S_DEFAULT; /* defensive: not a value that should ever be on flash */
    }
    *out_band_c_per_s = v;
    return true;
}

bool zones_config_set_rate_band_c_per_s(uint8_t zone_index, float band_c_per_s)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT || !isfinite(band_c_per_s) ||
        (band_c_per_s != 0.0f &&
         (band_c_per_s < ZONE_RATE_BAND_C_PER_S_MIN || band_c_per_s > ZONE_RATE_BAND_C_PER_S_MAX))) {
        return false;
    }
    s_zones.cfg.zones[zone_index].rate_band_c_per_s = band_c_per_s;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

/* Bundled setter, same "reject nothing half-written" discipline as every
 * bundled setter above. Each of the 8 fields checked against its own
 * independent bound (matching which ceiling parse_zone_fields() applies to
 * that specific key) -- no cross-field check between any pair of these 8,
 * matching the POST authority's own lack of one. */
bool zones_config_set_guard_thresholds(uint8_t zone_index, float wrong_dir_window_s,
                                       float wrong_dir_rate_c_per_min, float off_settle_s,
                                       float runaway_rate_c_per_min, float runaway_margin_c,
                                       float drift_period_s, float sensor_fault_debounce_ticks,
                                       float frozen_window_s)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(wrong_dir_window_s) || wrong_dir_window_s < 0.0f || wrong_dir_window_s > ZONE_GUARD_TIME_S_MAX) {
        return false;
    }
    if (!isfinite(wrong_dir_rate_c_per_min) || wrong_dir_rate_c_per_min < 0.0f ||
        wrong_dir_rate_c_per_min > ZONE_GUARD_RATE_C_PER_MIN_MAX) {
        return false;
    }
    if (!isfinite(off_settle_s) || off_settle_s < 0.0f || off_settle_s > ZONE_GUARD_TIME_S_MAX) {
        return false;
    }
    if (!isfinite(runaway_rate_c_per_min) || runaway_rate_c_per_min < 0.0f ||
        runaway_rate_c_per_min > ZONE_GUARD_RATE_C_PER_MIN_MAX) {
        return false;
    }
    if (!isfinite(runaway_margin_c) || runaway_margin_c < 0.0f || runaway_margin_c > ZONE_GUARD_MARGIN_C_MAX) {
        return false;
    }
    if (!isfinite(drift_period_s) || drift_period_s < 0.0f || drift_period_s > ZONE_GUARD_TIME_S_MAX) {
        return false;
    }
    if (!isfinite(sensor_fault_debounce_ticks) || sensor_fault_debounce_ticks < 0.0f ||
        sensor_fault_debounce_ticks > ZONE_GUARD_DEBOUNCE_TICKS_MAX) {
        return false;
    }
    if (!isfinite(frozen_window_s) || frozen_window_s < 0.0f || frozen_window_s > ZONE_GUARD_TIME_S_MAX) {
        return false;
    }
    zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    z->guard_wrong_dir_window_s = wrong_dir_window_s;
    z->guard_wrong_dir_rate_c_per_min = wrong_dir_rate_c_per_min;
    z->guard_off_settle_s = off_settle_s;
    z->guard_runaway_rate_c_per_min = runaway_rate_c_per_min;
    z->guard_runaway_margin_c = runaway_margin_c;
    z->guard_drift_period_s = drift_period_s;
    z->guard_sensor_fault_debounce_ticks = sensor_fault_debounce_ticks;
    z->guard_frozen_window_s = frozen_window_s;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

bool zones_config_get_cross_zone_delta(uint8_t zone_index, float *out_max_delta_c)
{
    if (!out_max_delta_c || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_max_delta_c = s_zones.cfg.zones[zone_index].cross_zone_max_delta_c;
    return true;
}

/* Same bound parse_zone_fields()'s z%u_xzone enforces. 0 is legal (the
 * documented "guard disabled" encoding). */
bool zones_config_set_cross_zone_delta(uint8_t zone_index, float max_delta_c)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(max_delta_c) || max_delta_c < 0.0f || max_delta_c > ZONE_CROSS_ZONE_DELTA_C_MAX) {
        return false;
    }
    s_zones.cfg.zones[zone_index].cross_zone_max_delta_c = max_delta_c;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

bool zones_config_get_model(uint8_t zone_index, float *out_k_dc, float *out_tau_s, float *out_dead_time_s)
{
    if (!out_k_dc || !out_tau_s || !out_dead_time_s || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    const zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    *out_k_dc = z->model_k_dc;
    *out_tau_s = z->model_tau_s;
    *out_dead_time_s = z->model_dead_time_s;
    return true;
}

bool zones_config_set_model(uint8_t zone_index, float k_dc, float tau_s, float dead_time_s)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    /* Same reject-without-writing-anything discipline as
     * zones_config_set_pid(): a caller handing us one bad number must not
     * end up with two of the three fields updated, because a half-written
     * model is indistinguishable from a whole one to every reader and would
     * feed the feedforward term a gain from one run and a tau from another.
     *
     * Note this deliberately accepts an all-zero triple: that is the
     * documented "no model" encoding, so writing it is how a caller clears a
     * stale model rather than a validation failure. A NEGATIVE value is
     * rejected outright -- a heater with negative static gain, or a plant
     * that responds before it is driven, is a fit that went wrong, not a
     * kiln. */
    if (!isfinite(k_dc) || !isfinite(tau_s) || !isfinite(dead_time_s) || k_dc < 0.0f || tau_s < 0.0f ||
        dead_time_s < 0.0f || k_dc > ZONE_MODEL_K_MAX || tau_s > ZONE_MODEL_TIME_MAX_S ||
        dead_time_s > ZONE_MODEL_TIME_MAX_S) {
        return false;
    }
    zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    z->model_k_dc = k_dc;
    z->model_tau_s = tau_s;
    z->model_dead_time_s = dead_time_s;
    /* Bumped before the NVS write for the same reason set_pid does it: the
     * model is live for the next control tick regardless of whether it
     * reaches flash, and a feedforward term computed from a stale K while a
     * firing is running is precisely what TODO.md 6A.7's reload path
     * exists to prevent. */
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

bool zones_config_get_tuning_quality(uint8_t zone_index, zone_tuning_quality_t *out)
{
    if (!out || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    const zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    out->valid = z->tuning_valid != 0;
    out->method = z->tuning_method;
    out->rule = z->tuning_rule;
    out->settled = z->tuning_settled != 0;
    out->extrapolation_converged = z->tuning_extrapolation_converged != 0;
    out->tau_consistent = z->tuning_tau_consistent != 0;
    out->baseline_c = z->tuning_baseline_c;
    out->step_ambient_c = z->tuning_step_ambient_c;
    out->raw_rise_c = z->tuning_raw_rise_c;
    out->rise_inf_c = z->tuning_rise_inf_c;
    return true;
}

bool zones_config_set_tuning_quality(uint8_t zone_index, const zone_tuning_quality_t *q)
{
    /* q->valid must be true -- see this setter's own header comment
     * (zones_http.h) for why a caller wanting to CLEAR the record uses
     * zones_config_set_pid() instead, and why this setter refuses rather
     * than silently writing a "set but empty" record. */
    if (!q || !q->valid || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(q->baseline_c) || !isfinite(q->step_ambient_c) || !isfinite(q->raw_rise_c) ||
        !isfinite(q->rise_inf_c) || q->method > 1 || q->rule > 3) {
        return false;
    }
    zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    z->tuning_valid = 1;
    z->tuning_method = q->method;
    z->tuning_rule = q->rule;
    z->tuning_settled = q->settled ? 1 : 0;
    z->tuning_extrapolation_converged = q->extrapolation_converged ? 1 : 0;
    z->tuning_tau_consistent = q->tau_consistent ? 1 : 0;
    z->tuning_baseline_c = q->baseline_c;
    z->tuning_step_ambient_c = q->step_ambient_c;
    z->tuning_raw_rise_c = q->raw_rise_c;
    z->tuning_rise_inf_c = q->rise_inf_c;
    /* Self-incrementing -- the caller does not supply a seq, see this
     * function's own header comment (zones_http.h) for why. */
    z->tuning_seq++;
    s_config_generation++;
    return nvs_save() == ESP_OK;
}

float zones_config_apply_cal(uint8_t zone_index, float raw_c)
{
    if (isnan(raw_c) || zone_index >= s_zones.cfg.thermo_count) {
        return raw_c;
    }
    return raw_c + s_zones.cfg.zones[zone_index].cal_offset_c;
}

/* ---- Whole-config export/import for kiln_cfg_store.c (see zones_http.h's
 * doc comment on this pair for the full rationale) -------------------------- */

_Static_assert(sizeof(zones_cfg_t) <= ZONES_CONFIG_BLOB_MAX_SIZE,
               "zones_cfg_t grew past ZONES_CONFIG_BLOB_MAX_SIZE -- widen the macro in "
               "zones_http.h (existing kiln_cfg_store entries keep their old, smaller blob "
               "size until re-saved, same discipline as ZONES_CFG_VERSION migrations)");

size_t zones_config_blob_size(void)
{
    return sizeof(zones_cfg_t);
}

bool zones_config_export_blob(void *out, size_t out_cap)
{
    if (!out || out_cap < sizeof(s_zones.cfg)) {
        return false;
    }
    memcpy(out, &s_zones.cfg, sizeof(s_zones.cfg));
    return true;
}

bool zones_config_import_blob(const void *blob, size_t len, char *reason_out, size_t reason_cap)
{
    if (reason_out && reason_cap) {
        reason_out[0] = '\0';
    }

    /* Same decoder nvs_load_from() uses -- length-vs-claimed-version check
     * before anything is interpreted, typed per-version conversion (never a
     * memcpy of one struct shape over another), zones_config_json_validate(), and a
     * CRC check on the current-version path. This blob may have been saved
     * years ago by older firmware under looser bounds (kiln_cfg_store.c), so
     * it gets exactly the same scrutiny a blob read off flash does -- no
     * separate, looser path for "this one came from a kiln config slot
     * instead of the live NVS key." */
    zones_cfg_t cand;
    const char *reason = "";
    zones_decode_result_t result = zones_config_json_decode_blob(blob, len, &cand, &reason);
    if (result != ZONES_DECODE_OK) {
        if (reason_out && reason_cap) {
            snprintf(reason_out, reason_cap, "%s", reason);
        }
        return false;
    }

    /* Pass 1 for settings_source: zones_config_json_decode_blob()/zones_config_json_validate()
     * do not check for an inheritance cycle (they're shared with the LOAD
     * path, which must collapse rather than reject one -- see
     * zones_config_json_normalize_settings_source_cycles()'s comment). The import path is
     * different: there is a live client on the other end of this call
     * (backup_http.c's importer) who can be handed a clear reason, so this
     * is refused HERE, before the commit point below, matching this file's
     * own two-pass-import discipline (reject in pass 1, never fail
     * mid-commit) rather than silently rewriting a hand-edited backup's
     * cycle to Custom underneath the operator. */
    {
    uint8_t import_thermo_count = cand.thermo_count > MAX31856_CHANNEL_COUNT ? MAX31856_CHANNEL_COUNT
                                                                             : cand.thermo_count;
    for (uint8_t i = 0; i < import_thermo_count; i++) {
        if (zones_config_json_settings_source_chain_has_cycle(cand.zones, i, import_thermo_count)) {
            if (reason_out && reason_cap) {
                snprintf(reason_out, reason_cap,
                         "zone %u's settings_source forms an inheritance cycle", (unsigned)i);
            }
            return false;
        }
    }
    }

    /* Commit point -- everything above only touched `cand`, a local scratch
     * copy; nothing has been written to s_zones or NVS until this line, so
     * any rejection above (bad version, bad size, any one field out of
     * range) leaves the live config completely untouched. Same
     * all-or-nothing discipline as zones_post_handler()'s own commit
     * point. */
    s_zones.cfg = cand;
    s_zones_config_valid = true;
    s_config_generation++;
    esp_err_t err = nvs_save();
    if (err != ESP_OK) {
        ESP_LOGE(ZONES_HTTP_TAG, "nvs_save after kiln-config apply failed: %s -- config applied live but "
                      "will not survive a reboot",
                 esp_err_to_name(err));
    }
    return true;
}

