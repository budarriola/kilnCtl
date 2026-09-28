// See dashboard_json.h for why these two functions live in their own
// dependency-light file. NOT a verbatim move out of dashboard_http.c: three
// fields (ff_hold_used_matrix, ff_hold_infeasible, ff_membership_change_count)
// were added to both format strings and their argument lists in the same
// commit that relocated these functions, and append_zone_status_json()'s
// truncation path has since been changed to log and still close the array
// (see its doc comment in dashboard_json.h) instead of bailing silently.
#include "dashboard_json.h"

#include <stdarg.h>
#include <stdio.h>

#include "esp_log.h"

#include <math.h>

static const char *TAG = "dashboard_json";

/* profile_executor_feedforward.c captures bd_ff_hold/bd_ff_climb/
 * bd_coupling_correction (and the other bd_* breakdown fields) BEFORE the
 * control path's own isfinite(u_ff) bail, so inf/NaN can reach here. %.4f on
 * a non-finite double emits bare `inf`/`nan`, which is not valid JSON and
 * breaks the whole /api/control response for the caller. Sanitize at this
 * emit site rather than the control path -- these fields are diagnostics,
 * not control inputs. */
static inline double bd_finite_or_zero(double v)
{
    return isfinite(v) ? v : 0.0;
}

size_t json_append_clamped(char *json, size_t cap, size_t o, const char *fmt, ...)
{
    if (o > cap - 1) {
        /* Already at/over the limit (shouldn't happen if every prior call
         * went through this same function, but a caller mixing this with a
         * raw snprintf could hand in an already-overrun `o` -- clamp before
         * doing any arithmetic on it so `cap - o` below can never wrap). */
        return cap - 1;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(json + o, cap - o, fmt, ap);
    va_end(ap);
    if (n > 0) {
        o += (size_t)n;
    }
    return o > cap - 1 ? cap - 1 : o;
}

void json_escape(const char *src, char *out, size_t out_cap)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 2 < out_cap; p++) {
        if (*p == '"' || *p == '\\') {
            if (o + 3 >= out_cap) {
                break;
            }
            out[o++] = '\\';
        }
        out[o++] = *p;
    }
    out[o] = '\0';
}

/* Shared by both handlers below: one JSON object per active zone. Appends
 * to *o, using the APPEND-into-json-buffer pattern every other handler in
 * dashboard_http.c already uses (the caller owns the buffer/APPEND macro
 * since C has no closures to hand this a local one). control_fields selects
 * between /api/profile_exec's exec-lifecycle shape and /api/control's
 * tuning-focused shape (TODO.md 6A.9 asks for both, as separate endpoints
 * with different focuses, not one bloated one). */
