// firing_score.c -- see firing_score.h for the design rationale.
// ITER_TUNE_REDESIGN.md sec 2.1-2.2, step 1.

#include "firing_score.h"

#include <math.h>
#include <string.h>

static float cfg_temp_bucket(const firing_score_cfg_t *cfg)
{
    return (cfg->temp_bucket_c > 0.0f) ? cfg->temp_bucket_c : FIRING_SCORE_DEFAULT_TEMP_BUCKET_C;
}

static float cfg_rate_bucket(const firing_score_cfg_t *cfg)
{
    return (cfg->rate_bucket_c_per_hr > 0.0f) ? cfg->rate_bucket_c_per_hr
                                              : FIRING_SCORE_DEFAULT_RATE_BUCKET_C_PER_HR;
}

static uint32_t cfg_min_ticks(const firing_score_cfg_t *cfg)
{
    return cfg->min_scored_ticks ? cfg->min_scored_ticks : FIRING_SCORE_MIN_SCORED_TICKS;
}

static int16_t bucket_of(float value, float width)
{
    if (width <= 0.0f) return 0;
    float b = floorf(value / width);
    if (b > 32000.0f) b = 32000.0f;
    if (b < -32000.0f) b = -32000.0f;
    return (int16_t)b;
}

firing_seg_kind_t firing_score_classify(float commanded_rate_c_per_hr)
{
    if (commanded_rate_c_per_hr >= FIRING_SCORE_RAMP_MIN_RATE_C_PER_HR) return FIRING_SEG_RAMP_UP;
    if (commanded_rate_c_per_hr <= -FIRING_SCORE_RAMP_MIN_RATE_C_PER_HR) return FIRING_SEG_RAMP_DOWN;
    return FIRING_SEG_DWELL;
}

void firing_score_seg_begin(firing_score_seg_t *seg, const firing_score_cfg_t *cfg, uint8_t zone_index,
                            float commanded_rate_c_per_hr, float mean_target_c, float dead_time_s, float tau_s)
{
    memset(seg, 0, sizeof(*seg));
    seg->cfg = *cfg;
    seg->zone_index = zone_index;
    seg->kind = firing_score_classify(commanded_rate_c_per_hr);
    seg->rate_c_per_s = (seg->kind == FIRING_SEG_DWELL) ? 0.0f : fabsf(commanded_rate_c_per_hr) / 3600.0f;
    seg->mean_target_c = mean_target_c;
    // Entry window sized from this zone's own identified model -- plan sec
    // 2.2's "L + 2*tau". Guard against an unidentified zone (both zero) by
    // falling back to one PWM window, which is the shortest interval over
    // which a dwell-entry peak is even meaningful at a 60 s window.
    float w = dead_time_s + 2.0f * tau_s;
    seg->entry_window_s = (w > 0.0f) ? w : 60.0f;
    seg->settle_last_outside_s = -1.0f;
}

