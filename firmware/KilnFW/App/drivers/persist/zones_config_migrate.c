// Version-dispatch migration and the public decode entry point for
// zones_config_json.c's split (ROADMAP.md M15, the 1500-line rule) -- see
// zones_config_json_internal.h's own doc comment for the full three-file map.
#include "zones_config_json_internal.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_crc.h"
#include "esp_log.h"
#include "persist_scratch.h"

/* ZONES_CFG_VERSION 20->21 (docs/ARCHITECTURE_DECISIONS.md#zones-page-clean-up-info-disclosure-schema-v20-v21-chartjs) widened settings_source
 * from a single byte to settings_source[SRC_GROUP_COUNT] -- a MID-STRUCT
 * field, unlike every migration since v12->13, which only ever appended a
 * new field at the true tail. Every "predates X, byte-for-byte identical
 * prefix" per-zone raw memcpy in cases 15..19 below relied on that
 * appended-at-the-tail property to be a valid whole-element memcpy; it no
 * longer is, now that everything from settings_source onward has shifted.
 *
 * This helper does the same job those raw memcpys used to, in two pieces
 * instead of one: (1) the part of the layout that is IDENTICAL in every
 * historical version from v12 through v20 -- name..settings_source's OWN
 * first byte, i.e. everything up to but not including settings_source
 * itself -- copied as one block, then settings_source fanned out to every
 * SRC_GROUP_COUNT group; (2) the part that comes immediately AFTER
 * settings_source -- tuning_valid onward -- which is ALSO byte-for-byte
 * identical in relative order/type between any vN_t (N>=12) and the current
 * zone_cfg_t (just possibly shorter, missing whatever the current struct
 * grew after that historical version), copied as a second block sized to
 * exactly how much the SOURCE struct actually has past that point. Every
 * offset is taken from the CALLER's own vN_t type via offsetof(), never
 * hardcoded, so this stays correct even if an earlier field's size ever
 * changes. */
static void zone_cfg_migrate_prefix_and_tail(zone_cfg_t *d, const void *src_zone, size_t src_zone_size,
                                              size_t src_settings_source_off, size_t src_tuning_valid_off)
{
    memcpy(d, src_zone, src_settings_source_off);
    uint8_t old_scalar = ((const uint8_t *)src_zone)[src_settings_source_off];
    for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
        d->settings_source[g] = old_scalar;
    }
    size_t tail_len = src_zone_size - src_tuning_valid_off;
    memcpy((uint8_t *)d + offsetof(zone_cfg_t, tuning_valid), (const uint8_t *)src_zone + src_tuning_valid_off,
           tail_len);
}

/* `scratch` (sizeof(zones_cfg_t) bytes, the largest layout) holds the historical
 * vN_t copy each case decodes from; heap, not stack, because this runs on the
 * shared httpd stack via nvs_load (check_httpd_task_stack_budget). */
/* The one D1 step (docs/CONFIG_MIGRATION_CHAIN.md sec 0.2, 4.1): v26 -> v27
 * (2026-10-16). tc_sheath_tau_s is new and occupies the two padding bytes v26
 * had between failsafe_state and hyst_c, so zones_cfg_v26_t and zones_cfg_t
 * have the same size and every other offset. Field copy, then the new field
 * is zeroed EXPLICITLY: those bytes were padding on v26, not guaranteed zero,
 * and a stray value there must never turn into a lead term. 0 is the "no
 * protection tube" default: compensation off, control identical to v26.
 * The caller has already checked the v26 blob's own CRC. */
_Static_assert(sizeof(zones_cfg_v26_t) == sizeof(zones_cfg_t),
               "v26 -> v27 only fills padding; the wrapper size must not change");
_Static_assert(sizeof(zone_cfg_v26_t) == sizeof(zone_cfg_t),
               "v26 -> v27 only fills padding; the per-zone size must not change");
static bool zones_cfg_step_v26_to_v27(const void *blob, zones_cfg_t *out, void *scratch)
{
    zones_cfg_v26_t *src = (zones_cfg_v26_t *)scratch; /* scratch, not the stack */
    memcpy(src, blob, sizeof(*src));
    out->thermo_count = src->thermo_count;
    out->relay_count = src->relay_count;
    out->max_simultaneous_relays = src->max_simultaneous_relays;
    out->continue_on_zone_trip = src->continue_on_zone_trip;
    out->safety_tc_type = src->safety_tc_type;
    out->pc_link_abort_silence_ms = src->pc_link_abort_silence_ms;
    out->timing_profile_count = src->timing_profile_count;
    memcpy(out->timing_profiles, src->timing_profiles, sizeof(out->timing_profiles));
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        memcpy(&out->zones[i], &src->zones[i], sizeof(src->zones[i]));
        out->zones[i].tc_sheath_tau_s = 0;
    }
    return true;
}