size_t append_zone_status_json(char *json, size_t cap, size_t o, const profile_exec_status_t *st,
                               bool control_fields)
{
    int n;
    bool first = true;
    n = snprintf(json + o, cap - o, "\"zones\":[");
    if (n < 0 || (size_t)n >= cap - o) goto truncated;
    o += (size_t)n;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        const profile_exec_zone_status_t *z = &st->zones[zi];
        if (!z->active) continue;
        char reason_escaped[sizeof(z->fault_reason) * 2 + 1];
        reason_escaped[0] = '\0';
        if (z->faulted) {
            json_escape(z->fault_reason, reason_escaped, sizeof(reason_escaped));
        }
        if (control_fields) {
            /* ROADMAP.md M15 B4: the rest of the duty pipeline, alongside
             * the pre-existing pid_p/pid_i/pid_d/pid_ff (TODO.md 6A.9). See
             * zone_duty_breakdown_t's doc comment (profile_executor.h) for
             * what each field means. bd_pre_clamp_total is DERIVED here
             * (pid_p+pid_i+pid_d+pid_ff, pid.c's own `unclamped` local after
             * the integral floor and before the final [0,1] clamp) rather
             * than a separately-stored field -- see that struct's doc
             * comment for why. */
            const zone_duty_breakdown_t *bd = &z->duty_breakdown;
            double pre_clamp_total = (double)z->pid_p + (double)z->pid_i + (double)z->pid_d + (double)z->pid_ff;
            n = snprintf(json + o, cap - o,
                        "%s{\"zone\":%u,\"control_mode\":%u,\"actual_c\":%.2f,\"actual_valid\":%s,"
                        "\"duty\":%.3f,\"relay_on\":%s,\"pid_p\":%.4f,\"pid_i\":%.4f,\"pid_d\":%.4f,"
                        "\"pid_ff\":%.4f,\"cooling_limited\":%s,\"faulted\":%s,\"fault_guard\":%u,"
                        "\"heat_blocked\":%s,\"heat_blocked_sources\":%lu,"
                        "\"relay_starved_s\":%.1f,\"relay_denied_reason\":%u,\"ff_hold_used_matrix\":%s,"
                        "\"ff_hold_infeasible\":%s,\"ff_membership_change_count\":%lu,"
                        "\"bd_ff_hold\":%.4f,\"bd_ff_climb\":%.4f,\"bd_coupling_correction\":%.4f,"
                        "\"bd_ff_rate_pretaper\":%.5f,\"bd_ff_rate_posttaper\":%.5f,"
                        "\"bd_kp_effective\":%.5f,\"bd_ki_effective\":%.5f,\"bd_kd_effective\":%.5f,"
                        "\"bd_pre_clamp_total\":%.4f,\"bd_post_clamp_total\":%.4f,"
                        "\"bd_load_cap_boost\":%.4f,\"bd_final_commanded\":%.4f}",
                        first ? "" : ",", zi, z->control_mode, (double)(z->actual_valid ? z->actual_c : 0.0f),
                        z->actual_valid ? "true" : "false", (double)z->duty,
                        z->relay_commanded_on ? "true" : "false", (double)z->pid_p, (double)z->pid_i,
                        (double)z->pid_d, (double)z->pid_ff, z->cooling_limited ? "true" : "false",
                        z->faulted ? "true" : "false", z->fault_guard,
                        z->heat_blocked ? "true" : "false", (unsigned long)z->heat_blocked_sources,
                        (double)z->relay_starved_s, (unsigned)z->relay_denied_reason,
                        z->ff_hold_used_matrix ? "true" : "false", z->ff_hold_infeasible ? "true" : "false",
                        (unsigned long)z->ff_membership_change_count,
                        bd_finite_or_zero((double)bd->ff_hold), bd_finite_or_zero((double)bd->ff_climb),
                        bd_finite_or_zero((double)bd->coupling_correction),
                        bd_finite_or_zero((double)bd->ff_rate_pretaper_c_per_s),
                        bd_finite_or_zero((double)bd->ff_rate_posttaper_c_per_s),
                        bd_finite_or_zero((double)bd->kp_effective), bd_finite_or_zero((double)bd->ki_effective),
                        bd_finite_or_zero((double)bd->kd_effective),
                        bd_finite_or_zero(pre_clamp_total), bd_finite_or_zero((double)bd->post_clamp_total),
                        bd_finite_or_zero((double)bd->load_cap_boost),
                        bd_finite_or_zero((double)bd->final_commanded));
        } else {
            /* firing_stats (PID_EXPANSION_PLAN.md Phase 7a dashboard wiring):
             * profile_exec_zone_status_t::firing_stats, live and still
             * accumulating while RUNNING/PAUSED, frozen at DONE/FAULTED --
             * see that field's own doc comment (profile_executor.h) for the
             * exclusion/sign/units rules. Nested rather than flattened so the
             * shape is self-describing and doesn't collide with any existing
             * top-level zone key. Only on the /api/profile_exec shape
             * (control_fields==false) per the sizing note in
             * dashboard_json.h -- /api/control stays tuning-focused. */
            const profile_exec_firing_stats_t *fs = &z->firing_stats;
            n = snprintf(json + o, cap - o,
                        "%s{\"zone\":%u,\"actual_c\":%.2f,\"actual_valid\":%s,\"relay_on\":%s,"
                        "\"duty\":%.3f,\"control_mode\":%u,\"faulted\":%s,\"fault_reason\":\"%s\","
                        "\"fault_guard\":%u,\"heat_blocked\":%s,\"heat_blocked_sources\":%lu,"
                        "\"relay_starved_s\":%.1f,\"relay_denied_reason\":%u,"
                        "\"ff_hold_used_matrix\":%s,\"ff_hold_infeasible\":%s,"
                        "\"ff_membership_change_count\":%lu,"
                        "\"ramp_lag_sustained\":%s,\"ramp_lag_held_s\":%.2f,"
                        "\"ramp_lag_commanded_rate_c_per_hr\":%.2f,\"ramp_lag_achieved_rate_c_per_hr\":%.2f,"
                        "\"ramp_dwell_credit_s\":%.2f,"
                        "\"firing_stats\":{\"mean_error_c\":%.2f,\"max_overshoot_c\":%.2f,"
                        "\"max_overshoot_elapsed_s\":%lu,\"max_overshoot_segment\":%u,"
                        "\"max_undershoot_c\":%.2f,\"max_undershoot_elapsed_s\":%lu,"
                        "\"max_undershoot_segment\":%u,\"iae_raw_c_s\":%.2f,\"iae_normalized\":%.4f,"
                        "\"ramp_err_mean_c\":%.2f,\"ramp_err_max_c\":%.2f,\"dwell_err_mean_c\":%.2f,"
                        "\"dwell_err_max_c\":%.2f,\"sample_count\":%lu,\"excluded_sample_count\":%lu,"
                        "\"duration_s\":%lu,\"start_temp_c\":%.2f}}",
                        first ? "" : ",", zi, (double)(z->actual_valid ? z->actual_c : 0.0f),
                        z->actual_valid ? "true" : "false", z->relay_commanded_on ? "true" : "false",
                        (double)z->duty, z->control_mode, z->faulted ? "true" : "false", reason_escaped,
                        z->fault_guard, z->heat_blocked ? "true" : "false",
                        (unsigned long)z->heat_blocked_sources,
                        (double)z->relay_starved_s, (unsigned)z->relay_denied_reason,
                        z->ff_hold_used_matrix ? "true" : "false", z->ff_hold_infeasible ? "true" : "false",
                        (unsigned long)z->ff_membership_change_count,
                        z->ramp_lag_sustained ? "true" : "false", (double)z->ramp_lag_held_s,
                        (double)z->ramp_lag_commanded_rate_c_per_hr, (double)z->ramp_lag_achieved_rate_c_per_hr,
                        (double)z->ramp_dwell_credit_s,
                        (double)fs->mean_error_c, (double)fs->max_overshoot_c,
                        (unsigned long)fs->max_overshoot_elapsed_s, fs->max_overshoot_segment,
                        (double)fs->max_undershoot_c, (unsigned long)fs->max_undershoot_elapsed_s,
                        fs->max_undershoot_segment, (double)fs->iae_raw_c_s, (double)fs->iae_normalized,
                        (double)fs->ramp_err_mean_c, (double)fs->ramp_err_max_c,
                        (double)fs->dwell_err_mean_c, (double)fs->dwell_err_max_c,
                        (unsigned long)fs->sample_count, (unsigned long)fs->excluded_sample_count,
                        (unsigned long)fs->duration_s, (double)z->start_temp_c);
        }
        if (n < 0 || (size_t)n >= cap - o) goto truncated;
        o += (size_t)n;
        first = false;
    }
    n = snprintf(json + o, cap - o, "]");
    if (n > 0 && (size_t)n < cap - o) o += (size_t)n;
    return o;

