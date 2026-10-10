// Per-zone field parser, split off zones_http_handlers.c (2026-09-04,
// ROADMAP.md M15's 1500-line item) -- see zones_http_internal.h for the
// full split map. MOVE-ONLY: the former `static bool parse_zone_fields()`,
// unchanged apart from widening it from `static` to file-scope-internal
// linkage (declared in zones_http_internal.h, prefixed
// zones_http_parse_zone_fields -- the codebase-wide rule this pass follows
// is to rename every symbol widened out of `static` with its owning
// module's prefix even when no collision was found, and none was found for
// `parse_zone_fields` here: it is now zones_http_post.c's own
// zones_post_handler() that calls it, across the new file boundary).

#include "zones_http_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_form.h"
#include "persist_scratch.h" /* persist_scratch_alloc() -- probe copy */
#include "zone_settings_source_chain.h"

/* Blank-tolerant presence for the guard fields and z%u_xzone: pre-strict-parse these
 * treated "key=" (a page pushing an empty <input type=number>) as omitted, i.e. 0/disabled.
 * Non-empty garbage still fails the range parse (400). */
static bool zones_http_field_nonblank(const char *body, const char *key)
{
    char probe[2];
    int n = http_form_find_field(body, key, probe, sizeof(probe));
    return n != -1 && n != 0; /* -2 (overflow) is non-blank: parse rejects it */
}

/* "key=" present but empty. A blank keeps the STORED value (omit-preserve, like pc_link_abort_silence_ms):
 * it must never fall back to the firmware default or disable guard 8 (owner decision: no guard-disable
 * path). A key that is wholly omitted keeps the documented whole-page-submit meaning (0). */
static bool zones_http_field_blank(const char *body, const char *key)
{
    char probe[2];
    return http_form_find_field(body, key, probe, sizeof(probe)) == 0;
}


/* SRC_GROUP_LIMITS/RELAY_TIMING/CONTROL/GUARDS/TC order, indexed by the
 * #defines in zones_config_accessors.h -- see zones_http_internal.h's
 * declaration of this array for why it lives here (parser and GET emitter
 * must never disagree on spelling). */
const char *const SRC_GROUP_NAMES[SRC_GROUP_COUNT] = {
    "limits", "relaytiming", "control", "guards", "tc",
};

/* ---- POST /api/zones ------------------------------------------------------
 * Whole-page submit; every field validated into a scratch struct before
 * anything is written to the in-RAM copy or NVS -- reject cleanly, never
 * partially apply, same discipline as every other untrusted-input boundary
 * in this codebase. */

/* Parses and validates zone index i's 7 fields from body into *z. thermo_count
 * is the just-parsed candidate count (not yet committed) -- zones at or past
 * it are still parsed (so a round-trip GET/POST of an unused zone block
 * doesn't need special-casing on the page) but not checked against
 * relay_count, since a shrunk relay_count would otherwise reject fields the
 * page never showed for a zone the submission isn't even claiming to use. */
