#include "profiles_http_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "MAX31856.h"
#include "http_form.h"
#include "live_profile.h" /* live_edit_name_collides() -- profile_post_handler()'s dup-name refusal */
#include "profile_executor.h" /* profile_executor_get_status() -- Opus review item 2,
                                 * PROFILE_SLOTS_100.md section 7: refuse to delete
                                 * the slot the executor is currently running/paused on. */
#include "profiles_builtin.h"
#include "profiles_favorites.h"
#include "cfg_fs_refusal_http.h"
#include "zones_config_accessors.h"

/* profiles_http_json_escape() now lives in profiles_http_internal.h (Opus review of
 * 5dd23944, finding 3) -- see that header's doc comment. Needed here
 * because the dup-name refusal below echoes an operator-supplied profile
 * name back into a JSON error body. */


/* ---- POST /api/profile ----------------------------------------------------
 * Validates into a scratch profile_t before touching s_profiles/NVS. Runs
 * the TODO.md section 5 feasibility check against zones_http.c's
 * user-entered per-zone ramp ceiling (zones_config_get_max_ramp()): a
 * segment whose ramp rate exceeds the ceiling rejects the whole submission;
 * one within 20% of it (PROFILE_RAMP_WARN_FRACTION) is accepted with a
 * warning, per TODO.md's explicit "warn, don't block" rule at that margin.
 * This is the creation-time half of that check -- TODO.md also calls for
 * re-checking at profile-*start* time, which belongs to the profile
 * executor, not this page, and profile_executor.c does exactly that against
 * every participating zone's ceiling as it stands at start. */

