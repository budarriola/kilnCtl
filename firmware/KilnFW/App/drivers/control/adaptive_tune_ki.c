// adaptive_tune_ki.c -- Integral (Ki) diagnosis, split out of adaptive_
// tune.c 2026-09-01 (see adaptive_tune_internal.h's own top comment for the
// full shape): the pure classifier (adaptive_tune_diagnose_ki(), host-tested
// directly) and its locked apply helper (adaptive_tune_refine_ki_locked()).
#include "adaptive_tune_internal.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"

#include "zones_config_accessors.h" // zones_config_get_pid/set_pid

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

    // EFFECTIVE-VS-REFERENCE GUARD (docs/audits/adaptive_tune_ki_effective_
    // reference_loop_2026-09-13.md): the diagnosis above was computed from
    // z->trace_actual_c[]/trace_duty[], the EFFECTIVE closed-loop behaviour
    // this zone actually produced -- and below, `new_ki` is about to be
    // written into zones_config's stored (REFERENCE) Ki via
    // zones_config_set_pid(). Those two are the same value only when
    // nothing rescales Ki between the reference read and its effect on the
    // plant. ZONE_CONTROL_MODE_PID_FUZZY's pid_fuzzy_adjust()
    // (profile_executor_pid_tick.c) is exactly such a rescale -- up to
    // +/-MAX_NUDGE_FRACTION per tick, keyed on live error/rate, invisible
    // to this function -- so a persistent fuzzy nudge (the centre rule
    // cell, 100% of observed hardware samples per
    // docs/audits/fuzzy_nine_cell_offline_probe_2026-09-11.md) shows up in
    // the trace as a steady offset or a limit cycle this layer attributes
    // to the REFERENCE Ki being wrong, then "corrects" by writing a fixed
    // +/-ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE nudge onto the very reference
    // fuzzy is scaling -- which does not change the divergence fuzzy caused,
    // so next run's trace looks the same and the correction repeats,
    // ratcheting the reference by a fixed ~20%/run until the cumulative
    // bound below binds. Same generating fault as the K_dc ratchets fixed
    // in 97288659/36f88d62: a correction inferred from a transformed
    // observable, written back to the untransformed reference. Refusing
    // here (rather than trying to divide the correction back out) is the
    // "cheap and honest" shape named in that audit doc's R6 finding --
    // scoped to fuzzy specifically because that is the only mechanism live
    // today that rescales Ki between reference and effect; a future
    // mechanism with the same shape (e.g. a temperature-keyed gain
    // schedule) needs its own equivalent check here, this one does not
    // generalize to it automatically.
    // K8 (docs/audits/adaptive_tune_ki_guard_timing_and_failopen_2026-09-14.md):
    // this used to read zones_config_get_control_mode()/get_fuzzy_strength_
    // pct() LIVE, right here, at refine time -- i.e. at run-end, after the
    // firing's own interlocks (which are the only thing that make those two
    // fields immutable mid-dwell) have already released. If fuzzy was active
    // WHILE this dwell's trace (z->trace_actual_c[]/trace_duty[], read
    // above) was captured, and is switched off before this function ever
    // runs, this guard used to miss exactly the case it exists for and the
    // reference-Ki ratchet described below was back. Both fields are now
    // read once, at dwell ENTRY (adaptive_tune_zone_tick()'s dwell_
    // just_entered branch, adaptive_tune.c), into z->trace_fuzzy_active/
    // trace_fuzzy_pct/trace_fuzzy_accessor_failed -- the snapshot reflects
    // the conditions this specific trace was actually gathered under, and is
    // consulted here instead of re-querying zones_config.
    //
    // Can fuzzy toggle MID-firing, spanning both states within one trace?
    // No, as things stand: zones_post_handler and backup_import.c (the only
    // two writers of control_mode/fuzzy_strength_pct) both sit behind
    // ota_http_check_interlocks(), which refuses while firing/hot/heater-
    // commanded -- true for the whole duration a dwell is being traced --
    // and POST /api/zones/pid (the one mid-firing config exception) has no
    // key for either field. So today a single trace is always captured
    // under one, unchanging mode, and dwell-entry vs. run-end would only
    // ever disagree because of the OFF-dwell gap this fix closes (fuzzy
    // switched off between the traced dwell ending and this function
    // running). If that interlock coverage ever changes and a trace could
    // genuinely span both states, the safe reading of "captured under
    // fuzzy at any point during this trace" is still exactly what the
    // dwell-entry snapshot gives: it was recorded before any tick of this
    // trace ran, so it can never miss a fuzzy-then-off transition that
    // happened inside the window it covers.
    //
    // Defect 2, fail CLOSED: trace_fuzzy_accessor_failed (set by the
    // dwell-entry snapshot when either accessor call failed) withholds the
    // correction exactly like a genuinely-active fuzzy trace would -- an
    // accessor failure must never read as "not fuzzy, proceed". Reported
    // through a message that names the failure distinctly from the normal
    // fuzzy-active refusal below, so the two are distinguishable in the
    // field. !trace_fuzzy_snapshot_valid is the same "we don't actually
    // know" case (defensive only -- refine_ki_locked never runs without a
    // dwell having entered first) and is folded into the same fail-closed
    // branch for the identical reason.
    if (!z->trace_fuzzy_snapshot_valid || z->trace_fuzzy_accessor_failed) {
        adaptive_tune_set_reason(z->ki_refusal_reason, sizeof(z->ki_refusal_reason),
                   "Ki accessor failed at dwell capture: withholding correction (fail-closed, not fuzzy)");
        return;
    }
    if (z->trace_fuzzy_active) {
        // K7 (docs/audits/ki_refusal_truncation_and_drift_check_lock_2026-09-13.md):
        // ki_refusal_reason is only 96 bytes -- this wording is 85 bytes at
        // trace_fuzzy_pct=100 (the worst case), well inside the buffer,
        // while keeping both halves of the message: what was withheld (the
        // correction) and why (trace reflects fuzzy's effect at capture
        // time, not the stored reference). See
        // test_ki_diagnosis_withholds_correction_when_zone_is_pid_fuzzy()
        // and test_ki_diagnosis_withholds_when_fuzzy_active_during_capture_
        // but_off_at_refine() (test_adaptive_tune_ki_bounds.c), which check
        // the full formatted length AND a token from the END of the
        // message, not just a prefix grep.
        adaptive_tune_set_reason(z->ki_refusal_reason, sizeof(z->ki_refusal_reason),
                   "zone PID_FUZZY %.0f%% at capture: withholding Ki -- trace reflects fuzzy, not reference",
                   (double)z->trace_fuzzy_pct);
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

    // U1: snapshot the PRE-change gains (kp/ki/kd, still the live values at
    // this point) and model (fetched fresh -- this layer never touches
    // K_dc/tau/dead_time itself, but a revert must still restore them to
    // whatever they currently are, not silently zero them), plus the
    // ki_baseline state AS OF right now -- which, if the `if (!z->ki_
    // baseline_valid)` branch above just latched it fresh from this same
    // `ki`, is exactly ki itself, so reverting Ki back to `ki` leaves the
    // baseline still correctly describing the live value. See adaptive_
    // tune_capture_revert_locked()'s own comment.
    float cur_k_dc = 0.0f, cur_tau_s = 0.0f, cur_dead_time_s = 0.0f;
    zones_config_get_model(zi, &cur_k_dc, &cur_tau_s, &cur_dead_time_s); // best-effort, same as adaptive_
                                                                          // tune_model.c's identical call
    adaptive_tune_capture_revert_locked(z, kp, ki, kd, cur_k_dc, cur_tau_s, cur_dead_time_s);

    if (!zones_config_set_pid(zi, kp, new_ki, kd)) {
        adaptive_tune_set_reason(z->ki_refusal_reason, sizeof(z->ki_refusal_reason), "zones_config_set_pid() rejected the corrected Ki");
        z->revert_available = false; // nothing was actually written
        return;
    }

    z->ki_applied = true;
    z->ki_refusal_reason[0] = '\0';
    ESP_LOGI(ADAPTIVE_TUNE_TAG, "zone %u: Ki %.5f -> %.5f (%.1f%%, verdict %u) from dwell trace diagnosis", (unsigned)zi,
             (double)ki, (double)new_ki, (double)capped_pct, (unsigned)diag.verdict);
}
