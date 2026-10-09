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
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// E-stop polarity selector -- config_store_record_t::estop_active_level,
// SET_PARAM 0x0212. 0 is the default in every direction (compiled default,
// erased flash, legacy record) and is the fail-safe polarity; see
// discrete_pin_policy_estop_asserted_ex() below.
#define DISCRETE_PIN_POLICY_ESTOP_ACTIVE_HIGH 0u
#define DISCRETE_PIN_POLICY_ESTOP_ACTIVE_LOW  1u

// GPIO9, E-stop. `gpio9_high` is the raw `gpio_get(SAFTYFW_PIN_ESTOP)`
// reading. Returns true (E-stop asserted / STOP) whenever the pin reads
// HIGH -- this covers three electrically-identical cases by design
// (docs/HARDWARE.md section 5): the button genuinely pressed, the
// normally-closed loop's wire cut, and nothing fitted to the connector at
// all. Returns false (healthy) only when the pin reads LOW, i.e. the
// normally-closed contact is intact and closed. NO inversion -- do not add
// a `!` here; see this header's own comment for exactly why that is wrong.
bool discrete_pin_policy_estop_asserted(bool gpio9_high);

// Configurable-polarity form of the above (owner decision 2026-09-08:
// "Estop polarity should be configureable but the state it is in now on my
// test setup should be considered the default and the prefered safe to fire
// state"). `active_level` is one of DISCRETE_PIN_POLICY_ESTOP_ACTIVE_*.
//
//   ACTIVE_HIGH (0, THE DEFAULT, and what this bench is wired for -- GPIO9
//     measured LOW 2026-09-08, i.e. healthy/safe-to-fire):
//     asserted == pin HIGH. This is the FAIL-SAFE choice and the only one
//     that satisfies "a lost E-stop signal is itself a trip": R10's 1k
//     pull-up means a cut wire, a pulled connector and an unfitted switch
//     all float the pin HIGH and are therefore indistinguishable from a
//     pressed button -- all of them STOP.
//
//   ACTIVE_LOW (1): asserted == pin LOW. Provided for an installation wired
//     the other way round. **This polarity CANNOT detect a lost signal.**
//     With the pull-up still fitted, a broken line reads HIGH, which under
//     this setting is the healthy state -- a cut cable is indistinguishable
//     from a working, un-pressed E-stop. Choosing it trades the
//     broken-wire detection away, and nothing in firmware can win it back;
//     only a pull-DOWN on the board would move which failure is detectable.
//     Documented in docs/HARDWARE.md section 5.1 so the asymmetry is visible
//     at wiring time, which is the only moment it can be acted on.
//
// Any value other than DISCRETE_PIN_POLICY_ESTOP_ACTIVE_LOW is treated as
// ACTIVE_HIGH. That is deliberate and matches config_store's decode: a
// legacy record's 0x00, erased flash's 0xFF, and any byte a future/older
// firmware might leave here all land on the fail-safe polarity rather than
// the one that cannot see a broken wire. An unreadable configuration must
// never be the thing that selects the less safe option.
bool discrete_pin_policy_estop_asserted_ex(bool gpio9_high, uint8_t active_level);

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
