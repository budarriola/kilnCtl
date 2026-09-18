// current_sense.c -- see current_sense.h. Implements docs/CURRENT_SENSE.md
// sections 2, 3b, 4 and 5 for the sampling/conversion/snapshot-publishing
// half only; S3/S4/S9 (the guards that would consume this) are explicitly
// Phase 7, once link_task exists to supply relay_recent_mask context.
//
// Sampling method, a deliberate deviation from ARCHITECTURE.md section 8's
// "use adc_set_round_robin() rather than switching the mux by hand": that
// guidance is written against the general RP2040-ADC-noob mistake of
// leaving stale FIFO data around while manually poking adc_select_input().
// It is superseded here by CURRENT_SENSE.md section 4's more specific and
// more authoritative instruction for THIS module: "No DMA, no free-running
// capture, no FFT, no RMS accumulator." Hardware round-robin
// (adc_set_round_robin() + adc_run(true) + FIFO) is exactly a free-running
// capture, and its natural sample order (one conversion per channel per
// round) does not match the required per-channel burst of "discard one,
// then 16 back-to-back real samples, then move on" -- reshaping that stream
// into per-channel bursts would need buffering equivalent to what free-run
// mode was supposed to avoid. So: adc_select_input() is called explicitly
// for each channel, in round-robin ORDER across the three channels every
// pass (satisfying the "round-robin across ADC0/1/2" requirement in
// ARCHITECTURE.md's module table), with single-shot adc_read() polling --
// no FIFO, no free-running, matching CURRENT_SENSE.md section 4 to the
// letter. This is documented here explicitly as a judgement call because
// the two docs read as being in tension until you notice one is general
// platform guidance and the other is this module's own spec.
//
// 2026-09-18, owner-directed 100 Hz rate increase: still no DMA, no free-
// running/round-robin FIFO capture, no FFT, no RMS accumulator -- those four
// bans still stand, and for the same reason as always: the front end is a
// rectified peak-envelope with a 1 s decay (R77||C57, tau=1.0s), not a raw
// AC waveform, so there is no waveform for an RMS accumulator or an FFT to
// operate on, and free-running capture buys nothing this module's own
// discard+oversample burst per channel doesn't already do more simply. What
// changed: current_sense_acquire_tick() now runs FIVE bounded, single-shot,
// manually-muxed rounds (still exactly the adc_select_input()+bounded-
// adc_read() polling described above, still no FIFO) per current_sense_
// sample() publish pass instead of one, at the SAME manually-muxed,
// non-free-running discipline -- see current_sense.h's CURRENT_SENSE_TICKS_
// PER_SAMPLE comment. This buys sqrt(5*16 / 16) ~= sqrt(5) ~= 2.24x
// reduction in the ADC's own broadband/quantization noise on top of the
// existing 16x-oversample's sqrt(16)=4x, IF that noise is uncorrelated
// sample to sample -- honestly, NOT for correlated pickup (mains hum
// coupled into the front end, supply ripple) since averaging more samples
// of a correlated signal does not shrink it. See docs/CURRENT_SENSE.md
// section 4's reconciliation note and docs/audits/
// ct_sampling_mains_aliasing_review_2026-09-18.md for the full reasoning
// this rate increase was built against.
#include "current_sense.h"

#include <math.h>

#include "FreeRTOS.h"
#include "task.h"

#include "hal_adc.h"

#include "board_pins.h"
#include "current_presence_policy.h"
#include "task_priorities.h"

// --- Constants from docs/CURRENT_SENSE.md ----------------------------------

#define CS_OVERSAMPLE_N        CURRENT_SENSE_OVERSAMPLE_N // current_sense.h -- section 4: "16 back-to-back conversions"
#define CS_ADC_VREF_V          3.3f    // 3.3v_Safty rail (unregulated-for-precision, section 4/5)
#define CS_ADC_FULL_SCALE      4096.0f // 12-bit SAR, per section 5's formula denominator
#define CS_ADC_MAX_COUNTS      4095u
#define CS_DEFAULT_GAIN        0.715f  // section 2: R46/R43 = 7.15k/10k, nominal
#define CS_SQRT2               1.41421356f

// Section 2: "A channel reading within ~50 mV of the rail ... is 'at least
// this much current, and the measurement is no longer valid'." Compared in
// the native count domain per this module's brief (avoid a needless
// division): 50 mV in 12-bit counts against a 3.3 V reference.
#define CS_CLIP_MARGIN_COUNTS  ((uint16_t)(0.050f * CS_ADC_FULL_SCALE / CS_ADC_VREF_V + 0.5f))
#define CS_CLIP_THRESHOLD_COUNTS  (CS_ADC_MAX_COUNTS - CS_CLIP_MARGIN_COUNTS)