truncated:
    /* A truncation here used to bail silently (return o unchanged, array
     * left open) -- the caller (dashboard_http.c's two handlers) still
     * appended its own trailing '}' on top of that, producing an
     * unterminated "zones":[{...},{...} -- invalid JSON served with HTTP
     * 200 and no error anywhere on the wire or in the log. Log it by name
     * (which shape, how big the buffer was) so a future field addition that
     * outgrows the budget is loud instead of silent, and still close the
     * array so the document stays at least PARSEABLE (fewer zones than
     * expected beats a syntax error). */
    ESP_LOGE(TAG, "%s JSON truncated at %u/%u bytes -- raise DASHBOARD_JSON_%s_BUF_SIZE "
             "(dashboard_json.h)", control_fields ? "/api/control" : "/api/profile_exec",
             (unsigned)o, (unsigned)cap, control_fields ? "CONTROL" : "PROFILE_EXEC");
    n = snprintf(json + o, cap - o, "]");
    if (n > 0 && (size_t)n < cap - o) o += (size_t)n;
    return o;
}

/* ---- GET /api/autotune body ----------------------------------------------
 * Moved out of dashboard_http.c's autotune_status_get_handler() verbatim
 * (2026-08-31 dashboard-split pass) -- these three name-lookup helpers and
 * the format function below have no dependency beyond autotune_engine.h's
 * types, json_escape() (above), and libc, so they host-test the same way
 * append_zone_status_json() does. See dashboard_json.h's doc comment on
 * dashboard_format_autotune_status_json(). */