static bool convert_versioned_blob_to_current_impl(uint8_t version, const void *blob, zones_cfg_t *out,
                                                    void *scratch)
{
    memset(out, 0, sizeof(*out));
    /* ZONES_CFG_VERSION 16->17: ease_off_window_mult is now PER-ZONE
     * (zone_cfg_t::ease_off_window_mult), not a single field set once here
     * ahead of the switch the way the removed global scalar's default used
     * to be. Every convert_zone_v*() helper below already memset()s its
     * destination zone_cfg_t to 0 before filling in the fields its source
     * layout actually has, and 0 IS this field's documented "use the
     * firmware default" sentinel (see ZONE_EASE_OFF_WINDOW_MULT_MIN/MAX/
     * DEFAULT's own comment) -- so every zone of every pre-v16 blob lands on
     * the sentinel automatically, which zones_config_get_ease_off_window_
     * mult() resolves to the real 2.0 default, EXACTLY the value the old
     * removed #define always was. Only case 16 (the actual v16->v17
     * migration, below) needs an explicit per-zone assignment, since that is
     * the one version that stored a real, possibly non-default, opinion
     * (the global scalar) that must be carried forward verbatim rather than
     * defaulted.
     *
     * ZONES_CFG_VERSION 17->18: approach_rate_cap_c_per_hr is a BRAND NEW
     * mechanism (PID_EXPANSION_PLAN.md sec 3.6d option (b)) -- unlike
     * ease_off_window_mult, no prior version ever stored an equivalent
     * global scalar to carry forward, so EVERY case below (1 through 17
     * inclusive) needs nothing extra: this function's entry memset already
     * zeroes the field, and 0 is this field's own "uncapped" sentinel (see
     * ZONE_APPROACH_RATE_CAP_C_PER_HR_MIN's own comment) -- exactly today's
     * behaviour, unchanged, for every zone of every upgrading board.
     *
     * ZONES_CFG_VERSION 18->19: error_band_c/rate_band_c_per_s are ALSO
     * brand new (PID_EXPANSION_PLAN.md sec 3.6g) -- same "no prior global
     * scalar to carry forward" shape as approach_rate_cap_c_per_hr just
     * above, not ease_off_window_mult's "carry the removed global verbatim"
     * shape, so again EVERY case below needs nothing extra: this function's
     * entry memset already zeroes both fields, and 0 IS each field's
     * documented "use the firmware default" sentinel (see ZONE_ERROR_BAND_
     * C_MIN/MAX/DEFAULT's own comment) -- zones_config_get_error_band_c()/
     * zones_config_get_rate_band_c_per_s() resolve that sentinel to 20.0f/
     * 0.5f, bit-identical to the removed ERROR_BAND_C/RATE_BAND_C_PER_S
     * #defines, for every zone of every upgrading board.
     *
     * ZONES_CFG_VERSION 19->20: relay_type is ALSO brand new
     * (RELAY_LIFE_BUDGET.md) -- same "no prior global scalar to
     * carry forward" shape as approach_rate_cap_c_per_hr/error_band_c/
     * rate_band_c_per_s above, so again EVERY case below needs nothing
     * extra: this function's entry memset already zeroes the field, and 0
     * (RELAY_TYPE_SSR) is exactly what every existing board's heater relays
     * already are. zones_config_push_all_relay_types() (zones_config_
     * store.c) pushes the migrated value out to relay_cycles.c right after
     * this function returns, same as it does for a same-version load. */
    switch (version) {
    /* Cases 1..18 were deleted at the 26->27 bump (docs/CONFIG_MIGRATION_CHAIN.md
     * sec 4.2, D2): ZONES_CFG_MIGRATION_EXPIRY_FLOOR (ZONES_CFG_VERSION - 8 = 19)
     * is the oldest version this firmware consumes. zones_config_json_decode_blob()
     * refuses anything older with a "too old to consume" reason before this
     * switch is reached. */
    case 19: {
        /* v19 -> v20 (THIS pass, RELAY_LIFE_BUDGET.md):
         * relay_type is brand new -- see this function's own top-of-function
         * comment for why every zone simply lands on the 0 (ssr) sentinel
         * via the entry memset, with no explicit per-zone assignment
         * needed. zone_cfg_v19_t (frozen in zones_config_json.h) is
         * byte-for-byte identical to what zone_cfg_t was at v19, so a
         * per-element memcpy of that prefix is exactly equivalent to a
         * whole-array memcpy, just typed against the smaller historical
         * shape -- same technique case 18 uses against zone_cfg_v18_t. */
        zones_cfg_v19_t *src = (zones_cfg_v19_t *)scratch;
        memcpy(src, blob, sizeof(*src));
        out->thermo_count = src->thermo_count;
        out->relay_count = src->relay_count;
        out->max_simultaneous_relays = src->max_simultaneous_relays;
        out->continue_on_zone_trip = src->continue_on_zone_trip;
        out->safety_tc_type = src->safety_tc_type;
        out->pc_link_abort_silence_ms = src->pc_link_abort_silence_ms; /* real v19 value */
        out->timing_profile_count = src->timing_profile_count;
        memcpy(out->timing_profiles, src->timing_profiles, sizeof(out->timing_profiles));
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            zone_cfg_migrate_prefix_and_tail(&out->zones[i], &src->zones[i], sizeof(src->zones[i]),
                                              offsetof(zone_cfg_v19_t, settings_source),
                                              offsetof(zone_cfg_v19_t, tuning_valid));
            /* out->zones[i].relay_type already 0 (ssr) from this function's
             * entry memset -- brand-new mechanism, no prior global opinion
             * to carry forward, same as error_band_c/rate_band_c_per_s's
             * own v18->v19 migration just above. */
        }
        /* src->crc32 deliberately NOT carried over -- it covered the v19
         * shape; nvs_save() stamps a fresh one over the current (v20)
         * struct. */
        return true;
    }
    case 20: {
        /* v20 -> v21 (THIS pass, docs/ARCHITECTURE_DECISIONS.md#zones-page-clean-up-info-disclosure-schema-v20-v21-chartjs): the single
         * whole-zone settings_source byte becomes settings_source[
         * SRC_GROUP_COUNT] -- a MID-STRUCT field growing, unlike relay_type
         * (v19->v20) or error_band_c/rate_band_c_per_s (v18->v19), which
         * were appended at the true tail. zone_cfg_v20_t is therefore NOT a
         * byte-for-byte prefix of the current (v21) zone_cfg_t the way
         * every other adjacent-version pair in this switch is -- everything
         * from settings_source onward shifted, so this case copies every
         * field explicitly instead of memcpy()ing a shared prefix, the same
         * discipline the v9->v10/v10->v11 coupling-array insertions used
         * the last time a field grew in the middle rather than at the tail.
         *
         * Migration rule: every group starts as a COPY of the old single
         * byte, not a fresh CUSTOM -- a board mid-firing with zone 1 set to
         * "same as zone 0" must keep behaving identically after this
         * upgrade, for every one of the five now-independent groups, not
         * silently reset to "custom" on four of them. */
        zones_cfg_v20_t *src = (zones_cfg_v20_t *)scratch;
        memcpy(src, blob, sizeof(*src));
        out->thermo_count = src->thermo_count;
        out->relay_count = src->relay_count;
        out->max_simultaneous_relays = src->max_simultaneous_relays;
        out->continue_on_zone_trip = src->continue_on_zone_trip;
        out->safety_tc_type = src->safety_tc_type;
        out->pc_link_abort_silence_ms = src->pc_link_abort_silence_ms; /* real v20 value */
        out->timing_profile_count = src->timing_profile_count;
        memcpy(out->timing_profiles, src->timing_profiles, sizeof(out->timing_profiles));
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            const zone_cfg_v20_t *s = &src->zones[i];
            zone_cfg_t *d = &out->zones[i];
            memcpy(d->name, s->name, sizeof(d->name));
            d->cal_offset_c = s->cal_offset_c;
            d->pid_kp = s->pid_kp;
            d->pid_ki = s->pid_ki;
            d->pid_kd = s->pid_kd;
            d->max_ramp_c_per_hr = s->max_ramp_c_per_hr;
            d->sanity_rate_c_per_min = s->sanity_rate_c_per_min;
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
            d->fuzzy_strength_pct = s->fuzzy_strength_pct;
            memcpy(d->coupling_coeff, s->coupling_coeff, sizeof(d->coupling_coeff));
            memcpy(d->coupling_tau_s, s->coupling_tau_s, sizeof(d->coupling_tau_s));
            memcpy(d->coupling_dead_time_s, s->coupling_dead_time_s, sizeof(d->coupling_dead_time_s));
            d->relay_mask = s->relay_mask;
            d->control_mode = s->control_mode;
            d->tc_type = s->tc_type;
            d->thermo_mask = s->thermo_mask;
            d->ct_mask = s->ct_mask;
            d->timing_profile = s->timing_profile;
            /* THE migration: one old byte fans out to every group. */
            for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
                d->settings_source[g] = s->settings_source;
            }
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
            d->coupling_diag_k_dc = s->coupling_diag_k_dc;
            d->ease_off_window_mult = s->ease_off_window_mult;
            d->approach_rate_cap_c_per_hr = s->approach_rate_cap_c_per_hr;
            d->error_band_c = s->error_band_c;
            d->rate_band_c_per_s = s->rate_band_c_per_s;
            d->relay_type = s->relay_type;
        }
        /* src->crc32 deliberately NOT carried over -- it covered the v20
         * shape; nvs_save() stamps a fresh one over the current (v21)
         * struct. */
        return true;
    }
    case 21: {
        /* v21 -> v22 (THIS pass, docs/audits/consumer_without_producer_
         * 2026-09-06.md finding 1): progress_band_c is brand new, appended
         * at the true tail after relay_type -- zone_cfg_v21_t is therefore
         * a byte-for-byte prefix of the current (v22) zone_cfg_t, same
         * "plain memcpy of the smaller historical shape" technique case 17/
         * case 18 use, not case 20's field-by-field copy (that one was
         * needed only because settings_source grew MID-struct at v20->v21;
         * this hop is a pure tail append like every other one since). Every
         * zone's progress_band_c lands on the 0 (use PROGRESS_BAND_C)
         * sentinel via this function's entry memset -- brand-new mechanism,
         * no prior global opinion to carry forward, same shape as relay_
         * type's own v19->v20 migration. */
        zones_cfg_v21_t *src = (zones_cfg_v21_t *)scratch;
        memcpy(src, blob, sizeof(*src));
        out->thermo_count = src->thermo_count;
        out->relay_count = src->relay_count;
        out->max_simultaneous_relays = src->max_simultaneous_relays;
        out->continue_on_zone_trip = src->continue_on_zone_trip;
        out->safety_tc_type = src->safety_tc_type;
        out->pc_link_abort_silence_ms = src->pc_link_abort_silence_ms; /* real v21 value */
        out->timing_profile_count = src->timing_profile_count;
        memcpy(out->timing_profiles, src->timing_profiles, sizeof(out->timing_profiles));
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            memcpy(&out->zones[i], &src->zones[i], sizeof(src->zones[i]));
            /* out->zones[i].progress_band_c already 0 from this function's
             * entry memset -- see this case's own top comment. */
        }
        /* src->crc32 deliberately NOT carried over -- it covered the v21
         * shape; nvs_save() stamps a fresh one over the current (v22)
         * struct. */
        return true;
    }
    case 22: {
        /* v22 -> v23 (THIS pass, docs/ON_OFF_ZONE.md step 1):
         * zone_type/failsafe_state/hyst_c/min_on_s/min_off_s are brand new,
         * appended at the true tail after progress_band_c -- zone_cfg_v22_t
         * is therefore a byte-for-byte prefix of the current (v23)
         * zone_cfg_t, same "plain memcpy of the smaller historical shape"
         * technique case 21 uses just above. Every zone's zone_type lands on
         * the 0 (ZONE_TYPE_HEATER) sentinel and failsafe_state on 0 (OFF) via
         * this function's entry memset -- brand-new mechanism, no prior
         * global opinion to carry forward, same shape as progress_band_c's
         * own v21->v22 migration. ZONE_TYPE_HEATER == 0 and failsafe_state ==
         * OFF == 0 are BOTH load-bearing here: a v22 board upgrading must
         * never silently gain an on/off zone or a fail-safe-ON device it
         * never configured. */
        zones_cfg_v22_t *src = (zones_cfg_v22_t *)scratch;
        memcpy(src, blob, sizeof(*src));
        out->thermo_count = src->thermo_count;
        out->relay_count = src->relay_count;
        out->max_simultaneous_relays = src->max_simultaneous_relays;
        out->continue_on_zone_trip = src->continue_on_zone_trip;
        out->safety_tc_type = src->safety_tc_type;
        out->pc_link_abort_silence_ms = src->pc_link_abort_silence_ms; /* real v22 value */
        out->timing_profile_count = src->timing_profile_count;
        memcpy(out->timing_profiles, src->timing_profiles, sizeof(out->timing_profiles));
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            memcpy(&out->zones[i], &src->zones[i], sizeof(src->zones[i]));
            /* out->zones[i].zone_type/failsafe_state/hyst_c/min_on_s/
             * min_off_s already 0 from this function's entry memset -- see
             * this case's own top comment. */
        }
        /* src->crc32 deliberately NOT carried over -- it covered the v22
         * shape; nvs_save() stamps a fresh one over the current (v23)
         * struct. */
        return true;
    }
    case 23: {
        /* v23 -> v24 (THIS pass, docs/audits/high_temperature_transfer_
         * analysis_2026-09-08.md): model_fit_temp_c/model_fit_ambient_c are
         * brand new, appended at the true tail after min_off_s --
         * zone_cfg_v23_t is therefore a byte-for-byte prefix of the current
         * (v24) zone_cfg_t, same "plain memcpy of the smaller historical
         * shape" technique case 22 uses just above. Every zone's
         * model_fit_temp_c/model_fit_ambient_c are left at 0 by this
         * function's entry memset here, but that is NOT this field's
         * "unknown" sentinel (ZONE_MODEL_FIT_TEMP_UNKNOWN, deliberately
         * -273.15f, never 0 -- see that field's own comment) -- so unlike
         * every prior tail-append, this one cannot rely on the entry
         * memset's zero being the correct default. zones_config_json_
         * decode_blob() backfills the real sentinel onto every zone right
         * after this function returns, for every pre-v24 version uniformly,
         * rather than duplicating that assignment in each case here. */
        zones_cfg_v23_t *src = (zones_cfg_v23_t *)scratch;
        memcpy(src, blob, sizeof(*src));
        out->thermo_count = src->thermo_count;
        out->relay_count = src->relay_count;
        out->max_simultaneous_relays = src->max_simultaneous_relays;
        out->continue_on_zone_trip = src->continue_on_zone_trip;
        out->safety_tc_type = src->safety_tc_type;
        out->pc_link_abort_silence_ms = src->pc_link_abort_silence_ms; /* real v23 value */
        out->timing_profile_count = src->timing_profile_count;
        memcpy(out->timing_profiles, src->timing_profiles, sizeof(out->timing_profiles));
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            memcpy(&out->zones[i], &src->zones[i], sizeof(src->zones[i]));
            /* out->zones[i].model_fit_temp_c/model_fit_ambient_c backfilled
             * to the UNKNOWN sentinel by the caller -- see this case's own
             * top comment. */
        }
        /* src->crc32 deliberately NOT carried over -- it covered the v23
         * shape; nvs_save() stamps a fresh one over the current (v24)
         * struct. */
        return true;
    }
    case 24: {
        /* v24 -> v25 (THIS pass, owner request: per-coil nameplate wattage
         * override): coil_power_w is brand new, appended at the true tail
         * after model_fit_ambient_c -- zone_cfg_v24_t is therefore a
         * byte-for-byte prefix of the current (v25) zone_cfg_t, same "plain
         * memcpy of the smaller historical shape" technique case 23 uses
         * just above. Every zone's coil_power_w is left at 0 by this
         * function's entry memset, and 0 IS this field's own "not
         * overridden, use an equal share of the sum nameplate" sentinel --
         * unlike case 23's model_fit_temp_c, no separate backfill is
         * needed. */
        zones_cfg_v24_t *src = (zones_cfg_v24_t *)scratch;
        memcpy(src, blob, sizeof(*src));
        out->thermo_count = src->thermo_count;
        out->relay_count = src->relay_count;
        out->max_simultaneous_relays = src->max_simultaneous_relays;
        out->continue_on_zone_trip = src->continue_on_zone_trip;
        out->safety_tc_type = src->safety_tc_type;
        out->pc_link_abort_silence_ms = src->pc_link_abort_silence_ms; /* real v24 value */
        out->timing_profile_count = src->timing_profile_count;
        memcpy(out->timing_profiles, src->timing_profiles, sizeof(out->timing_profiles));
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            memcpy(&out->zones[i], &src->zones[i], sizeof(src->zones[i]));
            /* out->zones[i].coil_power_w already 0 from this function's
             * entry memset -- see this case's own top comment. */
        }
        /* src->crc32 deliberately NOT carried over -- it covered the v24
         * shape; nvs_save() stamps a fresh one over the current (v25)
         * struct. */
        return true;
    }
    case 25: {
        /* v25 -> v26 (THIS pass, docs/audits/adaptive_tune_vs_owner_
         * requirements_2026-09-11.md): autotune_baseline_k_dc is brand new,
         * appended at the true tail after coil_power_w -- zone_cfg_v25_t is
         * therefore a byte-for-byte prefix of the current (v26) zone_cfg_t,
         * same "plain memcpy of the smaller historical shape" technique
         * case 24 uses just above. Every zone's autotune_baseline_k_dc is
         * left at 0 by this function's entry memset, and 0 IS this field's
         * own "no baseline recorded yet" sentinel -- same as case 24's
         * coil_power_w, no separate backfill needed. adaptive_tune_refine_
         * zone_locked() bootstraps a real baseline from the live model_k_dc
         * itself, the first time it runs against an upgraded zone that
         * already has a model but reads 0 here -- see that function's own
         * comment. */
        zones_cfg_v25_t *src = (zones_cfg_v25_t *)scratch;
        memcpy(src, blob, sizeof(*src));
        out->thermo_count = src->thermo_count;
        out->relay_count = src->relay_count;
        out->max_simultaneous_relays = src->max_simultaneous_relays;
        out->continue_on_zone_trip = src->continue_on_zone_trip;
        out->safety_tc_type = src->safety_tc_type;
        out->pc_link_abort_silence_ms = src->pc_link_abort_silence_ms; /* real v25 value */
        out->timing_profile_count = src->timing_profile_count;
        memcpy(out->timing_profiles, src->timing_profiles, sizeof(out->timing_profiles));
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            memcpy(&out->zones[i], &src->zones[i], sizeof(src->zones[i]));
            /* out->zones[i].autotune_baseline_k_dc already 0 from this
             * function's entry memset -- see this case's own top comment. */
        }
        /* src->crc32 deliberately NOT carried over -- it covered the v25
         * shape; nvs_save() stamps a fresh one over the current (v26)
         * struct. */
        return true;
    }
    case 26:
        return zones_cfg_step_v26_to_v27(blob, out, scratch);
    default:
        /* No known historical (or current) layout for this version --
         * zones_cfg_expected_len_for_version() already returned 0 for it and
         * zones_config_json_decode_blob() should never reach here; kept as a defensive
         * explicit refusal rather than silently guessing. */
        return false;
    }
}

