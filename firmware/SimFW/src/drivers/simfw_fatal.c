// simfw_fatal.c -- see simfw_fatal.h for the design rationale.
#include "simfw_fatal.h"

#include <stdarg.h>
#include <stdio.h>

#include "hardware/irq.h"
#include "hardware/sync.h" // save_and_disable_interrupts()
#include "hardware/timer.h" // hardware_alarm_* -- see "Cross-core halt" below
#include "pico/platform.h" // get_core_num()
#include "pico/stdlib.h" // gpio_*, busy_wait_ms, panic()

// --- Cross-core halt ---------------------------------------------------------
// See simfw_fatal.h's "CROSS-CORE HALT" comment for the why.
//
// MECHANISM (2026-08-23 rewrite): a dedicated RP2040 hardware timer alarm,
// forced to fire on demand rather than ever actually counting down. This
// replaces an earlier version that pushed a sentinel word over the SIO
// inter-core FIFO and caught it with an irq_set_exclusive_handler() on
// SIO_IRQ_PROC0 -- verified (against FreeRTOS-Kernel's RP2040 SMP port,
// portable/ThirdParty/GCC/RP2040/port.c) to be fatally broken on every boot,
// for two independent reasons:
//
//   1. IRQ-vector collision, unconditional. xPortStartSchedulerOnCore()
//      (and the configNUMBER_OF_CORES==1 fallback in xPortStartScheduler())
//      calls irq_set_exclusive_handler(SIO_IRQ_PROC0 + get_core_num(),
//      prvFIFOInterruptHandler) on EVERY core when vTaskStartScheduler()
//      runs -- i.e. strictly AFTER main() has already installed this file's
//      own exclusive handler on that same vector. pico-sdk's
//      irq_set_exclusive_handler() (hardware_irq/irq.c) does
//      `hard_assert(current == __unhandled_user_irq || current == handler)`,
//      so two different exclusive handlers on one vector is a guaranteed
//      panic, not a race. Switching this file to irq_add_shared_handler()
//      does not fix it either: FreeRTOS's install always calls the
//      *exclusive* function, and that same hard_assert() fires just as
//      surely when the vtable slot already holds a shared chain-slot
//      handler as when it holds a foreign exclusive one (irq.c's
//      irq_add_shared_handler(), by contrast, only tolerates
//      __unhandled_user_irq or an existing chain slot -- exclusive-vs-shared
//      is not symmetric, and neither order of this particular pairing
//      survives).
//
//   2. Wrong hardware to share, independent of (1). The SIO FIFO is
//      FreeRTOS's OWN inter-core doorbell in the SMP port, not idle hardware
//      this file could borrow: port.c pushes raw 0-words directly via
//      `sio_hw->fifo_wr = 0` (bypassing pico_multicore entirely) to wake the
//      other core for scheduling events, and prvFIFOInterruptHandler()
//      unconditionally calls multicore_fifo_drain() -- it never inspects
//      FIFO contents. Even a working handler-sharing scheme would have let
//      FreeRTOS's own drain silently eat this file's sentinel word (or vice
//      versa), so the cross-core halt could simply fail to fire with
//      nothing to flag it. Sharing this FIFO at all was the wrong design,
//      separate from the IRQ-registration conflict.
//
// A hardware timer alarm has neither problem. SimFW claims one
// (hardware_alarm_claim_unused(), required = true so an unexpected
// collision panics loudly instead of silently reusing someone else's alarm)
// that nothing else in this codebase or in FreeRTOS's SMP port touches --
// FreeRTOS's own tick source is each core's PRIVATE SysTick exception, not
// the shared RP2040 timer/alarm block (vPortSetupTimerInterrupt(), same
// port.c). Core 0 installs an ordinary exclusive callback for it via
// hardware_alarm_set_callback() (still one-directional, core-1-signals-
// core-0 only, matching the original design and simfw_fatal.h's own note
// that the mirror direction is unimplemented); core 1's simfw_fatal() calls
// hardware_alarm_force_irq() to fire it on demand instead of waiting for any
// real countdown. force_irq() only asserts the interrupt on cores that have
// it enabled in their OWN per-core NVIC -- RP2040's shared peripheral IRQs
// are wired to both cores, but each core's enable bit and vtable slot are
// independent (PICO_VTABLE_PER_CORE) -- so because only core 0 ever calls
// simfw_fatal_install_cross_core_halt(), forcing the alarm from core 1
// reaches core 0 and nothing else. There is no shared mutable state between
// the two cores for this signal to race or be dropped from: force_irq()'s
// own bookkeeping is protected by pico-sdk's PICO_SPINLOCK_ID_TIMER spinlock
// internally (hardware_timer/timer.c), and nothing else in this codebase
// calls any hardware_alarm_* function (grepped), so there is no other
// traffic on this alarm to collide with or be mistaken for.
#define SIMFW_FATAL_ALARM_NUM_UNSET (-1)
static int g_simfw_fatal_alarm_num = SIMFW_FATAL_ALARM_NUM_UNSET;

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
static void __not_in_flash_func(cross_core_halt_alarm_callback)(uint alarm_num)
{
    (void)alarm_num;
    save_and_disable_interrupts();
    for (;;) {
        tight_loop_contents();
    }
}