bool profiles_parse_profile_fields(const char *body, profile_t *p, char *err_msg, size_t err_cap)
{
    char name[PROFILE_NAME_MAX_LEN + 1];
    int name_len = http_form_find_field(body, "name", name, sizeof(name));
    if (name_len == -2) {
        snprintf(err_msg, err_cap, "name too long");
        return false;
    }
    if (name_len <= 0) {
        snprintf(err_msg, err_cap, "name missing");
        return false;
    }
    if (http_form_value_has_ctl(name, name_len)) {
        snprintf(err_msg, err_cap, "name contains a control character");
        return false;
    }
    strncpy(p->name, name, PROFILE_NAME_MAX_LEN);
    p->name[PROFILE_NAME_MAX_LEN] = '\0';

    char zone_val[8];
    int zone_len = http_form_find_field(body, "zone_mask", zone_val, sizeof(zone_val));
    if (zone_len <= 0) {
        snprintf(err_msg, err_cap, "zone_mask missing");
        return false;
    }
    char *end = NULL;
    long zone_mask = strtol(zone_val, &end, 10);
    uint8_t thermo_count = zones_config_get_thermo_count();
    uint8_t valid_bits = thermo_count >= 8 ? 0xFF : (uint8_t)((1u << thermo_count) - 1u);
    if (end == zone_val || zone_mask <= 0 || zone_mask > 0xFF || ((uint8_t)zone_mask & ~valid_bits) != 0) {
        snprintf(err_msg, err_cap,
                "zone_mask must select at least one configured zone (check Thermocouples & Zones settings)");
        return false;
    }
    p->zone_mask = (uint8_t)zone_mask;

    char seg_count_val[8];
    int seg_count_len = http_form_find_field(body, "seg_count", seg_count_val, sizeof(seg_count_val));
    if (seg_count_len <= 0) {
        snprintf(err_msg, err_cap, "seg_count missing");
        return false;
    }
    end = NULL;
    long seg_count = strtol(seg_count_val, &end, 10);
    if (end == seg_count_val || seg_count < 1 || seg_count > PROFILE_MAX_SEGMENTS) {
        snprintf(err_msg, err_cap, "seg_count out of range (1-12)");
        return false;
    }
    p->segment_count = (uint8_t)seg_count;

    for (uint8_t i = 0; i < p->segment_count; i++) {
        /* 24, not 16. The longest key built here is "seg%u_io_blocking", and
         * at the last segment index that is "seg11_io_blocking" -- 17
         * characters plus the terminator, which does not fit 16. The MSVC
         * host build does not run -Wformat-truncation, so this compiled and
         * passed every host test; only the target build (-Werror=format-
         * truncation) caught it. A truncated key would not have failed
         * loudly either: http_form_find_field() would simply not find
         * "seg11_io_blockin", and the field would silently read as absent,
         * taking its default. 2026-08-28. */
        char key[24];
        profile_segment_t *seg = &p->segments[i];
        memset(seg, 0, sizeof(*seg));

        /* seg%u_kind is OPTIONAL and defaults to PROFILE_SEG_KIND_ZONE_RAMP
         * (0) when absent -- every existing caller of this endpoint (the
         * profiles_page.html editor as it stands today, and any UART/scripted
         * submission written before this pass) never sends it and must keep
         * producing exactly the temperature-ramp segment it always has. */
        snprintf(key, sizeof(key), "seg%u_kind", i);
        char val[24];
        int len = http_form_find_field(body, key, val, sizeof(val));
        char *fend = NULL;
        long kind = len > 0 ? strtol(val, &fend, 10) : PROFILE_SEG_KIND_ZONE_RAMP;
        if (len > 0 && fend == val) {
            kind = PROFILE_SEG_KIND_ZONE_RAMP;
        }
        if (kind != PROFILE_SEG_KIND_ZONE_RAMP && kind != PROFILE_SEG_KIND_RELAY_IO) {
            snprintf(err_msg, err_cap, "segment %u: unknown segment kind %ld", i + 1, kind);
            return false;
        }
        seg->seg_kind = (uint8_t)kind;

        if (seg->seg_kind == PROFILE_SEG_KIND_RELAY_IO) {
            snprintf(key, sizeof(key), "seg%u_io_target", i);
            len = http_form_find_field(body, key, val, sizeof(val));
            end = NULL;
            long io_target = len > 0 ? strtol(val, &end, 10) : -1;
            if (len <= 0 || end == val || io_target < 0 || io_target > 255) {
                snprintf(err_msg, err_cap, "segment %u: io_target missing or out of range", i + 1);
                return false;
            }
            seg->io_target = (uint8_t)io_target;

            snprintf(key, sizeof(key), "seg%u_io_state", i);
            len = http_form_find_field(body, key, val, sizeof(val));
            seg->io_state = (len > 0 && val[0] != '0') ? 1 : 0;

            snprintf(key, sizeof(key), "seg%u_io_blocking", i);
            len = http_form_find_field(body, key, val, sizeof(val));
            /* Missing defaults to BLOCKING (1) -- the safer of the two: a
             * segment nobody said was non-blocking should still hold up the
             * schedule and get an explicit force-off at its own end, rather
             * than silently running loose in the background. */
            seg->io_blocking = (len <= 0 || val[0] != '0') ? 1 : 0;

            snprintf(key, sizeof(key), "seg%u_io_leave_on", i);
            len = http_form_find_field(body, key, val, sizeof(val));
            /* Owner's explicit instruction: "Default must be OFF (force it
             * off)". Missing, empty, or "0" all mean off -- only an explicit
             * nonzero value turns this on. */
            seg->io_leave_on_at_end = (len > 0 && val[0] != '0') ? 1 : 0;

            snprintf(key, sizeof(key), "seg%u_dwell", i);
            len = http_form_find_field(body, key, val, sizeof(val));
            end = NULL;
            long dwell = len > 0 ? strtol(val, &end, 10) : 0; /* missing = 0, same as "no hold" */
            if (len > 0 && (end == val || dwell < 0 || dwell > (long)PROFILE_DWELL_MIN_MAX)) {
                snprintf(err_msg, err_cap, "segment %u: dwell_min out of range (0-1440)", i + 1);
                return false;
            }
            seg->dwell_min = (uint32_t)(dwell < 0 ? 0 : dwell);

            if (!validate_io_segment(seg, (uint8_t)(i + 1), err_msg, err_cap)) {
                return false;
            }
            continue;
        }

        snprintf(key, sizeof(key), "seg%u_target", i);
        len = http_form_find_field(body, key, val, sizeof(val));
        fend = NULL;
        float target = len > 0 ? strtof(val, &fend) : NAN;
        if (len <= 0 || fend == val || isnan(target) || target < PROFILE_TARGET_C_MIN ||
            target > PROFILE_TARGET_C_MAX) {
            snprintf(err_msg, err_cap, "segment %u: target_c missing or out of range (%.0f-%.0f)", i + 1,
                     (double)PROFILE_TARGET_C_MIN, (double)PROFILE_TARGET_C_MAX);
            return false;
        }
        seg->target_c = target;

        snprintf(key, sizeof(key), "seg%u_ramp", i);
        len = http_form_find_field(body, key, val, sizeof(val));
        fend = NULL;
        float ramp = len > 0 ? strtof(val, &fend) : NAN;
        if (len <= 0 || fend == val || isnan(ramp) || ramp < PROFILE_RAMP_C_PER_HR_MIN ||
            ramp > PROFILE_RAMP_C_PER_HR_MAX) {
            snprintf(err_msg, err_cap, "segment %u: ramp_c_per_hr missing or out of range (0-1000)", i + 1);
            return false;
        }
        seg->ramp_c_per_hr = ramp;

        snprintf(key, sizeof(key), "seg%u_dwell", i);
        len = http_form_find_field(body, key, val, sizeof(val));
        end = NULL;
        long dwell = len > 0 ? strtol(val, &end, 10) : -1;
        if (len <= 0 || end == val || dwell < 0 || dwell > (long)PROFILE_DWELL_MIN_MAX) {
            snprintf(err_msg, err_cap, "segment %u: dwell_min missing or out of range (0-1440)", i + 1);
            return false;
        }
        seg->dwell_min = (uint32_t)dwell;
    }

    /* docs/ON_OFF_ZONE.md plan step 5 API surface -- indexed field
     * family "rule0_zone=3&rule0_segment=2&..." matching how segments are
     * already encoded above. A rule slot is present iff "rule%u_zone" is
     * present; presence stops at the first gap (no sparse holes over the
     * wire -- the stored array itself may still end up sparse in the sense
     * that not every segment/zone pair has one, but the wire encoding is a
     * dense 0..N-1 list). Range/reference validation (segment exists, zone
     * is ON_OFF, numeric bounds) is deliberately NOT duplicated here -- this
     * parser accepts any target byte 0..255 and leaves the verdict to
     * validate_on_off_rules() (profiles_http.c), which runs from
     * profiles_validate_candidate() (every POST /api/profile and live-edit
     * path) and again from profiles_http_save() (UART bridge/import), so a
     * rule can never be accepted by one entry point and rejected by another. */
    p->on_off_rule_count = 0;
    for (uint8_t i = 0; i < PROFILE_MAX_ON_OFF_RULES; i++) {
        char key[24];
        char val[24];
        snprintf(key, sizeof(key), "rule%u_zone", i);
        int len = http_form_find_field(body, key, val, sizeof(val));
        if (len <= 0) {
            break; /* first gap ends the list */
        }
        profile_on_off_rule_t *r = &p->on_off_rules[i];
        memset(r, 0, sizeof(*r));

        char *fend = NULL;
        long zone_index = strtol(val, &fend, 10);
        if (fend == val || zone_index < 0 || zone_index > 255) {
            snprintf(err_msg, err_cap, "rule %u: zone missing or out of range", i);
            return false;
        }
        /* rule%u_zone is the rule TARGET byte: 0..2 = a zone, 8..11 = aux relay
         * 1..4 (profile_rule_target.h, SPARE_RELAY_ONOFF_PLAN sec 6). Passed
         * through verbatim; profiles_validate_candidate() ->
         * validate_on_off_rules() owns the range/aux/duplicate checks. */
        r->zone_index = (uint8_t)zone_index;

        snprintf(key, sizeof(key), "rule%u_segment", i);
        len = http_form_find_field(body, key, val, sizeof(val));
        fend = NULL;
        long seg_idx = len > 0 ? strtol(val, &fend, 10) : -1;
        if (len <= 0 || fend == val || seg_idx < 0 || seg_idx > 255) {
            snprintf(err_msg, err_cap, "rule %u: segment missing or out of range", i);
            return false;
        }
        r->segment_index = (uint8_t)seg_idx;

        snprintf(key, sizeof(key), "rule%u_enable", i);
        len = http_form_find_field(body, key, val, sizeof(val));
        r->enable = (len > 0 && val[0] != '0') ? 1 : 0;

        snprintf(key, sizeof(key), "rule%u_phase", i);
        len = http_form_find_field(body, key, val, sizeof(val));
        fend = NULL;
        long phase = len > 0 ? strtol(val, &fend, 10) : 0;
        r->phase_mask = (fend == val) ? 0 : (uint8_t)(phase & 0xFF);

        snprintf(key, sizeof(key), "rule%u_direction", i);
        len = http_form_find_field(body, key, val, sizeof(val));
        fend = NULL;
        long direction = len > 0 ? strtol(val, &fend, 10) : 0;
        r->direction_mask = (fend == val) ? 0 : (uint8_t)(direction & 0xFF);

        snprintf(key, sizeof(key), "rule%u_temp_cmp", i);
        len = http_form_find_field(body, key, val, sizeof(val));
        fend = NULL;
        long temp_cmp = len > 0 ? strtol(val, &fend, 10) : 0;
        r->temp_cmp = (fend == val) ? 0 : (uint8_t)(temp_cmp & 0xFF);

        /* rule%u_temp_source was missing from this wire encoding until now --
         * profile_resolve_on_off_rule() (profile_executor.c) only honors
         * temp_cmp when temp_source == 1 ("measured, this zone's own TC"),
         * so without this field a rule%u_temp_cmp posted from the editor was
         * silently ignored at run time (temp_source stayed 0 = none, the
         * memset()'d default above). Only 0 (none)/1 (this zone's TC) are
         * wired by the executor as of ON_OFF_ZONE.md plan step 5 --
         * 2 (named zone)/3 (executor setpoint) are reserved encoding space,
         * so the client only ever needs to send 0 or 1 today; a value out of
         * 0-3 collapses to 0 here (validate_on_off_rules() re-checks the
         * range server-side regardless). */
        snprintf(key, sizeof(key), "rule%u_temp_source", i);
        len = http_form_find_field(body, key, val, sizeof(val));
        fend = NULL;
        long temp_source = len > 0 ? strtol(val, &fend, 10) : 0;
        r->temp_source = (fend == val || temp_source < 0 || temp_source > 3) ? 0 : (uint8_t)temp_source;

        snprintf(key, sizeof(key), "rule%u_temp_c", i);
        len = http_form_find_field(body, key, val, sizeof(val));
        fend = NULL;
        r->temp_threshold_c = len > 0 ? strtof(val, &fend) : 0.0f;
        if (len > 0 && !isfinite(r->temp_threshold_c)) {
            snprintf(err_msg, err_cap, "rule %u: temp_c not finite", i);
            return false;
        }
        /* A dormant threshold (no temperature condition) is never stored. */
        if (r->temp_source == 0 || r->temp_cmp == 0) r->temp_threshold_c = 0.0f;

        snprintf(key, sizeof(key), "rule%u_time_start_s", i);
        len = http_form_find_field(body, key, val, sizeof(val));
        fend = NULL;
        long time_start = len > 0 ? strtol(val, &fend, 10) : 0;
        r->time_start_s = (fend == val || time_start < 0 || time_start > 65535) ? 0 : (uint16_t)time_start;

        snprintf(key, sizeof(key), "rule%u_time_stop_s", i);
        len = http_form_find_field(body, key, val, sizeof(val));
        fend = NULL;
        long time_stop = len > 0 ? strtol(val, &fend, 10) : 0;
        r->time_stop_s = (fend == val || time_stop < 0 || time_stop > 65535) ? 0 : (uint16_t)time_stop;

        snprintf(key, sizeof(key), "rule%u_invert", i);
        len = http_form_find_field(body, key, val, sizeof(val));
        r->invert = (len > 0 && val[0] != '0') ? 1 : 0;

        p->on_off_rule_count = (uint8_t)(i + 1);
    }
    return true;
}

