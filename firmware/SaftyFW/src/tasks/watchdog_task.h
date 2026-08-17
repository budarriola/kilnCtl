// watchdog_task.h -- feeds the RP2040 hardware watchdog, but only when every
// registered task has checked in since the last feed (docs/ARCHITECTURE.md
// section 4, "Watchdog": "A hung thermo_task therefore reboots the chip").
//
// watchdog_task must not depend on safety_core (ARCHITECTURE.md section 4:
// "a control loop cannot be its own watchdog") -- this file has no
// safety_guards.h / safety_core.h include, by the same reasoning
// thermal_guard.h:19-22 gives on the KilnFW side.
#ifndef SAFTYFW_TASKS_WATCHDOG_TASK_H
#define SAFTYFW_TASKS_WATCHDOG_TASK_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// One bit per task that must check in before a feed is allowed. Order is
// arbitrary but stable -- do not renumber once other tasks are calling
// watchdog_task_checkin() with these values.
typedef enum {
    WATCHDOG_CHECKIN_RELAY_OWNER = 0,
    WATCHDOG_CHECKIN_SAFETY_CORE,
    WATCHDOG_CHECKIN_DISCRETE_TASK,
    WATCHDOG_CHECKIN_THERMO_TASK,
    WATCHDOG_CHECKIN_CURRENT_TASK,
    WATCHDOG_CHECKIN_LINK_TASK,
    WATCHDOG_CHECKIN_LOG_TASK,
    WATCHDOG_CHECKIN_UPDATE_TASK, // Phase 10, added this pass -- see update_task.c
    WATCHDOG_CHECKIN_COUNT
} watchdog_checkin_id_t;

// Enables the hardware watchdog (1 s timeout) and creates watchdog_task at
// SAFTYFW_PRIO_WATCHDOG_TASK, pinned to SAFTYFW_CORE_TRIP_PATH.
//
// pause_on_debug is hardcoded true inside this function for now -- there is
// no release/debug build distinction yet in this CMake project. TODO: add a
// SAFTYFW_RELEASE_BUILD (or similar) CMake option and flip this to false for
// it; docs/ARCHITECTURE.md section 8 is explicit that a release build which
// pauses on debug can have its watchdog silently disabled by an attached
// probe, "the one component where that matters".
//
// Returns false if task creation failed. Does not itself latch a
// SAFETY_TRIP_SELF_TEST boot_reason -- that is watchdog_task's own job once a
// checkin actually times out (TODO, needs boot_reason_latch_trip() wired up
// to a real timeout detection, not just "not fed").
bool watchdog_task_start(void);

// Called by every other task once per its own period, to prove liveness.
// Never blocks: this just sets a bit in a bitmask watchdog_task reads under a
// short critical section, so a caller cannot be delayed by this call.
void watchdog_task_checkin(watchdog_checkin_id_t id);

// True once every registered task has called watchdog_task_checkin() at
// least once since boot -- CUMULATIVE across the whole run, unlike the
// internal per-feed-window bitmask watchdog_task_fn() clears every
// SAFTYFW_PERIOD_WATCHDOG_TASK_MS. Added for Phase 10's PENDING_VERIFY ->
// VALID confirmation gate (src/update/confirm.h's all_tasks_checked_in),
// which needs "has every task proven it is alive at least once this boot",
// not "did every task check in during the last 250 ms window" -- the two
// questions sound similar but the periodic mask is deliberately reset
// every window (see watchdog_task.c's watchdog_task_fn()) and would read
// false almost all the time even on a perfectly healthy system, since not
// every task's period lines up with every 250 ms window. Safe to call from
// any task, same critical-section discipline as watchdog_task_checkin().
bool watchdog_task_all_checked_in_since_boot(void);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_WATCHDOG_TASK_H
