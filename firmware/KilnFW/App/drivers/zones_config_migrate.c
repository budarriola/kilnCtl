// Version-dispatch migration and the public decode entry point for
// zones_config_json.c's split (ROADMAP.md M15, the 1500-line rule) -- see
// zones_config_json_internal.h's own doc comment for the full three-file map.
#include "zones_config_json_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_crc.h"
#include "esp_log.h"


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
     * defaulted. */
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
            memcpy(&out->zones[i], &src.zones[i], sizeof(src.zones[i]));
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
            memcpy(&out->zones[i], &src.zones[i], sizeof(src.zones[i]));
            out->zones[i].ease_off_window_mult = src.ease_off_window_mult; /* the v16 global, carried verbatim */
        }
        /* src.crc32 deliberately NOT carried over -- it covered the v16
         * shape; nvs_save() stamps a fresh one over the current (v17)
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
