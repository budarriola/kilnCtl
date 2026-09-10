#pragma once
// firing_score.h -- ITER_TUNE_REDESIGN_PLAN.md sec 2.1-2.2, step 1.
//
// Per-SEGMENT, per-zone tracking scoring. This replaces the whole-firing
// normalized-IAE scalar the old iter_tune scored on, whose defect (plan
// sec 0.1) is that every tick of the run folds into one number -- including
// the ticks spent climbing to meet the profile from whatever temperature
// the kiln happened to start at. Two firings differing only in start
// temperature therefore scored differently for a reason that has nothing to
// do with the gains, and the old module papered over that with a 2 degC
// start-temperature comparability window that made it nearly always idle.
//
// The unit of comparison here is a matched SEGMENT (a ramp at a commanded
// rate, or a dwell at a commanded temperature), keyed by
// (zone, kind, rate_bucket, temperature_bucket). Start temperature stops
// mattering because of two exclusions applied before any tick is scored:
//
//   - CAPTURE-TRANSIENT EXCLUSION: every tick before the zone FIRST comes
//     within the tracking band of its target is discarded, for the whole
//     firing (not per segment). That is exactly the work whose size depends
//     on the start temperature.
//   - INFEASIBILITY EXCLUSION: a tick where the zone is saturated at full
//     duty and still below target is excluded -- scoring it measures the
//     heater, not the controller.
//
// PURE DECISION/MEASUREMENT LOGIC. No ESP-IDF, no NVS, no lock, no
// FreeRTOS, no floating allocation -- same posture as max31856_codec.c.
// It is fed one tick at a time by whatever owns the control loop, and
// hands back a small fixed-size score set.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A segment whose scored-tick count falls below this is dropped entirely:
// one 60 s PWM window at the 1 Hz executor tick rate. Below that the
// sub-scores are dominated by window phase, not by the gains.
#define FIRING_SCORE_MIN_SCORED_TICKS 60

// Plan sec 2.1's bucket granularities. Both are DEFAULTS carried in
// firing_score_cfg_t rather than hard constants, because the bench plant
// model's own usable temperature span (ambient + model_k_dc, i.e. roughly
// 20-40 degC -- see sim_wide_temp_sweep.c's structural-ceiling note) is
// narrower than one 25 degC bucket, so every simulated segment would
// collapse into a single class and the comparator would never see more
// than one matched pair. Production uses the plan's 25 degC figures; the
// simulator narrows them and says so.
#define FIRING_SCORE_DEFAULT_TEMP_BUCKET_C 25.0f
#define FIRING_SCORE_DEFAULT_RATE_BUCKET_C_PER_HR 25.0f

// A commanded rate below this is a dwell, not a ramp -- this is what keeps
// lag_s (which divides by the commanded rate) from ever dividing by a
// near-zero number.
#define FIRING_SCORE_RAMP_MIN_RATE_C_PER_HR 10.0f

// Dwell-entry peak is smoothed with this EMA time constant before the max is
// taken (see firing_score_seg_tick()'s comment) -- one heater_output.c PWM
// window, the natural period of the on/off ripple this is filtering out.
#define FIRING_SCORE_ENTRY_SMOOTH_TAU_S 60.0f

// Lag histogram: 512 bins of 2 s covers 0..1024 s of tracking lag at 2 s
// median resolution. A streaming histogram rather than a stored tick array
// because this module runs on the target with a fixed, small footprint --
// storing every tick of an hour-long segment would not.
#define FIRING_SCORE_LAG_BINS 512
#define FIRING_SCORE_LAG_BIN_S 2.0f

// Fixed capacity of one firing's score set: 3 zones x 8 segment classes.
#define FIRING_SCORE_MAX_ENTRIES 24

typedef enum {
    FIRING_SEG_RAMP_UP = 0,
    FIRING_SEG_RAMP_DOWN = 1,
    FIRING_SEG_DWELL = 2,
} firing_seg_kind_t;

// The three sub-scores of plan sec 2.2. All "lower is better". A RAMP
// segment produces LAG only; a DWELL produces ENTRY_PEAK and STEADY_RMS.
// That asymmetry is deliberate -- comparison is per sub-score.
typedef enum {
    FIRING_SUBSCORE_LAG_S = 0,
    FIRING_SUBSCORE_ENTRY_PEAK_C = 1,
    FIRING_SUBSCORE_STEADY_RMS_C = 2,
    FIRING_SUBSCORE_COUNT = 3,
} firing_subscore_t;

