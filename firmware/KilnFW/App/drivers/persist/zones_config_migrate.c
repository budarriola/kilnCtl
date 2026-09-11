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

static bool convert_versioned_blob_to_current(uint8_t version, const void *blob, zones_cfg_t *out)
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
    case 14: {
        /* v14 -> v15 (this pass): field-for-field carry-through, same shape
         * as case 13 above -- timing_profiles[]/timing_profile_count are
         * still in their CURRENT shape. The only real migration is
         * convert_zone_v14() leaving coupling_diag_k_dc at its zeroed
         * "not measured" default; see that function's own comment and
         * zone_cfg_t::coupling_diag_k_dc's ZONES_CFG_VERSION 14->15
         * comment. */
        zones_cfg_v14_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = src.safety_tc_type;
        out->pc_link_abort_silence_ms = src.pc_link_abort_silence_ms; /* real v14 value */
        out->timing_profile_count = src.timing_profile_count;
        memcpy(out->timing_profiles, src.timing_profiles, sizeof(out->timing_profiles));
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            convert_zone_v14(&src.zones[i], &out->zones[i]);
        }
        /* src.crc32 deliberately NOT carried over -- it covered the v14
         * shape; nvs_save() stamps a fresh one over the current (v15)
         * struct. */
        return true;
    }
    case 15: {
        /* v15 -> v17 (chained through the now-removed v16 shape): zone_cfg_t
         * at v15 predates BOTH the v16 global scalar and this pass's v17
         * per-zone field, so every zone lands on the 0 sentinel (via the
         * per-element copy's implicit zero-fill below, matching every other
         * pre-v16 case) -- which zones_config_get_ease_off_window_mult()
         * resolves to the real 2.0 default, exactly the old compile-time
         * PROFILE_EXECUTOR_EASE_OFF_WINDOW_MULT behaviour. zone_cfg_v16_t
         * (frozen in zones_config_json.h) is byte-for-byte identical to what
         * zone_cfg_t was at v15, so a per-element memcpy of that prefix is
         * exactly equivalent to the old "zone_cfg_t is byte-for-byte
         * identical" whole-array memcpy -- it just cannot be a single
         * whole-array memcpy any more now that the current zone_cfg_t has
         * grown one more tail field the v15 shape does not have. */
        zones_cfg_v15_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = src.safety_tc_type;
        out->pc_link_abort_silence_ms = src.pc_link_abort_silence_ms; /* real v15 value */
        out->timing_profile_count = src.timing_profile_count;
        memcpy(out->timing_profiles, src.timing_profiles, sizeof(out->timing_profiles));
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            /* src.zones[i] is zone_cfg_v16_t, not zone_cfg_v15_t -- see
             * zones_cfg_v15_t's own comment: it was repointed at
             * zone_cfg_v16_t (byte-for-byte identical to what zone_cfg_t
             * was at v15/v16) once the bare zone_cfg_t name stopped meaning
             * that shape, rather than freezing a separate, redundant type. */
            zone_cfg_migrate_prefix_and_tail(&out->zones[i], &src.zones[i], sizeof(src.zones[i]),
                                              offsetof(zone_cfg_v16_t, settings_source),
                                              offsetof(zone_cfg_v16_t, tuning_valid));
            /* out->zones[i].ease_off_window_mult already 0 from this
             * function's entry memset -- the sentinel, same as every
             * pre-v16 case. */
        }
        /* src.crc32 deliberately NOT carried over -- it covered the v15
         * shape; nvs_save() stamps a fresh one over the current (v17)
         * struct. */
        return true;
    }
    case 16: {
        /* v16 -> v17 (THIS pass, PID_EXPANSION_PLAN.md sec 3.6d): the single
         * board-wide ease_off_window_mult scalar becomes per-zone -- see
         * zone_cfg_t::ease_off_window_mult's own ZONES_CFG_VERSION 16->17
         * comment for why. Every zone gets EXACTLY the v16 board's one
         * global value carried forward VERBATIM, including the 0 sentinel
         * unchanged -- not re-defaulted, not re-resolved -- so an upgrading
         * board's ease-off behaviour is byte-for-byte identical on every
         * zone until an operator deliberately changes one zone's value.
         * This is what makes a v16->v17 upgrade produce EXACTLY today's
         * behaviour, the same guarantee every other single-field-added
         * migration in this switch documents for its own field. */
        zones_cfg_v16_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = src.safety_tc_type;
        out->pc_link_abort_silence_ms = src.pc_link_abort_silence_ms; /* real v16 value */
        out->timing_profile_count = src.timing_profile_count;
        memcpy(out->timing_profiles, src.timing_profiles, sizeof(out->timing_profiles));
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            zone_cfg_migrate_prefix_and_tail(&out->zones[i], &src.zones[i], sizeof(src.zones[i]),
                                              offsetof(zone_cfg_v16_t, settings_source),
                                              offsetof(zone_cfg_v16_t, tuning_valid));
            out->zones[i].ease_off_window_mult = src.ease_off_window_mult; /* the v16 global, carried verbatim */
        }
        /* src.crc32 deliberately NOT carried over -- it covered the v16
         * shape; nvs_save() stamps a fresh one over the current (v17)
         * struct. */
        return true;
    }
    case 17: {
        /* v17 -> v18 (THIS pass, PID_EXPANSION_PLAN.md sec 3.6d /
         * PER_ZONE_TARGET_DESIGN_STUDY.md option (b)): approach_rate_cap_
         * c_per_hr is brand new -- see this function's own top-of-function
         * comment for why every zone simply lands on the 0 (uncapped)
         * sentinel via the entry memset, with no explicit per-zone
         * assignment needed the way case 16 needed one for its own
         * (carried-forward, non-zero-capable) global scalar. zone_cfg_v17_t
         * (frozen in zones_config_json.h) is byte-for-byte identical to
         * what zone_cfg_t was at v17, so a per-element memcpy of that
         * prefix is exactly equivalent to a whole-array memcpy, just typed
         * against the smaller historical shape -- same technique case 15
         * uses against zone_cfg_v16_t. */
        zones_cfg_v17_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = src.safety_tc_type;
        out->pc_link_abort_silence_ms = src.pc_link_abort_silence_ms; /* real v17 value */
        out->timing_profile_count = src.timing_profile_count;
        memcpy(out->timing_profiles, src.timing_profiles, sizeof(out->timing_profiles));
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            zone_cfg_migrate_prefix_and_tail(&out->zones[i], &src.zones[i], sizeof(src.zones[i]),
                                              offsetof(zone_cfg_v17_t, settings_source),
                                              offsetof(zone_cfg_v17_t, tuning_valid));
            /* out->zones[i].approach_rate_cap_c_per_hr already 0 (uncapped)
             * from this function's entry memset -- brand-new mechanism, no
             * prior global opinion to carry forward, unlike
             * ease_off_window_mult's v16->v17 migration just above. */
        }
        /* src.crc32 deliberately NOT carried over -- it covered the v17
         * shape; nvs_save() stamps a fresh one over the current (v18)
         * struct. */
        return true;
    }
    case 18: {
        /* v18 -> v19 (THIS pass, PID_EXPANSION_PLAN.md sec 3.6g): error_
         * band_c/rate_band_c_per_s are brand new -- see this function's own
         * top-of-function comment for why every zone simply lands on the 0
         * (use-firmware-default) sentinel via the entry memset, with no
         * explicit per-zone assignment needed. zone_cfg_v18_t (frozen in
         * zones_config_json.h) is byte-for-byte identical to what
         * zone_cfg_t was at v18, so a per-element memcpy of that prefix is
         * exactly equivalent to a whole-array memcpy, just typed against
         * the smaller historical shape -- same technique case 17 uses
         * against zone_cfg_v17_t. */
        zones_cfg_v18_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = src.safety_tc_type;
        out->pc_link_abort_silence_ms = src.pc_link_abort_silence_ms; /* real v18 value */
        out->timing_profile_count = src.timing_profile_count;
        memcpy(out->timing_profiles, src.timing_profiles, sizeof(out->timing_profiles));
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            zone_cfg_migrate_prefix_and_tail(&out->zones[i], &src.zones[i], sizeof(src.zones[i]),
                                              offsetof(zone_cfg_v18_t, settings_source),
                                              offsetof(zone_cfg_v18_t, tuning_valid));
            /* out->zones[i].error_band_c/rate_band_c_per_s already 0 (use
             * firmware default) from this function's entry memset --
             * brand-new mechanism, no prior global opinion to carry
             * forward, same as approach_rate_cap_c_per_hr's own v17->v18
             * migration just above. */
        }
        /* src.crc32 deliberately NOT carried over -- it covered the v18
         * shape; nvs_save() stamps a fresh one over the current (v19)
         * struct. */
        return true;
    }
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
        zones_cfg_v19_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = src.safety_tc_type;
        out->pc_link_abort_silence_ms = src.pc_link_abort_silence_ms; /* real v19 value */
        out->timing_profile_count = src.timing_profile_count;
        memcpy(out->timing_profiles, src.timing_profiles, sizeof(out->timing_profiles));
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            zone_cfg_migrate_prefix_and_tail(&out->zones[i], &src.zones[i], sizeof(src.zones[i]),
                                              offsetof(zone_cfg_v19_t, settings_source),
                                              offsetof(zone_cfg_v19_t, tuning_valid));
            /* out->zones[i].relay_type already 0 (ssr) from this function's
             * entry memset -- brand-new mechanism, no prior global opinion
             * to carry forward, same as error_band_c/rate_band_c_per_s's
             * own v18->v19 migration just above. */
        }
        /* src.crc32 deliberately NOT carried over -- it covered the v19
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
        zones_cfg_v20_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = src.safety_tc_type;
        out->pc_link_abort_silence_ms = src.pc_link_abort_silence_ms; /* real v20 value */
        out->timing_profile_count = src.timing_profile_count;
        memcpy(out->timing_profiles, src.timing_profiles, sizeof(out->timing_profiles));
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            const zone_cfg_v20_t *s = &src.zones[i];
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
        /* src.crc32 deliberately NOT carried over -- it covered the v20
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
        zones_cfg_v21_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = src.safety_tc_type;
        out->pc_link_abort_silence_ms = src.pc_link_abort_silence_ms; /* real v21 value */
        out->timing_profile_count = src.timing_profile_count;
        memcpy(out->timing_profiles, src.timing_profiles, sizeof(out->timing_profiles));
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            memcpy(&out->zones[i], &src.zones[i], sizeof(src.zones[i]));
            /* out->zones[i].progress_band_c already 0 from this function's
             * entry memset -- see this case's own top comment. */
        }
        /* src.crc32 deliberately NOT carried over -- it covered the v21
         * shape; nvs_save() stamps a fresh one over the current (v22)
         * struct. */
        return true;
    }
    case 22: {
        /* v22 -> v23 (THIS pass, docs/ON_OFF_ZONE_PLAN.md step 1):
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
        zones_cfg_v22_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = src.safety_tc_type;
        out->pc_link_abort_silence_ms = src.pc_link_abort_silence_ms; /* real v22 value */
        out->timing_profile_count = src.timing_profile_count;
        memcpy(out->timing_profiles, src.timing_profiles, sizeof(out->timing_profiles));
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            memcpy(&out->zones[i], &src.zones[i], sizeof(src.zones[i]));
            /* out->zones[i].zone_type/failsafe_state/hyst_c/min_on_s/
             * min_off_s already 0 from this function's entry memset -- see
             * this case's own top comment. */
        }
        /* src.crc32 deliberately NOT carried over -- it covered the v22
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
        zones_cfg_v23_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = src.safety_tc_type;
        out->pc_link_abort_silence_ms = src.pc_link_abort_silence_ms; /* real v23 value */
        out->timing_profile_count = src.timing_profile_count;
        memcpy(out->timing_profiles, src.timing_profiles, sizeof(out->timing_profiles));
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            memcpy(&out->zones[i], &src.zones[i], sizeof(src.zones[i]));
            /* out->zones[i].model_fit_temp_c/model_fit_ambient_c backfilled
             * to the UNKNOWN sentinel by the caller -- see this case's own
             * top comment. */
        }
        /* src.crc32 deliberately NOT carried over -- it covered the v23
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
        zones_cfg_v24_t src;
        memcpy(&src, blob, sizeof(src));
        out->thermo_count = src.thermo_count;
        out->relay_count = src.relay_count;
        out->max_simultaneous_relays = src.max_simultaneous_relays;
        out->continue_on_zone_trip = src.continue_on_zone_trip;
        out->safety_tc_type = src.safety_tc_type;
        out->pc_link_abort_silence_ms = src.pc_link_abort_silence_ms; /* real v24 value */
        out->timing_profile_count = src.timing_profile_count;
        memcpy(out->timing_profiles, src.timing_profiles, sizeof(out->timing_profiles));
        for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
            memcpy(&out->zones[i], &src.zones[i], sizeof(src.zones[i]));
            /* out->zones[i].coil_power_w already 0 from this function's
             * entry memset -- see this case's own top comment. */
        }
        /* src.crc32 deliberately NOT carried over -- it covered the v24
         * shape; nvs_save() stamps a fresh one over the current (v25)
         * struct. */
        return true;
    }
    default:
        /* No known historical (or current) layout for this version --
         * zones_cfg_expected_len_for_version() already returned 0 for it and
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
