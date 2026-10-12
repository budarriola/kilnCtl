/* profiles_validate.c -- the SAVE-TIME validators for RELAY_IO segments and on/off rules.
 *
 * Split out of profiles_http.c so test_backup_import.c can link the REAL validators instead of a
 * stub, and so backup import's pass 1 can run them against the post-import candidate state
 * (profile_validate_state_t) instead of the live state. A NULL state means "read the live
 * zones/aux config", exactly what profiles_http_save() and the edit/export handlers always did.
 *
 * This file is #included by profiles_http.c (and by test_backup_import.c) rather than compiled as
 * its own object, so no CMake or host-test source list needs to learn a new file. */
#include "on_off_trigger_decide.h" /* ON_OFF_PHASE_*, ON_OFF_DIR_*, ON_OFF_TEMP_CMP_* */
/* Owner's design rule, the exact one rules_http.c's check_relay_not_zone_owned()
 * already enforces for RULE-driven relays (that file is untouched by this pass
 * -- see profiles_http.h's profile_seg_kind_t comment): a relay already
 * assigned to a zone's heater output must never ALSO be reachable as a
 * profile segment target -- a segment turning it on/off would fight (or
 * silently lose to) that zone's own PID/bang-bang control of the same
 * contact. This is an independent copy of the same check, not a shared call
 * into rules_http.c: this file owns profile validation and rules_http.c owns
 * rule validation, and neither may depend on the other (rules_*.* is deleted
 * in a later task; this file must keep working the day that happens, same as
 * rules_http.c's own comment already notes about zones_config_get_relay_mask()
 * being read fresh every check, never cached, since a relay can be
 * (re)assigned to a zone at any time from the Thermocouples & Zones page).
 * relay_1_4 is 1-based, matching kiln_io_set_relay()'s convention. Returns
 * true (refuse) if ANY configured zone currently claims this relay. */
static bool profile_relay_is_zone_owned(const profile_validate_state_t *st, uint8_t relay_1_4,
                                        uint8_t *out_zone_index)
{
    uint8_t bit = (uint8_t)(1u << (relay_1_4 - 1u));
    uint8_t zone_count = st != NULL ? st->zone_count : zones_config_get_thermo_count();
    if (zone_count > MAX31856_CHANNEL_COUNT) {
        zone_count = MAX31856_CHANNEL_COUNT;
    }
    for (uint8_t zi = 0; zi < zone_count; zi++) {
        uint8_t zone_mask = 0;
        if (st != NULL) {
            zone_mask = st->zone_relay_mask[zi];
        } else if (!zones_config_get_relay_mask(zi, &zone_mask)) {
            continue;
        }
        if ((zone_mask & bit) != 0) {
            if (out_zone_index) *out_zone_index = zi;
            return true;
        }
    }
    return false;
}

/* Validates one RELAY_IO segment's target/flags -- the SAVE-TIME half of the
 * "two independent checks, deliberately" the owner's IO-side gate needs. The
 * second is profile_executor.c's own re-check at run start (relay_io_target_
 * is_zone_owned() in that file), for the same reason profile_post_handler's
 * feasibility check is re-run at run start too: a relay can be reassigned to
 * a zone AFTER a profile was saved, same reload-time hazard zones_http.c's
 * relay_mask comment and rules_task.c's compute_heater_relay_mask() both
 * already document. Only meaningful for seg->seg_kind ==
 * PROFILE_SEG_KIND_RELAY_IO -- callers check the kind first. */