static const char *autotune_state_name(autotune_engine_state_t s)
{
    switch (s) {
    case AUTOTUNE_ENGINE_IDLE: return "idle";
    case AUTOTUNE_ENGINE_SETTLING: return "settling";
    case AUTOTUNE_ENGINE_STEPPING: return "stepping";
    case AUTOTUNE_ENGINE_RELAY_APPROACH: return "relay_approach";
    case AUTOTUNE_ENGINE_RELAY_CYCLING: return "relay_cycling";
    case AUTOTUNE_ENGINE_DONE: return "done";
    case AUTOTUNE_ENGINE_ABORTED: return "aborted";
    default: return "unknown";
    }
}

/* The rule is reported by name rather than by enum value because it is the
 * one part of a proposal an operator has to be able to judge for themselves:
 * "ziegler-nichols" is a warning label (pid_autotune.h: it is *designed* to
 * leave the loop oscillating), and a bare integer would not be. */
static const char *autotune_rule_name(autotune_rule_t r)
{
    switch (r) {
    case AUTOTUNE_RULE_SIMC: return "simc";
    case AUTOTUNE_RULE_ZIEGLER_NICHOLS: return "ziegler-nichols";
    case AUTOTUNE_RULE_TYREUS_LUYBEN: return "tyreus-luyben";
    case AUTOTUNE_RULE_COHEN_COON: return "cohen-coon";
    default: return "unknown";
    }
}

/* Same reasoning as autotune_rule_name() above: PID_EXPANSION_PLAN.md Phase 1
 * added this enum specifically so a refusal is distinguishable from every
 * other refusal (and from success) rather than collapsing to the same silent
 * kp=ki=kd=0 -- so the page gets the code by name, not just the number. */
static const char *autotune_refusal_name(autotune_refusal_t r)
{
    switch (r) {
    case AUTOTUNE_REFUSAL_OK: return "ok";
    case AUTOTUNE_REFUSAL_INVALID_MODEL: return "invalid_model";
    case AUTOTUNE_REFUSAL_RULE_NOT_ON_THIS_PATH: return "rule_not_on_this_path";
    case AUTOTUNE_REFUSAL_DEAD_TIME_TOO_SMALL: return "dead_time_too_small";
    case AUTOTUNE_REFUSAL_NONPOSITIVE_TAU: return "nonpositive_tau";
    case AUTOTUNE_REFUSAL_NONPOSITIVE_GAIN: return "nonpositive_gain";
    default: return "unknown";
    }
}

/* "probe" vs "identify" -- target mode's two STEPPING sub-phases, previously
 * indistinguishable on the wire (both report state=="stepping"; see
 * autotune_state_name() above, which this deliberately does NOT touch --
 * tools/PcTools/src/kilnctrl/devices_autotune.py, main_page.html's
 * AUTOTUNE_PHASE_LABEL/isAutotuneActive(), and tools/PcTools/tests' captured
 * .jsonl fixtures all match the "state" string literally, so changing what
 * it reports for STEPPING would be a wire break, not a refactor). This is a
 * SEPARATE field instead: "" whenever there is no meaningful sub-phase
 * (IDLE/SETTLING/DONE/ABORTED, or a relay-method or plain-duty run, none of
 * which have a probe/identify split at all -- st->target_mode is false for
 * all of those), "probe" while target_mode && probe_phase, "identify" while
 * target_mode && STEPPING && !probe_phase. */
