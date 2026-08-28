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
// (SAFETY_MODEL.md section 2).
//
// ASYNC as of the 2026-08-23 reboot-on-clear-trip investigation. This used
// to call safety_guards_try_clear() synchronously, in whichever task's
// context called it -- which in practice meant core 0's link_task (see
// link_task_handle_clear_trip()) mutating core 1's s_guard_state
// (safety_core.c's own, single-writer-by-design state, normally touched only
// by safety_core_task's own 100ms tick) with no lock. A hardware run
// reproduced a watchdog reboot specifically on this path, and only when a
// trip was actually latched -- exactly the branch that used to touch
// s_guard_state at all; the "nothing tripped" early-return never wrote
// anything and never reproduced. That symmetry, plus the total absence of
// any lock around a struct safety_core_task ticks every 100ms on the other
// core, is why this was changed to a queue rather than kept synchronous: it
// restores s_guard_state to single-writer, matching the pattern
// relay_owner.c already uses for exactly this reason (a command queue only
// the owning task drains).
//
// This is very likely what caused the reboot, but is not the only plausible
// mechanism the investigation flagged, and the fix does not require having
// picked the right one: the same call site also placed a
// safety_guard_input_t (130+ bytes) plus trip()'s vararg vsnprintf() into
// detail[96] onto link_task's stack, a stack that has already had to be
// raised once before for a real overflow (link_task.c's
// LINK_TASK_STACK_WORDS, configMINIMAL_STACK_SIZE*3 -> *6). Moving this work
// onto safety_core_task's own stack removes that frame from link_task
// entirely, so the fix is correct either way. See
// safety_core_get_clear_trip_stats() below for the counters that will tell
// us which mechanism it actually was if a reboot still happens.
//
// Now only enqueues a request; safety_core_task (safety_core.c) dequeues it
// and runs safety_guards_try_clear() on its own tick, on its own stack, on
// the one task/core allowed to write s_guard_state at all. This matches
// CommonFW/docs/LINK_PROTOCOL.md's own documented CLEAR_TRIP contract --
// "refused, with the reason reported in the next diagnostic frame" -- which
// was already async on the wire; only this function's internal
// implementation was (wrongly) synchronous. The return value reflects that:
// true means "queued for safety_core to process", never "cleared" -- this
// function always enqueues (link_task has already screened out the
// "nothing tripped" case before ever calling it); the actual accept/refuse
// decision, including the case where is_tripped has since gone false, and
// its log line, now happen inside safety_core_task once it dequeues the
// request (see safety_guards_decide_clear_trip_outcome(), safety_guards.h).
//
// Returns false only if the queue was full (safety_core_task did not drain
// a previous request in time before this one arrived) -- a dropped request
// is not retried here; same fire-and-forget contract as every other
// non-ACKed command in this protocol, and link_task_handle_clear_trip()
// logs the drop.
bool safety_core_request_clear_trip(void);

// Outcome of the most recently PROCESSED CLEAR_TRIP request (see
// safety_core_get_clear_trip_stats() below) -- NONE until at least one has
// been dequeued and run. safety_clear_trip_outcome_t itself lives in
// safety_guards.h (this header already includes it), classified by the
// pure, host-tested safety_guards_decide_clear_trip_outcome() -- see that
// function's own doc comment for why it is there and not here.
//
// Diagnostics for the CLEAR_TRIP queue above -- added alongside the
// queue-based fix so a hardware run is diagnostic rather than another guess
// (this investigation already burned two wrong diagnoses on inference
// alone). Distinguishes "link_task never received/forwarded the frame"
// (link_task.c has its own received-frame counter, logged there) from
// "safety_core never dequeued it" from "try_clear ran and refused", without
// needing a debugger session.
//   out_requested: count of successful xQueueSend()s from
//     safety_core_request_clear_trip() -- how many requests reached this
//     queue.
//   out_processed: how many of those safety_core_task has actually dequeued
//     and resolved (accepted or refused, either way).
//   out_last_outcome: the most recently processed one's result;
//     SAFETY_CLEAR_TRIP_OUTCOME_NONE if out_processed == 0.
// A live request/processed gap after a reboot (requested > processed) means
// safety_core_task itself stopped running before it could dequeue -- the
// single most useful fact this pair can report. Any output pointer may be
// NULL. Safe to call from any task -- three single-writer statics, same
// pattern as safety_core_get_trip_event()'s s_trip_* fields.
void safety_core_get_clear_trip_stats(uint32_t *out_requested, uint32_t *out_processed,
                                       safety_clear_trip_outcome_t *out_last_outcome);

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

// SWD/debug-only diagnostics for the 2026-08-27 audit item 1 fix (measured,
// not compile-time-constant, dt_s -- see safety_core.c's own comment at its
// tick_dt_compute_s() call site and tick_timing.h's header comment for the
// full reasoning). Deliberately NOT part of any link frame: CommonFW/docs/
// LINK_PROTOCOL.md's frames are frozen, and this is exactly the "observable
// rather than silent, but not a new wire field" surface safety_core.c's own
// s_clear_trip_pre_*/s_clear_trip_post_call_count statics (added for the
// 2026-08-23 CLEAR_TRIP investigation) already established the precedent
// for -- SWD-readable single-writer statics, safety_core_task's tick the
// only writer, read here through a getter any task may call, same as those.
//
//   out_last_measured_dt_s: the most recent tick's dt_s AFTER clamping (what
//     the guards actually integrated this tick) -- nominal_dt_s on a fallback
//     tick (first tick / clock stalled / the tick right after a stall), a
//     real measurement otherwise.
//   out_clamped_high_count: how many ticks since boot needed the upper clamp
//     (a measured dt_s that exceeded max_dt_s) -- a nonzero, growing count
//     here is direct evidence of the sustained scheduling pressure this fix
//     exists to surface (ARCHITECTURE.md section 8's flash-write stall,
//     above all), the exact condition that used to be completely silent.
//   out_clamped_low_count: the mirror image, ticks needing the lower clamp
//     (a spuriously tiny measured dt_s) -- expected to stay at 0 in normal
//     operation; a nonzero count here means something is measuring faster
//     ticks than vTaskDelayUntil() should ever produce, worth investigating
//     on its own.
//
// Any output pointer may be NULL. Safe to call from any task.
void safety_core_get_dt_diag(float *out_last_measured_dt_s, uint32_t *out_clamped_high_count,
                              uint32_t *out_clamped_low_count);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_SAFETY_CORE_H
