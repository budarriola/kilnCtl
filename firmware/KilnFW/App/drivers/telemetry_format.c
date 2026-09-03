#include "telemetry_format.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Replaces any byte that would break this format's whitespace-delimited
 * key=value parsing (space/tab, '=', '"') with '_', in place, so a free-text
 * field (only abort_reason today) can be embedded without quoting rules a
 * simple split(" ") parser would otherwise need to understand. Truncates at
 * cap-1 and always NUL-terminates, same convention as json_escape()
 * (dashboard_json.c) for the equivalent problem in JSON. */
static void telemetry_sanitize_token(const char *in, char *out, size_t cap)
{
    if (cap == 0) {
        return;
    }
    size_t i = 0;
    for (; in != NULL && in[i] != '\0' && i < cap - 1; i++) {
        char c = in[i];
        if (c == ' ' || c == '\t' || c == '=' || c == '"' || c == '\n' || c == '\r') {
            c = '_';
        }
        out[i] = c;
    }
    out[i] = '\0';
}

/* Worst case at MAX31856_CHANNEL_COUNT==5 all-active zones: header (~70B at
 * 4-digit elapsed_s/target_c) + 5 zones (5 * ~62B = 310B) = ~380B, bigger
 * than UART_LOG_TEXT_MAX (uart_log_bridge.c, currently 252B). In practice a
 * run targets at most the zones in profile_t.zone_mask, and this codebase's
 * boards run 1-3, so this is flagged here rather than solved: a rare 5-
 * zone-active firing's telemetry line truncates SAFELY (snprintf into a
 * fixed buffer just stops appending, never overflows) rather than losing
 * zones outright or corrupting memory. A future change that needs every
 * zone unconditionally at max channel count should split this into one
 * line per zone instead of trying to widen the single-line budget, which a
 * single queue entry cannot exceed regardless of scratch buffer size. */
int telemetry_format_firing(const profile_exec_status_t *st, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    if (st == NULL) {
        out[0] = '\0';
        return 0;
    }

    size_t o;
    int n = snprintf(out, cap, "KTEL%u FIRE t=%lu st=%u pid=%u seg=%u dwell=%u tgt=%.2f",
                      TELEMETRY_LOG_SCHEMA_VERSION, (unsigned long)st->total_elapsed_s, st->state,
                      st->profile_id, st->segment_index, st->dwelling ? 1u : 0u, (double)st->target_c);
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    o = ((size_t)n < cap) ? (size_t)n : cap - 1;
    int total = n;

    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        const profile_exec_zone_status_t *z = &st->zones[zi];
        if (!z->active) {
            continue;
        }
        float actual = z->actual_valid ? z->actual_c : 0.0f;
        float err = z->actual_valid ? (actual - st->target_c) : 0.0f;
        n = snprintf(cap > o ? out + o : NULL, cap > o ? cap - o : 0,
                     " z%u_c=%.2f z%u_v=%u z%u_e=%.2f z%u_d=%.3f z%u_fm=%u z%u_fi=%u",
                     zi, (double)actual, zi, z->actual_valid ? 1u : 0u, zi, (double)err, zi,
                     (double)z->duty, zi, z->ff_hold_used_matrix ? 1u : 0u, zi,
                     z->ff_hold_infeasible ? 1u : 0u);
        if (n < 0) {
            break;
        }
        total += n;
        o = (cap > o && (size_t)n < cap - o) ? o + (size_t)n : cap - (cap > 0 ? 1 : 0);
    }

    return total;
}

