// ct_i2s_gen -- pure, host-testable I2S sample-block synthesis for the 3 CT
// (current transformer) channels, replacing the PWM+RC path (see
// src/drivers/ct_wave_pwm.h) now that the hardware decision is 2x UDA1334A
// I2S stereo DAC modules at 16 kHz.
//
// WHY THIS EXISTS SEPARATELY FROM sine_synth.h
// ---------------------------------------------
// sine_synth.h already does the hard synthesis work (256-entry table, linear
// interpolation, distortion knobs, zero-crossing detection on the RAW
// reference waveform) and this module builds directly on it rather than
// reimplementing any of it. What sine_synth.h does NOT do is own per-channel
// *state* across calls, or gate a pending config change at a zero crossing --
// it is explicitly stateless/pure-function (see its header: "this module
// does not gate anything itself, since it has no notion of 'pending
// parameter change' to gate"). That gating has to live somewhere, and this
// module is where.
//
// WHY THE GATE MUST BE EXPLICIT HERE (it was implicit for PWM)
// --------------------------------------------------------------
// ct_wave_pwm.h's 256-entry table is clocked at 256*60 = 15.36 kHz, so one
// full DMA pass over the table IS exactly one 60 Hz cycle -- "DMA finished
// the table" already meant "rising zero crossing", and swapping a staged
// table at that IRQ was zero-crossing-aligned for free.
//
// At 16 kHz there is no such alignment: 16000/60 = 266.67 samples/cycle, not
// an integer, so no block length is a whole number of cycles. This module
// therefore gates explicitly, per sample, using sine_synth_zero_crossing()
// against the RAW reference waveform of the channel's *currently active*
// config -- not the block boundary, not the distorted output. See
// ct_i2s_gen.c's generate_one_sample() for the exact rule.
//
// AMPLITUDE / SIGN CONVENTIONS THAT CHANGED FROM THE PWM PATH
// --------------------------------------------------------------
// - amps -> ct_cal_apply() -> a 0..1 fraction, exactly as before. The
//   fraction used to mean "PWM duty scale"; it now means "fraction of DAC
//   full-scale amplitude". The numbers are unchanged and ct_calibration.c
//   itself needed no changes -- only the destination the fraction feeds is
//   different.
// - Output is SIGNED and centred on zero. The PWM path produced a *unipolar*
//   sine riding on a ~1.65 V DC bias that an external cap stripped before
//   the transformer; an I2S DAC's output is inherently bipolar about its own
//   centre, so no bias term belongs in the samples this module produces.
//   (cfg.synth.dc_offset is a distortion knob a caller can still opt into --
//   it is not a required bias.)
//
// CHANNEL -> OUTPUT MAPPING (fixed by the hardware decision)
// --------------------------------------------------------------
// Two UDA1334A stereo modules cover 3 CT channels:
//   Module A: left = CT channel 0, right = CT channel 1.
//   Module B: left = CT channel 2, right = unused -> always written 0
//             (silence), never left uninitialised.
//
// NO DDS / PHASE ACCUMULATOR
// --------------------------------------------------------------
// sine_synth_sample()/_raw() take an absolute time_s, so this module just
// evaluates each channel at t = n / CT_I2S_GEN_SAMPLE_RATE_HZ for a running
// sample index n. The running time cursor is stored as `double`, not
// `float`: a float has ~7 decimal digits of precision, so on a long-running
// fixture (hours of continuous audio) a float cursor would eventually be
// unable to resolve individual 16 kHz sample steps (period 62.5 us) once the
// absolute time grew into the thousands of seconds, silently aliasing
// samples. A double carries ~15-16 digits, which keeps sub-microsecond
// resolution out past a year of continuous runtime.
//
// sine_synth's own API takes a float time_s, though -- so a naive cast of
// the double cursor to float right before calling it would throw the
// precision away again once t grew large. ct_i2s_gen.c's
// reduce_time_for_synth() avoids that: since sine_synth only ever needs t's
// fractional position within one mains cycle, the module reduces the double
// cursor modulo the mains period *in double* first, then casts the small
// (<1/60 s) remainder to float. That keeps the value handed to the float
// API bounded and precise no matter how long the context has been running.
#ifndef SIMFW_SIM_CT_I2S_GEN_H
#define SIMFW_SIM_CT_I2S_GEN_H

#include <stdbool.h>
#include <stdint.h>

#include "ct_calibration.h"
#include "sine_synth.h"