static const char *autotune_sub_phase_name(const autotune_engine_status_t *st)
{
    if (!st->target_mode || st->state != AUTOTUNE_ENGINE_STEPPING) {
        return "";
    }
    return st->probe_phase ? "probe" : "identify";
}

int dashboard_format_autotune_status_json(char *json, size_t cap, const autotune_engine_status_t *st)
{
    char reason_escaped[sizeof(st->abort_reason) * 2 + 1];
    json_escape(st->abort_reason, reason_escaped, sizeof(reason_escaped));

    /* The relay fit's own rejection reason is reported verbatim alongside the
     * abort reason, for the same reason the RGA's is (see that handler): "not
     * a limit cycle" and "amplitude inside the hysteresis band" are different
     * findings about the kiln and the page must not flatten them. */
    char relay_reason_escaped[sizeof(st->relay.invalid_reason) * 2 + 1];
    json_escape(st->relay.invalid_reason, relay_reason_escaped, sizeof(relay_reason_escaped));

    /* proposed_gains is a zero-initialized struct whenever no tune has run
     * yet (state IDLE), so refusal/refusal_reason read AUTOTUNE_REFUSAL_OK /
     * "" in that case too -- exactly "empty string, not stale", since there
     * is no previous run's reason left lying around to leak. */
    char refusal_reason_escaped[sizeof(st->proposed_gains.refusal_reason) * 2 + 1];
    json_escape(st->proposed_gains.refusal_reason, refusal_reason_escaped, sizeof(refusal_reason_escaped));

    /* Diagnostic fit inputs/intermediates (2026-08-31): expose exactly what
     * pid_autotune_fit_fopdt() was actually called with and what it actually
     * computed, so a captured run's K can be checked against the fit's own
     * inputs instead of re-derived by hand from the raw trace. Only
     * meaningful when model_valid; 0 otherwise, same convention as
     * k_gain_c_per_duty etc. above. step_ambient_c is reported alongside
     * baseline_c deliberately -- see autotune_engine_status_t::step_ambient_c
     * -- they are different quantities that have been conflated before. */
    int n = snprintf(json, cap,
        "{\"state\":\"%s\",\"sub_phase\":\"%s\",\"method\":\"%s\",\"zone\":%u,\"elapsed_s\":%lu,\"sample_count\":%u,"
        "\"actual_c\":%.2f,\"actual_valid\":%s,\"duty\":%.3f,\"abort_reason\":\"%s\","
        "\"model_valid\":%s,\"model_settled\":%s,\"model_extrapolation_converged\":%s,"
        "\"model_tau_consistent\":%s,\"k_gain_c_per_duty\":%.3f,\"tau_s\":%.1f,\"dead_time_s\":%.1f,"
        "\"baseline_c\":%.2f,\"final_c\":%.2f,\"raw_rise_c\":%.2f,\"rise_inf_c\":%.2f,"
        "\"step_ambient_c\":%.2f,"
        "\"proposed_kp\":%.5f,\"proposed_ki\":%.5f,\"proposed_kd\":%.5f,\"rule\":\"%s\","
        "\"refusal\":\"%s\",\"refusal_reason\":\"%s\","
        "\"predicted_max_ramp_c_per_hr\":%.1f,"
        "\"relay_setpoint_c\":%.1f,\"relay_d\":%.3f,\"relay_h_c\":%.2f,"
        "\"relay_cycles_seen\":%u,\"relay_cycles_target\":%u,"
        "\"relay_valid\":%s,\"relay_ku\":%.5f,\"relay_tu_s\":%.1f,\"relay_amplitude_c\":%.2f,"
        "\"relay_cycles_used\":%d,\"relay_reason\":\"%s\","
        "\"external_write_reserved\":%s,\"external_write_reserved_zone\":%u}",
        autotune_state_name(st->state), autotune_sub_phase_name(st),
        st->method == AUTOTUNE_METHOD_RELAY ? "relay" : "step", st->zone_index,
        (unsigned long)st->elapsed_s, st->sample_count,
        (double)(st->actual_valid ? st->actual_c : 0.0f), st->actual_valid ? "true" : "false", (double)st->duty,
        reason_escaped, st->model.valid ? "true" : "false", st->model.settled ? "true" : "false",
        st->model.extrapolation_converged ? "true" : "false", st->model.tau_consistent_with_gain ? "true" : "false",
        (double)st->model.k_gain_c_per_duty,
        (double)st->model.tau_s, (double)st->model.dead_time_s,
        (double)st->model.baseline_c, (double)st->model.final_c, (double)st->model.raw_rise_c,
        (double)st->model.rise_inf_c, (double)st->step_ambient_c,
        (double)st->proposed_gains.kp,
        (double)st->proposed_gains.ki, (double)st->proposed_gains.kd,
        autotune_rule_name(st->proposed_gains.rule),
        autotune_refusal_name(st->proposed_gains.refusal), refusal_reason_escaped,
        (double)st->predicted_max_ramp_c_per_hr,
        (double)st->relay_setpoint_c, (double)st->relay_amplitude_duty, (double)st->relay_hysteresis_c,
        st->relay_cycles_seen, st->relay_cycles_target,
        st->relay.valid ? "true" : "false", (double)st->relay.ku, (double)st->relay.tu_s,
        (double)st->relay.amplitude_c, st->relay.cycles_used, relay_reason_escaped,
        st->external_write_reserved ? "true" : "false", (unsigned)st->external_write_reserved_zone);
    return n;
}

