// safety_core.h -- assembles snapshots, will call safety_guards_tick() (Phase
// 4's pure evaluator, already ported in src/safety_guards.c but NOT yet wired
// in here -- that integration is separate follow-on work, not this phase's),
// and commands relay_owner.
//
// THE ONE RULE THAT MATTERS (docs/ARCHITECTURE.md section 2): this file, and
// safety_core.c, must never #include a link or uart header. Checked by
// tools/check_isolation.ps1 as well as by inspection -- see that script.
#ifndef SAFTYFW_TASKS_SAFETY_CORE_H
#define SAFTYFW_TASKS_SAFETY_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include "safety_guards.h" // safety_trip_t -- not link/uart-shaped, fine for check_isolation.ps1

#ifdef __cplusplus
extern "C" {
#endif

// Creates safety_core at SAFTYFW_PRIO_SAFETY_CORE, pinned to
// SAFTYFW_CORE_TRIP_PATH. Returns false if task creation failed.
bool safety_core_start(void);

// Telemetry pull for link_task's status frame (LINK_PROTOCOL.md's Frame A):
// "K4 energized" and "heating currently permitted". link_task.c is
// structurally forbidden from including relay_owner.h or naming the relay at
// all (tools/check_isolation.ps1), so this is the one legal channel -- a
// plain data pull through safety_core, which already legitimately depends on
// relay_owner (it commands it), not a new coupling. relay_energized comes
// from relay_owner_is_energized() (is GPIO6 actually high right now);
// heating_enabled from relay_owner_get_state() == ARMED (would a genuine
// energize command be honoured right now) -- matching safety_link.h's own
// documented bit semantics ("heating currently permitted", not "currently
// heating"). Safe to call from any task.
void safety_core_get_output_status(bool *out_relay_energized, bool *out_heating_enabled);

// Diagnostics pull for link_task's Frame B (SAFETY_CMD_DIAG,
// CommonFW/docs/LINK_PROTOCOL.md section 6, TODO.md Phase 8). Same channel
// pattern as safety_core_get_output_status() above -- link_task cannot see
// s_guard_state (safety_core.c's local static) or relay_owner's state
// directly without violating the isolation rule, so this is the legal path
// for both.
//
//   out_trip_reason: the latched guard's reason, SAFETY_TRIP_NONE if nothing
//     has tripped. Real, not degraded -- safety_guards_tick() only ever
//     tracks one reason for the whole module in this build (5 of 13 guards
//     implemented, see safety_guards.h), so there is exactly one to report.
//
//   out_warn_active: true if S5 or S12 is currently in its WARN state
//     (safety_guard_state_t.s5_warn / .s12_warn) without having tripped.
//     This is an OR of the only two guards in this build that have a WARN
//     concept at all -- it cannot say *which* guard is warning, because
//     safety_guards.c has no per-guard identity to report beyond that,
//     honesty preferred over inventing one (see link_frame.h's warn_mask
//     doc comment for how the caller turns this into the wire field).
//
//   out_diag_state: DIAG byte 24 (LINK_PROTOCOL.md: 0 init/1 grace/2 armed/
//     3 warn/4 tripped), computed here because only safety_core can see both
//     relay_owner_get_state() and the guard warn flags needed to tell
//     "ARMED" apart from "ARMED but a guard is warning" -- relay_owner_state_t
//     itself has no WARN state (INIT/GRACE/ARMED/TRIPPED only; see
//     relay_owner.h), so this function is what maps the two together rather
//     than link_task or relay_owner trying to.
//
// Any output pointer may be NULL if the caller does not need it. Safe to
// call from any task.
void safety_core_get_diag_status(safety_trip_t *out_trip_reason, bool *out_warn_active,
                                  uint8_t *out_diag_state);

// Explicit operator-acknowledged clear -- the only way out of a latched trip
// (SAFETY_MODEL.md section 2). Re-evaluates against a fresh input snapshot
// before honoring the clear (safety_guards_try_clear(), see its own doc
// comment in safety_guards.h for exactly what this does and does not catch
// -- in short, an unwindowed guard like S7/S6a/S6b's hard backstop is caught
// reliably if still active, a graduated guard like S1/S2/S3/S5/S9/S11/S12/S13
// is not guaranteed to retrip on this single retick even if its underlying
// condition persists). Returns true if the trip is now clear (or was already
// not tripped), false if the clear was refused because the guard retripped.
//
// Called from link_task_handle_clear_trip() (src/tasks/link_task.c) on a
// SAFETY_CMD_CLEAR_TRIP (0x0A) frame whose trip_mask matches the currently-
// latched trip -- link_task checks the mask match and the "nothing tripped"
// case itself (via safety_core_get_diag_status()) before calling this, so by
// the time this is reached there is a real latched trip whose mask the ESP
// echoed correctly; the only refusal left for this function to make is the
// retick-still-tripped one described above.
bool safety_core_request_clear_trip(void);

// Trip-event pull for link_task's Frame D (SAFETY_CMD_TRIP_EVENT,
// CommonFW/docs/LINK_PROTOCOL.md sec 6). Same channel pattern as
// safety_core_get_output_status()/safety_core_get_diag_status() above --
// this is the legal, isolation-respecting path for link_task to learn "a
// trip just latched" without calling into, or being called by, safety_core:
// link_task polls this on its own schedule (LINK_PROTOCOL.md sec 2: "link_task
// is a PRODUCER... never a service anything waits on" applies in reverse
// here too -- link_task pulls, safety_core never pushes into the link).
//
// Returns true iff at least one trip has occurred since safety_core_start()
// (i.e. *out_trip_seq > 0); false (with every output zeroed/NaN) before the
// first trip this boot -- link_task must not send Frame D at all in that
// case, matching the frame's "pushed immediately on trip", not on a cadence.
//
//   out_trip_seq: increments (wrapping uint8_t) once per NEWLY-tripped event
//     (safety_guards_tick()'s own "true exactly once per trip" contract) --
//     this is the wire's trip_seq / the caller's dedup key. A trip -> clear
//     -> re-trip sequence produces a new value each time.
//   out_trip_reason: the reason latched by that trip (SAFETY_TRIP_NONE only
//     when out_trip_seq == 0, i.e. before any trip).
//   out_uptime_ms: Pico uptime (pico/time.h clock, same base link_task's own
//     uptime_ms fields already use) at the instant safety_guards_tick()
//     reported newly_tripped == true.
//   out_tc_c: the safety thermocouple reading (input.tc_c, the same value
//     safety_guards_tick() was evaluating) at that instant. NaN if the
//     reading was itself invalid at trip time (e.g. an S6/S7 trip with no
//     requirement that the thermocouple be valid).
//   out_deciding_threshold: safety_guards_deciding_threshold_c()'s best-effort
//     answer for that reason -- see its own doc comment for exactly which
//     guards get a real number vs. an honest NaN.
//
// Everything else Frame D needs (current_a[], relay_recent_mask,
// context_age_100ms) is NOT sourced from here: safety_core has no current-sense
// or ESP-context data in its guard input in this build (see
// safety_guard_input_t's own field comments -- only presence booleans, never
// raw amps; context arrives at link_task, never at safety_core). link_task
// pulls those directly from current_task/its own context snapshot at the
// moment it observes a new out_trip_seq, which is an honest "at detection
// time" (within one link_task poll period, ~100ms, of the real trip instant)
// rather than the literal guard-tick instant this function's other four
// fields capture exactly.
//
// Any output pointer may be NULL if the caller does not need it. Safe to
// call from any task.
bool safety_core_get_trip_event(uint8_t *out_trip_seq, safety_trip_t *out_trip_reason,
                                 uint32_t *out_uptime_ms, float *out_tc_c,
                                 float *out_deciding_threshold);

// SAFETY_CMD_REQUEST_ENABLE (0x02), CommonFW/docs/LINK_PROTOCOL.md section 4
// -- "advisory only, the Pico's interlocks always win" (SAFETY_MODEL.md
// section 6). Called from link_task_handle_request_enable() (src/tasks/
// link_task.c), the same "link_task calls INTO safety_core, never the
// reverse" direction safety_core_request_clear_trip() above already
// established -- link_task.c is structurally forbidden from calling
// relay_owner directly (tools/check_isolation.ps1: it must never name the
// relay at all), so this is the one legal path for an ESP energize/disable
// request to reach it.
//
// A thin forward to relay_owner_command_energize(enable), not a second
// policy layer: relay_owner's own state machine already refuses while
// TRIPPED, accepts-but-never-applies during GRACE, and only actually drives
// GPIO6 high while ARMED (relay_owner.c's RELAY_OWNER_CMD_ENERGIZE case) --
// every refusal SAFETY_MODEL.md requires is already enforced there, and
// duplicating any part of that check here would create exactly the kind of
// second, potentially-diverging copy this codebase's "port it, do not
// reimplement it" discipline (ARCHITECTURE.md section 3) warns against.
//
// Returns relay_owner_command_energize()'s own result: false if the command
// queue was full (dropped) or the Pico is currently TRIPPED (refused
// synchronously); true if the command was accepted for processing (which,
// during GRACE, still does not mean GPIO6 actually went high -- see
// relay_owner_is_energized() for that).
bool safety_core_request_enable(bool enable);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_SAFETY_CORE_H