bool zones_http_parse_zone_fields(const char *body, uint8_t i, uint8_t thermo_count, uint8_t relay_count,
                              uint8_t timing_profile_count, const zone_cfg_t *current_z, zone_cfg_t *z,
                              const char **err_reason)
{
    char key[24]; /* 16 -> 24 when the 8 guard-threshold override keys were
                   * added -- "z0_wrongdirwindow" is the longest at 18 chars
                   * plus terminator. */

    snprintf(key, sizeof(key), "z%u_name", i);
    char name[ZONE_NAME_MAX_LEN + 1];
    int name_len = http_form_find_field(body, key, name, sizeof(name));
    if (name_len == -2) {
        *err_reason = "zone name too long or contains a control character";
        return false;
    }
    if (name_len > 0 && http_form_value_has_ctl(name, name_len)) {
        *err_reason = "zone name contains a control character";
        return false;
    }
    if (name_len < 0) {
        /* Omitted: preserve the currently-stored name rather than blanking
         * it. In practice zones_page.html always sends z%u_name for every
         * zone it renders (i < thermo_count), so this only matters for a
         * slot the page never showed -- see the i >= thermo_count preserve
         * block below, which this keeps consistent with. */
        strncpy(z->name, current_z->name, ZONE_NAME_MAX_LEN);
        z->name[ZONE_NAME_MAX_LEN] = '\0';
    } else {
        strncpy(z->name, name, ZONE_NAME_MAX_LEN);
        z->name[ZONE_NAME_MAX_LEN] = '\0';
    }

    /* 2026-08-21, TODO.md owner-report item 1/4: this channel's MAX31856
     * thermocouple type. Parsed BEFORE the `i >= thermo_count` early return
     * below, deliberately unlike relay_mask/thermo_mask/every other zone
     * field -- this is per-CHANNEL hardware state (see zone_cfg_t::tc_type's
     * comment), not per-zone, so a channel physically present but not
     * currently claimed by any configured zone (thermo_count set lower than
     * the physical channel count) must still keep its own real type across
     * an ordinary page save rather than being silently zeroed to THERMO_TC_B
     * the moment it falls outside thermo_count's range -- which is exactly
     * what would happen if this fell after the early return, since tmp is
     * zero-initialized by the caller and 0 is a real, different, wrong type
     * here (see ZONES_CFG_VERSION's migration comment for the identical
     * reasoning).
     *
     * OPTIONAL, falling back to current_z->tc_type (the live value) rather
     * than to a fixed default when omitted -- there is no default
     * thermocouple type that could possibly be correct for a channel this
     * submission never mentioned, unlike thermo_mask's "zone i reads channel
     * i" legacy mapping just below. zones_page.html always sends this field
     * (see its JS), so the fallback matters only for a client that predates
     * thermocouple-type selection entirely (pc_tools/MCP, the test
     * harnesses).
     *
     * Present-but-out-of-range is still an error -- "in range" is
     * ZONE_TC_TYPE_MAX_REAL (0-7, the eight real thermocouple types), not
     * MAX31856_configure()'s wider 0-0x0F: see that macro's comment for why
     * this operator-facing endpoint is deliberately stricter than the raw
     * UART debug path. */
    snprintf(key, sizeof(key), "z%u_tctype", i);
    {
        char probe[8];
        if (zones_config_json_field_present(body, key)) {
            uint8_t tc_type_raw;
            if (!zones_config_json_parse_u8_field(body, key, 0, ZONE_TC_TYPE_MAX_REAL, &tc_type_raw)) {
                *err_reason = "zone thermocouple type must be a real thermocouple type (0-7: "
                              "B/E/J/K/N/R/S/T), not a voltage-input mode";
                return false;
            }
            z->tc_type = tc_type_raw;
        } else {
            z->tc_type = current_z->tc_type;
        }
    }

    if (i >= thermo_count) {
        /* DEFECT FIX (found by black-box testing against the live board):
         * this slot is past the just-submitted thermo_count, so
         * zones_page.html's whole-page submit never rendered UI for it and
         * carries no data for relay_mask/thermo_mask/cal/PID/ramp/guard
         * thresholds/model/etc. The caller's `z` starts zero-initialized
         * (zones_post_handler()'s memset(&tmp, 0, ...)), so returning here
         * unconditionally used to leave every one of those fields at 0 --
         * an accepted 200 OK POST that silently zeroed state the operator
         * never asked to change (reproduced live: thermo_count=1 zeroed
         * zone 1 and zone 2's thermo_mask on an ordinary re-save that only
         * replayed what GET had just reported).
         *
         * Fix: preserve the currently-stored zone_cfg_t for this slot
         * instead of leaving it zeroed. z->name and z->tc_type are already
         * set above (name/tc_type each have their own omit-means-preserve
         * handling), so save and restore just those two fields around a
         * bulk copy of everything else from current_z -- the live value,
         * same object z%u_tctype's fallback already reads from just above.
         * A future submission that raises thermo_count back up (or a
         * client that explicitly names this slot's fields once one exists)
         * still goes through the normal validated path below, unaffected --
         * this branch only runs for a slot outside today's thermo_count. */
        char preserved_name[ZONE_NAME_MAX_LEN + 1];
        strncpy(preserved_name, z->name, sizeof(preserved_name));
        preserved_name[ZONE_NAME_MAX_LEN] = '\0';
        uint8_t preserved_tc_type = z->tc_type;
        *z = *current_z;
        strncpy(z->name, preserved_name, ZONE_NAME_MAX_LEN);
        z->name[ZONE_NAME_MAX_LEN] = '\0';
        z->tc_type = preserved_tc_type;
        return true;
    }

    snprintf(key, sizeof(key), "z%u_relay_mask", i);
    uint8_t relay_mask_raw;
    if (!zones_config_json_parse_u8_field(body, key, 0, 0xFF, &relay_mask_raw)) {
        *err_reason = "zone relay_mask missing or invalid";
        return false;
    }
    uint8_t valid_bits = relay_count >= 8 ? 0xFF : (uint8_t)((1u << relay_count) - 1u);
    if ((relay_mask_raw & ~valid_bits) != 0) {
        *err_reason = "zone relay_mask references an unconfigured relay";
        return false;
    }
    z->relay_mask = relay_mask_raw;

    /* RELAY_LIFE_BUDGET.md (ZONES_CFG_VERSION 19->20): which
     * contact-life budget this zone's relay_mask relays are rated for.
     * OPTIONAL, same reason tc_type is optional just above (and unlike
     * relay_mask/control_mode, which are REQUIRED): every pre-existing
     * client (pc_tools/MCP, older test harnesses, the many host-test bodies
     * that predate this field) has never heard of z%u_relaytype and must not
     * start getting 400s on an otherwise-unrelated save, nor silently lose
     * an operator's earlier Contactor/Mercury choice back to SSR the next
     * time one of those clients replays a whole-page submit. Falls back to
     * current_z->relay_type (the live value) rather than to a fixed default
     * when omitted, same "preserve, don't default" choice tc_type makes.
     * Present-but-out-of-range is still an error. zones_post_handler()
     * pushes the validated result to relay_cycles_set_type() after a
     * successful save -- see that call site's own comment -- not here,
     * since this function only builds the candidate struct and must not
     * have any side effect before the whole submission is known to be
     * valid. */
    snprintf(key, sizeof(key), "z%u_relaytype", i);
    {
        char probe[8];
        if (zones_config_json_field_present(body, key)) {
            uint8_t relay_type_raw;
            if (!zones_config_json_parse_u8_field(body, key, 0, (long)ZONE_RELAY_TYPE_MAX, &relay_type_raw)) {
                *err_reason = "zone relay_type out of range (0-2: SSR/Contactor/Mercury)";
                return false;
            }
            z->relay_type = relay_type_raw;
        } else {
            z->relay_type = current_z->relay_type;
        }
    }

    /* docs/ON_OFF_ZONE.md step 6: zone_type/failsafe_state/hyst_c/
     * min_on_s/min_off_s (ZONES_CFG_VERSION 22->23 storage, unused by any
     * consumer until this UI pass). OPTIONAL, same reasoning as z%u_relaytype
     * above -- every pre-existing client (pc_tools/MCP, older test bodies)
     * has never heard of these keys and must not get a 400 on an otherwise
     * unrelated save, nor silently flip a zone back to HEATER or lose an
     * operator's fail-safe/hysteresis choice the next time one of those
     * clients replays a whole-page submit. Falls back to current_z (the live
     * value), never to a fixed default, for every field including
     * zone_type/failsafe_state -- the zones page itself always sends these
     * (this pass adds the control), so "omit preserves" only matters for a
     * client that predates the feature entirely. */
    snprintf(key, sizeof(key), "z%u_zonetype", i);
    {
        char probe[8];
        if (zones_config_json_field_present(body, key)) {
            uint8_t zone_type_raw;
            if (!zones_config_json_parse_u8_field(body, key, 0, (long)ZONE_TYPE_ON_OFF, &zone_type_raw)) {
                *err_reason = "zone zone_type out of range (0=heater, 1=on/off device)";
                return false;
            }
            z->zone_type = zone_type_raw;
        } else {
            z->zone_type = current_z->zone_type;
        }
    }
    snprintf(key, sizeof(key), "z%u_failsafe", i);
    {
        char probe[8];
        if (zones_config_json_field_present(body, key)) {
            uint8_t failsafe_raw;
            if (!zones_config_json_parse_u8_field(body, key, 0, 1, &failsafe_raw)) {
                *err_reason = "zone failsafe_state out of range (0=off, 1=on)";
                return false;
            }
            z->failsafe_state = failsafe_raw;
        } else {
            z->failsafe_state = current_z->failsafe_state;
        }
    }
    snprintf(key, sizeof(key), "z%u_hystc", i);
    {
        if (zones_config_json_field_present(body, key)) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_HYST_C_MAX, &z->hyst_c) ||
                (z->hyst_c != 0.0f && z->hyst_c < ZONE_HYST_C_MIN)) {
                *err_reason = "zone hyst_c out of range (0 = firmware default 2.0 C)";
                return false;
            }
        } else {
            z->hyst_c = current_z->hyst_c;
        }
    }
    /* min_on_s/min_off_s: uint16_t seconds, up to ZONE_MIN_ON_OFF_S_MAX
     * (3600) -- past zones_config_json_parse_u8_field()'s 0-255 range, so
     * parsed as a float (same helper heater_window_ms/heater_min_on_ms use
     * for their own millisecond counts, which run far higher than 3600) and
     * range/finite-checked before the narrowing cast to uint16_t. */
    snprintf(key, sizeof(key), "z%u_minons", i);
    {
        if (zones_config_json_field_present(body, key)) {
            float parsed;
            if (!zones_config_json_parse_float_field(body, key, 0.0f, (float)ZONE_MIN_ON_OFF_S_MAX, &parsed) ||
                (parsed != 0.0f && parsed < (float)ZONE_MIN_ON_OFF_S_MIN)) {
                *err_reason = "zone min_on_s out of range (0 = firmware default 30 s)";
                return false;
            }
            z->min_on_s = (uint16_t)parsed;
        } else {
            z->min_on_s = current_z->min_on_s;
        }
    }
    snprintf(key, sizeof(key), "z%u_minoffs", i);
    {
        if (zones_config_json_field_present(body, key)) {
            float parsed;
            if (!zones_config_json_parse_float_field(body, key, 0.0f, (float)ZONE_MIN_ON_OFF_S_MAX, &parsed) ||
                (parsed != 0.0f && parsed < (float)ZONE_MIN_ON_OFF_S_MIN)) {
                *err_reason = "zone min_off_s out of range (0 = firmware default 30 s)";
                return false;
            }
            z->min_off_s = (uint16_t)parsed;
        } else {
            z->min_off_s = current_z->min_off_s;
        }
    }

    /* ZONES_CFG_VERSION 24->25 (owner request: "allow the user to enter
     * different wattage for each coil"): per-coil nameplate wattage
     * override. OPTIONAL, same reasoning as z%u_hystc/z%u_minons above --
     * every pre-existing client has never heard of this key and must not
     * get a 400 on an otherwise unrelated save, nor silently lose an
     * operator's override the next time one of those clients replays a
     * whole-page submit. 0 is legal and means "not overridden, use an
     * equal share of the whole-kiln sum nameplate" (zones_config_json.h's
     * own coil_power_w comment) -- NOT validated against the sum here,
     * since the sum lives on the safety processor and may not have been
     * answered yet; that cross-check happens at the point of use
     * (zone_sweep_check_expected_current(), zones_current_sweep_engine.c). */
    snprintf(key, sizeof(key), "z%u_coilpower", i);
    {
        if (zones_config_json_field_present(body, key)) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_COIL_POWER_W_MAX,
                                                      &z->coil_power_w) ||
                (z->coil_power_w != 0.0f && z->coil_power_w < ZONE_COIL_POWER_W_MIN)) {
                *err_reason = "zone coil_power_w out of range (0 = use an equal share of the nameplate sum)";
                return false;
            }
        } else {
            z->coil_power_w = current_z->coil_power_w;
        }
    }

    /* 2026-08-27 (ZONES_CFG_VERSION 8->9, owner's request: "assign the zones
     * to them"): which timing_profiles[] slot this zone uses. REQUIRED, unlike
     * the nine fields it replaces (which were each individually OPTIONAL) --
     * there is no legacy client to preserve backward compatibility for here,
     * since no firmware before this pass ever had a "timing profile" concept
     * to send or omit. Bounded against timing_profile_count, the number of
     * profiles THIS submission itself defines (parsed by zones_post_handler()
     * before this per-zone loop runs -- see its own comment), not against
     * some fixed ceiling: a submission that only defines two profiles must not
     * let a zone reference a third one that doesn't exist in it. */
    snprintf(key, sizeof(key), "z%u_timingprofile", i);
    if (!zones_config_json_parse_u8_field(body, key, 0, timing_profile_count - 1, &z->timing_profile)) {
        *err_reason = "zone timing_profile missing or references a profile that doesn't exist "
                      "in this submission";
        return false;
    }

    /* TODO.md 10.8. Deliberately NOT validated/defaulted the same way as
     * z%u_xzone/z%u_k above (present-but-omit-means-0/disabled): omitting
     * this field must mean "this client doesn't know about multi-thermo,
     * keep controlling off the channel this zone always used", not "no
     * thermocouple assigned". zones_page.html doesn't send this field yet
     * (TODO.md 10.8's open item -- see the getter's header comment), and if
     * an absent field defaulted to 0 here, saving that page's form today
     * would silently blind every zone on the very next ordinary settings
     * save. bit i is that legacy mapping (zone i <-> channel i), same as
     * migrate_zones_cfg_v1_to_current() falls back to for an already-saved
     * blob that predates this field entirely -- one fallback rule for both
     * an old blob and a client that just doesn't send the key.
     *
     * A client that DOES know this field and sends it explicitly -- including
     * an explicit 0, deliberately clearing a zone's thermocouple -- is
     * honoured exactly as sent; present-but-out-of-range is still an error,
     * same discipline as relay_mask above. */
    snprintf(key, sizeof(key), "z%u_thermo_mask", i);
    {
        char probe[8];
        if (zones_config_json_field_present(body, key)) {
            uint8_t thermo_mask_raw;
            if (!zones_config_json_parse_u8_field(body, key, 0, 0xFF, &thermo_mask_raw)) {
                *err_reason = "zone thermo_mask missing or invalid";
                return false;
            }
            uint8_t valid_thermo_bits =
                thermo_count >= 8 ? 0xFF : (uint8_t)((1u << thermo_count) - 1u);
            if ((thermo_mask_raw & ~valid_thermo_bits) != 0) {
                *err_reason = "zone thermo_mask references an unconfigured thermocouple channel";
                return false;
            }
            z->thermo_mask = thermo_mask_raw;
        } else {
            z->thermo_mask = (uint8_t)(1u << i);
        }
    }

    /* 2026-08-27: purely informational (see ZONES_CFG_VERSION's 5->6
     * comment), so unlike thermo_mask above there is no legacy single-
     * channel mapping to preserve -- an omitted field is simply "no CT probe
     * mapped to this zone", the same safe-zero default a brand-new zone
     * already gets. Still validated against ZONE_CT_CHANNEL_COUNT (a fixed
     * hardware count, not relay_count/thermo_count) when present. */
    snprintf(key, sizeof(key), "z%u_ct_mask", i);
    {
        char probe[8];
        if (zones_config_json_field_present(body, key)) {
            uint8_t ct_mask_raw;
            if (!zones_config_json_parse_u8_field(body, key, 0, 0xFF, &ct_mask_raw)) {
                *err_reason = "zone ct_mask missing or invalid";
                return false;
            }
            uint8_t valid_ct_bits = (uint8_t)((1u << ZONE_CT_CHANNEL_COUNT) - 1u);
            if ((ct_mask_raw & ~valid_ct_bits) != 0) {
                *err_reason = "zone ct_mask references an unconfigured current-sense channel";
                return false;
            }
            z->ct_mask = ct_mask_raw;
        } else {
            z->ct_mask = 0;
        }
    }

    /* Sane numeric bounds -- firmware sanity bounds against a malformed/
     * typo'd submission, not real kiln-safety limits (that's the feasibility
     * check in profiles_http.c, on the ramp-rate side). Kp/Ki/Kd have no
     * natural physical bound, so 0..1000 is just generous headroom over
     * anything a real PID loop on this hardware would ever be tuned to. */
    snprintf(key, sizeof(key), "z%u_cal", i);
    if (!zones_config_json_parse_float_field(body, key, ZONE_CAL_OFFSET_MIN_C, ZONE_CAL_OFFSET_MAX_C, &z->cal_offset_c)) {
        *err_reason = "zone cal_offset_c missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_kp", i);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_PID_GAIN_MAX, &z->pid_kp)) {
        *err_reason = "zone pid_kp missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_ki", i);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_PID_GAIN_MAX, &z->pid_ki)) {
        *err_reason = "zone pid_ki missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_kd", i);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_PID_GAIN_MAX, &z->pid_kd)) {
        *err_reason = "zone pid_kd missing or out of range";
        return false;
    }
    /* ZONES_CFG_VERSION 12->13: invalidate the tuning-quality record (set 1
     * -- see zone_cfg_t::tuning_valid's own doc comment) when THIS path
     * changes the gains. *z started as *current_z (this function's own
     * comment/caller, "omit-means-preserve"), so z->tuning_valid already
     * carries the OLD record forward untouched by default -- exactly the
     * reset-one-side shape this whole feature exists to avoid, since this
     * whole-page submit writes pid_kp/ki/kd directly into the scratch
     * struct and never goes through zones_config_set_pid() (the narrow
     * POST /api/zones/pid endpoint's own choke point). A gain that reads
     * back identical to what was already stored (an unrelated field on the
     * same page changed, gains untouched) leaves the record standing --
     * only an ACTUAL change invalidates it. */
    /* z has NOT been seeded from current_z on this (in-range) path -- unlike
     * the i >= thermo_count early return above, which does `*z = *current_z`
     * wholesale, this path builds z field-by-field from the submission, and
     * the caller's z started zero-initialized (zones_post_handler()'s
     * memset(&tmp, 0, ...)). The tuning_* fields have no z%u_ POST key at
     * all (read-only, see GET /api/zones' own comment on this block), so
     * without an explicit carry-through here EVERY in-range save would zero
     * tuning_valid regardless of whether the gains actually changed -- the
     * exact reset-one-side shape this whole feature exists to avoid, just
     * from the opposite direction (wiping a GOOD record instead of keeping
     * a STALE one). Carry the old record through by default, then
     * invalidate ONLY on an actual gain change.
     *
     * "Gain changed" is zones_config_gain_changed() (zones_config_accessors.h,
     * which carries the tolerance rationale), the same helper
     * zones_config_set_pid_no_save() uses, so both paths agree. A tolerance
     * rather than `!=` because a repost through a lossy client (the old %.4f
     * GET) must not invalidate a good record on every resave; a client that
     * still rounds to %.4f does clear it for any gain below ~5, which is the
     * honest outcome (a Ki of 3.4e-5 reposted as 0.0000 really is zeroed;
     * test_gain_round_trip_at_9g_never_invalidates_any_magnitude covers the
     * lossless %.9g path). */
    z->tuning_valid = current_z->tuning_valid;
    z->tuning_method = current_z->tuning_method;
    z->tuning_rule = current_z->tuning_rule;
    z->tuning_settled = current_z->tuning_settled;
    z->tuning_extrapolation_converged = current_z->tuning_extrapolation_converged;
    z->tuning_tau_consistent = current_z->tuning_tau_consistent;
    z->tuning_baseline_c = current_z->tuning_baseline_c;
    z->tuning_step_ambient_c = current_z->tuning_step_ambient_c;
    z->tuning_raw_rise_c = current_z->tuning_raw_rise_c;
    z->tuning_rise_inf_c = current_z->tuning_rise_inf_c;
    z->tuning_seq = current_z->tuning_seq;
    /* model_fit_temp_c/model_fit_ambient_c (ZONES_CFG_VERSION 23->24):
     * exactly the same reset-one-side hazard as the tuning_* block just
     * above, for exactly the same reason -- these have no z%u_ POST key
     * either (zones_page.html never reads or writes them; only
     * zones_config_set_model_fit_context() does, from autotune's accept
     * path), so without this explicit carry-through every whole-page save
     * would silently zero a real fit's recorded operating point -- which,
     * unlike model_k_dc's own "0 means no model" convention, is NOT this
     * field's documented sentinel (ZONE_MODEL_FIT_TEMP_UNKNOWN, -273.15f)
     * and would misrepresent a known fit as one taken at a genuine 0 degC. */
    z->model_fit_temp_c = current_z->model_fit_temp_c;
    z->model_fit_ambient_c = current_z->model_fit_ambient_c;
    /* autotune_baseline_k_dc (ZONES_CFG_VERSION 25->26), opus adversarial
     * review of 9728865: the SAME reset-one-side hazard again, and the one
     * with the sharpest consequence of the three, because this field exists
     * specifically to be the fixed anchor that stops adaptive_tune's K_dc
     * ratchet. It has no z%u_ POST key either (its only writers are
     * autotune_engine_guard.c's accept path and adaptive_tune_refine_zone_
     * locked()'s one-shot bootstrap), so without this line EVERY whole-page
     * save from the zones page silently zeroed it -- and 0 is its "no
     * baseline recorded yet" sentinel, so the very next accepted refinement
     * would re-bootstrap the anchor from the live, already-adapted
     * model_k_dc. That is exactly the ratcheting reference 9728865 exists to
     * remove, merely gated behind an ordinary operator page save. */
    z->autotune_baseline_k_dc = current_z->autotune_baseline_k_dc;
    /* ZONES_CFG_VERSION 13->14's adaptive_tune_enabled, carried through for
     * exactly the same reason as the tuning_* block just above and for the
     * same reason coupling_diag_k_dc/fuzzy_strength_pct take the
     * omit-preserves branch below: it has NO z%u_ POST key at all (its only
     * writer is adaptive_tune.c via zones_config_set_adaptive_tune_enabled()),
     * and this path builds z field-by-field out of a caller-zeroed scratch
     * struct. Without this line every whole-page save from the zones page
     * silently cleared an operator's adaptive-tune opt-in for every in-range
     * zone and persisted the clear to NVS -- a reset-one-side defect, the
     * enable side having no idea the save happened. */
    z->adaptive_tune_enabled = current_z->adaptive_tune_enabled;
    if (zones_config_gain_changed(current_z->pid_kp, z->pid_kp) ||
        zones_config_gain_changed(current_z->pid_ki, z->pid_ki) ||
        zones_config_gain_changed(current_z->pid_kd, z->pid_kd)) {
        z->tuning_valid = 0;
    }
    snprintf(key, sizeof(key), "z%u_ramp", i);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_MAX_RAMP_C_PER_HR_MAX, &z->max_ramp_c_per_hr)) {
        *err_reason = "zone max_ramp_c_per_hr missing or out of range";
        return false;
    }
    /* 0 = "never configured" (profile_executor.c substitutes its own
     * default); 20 C/min is a generous ceiling -- well above anything this
     * board's bang-bang control could plausibly produce, just a sanity bound
     * against a typo. */
    snprintf(key, sizeof(key), "z%u_sanity", i);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_SANITY_RATE_MAX_C_PER_MIN, &z->sanity_rate_c_per_min)) {
        *err_reason = "zone sanity_rate_c_per_min missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_mode", i);
    uint8_t mode_raw;
    if (!zones_config_json_parse_u8_field(body, key, 0, (long)ZONE_CONTROL_MODE_PID_FUZZY, &mode_raw)) {
        *err_reason = "zone control_mode missing or out of range (0-3)";
        return false;
    }
    z->control_mode = mode_raw;
    /* 1400C ceiling matches PROFILE_TARGET_C_MAX (profiles_http.c) -- a
     * guard 5 limit tighter than what a profile could ever request would be
     * a contradiction between the two checks. */
    snprintf(key, sizeof(key), "z%u_maxtemp", i);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_MAX_TEMP_C_MAX, &z->max_temp_c)) {
        *err_reason = "zone max_temp_c missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_mintemp", i);
    if (!zones_config_json_parse_float_field(body, key, ZONE_MIN_TEMP_C_MIN, ZONE_MIN_TEMP_C_MAX, &z->min_temp_c)) {
        *err_reason = "zone min_temp_c missing or out of range";
        return false;
    }
    /* 0 = "not configured" (caller substitutes PROFILE_EXECUTOR_DEFAULT_*_MS,
     * same convention as sanity_rate_c_per_min above). 600000ms (10min) is a
     * generous upper bound on window_ms -- well past any window that would
     * still make sense against a kiln's thermal time constant; 60000ms on
     * min_on/min_off is the same generosity relative to window_ms's own
     * range. */
    snprintf(key, sizeof(key), "z%u_window", i);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_HEATER_WINDOW_MS_MAX, &z->heater_window_ms)) {
        *err_reason = "zone heater_window_ms missing or out of range";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_minon", i);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_HEATER_MIN_ON_OFF_MS_MAX, &z->heater_min_on_ms)) {
        *err_reason = "zone heater_min_on_ms missing or out of range";
        return false;
    }
    /* The one heater field with a LOWER bound too, and the only reason it
     * cannot just be another zones_config_json_parse_float_field() range: the accepted set is
     * disjoint (0, or >= the floor), not an interval. See
     * ZONE_HEATER_MIN_ON_MS_FLOOR in zones_http.h for why this is refused
     * rather than quietly raised. */
    if (z->heater_min_on_ms > 0.0f && z->heater_min_on_ms < ZONE_HEATER_MIN_ON_MS_FLOOR) {
        *err_reason = "zone heater_min_on_ms below the 10000 ms relay-protection floor "
                      "(use 0 for the firmware default)";
        return false;
    }
    /* The window-vs-min-on RELATIONSHIP (2026-08-29). Checked here, after
     * both fields are parsed, because it is the only check in this function
     * that needs two of them at once. A window that passes its own range but
     * fails this cannot render a fractional duty at all -- see
     * ZONE_HEATER_WINDOW_MIN_MULTIPLE in zones_http.h. */
    if (z->heater_window_ms > 0.0f && z->heater_window_ms < zone_required_window_ms(z->heater_min_on_ms)) {
        *err_reason = "zone heater_window_ms too short for its heater_min_on_ms: the window must be at "
                      "least 3x the minimum on-time or no fractional duty can be rendered "
                      "(use 0 for the firmware default)";
        return false;
    }
    snprintf(key, sizeof(key), "z%u_minoff", i);
    if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_HEATER_MIN_ON_OFF_MS_MAX, &z->heater_min_off_ms)) {
        *err_reason = "zone heater_min_off_ms missing or out of range";
        return false;
    }
    /* TODO.md 6A.3's remaining named guard thresholds. OPTIONAL, same reason
     * z%u_xzone below is: a submission that omits one leaves the
     * corresponding firmware default in force (z is zero-initialized by the
     * caller, and 0 is thermal_guard.c's own "substitute the default" value
     * for every one of these -- unlike z%u_xzone, omitting one of these does
     * NOT disable its guard). Bounds are generous sanity ceilings against a
     * typo, not real per-field tuning limits: rates 0-20C/min matches
     * z%u_sanity's own ceiling, windows/periods 0-7200s (2h) covers any
     * kiln's plausible time constant, debounce ticks 0-100, margin 0-500C. */
    snprintf(key, sizeof(key), "z%u_wrongdirwindow", i);
    {
        char probe[16];
        if (zones_http_field_nonblank(body, key)) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_TIME_S_MAX, &z->guard_wrong_dir_window_s)) {
                *err_reason = "zone guard_wrong_dir_window_s out of range";
                return false;
            }
        } else { /* blank OR missing */
            z->guard_wrong_dir_window_s = current_z->guard_wrong_dir_window_s; /* blank keeps the stored value (blank or missing); never a default/disable */
        }
    }
    snprintf(key, sizeof(key), "z%u_wrongdirrate", i);
    {
        char probe[16];
        if (zones_http_field_nonblank(body, key)) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_RATE_C_PER_MIN_MAX, &z->guard_wrong_dir_rate_c_per_min)) {
                *err_reason = "zone guard_wrong_dir_rate_c_per_min out of range";
                return false;
            }
        } else { /* blank OR missing */
            z->guard_wrong_dir_rate_c_per_min = current_z->guard_wrong_dir_rate_c_per_min; /* blank keeps the stored value (blank or missing); never a default/disable */
        }
    }
    snprintf(key, sizeof(key), "z%u_offsettle", i);
    {
        char probe[16];
        if (zones_http_field_nonblank(body, key)) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_TIME_S_MAX, &z->guard_off_settle_s)) {
                *err_reason = "zone guard_off_settle_s out of range";
                return false;
            }
        } else { /* blank OR missing */
            z->guard_off_settle_s = current_z->guard_off_settle_s; /* blank keeps the stored value (blank or missing); never a default/disable */
        }
    }
    snprintf(key, sizeof(key), "z%u_runawayrate", i);
    {
        char probe[16];
        if (zones_http_field_nonblank(body, key)) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_RATE_C_PER_MIN_MAX, &z->guard_runaway_rate_c_per_min)) {
                *err_reason = "zone guard_runaway_rate_c_per_min out of range";
                return false;
            }
        } else { /* blank OR missing */
            z->guard_runaway_rate_c_per_min = current_z->guard_runaway_rate_c_per_min; /* blank keeps the stored value (blank or missing); never a default/disable */
        }
    }
    snprintf(key, sizeof(key), "z%u_runawaymargin", i);
    {
        char probe[16];
        if (zones_http_field_nonblank(body, key)) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_MARGIN_C_MAX, &z->guard_runaway_margin_c)) {
                *err_reason = "zone guard_runaway_margin_c out of range";
                return false;
            }
        } else { /* blank OR missing */
            z->guard_runaway_margin_c = current_z->guard_runaway_margin_c; /* blank keeps the stored value (blank or missing); never a default/disable */
        }
    }
    snprintf(key, sizeof(key), "z%u_driftperiod", i);
    {
        char probe[16];
        if (zones_http_field_nonblank(body, key)) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_TIME_S_MAX, &z->guard_drift_period_s)) {
                *err_reason = "zone guard_drift_period_s out of range";
                return false;
            }
        } else { /* blank OR missing */
            z->guard_drift_period_s = current_z->guard_drift_period_s; /* blank keeps the stored value (blank or missing); never a default/disable */
        }
    }
    snprintf(key, sizeof(key), "z%u_debounce", i);
    {
        char probe[16];
        if (zones_http_field_nonblank(body, key)) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_DEBOUNCE_TICKS_MAX, &z->guard_sensor_fault_debounce_ticks)) {
                *err_reason = "zone guard_sensor_fault_debounce_ticks out of range";
                return false;
            }
        } else { /* blank OR missing */
            z->guard_sensor_fault_debounce_ticks = current_z->guard_sensor_fault_debounce_ticks; /* blank keeps the stored value (blank or missing); never a default/disable */
        }
    }
    snprintf(key, sizeof(key), "z%u_frozenwindow", i);
    {
        char probe[16];
        if (zones_http_field_nonblank(body, key)) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_GUARD_TIME_S_MAX, &z->guard_frozen_window_s)) {
                *err_reason = "zone guard_frozen_window_s out of range";
                return false;
            }
        } else { /* blank OR missing */
            z->guard_frozen_window_s = current_z->guard_frozen_window_s; /* blank keeps the stored value (blank or missing); never a default/disable */
        }
    }
    /* The nine v8 overrides that used to be parsed inline here now live on
     * the timing profile this zone points at -- see z%u_timingprofile above
     * and zones_config_json_parse_timing_profile_fields() (tp%u_progressduty..tp%u_ramplock),
     * parsed once per PROFILE rather than once per zone. */
    /* Guard 8. A submission that omits this field, or leaves it blank, keeps
     * the STORED threshold (owner decision: a thermal guard is never disabled
     * by omission; an explicit 0 is the only way to write 0). Older clients
     * (MCP/pc_tools, test harnesses) that post only the original 14 fields
     * therefore keep working AND no longer clear a saved threshold. Present
     * but malformed is still an error. 1000C is a sanity bound only. */
    snprintf(key, sizeof(key), "z%u_xzone", i);
    {
        char probe[16];
        if (zones_http_field_nonblank(body, key)) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_CROSS_ZONE_DELTA_C_MAX, &z->cross_zone_max_delta_c)) {
                *err_reason = "zone cross_zone_max_delta_c out of range";
                return false;
            }
        } else { /* blank OR missing */
            z->cross_zone_max_delta_c = current_z->cross_zone_max_delta_c; /* blank keeps the stored value (blank or missing); never a default/disable */
        }
    }
    /* The identified plant model (TODO.md 6A.4 -> 6A.2's feedforward).
     * OPTIONAL, for exactly the same reason z%u_xzone above is: a client
     * that predates these fields -- pc_tools/MCP, the test harnesses --
     * must not start getting 400s for a field it has never heard of.
     *
     * CONTRACT CHANGED 2026-09-14 (owner directive, docs/audits/
     * zones_post_omit_preserves_model_2026-09-14.md): omission now
     * PRESERVES the stored model instead of deleting it. Until this date the
     * comment here (see git history / the two audits below) documented the
     * OPPOSITE, deliberate "omit deletes it" behaviour, on the theory that
     * this field group should behave like every other one on the whole-page
     * submit and that it was safe in practice because both shipped clients
     * (zones_page.html, tools/PcTools/.../zones_http_client.py) always
     * re-echo all three keys from the last GET. That premise held right up
     * until it didn't: docs/audits/plant_model_loss_investigation_2026-09-14.md
     * found all three bench zones' identified models genuinely zeroed by a
     * whole-page POST that did NOT carry these keys (not a shipped-client
     * save -- something else hit the endpoint directly), undetected for three
     * days because no MCP tool surfaced the fields. See also
     * docs/audits/zones_post_model_key_omission_2026-09-13.md, which had
     * confirmed this was documented-intentional the day before the real
     * casualty was found; this comment is the retraction of that document's
     * conclusion, not a silent rewrite of it -- the old reasoning is named
     * here rather than erased, per this repo's "a retraction hid a stale
     * claim" lesson.
     *
     * These numbers are NOT typed by an operator; they are measured by a
     * multi-hour step test, so they now get the same omit-PRESERVES
     * convention every other measured-quantity field on this page already
     * uses (z%u_fuzzy_strength, z%u_coupling_c%u, coupling_diag_k_dc, etc,
     * just below) -- this field group is no longer the one documented
     * exception.
     *
     * Deliberate clear: this endpoint has no separate "clear model" key or
     * route. An explicit z%u_k=0&z%u_tau=0&z%u_deadtime=0 (all three,
     * present) still zeroes the model -- 0.0 remains model_k_dc's own "no
     * model" sentinel (zones_config_set_model()'s convention) -- but doing
     * so now requires a client to KNOW about and explicitly send these keys
     * with that value; a client that simply doesn't mention them (an old
     * client, a partial/malformed POST, anything that predates this field)
     * can no longer erase a real measurement by omission. That is
     * deliberately harder to trigger by accident than the old behaviour,
     * which erased on the mere absence of a key.
     *
     * Bounds are ZONE_MODEL_*_MAX so this path and zones_config_set_model()
     * accept exactly the same set of models; see their definition. Present
     * but malformed is still an error. */
    snprintf(key, sizeof(key), "z%u_k", i);
    {
        char probe[24];
        if (zones_config_json_field_present(body, key)) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_MODEL_K_MAX, &z->model_k_dc)) {
                *err_reason = "zone model K out of range";
                return false;
            }
        } else {
            z->model_k_dc = current_z->model_k_dc;
        }
    }
    snprintf(key, sizeof(key), "z%u_tau", i);
    {
        char probe[24];
        if (zones_config_json_field_present(body, key)) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_MODEL_TIME_MAX_S, &z->model_tau_s)) {
                *err_reason = "zone model tau out of range";
                return false;
            }
        } else {
            z->model_tau_s = current_z->model_tau_s;
        }
    }
    snprintf(key, sizeof(key), "z%u_deadtime", i);
    {
        char probe[24];
        if (zones_config_json_field_present(body, key)) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_MODEL_TIME_MAX_S, &z->model_dead_time_s)) {
                *err_reason = "zone model dead time out of range";
                return false;
            }
        } else {
            z->model_dead_time_s = current_z->model_dead_time_s;
        }
    }
    /* PID_EXPANSION_PLAN.md Phase 2/4 (2026-08-30): the fuzzy-PID and
     * cross-zone-coupling fields. OPTIONAL, same "older clients must not
     * start getting 400s for a field they've never heard of" reasoning as
     * z%u_xzone/z%u_k above -- but UNLIKE those, omitted means PRESERVE the
     * currently-stored value (current_z), the same convention z%u_tctype/
     * z%u_settings_source use, not "reset to 0". These three are measured
     * quantities (an operator-set adjustment knob, and an autotune-measured
     * coupling coefficient), and a whole-page save from a client that
     * predates this field (or simply didn't re-render every input) must not
     * silently delete a measurement/setting that took real effort to obtain
     * -- as of 2026-09-14 this is also the convention z%u_k/z%u_tau/
     * z%u_deadtime use just above, after a real casualty (docs/audits/
     * plant_model_loss_investigation_2026-09-14.md) proved that field group's
     * old "omit deletes it" behaviour was not actually safe in practice.
     * Present but out of range is still an error, never silently clamped
     * (PID_EXPANSION_PLAN.md's own "prove range checks refuse, not clamp"
     * rule). */
    snprintf(key, sizeof(key), "z%u_fuzzy_strength", i);
    {
        if (zones_config_json_field_present(body, key)) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_FUZZY_STRENGTH_PCT_MAX, &z->fuzzy_strength_pct)) {
                *err_reason = "zone fuzzy_strength_pct out of range (0-100)";
                return false;
            }
        } else {
            z->fuzzy_strength_pct = current_z->fuzzy_strength_pct;
        }
    }
    /* 2026-09-02 (ZONES_CFG_VERSION 14->15, PID_EXPANSION_PLAN.md 3.2
     * follow-up): the coupling identification's own diagonal cell -- see
     * zone_cfg_t::coupling_diag_k_dc's own doc comment. Same OPTIONAL,
     * omit-PRESERVES convention as z%u_fuzzy_strength/z%u_coupling_c%u just
     * above: this is a measured quantity, and a whole-page save from a
     * client that predates this field must not silently delete it. */
    snprintf(key, sizeof(key), "z%u_coupling_diag_k_dc", i);
    {
        if (zones_config_json_field_present(body, key)) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_MODEL_K_MAX, &z->coupling_diag_k_dc)) {
                *err_reason = "zone coupling_diag_k_dc out of range";
                return false;
            }
        } else {
            z->coupling_diag_k_dc = current_z->coupling_diag_k_dc;
        }
    }
    /* ZONES_CFG_VERSION 16->17 (PID_EXPANSION_PLAN.md sec 3.6d): the
     * terminal ease-off taper window multiplier, now per-zone -- was a
     * single whole-board z%u-less field (see zone_cfg_t::ease_off_window_
     * mult's own comment for why z0 needed its own reach). Same OPTIONAL/
     * omit-PRESERVES convention as z%u_coupling_diag_k_dc just above (an A/B
     * campaign toggling one zone's arm need not resubmit the whole form, and
     * a client that predates this field must not silently reset whichever
     * arm is currently running on THIS zone just by saving the zones page).
     *
     * Parsed against [0, MAX] first -- 0 is the legal "reset to the firmware
     * default" sentinel (same convention as pc_link_abort_silence_ms) --
     * then the (0, MIN) sliver zones_config_json_parse_float_field()'s
     * single contiguous range cannot express on its own is rejected here.
     * Same effective bound as the accessor setter/zones_config_json_
     * validate()'s per-zone check, just split across two tests because the
     * parser only takes one [min, max] pair. */
    snprintf(key, sizeof(key), "z%u_easeoffmult", i);
    {
        if (zones_config_json_field_present(body, key)) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_EASE_OFF_WINDOW_MULT_MAX,
                                   &z->ease_off_window_mult) ||
                (z->ease_off_window_mult != 0.0f && z->ease_off_window_mult < ZONE_EASE_OFF_WINDOW_MULT_MIN)) {
                *err_reason = "zone ease_off_window_mult out of range (0 = firmware default)";
                return false;
            }
        } else {
            z->ease_off_window_mult = current_z->ease_off_window_mult;
        }
    }
    /* ZONES_CFG_VERSION 17->18 (PID_EXPANSION_PLAN.md sec 3.6d / PER_ZONE_
     * TARGET_DESIGN_STUDY.md option (b)): the per-zone approach-rate cap.
     * Same OPTIONAL/omit-PRESERVES convention as z%u_easeoffmult just above
     * (an A/B campaign toggling one zone's cap need not resubmit the whole
     * form, and a client that predates this field must not silently clear
     * whichever cap is currently running on THIS zone just by saving the
     * zones page).
     *
     * Parsed against [0, MAX] first -- 0 is the legal "uncapped" sentinel --
     * then the (0, MIN) sliver zones_config_json_parse_float_field()'s
     * single contiguous range cannot express on its own is rejected here.
     * Same effective bound as the accessor setter/zones_config_json_
     * validate()'s per-zone check, split the same way z%u_easeoffmult's is. */
    snprintf(key, sizeof(key), "z%u_approachratecap", i);
    {
        if (zones_config_json_field_present(body, key)) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_APPROACH_RATE_CAP_C_PER_HR_MAX,
                                   &z->approach_rate_cap_c_per_hr) ||
                (z->approach_rate_cap_c_per_hr != 0.0f &&
                 z->approach_rate_cap_c_per_hr < ZONE_APPROACH_RATE_CAP_C_PER_HR_MIN)) {
                *err_reason = "zone approach_rate_cap_c_per_hr out of range (0 = uncapped)";
                return false;
            }
        } else {
            z->approach_rate_cap_c_per_hr = current_z->approach_rate_cap_c_per_hr;
        }
    }
    /* ZONES_CFG_VERSION 18->19 (PID_EXPANSION_PLAN.md sec 3.6g): the
     * fuzzy-PID membership-band widths. Same OPTIONAL/omit-PRESERVES
     * convention as z%u_easeoffmult/z%u_approachratecap just above (an
     * owner testing a rescaled band on one zone need not resubmit the whole
     * form, and a client that predates this field must not silently reset
     * whichever band is currently in effect on THIS zone just by saving the
     * zones page).
     *
     * Parsed against [0, MAX] first -- 0 is the legal "reset to the
     * firmware default" sentinel -- then the (0, MIN) sliver
     * zones_config_json_parse_float_field()'s single contiguous range
     * cannot express on its own is rejected here. Same effective bound as
     * the accessor setters/zones_config_json_validate()'s per-zone check,
     * split the same way z%u_easeoffmult's is. */
    snprintf(key, sizeof(key), "z%u_errorband", i);
    {
        if (zones_config_json_field_present(body, key)) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_ERROR_BAND_C_MAX,
                                   &z->error_band_c) ||
                (z->error_band_c != 0.0f && z->error_band_c < ZONE_ERROR_BAND_C_MIN)) {
                *err_reason = "zone error_band_c out of range (0 = firmware default)";
                return false;
            }
        } else {
            z->error_band_c = current_z->error_band_c;
        }
    }
    snprintf(key, sizeof(key), "z%u_rateband", i);
    {
        if (zones_config_json_field_present(body, key)) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_RATE_BAND_C_PER_S_MAX,
                                   &z->rate_band_c_per_s) ||
                (z->rate_band_c_per_s != 0.0f && z->rate_band_c_per_s < ZONE_RATE_BAND_C_PER_S_MIN)) {
                *err_reason = "zone rate_band_c_per_s out of range (0 = firmware default)";
                return false;
            }
        } else {
            z->rate_band_c_per_s = current_z->rate_band_c_per_s;
        }
    }
    /* ZONES_CFG_VERSION 21->22 (docs/audits/consumer_without_producer_
     * 2026-09-06.md finding 1): guard 1's arrival band. Same OPTIONAL/
     * omit-PRESERVES convention as z%u_errorband/z%u_rateband just above,
     * and the same [0, MAX] then (0, MIN)-sliver split. */
    snprintf(key, sizeof(key), "z%u_progressband", i);
    {
        if (zones_config_json_field_present(body, key)) {
            if (!zones_config_json_parse_float_field(body, key, 0.0f, ZONE_PROGRESS_BAND_C_MAX,
                                   &z->progress_band_c) ||
                (z->progress_band_c != 0.0f && z->progress_band_c < ZONE_PROGRESS_BAND_C_MIN)) {
                *err_reason = "zone progress_band_c out of range (0 = firmware default)";
                return false;
            }
        } else {
            z->progress_band_c = current_z->progress_band_c;
        }
    }
    /* 2026-08-30 (ZONES_CFG_VERSION 10->11): one indexed key per cell,
     * z%u_coupling_c%u -- e.g. z1_coupling_c0 is zone 1's measured response
     * to zone 0's heater. Same per-cell "omit preserves the currently-stored
     * value" convention z%u_fuzzy_strength above uses (these are measured
     * quantities; a whole-page save from a client that predates a cell must
     * not silently delete it), and the diagonal (j == i) is refused if a
     * client submits anything but 0 for it, matching zones_config_set_
     * coupling()'s own storage-layer rule. */
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        snprintf(key, sizeof(key), "z%u_coupling_c%u", i, j);
        if (zones_config_json_field_present(body, key)) {
            float cell;
            if (!zones_config_json_parse_float_field(body, key, 0.0f, (j == i) ? 0.0f : ZONE_COUPLING_COEFF_MAX, &cell)) {
                *err_reason = "zone coupling_coeff out of range";
                return false;
            }
            z->coupling_coeff[j] = cell;
        } else {
            z->coupling_coeff[j] = current_z->coupling_coeff[j];
        }
    }
    /* 2026-08-31 (ZONES_CFG_VERSION 11->12): coupling_tau_s[]/
     * coupling_dead_time_s[] have NO POST wire fields of their own -- unlike
     * coupling_coeff[], nothing on the settings page lets an operator type a
     * cross-zone time constant, so there is nothing to parse here. What DOES
     * matter is the omit case: `z` starts fresh (not copied from current_z),
     * so without this, every ordinary whole-page save from the settings page
     * -- which never sends these two arrays at all -- would silently zero out
     * whatever autotune_engine.c's finalize_fit() had persisted for every
     * zone, the exact "reset-one-side" class this codebase has shipped
     * before. Always preserve, unconditionally -- autotune_engine.c's own
     * persist path writes s_zones.cfg directly (via a coupling-cell setter),
     * never through this POST parser, so there is no legitimate way for a
     * POST to be the one updating these two arrays. */
    memcpy(z->coupling_tau_s, current_z->coupling_tau_s, sizeof(z->coupling_tau_s));
    memcpy(z->coupling_dead_time_s, current_z->coupling_dead_time_s, sizeof(z->coupling_dead_time_s));
    /* settings_source[group]: UNLIKE the three floats above, omitted must
     * NOT default to 0 -- 0 is a real, different value here ("copies zone
     * 0's settings"), not a safe empty default. Falls back to the CURRENT
     * stored value (current_z), same "omit preserves the live setting"
     * convention z%u_tctype uses just above, rather than to
     * ZONE_SETTINGS_SOURCE_CUSTOM unconditionally -- this is a whole-page
     * submit, and an older client that predates this field must not
     * silently flip every zone back to "custom" on an otherwise ordinary
     * save (see this file's own whole-page-submit discipline: every other
     * optional field either defaults to a safe zero or preserves the live
     * value, never invents a third behavior).
     *
     * docs/ARCHITECTURE_DECISIONS.md#zones-page-clean-up-info-disclosure-schema-v20-v21-chartjs (ZONES_CFG_VERSION 20->21): the old single
     * z%u_settings_source key becomes one key PER GROUP,
     * z%u_settings_source_<group> for <group> in the SRC_GROUP_NAMES list
     * below. The legacy scalar key is STILL ACCEPTED, applied to every
     * group that has no more-specific per-group key in the same submission
     * -- older clients (pc_tools/MCP, older browser tabs, the many
     * pre-existing test bodies that predate the per-group split) keep
     * working exactly as before, whole-zone mirroring all five groups at
     * once. A submission naming both the legacy key and a specific
     * per-group key for the same zone lets the per-group key win for that
     * one group -- the legacy key is a default, not an override. */
    {
        uint8_t legacy_raw = 0;
        bool have_legacy = false;
        snprintf(key, sizeof(key), "z%u_settings_source", i);
        if (zones_config_json_field_present(body, key)) {
            if (!zones_config_json_parse_u8_field(body, key, 0, 0xFF, &legacy_raw)) {
                *err_reason = "zone settings_source missing or invalid";
                return false;
            }
            have_legacy = true;
        }
        for (uint8_t g = 0; g < SRC_GROUP_COUNT; g++) {
            char gkey[40];
            snprintf(gkey, sizeof(gkey), "z%u_settings_source_%s", i, SRC_GROUP_NAMES[g]);
            uint8_t src_raw;
            bool have_group_key = zones_config_json_field_present(body, gkey);
            if (have_group_key) {
                if (!zones_config_json_parse_u8_field(body, gkey, 0, 0xFF, &src_raw)) {
                    *err_reason = "zone settings_source missing or invalid";
                    return false;
                }
            } else if (have_legacy) {
                src_raw = legacy_raw;
            } else {
                z->settings_source[g] = current_z->settings_source[g];
                continue;
            }
            if (src_raw != ZONE_SETTINGS_SOURCE_CUSTOM && src_raw >= MAX31856_CHANNEL_COUNT) {
                *err_reason = "zone settings_source references a zone that doesn't exist";
                return false;
            }
            /* Self-reference is the degenerate cycle ("zone 1 copies zone
             * 1"). The zones-POST handler owns the general cycle/disabled-
             * zone guards for the fully-assembled submission, but this one
             * case is free to reject here and saves that pass having to
             * unwind it. */
            if (src_raw == i) {
                *err_reason = "zone settings_source cannot point at itself";
                return false;
            }
            /* Longer cycle (2-zone, 3-zone, ...): same chain-walk
             * zones_config_set_settings_source() runs, against the live
             * s_zones.cfg for every OTHER zone, for THIS group only -- this
             * is a single-zone write (only slot i's link is changing here),
             * so any NEW cycle must run through zone i; walking from i
             * against everyone else's live value for this group is
             * sufficient, matching the setter's own reasoning. A whole-page
             * POST that changes several zones' links AT ONCE in a way that
             * only cycles once every change is applied is caught by the
             * zones-POST handler's own re-walk of every zone's chain (per
             * group) across the fully-assembled tmp.zones[], right after
             * this function's call site's loop and before that handler's
             * commit point -- see the comment there. */
            {
                /* Heap, not stack: zone_cfg_t[3] is well over 1 KB on the httpd stack. */
                zone_cfg_t *probe = persist_scratch_alloc(sizeof(zone_cfg_t) * MAX31856_CHANNEL_COUNT);
                if (probe == NULL) {
                    *err_reason = ZONES_HTTP_ERR_OOM;
                    return false;
                }
                memcpy(probe, s_zones.cfg.zones, sizeof(zone_cfg_t) * MAX31856_CHANNEL_COUNT);
                probe[i].settings_source[g] = src_raw;
                bool cycle = zones_config_json_settings_source_chain_has_cycle(probe, g, i, thermo_count);
                free(probe);
                if (cycle) {
                    *err_reason = "zone settings_source would create an inheritance cycle";
                    return false;
                }
            }
            z->settings_source[g] = src_raw;
        }
    }
    return true;
}
