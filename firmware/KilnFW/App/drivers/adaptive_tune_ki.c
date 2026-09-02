// adaptive_tune_ki.c -- Integral (Ki) diagnosis, split out of adaptive_
// tune.c 2026-09-01 (see adaptive_tune_internal.h's own top comment for the
// full shape): the pure classifier (adaptive_tune_diagnose_ki(), host-tested
// directly) and its locked apply helper (adaptive_tune_refine_ki_locked()).
#include "adaptive_tune_internal.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"

#include "zones_http.h" // zones_config_get_pid/set_pid

// ---------------------------------------------------------------------
// Integral (Ki) diagnosis -- pure classification (host-tested directly)
// plus its locked apply helper.
// ---------------------------------------------------------------------

bool adaptive_tune_diagnose_ki(const float *actual_c, const float *duty, uint32_t n, float dt_s,
                               float dwell_err_mean_c, float dwell_err_max_c, adaptive_tune_ki_diag_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!actual_c || !duty || n < ADAPTIVE_TUNE_KI_MIN_SAMPLES || !(dt_s > 0.0f)) {
        out->verdict = ADAPTIVE_TUNE_KI_INSUFFICIENT;
        return true;
    }

    double sum = 0.0;
    float amin = INFINITY, amax = -INFINITY;
    float dmin = INFINITY, dmax = -INFINITY;
    double dsum = 0.0;
    for (uint32_t k = 0; k < n; k++) {
        sum += (double)actual_c[k];
        if (actual_c[k] < amin) amin = actual_c[k];
        if (actual_c[k] > amax) amax = actual_c[k];
        dsum += (double)duty[k];
        if (duty[k] < dmin) dmin = duty[k];
        if (duty[k] > dmax) dmax = duty[k];
    }
    float mean = (float)(sum / (double)n);
    float dmean = (float)(dsum / (double)n);
    double dvar = 0.0;
    for (uint32_t k = 0; k < n; k++) {
        double d = (double)duty[k] - (double)dmean;
        dvar += d * d;
    }
    dvar /= (double)n;
    float amplitude = (amax - amin) / 2.0f;
    float duty_amp = (dmax - dmin) / 2.0f;

    // Zero crossings of (actual_c - mean), plus their (fractional) sample
    // index, for the regularity/period check below.
    float cross_idx[ADAPTIVE_TUNE_KI_TRACE_CAPACITY];
    uint32_t ncross = 0;
    for (uint32_t k = 1; k < n; k++) {
        float p0 = actual_c[k - 1] - mean;
        float p1 = actual_c[k] - mean;
        if ((p0 < 0.0f && p1 >= 0.0f) || (p0 > 0.0f && p1 <= 0.0f)) {
            if (ncross < ADAPTIVE_TUNE_KI_TRACE_CAPACITY) {
                cross_idx[ncross] = (float)k;
            }
            ncross++;
        }
    }
    out->zero_crossings = ncross;

    bool regular = false;
    float mean_gap = 0.0f;
    if (ncross >= 3 && ncross <= ADAPTIVE_TUNE_KI_TRACE_CAPACITY) {
        uint32_t ngaps = ncross - 1;
        double gsum = 0.0;
        for (uint32_t g = 0; g < ngaps; g++) gsum += (double)(cross_idx[g + 1] - cross_idx[g]);
        mean_gap = (float)(gsum / (double)ngaps);
        double gvar = 0.0;
        for (uint32_t g = 0; g < ngaps; g++) {
            double d = (double)(cross_idx[g + 1] - cross_idx[g]) - (double)mean_gap;
            gvar += d * d;
        }
        gvar /= (double)ngaps;
        float gstd = (float)sqrt(gvar);
        if (mean_gap > 0.0f && (gstd / mean_gap) <= ADAPTIVE_TUNE_KI_CYCLE_REGULARITY_MAX) {
            regular = true;
        }
    }

    // A regular, multi-crossing oscillation is the ONLY evidence this
    // function trusts for "Ki too large" -- see below for why the half-
    // window drift figure that used to sit here was removed rather than
    // fixed in place.
    if (amplitude > ADAPTIVE_TUNE_KI_NOISE_FLOOR_C && ncross >= ADAPTIVE_TUNE_KI_MIN_CROSSINGS) {
        if (regular) {
            out->verdict = ADAPTIVE_TUNE_KI_LIMIT_CYCLE;
            out->tu_estimate_s = 2.0f * mean_gap * dt_s; // consecutive crossings are ~half a period apart
            const float pi = 3.14159265358979f;
            float denom = pi * amplitude; // relay hysteresis h == 0 for a non-relay trace, see
                                           // ADAPTIVE_TUNE_KI_RELAY_HYSTERESIS_C
            out->ku_estimate = (denom > 1e-6f) ? (4.0f * duty_amp / denom) : 0.0f;
            // K4: derived from the per-run-cap constant, not a separately
            // hardcoded literal -- see that constant's own comment.
            out->ki_correction_pct = -(ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE * 100.0f);
        } else {
            out->verdict = ADAPTIVE_TUNE_KI_OSCILLATING; // hunting -- irregular
            out->ki_correction_pct = -(ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE * 100.0f);
        }
        return true;
    }

    // Floored-duty check FIRST, ahead of the offset test below and
    // unconditional on dwell_err_mean_c -- floored means NO correction,
    // always, full stop; it must never be reachable only through the
    // offset branch's own threshold gate (a floored zone whose steady
    // error happens to sit right at ADAPTIVE_TUNE_KI_OFFSET_THRESHOLD_C's
    // edge, or that gets shadowed by some earlier branch, must not slip
    // through with a Ki change).
    bool floored = ((float)dvar < ADAPTIVE_TUNE_KI_FLOOR_DUTY_VARIANCE) &&
                   (dmean < ADAPTIVE_TUNE_KI_FLOOR_DUTY_RAIL_BAND ||
                    dmean > 1.0f - ADAPTIVE_TUNE_KI_FLOOR_DUTY_RAIL_BAND);
    if (floored) {
        // Duty is pinned near a rail and essentially not moving -- the
        // classic -ff_hold floor signature (pid.c), NOT a small-Ki
        // signature, regardless of what the temperature trace or the
        // dwell error figures look like. See adaptive_tune_ki_verdict_t's
        // own doc comment. Deliberately NO correction: raising Ki here
        // would be inert (the floor still applies) at best.
        out->verdict = ADAPTIVE_TUNE_KI_FLOORED;
        out->ki_correction_pct = 0.0f;
        return true;
    }

    // A former branch here flagged any first-half-vs-second-half mean shift
    // above a fixed threshold (the now-removed ADAPTIVE_TUNE_KI_DRIFT_
    // THRESHOLD_C) as "OSCILLATING" (Ki too large) with fabsf() applied to
    // the shift. That is sign-blind: this
    // function is never handed the setpoint (see this file's top comment),
    // so it cannot tell a slow monotonic APPROACH to setpoint (still
    // settling -- the classic too-small-Ki signature) from a slow walk AWAY
    // from it, and a one-directional trend crosses its own window mean only
    // once, which is not oscillation evidence by any definition -- the
    // crossing/regularity branch above is what identifies a limit cycle,
    // not an unsigned half-window shift. Rather than guess a direction from
    // data that cannot support the guess, this function now makes NO
    // separate drift-based call: a window that fails the crossing test
    // above falls through to the offset/floored evaluation, which uses
    // dwell_err_mean_c/dwell_err_max_c -- the caller's own SIGNED-magnitude,
    // whole-dwell error figures -- and always corrects in the direction
    // that is actually justified by unsigned evidence (increase Ki for a
    // steady, non-floored offset; nothing otherwise).
    if (dwell_err_mean_c > ADAPTIVE_TUNE_KI_OFFSET_THRESHOLD_C &&
        dwell_err_max_c <= dwell_err_mean_c * ADAPTIVE_TUNE_KI_OFFSET_MAX_OVER_MEAN) {
        out->verdict = ADAPTIVE_TUNE_KI_OFFSET_TOO_SMALL;
        out->ki_correction_pct = ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE * 100.0f; // K4, see comment above
        return true;
    }

    out->verdict = ADAPTIVE_TUNE_KI_OK;
    return true;
}