void firing_score_seg_tick(firing_score_seg_t *seg, bool *zone_captured, float target_c, float actual_c,
                           bool saturated_high, float dt_s)
{
    if (dt_s <= 0.0f) return;
    seg->elapsed_s += dt_s;

    float err = actual_c - target_c;      // positive == above target
    float abs_err = fabsf(err);
    float band = (seg->cfg.band_c > 0.0f) ? seg->cfg.band_c : 5.0f;

    // Capture-transient exclusion: firing-wide, latched the first time this
    // zone comes within band of its target. Everything before it is the work
    // whose size depends on the start temperature, and is discarded.
    if (zone_captured && !*zone_captured) {
        if (abs_err <= band) {
            *zone_captured = true;
        } else {
            return;
        }
    }

    // Signed-lag companion (FIRING_SUBSCORE_LAG_SIGNED_S): fed on EVERY ramp
    // tick that survives only the capture-transient exclusion above, i.e.
    // BEFORE the infeasibility exclusion below and regardless of sign. This
    // is deliberate -- it exists to see the two things LAG_S's own
    // definition hides (docs/audits/firing_score_four_objective_scorecard_
    // 2026-09-13.md finding C): which direction the error is on, and the
    // saturated-and-short ticks that a heater-limited zone spends failing
    // hardest to track. LAG_S itself is left untouched below, byte-identical
    // to its pre-2026-09-13 behaviour, since it feeds the pinned A1 bar.
    if (seg->kind != FIRING_SEG_DWELL && seg->rate_c_per_s > 0.0f) {
        // Sign convention: positive == BEHIND schedule (the classic lag),
        // negative == AHEAD of schedule (leading). For a ramp-up, behind
        // means actual < target (err < 0), so signed lag = -err/rate. For a
        // ramp-down, behind means actual > target (err > 0) since the zone
        // has not cooled as far as commanded yet, so signed lag = +err/rate.
        float lag_signed_s = (seg->kind == FIRING_SEG_RAMP_DOWN)
                                  ? (err / seg->rate_c_per_s)
                                  : (-err / seg->rate_c_per_s);
        int sbin = (int)floorf(lag_signed_s / FIRING_SCORE_LAG_SIGNED_BIN_S) +
                   FIRING_SCORE_LAG_SIGNED_CENTER_BIN;
        if (sbin < 0) sbin = 0;
        if (sbin >= FIRING_SCORE_LAG_SIGNED_BINS) sbin = FIRING_SCORE_LAG_SIGNED_BINS - 1;
        if (seg->lag_signed_hist[sbin] < 0xffffu) seg->lag_signed_hist[sbin]++;
        seg->lag_signed_samples++;
    }

    // Infeasibility exclusion: saturated at full duty and still short of
    // target -- the heater is the limit, not the gains.
    if (saturated_high && err < 0.0f) return;

    seg->scored_ticks++;
    if (abs_err <= band) seg->in_band_ticks++;

    if (seg->kind != FIRING_SEG_DWELL) {
        // lag in SECONDS, so ramps at different commanded rates compare:
        // 0.5 degC at 50 degC/hr and 5 degC at 500 degC/hr are the same 36 s.
        if (seg->rate_c_per_s > 0.0f) {
            float lag_s = abs_err / seg->rate_c_per_s;
            int bin = (int)(lag_s / FIRING_SCORE_LAG_BIN_S);
            if (bin < 0) bin = 0;
            if (bin >= FIRING_SCORE_LAG_BINS) bin = FIRING_SCORE_LAG_BINS - 1;
            if (seg->lag_hist[bin] < 0xffffu) seg->lag_hist[bin]++;
            seg->lag_samples++;
        }
    } else {
        // Settle-time tracking (FIRING_SUBSCORE_SETTLE_S): runs across the
        // WHOLE dwell (entry window and steady phase both), on every scored
        // tick. `settle_last_outside_s` is the elapsed time of the LAST tick
        // seen outside the settle band -- if the zone re-enters and then
        // leaves the band again later, this correctly moves forward to the
        // later excursion, which is exactly "time until it enters and
        // REMAINS inside the band". If the last scored tick of the segment
        // is still outside the band the dwell NEVER settled, and that is NOT
        // reported as a number: `settle_outside_at_end` latches and
        // seg_finish() clears has[SETTLE_S] instead. Reporting the segment
        // duration there (as this did until 2026-09-14) is an in-band
        // sentinel -- indistinguishable from a slow-but-settled dwell, and
        // arithmetic downstream consumes it as a measurement. See
        // docs/audits/firing_score_subscore_enrolment_2026-09-14.md R5.
        seg->settle_have_tick = true;
        if (abs_err > FIRING_SCORE_SETTLE_BAND_C) {
            seg->settle_last_outside_s = seg->elapsed_s;
            seg->settle_outside_at_end = true;
        } else {
            seg->settle_outside_at_end = false;
        }

        if (seg->elapsed_s <= seg->entry_window_s) {
            if (!seg->entry_seen || err > seg->entry_peak_c) {
                seg->entry_peak_c = err;
            }
            if (!seg->entry_seen || err < seg->entry_trough_c) {
                seg->entry_trough_c = err;
            }
            seg->entry_seen = true;
        } else {
            seg->steady_ticks++;
            seg->steady_sumsq += (double)err * (double)err;
        }
    }
}