/* Appends a JSON string element for warnings[]; returns false (and leaves
 * *o unchanged) if it wouldn't fit, matching every other APPEND-macro
 * handler's "stop rather than overrun" convention. */
static bool append_warning(char *json, size_t cap, size_t *o, bool *first, const char *text)
{
    int n = snprintf(json + *o, cap - *o, "%s\"%s\"", *first ? "" : ",", text);
    if (n < 0 || (size_t)n >= cap - *o) {
        return false;
    }
    *o += (size_t)n;
    *first = false;
    return true;
}

/* docs/LIVE_PROFILE_EDIT.md section 7/8 -- the single validation body
 * shared by profile_post_handler() (mode ADVISORY, below), the live-edit
 * accept handler and the executor's pickup re-check (both mode HARD). Reads
 * zone config only; no httpd state, so it host-tests directly. `warnings_json`
 * (may be NULL/0-cap to skip) is filled with a JSON array of warning
 * strings, matching profile_post_handler()'s pre-extraction wire format
 * exactly (empty array "[]" when there is nothing to warn about). Returns
 * false on the first HARD violation, naming the segment/value/ceiling in
 * `err_msg`; in ADVISORY mode the same conditions become warnings instead
 * and the function keeps checking every segment/zone rather than stopping
 * at the first one. */
