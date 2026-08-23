// simfw_fatal.c -- see simfw_fatal.h for the design rationale.
#include "simfw_fatal.h"

#include <stdarg.h>
#include <stdio.h>

#include "hardware/irq.h"
#include "hardware/sync.h" // save_and_disable_interrupts()
#include "pico/multicore.h"
#include "pico/stdlib.h" // gpio_*, busy_wait_ms, panic()

// --- Cross-core halt ---------------------------------------------------------
// See simfw_fatal.h's "CROSS-CORE HALT" comment for the why. Mechanism: the
// SIO peripheral gives each core a 4-deep hardware FIFO to the OTHER core,
// with its own per-core IRQ (SIO_IRQ_PROC0 fires on core 0 when core 1
// pushes, SIO_IRQ_PROC1 the reverse) -- pico_multicore wraps it. A single
// magic word, pushed once per fatal, is all this needs: there is no reply,
// no payload beyond "halt now", and the FIFO is otherwise unused anywhere in
// this codebase (grepped: no other multicore_fifo_* call exists), so there
// is no other traffic this could collide with or be mistaken for.
#define SIMFW_FATAL_FIFO_SENTINEL 0xFA7A1004u // "FATAL" + core-independent tag; arbitrary but distinctive in a debugger's memory/register view

// Runs in IRQ context on core 0 ONLY (bound by simfw_fatal_install_cross_core_halt()
// being called from core 0 -- see simfw_fatal.h). Deliberately does NOT call
// simfw_fatal() itself: that would re-run the LED blink sequence and race
// core 1's own gpio_put() calls on the same GPIO25 (a real hazard -- both
// cores would be mid-sequence on the same pin with no lock between them),
// and panic()'s printf plumbing is not documented safe to re-enter from IRQ
// context on top of whatever core 1 was doing when it panicked. All this
// needs to guarantee is "core 0 never runs another scheduler tick or
// services another task", which a bare disable-and-spin gives directly and
// unconditionally.
static void __not_in_flash_func(sio_fifo_irq_handler_core0)(void)
{
    while (multicore_fifo_rvalid()) {
        uint32_t word = multicore_fifo_pop_blocking();
        if (word == SIMFW_FATAL_FIFO_SENTINEL) {
            save_and_disable_interrupts();
            for (;;) {
                tight_loop_contents();
            }
        }
        // Any other word is unexpected (nothing else uses this FIFO) but not
        // this handler's problem to diagnose -- drop it and keep draining so
        // a spurious/garbage word can never mask the real sentinel behind it.
    }
    multicore_fifo_clear_irq();
}

void simfw_fatal_install_cross_core_halt(void)
{
    irq_set_exclusive_handler(SIO_IRQ_PROC0, sio_fifo_irq_handler_core0);
    irq_set_enabled(SIO_IRQ_PROC0, true);
}

// Onboard LED, RP2040 Pico's own GPIO25 -- docs/HARDWARE.md section 1 lists
// it as "Heartbeat LED (on-board, not a header pin) | *(none yet, DESIGN_NOTES.md
// 3.6)*". No heartbeat body has ever landed, so nothing else in src/ claims
// this pin; this file is its first owner. A fast blink burst followed by a
// solid-on hold is the one signal available to a bench operator with zero
// tooling attached -- no debugger, no USB terminal (stdio_usb is disabled,
// CMakeLists.txt, and TinyUSB owns the one USB peripheral outright), no
// serial port (stdio_uart is disabled too). If a real heartbeat task is ever
// added, it must yield this pin the instant simfw_fatal() is entered rather
// than fight it for control -- a live heartbeat blink on a dead fixture
// would defeat the entire point of this file.
#define SIMFW_FATAL_LED_GPIO 25u

#define SIMFW_FATAL_BLINK_COUNT  10u
#define SIMFW_FATAL_BLINK_ON_MS  100u
#define SIMFW_FATAL_BLINK_OFF_MS 100u

void simfw_fatal(const char *subsystem, const char *reason_fmt, ...)
{
    char reason[128];
    va_list ap;
    va_start(ap, reason_fmt);
    vsnprintf(reason, sizeof(reason), reason_fmt, ap);
    va_end(ap);

    // Signal the other core FIRST, before spending ~2 s on the blink burst
    // below -- so a core-1 fatal starts halting core 0 immediately rather
    // than after this core's own LED sequence finishes. Harmless when there
    // is no other core to receive it yet (ct_wave_i2s.c's claim, like the
    // now-deleted ct_wave_pwm.c's before it, runs pre-scheduler on core 0
    // alone, before core 1 has been launched by the
    // FreeRTOS SMP port): the word just sits in core 1's inbox unread, and
    // this core halts via panic() below regardless. multicore_fifo_push_blocking()
    // cannot deadlock here in practice -- the SIO FIFO is 4 words deep, this
    // is the only call site in the whole codebase that ever pushes to it
    // (grepped), and a fatal fires at most once per boot.
    multicore_fifo_push_blocking(SIMFW_FATAL_FIFO_SENTINEL);

    // Bench-visible signal first, independent of any tooling: fast blink
    // burst, then solid on. Runs even if the format/vsnprintf above somehow
    // produced garbage -- the LED does not depend on `reason` at all.
    gpio_init(SIMFW_FATAL_LED_GPIO);
    gpio_set_dir(SIMFW_FATAL_LED_GPIO, GPIO_OUT);
    for (uint32_t i = 0; i < SIMFW_FATAL_BLINK_COUNT; i++) {
        gpio_put(SIMFW_FATAL_LED_GPIO, 1);
        busy_wait_ms(SIMFW_FATAL_BLINK_ON_MS);
        gpio_put(SIMFW_FATAL_LED_GPIO, 0);
        busy_wait_ms(SIMFW_FATAL_BLINK_OFF_MS);
    }
    gpio_put(SIMFW_FATAL_LED_GPIO, 1); // solid on: the final at-a-glance "this fixture is dead, do not trust it" state

    // stdio_uart/stdio_usb are both disabled, so panic()'s own printf reaches
    // no console -- called anyway for the two things it still buys: a
    // debugger attached over SWD sees this exact formatted message, and it
    // is the SAME halt mechanism pio_claim_unused_sm(pio, true) already uses
    // for PIO state-machine exhaustion (docs/HARDWARE.md section 1b.5's
    // "asymmetry" note) -- one fatal-halt convention for the whole fixture,
    // not two.
    panic("SimFW FATAL [%s]: %s\n", subsystem, reason);
}
