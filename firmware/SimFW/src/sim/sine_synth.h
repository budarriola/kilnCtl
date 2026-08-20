// sine_synth -- pure, host-testable 60 Hz CT-waveform synthesis for SimFW.
// docs/PLAN.md section 3.3 ("Synthesis detail") is authoritative.
//
// A 256-entry-per-cycle sine table, sampled at an arbitrary (time, phase)
// point with linear interpolation between adjacent table entries, plus the
// per-channel distortion knobs PLAN.md 3.3 lists: DC offset, clipping, and
// half-cycle dropout (as a failing zero-cross SSR would produce). Amplitude
// changes and relay-driven on/off are supposed to apply only at zero
// crossings (PLAN.md 3.3) -- sine_synth_zero_crossing() is the detector a
// caller (wave_owner in the real firmware) uses to gate those changes; this
// module does not gate anything itself, since it has no notion of "pending
// parameter change" to gate.
#ifndef SIMFW_SIM_SINE_SYNTH_H
#define SIMFW_SIM_SINE_SYNTH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SINE_SYNTH_TABLE_LEN 256u

/* Fills table[0..SINE_SYNTH_TABLE_LEN-1] with sin(2*pi*i/256), i.e. one full
 * cycle, entry 0 = 0.0 (rising through zero), entry 64 = +1.0 (peak),
 * entry 128 = 0.0 (falling through zero), entry 192 = -1.0 (trough). */
void sine_synth_init_table(float table[SINE_SYNTH_TABLE_LEN]);

/* Per-channel amplitude, phase, and distortion configuration
 * (PLAN.md 3.3: "amplitude ... phase, plus distortion knobs -- DC offset,
 * clipping, dropout"). */
typedef struct {
    float amplitude;         /* peak amplitude, simulated amps */
    float phase_deg;          /* 0/120/240 (three-phase-ish) or 0 (in-phase, default) */

    float dc_offset;           /* distortion: added after scaling by amplitude */
    float clip_fraction;        /* distortion: 0 = no clipping; 0.2 clips the top/
                                  * bottom 20% of the amplitude off (clip level =
                                  * amplitude * (1 - clip_fraction)) */
    bool dropout_half_cycle;    /* distortion: zero out one half-cycle per period
                                  * (a failing zero-cross SSR / missing half-wave) */
    bool dropout_negative_half; /* which half is dropped when dropout_half_cycle:
                                  * false = drop the positive (sin >= 0) half,
                                  * true = drop the negative half */
} sine_channel_cfg_t;

/* Samples the table (linear interpolation between the two nearest of the
 * 256 entries) at the given absolute time_s for a channel running at
 * freq_hz with cfg->phase_deg offset, in raw (undistorted, unit-amplitude)
 * form -- i.e. sin(2*pi*freq_hz*time_s + phase_rad). Exposed separately from
 * sine_synth_sample() because zero-crossing detection (PLAN.md 3.3) must
 * operate on the *raw* reference waveform, not the possibly-clipped/
 * dropped-out one -- a real zero-cross SSR triggers off the AC line's own
 * zero crossing, not off its own already-distorted output. */
float sine_synth_raw(const float table[SINE_SYNTH_TABLE_LEN], float freq_hz, float phase_deg, float time_s);

/* Full instantaneous sample: raw waveform scaled by amplitude, then
 * dc_offset, clip_fraction, and dropout_half_cycle applied in that order. */
float sine_synth_sample(const sine_channel_cfg_t *cfg, const float table[SINE_SYNTH_TABLE_LEN],
                         float freq_hz, float time_s);

/* True if the raw reference waveform crossed zero between prev_raw and
 * curr_raw (two consecutive raw samples from sine_synth_raw(), same channel,
 * consecutive times). *out_rising is set true for a rising (negative-to-
 * non-negative) crossing, false for falling. A sample landing exactly on
 * zero is grouped with the non-negative side (only x < 0 counts as
 * "negative"), so entering zero from below is the crossing and leaving zero
 * upward afterward is not counted again -- this is what keeps a single
 * physical zero crossing from being reported twice when a sampled sequence
 * happens to contain an exact-zero sample, as sine_synth's own 256-entry
 * table does at entry 0. */
bool sine_synth_zero_crossing(float prev_raw, float curr_raw, bool *out_rising);

/* The next zero-crossing time strictly after time_s for a channel at
 * freq_hz/phase_deg -- the value a caller gates a pending parameter change
 * on (PLAN.md 3.3). Crossings occur every half period; freq_hz must be > 0. */
float sine_synth_next_zero_crossing_time(float freq_hz, float phase_deg, float time_s);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_SIM_SINE_SYNTH_H