bool profiles_validate_candidate(const profile_t *candidate, profile_validate_mode_t mode, char *warnings_json,
                                  size_t warnings_json_cap, char *err_msg, size_t err_cap)
{
    size_t warn_o = 0;
    bool warn_first = true;
    bool have_warn_buf = warnings_json != NULL && warnings_json_cap > 0;
    if (have_warn_buf) {
        warnings_json[warn_o++] = '[';
    }

    /* Structural rule/segment checks come first, in EVERY mode: the live-edit
     * accept path (profiles_live_http.c -> live_profile_save_working) and the
     * executor's adopt step never reach profiles_http_save(), so without this a
     * rule targeting a disabled aux, an out-of-range target or a duplicate
     * (segment, target) pair was accepted there. The main save path runs the same
     * two functions again inside profiles_http_save() -- identical code, identical
     * messages, so the second pass can only repeat a verdict, never contradict it. */
    if (candidate->on_off_rule_count > PROFILE_MAX_ON_OFF_RULES) {
        snprintf(err_msg, err_cap, "on_off_rule_count out of range (0-%u)", (unsigned)PROFILE_MAX_ON_OFF_RULES);
        return false;
    }
    for (uint8_t i = 0; i < candidate->segment_count && i < PROFILE_MAX_SEGMENTS; i++) {
        if (candidate->segments[i].seg_kind == PROFILE_SEG_KIND_RELAY_IO &&
            !validate_io_segment(&candidate->segments[i], (uint8_t)(i + 1), err_msg, err_cap)) {
            return false;
        }
    }
    if (!validate_on_off_rules(candidate, err_msg, err_cap)) {
        return false;
    }

    /* Multi-zone (TODO.md 6A.5): check every participating zone's ceiling
     * against every ramped segment -- a profile is only feasible if ALL of
     * its zones can sustain the requested rate, since ramp-lock will hold
     * the shared setpoint back to whichever zone is slowest anyway. */
    for (uint8_t i = 0; i < candidate->segment_count; i++) {
        if (candidate->segments[i].seg_kind != PROFILE_SEG_KIND_ZONE_RAMP) {
            continue; /* a relay/IO segment has no ramp rate/target to check against a zone's ceiling */
        }
        float rate = candidate->segments[i].ramp_c_per_hr;
        float target = candidate->segments[i].target_c;
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if (!(candidate->zone_mask & (1u << zi))) {
                continue;
            }

            if (mode == PROFILE_VALIDATE_HARD) {
                /* Section 7's stricter live-edit rule: max_temp_c == 0 means
                 * "uncommissioned", never "no limit" -- refuse rather than
                 * let a missing ceiling read as an infinite one. */
                float max_temp_c = 0.0f, min_temp_c = 0.0f;
                bool have_limits = zones_config_get_temp_limits(zi, &max_temp_c, &min_temp_c);
                if (!have_limits || max_temp_c <= 0.0f) {
                    snprintf(err_msg, err_cap,
                            "segment %u: zone %u has no configured temperature ceiling (uncommissioned)", i + 1, zi);
                    return false;
                }
                if (target > max_temp_c) {
                    snprintf(err_msg, err_cap, "segment %u: target %.1f C exceeds zone %u's %.1f C ceiling", i + 1,
                            (double)target, zi, (double)max_temp_c);
                    return false;
                }
            }

            if (rate <= 0.0f) {
                continue; /* no ramp-rate constraint on this segment (TODO.md section 5) */
            }
            float ramp_ceiling = 0.0f;
            zones_config_get_max_ramp(zi, &ramp_ceiling); /* zone already validated < thermo_count */
            if (rate > ramp_ceiling) {
                if (mode == PROFILE_VALIDATE_HARD) {
                    snprintf(err_msg, err_cap, "segment %u: ramp rate %.1f C/hr exceeds zone %u's %.1f C/hr ceiling",
                            i + 1, (double)rate, zi, (double)ramp_ceiling);
                    return false;
                }
                if (have_warn_buf) {
                    char text[96];
                    snprintf(text, sizeof(text), "segment %u: ramp rate %.1f C/hr exceeds zone %u's %.1f C/hr ceiling",
                            i + 1, (double)rate, zi, (double)ramp_ceiling);
                    append_warning(warnings_json, warnings_json_cap, &warn_o, &warn_first, text);
                }
                /* ADVISORY mode historically refused this exact condition
                 * (a hard over-ramp-ceiling save) via profile_post_handler's
                 * own inline check before this extraction -- preserved
                 * as a refusal in ADVISORY too, since profile feasibility
                 * (unlike the temp ceiling) was never made portable/advisory
                 * by the 2026-09-02 owner correction below. */
                if (mode == PROFILE_VALIDATE_ADVISORY) {
                    snprintf(err_msg, err_cap, "segment %u: ramp rate %.1f C/hr exceeds zone %u's %.1f C/hr ceiling",
                            i + 1, (double)rate, zi, (double)ramp_ceiling);
                    return false;
                }
            } else if (rate > PROFILE_RAMP_WARN_FRACTION * ramp_ceiling && have_warn_buf) {
                char text[96];
                snprintf(text, sizeof(text),
                        "segment %u: ramp rate %.1f C/hr is within 20%% of zone %u's %.1f C/hr ceiling", i + 1,
                        (double)rate, zi, (double)ramp_ceiling);
                append_warning(warnings_json, warnings_json_cap, &warn_o, &warn_first, text);
            }
        }
    }

    /* OWNER CORRECTION (2026-09-02): at save time (ADVISORY), a target
     * exceeding the zone's CURRENT max_temp_c is a warning, not a refusal --
     * profiles are portable between kilns (profile_exceeds_zone_ceiling()'s
     * own comment); enforcement stays profile_executor_run.c's run-start
     * re-check. Section 7 makes HARD mode stricter on purpose: a live edit
     * is not portable, it reaches the elements within one tick, so this
     * exact condition is a hard target/ceiling refusal above instead, and is
     * skipped here to avoid double-reporting it as a warning too. */
    if (mode == PROFILE_VALIDATE_ADVISORY && have_warn_buf) {
        char ceiling_note[256];
        if (profile_exceeds_zone_ceiling(candidate, ceiling_note, sizeof(ceiling_note))) {
            append_warning(warnings_json, warnings_json_cap, &warn_o, &warn_first, ceiling_note);
        }
    }

    if (have_warn_buf) {
        if (warn_o + 1 < warnings_json_cap) {
            warnings_json[warn_o++] = ']';
        }
        warnings_json[warn_o < warnings_json_cap ? warn_o : warnings_json_cap - 1] = '\0';
    }
    return true;
}

