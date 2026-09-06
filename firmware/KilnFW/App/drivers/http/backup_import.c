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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "MAX31856.h"
#include "ota_http.h" /* ota_http_check_interlocks() -- see backup_http.h's header comment */
#include "profiles_http.h"
#include "zones_config_accessors.h"

static bool backup_import_apply(const char *body, char *err_msg, size_t err_cap)
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
    typedef struct {
        bool has_id;
        uint8_t id;
        profile_t p;
    } profile_candidate_t;
    profile_candidate_t candidates[PROFILES_MAX_COUNT];
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
         * for the same input (parse_profile_fields(), via
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
        uint8_t settings_source; /* defaults to ZONE_SETTINGS_SOURCE_CUSTOM -- see comment above */
    } zone_candidate_t;
    zone_candidate_t zone_candidates[MAX31856_CHANNEL_COUNT];
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
        zc->settings_source = ZONE_SETTINGS_SOURCE_CUSTOM;

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
         * must NOT leave zc->settings_source at 0 (see zone_candidate_t's own
         * comment): zc->settings_source was already pre-seeded to
         * ZONE_SETTINGS_SOURCE_CUSTOM right after this candidate's memset,
         * above, so this block only ever OVERWRITES it when the key is
         * actually present. No has_* flag: pass 2 always commits
         * zc->settings_source for every candidate. */
        double dsrc;
        if (backup_json_field_num(ze, "settings_source", &dsrc)) {
            if (dsrc < 0 || dsrc > 255) {
                snprintf(err_msg, err_cap, "zone tuning entry %u: settings_source out of range",
                        (unsigned)zone_candidate_count);
                return false;
            }
            /* Same "reject a fractional index, never truncate" rule as
             * coupling_neighbor_zone above: settings_source is a zone INDEX
             * (or the ZONE_SETTINGS_SOURCE_CUSTOM sentinel) living in a
             * float here, and (uint8_t)dsrc below would otherwise silently
             * truncate e.g. 1.7 to 1 -- a hand-edited backup could make a
             * zone inherit a DIFFERENT zone's settings than the one written
             * in the file, with no error. */
            if (dsrc != floor(dsrc)) {
                snprintf(err_msg, err_cap, "zone tuning entry %u: settings_source must be a whole zone index",
                        (unsigned)zone_candidate_count);
                return false;
            }
            uint8_t src_raw = (uint8_t)dsrc;
            /* Same bound zones_config_set_settings_source()/
             * parse_zone_fields()'s z%u_settings_source enforce: either
             * ZONE_SETTINGS_SOURCE_CUSTOM (0xFF) or a real zone index other
             * than this entry's own. */
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
            zc->settings_source = src_raw;
        }

        zone_candidate_count++;
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
        bool has_override[MAX31856_CHANNEL_COUNT] = {0};
        uint8_t override_source[MAX31856_CHANNEL_COUNT] = {0};
        for (size_t i = 0; i < zone_candidate_count; i++) {
            has_override[zone_candidates[i].index] = true;
            override_source[zone_candidates[i].index] = zone_candidates[i].settings_source;
        }
        /* WEB_UI_PLAN.md section 2 (ZONES_CFG_VERSION 20->21): the backup
         * format's single "settings_source" key is applied to every one of
         * the SRC_GROUP_COUNT independent groups on commit (see this
         * file's own comment on the export side, backup_export.c), so the
         * cross-entry cycle check must run once per group too -- a set that
         * is acyclic in one group is not automatically acyclic when the
         * same links are replayed into a different group's independent
         * chain... except here they ARE the exact same override_source for
         * every group, so in practice all five checks either all pass or
         * all fail together; this still checks every group explicitly
         * rather than assuming that, since nothing enforces it structurally
         * and a future backup-format change could break the assumption
         * silently otherwise. */
        for (uint8_t group = 0; group < SRC_GROUP_COUNT; group++) {
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
        /* Applied to every group -- see this file's pass-1 comment above on
         * why the single backup-format key fans out to all SRC_GROUP_COUNT
         * groups on import. */
        for (uint8_t group = 0; group < SRC_GROUP_COUNT; group++) {
            if (!zones_config_set_settings_source_unchecked(zc->index, group, zc->settings_source)) {
                snprintf(err_msg, err_cap,
                        "zone tuning entry %u (channel %u) rejected at commit setting settings_source",
                        (unsigned)i, zc->index);
                return false;
            }
        }
    }
    if (has_safety_tc) {
        zones_config_set_safety_tc_type((uint8_t)dsafety); /* only fails on out-of-range, already checked above */
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

    char err_msg[160];
    bool ok = backup_import_apply(body, err_msg, sizeof(err_msg));
    free(body);

    if (!ok) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, err_msg, strlen(err_msg));
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    const char *ok_json = "{\"ok\":true}";
    return httpd_resp_send(req, ok_json, strlen(ok_json));
}