bool firing_score_seg_finish(const firing_score_seg_t *seg, firing_segment_score_t *out)
{
    if (seg->scored_ticks < cfg_min_ticks(&seg->cfg)) return false;
    if (!out) return false;

    memset(out, 0, sizeof(*out));
    out->key.zone_index = seg->zone_index;
    out->key.kind = (uint8_t)seg->kind;
    out->key.rate_bucket = (seg->kind == FIRING_SEG_DWELL)
                               ? 0
                               : bucket_of(seg->rate_c_per_s * 3600.0f, cfg_rate_bucket(&seg->cfg));
    out->key.temp_bucket = bucket_of(seg->mean_target_c, cfg_temp_bucket(&seg->cfg));
    out->rate_c_per_s = seg->rate_c_per_s;
    out->scored_ticks = seg->scored_ticks;
    out->merged = 1;
    out->in_band_frac = (float)seg->in_band_ticks / (float)seg->scored_ticks;

    if (seg->kind != FIRING_SEG_DWELL) {
        if (seg->lag_samples > 0) {
            // Median from the streaming histogram: bin centre of the bin
            // holding the (n+1)/2-th sample. Median, not mean, so one
            // excluded-tick boundary artifact cannot move the score.
            uint32_t half = (seg->lag_samples + 1u) / 2u;
            uint32_t cum = 0;
            int bin = FIRING_SCORE_LAG_BINS - 1;
            for (int i = 0; i < FIRING_SCORE_LAG_BINS; i++) {
                cum += seg->lag_hist[i];
                if (cum >= half) { bin = i; break; }
            }
            out->has[FIRING_SUBSCORE_LAG_S] = true;
            out->value[FIRING_SUBSCORE_LAG_S] = ((float)bin + 0.5f) * FIRING_SCORE_LAG_BIN_S;
        }
        if (seg->lag_signed_samples > 0) {
            uint32_t half = (seg->lag_signed_samples + 1u) / 2u;
            uint32_t cum = 0;
            int bin = FIRING_SCORE_LAG_SIGNED_BINS - 1;
            for (int i = 0; i < FIRING_SCORE_LAG_SIGNED_BINS; i++) {
                cum += seg->lag_signed_hist[i];
                if (cum >= half) { bin = i; break; }
            }
            out->has[FIRING_SUBSCORE_LAG_SIGNED_S] = true;
            out->value[FIRING_SUBSCORE_LAG_SIGNED_S] =
                ((float)(bin - FIRING_SCORE_LAG_SIGNED_CENTER_BIN) + 0.5f) * FIRING_SCORE_LAG_SIGNED_BIN_S;
        }
    } else {
        if (seg->entry_seen) {
            out->has[FIRING_SUBSCORE_ENTRY_PEAK_C] = true;
            // Overshoot only: a dwell entered from below never overshoots, and
            // reporting a negative "overshoot" would let an undershooting
            // trial score better on the overshoot axis for the wrong reason.
            // Left exactly as-is (2edbb6eb confirmed the raw-peak definition
            // sound) -- undershoot now has its OWN axis below instead of
            // being folded, sign-flipped, into this one.
            out->value[FIRING_SUBSCORE_ENTRY_PEAK_C] = (seg->entry_peak_c > 0.0f) ? seg->entry_peak_c : 0.0f;

            out->has[FIRING_SUBSCORE_ENTRY_UNDERSHOOT_C] = true;
            // Undershoot only, same anti-gaming shape as ENTRY_PEAK_C above
            // but on its own axis: a perfect or over-shooting entry scores
            // 0.0 here (never negative), and this value is NEVER cancelled
            // by a later recovery -- it is the min error seen anywhere in
            // the entry window, not the final one.
            out->value[FIRING_SUBSCORE_ENTRY_UNDERSHOOT_C] =
                (seg->entry_trough_c < 0.0f) ? -seg->entry_trough_c : 0.0f;
        }
        if (seg->steady_ticks > 0) {
            out->has[FIRING_SUBSCORE_STEADY_RMS_C] = true;
            out->value[FIRING_SUBSCORE_STEADY_RMS_C] =
                (float)sqrt(seg->steady_sumsq / (double)seg->steady_ticks);
        }
        if (seg->settle_have_tick && !seg->settle_outside_at_end) {
            // Settled: the last scored tick was inside the band, so the
            // elapsed time of the LAST excursion is a real settle time.
            out->has[FIRING_SUBSCORE_SETTLE_S] = true;
            out->value[FIRING_SUBSCORE_SETTLE_S] =
                (seg->settle_last_outside_s < 0.0f) ? 0.0f : seg->settle_last_outside_s;
        }
        // else: NEVER settled. has[SETTLE_S] stays false -- the comparator's
        // first-class "this pair has nothing to say about that sub-score"
        // outcome, not a fabricated number a difference can be taken of.
        // A separate `dwell_unsettled` counter existed here from 2026-09-14
        // to report this out of band "for humans"; it was removed the same
        // day
        // (docs/audits/observability_gaps_closed_2026-09-14.md GAP 1) because
        // it had no consumer anywhere outside three test assertions --
        // firing_score has no live production caller yet at all, so there is
        // no real reader to surface it to, and has[SETTLE_S] == false already
        // makes "never settled" structurally distinct from any settled
        // value, with or without a count. Re-add a statistic like this only
        // once an actual caller (iter_tune.c or an HTTP surface) needs it,
        // shaped to what that caller actually consumes.
    }
    return true;
}

