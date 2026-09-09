// firing_compare.c -- see firing_compare.h. ITER_TUNE_REDESIGN_PLAN.md
// sec 2.3 + sec 3, step 2.

#include "firing_compare.h"

#include <math.h>
#include <string.h>

float firing_compare_bar1_floor(firing_subscore_t sub, float rate_c_per_s)
{
    if (sub == FIRING_SUBSCORE_LAG_S) {
        // 0.5 degC expressed as seconds of lag at this class's commanded
        // rate. A zero/absent rate cannot produce a lag sub-score at all
        // (firing_score.c only records lag for ramps), but return a finite
        // fallback rather than an infinity so no caller divides by it.
        if (rate_c_per_s > 0.0f) return FIRING_COMPARE_OWNER_FLOOR_C / rate_c_per_s;
        return FIRING_COMPARE_OWNER_FLOOR_C;
    }
    return FIRING_COMPARE_OWNER_FLOOR_C;
}

static void sort_floats(float *v, int n)
{
    for (int i = 1; i < n; i++) {
        float k = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; }
        v[j + 1] = k;
    }
}

static float median_of(float *v, int n)
{
    if (n <= 0) return 0.0f;
    sort_floats(v, n);
    if (n & 1) return v[n / 2];
    return 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

static const firing_segment_score_t *find_key(const firing_score_set_t *set, const firing_class_key_t *key)
{
    for (uint8_t i = 0; i < set->count; i++) {
        if (firing_class_key_equal(&set->entry[i].key, key)) return &set->entry[i];
    }
    return NULL;
}

firing_compare_verdict_t firing_compare(const firing_score_set_t *baseline, const firing_score_set_t *trial,
                                        const firing_compare_floors_t *floors, firing_compare_result_t *out)
{
    firing_compare_result_t r;
    memset(&r, 0, sizeof(r));

    float raw[FIRING_SUBSCORE_COUNT][FIRING_COMPARE_MAX_PAIRS];
    float norm[FIRING_SUBSCORE_COUNT][FIRING_COMPARE_MAX_PAIRS];
    int   cnt[FIRING_SUBSCORE_COUNT] = {0};
    int   improved[FIRING_SUBSCORE_COUNT] = {0};
    float in_band[FIRING_COMPARE_MAX_PAIRS];
    int   in_band_n = 0;

    for (uint8_t i = 0; i < baseline->count; i++) {
        const firing_segment_score_t *a = &baseline->entry[i];
        const firing_segment_score_t *b = find_key(trial, &a->key);
        if (!b) continue;
        r.matched_classes++;

        if (in_band_n < FIRING_COMPARE_MAX_PAIRS) {
            in_band[in_band_n++] = b->in_band_frac - a->in_band_frac;
        }

        for (int s = 0; s < FIRING_SUBSCORE_COUNT; s++) {
            if (!a->has[s] || !b->has[s]) continue;
            if (cnt[s] >= FIRING_COMPARE_MAX_PAIRS) continue;
            float d = b->value[s] - a->value[s];
            // Both sets scored the same class, so either firing's rate is
            // the class's rate; baseline's is used for determinism.
            float floor_v = firing_compare_bar1_floor((firing_subscore_t)s, a->rate_c_per_s);
            if (!(floor_v > 0.0f)) floor_v = FIRING_COMPARE_OWNER_FLOOR_C;
            raw[s][cnt[s]] = d;
            norm[s][cnt[s]] = d / floor_v;
            if (d < 0.0f) improved[s]++;
            cnt[s]++;
        }
    }

    bool any_bar1 = false;
    bool any_bar2 = false;
    bool any_degraded = false;
    bool have_any_sample = false;
    float composite_sum = 0.0f;
    int composite_n = 0;

    r.bar2_applied = (floors && floors->available);

    for (int s = 0; s < FIRING_SUBSCORE_COUNT; s++) {
        firing_compare_subscore_t *ss = &r.sub[s];
        ss->n = (uint16_t)cnt[s];
        ss->improved = (uint16_t)improved[s];
        if (cnt[s] == 0) continue;
        have_any_sample = true;

        ss->median_raw = median_of(raw[s], cnt[s]);
        ss->median_normalised = median_of(norm[s], cnt[s]);
        composite_sum += ss->median_normalised;
        composite_n++;

        ss->bar1_cleared = (ss->median_normalised <= -1.0f);
        if (ss->bar1_cleared) any_bar1 = true;

        if (r.bar2_applied) {
            int need = (int)ceilf(FIRING_COMPARE_BAR2_SIGN_FRACTION * (float)cnt[s]);
            bool sign_ok = (cnt[s] >= FIRING_COMPARE_BAR2_MIN_N) && (improved[s] >= need);
            float f = floors->floor_value[s];
            bool magnitude_ok = (f > 0.0f) && (-ss->median_raw > f);
            ss->bar2_cleared = sign_ok && magnitude_ok;
            if (ss->bar2_cleared) any_bar2 = true;
        }

        // No-degradation veto: this sub-score got materially WORSE, by more
        // than one Bar-1 floor, with enough matched pairs to believe it.
        if (cnt[s] >= FIRING_COMPARE_VETO_MIN_N && ss->median_normalised >= 1.0f) {
            ss->degraded = true;
            any_degraded = true;
        }
    }

    r.in_band_n = (uint16_t)in_band_n;
    if (in_band_n > 0) {
        r.in_band_median_delta = median_of(in_band, in_band_n);
        if (in_band_n >= FIRING_COMPARE_VETO_MIN_N &&
            r.in_band_median_delta < -FIRING_COMPARE_IN_BAND_TOLERANCE) {
            r.in_band_veto = true;
        }
    }
    r.composite_normalised = composite_n ? (composite_sum / (float)composite_n) : 0.0f;

    if (!have_any_sample) {
        r.verdict = FIRING_COMPARE_NO_MATCHED_PAIRS;
    } else if (any_degraded || r.in_band_veto) {
        // The veto outranks any improvement: non-dominance, not a trade.
        r.verdict = FIRING_COMPARE_REJECT_DEGRADED;
    } else if (any_bar1 && (!r.bar2_applied || any_bar2)) {
        r.verdict = FIRING_COMPARE_ACCEPT;
    } else {
        r.verdict = FIRING_COMPARE_INSUFFICIENT;
    }

    if (out) *out = r;
    return r.verdict;
}