// Section 4: "a slow first-order filter (tau ~= 0.5 s) on top, for reporting
// only."
#define CS_FILTER_TAU_S        0.5f
#define CS_SAMPLE_PERIOD_S     (SAFTYFW_PERIOD_CURRENT_TASK_MS / 1000.0f)

// Section 3b: "fraction of the last power_window_s (default 120 s)".
#define CS_POWER_WINDOW_S      120u
#define CS_POWER_WINDOW_SAMPLES \
    ((CS_POWER_WINDOW_S * 1000u) / SAFTYFW_PERIOD_CURRENT_TASK_MS)

static const uint8_t s_adc_channel[3] = {
    SAFTYFW_ADC_CH_CURRENT1,
    SAFTYFW_ADC_CH_CURRENT2,
    SAFTYFW_ADC_CH_CURRENT3,
};

// Calibration. Zero-initialized placeholder until config_store.c (Phase 9)
// calls current_sense_set_cal() with real commissioned values -- see
// current_sense.h's field-by-field doc comments for why each default is (or
// is not) safe to compile in.
static current_sense_cal_t s_cal;

// Most recent unfiltered snapshot (docs/ARCHITECTURE.md section 6).
static current_snapshot_t s_snapshot;

// Filtered/derived reporting quantities (docs/CURRENT_SENSE.md section 3b).
static current_sense_power_t s_power;

// Per-channel rolling window of "was this channel's filtered reading above
// i_present_a" over the last CS_POWER_WINDOW_SAMPLES passes, kept as a plain
// ring of 0/1 bytes with a running sum so conduction_fraction is an O(1)
// update rather than an O(window) rescan every 50 ms.
typedef struct {
    uint8_t  history[CS_POWER_WINDOW_SAMPLES];
    uint32_t head;
    uint32_t sum;
    bool     filled;
} cs_conduction_window_t;

static cs_conduction_window_t s_window[3];

// Filtered amps per channel, tau=0.5s low-pass of the unfiltered amps
// (docs/CURRENT_SENSE.md section 4). Feeds i_conducting_a/conduction_fraction
// -- see current_sense.h's header comment for why this never reaches
// current_snapshot_t.
static float s_filtered_amps[3];
static bool  s_filter_initialized[3];

// Acquisition accumulator, filled by current_sense_acquire_tick(), drained
// by current_sense_sample() -- see current_sense.h's comments on both. Only
// the merge into these fields runs under taskENTER_CRITICAL(); the ADC
// reads and the oversample-average arithmetic that feed the merge run
// outside it, and the divide-by-count arithmetic in the drain step also
// runs outside it (a local copy is taken under the critical section, then
// the copy is used unlocked) -- "accumulate under a short critical section,
// do arithmetic outside" per this task's acquisition requirements.
typedef struct {
    uint64_t sum;      // sum of this tick's 16x-oversampled per-tick averages
    uint16_t count;    // successful ticks accumulated since the last drain
    uint16_t timeouts; // ticks this channel aborted on a bounded-read timeout
} cs_accum_t;

static cs_accum_t s_accum[3];

static uint32_t s_degraded_window_count;

void current_sense_set_cal(const current_sense_cal_t *cal)
{
    s_cal = *cal;
}

void current_sense_set_ct_cal(const ct_amps_cal_table_t *ct_cal)
{
    s_cal.ct_cal = *ct_cal;
}

void current_sense_init(void)
{
    // s_cal, s_snapshot, s_power, s_window, s_filtered_amps are all static
    // storage, so they start zeroed -- explicit reset here anyway so
    // current_sense_init() is idempotent and does not depend on being
    // called exactly once at power-on.
    current_sense_cal_t zero_cal = {0};
    s_cal = zero_cal; // ct_cal's zero value == ct_amps_cal_uncalibrated_table()
                       // (calibrated false on every channel, gain/offset
                       // irrelevant) -- see current_sense.h's field comment.

    current_snapshot_t zero_snap = {0};
    s_snapshot = zero_snap;
    for (int n = 0; n < 3; n++) {
        s_snapshot.amps[n] = 0.0f;
    }

    current_sense_power_t zero_power = {0};
    s_power = zero_power;
    for (int n = 0; n < 3; n++) {
        s_power.p_avg_w[n] = NAN; // mains_voltage_v starts unconfigured
    }
    s_power.mains_voltage_v = NAN;
    s_power.p_total_w = NAN;
    s_power.energy_wh = 0.0;

    for (int n = 0; n < 3; n++) {
        s_window[n].head = 0;
        s_window[n].sum = 0;
        s_window[n].filled = false;
        for (uint32_t i = 0; i < CS_POWER_WINDOW_SAMPLES; i++) {
            s_window[n].history[i] = 0;
        }
        s_filtered_amps[n] = 0.0f;
        s_filter_initialized[n] = false;

        s_accum[n].sum = 0;
        s_accum[n].count = 0;
        s_accum[n].timeouts = 0;
    }
    s_degraded_window_count = 0;
}