/* live_edit_name_collides()'s name_at seam, backed directly by s_profiles --
 * same pattern profiles_live_http.c's live_http_name_at()/profiles_http.c's
 * profiles_http_name_at() already use (three small copies rather than one
 * shared symbol, matching how s_profiles itself is already reached from each
 * of these files independently via profiles_http_internal.h). */
static const char *profile_post_name_at(void *ctx, uint8_t id)
{
    (void)ctx;
    if (id >= PROFILES_MAX_COUNT || !profiles_slot_used(id)) {
        return NULL;
    }
    return s_profiles.profiles[id].name;
}

esp_err_t profile_post_handler(httpd_req_t *req)
{
    if (cfg_fs_http_refuse_if_unmounted(req)) {
        return ESP_OK;
    }
    if (req->content_len <= 0 || req->content_len > PROFILE_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }

    /* HEAP (PSRAM), not stack, and freed the moment profiles_parse_profile_fields()
     * is done with it, BEFORE warn_json below is even allocated -- this used
     * to be the biggest of three buffers (2049B) that all lived on the
     * stack simultaneously for the whole function (body + warn_json[1168] +
     * the final json[1424] = 4641B in one frame, coordinator review,
     * 2026-08-31 httpd_worker stack-overflow audit). `body` is never
     * referenced again after the profiles_parse_profile_fields() call a few lines
     * down (the id_val lookup and that one call are its only two uses), so
     * it does not genuinely need to overlap with warn_json/json at all --
     * sequencing it out drops this function's peak transient allocation
     * from 4641B to ~2592B (warn_json+json, which DO need to coexist since
     * the final response embeds warn_json's text via %s). */
    char *body = heap_caps_malloc(PROFILE_BODY_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body == NULL) {
        ESP_LOGE(PROFILES_TAG, "POST /api/profile: malloc(%u) failed for the request body buffer",
                 (unsigned)(PROFILE_BODY_MAX + 1));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"error\":\"out of memory reading the request body\"}");
    }
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            ESP_LOGW(PROFILES_TAG, "profile body read failed/short: %d", ret);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            free(body);
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    /* id: empty or "-1" creates in the first free slot; a valid existing id
     * overwrites that slot. Any other value in 0..7 also targets that exact
     * slot (create-or-overwrite), so a client that already knows its id can
     * address it directly rather than relying on "first free". */
    char id_val[8];
    int id_len = http_form_find_field(body, "id", id_val, sizeof(id_val));
    long requested_id = -1;
    if (id_len == -2 || (id_len > 0 && !http_form_parse_long(id_val, id_len, -1, PROFILES_MAX_COUNT - 1, &requested_id))) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id invalid");
        return ESP_OK;
    }

    /* check_httpd_task_stack_budget.py: profile_t (~428 B) used to be a
     * plain local (`tmp`) here, contributing to this handler's own
     * httpd_worker frame for the whole function (it stays live until the
     * commit near the end). Heap (PSRAM preferred), freed on every return
     * path below -- same convention as `body`/`warn_json`/`json` in this
     * same function. */
    profile_t *tmp = heap_caps_malloc(sizeof(*tmp), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (tmp == NULL) {
        tmp = malloc(sizeof(*tmp));
    }
    if (tmp == NULL) {
        free(body);
        ESP_LOGE(PROFILES_TAG, "POST /api/profile: malloc(%u) failed for the profile_t scratch",
                 (unsigned)sizeof(*tmp));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"error\":\"out of memory\"}");
    }
    memset(tmp, 0, sizeof(*tmp));
    char err_msg[128];
    bool parse_ok = profiles_parse_profile_fields(body, tmp, err_msg, sizeof(err_msg));
    /* Last use of `body` in this function either way -- free it here, before
     * warn_json is allocated below, rather than holding it until the
     * function returns. */
    free(body);
    body = NULL;
    if (!parse_ok) {
        char json[192];
        int n = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", err_msg);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        esp_err_t ret = httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
        free(tmp);
        return ret;
    }

    /* Feasibility + ceiling check, docs/LIVE_PROFILE_EDIT.md section 8
     * item 2 -- profiles_validate_candidate() is the one function the save
     * handler here, the live-edit handler and the executor's pickup check
     * all call. ADVISORY mode reproduces this handler's pre-extraction
     * behavior exactly: an over-ramp-ceiling segment still refuses (a hard
     * 400, unchanged), an over-temp-ceiling one only warns (the 2026-09-02
     * owner correction, profiles are portable between kilns).
     *
     * HEAP (PSRAM): `body` above is already freed by the time this is
     * allocated, so this and the final `json` below (which embeds this
     * buffer's text) are the only two transient buffers actually coexisting
     * in this function -- see this function's own opening comment for the
     * peak-size accounting. Freed on every return path below. */
    const size_t warn_json_cap = PROFILE_MAX_SEGMENTS * 96 + 16;
    char *warn_json = heap_caps_malloc(warn_json_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (warn_json == NULL) {
        ESP_LOGE(PROFILES_TAG, "POST /api/profile: malloc(%u) failed for the warnings buffer",
                 (unsigned)warn_json_cap);
        free(tmp);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"error\":\"out of memory building the response\"}");
    }
    char validate_err[224];
    if (!profiles_validate_candidate(tmp, PROFILE_VALIDATE_ADVISORY, warn_json, warn_json_cap, validate_err,
                                     sizeof(validate_err))) {
        char json[256];
        int n = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", validate_err);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        esp_err_t ret = httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
        free(warn_json);
        free(tmp);
        return ret;
    }

    if (profiles_http_convert_busy()) {
        char json[96];
        int bn = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"busy: zone conversion running, retry\"}");
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        esp_err_t ret = httpd_resp_send(req, json, bn < 0 ? 0 : (size_t)bn);
        free(warn_json);
        free(tmp);
        return ret;
    }
    /* Slot allocation, duplicate-name check, assign and save are one section
     * under the save mutex (two creates must not pick the same free slot). */
    profiles_save_lock();
    uint8_t target_id;
    if (requested_id >= 0 && requested_id < PROFILES_MAX_COUNT) {
        target_id = (uint8_t)requested_id;
    } else {
        int free_slot = -1;
        for (uint8_t i = 0; i < PROFILES_MAX_COUNT; i++) {
            if (!profiles_slot_used(i)) {
                free_slot = i;
                break;
            }
        }
        if (free_slot < 0) {
            profiles_save_unlock();
            free(warn_json);
            free(tmp);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "profile storage full");
            return ESP_OK;
        }
        target_id = (uint8_t)free_slot;
    }

    /* Owner request 2026-09-19: saving must never silently create/overwrite a
     * duplicate name. Same check/helper profiles_http_save() now runs --
     * see that function's comment. exclude_id lets overwriting a slot with
     * its own unchanged name stay legal. */
    {
        /* target_id alone is enough: profile_post_name_at() already returns
         * NULL for an unused slot, so an unused target_id is naturally
         * skipped by live_edit_name_collides()'s own `if (!existing)
         * continue;` -- the profiles_slot_used()-guarded 0xFF fallback here
         * was redundant with that. */
        char name_err[128];
        /* include_builtins=false (Opus review of 5dd23944, finding 1/BLOCKER):
         * this is a USER-SLOT save, and "Copy builtin" deliberately posts the
         * builtin's own code back as the new slot's name -- that must save,
         * not 400. See live_edit_name_collides_ex()'s doc comment. */
        if (live_edit_name_collides_ex(tmp->name, profile_post_name_at, NULL, target_id, false, name_err,
                                        sizeof(name_err))) {
            /* name_err can echo the operator-supplied name back verbatim
             * (see live_edit_name_collides()'s "%s" formats) -- escape
             * before embedding, else a name containing '"' breaks the JSON
             * and the page's r.json() throws instead of showing the real
             * refusal reason. */
            char name_err_escaped[sizeof(name_err) * 2 + 1];
            profiles_http_json_escape(name_err, name_err_escaped, sizeof(name_err_escaped));
            char json[192 + sizeof(name_err_escaped)];
            int n = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", name_err_escaped);
            profiles_save_unlock();
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            esp_err_t ret = httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
            free(warn_json);
            free(tmp);
            return ret;
        }
    }

    /* Re-validate under the lock: a zone/aux conversion may have committed since
     * the unlocked validation above. */
    if (!validate_on_off_rules(tmp, validate_err, sizeof(validate_err))) {
        profiles_save_unlock();
        char json[256];
        int vn = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", validate_err);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        esp_err_t ret = httpd_resp_send(req, json, vn < 0 ? 0 : (size_t)vn);
        free(warn_json);
        free(tmp);
        return ret;
    }
    s_profiles.profiles[target_id] = *tmp;
    free(tmp);
    profiles_slot_set(target_id);
    esp_err_t err = nvs_save_slot_locked(target_id);
    profiles_save_unlock();
    if (err != ESP_OK) {
        ESP_LOGE(PROFILES_TAG, "nvs_save_slot(%u) failed: %s -- profile applied live but will not survive a reboot",
                 target_id, esp_err_to_name(err));
        free(warn_json);
        return cfg_fs_http_persist_failed(req);
    }

    /* HEAP (PSRAM), same reasoning as warn_json above -- embeds warn_json's
     * text via %s, so the two DO need to coexist for this one snprintf
     * call; warn_json is freed immediately after, before this buffer is
     * sent, rather than both living until the function returns. */
    const size_t json_cap = 256 + warn_json_cap;
    char *json = heap_caps_malloc(json_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (json == NULL) {
        ESP_LOGE(PROFILES_TAG, "POST /api/profile: malloc(%u) failed for the response buffer", (unsigned)json_cap);
        free(warn_json);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"error\":\"out of memory building the response\"}");
    }
    int n = snprintf(json, json_cap, "{\"ok\":true,\"id\":%u,\"warnings\":%s}", target_id, warn_json);
    free(warn_json);
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
    free(json);
    return ret;
}

