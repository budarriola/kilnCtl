// ct_wave_i2s.h -- PIO+DMA I2S MASTER transport for 2x UDA1334A stereo DAC
// modules. This REPLACES the PWM+RC current-transformer CT waveform path
// (formerly ct_wave_pwm.{c,h}, deleted 2026-08-23 per docs/DESIGN_NOTES.md
// section 3.3's decision) as wave_owner's CT synthesis backend -- decided,
// not re-litigated here. wave_owner_start() now calls ct_wave_i2s_init()
// unconditionally; see this header's PIO PROGRAM MEMORY note below for why
// that call is EXPECTED TO FAIL on the current build regardless.
//
// SINGLE-OWNER DOCTRINE (docs/DESIGN_NOTES.md section 4): this is the ONLY
// file in the tree that may touch PIO1's 2 free state machines (the ones
// max31856_pio_engine.c's bus B leaves idle -- docs/HARDWARE.md section
// 1b.6 note 7), the 2 DMA channels it claims, or GPIO27/28/16/18 as
// configured below. See ct_wave_i2s.c's header for the GPIO/PIO/DMA
// resource-conflict details this doctrine has to live with on THIS
// fixture, which are real and unresolved, not oversights -- read that
// header before assuming this driver "just works" once wired up.
//
// PURE TRANSPORT, NOT A WAVEFORM GENERATOR. This file does not know what a
// sine wave, a CT channel, or an amplitude is -- src/sim/ is exempt from
// this file's reach entirely, on purpose, mirroring ct_wave_pwm.h's
// table-in/PWM-out split. The caller supplies already-computed int16
// samples through the refill callback below; this driver's only job is
// clocking them out over BCLK/WS/DATA at the right rate.
//
// SEAM CHOICE: DOUBLE-BUFFERED POLL-AND-FILL, NOT AN IRQ CALLBACK. Two
// reasons, both load-bearing, not just style preference:
//
//   1. NO DMA IRQ VECTOR IS AVAILABLE. docs/HARDWARE.md section 1b: DMA_IRQ_0
//      is owned by max31856_pio_engine.c (both SPI buses share it),
//      DMA_IRQ_1 by ct_wave_pwm.c. tools/check_single_owner.ps1's DMA SAFETY
//      RULES section hard-fails the build's own reasoning if a DMA vector
//      gets a SECOND irq_set_exclusive_handler() call site, and
//      irq_set_exclusive_handler() itself PANICS AT BOOT for the same
//      reason -- so a third handler on either vector is not just
//      undesirable, it is a guaranteed boot-time crash the instant this
//      driver is ever wired up to run. Rather than fight either existing
//      owner for a vector neither can give up, this driver simply never
//      installs a DMA IRQ handler at all.
//   2. TASK-CONTEXT SAFETY IS TRIVIAL; IRQ-CONTEXT SAFETY IS NOT. A
//      poll-and-fill design lets ct_wave_i2s_poll() call the caller's
//      refill callback from ordinary FreeRTOS task context: it may block,
//      take a mutex, call into src/sim/, log, whatever a normal task may
//      do. An IRQ-invoked callback would inherit every constraint
//      ct_wave_pwm.c's own DMA_IRQ_1 handler lives under (no FreeRTOS
//      primitives, __not_in_flash_func, minimal work, `noreturn`-free
//      short critical sections) for zero benefit here, since there was
//      never a spare vector to hang it on regardless.
//
// Tradeoff, stated plainly: the caller is responsible for calling
// ct_wave_i2s_poll() often enough. Each module's buffer holds
// CT_WAVE_I2S_FRAMES_PER_BUFFER stereo frames, i.e.
// CT_WAVE_I2S_FRAMES_PER_BUFFER / CT_WAVE_I2S_SAMPLE_RATE_HZ seconds of
// audio; poll() must run at least that often, with margin, or that
// module's PIO state machine stalls waiting on its TX FIFO's autopull --
// see ct_wave_i2s.c's header for what a stall does to module B specifically
// (it is worse than a click).
//
// DMA BUDGET, NOW THAT ct_wave_pwm.c IS GONE. docs/HARDWARE.md section 1b:
// with the PWM backend deleted, bus A (5) + bus B (3) = 8 of the RP2040's 12
// DMA channels are budgeted before this driver claims its 2 (one per
// module), leaving 2 spare after this driver runs -- comfortable headroom,
// unlike the PWM-era 1-channel margin. DMA is not what blocks this driver
// today; see the PIO program-memory conflict below for what does.
//
// THE CONFLICT THAT ACTUALLY BLOCKS THIS DRIVER TODAY: PIO1
// PROGRAM MEMORY, NOT JUST STATE-MACHINE COUNT. docs/HARDWARE.md section
// 1b's "PIO1 has 2 SMs free" framing describes state-machine COUNT --
// program memory is a separate, per-PIO-BLOCK 32-instruction-word budget
// shared by all 4 SMs in that block, and it is NOT free. Bus B
// (max31856_pio_engine.c, PIO1) already loads its shared RX program (12
// instructions) and its TX_b program (17 instructions) = 29 of PIO1's 32
// words, leaving 3 free. This driver's ct_wave_i2s_out program
// (ct_wave_i2s.pio) is 8 instructions -- it CANNOT fit in the 3 words PIO1
// actually has free, regardless of 2 of its 4 state machines sitting idle.
// ct_wave_i2s_init() checks this with pio_can_add_program() (never the
// panicking pio_add_program() alone) and calls simfw_fatal() with the exact
// word counts on failure, the same posture as every other resource claim in
// this file -- but unlike the DMA conflict above, THIS ONE WILL FIRE ON THE
// CURRENT BUILD THE MOMENT ANYTHING CALLS ct_wave_i2s_init(), independent
// of boot ordering or which backend ran first. Freeing this needs a real
// decision (shrink max31856_spi_slave.pio's TX_b program, or find PIO1 room
// some other way) that is out of this file's scope; flagged here and in
// this task's report for whoever owns that reconciliation.
#ifndef SIMFW_DRIVERS_CT_WAVE_I2S_H
#define SIMFW_DRIVERS_CT_WAVE_I2S_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Module 0 ("A"): CT channel 0 = left, CT channel 1 = right.
// Module 1 ("B"): CT channel 2 = left, silence = right (caller's choice --
// this driver has no opinion on what fills a channel; see file header).
#define CT_WAVE_I2S_NUM_MODULES 2u