// One 16x-oversampled, first-discarded read of the given ADC channel index
// (0/1/2 into s_adc_channel[]). Blocking, single-shot -- see this file's
// header comment for why this is manual polling rather than free-running
// capture.
static uint32_t cs_read_channel_counts(int n)
{
    hal_adc_select(s_adc_channel[n]);

    // "The first conversion after a mux change is the one to distrust;
    // discard it." (ARCHITECTURE.md section 8, CURRENT_SENSE.md section 4.)
    (void)hal_adc_read_raw();

    uint32_t sum = 0;
    for (uint32_t i = 0; i < CS_OVERSAMPLE_N; i++) {
        sum += hal_adc_read_raw();
    }
    return sum / CS_OVERSAMPLE_N;
}

// One bounded, non-blocking round-robin acquisition tick -- see
// current_sense.h's comment. Each individual conversion goes through
// hal_adc_read_raw_bounded() (HAL_OK / HAL_TIMEOUT), never the old unbounded
// hal_adc_read_raw() -- that stays reserved for cs_read_channel_counts()/
// current_sense_recalibrate_zero(), an explicitly blocking, commissioning-
// only, hardware-unvalidated path (current_sense.h's own doc comment on
// that function). A channel that times out on ANY conversion this tick
// (the discard read or any of the 16 oversamples) aborts just that
// channel's loop immediately and records a timeout -- it never averages a
// partial set of samples and calls that a reading.
void current_sense_acquire_tick(void)
{
    for (int n = 0; n < 3; n++) {
        hal_adc_select(s_adc_channel[n]);

        bool timed_out = false;
        uint16_t v;

        // Discard-first-after-mux-switch, same policy as cs_read_channel_
        // counts() -- see this file's header comment.
        if (hal_adc_read_raw_bounded(&v) != HAL_OK) {
            timed_out = true;
        }

        uint32_t sum = 0;
        if (!timed_out) {
            for (uint32_t i = 0; i < CS_OVERSAMPLE_N; i++) {
                if (hal_adc_read_raw_bounded(&v) != HAL_OK) {
                    timed_out = true;
                    break;
                }
                sum += v;
            }
        }

        // Merge into the shared accumulator. Only this handful of integer
        // updates runs inside the critical section -- no hardware access,
        // no division, bounded to a few instructions per channel per tick.
        taskENTER_CRITICAL();
        if (timed_out) {
            s_accum[n].timeouts++;
        } else {
            s_accum[n].sum += (sum / CS_OVERSAMPLE_N);
            s_accum[n].count++;
        }
        taskEXIT_CRITICAL();
    }
}

// Section 5's exact formula:
//   I = max(0, (counts - zero_counts)) * vref / 4096 / (0.715 * sqrt(2) * k_ct)
// with `gain` (this file's compiled-default-if-zero) standing in for the
// literal 0.715, and k_ct having NO safe default -- amps is 0 (cannot be
// computed at all) when k_ct_v_per_a <= 0.
static float cs_counts_to_amps(int n, uint32_t counts_avg)
{
    float k_ct = s_cal.k_ct_v_per_a[n];
    if (k_ct <= 0.0f) {
        // Not commissioned for this channel -- no unit conversion is
        // possible, and reporting 0 here is the same "obviously not a real
        // reading" honesty current_snapshot_t.calibrated already signals at
        // the whole-snapshot level.
        return 0.0f;
    }

    float gain = (s_cal.gain[n] > 0.0f) ? s_cal.gain[n] : CS_DEFAULT_GAIN;

    int32_t delta_counts = (int32_t)counts_avg - (int32_t)s_cal.zero_counts[n];
    if (delta_counts < 0) {
        delta_counts = 0; // "max(0, ...)" -- never a negative current
    }

    float v_adc = (float)delta_counts * CS_ADC_VREF_V / CS_ADC_FULL_SCALE;
    float amps = v_adc / (gain * CS_SQRT2 * k_ct);
    return amps;
}