esp_err_t profile_delete_post_handler(httpd_req_t *req)
{
    if (cfg_fs_http_refuse_if_unmounted(req)) {
        return ESP_OK;
    }
    if (req->content_len <= 0 || req->content_len > 64) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }
    char body[65];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            ESP_LOGW(PROFILES_TAG, "profile delete body read failed/short: %d", ret);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    char id_val[8];
    int id_len = http_form_find_field(body, "id", id_val, sizeof(id_val));
    long id = -1;
    bool id_ok = id_len > 0 && http_form_parse_long(id_val, id_len, 0, 255, &id);
    if (id_ok && profiles_builtin_id_valid((uint8_t)id)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "built-in schedules are read-only and cannot be deleted -- "
                            "hide it instead (POST /api/profile/builtin/hide)");
        return ESP_OK;
    }
    if (!id_ok || id < 0 || id >= PROFILES_MAX_COUNT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id missing or out of range");
        return ESP_OK;
    }
    if (!profiles_slot_used(id)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such profile");
        return ESP_OK;
    }
    /* Opus review item 2 (PROFILE_SLOTS_100.md section 7): refuse to
     * delete a slot the executor is currently running or has paused. Same
     * check as profiles_http.c's benchproto profiles_http_delete(). */
    /* Only "is this id currently running/paused" is needed here -- use the
     * narrow accessor profile_executor.h recommends over a 1464-byte
     * profile_exec_status_t stack local on the httpd task. */
    uint8_t active_id = 0;
    if (profile_executor_get_active_id(&active_id) && active_id == id) {
        /* Set explicitly rather than via httpd_resp_send_err(): esp_http_server
         * has no HTTPD_409_CONFLICT enumerator (kiln_cfg_http.c's identical
         * comment/pattern). */
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "profile is currently running -- stop it before deleting");
    }

    /* Clear the favorite mark BEFORE erasing the slot (review fold-in,
     * PROFILE_SLOTS_100.md section 7): erase-then-clear left a window
     * where a power cut between the two steps could survive with the slot
     * erased but its favorite bit still set -- an import that later lands on
     * this same id inherits that orphaned favorite (profiles_favorites.h's
     * lifecycle keeps favorites across import, deliberately, unlike delete).
     * A failed save is logged inside the module and does not fail the
     * delete. */
    (void)profiles_favorites_set((uint8_t)id, false);
    /* Prune firing history before the slot is touched so a failure leaves the
     * slot in place and the delete retryable (see profiles_http_delete()). */
    if (firing_stats_erase((uint8_t)id) != ESP_OK) {
        return cfg_fs_http_persist_failed(req);
    }
    profiles_save_lock();
    esp_err_t err = nvs_erase_slot_locked((uint8_t)id);
    if (err == ESP_OK) {
        profiles_slot_clear(id);
        memset(&s_profiles.profiles[id], 0, sizeof(s_profiles.profiles[id]));
    }
    profiles_save_unlock();
    if (err != ESP_OK) {
        ESP_LOGE(PROFILES_TAG, "nvs_erase_slot(%ld) failed: %s -- slot kept, retry", id,
                 esp_err_to_name(err));
        return cfg_fs_http_persist_failed(req);
    }
    return httpd_resp_sendstr(req, "ok");
}

