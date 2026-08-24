// discrete_pin_policy.h -- pure raw-GPIO-level-to-logical-meaning mapping
// for the two discrete inputs discrete_task.c samples, split out the same
// way max31856_tc_type_policy.h is split out of max31856.c (see that
// header's own comment): no pico-sdk/FreeRTOS/hardware includes, so this is
// host-testable directly (test/test_discrete_pin_policy.c links this .c
// file alone) even though discrete_task.c itself cannot be (it calls
// hardware/gpio.h's gpio_get()).
//
// Why this exists (2026-08-24, the E-stop polarity fix): discrete_task.c
// used to read `bool estop_raw = !gpio_get(SAFTYFW_PIN_ESTOP);` under an
// "active low" comment. GPIO9 is actually ACTIVE HIGH for stop
// (docs/HARDWARE.md section 5: R10 1k pull-up to 3.3v_Safty, normally-
// CLOSED contact to GND_Safty -- LOW = contact closed = healthy, HIGH =
// open = pressed, OR a cut wire, OR nothing fitted at all = STOP, and all
// three read identically on purpose). The inversion made a real E-stop
// press, a cut cable, and an unwired board all read HEALTHY -- silently
// disabling S7 in the exact direction the normally-closed wiring exists to
// prevent -- while a correctly-jumpered healthy board latched S7
// permanently and could never arm. GUARD_TEST_MATRIX.md's own S7 rows
// recorded `estop_at_boot`/`estop_midfire` PASSING for the entire period
// this was broken, because those scenarios feed a synthesized
// `estop_pressed` boolean directly into `safety_guards.c` and never
// exercise a GPIO read at all -- nothing in the existing suite could have
// caught a polarity bug at the hardware boundary.
//
// This module pins that boundary down as two named, independently testable
// functions instead of one bare `!`/no-`!` choice buried in discrete_task.c
// -- specifically so a future edit that "simplifies" one pin's sense cannot
// silently make it match the other's. The two pins do NOT share a polarity
// (discrete_task.h's own header comment says so explicitly, having once
// wrongly claimed they did -- that wrong claim is exactly how this bug
// happened), and a test asserting only one pin's correctness would not have
// caught a regression that swapped which pin got the `!`.
#ifndef SAFTYFW_DISCRETE_PIN_POLICY_H
#define SAFTYFW_DISCRETE_PIN_POLICY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// GPIO9, E-stop. `gpio9_high` is the raw `gpio_get(SAFTYFW_PIN_ESTOP)`
// reading. Returns true (E-stop asserted / STOP) whenever the pin reads
// HIGH -- this covers three electrically-identical cases by design
// (docs/HARDWARE.md section 5): the button genuinely pressed, the
// normally-closed loop's wire cut, and nothing fitted to the connector at
// all. Returns false (healthy) only when the pin reads LOW, i.e. the
// normally-closed contact is intact and closed. NO inversion -- do not add
// a `!` here; see this header's own comment for exactly why that is wrong.
bool discrete_pin_policy_estop_asserted(bool gpio9_high);

// GPIO10, mainFault. `gpio10_high` is the raw `gpio_get(SAFTYFW_PIN_MAIN_
// FAULT)` reading. Returns true (mainFault asserted) whenever the pin reads
// LOW -- U1's open-collector output pulls the line low when it asserts;
// R8's pull-up holds it high otherwise (docs/HARDWARE.md, board_pins.h's
// own "active low" comment on SAFTYFW_PIN_MAIN_FAULT). This is the OPPOSITE
// polarity from discrete_pin_policy_estop_asserted() above -- deliberately
// two separate functions, not one parameterised by an "active_low" bool,
// so the two pins' senses cannot be accidentally unified the way discrete_
// task.h's comment once wrongly claimed they already were.
bool discrete_pin_policy_main_fault_asserted(bool gpio10_high);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_DISCRETE_PIN_POLICY_H
