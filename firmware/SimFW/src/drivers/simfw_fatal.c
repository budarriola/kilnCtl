// simfw_fatal.c -- see simfw_fatal.h for the design rationale.
#include "simfw_fatal.h"

#include <stdarg.h>
#include <stdio.h>

#include "pico/stdlib.h" // gpio_*, busy_wait_ms, panic()

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