int telemetry_format_autotune(const autotune_engine_status_t *st, char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    if (st == NULL) {
        out[0] = '\0';
        return 0;
    }

    size_t o;
    int n = snprintf(out, cap, "KTEL%u TUNE t=%lu st=%u meth=%u zone=%u n=%u c=%.2f v=%u duty=%.3f",
                      TELEMETRY_LOG_SCHEMA_VERSION, (unsigned long)st->elapsed_s, st->state, st->method,
                      st->zone_index, (unsigned)st->sample_count, (double)st->actual_c,
                      st->actual_valid ? 1u : 0u, (double)st->duty);
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    o = ((size_t)n < cap) ? (size_t)n : cap - 1;
    int total = n;

    if (st->state == AUTOTUNE_ENGINE_DONE) {
        if (st->method == AUTOTUNE_METHOD_STEP && st->model.valid) {
            n = snprintf(cap > o ? out + o : NULL, cap > o ? cap - o : 0,
                         " k=%.4f tau=%.1f l=%.1f base=%.2f final=%.2f rise=%.2f riseinf=%.2f amb=%.2f "
                         "settled=%u tauok=%u conv=%u kp=%.4f ki=%.4f kd=%.4f rule=%u ref=%u",
                         (double)st->model.k_gain_c_per_duty, (double)st->model.tau_s,
                         (double)st->model.dead_time_s, (double)st->model.baseline_c,
                         (double)st->model.final_c, (double)st->model.raw_rise_c,
                         (double)st->model.rise_inf_c, (double)st->step_ambient_c,
                         st->model.settled ? 1u : 0u, st->model.tau_consistent_with_gain ? 1u : 0u,
                         st->model.extrapolation_converged ? 1u : 0u, (double)st->proposed_gains.kp,
                         (double)st->proposed_gains.ki, (double)st->proposed_gains.kd,
                         st->proposed_gains.rule, st->proposed_gains.refusal);
        } else if (st->method == AUTOTUNE_METHOD_RELAY && st->relay.valid) {
            n = snprintf(cap > o ? out + o : NULL, cap > o ? cap - o : 0,
                         " ku=%.4f tu=%.1f amp=%.2f cyc=%d kp=%.4f ki=%.4f kd=%.4f rule=%u ref=%u",
                         (double)st->relay.ku, (double)st->relay.tu_s, (double)st->relay.amplitude_c,
                         st->relay.cycles_used, (double)st->proposed_gains.kp,
                         (double)st->proposed_gains.ki, (double)st->proposed_gains.kd,
                         st->proposed_gains.rule, st->proposed_gains.refusal);
        } else {
            n = 0;
        }
        if (n < 0) {
            n = 0;
        }
        total += n;
        o = (cap > o && (size_t)n < cap - o) ? o + (size_t)n : cap - (cap > 0 ? 1 : 0);
    } else if (st->state == AUTOTUNE_ENGINE_ABORTED) {
        char reason[64];
        telemetry_sanitize_token(st->abort_reason, reason, sizeof(reason));
        n = snprintf(cap > o ? out + o : NULL, cap > o ? cap - o : 0, " reason=%s", reason);
        if (n < 0) {
            n = 0;
        }
        total += n;
        o = (cap > o && (size_t)n < cap - o) ? o + (size_t)n : cap - (cap > 0 ? 1 : 0);
    }

    return total;
}

uint8_t telemetry_ramp_lag_encode_rate_byte(float rate_c_per_hr)
{
    long v = lroundf(rate_c_per_hr);
    if (v > 126) v = 126;
    if (v < -126) v = -126;
    return (uint8_t)(v + 128);
}

int telemetry_ramp_lag_event_for_transition(bool prev_sustained, bool cur_sustained, float actual_c,
                                            float cur_commanded_rate, float cur_achieved_rate,
                                            float prev_held_s, float prev_commanded_rate,
                                            float prev_achieved_rate, int32_t *out_arg,
                                            uint8_t out_note[EVENT_LOG_NOTE_LEN])
{
    if (prev_sustained == cur_sustained) {
        return -1;
    }
    memset(out_note, 0, EVENT_LOG_NOTE_LEN);
    if (cur_sustained) {
        /* Rising edge -- this tick's own fields are the live, meaningful
         * ones (profile_exec_zone_status_t only fills them in while
         * ramp_lag_sustained is true). */
        out_note[0] = telemetry_ramp_lag_encode_rate_byte(cur_commanded_rate);
        out_note[1] = telemetry_ramp_lag_encode_rate_byte(cur_achieved_rate);
        *out_arg = (int32_t)lroundf(actual_c * 100.0f);
        return EVENT_CODE_FIRING_RAMP_LAG_STARTED;
    }
    /* Falling edge -- this tick's fields already read 0 (reset the same
     * tick ramp_lag_sustained cleared, see profile_executor_ramp_assist.c's
     * ramp_assist_zone_lag_tick()), so report what the CALLER remembered
     * from the last tick sustained was still true. */
    out_note[0] = telemetry_ramp_lag_encode_rate_byte(prev_commanded_rate);
    out_note[1] = telemetry_ramp_lag_encode_rate_byte(prev_achieved_rate);
    *out_arg = (int32_t)lroundf(prev_held_s);
    return EVENT_CODE_FIRING_RAMP_LAG_CLEARED;
}