static void cs_update_window(int n, bool conducting)
{
    cs_conduction_window_t *w = &s_window[n];
    uint8_t new_val = conducting ? 1u : 0u;

    if (w->filled) {
        w->sum -= w->history[w->head];
    }
    w->history[w->head] = new_val;
    w->sum += new_val;
    w->head++;
    if (w->head >= CS_POWER_WINDOW_SAMPLES) {
        w->head = 0;
        w->filled = true;
    }
}

static float cs_window_fraction(int n)
{
    const cs_conduction_window_t *w = &s_window[n];
    uint32_t denom = w->filled ? CS_POWER_WINDOW_SAMPLES : w->head;
    if (denom == 0) {
        return 0.0f;
    }
    return (float)w->sum / (float)denom;
}

void current_sense_sample(void)
{
    // Drive the 100 Hz acquisition ourselves: CURRENT_SENSE_TICKS_PER_SAMPLE
    // (5) bounded, non-blocking ticks against the existing 50 ms/20 Hz
    // publish cadence current_task.c's loop already calls us at -- see
    // current_sense.h's header comment on CURRENT_SENSE_TICKS_PER_SAMPLE for
    // why this needs no change to current_task.c, SAFTYFW_PERIOD_CURRENT_
    // TASK_MS, or any constant paired against that period.
    for (uint32_t t = 0; t < CURRENT_SENSE_TICKS_PER_SAMPLE; t++) {
        current_sense_acquire_tick();
    }

    // Drain: snapshot-and-clear the accumulator under a short critical
    // section (no arithmetic inside it), then do the divide-by-count
    // arithmetic unlocked.
    cs_accum_t local[3];
    taskENTER_CRITICAL();
    for (int n = 0; n < 3; n++) {
        local[n] = s_accum[n];
        s_accum[n].sum = 0;
        s_accum[n].count = 0;
        s_accum[n].timeouts = 0;
    }
    taskEXIT_CRITICAL();

    uint32_t counts_avg[3];
    bool degraded = false;
    for (int n = 0; n < 3; n++) {
        if (local[n].count == 0 || local[n].timeouts > 0) {
            degraded = true;
        } else {
            counts_avg[n] = (uint32_t)(local[n].sum / local[n].count);
        }
    }

    if (degraded) {
        // Never publish a partial/aborted window as complete: hold
        // s_snapshot/s_power (and their timestamp_ms) exactly as they were.
        // safety_core.c's existing snapshot_is_fresh()/CURRENT_MAX_AGE_MS
        // staleness gate ages this out on its own if degraded windows keep
        // happening -- nothing here needs to know about guards to be safe.
        if (s_degraded_window_count < UINT32_MAX) {
            s_degraded_window_count++;
        }
        return;
    }

    current_snapshot_t snap = {0};
    snap.timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    snap.calibrated = s_cal.calibrated;

    for (int n = 0; n < 3; n++) {
        // Published unconditionally, independent of calibration state --
        // see snapshots.h's counts_avg field comment. Always fits u16
        // (12-bit ADC, CS_ADC_MAX_COUNTS == 4095).
        snap.counts_avg[n] = (uint16_t)counts_avg[n];

        bool clipped = (counts_avg[n] >= CS_CLIP_THRESHOLD_COUNTS);
        float amps = cs_counts_to_amps(n, counts_avg[n]);

        // S3/S9/S11/S6b's presence fact, decoupled from k_ct_v_per_a -- see
        // current_presence_policy.h's header comment. Computed from the
        // SAME counts_avg/zero_counts this pass already has in hand, using
        // the caller-resolved gain (mirrors cs_counts_to_amps()'s own
        // "substitute CS_DEFAULT_GAIN when the cal field is <= 0" rule, so
        // the two functions never disagree about which gain a commissioned
        // channel is using).
        float resolved_gain = (s_cal.gain[n] > 0.0f) ? s_cal.gain[n] : CS_DEFAULT_GAIN;
        bool present = current_presence_is_flowing(counts_avg[n], s_cal.zero_counts[n],
                                                     s_cal.i_present_a, s_cal.k_ct_v_per_a[n],
                                                     resolved_gain);

        // Phase 9: end-to-end CT amps correction (ct_amps_cal.h), applied
        // AFTER the physics-based ADC->amps conversion above, on top of it
        // rather than instead of it -- see current_sense.h's field comment
        // on current_sense_cal_t.ct_cal for why these are two separate
        // stages. An uncommissioned channel (the compiled-in default)
        // returns `amps` completely unchanged -- see ct_amps_cal_apply()'s
        // own doc comment; this call never introduces a behavior change on
        // a channel nobody has calibrated.
        amps = ct_amps_cal_apply(&s_cal.ct_cal, (uint8_t)n, amps);

        snap.amps[n] = amps;
        snap.clipped[n] = clipped;
        snap.present[n] = present;

        // Slow reporting filter, tau=0.5s (section 4). First sample seeds
        // the filter directly rather than ramping up from 0, so a channel
        // that is already conducting when current_sense_init() runs is not
        // seen as a fake multi-second ramp.
        if (!s_filter_initialized[n]) {
            s_filtered_amps[n] = amps;
            s_filter_initialized[n] = true;
        } else {
            float alpha = CS_SAMPLE_PERIOD_S / (CS_FILTER_TAU_S + CS_SAMPLE_PERIOD_S);
            s_filtered_amps[n] += alpha * (amps - s_filtered_amps[n]);
        }

        // Section 3b: i_conducting_a is "sampled only when above
        // i_present_a" -- so it holds the last conducting reading rather
        // than decaying to a number that is neither the conducting current
        // nor a real average (the same RC-artifact problem section 3b
        // warns against for a naive average).
        bool conducting = (s_filtered_amps[n] > s_cal.i_present_a);
        if (conducting) {
            s_power.i_conducting_a[n] = s_filtered_amps[n];
        }

        cs_update_window(n, conducting);
        s_power.conduction_fraction[n] = cs_window_fraction(n);

        if (s_cal.mains_voltage_v > 0.0f) {
            s_power.p_avg_w[n] = s_cal.mains_voltage_v
                                * s_power.i_conducting_a[n]
                                * s_power.conduction_fraction[n];
        } else {
            s_power.p_avg_w[n] = NAN; // "otherwise absent" (section 3b)
        }
    }

    s_power.mains_voltage_v = (s_cal.mains_voltage_v > 0.0f) ? s_cal.mains_voltage_v : NAN;
    s_power.calibrated = s_cal.calibrated;
    s_power.any_clipped = snap.clipped[0] || snap.clipped[1] || snap.clipped[2];

    // p_total_w: NAN if any contributing channel's p_avg_w is NAN (unset
    // mains voltage propagates the same way p_avg_w[n] already does per
    // channel -- see current_sense.h's KILNLINK_POWER wire contract comment).
    float total = 0.0f;
    bool total_valid = true;
    for (int n = 0; n < 3; n++) {
        if (isnan(s_power.p_avg_w[n])) {
            total_valid = false;
            break;
        }
        total += s_power.p_avg_w[n];
    }
    s_power.p_total_w = total_valid ? total : NAN;

    // Energy accumulation: trapezoidal-ish (here: rectangular, since
    // CS_SAMPLE_PERIOD_S is short relative to any load change worth
    // resolving) integration of p_total_w over wall time, in watt-hours,
    // since current_sense_init() -- i.e. since last boot, matching
    // kilnlink_power.h's documented energy_wh contract. Frozen (does not
    // advance) whenever p_total_w is NAN this pass, per current_sense.h's
    // doc comment on this field -- never silently treated as a 0 W interval.
    if (total_valid) {
        s_power.energy_wh += (double)total * (double)CS_SAMPLE_PERIOD_S / 3600.0;
    }

    s_snapshot = snap;
    s_power.timestamp_ms = snap.timestamp_ms;
}

uint32_t current_sense_get_degraded_window_count(void)
{
    return s_degraded_window_count;
}

void current_sense_get_snapshot(current_snapshot_t *out)
{
    *out = s_snapshot;
}

void current_sense_get_power(current_sense_power_t *out)
{
    *out = s_power;
}

void current_sense_recalibrate_zero(current_sense_cal_t *cal_inout, uint16_t n_samples)
{
    if (n_samples == 0) {
        return;
    }

    uint32_t sum[3] = {0, 0, 0};
    for (uint16_t pass = 0; pass < n_samples; pass++) {
        for (int n = 0; n < 3; n++) {
            sum[n] += cs_read_channel_counts(n);
        }
    }

    for (int n = 0; n < 3; n++) {
        cal_inout->zero_counts[n] = (uint16_t)(sum[n] / n_samples);
    }
    // Deliberately does NOT set cal_inout->calibrated -- see current_sense.h.
}
