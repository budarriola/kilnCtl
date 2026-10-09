// firing_score_from_capture -- adapter that feeds a recorded bench capture's
// real per-tick data into the PRODUCTION firing_score.c / firing_compare.c
// (linked as-is, never a Python or test-local mirror), so ITER_TUNE's Bar 2
// noise floor can be measured from an actual matched-condition pair instead
// of guessed. See docs/ITER_TUNE_REDESIGN.md sec 3.1/3.1.1 and the task
// that produced this file (2026-09-10).
//
// WHAT THIS IS NOT: it does not reimplement firing_score's scoring logic --
// every sub-score number below comes out of firing_score_seg_tick()/
// _seg_finish()/_set_add() and firing_compare() exactly as profile_executor
// would call them. This file's own job is purely mechanical: turn one
// capture's JSONL ticks into the (target_c, actual_c, duty, dt_s) sequence
// those functions want, split into RAMP/DWELL segments the way the capture's
// own "dwelling" flag reports them (a capture's "segment_index" spans BOTH a
// ramp AND its trailing dwell -- profile_executor's segment concept, not
// firing_score's -- so segments here are re-cut on every dwelling transition,
// not on segment_index changes).
//
// The minimal flat-JSON field extraction (parse_*_after/nth_key/load_capture)
// is a byte-for-byte copy of sim_credibility_gate.c's own parser: same
// capture schema, same "not a general JSON parser" caveat, kept in its own
// copy rather than shared through a new header because sim_credibility_gate.c
// says explicitly not to reuse it for anything not shaped exactly like this.
//
// Usage: firing_score_from_capture <baseline.jsonl> <trial.jsonl>
// Exit codes: 0 ran (see printed verdict), 2 usage/parse error,
// 3 SKIP -- a named capture file is missing (gitignored/local-only, same
// convention as sim_credibility_gate.c -- never silently pass).

#include "../drivers/control/firing_compare.h"
#include "../drivers/control/firing_score.h"
#include "sim_measured_zone_constants.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NZ 3
#define MAX_TICKS 20000
#define BAND_C 5.0f  // same convention as sim_iter_tune.c's BAND_C

typedef struct {
    double t;
    float  target_c;
    bool   dwelling;
    int    segment_index;
    float  duty[NZ];
    float  actual_c[NZ];
    bool   actual_valid[NZ];
} tick_t;

// ---- copied from sim_credibility_gate.c: minimal flat-JSON extraction ----
static const char *find_key(const char *from, const char *key) { return strstr(from, key); }

static bool parse_float_after(const char *p, const char *key, float *out)
{
    const char *k = find_key(p, key);
    if (!k) return false;
    const char *colon = strchr(k, ':');
    if (!colon) return false;
    *out = strtof(colon + 1, NULL);
    return true;
}

static bool parse_double_after(const char *p, const char *key, double *out)
{
    const char *k = find_key(p, key);
    if (!k) return false;
    const char *colon = strchr(k, ':');
    if (!colon) return false;
    *out = strtod(colon + 1, NULL);
    return true;
}

static bool parse_bool_after(const char *p, const char *key, bool *out)
{
    const char *k = find_key(p, key);
    if (!k) return false;
    const char *colon = strchr(k, ':');
    if (!colon) return false;
    colon++;
    while (isspace((unsigned char)*colon)) colon++;
    *out = (strncmp(colon, "true", 4) == 0);
    return true;
}

static bool parse_int_after(const char *p, const char *key, int *out)
{
    const char *k = find_key(p, key);
    if (!k) return false;
    const char *colon = strchr(k, ':');
    if (!colon) return false;
    *out = (int)strtol(colon + 1, NULL, 10);
    return true;
}

static const char *nth_key(const char *from, const char *key, int n)
{
    const char *p = from;
    for (int i = 0; i <= n; i++) {
        p = find_key(p, key);
        if (!p) return NULL;
        p += strlen(key);
    }
    const char *colon = strchr(p, ':');
    return colon ? colon + 1 : NULL;
}

static int load_capture(const char *path, tick_t *out, int max_ticks)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    static char line[262144];
    int n = 0;
    while (n < max_ticks && fgets(line, sizeof(line), f)) {
        tick_t tk;
        memset(&tk, 0, sizeof(tk));
        if (!parse_double_after(line, "\"t\":", &tk.t)) continue;
        if (!parse_float_after(line, "\"target_c\":", &tk.target_c)) continue;
        parse_bool_after(line, "\"dwelling\":", &tk.dwelling);
        parse_int_after(line, "\"segment_index\":", &tk.segment_index);
        bool ok = true;
        for (int z = 0; z < NZ; z++) {
            const char *dp = nth_key(line, "\"duty\"", z);
            const char *ap = nth_key(line, "\"actual_c\"", z);
            const char *vp = nth_key(line, "\"actual_valid\"", z);
            if (!dp || !ap || !vp) { ok = false; break; }
            tk.duty[z] = strtof(dp, NULL);
            tk.actual_c[z] = strtof(ap, NULL);
            while (isspace((unsigned char)*vp)) vp++;
            tk.actual_valid[z] = (strncmp(vp, "true", 4) == 0);
        }
        if (!ok) continue;
        out[n++] = tk;
    }
    fclose(f);
    return n;
}
// ---- end copied parser ----