/* ---- GET /api/firing_history body ----------------------------------------
 * PID_EXPANSION_PLAN.md Phase 7a-2/7a-3's historical (last-5-per-profile)
 * tracking-quality records, profile_executor_get_firing_history()
 * (profile_executor.h). Output is unbounded in TWO nested dimensions -- up
 * to PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH records, each with up to
 * MAX31856_CHANNEL_COUNT zones -- so a future depth/channel-count increase
 * must degrade to a truncated-but-still-valid document instead of an
 * overflow, same discipline append_zone_status_json() already established
 * for the per-zone array. Unlike that function's "close whatever's open"
 * approach, this one commits a RECORD only as a whole (header + every active
 * zone + its own closing brackets) -- a record that doesn't fully fit is
 * dropped in its entirety rather than left half-written, so the offset
 * tracked here (`commit`) is always the end of the last COMPLETE record,
 * and the trailing "]}" appended at the end is guaranteed room via the
 * `reserve` bytes withheld from every snprintf cap during the loop (see
 * inline comment below). */
size_t dashboard_format_firing_history_json(char *json, size_t cap, uint8_t profile_id,
                                            const profile_firing_run_record_t *records,
                                            size_t record_count)
{
    if (cap == 0) {
        return 0;
    }
    /* Withheld from every snprintf cap while building record content, so
     * cap-o inside the loop never reports room this function has actually
     * reserved for the trailing "]}\0" -- guaranteeing that final close
     * (written against the REAL cap-o, not the reduced one) always has the
     * 3 bytes ("]}\0") it needs, however far content-building got. */
    const size_t reserve = (cap >= 3) ? 2 : 0;
    const size_t work_cap = cap - reserve;

    int n = snprintf(json, work_cap, "{\"profile_id\":%u,\"records\":[", profile_id);
    if (n < 0 || (size_t)n >= work_cap) {
        /* Header itself didn't fit (pathologically tiny cap, well below any
         * real handler's json_cap) -- nothing sensible to build. Degrade to
         * the smallest valid document that fits: "{}" if there's room for
         * it, else just NUL the buffer (an empty string, which the caller's
         * httpd_resp_send(..., 0) sends as a zero-length body -- not this
         * function's problem to invent a longer minimum). */
        if (cap >= 3) {
            json[0] = '{'; json[1] = '}'; json[2] = '\0';
            return 2;
        }
        json[0] = '\0';
        return 0;
    }
    size_t o = (size_t)n;
    size_t commit = o;
    bool wrote_any_record = false;

    for (size_t ri = 0; ri < record_count; ri++) {
        const profile_firing_run_record_t *rec = &records[ri];
        char name_escaped[sizeof(rec->profile_name) * 2 + 1];
        json_escape(rec->profile_name, name_escaped, sizeof(name_escaped));

        n = snprintf(json + o, work_cap - o,
            "%s{\"profile_name\":\"%s\",\"run_started_unix_s\":%lu,\"duration_s\":%lu,"
            "\"zone_mask\":%u,\"zones\":[",
            wrote_any_record ? "," : "", name_escaped, (unsigned long)rec->run_started_unix_s,
            (unsigned long)rec->duration_s, rec->zone_mask);
        if (n < 0 || (size_t)n >= work_cap - o) {
            break; /* this record's header alone doesn't fit -- stop, don't touch o */
        }
        size_t o_try = o + (size_t)n;
        bool zone_first = true;
        bool record_ok = true;
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            const profile_firing_zone_record_t *zr = &rec->zones[zi];
            if (!zr->active) continue;
            const profile_exec_firing_stats_t *fs = &zr->stats;
            n = snprintf(json + o_try, work_cap - o_try,
                "%s{\"zone\":%u,\"kp\":%.5f,\"ki\":%.5f,\"kd\":%.5f,"
                "\"firing_stats\":{\"mean_error_c\":%.2f,\"max_overshoot_c\":%.2f,"
                "\"max_overshoot_elapsed_s\":%lu,\"max_overshoot_segment\":%u,"
                "\"max_undershoot_c\":%.2f,\"max_undershoot_elapsed_s\":%lu,"
                "\"max_undershoot_segment\":%u,\"iae_raw_c_s\":%.2f,\"iae_normalized\":%.4f,"
                "\"ramp_err_mean_c\":%.2f,\"ramp_err_max_c\":%.2f,\"dwell_err_mean_c\":%.2f,"
                "\"dwell_err_max_c\":%.2f,\"sample_count\":%lu,\"excluded_sample_count\":%lu,"
                "\"duration_s\":%lu}}",
                zone_first ? "" : ",", zi, (double)zr->kp, (double)zr->ki, (double)zr->kd,
                (double)fs->mean_error_c, (double)fs->max_overshoot_c,
                (unsigned long)fs->max_overshoot_elapsed_s, fs->max_overshoot_segment,
                (double)fs->max_undershoot_c, (unsigned long)fs->max_undershoot_elapsed_s,
                fs->max_undershoot_segment, (double)fs->iae_raw_c_s, (double)fs->iae_normalized,
                (double)fs->ramp_err_mean_c, (double)fs->ramp_err_max_c,
                (double)fs->dwell_err_mean_c, (double)fs->dwell_err_max_c,
                (unsigned long)fs->sample_count, (unsigned long)fs->excluded_sample_count,
                (unsigned long)fs->duration_s);
            if (n < 0 || (size_t)n >= work_cap - o_try) {
                record_ok = false;
                break;
            }
            o_try += (size_t)n;
            zone_first = false;
        }
        if (record_ok) {
            n = snprintf(json + o_try, work_cap - o_try, "]}"); /* close zones array + this record */
            if (n < 0 || (size_t)n >= work_cap - o_try) {
                record_ok = false;
            } else {
                o_try += (size_t)n;
            }
        }
        if (!record_ok) {
            /* This record didn't fit as a whole (header fit, but some zone
             * or the closing brackets didn't) -- discard the partial write
             * by NOT advancing o/commit past the last known-good state, and
             * stop; a later, shorter record wouldn't be newest-first any
             * more if skipped ahead to, so this stops rather than tries the
             * next one. */
            break;
        }
        o = o_try;
        commit = o;
        wrote_any_record = true;
    }

    o = commit;
    n = snprintf(json + o, cap - o, "]}");
    if (n > 0 && (size_t)n < cap - o) {
        o += (size_t)n;
    }
    return o;
}