void simfw_fatal_install_cross_core_halt(void)
{
    g_simfw_fatal_alarm_num = hardware_alarm_claim_unused(true);
    hardware_alarm_set_callback((uint)g_simfw_fatal_alarm_num, cross_core_halt_alarm_callback);
}

// Onboard LED, RP2040 Pico's own GPIO25 -- docs/HARDWARE.md section 1 lists
// it as "Heartbeat LED (on-board, not a header pin) | *(none yet, DESIGN_NOTES.md
// 3.6)*". No heartbeat body has ever landed, so nothing else in src/ claims
// this pin; this file is its first owner. A fast blink burst followed by a
// solid-on hold is the one signal available to a bench operator with zero
// tooling attached -- no debugger and no USB terminal (stdio_usb is
// disabled, CMakeLists.txt, and TinyUSB owns the one USB peripheral
// outright). stdio_uart, by contrast, IS enabled as of 2026-08-23
// (pico_enable_stdio_uart(SimFW 1), UART0 on GP16/GP17) -- but it only helps
// if a probe is actually wired to those pins, which is exactly the case this
// LED exists to cover. During the bench debug pass that added it, the
// attached probe's UART delivered nothing at all and the LED was the only
// working channel, so do not weaken this on the grounds that a console now
// exists. If a real heartbeat task is ever
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

    // UART trace FIRST, before anything else in this function -- this is the
    // single most valuable message the whole fixture can produce (bench
    // debug pass, 2026-08-23: a dark LED and no USB left a real stack
    // overflow completely undiagnosable, see vApplicationStackOverflowHook()'s
    // comment in main.c). printf() here goes out over UART0 (pico_stdio_uart,
    // CMakeLists.txt) if stdio_init_all() has already run; if this fires
    // before that (main.c calls it before stdio_init_all() only in no code
    // path today, but nothing here assumes otherwise), the call is a no-op,
    // not a hang -- pico-sdk's stdio plumbing tolerates printf() before
    // stdio_init_all() by simply dropping the output. Flushed explicitly
    // since stdout may be buffered even though uart_write_blocking()
    // underneath is not.
    printf("\r\n*** SimFW FATAL [%s]: %s ***\r\n", subsystem, reason);
    fflush(stdout);

    // Signal the other core NEXT (the UART trace above is the only thing
    // that runs before it), before spending ~2 s on the blink burst below --
    // so a core-1 fatal starts halting core 0 immediately rather than after
    // this core's own LED sequence finishes. Only meaningful, and only
    // safe, when THIS core is core 1: core 0 is the only core that ever
    // calls simfw_fatal_install_cross_core_halt() (main(), before
    // vTaskStartScheduler()), so a core-0-originated fatal forcing the alarm
    // onto ITSELF would immediately re-enter this same IRQ vector on top of
    // whatever this function is doing -- pointless (core 0 already halts via
    // panic() below regardless) and hazardous (a halt-loop nested inside
    // this function's own still-running blink/panic sequence, with
    // interrupts disabled by the callback and never returning). Skipping it
    // on core 0 preserves the pre-scheduler case exactly as before (ct_wave_i2s.c's
    // claim, like the now-deleted ct_wave_pwm.c's before it, runs
    // pre-scheduler on core 0 alone, before core 1 has been launched by the
    // FreeRTOS SMP port): there is no core 1 to signal yet, and this core
    // halts via panic() below regardless.
    if (get_core_num() != 0 && g_simfw_fatal_alarm_num != SIMFW_FATAL_ALARM_NUM_UNSET) {
        hardware_alarm_force_irq((uint)g_simfw_fatal_alarm_num);
    }

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

    // stdio_usb is disabled and stdio_uart reaches a console only when a
    // probe is wired to GP16/GP17, so panic()'s own printf often reaches
    // nobody -- called anyway for the two things it still buys: a
    // debugger attached over SWD sees this exact formatted message, and it
    // is the SAME halt mechanism pio_claim_unused_sm(pio, true) already uses
    // for PIO state-machine exhaustion (docs/HARDWARE.md section 1b.5's
    // "asymmetry" note) -- one fatal-halt convention for the whole fixture,
    // not two.
    panic("SimFW FATAL [%s]: %s\n", subsystem, reason);
}