bool validate_io_segment_in_state(const profile_segment_t *seg, uint8_t seg_num, const profile_validate_state_t *st,
                                  char *err_msg, size_t err_cap)
{
    uint8_t t = seg->io_target;
    bool is_relay = (t >= PROFILE_IO_TARGET_RELAY_BASE) && (t < PROFILE_IO_TARGET_RELAY_BASE + KILN_IO_RELAY_COUNT);
    bool is_io = (t >= PROFILE_IO_TARGET_IO_BASE) && (t < PROFILE_IO_TARGET_IO_BASE + KILN_IO_DIGITAL_COUNT);
    if (!is_relay && !is_io) {
        /* Covers PROFILE_IO_TARGET_NONE, the deliberate dead gap between the
         * two ranges (would-be DRDY/LCD encodings -- see profiles_http.h),
         * and anything past either range -- all rejected the same way,
         * before this value is ever turned into a kiln_io call. This is the
         * refusal the investigation found missing: kiln_io_set_io()'s own
         * index parameter structurally can't reach IO8-10 (~DRDY) or IO14-15
         * (LCD_IORQ/LCD_Reset) either (it only accepts 1-7), but that
         * structural limit lives in a driver two layers away from a saved
         * profile and must not be the ONLY thing standing between a bad
         * io_target value and those lines -- this gate is the explicit,
         * named one, checked before a value is ever handed to that driver. */
        snprintf(err_msg, err_cap,
                "segment %u: io_target %u is not a valid relay (1-%u) or IO (%u-%u) target",
                seg_num, t, (unsigned)KILN_IO_RELAY_COUNT, (unsigned)PROFILE_IO_TARGET_IO_BASE,
                (unsigned)(PROFILE_IO_TARGET_IO_BASE + KILN_IO_DIGITAL_COUNT - 1u));
        return false;
    }
    if (is_relay) {
        uint8_t owning_zone = 0;
        if (profile_relay_is_zone_owned(st, t, &owning_zone)) {
            snprintf(err_msg, err_cap,
                    "segment %u: relay %u is assigned to zone %u -- only relays not owned by any "
                    "zone can be a segment target",
                    seg_num, t, owning_zone);
            return false;
        }
        /* Owner decision 2026-10-04 (plan sec 14 item 10): a relay bound to an
         * ENABLED aux output belongs to the aux evaluator; a RELAY_IO segment
         * on it would fight the aux rule. Refused at save. */
        uint8_t aux_enabled = 0;
        if (st != NULL) {
            for (uint8_t ai = 0; ai < AUX_OUTPUTS_COUNT; ai++) {
                if (st->aux[ai].enabled) {
                    aux_enabled = (uint8_t)(aux_enabled | (1u << ai));
                }
            }
        } else {
            aux_enabled = aux_outputs_cfg_enabled_mask();
        }
        if ((aux_enabled & (uint8_t)(1u << (t - 1u))) != 0) {
            snprintf(err_msg, err_cap,
                    "segment %u: relay %u is bound to an aux output -- only relays not owned by a zone "
                    "or an aux output can be a segment target",
                    seg_num, t);
            return false;
        }
    }
    if (seg->io_leave_on_at_end && (seg->io_blocking || !seg->io_state)) {
        /* "Leave it on at run end" is nonsensical for a segment that isn't
         * commanding the relay/IO ON in the first place, and for a BLOCKING
         * segment the schedule has already waited for it and moved past it
         * by the time the run could possibly end mid-segment -- there is no
         * "still running when the profile ends" case for a blocking segment
         * to leave anything in. Rejected rather than silently ignored, same
         * as every other malformed-combination gate in this file. */
        snprintf(err_msg, err_cap,
                "segment %u: leave-on-at-end only applies to a non-blocking segment commanding the "
                "relay/IO ON",
                seg_num);
        return false;
    }
    return true;
}

/* docs/ON_OFF_ZONE.md plan step 5 validation -- shared by
 * profiles_http_save() (both the HTTP POST and UART-bridge entry points)
 * so a rule referencing a nonexistent segment or a non-ON_OFF zone can
 * never be persisted, regardless of entry point. THIS IS THE DANGEROUS
 * DIRECTION this check exists to close: a rule pointing at a HEATER zone
 * would let on/off (bang-bang, no PID, no guards 1-4/9) logic drive a real
 * heating element -- see this function's own negative test. Bounds every
 * numeric field so a corrupt/hand-crafted candidate cannot smuggle an
 * out-of-range value past decode. */
