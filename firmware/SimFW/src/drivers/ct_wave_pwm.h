// ct_wave_pwm -- low-level RP2040 PWM+DMA driver for the 3 CT sine-wave
// channels (docs/PLAN.md section 3.3 "Synthesis detail"). wave_owner.c is
// this driver's sole caller/owner (PLAN.md section 4's single-owner-per-
// peripheral doctrine) -- nothing else may touch these PWM slices or DMA
// channels.
//
// Design (see ct_wave_pwm.c for the full arithmetic derivation):
//   - One dedicated, GPIO-less "pacer" PWM slice free-runs at the 15.36 kHz
//     sample rate PLAN.md 3.3 specifies (256-entry table x 60 Hz); its wrap
//     DREQ paces one DMA channel per CT channel.
//   - Each CT channel gets its own PWM slice (~244 kHz carrier, 8-bit
//     resolution) driving its own GPIO, and its own DMA channel that walks a
//     256-entry uint16_t duty-level table (one full 60 Hz cycle) into that
//     slice's channel-A compare register, one 16-bit halfword per pacer
//     tick.
//   - The DMA channel's transfer count is exactly 256 (one table = one
//     cycle); completion fires a shared IRQ (DMA_IRQ_1, chosen to stay clear
//     of DMA_IRQ_0, the conventional first choice the parallel PIO-SPI-
//     engine agent's spi_emu_a/b is most likely to claim -- see this
//     driver's .c file header for the collision-check note). Because table
//     index 0 is an exact zero (sine_synth_init_table()'s contract) and one
//     table = one full period, "DMA just finished the table and is about to
//     restart it" IS a rising zero crossing -- so restarting the DMA
//     transfer in that IRQ is inherently zero-crossing-gated: any table
//     content swapped in right there lands exactly at a zero crossing, with
//     no separate zero-crossing polling needed in the steady-state path.
//     ct_wave_pwm_load_table()'s `apply_immediately` argument is the
//     explicit escape hatch for PLAN.md 3.3's "unless a distortion knob says
//     otherwise -- step-in-mid-cycle is itself a selectable distortion".
#ifndef SIMFW_DRIVERS_CT_WAVE_PWM_H
#define SIMFW_DRIVERS_CT_WAVE_PWM_H

#include <stdbool.h>
#include <stdint.h>

#include "sim/sine_synth.h" // SINE_SYNTH_TABLE_LEN -- one ct_wave_pwm table = one sine_synth table's worth of samples

#ifdef __cplusplus
extern "C" {
#endif

#define CT_WAVE_PWM_NUM_CHANNELS 3u

// Claims the 3 CT GPIOs + their PWM slices, the pacer slice, 3 DMA channels,
// and installs the shared DMA_IRQ_1 handler. Must be called exactly once,
// from wave_owner's task, before the first ct_wave_pwm_load_table() call.
// Returns false if a PWM slice or DMA channel could not be claimed (e.g.
// already claimed by another module -- see the collision-check note in the
// .c file).
bool ct_wave_pwm_init(void);

// Loads a new 256-entry duty-level table (each entry 0..255, 8-bit PWM
// compare value) for `channel` (0..CT_WAVE_PWM_NUM_CHANNELS-1).
//
// apply_immediately == false (the default path): the table is staged and
// swapped in by the DMA-completion IRQ the next time that channel's DMA
// finishes its current 256-sample pass -- i.e. at the next rising zero
// crossing (see this header's top comment). This is PLAN.md 3.3's normal
// "amplitude changes ... apply at zero crossings only" behavior.
//
// apply_immediately == true: the running DMA transfer is aborted and the new
// table takes effect at whatever sample the carrier is on right now -- a
// deliberate mid-cycle step, PLAN.md 3.3's "step-in-mid-cycle is itself a
// selectable distortion" path. Callers only set this when a distortion
// config explicitly asks for it.
//
// Safe to call from wave_owner's own task (never from an ISR).
void ct_wave_pwm_load_table(uint8_t channel, const uint16_t levels[SINE_SYNTH_TABLE_LEN],
                             bool apply_immediately);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_DRIVERS_CT_WAVE_PWM_H