// Builds one firing_score_set_t from a capture: per zone, re-cuts the tick
// stream into RAMP/DWELL runs on every "dwelling" transition (NOT on
// segment_index, which spans both halves -- see file header), computes each
// run's commanded rate from its own target_c delta, and feeds every tick
// through the real firing_score_seg_tick(). zone_captured is FIRING-wide per
// zone, per firing_score.h's contract.
static bool build_score_set(const tick_t *ticks, int n, firing_score_set_t *set)
{
    memset(set, 0, sizeof(*set));
    firing_score_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.band_c = BAND_C;

    for (int z = 0; z < NZ; z++) {
        bool zone_captured = false;
        int run_start = -1;
        for (int i = 0; i <= n; i++) {
            bool boundary = (i == n) || (run_start >= 0 && ticks[i].dwelling != ticks[run_start].dwelling);
            if (run_start < 0) { run_start = i; continue; }
            if (!boundary) continue;

            int run_end = i - 1; // inclusive
            bool dwelling = ticks[run_start].dwelling;
            float commanded_rate_c_per_hr = 0.0f;
            float mean_target_c;
            if (!dwelling) {
                double dt_hr = (ticks[run_end].t - ticks[run_start].t) / 3600.0;
                commanded_rate_c_per_hr = (dt_hr > 0.0)
                    ? (float)((ticks[run_end].target_c - ticks[run_start].target_c) / dt_hr)
                    : 0.0f;
                mean_target_c = 0.5f * (ticks[run_start].target_c + ticks[run_end].target_c);
            } else {
                double sum = 0.0; int cnt = 0;
                for (int j = run_start; j <= run_end; j++) { sum += ticks[j].target_c; cnt++; }
                mean_target_c = (cnt > 0) ? (float)(sum / cnt) : ticks[run_start].target_c;
            }

            firing_score_seg_t seg;
            firing_score_seg_begin(&seg, &cfg, (uint8_t)z, commanded_rate_c_per_hr, mean_target_c,
                                    g_dead_time_s[z], g_tau_s[z]);

            for (int j = run_start; j <= run_end; j++) {
                if (j == 0) continue; // no predecessor tick to derive dt_s from
                if (!ticks[j].actual_valid[z]) continue;
                float dt_s = (float)(ticks[j].t - ticks[j - 1].t);
                bool saturated_high = ticks[j].duty[z] >= 0.98f;
                firing_score_seg_tick(&seg, &zone_captured, ticks[j].target_c, ticks[j].actual_c[z],
                                      saturated_high, dt_s);
            }

            firing_score_set_finish_segment(set, &seg);
            run_start = i;
        }
    }
    return true;
}