bool validate_on_off_rules_in_state(const profile_t *candidate, const profile_validate_state_t *st, char *err_msg,
                                    size_t err_cap)
{
    for (uint8_t i = 0; i < candidate->on_off_rule_count; i++) {
        const profile_on_off_rule_t *r = &candidate->on_off_rules[i];
        if (i >= PROFILE_MAX_ON_OFF_RULES) {
            snprintf(err_msg, err_cap, "rule %u: on_off_rule_count exceeds PROFILE_MAX_ON_OFF_RULES (%u)", i,
                     (unsigned)PROFILE_MAX_ON_OFF_RULES);
            return false;
        }
        if (r->segment_index >= candidate->segment_count) {
            snprintf(err_msg, err_cap, "rule %u: segment_index %u does not exist in this profile (%u segments)",
                     i, r->segment_index, candidate->segment_count);
            return false;
        }
        for (uint8_t j = 0; j < i; j++) {
            /* At most one rule per (segment, target): the executor's resolver takes the
             * first match and never looks for a second (profile_executor.c). */
            if (candidate->on_off_rules[j].segment_index == r->segment_index &&
                candidate->on_off_rules[j].zone_index == r->zone_index) {
                snprintf(err_msg, err_cap, "rule %u: duplicates rule %u (segment %u, target %u) -- one rule per "
                         "segment and target", i, j, r->segment_index, r->zone_index);
                return false;
            }
        }
        if (profile_rule_target_is_aux(r->zone_index)) {
            /* Aux target (plan sec 6): the aux entry for that relay must be enabled
             * and not conflicted; a temperature axis needs a valid tc_zone and the
             * "this zone's TC" source (1 = the entry's tc_zone). Sources 2/3 stay
             * reserved. */
            uint8_t relay = profile_rule_target_aux_relay(r->zone_index);
            aux_output_t ax;
            bool have_aux = true;
            if (st != NULL) {
                have_aux = relay >= 1u && relay <= AUX_OUTPUTS_COUNT;
                if (have_aux) {
                    ax = st->aux[relay - 1u];
                }
            } else {
                have_aux = aux_outputs_cfg_get(relay, &ax);
            }
            if (!have_aux) {
                snprintf(err_msg, err_cap, "rule %u: aux relay %u cannot be read", i, relay);
                return false;
            }
            if (ax.conflicted) {
                snprintf(err_msg, err_cap,
                         "rule %u: aux relay %u is conflicted (a zone also claims that relay) -- resolve it on the "
                         "zones page first",
                         i, relay);
                return false;
            }
            if (!ax.enabled) {
                snprintf(err_msg, err_cap,
                         "rule %u: aux relay %u is not an enabled aux output -- enable it on the zones page first",
                         i, relay);
                return false;
            }
            if (r->temp_source > 1) {
                snprintf(err_msg, err_cap, "rule %u: temp_source %u is reserved for aux targets (use 0 or 1)", i,
                         r->temp_source);
                return false;
            }
            if (r->temp_cmp != ON_OFF_TEMP_CMP_NONE &&
                (r->temp_source != 1 || ax.tc_zone == AUX_TC_ZONE_NONE)) {
                snprintf(err_msg, err_cap,
                         "rule %u: aux relay %u temperature rule needs temp_source 1 and a thermocouple zone "
                         "set on the aux output",
                         i, relay);
                return false;
            }
        } else {
            if (r->zone_index >= MAX31856_CHANNEL_COUNT) {
                snprintf(err_msg, err_cap,
                         "rule %u: zone_index %u out of range (zones 0-%u, aux relays %u-%u)", i, r->zone_index,
                         (unsigned)(MAX31856_CHANNEL_COUNT - 1u), (unsigned)PROFILE_RULE_TARGET_AUX_BASE,
                         (unsigned)(PROFILE_RULE_TARGET_AUX_BASE + PROFILE_RULE_TARGET_AUX_COUNT - 1u));
                return false;
            }
            zone_type_t zt = ZONE_TYPE_HEATER;
            bool have_zt = true;
            if (st != NULL) {
                zt = (zone_type_t)st->zone_type[r->zone_index];
            } else {
                have_zt = zones_config_get_zone_type(r->zone_index, &zt);
            }
            if (!have_zt || zt != ZONE_TYPE_ON_OFF) {
                /* THE dangerous direction: refuse a rule aimed at anything that
                 * is not (already, currently) a typed on/off device -- most
                 * importantly a HEATER, which on/off logic must never drive. */
                snprintf(err_msg, err_cap,
                         "rule %u: zone %u is not configured as an on/off device -- refusing to let on/off logic "
                         "drive it",
                         i, r->zone_index);
                return false;
            }
        }
        if ((r->phase_mask & (uint8_t)~(ON_OFF_PHASE_RAMP | ON_OFF_PHASE_DWELL)) != 0) {
            snprintf(err_msg, err_cap, "rule %u: phase_mask has unknown bits set", i);
            return false;
        }
        if ((r->direction_mask & (uint8_t)~(ON_OFF_DIR_HEATING | ON_OFF_DIR_COOLING | ON_OFF_DIR_FLAT)) != 0) {
            snprintf(err_msg, err_cap, "rule %u: direction_mask has unknown bits set", i);
            return false;
        }
        if (r->temp_cmp > ON_OFF_TEMP_CMP_BELOW) {
            snprintf(err_msg, err_cap, "rule %u: temp_cmp %u is not a known comparison", i, r->temp_cmp);
            return false;
        }
        if (r->temp_source > 3) {
            snprintf(err_msg, err_cap, "rule %u: temp_source %u is not a known source", i, r->temp_source);
            return false;
        }
        if (r->temp_source == 2 && r->temp_ref_zone >= MAX31856_CHANNEL_COUNT) {
            snprintf(err_msg, err_cap, "rule %u: temp_ref_zone %u out of range", i, r->temp_ref_zone);
            return false;
        }
        /* TODO (Opus review N8): the catalog/export/edit HTTP handlers do not
         * yet round-trip temp_ref_zone at all -- this bounds check is the
         * only place today that knows the field exists. When temp_source==2
         * (an explicit reference-zone thermocouple, distinct from
         * temp_source 0/1) is actually wired up end to end, add
         * temp_ref_zone to profiles_catalog_http.c's JSON output,
         * profiles_export_http.c's export/import, and profiles_edit_http.c's
         * form parser in the SAME change -- adding it to only one leaves the
         * others silently dropping or defaulting the field. */
        if (r->temp_cmp != ON_OFF_TEMP_CMP_NONE &&
            (isnan(r->temp_threshold_c) || r->temp_threshold_c < PROFILE_TARGET_C_MIN ||
             r->temp_threshold_c > PROFILE_TARGET_C_MAX)) {
            snprintf(err_msg, err_cap, "rule %u: temp_threshold_c out of range (%.0f-%.0f)", i,
                     (double)PROFILE_TARGET_C_MIN, (double)PROFILE_TARGET_C_MAX);
            return false;
        }
        /* time_stop_s == 0 means "to end of segment" (profiles_types.h) --
         * only a NONZERO stop must be after start. Both fields are
         * uint16_t, so an upper bound is enforced structurally already
         * (max 65535 s ~ 18.2h, comfortably above PROFILE_DWELL_MIN_MAX's
         * 1440 minutes/segment); no separate range check needed. */
        if (r->time_stop_s != 0 && r->time_stop_s <= r->time_start_s) {
            snprintf(err_msg, err_cap, "rule %u: time_stop_s must be after time_start_s (or 0 for end-of-segment)",
                     i);
            return false;
        }
        if (r->enable > 1 || r->invert > 1) {
            snprintf(err_msg, err_cap, "rule %u: enable/invert must be 0 or 1", i);
            return false;
        }
    }
    return true;
}

bool validate_io_segment(const profile_segment_t *seg, uint8_t seg_num, char *err_msg, size_t err_cap)
{
    return validate_io_segment_in_state(seg, seg_num, NULL, err_msg, err_cap);
}

bool validate_on_off_rules(const profile_t *candidate, char *err_msg, size_t err_cap)
{
    return validate_on_off_rules_in_state(candidate, NULL, err_msg, err_cap);
}