bool firing_class_key_equal(const firing_class_key_t *a, const firing_class_key_t *b)
{
    return a->zone_index == b->zone_index && a->kind == b->kind && a->rate_bucket == b->rate_bucket &&
           a->temp_bucket == b->temp_bucket;
}

bool firing_score_set_add(firing_score_set_t *set, const firing_segment_score_t *score)
{
    for (uint8_t i = 0; i < set->count; i++) {
        firing_segment_score_t *e = &set->entry[i];
        if (!firing_class_key_equal(&e->key, &score->key)) continue;
        // Same class twice in one firing == one observation of that class,
        // averaged. Treating them as two independent samples would inflate
        // the comparator's n with correlated data from a single firing.
        float w_old = (float)e->merged;
        float w_new = (float)score->merged;
        float w_tot = w_old + w_new;
        for (int s = 0; s < FIRING_SUBSCORE_COUNT; s++) {
            if (score->has[s] && e->has[s]) {
                e->value[s] = (e->value[s] * w_old + score->value[s] * w_new) / w_tot;
            } else if (s == FIRING_SUBSCORE_SETTLE_S) {
                // SETTLE_S merges only when BOTH segments settled. Absence
                // here means "one of these dwells never settled", which is
                // information, not a missing sample: letting the settled
                // one's number stand in for the class would resurrect the
                // in-band sentinel one level up.
                e->has[s] = false;
            } else if (score->has[s]) {
                e->has[s] = true;
                e->value[s] = score->value[s];
            }
        }
        e->in_band_frac = (e->in_band_frac * w_old + score->in_band_frac * w_new) / w_tot;
        e->scored_ticks += score->scored_ticks;
        e->merged = (uint16_t)(e->merged + score->merged);
        return true;
    }
    if (set->count >= FIRING_SCORE_MAX_ENTRIES) {
        set->dropped_full++;
        return false;
    }
    set->entry[set->count++] = *score;
    return true;
}

bool firing_score_set_finish_segment(firing_score_set_t *set, const firing_score_seg_t *seg)
{
    firing_segment_score_t s;
    if (!firing_score_seg_finish(seg, &s)) {
        set->dropped_short++;
        return false;
    }
    return firing_score_set_add(set, &s);
}
