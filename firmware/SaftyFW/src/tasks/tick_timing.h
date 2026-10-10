// tick_timing.h -- pure, host-testable helpers for safety_core.c's per-tick
// timing decisions (2026-08-27 audit items 1/2). Split out the same way
// relay_grace.c/current_presence_policy.c are (see either's own header
// comment): no FreeRTOS/pico-sdk dependency, so test/build_host_tests.ps1
// can link this .c file alone. safety_core.c is the only production caller.
//
// Two unrelated-looking decisions live here together deliberately: both are
// "how much do I trust this millisecond-stamped fact, right now" questions
// safety_core_build_input() has to answer every tick, and both need the same
// wraparound-safe unsigned-subtraction treatment safety_core.c already uses
// for context_valid/reboot_grace_active (see that file's own comments on
// those two fields for the identical reasoning, not repeated here).
#ifndef SAFTYFW_TASKS_TICK_TIMING_H
#define SAFTYFW_TASKS_TICK_TIMING_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Item 1: measured dt_s, not a compile-time constant ---------------------
//
// AUDIT 2026-08-27, item 1: every graduated guard (S1/S3/S5/S9/S11/S12/S13)
// integrates dt_s to decide when a condition has persisted long enough to
// trip. Before this, dt_s was SAFTYFW_PERIOD_SAFETY_CORE_MS/1000.0f, a
// compile-time constant -- so a tick that actually took 150ms under
// scheduling pressure (ARCHITECTURE.md section 8: flash writes park the
// OTHER core for the duration of the write, on an SMP build; safety_core_task
// runs at priority 5, so anything higher-priority hogging the CPU has the
// same effect) still told every guard "100ms of window elapsed", silently
// LENGTHENING every duration bar with nothing anywhere reporting it. A 20s
// window becomes 30s of real time before the guard fires while it keeps
// logging "tripped after 20.0s".
//
// tick_dt_compute_s() is the fix: measure the ACTUAL elapsed wall-clock time
// between successive ticks (now_ms - prev_now_ms, from the same clock
// safety_core.c's own clock_health_observe() already watches) and clamp it,
// falling back to nominal_dt_s (the old compile-time constant) whenever there
// is no valid measurement to trust yet.
//
// FALLBACK CASES (return nominal_dt_s unclamped, not a measurement):
//   - have_prev_now_ms == false: the first tick after boot. There is no
//     previous timestamp to diff against -- inventing one (e.g. 0) would
//     produce a garbage multi-second "dt" on tick one and could instantly
//     satisfy a graduated guard's entire duration bar in a single tick. Same
//     "not enough history yet" default clock_health.c's own have_last uses.
//   - clock_stalled == true: clock_health_observe() has already declared
//     to_ms_since_boot(get_absolute_time()) unreliable this tick (frozen, or
//     -- see that file's own header comment -- any other failure mode that
//     detector catches). A stalled clock cannot honestly measure elapsed
//     time, so trusting a diff against it is exactly the "silently passes
//     because its clock stopped" failure clock_health.h warns is "the worst
//     shape of bug in this codebase". The caller is also responsible for the
//     COROLLARY this function cannot enforce on its own: after a stalled
//     tick, the PREVIOUS timestamp is no longer trustworthy either (it may
//     be from before, or mid-way through, the stall), so the caller must
//     clear its own have_prev_now_ms latch whenever clock_stalled is true --
//     see safety_core.c's own call site comment. That makes the tick
//     immediately after a stall fall back to nominal_dt_s too (have_prev_
//     now_ms is false on that tick), and only the tick after THAT resumes
//     measuring real dt against a freshly-established baseline. Never
//     produces a dt spanning "however long the stall itself lasted".
//
// CLAMP (min_dt_s..max_dt_s), applied only to an actual measurement, never
// to the nominal_dt_s fallback above:
//   - Upper bound (max_dt_s) protects against a NUISANCE-TRIP STORM: one
//     genuinely long stall (the exact ARCHITECTURE.md section 8 flash-write
//     case, or anything worse) must not inject a dt so large it alone
//     satisfies a graduated guard's whole duration bar in a single tick --
//     SAFETY_MODEL.md section 2's "a magnitude AND a duration, one alone is
//     never enough" doctrine exists precisely to prevent a single transient
//     from reading as sustained. safety_core.c calls this with
//     max_dt_s == nominal_dt_s * 10 (1.0s at the real 100ms period): the
//     smallest documented graduated-guard duration bar in this codebase is
//     S5's bad_read_time_s default of 5.0s (safety_guards.c
//     BAD_READ_TIME_S_DEFAULT), so one maximally-clamped tick can supply at
//     most 1/5 of even the shortest bar -- never enough, alone, to trip
//     anything. Multiple genuinely-elongated ticks are still required, which
//     is the real elongation this feature exists to surface, not fake.
//   - Lower bound (min_dt_s) protects against a spuriously tiny/near-zero
//     measured diff (two reads landing unexpectedly close together) being
//     trusted at face value: that would UNDER-integrate every window,
//     silently lengthening every guard's real-world trip latency past its
//     documented duration -- the opposite failure direction from the storm
//     above, but still a silent divergence from the documented number
//     nothing reports. Not a safety hazard the way the upper bound is (a
//     slow-to-trip guard is still fail-safe, just later than documented), so
//     this bound exists for honesty against the doc, not to prevent a
//     hazard. safety_core.c calls this with min_dt_s == nominal_dt_s * 0.5,
//     the same "half of nominal" fraction clock_health.c's own healthy/
//     stalled threshold already uses (CLOCK_HEALTH_STALL_DEBOUNCE's
//     half_expected_ms) -- a real vTaskDelayUntil()-paced tick should never
//     legitimately measure below that, so anything that does is treated as
//     measurement noise, not a real cadence change.
//   - Neither bound is nominal_dt_s itself in either direction -- clamping
//     to exactly the constant would silently undo the whole feature (a tick
//     that really took 150ms must still report something other than
//     100ms). The bounds only cap how far a single tick's contribution can
//     drift from nominal, not whether it can drift at all.
//
// now_ms/prev_now_ms: same to_ms_since_boot(get_absolute_time()) clock
// safety_core.c's context_valid/reboot_grace_active already read, compared
// via unsigned subtraction -- wraparound-safe for the identical reason those
// two fields already document (both operands come from the same clock, so
// the difference wraps correctly in uint32_t arithmetic regardless of which
// side is numerically larger).
float tick_dt_compute_s(uint32_t now_ms, uint32_t prev_now_ms, bool have_prev_now_ms,
                         bool clock_stalled, float nominal_dt_s, float min_dt_s, float max_dt_s);