#ifdef __cplusplus
extern "C" {
#endif

// Must equal tasks/wave_owner.h's CT_WAVE_NUM_CHANNELS and
// ct_calibration.h's CT_CAL_NUM_CHANNELS. Not #included from wave_owner.h:
// that header is under src/tasks/ and everything under src/sim/ must stay
// pure (tools/check_sim_purity.ps1) -- ct_i2s_gen.c carries a compile-time
// assert that this agrees with CT_CAL_NUM_CHANNELS, and the driver that
// wires this module up is responsible for asserting it against
// CT_WAVE_NUM_CHANNELS too.
#define CT_I2S_GEN_NUM_CHANNELS 3u

// The mains reference frequency every CT channel is synthesized against
// (DESIGN_NOTES.md 3.3: 60 Hz).
#define CT_I2S_GEN_MAINS_FREQ_HZ 60.0f

// The sample rate the UDA1334A modules run at (the hardware decision this
// module exists for).
#define CT_I2S_GEN_SAMPLE_RATE_HZ 16000.0

// A reference block size a driver can build its DMA buffers around. Not
// enforced by this module -- ct_i2s_gen_fill_block() accepts any num_frames
// -- but named so no caller has to invent its own magic number.
#define CT_I2S_GEN_FRAMES_PER_BLOCK 128u

// Mirrors wave_owner.h's ct_wave_mode_t vocabulary (MODEL tracks the thermal
// model every tick, MANUAL freezes at the last commanded amps). Carried here
// for API-shape parity only -- this module does not consult it. Resolving
// MODEL vs MANUAL into a concrete `amps` value is the caller's (wave_owner's)
// job, same as it is today for the PWM path; by the time a config reaches
// ct_i2s_gen_stage_config(), `amps` is already the resolved number.
typedef enum {
    CT_I2S_GEN_MODE_MODEL = 0,
    CT_I2S_GEN_MODE_MANUAL = 1,
} ct_i2s_gen_mode_t;

// Per-channel configuration. Reuses sine_channel_cfg_t wholesale for
// phase_deg and the distortion knobs (dc_offset/clip_fraction/
// dropout_half_cycle/dropout_negative_half) rather than duplicating those
// fields -- see sine_synth.h for their exact contract.
//
// `synth.amplitude` is NOT read from the config a caller stages: this
// module overwrites it internally, every sample, with
// ct_cal_apply(cal_table, channel, amps) (0..1, "fraction of DAC full-scale
// amplitude" -- see this header's top comment). Callers may leave it at 0;
// whatever is set there is ignored.
typedef struct {
    ct_i2s_gen_mode_t mode;   // API parity only (see above); not consulted here
    float amps;                // simulated amps, PRE-calibration
    sine_channel_cfg_t synth;  // .amplitude ignored/overwritten; phase_deg + distortion used as-is
    bool apply_immediately;    // false (default): gate at this channel's next zero crossing of the
                                 // RAW reference waveform; true: step on the very next sample
                                 // (wave_owner's existing escape hatch, DESIGN_NOTES.md 3.3's
                                 // "step-in-mid-cycle is itself a selectable distortion")
} ct_i2s_gen_channel_cfg_t;

typedef struct {
    ct_i2s_gen_channel_cfg_t active;   // currently in effect
    ct_i2s_gen_channel_cfg_t staged;   // pending, meaningful only if staged_pending
    bool staged_pending;
    float prev_raw;                    // previous RAW sample of `active`'s waveform, for gating
    bool prev_raw_valid;               // false until at least one sample has been generated
} ct_i2s_gen_channel_state_t;

// Caller-owned context -- no globals, no dynamic allocation. Zero-initialize
// or pass through ct_i2s_gen_init() before first use.
typedef struct {
    float sine_table[SINE_SYNTH_TABLE_LEN];
    const ct_cal_table_t *cal_table;  // may be NULL -> ct_cal_apply()'s identity fallback
    double time_s;                     // running absolute time cursor, see header comment on precision
    ct_i2s_gen_channel_state_t channels[CT_I2S_GEN_NUM_CHANNELS];
} ct_i2s_gen_ctx_t;

// Initializes the sine table, zeroes the time cursor, and sets every channel
// to an inert default (MODEL mode, 0 amps, 0 phase, no distortion,
// apply_immediately false, no pending staged change). `cal_table` may be
// NULL (identity calibration on every channel, ct_calibration.h's contract);
// it is stored by pointer, not copied, and must outlive `ctx`.
void ct_i2s_gen_init(ct_i2s_gen_ctx_t *ctx, const ct_cal_table_t *cal_table);

// Stages a new config for `channel`, replacing any previously-staged (not
// yet applied) config. Takes effect per `cfg->apply_immediately`: on the
// very next generated sample if true, or at that channel's next zero
// crossing of the RAW reference waveform if false (see this header's top
// comment for why that gate must be explicit at 16 kHz). Returns false
// (no-op) for an out-of-range channel.
bool ct_i2s_gen_stage_config(ct_i2s_gen_ctx_t *ctx, uint8_t channel, const ct_i2s_gen_channel_cfg_t *cfg);

// Fills `num_frames` interleaved stereo int16 frames into each of the two
// module buffers, continuing exactly where the previous call left off (the
// context's time cursor and every channel's gating state persist across
// calls -- no discontinuity, no re-derived/drifting phase at the seam).
//
//   module_a_frames[2*n + 0] = channel 0 (left),  module_a_frames[2*n + 1] = channel 1 (right)
//   module_b_frames[2*n + 0] = channel 2 (left),  module_b_frames[2*n + 1] = 0 (right, unused, always silence)
//
// Both buffers must have room for num_frames*2 int16 values. Every sample is
// saturated to [INT16_MIN, INT16_MAX] -- it never wraps.
void ct_i2s_gen_fill_block(ct_i2s_gen_ctx_t *ctx, int16_t *module_a_frames, int16_t *module_b_frames,
                            uint32_t num_frames);

// Read-only accessors for callers (and tests) that need to inspect state
// without racing the gate: the currently-active config for `channel`, and
// whether a staged change is still waiting for its gate. Both return false
// (out left unmodified) for an out-of-range channel.
bool ct_i2s_gen_get_active_config(const ct_i2s_gen_ctx_t *ctx, uint8_t channel, ct_i2s_gen_channel_cfg_t *out);
bool ct_i2s_gen_has_pending_change(const ct_i2s_gen_ctx_t *ctx, uint8_t channel, bool *out_pending);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_SIM_CT_I2S_GEN_H