void adaptive_tune_refine_ki_locked(uint8_t zi, const profile_exec_firing_stats_t *stats)
{
    adaptive_tune_zone_t *z = &adaptive_tune_zones[zi];
    z->ki_applied = false;

    if (z->trace_count < ADAPTIVE_TUNE_KI_MIN_SAMPLES) {
        z->ki_verdict = (uint8_t)ADAPTIVE_TUNE_KI_INSUFFICIENT;
        z->ki_correction_pct = 0.0f;
        adaptive_tune_set_reason(z->ki_refusal_reason, sizeof(z->ki_refusal_reason),
                   "only %u/%u within-dwell trace samples", (unsigned)z->trace_count,
                   (unsigned)ADAPTIVE_TUNE_KI_MIN_SAMPLES);
        return;
    }

    float abuf[ADAPTIVE_TUNE_KI_TRACE_CAPACITY], dbuf[ADAPTIVE_TUNE_KI_TRACE_CAPACITY];
    for (uint32_t i = 0; i < z->trace_count; i++) {
        uint32_t idx = (z->trace_head + i) % ADAPTIVE_TUNE_KI_TRACE_CAPACITY;
        abuf[i] = z->trace_actual_c[idx];
        dbuf[i] = z->trace_duty[idx];
    }
    uint32_t first_idx = z->trace_head;
    uint32_t last_idx = (z->trace_head + z->trace_count - 1) % ADAPTIVE_TUNE_KI_TRACE_CAPACITY;
    float span_s = z->trace_t_s[last_idx] - z->trace_t_s[first_idx];
    float dt_est = (z->trace_count > 1) ? (span_s / (float)(z->trace_count - 1)) : 0.0f;

    adaptive_tune_ki_diag_t diag;
    adaptive_tune_diagnose_ki(abuf, dbuf, z->trace_count, dt_est, stats->dwell_err_mean_c, stats->dwell_err_max_c,
                              &diag);
    z->ki_verdict = (uint8_t)diag.verdict;
    z->ki_correction_pct = diag.ki_correction_pct;

    if (diag.verdict == ADAPTIVE_TUNE_KI_OK || diag.verdict == ADAPTIVE_TUNE_KI_INSUFFICIENT) {
        adaptive_tune_set_reason(z->ki_refusal_reason, sizeof(z->ki_refusal_reason), "no Ki correction indicated (verdict %u)",
                   (unsigned)diag.verdict);
        return;
    }
    if (diag.verdict == ADAPTIVE_TUNE_KI_FLOORED) {
        adaptive_tune_set_reason(z->ki_refusal_reason, sizeof(z->ki_refusal_reason),
                   "offset matches the -ff_hold integral floor signature, not small Ki -- withholding correction");
        return;
    }

    float kp, ki, kd;
    if (!zones_config_get_pid(zi, &kp, &ki, &kd) || !(ki > 0.0f)) {
        adaptive_tune_set_reason(z->ki_refusal_reason, sizeof(z->ki_refusal_reason), "no existing positive Ki to refine");
        return;
    }

    // P1/K5: latch this zone's baseline the first time this layer reaches a
    // live Ki for it -- see ki_baseline's struct comment (adaptive_tune_
    // internal.h). RAM-only here on purpose: adaptive_tune_run_end()
    // (adaptive_tune.c) snapshots adaptive_tune_zones[*].ki_baseline* AFTER this call
    // returns, still under adaptive_tune_lock, and dispatches the actual NVS
    // write to the flash worker only once the lock is released -- this
    // function must never itself touch NVS (it runs with adaptive_tune_lock
    // held, and a flash-worker wait must never happen under that lock, see
    // adaptive_tune_set_enabled()'s identical reasoning).
    //
    // Q4: this is only ONE of the two places ki_baseline gets written now.
    // The comment here used to call this "the autotuned baseline", which
    // stopped being true the moment adaptive_tune_refine_zone_locked() (adaptive_tune_
    // model.c) started rewriting Ki from a fresh SIMC recompute independent
    // of this layer -- a zone whose SIMC refine legitimately raised Ki past
    // 5x a stale value latched here would have its diagnosis muted
    // permanently, with no escape (see adaptive_tune_run_end()'s D5 comment
    // for why the two layers never both act in the same run, so this really
    // could go stale for good). adaptive_tune_refine_zone_locked() now re-latches
    // ki_baseline to its own freshly-written SIMC Ki every time it applies
    // (see that function's own comment) -- so this `if (!z->ki_baseline_
    // valid)` branch only ever fires for a zone this layer has NEVER reached
    // through EITHER path yet; once either layer has touched it, the
    // baseline tracks the more authoritative of the two (a direct SIMC
    // refit beats this layer's own shape-based inference, same priority
    // order D5 already applies to which correction gets to run at all).
    if (!z->ki_baseline_valid) {
        z->ki_baseline = ki;
        z->ki_baseline_valid = true;
    }

    float cap_pct = ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE * 100.0f;
    float capped_pct = diag.ki_correction_pct;
    if (capped_pct > cap_pct) capped_pct = cap_pct;
    if (capped_pct < -cap_pct) capped_pct = -cap_pct;
    float new_ki = ki * (1.0f + capped_pct / 100.0f);
    if (!(new_ki > 0.0f) || !isfinite(new_ki)) {
        adaptive_tune_set_reason(z->ki_refusal_reason, sizeof(z->ki_refusal_reason), "corrected Ki %.5f is not a valid gain",
                   (double)new_ki);
        return;
    }

    // P2/K6: the operational bound this layer is accountable for -- see
    // ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT's own comment (adaptive_tune_
    // internal.h) for why 5x (not the original 50x) is the right figure and
    // what the correct operator response is once it binds. Checked BEFORE
    // the setter call so a bound refusal is reported as such, not as an
    // ordinary rejection.
    float cumulative_ceiling = z->ki_baseline * ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT;
    if (new_ki > cumulative_ceiling) {
        // Message deliberately short -- ki_refusal_reason is only 96 bytes
        // (adaptive_tune_internal.h), and this needs to fit BOTH "cumulative
        // bound" (the guard's own name, already asserted on by the pre-P2
        // test) and "re-autotune" (P2's remediation hint) inside it.
        // K6/Q6: %.6f, not the original %.4f -- realistic kiln Ki magnitudes
        // (kc/ti) are order 1e-3, and at 4 decimal places every one of them
        // rendered as the useless "(0.0000)". Still well inside the 96-byte
        // buffer -- see this file's own length check in test_adaptive_tune.c.
        adaptive_tune_set_reason(z->ki_refusal_reason, sizeof(z->ki_refusal_reason),
                   "Ki %.6f > cumulative bound %.1fx baseline (%.6f) -- re-autotune this zone", (double)new_ki,
                   (double)ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT, (double)z->ki_baseline);
        return;
    }

    // Q2: the SYMMETRIC lower cumulative bound -- until now this layer only
    // ever bounded GROWTH (the ceiling above); the only floor was `!(new_ki >
    // 0.0f)` at the setter-input check above, which is not an operational
    // bound at all (it only rejects the literal non-positive/non-finite
    // case). A source of oscillation that does NOT scale with Ki -- coupling
    // from a neighbouring zone, relay chatter, thermocouple noise sitting
    // right above ADAPTIVE_TUNE_KI_NOISE_FLOOR_C -- decays Ki by up to
    // ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE (20%) every run with nothing to
    // stop it, and since P1 that decay now PERSISTS across reboots the same
    // way runaway growth does (ki_baseline survives a power cycle). Reuses
    // the same 5x figure as the ceiling -- see ADAPTIVE_TUNE_KI_CUMULATIVE_
    // MAX_MULT's own comment for why 5x is the right figure in either
    // direction: a zone that needs to shrink its Ki by more than 5x from its
    // own autotuned/SIMC baseline is, by this file's own standard, telling
    // us the FOPDT model is wrong, not that the integral term needs to keep
    // shrinking. Named distinctly ("cumulative floor", not "cumulative
    // bound") so the two refusal reasons are independently greppable/
    // testable -- see test_ki_diagnosis_cumulative_floor_binds_and_names_
    // itself() and test_ki_diagnosis_cumulative_floor_does_not_block_
    // legitimate_convergence() (test_adaptive_tune.c).
    float cumulative_floor = z->ki_baseline / ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT;
    if (new_ki < cumulative_floor) {
        adaptive_tune_set_reason(z->ki_refusal_reason, sizeof(z->ki_refusal_reason),
                   "Ki %.6f < cumulative floor %.2fx baseline (%.6f) -- re-autotune this zone", (double)new_ki,
                   (double)(1.0f / ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT), (double)z->ki_baseline);
        return;
    }

    if (!zones_config_set_pid(zi, kp, new_ki, kd)) {
        adaptive_tune_set_reason(z->ki_refusal_reason, sizeof(z->ki_refusal_reason), "zones_config_set_pid() rejected the corrected Ki");
        return;
    }

    z->ki_applied = true;
    z->ki_refusal_reason[0] = '\0';
    ESP_LOGI(ADAPTIVE_TUNE_TAG, "zone %u: Ki %.5f -> %.5f (%.1f%%, verdict %u) from dwell trace diagnosis", (unsigned)zi,
             (double)ki, (double)new_ki, (double)capped_pct, (unsigned)diag.verdict);
}
