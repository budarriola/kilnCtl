// debounce_policy.h -- pure consecutive-sample debounce, pulled out of
// discrete_task.c's static debounce_update() the same way discrete_pin_
// policy.h was pulled out of the same file (see that header's own comment
// for the precedent this follows). No pico-sdk/FreeRTOS/hardware includes,
// so this is host-testable directly (test/test_debounce_policy.c and
// test/test_guard_nuisance.c link this .c file alone) even though
// discrete_task.c itself cannot be (it calls hardware/gpio.h's gpio_get()).
//
// Why this exists (2026-09-04, closing GUARD_TEST_MATRIX.md's S6a/S7 gap):
// discrete_task.c debounces both discrete inputs -- E-stop (50ms window,
// S7) and mainFault (200ms window, S6a) -- with a static consecutive-sample
// debounce function, `debounce_update()`, called only from
// `discrete_task_fn()`, which is gated on FreeRTOS + RP2040 GPIO headers.
// The function was never declared in a header and never referenced from
// anywhere under test/, so GUARD_TEST_MATRIX.md section 1's S6a ("mainFault
// glitching for 100ms must not trip") and S7 ("30ms of contact bounce must
// not trip") rows had no host-reachable entry point to exercise at all --
// `safety_guards_tick()` only ever receives an already-debounced level, so
// nothing at that boundary can prove the debounce itself rejects a glitch
// shorter than its window.
//
// This module is a behaviour-preserving extraction, not a redesign: the
// state struct and the update logic below are copied verbatim from
// discrete_task.c's debounce_state_t/debounce_update() (a standard
// consecutive-sample debounce -- a new raw value is only published once it
// has been seen `n_samples` times in a row; any disagreement resets the
// streak against the new value, so a single noisy sample cannot "borrow"
// progress from an unrelated earlier streak). discrete_task.c now calls
// this function instead of its own local copy.
#ifndef SAFTYFW_DEBOUNCE_POLICY_H
#define SAFTYFW_DEBOUNCE_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     candidate;
    uint32_t streak;
    bool     published;
} debounce_policy_state_t;

// Advances the debounce by one sample. `raw` is the newest raw reading;
// `n_samples` is the number of consecutive agreeing samples required before
// `published` changes (computed by the caller, e.g.
// SAFTYFW_DEBOUNCE_SAMPLES() in discrete_task.c, from a time window and the
// caller's own sample period -- this module has no notion of time, only
// sample counts, exactly like the code it was extracted from). Returns the
// (possibly unchanged) published value, same as the extracted original.
bool debounce_policy_update(debounce_policy_state_t *db, bool raw, uint32_t n_samples);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_DEBOUNCE_POLICY_H
