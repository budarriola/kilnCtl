// simfw_fatal.h -- shared "this cannot be a silent failure" halt path.
//
// docs/HARDWARE.md section 1b.5's DMA-budget audit found that every
// dma_claim_unused_channel() call site in the fixture passed required =
// false and, on exhaustion, just returned false up a chain main.c discards
// with (void) -- a fixture that boots, enumerates over USB, and answers
// commands while quietly having no CT output or one dead SPI bus. The
// neighbouring pio_claim_unused_sm(pio, true) calls in the same functions
// already panic on exhaustion (required = true); this file gives the DMA
// call sites (drivers/ct_wave_pwm.c, drivers/max31856_pio_engine.c) the
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
// sequence, e.g. ct_wave_pwm_init()'s DMA claims) and from inside a running
// FreeRTOS task on either core (e.g. max31856_pio_engine_init(), called from
// spi_emu_a.c/spi_emu_b.c's task bodies, which run AFTER the scheduler has
// started). One thing it does NOT guarantee in the second case: pico-sdk's
// panic() only halts the CALLING core. SimFW's core split
// (task_priorities.h: SIMFW_CORE_RT_PATH = core 1 for spi_emu_a/b and
// wave_owner, SIMFW_CORE_ELASTIC_PATH = core 0 for usb_owner/telemetry/
// cmd_task/etc.) means a DMA exhaustion inside spi_emu_a/b halts core 1
// while core 0 keeps scheduling -- USB stays enumerated and other
// subsystems keep answering. What IS guaranteed: the affected bus's own
// state stops changing forever (transaction counters frozen, reads/writes
// to it fail every time, the onboard LED goes solid), so it cannot be
// mistaken for a live, healthy bus -- it can only ever be mistaken for one
// that a human has not yet looked at. Escalating to a true whole-board halt
// from a single core would need cross-core signalling (or a watchdog) this
// pass did not add; see the caller's own report for that as a follow-up.
void simfw_fatal(const char *subsystem, const char *reason_fmt, ...)
    __attribute__((noreturn, format(printf, 2, 3)));

#endif // SIMFW_FATAL_H
