// firing_score.c -- see firing_score.h for the design rationale.
// ITER_TUNE_REDESIGN_PLAN.md sec 2.1-2.2, step 1.

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
        if (seg->elapsed_s <= seg->entry_window_s) {
            if (!seg->entry_seen || err > seg->entry_peak_c) {
                seg->entry_peak_c = err;
                seg->entry_seen = true;
            }
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
    } else {
        if (seg->entry_seen) {
            out->has[FIRING_SUBSCORE_ENTRY_PEAK_C] = true;
            // Overshoot only: a dwell entered from below never overshoots, and
            // reporting a negative "overshoot" would let an undershooting
            // trial score better on the overshoot axis for the wrong reason.
            out->value[FIRING_SUBSCORE_ENTRY_PEAK_C] = (seg->entry_peak_c > 0.0f) ? seg->entry_peak_c : 0.0f;
        }
        if (seg->steady_ticks > 0) {
            out->has[FIRING_SUBSCORE_STEADY_RMS_C] = true;
            out->value[FIRING_SUBSCORE_STEADY_RMS_C] =
                (float)sqrt(seg->steady_sumsq / (double)seg->steady_ticks);
        }
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
