// wave_owner.h -- single owner of the CT waveform PIO state machines + DMA
// (docs/DESIGN_NOTES.md section 4.1 task map): "60 Hz synthesis, amplitude/
// phase/distortion updates at zero-crossings." Drives the three CT sine-wave
// channels (section 3.3) into J13/J15/J17 via isolation transformers, through
// src/drivers/ct_wave_i2s.h (this task's private driver -- nothing else may
// touch those PIO1 state machines or DMA channels, single-owner-per-
// peripheral doctrine, DESIGN_NOTES.md section 4's opening paragraph) and
// src/sim/ct_i2s_gen.h (the pure sample generator that driver's refill
// callback pulls from). Formerly drove src/drivers/ct_wave_pwm.h's PWM+RC
// path; that backend is retired (deleted 2026-08-23, DESIGN_NOTES.md section
// 3.3's PWM->I2S decision) and this header's public API below is unchanged
// across that switch -- cmd_task.c and other callers needed no changes.
//
// Public API below mirrors i2c_owner.h's queue-then-apply-next-tick
// contract: every setter posts a command to wave_owner's own queue and
// returns immediately (never blocks the hard-real-time core-1 task, DESIGN_NOTES.md
// section 4.5's "nothing ever blocks ... wave_owner -- a full queue toward
// them is a counted drop plus event, never a stall"). This is also the
// eventual backing for the future CT command group (DESIGN_NOTES.md section 5's
// CT_SET_MODE / CT_SET_AMPS / CT_SET_DISTORTION / CT_GET_STATE), which
// cmd_task.c will wire up once the USB protocol lands -- not this pass's
// scope, but the API shape here is written to match that sketch directly.
#ifndef SIMFW_TASKS_WAVE_OWNER_H
#define SIMFW_TASKS_WAVE_OWNER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// DESIGN_NOTES.md 3.6: "CT sine PWM x3". Channel index i corresponds to
// sim_snapshot_t.zones[i] (sim/sim_snapshot.h) -- only the first
// CT_WAVE_NUM_CHANNELS of SIM_SNAPSHOT_MAX_ZONES zones have a CT channel;
// DESIGN_NOTES.md 3.1's optional 4th zone is explicitly "a zone with no dedicated
// CT/TC".
#define CT_WAVE_NUM_CHANNELS 3u

// Every mutable CT signal has a mode (DESIGN_NOTES.md section 5's MODEL/MANUAL
// doctrine): MODEL tracks sim_snapshot_t.zones[channel].current_a every
// tick; MANUAL freezes at the last ct_wave_set_amps() value.
typedef enum {
    CT_WAVE_MODE_MODEL = 0,
    CT_WAVE_MODE_MANUAL = 1,
} ct_wave_mode_t;

// Distortion knobs, DESIGN_NOTES.md 3.3: "DC offset, clipping, dropout". Mirrors
// sine_synth.h's sine_channel_cfg_t distortion fields exactly (this struct
// is what gets copied into a sine_channel_cfg_t each recompute) plus one
// field sine_synth.h intentionally has no notion of: apply_immediately,
// DESIGN_NOTES.md 3.3's "unless a distortion knob says otherwise -- step-in-mid-
// cycle is itself a selectable distortion" -- see ct_wave_pwm.h's top
// comment for how that maps onto the DMA table swap.
typedef struct {
    float dc_offset;
    float clip_fraction;
    bool dropout_half_cycle;
    bool dropout_negative_half;
    bool apply_immediately; // false (default): gate at the next zero crossing; true: step now
} ct_wave_distortion_t;

// Read-only snapshot for CT_GET_STATE (DESIGN_NOTES.md section 5), i2c_owner.h's
// i2c_owner_relay_states_t-style plain struct returned by value.
typedef struct {
    ct_wave_mode_t mode;
    float amps;                     // MANUAL target, or last-read MODEL amps
    float phase_deg;
    ct_wave_distortion_t distortion;
    float last_pwm_scale;            // last value ct_wave_amps_to_pwm_scale() returned, 0..1
    bool valid;                      // false until wave_owner_start() has run at least one tick
} ct_wave_channel_state_t;