static bool convert_versioned_blob_to_current(uint8_t version, const void *blob, zones_cfg_t *out, bool *out_oom)
{
    void *scratch = persist_scratch_alloc(sizeof(zones_cfg_t));
    if (!scratch) {
        memset(out, 0, sizeof(*out));
        *out_oom = true; /* distinct from a failed conversion: the bytes were never judged */
        return false;
    }
    bool ok = convert_versioned_blob_to_current_impl(version, blob, out, scratch);
    free(scratch);
    return ok;
}

/* esp_crc32_le() (same helper crash_report.c's compute_crc() uses) over the
 * struct with crc32 itself zeroed -- computed over a local copy so a caller
 * re-validating an already-loaded cfg's crc32 is never mutated by asking. */
uint32_t zones_config_json_compute_crc(const zones_cfg_t *cfg)
{
    /* NOT a `zones_cfg_t tmp = *cfg` local anymore (2026-09-09 panic,
     * docs/audits/executor_panic_stack_overflow_2026-09-09.md): that ~900 B
     * copy was one frame of the run-end path (adaptive_tune_run_end -> ... ->
     * nvs_save -> zones_config_cfg_fs_save -> zones_config_json_compute_crc)
     * that overflowed profile_executor's 4096 B stack. This function is also
     * reached routinely from main/httpd_worker (larger stacks, never in
     * danger), so the fix has to hold for every caller, not just this one.
     *
     * `crc32` is the LAST field of zones_cfg_t (_Static_assert below pins
     * that), so "compute the CRC as if crc32 were zero" needs no copy of the
     * struct and no mutation of the caller's cfg at all: CRC everything
     * before the field over the real struct in place, then feed exactly 4
     * zero bytes for the field itself. esp_crc32_le()'s running-seed
     * argument makes that a two-call chain rather than a buffer to build. */
    _Static_assert(offsetof(zones_cfg_t, crc32) + sizeof(((zones_cfg_t *)0)->crc32) == sizeof(zones_cfg_t),
                   "zones_config_json_compute_crc()'s zero-copy trick assumes crc32 is the last field");
    static const uint8_t zero4[sizeof(cfg->crc32)] = {0};
    uint32_t crc = esp_crc32_le(0, (const uint8_t *)cfg, offsetof(zones_cfg_t, crc32));
    crc = esp_crc32_le(crc, zero4, sizeof(zero4));
    return crc;
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
            ESP_LOGW(ZONES_CFG_TAG, "zone %u heater_min_on_ms %.0f ms is below the %.0f ms relay-protection "
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
            ESP_LOGW(ZONES_CFG_TAG, "zone %u heater_window_ms %.0f ms is shorter than %.0f ms (%.0fx its "
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
/* See zones_config_json.h for the full rationale (opus review defect D):
 * the migration branch below and nvs_load_from()'s defaults path are the
 * complete set of ways an unfitted zones_cfg_t comes into existence, and
 * both must land on the sentinel rather than 0.0f. */
void zones_config_json_apply_model_fit_defaults(zones_cfg_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    for (uint8_t bi = 0; bi < MAX31856_CHANNEL_COUNT; bi++) {
        cfg->zones[bi].model_fit_temp_c = ZONE_MODEL_FIT_TEMP_UNKNOWN;
        cfg->zones[bi].model_fit_ambient_c = ZONE_MODEL_FIT_TEMP_UNKNOWN;
    }
}

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

    if (version < ZONES_CFG_MIGRATION_EXPIRY_FLOOR) {
        /* D2 expiry (docs/CONFIG_MIGRATION_CHAIN.md sec 4.2): the converter
         * for this version was deleted. Refuse, never reset; the caller
         * quarantines and leaves flash untouched. */
        *reason = "config version is older than the migration expiry floor -- too old to consume";
        return ZONES_DECODE_CORRUPT;
    }
    size_t expected = zones_cfg_expected_len_for_version(version);
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
        /* Opus review finding 10 (2026-09-10): every OLDER-version branch
         * discarded its own blob's crc32 unchecked before this fix -- the
         * length-must-match-the-claimed-version check above is the only
         * integrity gate a v1..v24 blob ever passed through, so a single
         * flipped bit anywhere in range (a PID gain, say) migrates straight
         * to the current layout and passes zones_config_json_validate()
         * silently. This is a PRE-EXISTING gap this pass did not create,
         * but ZONES_CFG_VERSION's 24->25 bump widens it immediately: a v24
         * blob that used to take the `version == ZONES_CFG_VERSION` branch
         * above (full CRC check) now takes THIS branch instead, with its
         * own real, computable crc32 (zones_cfg_v24_t's tail field)
         * discarded by case 24's converter same as every other case here.
         * Closing that one immediately-widened step, narrowly, rather than
         * every historical version at once: v1..v6 predate crc32 entirely
         * (zones_cfg_t::crc32's own comment) and v7..v23 would need their
         * own frozen-struct verification added one at a time, each a
         * separate, independently reviewable change -- bundling all of them
         * into this pass risks a copy-paste offset mistake in a codepath
         * that, unlike a fresh corruption, has never been exercised by any
         * existing test fixture for those versions. Documented here as the
         * accepted, narrower scope rather than silently left same as before. */
        /* ZONES_CFG_VERSION 25->26: this check follows the immediately-prior
         * version (whichever ZONES_CFG_VERSION - 1 names), same "narrow,
         * one-version-at-a-time" scope opus review finding 10's comment
         * above accepted -- v1..v23 remain uncovered by this specific gate
         * for the same reasons given there (v24 is covered separately,
         * below, added in the 2026-09-16 release-hardening pass). */
        if (version == ZONES_CFG_VERSION - 1) {
            _Static_assert(offsetof(zones_cfg_v26_t, crc32) + sizeof(uint32_t) == sizeof(zones_cfg_v26_t),
                           "zones_cfg_v26_t's CRC check below assumes crc32 is its last field");
            uint32_t stored_crc = 0;
            memcpy(&stored_crc, (const uint8_t *)blob + offsetof(zones_cfg_v26_t, crc32), sizeof(stored_crc));
            static const uint8_t zero4[sizeof(uint32_t)] = {0};
            uint32_t computed_crc =
                esp_crc32_le(0, (const uint8_t *)blob, offsetof(zones_cfg_v26_t, crc32));
            computed_crc = esp_crc32_le(computed_crc, zero4, sizeof(zero4));
            if (computed_crc != stored_crc) {
                *reason = "CRC mismatch on the stored v26 blob -- treating as corrupt rather than migrating it";
                return ZONES_DECODE_CORRUPT;
            }
        }
        /* Release-hardening pass, 2026-09-16 (docs/audits/
         * release_hardening_plan_verify_1_2_5_7_8_2026-09-16.md, blocker 7):
         * the 25->26 bump above already re-derived this gate for its own
         * new N-1 (v25) -- but that same bump is *also* the moment v24
         * silently dropped OFF this gate and onto the "uncovered" list,
         * exactly the "immediately widens" pattern opus review finding 10's
         * comment names. v24 is judged reachable enough to close now,
         * narrowly, same one-version-at-a-time scope: this project's board
         * is reflashed within the same development session far more often
         * than not (CLAUDE.md's flash/reset log), so an NVS blob surviving
         * completely unmigrated across TWO schema bumps (a short gap
         * between sessions) is plausible; surviving across the other 17
         * (v7..v23) each require the board to have gone unflashed across
         * that whole historical stretch, which nothing in this project's
         * history suggests ever happened -- those remain deliberately
         * unimplemented, same reasoning as the comment above, not widened
         * by this pass either. v1..v6 predate crc32 entirely and cannot be
         * checked at all. zones_cfg_v24_t has the identical "crc32 is the
         * last field" shape as v25 (frozen struct comment, zones_config_
         * json.h), so this mirrors the v25 branch exactly. */
        /* ZONES_CFG_VERSION 26->27 (sheath tau): the N-2 gate moves to v25;
         * v24 keeps its own gate by literal version so this bump does not
         * silently drop it back onto the uncovered list. */
        if (version == ZONES_CFG_VERSION - 2 /* v25 */) {
            _Static_assert(offsetof(zones_cfg_v25_t, crc32) + sizeof(uint32_t) == sizeof(zones_cfg_v25_t),
                           "zones_cfg_v25_t's CRC check below assumes crc32 is its last field");
            uint32_t stored_crc = 0;
            memcpy(&stored_crc, (const uint8_t *)blob + offsetof(zones_cfg_v25_t, crc32), sizeof(stored_crc));
            static const uint8_t zero4[sizeof(uint32_t)] = {0};
            uint32_t computed_crc =
                esp_crc32_le(0, (const uint8_t *)blob, offsetof(zones_cfg_v25_t, crc32));
            computed_crc = esp_crc32_le(computed_crc, zero4, sizeof(zero4));
            if (computed_crc != stored_crc) {
                *reason = "CRC mismatch on the stored v25 blob -- treating as corrupt rather than migrating it";
                return ZONES_DECODE_CORRUPT;
            }
        }
        if (version == 24) {
            _Static_assert(offsetof(zones_cfg_v24_t, crc32) + sizeof(uint32_t) == sizeof(zones_cfg_v24_t),
                           "zones_cfg_v24_t's CRC check below assumes crc32 is its last field");
            uint32_t stored_crc = 0;
            memcpy(&stored_crc, (const uint8_t *)blob + offsetof(zones_cfg_v24_t, crc32), sizeof(stored_crc));
            static const uint8_t zero4[sizeof(uint32_t)] = {0};
            uint32_t computed_crc =
                esp_crc32_le(0, (const uint8_t *)blob, offsetof(zones_cfg_v24_t, crc32));
            computed_crc = esp_crc32_le(computed_crc, zero4, sizeof(zero4));
            if (computed_crc != stored_crc) {
                *reason = "CRC mismatch on the stored v24 blob -- treating as corrupt rather than migrating it";
                return ZONES_DECODE_CORRUPT;
            }
        }
        bool convert_oom = false;
        if (!convert_versioned_blob_to_current(version, blob, out, &convert_oom)) {
            memset(out, 0, sizeof(*out));
            if (convert_oom) {
                *reason = "out of memory converting the stored version -- not judged";
                return ZONES_DECODE_OOM;
            }
            *reason = "unable to convert stored version to the current layout";
            return ZONES_DECODE_CORRUPT;
        }
        out->version = ZONES_CFG_VERSION;
        /* out->crc32 stays 0 here -- a migrated struct has never been saved
         * in the current format yet, so there is no stored CRC to check
         * against. nvs_save() stamps a real one the next time this config is
         * written, current or not. */

        /* model_fit_temp_c/model_fit_ambient_c backfill (ZONES_CFG_VERSION
         * 23->24): every version this function can migrate FROM predates
         * this field, so it is always 0 (this function's own entry memset)
         * coming out of convert_versioned_blob_to_current() above -- and 0
         * is NOT this field's "unknown" meaning (ZONE_MODEL_FIT_TEMP_UNKNOWN,
         * deliberately -273.15f -- see zone_cfg_t's own comment on why 0
         * would be indistinguishable from a genuine ambient fit). Applied
         * centrally here, once, for every historical version uniformly,
         * rather than duplicated inside each per-version case above -- an
         * old record with a real, previously-fitted model_k_dc/tau/dead_time
         * honestly reads its fit context as unknown rather than inventing a
         * plausible-looking temperature nobody ever measured. */
        zones_config_json_apply_model_fit_defaults(out);
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
