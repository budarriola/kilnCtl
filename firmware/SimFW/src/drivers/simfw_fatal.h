// simfw_fatal.h -- shared "this cannot be a silent failure" halt path.
//
// docs/HARDWARE.md section 1b.5's DMA-budget audit found that every
// dma_claim_unused_channel() call site in the fixture passed required =
// false and, on exhaustion, just returned false up a chain main.c discards
// with (void) -- a fixture that boots, enumerates over USB, and answers
// commands while quietly having no CT output or one dead SPI bus. The
// neighbouring pio_claim_unused_sm(pio, true) calls in the same functions
// already panic on exhaustion (required = true); this file gives the DMA
// call sites (drivers/ct_wave_i2s.c -- formerly drivers/ct_wave_pwm.c,
// deleted 2026-08-23 -- and drivers/max31856_pio_engine.c) the
// same halt-on-exhaustion posture, through one shared, nameable entry point
// instead of four separate ad hoc panic() calls that could drift in wording
// or, worse, quietly turn back into a `return false` under future editing.
//
// Not a peripheral driver -- nothing here claims a DMA/PIO/PWM/I2C
// resource, so it is exempt from docs/DESIGN_NOTES.md section 4's
// single-owner-per-peripheral doctrine the same way src/sim/ is exempt for
// being pure math. It DOES claim one GPIO (the onboard LED, GPIO25 --
// docs/HARDWARE.md section 1's row for it says "owner file: none yet"), so
// this file is that pin's first owner; see simfw_fatal.c's header comment.
#ifndef SIMFW_FATAL_H
#define SIMFW_FATAL_H

// Halts the fixture and reports which subsystem failed and why, in a form a
// bench operator can see with ZERO tooling attached (stdio_uart and
// stdio_usb are both disabled -- CMakeLists.txt -- so this cannot rely on a
// console) as well as in a form a debugger session gets via panic()'s own
// message. NEVER RETURNS.
//
// Safe to call both before vTaskStartScheduler() (main.c's own startup
// sequence, e.g. ct_wave_i2s_init()'s PIO/DMA claims) and from inside a running
// FreeRTOS task on either core (e.g. max31856_pio_engine_init(), called from
// spi_emu_a.c/spi_emu_b.c's task bodies, which run AFTER the scheduler has
// started).
//
// CROSS-CORE HALT: pico-sdk's panic() only halts the CALLING core, and
// SimFW's core split (task_priorities.h: SIMFW_CORE_RT_PATH = core 1 for
// spi_emu_a/b and wave_owner, SIMFW_CORE_ELASTIC_PATH = core 0 for
// usb_owner/telemetry/cmd_task/etc.) means a fatal raised from a core-1 task
// body would, by that alone, leave core 0 scheduling -- USB stays
// enumerated, telemetry keeps sending, cmd_task keeps answering, all while
// the affected bus is permanently dead. For a fixture whose entire value is
// being trustworthy at a bench, "give partial, stale answers forever" is a
// worse failure mode than "go dark", so this function forces a dedicated RP2040
// hardware timer alarm before entering its own halt (simfw_fatal.c has the
// full mechanism and why it is NOT the SIO inter-core FIFO/IRQ -- that
// collides with, and was actively drained by, FreeRTOS-Kernel's own RP2040
// SMP port); simfw_fatal_install_cross_core_halt() (called once from
// main(), core 0, before vTaskStartScheduler()) installs the alarm callback
// that receives it and halts core 0 too -- interrupts disabled, spin
// forever, no scheduler tick, no task runs again. The net effect: ANY
// simfw_fatal() call, on either core, takes the WHOLE board down, not just
// the calling core. See simfw_fatal.c for why a true halt was chosen over a
// "core 0 refuses to report healthy" flag: the LED this function already
// drives solid-on is the bench-visible "do not trust this fixture" signal; a
// half-alive board that still answers protocol traffic (even truthfully)
// undermines that signal more than it helps a remote operator, and a bench
// operator relies on the LED, not on parsing telemetry, in the no-debugger
// case this file exists for. Only the core-1-originated direction is wired
// up (no core-0-side caller exists today -- ct_wave_i2s.c's claim, like the
// now-deleted ct_wave_pwm.c's before it, runs pre-scheduler, so a fatal
// there halts core 0 before core 1 even starts): a future simfw_fatal() call
// from a core-0 task body would need the mirror (a second alarm, or the same
// one with core 1 also installing a callback for it, from code that
// actually runs on core 1) added at that time.
void simfw_fatal(const char *subsystem, const char *reason_fmt, ...)
    __attribute__((noreturn, format(printf, 2, 3)));

// Installs the hardware-timer-alarm callback that lets simfw_fatal() halt
// core 0 in response to a fatal raised on core 1 -- see simfw_fatal()'s
// cross-core-halt comment above, and simfw_fatal.c for why this claims a
// dedicated RP2040 alarm instead of the SIO inter-core FIFO/IRQ. MUST be
// called from core 0, before vTaskStartScheduler() (main.c does this once,
// early in main()): hardware_alarm_set_callback() installs into whichever
// core's own vtable calls it, and core 0 is the only core running at that
// point in boot.
void simfw_fatal_install_cross_core_halt(void);

#endif // SIMFW_FATAL_H