#define CT_WAVE_I2S_SAMPLE_RATE_HZ 16000u // UDA1334ATS datasheet p3: "16 to 100 kHz" -- this is exactly the supported minimum, do not go lower (see ct_wave_i2s.c header for the derived BCLK/clkdiv arithmetic)

// Stereo frames per half-buffer. 128 frames / 16 kHz = 8 ms of audio per
// buffer -- ct_wave_i2s_poll() must be called at least that often (with
// margin; see ct_wave_i2s_get_underrun_count()'s comment) or a module's SM
// stalls waiting on its TX FIFO's autopull.
#define CT_WAVE_I2S_FRAMES_PER_BUFFER 128u
#define CT_WAVE_I2S_SAMPLES_PER_BUFFER (CT_WAVE_I2S_FRAMES_PER_BUFFER * 2u) // interleaved L,R int16 per frame

// Fills `frame_count` stereo frames (frame_count*2 int16 samples,
// interleaved L,R -- this driver's seam contract, see file header) into
// `out` for `module` (0 = A, 1 = B). Called only from ct_wave_i2s_poll()'s
// own task context, per this header's SEAM CHOICE comment -- safe to block,
// take a mutex, or do anything else a normal FreeRTOS task may do. NEVER
// called from an ISR: this driver does not use one.
typedef void (*ct_wave_i2s_refill_fn)(uint8_t module, int16_t *out,
                                       uint32_t frame_count, void *user_ctx);

// Claims 2 PIO1 state machines (running the same program instance, started
// in lock-step via pio_enable_sm_mask_in_sync() so BCLK/WS and module B's
// own bit/frame counter never drift relative to each other), 2 DMA channels
// (one per module), and GPIO27 (BCLK), GPIO28 (WS), GPIO16 (DIN_A), GPIO18
// (DIN_B) -- ALL FOUR PROVISIONAL, see ct_wave_i2s.c's header for the
// pin-budget conflict this creates against docs/HARDWARE.md section 1.
// Pre-fills both buffers of both modules via `refill` before starting
// playback, so the first frame out is never silence-by-omission or
// uninitialised memory.
//
// Returns false only if `refill` is NULL. Every hardware resource claim
// failure (DMA exhaustion, PIO SM exhaustion, PIO1 program-memory
// exhaustion) is routed through simfw_fatal() instead, per this project's
// DMA/PIO exhaustion doctrine (docs/HARDWARE.md section 1b.5) -- see this
// header's and ct_wave_i2s.c's top comments for exactly which failure mode
// is EXPECTED to fire on the current build (the PIO-program-memory conflict
// is real today, not hypothetical; the DMA budget is not what blocks this
// driver any more, now that ct_wave_pwm.c is gone).
//
// wave_owner_start() calls this unconditionally and treats any false return
// (and every simfw_fatal() path below) as the fixture having no working CT
// output -- there is no fallback backend to fall back to.
bool ct_wave_i2s_init(ct_wave_i2s_refill_fn refill, void *user_ctx);

// Call at least once per CT_WAVE_I2S_FRAMES_PER_BUFFER worth of playback
// time (see this header's SEAM CHOICE comment for the margin needed), from
// ordinary task context. For each module whose currently-playing DMA
// transfer has finished, refills the now-idle buffer via the callback
// passed to ct_wave_i2s_init() and re-arms that module's DMA channel to
// play it, swapping which buffer is "live" (double buffering).
//
// Safe to call from any one task, but only ever from ONE task -- this
// driver keeps no internal lock, matching ct_wave_pwm_load_table()'s
// "caller's own task, never an ISR, never more than one caller" contract.
void ct_wave_i2s_poll(void);

// Count of times ct_wave_i2s_poll() found `module`'s DMA channel idle for
// MORE than roughly 2 buffer-periods since it was last armed -- i.e. poll()
// was called too late and that module's PIO state machine almost certainly
// stalled waiting on its TX FIFO's autopull. See ct_wave_i2s.c's header for
// what a module-B (the "follower" SM that never drives BCLK/WS itself)
// underrun does to its WS/DATA alignment -- it can desync module B's
// left/right mapping until the whole peripheral is stopped and restarted
// together, and this driver does NOT attempt that recovery automatically
// today. Returns 0 for an out-of-range `module`.
uint32_t ct_wave_i2s_get_underrun_count(uint8_t module);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_DRIVERS_CT_WAVE_I2S_H