typedef struct {
    uint8_t zone_index;
    uint8_t kind;        // firing_seg_kind_t
    int16_t rate_bucket; // commanded degC/hr / rate_bucket_c_per_hr; DWELL -> 0
    int16_t temp_bucket; // segment mean target / temp_bucket_c
} firing_class_key_t;

typedef struct {
    firing_class_key_t key;
    bool  has[FIRING_SUBSCORE_COUNT];
    float value[FIRING_SUBSCORE_COUNT];
    float in_band_frac;   // diagnostic + veto input, NOT a primary score (plan sec 2.2)
    float rate_c_per_s;   // |commanded rate|, so the comparator can size lag's Bar-1 floor
    uint32_t scored_ticks;
    uint16_t merged;      // how many same-key segments were folded into this entry
} firing_segment_score_t;

typedef struct {
    firing_segment_score_t entry[FIRING_SCORE_MAX_ENTRIES];
    uint8_t count;
    uint16_t dropped_short;  // segments discarded for < min_scored_ticks
    uint16_t dropped_full;   // segments discarded because the set was full
} firing_score_set_t;

typedef struct {
    float band_c;                 // profile-executor tracking band; capture + in-band diagnostic
    float temp_bucket_c;          // 0 -> FIRING_SCORE_DEFAULT_TEMP_BUCKET_C
    float rate_bucket_c_per_hr;   // 0 -> FIRING_SCORE_DEFAULT_RATE_BUCKET_C_PER_HR
    uint32_t min_scored_ticks;    // 0 -> FIRING_SCORE_MIN_SCORED_TICKS
} firing_score_cfg_t;

// One segment being accumulated. Zero-initialise then firing_score_seg_begin().
typedef struct {
    firing_score_cfg_t cfg;
    uint8_t zone_index;
    firing_seg_kind_t kind;
    float rate_c_per_s;        // |commanded rate|, 0 for a dwell
    float mean_target_c;
    float entry_window_s;      // L + 2*tau for this zone; dwell only
    float elapsed_s;           // segment time, advanced on EVERY tick (excluded or not)
    uint32_t scored_ticks;
    uint32_t in_band_ticks;
    uint16_t lag_hist[FIRING_SCORE_LAG_BINS];
    uint32_t lag_samples;
    bool  entry_seen;
    float entry_peak_c;
    float entry_err_ema_c;     // PWM-window-smoothed error; see firing_score.c's entry-peak comment
    uint32_t steady_ticks;
    double steady_sumsq;
} firing_score_seg_t;

// Classifies a commanded rate. |rate| < FIRING_SCORE_RAMP_MIN_RATE_C_PER_HR
// is a DWELL regardless of sign.
firing_seg_kind_t firing_score_classify(float commanded_rate_c_per_hr);

// Starts accumulating one segment. `mean_target_c` is the segment's mean
// commanded target (its midpoint for a ramp, its level for a dwell);
// `dead_time_s`/`tau_s` are this zone's own persisted identified model
// values, used ONLY to size the dwell entry window (L + 2*tau) so it is
// physically sized per zone instead of a fixed constant.
void firing_score_seg_begin(firing_score_seg_t *seg, const firing_score_cfg_t *cfg, uint8_t zone_index,
                            float commanded_rate_c_per_hr, float mean_target_c, float dead_time_s, float tau_s);

// One control tick. `zone_captured` is the caller-owned, FIRING-wide latch
// for the capture-transient exclusion -- pass the same bool for every
// segment of one zone's firing, and this function sets it the first time
// the zone comes within band. `saturated_high` is true when the zone
// commanded >= 98% duty for the whole preceding PWM window.
void firing_score_seg_tick(firing_score_seg_t *seg, bool *zone_captured, float target_c, float actual_c,
                           bool saturated_high, float dt_s);

// Finalises the segment into `out`. Returns false (and writes nothing) if
// the segment did not survive the min-scored-ticks rule.
bool firing_score_seg_finish(const firing_score_seg_t *seg, firing_segment_score_t *out);

// Adds one finished segment score to a firing's set, merging (count-weighted
// mean) into an existing entry with the identical class key -- two segments
// of the same class in one firing are one observation of that class, not two
// independent ones. Returns false if the set is full.
bool firing_score_set_add(firing_score_set_t *set, const firing_segment_score_t *score);

// Convenience: run seg_finish then set_add. Returns false if the segment was
// dropped for either reason (the set records which).
bool firing_score_set_finish_segment(firing_score_set_t *set, const firing_score_seg_t *seg);

bool firing_class_key_equal(const firing_class_key_t *a, const firing_class_key_t *b);

#ifdef __cplusplus
}
#endif