/* ---- Builtin hide / unhide / restore ---------------------------------------
 *
 * "Remove this shipped schedule" cannot be a delete -- the catalogue is a
 * const table in flash -- so it is a persisted hide, and unhiding is
 * therefore always possible. See profiles_builtin.h.
 *
 * POST /api/profile/builtin/hide     body: id=<128..>&hidden=0|1
 * POST /api/profile/builtin/restore  body: (none) -- unhides everything
 */

static bool read_small_body(httpd_req_t *req, char *buf, size_t cap)
{
    if ((size_t)req->content_len >= cap) {
        return false;
    }
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret <= 0) {
            return false;
        }
        received += (size_t)ret;
    }
    buf[received] = '\0';
    return true;
}

esp_err_t builtin_hide_post_handler(httpd_req_t *req)
{
    if (cfg_fs_http_refuse_if_unmounted(req)) {
        return ESP_OK;
    }
    char body[65];
    if (!read_small_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing, too large, or read failed");
        return ESP_OK;
    }

    char id_val[8];
    int id_len = http_form_find_field(body, "id", id_val, sizeof(id_val));
    long id = -1;
    bool id_ok = id_len > 0 && http_form_parse_long(id_val, id_len, 0, 255, &id);
    if (!id_ok || !profiles_builtin_id_valid((uint8_t)id)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such built-in schedule");
        return ESP_OK;
    }

    /* Missing "hidden" defaults to 1: the endpoint is named "hide", so the
     * request with no qualifier means hide. Unhiding takes an explicit
     * hidden=0. */
    char hid_val[8];
    int hid_len = http_form_find_field(body, "hidden", hid_val, sizeof(hid_val));
    bool hidden = (hid_len <= 0) || (hid_val[0] != '0');

    esp_err_t err = profiles_builtin_set_hidden((uint8_t)id, hidden);
    if (err != ESP_OK) {
        return cfg_fs_http_persist_failed(req);
    }
    char json[128];
    int n = snprintf(json, sizeof(json), "{\"ok\":%s,\"id\":%ld,\"hidden\":%s,\"persisted\":%s}",
                     "true", id, hidden ? "true" : "false", err == ESP_OK ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
}

esp_err_t builtin_restore_post_handler(httpd_req_t *req)
{
    if (cfg_fs_http_refuse_if_unmounted(req)) {
        return ESP_OK;
    }
    char body[65];
    if (req->content_len > 0 && !read_small_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large or read failed");
        return ESP_OK;
    }
    esp_err_t err = profiles_builtin_restore_all();
    if (err != ESP_OK) {
        return cfg_fs_http_persist_failed(req);
    }
    char json[96];
    int n = snprintf(json, sizeof(json), "{\"ok\":true,\"persisted\":%s}", err == ESP_OK ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
}


/* ---- Favorite / unfavorite --------------------------------------------------
 *
 * POST /api/profile/favorite  body: id=<0..>&favorite=0|1
 *
 * Accepts an id in EITHER namespace -- a saved slot or a shipped catalogue
 * entry -- because any profile can be favorited. Unlike the delete route
 * above, this one therefore does not refuse builtin ids: favoriting a shipped
 * schedule is a supported operation, not an attempt to modify read-only
 * flash. Nothing about the profile itself changes; only the mark does.
 */
esp_err_t profile_favorite_post_handler(httpd_req_t *req)
{
    /* cfg-only persistence (docs/CONFIG_FILESYSTEM.md): refuse before any state change. */
    if (cfg_fs_http_refuse_if_unmounted(req)) {
        return ESP_OK;
    }
    char body[65];
    if (!read_small_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing, too large, or read failed");
        return ESP_OK;
    }

    char id_val[8];
    int id_len = http_form_find_field(body, "id", id_val, sizeof(id_val));
    long id = -1;
    bool id_ok = id_len > 0 && http_form_parse_long(id_val, id_len, 0, 255, &id);
    if (!id_ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id missing or out of range");
        return ESP_OK;
    }
    bool known = (id < PROFILES_MAX_COUNT) || profiles_builtin_id_valid((uint8_t)id);
    if (!known) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such profile");
        return ESP_OK;
    }

    /* Missing "favorite" defaults to 1: the endpoint is named "favorite", so
     * the request with no qualifier means favorite. Unfavoriting takes an
     * explicit favorite=0 -- the same convention builtin_hide_post_handler
     * uses for "hidden". */
    char fav_val[8];
    int fav_len = http_form_find_field(body, "favorite", fav_val, sizeof(fav_val));
    if (fav_len == -2 || (fav_len > 0 && !(fav_len == 1 && (fav_val[0] == '0' || fav_val[0] == '1')))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "favorite must be 0 or 1");
        return ESP_OK;
    }
    if (id < PROFILES_MAX_COUNT && !profiles_slot_used((uint8_t)id)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such profile");
        return ESP_OK;
    }
    bool favorite = (fav_len <= 0) || (fav_val[0] != '0');

    esp_err_t err = profiles_favorites_set((uint8_t)id, favorite);
    if (err != ESP_OK) {
        return cfg_fs_http_persist_failed(req);
    }
    char json[128];
    int n = snprintf(json, sizeof(json), "{\"ok\":true,\"id\":%ld,\"favorite\":%s,\"persisted\":%s}", id,
                     favorite ? "true" : "false", err == ESP_OK ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
}
