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

// TRIPPED -> GRACE-or-ARMED, on a clear. 2026-08-27 audit fix: a bare
// unconditional TRIPPED -> ARMED (the previous behaviour, still visible in
// relay_owner_clear_trip()'s own doc comment history) let a trip asserted
// and cleared partway through the 60s startup GRACE window jump straight to
// ARMED with the remaining grace simply discarded -- an E-stop asserted and
// cleared 10s into a boot left the board ARMED at t=10s instead of t=60s,
// with the S3/S4/etc rolling windows relay_owner_is_energized()/S9 depend on
// still empty. A clear during GRACE must land back in GRACE, not ARMED.
//
// DELIBERATE CHOICE, spelled out because there are two plausible shapes and
// they behave very differently under repeated trip/clear cycling:
//   (a) grant a FRESH full grace_ticks window on every clear, or
//   (b) resume the ORIGINAL boot-relative window, unmoved by the trip/clear.
// This function implements (b). `elapsed_ticks`/`grace_ticks` are computed
// from THE SAME boot-relative clock relay_grace_tick() already uses (the
// caller passes xTaskGetTickCount() minus the one grace_start captured at
// task entry -- see relay_owner.c -- never a clock restarted at the clear),
// so nothing here resets it. (a) was rejected on purpose: an adversary or a
// flaky sensor that can assert-then-clear a trip on a ~1s cadence would
// otherwise be able to hold the board in GRACE (never-armed) indefinitely by
// repeating that cycle forever, which is strictly worse than the bug this
// fix closes -- GRACE refuses every energize, so "stuck in GRACE forever"
// reads as safe locally but is actually an availability attack against a
// kiln that may need to hold temperature. Under (b), each clear can only
// ever land in GRACE if the ORIGINAL boot-relative window has not yet
// elapsed, so the state machine provably reaches ARMED (or TRIPPED again, on
// a real fault) no later than t = grace_ticks after boot, regardless of how
// many trip/clear cycles happen before then. Any state other than TRIPPED is
// returned unchanged, matching relay_trip_transition()'s own convention --
// this is not a general "tick the whole machine" function either.
relay_owner_state_t relay_clear_trip_transition(relay_owner_state_t state, uint32_t elapsed_ticks,
                                                  uint32_t grace_ticks);

// The Pico's OWN half of the mutual "heating is not allowed during updates"
// interlock (ROADMAP.md M8): whether a request to newly ENERGIZE the relay
// should be honoured given whether an update transfer is currently active on
// THIS processor (update_task.c's s_transfer_active, read via
// update_task_transfer_active()). Pure boolean, not a relay_owner_state_t
// transition -- this is an ADDITIONAL precondition safety_core_request_
// enable() applies before it ever calls relay_owner_command_energize(), not
// a replacement for relay_owner's own ARMED/GRACE/TRIPPED gate, so it does
// not belong in relay_owner_task()'s state machine itself.
//
// Never applies to de-energizing: callers must only consult this when
// `enable` (the caller's own request) is true, matching this codebase's
// "turning OFF is never gated" rule (relay_authority.h on the KilnFW side
// states the identical rule for the same reason -- the safe direction must
// always be reachable).
bool relay_energize_allowed_during_update(bool update_in_progress);

// Pure decision for safety_core_task()'s trip-command retry loop (2026-08-27
// audit fix item 2: a dropped relay_owner_command_trip() must not silently
// fail to open K4). safety_core_task() latches "a trip command is owed"
// (s_trip_command_owed) the tick a trip is decided, then every tick after
// that -- while still owed -- attempts relay_owner_command_trip() again and
// calls this function with the outcome to decide whether the flag should
// stay set:
//   - `is_tripped` false: the guard state has already gone back to "not
//     tripped" (a CLEAR_TRIP landed while the very first send was still
//     stuck behind a full queue). Nothing was ever actually sent in that
//     case -- see safety_core.c's own comment at the call site for why
//     re-sending a now-stale trip command after a legitimate clear would be
//     exactly backwards -- so the flag is dropped (false) regardless of
//     `send_succeeded`.
//   - `is_tripped` true: still owed exactly when the just-attempted send
//     did NOT succeed (`!send_succeeded`) -- a successful send clears it, a
//     dropped one keeps it latched so the very next tick tries again.
// The caller (safety_core.c) is responsible for only attempting the send
// (and thus having a meaningful `send_succeeded` to pass) while `is_tripped`
// is true -- this function makes the right call either way, but skipping
// the doomed send when not tripped avoids calling relay_owner_command_trip()
// on a state relay_owner would refuse to trip anyway.
bool relay_trip_command_still_owed(bool is_tripped, bool send_succeeded);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_RELAY_GRACE_H
