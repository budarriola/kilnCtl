// relay_grace.h -- pure state-transition decisions factored out of
// relay_owner_task()'s GRACE/TRIP handling, so this session's guard-test-
// matrix pass (commit 9a6d3e9) can host-test what was previously only
// reachable inside a FreeRTOS task function.
//
// Deliberately free of FreeRTOS/pico-sdk, same discipline link_frame.h/
// tx_watermark.h already follow: every function here is a pure transform on
// caller-supplied values, buildable and testable on the host
// (firmware/SaftyFW/test/build_host_tests.ps1). relay_owner.c is the only
// production caller -- it gathers the FreeRTOS-sourced inputs (tick counts)
// and drives GPIO6/watchdog checkins itself; these functions only decide
// what the new state should be.
#ifndef SAFTYFW_TASKS_RELAY_GRACE_H
#define SAFTYFW_TASKS_RELAY_GRACE_H

#include <stdint.h>

#include "relay_owner.h" // relay_owner_state_t -- pure enum, no RTOS/SDK dependency itself

#ifdef __cplusplus
extern "C" {
#endif

// GRACE -> ARMED, timer-driven. Mirrors relay_owner_task()'s own loop body
// exactly:
//   if (s_state == RELAY_OWNER_STATE_GRACE) {
//       if ((xTaskGetTickCount() - grace_start) >= pdMS_TO_TICKS(GRACE_MS)) {
//           s_state = RELAY_OWNER_STATE_ARMED;
//       }
//   }
// `elapsed_ticks`/`grace_ticks` are already-computed tick counts (the caller
// does the xTaskGetTickCount()/pdMS_TO_TICKS() conversions, since those are
// RTOS calls this file must not make) -- this function only applies the
// comparison. Any state other than GRACE is returned unchanged: this is
// deliberately not a general "tick the whole state machine" function, only
// the one GRACE-timeout transition relay_owner_task actually performs here.
//
// Boundary is inclusive (>=), exactly as the original code: elapsed_ticks
// EQUAL to grace_ticks already counts as elapsed, not one tick later. The
// subtraction is unsigned and wraps the same way TickType_t arithmetic does
// on a real xTaskGetTickCount() rollover -- this function does not change
// that, only makes it callable without a real tick source, per this pass's
// "no behavior change" rule.
relay_owner_state_t relay_grace_tick(relay_owner_state_t state, uint32_t elapsed_ticks,
                                      uint32_t grace_ticks);

// TRIPPED latch, unconditional regardless of the incoming state -- mirrors
// relay_owner_task()'s RELAY_OWNER_CMD_TRIP case body exactly (`s_state =
// RELAY_OWNER_STATE_TRIPPED;`, no guard on the previous state at all: a trip
// during INIT/GRACE/ARMED/an already-TRIPPED state all land on TRIPPED). This
// is what makes the "trip during GRACE latches" test case exercisable: this
// function returns TRIPPED for every input, on purpose -- the original code
// really is that unconditional, not a bug to fix here (this pass preserves
// behavior, it does not silently change it even though a "some states can't
// trip further" guard might look more defensive).
relay_owner_state_t relay_trip_transition(relay_owner_state_t state);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_RELAY_GRACE_H