// --- Item 2: is a timestamped snapshot still fresh enough to trust ----------
//
// AUDIT 2026-08-27, item 2: ARCHITECTURE.md section 6, "Data flow and
// snapshots" -- "Staleness is checked by the consumer, not the producer. A
// producer that has stopped updating cannot mark its own data stale, which is
// precisely the case that matters." safety_core_build_input() already applies
// this to context_snapshot_t (context_valid, safety_core.c ~line 593) but
// never to thermo_snapshot_t/current_snapshot_t -- so a thermo_task or
// current_task that keeps feeding the watchdog (proving the TASK is alive)
// while repeatedly losing its own publish mutex (so the SNAPSHOT stops
// updating) leaves safety_core reading a last-good snapshot that looks as
// fresh as a live one forever. The "stale reading looks fresh" class
// ARCHITECTURE.md's own doctrine exists to prevent.
//
// snapshot_is_fresh() is the same age-vs-max-age comparison safety_core.c
// already writes inline for context_valid/reboot_grace_active, factored out
// here ONLY because it needs to be host-tested (safety_core_build_input()
// itself cannot be -- it pulls FreeRTOS/pico-sdk -- and this codebase's
// negative-test-every-check rule needs a provable pass/fail for both a stale
// snapshot being rejected and a normal, slightly-late tick still being
// accepted). Not a new abstraction over context_valid's own inline version --
// that one stays inline in safety_core.c, matching this file's own doc
// comment on why it does not also rehome that check: it is not needed twice
// for the SAME safety property with two different call shapes, only where a
// second, independently-callable copy is required for testability.
//
// timestamp_ms == 0 (thermo_task.c/current_sense.c's own "never published
// yet" zero-init value, ARCHITECTURE.md section 6) is not special-cased: it
// simply produces a large age against any real now_ms, correctly reading as
// stale -- the same "a snapshot that was never published comes back... via
// the struct's own default field values" contract safety_core_build_input()
// already documents for thermo_task_get_snapshot().
bool snapshot_is_fresh(uint32_t now_ms, uint32_t timestamp_ms, uint32_t max_age_ms);

// Guard review 2026-10-09 F7: wrap-safe ANNOUNCE_REBOOT grace window.
// reboot_announce keeps its timestamp for the whole boot, so a bare
// (now - announced) < window test silently RE-ARMS the S6b grace
// suppression when the 32-bit ms tick wraps (~49.7 days of uptime) and the
// old announcement's age reads small again. This helper expires an
// announcement once: after the first evaluation that finds its age >= window
// it records the announcement's timestamp in *expired_at_ms / *expired_valid
// and ignores that same timestamp from then on. A genuinely new announcement
// (different timestamp) is honoured again. Caller owns the two state words
// (single task, no locking).
bool reboot_grace_evaluate(bool announced, uint32_t announced_at_ms, uint32_t now_ms,
                           uint32_t window_ms, bool *expired_valid, uint32_t *expired_at_ms);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_TICK_TIMING_H
