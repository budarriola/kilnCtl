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

// K9 (docs/audits/simc_sole_gain_writer_2026-09-14.md, owner decision
// 2026-09-14 adopting options A+B of docs/audits/concurrent_fuzzy_pid_
// adaptation_2026-09-14.md): this function is now DIAGNOSTIC ONLY. It
// classifies the within-dwell trace exactly as before and publishes
// ki_verdict/ki_correction_pct/ku_estimate/tu_estimate_s (the last two via
// the verdict path, unused today but kept per that audit's section 4.1
// recommendation for a future rebuild) -- but it NEVER writes zones_config
// any more. adaptive_tune_refine_zone_locked() (adaptive_tune_model.c),
// the K_dc/SIMC path, is now the SOLE writer of this zone's PID gains.
//
// Why: adaptive_tune_diagnose_ki() classifies TRACE SHAPE (amplitude, zero
// crossings, steady offset) -- properties of the closed loop, i.e. of the
// plant AND whatever gains were actually running -- then used to write a
// correction relative to the STORED REFERENCE Ki, which is not necessarily
// the Ki that produced the trace whenever something rescales Ki between
// reference and effect (ZONE_CONTROL_MODE_PID_FUZZY's pid_fuzzy_adjust(),
// concretely, per docs/audits/adaptive_tune_ki_effective_reference_loop_
// 2026-09-13.md). That is a correction inferred from a transformed
// observable, written back to the untransformed reference -- the same
// generating fault as the K_dc ratchets fixed in 97288659/36f88d62 -- and it
// is not a one-off: with fuzzy sitting in its centre rule cell (the
// steady-state case), the same trace shape recurs run after run, so the
// correction repeats and the reference ratchets ~20%/run (measured:
// 4.2998x baseline over 10 runs with the guard disabled, see this file's
// git history and the review appended to the 2026-09-13 audit doc).
//
// 2026-09-13/14 (e78fbc5b, then 83627343/ac5c26a3) fixed this with a guard
// that withheld the correction specifically while PID_FUZZY was active,
// snapshotted at dwell entry and failing closed on an accessor error. The
// owner has since decided the fuzzy layer and the self-improving PID must
// run CONCURRENTLY WITH NO INTERLOCK (docs/audits/concurrent_fuzzy_pid_
// adaptation_2026-09-14.md) -- and per that document's section 1.1, the
// K_dc/SIMC path is ALREADY frame-independent (it fits a settled dwell's
// duty/rise, which a fixed point of a loop with integral action pins to the
// plant and setpoint alone, regardless of what the gains were) while this
// layer's shape-based inference is not and, per that document's section
// 1.2, its correction magnitude is a bang-bang +/-20% classifier output
// with no measurement content besides. With the fuzzy-conditional guard
// withholding nothing left to correct (SIMC being the only writer, and
// frame-independent by construction), the guard's own conditional --
// "withhold only when PID_FUZZY at non-zero strength" -- became dead
// machinery once generalized to "withhold always": rather than leave that
// dead conditional (the dwell-entry snapshot, the fail-closed accessor
// check, the two-worded refusal strings) in place, it is removed here along
// with the write path it used to guard. This SUPERSEDES e78fbc5b/83627343/
// ac5c26a3's guard -- not a revert-by-accident: those fixes closed a real
// hole in a mechanism that no longer exists; the write path itself is what
// is gone now, so there is nothing left for that guard to protect.
//
// What is UNCHANGED: the autotune_baseline_k_dc envelope (97288659/
// 36f88d62) -- this function never touched it and still doesn't; the
// strength_pct == 0 bit-for-bit contract (pid_fuzzy.c, 233ded79) and the
// no-model-runs-plain-PID behaviour, neither of which this file has any
// bearing on; and adaptive_tune_diagnose_ki()'s pure classification math
// above, untouched, still host-tested directly and still exactly as
// sensitive to trace shape as before -- only the APPLY half of this file
// changed.
void adaptive_tune_refine_ki_locked(uint8_t zi, const profile_exec_firing_stats_t *stats)
{
    adaptive_tune_zone_t *z = &adaptive_tune_zones[zi];
    z->ki_applied = false; // K9: this layer never writes any more -- always false, kept as a field
                            // (rather than removed) since adaptive_tune_get_status() still publishes
                            // it and a reader should see "never applies" rather than a vanished field.

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

    // K9: every remaining verdict (LIMIT_CYCLE/OSCILLATING/OFFSET_TOO_SMALL)
    // DOES indicate a correction, by this layer's own classifier -- but this
    // layer no longer writes it. Report the diagnosis (verdict/correction_pct
    // above already published) and stop; SIMC (adaptive_tune_model.c) is the
    // sole gain writer now.
    adaptive_tune_set_reason(z->ki_refusal_reason, sizeof(z->ki_refusal_reason),
               "diagnostic only (v%u %.0f%%) -- SIMC is the sole gain writer",
               (unsigned)diag.verdict, (double)diag.ki_correction_pct);
}