// Creates wave_owner at SIMFW_PRIO_WAVE_OWNER, pinned to SIMFW_CORE_RT_PATH
// (task_priorities.h) -- core 1, the hard-real-time producers' core.
// Initializes ct_wave_i2s (PIO1 state machines + DMA) before the task loop
// starts. There is no fallback CT backend: if ct_wave_i2s_init() cannot
// claim its resources, wave_owner_start() halts the fixture via
// simfw_fatal() rather than returning with a silently-dead CT path (see
// wave_owner.c's header and ct_wave_i2s.h for why that failure is EXPECTED
// on the current build -- PIO1 program-memory exhaustion, docs/HARDWARE.md
// section 1b.7). Otherwise returns false only if task creation itself
// failed.
bool wave_owner_start(void);

// MODEL (default) tracks the thermal model's current_a for this channel
// every tick; MANUAL freezes at the last ct_wave_set_amps() value. Applied
// (like every other change here) at the next zero crossing unless the
// channel's distortion.apply_immediately is set.
bool ct_wave_set_mode(uint8_t channel, ct_wave_mode_t mode);

// Only meaningful in MANUAL mode (DESIGN_NOTES.md section 5: "CT_SET_AMPS"); a
// pending MANUAL amps value is still stored while in MODEL mode (so
// switching to MANUAL later starts from something sane), but has no effect
// on the output until the channel is actually in MANUAL mode.
bool ct_wave_set_amps(uint8_t channel, float amps);

// Per-channel phase offset (DESIGN_NOTES.md 3.3: "0/120/240 (three-phase-ish) or 0
// (in-phase, default)" -- selectable, not restricted to those three values).
bool ct_wave_set_phase(uint8_t channel, float phase_deg);

// Replaces channel's distortion config wholesale (DESIGN_NOTES.md section 5:
// "CT_SET_DISTORTION"). Pass a zeroed/false struct to clear all distortion.
bool ct_wave_set_distortion(uint8_t channel, const ct_wave_distortion_t *distortion);

// DESIGN_NOTES.md section 5: "CT_GET_STATE". Returns false (out left unmodified) for
// an out-of-range channel.
bool ct_wave_get_state(uint8_t channel, ct_wave_channel_state_t *out);

// Calibration hook (DESIGN_NOTES.md 3.3's "amplitude (in simulated amps, fixture
// converts via calibration table)"). Converts a target current in simulated
// amps into a DAC full-scale amplitude fraction (0..1, 0 = silent/centre,
// 1 = maximum swing the UDA1334A's output allows) -- NAME IS A HOLDOVER from
// when this fed a PWM duty cycle instead (retired 2026-08-23, DESIGN_NOTES.md
// section 3.3's PWM->I2S decision); kept because existing callers depend on
// it. See wave_owner.c's header comment on this function for a rename
// suggestion for whoever next touches this API.
//
// Implemented as clamp(gain[channel] * amps + offset[channel], 0, 1) against
// the compiled-in per-channel table in sim/ct_calibration.h -- the arithmetic
// DESIGN_NOTES.md 3.3 and tools/ct_calibration/README.md's "Remaining firmware work"
// section specify. src/sim/ct_i2s_gen.c applies this exact same mapping
// again, internally, every sample; this function is not itself in that data
// path -- it exists for ct_wave_get_state()'s last_pwm_scale field and for
// any external caller that wants the mapping without generating a sample.
//
// M-D IS NOT CLOSED. No CT hardware exists on this bench and no calibration
// run has ever been taken, so the compiled-in table
// (sim/ct_calibration_defaults.h, generated) marks every channel
// UNCALIBRATED, and an uncalibrated channel falls back to exact IDENTITY:
// `amps` is treated directly as a 0..1 PWM-scale fraction (clamped), i.e.
// "1.0 simulated amp" == full-scale swing -- not a real current unit yet,
// exactly as before. What this function gained is the *ability* to apply a
// calibration once a real bench run produces one (regenerate the table with
// tools/gen_ct_cal_table.py); the constants themselves stay hardware-gated.
float ct_wave_amps_to_pwm_scale(uint8_t channel, float amps);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_TASKS_WAVE_OWNER_H