static const char *subscore_name(firing_subscore_t s)
{
    switch (s) {
        case FIRING_SUBSCORE_LAG_S: return "lag_s";
        case FIRING_SUBSCORE_ENTRY_PEAK_C: return "entry_peak_c";
        case FIRING_SUBSCORE_STEADY_RMS_C: return "steady_rms_c";
        case FIRING_SUBSCORE_SETTLE_S: return "settle_s";
        case FIRING_SUBSCORE_ENTRY_UNDERSHOOT_C: return "entry_undershoot_c";
        case FIRING_SUBSCORE_LAG_SIGNED_S: return "lag_signed_s";
        default: return "?";
    }
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: firing_score_from_capture <baseline.jsonl> <trial.jsonl>\n");
        return 2;
    }
    const char *base_path = argv[1];
    const char *trial_path = argv[2];

    for (int i = 1; i <= 2; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) {
            printf("SKIP: firing_score_from_capture -- required input missing: %s\n"
                   "  Captures under logs/coupling/*.jsonl are gitignored/local-only.\n"
                   "  This harness refuses to report a result without them rather than\n"
                   "  silently passing.\n", argv[i]);
            return 3;
        }
        fclose(f);
    }

    static tick_t base_ticks[MAX_TICKS];
    static tick_t trial_ticks[MAX_TICKS];
    int base_n = load_capture(base_path, base_ticks, MAX_TICKS);
    int trial_n = load_capture(trial_path, trial_ticks, MAX_TICKS);
    if (base_n <= 0 || trial_n <= 0) {
        fprintf(stderr, "failed to parse captures (base_n=%d trial_n=%d)\n", base_n, trial_n);
        return 2;
    }

    firing_score_set_t base_set, trial_set;
    build_score_set(base_ticks, base_n, &base_set);
    build_score_set(trial_ticks, trial_n, &trial_set);

    printf("=== firing_score_from_capture ===\n");
    printf("baseline: %s (%d ticks) -> %d scored segment classes (dropped_short=%u dropped_full=%u)\n",
           base_path, base_n, base_set.count, base_set.dropped_short, base_set.dropped_full);
    printf("trial:    %s (%d ticks) -> %d scored segment classes (dropped_short=%u dropped_full=%u)\n",
           trial_path, trial_n, trial_set.count, trial_set.dropped_short, trial_set.dropped_full);

    printf("\n-- baseline classes --\n");
    for (int i = 0; i < base_set.count; i++) {
        firing_segment_score_t *e = &base_set.entry[i];
        printf("  z%d kind=%d rate_bucket=%d temp_bucket=%d scored_ticks=%u merged=%u in_band=%.3f",
               e->key.zone_index, e->key.kind, e->key.rate_bucket, e->key.temp_bucket, e->scored_ticks,
               e->merged, e->in_band_frac);
        for (int s = 0; s < FIRING_SUBSCORE_COUNT; s++)
            if (e->has[s]) printf(" %s=%.4f", subscore_name((firing_subscore_t)s), e->value[s]);
        printf("\n");
    }
    printf("\n-- trial classes --\n");
    for (int i = 0; i < trial_set.count; i++) {
        firing_segment_score_t *e = &trial_set.entry[i];
        printf("  z%d kind=%d rate_bucket=%d temp_bucket=%d scored_ticks=%u merged=%u in_band=%.3f",
               e->key.zone_index, e->key.kind, e->key.rate_bucket, e->key.temp_bucket, e->scored_ticks,
               e->merged, e->in_band_frac);
        for (int s = 0; s < FIRING_SUBSCORE_COUNT; s++)
            if (e->has[s]) printf(" %s=%.4f", subscore_name((firing_subscore_t)s), e->value[s]);
        printf("\n");
    }

    // Null comparison: floors=NULL (Bar 2 is what we're trying to measure --
    // it cannot gate its own measurement), so this exercises Bar 1 + the
    // no-degradation veto's raw arithmetic on genuinely comparable identical
    // conditions. Any nonzero median IS the floor this pair can bound.
    firing_compare_result_t result;
    firing_compare_verdict_t verdict = firing_compare(&base_set, &trial_set, NULL, &result);

    const char *verdict_name =
        verdict == FIRING_COMPARE_ACCEPT ? "ACCEPT" :
        verdict == FIRING_COMPARE_REJECT_DEGRADED ? "REJECT_DEGRADED" :
        verdict == FIRING_COMPARE_INSUFFICIENT ? "INSUFFICIENT" :
        verdict == FIRING_COMPARE_NO_MATCHED_PAIRS ? "NO_MATCHED_PAIRS" :
        verdict == FIRING_COMPARE_ALLOC_FAILED ? "ALLOC_FAILED" : "?";

    printf("\n=== firing_compare (null comparison: same profile+preset, per-channel\n"
           "    start delta 0.10-0.15C) ===\n");
    printf("verdict: %s\n", verdict_name);
    printf("matched_classes: %u\n", result.matched_classes);
    printf("in_band: n=%u median_delta=%.4f veto=%s degraded_untrusted=%s\n",
           result.in_band_n, result.in_band_median_delta,
           result.in_band_veto ? "true" : "false",
           result.in_band_degraded_untrusted ? "true" : "false");
    for (int s = 0; s < FIRING_SUBSCORE_COUNT; s++) {
        firing_compare_subscore_t *sub = &result.sub[s];
        printf("  %-13s n=%-3u improved=%-3u median_raw=%9.4f median_normalised=%8.4f bar1=%s bar2=%s degraded=%s degraded_untrusted=%s\n",
               subscore_name((firing_subscore_t)s), sub->n, sub->improved, sub->median_raw, sub->median_normalised,
               sub->bar1_cleared ? "Y" : "n", sub->bar2_cleared ? "Y" : "n",
               sub->degraded ? "Y" : "n", sub->degraded_untrusted ? "Y" : "n");
    }
    printf("composite_normalised (human-facing only): %.4f\n", result.composite_normalised);

    printf("\nFIRING_COMPARE_BAR1_MIN_N=%d FIRING_COMPARE_BAR2_MIN_N=%d (reachability: see printed n above)\n",
           FIRING_COMPARE_BAR1_MIN_N, FIRING_COMPARE_BAR2_MIN_N);

    return 0;
}
